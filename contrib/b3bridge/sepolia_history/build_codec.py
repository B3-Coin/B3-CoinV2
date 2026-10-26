#!/usr/bin/env python3
"""One-shot bounded build of the offline maintained Nitro Geth serializer.

Run inside a2GiB/2CPU systemd scope. All new Go proxy/checksum-log response
bytes pass through the local128MiB budget; existing public caches are read-only.
There is no node initialization, wallet, chain RPC or snapshot downloader.
"""
import argparse
import hashlib
import http.server
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import threading
import time
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parent
MAX_DOWNLOAD = 128 * 1024 * 1024
COMMIT = '5eee8f546e7602b7deeef7909f291bbebc77e233'
MODULE = 'v0.0.0-20260916182944-5eee8f546e76'
MODULE_H1 = 'h1:KkMlxmymHsJXSUXubbnfgLVrScmG6vIBce9JKAfjMhU='


def require(ok, message):
    if not ok:
        raise ValueError(message)


class OfficialRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, msg, headers, newurl):
        parsed = urllib.parse.urlsplit(newurl)
        require(parsed.scheme == 'https' and parsed.hostname in ('proxy.golang.org', 'storage.googleapis.com')
                and parsed.username is None and parsed.password is None, 'nonofficial Go archive redirect refused')
        return super().redirect_request(request, fp, code, msg, headers, newurl)


