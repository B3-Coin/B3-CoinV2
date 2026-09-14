#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""One bounded, private, generated-data qualification pass; no build or UI launch.

Example (after freezing and building the reviewed source):
  python3 contrib/closed-test/run-qualification.py --build-dir /abs/build \
      --output-dir /abs/new-private-evidence --source-manifest /abs/source.json --run

Without --run this prints a plan only. --only selects a named affected group
for a separately labelled rerun; it never overwrites an earlier result. This
does not qualify native screens, power loss, WAN, or public deployment.
"""
import argparse
import configparser
from datetime import datetime, timezone
import hashlib
import importlib.util
import ipaddress
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import time

# Verification and fixture imports must not add bytecode to the frozen source.
sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[2]
CORE_FLOWMESH = ",".join((
    "flowmesh_runtime_tests", "flowmesh_net_tests", "flowmesh_service_tests",
    "flowmesh_transport_policy_tests", "flowmesh_service_startup_tests",
    "flowmesh_transport_compat_tests", "flowmesh_checkpoint_codec_tests",
    "flowmesh_seat_binding_tests", "flowmesh_fee_allocation_tests"))
CORE_CLIENT = "flowmesh_https_tests,flowmesh_client_evidence_tests,wallet_rpc_tests"
CORE_WALLET = ("legacy_wallet_dump_tests,walletdb_tests:"
               "wallet_tests/b3_validator_key_and_stake_outputs:"
               "mempool_tests/MempoolCheckUsesRepresentableHeight")
QT_TARGETS = ("test_b3_flowmeshtrading-qt", "test_b3_flowmeshworkspace-qt",
              "test_b3_flowmeshtradingpanel-qt", "test_b3_stakecoincontrol-qt",
              "test_b3_splash-qt")
GROUPS = ("flowmesh-core", "client-https-core", "wallet-core", *QT_TARGETS,
          "reconciliation-unit", "reconciliation-functional", "ordinary-functional")
FIXTURES = {
    "reconciliation-functional": ("wallet_readonly_reconcile.py", "WalletReadOnlyReconcile"),
    "ordinary-functional": ("feature_flowmesh_remote_client.py", "FlowMeshRemoteClientTest"),
}


def utc():
    return datetime.now(timezone.utc).isoformat()


def require(value, reason):
    if not value:
        raise RuntimeError(reason)


def digest(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def write_json(path, value):
    # Every result belongs to an exclusively created private evidence directory.
    with Path(path).open("x") as target:
        json.dump(value, target, indent=2, sort_keys=True)
        target.write("\n")


def git(*args):
    return subprocess.check_output(["git", "-C", str(SOURCE), *args], text=True).strip()


def verify_export(manifest_path):
    """Reuse the packager's byte/mode/file-set verification; never discover git."""
    require(manifest_path.is_file() and not manifest_path.is_relative_to(SOURCE),
            "Source manifest must be a preserved file outside the exported source")
    document = json.loads(manifest_path.read_text())
    require(re.fullmatch(r"[0-9a-f]{64}", document.get("source_tree_sha256", "")),
            "Source manifest lacks an exact source_tree_sha256")
    require(isinstance(document.get("files"), list) and document["files"], "Source manifest file list is empty")
    paths = [row["path"] for row in document["files"]]
    require(len(paths) == len(set(paths)), "Duplicate source manifest paths")
    spec = importlib.util.spec_from_file_location("closed_package_verifier", SOURCE / "contrib/closed-test/package-mac.py")
    verifier = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(verifier)
    verifier.verify_export(SOURCE, document)
    return {"source_commit": None, "source_tree_sha256": document["source_tree_sha256"],
            "source_manifest": str(manifest_path), "source_manifest_sha256": digest(manifest_path),
            "source_provenance": "manifest-verified frozen source export; uncommitted, NOT clean-checkout qualification"}


def group_members(group):
    output = subprocess.check_output(["ps", "-eo", "pid=,pgid="], text=True)
    return [int(pid) for line in output.splitlines() if len(parts := line.split()) == 2
            for pid, pgid in [parts] if int(pgid) == group]


