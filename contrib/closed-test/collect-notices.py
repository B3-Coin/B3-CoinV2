#!/usr/bin/env python3
"""Collect exact-version public Qt notices and installed dependency notices.

Run separately from packaging, with explicit network permission if required.
Only public upstream text is fetched; nothing is executed. Output is an unused
directory outside source. Collection is not a legal/compliance certification.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import posixpath
import re
import ssl
import urllib.request

SOURCE = Path(__file__).resolve().parents[2]
FORMULAS = ('openssl@3', 'libevent', 'qrencode', 'libb2', 'brotli', 'dbus',
            'double-conversion', 'freetype', 'glib', 'graphite2', 'harfbuzz',
            'icu4c@78', 'gettext', 'jpeg-turbo', 'md4c', 'pcre2', 'libpng', 'zstd')


def get(url, maximum):
    request = urllib.request.Request(url, headers={'User-Agent': 'B3-closed-test-notice-collector'})
    with urllib.request.urlopen(request, context=ssl.create_default_context(), timeout=30) as response:
        data = response.read(maximum + 1)
    if len(data) > maximum:
        raise RuntimeError('Public notice input exceeds bound: ' + url)
    return data


def read_attribution(data):
    # Qt's pinned public attribution records can contain literal line breaks
    # inside quoted copyright strings (e.g. qtbase forkfd6.11.1). Retain their
    # original hashed bytes. This text-only collector permits those controls;
    # no application/protocol/wallet JSON parser is changed or reused here.
    document = json.loads(data, strict=False)
    entries = document if isinstance(document, list) else [document]
    if not all(isinstance(entry, dict) for entry in entries):
        raise ValueError('Attribution must be an object or array of objects')
    return entries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--qt-version', required=True)
    parser.add_argument('--homebrew-prefix', type=Path,
                        help='Optional declared build dependency installation; direct notice files only')
    args = parser.parse_args()
    if not re.fullmatch(r'6\.[0-9]+\.[0-9]+', args.qt_version):
        parser.error('An exact Qt6 patch version is required')
    output = args.output_dir.absolute()
    if output.exists() or output.is_symlink() or output.is_relative_to(SOURCE):
        parser.error('Output must be unused and outside source')
    if output.parent.resolve(strict=True) != output.parent:
        parser.error('Output parent must not redirect')
    os.umask(0o077)
    output.mkdir(mode=0o700)
    rows, gaps = [], []
    completed = False

    def retain(relative, data, provenance):
        path = output / relative
        if not path.resolve().is_relative_to(output):
            raise RuntimeError('Unsafe notice path')
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open('xb') as stream:
            stream.write(data)
        rows.append({'path': str(path.relative_to(output)), 'bytes': len(data),
                     'sha256': hashlib.sha256(data).hexdigest(), 'provenance': provenance})

    try:
        for module in ('qtbase', 'qtsvg'):
            base = 'https://api.github.com/repos/qt/' + module
            tag = 'v' + args.qt_version
            tree_url = base + '/git/trees/' + tag + '?recursive=1'
            tree = json.loads(get(tree_url, 24 * 1024 * 1024))
            if tree.get('truncated'):
                raise RuntimeError('Upstream tree is incomplete')
            files = {row['path']: row for row in tree['tree'] if row['type'] == 'blob'}
            selected = {path for path in files if path.startswith('LICENSES/') and path.endswith('.txt')}
            attributions = sorted(path for path in files if path.endswith('/qt_attribution.json'))
            cache = {}

            def fetch(path):
                normalized = posixpath.normpath(path)
                if normalized.startswith('../') or PurePosixPath(normalized).is_absolute() or normalized not in files:
                    raise RuntimeError('Missing/unsafe upstream notice reference: ' + path)
                if normalized in cache:
                    return cache[normalized]
                url = 'https://raw.githubusercontent.com/qt/' + module + '/' + tag + '/' + normalized
                data = get(url, 2 * 1024 * 1024)
                blob = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
                if blob != files[normalized]['sha']:
                    raise RuntimeError('Upstream notice changed while collecting: ' + normalized)
                retain(module + '/' + normalized, data,
                       {'url': url, 'git_blob': blob, 'tree': tree['sha'], 'tag': tag})
                cache[normalized] = data
                return data

            for path in sorted(selected):
                fetch(path)
            for path in attributions:
                entries = read_attribution(fetch(path))
                for entry in entries:
                    values = entry.get('LicenseFile', entry.get('LicenseFiles', []))
                    values = [values] if isinstance(values, str) else values
                    for value in values:
                        target = posixpath.normpath(posixpath.join(posixpath.dirname(path), value))
                        if target in files:
                            fetch(target)
                        else:
                            gaps.append({'module': module, 'attribution': path,
                                         'reason': 'LicenseFile reference not found in pinned tree', 'reference': value})
            if not any('LGPL-3.0' in path for path in selected):
                raise RuntimeError('Expected LGPL3 license missing from ' + module)
        if args.homebrew_prefix:
            prefix = args.homebrew_prefix.resolve(strict=True)
            for formula in FORMULAS:
                installed = prefix / 'opt' / formula
                if not installed.exists():
                    gaps.append({'component': formula, 'reason': 'Declared installation not present; compare actual bundle'})
                    continue
                installed = installed.resolve(strict=True)
                candidates = [path for path in installed.iterdir() if path.is_file() and
                              re.match(r'^(COPYING|LICENSE|LICENCE|NOTICE|COPYRIGHT|AUTHORS)(\.|$|[-_])', path.name, re.I)]
                if not candidates:
                    gaps.append({'component': formula, 'reason': 'Bottle contains no top-level notice; source review required'})
                for path in candidates:
                    retain('installed/' + formula + '/' + path.name, path.read_bytes(),
                           {'installed_component': formula, 'installed_version': installed.name,
                            'source_filename': path.name})
        retain('NOTICE-SCOPE.txt', (
            'Exact upstream Qt/QtSvg license and third-party attribution texts are retained with hashes.\n'
            'Installed dependency notices are retained where supplied by the declared build bottles.\n'
            'This is not a complete source-code offer or external-distribution compliance certification.\n'
            'Inspect the manifest gaps against actual bundled libraries before external handoff.\n'
            'Qt source: https://code.qt.io/cgit/qt/qtbase.git/ and https://code.qt.io/cgit/qt/qtsvg.git/\n'
            'No system security protection is disabled by this local test package.\n').encode(),
            {'scope': 'collection limits, not legal advice'})
        completed = True
    finally:
        with (output / 'NOTICE-MANIFEST.json').open('x') as stream:
            json.dump({'qt_version': args.qt_version, 'files': rows, 'gaps': gaps,
                       'collection_complete': completed,
                       'status': 'public notice collection; external compliance review pending'}, stream, indent=2, sort_keys=True)
            stream.write('\n')
    print(json.dumps({'files': len(rows), 'gaps': len(gaps), 'output': str(output)}), flush=True)


if __name__ == '__main__':
    main()