class Budget:
    def __init__(self):
        self.used, self.items, self.lock = 0, [], threading.Lock()
        self.opener = urllib.request.build_opener(OfficialRedirect())

    def read(self, path):
        # net/http percent-escapes the exclamation markers used by Go's module
        # path case encoding. Decode that one allowed escape only.
        path = path.replace('%21', '!')
        parsed = urllib.parse.urlsplit(path)
        require(not parsed.query and not parsed.fragment and not parsed.netloc and parsed.path == path
                and '..' not in path.split('/') and re.fullmatch(r'/[A-Za-z0-9!_./@+~=-]+', path), 'Go proxy path refused')
        require('/@v/' in path or path.startswith('/sumdb/sum.golang.org/'), 'only versioned modules/checksum log allowed')
        request = urllib.request.Request('https://proxy.golang.org' + path,
                                         headers={'Accept-Encoding': 'identity', 'User-Agent': 'Go-http-client/1.1'})
        with self.opener.open(request, timeout=30) as response:
            require(response.status == 200 and response.headers.get('Content-Encoding', 'identity') == 'identity', 'Go proxy response refused')
            length = response.headers.get('Content-Length')
            if length is not None:
                require(length.isdecimal() and int(length) <= 64 * 1024 * 1024, 'individual module response exceeds64MiB')
            data = bytearray()
            while True:
                chunk = response.read(65536)
                if not chunk:
                    break
                with self.lock:
                    require(self.used + len(chunk) <= MAX_DOWNLOAD, 'new dependency download exceeds128MiB')
                    self.used += len(chunk)
                data.extend(chunk)
                require(len(data) <= 64 * 1024 * 1024, 'individual module response exceeds64MiB')
            with self.lock:
                self.items.append({'path': path, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
            return bytes(data)


def build(workspace, go, cached_proxy, offline=False):
    require(os.geteuid() != 0, 'build as the unprivileged local developer in the bounded scope')
    group = next(line.split(':', 2)[2] for line in Path('/proc/self/cgroup').read_text().splitlines() if line.startswith('0::'))
    limits = Path('/sys/fs/cgroup') / group.lstrip('/')
    memory = (limits / 'memory.max').read_text().strip()
    quota, period = (limits / 'cpu.max').read_text().split()
    require(memory.isdecimal() and int(memory) <= 2 * 1024 ** 3 and quota.isdecimal()
            and int(quota) <= 2 * int(period) and (limits / 'memory.swap.max').read_text().strip() == '0',
            'enforced2GiB/2CPU/no-swap scope required')
    workspace = workspace.absolute(); go = go.resolve(strict=True); cached_proxy = cached_proxy.resolve(strict=True)
    require(not workspace.exists() and workspace.parent.is_dir() and not workspace.parent.is_symlink(), 'fresh build directory required')
    shared_cache = '/go-modcache/cache/download' in str(cached_proxy)
    codec_cache = (str(cached_proxy).startswith('/home/josh/B3-tools/sepolia-history-codec-builds/')
                   and str(cached_proxy).endswith('/modules/cache/download'))
    require(cached_proxy.name == 'download' and (shared_cache or codec_cache), 'only existing public Go download cache allowed')
    if codec_cache:
        prior = json.loads((cached_proxy.parents[2] / 'build-manifest.json').read_text())
        require(prior['geth_commit'] == COMMIT and prior['geth_module_h1'] == MODULE_H1,
                'previous verified maintained codec cache required')
    workspace.mkdir(mode=0o700)
    source = workspace / 'source'; shutil.copytree(ROOT / 'codec', source)
    for name in ('cache', 'modules', 'home'):
        (workspace / name).mkdir(mode=0o700)
    budget = Budget()
    class Proxy(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass
        def do_GET(self):
            try:
                data = budget.read(self.path)
                self.send_response(200); self.send_header('Content-Type', 'application/octet-stream')
                self.send_header('Content-Length', str(len(data))); self.end_headers(); self.wfile.write(data)
            except urllib.error.HTTPError as error:
                self.send_error(error.code if error.code in (404,410) else 502)
            except Exception:
                self.send_error(502, 'bounded official dependency read refused')
    server = None
    if not offline:
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Proxy)
        thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
    environment = {'PATH': str(go.parent) + ':/usr/bin:/bin', 'HOME': str(workspace / 'home'), 'GOROOT': str(go.parent.parent),
                   'GOMODCACHE': str(workspace / 'modules'), 'GOCACHE': str(workspace / 'cache'), 'GOTOOLCHAIN': 'local',
                   'GOPROXY': cached_proxy.as_uri() if offline else cached_proxy.as_uri() + ',http://127.0.0.1:' + str(server.server_port),
                   # Offline builds verify the checked-in full go.sum against
                   # previously checksum-log-verified local cache archives.
                   'GOSUMDB': 'off' if offline else 'sum.golang.org', 'CGO_ENABLED': '0', 'GOMAXPROCS': '2', 'GOMEMLIMIT': '768MiB',
                   'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8', 'TZ': 'UTC'}
    binary = workspace / 'b3-sepolia-history-codec'
    try:
        with (workspace / 'build.log').open('wb') as log:
            process = subprocess.Popen([str(go), 'build', '-mod=readonly' if offline else '-mod=mod', '-p=2', '-trimpath', '-buildvcs=false', '-o', str(binary), '.'],
                                       cwd=source, env=environment, stdout=log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 900
            try:
                while process.poll() is None:
                    require(time.monotonic() < deadline, '15-minute codec build deadline exceeded')
                    used = int(subprocess.check_output(['/usr/bin/du', '-sb', str(workspace)], text=True, timeout=20).split()[0])
                    require(used <= 4 * 1024 ** 3, '4GiB codec build disk budget exceeded')
                    time.sleep(1)
                require(process.returncode == 0, 'bounded codec build failed; inspect retained build.log')
            finally:
                if process.poll() is None:
                    process.terminate(); process.wait(timeout=15)
        manifest = {'format': 'b3-maintained-nitro-history-codec-build/1', 'geth_commit': COMMIT, 'geth_module': MODULE,
                    'geth_module_h1': MODULE_H1, 'go_version': subprocess.check_output([str(go), 'version'], env=environment, text=True).strip(),
                    'binary': str(binary), 'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
                    'source_files': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted((ROOT / 'codec').glob('*')) if p.is_file()},
                    'resolved_go_mod_sha256': hashlib.sha256((source / 'go.mod').read_bytes()).hexdigest(),
                    'resolved_go_sum_sha256': hashlib.sha256((source / 'go.sum').read_bytes()).hexdigest(),
                    'download_budget_bytes': MAX_DOWNLOAD, 'downloaded_bytes': budget.used, 'downloads': budget.items,
                    'offline_build': offline,
                    'memory_cap_bytes': 2 * 1024 ** 3, 'cpu_cap': 2, 'cgo_enabled': False,
                    'mainnet_enabled': False, 'source_authority': False}
        (workspace / 'build-manifest.json').write_text(json.dumps(manifest, sort_keys=True, indent=2) + '\n')
        print(json.dumps({'binary': str(binary), 'sha256': manifest['binary_sha256'], 'downloaded_bytes': budget.used}), flush=True)
        return manifest
    finally:
        if server is not None:
            server.shutdown(); server.server_close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', type=Path, required=True); parser.add_argument('--go', type=Path, required=True)
    parser.add_argument('--cached-proxy', type=Path, required=True)
    parser.add_argument('--offline', action='store_true', help='only existing cache and checked-in full dependency sums; no network proxy')
    args = parser.parse_args(); build(args.workspace, args.go, args.cached_proxy, args.offline)


if __name__ == '__main__':
    main()
