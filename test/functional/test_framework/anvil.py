#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Local anvil and forge helpers for the regtest bridge harness.

REGTEST ONLY. Every endpoint here is a process this test started on
127.0.0.1: anvil never forks (no --fork-url), JSON-RPC bypasses any
configured HTTP proxy, and assert_local_url() refuses every other host.
forge always runs with --offline and sends its build output, cache and
broadcast records below <BUILDDIR>/test/forge. One exception: a forge script
can write only where contracts/foundry.toml's fs_permissions allow, so RT0's
Deploy.s.sol run writes its candidate manifest to
contracts/deployments/local-rt0-<pid>.json (gitignored) and deletes it
afterwards; a killed run can leave that file behind. Dev accounts are
unlocked by anvil itself; no private key is handled here.
ArchiveCodeProxy, also bound to 127.0.0.1 only, stands in for an archive
provider where anvil has dropped old state.

A missing foundry binary is not an error: find_foundry_tool() returns
None so the caller can skip the test.
"""

import configparser
import contextlib
import fcntl
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import importlib
import itertools
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

LOCAL_HOSTS = ("127.0.0.1", "localhost")
DEFAULT_CHAIN_ID = 31337
FOUNDRY_FALLBACK_DIR = "/opt/homebrew/bin"


def _source_dir():
    """Repository root of the checked-out source tree.

    Build-directory test files are symlinks into the source tree, so the
    resolved module path normally yields it. A copied tree falls back to the
    SRCDIR recorded in the adjacent build config.ini.
    """
    resolved = Path(__file__).resolve().parents[3]
    if (resolved / "contrib" / "b3bridge").is_dir():
        return resolved
    config_path = Path(os.path.abspath(__file__)).parents[2] / "config.ini"
    if config_path.is_file():
        config = configparser.ConfigParser()
        config.read(config_path)
        srcdir = Path(config["environment"]["SRCDIR"])
        if (srcdir / "contrib" / "b3bridge").is_dir():
            return srcdir
    raise ImportError("contrib/b3bridge not found next to the functional test framework")


SRCDIR = _source_dir()
B3BRIDGE_CONTRIB_DIR = SRCDIR / "contrib" / "b3bridge"
B3BRIDGE_REGTEST_DIR = B3BRIDGE_CONTRIB_DIR / "regtest"


def add_b3bridge_contrib_to_path():
    """Make the stdlib-only contrib/b3bridge modules importable."""
    for path in (B3BRIDGE_REGTEST_DIR, B3BRIDGE_CONTRIB_DIR):
        if str(path) not in sys.path:
            sys.path.insert(0, str(path))


def import_b3bridge_contrib(name):
    """Import a contrib/b3bridge module without writing bytecode there."""
    add_b3bridge_contrib_to_path()
    previous = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        return importlib.import_module(name)
    finally:
        sys.dont_write_bytecode = previous


keccak256 = import_b3bridge_contrib("capture_eth_receipts_fixture").keccak256


def find_foundry_tool(name):
    """Locate anvil, forge or cast; None means the caller should skip."""
    candidates = [os.environ.get(name.upper()), shutil.which(name),
                  os.path.join(FOUNDRY_FALLBACK_DIR, name)]
    for candidate in candidates:
        if candidate and os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return os.path.abspath(candidate)
    return None


def assert_local_url(url):
    """Refuse every endpoint except plain http on 127.0.0.1 or localhost."""
    try:
        parts = urllib.parse.urlsplit(url)
        host = parts.hostname
        parts.port  # Raises ValueError on a malformed port.
    except (TypeError, ValueError, AttributeError):
        raise AssertionError(f"Refusing malformed endpoint {url!r}") from None
    if (parts.scheme != "http" or host not in LOCAL_HOSTS or
            parts.username is not None or parts.password is not None):
        raise AssertionError(
            f"Refusing non-local endpoint {url!r}: only http://127.0.0.1 "
            "or http://localhost are allowed in the regtest bridge harness")
    return url


def free_local_port():
    """Return a currently unused TCP port on 127.0.0.1."""
    with contextlib.closing(socket.socket(socket.AF_INET, socket.SOCK_STREAM)) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def to_hex(value):
    """Ethereum JSON-RPC quantity (int) or data (bytes) encoding."""
    if isinstance(value, int):
        assert value >= 0
        return hex(value)
    return "0x" + bytes(value).hex()


def from_hex(text):
    """Decode a 0x-prefixed JSON-RPC data string."""
    assert isinstance(text, str) and text.startswith("0x"), text
    return bytes.fromhex(text[2:])


def block_tag(block):
    return to_hex(block) if isinstance(block, int) else block


def normalize_address(address):
    """Lower-case 0x-prefixed 20-byte address string."""
    if isinstance(address, (bytes, bytearray)):
        assert len(address) == 20
        return "0x" + bytes(address).hex()
    assert isinstance(address, str) and address.startswith("0x") and len(address) == 42, address
    bytes.fromhex(address[2:])
    return address.lower()


class AnvilRPCError(Exception):
    def __init__(self, method, error):
        self.method = method
        self.code = error.get("code")
        self.message = error.get("message", "")
        self.data = error.get("data")
        super().__init__(f"{method}: {self.code} {self.message}")


_DIRECT_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))
_REQUEST_IDS = itertools.count(1)


def local_json_rpc(url, method, params=None, timeout=120):
    """JSON-RPC to a local endpoint, never through a proxy."""
    assert_local_url(url)
    body = json.dumps({"jsonrpc": "2.0", "id": next(_REQUEST_IDS),
                       "method": method, "params": params or []}).encode()
    request = urllib.request.Request(url, data=body,
                                     headers={"Content-Type": "application/json"})
    with _DIRECT_OPENER.open(request, timeout=timeout) as response:
        reply = json.load(response)
    if reply.get("error") is not None:
        raise AnvilRPCError(method, reply["error"])
    return reply["result"]


class Anvil:
    """One local anvil chain (Ethereum shape: osaka with EIP-7825 enforced)."""

    def __init__(self, binary, logdir, chain_id=DEFAULT_CHAIN_ID, *,
                 accounts=4, balance=1_000_000, hardfork="osaka"):
        self.binary = binary
        self.logdir = os.path.abspath(logdir)
        self.chain_id = chain_id
        self.accounts_count = accounts
        self.balance = balance
        self.hardfork = hardfork
        self.port = None
        self.process = None
        self._log = None

    @property
    def url(self):
        assert self.port is not None, "anvil is not started"
        return f"http://127.0.0.1:{self.port}"

    @property
    def log_path(self):
        return os.path.join(self.logdir, "anvil.log")

    def start(self, port=None, timeout=30):
        assert self.process is None, "anvil already started"
        os.makedirs(self.logdir, exist_ok=True)
        self.port = port or free_local_port()
        command = [
            self.binary,
            "--host", "127.0.0.1",
            "--port", str(self.port),
            "--chain-id", str(self.chain_id),
            "--hardfork", self.hardfork,
            "--enable-tx-gas-limit",
            "--accounts", str(self.accounts_count),
            "--balance", str(self.balance),
            # Keep only anvil's in-memory state history (its newest 500
            # blocks). Its default also persists up to 3,600 older states,
            # about 2 MB each, under ~/.foundry/anvil/tmp: outside the test
            # tmpdir and not removed on exit (7 GB after 20,000 blocks).
            "--max-persisted-states", "0",
        ]
        assert not any(arg.startswith("--fork") for arg in command)
        self._log = open(self.log_path, "ab")
        self.process = subprocess.Popen(command, stdin=subprocess.DEVNULL,
                                        stdout=self._log, stderr=subprocess.STDOUT,
                                        cwd=self.logdir)
        deadline = time.time() + timeout
        while True:
            if self.process.poll() is not None:
                self._close_log()
                raise AssertionError(
                    f"anvil exited with {self.process.returncode}; see {self.log_path}")
            try:
                chain_id = int(self.rpc("eth_chainId", timeout=5), 16)
            except (OSError, urllib.error.URLError):
                chain_id = None
            if chain_id is not None:
                if chain_id != self.chain_id:
                    self.stop()
                    raise AssertionError(f"anvil reports chain id {chain_id}, not {self.chain_id}")
                return self
            if time.time() >= deadline:
                self.stop()
                raise AssertionError(f"anvil did not answer within {timeout} s")
            time.sleep(0.1)

    def stop(self, timeout=10):
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=timeout)
        self.process = None
        self._close_log()

    def _close_log(self):
        if self._log is not None:
            self._log.close()
            self._log = None

    def __enter__(self):
        return self.start() if self.process is None else self

    def __exit__(self, *exc):
        self.stop()

    # -- JSON-RPC ---------------------------------------------------------
    def rpc(self, method, *params, timeout=120):
        return local_json_rpc(self.url, method, list(params), timeout=timeout)

    def chain_id_rpc(self):
        return int(self.rpc("eth_chainId"), 16)

    def block_number(self):
        return int(self.rpc("eth_blockNumber"), 16)

    def block(self, number="latest", full=False):
        return self.rpc("eth_getBlockByNumber", block_tag(number), full)

    def mine(self, count=1, interval=None):
        """Mine count empty-or-pending blocks (anvil_mine)."""
        params = [to_hex(count)] + ([to_hex(interval)] if interval is not None else [])
        self.rpc("anvil_mine", *params)
        return self.block_number()

    def mine_to(self, height):
        head = self.block_number()
        assert height >= head, f"anvil head {head} is already above {height}"
        if height > head:
            self.mine(height - head)
        return self.block_number()

    # Snapshot and warp (plan section 3.2): scaffolding for the later
    # milestones' clock and drill steps; RT0, RT2 and RT3a do not use them.
    def snapshot(self):
        return self.rpc("evm_snapshot")

    def revert(self, snapshot_id):
        assert self.rpc("evm_revert", snapshot_id) is True, "evm_revert failed"

    def set_next_timestamp(self, timestamp):
        self.rpc("evm_setNextBlockTimestamp", to_hex(timestamp))

    def increase_time(self, seconds):
        result = self.rpc("evm_increaseTime", to_hex(seconds))
        # anvil answers with a plain JSON number; tolerate a quantity string.
        return int(result, 16) if isinstance(result, str) else int(result)

    def accounts(self):
        return [normalize_address(a) for a in self.rpc("eth_accounts")]

    def receipt(self, tx_hash, timeout=30):
        deadline = time.time() + timeout
        while True:
            receipt = self.rpc("eth_getTransactionReceipt", tx_hash)
            if receipt is not None:
                return receipt
            if time.time() >= deadline:
                raise AssertionError(f"No receipt for {tx_hash} after {timeout} s")
            time.sleep(0.05)

    def send(self, sender, to, data=b"", value=0, gas=None):
        """eth_sendTransaction from an unlocked dev account; returns the receipt.

        to=None deploys data as init code. The transaction must succeed on
        chain. RPC-level refusals, e.g. EIP-7825's gas cap, raise
        AnvilRPCError from here.
        """
        tx = {"from": normalize_address(sender), "data": to_hex(data),
              "value": to_hex(value)}
        if to is not None:
            tx["to"] = normalize_address(to)
        if gas is not None:
            tx["gas"] = to_hex(gas)
        receipt = self.receipt(self.rpc("eth_sendTransaction", tx))
        if int(receipt["status"], 16) != 1:
            raise AssertionError(f"transaction {receipt['transactionHash']} reverted")
        return receipt

    def call(self, to, data, block="latest", sender=None):
        tx = {"to": normalize_address(to), "data": to_hex(data)}
        if sender is not None:
            tx["from"] = normalize_address(sender)
        return from_hex(self.rpc("eth_call", tx, block_tag(block)))

    def code(self, address, block="latest"):
        return from_hex(self.rpc("eth_getCode", normalize_address(address), block_tag(block)))

    def get_logs(self, address, topics=(), from_block=0, to_block="latest"):
        """eth_getLogs of one address, in chain order (block, then log index).

        Each topic position is a 32-byte value (bytes or a 0x-prefixed
        string), a list of alternatives or None (any value).
        """
        def topic(value):
            if value is None:
                return None
            if isinstance(value, (list, tuple)):
                return [topic(v) for v in value]
            data = bytes(value) if isinstance(value, (bytes, bytearray)) else from_hex(value)
            assert len(data) == 32, "a log topic is 32 bytes"
            return to_hex(data)

        logs = self.rpc("eth_getLogs", {"address": normalize_address(address),
                                        "topics": [topic(t) for t in topics],
                                        "fromBlock": block_tag(from_block), "toBlock": block_tag(to_block)})
        return sorted(logs, key=lambda log: (int(log["blockNumber"], 16), int(log["logIndex"], 16)))

    def code_hash(self, address, block="latest"):
        """keccak256 of the deployed runtime code, computed here."""
        return keccak256(self.code(address, block))

    def get_proof(self, address, slots, block="latest"):
        """eth_getProof (EIP-1186) for address and storage slots at block.

        A slot is an int, 32 raw bytes or a 0x-prefixed 32-byte word (as
        getbridgev2info's observation_plan lists them).
        """
        def key(slot):
            if isinstance(slot, (bytes, bytearray)):
                assert len(slot) == 32, "a storage key is 32 bytes"
                return "0x" + bytes(slot).hex()
            if isinstance(slot, str):
                return "0x" + from_hex(slot).rjust(32, b"\0").hex()
            return "0x" + int(slot).to_bytes(32, "big").hex()

        return self.rpc("eth_getProof", normalize_address(address), [key(slot) for slot in slots],
                        block_tag(block))


class ArchiveCodeProxy:
    """A local JSON-RPC proxy in front of anvil that also serves recorded code.

    anvil keeps the state of only its newest 500 blocks (Anvil.start() sets
    --max-persisted-states 0). Keeping more does not scale: every retained
    state copies the EIP-2935 history storage and anvil's block-hash table,
    so a full in-memory history measured about 10 GB of RSS at 9,000 osaka
    blocks and 24 GB at 21,000 cancun blocks. Older blocks answer
    BlockOutOfRangeError, which a real archive provider would not. The
    unchanged relayer re-checks every pinned runtime at the vault deployment
    block on each cycle (b3_bridge_relayer.verify_runtime_code_pins), and a
    +20,000-block light-client window puts that block out of anvil's range.

    record_code() reads eth_getCode(address, block) from anvil while anvil
    still holds that state. The proxy forwards every request body to anvil
    byte for byte and returns anvil's reply unchanged, except that a single
    eth_getCode request for a recorded (address, block) that anvil refuses
    as out of range gets the recorded result. It binds 127.0.0.1 only and
    forwards only to the local anvil it was given.
    """

    def __init__(self, upstream_url):
        self.upstream = assert_local_url(upstream_url)
        self._code = {}
        self._lock = threading.Lock()
        self.served_from_archive = 0
        self._server = None
        self._thread = None

    @property
    def url(self):
        assert self._server is not None, "archive proxy is not serving"
        return f"http://127.0.0.1:{self._server.server_address[1]}"

    def start(self):
        assert self._server is None, "archive proxy already serving"
        server = ThreadingHTTPServer(("127.0.0.1", 0), _ArchiveProxyHandler)
        server.daemon_threads = True
        server.proxy = self
        self._server = server
        self._thread = threading.Thread(target=server.serve_forever,
                                        name="anvil-archive-proxy", daemon=True)
        self._thread.start()
        return self

    def stop(self):
        if self._server is not None:
            self._server.shutdown()
            self._server.server_close()
            self._thread.join(timeout=10)
        self._server = None
        self._thread = None

    def record_code(self, address, block):
        """Record anvil's eth_getCode at a block whose state it still holds."""
        address = normalize_address(address)
        code = local_json_rpc(self.upstream, "eth_getCode", [address, to_hex(block)])
        assert code not in (None, "0x"), f"no runtime code at {address} in block {block}"
        with self._lock:
            self._code[(address, block)] = code
        return from_hex(code)

    def forward(self, body):
        """(HTTP status, reply bytes) for one request body."""
        request = urllib.request.Request(self.upstream, data=body,
                                         headers={"Content-Type": "application/json"})
        try:
            with _DIRECT_OPENER.open(request, timeout=120) as response:
                status, data = response.status, response.read()
        except urllib.error.HTTPError as e:
            status, data = e.code, e.read()
        if status == 200:
            data = self._archive_reply(body, data)
        return status, data

    def _archive_reply(self, body, data):
        try:
            request = json.loads(body)
            if not isinstance(request, dict) or request.get("method") != "eth_getCode":
                return data
            address, block = request["params"]
            key = (normalize_address(address), int(block, 16))
            reply = json.loads(data)
        except (ValueError, TypeError, KeyError, AssertionError, AttributeError):
            return data
        error = reply.get("error") if isinstance(reply, dict) else None
        if not isinstance(error, dict) or "BlockOutOfRange" not in str(error.get("message")):
            return data
        with self._lock:
            code = self._code.get(key)
            if code is None:
                return data
            self.served_from_archive += 1
        return json.dumps({"jsonrpc": "2.0", "id": request.get("id"), "result": code}).encode()

    def __repr__(self):
        return f"ArchiveCodeProxy(upstream={self.upstream})"


class _ArchiveProxyHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        status, data = self.server.proxy.forward(body)
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, format, *args):
        pass


