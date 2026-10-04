# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Strict student actions and synthetic public diagnostics; no node executor."""

import json
import math
import re

from ..language.batches import _json


TOOLS = ("getblockchaininfo", "getnetworkinfo", "getfinalitystatus", "listflowmeshmarkets")
PUBLIC_ERROR = "Public node data is unavailable or has an unsupported format."
MAX_ACTION_BYTES = 2048
MAX_FIXTURE_BYTES = 16384
_HASH = re.compile(r"[0-9a-fA-F]{64}\Z")


def compact(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), allow_nan=False)


def validate_action(raw, tools_enabled=True):
    """Accept one entire genuine JSON output; never repair, extract or choose it."""
    if type(tools_enabled) is not bool or type(raw) not in (str, bytes):
        raise ValueError("Action requires raw UTF-8 JSON and a boolean host permission.")
    try:
        payload = raw.encode("utf-8", errors="strict") if type(raw) is str else raw
        if not payload or len(payload) > MAX_ACTION_BYTES:
            raise ValueError("Student action exceeds its size bound or is empty.")
        # Decode explicitly: json.loads(bytes) also autodetects UTF-16/UTF-32.
        action = _json(payload.decode("utf-8", errors="strict"))
    except UnicodeError as error:
        raise ValueError("Student action is not valid UTF-8.") from error
    if type(action) is not dict:
        raise ValueError("Student action must be one JSON object.")
    if set(action) == {"reply"}:
        reply = action["reply"]
        if (type(reply) is not str or not reply.strip() or "\0" in reply
                or len(reply.encode("utf-8", errors="strict")) > 1024):
            raise ValueError("Invalid bounded student reply.")
    elif set(action) == {"tool", "arguments"}:
        if not tools_enabled:
            raise ValueError("The host has disabled tools.")
        if type(action["tool"]) is not str or action["tool"] not in TOOLS:
            raise ValueError("The generated tool is outside the diagnostic allowlist.")
        if type(action["arguments"]) is not dict or action["arguments"]:
            raise ValueError("Diagnostic arguments must be an empty JSON object.")
    else:
        raise ValueError("Student output must contain only a reply or a diagnostic request.")
    return action


def _fields(value, *, numbers=(), booleans=(), hashes=(), required=()):
    if type(value) is not dict or not set(required) <= set(value):
        raise ValueError("Synthetic projection has missing required fields.")
    if set(value) - (set(numbers) | set(booleans) | set(hashes)):
        raise ValueError("Synthetic projection contains unapproved fields.")
    for key in numbers:
        if key in value and (type(value[key]) not in (int, float)
                             or not math.isfinite(value[key]) or not 0 <= value[key] <= 2**53):
            raise ValueError("Synthetic projection requires bounded finite numbers.")
    for key in booleans:
        if key in value and type(value[key]) is not bool:
            raise ValueError("Synthetic projection requires boolean flags.")
    for key in hashes:
        if key in value and (type(value[key]) is not str or _HASH.fullmatch(value[key]) is None):
            raise ValueError("Synthetic projection requires complete hexadecimal hashes.")


