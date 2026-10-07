#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""REGTEST ONLY mock Ethereum beacon node over a local anvil.

Serves the light-client part of the Beacon API from the synthetic
sync-committee chain that b3-bridge-regtest-beacon lays over anvil's real
execution blocks (doc/design/b3-bridge-v2-regtest-plan.md sections 3.2 and
3.4). The unchanged relayer (contrib/b3bridge/b3_bridge_relayer.py) and
b3-bridge-ethcheck consume it exactly as they would a real beacon node.

Guards, all fail closed:

- every endpoint is plain http on 127.0.0.1 or localhost; the server binds
  127.0.0.1 only and anvil is read without any HTTP proxy;
- the Ethereum mainnet genesis validators root, any fork version outside the
  synthetic 0xb3 namespace and any electra_epoch other than 0 are refused
  here and again by the tool, so nothing served can verify on a real network.

Period policy for a finalized execution block N_f (one best update per
sync-committee period q, as a beacon node serves them):

- q before period(N_f): finalize slot (q+1)*8192-3, attested -2, signature
  at the last slot of q, carrying the next committee;
- q == period(N_f): finalize N_f, attested N_f+1, carrying the next
  committee;
- later periods: none.

finality_update finalizes N_f without a next committee. finalize_through()
picks N_f so that its attested and signature slots stay inside its period.

Uses only the Python standard library. Run with --help for the CLI.
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PERIOD_SLOTS = 8192
SLOTS_PER_EPOCH = 32
SYNC_COMMITTEE_SIZE = 512
# Last slot offset in a period whose attested (+1) and signature (+2) slots
# are still inside the same period.
LAST_FINALIZABLE_OFFSET = PERIOD_SLOTS - 3
# MAX_REQUEST_LIGHT_CLIENT_UPDATES in the Ethereum consensus specs.
MAX_REQUEST_LIGHT_CLIENT_UPDATES = 128
FORK_VERSION_NAMESPACE = 0xB3
# Fork name reported in the "version" field; electra_epoch is always 0.
BEACON_API_VERSION = "electra"

DEFAULT_GENESIS_VALIDATORS_ROOT = hashlib.sha256(
    b"B3-REGTEST-SYNC-COMMITTEE-V1/genesis_validators_root").digest()
DEFAULT_FORK_VERSION = bytes.fromhex("b3000001")
DEFAULT_PORT = 5052
ETHEREUM_MAINNET_GENESIS_VALIDATORS_ROOT = bytes.fromhex(
    "4b363db94e286120d76eb905340fdd4e54bfe9f06bf33ff6cf5ad27f511bfe95")

LOCAL_HOSTS = ("127.0.0.1", "localhost")
# The eth_getBlockByNumber fields the tool reads; nothing else is sent.
HEADER_FIELDS = (
    "number", "hash", "parentHash", "miner", "stateRoot", "receiptsRoot",
    "logsBloom", "mixHash", "gasLimit", "gasUsed", "timestamp", "extraData",
    "baseFeePerGas", "transactionsRoot", "withdrawalsRoot", "blobGasUsed",
    "excessBlobGas",
)

_DIRECT_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


class MockBeaconError(RuntimeError):
    pass


def check_local_url(url):
    """Refuse every endpoint except plain http on 127.0.0.1 or localhost."""
    try:
        parts = urllib.parse.urlsplit(url)
        host = parts.hostname
        parts.port  # Raises ValueError on a malformed port.
    except (TypeError, ValueError, AttributeError):
        raise MockBeaconError(f"refusing malformed endpoint {url!r}") from None
    if (parts.scheme != "http" or host not in LOCAL_HOSTS or
            parts.username is not None or parts.password is not None):
        raise MockBeaconError(
            f"refusing non-local endpoint {url!r}: only http://127.0.0.1 or "
            "http://localhost are allowed (regtest only)")
    return url


def as_bytes(value, size, name):
    if isinstance(value, str):
        text = value[2:] if value.startswith("0x") else value
        try:
            value = bytes.fromhex(text)
        except ValueError:
            raise MockBeaconError(f"{name} is not hex") from None
    value = bytes(value)
    if len(value) != size:
        raise MockBeaconError(f"{name} must be {size} bytes")
    return value


