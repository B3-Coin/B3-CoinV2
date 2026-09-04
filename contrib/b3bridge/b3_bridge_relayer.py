#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Automatic, fail-closed Ethereum -> B3 deposit relayer.

Python discovers finalized deposits and assembles evidence.  Canonical type-10
serialization and all consensus verification remain in b3-bridge-ethcheck.
"""
import argparse
import base64
import contextlib
from decimal import Decimal, InvalidOperation
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

import capture_eth_receipts_fixture as receipts
from eth_live_test import encode_exec_header


DEPOSIT_TOPIC = "0xdaf0af297d25c0e96a0b209d35692b4e07c503634eeca57fc5c35c006acf527f"
PERIOD_SLOTS = 8192
MAX_ANCESTRY_DISTANCE = 20000
MAX_LC_EXECUTION_ADVANCE = 20000
B3_ATOMS = 1_000_000_000


class RelayerError(RuntimeError):
    pass


class RpcError(RelayerError):
    def __init__(self, error):
        self.code = error.get("code") if isinstance(error, dict) else None
        self.message = error.get("message", str(error)) if isinstance(error, dict) else str(error)
        super().__init__(f"RPC {self.code}: {self.message}")


def log(event, **fields):
    print(json.dumps({"event": event, **fields}, sort_keys=True), file=sys.stderr, flush=True)


def norm_hex(value, size=None, prefix=True):
    if not isinstance(value, str):
        raise RelayerError("expected a hexadecimal string")
    text = value[2:] if value.startswith("0x") else value
    if len(text) % 2 or not text or any(c not in "0123456789abcdefABCDEF" for c in text):
        raise RelayerError(f"invalid hexadecimal value: {value!r}")
    if size is not None and len(text) != size * 2:
        raise RelayerError(f"expected {size} bytes, got {len(text) // 2}")
    return ("0x" if prefix else "") + text.lower()


def quantity(value):
    if isinstance(value, int):
        return value
    if not isinstance(value, str):
        raise RelayerError("expected an integer quantity")
    return int(value, 16) if value.startswith("0x") else int(value)


def b3_amount_atoms(value, name):
    """Parse one B3-denominated RPC/CLI amount without floating-point loss."""
    try:
        amount = Decimal(str(value))
    except (InvalidOperation, ValueError) as e:
        raise RelayerError(f"{name} is not a decimal B3 amount") from e
    if not amount.is_finite() or amount < 0:
        raise RelayerError(f"{name} must be a non-negative finite B3 amount")
    atoms = amount * B3_ATOMS
    if atoms != atoms.to_integral_value() or atoms > (1 << 63) - 1:
        raise RelayerError(f"{name} has more than 9 decimals or is too large")
    return int(atoms)


def provider_origin(url):
    """Conservative provider identity: credentials, path and API key do not count."""
    try:
        parsed = urllib.parse.urlsplit(url)
        scheme = parsed.scheme.lower()
        host = (parsed.hostname or "").rstrip(".").lower()
        port = parsed.port
    except ValueError:
        raise RelayerError("invalid Ethereum RPC URL") from None
    if scheme not in ("http", "https") or not host:
        raise RelayerError("Ethereum RPC URLs must use http or https with a hostname")
    effective_port = port if port is not None else (443 if scheme == "https" else 80)
    return host, effective_port


def endpoint_label(url):
    """Credential/path-free endpoint name safe for operator logs and errors."""
    try:
        parsed = urllib.parse.urlsplit(url)
        scheme = parsed.scheme.lower()
        host = (parsed.hostname or "").rstrip(".").lower()
        port = parsed.port
    except (TypeError, ValueError):
        return "configured endpoint"
    if scheme not in ("http", "https") or not host:
        return "configured endpoint"
    if ":" in host and not host.startswith("["):
        host = f"[{host}]"
    default_port = 443 if scheme == "https" else 80
    suffix = f":{port}" if port is not None and port != default_port else ""
    return f"{scheme}://{host}{suffix}"


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    try:
        with os.fdopen(fd, "w") as f:
            json.dump(value, f, sort_keys=True, separators=(",", ":"))
            f.write("\n")
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    finally:
        with contextlib.suppress(FileNotFoundError):
            os.unlink(tmp)


def prune_deposit_workdirs(work_root, retain):
    """Bound generated proof artifacts; durable payloads live in SQLite."""
    root = Path(work_root)
    if not root.exists():
        return
    candidates = []
    for child in root.iterdir():
        if (child.name.startswith("deposit-") and child.is_dir() and
                not child.is_symlink()):
            candidates.append(child)
    candidates.sort(key=lambda path: (path.stat().st_mtime_ns, path.name),
                    reverse=True)
    for child in candidates[retain:]:
        shutil.rmtree(child)


def b58check(payload):
    alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
    raw = payload + hashlib.sha256(hashlib.sha256(payload).digest()).digest()[:4]
    zeros = len(raw) - len(raw.lstrip(b"\0"))
    n, out = int.from_bytes(raw, "big"), ""
    while n:
        n, r = divmod(n, 58)
        out = alphabet[r] + out
    return "1" * zeros + out


def recipient_address(raw, prefix):
    data = bytes.fromhex(norm_hex(raw, 32, False))
    if data[:11] != b"\0" * 11 or data[11] != 1:
        raise RelayerError("mint b3_recipient is not canonical RECIPIENT_V1")
    if data[12:] == b"\0" * 20:
        raise RelayerError("mint b3_recipient has an unspendable zero P2PKH hash")
    if not 0 <= prefix <= 255:
        raise RelayerError("B3 P2PKH prefix must fit one byte")
    return b58check(bytes([prefix]) + data[12:])


class JsonRpc:
    def __init__(self, url, user=None, password=None, cookie=None, timeout=60):
        self.url, self.user, self.password = url.rstrip("/"), user, password
        self.endpoint = endpoint_label(self.url)
        self.cookie, self.timeout, self.next_id = cookie, timeout, 0

    def wallet(self, name):
        return JsonRpc(self.url + "/wallet/" + urllib.parse.quote(name, safe=""),
                       self.user, self.password, self.cookie, self.timeout)

    def call(self, method, params=()):
        self.next_id += 1
        auth = None
        if self.cookie:
            try:
                auth = Path(self.cookie).read_text().strip()
            except OSError as e:
                raise RelayerError(f"cannot read B3 RPC cookie: {e}") from e
        elif self.user is not None:
            auth = f"{self.user}:{self.password}"
        headers = {"Content-Type": "application/json", "User-Agent": "b3-bridge-relayer/1.1"}
        if auth is not None:
            headers["Authorization"] = "Basic " + base64.b64encode(auth.encode()).decode()
        req = urllib.request.Request(self.url, json.dumps({
            "jsonrpc": "2.0", "id": self.next_id, "method": method,
            "params": list(params)}).encode(), headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as response:
                result = json.load(response, parse_float=Decimal)
        except urllib.error.HTTPError as e:
            try:
                result = json.loads(e.read(), parse_float=Decimal)
            except Exception:
                raise RelayerError(
                    f"HTTP {e.code} from {self.endpoint}") from None
        except (OSError, ValueError) as e:
            raise RelayerError(
                f"RPC transport failure for {method} via {self.endpoint} "
                f"({type(e).__name__})") from None
        if result.get("error") is not None:
            raise RpcError(result["error"])
        return result.get("result")


def fetch(url):
    req = urllib.request.Request(url, headers={"Accept": "application/json",
                                               "User-Agent": "b3-bridge-relayer/1.1"})
    try:
        with urllib.request.urlopen(req, timeout=60) as response:
            return json.load(response, parse_float=Decimal)
    except (OSError, ValueError) as e:
        raise RelayerError(
            f"beacon API failure for {endpoint_label(url)} "
            f"({type(e).__name__})") from None


class State:
    def __init__(self, path):
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        self.db = sqlite3.connect(path, isolation_level=None)
        os.chmod(path, 0o600)
        self.db.row_factory = sqlite3.Row
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.execute("PRAGMA synchronous=FULL")
        self.db.execute("PRAGMA foreign_keys=ON")
        self.db.executescript("""
          CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY,value TEXT NOT NULL);
          CREATE TABLE IF NOT EXISTS deposits(
            id INTEGER PRIMARY KEY,deposit_id TEXT NOT NULL,block_number INTEGER NOT NULL,
            block_hash TEXT NOT NULL,tx_hash TEXT NOT NULL,tx_index INTEGER NOT NULL,
            receipt_log_index INTEGER NOT NULL,amount TEXT NOT NULL,recipient TEXT NOT NULL,
            planned INTEGER NOT NULL DEFAULT 0,UNIQUE(deposit_id),UNIQUE(tx_hash,receipt_log_index));
          CREATE TABLE IF NOT EXISTS jobs(
            id INTEGER PRIMARY KEY,payload_id TEXT NOT NULL UNIQUE,kind TEXT NOT NULL,
            payload TEXT NOT NULL,amount TEXT,address TEXT,metadata TEXT NOT NULL,
            state TEXT NOT NULL DEFAULT 'planned',txid TEXT,raw_tx TEXT,
            effect_height INTEGER,effect_block TEXT,fee_atoms INTEGER,fee_day TEXT,
            created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);
        """)
        columns = {row[1] for row in self.db.execute("PRAGMA table_info(jobs)")}
        if "effect_height" not in columns:
            self.db.execute("ALTER TABLE jobs ADD COLUMN effect_height INTEGER")
        if "effect_block" not in columns:
            self.db.execute("ALTER TABLE jobs ADD COLUMN effect_block TEXT")
        if "fee_atoms" not in columns:
            self.db.execute("ALTER TABLE jobs ADD COLUMN fee_atoms INTEGER")
        if "fee_day" not in columns:
            self.db.execute("ALTER TABLE jobs ADD COLUMN fee_day TEXT")

    def close(self):
        if self.db is not None:
            self.db.close()
            self.db = None

    def __del__(self):
        with contextlib.suppress(Exception):
            self.close()

    @contextlib.contextmanager
    def tx(self):
        self.db.execute("BEGIN IMMEDIATE")
        try:
            yield
            self.db.execute("COMMIT")
        except Exception:
            self.db.execute("ROLLBACK")
            raise

    def bind(self, identity, start):
        encoded = json.dumps(identity, sort_keys=True, separators=(",", ":"))
        with self.tx():
            row = self.db.execute("SELECT value FROM meta WHERE key='identity'").fetchone()
            if row and row[0] != encoded:
                raise RelayerError("state database belongs to different bridge pins")
            if not row:
                self.db.execute("INSERT INTO meta VALUES('identity',?)", (encoded,))
            deployment = self.db.execute("SELECT value FROM meta WHERE key='deployment_start'").fetchone()
            cursor = self.db.execute("SELECT value FROM meta WHERE key='next_block'").fetchone()
            if not cursor:
                if start is None:
                    raise RelayerError("--start-block is required for a new state database")
                self.db.execute("INSERT INTO meta VALUES('deployment_start',?)", (str(start),))
                self.db.execute("INSERT INTO meta VALUES('next_block',?)", (str(start),))
            else:
                # Migrate databases created by the initial relayer preview.
                if not deployment:
                    if start is None:
                        raise RelayerError("legacy state needs --start-block once to bind deployment start")
                    self.db.execute("INSERT INTO meta VALUES('deployment_start',?)", (str(start),))
                    deployment = (str(start),)
                if start is not None and int(deployment[0]) != start:
                    raise RelayerError("--start-block disagrees with immutable deployment start")
            authenticated = self.db.execute(
                "SELECT value FROM meta WHERE key='authenticated_scan_version'").fetchone()
            if not authenticated:
                # Preview builds advanced this cursor from eth_getLogs. Replay
                # once from the immutable deployment height under the
                # receipts-root completeness algorithm; deposits/jobs dedupe.
                deployment = self.db.execute(
                    "SELECT value FROM meta WHERE key='deployment_start'").fetchone()
                self.db.execute("UPDATE meta SET value=? WHERE key='next_block'",
                                (deployment[0],))
                self.db.execute(
                    "INSERT INTO meta VALUES('authenticated_scan_version','1')")
            elif authenticated[0] != "1":
                raise RelayerError("unsupported authenticated scan state version")

    def cursor(self):
        return int(self.db.execute("SELECT value FROM meta WHERE key='next_block'").fetchone()[0])

    def unplanned(self):
        return self.db.execute("SELECT * FROM deposits WHERE planned=0 ORDER BY block_number,id").fetchall()

    def _insert_plan(self, records_to_add):
        for record in records_to_add:
            payload = norm_hex(record["payload_hex"], prefix=False)
            pid = hashlib.sha256(bytes.fromhex(payload)).hexdigest()
            metadata = json.dumps(record, sort_keys=True, separators=(",", ":"))
            self.db.execute("""INSERT OR IGNORE INTO jobs
              (payload_id,kind,payload,amount,address,metadata) VALUES(?,?,?,?,?,?)""",
              (pid, record["kind"], payload, record.get("amount"),
               record.get("address"), metadata))
            old = self.db.execute(
                "SELECT kind,payload,amount,address,metadata FROM jobs WHERE payload_id=?",
                (pid,)).fetchone()
            expected = (record["kind"], payload, record.get("amount"),
                        record.get("address"), metadata)
            if tuple(old) != expected:
                raise RelayerError(
                    "duplicate payload id has conflicting RPC metadata")

    def add_plan(self, records_to_add, deposit_id=None):
        with self.tx():
            self._insert_plan(records_to_add)
            if deposit_id is not None:
                self.db.execute("UPDATE deposits SET planned=1 WHERE id=?", (deposit_id,))

    def add_verified_deposit(self, deposit, records_to_add):
        """Atomically persist one proof-verified deposit and its B3 plan."""
        values = (str(deposit["deposit_id"]), deposit["block_number"],
                  norm_hex(deposit["block_hash"], 32),
                  norm_hex(deposit["tx_hash"], 32), deposit["tx_index"],
                  deposit["receipt_log_index"], str(deposit["amount"]),
                  norm_hex(deposit["recipient"], 32))
        with self.tx():
            by_id = self.db.execute(
                "SELECT * FROM deposits WHERE deposit_id=?", (values[0],)).fetchone()
            by_log = self.db.execute(
                "SELECT * FROM deposits WHERE tx_hash=? AND receipt_log_index=?",
                (values[3], values[5])).fetchone()
            if by_id and by_log and by_id["id"] != by_log["id"]:
                raise RelayerError("deposit identity collides with another finalized log")
            old = by_id or by_log
            if old:
                actual = tuple(old[name] for name in (
                    "deposit_id", "block_number", "block_hash", "tx_hash",
                    "tx_index", "receipt_log_index", "amount", "recipient"))
                if actual != values:
                    raise RelayerError(
                        f"conflicting finalized deposit id {deposit['deposit_id']}")
                deposit_row_id = old["id"]
            else:
                cursor = self.db.execute("""INSERT INTO deposits
                  (deposit_id,block_number,block_hash,tx_hash,tx_index,receipt_log_index,amount,recipient)
                  VALUES(?,?,?,?,?,?,?,?)""", values)
                deposit_row_id = cursor.lastrowid
            self._insert_plan(records_to_add)
            self.db.execute("UPDATE deposits SET planned=1 WHERE id=?",
                            (deposit_row_id,))

    def advance_cursor(self, expected, next_block):
        if next_block <= expected:
            raise RelayerError("Ethereum scan cursor did not advance")
        with self.tx():
            current = self.db.execute(
                "SELECT value FROM meta WHERE key='next_block'").fetchone()
            if current is None or int(current[0]) != expected:
                raise RelayerError("Ethereum scan cursor changed unexpectedly")
            self.db.execute("UPDATE meta SET value=? WHERE key='next_block'",
                            (str(next_block),))

    def pending(self):
        return self.db.execute(
            """SELECT * FROM jobs WHERE state NOT IN ('confirmed','superseded')
               ORDER BY CASE WHEN kind IN ('bootstrap','update') THEN 0 ELSE 1 END,id""").fetchall()

    def job_counts(self):
        row = self.db.execute("SELECT COUNT(*),SUM(state='confirmed') FROM jobs").fetchone()
        return row[0], row[1] or 0

    def retained_anchors(self):
        out = []
        for row in self.db.execute("""SELECT kind,metadata FROM jobs
                WHERE state!='superseded'
                  AND kind IN ('bootstrap','update','execution-backfill') ORDER BY id"""):
            metadata = json.loads(row[1])
            value = (metadata.get("anchor_hash") if row[0] in ("bootstrap", "update")
                     else metadata.get("target_block_hash"))
            if value is not None:
                out.append(norm_hex(value, 32))
        return out

    def first(self):
        return self.db.execute(
            """SELECT * FROM jobs WHERE state NOT IN ('confirmed','superseded')
               ORDER BY CASE WHEN kind IN ('bootstrap','update') THEN 0 ELSE 1 END,id LIMIT 1""").fetchone()

    def confirmed(self):
        return self.db.execute("SELECT * FROM jobs WHERE state='confirmed' ORDER BY id").fetchall()

    def has_later(self, row_id):
        return self.db.execute("SELECT 1 FROM jobs WHERE id>? LIMIT 1", (row_id,)).fetchone() is not None

    def delete_tail(self, row_id):
        with self.tx():
            if self.db.execute("SELECT 1 FROM jobs WHERE id>? LIMIT 1", (row_id,)).fetchone():
                raise RelayerError("cannot discard a superseded bridge job with dependent records")
            self.db.execute("DELETE FROM jobs WHERE id=?", (row_id,))

    def reopen_from(self, row_id):
        with self.tx():
            self.db.execute("""UPDATE jobs SET state='planned',txid=NULL,raw_tx=NULL,
                               effect_height=NULL,effect_block=NULL,
                               fee_atoms=NULL,fee_day=NULL
                               WHERE id>=?""", (row_id,))

    def prepared(self, row_id, txid, raw_tx, fee_atoms, fee_day,
                 max_fee_atoms, daily_fee_budget_atoms):
        if fee_atoms < 0 or fee_atoms > max_fee_atoms:
            raise RelayerError(
                f"prepared transaction fee {fee_atoms} atoms exceeds per-transaction budget")
        with self.tx():
            reserved = self.db.execute(
                "SELECT COALESCE(SUM(fee_atoms),0) FROM jobs WHERE fee_day=? AND id!=?",
                (fee_day, row_id)).fetchone()[0]
            if reserved < 0 or fee_atoms > daily_fee_budget_atoms - reserved:
                raise RelayerError(
                    f"daily relayer fee budget exhausted for {fee_day}")
            changed = self.db.execute(
                """UPDATE jobs SET state='prepared',txid=?,raw_tx=?,fee_atoms=?,fee_day=?
                   WHERE id=? AND state='planned'""",
                (txid, raw_tx, fee_atoms, fee_day, row_id)).rowcount
            if changed != 1:
                raise RelayerError("bridge job changed while reserving its network fee")

    def reset_transaction(self, row_id):
        with self.tx():
            self.db.execute("""UPDATE jobs SET state='planned',txid=NULL,raw_tx=NULL,
                               effect_height=NULL,effect_block=NULL,
                               fee_atoms=NULL,fee_day=NULL WHERE id=?""", (row_id,))

    def set_state(self, row_id, state):
        with self.tx():
            self.db.execute("UPDATE jobs SET state=? WHERE id=?", (state, row_id))

    def confirm_effect(self, row_id, height, block_hash):
        with self.tx():
            self.db.execute("""UPDATE jobs SET state='confirmed',effect_height=?,effect_block=?
                               WHERE id=?""", (height, block_hash, row_id))


class ProcessLock:
    def __init__(self, state_path):
        path = Path(str(state_path) + ".lock")
        path.parent.mkdir(parents=True, exist_ok=True)
        self.file = open(path, "a+")
        os.chmod(path, 0o600)
        try:
            fcntl.flock(self.file, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as e:
            self.file.close()
            raise RelayerError(f"another relayer holds {path}") from e

    def close(self):
        fcntl.flock(self.file, fcntl.LOCK_UN)
        self.file.close()


def bridge_identity(info, eth_chain, trusted_root):
    required = ("vault", "token", "registry_id", "asset_id", "ethereum_chain_id",
                "origin_deployment_block",
                "activation_height", "vault_runtime_code_hash",
                "implementation_or_adapter", "adapter_version",
                "recipient_encoding_version", "withdrawal_mode",
                "decentralized_verifier", "decentralized_verifier_code_hash",
                "bootstrap_validator_set_hash", "withdrawal_rules_version",
                "withdrawal_rules_commitment", "min_bridge_validators",
                "max_bridge_validators", "min_bridge_total_weight",
                "max_epoch_lag", "max_per_block", "max_per_epoch",
                "mint_epoch_length_blocks",
                "origin_decimals", "asset_decimals", "trusted_checkpoint_root",
                "trusted_checkpoint_slot", "genesis_validators_root",
                "ethereum_fork_schedule", "fork_schedule_valid_through_epoch",
                "electra_epoch", "min_sync_committee_participants", "max_sync_lag_slots")
    if not info.get("configured") or not info.get("ready") or any(k not in info for k in required):
        raise RelayerError("B3 bridge is unconfigured or incomplete")
    if quantity(info["ethereum_chain_id"]) != eth_chain:
        raise RelayerError("Ethereum RPC chain id does not match the B3 consensus pin")
    if norm_hex(info["trusted_checkpoint_root"], 32) != norm_hex(trusted_root, 32):
        raise RelayerError("--trusted-root does not match the B3 consensus pin")
    if info["withdrawal_mode"] != "decentralized-verifier-v1":
        raise RelayerError("automatic production relaying requires the decentralized verifier")
    if quantity(info["adapter_version"]) != 1:
        raise RelayerError("automatic production relaying supports only the direct-token adapter v1")
    if quantity(info["recipient_encoding_version"]) != 1:
        raise RelayerError("automatic production relaying supports only RECIPIENT_V1")
    if quantity(info["origin_decimals"]) != 6 or quantity(info["asset_decimals"]) != 6:
        raise RelayerError("production bUSD requires exact six-decimal origin and asset units")

    def nonzero_hex(name, size):
        value = norm_hex(info[name], size)
        if int(value, 16) == 0:
            raise RelayerError(f"B3 bridge pin {name} is zero")
        return value

    origin_deployment_block = quantity(info["origin_deployment_block"])
    activation_height = quantity(info["activation_height"])
    if origin_deployment_block <= 0 or activation_height < 0:
        raise RelayerError("B3 bridge deployment or activation height is invalid")
    min_validators = quantity(info["min_bridge_validators"])
    max_validators = quantity(info["max_bridge_validators"])
    min_weight = quantity(info["min_bridge_total_weight"])
    max_epoch_lag = quantity(info["max_epoch_lag"])
    max_per_block = quantity(info["max_per_block"])
    max_per_epoch = quantity(info["max_per_epoch"])
    mint_epoch_length = quantity(info["mint_epoch_length_blocks"])
    if (min_validators <= 0 or max_validators < min_validators or min_weight <= 0 or
            max_epoch_lag <= 0 or max_per_block <= 0 or max_per_epoch < max_per_block or
            mint_epoch_length <= 0):
        raise RelayerError("B3 bridge verifier threshold or mint-cap pin is invalid")

    identity = {
            "ethereum_chain_id": eth_chain,
            "vault": nonzero_hex("vault", 20),
            "vault_runtime_code_hash": nonzero_hex("vault_runtime_code_hash", 32),
            "token": nonzero_hex("token", 20),
            # Direct-token adapter v1 defines this commitment as the exact
            # origin-token runtime-code hash.
            "origin_token_runtime_code_hash": nonzero_hex("implementation_or_adapter", 32),
            "adapter_version": 1,
            "recipient_encoding_version": 1,
            "registry_id": nonzero_hex("registry_id", 32),
            "origin_deployment_block": origin_deployment_block,
            "activation_height": activation_height,
            "approval_last_height": (quantity(info["approval_last_height"])
                                     if "approval_last_height" in info else None),
            "asset_id": nonzero_hex("asset_id", 32),
            "trusted_root": norm_hex(trusted_root, 32),
            "trusted_checkpoint_slot": quantity(info["trusted_checkpoint_slot"]),
            "genesis_validators_root": nonzero_hex("genesis_validators_root", 32),
            "ethereum_fork_schedule": [{
                "activation_epoch": quantity(f["activation_epoch"]),
                "fork_version": norm_hex(f["fork_version"], 4)}
                for f in info["ethereum_fork_schedule"]],
            "fork_schedule_valid_through_epoch": quantity(info["fork_schedule_valid_through_epoch"]),
            "electra_epoch": quantity(info["electra_epoch"]),
            "min_sync_committee_participants": quantity(info["min_sync_committee_participants"]),
            "max_sync_lag_slots": quantity(info["max_sync_lag_slots"]),
            "origin_decimals": 6,
            "asset_decimals": 6,
            "withdrawal_mode": info["withdrawal_mode"],
            "decentralized_verifier": nonzero_hex("decentralized_verifier", 20),
            "decentralized_verifier_code_hash": nonzero_hex(
                "decentralized_verifier_code_hash", 32),
            "bootstrap_validator_set_hash": nonzero_hex(
                "bootstrap_validator_set_hash", 32),
            "withdrawal_rules_version": quantity(info["withdrawal_rules_version"]),
            "withdrawal_rules_commitment": nonzero_hex(
                "withdrawal_rules_commitment", 32),
            "min_bridge_validators": min_validators,
            "max_bridge_validators": max_validators,
            "min_bridge_total_weight": min_weight,
            "max_epoch_lag": max_epoch_lag,
            "max_per_block": max_per_block,
            "max_per_epoch": max_per_epoch,
            "mint_epoch_length_blocks": mint_epoch_length,
    }
    if identity["withdrawal_rules_version"] <= 0:
        raise RelayerError("B3 bridge withdrawal-rules version is invalid")
    if len(identity["ethereum_fork_schedule"]) == 0:
        raise RelayerError("B3 bridge Ethereum fork schedule is empty")
    if len({identity["vault"], identity["token"],
            identity["decentralized_verifier"]}) != 3:
        raise RelayerError("B3 bridge vault, token, and verifier addresses must be distinct")
    return identity


def runtime_code_hash(code, label):
    """Return Ethereum extcodehash for non-empty eth_getCode bytes."""
    if not isinstance(code, str):
        raise RelayerError(f"{label} eth_getCode result is not hexadecimal")
    encoded = code[2:] if code.startswith("0x") else code
    if (not encoded or len(encoded) % 2 or
            any(c not in "0123456789abcdefABCDEF" for c in encoded)):
        raise RelayerError(f"{label} has no valid runtime bytecode")
    return "0x" + receipts.keccak256(bytes.fromhex(encoded)).hex()


def verify_runtime_code_pins(providers, identity):
    """Verify every pinned Ethereum runtime at deployment and latest state."""
    if not providers:
        raise RelayerError("at least one Ethereum provider is required for code-pin checks")
    checks = (
        ("vault", identity["vault"], identity["vault_runtime_code_hash"]),
        ("verifier", identity["decentralized_verifier"],
         identity["decentralized_verifier_code_hash"]),
        ("origin token", identity["token"],
         identity["origin_token_runtime_code_hash"]),
    )
    blocks = (hex(identity["origin_deployment_block"]), "latest")
    for provider in providers:
        for block in blocks:
            for label, address, expected in checks:
                actual = runtime_code_hash(
                    provider.call("eth_getCode", [address, block]), label)
                if actual != expected:
                    point = "latest" if block == "latest" else "deployment block"
                    raise RelayerError(
                        f"{label} runtime code hash disagrees with the B3 pin at {point}")


def validate_store_snapshot(snapshot, info):
    """Bind an exported store to the exact LC state advertised by B3."""
    try:
        if not isinstance(snapshot, dict) or quantity(snapshot["version"]) != 1:
            raise RelayerError("unsupported B3 light-client store snapshot")
        connection = snapshot["connection"]
        finalized = snapshot["b3_finalized"]
        store = snapshot["store"]
        header = store["finalized_header"]
        beacon = header["beacon"]
        execution = header["execution"]
        if quantity(connection["height"]) > quantity(finalized["height"]):
            raise RelayerError("B3 light-client store connection is not finalized")
        checks = (
            quantity(connection["height"]) == quantity(
                info["light_client_connected_height"]),
            norm_hex(connection["block_hash"], 32) == norm_hex(
                info["light_client_connected_block"], 32),
            quantity(store["period"]) == quantity(info["light_client_period"]),
            quantity(beacon["slot"]) == quantity(info["finalized_beacon_slot"]),
            quantity(execution["block_number"]) == quantity(
                info["finalized_execution_block"]),
            norm_hex(execution["block_hash"], 32) == norm_hex(
                info["finalized_execution_hash"], 32),
        )
    except (KeyError, TypeError, ValueError) as e:
        raise RelayerError("malformed B3 light-client store snapshot") from e
    if not all(checks):
        raise RelayerError("exported light-client store disagrees with getbridgeinfo")
    return snapshot


def store_snapshot_fingerprint(snapshot):
    if snapshot is None:
        return None
    return hashlib.sha256(json.dumps(
        snapshot, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def validate_execution_anchor(snapshot, requested_target):
    """Normalize one B3-finalized retained execution-anchor selection."""
    try:
        if not isinstance(snapshot, dict) or not snapshot.get("found"):
            raise RelayerError(
                f"B3 retains no finalized execution anchor usable for Ethereum block {requested_target}")
        if quantity(snapshot["target_block"]) != requested_target:
            raise RelayerError("B3 execution-anchor response changed its target")
        anchor = {
            "block_number": quantity(snapshot["block_number"]),
            "block_hash": norm_hex(snapshot["block_hash"], 32),
            "receipts_root": norm_hex(snapshot["receipts_root"], 32),
            "source_finalized_beacon_slot": quantity(
                snapshot["source_finalized_beacon_slot"]),
            "source_finalized_execution_block": quantity(
                snapshot["source_finalized_execution_block"]),
            "execution_timestamp": quantity(snapshot["execution_timestamp"]),
            "connected_height": quantity(snapshot["connected_height"]),
            "connected_block": norm_hex(snapshot["connected_block"], 32),
            "b3_finalized_height": quantity(snapshot["b3_finalized_height"]),
            "b3_finalized_block": norm_hex(snapshot["b3_finalized_block"], 32),
        }
    except (KeyError, TypeError, ValueError) as e:
        raise RelayerError("malformed B3 execution-anchor response") from e
    source = anchor["source_finalized_execution_block"]
    if (anchor["block_number"] < requested_target or
            source < anchor["block_number"] or
            source - requested_target > MAX_LC_EXECUTION_ADVANCE or
            anchor["connected_height"] > anchor["b3_finalized_height"] or
            anchor["source_finalized_beacon_slot"] == 0 or
            anchor["execution_timestamp"] == 0 or
            any(int(anchor[name], 16) == 0 for name in (
                "block_hash", "receipts_root", "connected_block",
                "b3_finalized_block"))):
        raise RelayerError("B3 execution anchor is incomplete or outside the backfill window")
    return anchor


def execution_anchor_identity(anchor):
    """Fields that must remain exact even if the B3 finality tip advances."""
    names = ("block_number", "block_hash", "receipts_root",
             "source_finalized_beacon_slot",
             "source_finalized_execution_block", "execution_timestamp",
             "connected_height", "connected_block")
    return tuple(anchor[name] for name in names)


def execution_anchor_payload_identity(anchor):
    """Complete captured RPC identity that receipt JSON must reproduce."""
    return execution_anchor_identity(anchor) + (
        anchor["b3_finalized_height"], anchor["b3_finalized_block"])


def fetch_execution_anchor(node, target):
    return validate_execution_anchor(
        node.call("getbridgeanchorforblock", [target]), target)


def recheck_execution_anchor(node, anchor):
    rechecked = fetch_execution_anchor(node, anchor["block_number"])
    if execution_anchor_identity(rechecked) != execution_anchor_identity(anchor):
        raise RelayerError("B3 retained execution anchor changed while assembling proof; retry")
    return rechecked


def corroborate_execution_anchor(anchor, providers):
    """Require every configured execution provider to reproduce the B3 anchor."""
    for provider in providers:
        header = provider.call(
            "eth_getBlockByNumber", [hex(anchor["block_number"]), False])
        if (not isinstance(header, dict) or
                quantity(header.get("number")) != anchor["block_number"] or
                norm_hex(header.get("hash"), 32) != anchor["block_hash"] or
                norm_hex(header.get("receiptsRoot"), 32) != anchor["receipts_root"] or
                quantity(header.get("timestamp")) != anchor["execution_timestamp"]):
            raise RelayerError(
                "Ethereum provider disagrees with the B3-finalized execution anchor")
        encoded = encode_exec_header(header)
        if receipts.keccak256(encoded).hex() != anchor["block_hash"][2:]:
            raise RelayerError(
                "Ethereum provider returned a non-canonical retained-anchor header")


def validate_update_horizon(update, horizon):
    try:
        data = update["data"]
        signature_slot = quantity(data["signature_slot"])
        attested_slot = quantity(data["attested_header"]["beacon"]["slot"])
        finalized_slot = quantity(data["finalized_header"]["beacon"]["slot"])
    except (KeyError, TypeError, ValueError) as e:
        raise RelayerError("malformed beacon light-client update") from e
    if signature_slot <= 0 or max((signature_slot - 1) // 32,
                                  attested_slot // 32,
                                  finalized_slot // 32) > horizon:
        raise RelayerError("beacon update exceeds the B3-pinned fork schedule horizon")


def capture_common(beacon, trusted_root, info, store_snapshot=None):
    genesis = fetch(beacon + "/eth/v1/beacon/genesis")["data"]
    forks = fetch(beacon + "/eth/v1/config/fork_schedule")["data"]
    bootstrap = None
    if store_snapshot is None:
        bootstrap = fetch(beacon + "/eth/v1/beacon/light_client/bootstrap/" + norm_hex(trusted_root, 32))
    finality = fetch(beacon + "/eth/v1/beacon/light_client/finality_update")
    head = fetch(beacon + "/eth/v1/beacon/headers/head")
    final_slot = quantity(finality["data"]["finalized_header"]["beacon"]["slot"])
    horizon = quantity(info["fork_schedule_valid_through_epoch"])
    head_slot = quantity(head["data"]["header"]["message"]["slot"])
    if norm_hex(genesis["genesis_validators_root"], 32) != norm_hex(info["genesis_validators_root"], 32):
        raise RelayerError("beacon genesis validators root does not match B3 consensus")
    if store_snapshot is None:
        boot_slot = quantity(bootstrap["data"]["header"]["beacon"]["slot"])
        if boot_slot != quantity(info["trusted_checkpoint_slot"]):
            raise RelayerError("trusted checkpoint bootstrap slot does not match B3 consensus")
        b3_slot = quantity(info.get("finalized_beacon_slot", boot_slot))
        start_period = boot_slot // PERIOD_SLOTS
    else:
        validate_store_snapshot(store_snapshot, info)
        b3_slot = quantity(
            store_snapshot["store"]["finalized_header"]["beacon"]["slot"])
        start_period = quantity(store_snapshot["store"]["period"])
    if final_slot // 32 > horizon:
        raise RelayerError("Ethereum finality passed the B3-pinned fork schedule horizon")
    if head_slot < final_slot or head_slot - final_slot > quantity(info["max_sync_lag_slots"]):
        raise RelayerError("beacon finality exceeds the B3-pinned maximum lag")
    if b3_slot > final_slot:
        raise RelayerError("B3 finalized beacon slot is ahead of the beacon API")
    updates = []
    period, last = start_period, final_slot // PERIOD_SLOTS
    while period <= last:
        count = min(128, last - period + 1)
        batch = fetch(beacon + f"/eth/v1/beacon/light_client/updates?start_period={period}&count={count}")
        if not isinstance(batch, list):
            raise RelayerError("beacon updates response is not an array")
        for update in batch:
            validate_update_horizon(update, horizon)
        updates.extend(batch)
        period += count
    validate_update_horizon(finality, horizon)
    pinned_forks = [{"epoch": quantity(f["activation_epoch"]),
                     "version": norm_hex(f["fork_version"], 4)}
                    for f in info["ethereum_fork_schedule"]]
    beacon_forks = [{"epoch": quantity(f["epoch"]),
                     "version": norm_hex(f["current_version"], 4)} for f in forks]
    if ([f for f in beacon_forks if f["epoch"] <= horizon] !=
            [f for f in pinned_forks if f["epoch"] <= horizon]):
        raise RelayerError("beacon fork schedule disagrees with B3 consensus pins")
    config = {"genesis_validators_root": genesis["genesis_validators_root"],
              "forks": pinned_forks,
              "electra_epoch": quantity(info["electra_epoch"]),
              "min_participants": quantity(info["min_sync_committee_participants"]),
              "trusted_root": norm_hex(trusted_root, 32)}
    execution = finality["data"]["finalized_header"]["execution"]
    return (config, bootstrap, updates, finality,
            quantity(execution["block_number"]),
            norm_hex(execution["block_hash"], 32), store_snapshot)


def parse_deposit_log(item, eth, vault, token):
    topics = item.get("topics", [])
    if norm_hex(item.get("address"), 20) != vault or len(topics) != 3 or norm_hex(topics[0], 32) != DEPOSIT_TOPIC:
        raise RelayerError("Ethereum RPC returned a non-canonical Deposit log")
    if norm_hex(topics[2], 32)[-40:] != token[2:] or int(topics[2], 16) >> 160:
        raise RelayerError("Deposit log token does not match the B3 pin")
    data = bytes.fromhex(norm_hex(item.get("data"), 64, False))
    receipt = eth.call("eth_getTransactionReceipt", [item["transactionHash"]])
    if not receipt or quantity(receipt["status"]) != 1 or norm_hex(receipt["blockHash"], 32) != norm_hex(item["blockHash"], 32):
        raise RelayerError("Deposit receipt is missing, failed, or changed block")
    global_index = quantity(item["logIndex"])
    local = [i for i, entry in enumerate(receipt["logs"]) if quantity(entry["logIndex"]) == global_index]
    if len(local) != 1:
        raise RelayerError("cannot resolve receipt-local Deposit log index")
    exact = receipt["logs"][local[0]]
    if (norm_hex(exact["address"], 20) != norm_hex(item["address"], 20) or
            [norm_hex(x, 32) for x in exact["topics"]] != [norm_hex(x, 32) for x in topics] or
            norm_hex(exact["data"], 64) != norm_hex(item["data"], 64)):
        raise RelayerError("eth_getLogs entry disagrees with its transaction receipt")
    deposit_id, amount = int(topics[1], 16), int.from_bytes(data[:32], "big")
    if deposit_id >= 1 << 64 or amount == 0:
        raise RelayerError("Deposit id/amount is outside the canonical event range")
    return {"deposit_id": deposit_id, "block_number": quantity(item["blockNumber"]),
            "block_hash": norm_hex(item["blockHash"], 32), "tx_hash": norm_hex(item["transactionHash"], 32),
            "tx_index": quantity(receipt["transactionIndex"]), "receipt_log_index": local[0],
            "amount": amount, "recipient": "0x" + data[32:].hex()}


def canonical_log_set(logs):
    """Provider-independent identity for one finalized eth_getLogs page."""
    if not isinstance(logs, list):
        raise RelayerError("eth_getLogs result is not an array")
    normalized = []
    for item in logs:
        try:
            normalized.append((
                norm_hex(item["address"], 20),
                tuple(norm_hex(topic, 32) for topic in item["topics"]),
                norm_hex(item["data"]),
                quantity(item["blockNumber"]),
                norm_hex(item["blockHash"], 32),
                norm_hex(item["transactionHash"], 32),
                quantity(item["transactionIndex"]),
                quantity(item["logIndex"]),
            ))
        except (KeyError, TypeError, ValueError) as e:
            raise RelayerError("malformed eth_getLogs entry") from e
    return sorted(normalized)


def bloom_contains(logs_bloom, value):
    """Ethereum log-bloom membership. False is a cryptographic non-inclusion."""
    bloom = bytes.fromhex(norm_hex(logs_bloom, 256, False))
    digest = receipts.keccak256(value)
    for offset in (0, 2, 4):
        bit = ((digest[offset] << 8) | digest[offset + 1]) & 2047
        if not (bloom[255 - bit // 8] & (1 << (bit % 8))):
            return False
    return True


def block_might_contain_deposit(header, identity):
    bloom = header.get("logsBloom")
    values = (
        bytes.fromhex(norm_hex(identity["vault"], 20, False)),
        bytes.fromhex(norm_hex(DEPOSIT_TOPIC, 32, False)),
        bytes.fromhex("00" * 12 + norm_hex(identity["token"], 20, False)),
    )
    return all(bloom_contains(bloom, value) for value in values)


def verified_execution_headers(eth, start, finalized, finalized_hash,
                               max_ancestry):
    """Return a finalized-hash-anchored header map for one forward scan."""
    if start > finalized:
        return {}
    depth = finalized - start
    if depth > max_ancestry:
        raise RelayerError(
            f"Ethereum scan cursor is {depth} blocks behind finality; "
            "a retained historical B3 anchor is required")
    headers, expected_hash = {}, norm_hex(finalized_hash, 32)
    for number in range(finalized, start - 1, -1):
        header = eth.call("eth_getBlockByNumber", [hex(number), False])
        if (not isinstance(header, dict) or quantity(header.get("number")) != number or
                norm_hex(header.get("hash"), 32) != expected_hash):
            raise RelayerError(f"execution header mismatch at block {number}")
        encoded = encode_exec_header(header)
        if receipts.keccak256(encoded).hex() != expected_hash[2:]:
            raise RelayerError(f"execution RLP mismatch at block {number}")
        # Parse these now so a malformed negative-bloom header cannot advance
        # the durable cursor without ever taking the receipt path.
        norm_hex(header.get("receiptsRoot"), 32)
        norm_hex(header.get("logsBloom"), 256)
        if not isinstance(header.get("transactions"), list):
            raise RelayerError("execution header transaction list is unavailable")
        headers[number] = header
        expected_hash = norm_hex(header.get("parentHash"), 32)
    return headers


def authenticated_block_deposits(eth, header, identity):
    """Enumerate every matching log only after authenticating all receipts."""
    if not block_might_contain_deposit(header, identity):
        return []
    block_number = quantity(header["number"])
    block_hash = norm_hex(header["hash"], 32)
    block_receipts = eth.call("eth_getBlockReceipts", [hex(block_number)])
    if not isinstance(block_receipts, list) or not block_receipts:
        raise RelayerError("bloom-positive block returned no receipt array")
    if len(block_receipts) != len(header["transactions"]):
        raise RelayerError("receipt count does not match the authenticated block")
    items = []
    for index, receipt in enumerate(block_receipts):
        try:
            if (quantity(receipt["transactionIndex"]) != index or
                    quantity(receipt["blockNumber"]) != block_number or
                    norm_hex(receipt["blockHash"], 32) != block_hash):
                raise RelayerError("receipt metadata disagrees with its authenticated block")
            items.append((receipts.rlp(index), receipts.encode_receipt(receipt)))
        except (KeyError, TypeError, ValueError, AssertionError) as e:
            raise RelayerError("Ethereum RPC returned a malformed block receipt") from e
    trie = receipts.Trie(items)
    expected_root = bytes.fromhex(norm_hex(header["receiptsRoot"], 32, False))
    if trie.root() != expected_root:
        raise RelayerError("complete receipt array does not match the authenticated receipts root")

    token_topic = "0x" + "00" * 12 + norm_hex(identity["token"], 20, False)
    found = []
    for tx_index, receipt in enumerate(block_receipts):
        if quantity(receipt["status"]) != 1:
            continue
        tx_hash = norm_hex(receipt.get("transactionHash"), 32)
        logs = receipt.get("logs")
        if not isinstance(logs, list):
            raise RelayerError("authenticated receipt logs are malformed")
        for receipt_log_index, entry in enumerate(logs):
            topics = entry.get("topics")
            if (norm_hex(entry.get("address"), 20) != identity["vault"] or
                    not isinstance(topics, list) or len(topics) != 3 or
                    norm_hex(topics[0], 32) != DEPOSIT_TOPIC or
                    norm_hex(topics[2], 32) != token_topic):
                continue
            data = bytes.fromhex(norm_hex(entry.get("data"), 64, False))
            deposit_id = int(norm_hex(topics[1], 32, False), 16)
            amount = int.from_bytes(data[:32], "big")
            if deposit_id >= 1 << 64 or amount == 0:
                raise RelayerError("authenticated Deposit id/amount is outside the canonical range")
            found.append({
                "deposit_id": deposit_id, "block_number": block_number,
                "block_hash": block_hash, "tx_hash": tx_hash,
                "tx_index": tx_index, "receipt_log_index": receipt_log_index,
                "amount": amount, "recipient": "0x" + data[32:].hex(),
                "token": identity["token"],
            })
    return found


def scan_finalized(state, eth, identity, finalized, finalized_hash, chunk,
                   max_ancestry, plan_deposit, persist_cursor=True,
                   before_commit=None):
    start = state.cursor()
    headers = verified_execution_headers(
        eth, start, finalized, finalized_hash, max_ancestry)
    if headers and before_commit is not None:
        before_commit()
    batch_first, batch_deposits = start, 0
    while start <= finalized:
        found = authenticated_block_deposits(eth, headers[start], identity)
        for deposit in found:
            state.add_verified_deposit(deposit, plan_deposit(deposit))
        if persist_cursor:
            state.advance_cursor(start, start + 1)
        batch_deposits += len(found)
        batch_end = start
        start += 1
        if start > finalized or start - batch_first >= chunk:
            log("ethereum_scan", first=batch_first, last=batch_end,
                deposits=batch_deposits, completeness="receipts-root")
            batch_first, batch_deposits = start, 0


def write_common(workdir, common):
    workdir.mkdir(parents=True, exist_ok=True)
    atomic_json(workdir / "config.json", common[0])
    atomic_json(workdir / "updates.json", common[2])
    atomic_json(workdir / "finality_update.json", common[3])
    store_snapshot = common[6] if len(common) > 6 else None
    if store_snapshot is None:
        atomic_json(workdir / "bootstrap.json", common[1])
        with contextlib.suppress(FileNotFoundError):
            (workdir / "store.json").unlink()
    else:
        atomic_json(workdir / "store.json", store_snapshot)
        with contextlib.suppress(FileNotFoundError):
            (workdir / "bootstrap.json").unlink()


def capture_receipt(workdir, dep, common, eth, vault, token, registry, max_ancestry,
                    retained_anchors=(), origin_decimals=6, asset_decimals=6,
                    source_anchor=None):
    final_block, final_hash = common[4], common[5]
    if source_anchor is not None:
        final_block = source_anchor["block_number"]
        final_hash = source_anchor["block_hash"]
    depth = final_block - dep["block_number"]
    if depth < 0 or depth > max_ancestry:
        raise RelayerError(f"deposit ancestry depth {depth} is outside configured limit")
    chain, expected_hash = [], final_hash
    target_header = None
    for number in range(final_block, dep["block_number"] - 1, -1):
        header = eth.call("eth_getBlockByNumber", [hex(number), False])
        if not header or norm_hex(header["hash"], 32) != expected_hash:
            raise RelayerError(f"execution header mismatch at block {number}")
        encoded = encode_exec_header(header)
        if receipts.keccak256(encoded).hex() != expected_hash[2:]:
            raise RelayerError(f"execution RLP mismatch at block {number}")
        chain.append("0x" + encoded.hex())
        target_header, expected_hash = header, norm_hex(header["parentHash"], 32)
    if norm_hex(target_header["hash"], 32) != dep["block_hash"]:
        raise RelayerError("finalized deposit block hash changed")
    block_receipts = eth.call("eth_getBlockReceipts", [hex(dep["block_number"])])
    if dep["tx_index"] >= len(block_receipts):
        raise RelayerError("deposit transaction index exceeds receipt array")
    selected = block_receipts[dep["tx_index"]]
    if norm_hex(selected["transactionHash"], 32) != dep["tx_hash"] or quantity(selected["status"]) != 1:
        raise RelayerError("selected deposit receipt does not match discovery")
    items = [(receipts.rlp(i), receipts.encode_receipt(r)) for i, r in enumerate(block_receipts)]
    trie = receipts.Trie(items)
    want = bytes.fromhex(norm_hex(target_header["receiptsRoot"], 32, False))
    if trie.root() != want:
        raise RelayerError("locally rebuilt receipt trie root does not match header")
    key = receipts.rlp(dep["tx_index"])
    proof = {"block_number": dep["block_number"], "receipts_root": "0x" + want.hex(),
             "index": dep["tx_index"], "key": "0x" + key.hex(),
             "value": "0x" + items[dep["tx_index"]][1].hex(),
             "proof": ["0x" + n.hex() for n in trie.prove(key)], "exec_chain": chain,
             "vault": vault, "token": token, "expect_deposits": 1,
             "registry_id": registry,
             "receipt_log_index": dep["receipt_log_index"],
             "origin_decimals": origin_decimals, "asset_decimals": asset_decimals,
             "retained_anchor_hashes": list(retained_anchors)}
    if source_anchor is not None:
        proof["source_anchor"] = source_anchor
    atomic_json(workdir / "receipt_proof.json", proof)


def emit_plan(tool, workdir, runner=subprocess.run):
    result = runner([tool, str(workdir), "--emit-payloads"], capture_output=True, text=True)
    if result.returncode:
        raise RelayerError(f"payload tool failed ({result.returncode}): {result.stderr.strip()}")
    try:
        plan = json.loads(result.stdout)
    except (ValueError, TypeError) as e:
        raise RelayerError("payload tool stdout is not one JSON object") from e
    if not isinstance(plan, dict) or not isinstance(plan.get("updates"), list) or not isinstance(plan.get("backfills"), list):
        raise RelayerError("payload tool returned a malformed plan")
    return plan


def store_matches(record, info):
    """Compare the complete LC store, including committee and execution roots."""
    period = record.get("light_client_period", record.get("store_period"))
    required = (period, record.get("finalized_beacon_slot"),
                record.get("current_sync_committee_root"),
                info.get("light_client_period"), info.get("finalized_beacon_slot"),
                info.get("current_sync_committee_root"))
    if any(value is None for value in required):
        return False
    if quantity(period) != quantity(info["light_client_period"]):
        return False
    if quantity(record["finalized_beacon_slot"]) != quantity(info["finalized_beacon_slot"]):
        return False
    if norm_hex(record.get("finalized_beacon_root"), 32) != norm_hex(info.get("finalized_beacon_root"), 32):
        return False
    if quantity(record.get("anchor_block_number", -1)) != quantity(info.get("finalized_execution_block", -2)):
        return False
    if norm_hex(record.get("anchor_hash"), 32) != norm_hex(info.get("finalized_execution_hash"), 32):
        return False
    if norm_hex(record["current_sync_committee_root"], 32) != norm_hex(info["current_sync_committee_root"], 32):
        return False
    record_next, info_next = record.get("next_sync_committee_root"), info.get("next_sync_committee_root")
    if (record_next is None) != (info_next is None):
        return False
    return record_next is None or norm_hex(record_next, 32) == norm_hex(info_next, 32)


def check_lc_execution_windows(stores, initial_block=None):
    """Mirror the consensus bound before asking a wallet to fund carriers."""
    previous = initial_block
    for record in stores:
        current = quantity(record.get("anchor_block_number"))
        if previous is not None and (current < previous or
                                     current - previous > MAX_LC_EXECUTION_ADVANCE):
            raise RelayerError(
                "light-client execution advance exceeds the 20,000-block "
                "backfill window; the beacon provider must supply missing "
                "intermediate period updates")
        previous = current


def plan_finalized_execution(plan):
    """Read the final execution anchor from verifier-produced store metadata."""
    initial = plan.get("bootstrap") or plan.get("store")
    if not isinstance(initial, dict) or not isinstance(plan.get("updates"), list):
        raise RelayerError("payload plan omitted its verified light-client store")
    stores = [initial, *plan["updates"]]
    if any(not isinstance(record, dict) for record in stores):
        raise RelayerError("payload plan has malformed light-client metadata")
    final = stores[-1]
    return (quantity(final.get("anchor_block_number")),
            norm_hex(final.get("anchor_hash"), 32))


def select_records(plan, info, prefix, dep=None):
    bootstrap, snapshot = plan.get("bootstrap"), plan.get("store")
    if (bootstrap is None) == (snapshot is None):
        raise RelayerError("payload plan must contain exactly one initial store")
    stores = [bootstrap if bootstrap is not None else snapshot] + plan["updates"]
    if any(not isinstance(record, dict) for record in stores):
        raise RelayerError("payload plan omitted bootstrap/store metadata")
    if info.get("light_client_bootstrapped"):
        matches = [i for i, record in enumerate(stores) if store_matches(record, info)]
        if not matches:
            raise RelayerError("emitted light-client history cannot reconcile exact B3 store")
        check_lc_execution_windows(stores[matches[-1]:])
        ordered = stores[matches[-1] + 1:]
    else:
        if bootstrap is None:
            raise RelayerError("cannot initialize B3 from an exported store without a bootstrap")
        check_lc_execution_windows(
            stores, quantity(info["origin_deployment_block"]))
        ordered = stores
    if dep is None:
        if plan["backfills"] or plan.get("mint") is not None:
            raise RelayerError("sync-only payload plan unexpectedly contains a deposit")
    else:
        ordered += plan["backfills"]
        if plan.get("mint") is None:
            raise RelayerError("deposit payload plan omitted mint")
        expected_source = dep.get("source_anchor")
        emitted_source = plan.get("source_anchor")
        if not isinstance(expected_source, dict) or not isinstance(emitted_source, dict):
            raise RelayerError("deposit payload plan omitted its B3 retained source anchor")
        try:
            emitted_identity = (
                quantity(emitted_source["block_number"]),
                norm_hex(emitted_source["block_hash"], 32),
                norm_hex(emitted_source["receipts_root"], 32),
                quantity(emitted_source["source_finalized_beacon_slot"]),
                quantity(emitted_source["source_finalized_execution_block"]),
                quantity(emitted_source["execution_timestamp"]),
                quantity(emitted_source["connected_height"]),
                norm_hex(emitted_source["connected_block"], 32),
                quantity(emitted_source["b3_finalized_height"]),
                norm_hex(emitted_source["b3_finalized_block"], 32),
            )
        except (KeyError, TypeError, ValueError) as e:
            raise RelayerError("payload tool emitted malformed retained-anchor metadata") from e
        if emitted_identity != execution_anchor_payload_identity(expected_source):
            raise RelayerError("payload source anchor does not match B3 finalized state")
        mint = plan["mint"]
        if quantity(mint.get("deposit_id")) != int(dep["deposit_id"]) or quantity(mint.get("tx_index")) != dep["tx_index"]:
            raise RelayerError("mint metadata does not match discovered deposit")
        if quantity(mint.get("receipt_log_index")) != dep["receipt_log_index"] or quantity(mint.get("target_block_number")) != dep["block_number"]:
            raise RelayerError("mint proof location does not match discovered deposit")
        if norm_hex(mint.get("registry_id"), 32) != norm_hex(info["registry_id"], 32):
            raise RelayerError("mint registry does not match B3 getbridgeinfo")
        if (norm_hex(mint.get("origin_token"), 20) != norm_hex(dep["token"], 20) or
                norm_hex(dep["token"], 20) != norm_hex(info["token"], 20)):
            raise RelayerError("mint token does not match the configured bridge token")
        if quantity(mint.get("origin_amount")) != int(dep["amount"]):
            raise RelayerError("mint origin amount does not match discovered Deposit")
        if norm_hex(mint.get("b3_recipient"), 32) != norm_hex(dep["recipient"], 32):
            raise RelayerError("mint recipient does not match discovered Deposit")
        if norm_hex(mint.get("target_block_hash"), 32) != norm_hex(dep["block_hash"], 32):
            raise RelayerError("mint target hash does not match finalized Deposit block")
        mint = dict(mint)
        amount = quantity(mint.get("amount"))
        if amount <= 0:
            raise RelayerError("mint amount is not positive")
        mint["amount"], mint["address"] = str(amount), recipient_address(mint.get("b3_recipient"), prefix)
        ordered.append(mint)
    out = []
    for record in ordered:
        if not isinstance(record, dict) or record.get("kind") not in ("bootstrap", "update", "execution-backfill", "mint"):
            raise RelayerError("payload plan has an invalid record kind")
        norm_hex(record.get("payload_hex"), prefix=False)
        out.append(record)
    return out


def confirmations(b3, txid):
    try:
        result = b3.call("gettransaction", [txid])
    except RpcError as e:
        if e.code in (-5, -8):
            return None
        raise
    value = quantity(result.get("confirmations", 0))
    return value


def finalized_height(node):
    value = node.call("getfinalitystatus").get("finalized")
    return None if not value else quantity(value["height"])


def transaction_height(wallet, node, txid):
    result = wallet.call("gettransaction", [txid])
    if result.get("blockheight") is not None:
        return quantity(result["blockheight"])
    if result.get("blockhash"):
        return quantity(node.call("getblockheader", [result["blockhash"]])["height"])
    return None


def store_ahead(record, info):
    if not info.get("light_client_bootstrapped"):
        return False
    record_period = quantity(record.get("light_client_period", record.get("store_period")))
    current_period = quantity(info["light_client_period"])
    if current_period != record_period:
        return current_period > record_period
    record_slot, current_slot = quantity(record["finalized_beacon_slot"]), quantity(info["finalized_beacon_slot"])
    if current_slot != record_slot:
        return current_slot > record_slot
    same_current = norm_hex(record["current_sync_committee_root"], 32) == norm_hex(
        info["current_sync_committee_root"], 32)
    same_finality = (
        norm_hex(record["finalized_beacon_root"], 32) == norm_hex(info["finalized_beacon_root"], 32) and
        quantity(record["anchor_block_number"]) == quantity(info["finalized_execution_block"]) and
        norm_hex(record["anchor_hash"], 32) == norm_hex(info["finalized_execution_hash"], 32))
    return (same_current and same_finality and
            record.get("next_sync_committee_root") is None and
            info.get("next_sync_committee_root") is not None)


def effect_status(node, row, info, final):
    """Return (status, connection height, connection block)."""
    metadata = json.loads(row["metadata"])
    if row["kind"] in ("bootstrap", "update"):
        if not info.get("light_client_bootstrapped"):
            return "absent", None, None
        height = info.get("light_client_connected_height")
        block = info.get("light_client_connected_block")
        if height is None or block is None:
            raise RelayerError("getbridgeinfo omitted light-client connection metadata")
        if store_matches(metadata, info):
            status = "final"
        elif store_ahead(metadata, info):
            status = "superseded"
        else:
            target_period = quantity(metadata.get("light_client_period", metadata.get("store_period")))
            current_period = quantity(info["light_client_period"])
            target_slot = quantity(metadata["finalized_beacon_slot"])
            current_slot = quantity(info["finalized_beacon_slot"])
            same_current = norm_hex(metadata["current_sync_committee_root"], 32) == norm_hex(
                info["current_sync_committee_root"], 32)
            target_is_next = (target_period > current_period or
                              (target_period == current_period and target_slot > current_slot) or
                              (target_period == current_period and target_slot == current_slot and
                               same_current and metadata.get("next_sync_committee_root") is not None and
                               info.get("next_sync_committee_root") is None))
            if target_is_next:
                return "absent", None, None
            raise RelayerError("active B3 light-client store diverges from the queued history")
        height = quantity(height)
        block = norm_hex(block, 32, False)
        if active_block_hash(node, height) != block:
            return "absent", None, None
        return ("pending" if final is None or height > final else status), height, block
    if row["kind"] not in ("execution-backfill", "mint"):
        return "absent", None, None
    target = metadata.get("target_block_hash")
    deposit_id = quantity(metadata["deposit_id"]) if row["kind"] == "mint" else None
    result = node.call("getbridgeproofstatus", [target, deposit_id])
    if not result.get("state_available"):
        raise RelayerError("B3 bridge proof status is unavailable")
    if row["kind"] == "mint":
        effect = result.get("deposit", {})
        if not effect.get("claimed"):
            return "absent", None, None
        height = effect.get("claimed_height")
        block = effect.get("claimed_block")
    else:
        effect = result.get("anchor", {})
        if not effect.get("known"):
            return "absent", None, None
        height = effect.get("connected_height")
        block = effect.get("connected_block")
    if height is None or block is None:
        raise RelayerError(f"{row['kind']} effect omitted B3 connection metadata")
    height = quantity(height)
    block = norm_hex(block, 32, False)
    if active_block_hash(node, height) != block:
        return "absent", None, None
    return ("pending" if final is None or height > final else "final"), height, block


def local_transaction_final(wallet, node, row, required, final):
    if not row["txid"]:
        return None
    try:
        result = wallet.call("gettransaction", [row["txid"]])
        seen = quantity(result.get("confirmations", 0))
        if seen < 0 or not result.get("blockhash"):
            return None
        included = (quantity(result["blockheight"]) if result.get("blockheight") is not None
                    else transaction_height(wallet, node, row["txid"]))
    except (RpcError, RelayerError):
        return None
    if seen < required or included is None or final is None or included > final:
        return None
    active = active_block_hash(node, included)
    if norm_hex(result["blockhash"], 32, False) != active:
        return None
    return included, active


def active_block_hash(node, height):
    return norm_hex(node.call("getblockhash", [height]), 32, False)


def mempool_has(node, txid):
    try:
        node.call("getmempoolentry", [txid])
        return True
    except RpcError as e:
        if e.code in (-5, -8):
            return False
        raise


def transaction_conflict_error(error):
    if not isinstance(error, RpcError) or error.code not in (-25, -26):
        return False
    message = error.message.lower()
    return any(marker in message for marker in (
        "txn-mempool-conflict", "mempool conflict", "missing inputs",
        "inputs-missingorspent", "inputs missing or spent"))


def audit_confirmed(state, node, wallet, required):
    rows = state.confirmed()
    if not rows:
        return
    final = finalized_height(node)
    info = node.call("getbridgeinfo")
    for row in rows:
        if row["effect_height"] is not None and final is not None:
            height = quantity(row["effect_height"])
            if height <= final and active_block_hash(node, height) == row["effect_block"]:
                continue
            state.reopen_from(row["id"])
            log("job_reopened", sequence=row["id"], kind=row["kind"], reason="active-chain-changed")
            return
        inclusion = local_transaction_final(wallet, node, row, required, final)
        if inclusion is not None:
            state.confirm_effect(row["id"], inclusion[0], inclusion[1])
            continue
        status, height, block = effect_status(node, row, info, final)
        if status == "final":
            state.confirm_effect(row["id"], height, block)
            continue
        if status == "superseded":
            raise RelayerError("cannot re-audit a historical external LC step from the current store")
        state.reopen_from(row["id"])
        log("job_reopened", sequence=row["id"], kind=row["kind"], reason=status)
        return


def process_jobs(state, node, wallet, required, dry_run,
                 max_fee_atoms=None, daily_fee_budget_atoms=None):
    if dry_run:
        for row in state.pending():
            log("dry_run_job", sequence=row["id"], kind=row["kind"], state=row["state"], payload_id=row["payload_id"])
        return
    if max_fee_atoms is None or daily_fee_budget_atoms is None:
        raise RelayerError("live relaying requires explicit network-fee budgets")
    while True:
        row = state.first()
        if row is None:
            return
        info = node.call("getbridgeinfo")
        if not info.get("active") or not info.get("state_available"):
            raise RelayerError("B3 bridge is not active with available state")
        final = finalized_height(node)
        inclusion = local_transaction_final(wallet, node, row, required, final)
        if inclusion is not None:
            state.confirm_effect(row["id"], inclusion[0], inclusion[1])
            log("job_confirmed", kind=row["kind"], txid=row["txid"])
            continue
        effect, effect_height, effect_block = effect_status(node, row, info, final)
        if effect == "final":
            state.confirm_effect(row["id"], effect_height, effect_block)
            log("job_reconciled", kind=row["kind"], txid=row["txid"])
            continue
        if effect == "pending":
            log("job_waiting_effect_finality", kind=row["kind"], txid=row["txid"])
            return
        if effect == "superseded":
            state.set_state(row["id"], "superseded")
            log("job_superseded", kind=row["kind"])
            continue
        if row["kind"] == "mint" and not info.get("mint_approval_open"):
            raise RelayerError("B3 mint approval window is closed")
        if row["state"] == "planned":
            options = {"broadcast": False, "replaceable": False}
            if row["kind"] == "mint":
                result = wallet.call("claimbridgedeposit", [row["payload"], int(row["amount"]), row["address"], options])
            else:
                result = wallet.call("submitbridgecarrier", [row["payload"], options])
            txid, raw = norm_hex(result.get("txid"), 32, False), norm_hex(result.get("hex"), prefix=False)
            if result.get("broadcast") is not False:
                raise RelayerError("wallet RPC unexpectedly broadcast before durable persistence")
            fee_atoms = b3_amount_atoms(result.get("network_fee"), "wallet network_fee")
            fee_day = time.strftime("%Y-%m-%d", time.gmtime())
            state.prepared(row["id"], txid, raw, fee_atoms, fee_day,
                           max_fee_atoms, daily_fee_budget_atoms)
            log("job_prepared", kind=row["kind"], txid=txid,
                fee_atoms=fee_atoms, fee_day=fee_day)
            continue
        seen = confirmations(wallet, row["txid"])
        if seen is not None and seen < 0:
            state.reset_transaction(row["id"])
            log("job_reprepared", kind=row["kind"], txid=row["txid"],
                reason="wallet-conflict")
            return
        in_mempool = (mempool_has(node, row["txid"])
                      if seen is None or seen == 0 else False)
        if in_mempool:
            seen = 0
            if row["state"] == "prepared":
                state.set_state(row["id"], "broadcast")
                log("job_broadcast_recovered", kind=row["kind"],
                    txid=row["txid"])
        if (row["state"] in ("prepared", "broadcast") and
                (seen is None or (seen == 0 and not in_mempool))):
            try:
                sent = norm_hex(node.call("sendrawtransaction", [row["raw_tx"]]), 32, False)
            except RpcError as send_error:
                seen = confirmations(wallet, row["txid"])
                if seen is not None and seen < 0:
                    state.reset_transaction(row["id"])
                    log("job_reprepared", kind=row["kind"], txid=row["txid"],
                        reason="send-conflict")
                    # Bound conflict recovery to one wallet preparation per
                    # cycle.  A conflicting wallet transaction may keep
                    # reporting the same abandoned txid until its wallet state
                    # settles; spinning here could repeatedly reserve/release
                    # fees and monopolize the daemon loop.
                    return
                if (seen is None or seen == 0) and mempool_has(node, row["txid"]):
                    seen = 0
                elif transaction_conflict_error(send_error):
                    state.reset_transaction(row["id"])
                    log("job_reprepared", kind=row["kind"], txid=row["txid"],
                        reason="node-conflict")
                    return
                elif seen is None or seen == 0:
                    raise send_error
            else:
                if sent != row["txid"]:
                    raise RelayerError("sendrawtransaction returned a different txid")
            state.set_state(row["id"], "broadcast")
            log("job_broadcast", kind=row["kind"], txid=row["txid"])
            seen = confirmations(wallet, row["txid"])
        if seen is None or seen < required:
            log("job_waiting", kind=row["kind"], txid=row["txid"], confirmations=seen or 0, required=required)
            return
        inclusion = local_transaction_final(wallet, node, row, required, finalized_height(node))
        if inclusion is None:
            log("job_waiting_finality", kind=row["kind"], txid=row["txid"],
                finalized_height=finalized_height(node))
            return
        state.confirm_effect(row["id"], inclusion[0], inclusion[1])
        log("job_confirmed", kind=row["kind"], txid=row["txid"], confirmations=seen)


def run_once(args, state, eth, witnesses, node, wallet, prefix):
    info = node.call("getbridgeinfo")
    eth_chain = quantity(eth.call("eth_chainId"))
    if eth_chain != args.ethereum_chain_id:
        raise RelayerError(f"Ethereum chain id {eth_chain} is not {args.ethereum_chain_id}")
    for witness in witnesses:
        if quantity(witness.call("eth_chainId")) != eth_chain:
            raise RelayerError("Ethereum providers disagree on chain id")
    identity = bridge_identity(info, eth_chain, args.trusted_root)
    pinned_start = identity["origin_deployment_block"]
    if args.start_block is not None and args.start_block != pinned_start:
        raise RelayerError("--start-block does not match B3 origin_deployment_block")
    # Do this before binding or advancing durable state. The receipt proof only
    # authenticates the log emitter address; these checks prove that every
    # provider sees the exact reviewed vault/verifier/token runtime at the first
    # admissible block and still sees it now.
    verify_runtime_code_pins([eth, *witnesses], identity)
    state.bind(identity, pinned_start)
    audit_confirmed(state, node, wallet, args.b3_confirmations)
    store_snapshot = None
    if info.get("light_client_bootstrapped"):
        store_snapshot = validate_store_snapshot(
            node.call("getbridgelightclientstore"), info)
    common = capture_common(args.beacon_url.rstrip("/"), args.trusted_root,
                            info, store_snapshot)
    sync_dir = Path(args.work_root) / "sync"
    write_common(sync_dir, common)
    with contextlib.suppress(FileNotFoundError):
        (sync_dir / "receipt_proof.json").unlink()
    # Verify the beacon bootstrap/update chain in the consensus C++ verifier
    # before persisting any Ethereum scan cursor derived from that claimed
    # finality.  Otherwise a faulty beacon endpoint could move the cursor past
    # blocks that are not final yet, and a later reorg could make us skip the
    # replacement deposits even though no invalid mint could pass consensus.
    sync_plan = emit_plan(args.payload_tool, sync_dir)
    proven_number, proven_hash = plan_finalized_execution(sync_plan)
    common = (*common[:4], proven_number, proven_hash, *common[6:])
    rpc_final = eth.call("eth_getBlockByNumber", [hex(proven_number), False])
    if not rpc_final or norm_hex(rpc_final["hash"], 32) != proven_hash:
        raise RelayerError("execution RPC disagrees with the proven finalized execution hash")
    for witness in witnesses:
        witnessed_final = witness.call(
            "eth_getBlockByNumber", [hex(proven_number), False])
        if (not witnessed_final or
                norm_hex(witnessed_final["hash"], 32) != proven_hash):
            raise RelayerError(
                "secondary Ethereum RPC disagrees with the proven finalized execution hash")
    sync_records = select_records(sync_plan, info, prefix)
    # Refuse to persist a plan if another process advanced or replaced the B3
    # LC store while network evidence and payloads were being assembled.
    rechecked_info = node.call("getbridgeinfo")
    if bridge_identity(rechecked_info, eth_chain, args.trusted_root) != identity:
        raise RelayerError("B3 bridge pins changed while assembling a relayer plan")
    rechecked_store = None
    if rechecked_info.get("light_client_bootstrapped"):
        rechecked_store = validate_store_snapshot(
            node.call("getbridgelightclientstore"), rechecked_info)
    if store_snapshot_fingerprint(rechecked_store) != store_snapshot_fingerprint(store_snapshot):
        raise RelayerError("B3 light-client store changed while assembling a relayer plan; retry")
    state.add_plan(sync_records)
    if args.dry_run and sync_records:
        # The verified sync records are intentionally not applied in dry-run
        # mode, so the B3 node cannot yet expose their retained execution
        # anchor. Report the planned records and stop before deposit scanning;
        # a later rehearsal can continue once those records exist on B3.
        process_jobs(state, node, wallet, args.b3_confirmations, True,
                     args.max_fee_atoms, args.daily_fee_budget_atoms)
        return
    if not args.dry_run:
        process_jobs(state, node, wallet, args.b3_confirmations, False,
                     args.max_fee_atoms, args.daily_fee_budget_atoms)
        if state.first() is not None:
            return
        info = node.call("getbridgeinfo")

    providers = [eth, *witnesses]
    anchor_cache = {}

    def source_anchor_for(target):
        anchor = fetch_execution_anchor(node, target)
        key = execution_anchor_identity(anchor)
        if key not in anchor_cache:
            corroborate_execution_anchor(anchor, providers)
            anchor_cache[key] = anchor
        return anchor_cache[key]

    def plan_deposit(dep):
        dep = dict(dep)
        source_anchor = dep.get("source_anchor")
        if source_anchor is None:
            source_anchor = source_anchor_for(dep["block_number"])
            dep["source_anchor"] = source_anchor
        workdir = Path(args.work_root) / f"deposit-{dep['deposit_id']}-{dep['tx_hash'][2:18]}"
        write_common(workdir, common)
        retained = state.retained_anchors()
        if info.get("light_client_bootstrapped"):
            retained.append(norm_hex(info["finalized_execution_hash"], 32))
        retained.append(source_anchor["block_hash"])
        capture_receipt(workdir, dep, common, eth, identity["vault"], identity["token"],
                        identity["registry_id"],
                        args.max_ancestry, sorted(set(retained)),
                        identity["origin_decimals"], identity["asset_decimals"],
                        source_anchor=source_anchor)
        plan = emit_plan(args.payload_tool, workdir)
        recheck_execution_anchor(node, source_anchor)
        records = select_records(plan, info, prefix, dep)
        prune_deposit_workdirs(args.work_root, args.workdir_retention)
        log("deposit_planned", deposit_id=dep["deposit_id"], txid=dep["tx_hash"])
        return records

    # Finish any row left by the pre-atomic preview implementation before
    # advancing the cursor. New pages persist each Deposit and plan together.
    for dep_row in state.unplanned():
        dep = dict(dep_row)
        # Legacy preview databases predate the explicit token field; the
        # immutable database identity already binds the configured token.
        dep["token"] = identity["token"]
        state.add_plan(plan_deposit(dep), dep["id"])
    cursor = state.cursor()
    if cursor <= common[4]:
        scan_anchor = source_anchor_for(cursor)

        def plan_scanned_deposit(dep):
            dep = dict(dep)
            dep["source_anchor"] = scan_anchor
            return plan_deposit(dep)

        scan_finalized(
            state, eth, identity, scan_anchor["block_number"],
            scan_anchor["block_hash"], args.scan_chunk, args.max_ancestry,
            plan_scanned_deposit, persist_cursor=not args.dry_run,
            before_commit=lambda: recheck_execution_anchor(node, scan_anchor))
    process_jobs(state, node, wallet, args.b3_confirmations, args.dry_run,
                 args.max_fee_atoms, args.daily_fee_budget_atoms)


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--ethereum-rpc", required=True)
    p.add_argument("--ethereum-rpc-secondary", action="append", default=[],
                   help="independent execution RPC used to corroborate the proven finalized block; repeatable")
    p.add_argument("--beacon-url", required=True)
    p.add_argument("--b3-rpc-url", default="http://127.0.0.1:5467")
    p.add_argument("--b3-rpc-user")
    p.add_argument("--b3-rpc-password")
    p.add_argument("--b3-cookie")
    p.add_argument("--wallet", required=True)
    p.add_argument("--payload-tool", default="build/bin/b3-bridge-ethcheck")
    p.add_argument("--trusted-root", required=True)
    p.add_argument("--start-block", type=int)
    p.add_argument("--state", default="b3-bridge-relayer.sqlite3")
    p.add_argument("--work-root")
    p.add_argument("--ethereum-chain-id", type=int, default=1)
    p.add_argument("--b3-p2pkh-prefix", type=int)
    p.add_argument("--b3-confirmations", type=int, default=1)
    p.add_argument("--max-fee-b3",
                   help="required live per-transaction native B3 fee ceiling")
    p.add_argument("--daily-fee-budget-b3",
                   help="required live UTC-day native B3 fee budget")
    p.add_argument("--scan-chunk", type=int, default=1000)
    p.add_argument("--max-ancestry", type=int, default=MAX_ANCESTRY_DISTANCE)
    p.add_argument("--workdir-retention", type=int, default=8,
                   help="number of completed deposit proof directories to retain")
    p.add_argument("--poll-seconds", type=float, default=30)
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--one-shot", action="store_true")
    args = p.parse_args(argv)
    if bool(args.b3_rpc_user) != bool(args.b3_rpc_password):
        p.error("--b3-rpc-user and --b3-rpc-password must be supplied together")
    if args.b3_cookie and args.b3_rpc_user:
        p.error("choose cookie or user/password B3 RPC authentication")
    if (args.b3_confirmations < 1 or args.scan_chunk < 1 or args.max_ancestry < 0 or
            args.max_ancestry > MAX_ANCESTRY_DISTANCE or
            args.workdir_retention < 0 or args.workdir_retention > 1000):
        p.error("confirmation/chunk limits are invalid")
    if not args.dry_run and not args.ethereum_rpc_secondary:
        p.error("live relaying requires at least one --ethereum-rpc-secondary")
    try:
        origins = [provider_origin(url) for url in
                   [args.ethereum_rpc, *args.ethereum_rpc_secondary]]
    except RelayerError as e:
        p.error(str(e))
    if len(set(origins)) != len(origins):
        p.error("Ethereum RPC providers must use distinct hostname/port origins")
    if not args.dry_run and (args.max_fee_b3 is None or
                             args.daily_fee_budget_b3 is None):
        p.error("live relaying requires --max-fee-b3 and --daily-fee-budget-b3")
    try:
        args.max_fee_atoms = (None if args.max_fee_b3 is None else
                              b3_amount_atoms(args.max_fee_b3, "--max-fee-b3"))
        args.daily_fee_budget_atoms = (
            None if args.daily_fee_budget_b3 is None else
            b3_amount_atoms(args.daily_fee_budget_b3,
                            "--daily-fee-budget-b3"))
    except RelayerError as e:
        p.error(str(e))
    if not args.dry_run and (args.max_fee_atoms <= 0 or
                             args.daily_fee_budget_atoms < args.max_fee_atoms):
        p.error("live daily fee budget must be at least one positive per-transaction budget")
    args.work_root = args.work_root or args.state + ".work"
    return args


def main(argv=None):
    args = parse_args(argv)
    prune_deposit_workdirs(args.work_root, args.workdir_retention)
    process_lock = ProcessLock(args.state)
    state = State(args.state)
    eth = JsonRpc(args.ethereum_rpc)
    witnesses = [JsonRpc(url) for url in args.ethereum_rpc_secondary]
    node = JsonRpc(args.b3_rpc_url, args.b3_rpc_user, args.b3_rpc_password, args.b3_cookie)
    wallet = node.wallet(args.wallet)
    chain = node.call("getblockchaininfo").get("chain")
    prefix = args.b3_p2pkh_prefix
    if prefix is None:
        if chain == "main":
            prefix = 63
        elif chain in ("test", "testnet", "regtest", "signet"):
            prefix = 111
        else:
            raise RelayerError(f"unknown B3 chain {chain!r}; specify --b3-p2pkh-prefix")
    while True:
        try:
            run_once(args, state, eth, witnesses, node, wallet, prefix)
        except Exception as e:
            log("cycle_failed", error=str(e))
            if args.one_shot:
                state.close()
                process_lock.close()
                return 1
        if args.one_shot:
            state.close()
            process_lock.close()
            return 0
        time.sleep(args.poll_seconds)


if __name__ == "__main__":
    sys.exit(main())