def validate_fixture(tool, envelope):
    """Validate already projected, deliberately synthetic data, without an RPC."""
    if tool not in TOOLS or type(envelope) is not dict or envelope.get("simulated") is not True:
        raise ValueError("Only explicitly simulated permitted diagnostics are accepted.")
    if len(compact(envelope).encode("utf-8")) > MAX_FIXTURE_BYTES:
        raise ValueError("Synthetic diagnostic exceeds its size bound.")
    if envelope.get("ok") is False:
        if set(envelope) != {"simulated", "ok", "error"} or envelope["error"] != PUBLIC_ERROR:
            raise ValueError("Only the fixed public-unavailable error is accepted.")
        return envelope
    if envelope.get("ok") is not True or set(envelope) != {"simulated", "ok", "data"}:
        raise ValueError("Invalid simulated diagnostic envelope.")
    data = envelope["data"]
    if tool == "getblockchaininfo":
        if type(data) is not dict or data.get("chain") not in ("main", "test", "testnet4", "signet", "regtest"):
            raise ValueError("Invalid synthetic chain projection.")
        _fields({k: v for k, v in data.items() if k != "chain"},
                numbers=("blocks", "headers", "difficulty", "mediantime", "verificationprogress", "pruneheight"),
                booleans=("initialblockdownload", "pruned"), hashes=("bestblockhash",), required=("blocks",))
    elif tool == "getnetworkinfo":
        _fields(data, numbers=("version", "protocolversion", "connections", "connections_in", "connections_out"),
                booleans=("networkactive", "localrelay"), required=("networkactive", "connections"))
    elif tool == "getfinalitystatus":
        if type(data) is not dict:
            raise ValueError("Invalid synthetic finality projection.")
        nested = ("finalized", "pin", "validator_set", "set0_preview")
        _fields({k: v for k, v in data.items() if k not in nested},
                numbers=("epoch", "epoch_start"),
                booleans=("configured", "active", "bootstrapped", "handover_certified", "lineage_broken"),
                required=("configured", "active"))
        for key in ("finalized", "pin"):
            if key in data:
                _fields(data[key], numbers=("height", "epoch"), hashes=("hash",))
        for key in ("validator_set", "set0_preview"):
            if key in data:
                _fields(data[key], numbers=("size", "total_weight", "quorum_weight", "snapshot_height",
                                          "activation_height", "minimum_size") if key == "set0_preview"
                        else ("size", "total_weight", "quorum_weight"),
                        booleans=("ready",) if key == "set0_preview" else (), hashes=("set_hash", "next_set_hash"))
    else:
        if type(data) is not list or len(data) > 128:
            raise ValueError("Synthetic markets must be a bounded array.")
        for market in data:
            if type(market) is not dict:
                raise ValueError("Synthetic market must be an object.")
            special = ("quote_asset", "halt", "verification")
            _fields({k: v for k, v in market.items() if k not in special},
                    numbers=("epoch", "next_microblock_sequence", "next_effect_index", "round",
                             "pending_checkpoint_sequence", "pending_checkpoint_effect_count"),
                    booleans=("available", "running", "paused", "pending_handoff", "checkpoint_pending", "genesis_checkpoint_required"),
                    hashes=("domain", "market_id", "vault_id", "base_asset_id", "execution_config_id",
                            "last_microblock_hash", "state_root", "pending_checkpoint_id"),
                    required=("market_id", "available", "running"))
            if "quote_asset" in market and market["quote_asset"] != "B3":
                raise ValueError("Synthetic quote asset must be the projected B3 literal.")
            if "halt" in market and market["halt"] not in ("none", "invalid-config", "store-failure", "signing-conflict",
                                                         "certificate-conflict", "anchor-invalidated", "unavailable", "unknown"):
                raise ValueError("Invalid synthetic market halt.")
            if "verification" in market:
                _fields(market["verification"], booleans=("certificate_verified", "b3_checkpoint_confirmed", "freshest_network_head_proven"))
    return envelope


def request_prompt(question, tools_enabled):
    if type(tools_enabled) is not bool or type(question) is not str or not question.strip():
        raise ValueError("A bounded question and explicit boolean tool permission are required.")
    if len(question.encode("utf-8", errors="strict")) > 256 or any(ord(c) < 32 for c in question):
        raise ValueError("Question exceeds its framing bound or contains control characters.")
    return "\nTools: " + ("enabled" if tools_enabled else "disabled") + "\nUser: " + question + "\nAssistant: "


def result_prompt(question, action, envelope, tools_enabled=True):
    action = validate_action(compact(action), tools_enabled)
    if "tool" not in action:
        raise ValueError("Only a generated diagnostic request can receive a tool result.")
    validate_fixture(action["tool"], envelope)
    return (request_prompt(question, tools_enabled) + compact(action) + "\nTool: "
            + compact(envelope) + "\nAssistant: ")


def simulated_dispatch(raw_action, fixtures, tools_enabled=True):
    """Dispatch exactly the generated name into fixtures; no live or fallback tools."""
    action = validate_action(raw_action, tools_enabled)
    if "tool" not in action or type(fixtures) is not dict:
        raise ValueError("A diagnostic request and explicit fixture mapping are required.")
    tool = action["tool"]
    if tool not in fixtures:
        raise ValueError("No simulated result exists for the generated diagnostic.")
    return action, validate_fixture(tool, fixtures[tool])


def make_fixture(tool, seed, unavailable=False):
    """Deterministic exam fixtures; values are examples, never observations."""
    if tool not in TOOLS or type(seed) is not int or not 0 <= seed <= 1000 or type(unavailable) is not bool:
        raise ValueError("Invalid synthetic fixture request.")
    if unavailable:
        envelope = {"simulated": True, "ok": False, "error": PUBLIC_ERROR}
    else:
        if tool == "getblockchaininfo":
            data = {"chain": "regtest", "blocks": 700000 + seed, "headers": 700003 + seed,
                    "initialblockdownload": bool(seed % 2)}
        elif tool == "getnetworkinfo":
            data = {"networkactive": True, "connections": 70 + seed % 5}
        elif tool == "getfinalitystatus":
            data = {"configured": True, "active": True, "epoch": 300 + seed,
                    "finalized": {"height": 600000 + seed, "hash": "f" * 64}, "validator_set": {"size": 14 + seed % 4}}
        else:
            data = [] if seed % 2 else [{"market_id": f"{seed + i + 1:064x}", "available": True,
                                       "running": bool(i % 2), "quote_asset": "B3"} for i in range(2)]
        envelope = {"simulated": True, "ok": True, "data": data}
    return validate_fixture(tool, envelope)
