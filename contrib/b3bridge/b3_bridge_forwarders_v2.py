#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""REGTEST ONLY bridge V2 forwarder relayer: route (b) deposit forwarders F_d
and route (d3) exit forwarders F_x (doc/design/b3-bridge-v2-contracts-spec.md
sections 7 to 9).

Subcommands (each one shot):

flush
    Deploys a funded F_d if it has no code yet, then flushes it:
    B3DepositForwarderFactory.flush(p, fee) deposits min(free - fee,
    MAX_DEPOSIT_RAW) into the launch Vault V2 for p.b3Recipient, which B3
    mints as bUSD V2 once the Deposit is proven. p comes from --params (the
    wallet's bridgeforwarderaddress result, or its params object) or, with
    --account, from the factory's Published events (a relayer nobody handed
    p). In order:
    1. deposit_guard (b3_bridge_deposit_guard_v2.py): once B3 has closed
       bridge V2 deposits for good, exit 1 and send nothing;
    2. refuse a retired launch vault entry (getbridgev2info.retired): the
       factory deposits only into the launch vault, whose later deposits are
       only swept to the successor;
    3. the factory must be the pinned deposit factory, p.vault the pinned
       vault, and factory.accountOf(p) the derivation made here under the
       pinned init-code hash;
    4. Vault V2 depositGateOpen() must be true;
    5. report nothing-to-flush and send nothing when the free balance is
       below MIN_FLUSH_RAW + fee (an expired F_d or a fee above the rest of
       the lifetime fee cap is refused);
    6. B3 must be able to mint the deposit promptly (node changes section
       11.3): once the V2 light client is bootstrapped it must be fresh at
       B3 (getbridgev2info light_client.fresh) and its horizon at least 7
       days past the latest Ethereum block (light_client.horizon_time;
       section 12.3), and the amount must fit max_per_block and the mint
       epoch's epoch_mint_headroom. Before the bootstrap every mint waits for
       it anyway, so only the caps apply. A refusal sends nothing; the
       funds stay in F_d for a later run, a refund or returnToOwner;
    7. otherwise deploy if codeless, then flush, each simulated by eth_call
       first, and check the flush receipt: exactly the token Transfer
       (F_d -> vault) and the vault Deposit (PR-1, D1), its worst-case
       receipt-trie leaf within 7,168 bytes (PR-3). PR-1 and PR-2 fix the
       flush's log shape, so the prediction is a constant 634 bytes and
       only this post-check can find a larger leaf.
    One flush per run deposits at most MAX_DEPOSIT_RAW; remaining_free says
    whether to run again.

forward
    Releases a route (d3) burn's leaf V2 to its exit forwarder F_x and
    forwards F_x through the USDT0 OFT adapter in one
    B3RelayBatch.relayWithForwards call, without certificates (relay the
    covering certificate first), the F_x fee paying for both.
    getbridgewithdrawalproofv2 must return the burn's exit block, and F_x
    derived here must equal its recipient and exitFactory.accountOf(p). A
    withdrawal already released is forwarded without a release item; one
    whose F_x also holds nothing is refused. The batch is simulated by
    eth_call (it must pay every release and send the forward), sent with
    nativeFee = quoteForward + --native-margin, and its receipt must carry
    the vault's Withdrawn (or the batch's ReleaseSkipped after a race) and
    the factory's Forwarded, and no CertificateFailed, ReleaseFailed,
    ForwardFailed or ForwardSkipped. The deposit guard is deliberately not
    applied: releases continue after B3 closes deposits (C-1).

published
    Lists a factory's decoded Published events (F_d or F_x parameters) and
    whether each account matches its derivation.

Fail closed: both endpoints are plain http on 127.0.0.1 or localhost, read
without any HTTP proxy; the Ethereum chain id must be 31337 (and equal
getbridgev2info.origin_chain_id) and the B3 chain regtest. No key is handled
here: transactions go out with eth_sendTransaction from an account the
local anvil holds unlocked (--from), which must be a codeless EOA (PR-2).

