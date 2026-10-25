#!/usr/bin/env python3
"""Build pinned upstream testnode/contracts in fresh bounded local assets.

Public dependency downloads happen only here. Steady test containers use a
separate non-masquerading network. This never runs upstream destructive init.
"""
import argparse
import datetime
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tarfile
import time
import urllib.request
import uuid

from runtime import NODE, NITRO, GETH, ENV, LABEL, assets_info, docker, docker_logs, expect, read_json, safe, sha, write_json

ARCHIVES = (
    ('testnode', 'nitro-testnode', 'd46fe1bfe6dd0dbc2e27dcc6d0dc559a248433f5', '88daa99305feaa58714759e25219185d339a101110e709f406b120ddb16b51c3'),
    ('contracts', 'nitro-contracts', '4341b132cfbdcc980ead03765ca5224ff6cb5d97', '9a3ccbf6ab73158d0639c8c1567c39bee3313195b5c92fdf03f100e74b7b16c2'),
    ('contracts/src/precompiles', 'nitro-precompile-interfaces', 'f49a4889b486fd804a7901203f5f663cfd1581c8', '3359013880a2ed997c229e33842514f77dbf860a1f557d960ffb9ad00305ab57'),
)
SOLC_SHA = '99f2070b776e9714f1f76c43c229cf99b8978a92938ee8d2364c6de11c1a03d4'
FORGE_SHA = '06c477aee83d0996e57aa92b0841fdf06382cb172fd69bbb88920bb0054dfd7f'
OVERRIDE = '''import { subtask } from 'hardhat/config';
import { TASK_COMPILE_SOLIDITY_GET_SOLC_BUILD } from 'hardhat/builtin-tasks/task-names';
const base = require('./hardhat.config');
subtask(TASK_COMPILE_SOLIDITY_GET_SOLC_BUILD).setAction(async ({solcVersion}: any) => {
  if (solcVersion !== '0.8.17') throw new Error('unexpected compiler version');
  return {compilerPath:'/assets/tools/solc-0.8.17', isSolcJs:false,
          version:'0.8.17', longVersion:'0.8.17+commit.8df45f5f'};
});
for (const c of [...base.solidity.compilers, ...Object.values(base.solidity.overrides)]) {
  c.settings.outputSelection = {'*': {'*': ['abi','evm.bytecode','evm.deployedBytecode','evm.methodIdentifiers','metadata','storageLayout'], '': ['ast']}};
}
export default base;
'''


def download(url, expected_sha, maximum):
    expect(url.startswith(('https://codeload.github.com/OffchainLabs/', 'https://raw.githubusercontent.com/ethereum/solc-bin/')), 'unrecognized upstream')
    with urllib.request.urlopen(url, timeout=45) as response:
        expect(response.status == 200, 'upstream download refused')
        data = response.read(maximum + 1)
    expect(len(data) <= maximum and hashlib.sha256(data).hexdigest() == expected_sha, 'upstream size or SHA256 mismatch')
    return data


def extract(data, target):
    expect(len(data) <= 64 * 1024 * 1024, 'compressed source bound exceeded')
    target.mkdir(mode=0o700)
    total = count = 0
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as archive:
        for member in archive:
            path = PurePosixPath(member.name)
            expect(not path.is_absolute() and path.parts and all(x not in ('.', '..') for x in path.parts), 'archive traversal refused')
            expect(member.isfile() or member.isdir(), 'archive links/devices refused')
            if len(path.parts) == 1:
                continue
            count += 1; total += member.size
            expect(count <= 12000 and total <= 128 * 1024 * 1024, 'unpacked source budget exceeded')
            destination = target.joinpath(*path.parts[1:])
            if member.isdir():
                destination.mkdir(mode=0o700, parents=True, exist_ok=True)
            else:
                destination.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
                fd = os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o644)
                with os.fdopen(fd, 'wb') as stream:
                    raw = archive.extractfile(member).read(member.size + 1)
                    expect(len(raw) == member.size, 'truncated source archive')
                    stream.write(raw)
    return {'files': count, 'unpacked_bytes': total}


