#!/usr/bin/env python3
"""Package one clean, enabled local candidate; never launch, upload or distribute.

All dependencies come from the declared installation used for the clean build,
not an earlier evidence bundle. A zero exit from macdeployqt alone is not proof
of dependency closure. Runtime keys, wallets and logs are never archive inputs.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import tarfile
import time

SOURCE = Path(__file__).resolve().parents[2]
TARGET = 'test_b3_flowmeshclosed-gui'


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def save(path, value):
    with Path(path).open('x') as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write('\n')


def cache_values(path):
    values = {}
    for line in path.read_text().splitlines():
        if not line or line.startswith(('#', '//')) or '=' not in line:
            continue
        key, value = line.split('=', 1)
        values[key.split(':', 1)[0]] = value
    return values


def version(text):
    return tuple(map(int, text.split('.'))) + (0,) * (3 - text.count('.') - 1)


def framework_version(plist):
    # Qt's CFBundleShortVersionString is major.minor, not the patch identity.
    value = plist.get('CFBundleVersion')
    if not isinstance(value, str) or not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', value):
        raise RuntimeError('Framework does not declare an exact patch version')
    return value


def loopback_https_endpoint(endpoint):
    # The client accepts an HTTPS origin, origin/, or the fixed API path.
    # Closed mode A additionally requires an explicit local service port.
    if not isinstance(endpoint, str):
        return False
    match = re.fullmatch(r'https://(?:127\.0\.0\.1|localhost):([0-9]+)(?:/|/flowmesh/v1)?', endpoint)
    return bool(match and 0 < int(match.group(1)) <= 65535)


def verify_export(root, document):
    rows = document['files']
    canonical = ''
    for row in sorted(rows, key=lambda value: value['path']):
        if type(row['executable']) is not bool or not re.fullmatch('[0-9a-f]{64}', row['sha256']):
            raise RuntimeError('Invalid frozen source row')
        if '\n' in row['path'] or '\r' in row['path']:
            raise RuntimeError('Invalid frozen source path')
        canonical += row['sha256'] + ' ' + ('x' if row['executable'] else '-') + ' ' + row['path'] + '\n'
    if hashlib.sha256(canonical.encode()).hexdigest() != document['source_tree_sha256']:
        raise RuntimeError('Frozen source manifest tree digest differs')
    expected = {row['path']: row for row in document['files']}
    if len(expected) != len(rows):
        raise RuntimeError('Duplicate frozen source path')
    actual = set()
    for item in root.rglob('*'):
        relative = str(item.relative_to(root))
        if relative == '.git' or relative.startswith('.git/'):
            continue
        if item.is_symlink():
            raise RuntimeError('Frozen source must not contain redirected inputs: ' + relative)
        if not item.is_file():
            continue
        actual.add(relative)
        row = expected.get(relative)
        if row is None or sha(item) != row['sha256'] or bool(item.stat().st_mode & 0o111) != row['executable']:
            raise RuntimeError('Frozen source file differs: ' + relative)
    if actual != set(expected):
        raise RuntimeError('Frozen source file set differs')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--profile', required=True, type=Path)
    parser.add_argument('--candidate-id', required=True)
    parser.add_argument('--macdeployqt', required=True, type=Path)
    parser.add_argument('--qtsvg-framework', required=True, type=Path)
    parser.add_argument('--notices-dir', required=True, type=Path)
    parser.add_argument('--source-manifest', type=Path,
                        help='Explicit preserved source export when commits are blocked; NOT a clean-checkout qualification')
    args = parser.parse_args()
    os.umask(0o077)
    if not re.fullmatch(r'[a-z0-9][a-z0-9-]{0,79}', args.candidate_id):
        parser.error('Unsafe candidate identity')
    build = args.build_dir.resolve(strict=True)
    profile_path = args.profile.resolve(strict=True)
    destination = args.output_dir.absolute()
    if destination.exists() or destination.is_symlink():
        parser.error('Output must be new; frozen packages are never overwritten')
    if destination.parent.resolve(strict=True) != destination.parent or destination.is_relative_to(SOURCE):
        parser.error('Output must be outside source with a non-redirected existing parent')
    env = dict(os.environ, GIT_OPTIONAL_LOCKS='0', PYTHONDONTWRITEBYTECODE='1')
    for name in list(env):
        if name.startswith(('DYLD_', 'QT_', 'QML_')):
            del env[name]
    def checked(command, cwd=SOURCE):
        return subprocess.check_output(list(map(str, command)), cwd=cwd, env=env, text=True)
    export = None
    if args.source_manifest:
        manifest_path = args.source_manifest.resolve(strict=True)
        if manifest_path.is_relative_to(SOURCE):
            parser.error('Frozen source manifest must be stored outside the exported source')
        export = json.loads(manifest_path.read_text())
        verify_export(SOURCE, export)
        commit = None
        tree = export['source_tree_sha256']
        provenance = 'explicit manifest-verified source export; uncommitted, NOT clean-checkout qualification'
    else:
        if Path(checked(['git', 'rev-parse', '--show-toplevel']).strip()).resolve() != SOURCE:
            parser.error('Source must be the actual repository root, not a parent repository discovery')
        if checked(['git', 'status', '--porcelain', '--untracked-files=all']).strip():
            parser.error('Source checkout must contain only committed inputs and be clean')
        commit = checked(['git', 'rev-parse', 'HEAD']).strip()
        tree = checked(['git', 'rev-parse', 'HEAD^{tree}']).strip()
        provenance = 'clean committed checkout'
    cache = cache_values(build / 'CMakeCache.txt')
    if Path(cache['CMAKE_HOME_DIRECTORY']).resolve() != SOURCE:
        parser.error('Build source is not this clean checkout')
    if Path(cache['B3_FLOWMESH_CLOSED_TEST_PROFILE']).resolve() != profile_path:
        parser.error('Build embeds a different profile')
    if cache['CMAKE_OSX_ARCHITECTURES'] != 'arm64':
        parser.error('Only the currently qualified arm64 platform is supported')
    compilation = (build / 'compile_commands.json').read_text()
    if re.search(r'[-/]D\s*FLOWMESH_CLIENT_CRASH_TEST_HOOKS', compilation):
        parser.error('Crash-injection hooks must not be enabled in the ordinary tester executable')
    profile_bytes = profile_path.read_bytes()
    if len(profile_bytes) > 32768 or b'PRIVATE KEY' in profile_bytes:
        parser.error('Profile is oversized or contains private key material')
    profile = json.loads(profile_bytes)
    if profile.get('ready') is not True or profile.get('network') != 'regtest':
        parser.error('Only an enabled isolated regtest profile may be packaged')
    if not profile['b3_peer'].startswith('127.0.0.1:') or any(
        not loopback_https_endpoint(endpoint)
        for endpoint in profile['https_endpoints']):
        parser.error('This mode-A package requires loopback B3 and HTTPS endpoints')
    notices = args.notices_dir.resolve(strict=True)
    notice_manifest = json.loads((notices / 'NOTICE-MANIFEST.json').read_text())
    if notice_manifest.get('collection_complete') is not True:
        parser.error('Notice collection did not complete; preserve partial evidence and use a completed successor')
    for row in notice_manifest['files']:
        item = notices / row['path']
        if item.is_symlink() or not item.resolve().is_relative_to(notices) or sha(item) != row['sha256']:
            parser.error('Notice input differs from its reviewed manifest')
    for module in ('qtbase', 'qtsvg'):
        if not any(row['path'].startswith(module + '/') and 'LGPL-3.0' in row['path']
                   for row in notice_manifest['files']):
            parser.error('Missing exact Qt/QtSvg LGPL license texts')
    destination.mkdir(mode=0o700)
    payload = destination / 'local-package'
    payload.mkdir()
    logs = destination / 'package-logs'
    logs.mkdir()
    symbols = destination / 'symbols'
    symbols.mkdir()
    commands = []
    def run(name, command):
        row = {'name': name, 'argv': list(map(str, command)), 'started_at': time.time()}
        commands.append(row)
        result = subprocess.run(row['argv'], cwd=SOURCE, env=env, capture_output=True, text=True)
        row.update(exit_status=result.returncode, finished_at=time.time())
        (logs / (name + '.log')).write_text(result.stdout + result.stderr)
        print(json.dumps(row), flush=True)
        if result.returncode:
            raise RuntimeError(name + ' failed; original output retained')
        return result.stdout
    try:
        app = payload / 'B3 FlowMesh CLOSED TEST.app'
        shutil.copytree(build / 'bin' / (TARGET + '.app'), app, symlinks=True)
        executable = app / 'Contents/MacOS' / TARGET
        original_sha = sha(executable)
        run('dsymutil', ['/usr/bin/dsymutil', executable, '-o', symbols / (TARGET + '.dSYM')])
        run('macdeployqt', [args.macdeployqt.resolve(strict=True), app, '-verbose=1'])
        frameworks = app / 'Contents/Frameworks'
        # Qt's deploy helper has previously omitted QtSvg while copying SVG
        # plugins. Supply only the declared exact build-version framework.
        svg = args.qtsvg_framework.resolve(strict=True)
        qt_prefix = Path(cache['Qt6_DIR']).resolve().parents[2]
        qt_config = qt_prefix / 'lib/cmake/Qt6/Qt6ConfigVersionImpl.cmake'
        qt_version = re.search(r'set\(PACKAGE_VERSION "([0-9.]+)"\)', qt_config.read_text()).group(1)
        svg_plist = plistlib.loads((svg / 'Versions/A/Resources/Info.plist').read_bytes())
        if framework_version(svg_plist) != qt_version:
            raise RuntimeError('Declared QtSvg and configured Qt versions differ')
        if not (frameworks / 'QtSvg.framework').exists():
            shutil.copytree(svg, frameworks / 'QtSvg.framework', symlinks=True)
        run('normalize', ['/usr/bin/ruby', SOURCE / 'contrib/macdeploy/normalize_bundled_libraries.rb', app])
        executable_load = checked(['/usr/bin/otool', '-l', executable])
        if 'path @executable_path/../Frameworks (offset' not in executable_load:
            run('framework-rpath', ['/usr/bin/install_name_tool', '-add_rpath', '@executable_path/../Frameworks', executable])
        binaries = []
        plist = plistlib.loads((app / 'Contents/Info.plist').read_bytes())
        declared_min = plist['LSMinimumSystemVersion']
        for path in sorted(app.rglob('*')):
            if path.is_symlink():
                if not path.resolve().is_relative_to(app):
                    raise RuntimeError('Escaping application symlink: ' + str(path))
                continue
            if not path.is_file() or 'Mach-O' not in checked(['/usr/bin/file', '-b', path]):
                continue
            architecture = checked(['/usr/bin/lipo', '-archs', path]).strip()
            if architecture != 'arm64':
                raise RuntimeError('Unexpected dependency architecture: ' + str(path))
            load = checked(['/usr/bin/otool', '-l', path])
            minimums = re.findall(r'\bminos ([0-9.]+)', load)
            if not minimums or any(version(value) > version(declared_min) for value in minimums):
                raise RuntimeError('Actual dependency minimum exceeds declared OS: ' + str(path))
            rpaths = re.findall(r'cmd LC_RPATH\n\s*cmdsize \d+\n\s*path (.*?) \(offset', load)
            for index, value in enumerate(rpaths):
                if value.startswith('/'):
                    run('remove-rpath-' + str(len(commands)), ['/usr/bin/install_name_tool', '-delete_rpath', value, path])
            deps = [line.strip().split(' (compatibility version')[0]
                    for line in checked(['/usr/bin/otool', '-L', path]).splitlines()[1:]]
            for dep in deps:
                if dep.startswith(('/System/Library/', '/usr/lib/')):
                    continue
                if dep.startswith('@executable_path/'):
                    target = executable.parent / dep[len('@executable_path/'):]
                elif dep.startswith('@loader_path/'):
                    target = path.parent / dep[len('@loader_path/'):]
                elif dep.startswith('@rpath/'):
                    target = frameworks / dep[len('@rpath/'):]
                else:
                    raise RuntimeError('Unbundled dependency: ' + dep)
                if not target.exists() or not target.resolve().is_relative_to(app):
                    raise RuntimeError('Unresolved in-bundle dependency: ' + dep)
            binaries.append({'path': str(path.relative_to(app)), 'architecture': architecture,
                             'minimum_macos': minimums, 'dependencies': deps})
        run('adhoc-sign', ['/usr/bin/codesign', '--force', '--deep', '--sign', '-', app])
        run('verify-signature', ['/usr/bin/codesign', '--verify', '--deep', '--strict', '--verbose=2', app])
        run('signature-state', ['/usr/bin/codesign', '-dvv', app])
        uuid = run('binary-uuid', ['/usr/bin/dwarfdump', '--uuid', executable]).split()[1]
        if uuid != run('symbols-uuid', ['/usr/bin/dwarfdump', '--uuid', symbols / (TARGET + '.dSYM')]).split()[1]:
            raise RuntimeError('Application and diagnostic symbols do not match')
        for row in binaries:
            row['sha256'] = sha(app / row['path'])
        save(destination / 'dependency-closure.json', {'status': 'passed', 'binaries': binaries,
            'declared_qt': qt_version, 'qtsvg_source': str(svg)})
        shutil.copy2(SOURCE / 'COPYING', payload / 'COPYING')
        shutil.copytree(notices, payload / 'licenses')
        for name in ('README.md', 'TESTER-CHECKLIST.md', 'KNOWN-ISSUES.md', 'PRIVATE-REPORT.md', 'Launch-CLOSED-TEST.command'):
            shutil.copy2(SOURCE / 'contrib/closed-test' / name, payload / name)
        (payload / 'Launch-CLOSED-TEST.command').chmod(0o755)
        # Only public profile material is retained. Private CA/server keys and
        # all runtime state remain outside the package.
        (payload / 'PUBLIC-PROFILE.json').write_bytes(profile_bytes)
        identity = {'candidate_id': args.candidate_id, 'source_commit': commit, 'source_tree': tree,
            'source_provenance': provenance,
            'profile_id': profile['profile_id'], 'profile_sha256': hashlib.sha256(profile_bytes).hexdigest(),
            'profile_ready': True, 'architecture': 'arm64', 'minimum_macos': declared_min,
            'qt': qt_version, 'build_type': cache['CMAKE_BUILD_TYPE'], 'uuid': uuid,
            'executable': str(executable.relative_to(payload)), 'executable_sha256': sha(executable),
            'pre_package_executable_sha256': original_sha,
            'application_dsym': 'symbols/' + TARGET + '.dSYM',
            'signing': 'local ad-hoc only; not Developer ID signed or notarized',
            'status': 'LOCAL PILOT CANDIDATE; native execution qualified separately',
            'external_handoff': 'NOT AUTHORIZED; signing/distribution and notice-compliance review remain separate',
            'known_issue': 'Original accessibilitySelectedChildren SIGSEGV OPEN / UNRESOLVED',
            'availability': profile['availability'],
            'notice_collection': notice_manifest.get('status', 'collection only; external review pending')}
        save(payload / 'CANDIDATE.json', identity)
        save(destination / 'binary-identity.json', identity)
        artifacts = destination / 'artifacts'
        artifacts.mkdir()
        archives = []
        for label, directory in [('LOCAL-PILOT', payload), ('symbols', symbols)]:
            archive = artifacts / (args.candidate_id + '-' + label + '.zip')
            run('archive-' + label, ['/usr/bin/ditto', '-c', '-k', '--keepParent', '--noextattr', directory, archive])
            archives.append({'path': str(archive.relative_to(destination)), 'bytes': archive.stat().st_size, 'sha256': sha(archive)})
        source_archive = artifacts / (args.candidate_id + '-source.tar.gz')
        if export is None:
            run('source-archive', ['git', 'archive', '--format=tar.gz', '--output=' + str(source_archive), commit])
        else:
            with tarfile.open(source_archive, 'w:gz') as archive:
                for row in sorted(export['files'], key=lambda value: value['path']):
                    archive.add(SOURCE / row['path'], arcname=row['path'], recursive=False)
            save(destination / 'EXPORTED-SOURCE-MANIFEST.json', export)
        archives.append({'path': str(source_archive.relative_to(destination)), 'bytes': source_archive.stat().st_size, 'sha256': sha(source_archive)})
        expanded = destination / 'package-preparation/expanded'
        expanded.parent.mkdir()
        run('extract', ['/usr/bin/ditto', '-x', '-k', destination / archives[0]['path'], expanded])
        extracted_app = expanded / 'local-package' / app.name
        extracted = expanded / 'local-package' / identity['executable']
        if sha(extracted) != identity['executable_sha256']:
            raise RuntimeError('Extracted executable differs')
        for row in binaries:
            if sha(extracted_app / row['path']) != row['sha256']:
                raise RuntimeError('Extracted dependency differs')
        for path in extracted_app.rglob('*'):
            if path.is_symlink() and not path.resolve().is_relative_to(extracted_app):
                raise RuntimeError('Extracted application link escapes')
        run('extracted-signature', ['/usr/bin/codesign', '--verify', '--deep', '--strict', '--verbose=2', extracted_app])
        save(destination / 'ARTIFACTS.json', {'candidate': identity, 'artifacts': archives,
            'extracted_executable': str(extracted)})
        (destination / 'SHA256SUMS').write_text(''.join(row['sha256'] + '  ' + row['path'] + '\n' for row in archives))
        print(json.dumps({'status': 'packaged-and-extracted; native check remains separate', **identity}), flush=True)
    finally:
        save(destination / 'package-commands.json', commands)
        if export is not None:
            verify_export(SOURCE, export)
        elif checked(['git', 'rev-parse', 'HEAD']).strip() != commit or checked(['git', 'status', '--porcelain', '--untracked-files=all']).strip():
            raise RuntimeError('Source changed while packaging; candidate requires review')


if __name__ == '__main__':
    main()
