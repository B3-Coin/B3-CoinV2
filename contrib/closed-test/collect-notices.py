#!/usr/bin/env python3
"""Collect exact-version public Qt notices and installed dependency notices.

Run separately from packaging, with explicit network permission if required.
Only public upstream text is fetched; nothing is executed. Output is an unused
directory outside source. Collection is not a legal/compliance certification.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import posixpath
import re
import ssl
import tarfile
import urllib.error
import urllib.request

SOURCE = Path(__file__).resolve().parents[2]
FORMULAS = ('openssl@3', 'libevent', 'qrencode', 'libb2', 'brotli', 'dbus',
            'double-conversion', 'freetype', 'glib', 'graphite2', 'harfbuzz',
            'icu4c@78', 'gettext', 'jpeg-turbo', 'md4c', 'pcre2', 'libpng', 'zstd')
TREE_API = re.compile(r'https://api\.github\.com/repos/qt/(?:qtbase|qtsvg)/git/trees/v6\.[0-9]+\.[0-9]+\?recursive=1')
# Same upstream versions can have different Homebrew build recipes. Select
# only an independently reviewed installed-recipe SHA256, never a version-only
# fallback or today's "latest" recipe. The selected pinned public recipe must
# still equal the installed bytes after removing only its bottle table.
GLIB_SOURCE = {
    '2.88.3': {
        'recipes': {
            'd2ebed597d64ab5518ea78604840e28fb9839fa923bd3917bef5f50b76d369be': {
                'homebrew_commit': '190a6fa77b81b8ffbeeee1022b852063d55d71cc',
                'formula_sha256': '18ec76babcd6aebbdfeca6f09f9bfb24b3694d4e432c926c7f9612ee53515742',
            },
            # August 3: post-install path uses a Homebrew template token.
            '1c0b615f2c23a2340eda0a52581e9157631c3dfd55227edcf3e14ea565e0a36f': {
                'homebrew_commit': '222fed87fa0d1a1a847a2b610a808a48d5afb526',
                'formula_sha256': '658003a495be4141e3f5856ff34a1334c58fe7a0fc745afbc84258394472a6db',
            },
            # August 17: same build inputs, plus build network sandboxing.
            '0105d4552c1cc68b38cdcd23c4df05c2cb0d93ce44929f581204a6d33dd16d59': {
                'homebrew_commit': '11758a37429b3c8830127b550146c95b2c7ac1c9',
                'formula_sha256': 'd28135a54cacf6e9016a97a412c7cb74b124865d883115dcbf07106237d4aaef',
            },
        },
        'patch_sha256': 'd846efd0bf62918350da94f850db33b0f8727fece9bfaf8164566e3094e80c97',
        'introspection_url': 'https://download.gnome.org/sources/gobject-introspection/1.86/gobject-introspection-1.86.0.tar.xz',
        'introspection_sha256': '920d1a3fcedeadc32acff95c2e203b319039dd4b4a08dd1a2dfd283d19c0b9ae',
    },
}


def glib_source_pin(version, recipe_sha256):
    version_pin = GLIB_SOURCE.get(version)
    if version_pin is None:
        raise RuntimeError('Installed GLib version needs corresponding-source pin review')
    recipe_pin = version_pin['recipes'].get(recipe_sha256)
    if recipe_pin is None:
        # Hash only: do not expose an installed recipe, receipt or local path.
        raise RuntimeError('Installed GLib build recipe needs pin review; sha256=' + recipe_sha256)
    return {**version_pin, **recipe_pin}


class RefuseCredentialRedirect(urllib.request.HTTPRedirectHandler):
    """Do not forward the CI credential, even to another GitHub API URL."""

    def redirect_request(self, request, fp, code, message, headers, newurl):
        raise urllib.error.HTTPError(request.full_url, code,
                                     'Authenticated notice requests refuse redirects', headers, fp)


def get(url, maximum):
    headers = {'User-Agent': 'B3-closed-test-notice-collector'}
    # This optional, step-scoped read-only CI token is only for the two exact
    # public tree API requests. It is never used on raw notice downloads,
    # other API paths, alternate authorities/ports, or redirected requests.
    token = os.environ.get('B3_NOTICE_GITHUB_TOKEN') if TREE_API.fullmatch(url) else None
    if token:
        if not re.fullmatch(r'[A-Za-z0-9_.-]{1,4096}', token):
            raise RuntimeError('Malformed CI notice token')
        headers['Authorization'] = 'Bearer ' + token
    request = urllib.request.Request(url, headers=headers)
    context = ssl.create_default_context()
    if token:
        opener = urllib.request.build_opener(urllib.request.HTTPSHandler(context=context),
                                             RefuseCredentialRedirect())
        response = opener.open(request, timeout=30)
    else:
        response = urllib.request.urlopen(request, context=context, timeout=30)
    with response:
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


def glib_source_identity(installed):
    """Read data, never execute the installed Homebrew Ruby recipe.

    A version inferred from today's upstream release would not describe the
    bundled bottle. Require the installed pkg-config, receipt and exact
    source URL in its retained recipe to agree instead.
    """
    recipe_path = installed / '.brew/glib.rb'
    receipt_path = installed / 'INSTALL_RECEIPT.json'
    pc_path = installed / 'lib/pkgconfig/glib-2.0.pc'
    for path in (recipe_path, receipt_path, pc_path):
        if path.is_symlink() or not path.is_file() or path.stat().st_size > 1024 * 1024:
            raise RuntimeError('Missing/unsafe installed GLib provenance')
    recipe = recipe_path.read_bytes()
    receipt = json.loads(receipt_path.read_bytes())
    pc = pc_path.read_text()
    match = re.search(r'^Version: (2\.[0-9]+\.[0-9]+)$', pc, re.M)
    if not match or receipt.get('source', {}).get('versions', {}).get('stable') != match[1]:
        raise RuntimeError('Installed GLib version evidence differs')
    version = match[1]
    # Only the main source declaration, before dependency resources/bottles.
    declaration = re.split(r'^  (?:resource|bottle|head|depends_on)\b', recipe.decode(), maxsplit=1, flags=re.M)[0]
    urls = re.findall(r'^  url "([^"]+)"$', declaration, re.M)
    hashes = re.findall(r'^  sha256 "([0-9a-f]{64})"$', declaration, re.M)
    expected = 'https://download.gnome.org/sources/glib/' + '.'.join(version.split('.')[:2]) + '/glib-' + version + '.tar.xz'
    if urls != [expected] or len(hashes) != 1:
        raise RuntimeError('Unsupported installed GLib source declaration')
    return {'version': version, 'url': expected, 'sha256': hashes[0]}, recipe


def glib_archive_notices(data, version):
    """Read bounded notice members; never extract or execute archive paths."""
    root = 'glib-' + version + '/'
    notices, seen, total = {}, set(), 0
    copying_target = None
    with tarfile.open(fileobj=io.BytesIO(data), mode='r|xz') as archive:
        for count, member in enumerate(archive, 1):
            path = PurePosixPath(member.name)
            if count > 50000 or path.is_absolute() or '..' in path.parts or member.name in seen:
                raise RuntimeError('Unsafe/unbounded GLib source archive')
            seen.add(member.name)
            total += member.size
            if member.name == root.rstrip('/') and member.isdir():
                continue
            if total > 512 * 1024 * 1024 or not member.name.startswith(root):
                raise RuntimeError('Unsafe/unbounded GLib source archive')
            relative = member.name[len(root):]
            if relative == 'COPYING' and member.issym():
                if member.linkname != 'LICENSES/LGPL-2.1-or-later.txt':
                    raise RuntimeError('Unexpected GLib COPYING reference')
                copying_target = member.linkname
                continue
            selected = (('/LICENSES/' in '/' + relative and relative.endswith('.txt')) or
                        ('/' not in relative and re.match(r'^(COPYING|LICENSE|LICENCE|NOTICE|COPYRIGHT|AUTHORS)(\.|$|[-_])', relative, re.I)))
            if not selected or member.isdir():
                continue
            if not member.isfile() or member.size > 2 * 1024 * 1024:
                raise RuntimeError('Unsafe GLib notice member')
            content = archive.extractfile(member).read(2 * 1024 * 1024 + 1)
            if len(content) != member.size:
                raise RuntimeError('Incomplete GLib notice member')
            notices[relative] = (content, {})
    license_path = 'LICENSES/LGPL-2.1-or-later.txt'
    if license_path not in notices:
        raise RuntimeError('Exact GLib LGPL license missing')
    if copying_target:
        notices['COPYING'] = (notices[copying_target][0], {'source_symlink_target': copying_target})
    return notices


def collect_glib_source(installed, retain):
    identity, recipe = glib_source_identity(installed)
    recipe_sha256 = hashlib.sha256(recipe).hexdigest()
    pin = glib_source_pin(identity['version'], recipe_sha256)
    base = 'https://raw.githubusercontent.com/Homebrew/homebrew-core/' + pin['homebrew_commit'] + '/'
    public_recipe = get(base + 'Formula/g/glib.rb', 1024 * 1024)
    if hashlib.sha256(public_recipe).hexdigest() != pin['formula_sha256']:
        raise RuntimeError('Pinned Homebrew GLib recipe checksum differs')
    normalized_recipe = re.sub(rb'(?ms)^  bottle do\n.*?^  end\n\n', b'', public_recipe, count=1)
    if normalized_recipe != recipe:
        raise RuntimeError('Installed GLib build recipe differs from pinned source recipe')
    patch = get(base + 'Patches/glib/hardcoded-paths.diff', 1024 * 1024)
    if hashlib.sha256(patch).hexdigest() != pin['patch_sha256']:
        raise RuntimeError('Pinned Homebrew GLib patch checksum differs')
    data = get(identity['url'], 32 * 1024 * 1024)
    if hashlib.sha256(data).hexdigest() != identity['sha256']:
        raise RuntimeError('GLib source differs from installed recipe checksum')
    notices = glib_archive_notices(data, identity['version'])
    introspection = get(pin['introspection_url'], 32 * 1024 * 1024)
    if hashlib.sha256(introspection).hexdigest() != pin['introspection_sha256']:
        raise RuntimeError('GLib introspection build resource checksum differs')
    provenance = {'installed_component': 'glib', 'installed_version': installed.name,
                  'upstream_source': identity, 'homebrew_commit': pin['homebrew_commit'],
                  'installed_recipe_sha256': recipe_sha256, 'public_recipe_sha256': pin['formula_sha256']}
    retain('installed/glib/source/glib-' + identity['version'] + '.tar.xz', data, provenance)
    retain('installed/glib/source/homebrew-glib.rb', recipe, provenance)
    retain('installed/glib/source/homebrew-core/Formula/g/glib.rb', public_recipe,
           {**provenance, 'url': base + 'Formula/g/glib.rb'})
    retain('installed/glib/source/homebrew-core/Patches/glib/hardcoded-paths.diff', patch,
           {**provenance, 'url': base + 'Patches/glib/hardcoded-paths.diff'})
    retain('installed/glib/source/gobject-introspection-1.86.0.tar.xz', introspection,
           {**provenance, 'url': pin['introspection_url']})
    receipt = json.loads((installed / 'INSTALL_RECEIPT.json').read_bytes())
    # Preserve public build facts, not developer-local receipt paths.
    build_facts = {key: receipt.get(key) for key in (
        'homebrew_version', 'used_options', 'unused_options', 'built_as_bottle',
        'poured_from_bottle', 'compiler', 'arch', 'built_on')}
    retain('installed/glib/source/BUILD-PROVENANCE.json',
           (json.dumps(build_facts, indent=2, sort_keys=True) + '\n').encode(), provenance)
    for path, (content, extra) in sorted(notices.items()):
        retain('installed/glib/' + path, content, {**provenance, **extra})
    # Preserve the actual patch and recipe, not only upstream source. This
    # closes this specific missing-input gap, not every distribution duty.


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
                if formula == 'glib':
                    collect_glib_source(installed, retain)
                    # Upstream COPYING can be a symlink, omitted by Homebrew.
                    # Its exact target license is now retained independently.
                elif not candidates:
                    gaps.append({'component': formula, 'reason': 'Bottle contains no top-level notice; source review required'})
                for path in candidates if formula != 'glib' else ():
                    retain('installed/' + formula + '/' + path.name, path.read_bytes(),
                           {'installed_component': formula, 'installed_version': installed.name,
                            'source_filename': path.name})
        retain('NOTICE-SCOPE.txt', (
            'Exact upstream Qt/QtSvg license and third-party attribution texts are retained with hashes.\n'
            'Installed dependency notices are retained where supplied by the declared build bottles.\n'
            'GLib includes checksum-pinned upstream source, its matching installed/public Homebrew recipe, patch and introspection build resource.\n'
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