def check_domain(genesis_validators_root, fork_version, electra_epoch=0):
    """The synthetic-domain guard (also enforced by the tool and the node)."""
    if genesis_validators_root == bytes(32):
        raise MockBeaconError("genesis validators root must be non-zero")
    if genesis_validators_root == ETHEREUM_MAINNET_GENESIS_VALIDATORS_ROOT:
        raise MockBeaconError(
            "refusing the Ethereum mainnet genesis validators root (regtest only)")
    if fork_version[0] != FORK_VERSION_NAMESPACE:
        raise MockBeaconError(
            f"refusing fork version 0x{fork_version.hex()} outside the synthetic "
            "0xb3 namespace (regtest only)")
    if type(electra_epoch) is not int or electra_epoch != 0:
        raise MockBeaconError("electra_epoch must be 0 (regtest only)")


def to_0x(data):
    return "0x" + bytes(data).hex()


def beacon_api_json(value):
    """Beacon-API number encoding: every integer as a decimal string."""
    if isinstance(value, bool):
        return value
    if isinstance(value, int):
        return str(value)
    if isinstance(value, dict):
        return {key: beacon_api_json(item) for key, item in value.items()}
    if isinstance(value, list):
        return [beacon_api_json(item) for item in value]
    return value


def eth_rpc(url, method, params, timeout=60):
    """JSON-RPC to the local anvil, never through a proxy."""
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method,
                       "params": params}).encode()
    request = urllib.request.Request(check_local_url(url), data=body,
                                     headers={"Content-Type": "application/json"})
    with _DIRECT_OPENER.open(request, timeout=timeout) as response:
        reply = json.load(response)
    if reply.get("error") is not None:
        raise MockBeaconError(f"{method}: {reply['error']}")
    return reply["result"]