# -- forge ----------------------------------------------------------------
def forge_dirs(builddir):
    """(work, out, cache) under <BUILDDIR>/test/forge, as absolute paths.

    forge resolves a relative --out or --cache-path against --root, i.e.
    inside the source tree, so these must stay absolute.
    """
    work = os.path.join(os.path.abspath(builddir), "test", "forge")
    return work, os.path.join(work, "out"), os.path.join(work, "cache")


def run_forge(forge, srcdir, builddir, subcommand, *args, env=None, timeout=600):
    """Run `forge <subcommand> --offline` against <SRCDIR>/contracts.

    Every output path is redirected below <BUILDDIR>/test/forge, and an
    fcntl lock there serialises concurrent tests sharing the out directory.
    Returns the CompletedProcess; a non-zero exit raises with the output.
    """
    work, out, cache = forge_dirs(builddir)
    os.makedirs(work, exist_ok=True)
    root = os.path.join(os.path.abspath(srcdir), "contracts")
    command = [forge, subcommand, "--offline", "--root", root,
               "--out", out, "--cache-path", cache, *args]
    full_env = dict(os.environ)
    full_env["FOUNDRY_BROADCAST"] = os.path.join(work, "broadcast")
    full_env.update(env or {})
    with open(os.path.join(work, "forge.lock"), "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        try:
            result = subprocess.run(command, cwd=work, env=full_env, timeout=timeout,
                                    stdin=subprocess.DEVNULL, capture_output=True,
                                    text=True)
        finally:
            fcntl.flock(lock, fcntl.LOCK_UN)
    if result.returncode != 0:
        raise AssertionError(
            f"forge {subcommand} failed ({result.returncode}):\n"
            f"{result.stdout[-8000:]}\n{result.stderr[-8000:]}")
    return result


def forge_build(forge, srcdir, builddir, *args, timeout=600):
    return run_forge(forge, srcdir, builddir, "build", *args, timeout=timeout)


def forge_test(forge, srcdir, builddir, *args, timeout=1800):
    return run_forge(forge, srcdir, builddir, "test", *args, timeout=timeout)


def artifact(builddir, file, contract):
    """Read a forge artifact: abi, creation bytecode and runtime bytecode."""
    _, out, _ = forge_dirs(builddir)
    with open(os.path.join(out, file, f"{contract}.json"), encoding="utf8") as f:
        data = json.load(f)
    for section in ("bytecode", "deployedBytecode"):
        assert not data[section].get("linkReferences"), f"{contract} needs library linking"
    bytecode = from_hex(data["bytecode"]["object"])
    assert bytecode, f"{contract} is abstract or an interface"
    return {"abi": data["abi"], "bytecode": bytecode,
            "deployed_bytecode": from_hex(data["deployedBytecode"]["object"])}


def deploy(anvil, sender, contract_artifact, ctor_bytes=b"", gas=None):
    """Deploy creation code plus ABI-encoded constructor arguments.

    Returns (address, block number, keccak of the deployed runtime code).
    """
    receipt = anvil.send(sender, None, contract_artifact["bytecode"] + bytes(ctor_bytes),
                         gas=gas)
    address = normalize_address(receipt["contractAddress"])
    block = int(receipt["blockNumber"], 16)
    code = anvil.code(address, block)
    assert code, f"no runtime code at {address}"
    return address, block, keccak256(code)