def bounded_process(command, directory, environment, timeout):
    """A process session created here is the only timeout/cleanup signal target."""
    directory.mkdir(mode=0o700)
    record = {"command": command, "cwd": str(SOURCE), "start_utc": utc(),
              "timeout_seconds": timeout, "signals": [], "timed_out": False,
              "command_executable_sha256": digest(command[0])}
    write_json(directory / "planned.json", record)
    with (directory / "output.log").open("xb") as output:
        child = subprocess.Popen(command, cwd=SOURCE, env=environment,
                                 stdout=output, stderr=subprocess.STDOUT,
                                 start_new_session=True)
        record["pid"] = child.pid
        record["process_group"] = child.pid
        try:
            child.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            record["timed_out"] = True
        except KeyboardInterrupt:
            record["interrupted"] = True
        finally:
            # Includes generated daemon descendants left after framework failure.
            # Never signal by executable basename or an existing user process.
            for signum, grace in ((signal.SIGINT, 60), (signal.SIGTERM, 15), (signal.SIGKILL, 5)):
                members = group_members(child.pid)
                if not members:
                    break
                record["signals"].append({"signal": signum.name, "utc": utc(), "owned_group_pids": members})
                try:
                    os.killpg(child.pid, signum)
                except ProcessLookupError:
                    break
                deadline = time.monotonic() + grace
                while time.monotonic() < deadline:
                    child.poll()
                    if not group_members(child.pid):
                        break
                    time.sleep(0.1)
            child.poll()
            record["exit_code"] = child.returncode
            record["reaped"] = child.returncode is not None
    record["finish_utc"] = utc()
    record["remaining_owned_pids"] = group_members(child.pid)
    record["output_sha256"] = digest(directory / "output.log")
    record["command_executable_unchanged"] = digest(command[0]) == record["command_executable_sha256"]
    record["passed"] = (record["exit_code"] == 0 and record["reaped"] and
                        not record["timed_out"] and not record.get("interrupted") and
                        not record["signals"] and not record["remaining_owned_pids"] and
                        record["command_executable_unchanged"])
    write_json(directory / "process-result.json", record)
    return record


def loopback(value):
    if value.startswith("["):
        host = value[1:value.index("]")]
    else:
        host = value.split(":", 1)[0]
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