class SyntheticBeacon:
    """The synthetic beacon chain over one anvil, as a library and a server.

    slot(N) = slot_offset + N for every execution block N >= base_block; the
    bootstrap is H(base_block). Tool outputs are cached by their exact
    request, so equal anvil headers give byte-identical outputs.
    """

    def __init__(self, anvil_url, tool, workdir, gvr, fork_version, slot_offset,
                 base_block, *, electra_epoch=0, tool_timeout=900):
        self.anvil_url = check_local_url(anvil_url)
        self.tool = os.path.abspath(tool)
        if not (os.path.isfile(self.tool) and os.access(self.tool, os.X_OK)):
            raise MockBeaconError(f"synthetic beacon tool {self.tool} is not executable")
        self.workdir = os.path.abspath(workdir)
        os.makedirs(self.workdir, exist_ok=True)
        self.gvr = as_bytes(gvr, 32, "genesis validators root")
        self.fork_version = as_bytes(fork_version, 4, "fork version")
        self.electra_epoch = electra_epoch
        check_domain(self.gvr, self.fork_version, electra_epoch)
        if (type(slot_offset) is not int or type(base_block) is not int or
                slot_offset < 0 or base_block < 0 or slot_offset + base_block == 0):
            raise MockBeaconError("slot_offset and base_block must give a non-zero base slot")
        self.slot_offset = slot_offset
        self.base_block = base_block
        self.tool_timeout = tool_timeout
        self._headers = {}
        self._outputs = {}
        self._participants = {}
        self._finalized = None
        self._snapshot = None
        self._lock = threading.RLock()
        self._log_lock = threading.Lock()
        self._server = None
        self._thread = None
        self._log_path = os.path.join(self.workdir, "mock_beacon.log")

    # -- slots ---------------------------------------------------------------
    def slot(self, block):
        return self.slot_offset + block

    def block_at_slot(self, slot):
        block = slot - self.slot_offset
        if block < self.base_block:
            raise MockBeaconError(f"slot {slot} is before the base block")
        return block

    def period_of_block(self, block):
        return self.slot(block) // PERIOD_SLOTS

    @property
    def finalized_block(self):
        return self._finalized

    # -- anvil headers -------------------------------------------------------
    def anvil_head(self):
        return int(eth_rpc(self.anvil_url, "eth_blockNumber", []), 16)

    def _fetch_header(self, number):
        header = eth_rpc(self.anvil_url, "eth_getBlockByNumber", [hex(number), False])
        if not isinstance(header, dict) or int(header.get("number", "0x0"), 16) != number:
            raise MockBeaconError(f"anvil has no block {number}")
        return {field: header[field] for field in HEADER_FIELDS if field in header}

    def headers(self, through):
        """Execution headers base_block..through, cached by number.

        The newest cached header is re-read from anvil first, so an anvil
        revert is detected (call forget_headers() after one) instead of
        silently serving a stale chain; parent links do the rest.
        """
        if through < self.base_block:
            raise MockBeaconError(f"block {through} is before the base block")
        with self._lock:
            if self._headers:
                top = max(self._headers)
                if self._fetch_header(top)["hash"] != self._headers[top]["hash"]:
                    raise MockBeaconError(
                        f"anvil block {top} changed under the mock beacon; "
                        "call forget_headers() after reverting anvil")
            for number in range(self.base_block, through + 1):
                if number in self._headers:
                    continue
                header = self._fetch_header(number)
                parent = self._headers.get(number - 1)
                if parent is not None and header["parentHash"] != parent["hash"]:
                    raise MockBeaconError(f"anvil block {number} does not link to its parent")
                self._headers[number] = header
            return [self._headers[n] for n in range(self.base_block, through + 1)]

    def forget_headers(self):
        """Drop every cached header and served state (after an anvil revert)."""
        with self._lock:
            self._headers.clear()
            self._finalized = None
            self._snapshot = None

    # -- tool ----------------------------------------------------------------
    def request(self, outputs, through=None):
        """Run the tool once for a list of output specs; returns its outputs.

        through is the last execution header sent (default: the newest
        header any requested update needs, at least the base block).
        """
        outputs = [dict(spec) for spec in outputs]
        if not outputs:
            raise MockBeaconError("no outputs requested")
        if through is None:
            through = self.base_block
            for spec in outputs:
                if spec.get("kind") == "update":
                    through = max(through, int(spec["finalized"]) + 1)
        request = {
            "version": 1,
            "genesis_validators_root": to_0x(self.gvr),
            "fork_version": to_0x(self.fork_version),
            "electra_epoch": self.electra_epoch,
            "slot_offset": self.slot_offset,
            "base_block": self.base_block,
            "headers": self.headers(through),
            "outputs": outputs,
        }
        encoded = json.dumps(request, sort_keys=True, separators=(",", ":")).encode()
        key = hashlib.sha256(encoded).hexdigest()
        with self._lock:
            cached = self._outputs.get(key)
        if cached is not None:
            return json.loads(cached)["outputs"]
        request_path = os.path.join(self.workdir, f"beacon-{key[:16]}.request.json")
        with open(request_path, "wb") as f:
            f.write(encoded)
        try:
            result = subprocess.run([self.tool, request_path], capture_output=True,
                                    timeout=self.tool_timeout, stdin=subprocess.DEVNULL)
        except subprocess.TimeoutExpired:
            raise MockBeaconError(f"synthetic beacon tool timed out after {self.tool_timeout} s") from None
        if result.returncode != 0:
            message = result.stderr.decode(errors="replace").strip()
            raise MockBeaconError(f"synthetic beacon tool failed ({result.returncode}): {message}")
        try:
            parsed = json.loads(result.stdout)
            produced = parsed["outputs"]
        except (ValueError, KeyError, TypeError):
            raise MockBeaconError("synthetic beacon tool stdout is not its output object") from None
        if not isinstance(produced, list) or len(produced) != len(outputs):
            raise MockBeaconError("synthetic beacon tool returned the wrong number of outputs")
        with open(os.path.join(self.workdir, f"beacon-{key[:16]}.output.json"), "wb") as f:
            f.write(result.stdout)
        with self._lock:
            self._outputs[key] = result.stdout
        return produced

    def bootstrap(self):
        """The bootstrap output for H(base_block) (the pinned checkpoint)."""
        return self.request([{"kind": "bootstrap"}])[0]

    def update(self, finalized, **variants):
        """One update output finalizing block `finalized`, attested by +1.

        variants are the tool's: participants, next_committee,
        signature_slot, slot_shift, sign_genesis_validators_root,
        sign_fork_version (hex or bytes), finalized_proposer_index.
        """
        return self.updates([dict(variants, finalized=finalized)])[0]

    def updates(self, specs):
        """Several update outputs in one tool run (committees derived once)."""
        outputs = []
        for spec in specs:
            spec = dict(spec)
            for name in ("sign_genesis_validators_root", "sign_fork_version"):
                if isinstance(spec.get(name), (bytes, bytearray)):
                    spec[name] = to_0x(spec[name])
            spec["kind"] = "update"
            outputs.append(spec)
        return self.request(outputs)

    # -- served state ----------------------------------------------------------
    def set_participants(self, period, participants):
        """Sign every served update whose signature slot is in `period` with
        the first `participants` members (None restores all 512)."""
        if participants is not None and not 1 <= participants <= SYNC_COMMITTEE_SIZE:
            raise MockBeaconError("participants must be between 1 and 512")
        with self._lock:
            if participants is None:
                self._participants.pop(period, None)
            else:
                self._participants[period] = participants
            if self._finalized is not None:
                self._rebuild()

    def finalize_through(self, block):
        """Finalize the newest valid execution block N <= block; returns N.

        N needs its attested successor on anvil, and slot(N) mod 8192 must be
        at most 8189 so the attested and signature slots stay in N's period.
        """
        with self._lock:
            newest = min(block, self.anvil_head() - 1)
            while newest >= self.base_block and self.slot(newest) % PERIOD_SLOTS > LAST_FINALIZABLE_OFFSET:
                newest -= 1
            if newest < self.base_block:
                raise MockBeaconError(
                    f"no finalizable block at or below {block} (base block "
                    f"{self.base_block}, anvil head {self.anvil_head()})")
            previous = self._finalized
            self._finalized = newest
            try:
                self._rebuild()
            except Exception:
                self._finalized = previous
                raise
            return newest

    def _period_specs(self):
        finalized = self._finalized
        first, last = self.period_of_block(self.base_block), self.period_of_block(finalized)
        specs = []
        for period in range(first, last + 1):
            if period < last:
                finalized_slot = (period + 1) * PERIOD_SLOTS - 3
                if finalized_slot < self.slot(self.base_block):
                    raise MockBeaconError(
                        f"the bootstrap slot {self.slot(self.base_block)} is too late in "
                        f"period {period} for a rotation update; choose another slot_offset")
                spec = {"kind": "update", "finalized": self.block_at_slot(finalized_slot),
                        "signature_slot": (period + 1) * PERIOD_SLOTS - 1,
                        "next_committee": True}
            else:
                spec = {"kind": "update", "finalized": finalized, "next_committee": True}
            if period in self._participants:
                spec["participants"] = self._participants[period]
            specs.append((period, spec))
        finality = {"kind": "update", "finalized": finalized, "next_committee": False}
        if last in self._participants:
            finality["participants"] = self._participants[last]
        return specs, finality

    def _rebuild(self):
        specs, finality = self._period_specs()
        produced = self.request([{"kind": "bootstrap"}] + [spec for _, spec in specs] + [finality],
                                through=self._finalized + 1)
        bootstrap, by_period, finality_output = produced[0], {}, produced[-1]
        for (period, _), output in zip(specs, produced[1:-1]):
            if output["period"] != period:
                raise MockBeaconError(f"update for period {period} reports period {output['period']}")
            by_period[period] = output
        self._snapshot = {"finalized": self._finalized, "bootstrap": bootstrap,
                          "updates": by_period, "finality_update": finality_output}

    def snapshot(self):
        """The served state: finalized block, bootstrap, per-period updates
        and finality update (None before finalize_through()).

        Replaced as a whole by one assignment, so the server reads it without
        waiting for a tool run that is building the next one.
        """
        return self._snapshot

    def period_updates(self):
        """Served per-period update outputs, oldest period first."""
        snapshot = self.snapshot()
        if snapshot is None:
            return []
        return [snapshot["updates"][period] for period in sorted(snapshot["updates"])]

    # -- Beacon API objects ----------------------------------------------------
    @staticmethod
    def api_object(output):
        """{"version", "data"} as the Beacon API returns an LC object."""
        return {"version": BEACON_API_VERSION, "data": beacon_api_json(output["data"])}

    def api_genesis(self):
        return {"data": {"genesis_validators_root": to_0x(self.gvr),
                         "genesis_fork_version": to_0x(self.fork_version)}}

    def api_fork_schedule(self):
        version = to_0x(self.fork_version)
        return {"data": [{"previous_version": version, "current_version": version,
                          "epoch": "0"}]}

    def api_head(self):
        """Only the head slot is modelled: slot(anvil head) - 1."""
        return {"execution_optimistic": False, "finalized": False,
                "data": {"canonical": True,
                         "header": {"message": {"slot": str(self.slot(self.anvil_head()) - 1)}}}}

    def handle_get(self, path):
        """(HTTP status, JSON body) for one GET request path."""
        parts = urllib.parse.urlsplit(path)
        route = parts.path.rstrip("/")
        query = urllib.parse.parse_qs(parts.query)
        try:
            if route == "/eth/v1/beacon/genesis":
                return 200, self.api_genesis()
            if route == "/eth/v1/config/fork_schedule":
                return 200, self.api_fork_schedule()
            if route == "/eth/v1/beacon/headers/head":
                return 200, self.api_head()
            snapshot = self.snapshot()
            prefix = "/eth/v1/beacon/light_client/bootstrap/"
            if route.startswith(prefix):
                bootstrap = snapshot["bootstrap"] if snapshot else self.bootstrap()
                if route[len(prefix):].lower() != bootstrap["checkpoint_root"].lower():
                    return 404, {"code": 404, "message": "LC bootstrap unavailable"}
                return 200, self.api_object(bootstrap)
            if route == "/eth/v1/beacon/light_client/updates":
                try:
                    start = int(query["start_period"][0])
                    count = int(query["count"][0])
                except (KeyError, IndexError, ValueError):
                    return 400, {"code": 400, "message": "start_period and count are required integers"}
                if start < 0 or count < 1:
                    return 400, {"code": 400, "message": "invalid start_period or count"}
                count = min(count, MAX_REQUEST_LIGHT_CLIENT_UPDATES)
                served = snapshot["updates"] if snapshot else {}
                return 200, [self.api_object(served[period])
                             for period in range(start, start + count) if period in served]
            if route == "/eth/v1/beacon/light_client/finality_update":
                if snapshot is None:
                    return 404, {"code": 404, "message": "no finality update available"}
                return 200, self.api_object(snapshot["finality_update"])
        except Exception as e:  # Report, never kill the server thread.
            return 500, {"code": 500, "message": f"{type(e).__name__}: {e}"}
        return 404, {"code": 404, "message": f"unsupported endpoint {route}"}

    # -- HTTP server -----------------------------------------------------------
    def serve(self, port=0):
        """Start serving on 127.0.0.1 (port 0 picks one); returns the URL."""
        if self._server is not None:
            raise MockBeaconError("mock beacon already serving")
        server = ThreadingHTTPServer(("127.0.0.1", port), _Handler)
        server.daemon_threads = True
        server.beacon = self
        self._server = server
        self._thread = threading.Thread(target=server.serve_forever, name="mock-beacon",
                                        daemon=True)
        self._thread.start()
        return self.url

    @property
    def url(self):
        if self._server is None:
            raise MockBeaconError("mock beacon is not serving")
        return f"http://127.0.0.1:{self._server.server_address[1]}"

    def stop(self):
        if self._server is not None:
            self._server.shutdown()
            self._server.server_close()
            self._thread.join(timeout=10)
        self._server = None
        self._thread = None

    def log_request(self, line):
        # Its own lock: a request must not wait for a tool run to log.
        with self._log_lock, open(self._log_path, "a", encoding="utf8") as f:
            f.write(f"{time.strftime('%Y-%m-%dT%H:%M:%S')} {line}\n")

    def __repr__(self):
        return f"SyntheticBeacon(base_block={self.base_block}, slot_offset={self.slot_offset})"


