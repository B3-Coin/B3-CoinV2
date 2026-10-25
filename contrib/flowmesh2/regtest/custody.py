#!/usr/bin/env python3
"""Standalone local-EVM custody tooling, not a FlowMesh implementation.

Only Python's standard library and the pinned local Foundry tools are used.
RPC is restricted to literal loopback addresses, unforked Anvil and chain31337.
The fixture mnemonic is public test data. Never supply an actual wallet/key.
"""
# TODO(rebirth:M8-CUS-01) 3-of-4 EVM custody fixture; it contradicts the planned
# keyless VaultV2 (D2, SEC-1) and waits for the owner's custody ruling.

import argparse
import ipaddress
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

CHAIN_ID = 31337
Q_ASSET = "0x6439dd037ac1af8e258b4177319230f77f127c3fcf5ec6436aba005bb25bd7ed"
SYNTHETIC_ASSETS = {"Q": Q_ASSET,
                    "TOKENA": "0x9f84daf3e5e62cba57a6cc6dc2e02a3ac8fbae94d39cf085a3884f87e1533265",
                    "TOKENB": "0xfc416025aa6b00dd141d6dd9911cbfa7f0978707da4bfc3f0a4c7ca0c1c19283"}
TEST_MNEMONIC = "test test test test test test test test test test test junk"
MAX_ATOMS = 1 << 62
TOOLS_DEFAULT = "/home/josh/B3-tools/foundry-v1.8.5"
SOLC_DEFAULT = "/home/josh/B3-tools/solc-0.8.35"
WITHDRAWAL_ABI = "(bytes32,bytes32,bytes32,uint256,address,uint64,bytes32,uint64)"


class CustodyError(RuntimeError):
    pass


class RpcError(CustodyError):
    pass


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, file_pointer, code, message, headers, new_url):
        raise RpcError("RPC HTTP redirects are forbidden")


def expect(condition, message):
    if not condition:
        raise CustodyError("standalone assertion failed: " + message)


def hex_value(value, size=None):
    if not isinstance(value, str) or not re.fullmatch(r"0x[0-9a-fA-F]*", value):
        raise CustodyError("expected 0x-prefixed hexadecimal data")
    if len(value) % 2 != 0 or (size is not None and len(value) != 2 + 2 * size):
        raise CustodyError("hex data has the wrong byte length")
    return value.lower()


def address(value):
    return hex_value(value, 20)


def uint(value, bits=256):
    if isinstance(value, bool):
        raise CustodyError("boolean is not an unsigned integer")
    if isinstance(value, int):
        number = value
    elif isinstance(value, str) and re.fullmatch(r"[0-9]+", value):
        number = int(value)
    else:
        raise CustodyError("expected an exact integer or decimal-integer string")
    if not 0 <= number < (1 << bits):
        raise CustodyError("unsigned integer outside its range")
    return number


def q_units(amount):
    """Parse six-decimal Q without floats, exponent syntax or rounding."""
    if not isinstance(amount, str) or not re.fullmatch(r"[0-9]+(?:\.[0-9]{1,6})?", amount):
        raise CustodyError("Q amount must be a nonnegative decimal with at most six places")
    whole, dot, fraction = amount.partition(".")
    raw = int(whole) * 1_000_000 + (int(fraction.ljust(6, "0")) if dot else 0)
    if not 0 < raw <= MAX_ATOMS:
        raise CustodyError("Q amount outside (0, MAX_ATOMS]")
    return raw


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    # Atomic replacement must not broaden an existing private identity.
    # Set permissions before writing, including a retained temporary file.
    flags = os.O_WRONLY | os.O_CREAT | os.O_TRUNC | getattr(os, "O_NOFOLLOW", 0)
    fd = os.open(temporary, flags, 0o600)
    try:
        os.fchmod(fd, 0o600)
    except BaseException:
        # Refuse permission failures before writing; retain no open descriptor
        # when fdopen has not yet taken ownership of the private temporary.
        os.close(fd)
        raise
    with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)
    if hasattr(os, "O_DIRECTORY"):
        fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)


def local_rpc_url(url):
    parsed = urllib.parse.urlsplit(url)
    if parsed.scheme != "http" or parsed.username or parsed.password or parsed.query or parsed.fragment:
        raise CustodyError("RPC must be plain HTTP on a literal loopback address")
    if parsed.path not in ("", "/") or parsed.port is None:
        raise CustodyError("RPC URL must have an explicit local port and no extra path")
    try:
        host = ipaddress.ip_address(parsed.hostname or "")
    except ValueError as error:
        raise CustodyError("RPC host must be a literal loopback IP, not a DNS name") from error
    if not host.is_loopback or not 1 <= parsed.port <= 65535:
        raise CustodyError("non-loopback RPC is forbidden")
    return url