Output: one JSON object on stdout. Any refusal prints "error: ..." on stderr
and exits 1. Standard library only. The library part (salts and CREATE2
addresses, a minimal ABI codec, event decoding and the receipt-leaf size
model) is importable; contrib/b3bridge/regtest/mock_lz_relayer.py uses it.
"""

import argparse
import json
from pathlib import Path
import sys
import time

_HERE = str(Path(__file__).resolve().parent)
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

from b3_bridge_deposit_guard_v2 import deposit_guard  # noqa: E402
from b3_bridge_observe_v2 import (  # noqa: E402
    REGTEST_CHAIN,
    JsonRpc,
    ObserverError,
    RpcError,
    hex_bytes,
    quantity,
)
from capture_eth_receipts_fixture import encode_receipt, keccak256, rlp  # noqa: E402

REGTEST_ETH_CHAIN_ID = 31337

# -- normative constants (contracts spec section 7.2; B3ForwarderTypesV2) -------
DEPOSIT_TAG = keccak256(b"B3/FWD/DEPOSIT/V2")
EXIT_TAG = keccak256(b"B3/FWD/EXIT/OFT/V2")
MAX_BRIDGE_FEE_BPS = 50
BPS_DENOMINATOR = 10_000
# B3DepositForwarderFactory.MIN_FLUSH_RAW (also read from the factory).
MIN_FLUSH_RAW = 1_000_000
# OI-2: the lifetime fee cap pays deploy, two flushes and returnToOwner.
DEPOSIT_FEE_CAP_TXS = 4
# Node changes section 12.3: no flush within 7 days of the light-client horizon.
HORIZON_REFUSAL_SECONDS = 7 * 24 * 60 * 60
SALT_PREIMAGE_LENGTH = 192
CREATE2_PREIMAGE_LENGTH = 85
UINT64_MAX = (1 << 64) - 1
UINT96_MAX = (1 << 96) - 1

# -- receipt-leaf budget (contracts spec section 7.1; ReceiptSizeV2.sol) --------
# PR-3's bound on a flush receipt's worst-case leaf. PR-1 and PR-2 fix the
# flush's log shape, so the predicted leaf is a constant 634 bytes; the
# relayer checks each actual receipt against the bound after sending.
PR3_LEAF_LIMIT = 7_168
RECEIPT_TX_TYPE = 2
RECEIPT_BLOOM_BYTES = 256
# A cumulativeGasUsed with 8 significant bytes, above any block gas limit.
WORST_CUMULATIVE_GAS = UINT64_MAX
# rlp(0xffff): the longest key a receipt of a real block has.
WORST_RECEIPT_KEY = rlp(0xFFFF)
# PR-1's fixed flush log shape, as (topics, data bytes): the token's
# Transfer(from, to, value) and the vault's Deposit(id, token, amount, recipient).
FLUSH_LOG_SHAPE = ((3, 32), (3, 64))

# -- gas -----------------------------------------------------------------------
# Per-item caps of B3RelayBatch (contracts/README.md gas table: a release
# about 0.1M, a first forward about 0.4M).
DEFAULT_RELEASE_GAS = 500_000
DEFAULT_FORWARD_GAS = 1_500_000
DEFAULT_RECEIPT_TIMEOUT = 60

# -- ABI types and signatures ----------------------------------------------------
DEPOSIT_PARAMS_TYPE = "(address,bytes32,address,uint96,uint64)"
EXIT_PARAMS_TYPE = "(uint32,bytes32,uint16,uint96,address)"
WITHDRAWAL_V2_TYPE = "(uint64,address,uint256,uint64)"
RELEASE_ITEM_TYPE = f"({WITHDRAWAL_V2_TYPE},bytes32[32])"
EXIT_FORWARD_TYPE = f"({EXIT_PARAMS_TYPE},uint256,address,uint256)"
FINALIZED_BLOCK_V2_TYPE = "(bytes32,uint64,bytes32,bytes32,bytes32,uint64,uint64,bytes32,uint8)"
SET_HEADER_TYPE = "(uint64,uint16,uint32,uint64,uint64,bytes,bytes32)"
CERT_INPUT_TYPE = f"({FINALIZED_BLOCK_V2_TYPE},{SET_HEADER_TYPE},{SET_HEADER_TYPE},bytes32[32],bytes)"

SIG_D_ACCOUNT_OF = f"accountOf({DEPOSIT_PARAMS_TYPE})"
SIG_D_IS_VALID = f"isValid({DEPOSIT_PARAMS_TYPE})"
SIG_D_DEPLOY = f"deploy({DEPOSIT_PARAMS_TYPE})"
SIG_D_FLUSH = f"flush({DEPOSIT_PARAMS_TYPE},uint256)"
SIG_D_FREE_BALANCE = "freeBalance(address)"
SIG_D_FEES_CHARGED = "feesCharged(address)"
SIG_D_IS_ACCOUNT = "isAccount(address)"
SIG_D_VAULT = "VAULT()"
SIG_D_MAX_DEPOSIT_RAW = "MAX_DEPOSIT_RAW()"
SIG_D_MIN_FLUSH_RAW = "MIN_FLUSH_RAW()"
SIG_X_ACCOUNT_OF = f"accountOf({EXIT_PARAMS_TYPE})"
SIG_X_IS_VALID = f"isValid({EXIT_PARAMS_TYPE})"
SIG_X_QUOTE_FORWARD = f"quoteForward({EXIT_PARAMS_TYPE},uint256)"
SIG_X_FORWARD = f"forward({EXIT_PARAMS_TYPE},uint256,address,address)"
SIG_X_DST_EID = "DST_EID()"
SIG_TOKEN = "TOKEN()"
SIG_INIT_CODE_HASH = "FORWARDER_INIT_CODE_HASH()"
SIG_VAULT_DEPOSIT_GATE_OPEN = "depositGateOpen()"
SIG_VAULT_RELEASED = "released(uint64)"
SIG_VAULT_RELEASE = f"release({WITHDRAWAL_V2_TYPE},bytes32[32])"
SIG_BALANCE_OF = "balanceOf(address)"
SIG_RELAY_WITH_FORWARDS = (f"relayWithForwards(address,{CERT_INPUT_TYPE}[],uint256,address,"
                           f"{RELEASE_ITEM_TYPE}[],uint256,address,{EXIT_FORWARD_TYPE}[],uint256,address)")

EV_PUBLISHED_DEPOSIT = f"Published(address,{DEPOSIT_PARAMS_TYPE})"
EV_PUBLISHED_EXIT = f"Published(address,{EXIT_PARAMS_TYPE})"
EV_TRANSFER = "Transfer(address,address,uint256)"
EV_DEPOSIT = "Deposit(uint64,address,uint256,bytes32)"
EV_WITHDRAWN = "Withdrawn(uint64,address,address,uint256,uint64)"
EV_FORWARDED = "Forwarded(address,bytes32,bytes32,uint256,uint256,uint256,address,uint256)"
EV_CERTIFICATE_FAILED = "CertificateFailed(uint256,bytes)"
EV_RELEASE_SKIPPED = "ReleaseSkipped(uint256,uint64)"
EV_RELEASE_FAILED = "ReleaseFailed(uint256,uint64,bytes)"
EV_FORWARD_SKIPPED = "ForwardSkipped(uint256,address)"
EV_FORWARD_FAILED = "ForwardFailed(uint256,address,bytes)"

# Custom errors of the contracts this relayer calls, for readable refusals.
KNOWN_ERRORS = (
    # B3DepositForwarderFactory and B3OftExitFactory
    "AlreadyDeployed()", "BadB3Recipient()", "BadCaller()", "BadConfig()", "BadFeeRecipient()",
    "BadRefundEOA()", "BadRefundTarget()", "BadWithdrawTarget()", "BelowMinimum()", "DeployFailed()",
    "Expired()", "FeeCapExceeded()", "InsufficientBalance()", "InvalidParams()",
    "NativeFeeShort(uint256,uint256)", "NativeRefundFailed()", "NoCode(address)", "NotDeployed()",
    "NotExpired()", "NotRefundEOA()", "NothingOwed()", "NothingToForward()", "NothingToReturn()",
    "NotFactory()", "Reentrancy()", "TokenCallFailed()", "WrongVault()", "ZeroAmount()",
    "ZeroNativeRefundTo()",
    # B3VaultV2
    "AlreadyReleased()", "DepositCapExceeded()", "DepositClosed()", "InsufficientReserve()",
    "RootBelowLeafHeight()", "TransferFailed()", "UnknownRoot()", "WithdrawalBeforeActivation()",
    "WithdrawalIdOutOfRange()", "ZeroRecipient()",
    # B3RelayBatch
    "RefundFailed()",
    # Regtest LayerZero and OFT mocks (contracts/regtest)
    "AlreadyDelivered(uint32,bytes32,uint64)", "AmountSDOverflowed(uint256)", "BadEid()", "BadMessage()",
    "Blocked(address)", "ComposeUnsupported()", "EndpointPaused()", "InsufficientFee(uint256,uint256)",
    "LockShort(uint256,uint256)", "LzTokenUnavailable()", "NoPeer(uint32)", "NotEnoughNative(uint256)",
    "OnlyEndpoint(address)", "OnlyPeer(uint32,bytes32)", "SlippageExceeded(uint256,uint256)",
    "ZeroRefundAddress()",
)


class ForwarderError(ObserverError):
    pass


# =============================================================================
# Minimal ABI codec: elementary types, bytes/string, tuples, T[] and T[k].
# =============================================================================
_FIXED_BASES = ("address", "bool")


def parse_type(text):
    """An ABI type string as ('base', name) / ('tuple', (...)) / ('array', elem, size|None)."""
    parsed, end = _parse_type(text, 0)
    if end != len(text):
        raise ForwarderError(f"malformed ABI type {text!r}")
    return parsed


def _parse_type(text, i):
    if i >= len(text):
        raise ForwarderError(f"malformed ABI type {text!r}")
    if text[i] == "(":
        components = []
        i += 1
        if i < len(text) and text[i] == ")":
            i += 1
        else:
            while True:
                component, i = _parse_type(text, i)
                components.append(component)
                if i < len(text) and text[i] == ",":
                    i += 1
                elif i < len(text) and text[i] == ")":
                    i += 1
                    break
                else:
                    raise ForwarderError(f"malformed ABI type {text!r}")
        parsed = ("tuple", tuple(components))
    else:
        j = i
        while j < len(text) and text[j].isalnum():
            j += 1
        name = text[i:j]
        _check_base(name, text)
        parsed = ("base", name)
        i = j
    while i < len(text) and text[i] == "[":
        j = text.find("]", i)
        if j < 0:
            raise ForwarderError(f"malformed ABI type {text!r}")
        size = text[i + 1:j]
        if size and (not size.isdigit() or int(size) == 0):
            raise ForwarderError(f"malformed ABI array size in {text!r}")
        parsed = ("array", parsed, int(size) if size else None)
        i = j + 1
    return parsed, i


def _check_base(name, text):
    if name in _FIXED_BASES or name in ("bytes", "string"):
        return
    for prefix, low, high, step in (("uint", 8, 256, 8), ("int", 8, 256, 8), ("bytes", 1, 32, 1)):
        rest = name[len(prefix):]
        if name.startswith(prefix) and rest.isdigit() and low <= int(rest) <= high and int(rest) % step == 0:
            return
    raise ForwarderError(f"unsupported ABI type {name!r} in {text!r}")


def _is_dynamic(t):
    if t[0] == "base":
        return t[1] in ("bytes", "string")
    if t[0] == "array":
        return t[2] is None or _is_dynamic(t[1])
    return any(_is_dynamic(c) for c in t[1])


def _static_size(t):
    if t[0] == "base":
        return 32
    if t[0] == "array":
        return t[2] * _static_size(t[1])
    return sum(_static_size(c) for c in t[1])


def _word(value):
    return value.to_bytes(32, "big")


def _as_int(value, name):
    if isinstance(value, bool) or not isinstance(value, int):
        raise ForwarderError(f"{name} is not an integer")
    return value


def _as_bytes(value, size, name):
    if isinstance(value, (bytes, bytearray)):
        data = bytes(value)
        if size is not None and len(data) != size:
            raise ForwarderError(f"{name} must be {size} bytes, got {len(data)}")
        return data
    return hex_bytes(value, size, name)


def _encode_base(name, value):
    if name.startswith("uint"):
        value = _as_int(value, name)
        if not 0 <= value < 1 << int(name[4:]):
            raise ForwarderError(f"{value} does not fit {name}")
        return _word(value)
    if name.startswith("int"):
        value = _as_int(value, name)
        bits = int(name[3:])
        if not -(1 << (bits - 1)) <= value < 1 << (bits - 1):
            raise ForwarderError(f"{value} does not fit {name}")
        return _word(value % (1 << 256))
    if name == "address":
        return bytes(12) + _as_bytes(value, 20, "address")
    if name == "bool":
        if not isinstance(value, bool):
            raise ForwarderError("bool value expected")
        return _word(int(value))
    if name in ("bytes", "string"):
        data = value.encode() if name == "string" and isinstance(value, str) else _as_bytes(value, None, name)
        padded = data + bytes(-len(data) % 32)
        return _word(len(data)) + padded
    size = int(name[5:])
    return _as_bytes(value, size, name) + bytes(32 - size)


def _encode(t, value):
    if t[0] == "base":
        return _encode_base(t[1], value)
    if t[0] == "array":
        items = list(value)
        if t[2] is not None and len(items) != t[2]:
            raise ForwarderError(f"fixed array needs {t[2]} items, got {len(items)}")
        body = _encode_sequence([t[1]] * len(items), items)
        return body if t[2] is not None else _word(len(items)) + body
    items = list(value)
    if len(items) != len(t[1]):
        raise ForwarderError(f"tuple needs {len(t[1])} values, got {len(items)}")
    return _encode_sequence(t[1], items)


def _encode_sequence(types, values):
    head_size = sum(32 if _is_dynamic(t) else _static_size(t) for t in types)
    heads, tails = [], []
    tail_size = 0
    for t, value in zip(types, values):
        encoded = _encode(t, value)
        if _is_dynamic(t):
            heads.append(_word(head_size + tail_size))
            tails.append(encoded)
            tail_size += len(encoded)
        else:
            heads.append(encoded)
    return b"".join(heads) + b"".join(tails)


def encode_abi(types, values):
    """abi.encode(values...) for type strings `types`."""
    parsed = [parse_type(t) if isinstance(t, str) else t for t in types]
    values = list(values)
    if len(parsed) != len(values):
        raise ForwarderError(f"{len(parsed)} ABI types for {len(values)} values")
    return _encode_sequence(parsed, values)


def _read_word(data, pos):
    if pos < 0 or pos + 32 > len(data):
        raise ForwarderError("ABI data is too short")
    return data[pos:pos + 32]


def _decode_base(name, data, pos):
    word = _read_word(data, pos)
    value = int.from_bytes(word, "big")
    if name.startswith("uint"):
        if value >> int(name[4:]):
            raise ForwarderError(f"ABI value does not fit {name}")
        return value
    if name.startswith("int"):
        bits = int(name[3:])
        value = value - (1 << 256) if value >> 255 else value
        if not -(1 << (bits - 1)) <= value < 1 << (bits - 1):
            raise ForwarderError(f"ABI value does not fit {name}")
        return value
    if name == "address":
        if value >> 160:
            raise ForwarderError("ABI address word has non-zero upper bytes")
        return "0x" + word[12:].hex()
    if name == "bool":
        if value > 1:
            raise ForwarderError("ABI bool word is neither 0 nor 1")
        return value == 1
    if name in ("bytes", "string"):
        length = value
        if pos + 32 + length > len(data):
            raise ForwarderError("ABI bytes run past the data")
        raw = data[pos + 32:pos + 32 + length]
        return raw.decode() if name == "string" else raw
    size = int(name[5:])
    if any(word[size:]):
        raise ForwarderError(f"ABI {name} has non-zero padding")
    return word[:size]


def _decode(t, data, pos):
    if t[0] == "base":
        return _decode_base(t[1], data, pos)
    if t[0] == "array":
        if t[2] is None:
            count = int.from_bytes(_read_word(data, pos), "big")
            if count > len(data):
                raise ForwarderError("ABI array length runs past the data")
            return _decode_sequence([t[1]] * count, data, pos + 32)
        return _decode_sequence([t[1]] * t[2], data, pos)
    return tuple(_decode_sequence(t[1], data, pos))


def _decode_sequence(types, data, base):
    out, head = [], base
    for t in types:
        if _is_dynamic(t):
            offset = int.from_bytes(_read_word(data, head), "big")
            if offset > len(data):
                raise ForwarderError("ABI offset runs past the data")
            out.append(_decode(t, data, base + offset))
            head += 32
        else:
            out.append(_decode(t, data, head))
            head += _static_size(t)
    return out


def decode_abi(types, data):
    """abi.decode(data, (types...)) as a list; addresses as 0x-hex, bytesN as bytes."""
    parsed = [parse_type(t) if isinstance(t, str) else t for t in types]
    return _decode_sequence(parsed, bytes(data), 0)


def split_signature(signature):
    """("name", [argument types]) of a canonical function or event signature."""
    open_at = signature.find("(")
    if open_at <= 0:
        raise ForwarderError(f"malformed signature {signature!r}")
    parsed = parse_type(signature[open_at:])
    if parsed[0] != "tuple":
        raise ForwarderError(f"malformed signature {signature!r}")
    return signature[:open_at], list(parsed[1])


def selector(signature):
    return keccak256(signature.encode())[:4]


def event_topic(signature):
    return keccak256(signature.encode())


def encode_call(signature, *args):
    """Calldata of `signature` (canonical types) with `args`."""
    _, types = split_signature(signature)
    return selector(signature) + encode_abi(types, args)


ERROR_SIGNATURES = {selector(sig): sig for sig in KNOWN_ERRORS}
_ERROR_STRING = selector("Error(string)")
_PANIC = selector("Panic(uint256)")


def describe_revert(data):
    """A readable revert reason: a known custom error with its arguments,
    Error(string), Panic(uint256), or the raw data."""
    data = bytes(data or b"")
    if not data:
        return "reverted without data"
    head, body = data[:4], data[4:]
    try:
        if head == _ERROR_STRING:
            return repr(decode_abi(["string"], body)[0])
        if head == _PANIC:
            return f"Panic(0x{decode_abi(['uint256'], body)[0]:x})"
        if head in ERROR_SIGNATURES:
            sig = ERROR_SIGNATURES[head]
            name, types = split_signature(sig)
            values = decode_abi(types, body)
            shown = ", ".join(("0x" + v.hex()) if isinstance(v, bytes) else str(v) for v in values)
            return f"{name}({shown})"
    except (ForwarderError, UnicodeDecodeError):
        pass
    return "0x" + data.hex()


# =============================================================================
# Forwarder parameters and derivations (contracts spec section 7.2).
# =============================================================================
def as_address(value, name):
    """20 address bytes from bytes or a (0x-prefixed) hex string."""
    return _as_bytes(value, 20, name)


_address = as_address


def _bytes32(value, name):
    return _as_bytes(value, 32, name)


def _uint(value, bits, name):
    number = quantity(value, name)
    if not 0 <= number < 1 << bits:
        raise ForwarderError(f"{name} does not fit uint{bits}: {number}")
    return number


def to_0x(data):
    return "0x" + bytes(data).hex()


def address_word(address):
    return bytes(12) + _address(address, "address")


class DepositParams:
    """B3ForwarderTypesV2.DepositParams p of one F_d."""

    FIELDS = ("vault", "b3_recipient", "refund_eoa", "fee_cap", "not_after")

    def __init__(self, vault, b3_recipient, refund_eoa, fee_cap, not_after):
        self.vault = _address(vault, "p.vault")
        self.b3_recipient = _bytes32(b3_recipient, "p.b3_recipient")
        self.refund_eoa = _address(refund_eoa, "p.refund_eoa")
        self.fee_cap = _uint(fee_cap, 96, "p.fee_cap")
        self.not_after = _uint(not_after, 64, "p.not_after")

    @classmethod
    def from_json(cls, obj):
        """From the params object (or the whole bridgeforwarderaddress result)."""
        if isinstance(obj, dict) and isinstance(obj.get("params"), dict):
            obj = obj["params"]
        if not isinstance(obj, dict):
            raise ForwarderError("deposit params must be a JSON object")
        missing = [key for key in cls.FIELDS if key not in obj]
        if missing:
            raise ForwarderError(f"deposit params lack {', '.join(missing)}")
        return cls(*(obj[key] for key in cls.FIELDS))

    def abi(self):
        return (self.vault, self.b3_recipient, self.refund_eoa, self.fee_cap, self.not_after)

    def to_json(self):
        return {"vault": to_0x(self.vault), "b3_recipient": to_0x(self.b3_recipient),
                "refund_eoa": to_0x(self.refund_eoa), "fee_cap": self.fee_cap, "not_after": self.not_after}

    def __eq__(self, other):
        return isinstance(other, DepositParams) and self.abi() == other.abi()


class ExitParams:
    """B3ForwarderTypesV2.ExitParams p of one F_x."""

    FIELDS = ("dst_eid", "to", "max_bridge_fee_bps", "fee_cap", "refund_eoa")

    def __init__(self, dst_eid, to, max_bridge_fee_bps, fee_cap, refund_eoa):
        self.dst_eid = _uint(dst_eid, 32, "p.dst_eid")
        self.to = _bytes32(to, "p.to")
        self.max_bridge_fee_bps = _uint(max_bridge_fee_bps, 16, "p.max_bridge_fee_bps")
        self.fee_cap = _uint(fee_cap, 96, "p.fee_cap")
        self.refund_eoa = _address(refund_eoa, "p.refund_eoa")

    @classmethod
    def from_json(cls, obj):
        if not isinstance(obj, dict):
            raise ForwarderError("exit params must be a JSON object")
        missing = [key for key in cls.FIELDS if key not in obj]
        if missing:
            raise ForwarderError(f"exit params lack {', '.join(missing)}")
        return cls(*(obj[key] for key in cls.FIELDS))

    def abi(self):
        return (self.dst_eid, self.to, self.max_bridge_fee_bps, self.fee_cap, self.refund_eoa)

    def to_json(self):
        return {"dst_eid": self.dst_eid, "to": to_0x(self.to), "max_bridge_fee_bps": self.max_bridge_fee_bps,
                "fee_cap": self.fee_cap, "refund_eoa": to_0x(self.refund_eoa)}

    def __eq__(self, other):
        return isinstance(other, ExitParams) and self.abi() == other.abi()


def deposit_salt_preimage(p):
    """abi.encode(DEPOSIT_TAG, vault, b3Recipient, refundEOA, feeCap, notAfter): 192 bytes."""
    return (DEPOSIT_TAG + address_word(p.vault) + p.b3_recipient + address_word(p.refund_eoa) +
            _word(p.fee_cap) + _word(p.not_after))


def exit_salt_preimage(p):
    """abi.encode(EXIT_TAG, dstEid, to, maxBridgeFeeBps, feeCap, refundEOA): 192 bytes."""
    return (EXIT_TAG + _word(p.dst_eid) + p.to + _word(p.max_bridge_fee_bps) + _word(p.fee_cap) +
            address_word(p.refund_eoa))


def deposit_salt(p):
    return keccak256(deposit_salt_preimage(p))


def exit_salt(p):
    return keccak256(exit_salt_preimage(p))


def create2_preimage(factory, salt, init_code_hash):
    return b"\xff" + _address(factory, "factory") + _bytes32(salt, "salt") + _bytes32(init_code_hash, "init_code_hash")


def create2_address(factory, salt, init_code_hash):
    """EIP-1014: keccak256(0xff ‖ factory ‖ salt ‖ initCodeHash)[12:]."""
    return keccak256(create2_preimage(factory, salt, init_code_hash))[12:]


def deposit_forwarder_address(factory, init_code_hash, p):
    return create2_address(factory, deposit_salt(p), init_code_hash)


def exit_forwarder_address(factory, init_code_hash, p):
    return create2_address(factory, exit_salt(p), init_code_hash)


def recipient_v1_valid(word):
    """RECIPIENT_V1: 11 zero bytes, version byte 1, a non-zero 20-byte key hash."""
    value = int.from_bytes(_bytes32(word, "recipient"), "big")
    return value >> 160 == 1 and value & ((1 << 160) - 1) != 0


def address_word_valid(word):
    """An EVM address word: upper 12 bytes zero, lower 20 bytes non-zero."""
    value = int.from_bytes(_bytes32(word, "address word"), "big")
    return value >> 160 == 0 and value != 0


def deposit_params_valid(p, vault, token, factory):
    """The deposit factory's isValid(p): p.vault is the factory's vault,
    RECIPIENT_V1, refundEOA non-zero and none of vault, token, factory."""
    vault, token, factory = _address(vault, "vault"), _address(token, "token"), _address(factory, "factory")
    return (p.vault == vault and recipient_v1_valid(p.b3_recipient) and
            p.refund_eoa not in (bytes(20), vault, token, factory))


def exit_params_valid(p, pinned_dst_eid):
    """The exit factory's isValid(p) (EOA-ness of refundEOA is the wallet's check)."""
    return (pinned_dst_eid != 0 and p.dst_eid == pinned_dst_eid and address_word_valid(p.to) and
            p.max_bridge_fee_bps <= MAX_BRIDGE_FEE_BPS and p.refund_eoa != bytes(20))


def deposit_fee_cap(fee_per_tx):
    """OI-2: fee_per_tx x 4, covering deploy, two flushes (one for a repeat
    arrival) and returnToOwner; wallets cap a route (b) delivery at
    MAX_DEPOSIT_RAW per forwarder, so one flush moves it whatever fee within
    the cap is taken. None on a zero fee or above uint64 (as
    src/modern/bridge_forwarder.h)."""
    if fee_per_tx <= 0:
        return None
    cap = fee_per_tx * DEPOSIT_FEE_CAP_TXS
    return cap if cap <= UINT64_MAX else None


def min_amount_after_fee(amount, bps):
    """minAmountLD of an OFT send of `amount` with at most `bps` bridge fee."""
    return amount - amount * bps // BPS_DENOMINATOR


# =============================================================================
# Events and the receipt-leaf size model (PR-1, PR-3).
# =============================================================================
def log_parts(log):
    """(address, [topics], data) bytes of an eth_getLogs or receipt log."""
    try:
        return (hex_bytes(log["address"], 20, "log address"),
                [hex_bytes(topic, 32, "log topic") for topic in log["topics"]],
                hex_bytes(log["data"], None, "log data"))
    except (KeyError, TypeError):
        raise ForwarderError("malformed log object") from None


def decode_published(log):
    """(kind, account, params) of a deposit or exit factory Published log."""
    _, topics, data = log_parts(log)
    if len(topics) != 2 or topics[1][:12] != bytes(12):
        raise ForwarderError("malformed Published log")
    account = topics[1][12:]
    if len(data) != 160:
        raise ForwarderError("malformed Published log data")
    if topics[0] == event_topic(EV_PUBLISHED_DEPOSIT):
        return "deposit", account, DepositParams(*decode_abi([DEPOSIT_PARAMS_TYPE], data)[0])
    if topics[0] == event_topic(EV_PUBLISHED_EXIT):
        return "exit", account, ExitParams(*decode_abi([EXIT_PARAMS_TYPE], data)[0])
    raise ForwarderError("not a forwarder factory Published log")


def _nibbles(key):
    out = []
    for byte in key:
        out += [byte >> 4, byte & 0x0F]
    return out


def _hex_prefix(nibbles, leaf):
    flags = 2 if leaf else 0
    if len(nibbles) % 2:
        out, rest = [((flags | 1) << 4) | nibbles[0]], nibbles[1:]
    else:
        out, rest = [flags << 4], nibbles
    for i in range(0, len(rest), 2):
        out.append((rest[i] << 4) | rest[i + 1])
    return bytes(out)


def receipt_leaf_size(logs, *, status=1, cumulative_gas=WORST_CUMULATIVE_GAS,
                      bloom=bytes(RECEIPT_BLOOM_BYTES), key=WORST_RECEIPT_KEY, tx_type=RECEIPT_TX_TYPE):
    """Bytes of rlp([hexPrefix(key), receipt]) for a receipt carrying `logs`
    ((address, [topics], data) triples). The defaults are the worst case B3's
    receipt proof can meet (ReceiptSizeV2.sol): status 1, an 8-byte
    cumulativeGasUsed, the 256-byte bloom and the 3-byte key rlp(0xffff)."""
    body = rlp([status, cumulative_gas, bloom, [[address, list(topics), data] for address, topics, data in logs]])
    typed = bytes([tx_type]) + body if tx_type else body
    return len(rlp([_hex_prefix(_nibbles(key), True), typed]))


def predicted_flush_leaf_size(log_shape=FLUSH_LOG_SHAPE):
    """PR-3's prediction: the worst-case leaf of a flush receipt with PR-1's
    fixed log shape (634 bytes; contracts test test_PR3_FlushReceiptLeafWithinBudget)."""
    logs = [(bytes(20), [bytes(32)] * topics, bytes(size)) for topics, size in log_shape]
    return receipt_leaf_size(logs)


def receipt_logs(receipt):
    return [log_parts(log) for log in receipt.get("logs") or []]


def actual_receipt_leaf_size(receipt):
    """The leaf of `receipt` under its full key rlp(transactionIndex), with its
    own status, cumulativeGasUsed, bloom and type (an upper bound on the leaf
    a receipt proof ends in, which may carry a shorter path)."""
    try:
        typed = encode_receipt(receipt)
    except (KeyError, TypeError, ValueError):
        raise ForwarderError("malformed transaction receipt") from None
    key = rlp(quantity(receipt.get("transactionIndex"), "transactionIndex"))
    return len(rlp([_hex_prefix(_nibbles(key), True), typed]))


# =============================================================================
# Ethereum JSON-RPC helpers (anvil, unlocked dev accounts).
# =============================================================================
class CallReverted(ForwarderError):
    """An eth_call or transaction simulation reverted; .reason is readable."""

    def __init__(self, what, data):
        self.data = bytes(data or b"")
        self.reason = describe_revert(self.data)
        super().__init__(f"{what} reverted: {self.reason}")


def _is_revert(error):
    return error.code == 3 or "revert" in str(error.message).lower()


def eth_chain_id(eth):
    return quantity(eth.call("eth_chainId"), "eth_chainId")


def require_regtest_ethereum(eth, origin_chain_id=None):
    chain_id = eth_chain_id(eth)
    if chain_id != REGTEST_ETH_CHAIN_ID:
        raise ForwarderError(f"refusing Ethereum chain id {chain_id}: the bridge V2 forwarder relayer runs only "
                             f"against the regtest anvil (chain id {REGTEST_ETH_CHAIN_ID})")
    if origin_chain_id is not None and chain_id != origin_chain_id:
        raise ForwarderError(f"the Ethereum endpoint reports chain id {chain_id}; the bridge V2 vault entry is on "
                             f"chain {origin_chain_id}")
    return chain_id


def require_regtest_b3(node):
    info = node.call("getblockchaininfo")
    chain = info.get("chain") if isinstance(info, dict) else None
    if chain != REGTEST_CHAIN:
        raise ForwarderError(f"refusing B3 chain {chain!r}: the bridge V2 forwarder relayer is regtest only")


def eth_call(eth, to, data, *, sender=None, value=0, gas=None, what="eth_call"):
    """eth_call at the latest block; a revert raises CallReverted with its reason."""
    tx = {"to": to_0x(_address(to, "call target")), "data": to_0x(data)}
    if sender is not None:
        tx["from"] = to_0x(_address(sender, "call sender"))
    if value:
        tx["value"] = hex(value)
    if gas is not None:
        tx["gas"] = hex(gas)
    try:
        result = eth.call("eth_call", [tx, "latest"])
    except RpcError as e:
        if _is_revert(e):
            data_hex = getattr(e, "data", None)
            raise CallReverted(what, hex_bytes(data_hex, None, "revert data")
                               if isinstance(data_hex, str) else b"") from None
        raise
    return hex_bytes(result, None, f"{what} result")


def view(eth, to, signature, *args, returns="uint256"):
    """One value of a view call; `returns` is its ABI type."""
    name, _ = split_signature(signature)
    data = eth_call(eth, to, encode_call(signature, *args), what=name)
    if not data:
        raise ForwarderError(f"{name} on {to_0x(_address(to, 'target'))} returned nothing (no contract there?)")
    return decode_abi([returns], data)[0]


def code_of(eth, address):
    return hex_bytes(eth.call("eth_getCode", [to_0x(_address(address, "address")), "latest"]), None, "eth_getCode")


def latest_block(eth):
    block = eth.call("eth_getBlockByNumber", ["latest", False])
    if not isinstance(block, dict):
        raise ForwarderError("eth_getBlockByNumber returned no latest block")
    return block


def wait_receipt(eth, tx_hash, timeout=DEFAULT_RECEIPT_TIMEOUT):
    deadline = time.monotonic() + timeout
    while True:
        receipt = eth.call("eth_getTransactionReceipt", [tx_hash])
        if isinstance(receipt, dict):
            return receipt
        if time.monotonic() >= deadline:
            raise ForwarderError(f"no receipt for {tx_hash} after {timeout} s")
        time.sleep(0.05)


def send_transaction(eth, sender, to, data, *, value=0, gas=None, timeout=DEFAULT_RECEIPT_TIMEOUT, what="transaction"):
    """eth_sendTransaction from an unlocked anvil account; the mined receipt,
    which must have status 1."""
    tx = {"from": to_0x(_address(sender, "sender")), "to": to_0x(_address(to, "target")),
          "data": to_0x(data), "value": hex(value)}
    if gas is not None:
        tx["gas"] = hex(gas)
    try:
        tx_hash = eth.call("eth_sendTransaction", [tx])
    except RpcError as e:
        raise ForwarderError(f"{what}: eth_sendTransaction refused ({e.message})") from None
    receipt = wait_receipt(eth, tx_hash, timeout)
    if quantity(receipt.get("status"), "receipt status") != 1:
        raise ForwarderError(f"{what}: transaction {tx_hash} reverted on chain")
    return receipt


def get_logs(eth, address, topics, from_block, to_block="latest"):
    """eth_getLogs of one address, in chain order."""
    def tag(block):
        return block if isinstance(block, str) else hex(block)

    def topic_filter(topic):
        if topic is None:
            return None
        if isinstance(topic, (list, tuple)):
            return [to_0x(t) for t in topic]
        return to_0x(topic)

    logs = eth.call("eth_getLogs", [{"address": to_0x(_address(address, "log address")),
                                     "topics": [topic_filter(t) for t in topics],
                                     "fromBlock": tag(from_block), "toBlock": tag(to_block)}])
    if not isinstance(logs, list):
        raise ForwarderError("eth_getLogs returned no list")
    return sorted(logs, key=lambda log: (quantity(log.get("blockNumber"), "blockNumber"),
                                         quantity(log.get("logIndex"), "logIndex")))


# =============================================================================
# B3 side.
# =============================================================================
def load_v2_info(node):
    try:
        info = node.call("getbridgev2info")
    except RpcError as e:
        raise ForwarderError(f"getbridgev2info refused: {e.message}") from None
    if not isinstance(info, dict) or info.get("configured") is not True:
        raise ForwarderError("the bridge V2 configuration is incomplete")
    return info


def deposit_forwarder_pins(info):
    """(factory, init_code_hash) of getbridgev2info.forwarders.deposit."""
    pins = (info.get("forwarders") or {}).get("deposit")
    if not isinstance(pins, dict):
        raise ForwarderError("no deposit forwarder factory is pinned (getbridgev2info.forwarders.deposit)")
    return _address(pins.get("factory"), "pinned deposit factory"), _bytes32(pins.get("init_code_hash"),
                                                                             "pinned init_code_hash")


def exit_forwarder_pins(info):
    """(factory, init_code_hash, dst_eid) of getbridgev2info.forwarders.exit."""
    pins = (info.get("forwarders") or {}).get("exit")
    if not isinstance(pins, dict):
        raise ForwarderError("no exit forwarder factory is pinned (getbridgev2info.forwarders.exit)")
    return (_address(pins.get("factory"), "pinned exit factory"),
            _bytes32(pins.get("init_code_hash"), "pinned init_code_hash"),
            _uint(pins.get("dst_eid"), 32, "pinned dst_eid"))


# =============================================================================
# flush (route b)
# =============================================================================
def published_deposit_params(eth, factory, account, from_block):
    """p of `account` from the deposit factory's Published events."""
    found = []
    for log in get_logs(eth, factory, [event_topic(EV_PUBLISHED_DEPOSIT), address_word(account)], from_block):
        kind, logged_account, p = decode_published(log)
        if kind == "deposit" and logged_account == _address(account, "account"):
            found.append(p)
    if not found:
        raise ForwarderError(f"no Published event names forwarder {to_0x(account)} since block {from_block}; "
                             "pass its params with --params")
    if any(p != found[0] for p in found):
        raise ForwarderError(f"Published events name different params for {to_0x(account)}")
    return found[0]


