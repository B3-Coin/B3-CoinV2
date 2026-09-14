#!/usr/bin/env python3
"""Record actual extracted GUI child lifetime; coordinator handles attended UI."""
import argparse
from datetime import datetime, timezone
import fcntl
import hashlib
import http.client
import json
import os
from pathlib import Path
import signal
import ssl
import subprocess
import time
from urllib.parse import urlsplit


def save(path, obj):
    with path.open('x') as stream:
        json.dump(obj, stream, sort_keys=True, indent=2); stream.write('\n')

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--package-dir', required=True, type=Path)
    parser.add_argument('--session-dir', required=True, type=Path)
    parser.add_argument('--name', required=True)
    parser.add_argument('--reopen', action='store_true')
    parser.add_argument('--timeout', type=int, default=1200)
    args = parser.parse_args()
    os.umask(0o077)
    HERE = args.package_dir.resolve(strict=True)
    session_dir = args.session_dir.resolve(strict=True)
    readiness = json.loads((session_dir / 'ready.json').read_text())
    if not readiness.get('ready') or (session_dir / 'exit.json').exists():
        raise RuntimeError('The generated pilot must be running; expired/stopped endpoints are not usable')
    os.kill(readiness['parent_pid'], 0)
    END = datetime.fromisoformat(readiness['deadline_utc'].replace('Z', '+00:00')).timestamp()
    assert args.name.replace('-', '').isalnum() and 1 <= args.timeout <= 3600
    identity = json.loads((HERE / 'ARTIFACTS.json').read_text())
    candidate = identity['candidate']; exe = Path(identity['extracted_executable'])
    assert candidate['profile_ready'] and candidate['profile_id'] == readiness['profile_id']
    assert exe.is_relative_to(HERE / 'package-preparation/expanded') and not exe.is_symlink()
    assert hashlib.sha256((session_dir / 'enabled-profile.json').read_bytes()).hexdigest() == candidate['profile_sha256']
    profile = json.loads((session_dir / 'enabled-profile.json').read_text())
    context = ssl.create_default_context(cadata=profile['ca_pem'])
    for endpoint in profile['https_endpoints']:
        parsed = urlsplit(endpoint)
        if parsed.scheme != 'https' or parsed.hostname not in ('localhost', '127.0.0.1'):
            raise RuntimeError('Only declared loopback HTTPS endpoints are allowed')
        healthy = False
        for attempt in range(3):
            connection = http.client.HTTPSConnection(parsed.hostname, parsed.port, context=context, timeout=5)
            try:
                connection.request('POST', parsed.path, json.dumps({'method':'markets','params':{}}),
                                   {'Content-Type':'application/json'})
                response = connection.getresponse()
                body = response.read(1024 * 1024 + 1)
                if len(body) <= 1024 * 1024 and response.status == 200 and json.loads(body).get('ok'):
                    healthy = True
                    break
            finally:
                connection.close()
            if attempt < 2:
                time.sleep(.25) # Bounded read-only preflight, not a runtime/performance change.
        if not healthy:
            raise RuntimeError('Endpoint is not currently usable; no Qt instance launched')
    assert hashlib.sha256(exe.read_bytes()).hexdigest() == candidate['executable_sha256']
    assert time.time() < END - 150, 'Insufficient attended/graceful-cleanup window; no new launch'
    data = Path.home() / 'Library/Application Support/B3FlowMeshClosedTest' / candidate['profile_id']
    assert not data.is_symlink()
    assert data.exists() == args.reopen, 'Unexpected first-launch/reopen data state; review instead of deleting'
    launcher_lock = data / 'launcher.lock'
    lock_check = 'not_present_first_launch'
    if args.reopen:
        assert launcher_lock.is_file() and not launcher_lock.is_symlink()
        # Read-only inspection of the existing guard. Do not create or delete it.
        with launcher_lock.open('rb') as stream:
            fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            fcntl.flock(stream.fileno(), fcntl.LOCK_UN)
        lock_check = 'existing_launcher_lock_exclusive_probe_passed_and_released'
    ps = subprocess.check_output(['/bin/ps', '-axo', 'pid=,comm='], text=True)
    owners = [line for line in ps.splitlines() if str(exe) in line]
    assert not owners, 'Exact candidate is already running; do not duplicate'
    record = HERE / 'native-captures' / args.name
    record.mkdir(parents=True, exist_ok=False)
    debug = data / 'node/regtest/debug.log'
    before_size = debug.stat().st_size if debug.exists() else 0
    env = dict(os.environ)
    removed = [key for key in env if key.startswith(('DYLD_', 'QT_', 'QML_'))]
    for key in removed: del env[key]
    timeout = min(args.timeout, END - time.time() - 120)
    with (record / 'stdout.log').open('x') as out, (record / 'stderr.log').open('x') as err:
        child = subprocess.Popen([str(exe)], cwd=exe.parent, env=env, stdout=out,
                                 stderr=err, start_new_session=True)
        started = dict(pid=child.pid, argv=[str(exe)], started_at=time.time(),
            executable_sha256=candidate['executable_sha256'], data_path=str(data),
            scenario=args.name, reopen=args.reopen, timeout_seconds=timeout,
            before_debug_bytes=before_size, previous_exact_owners=owners,
            launcher_lock_ownership_check=lock_check,
            graceful_cleanup_reserve_seconds=120,
            removed_diagnostic_environment_names=removed)
        save(record / 'started.json', started); print(json.dumps(started), flush=True)
        timeout_cleanup = False
        cleanup_signals = []
        cleanup_started = None
        def request_cleanup(number, frame):
            nonlocal cleanup_started
            cleanup_signals.append(number)
            if child.poll() is None and cleanup_started is None:
                cleanup_started = time.time(); child.terminate()
        signal.signal(signal.SIGTERM, request_cleanup)
        signal.signal(signal.SIGINT, request_cleanup)
        interaction_deadline = time.time() + timeout
        cleanup_notice = False
        while child.poll() is None:
            now = time.time()
            if now >= interaction_deadline and cleanup_started is None:
                timeout_cleanup = True; cleanup_started = now; child.terminate()
            if cleanup_started is not None and now - cleanup_started >= 90 and not cleanup_notice:
                pending = dict(event='graceful_cleanup_still_pending', pid=child.pid,
                    observed_at=now, actual_child_exit_status=None, child_reaped=False,
                    cleanup_elapsed_seconds=now-cleanup_started,
                    action_required='Coordinator must inspect and perform targeted test-child cleanup; no automatic SIGKILL',
                    session_end_utc=readiness['deadline_utc'])
                save(record / 'cleanup-pending.json', pending)
                print(json.dumps(pending), flush=True)
                cleanup_notice = True
            time.sleep(0.25)
        status = child.wait()
    # The direct child is reaped before final recording; never infer status
    # from a vanished PID or a launcher killed before it records its child.
    tail = b''
    if debug.exists():
        with debug.open('rb') as stream:
            if debug.stat().st_size >= before_size: stream.seek(before_size)
            tail = stream.read()
    with (record / 'new-debug.log').open('xb') as stream: stream.write(tail)
    ps = subprocess.check_output(['/bin/ps', '-axo', 'pid=,comm='], text=True)
    remaining = [line for line in ps.splitlines() if str(exe) in line]
    stderr = (record / 'stderr.log').read_text(errors='replace')
    result = dict(actual_child_exit_status=status, child_reaped=True,
        timed_out_cleanup=timeout_cleanup, finished_at=time.time(),
        capture_cleanup_signals=cleanup_signals,
        cleanup_required_operator_attention=cleanup_notice,
        new_shutdown_done=b'Shutdown done' in tail,
        gui_main_exit_recorded='GuiMain exit status=' in stderr,
        remaining_exact_candidate_processes=remaining,
        clean_runtime_exit=status == 0 and not timeout_cleanup and not cleanup_signals and b'Shutdown done' in tail and not remaining,
        ui_quit_qualification='Requires separate attended input evidence; this record alone does not prove UI Quit',
        data_preserved=data.exists(), executable_sha256=hashlib.sha256(exe.read_bytes()).hexdigest())
    save(record / 'result.json', result); print(json.dumps(result), flush=True)

if __name__ == '__main__':
    main()