class _Handler(BaseHTTPRequestHandler):
    server_version = "b3-regtest-mock-beacon/1"

    def do_GET(self):
        status, body = self.server.beacon.handle_get(self.path)
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, format, *args):
        self.server.beacon.log_request(format % args)


# -- CLI -------------------------------------------------------------------------
def beacon_from_pins(pins_path, anvil_url, tool, workdir):
    """A SyntheticBeacon for the checkpoint a -b3bridgepins file names."""
    with open(pins_path, encoding="utf8") as f:
        pins = json.load(f)
    try:
        if pins["format"] != "b3-regtest-bridge-pins/1":
            raise MockBeaconError("unsupported pin file format")
        light = pins["v1_decentralized"]["light_client"]
        forks = light["fork_schedule"]
        if len(forks) != 1 or forks[0]["activation_epoch"] != 0:
            raise MockBeaconError("the synthetic beacon serves exactly one fork from epoch 0")
        checkpoint_slot = light["trusted_checkpoint_slot"]
        checkpoint_block = light["checkpoint_execution_block"]
        beacon = SyntheticBeacon(anvil_url, tool, workdir, light["genesis_validators_root"],
                                 forks[0]["fork_version"], checkpoint_slot - checkpoint_block,
                                 checkpoint_block, electra_epoch=light["electra_epoch"])
        expected_root = light["trusted_checkpoint_root"].lower()
    except (KeyError, TypeError) as e:
        raise MockBeaconError(f"malformed pin file: missing {e}") from None
    if beacon.bootstrap()["checkpoint_root"].lower() != expected_root:
        raise MockBeaconError("the pin file's checkpoint root is not H(checkpoint block) on this anvil")
    return beacon


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        description="REGTEST ONLY mock beacon light-client API over a local anvil. "
                    "It follows the anvil head (finalizing head-1) for the checkpoint "
                    "a -b3bridgepins file names, and binds 127.0.0.1 only.")
    parser.add_argument("--anvil", required=True, help="local anvil URL, e.g. http://127.0.0.1:8545")
    parser.add_argument("--tool", required=True, help="path to b3-bridge-regtest-beacon")
    parser.add_argument("--pins", required=True, help="the -b3bridgepins JSON file")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT,
                        help=f"port on 127.0.0.1 (default {DEFAULT_PORT})")
    parser.add_argument("--workdir", help="tool request/output directory (default: a new temp dir)")
    parser.add_argument("--poll-seconds", type=float, default=2.0,
                        help="anvil head poll interval (default 2)")
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    try:
        workdir = args.workdir or tempfile.mkdtemp(prefix="b3-mock-beacon-")
        beacon = beacon_from_pins(args.pins, args.anvil, args.tool, workdir)
        url = beacon.serve(args.port)
    except (MockBeaconError, OSError, ValueError) as e:
        print(f"FAIL: {e}", file=sys.stderr)
        return 1
    print(json.dumps({"event": "serving", "url": url, "workdir": workdir,
                      "base_block": beacon.base_block, "slot_offset": beacon.slot_offset}),
          flush=True)
    finalized = None
    try:
        while True:
            try:
                head = beacon.anvil_head()
                if head - 1 >= beacon.base_block and (finalized is None or head - 1 > finalized):
                    finalized = beacon.finalize_through(head - 1)
                    print(json.dumps({"event": "finalized", "block": finalized,
                                      "slot": beacon.slot(finalized),
                                      "period": beacon.period_of_block(finalized)}), flush=True)
            except (MockBeaconError, OSError) as e:
                print(json.dumps({"event": "error", "error": str(e)}), flush=True)
            time.sleep(args.poll_seconds)
    except KeyboardInterrupt:
        pass
    finally:
        beacon.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