def check_flush_receipt(receipt, token, vault, account, b3_recipient, amount):
    """PR-1 / D1: exactly the token Transfer(account -> vault, amount) and the
    vault Deposit(id, token, amount, b3Recipient). Returns the deposit id."""
    logs = receipt_logs(receipt)
    tx = receipt.get("transactionHash")
    if len(logs) != 2:
        raise ForwarderError(f"PR-1 post-check: flush {tx} emitted {len(logs)} logs, not exactly the token "
                             "Transfer and the vault Deposit")
    (t_address, t_topics, t_data), (d_address, d_topics, d_data) = logs
    if (t_address != token or t_topics != [event_topic(EV_TRANSFER), address_word(account), address_word(vault)]
            or t_data != _word(amount)):
        raise ForwarderError(f"PR-1 post-check: flush {tx} does not start with the token Transfer of {amount} "
                             f"from {to_0x(account)} to the vault")
    if (d_address != vault or len(d_topics) != 3 or d_topics[0] != event_topic(EV_DEPOSIT) or
            d_topics[2] != address_word(token) or d_data != _word(amount) + b3_recipient):
        raise ForwarderError(f"PR-1 post-check: flush {tx} does not end with the vault Deposit of {amount} for "
                             f"{to_0x(b3_recipient)}")
    return int.from_bytes(d_topics[1], "big")


