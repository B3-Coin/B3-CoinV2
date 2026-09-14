# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Bounded local controls for an explicitly attended, disposable Qt fixture.

The harness must construct this controller only inside its opt-in Qt hold.
It opens no listener, forwards no RPC, and accepts no filesystem paths or
economic actions. The only optional callback mines 1..32 isolated blocks and
returns their public hashes. Counterparty orders remain explicit fixture work.

Write a JSON object to the manifest's command_file, preferably by atomic local
replacement. Examples: {"id": 1, "command": "snapshot"},
{"id": 2, "command": "endpoint_fault", "endpoint_index": 0,
 "drop_submit_once": true}, {"id": 3, "command": "clear_faults"},
{"id": 4, "command": "mine_blocks", "count": 1}.
An endpoint fault may also set response_hold_ms={"action":6000,"submit":6000}
or receipt_expired_action_id="<exact public action id>". Holds are after upstream
completion and clear_faults releases them. request_page accepts endpoint_index,
offset, limit (1..64), and optional end_offset; carry the returned end_offset
through subsequent pages to cover one captured high-water range without gaps.
Each new ID is consumed before invoking any callback. Repeated IDs never run
again, including after a callback failure. Existing controller artifacts refuse
construction, so a restarted controller cannot forget earlier command IDs.
"""

import hashlib
import json
import os
from pathlib import Path
import re
import stat
import tempfile
import time


MAX_COMMAND_BYTES = 4096
MAX_COMMANDS = 128
MAX_RESULT_BYTES = 1024 * 1024
MAX_LOG_BYTES = 16 * 1024 * 1024
MAX_RELAYS = 4
MAX_RECENT_REQUESTS = 16
MAX_ACTION_HEX = 8192
MAX_MINE_BLOCKS = 32
MAX_RESPONSE_HOLD_MS = 6000
RESPONSE_HOLD_METHODS = frozenset({"markets", "snapshot", "updates", "action", "submit"})
MAX_REQUEST_PAGE_RECORDS = 64
MAX_RETAINED_REQUESTS = 4096
MAX_BODY_HEX = 16384


class FlowMeshQtReviewController:
    def __init__(self, tmpdir, relays, *, mine_blocks=None):
        self.directory = Path(tmpdir).resolve(strict=True)
        if not self.directory.is_dir() or self.directory == Path(self.directory.anchor):
            raise ValueError("Qt review controls require the exact disposable fixture directory")
        self.relays = tuple(relays)
        if not 1 <= len(self.relays) <= MAX_RELAYS:
            raise ValueError("Qt review controls require 1..4 test relays")
        if mine_blocks is not None and not callable(mine_blocks):
            raise ValueError("mine_blocks must be a fixture-owned callable")
        self.mine_blocks = mine_blocks
        self.command_file = self.directory / "qt-review-command.json"
        self.status_file = self.directory / "qt-review-control-status.json"
        self.log_file = self.directory / "qt-review-controls.jsonl"
        if any(os.path.lexists(path) for path in (self.command_file, self.status_file, self.log_file)):
            raise ValueError("Qt review controller files must not predate this attended hold")
        self.last_id = 0
        self.commands_seen = 0
        self.log_bytes = 0
        self.last_input = None
        self.disabled = False
        self.log_fd = os.open(self.log_file, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        try:
            self._publish({"state": "ready", "last_id": 0, "commands_seen": 0})
        except Exception:
            self.close()
            raise

    def manifest(self):
        return {"command_file": str(self.command_file), "status_file": str(self.status_file),
                "log_file": str(self.log_file), "endpoint_count": len(self.relays),
                "commands": ["endpoint_fault", "clear_faults", "snapshot", "request_page"] +
                            (["mine_blocks"] if self.mine_blocks is not None else []),
                "max_commands": MAX_COMMANDS, "max_command_bytes": MAX_COMMAND_BYTES,
                "max_mine_blocks": MAX_MINE_BLOCKS, "initial_command_id": 1,
                "max_response_hold_ms": MAX_RESPONSE_HOLD_MS,
                "response_hold_methods": sorted(RESPONSE_HOLD_METHODS),
                "max_request_page_records": MAX_REQUEST_PAGE_RECORDS,
                "request_page_instruction": "Start with offset 0; carry end_offset and next_offset across pages. Coverage requires complete=true and records_dropped=0. Refresh pages after handlers complete to capture final completion fields. Body-size omissions remain explicit.",
                "instruction": "Use increasing IDs; consult status after each command. IDs are never replayed. No economic action or generic RPC control is exposed."}

    def _publish(self, result):
        data = json.dumps(result, sort_keys=True, separators=(",", ":")).encode() + b"\n"
        if len(data) > MAX_RESULT_BYTES:
            raise ValueError("Qt review status exceeds its bound")
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(prefix="qt-review-status-", dir=self.directory, delete=False) as output:
                temporary = Path(output.name)
                output.write(data)
                output.flush()
                os.fsync(output.fileno())
            os.replace(temporary, self.status_file)
            temporary = None
        finally:
            if temporary is not None:
                temporary.unlink()

    def _record(self, event):
        data = json.dumps(event, sort_keys=True, separators=(",", ":")).encode() + b"\n"
        if len(data) > MAX_RESULT_BYTES or self.log_bytes + len(data) > MAX_LOG_BYTES:
            raise ValueError("Qt review evidence log exceeds its bound")
        remaining = memoryview(data)
        while remaining:
            written = os.write(self.log_fd, remaining)
            if written <= 0:
                raise OSError("Qt review evidence write failed")
            remaining = remaining[written:]
        os.fsync(self.log_fd)
        self.log_bytes += len(data)
        self._publish(event)

    def _read_command(self):
        # O_NONBLOCK avoids a substituted FIFO blocking the harness thread;
        # O_NOFOLLOW and fstat reject links/devices before reading any contents.
        try:
            fd = os.open(self.command_file, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        except FileNotFoundError:
            return None
        with os.fdopen(fd, "rb") as source:
            info = os.fstat(source.fileno())
            if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_uid != os.getuid():
                raise ValueError("Command must be a fixture-owned regular file with one link")
            if info.st_size > MAX_COMMAND_BYTES:
                raise ValueError("Command exceeds 4096 bytes")
            data = source.read(MAX_COMMAND_BYTES + 1)
        if len(data) > MAX_COMMAND_BYTES:
            raise ValueError("Command exceeds 4096 bytes")
        return data

    @staticmethod
    def _parse(data):
        def unique_object(pairs):
            result = {}
            for key, value in pairs:
                if key in result:
                    raise ValueError("Duplicate command field")
                result[key] = value
            return result
        value = json.loads(data, object_pairs_hook=unique_object)
        if not isinstance(value, dict):
            raise ValueError("Command must be an object")
        identifier = value.get("id")
        if type(identifier) is not int or not 1 <= identifier <= 2**31 - 1:
            raise ValueError("Command ID must be an integer in 1..2147483647")
        return value

    def _validate(self, value):
        command = value.get("command")
        fields = {"id", "command"}
        if command == "endpoint_fault":
            fields |= {"endpoint_index", "unavailable", "drop_submit_once", "response_hold_ms", "receipt_expired_action_id"}
            index = value.get("endpoint_index")
            if type(index) is not int or not 0 <= index < len(self.relays):
                raise ValueError("endpoint_index is outside this fixture")
            for key in ("unavailable", "drop_submit_once"):
                if type(value.get(key, False)) is not bool:
                    raise ValueError("Endpoint fault fields must be booleans")
            holds = value.get("response_hold_ms", {})
            if not isinstance(holds, dict) or set(holds) - RESPONSE_HOLD_METHODS:
                raise ValueError("Response holds require allowlisted public client methods")
            if any(type(delay) is not int or not 0 <= delay <= MAX_RESPONSE_HOLD_MS for delay in holds.values()):
                raise ValueError("Response holds must be integers in 0..6000 milliseconds")
            if "receipt_expired_action_id" in value and (
                    not isinstance(value["receipt_expired_action_id"], str) or
                    re.fullmatch(r"[0-9a-f]{64}", value["receipt_expired_action_id"]) is None):
                raise ValueError("Receipt expiry requires one exact public action ID")
        elif command == "request_page":
            fields |= {"endpoint_index", "offset", "limit", "end_offset"}
            index, offset, limit = value.get("endpoint_index"), value.get("offset"), value.get("limit")
            if type(index) is not int or not 0 <= index < len(self.relays):
                raise ValueError("endpoint_index is outside this fixture")
            if type(offset) is not int or not 0 <= offset <= MAX_RETAINED_REQUESTS:
                raise ValueError("Request page offset is outside the retained bound")
            if type(limit) is not int or not 1 <= limit <= MAX_REQUEST_PAGE_RECORDS:
                raise ValueError("Request page limit must be in 1..64")
            end = value.get("end_offset", MAX_RETAINED_REQUESTS)
            if type(end) is not int or not offset <= end <= MAX_RETAINED_REQUESTS:
                raise ValueError("Request page end_offset is outside the retained bound")
        elif command in ("clear_faults", "snapshot"):
            pass
        elif command == "mine_blocks":
            fields.add("count")
            count = value.get("count")
            if self.mine_blocks is None:
                raise ValueError("Mining is not enabled in this attended fixture")
            if type(count) is not int or not 1 <= count <= MAX_MINE_BLOCKS:
                raise ValueError("Mining count must be in 1..32")
        else:
            raise ValueError("Unsupported Qt review command")
        if set(value) - fields:
            raise ValueError("Unexpected command fields")

    @staticmethod
    def _public_request(row, *, full_body=False):
        public = {key: row[key] for key in (
            "method", "body_sha256", "body_bytes", "body_hex_omitted_for_size", "record_index",
            "forwarded", "action_id", "response_dropped", "host_monotonic_us",
            "upstream_started_us", "upstream_http_status", "upstream_completed_us",
            "response_hold_ms", "response_hold_started_us", "response_hold_completed_us", "response_hold_released",
            "receipt_expired_mutation", "client_response_attempted_us", "client_response_completed_us",
            "handler_completed_us") if key in row}
        action_hex = row.get("action_hex")
        if isinstance(action_hex, str):
            public["action_hex_sha256"] = hashlib.sha256(action_hex.encode()).hexdigest()
            public["action_hex_bytes"] = len(action_hex)
            if len(action_hex) <= MAX_ACTION_HEX:
                public["action_hex"] = action_hex
            else:
                public["action_hex_omitted_for_size"] = True
        if full_body and isinstance(row.get("body_hex"), str):
            if len(row["body_hex"]) <= MAX_BODY_HEX:
                public["body_hex"] = row["body_hex"]
            else:
                public["body_hex_omitted_for_size"] = True
        return public

    def _snapshot(self):
        result = []
        for index, relay in enumerate(self.relays):
            snapshot = relay.snapshot()
            records = snapshot["requests"]
            recent = [self._public_request(row) for row in records[-MAX_RECENT_REQUESTS:]]
            result.append({"endpoint_index": index, "url": snapshot["url"],
                           "unavailable": snapshot["unavailable"],
                           "response_hold_ms": snapshot.get("response_hold_ms", {}),
                           "receipt_expired_action_id": snapshot.get("receipt_expired_action_id"),
                           "record_count": len(records), "records_dropped": snapshot["records_dropped"],
                           "recent_requests": recent, "older_requests_omitted": max(0, len(records) - len(recent))})
        return {"endpoints": result}

    def _request_page(self, value):
        snapshot = self.relays[value["endpoint_index"]].snapshot()
        records = snapshot["requests"]
        offset = value["offset"]
        end = value.get("end_offset", len(records))
        if len(records) > MAX_RETAINED_REQUESTS or not offset <= end <= len(records):
            raise ValueError("Request page must be within a retained snapshot")
        page, used = [], 0
        for index in range(offset, min(end, offset + value["limit"])):
            row = self._public_request(records[index], full_body=True)
            size = len(json.dumps(row, separators=(",", ":")).encode()) + 1
            if used + size > MAX_RESULT_BYTES - 8192:
                if not page:
                    raise ValueError("A public request record exceeds the page bound")
                break
            page.append(row)
            used += size
        next_offset = offset + len(page)
        return {"endpoint_index": value["endpoint_index"], "url": snapshot["url"],
                "offset": offset, "next_offset": next_offset, "end_offset": end,
                "complete": next_offset == end, "record_count": len(records),
                "records_dropped": snapshot["records_dropped"], "record_bytes": snapshot.get("record_bytes"),
                "snapshot_monotonic_us": time.monotonic_ns() // 1000,
                "requests": page, "records_added_after_end": len(records) - end}

    def _execute(self, value):
        command = value["command"]
        if command == "endpoint_fault":
            options = {"unavailable": value.get("unavailable", False), "drop_submit_once": value.get("drop_submit_once", False)}
            for field in ("response_hold_ms", "receipt_expired_action_id"):
                if field in value:
                    options[field] = value[field]
            self.relays[value["endpoint_index"]].configure(**options)
            return {"endpoint_index": value["endpoint_index"], **options}
        if command == "clear_faults":
            for relay in self.relays:
                relay.configure()
            return {"cleared_endpoints": len(self.relays)}
        if command == "snapshot":
            return self._snapshot()
        if command == "request_page":
            return self._request_page(value)
        hashes = self.mine_blocks(value["count"])
        if (not isinstance(hashes, list) or len(hashes) != value["count"] or
                any(not isinstance(item, str) or re.fullmatch(r"[0-9a-f]{64}", item) is None for item in hashes)):
            raise ValueError("Fixture mining callback did not return the requested public block hashes")
        return {"blocks_mined": len(hashes), "block_hashes": hashes}

    def poll_once(self):
        if self.disabled:
            return
        try:
            data = self._read_command()
        except (OSError, ValueError) as error:
            # A persistent invalid path is recorded once, not on every poll.
            data = ("invalid-command-file:" + type(error).__name__).encode()
        if data is None:
            return
        digest = hashlib.sha256(data).hexdigest()
        if digest == self.last_input:
            return
        self.last_input = digest
        if self.commands_seen >= MAX_COMMANDS or self.log_bytes + MAX_RESULT_BYTES + 8192 > MAX_LOG_BYTES:
            self.disabled = True
            self._publish({"state": "disabled", "reason": "attended control budget exhausted",
                           "last_id": self.last_id, "commands_seen": self.commands_seen})
            return
        self.commands_seen += 1
        event = {"command_sha256": digest, "unix_time_ns": time.time_ns(),
                 "host_monotonic_ns": time.monotonic_ns(), "commands_seen": self.commands_seen}
        try:
            value = self._parse(data)
            event["id"] = value["id"]
            if value["id"] <= self.last_id:
                raise ValueError("Command ID has already been consumed or is older")
            self.last_id = value["id"]
            self._validate(value)
        except (ValueError, TypeError, RecursionError) as error:
            event.update(state="rejected", last_id=self.last_id,
                         reason=str(error)[:160] if isinstance(error, ValueError) else "Invalid command shape")
            self._record(event)
            return
        event.update(state="started", last_id=self.last_id, command=value)
        self._record(event)  # Durable consumed ID before any fixture side effect.
        try:
            result = self._execute(value)
            completed = dict(event, state="completed", result=result,
                             completed_unix_time_ns=time.time_ns(), completed_monotonic_ns=time.monotonic_ns())
            if len(json.dumps(completed, separators=(",", ":")).encode()) + 1 > MAX_RESULT_BYTES:
                raise ValueError("Fixture result exceeds the evidence bound")
        except Exception as error:
            # Never copy callback exception text, RPC credentials or private
            # return data into evidence. The consumed command must not replay.
            completed = dict(event, state="failed", error_type=type(error).__name__,
                             reason="Fixture operation failed; ID remains consumed, inspect the isolated fixture before any new command",
                             completed_unix_time_ns=time.time_ns(), completed_monotonic_ns=time.monotonic_ns())
        self._record(completed)

    def close(self):
        if self.log_fd is not None:
            os.close(self.log_fd)
            self.log_fd = None
        self.disabled = True