class LocalRpc:
    ALLOWED = {
        "eth_chainId", "eth_accounts", "eth_getBalance", "eth_blockNumber", "eth_getTransactionReceipt",
        "eth_getBlockByNumber", "eth_getBlockByHash", "eth_getTransactionByHash", "eth_getTransactionCount",
        "eth_gasPrice", "eth_estimateGas", "eth_sendTransaction", "eth_sendRawTransaction", "eth_call", "eth_getCode",
        "anvil_nodeInfo", "eth_getLogs", "eth_getStorageAt", "evm_increaseTime", "evm_mine", "evm_snapshot", "evm_revert",
    }
    READ_ONLY = {
        'eth_chainId', 'eth_accounts', 'eth_getBalance', 'eth_blockNumber', 'eth_getTransactionReceipt',
        'eth_getBlockByNumber', 'eth_getBlockByHash', 'eth_getTransactionByHash', 'eth_getTransactionCount',
        'eth_gasPrice', 'eth_call', 'eth_getCode', 'anvil_nodeInfo', 'eth_getLogs', 'eth_getStorageAt',
    }
    MAX_BATCH_ITEMS = 32
    MAX_REQUEST_BYTES = 1_048_576
    MAX_RESPONSE_BYTES = 4_194_304

    def __init__(self, url, timeout=10):
        self.url = local_rpc_url(url)
        self.timeout = timeout
        self.counter = 0
        self.counter_lock = threading.Lock()
        # Environment proxies must never forward local custody data elsewhere.
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())

    @staticmethod
    def _unique_fields(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('duplicate RPC JSON field')
            result[key] = value
        return result

    @staticmethod
    def _invalid_constant(value):
        raise ValueError('nonfinite RPC JSON number is forbidden')

    def _request_ids(self, count):
        with self.counter_lock:
            start = self.counter + 1
            self.counter += count
        return list(range(start, start + count))

    def _exchange(self, payload):
        data = json.dumps(payload, separators=(',', ':'), allow_nan=False).encode()
        if len(data) > self.MAX_REQUEST_BYTES:
            raise RpcError('RPC request exceeds 1MiB')
        request = urllib.request.Request(self.url, data=data, headers={"Content-Type": "application/json"})
        try:
            with self.opener.open(request, timeout=self.timeout) as reply:
                raw = reply.read(self.MAX_RESPONSE_BYTES + 1)
            if len(raw) > self.MAX_RESPONSE_BYTES:
                raise RpcError("RPC response exceeds 4MiB")
            return json.loads(raw, object_pairs_hook=self._unique_fields, parse_constant=self._invalid_constant)
        except (urllib.error.URLError, TimeoutError, OSError, UnicodeError, ValueError) as error:
            raise RpcError("local RPC transport/JSON failure: " + str(error)) from error

    @staticmethod
    def _result(result, request_id):
        if (not isinstance(result, dict) or type(result.get('id')) is not int or
                result['id'] != request_id or result.get('jsonrpc') != '2.0'):
            raise RpcError("RPC response identity mismatch")
        if 'error' in result:
            raise RpcError('local RPC error: ' + json.dumps(result['error'], sort_keys=True))
        if set(result) != {'jsonrpc', 'id', 'result'}:
            raise RpcError('RPC result has missing or unexpected fields')
        return result['result']

    def call(self, method, params=()):
        if not isinstance(method, str) or method not in self.ALLOWED:
            raise CustodyError("RPC method is outside the local fixture allowlist")
        request_id = self._request_ids(1)[0]
        return self._result(self._exchange({'jsonrpc': '2.0', 'id': request_id,
                            'method': method, 'params': list(params)}), request_id)

    def batch(self, calls):
        """Bounded read-only JSON-RPC batch; reject every incomplete response.

        Servers may reorder their responses. The exact ID set, not array
        position, binds each returned value to its requested operation.
        """
        if not isinstance(calls, (list, tuple)) or not 1 <= len(calls) <= self.MAX_BATCH_ITEMS:
            raise CustodyError('read-only RPC batch item count is outside its bound')
        for item in calls:
            if (not isinstance(item, (list, tuple)) or len(item) != 2 or not isinstance(item[0], str) or item[0] not in self.READ_ONLY or
                    not isinstance(item[1], (list, tuple))):
                raise CustodyError('RPC batches accept only explicit read-only method/parameter pairs')
        identifiers = self._request_ids(len(calls))
        requests = [{'jsonrpc': '2.0', 'id': identity, 'method': method, 'params': list(params)}
                    for identity, (method, params) in zip(identifiers, calls)]
        replies = self._exchange(requests)
        if not isinstance(replies, list) or len(replies) != len(requests):
            raise RpcError('RPC batch response is missing, partial or not an array')
        expected, found = set(identifiers), {}
        for reply in replies:
            if not isinstance(reply, dict) or type(reply.get('id')) is not int:
                raise RpcError('RPC batch response has an invalid ID')
            identity = reply['id']
            if identity not in expected or identity in found:
                raise RpcError('RPC batch response has a duplicate or unexpected ID')
            found[identity] = self._result(reply, identity)
        if set(found) != expected:
            raise RpcError('RPC batch response does not cover its exact request ID set')
        return [found[identity] for identity in identifiers]

    @staticmethod
    def _fixture_identity(chain, info):
        if not isinstance(chain, str) or chain != hex(CHAIN_ID):
            raise CustodyError('only local chain31337 is permitted')
        if (not isinstance(info, dict) or not isinstance(info.get('environment'), dict) or
                info['environment'].get('chainId') != CHAIN_ID):
            raise CustodyError('RPC must identify as the expected Anvil chain31337')
        fork = info.get('forkConfig')
        if not isinstance(fork, dict) or 'forkUrl' not in fork or 'forkBlockNumber' not in fork:
            raise CustodyError('unrecognized Anvil fork metadata; refusing to assume an unforked chain')
        if fork['forkUrl'] is not None or fork['forkBlockNumber'] is not None:
            raise CustodyError('forked EVM instances are forbidden')
        return info

    @staticmethod
    def _block_point(value):
        if not isinstance(value, dict) or not isinstance(value.get('number'), str):
            raise RpcError('canonical RPC snapshot block is missing or malformed')
        try:
            height = uint(int(value['number'], 16), 64)
            if value['number'] != hex(height):
                raise ValueError('noncanonical block number')
            return {'height': height, 'hash': hex_value(value['hash'], 32)}
        except (KeyError, TypeError, ValueError, CustodyError) as error:
            raise RpcError('canonical RPC snapshot block is missing or malformed') from error

    def state_batch(self, calls, *, block_heights=()):
        """Read identity/code/storage at one pinned canonical block.

        Calls omit their block parameter; this function supplies the same
        explicit number to every state read. A separate request rechecks its
        hash after all batch replies, since batch execution order is undefined.
        Optional explicit source block lookups share that bounded read batch.
        No mutable value or latest-state result is cached between invocations.
        """
        if not isinstance(calls, (list, tuple)) or not 1 <= len(calls) <= self.MAX_BATCH_ITEMS:
            raise CustodyError('canonical state batch item count is outside its bound')
        if not isinstance(block_heights, (list, tuple)) or len(block_heights) > 8:
            raise CustodyError('canonical snapshot source block list exceeds its bound')
        heights = [uint(height, 64) for height in block_heights]
        if len(set(heights)) != len(heights) or len(calls) + len(heights) > self.MAX_BATCH_ITEMS:
            raise CustodyError('canonical snapshot complete read set exceeds its bound or repeats a source block')
        prepared = []
        for item in calls:
            if not isinstance(item, (list, tuple)) or len(item) != 2 or not isinstance(item[1], (list, tuple)):
                raise CustodyError('canonical state batch requires explicit method/parameter pairs')
            method, params = item
            if method == 'eth_getCode' and len(params) == 1:
                params = [address(params[0])]
            elif method == 'eth_getStorageAt' and len(params) == 2:
                params = [address(params[0]), hex_value(params[1], 32)]
            elif (method == 'eth_call' and len(params) == 1 and isinstance(params[0], dict) and
                  set(params[0]) == {'to', 'data'}):
                params = [{'to': address(params[0]['to']), 'data': hex_value(params[0]['data'])}]
            else:
                raise CustodyError('canonical state batches accept only pinned code/storage/contract reads')
            prepared.append((method, params))
        chain, info, genesis, head = self.batch([
            ('eth_chainId', []), ('anvil_nodeInfo', []),
            ('eth_getBlockByNumber', ['0x0', False]), ('eth_getBlockByNumber', ['latest', False])])
        self._fixture_identity(chain, info)
        genesis, head = self._block_point(genesis), self._block_point(head)
        if genesis['height'] != 0:
            raise RpcError('RPC snapshot genesis has a nonzero height')
        reads = [(method, params + [hex(head['height'])]) for method, params in prepared]
        reads.extend(('eth_getBlockByNumber', [hex(height), False]) for height in heights)
        result = self.batch(reads)
        if heights:
            block, final_chain, final_info = self.batch([
                ('eth_getBlockByNumber', [hex(head['height']), False]),
                ('eth_chainId', []), ('anvil_nodeInfo', [])])
            self._fixture_identity(final_chain, final_info)
            checked = self._block_point(block)
        else:
            checked = self._block_point(self.call('eth_getBlockByNumber', [hex(head['height']), False]))
        if checked != head:
            raise RpcError('canonical RPC snapshot changed during its state batch; refusing mixed source state')
        snapshot = {'head': head, 'genesis': genesis, 'results': result[:len(prepared)]}
        if heights:
            snapshot['blocks'] = result[len(prepared):]
        return snapshot

    def assert_fixture(self):
        chain, info = self.batch([('eth_chainId', []), ('anvil_nodeInfo', [])])
        return self._fixture_identity(chain, info)


class Cast:
    def __init__(self, executable):
        self.executable = str(Path(executable).resolve(strict=True))
        self.public_encoding_cache = {}

    def run(self, *arguments):
        arguments = tuple(map(str, arguments))
        pure = bool(arguments) and arguments[0] in ('calldata', 'abi-encode', 'keccak')
        if pure and arguments in self.public_encoding_cache:
            return self.public_encoding_cache[arguments]
        result = subprocess.run([self.executable, *map(str, arguments)], check=False, capture_output=True,
                                text=True, timeout=30)
        if result.returncode:
            raise CustodyError("local cast command failed: " + result.stderr[-2000:].strip())
        output = result.stdout.strip()
        if pure:
            if len(self.public_encoding_cache) >= 4096:
                self.public_encoding_cache.clear()
            self.public_encoding_cache[arguments] = output
        return output

    def calldata(self, signature, *arguments):
        return hex_value(self.run("calldata", signature, *arguments))

    def abi_encode(self, signature, *arguments):
        return hex_value(self.run("abi-encode", signature, *arguments))

    def keccak(self, value):
        return hex_value(self.run("keccak", value), 32)

    def test_address(self, index):
        return address(self.run("wallet", "address", "--mnemonic", TEST_MNEMONIC, "--mnemonic-index", uint(index, 31)))

    def sign_fixture_digest(self, index, digest):
        """One public fixture key only. Production/operator consensus is not implemented here."""
        return hex_value(self.run("wallet", "sign", "--mnemonic", TEST_MNEMONIC,
                                  "--mnemonic-index", uint(index, 31), "--no-hash", hex_value(digest, 32)), 65)


def words(encoded, count):
    value = bytes.fromhex(hex_value(encoded)[2:])
    if len(value) != count * 32:
        raise CustodyError("ABI return/event data has the wrong fixed length")
    return [value[i * 32:(i + 1) * 32] for i in range(count)]


def word_address(word):
    if any(word[:12]):
        raise CustodyError("noncanonical ABI address padding")
    return "0x" + word[12:].hex()


class CustodyDriver:
    """Bounded local EVM operations; request_withdrawal accepts external signatures.

    This class does not decide or certify FlowMesh state. Each actual operator
    must validate inputs/execute state and persist its own signing obligations.
    """

    def __init__(self, config, evidence_dir):
        self.config = dict(config)
        self.relayer = None
        from custody_policy import LEGACY_PROFILE, PROFILE
        self.custody_profile = config.get('custody_profile', LEGACY_PROFILE)
        if self.custody_profile not in (LEGACY_PROFILE, PROFILE):
            raise CustodyError('unknown custody release profile')
        if config.get('mainnet_enabled', False) is not False:
            raise CustodyError('mainnet custody remains disabled in this release')
        self.evidence = Path(evidence_dir).resolve()
        self.evidence.mkdir(parents=True, exist_ok=True)
        if 'regtest_arbitrum_fork' in config:
            from arbitrum_fork import ForkRpc
            self.rpc = ForkRpc(config["rpc_url"], config['regtest_arbitrum_fork'])
        else:
            self.rpc = LocalRpc(config["rpc_url"])
        self.cast = Cast(config["cast"])
        self.rpc.assert_fixture()
        if config.get("test_mnemonic", TEST_MNEMONIC) != TEST_MNEMONIC:
            raise CustodyError("only the public default Anvil test mnemonic is supported")
        self.accounts = [address(item) for item in self.rpc.call("eth_accounts")]
        count = uint(config.get("account_count", 6), 8)
        if count < 5 or len(self.accounts) < count:
            raise CustodyError("Anvil must expose at least the configured funded test accounts")
        self.accounts = self.accounts[:count]
        for index, account in enumerate(self.accounts):
            if account != self.cast.test_address(index) or int(self.rpc.call("eth_getBalance", [account, "latest"]), 16) == 0:
                raise CustodyError("account is not the funded expected public Anvil fixture account")
        self.operator_indexes = [uint(i, 8) for i in config.get("operator_indexes", [1, 2, 3, 4])]
        if len(self.operator_indexes) != 4 or len(set(self.operator_indexes)) != 4 or max(self.operator_indexes) >= count:
            raise CustodyError("exactly four distinct known test operator indexes are required")
        self.operator_indexes.sort(key=lambda index: self.accounts[index])
        self.operator_addresses = [self.accounts[index] for index in self.operator_indexes]
        if "operator_addresses" in config:
            supplied = [address(value) for value in config["operator_addresses"]]
            if (len(supplied) != 4 or supplied != sorted(supplied) or len(set(supplied)) != 4 or
                    "0x" + "00" * 20 in supplied):
                raise CustodyError("operator_addresses must be exactly four sorted distinct nonzero public addresses")
            self.operator_addresses = supplied
        self.deployer_index = uint(config.get("deployer_index", 0), 8)
        if self.deployer_index >= count:
            raise CustodyError("deployer must be a funded known test account")
        self.token = address(config["token"]) if config.get("token") else None
        self.vault = address(config["vault"]) if config.get("vault") else None
        self.instance = hex_value(config["instance"], 32)
        if hex_value(config.get("q_asset", Q_ASSET), 32) != Q_ASSET:
            raise CustodyError("the first fixture supports only the exact synthetic regtest Q AssetId")
        self.asset = hex_value(config.get("custody_asset", Q_ASSET), 32)
        if self.asset not in SYNTHETIC_ASSETS.values():
            raise CustodyError("custody routes are limited to exact synthetic TOKENA/TOKENB/Q AssetIds")
        if 'regtest_arbitrum_fork' in self.config and self.token and self.vault and self.custody_profile != PROFILE:
            raise CustodyError('bound local Arbitrum fork routes require the explicit custody-v2 regtest policy')
        if self.token and self.vault:
            self.verify_contract_identity()

    @classmethod
    def from_config(cls, path, evidence_dir=None):
        path = Path(path).resolve(strict=True)
        return cls(json.loads(path.read_text(encoding="utf-8")), evidence_dir or path.parent)

    def call(self, target, signature, *arguments):
        self.rpc.assert_fixture()
        return self.rpc.call("eth_call", [{"to": address(target), "data": self.cast.calldata(signature, *arguments)}, "latest"])

    def call_uint(self, target, signature, *arguments):
        return int.from_bytes(words(self.call(target, signature, *arguments), 1)[0], "big")

    def verify_contract_identity(self):
        from custody_policy import PROFILE, verify_custody
        if 'regtest_arbitrum_fork' in self.config and self.custody_profile != PROFILE:
            raise CustodyError('local Arbitrum fork custody cannot use the legacy fixture policy')
        if self.custody_profile == PROFILE:
            # Public identity/policy is recomputed independently, and all live
            # code/chain/roster checks still run for every observation/signature.
            return verify_custody(self)
        if self.call_uint(self.token, "decimals()") != 6:
            raise CustodyError("fixture token must have six decimals")
        token = word_address(words(self.call(self.vault, "TOKEN()"), 1)[0])
        q = hex_value(self.call(self.vault, "Q_ASSET_ID()"), 32)
        instance = hex_value(self.call(self.vault, "INSTANCE_ID()"), 32)
        if token != self.token or q != self.asset or instance != self.instance:
            raise CustodyError("vault token/Q/instance identity differs from configuration")
        for position, operator in enumerate(self.operator_addresses):
            if word_address(words(self.call(self.vault, "operators(uint256)", position), 1)[0]) != operator:
                raise CustodyError("vault operator roster differs from configuration")

    def canonical_receipt(self, tx_hash, timeout=20, require_success=True):
        tx_hash = hex_value(tx_hash, 32)
        deadline = time.monotonic() + timeout
        receipt = None
        while time.monotonic() < deadline:
            receipt = self.rpc.call("eth_getTransactionReceipt", [tx_hash])
            if receipt is not None:
                break
            time.sleep(0.1)
        if not isinstance(receipt, dict):
            raise CustodyError("transaction outcome remains unknown; retain and retry its exact saved intent")
        self.rpc.assert_fixture()
        if hex_value(receipt["transactionHash"], 32) != tx_hash:
            raise CustodyError("receipt names another transaction")
        block_number = int(receipt["blockNumber"], 16)
        block_hash = hex_value(receipt["blockHash"], 32)
        block = self.rpc.call("eth_getBlockByNumber", [hex(block_number), False])
        by_hash = self.rpc.call("eth_getBlockByHash", [block_hash, False])
        if (not isinstance(block, dict) or not isinstance(by_hash, dict) or hex_value(block["hash"], 32) != block_hash or
                hex_value(by_hash["hash"], 32) != block_hash or int(by_hash["number"], 16) != block_number):
            raise CustodyError("receipt block is not the current canonical block at its height")
        position = int(receipt["transactionIndex"], 16)
        if position < 0 or position >= len(block["transactions"]) or hex_value(block["transactions"][position], 32) != tx_hash:
            raise CustodyError("transaction is not at the receipt's canonical block position")
        seen_logs = set()
        for log in receipt["logs"]:
            if log.get("removed") is not False or hex_value(log["blockHash"], 32) != block_hash or hex_value(log["transactionHash"], 32) != tx_hash:
                raise CustodyError("receipt contains removed or differently anchored logs")
            log_index = int(log["logIndex"], 16)
            if (int(log["blockNumber"], 16) != block_number or int(log["transactionIndex"], 16) != position or
                    log_index < 0 or log_index in seen_logs):
                raise CustodyError("receipt contains inconsistent or duplicate log positions")
            seen_logs.add(log_index)
        status = int(receipt["status"], 16)
        if status not in (0, 1):
            raise CustodyError("receipt has a noncanonical EVM execution status")
        if require_success and status != 1:
            raise CustodyError("EVM transaction reverted: " + tx_hash)
        return receipt

    @staticmethod
    def _same_transaction(found, payload):
        keys = ("from", "to", "input", "value", "gas", "gasPrice", "nonce")
        wanted = dict(payload)
        wanted["input"] = wanted.pop("data")
        for key in keys:
            left, right = found.get(key), wanted.get(key)
            if key in ("gas", "gasPrice", "nonce", "value"):
                if int(left or "0x0", 16) != int(right or "0x0", 16):
                    return False
            elif (left or "").lower() != (right or "").lower():
                return False
        return True

    def _find_nonce(self, payload):
        latest = int(self.rpc.call("eth_blockNumber"), 16)
        heights = ["pending"] + [hex(n) for n in range(latest, max(-1, latest - 128), -1)]
        for height in heights:
            block = self.rpc.call("eth_getBlockByNumber", [height, True])
            for item in (block or {}).get("transactions", []):
                if address(item["from"]) == payload["from"] and int(item["nonce"], 16) == int(payload["nonce"], 16):
                    if not self._same_transaction(item, payload):
                        raise CustodyError("saved nonce was used by a different transaction; refusing a replacement")
                    return hex_value(item["hash"], 32)
        return None

    def resume_intent(self, intent_path, require_success=True):
        """Retry identical sender/nonce/body; never assign a replacement nonce."""
        intent_path = Path(intent_path).resolve(strict=True)
        intent = json.loads(intent_path.read_text(encoding="utf-8"))
        if intent.get("rpc_url") != self.rpc.url or intent.get("instance") != self.instance:
            raise CustodyError("transaction intent belongs to another local instance")
        payload = intent["payload"]
        if address(payload["from"]) not in self.accounts or int(payload["chainId"], 16) != CHAIN_ID:
            raise CustodyError("transaction intent sender/chain is not a known local test account")
        self.rpc.assert_fixture()
        tx_hash = intent.get("transaction_hash")
        if tx_hash:
            tx_hash = hex_value(tx_hash, 32)
            existing = self.rpc.call("eth_getTransactionByHash", [tx_hash])
            if existing is None:
                tx_hash = None  # Evicted/dropped submissions may be rebroadcast identically.
            elif not self._same_transaction(existing, payload):
                raise CustodyError("saved transaction hash identifies another payload")
            else:
                prior_receipt = self.rpc.call("eth_getTransactionReceipt", [tx_hash])
                if prior_receipt is not None:
                    canonical = self.rpc.call("eth_getBlockByNumber", [prior_receipt["blockNumber"], False])
                    if canonical is None or hex_value(canonical["hash"], 32) != hex_value(prior_receipt["blockHash"], 32):
                        tx_hash = None  # Reorg: look for this exact nonce in the new canonical chain/pool.
        if not tx_hash:
            tx_hash = self._find_nonce(payload)
            consumed = int(self.rpc.call("eth_getTransactionCount", [payload["from"], "latest"]), 16)
            if not tx_hash and consumed > int(payload["nonce"], 16):
                raise CustodyError("saved nonce is consumed but its exact transaction is outside the bounded lookup")
        for attempt in range(3):
            if tx_hash:
                break
            try:
                tx_hash = hex_value(self.rpc.call("eth_sendTransaction", [payload]), 32)
            except RpcError:
                tx_hash = self._find_nonce(payload)
                if not tx_hash and attempt == 2:
                    raise CustodyError("send outcome unresolved; retain exact intent " + str(intent_path))
        intent["transaction_hash"] = tx_hash
        atomic_json(intent_path, intent)
        # A mined revert is a resolved intent too. Persist its status before
        # reporting failure so it cannot permanently block the sender nonce.
        receipt = self.canonical_receipt(tx_hash, require_success=False)
        transaction = self.rpc.call("eth_getTransactionByHash", [tx_hash])
        if not isinstance(transaction, dict) or not self._same_transaction(transaction, payload):
            raise CustodyError("receipt transaction differs from the durable submitted intent")
        atomic_json(self.evidence / "receipts" / (tx_hash + ".json"), receipt)
        intent["receipt_status"] = int(receipt["status"], 16)
        atomic_json(intent_path, intent)
        if require_success and intent["receipt_status"] != 1:
            raise CustodyError("EVM transaction reverted: " + tx_hash)
        return receipt

    def transact(self, target, data, sender_index=None, gas=None, require_success=True, native_value=0):
        self.rpc.assert_fixture()
        if self.token and self.vault:
            self.verify_contract_identity()
        index = self.deployer_index if sender_index is None else uint(sender_index, 8)
        if index >= len(self.accounts):
            raise CustodyError("sender is outside the funded known Anvil accounts")
        sender = self.accounts[index]
        if 'regtest_arbitrum_fork' in self.config and self.asset == Q_ASSET and self.token is not None:
            from arbitrum_fork import verify_fork_driver
            verify_fork_driver(self, accounts=[sender], check_compliance=True)
        # Unknown older submissions block new work instead of being silently
        # replaced by the next nonce after a timeout or process interruption.
        for existing in sorted((self.evidence / "intents").glob(sender + "-*.json")):
            prior = json.loads(existing.read_text(encoding="utf-8"))
            if "receipt_status" not in prior:
                raise CustodyError("resolve saved sender intent before new work: " + str(existing))
        nonce = int(self.rpc.call("eth_getTransactionCount", [sender, "pending"]), 16)
        payload = {"from": sender, "nonce": hex(nonce), "data": hex_value(data), "value": hex(uint(native_value)),
                   "gasPrice": self.rpc.call("eth_gasPrice"), "chainId": hex(CHAIN_ID), "type": "0x0"}
        if target is not None:
            payload["to"] = address(target)
        estimated = uint(gas) if gas is not None else int(self.rpc.call("eth_estimateGas", [payload]), 16) * 6 // 5 + 1000
        if not 21_000 <= estimated <= 15_000_000:
            raise CustodyError("gas estimate outside local fixture bounds")
        payload["gas"] = hex(estimated)
        path = self.evidence / "intents" / (sender + "-" + str(nonce).zfill(12) + ".json")
        if path.exists():
            raise CustodyError("nonce already has an intent; resume it explicitly")
        atomic_json(path, {"rpc_url": self.rpc.url, "instance": self.instance, "payload": payload})
        return self.resume_intent(path, require_success=require_success)

    def deploy(self, artifact, constructor_data="0x"):
        artifact = json.loads(Path(artifact).read_text(encoding="utf-8"))
        bytecode = hex_value(artifact["bytecode"]["object"])
        receipt = self.transact(None, bytecode + hex_value(constructor_data)[2:])
        deployed = address(receipt["contractAddress"])
        if self.rpc.call("eth_getCode", [deployed, "latest"]) == "0x":
            raise CustodyError("deployment has no runtime code")
        return deployed, receipt

    def mint(self, recipient_index, amount):
        recipient = self.accounts[uint(recipient_index, 8)]
        if 'regtest_arbitrum_fork' in self.config and self.asset == Q_ASSET:
            from arbitrum_assets import fund_local_fork_account
            from arbitrum_fork import verify_fork_driver
            verify_fork_driver(self)
            return fund_local_fork_account(self.rpc, self.cast, self.config['regtest_arbitrum_fork']['asset_pin'],
                                           recipient, uint(amount))
        return self.transact(self.token, self.cast.calldata("mint(address,uint256)", recipient, uint(amount)))

    def approve(self, sender_index, amount):
        return self.transact(self.token, self.cast.calldata("approve(address,uint256)", self.vault, uint(amount)), sender_index)

    def deposit(self, sender_index, account_id, amount):
        return self.transact(self.vault, self.cast.calldata("deposit(uint256,bytes32)", uint(amount), hex_value(account_id, 32)), sender_index)

    def _event(self, receipt, signature, topic_count, data_words, log_index=None):
        self.verify_contract_identity()
        receipt = self.canonical_receipt(receipt["transactionHash"])
        topic = self.cast.keccak(signature)
        selected = [log for log in receipt["logs"] if address(log["address"]) == self.vault and log["topics"] and hex_value(log["topics"][0], 32) == topic]
        if log_index is not None:
            selected = [log for log in selected if int(log['logIndex'], 16) == uint(log_index, 32)]
        if len(selected) != 1:
            raise CustodyError("expected exactly one canonical vault event: " + signature)
        log = selected[0]
        if len(log["topics"]) != topic_count:
            raise CustodyError("event has the wrong indexed-field count")
        data = words(log["data"], data_words)
        base = {"chain_id": CHAIN_ID, "token": self.token, "vault": self.vault, "instance": self.instance,
                "asset": self.asset, "transaction_hash": hex_value(receipt["transactionHash"], 32),
                "log_index": int(log["logIndex"], 16), "block_number": int(receipt["blockNumber"], 16),
                "block_hash": hex_value(receipt["blockHash"], 32)}
        return log, data, base

    def expected_deposit(self, receipt, account_id=None, amount=None, sender=None, log_index=None):
        log, data, record = self._event(receipt, "Deposit(uint64,bytes32,bytes32,uint256,address)", 4, 2, log_index)
        deposit_id = int(hex_value(log["topics"][1], 32), 16)
        account = hex_value(log["topics"][2], 32)
        asset = hex_value(log["topics"][3], 32)
        received = int.from_bytes(data[0], "big")
        depositor = word_address(data[1])
        if deposit_id >= 1 << 64 or account == "0x" + "00" * 32 or asset != self.asset or not 0 < received <= MAX_ATOMS:
            raise CustodyError("deposit event fields violate fixture policy")
        if ((account_id is not None and account != hex_value(account_id, 32)) or
                (amount is not None and received != uint(amount)) or (sender is not None and depositor != address(sender))):
            raise CustodyError("deposit receipt does not match the expected account/amount/sender")
        record.update({"kind": "deposit", "deposit_id": deposit_id, "account": account,
                       "amount_raw": str(received), "sender": depositor})
        return record

    def expected_paid(self, receipt, withdrawal_id=None, amount=None, recipient=None, log_index=None):
        log, data, record = self._event(receipt, "Withdrawn(bytes32,address,uint256)", 3, 1, log_index)
        identity = hex_value(log["topics"][1], 32)
        paid_to = word_address(bytes.fromhex(hex_value(log["topics"][2], 32)[2:]))
        paid = int.from_bytes(data[0], "big")
        if not 0 < paid <= MAX_ATOMS or paid_to == "0x" + "00" * 20:
            raise CustodyError("payout event fields violate fixture policy")
        if ((withdrawal_id is not None and identity != hex_value(withdrawal_id, 32)) or
                (amount is not None and paid != uint(amount)) or (recipient is not None and paid_to != address(recipient))):
            raise CustodyError("payout receipt does not match the expected withdrawal/amount/recipient")
        record.update({"kind": "paid", "withdrawal_id": identity, "amount_raw": str(paid), "recipient": paid_to})
        return record

    @staticmethod
    def withdrawal_tuple(value):
        required = {"id", "account", "asset", "amount", "recipient", "slot", "stateRoot", "validUntil"}
        if set(value) != required or hex_value(value["asset"], 32) != Q_ASSET:
            raise CustodyError("withdrawal must contain exactly the fixture's authenticated fields and Q asset")
        fields = [hex_value(value["id"], 32), hex_value(value["account"], 32), Q_ASSET, str(uint(value["amount"])),
                  address(value["recipient"]), str(uint(value["slot"], 64)), hex_value(value["stateRoot"], 32),
                  str(uint(value["validUntil"], 64))]
        return "(" + ",".join(fields) + ")"

    def withdrawal_digest(self, withdrawal):
        from custody_policy import PROFILE, withdrawal_digest
        if self.custody_profile == PROFILE:
            return withdrawal_digest(self, withdrawal)
        return hex_value(self.call(self.vault, "withdrawalDigest(" + WITHDRAWAL_ABI + ")", self.withdrawal_tuple(withdrawal)), 32)

    def request_withdrawal(self, withdrawal, signatures, sender_index=None):
        """Signatures must come from independent operators; no certificate is fabricated."""
        if not 3 <= len(signatures) <= 4:
            raise CustodyError("three or four independently supplied operator signatures are required")
        if 'regtest_arbitrum_fork' in self.config:
            from arbitrum_fork import verify_fork_driver
            verify_fork_driver(self, accounts=[withdrawal['recipient']], check_compliance=True)
        from custody_policy import PROFILE
        if self.custody_profile == PROFILE:
            if self.relayer is None or sender_index is not None:
                raise CustodyError('custody-v2 payout requires its dedicated private signed gas relayer')
            return self.relayer.request('request', withdrawal=withdrawal, signatures=signatures)
        encoded = "[" + ",".join(hex_value(signature, 65) for signature in signatures) + "]"
        return self.transact(self.vault, self.cast.calldata("requestWithdrawal(" + WITHDRAWAL_ABI + ",bytes[])",
                                                         self.withdrawal_tuple(withdrawal), encoded), sender_index)

    def finalize(self, withdrawal_id, sender_index=None, require_success=True, gas=None):
        from custody_policy import PROFILE
        if 'regtest_arbitrum_fork' in self.config:
            from arbitrum_fork import verify_fork_driver
            verify_fork_driver(self, accounts=[self.withdrawal_status(withdrawal_id)['recipient']], check_compliance=True)
        if self.custody_profile == PROFILE:
            if self.relayer is None or sender_index is not None:
                raise CustodyError('custody-v2 payout requires its dedicated private signed gas relayer')
            return self.relayer.request('finalize', withdrawal_id=hex_value(withdrawal_id, 32),
                                        require_success=require_success, gas=gas)
        return self.transact(self.vault, self.cast.calldata("finalizeWithdrawal(bytes32)", hex_value(withdrawal_id, 32)),
                             sender_index, gas, require_success)

    def withdrawal_status(self, withdrawal_id):
        result = words(self.call(self.vault, "withdrawals(bytes32)", hex_value(withdrawal_id, 32)), 5)
        return {"digest": "0x" + result[0].hex(), "recipient": word_address(result[1]),
                "amount_raw": str(int.from_bytes(result[2], "big")), "release_at": int.from_bytes(result[3], "big"),
                "paid": bool(int.from_bytes(result[4], "big"))}

    def balances(self, account):
        account = address(account)
        return {"account": account, "q_raw": str(self.call_uint(self.token, "balanceOf(address)", account)),
                "native_wei": str(int(self.rpc.call("eth_getBalance", [account, "latest"]), 16)),
                "locked_raw": str(self.call_uint(self.vault, "locked()")),
                "pending_raw": str(self.call_uint(self.vault, "pendingTotal()"))}

    def mine(self, blocks=1, advance_seconds=0):
        self.rpc.assert_fixture()
        blocks, advance_seconds = uint(blocks, 16), uint(advance_seconds, 32)
        if not 1 <= blocks <= 1000 or advance_seconds > 3600:
            raise CustodyError("mining/time step exceeds the local fixture bound")
        if advance_seconds:
            self.rpc.call("evm_increaseTime", [advance_seconds])
        for _ in range(blocks):
            self.rpc.call("evm_mine")


def fixture(args):
    source = Path(args.source_root).resolve(strict=True)
    evidence = Path(args.evidence_dir).resolve()
    if evidence == source or source in evidence.parents:
        raise CustodyError("evidence/build/data must be outside the source checkout")
    if evidence.exists() and any(evidence.iterdir()):
        raise CustodyError("standalone fixture needs a fresh empty evidence directory")
    evidence.mkdir(parents=True, exist_ok=True)
    tools = Path(args.tools).resolve(strict=True)
    solc = Path(args.solc).resolve(strict=True)
    out = evidence / "contracts-out"
    build = [str(tools / "forge"), "build", "--root", str(source / "contracts"), "--offline", "--use", str(solc),
             "--out", str(out), "--cache-path", str(evidence / "contracts-cache")]
    with (evidence / "forge-build.log").open("w", encoding="utf-8") as log:
        subprocess.run(build, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=600)
    with socket.socket() as local:
        local.bind(("127.0.0.1", 0))
        port = local.getsockname()[1]
    url = "http://127.0.0.1:" + str(port)
    # Fixed arguments only. No fork URL, remote provider or arbitrary extra flags.
    command = [str(tools / "anvil"), "--host", "127.0.0.1", "--port", str(port), "--chain-id", str(CHAIN_ID),
               "--mnemonic", TEST_MNEMONIC, "--accounts", "6", "--balance", "10000", "--silent"]
    log = (evidence / "anvil.log").open("w", encoding="utf-8")
    process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
    try:
        rpc = LocalRpc(url)
        for _ in range(100):
            if process.poll() is not None:
                raise CustodyError("owned local Anvil exited; inspect its retained log")
            try:
                rpc.assert_fixture()
                break
            except RpcError:
                time.sleep(0.1)
        else:
            raise CustodyError("owned local Anvil did not start within its bound")
        cast = Cast(tools / "cast")
        config = {"rpc_url": url, "cast": str(tools / "cast"), "account_count": 6, "deployer_index": 0,
                  "operator_indexes": [1, 2, 3, 4], "q_asset": Q_ASSET, "test_mnemonic": TEST_MNEMONIC,
                  "instance": cast.keccak("TEST rebirth standalone EVM " + uuid.uuid4().hex), "standalone_fixture": True}
        driver = CustodyDriver(config, evidence)
        token, token_receipt = driver.deploy(out / "RebirthRegtestToken.sol" / "RebirthRegtestToken.json")
        driver.token = config["token"] = token
        roster = "[" + ",".join(driver.accounts[index] for index in driver.operator_indexes) + "]"
        dispute_seconds = 30
        constructor = cast.abi_encode("constructor(address,bytes32,bytes32,address[4],uint64)", token, Q_ASSET,
                                      driver.instance, roster, dispute_seconds)
        vault, vault_receipt = driver.deploy(out / "RebirthRegtestVault.sol" / "RebirthRegtestVault.json", constructor)
        driver.vault = config["vault"] = vault
        driver.verify_contract_identity()
        config["accounts"] = driver.accounts
        config["operator_addresses"] = [driver.accounts[index] for index in driver.operator_indexes]
        config["operator_indexes"] = driver.operator_indexes
        config["domain_separator"] = hex_value(driver.call(vault, "DOMAIN_SEPARATOR()"), 32)
        config["dispute_seconds"] = dispute_seconds
        atomic_json(evidence / "custody-config.json", config)
        account = cast.keccak("TEST standalone FlowMesh account")
        driver.mint(0, q_units("10.000000"))
        driver.approve(0, q_units("5.000000"))
        deposited = driver.deposit(0, account, q_units("5.000000"))
        deposit_record = driver.expected_deposit(deposited, account, q_units("5.000000"), driver.accounts[0])
        state_root = cast.keccak("TEST standalone state commitment; not a FlowMesh certificate")

        def request(number, amount):
            timestamp = int(rpc.call("eth_getBlockByNumber", ["latest", False])["timestamp"], 16)
            withdrawal = {"id": "0x" + number.to_bytes(32, "big").hex(), "account": account, "asset": Q_ASSET,
                          "amount": str(amount), "recipient": driver.accounts[5], "slot": number,
                          "stateRoot": state_root, "validUntil": timestamp + 120}
            digest = driver.withdrawal_digest(withdrawal)
            # Standalone EVM test only. These signatures assert synthetic test
            # data; they are never labelled a completed FlowMesh decision.
            signatures = [cast.sign_fixture_digest(index, digest) for index in driver.operator_indexes[:3]]
            receipt = driver.request_withdrawal(withdrawal, signatures)
            return withdrawal, signatures, receipt

        withdrawal, signatures, queued = request(1, q_units("1.250000"))
        early = driver.finalize(withdrawal["id"], require_success=False, gas=200_000)
        expect(int(early["status"], 16) == 0, "early payout must revert")
        driver.mine(advance_seconds=dispute_seconds)
        payout = driver.finalize(withdrawal["id"])
        paid_record = driver.expected_paid(payout, withdrawal["id"], q_units("1.250000"), driver.accounts[5])
        replay = driver.finalize(withdrawal["id"], require_success=False, gas=200_000)
        expect(int(replay["status"], 16) == 0, "replayed payout must revert")
        expect(driver.balances(driver.accounts[5])["q_raw"] == "1250000", "first payout must be exact")

        second, _, _ = request(2, q_units("0.500000"))
        pause_digest = hex_value(driver.call(vault, "pauseDigest(bool,uint64)", "true", 1), 32)
        pause_sigs = [cast.sign_fixture_digest(index, pause_digest) for index in driver.operator_indexes[:3]]
        driver.transact(vault, cast.calldata("setPaused(bool,uint64,bytes[])", "true", 1, "[" + ",".join(pause_sigs) + "]"))
        driver.mine(advance_seconds=dispute_seconds)
        paused = driver.finalize(second["id"], require_success=False, gas=200_000)
        expect(int(paused["status"], 16) == 0 and not driver.withdrawal_status(second["id"])["paid"],
               "pause must preserve the unpaid claim")
        unpause_digest = hex_value(driver.call(vault, "pauseDigest(bool,uint64)", "false", 2), 32)
        unpause_sigs = [cast.sign_fixture_digest(index, unpause_digest) for index in driver.operator_indexes[:3]]
        driver.transact(vault, cast.calldata("setPaused(bool,uint64,bytes[])", "false", 2, "[" + ",".join(unpause_sigs) + "]"))
        driver.transact(token, cast.calldata("setTransfersEnabled(bool)", "false"))
        failed = driver.finalize(second["id"], require_success=False, gas=200_000)
        expect(int(failed["status"], 16) == 0 and not driver.withdrawal_status(second["id"])["paid"],
               "failed token transfer must preserve the unpaid claim")
        expect(driver.balances(driver.accounts[5])["pending_raw"] == "500000", "failed transfer must retain reserves")
        driver.transact(token, cast.calldata("setTransfersEnabled(bool)", "true"))
        recovered = driver.finalize(second["id"])
        recovered_record = driver.expected_paid(recovered, second["id"], 500_000, driver.accounts[5])
        balances = driver.balances(driver.accounts[5])
        expect(balances["q_raw"] == "1750000" and balances["locked_raw"] == "3250000" and balances["pending_raw"] == "0",
               "recovered payout and reserve totals must reconcile")
        expect(driver.call_uint(token, "balanceOf(address)", vault) == 3_250_000,
               "actual vault token balance must equal locked reserves")
        report = {"passed": True, "scope": "standalone unforked local EVM custody fixture only; not FlowMesh qualification",
                  "chain_id": CHAIN_ID, "config": config, "anvil_info": rpc.assert_fixture(),
                  "deposit": deposit_record, "payout": paid_record, "recovered_payout": recovered_record,
                  "final_balances": balances, "assertions": ["actual deposit", "3of4 raw ECDSA",
                  "dispute delay", "exact-once payout", "replayed payout reverted", "quorum pause",
                  "failed transfer preserved claim", "same claim recovered", "reserve reconciliation"],
                  "owned_anvil_command": command, "token_deployment": token_receipt["transactionHash"],
                  "vault_deployment": vault_receipt["transactionHash"]}
        atomic_json(evidence / "standalone-evm-report.json", report)
        print(json.dumps({"passed": True, "report": str(evidence / "standalone-evm-report.json")}, sort_keys=True))
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
        log.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    stand = commands.add_parser("standalone-test")
    stand.add_argument("--source-root", required=True)
    stand.add_argument("--evidence-dir", required=True)
    stand.add_argument("--tools", default=TOOLS_DEFAULT)
    stand.add_argument("--solc", default=SOLC_DEFAULT)
    units = commands.add_parser("q-units")
    units.add_argument("amount")
    receipt = commands.add_parser("receipt")
    receipt.add_argument("--config", required=True)
    receipt.add_argument("--transaction", required=True)
    receipt.add_argument("--kind", choices=["deposit", "paid"], required=True)
    resume = commands.add_parser("resume")
    resume.add_argument("--config", required=True)
    resume.add_argument("--intent", required=True)
    args = parser.parse_args()
    try:
        if args.command == "standalone-test":
            fixture(args)
        elif args.command == "q-units":
            print(q_units(args.amount))
        else:
            driver = CustodyDriver.from_config(args.config)
            if args.command == "resume":
                result = driver.resume_intent(args.intent)
            else:
                data = driver.canonical_receipt(args.transaction)
                result = driver.expected_deposit(data) if args.kind == "deposit" else driver.expected_paid(data)
            print(json.dumps(result, indent=2, sort_keys=True))
    except (CustodyError, subprocess.CalledProcessError, subprocess.TimeoutExpired, ValueError, KeyError, AssertionError) as error:
        print("local EVM custody fixture failed: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