def check_mint_readiness(info, amount, eth_now):
    """Node changes section 11.3 before a flush: B3 can mint `amount` promptly.
    Once the V2 light client is bootstrapped it must be fresh at B3 and its
    horizon at least 7 days past `eth_now` (section 12.3); the amount must fit
    max_per_block and, while a mint epoch is reported, its headroom. Before
    the bootstrap every mint waits for it, so only the caps apply."""
    light = info.get("light_client")
    if not isinstance(light, dict) or not isinstance(light.get("bootstrapped"), bool):
        raise ForwarderError("getbridgev2info has no light_client status")
    if light["bootstrapped"]:
        if light.get("fresh") is not True:
            raise ForwarderError(f"the bridge V2 light client is stale at B3 (stale_after "
                                 f"{light.get('stale_after')}): B3 mints nothing until a light-client update "
                                 "connects; nothing is flushed")
        horizon = quantity(light.get("horizon_time"), "light_client.horizon_time")
        if horizon - eth_now < HORIZON_REFUSAL_SECONDS:
            raise ForwarderError(f"the bridge V2 light-client horizon ({horizon}) is less than 7 days after the "
                                 f"latest Ethereum block ({eth_now}): a deposit flushed now could wait for the "
                                 "extension hard fork; nothing is flushed")
    max_per_block = quantity(info.get("max_per_block"), "max_per_block")
    if amount > max_per_block:
        raise ForwarderError(f"the flush amount {amount} exceeds B3's max_per_block {max_per_block}; "
                             "nothing is flushed")
    if info.get("epoch_mint_headroom") is not None:
        headroom = quantity(info["epoch_mint_headroom"], "epoch_mint_headroom")
        if amount > headroom:
            raise ForwarderError(f"the flush amount {amount} exceeds the mint epoch's headroom {headroom}: B3 "
                                 "would mint it only in a later epoch; nothing is flushed")