def build(assets):
    manifest = read_json(assets / 'assets.json', 4 * 1024 * 1024)
    expect(manifest.get('build') is None, 'existing built assets are immutable; create fresh assets')
    instance = uuid.uuid4().hex[:12]
    for role, directory, command in (
        ('contracts', '/assets/contracts', 'yarn install --frozen-lockfile --non-interactive && yarn hardhat --config b3-hardhat.config.ts compile && FOUNDRY_PROFILE=yul forge build --use /assets/tools/solc-0.8.17'),
        ('scripts', '/assets/testnode/scripts', 'yarn install --frozen-lockfile --non-interactive && yarn build'),
    ):
        expect(all(sha(assets / p) == digest for p, digest in manifest['source_files'].items()), 'pinned source changed')
        name = 'b3-nitro-settlement-prep-' + instance + '-' + role
        args = ['create', '--name', name, '--label', LABEL + '=prep-' + instance,
                '--memory', '4g', '--memory-swap', '4g', '--cpus', '2', '--pids-limit', '512',
                '--cap-drop', 'ALL', '--security-opt', 'no-new-privileges', '--read-only',
                '--user', '1000:1000', '--tmpfs', '/tmp:rw,nosuid,nodev,size=536870912',
                '--log-opt', 'max-size=10m', '--log-opt', 'max-file=2',
                '--mount', 'type=bind,src=' + str(assets) + ',dst=/assets', '--workdir', directory]
        for key, value in {'HOME': '/assets/home', 'XDG_CACHE_HOME': '/assets/home/cache', 'YARN_CACHE_FOLDER': '/assets/home/yarn',
                           'npm_config_cache': '/assets/home/npm', 'PATH': '/assets/tools:/usr/local/bin:/usr/bin:/bin',
                           'NODE_OPTIONS': '--max-old-space-size=3072', 'npm_config_jobs': '2'}.items():
            args.extend(['--env', key + '=' + value])
        cid = docker(*args, NODE, '/bin/sh', '-c', command)
        print('Building ' + role + ' in ' + name, flush=True)
        try:
            docker('start', cid)
            deadline = time.monotonic() + 1800
            while True:
                state = json.loads(docker('inspect', cid))[0]['State']
                if not state['Running']:
                    (assets / (role + '-build.log')).write_text(docker_logs(cid) + '\n')
                    expect(state['ExitCode'] == 0 and not state['OOMKilled'], 'bounded helper failed; inspect retained build log')
                    break
                expect(time.monotonic() < deadline, '30-minute helper build deadline exceeded')
                size = int(subprocess.check_output(['/usr/bin/du', '-sb', str(assets)], text=True, timeout=30).split()[0])
                expect(size <= 16 * 1024 ** 3, '16 GiB asset budget exceeded')
                time.sleep(3)
            expect(all(sha(assets / p) == digest for p, digest in manifest['source_files'].items()), 'pinned source changed during build')
        finally:
            if json.loads(docker('inspect', cid))[0]['State']['Running']:
                docker('stop', '--time', '20', cid)
    manifest['build'] = {'contracts': True, 'scripts': True, 'mainnet_enabled': False,
                         'hardhat_config_adapter_sha256': sha(assets / 'contracts/b3-hardhat.config.ts'),
                         'reader4844_sha256': sha(assets / 'contracts/out/yul/Reader4844.yul/Reader4844.json')}
    write_json(assets / 'assets.json', manifest, fresh=False)
    assets_info(assets)


def prepare(parent, forge):
    expect(os.geteuid() == 0, 'explicit local root Docker required')
    parent, forge = safe(parent), safe(forge)
    expect(parent.is_dir() and parent.stat().st_uid == 0 and not parent.stat().st_mode & 0o077, 'root-owned private assets parent required')
    expect(',' not in str(parent) and '\n' not in str(parent) and sha(forge) == FORGE_SHA, 'pinned Forge or safe Docker bind path required')
    for image in (NODE, NITRO, GETH):
        docker('image', 'inspect', image)
    # Run build preparation before the full twelve-GiB test stack, never beside it.
    expect(not docker('ps', '--filter', 'label=' + LABEL, '--format', '{{.ID}}'), 'stop owned test roles before helper build allocation')
    suffix = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%d-') + uuid.uuid4().hex[:12]
    assets = parent / suffix; assets.mkdir(mode=0o700)
    manifest = {'format': 'b3-owned-nitro-settlement-assets/1', 'mainnet_enabled': False, 'directory': str(assets), 'upstream': {}}
    for relative, repository, commit, digest in ARCHIVES:
        data = download('https://codeload.github.com/OffchainLabs/' + repository + '/tar.gz/' + commit, digest, 64 * 1024 * 1024)
        target = assets / relative
        if relative.endswith('/precompiles') and target.exists():
            expect(target.is_dir() and not target.is_symlink() and not any(target.iterdir()), 'fresh empty gitlink required')
            target.rmdir()  # only this new archive's verified-empty gitlink placeholder
        key = 'precompiles' if relative.endswith('/precompiles') else relative
        manifest['upstream'][key] = {**extract(data, target), 'repository': 'OffchainLabs/' + repository,
                                     'commit': commit, 'archive_sha256': digest, 'archive_bytes': len(data)}
    tools = assets / 'tools'; tools.mkdir(mode=0o700)
    compiler = tools / 'solc-0.8.17'
    compiler.write_bytes(download('https://raw.githubusercontent.com/ethereum/solc-bin/gh-pages/linux-amd64/solc-linux-amd64-v0.8.17+commit.8df45f5f', SOLC_SHA, 32 * 1024 * 1024))
    compiler.chmod(0o755); shutil.copyfile(forge, tools / 'forge'); (tools / 'forge').chmod(0o755)
    manifest['tools'] = {'solc_version': '0.8.17', 'solc_sha256': SOLC_SHA, 'forge_version': '1.8.5', 'forge_sha256': FORGE_SHA}
    manifest['locks'] = {name: sha(assets / path) for name, path in (('testnode_yarn', 'testnode/scripts/yarn.lock'), ('contracts_yarn', 'contracts/yarn.lock'))}
    (assets / 'contracts/scripts/config.ts').touch(exist_ok=False)
    (assets / 'contracts/b3-hardhat.config.ts').write_text(OVERRIDE)
    (assets / 'home').mkdir(mode=0o700)
    manifest['source_files'] = {str(p.relative_to(assets)): sha(p) for name in ('contracts', 'testnode', 'tools') for p in (assets / name).rglob('*') if p.is_file()}
    write_json(assets / 'assets.json', manifest)
    # The new private tree contains verified regular archive members only.
    for path in [assets, *assets.rglob('*')]:
        expect(not path.is_symlink(), 'new asset symlink refused')
        os.chown(path, 1000, 1000)
    build(assets)
    print(str(assets), flush=True)
    return assets


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--assets-parent', type=Path)
    p.add_argument('--forge', type=Path)
    p.add_argument('--verify', type=Path, help='read-only check of existing built assets')
    a = p.parse_args()
    if a.verify:
        expect(a.assets_parent is None and a.forge is None, 'verify accepts no mutation arguments')
        assets, _ = assets_info(a.verify); print('Verified pinned built assets: ' + str(assets))
    else:
        expect(a.assets_parent is not None and a.forge is not None, 'assets-parent and forge required')
        prepare(a.assets_parent, a.forge)


if __name__ == '__main__':
    main()