def fixture_child(group, run, build, framework_args):
    """Observe unchanged fixtures; constrain only their generated process launch."""
    run, build = Path(run).resolve(), Path(build).resolve()
    require(run.is_dir() and (run / "planned.json").is_file(), "Missing parent-owned run plan")
    require((run / "fixture").exists() is False, "Refusing reused functional data")
    require(group in FIXTURES, "Unknown fixture")
    sys.path.insert(0, str(SOURCE / "test/functional"))
    from test_framework.test_node import TestNode
    from test_framework.util import p2p_port
    original_start = TestNode.start
    original_stopped = TestNode.is_node_stopped
    original_rpc = TestNode.wait_for_rpc_connection
    records, nodes = [], []
    lifecycle = run / "node-lifecycle.jsonl"

    def event(kind, record, **extra):
        with lifecycle.open("a") as output:
            output.write(json.dumps({"event": kind, "utc": utc(), "pid": record["pid"],
                                     "node": record["node"], **extra}, sort_keys=True) + "\n")

    def capture_exit(record):
        process = record["process"]
        if process.poll() is None or "exit_code" in record:
            return
        record["exit_code"] = process.wait(timeout=0)
        debug = record["debug_path"]
        end = debug.stat().st_size if debug.exists() else 0
        record["debug_end_offset"] = end
        segment_digest = hashlib.sha256()
        unsafe_warning = False
        with debug.open("rb") if debug.exists() else open(os.devnull, "rb") as stream:
            stream.seek(record["debug_start_offset"])
            remaining = max(0, end - record["debug_start_offset"])
            carry = b""
            while remaining:
                chunk = stream.read(min(1024 * 1024, remaining))
                if not chunk:
                    break
                segment_digest.update(chunk)
                unsafe_warning |= b"SQLite is configured to not wait for data to be flushed" in carry + chunk
                carry = chunk[-256:]
                remaining -= len(chunk)
        record["debug_segment_sha256"] = segment_digest.hexdigest()
        record["unsafe_sqlite_warning"] = unsafe_warning
        with debug.open("rb") if debug.exists() else open(os.devnull, "rb") as stream:
            stream.seek(max(record["debug_start_offset"], end - 1024 * 1024))
            tail = stream.read(1024 * 1024).decode(errors="replace")
        record["shutdown_done"] = "Shutdown done" in tail or "Shutdown: done" in tail
        event("process_exited", record, exit_code=record["exit_code"],
              shutdown_done=record["shutdown_done"], debug_end_offset=end)

    def start(node, extra_args=None, **kwargs):
        require(node.chain == "regtest", "Only generated regtest nodes are allowed")
        data = Path(node.datadir_path).resolve()
        require(data.is_relative_to(run / "fixture") or data.is_relative_to(run / "cache"),
                "Node datadir escapes the new generated fixture/cache")
        require(Path(node.args[0]).resolve() == build / "bin/b3coind", "Unexpected daemon executable")
        require(not kwargs.get("start_new_session"), "Node must remain in parent-owned test process group")
        for record in records:
            capture_exit(record)
            require(not (record["datadir"] == str(data) and record["process"].poll() is None),
                    "Duplicate live datadir owner")
        args = list((node.extra_args if extra_args is None else extra_args) or [])
        # Never alter a fixture's quorum, deadline, action or signed object.
        # Durability is intentionally stronger than initialize_datadir's default.
        args += ["-unsafesqlitesync=0", "-rpcbind=127.0.0.1", "-rpcallowip=127.0.0.1",
                 "-discover=0", "-dnsseed=0", "-fixedseeds=0", "-natpmp=0", "-listenonion=0"]
        if not node.has_explicit_bind and not any(arg.startswith("-bind=") for arg in args):
            args.append(f"-bind=127.0.0.1:{p2p_port(node.index)}")
        config = node.bitcoinconf.read_text()
        # The only config is freshly produced by these established fixtures.
        # Reject external includes/storage/network exposure rather than editing it.
        for line in config.splitlines():
            if not line.strip() or line.lstrip().startswith(("#", "[")) or "=" not in line:
                continue
            key, value = line.strip().split("=", 1)
            require(key not in ("includeconf", "walletdir", "blocksdir", "rpccookiefile"),
                    "External config/storage override is not allowed")
            if key in ("bind", "rpcbind", "flowmeshbind", "flowmeshapibind"):
                require(loopback(value), "Nonloopback fixture configuration")
        for arg in [*node.args, *args]:
            if arg.startswith(("-bind=", "-rpcbind=", "-flowmeshbind=", "-flowmeshapibind=", "-flowmeshconnect=")):
                require(loopback(arg.split("=", 1)[1]), "Nonloopback fixture argument")
        debug = data / "regtest/debug.log"
        offset = debug.stat().st_size if debug.exists() else 0
        result = original_start(node, args, **kwargs)
        if node not in nodes:
            nodes.append(node)
        record = {"node": node.index, "datadir": str(data), "pid": node.process.pid,
                  "process": node.process, "argv": list(node.process.args),
                  "daemon_sha256": digest(build / "bin/b3coind"), "debug_path": debug,
                  "debug_start_offset": offset, "startup_durability_verified": False}
        records.append(record)
        event("process_started", record, argv=record["argv"], datadir=str(data),
              daemon_sha256=record["daemon_sha256"], debug_start_offset=offset)
        return result

    def wait_rpc(node, *args, **kwargs):
        result = original_rpc(node, *args, **kwargs)
        record = next(row for row in reversed(records) if row["process"] is node.process)
        with record["debug_path"].open("rb") as source:
            source.seek(record["debug_start_offset"])
            startup = source.read(1024 * 1024).decode(errors="replace")
        settings = [line for line in startup.splitlines() if "unsafesqlitesync=" in line]
        observed = any(re.search(r'Command-line arg: unsafesqlitesync="?0"?(?:\s|$)', line) for line in settings)
        unsafe = "SQLite is configured to not wait for data to be flushed" in startup
        record["startup_durability_verified"] = observed and not unsafe
        event("rpc_started", record, durability_settings=settings,
              startup_durability_verified=record["startup_durability_verified"])
        require(record["startup_durability_verified"], "Normal SQLite durability not established in startup log")
        return result

    def stopped(node, *args, **kwargs):
        try:
            return original_stopped(node, *args, **kwargs)
        finally:
            for record in records:
                capture_exit(record)

    TestNode.start, TestNode.wait_for_rpc_connection, TestNode.is_node_stopped = start, wait_rpc, stopped
    filename, classname = FIXTURES[group]
    path = SOURCE / "test/functional" / filename
    module_spec = importlib.util.spec_from_file_location("closed_qualification_fixture", path)
    module = importlib.util.module_from_spec(module_spec)
    module_spec.loader.exec_module(module)
    sys.argv = [str(path), *framework_args]
    fixture = getattr(module, classname)(str(path))
    code = 1
    try:
        fixture.main()
    except SystemExit as exc:
        code = exc.code if isinstance(exc.code, int) else 1
    finally:
        for node in nodes:
            if node.process and node.process.poll() is None:
                process = node.process
                try:
                    node.stop_node()
                except Exception as exc:
                    # Parent timeout cleanup owns remaining generated processes.
                    record = next(row for row in reversed(records) if row["process"] is process)
                    event("cleanup_error", record, error_type=type(exc).__name__)
        public = []
        for record in records:
            capture_exit(record)
            public.append({key: str(value) if isinstance(value, Path) else value
                           for key, value in record.items() if key != "process"})
        clean = bool(records) and all(row.get("exit_code") == 0 and row.get("shutdown_done") and
                                     row["startup_durability_verified"] and not row.get("unsafe_sqlite_warning")
                                     for row in public)
        summary = {"framework_exit": code, "all_nodes_clean": clean, "nodes": public,
                   "native_ui_qualified": False, "power_loss_qualified": False,
                   "unchanged_fixture_assertions": True,
                   "normal_sqlite_durability_required_on_every_start": True}
        write_json(run / "fixture-result.json", summary)
    return code if code else (0 if clean else 1)