def flush_forwarder(node, eth, sender, factory, fee, *, params=None, account=None, from_block=None,
                    timeout=DEFAULT_RECEIPT_TIMEOUT):
    """One deploy-if-needed and flush of an F_d; returns the stdout object."""
    sender = _address(sender, "--from")
    factory = _address(factory, "--factory")
    fee = _uint(fee, 256, "--fee")
    if (params is None) == (account is None):
        raise ForwarderError("give exactly one of the forwarder params or its account")
    if account is not None:
        account = _address(account, "--account")

    # 1. B3 first: once deposits are closed for good nothing is sent.
    guard = deposit_guard(node)
    info = load_v2_info(node)
    # 2. A retired launch entry: its deposits would only be swept.
    if info.get("retired") is True:
        raise ForwarderError("the launch vault entry is retired (B-23 migration): this relayer does not flush "
                             "its forwarders; their owners refund them (refund by refundEOA, or "
                             "returnToOwner after notAfter)")
    origin_chain_id = quantity(info.get("origin_chain_id"), "origin_chain_id")
    require_regtest_ethereum(eth, origin_chain_id)
    # 3. Pins and identity.
    pinned_factory, init_code_hash = deposit_forwarder_pins(info)
    if factory != pinned_factory:
        raise ForwarderError(f"factory {to_0x(factory)} is not the pinned deposit forwarder factory "
                             f"{to_0x(pinned_factory)}")
    vault = _address(info.get("vault"), "getbridgev2info vault")
    token = _address(info.get("token"), "getbridgev2info token")
    if params is None:
        start = from_block if from_block is not None else quantity(info.get("origin_deployment_block", 0),
                                                                   "origin_deployment_block")
        params = published_deposit_params(eth, factory, account, start)
    p = params
    if p.vault != vault:
        raise ForwarderError(f"p.vault {to_0x(p.vault)} is not the bridge V2 vault {to_0x(vault)}")
    local = deposit_forwarder_address(factory, init_code_hash, p)
    if account is not None and account != local:
        raise ForwarderError(f"the params derive forwarder {to_0x(local)}, not {to_0x(account)}")
    if not deposit_params_valid(p, vault, token, factory):
        raise ForwarderError("the params are invalid for the deposit factory (RECIPIENT_V1 recipient, and a "
                             "refundEOA that is none of zero, the vault, the token or the factory)")
    onchain = view(eth, factory, SIG_D_ACCOUNT_OF, p.abi(), returns="address")
    if hex_bytes(onchain, 20) != local:
        raise ForwarderError(f"factory.accountOf(p) is {onchain}, not {to_0x(local)} as derived under the pinned "
                             "init-code hash")
    for signature, expected, label in ((SIG_D_VAULT, vault, "VAULT"), (SIG_TOKEN, token, "TOKEN")):
        if hex_bytes(view(eth, factory, signature, returns="address"), 20) != expected:
            raise ForwarderError(f"the deposit factory's {label}() is not the pinned {label.lower()}")
    min_flush = view(eth, factory, SIG_D_MIN_FLUSH_RAW)
    max_deposit = view(eth, factory, SIG_D_MAX_DEPOSIT_RAW)
    if code_of(eth, sender):
        raise ForwarderError(f"PR-2: --from {to_0x(sender)} has code; flush runs only as the top-level call of a "
                             "codeless EOA")
    # 4. The vault's own gate (verifier window, token status).
    if view(eth, vault, SIG_VAULT_DEPOSIT_GATE_OPEN, returns="bool") is not True:
        raise ForwarderError("Vault V2 depositGateOpen() is false (verifier deposit window or token status); "
                             "nothing is flushed")

    result = {"account": to_0x(local), "factory": to_0x(factory), "vault": to_0x(vault),
              "params": p.to_json(), "fee": fee, "deposits": guard["deposits"]}
    # 5. Expiry, the fee cap and the free balance.
    now = quantity(latest_block(eth).get("timestamp"), "block timestamp")
    if now >= p.not_after:
        raise ForwarderError(f"forwarder {to_0x(local)} expired at {p.not_after} (latest block time {now}): it "
                             "can only be refunded or returned to its owner")
    fees_charged = view(eth, factory, SIG_D_FEES_CHARGED, local)
    if fees_charged > p.fee_cap or fee > p.fee_cap - fees_charged:
        raise ForwarderError(f"fee {fee} exceeds what is left of the lifetime fee cap "
                             f"({p.fee_cap} - {fees_charged} charged)")
    free = view(eth, factory, SIG_D_FREE_BALANCE, local)
    result["free"] = free
    if free < min_flush + fee:
        result["status"] = "nothing-to-flush"
        return result
    amount = min(free - fee, max_deposit)
    # 6. B3 can mint it promptly.
    check_mint_readiness(info, amount, now)

    # 7. Deploy if codeless, then flush; each simulated first.
    deploy_tx = None
    if view(eth, factory, SIG_D_IS_ACCOUNT, local, returns="bool") is not True:
        deploy_data = encode_call(SIG_D_DEPLOY, p.abi())
        eth_call(eth, factory, deploy_data, sender=sender, what="deploy")
        deploy_tx = send_transaction(eth, sender, factory, deploy_data, timeout=timeout,
                                     what="deploy")["transactionHash"]
    flush_data = encode_call(SIG_D_FLUSH, p.abi(), fee)
    simulated = decode_abi(["uint256"], eth_call(eth, factory, flush_data, sender=sender, what="flush"))[0]
    if simulated != amount:
        raise ForwarderError(f"flush simulation deposits {simulated}, expected {amount}; nothing flushed")
    receipt = send_transaction(eth, sender, factory, flush_data, timeout=timeout, what="flush")
    deposit_id = check_flush_receipt(receipt, token, vault, local, p.b3_recipient, amount)
    leaf_size = actual_receipt_leaf_size(receipt)
    worst = receipt_leaf_size(receipt_logs(receipt))
    if max(leaf_size, worst) > PR3_LEAF_LIMIT:
        raise ForwarderError(f"PR-3 post-check: flush {receipt.get('transactionHash')} has a {worst}-byte "
                             f"worst-case receipt leaf, above {PR3_LEAF_LIMIT}")
    result.update({
        "status": "flushed",
        "deploy_tx": deploy_tx,
        "flush_tx": receipt.get("transactionHash"),
        "block": quantity(receipt.get("blockNumber"), "blockNumber"),
        "deposit_id": deposit_id,
        "amount": amount,
        "leaf_size": leaf_size,
        "leaf_size_worst_case": worst,
        "remaining_free": view(eth, factory, SIG_D_FREE_BALANCE, local),
    })
    return result


