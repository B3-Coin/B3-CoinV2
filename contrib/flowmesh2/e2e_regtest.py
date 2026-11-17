#!/usr/bin/env python3
# Copyright (c) 2026 The B3 developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Four fm2d on regtest, end to end (stage 2 steps 5 and 6, M5-NODE-01 and M5-CLI-01).

Runs fm2-keygen for four seats on free ports, starts four fm2d over one shared
dev chain file, waits for every seat's ready file, checks that heartbeat slots
are decided and that every seat publishes the same height, makes two client
keys with fm2-cli, faucets deposits to their accounts through fm2-devchain and
checks the balances on EVERY seat, refuses a second fm2d on a live datadir
at the journal's directory lock, trades through fm2-cli (bob's ask rests,
alice's bid fills it, both verified with b3verify by the client and settled
on every seat; a resend answers DUPLICATE everywhere; the outbox and a
verified slot stream agree), stops seat 3, faucets another deposit while it
is down, lets the others decide twenty more slots (more than the old
catch-up ring held), restarts seat 3 on its datadir and checks that it
catches up to the others' head and shows the deposit and the trade, then
stops everything. Exit
code 0 only if every step held.

REGTEST only: the dev chain file replaces the b3d adapter (stage 3). This
driver's own reads talk TLS without verifying the self-signed certificate (a
test on loopback); fm2-cli pins each seat's SPKI fingerprint from its ready
file.
"""
import argparse
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
        self.pins = os.path.join(self.work, "pins.json")
        self.fingerprints = {}

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
                    info = json.load(f)
                self.fingerprints[str(i)] = info["fingerprint"]
                with open(self.pins, "w") as f:
                    json.dump(self.fingerprints, f)
                return info
            if self.procs[i].poll() is not None:
                raise RuntimeError(f"seat {i} exited early with {self.procs[i].returncode}; see seat{i}.out")
            time.sleep(0.05)
        raise RuntimeError(f"seat {i} never became ready")

    def start_duplicate(self, i, timeout=20):
        """A second fm2d on seat i's live datadir: must exit non-zero at the journal lock, before touching anything.
        Returns (exit code, output)."""
        result = subprocess.run(
            [self.args.fm2d, "--datadir", self.datadir(i), "--genesis", self.genesis, "--seat", str(i),
             "--keys", os.path.join(self.keys, f"seat{i}"), "--devchain", self.devchain, "--log", "console",
             "--heartbeat-ms", "300"],
            capture_output=True, text=True, timeout=timeout)
        return result.returncode, result.stdout + result.stderr

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

    def cli(self, who, command, expect_code=0, timeout=90):
        """fm2-cli as client `who` (its key and outbox under the work dir); the JSON object it printed."""
        cmd = [self.args.cli, "--genesis", self.genesis, "--key", os.path.join(self.work, f"{who}.key"),
               "--outbox", os.path.join(self.work, f"{who}.outbox"), "--pins", self.pins] + command
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        if result.returncode != expect_code:
            raise RuntimeError(f"fm2-cli {' '.join(command)} exited {result.returncode} (want {expect_code}): {result.stdout} {result.stderr}")
        lines = [line for line in result.stdout.splitlines() if line.strip()]
        if not lines:
            raise RuntimeError(f"fm2-cli {' '.join(command)} printed nothing: {result.stderr}")
        return json.loads(lines[-1])

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
    parser.add_argument("--cli", required=True)
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
        # A second fm2d on seat 0's live datadir is refused at the signer journal's directory lock (review of
        # 2026-11-16, defect 6), before it can read, repair or sign anything; seat 0 goes on undisturbed.
        code, output = cluster.start_duplicate(0)
        if code == 0 or "JOURNAL_DIR_LOCKED" not in output:
            raise RuntimeError(f"a duplicate fm2d on seat 0's datadir: exit {code}, output {output[-600:]}")
        if cluster.health(0).get("published_height", 0) < hs[0]:
            raise RuntimeError("seat 0 lost height after the duplicate was refused")
        steps.append("a second fm2d on seat 0's datadir was refused at the journal lock")
        # Two clients: their accounts follow their keys (AccountId::FromKey).
        alice = cluster.cli("alice", ["keygen"])["account"]
        bob = cluster.cli("bob", ["keygen"])["account"]
        if cluster.cli("alice", ["account"])["account"] != alice:
            raise RuntimeError("alice's account does not follow her key")
        steps.append("two client keys made")
        # A deposit through the faucet reaches every seat with the same balance.
        rusd = cluster.doc["assets"]["rUSD"]
        b3 = cluster.doc["assets"]["B3"]
        cluster.deposit([(alice, "rUSD", 1_000_000_000), (bob, "B3", 10_000_000_000)])

        def funded():
            seen = [balance_of(cluster.state(i, alice), rusd) for i in range(cluster.k)]
            return seen if all(v == 1_000_000_000 for v in seen) else None
        cluster.wait(funded, "alice's deposit on every seat", timeout=60)
        steps.append("deposit of block 130 settled on every seat")
        # The trade: bob's ask rests in one slot, alice's bid fills it in a later one (a same-slot cross is M3-ENG-02).
        ask = cluster.cli("bob", ["place", "--market", "B3/rUSD", "--side", "sell", "--price", "100", "--lots", "5", "--wait", "--wait-ms", "60000"])
        if ask.get("accepted_by") != cluster.k or ask.get("result", {}).get("status") != "INCLUDED" or not ask["result"].get("verified"):
            raise RuntimeError(f"bob's ask: {ask}")
        if ask["result"]["outcome"]["code"] != "RESTING":
            raise RuntimeError(f"bob's ask did not rest: {ask['result']['outcome']}")
        steps.append(f"bob's ask admitted by {ask['accepted_by']} seats and rests in slot {ask['result']['height']} (verified)")
        bid = cluster.cli("alice", ["place", "--market", "B3/rUSD", "--side", "buy", "--price", "100", "--lots", "5", "--wait", "--wait-ms", "60000"])
        if bid.get("result", {}).get("status") != "INCLUDED" or not bid["result"].get("verified"):
            raise RuntimeError(f"alice's bid: {bid}")
        fills = bid["result"]["outcome"]["fills"]
        if len(fills) != 1 or fills[0]["lots"] != 5 or fills[0]["price_ticks"] != 100:
            raise RuntimeError(f"alice's bid did not fill bob's ask: {bid['result']['outcome']}")
        steps.append(f"alice's bid filled 5 lots at 100 in slot {bid['result']['height']} (verified)")

        def balance(state_json, asset):
            for b in state_json["leaf"]["balances"]:
                if b["asset"] == asset:
                    return b["available"]
            return 0
        alice_state = cluster.cli("alice", ["state"])
        bob_state = cluster.cli("bob", ["state"])
        if not alice_state.get("verified") or balance(alice_state, "B3") != 5_000_000 or balance(alice_state, "rUSD") != 1_000_000_000 - 500:
            raise RuntimeError(f"alice's verified state after the trade: {alice_state}")
        # The seller pays the fee (D5): fee_ppm 100 of 500 quote atoms rounds up to 1 (core/fee_math.h), so bob keeps 499.
        if not bob_state.get("verified") or balance(bob_state, "B3") != 10_000_000_000 - 5_000_000 or balance(bob_state, "rUSD") != 499:
            raise RuntimeError(f"bob's verified state after the trade: {bob_state}")
        if fills[0]["seller_fee"] != 1:
            raise RuntimeError(f"the fill's seller fee is not 1: {fills[0]}")
        steps.append(f"verified balances: alice B3 {balance(alice_state, 'B3')} rUSD {balance(alice_state, 'rUSD')}, bob rUSD {balance(bob_state, 'rUSD')}")
        # The exact bytes again: every seat answers DUPLICATE, nothing is re-signed.
        again = cluster.cli("alice", ["resend", "--label", bid["label"]])
        if again.get("accepted_by") != cluster.k or any(a.get("code") != "DUPLICATE" for a in again["admission"]):
            raise RuntimeError(f"resend: {again}")
        outbox = cluster.cli("alice", ["outbox"])
        if len(outbox["entries"]) != 1 or outbox["entries"][0]["result"]["outcome"] != "INCLUDED":
            raise RuntimeError(f"outbox: {outbox}")
        steps.append("resend answered DUPLICATE on every seat; the outbox holds the included bid")
        # A slot stream event from seat 0, verified by the client.
        event = cluster.cli("alice", ["stream", "--channel", "slots", "--count", "1", "--limit-ms", "10000", "--seat", "0"])
        if not event.get("verified"):
            raise RuntimeError(f"stream: {event}")
        steps.append("a streamed slot envelope verified")
        head_before = max(cluster.health(i)["published_height"] for i in range(cluster.k))
        # Seat 3 goes down; the rest keep deciding; a second deposit lands meanwhile.
        code = cluster.stop(3)
        if code != 0:
            raise RuntimeError(f"seat 3 exit code {code}")
        steps.append("seat 3 stopped cleanly")
        cluster.deposit([(alice, "rUSD", 5)])

        # More slots than the old sixteen-record catch-up ring could serve (review of 2026-11-16, defect 2).
        gap = 20

        def others_advance():
            hs = [cluster.health(i)["published_height"] for i in range(3)]
            return hs if all(h >= head_before + gap for h in hs) else None
        hs = cluster.wait(others_advance, f"the three remaining seats to decide {gap} more slots", timeout=90)
        steps.append(f"three seats went on without seat 3: heights {hs}")
        # Seat 3 restarts on its datadir: recovery, replay of the dev chain, catch-up to the others' head.
        info = cluster.start(3)
        if not info.get("restart"):
            raise RuntimeError("seat 3 did not recognise its datadir as a restart")
        steps.append(f"seat 3 restarted at head {info['head']}")

        def caught_up():
            hs = [cluster.health(i)["published_height"] for i in range(cluster.k)]
            return hs if hs[3] >= max(hs[:3]) - 1 and hs[3] >= head_before + gap else None
        hs = cluster.wait(caught_up, "seat 3 to catch up", timeout=90)
        steps.append(f"seat 3 caught up: heights {hs}")
        if balance_of(cluster.state(3, alice), rusd) != 1_000_000_000 - 500 + 5:
            # The second deposit may still be in flight on seat 3's published view; wait for it.
            cluster.wait(lambda: balance_of(cluster.state(3, alice), rusd) == 1_000_000_000 - 500 + 5, "the second deposit on seat 3", timeout=30)
        if balance_of(cluster.state(3, alice), b3) != 5_000_000:
            raise RuntimeError("seat 3 does not show alice's filled lots")
        steps.append("seat 3 shows both deposits and the trade")
        # Every seat agrees on alice's balances and bob's.
        for i in range(cluster.k):
            if balance_of(cluster.state(i, bob), b3) != 10_000_000_000 - 5_000_000 or balance_of(cluster.state(i, alice), b3) != 5_000_000:
                raise RuntimeError(f"seat {i} disagrees on the balances")
        steps.append("every seat agrees on every balance")
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