def select_ports():
    # Each group runs serially. Only loopback bind probes; no service starts here.
    for seed in range(41, 141):
        base = 25000 + (12 * seed) % (5000 - 1 - 12)
        ports = [base + section + offset for section in (0, 5000, 10000) for offset in range(13)]
        held = []
        try:
            for port in ports:
                sock = socket.socket()
                held.append(sock)
                sock.bind(("127.0.0.1", port))
            return seed, ports
        except OSError:
            pass
        finally:
            for sock in held:
                sock.close()
    raise RuntimeError("No free bounded loopback port range found")


def parent(args):
    build, output = args.build_dir.resolve(), args.output_dir.resolve()
    selected = args.only or list(GROUPS)
    require(len(selected) == len(set(selected)), "Each group may appear only once; use a separate evidence identity for reruns")
    require(not output.exists(), "Output already exists; preserve it and choose a new run identity")
    require(not output.is_relative_to(SOURCE), "Evidence must not be written into the reviewed source tree")
    require(output != output.parent, "A filesystem root is not an evidence directory")
    config_path = build / "test/config.ini"
    config = configparser.ConfigParser()
    config.read(config_path)
    require(config.has_section("environment"), "Missing clean-build functional config")
    require(Path(config["environment"]["SRCDIR"]).resolve() == SOURCE, "Build config references a different source tree")
    require(Path(config["environment"]["BUILDDIR"]).resolve() == build, "Build config references another build directory")
    if args.source_manifest:
        source_identity = verify_export(args.source_manifest.resolve(strict=True))
    else:
        require(Path(git("rev-parse", "--show-toplevel")).resolve() == SOURCE,
                "Source is not a standalone checkout; use --source-manifest for a frozen export")
        require(not git("status", "--porcelain", "--untracked-files=all"), "Qualification source must be fully committed and clean")
        source_identity = {"source_commit": git("rev-parse", "HEAD"), "source_tree": git("rev-parse", "HEAD^{tree}"),
                           "source_provenance": "clean committed checkout"}
    binaries = [build / "bin/test_bitcoin", build / "bin/b3coind", build / "bin/b3coin-cli"]
    binaries += [build / "bin" / target for target in QT_TARGETS if target in selected]
    for binary in binaries:
        require(binary.is_file(), f"Required built target missing: {binary.name}")
    identity = {**source_identity,
                "source_directory": str(SOURCE), "build_directory": str(build),
                "build_config_sha256": digest(config_path), "helper_sha256": digest(__file__),
                "binaries": {str(binary.relative_to(build)): digest(binary) for binary in binaries},
                "groups": selected, "private_generated_evidence_only": True}
    if not args.run:
        print(json.dumps({"plan_only": True, **identity}, indent=2))
        return 0
    os.umask(0o077)
    output.mkdir(parents=True, mode=0o700, exist_ok=False)
    write_json(output / "identity.json", identity)
    environment = os.environ.copy()
    for key in list(environment):
        if key.startswith(("BITCOIN", "B3_")) or key in ("PYTHONPATH", "PREVIOUS_RELEASES_DIR"):
            environment.pop(key)
    environment.update(PYTHONDONTWRITEBYTECODE="1", QT_QPA_PLATFORM="minimal",
                       BITCOIND=str(build / "bin/b3coind"), BITCOINCLI=str(build / "bin/b3coin-cli"),
                       TEST_RUNNER_PORT_MIN="25000")
    results = []
    for group in selected:
        directory = output / group
        timeout = 180
        if group in ("flowmesh-core", "client-https-core", "wallet-core"):
            suites = {"flowmesh-core": CORE_FLOWMESH, "client-https-core": CORE_CLIENT, "wallet-core": CORE_WALLET}[group]
            command = [str(build / "bin/test_bitcoin"), f"--run_test={suites}", "--log_level=test_suite",
                       "--report_level=detailed", "--color_output=no", "--",
                       f"-testdatadir={directory / 'generated-data'}", "-unsafesqlitesync=0"]
        elif group in QT_TARGETS:
            command = [str(build / "bin" / group)]
        elif group == "reconciliation-unit":
            command = [sys.executable, str(SOURCE / "contrib/test_wallet_reconcile.py")]
        else:
            seed, ports = select_ports()
            command = [sys.executable, str(Path(__file__).resolve()), "--fixture-child", group,
                       str(directory), str(build), "--", f"--configfile={config_path}",
                       f"--tmpdir={directory / 'fixture'}", f"--cachedir={directory / 'cache'}",
                       "--nocleanup", f"--portseed={seed}", "--randomseed=120913", "--timeout-factor=1"]
            if group == "ordinary-functional":
                command.append("--check-default-off")
            timeout = 900
        print(json.dumps({"event": "selection_started", "group": group, "utc": utc()}), flush=True)
        result = bounded_process(command, directory, environment, timeout)
        if group in FIXTURES:
            path = directory / "fixture-result.json"
            result["fixture_summary_present"] = path.is_file()
            result["passed"] = result["passed"] and path.is_file()
            if path.is_file():
                observation = json.loads(path.read_text())
                result["passed"] = result["passed"] and observation["all_nodes_clean"] and observation["framework_exit"] == 0
        write_json(directory / "result.json", result)
        results.append({"group": group, "passed": result["passed"], "exit_code": result["exit_code"],
                        "result_path": str(directory / "result.json")})
        print(json.dumps({"event": "selection_finished", **results[-1], "utc": utc()}), flush=True)
        if not result["passed"]:
            break
    if args.source_manifest:
        try:
            source_unchanged = verify_export(args.source_manifest.resolve(strict=True)) == source_identity
        except (RuntimeError, OSError, ValueError, KeyError):
            source_unchanged = False
    else:
        source_unchanged = (identity["source_commit"] == git("rev-parse", "HEAD") and
                            not git("status", "--porcelain", "--untracked-files=all"))
    unchanged = (source_unchanged and
                 all(digest(build / name) == value for name, value in identity["binaries"].items()) and
                 digest(config_path) == identity["build_config_sha256"] and digest(__file__) == identity["helper_sha256"])
    passed = len(results) == len(selected) and all(row["passed"] for row in results) and unchanged
    write_json(output / "summary.json", {"passed": passed, "identity_unchanged": unchanged, "results": results,
               "unexecuted_groups": selected[len(results):], "native_ui_qualified": False,
               "new_power_loss_or_wan_claim": False, "public_release_approved": False})
    return 0 if passed else 1


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--fixture-child":
        require(len(sys.argv) > 6 and sys.argv[5] == "--", "Invalid internal fixture invocation")
        sys.exit(fixture_child(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[6:]))
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--only", nargs="+", choices=GROUPS)
    parser.add_argument("--source-manifest", type=Path,
                        help="Preserved manifest of a frozen uncommitted export; never claim clean-checkout qualification")
    parser.add_argument("--run", action="store_true")
    try:
        sys.exit(parent(parser.parse_args()))
    except RuntimeError as exc:
        print(json.dumps({"error": str(exc), "passed": False}), file=sys.stderr)
        sys.exit(1)
