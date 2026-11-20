#!/usr/bin/env python3
"""Build the small availability adapter against an exact Nimbus source tree.

Does not fetch a moving branch, install a node, or change B3 sources. Fetch the
documented Nimbus commit and its pinned submodules into a separate work tree
first. Build outputs and hashes are local evidence, never chain authority.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import selectors
import subprocess
import time

NIMBUS_COMMIT = "963756c91c19b7824cea57e7aab5857ce2c776c7"
FORMAT = "b3-nimbus-sepolia-gloas-collector/2"
RUNTIME_ENV = ("PATH", "HOME", "USER", "LOGNAME", "LANG", "LC_ALL", "TZ", "TMPDIR",
               "SYSTEMROOT", "WINDIR", "TEMP", "TMP")


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def clean_environment() -> dict[str, str]:
    return {name: os.environ[name] for name in RUNTIME_ENV if name in os.environ}


def bounded_version(binary: Path) -> dict:
    process = subprocess.Popen([str(binary), "--b3-version"], env=clean_environment(),
                               stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL)
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            output, deadline = bytearray(), time.monotonic() + 10
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0 or not selector.select(remaining):
                    raise ValueError("Peer backend version query timed out")
                data = os.read(process.stdout.fileno(), 4097 - len(output))
                if not data:
                    break
                output.extend(data)
                if len(output) > 4096:
                    raise ValueError("Peer backend version response exceeds its bound")
            if process.wait(timeout=2) != 0:
                raise ValueError("Peer backend version query failed")
            return json.loads(output)
    finally:
        if process.poll() is None:
            process.kill()
        process.wait(timeout=5)
        process.stdout.close()


def checked_source(path: Path) -> Path:
    root = path.resolve(strict=True)
    if not root.is_dir():
        raise ValueError("Nimbus source directory is missing")
    if (root / ".env").exists() or (root / ".env").is_symlink():
        raise ValueError("Nimbus local shell overrides are not permitted in the pinned build")
    def git(*args: str) -> str:
        return subprocess.check_output(["git", "-C", str(root), *args],
                                       env=clean_environment(), text=True).strip()
    if git("rev-parse", "HEAD") != NIMBUS_COMMIT:
        raise ValueError("Nimbus source commit does not match the reviewed release")
    if git("status", "--porcelain", "--untracked-files=no"):
        raise ValueError("Nimbus tracked sources or pinned submodules are modified")
    modules = subprocess.check_output(
        ["git", "-C", str(root), "submodule", "status", "--recursive"],
        env=clean_environment(), text=True).splitlines()
    if not modules or any(not line.startswith(" ") for line in modules):
        raise ValueError("Nimbus submodules are absent or do not match their pinned commits")
    return root


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nimbus-source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    if os.name != "posix":
        raise ValueError("Build the reviewed peer backend on Linux/WSL")
    root = checked_source(args.nimbus_source)
    output, report = args.output.absolute(), args.report.absolute()
    if output.exists() or report.exists():
        raise ValueError("Build output and report must be fresh paths")
    output.parent.mkdir(parents=True, exist_ok=True)
    report.parent.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).with_name("b3_gloas_peer_collector.nim")
    contents = source.read_bytes()
    # Keep every previously built adapter intact in the pinned upstream tree.
    # A source change gets a distinct untracked module and build cache.
    module = "b3_gloas_peer_collector_" + hashlib.sha256(contents).hexdigest()[:16] + ".nim"
    target = root / "beacon_chain" / module
    if target.exists() and (target.is_symlink() or target.read_bytes() != contents):
        raise ValueError("Refusing to replace a different Nimbus adapter source")
    if not target.exists():
        target.write_bytes(contents)
    log = report.with_suffix(report.suffix + ".build.log")
    if log.exists():
        raise ValueError("Build log must be a fresh path")
    env = clean_environment()
    with log.open("xb") as stream:
        subprocess.run(["make", "-j2", "USE_LIBBACKTRACE=0", "deps"], cwd=root,
                       env=env, stdout=stream, stderr=subprocess.STDOUT, check=True,
                       timeout=3600)
        subprocess.run([
            str(root / "env.sh"), "nim", "c", "--skipUserCfg", "-d:release", "--parallelBuild:2",
            "-d:disableLTO", "-d:disable_libbacktrace",
            "-d:chronicles_sinks=textlines[stderr]",
            "-d:chronicles_runtime_filtering=on", "--out:" + str(output),
            "beacon_chain/" + module], cwd=root, env=env,
            stdout=stream, stderr=subprocess.STDOUT, check=True, timeout=3600)
    checked_source(root)
    if target.read_bytes() != contents:
        raise ValueError("Adapter source changed while its backend was being built")
    version = bounded_version(output)
    if version != {"format": FORMAT, "upstream_commit": NIMBUS_COMMIT,
                   "authority": "availability-only"}:
        raise ValueError("Peer backend version does not match the reviewed adapter")
    result = {"format": "b3-gloas-peer-backend-build/1", "mainnet_enabled": False,
              "arbitrum_authority": False, "upstream_commit": NIMBUS_COMMIT,
              "adapter_sha256": hashlib.sha256(contents).hexdigest(),
              "backend_sha256": sha256(output), "version": version,
              "build_log_sha256": sha256(log)}
    report.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