# =============================================================================
# forward (route d3)
# =============================================================================
def batch_gas_limit(calldata, releases, release_gas, forwards, forward_gas):
    """A transaction gas limit giving every batch item its full cap. Estimation
    cannot be used: the batch catches item failures, so an estimate finds the
    gas at which items fail inside a successful transaction."""
    def item(cap):
        return -(-cap * 64 // 63) + 40_000

    return 250_000 + 16 * len(calldata) + releases * item(release_gas) + forwards * item(forward_gas)


def _batch_failures(logs, batch):
    topics = {event_topic(EV_CERTIFICATE_FAILED): ("CertificateFailed", ["bytes"]),
              event_topic(EV_RELEASE_FAILED): ("ReleaseFailed", ["bytes"]),
              event_topic(EV_FORWARD_FAILED): ("ForwardFailed", ["bytes"]),
              event_topic(EV_FORWARD_SKIPPED): ("ForwardSkipped", [])}
    failures = []
    for address, log_topics, data in logs:
        if address == batch and log_topics and log_topics[0] in topics:
            name, types = topics[log_topics[0]]
            reason = describe_revert(decode_abi(types, data)[0]) if types else "nativeFee above the budget"
            failures.append(f"{name}: {reason}")
    return failures


def check_forward_receipt(receipt, batch, vault, factory, account, withdrawal_id, released_item, p, fee):
    """Withdrawn (or ReleaseSkipped after a race) and exactly one Forwarded of
    `account`, no failed or skipped item. Returns the Forwarded fields."""
    logs = receipt_logs(receipt)
    tx = receipt.get("transactionHash")
    failures = _batch_failures(logs, batch)
    if failures:
        raise ForwarderError(f"relayWithForwards {tx} reports {'; '.join(failures)}")
    withdrawn = [(topics, data) for address, topics, data in logs
                 if address == vault and len(topics) == 4 and topics[0] == event_topic(EV_WITHDRAWN)
                 and topics[1] == _word(withdrawal_id)]
    skipped = any(address == batch and topics and topics[0] == event_topic(EV_RELEASE_SKIPPED)
                  for address, topics, _ in logs)
    if released_item and not withdrawn and not skipped:
        raise ForwarderError(f"relayWithForwards {tx} carries no Withdrawn for withdrawal {withdrawal_id}")
    if withdrawn and withdrawn[0][0][3] != address_word(account):
        raise ForwarderError(f"relayWithForwards {tx} released withdrawal {withdrawal_id} to another recipient")
    forwarded = [(topics, data) for address, topics, data in logs
                 if address == factory and len(topics) == 4 and topics[0] == event_topic(EV_FORWARDED)
                 and topics[1] == address_word(account)]
    if len(forwarded) != 1:
        raise ForwarderError(f"relayWithForwards {tx} carries {len(forwarded)} Forwarded events of "
                             f"{to_0x(account)}, not one")
    topics, data = forwarded[0]
    amount_sent, amount_received, logged_fee, fee_recipient, native_fee = decode_abi(
        ["uint256", "uint256", "uint256", "address", "uint256"], data)
    if topics[2] != p.to or logged_fee != fee:
        raise ForwarderError(f"relayWithForwards {tx} forwarded to another destination or with another fee")
    return {"released_by_this_tx": bool(withdrawn), "amount_sent": amount_sent,
            "amount_received": amount_received, "fee_recipient": fee_recipient,
            "native_fee": native_fee, "guid": to_0x(topics[3])}


def _item_reason(eth, sender, target, data, value=0):
    """Why one batch item would fail on its own (simulated), or None."""
    try:
        eth_call(eth, target, data, sender=sender, value=value, what="item")
    except CallReverted as e:
        return e.reason
    return None


def release_and_forward(node, eth, sender, batch, txid, burn_vout, certificate_block, fee, *,
                        fee_recipient=None, native_refund_to=None, release_gas=DEFAULT_RELEASE_GAS,
                        forward_gas=DEFAULT_FORWARD_GAS, native_margin=0, tx_gas=None,
                        timeout=DEFAULT_RECEIPT_TIMEOUT):
    """Release (unless already released) and forward one route (d3) withdrawal
    in one B3RelayBatch.relayWithForwards call; returns the stdout object.
    The deposit guard is not applied: releases continue after closure (C-1)."""
    sender = _address(sender, "--from")
    batch = _address(batch, "--batch")
    fee = _uint(fee, 256, "--fee")
    fee_recipient = _address(fee_recipient, "--fee-recipient") if fee_recipient is not None else sender
    native_refund_to = (_address(native_refund_to, "--native-refund-to")
                        if native_refund_to is not None else sender)
    if native_refund_to == bytes(20):
        raise ForwarderError("--native-refund-to must be non-zero")
    for value, name in ((release_gas, "--release-gas"), (forward_gas, "--forward-gas"),
                        (native_margin, "--native-margin")):
        if isinstance(value, bool) or not isinstance(value, int) or value < 0:
            raise ForwarderError(f"{name} must be a non-negative integer")

    require_regtest_b3(node)
    info = load_v2_info(node)
    chain_id = require_regtest_ethereum(eth, quantity(info.get("origin_chain_id"), "origin_chain_id"))
    pinned_factory, init_code_hash, pinned_dst_eid = exit_forwarder_pins(info)
    try:
        proof = node.call("getbridgewithdrawalproofv2", [txid, burn_vout, certificate_block])
    except RpcError as e:
        raise ForwarderError(f"getbridgewithdrawalproofv2 refused: {e.message}") from None
    if not isinstance(proof, dict):
        raise ForwarderError("getbridgewithdrawalproofv2 returned no object")
    exit_block = proof.get("exit")
    if not isinstance(exit_block, dict):
        raise ForwarderError(f"burn {txid}:{burn_vout} carries no route (d3) exit block: it pays its recipient "
                             "directly, so there is nothing to forward")
    p = ExitParams.from_json(exit_block)
    factory = _address(exit_block.get("factory"), "exit.factory")
    if factory != pinned_factory:
        raise ForwarderError(f"exit.factory {to_0x(factory)} is not the pinned exit forwarder factory")
    if not exit_params_valid(p, pinned_dst_eid):
        raise ForwarderError("the burn's exit parameters are invalid for the pinned exit factory")
    account = exit_forwarder_address(factory, init_code_hash, p)
    recipient = _address(proof.get("recipient"), "recipient")
    if recipient != account or _address(exit_block.get("account"), "exit.account") != account:
        raise ForwarderError(f"the burn's recipient {to_0x(recipient)} is not the exit forwarder {to_0x(account)} "
                             "derived from its exit block")
    if quantity(proof.get("destination_chain_id"), "destination_chain_id") != chain_id:
        raise ForwarderError("the withdrawal names another Ethereum chain")
    withdrawal_id = _uint(proof.get("withdrawal_id"), 64, "withdrawal_id")
    amount = _uint(proof.get("amount_raw"), 256, "amount_raw")
    b3_height = _uint(proof.get("b3_height"), 64, "b3_height")
    vault = _address(proof.get("vault"), "vault")
    token = _address(proof.get("token"), "token")
    path = proof.get("path")
    if not isinstance(path, list) or len(path) != 32:
        raise ForwarderError("the withdrawal proof path is not 32 siblings")
    path = [_bytes32(sibling, "path sibling") for sibling in path]
    if fee > p.fee_cap:
        raise ForwarderError(f"fee {fee} exceeds the exit forwarder's per-forward fee cap {p.fee_cap}")
    if fee and fee_recipient in (bytes(20), account, factory):
        raise ForwarderError("the fee recipient must be non-zero and neither the forwarder nor its factory")

    for address, label in ((batch, "B3RelayBatch"), (vault, "Vault V2"), (factory, "exit factory")):
        if not code_of(eth, address):
            raise ForwarderError(f"{label} {to_0x(address)} has no code on the Ethereum endpoint")
    if hex_bytes(view(eth, factory, SIG_X_ACCOUNT_OF, p.abi(), returns="address"), 20) != account:
        raise ForwarderError("exitFactory.accountOf(p) differs from the derivation under the pinned init-code hash")
    if view(eth, factory, SIG_X_DST_EID, returns="uint32") != pinned_dst_eid:
        raise ForwarderError("the exit factory's DST_EID() is not the pinned dst_eid")
    if hex_bytes(view(eth, factory, SIG_TOKEN, returns="address"), 20) != token:
        raise ForwarderError("the exit factory's TOKEN() is not the withdrawal's token")

    released = view(eth, vault, SIG_VAULT_RELEASED, withdrawal_id, returns="bool")
    balance = view(eth, token, SIG_BALANCE_OF, account)
    if released and balance == 0:
        raise ForwarderError(f"withdrawal {withdrawal_id} is already released and its exit forwarder "
                             f"{to_0x(account)} holds nothing: nothing to forward")
    expected = balance + (0 if released else amount)
    if expected <= fee:
        raise ForwarderError(f"the exit forwarder would hold {expected}, not more than the fee {fee}")
    quote = view(eth, factory, SIG_X_QUOTE_FORWARD, p.abi(), expected - fee)
    native_value = quote + native_margin

    withdrawal = (withdrawal_id, recipient, amount, b3_height)
    releases = [] if released else [(withdrawal, path)]
    forwards = [(p.abi(), fee, fee_recipient, native_value)]
    verifier = _address(info.get("verifier"), "getbridgev2info verifier")
    calldata = encode_call(SIG_RELAY_WITH_FORWARDS, verifier, [], 0, vault, releases, release_gas, factory,
                           forwards, forward_gas, native_refund_to)
    gas = tx_gas if tx_gas is not None else batch_gas_limit(calldata, len(releases), release_gas, 1, forward_gas)
    _, releases_paid, forwards_sent = decode_abi(
        ["uint256", "uint256", "uint256"],
        eth_call(eth, batch, calldata, sender=sender, value=native_value, gas=gas, what="relayWithForwards"))
    if releases_paid != len(releases) or forwards_sent != 1:
        if releases and releases_paid != len(releases):
            reason = _item_reason(eth, sender, vault, encode_call(SIG_VAULT_RELEASE, withdrawal, path))
            detail = f"the release would fail ({reason or 'out of gas within --release-gas'})"
        else:
            reason = (_item_reason(eth, sender, factory, encode_call(SIG_X_FORWARD, p.abi(), fee, fee_recipient,
                                                                     native_refund_to), native_value)
                      if released else None)
            detail = f"the forward would fail ({reason or 'see ForwardFailed; or out of gas within --forward-gas'})"
        raise ForwarderError(f"relayWithForwards simulation pays {releases_paid} of {len(releases)} releases and "
                             f"sends {forwards_sent} of 1 forwards: {detail}; nothing sent")
    receipt = send_transaction(eth, sender, batch, calldata, value=native_value, gas=gas, timeout=timeout,
                               what="relayWithForwards")
    outcome = check_forward_receipt(receipt, batch, vault, factory, account, withdrawal_id, bool(releases), p, fee)
    result = {
        "status": "forwarded",
        "withdrawal_id": withdrawal_id,
        "source_txid": proof.get("source_txid"),
        "account": to_0x(account),
        "factory": to_0x(factory),
        "batch": to_0x(batch),
        "vault": to_0x(vault),
        "exit": p.to_json(),
        "release_included": bool(releases),
        "tx": receipt.get("transactionHash"),
        "block": quantity(receipt.get("blockNumber"), "blockNumber"),
        "gas_limit": gas,
        "fee": fee,
        "native_value": native_value,
    }
    result.update(outcome)
    return result


# =============================================================================
# published
# =============================================================================
def list_published(eth, factory, from_block=0):
    """Every Published event of a deposit or exit factory, decoded."""
    require_regtest_ethereum(eth)
    factory = _address(factory, "--factory")
    if not code_of(eth, factory):
        raise ForwarderError(f"factory {to_0x(factory)} has no code on the Ethereum endpoint")
    init_code_hash = view(eth, factory, SIG_INIT_CODE_HASH, returns="bytes32")
    out = []
    for log in get_logs(eth, factory, [[event_topic(EV_PUBLISHED_DEPOSIT), event_topic(EV_PUBLISHED_EXIT)]],
                        from_block):
        kind, account, p = decode_published(log)
        derive = deposit_forwarder_address if kind == "deposit" else exit_forwarder_address
        out.append({"kind": kind, "account": to_0x(account), "params": p.to_json(),
                    "derivation_ok": derive(factory, init_code_hash, p) == account,
                    "block": quantity(log.get("blockNumber"), "blockNumber"),
                    "tx": log.get("transactionHash")})
    return {"factory": to_0x(factory), "init_code_hash": to_0x(init_code_hash), "from_block": from_block,
            "published": out}


# =============================================================================
# CLI
# =============================================================================
def _params_argument(text):
    """--params: a JSON object, or the path of a file holding one."""
    try:
        raw = text if text.lstrip().startswith("{") else Path(text).read_text()
        return DepositParams.from_json(json.loads(raw))
    except OSError as e:
        raise ForwarderError(f"cannot read --params file: {e}") from None
    except ValueError as e:
        raise ForwarderError(f"--params is not JSON: {e}") from None


def _block_argument(text):
    return int(text) if text.isdigit() else text


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="command", required=True)

    def ethereum(parser):
        parser.add_argument("--ethereum-rpc", required=True, help="local Ethereum JSON-RPC (the regtest anvil)")
        parser.add_argument("--timeout", type=float, default=DEFAULT_RECEIPT_TIMEOUT)

    def b3(parser):
        parser.add_argument("--b3-rpc-url", required=True, help="local B3 node RPC")
        parser.add_argument("--b3-rpc-user")
        parser.add_argument("--b3-rpc-password")
        parser.add_argument("--b3-cookie")
        parser.add_argument("--from", dest="sender", required=True,
                            help="unlocked anvil account (a codeless EOA) that sends and pays gas")

    flush = sub.add_parser("flush", help="deploy if needed and flush one deposit forwarder F_d")
    ethereum(flush)
    b3(flush)
    flush.add_argument("--factory", required=True, help="the pinned B3DepositForwarderFactory")
    source = flush.add_mutually_exclusive_group(required=True)
    source.add_argument("--params", help="DepositParams JSON (or a file): bridgeforwarderaddress's result "
                                         "or its params object")
    source.add_argument("--account", help="F_d address; its params are read from the factory's Published events")
    flush.add_argument("--fee", type=int, required=True, help="raw six-decimal fee the flush accrues to --from")
    flush.add_argument("--from-block", type=int,
                       help="first block scanned for Published (default: origin_deployment_block)")

    forward = sub.add_parser("forward", help="release and forward one route (d3) withdrawal through B3RelayBatch")
    ethereum(forward)
    b3(forward)
    forward.add_argument("--batch", required=True, help="B3RelayBatch address")
    forward.add_argument("--txid", required=True, help="the B3 burn transaction id")
    forward.add_argument("--vout", type=int, required=True, help="its bUSD V2 BURN output")
    forward.add_argument("--certificate-block", required=True, type=_block_argument,
                         help="B3 block (height or hash) carrying a relayed certificate covering the burn")
    forward.add_argument("--fee", type=int, required=True, help="raw six-decimal F_x fee, at most p.feeCap")
    forward.add_argument("--fee-recipient", help="who receives the fee (default: --from)")
    forward.add_argument("--native-refund-to", help="who receives unspent ETH (default: --from)")
    forward.add_argument("--release-gas", type=int, default=DEFAULT_RELEASE_GAS)
    forward.add_argument("--forward-gas", type=int, default=DEFAULT_FORWARD_GAS)
    forward.add_argument("--native-margin", type=int, default=0,
                         help="wei added to the OFT quote; the factory returns the change")
    forward.add_argument("--tx-gas", type=int, help="transaction gas limit (default: sized from the item caps)")

    published = sub.add_parser("published", help="list a forwarder factory's Published events")
    ethereum(published)
    published.add_argument("--factory", required=True)
    published.add_argument("--from-block", type=int, default=0)

    args = p.parse_args(argv)
    if args.command in ("flush", "forward"):
        if bool(args.b3_rpc_user) != bool(args.b3_rpc_password):
            p.error("--b3-rpc-user and --b3-rpc-password must be supplied together")
        if args.b3_cookie and args.b3_rpc_user:
            p.error("choose cookie or user/password B3 RPC authentication")
    return args


def run(args):
    eth = JsonRpc(args.ethereum_rpc, timeout=args.timeout)
    if args.command == "published":
        return list_published(eth, args.factory, args.from_block)
    node = JsonRpc(args.b3_rpc_url, args.b3_rpc_user, args.b3_rpc_password, args.b3_cookie, timeout=args.timeout)
    if args.command == "flush":
        params = _params_argument(args.params) if args.params is not None else None
        return flush_forwarder(node, eth, args.sender, args.factory, args.fee, params=params,
                               account=args.account, from_block=args.from_block, timeout=args.timeout)
    return release_and_forward(node, eth, args.sender, args.batch, args.txid, args.vout, args.certificate_block,
                               args.fee, fee_recipient=args.fee_recipient,
                               native_refund_to=args.native_refund_to, release_gas=args.release_gas,
                               forward_gas=args.forward_gas, native_margin=args.native_margin,
                               tx_gas=args.tx_gas, timeout=args.timeout)


def main(argv=None):
    args = parse_args(argv)
    try:
        result = run(args)
    except ObserverError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
