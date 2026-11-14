#!/usr/bin/env python3
# Copyright (c) 2026 The B3 developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Four fm2d on regtest, end to end (stage 2 step 5, M5-NODE-01).

Runs fm2-keygen for four seats on free ports, starts four fm2d over one shared
dev chain file, waits for every seat's ready file, checks that heartbeat slots
are decided and that every seat publishes the same height, faucets a deposit
through fm2-devchain and checks the balance on EVERY seat, stops seat 3,
faucets another deposit while it is down, restarts seat 3 on its datadir and
checks that it catches up to the others' head and shows the second deposit,
then stops everything. Exit code 0 only if every step held.

REGTEST only: the dev chain file replaces the b3d adapter (stage 3). This
driver talks TLS without verifying the self-signed certificate (a test on
loopback); clients pin the SPKI fingerprint (fm2-cli, M5-CLI-01).
"""
import argparse
import hashlib
import http.client
import json
import os
import shutil
import signal
import socket
import ssl
import subprocess
import sys
import time


def free_port_base(count):
    for base in range(20000, 60000, 64):
        ok = True
        for p in range(base, base + count):
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            try:
                s.bind(("127.0.0.1", p))
            except OSError:
                ok = False
            finally:
                s.close()
            if not ok:
                break
        if ok:
            return base
    raise RuntimeError("no free port range")


def account_hex(label):
    # Any 32-byte id names an account for a deposit. A real account is AccountId::FromKey of a client's key
    # (fm2-cli, M5-CLI-01); the faucet only needs the same hex on the deposit and on the state query.
    return hashlib.sha256(label.encode()).hexdigest()


def get_json(port, path, timeout=5.0):
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    ctx.minimum_version = ssl.TLSVersion.TLSv1_3
    conn = http.client.HTTPSConnection("127.0.0.1", port, timeout=timeout, context=ctx)
    try:
        conn.request("GET", path)
        resp = conn.getresponse()
        body = resp.read().decode()
        return resp.status, json.loads(body) if body else None
    finally:
        conn.close()


class Cluster:
    def __init__(self, args, k=4):
        self.args = args
        self.k = k
        self.work = os.path.abspath(args.workdir)
        self.procs = {}
        self.logs = {}
        shutil.rmtree(self.work, ignore_errors=True)
        os.makedirs(self.work)
        self.mesh_base = free_port_base(k)
        self.api_base = free_port_base(k)
        while self.api_base == self.mesh_base:
            self.api_base = free_port_base(k) + 64
        self.keys = os.path.join(self.work, "keys")
        self.devchain = os.path.join(self.work, "devchain.log")
        run([args.keygen, "--out", self.keys, "--seats", str(k), "--mesh-port-base", str(self.mesh_base),
             "--api-port-base", str(self.api_base)])
        self.genesis = os.path.join(self.keys, "genesis.json")
        with open(self.genesis) as f:
            self.doc = json.load(f)

    def datadir(self, i):
        return os.path.join(self.work, f"seat{i}")

    def start(self, i):
        datadir = self.datadir(i)
        os.makedirs(datadir, exist_ok=True)
        ready = os.path.join(datadir, "fm2d.ready")
        if os.path.exists(ready):
            os.remove(ready)
        log = open(os.path.join(self.work, f"seat{i}.out"), "ab")
        self.logs[i] = log
        self.procs[i] = subprocess.Popen(
            [self.args.fm2d, "--datadir", datadir, "--genesis", self.genesis, "--seat", str(i),
             "--keys", os.path.join(self.keys, f"seat{i}"), "--devchain", self.devchain, "--log", "file",
             "--heartbeat-ms", "300"],
            stdout=log, stderr=subprocess.STDOUT)
        deadline = time.time() + 30
        while time.time() < deadline:
            if os.path.exists(ready):
                with open(ready) as f:
                    return json.load(f)
            if self.procs[i].poll() is not None:
                raise RuntimeError(f"seat {i} exited early with {self.procs[i].returncode}; see seat{i}.out")
            time.sleep(0.05)
        raise RuntimeError(f"seat {i} never became ready")

    def stop(self, i, timeout=20):
        proc = self.procs.pop(i, None)
        if proc is None:
            return 0
        proc.send_signal(signal.SIGTERM)
        try:
            code = proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            code = proc.wait()
            raise RuntimeError(f"seat {i} did not stop on SIGTERM")
        self.logs[i].close()
        return code

    def api_port(self, i):
        return self.api_base + i

    def health(self, i):
        status, body = get_json(self.api_port(i), "/v2/health")
        if status != 200:
            raise RuntimeError(f"seat {i} health {status}: {body}")
        return body

    def state(self, i, account):
        status, body = get_json(self.api_port(i), f"/v2/state/{account}")
        if status != 200:
            raise RuntimeError(f"seat {i} state {status}: {body}")
        return body

    def deposit(self, triples):
        cmd = [self.args.devchain, "--file", self.devchain, "--genesis", self.genesis, "deposit"]
        for account, asset, amount in triples:
            cmd += [account, asset, str(amount)]
        run(cmd)

    def wait(self, predicate, what, timeout=60):
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            try:
                last = predicate()
                if last:
                    return last
            except Exception as e:  # the API may not answer during a restart
                last = e
            time.sleep(0.25)
        raise RuntimeError(f"timeout waiting for {what}: last {last}")


def run(cmd):
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"{cmd[0]} failed ({result.returncode}): {result.stdout} {result.stderr}")
    return result.stdout


def balance_of(state_body, asset_hex):
    if state_body.get("status") != "FOUND":
        return None
    for b in state_body["witness"]["leaf"]["balances"]:
        if b["asset"] == asset_hex:
            return b["available"]
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--keygen", required=True)
    parser.add_argument("--devchain", required=True)
    parser.add_argument("--fm2d", required=True)
    parser.add_argument("--workdir", required=True)
    args = parser.parse_args()
    cluster = Cluster(args)
    steps = []
    try:
        for i in range(cluster.k):
            info = cluster.start(i)
            steps.append(f"seat {i} ready on api {info['api_port']} mesh {info['mesh_port']}")
        # Heartbeat slots: every seat publishes, and all publish the same height after a while.
        def heights():
            hs = [cluster.health(i).get("published_height") for i in range(cluster.k)]
            return hs if all(h is not None and h >= 3 for h in hs) else None
        hs = cluster.wait(heights, "three heartbeat slots on every seat", timeout=60)
        steps.append(f"heartbeats: heights {hs}")
        # A deposit through the faucet reaches every seat with the same balance.
        rusd = cluster.doc["assets"]["rUSD"]
        b3 = cluster.doc["assets"]["B3"]
        alice = account_hex("fm2/e2e/alice")
        bob = account_hex("fm2/e2e/bob")
        cluster.deposit([(alice, "rUSD", 1_000_000_000), (bob, "B3", 10_000_000_000)])

        def funded():
            seen = [balance_of(cluster.state(i, alice), rusd) for i in range(cluster.k)]
            return seen if all(v == 1_000_000_000 for v in seen) else None
        cluster.wait(funded, "alice's deposit on every seat", timeout=60)
        steps.append("deposit of block 130 settled on every seat")
        head_before = max(cluster.health(i)["published_height"] for i in range(cluster.k))
        # Seat 3 goes down; the rest keep deciding; a second deposit lands meanwhile.
        code = cluster.stop(3)
        if code != 0:
            raise RuntimeError(f"seat 3 exit code {code}")
        steps.append("seat 3 stopped cleanly")
        cluster.deposit([(alice, "rUSD", 5)])

        def others_advance():
            hs = [cluster.health(i)["published_height"] for i in range(3)]
            return hs if all(h >= head_before + 3 for h in hs) else None
        hs = cluster.wait(others_advance, "the three remaining seats to decide three more slots", timeout=60)
        steps.append(f"three seats went on without seat 3: heights {hs}")
        # Seat 3 restarts on its datadir: recovery, replay of the dev chain, catch-up to the others' head.
        info = cluster.start(3)
        if not info.get("restart"):
            raise RuntimeError("seat 3 did not recognise its datadir as a restart")
        steps.append(f"seat 3 restarted at head {info['head']}")

        def caught_up():
            hs = [cluster.health(i)["published_height"] for i in range(cluster.k)]
            return hs if hs[3] >= max(hs[:3]) - 1 and hs[3] >= head_before + 3 else None
        hs = cluster.wait(caught_up, "seat 3 to catch up", timeout=90)
        steps.append(f"seat 3 caught up: heights {hs}")
        if balance_of(cluster.state(3, alice), rusd) != 1_000_000_005:
            # The second deposit may still be in flight on seat 3's published view; wait for it.
            cluster.wait(lambda: balance_of(cluster.state(3, alice), rusd) == 1_000_000_005, "the second deposit on seat 3", timeout=30)
        steps.append("seat 3 shows both deposits")
        # Every seat agrees on alice's balance and bob's.
        for i in range(cluster.k):
            if balance_of(cluster.state(i, bob), b3) != 10_000_000_000:
                raise RuntimeError(f"seat {i} disagrees on bob's balance")
        steps.append("every seat agrees on both balances")
        codes = [cluster.stop(i) for i in range(cluster.k)]
        if any(c != 0 for c in codes):
            raise RuntimeError(f"exit codes {codes}")
        steps.append("every seat stopped cleanly")
    except Exception as e:
        for step in steps:
            print("ok:", step)
        print("FAILED:", e, file=sys.stderr)
        for i in list(cluster.procs):
            try:
                cluster.procs[i].kill()
            except Exception:
                pass
        for i in range(cluster.k):
            out = os.path.join(cluster.work, f"seat{i}.out")
            log = os.path.join(cluster.datadir(i), "debug.log")
            for path in (out, log):
                if os.path.exists(path):
                    with open(path, errors="replace") as f:
                        tail = f.read()[-3000:]
                    print(f"--- {path}\n{tail}", file=sys.stderr)
        return 1
    for step in steps:
        print("ok:", step)
    print(json.dumps({"ok": True, "steps": len(steps)}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
