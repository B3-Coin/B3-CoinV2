"""Local POSIX SQLite durability adapter for generated TEST replicas only.

SQLite's DELETE journal and FULL synchronization provide transaction atomicity.
Our extra database/directory fsync barrier is independently fault injectable;
it is not a SQLite VFS or a simulation of device power-loss behavior. A failed
write poisons this instance even if SQLite may already have committed. Reopen
the same store to reconcile that uncertain outcome. No error erases a store.

The checksummed change chain detects accidental alteration and disagreement
with materialized rows. Only an external trusted head detects a coherent old
backup. Lifetime flock ownership is limited to cooperating local processes.
"""
import fcntl
import hashlib
import os
from pathlib import Path
import sqlite3
import stat
from urllib.parse import quote

from fm_codec import CodecError, decode, encode


STORE_FILE = "store.sqlite3"
LOCK_FILE = "store.lock"
SCHEMA_VERSION = 1
MAX_JOURNAL_ENTRIES = 100000
MAX_JOURNAL_BYTES = 256 * 1024 * 1024
MAX_DATABASE_BYTES = 512 * 1024 * 1024
MAX_ROWS = 20000
MAX_ROW_BYTES = 8 * 1024 * 1024
MAX_KEY_BYTES = 4096
MAX_IDENTITY_BYTES = 65536
MAX_REASON_BYTES = 256
STAGES = ("before_begin", "after_snapshot_row", "after_rows", "before_commit", "after_commit",
          "before_fsync", "after_fsync")
ZERO_DIGEST = "0" * 64
SECTIONS = frozenset(("metadata", "records", "retained_bodies"))
META_FIELDS = frozenset(("sequence", "snapshot", "anchor", "parent", "fenced", "halt"))
RECORD_FIELDS = frozenset(("instance", "view", "mode", "accepted", "new_views",
                           "prepared", "highest", "intents", "intent_bodies",
                           "signed", "decision", "body", "applied", "apply_count",
                           "before", "anchor_before"))
PHASES = frozenset(("PROPOSE", "PREPARE", "COMMIT", "VIEW_CHANGE", "NEW_VIEW"))


class StorageError(RuntimeError):
    """Safe refusal classification: never includes paths, rows or SQL errors."""
    def __init__(self, code):
        self.code = self.reason = code
        super().__init__(code)


def _require(ok, code="STORE_CORRUPT"):
    if not ok:
        raise StorageError(code)


def _integer(value, maximum=2 ** 63 - 1):
    return type(value) is int and 0 <= value <= maximum


def _digest(value):
    return (type(value) is str and len(value) == 64 and
            all(char in "0123456789abcdef" for char in value))


def _head(value):
    return (type(value) is dict and set(value) == {"generation", "digest"} and
            _integer(value["generation"]) and _digest(value["digest"]))


def _record(key, rec):
    """Required persisted shape; protocol authentication stays in the replica."""
    _require(_integer(key) and type(rec) is dict and set(rec) == RECORD_FIELDS)
    instance = rec["instance"]
    _require(type(instance) is dict and
             set(instance) == {"profile", "domain", "config", "epoch", "set", "sequence", "parent"})
    _require(type(instance["sequence"]) is int and instance["sequence"] == key and
             _integer(instance["epoch"]) and all(type(instance[x]) is str for x in
             ("profile", "domain", "config", "set", "parent")))
    _require(_integer(rec["view"]) and rec["mode"] in ("ACTIVE", "CHANGING", "DECIDED") and
             type(rec["applied"]) is bool and type(rec["apply_count"]) is int and
             rec["apply_count"] == int(rec["applied"]) and
             type(rec["before"]) is bytes and type(rec["anchor_before"]) is dict)
    for field in ("accepted", "new_views", "prepared", "intents", "intent_bodies", "signed"):
        _require(type(rec[field]) is dict)
    _require(rec["highest"] is None or type(rec["highest"]) is dict)
    _require(rec["decision"] is None or type(rec["decision"]) is dict)
    _require(rec["body"] is None or type(rec["body"]) is dict)
    _require((rec["decision"] is None) == (rec["body"] is None))
    _require(not rec["applied"] or rec["decision"] is not None)
    _require(rec["mode"] != "DECIDED" or rec["decision"] is not None)
    for slot, intent in rec["intents"].items():
        _require(type(slot) is tuple and len(slot) == 2 and slot[0] in PHASES and _integer(slot[1]))
        _require(type(intent) is dict and intent.get("phase") == slot[0] and
                 intent.get("view") == slot[1] and intent.get("instance") == instance)
        if intent.get("value") is not None:
            _require(intent["value"] in rec["intent_bodies"])
    for slot, signed in rec["signed"].items():
        _require(slot in rec["intents"] and type(signed) is dict and
                 set(signed) == {"payload", "auth"} and
                 signed["payload"] == rec["intents"][slot] and type(signed["auth"]) is str)
    for view, accepted in rec["accepted"].items():
        _require(_integer(view) and type(accepted) is dict and set(accepted) == {"signed", "body"} and
                 type(accepted["signed"]) is dict and type(accepted["body"]) is dict)
    for view, nv in rec["new_views"].items():
        _require(_integer(view) and type(nv) is dict)
    for slot, prepared in rec["prepared"].items():
        _require(type(slot) is tuple and len(slot) == 2 and _integer(slot[0]) and
                 type(slot[1]) is str and type(prepared) is dict)
    for value, body in rec["intent_bodies"].items():
        _require(type(value) is str and type(body) is dict)


class DiskStore:
    def __init__(self, directory, identity, *, initial=None, trusted_head=None, fault=None):
        self.directory = Path(os.path.abspath(os.fspath(directory)))
        self.path = self.directory / STORE_FILE
        self.fault = fault
        self._connection = None
        self._lock_fd = self._dir_fd = self._db_fd = None
        self._closed = False
        self._poisoned = False
        self._head = None
        self._rows = {}
        self._bound_maps = {}
        self._journal_bytes = 0
        try:
            _require(type(identity) is dict and bool(identity), "IDENTITY_MISMATCH")
            self._identity = encode(identity)
            _require(len(self._identity) <= MAX_IDENTITY_BYTES, "STORAGE_BOUND")
            self._identity_digest = hashlib.sha256(self._identity).digest()
            _require(trusted_head is None or _head(trusted_head), "ROLLBACK_DETECTED")
            self._open(initial is not None)
            if self._fresh:
                _require(trusted_head is None, "ROLLBACK_DETECTED")
                self._initialize(initial)
            else:
                self._verify(trusted_head)
        except StorageError:
            self.close()
            raise
        except CodecError as exc:
            self.close()
            raise StorageError("STORAGE_BOUND" if exc.code == "CODEC_BOUND" else "STORE_CORRUPT") from None
        except sqlite3.Error as exc:
            self.close()
            code = getattr(exc, "sqlite_errorcode", 0) & 255
            unavailable = (sqlite3.SQLITE_CANTOPEN, sqlite3.SQLITE_PERM,
                           sqlite3.SQLITE_READONLY, sqlite3.SQLITE_IOERR,
                           sqlite3.SQLITE_BUSY, sqlite3.SQLITE_LOCKED, sqlite3.SQLITE_FULL)
            raise StorageError("STORE_UNAVAILABLE" if code in unavailable else "STORE_CORRUPT") from None
        except (KeyError, TypeError, ValueError, RecursionError):
            self.close()
            raise StorageError("STORE_CORRUPT") from None
        except OSError:
            self.close()
            raise StorageError("STORE_UNAVAILABLE") from None

    def _regular(self, name, *, missing=False):
        try:
            result = os.stat(name, dir_fd=self._dir_fd, follow_symlinks=False)
        except FileNotFoundError:
            if missing:
                return None
            raise StorageError("STORE_MISSING") from None
        _require(stat.S_ISREG(result.st_mode) and result.st_nlink == 1, "STORE_UNAVAILABLE")
        _require(result.st_size <= MAX_DATABASE_BYTES, "STORAGE_BOUND")
        return result

    def _open(self, provision):
        if not self.directory.exists():
            _require(provision, "STORE_MISSING")
            self.directory.mkdir(mode=0o700, parents=True)
        info = self.directory.lstat()
        _require(stat.S_ISDIR(info.st_mode), "STORE_UNAVAILABLE")
        self._dir_fd = os.open(self.directory, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
        opened = os.fstat(self._dir_fd)
        _require((info.st_dev, info.st_ino) == (opened.st_dev, opened.st_ino), "STORE_UNAVAILABLE")
        found = self._regular(STORE_FILE, missing=True)
        _require(found is not None or provision, "STORE_MISSING")
        self._lock_fd = os.open(LOCK_FILE, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC,
                                0o600, dir_fd=self._dir_fd)
        lock = os.fstat(self._lock_fd)
        _require(stat.S_ISREG(lock.st_mode) and lock.st_nlink == 1, "STORE_UNAVAILABLE")
        try:
            fcntl.flock(self._lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise StorageError("STORE_LOCKED") from None
        # Check again only after ownership. Even an incomplete old database is
        # never treated as a fresh directory or reset by explicit provisioning.
        found = self._regular(STORE_FILE, missing=True)
        self._fresh = found is None
        _require(not self._fresh or provision, "STORE_MISSING")
        for sidecar in (STORE_FILE + "-journal", STORE_FILE + "-wal", STORE_FILE + "-shm"):
            side_info = self._regular(sidecar, missing=True)
            _require(not self._fresh or side_info is None, "STORE_CORRUPT")
        flags = os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC
        if self._fresh:
            flags |= os.O_CREAT | os.O_EXCL
        self._db_fd = os.open(STORE_FILE, flags, 0o600, dir_fd=self._dir_fd)
        actual = os.fstat(self._db_fd)
        if found is not None:
            _require((found.st_dev, found.st_ino) == (actual.st_dev, actual.st_ino), "STORE_UNAVAILABLE")
        self._db_identity = (actual.st_dev, actual.st_ino)
        uri = "file:" + quote(str(self.path), safe="/") + "?mode=rw"
        self._connection = sqlite3.connect(uri, uri=True, isolation_level=None, timeout=0)
        _require(self._connection.execute("PRAGMA journal_mode=DELETE").fetchone() == ("delete",),
                 "STORE_UNAVAILABLE")
        self._connection.execute("PRAGMA synchronous=FULL")
        self._connection.execute("PRAGMA trusted_schema=OFF")
        self._connection.execute("PRAGMA foreign_keys=ON")
        if hasattr(self._connection, "setlimit"):
            self._connection.setlimit(sqlite3.SQLITE_LIMIT_LENGTH, 20 * 1024 * 1024)
            self._connection.setlimit(sqlite3.SQLITE_LIMIT_SQL_LENGTH, 65536)
        page_size = self._connection.execute("PRAGMA page_size").fetchone()[0]
        self._connection.execute("PRAGMA max_page_count=" + str(MAX_DATABASE_BYTES // page_size))
        self._check_file()

    def _check_file(self):
        info = self._regular(STORE_FILE)
        _require((info.st_dev, info.st_ino) == self._db_identity, "STORE_UNAVAILABLE")
        for suffix in ("-journal", "-wal", "-shm"):
            self._regular(STORE_FILE + suffix, missing=True)

    def _ready(self):
        _require(not self._closed, "STORE_CLOSED")
        _require(not self._poisoned, "STORE_POISONED")

    @property
    def head(self):
        self._ready()
        return dict(self._head)

    def _hash(self, generation, previous, payload):
        return hashlib.sha256(b"FLOWMESH/TEST/DISK/1\0" + self._identity_digest +
                              generation.to_bytes(8, "big") + bytes.fromhex(previous) + payload).hexdigest()

    def _state(self, rows):
        result = {"records": {}, "retained_bodies": {}}
        for (section, key), value in rows.items():
            decoded_key, decoded_value = decode(key), decode(value)
            if section == "metadata":
                _require(type(decoded_key) is str and decoded_key in META_FIELDS)
                result[decoded_key] = decoded_value
            else:
                _require(section in SECTIONS)
                _require(decoded_key not in result[section])
                result[section][decoded_key] = decoded_value
        self._validate(result)
        return result

    def _validate(self, state, *, changed_records=None):
        _require(type(state) is dict and set(state) == META_FIELDS | {"records", "retained_bodies"})
        _require(_integer(state["sequence"]) and type(state["snapshot"]) is bytes and
                 type(state["anchor"]) is dict and type(state["parent"]) is str and
                 type(state["fenced"]) is bool and type(state["halt"]) is str)
        records, bodies = state["records"], state["retained_bodies"]
        _require(hasattr(records, "items") and hasattr(bodies, "items"))
        _require(len(records) + len(bodies) + len(META_FIELDS) <= MAX_ROWS, "STORAGE_BOUND")
        seq = state["sequence"]
        _require(seq <= MAX_ROWS)
        # History is contiguous and cannot be silently pruned. Exhaustion may
        # leave the last applied record as the final one, with no current slot.
        _require(len(records) == seq + (seq in records))
        _require(seq in records or bool(state["halt"]))
        if changed_records is None:
            for index in range(seq):
                _require(index in records and records[index]["applied"])
            validate_records = records.items()
        else:
            validate_records = changed_records.items()
        for index, rec in validate_records:
            _record(index, rec)
            _require(index <= seq and (index == seq or rec["applied"]))
            identity = decode(self._identity)
            if "config" in identity:
                _require(rec["instance"]["config"] == identity["config"], "IDENTITY_MISMATCH")
            if index == 0 and "genesis" in identity:
                _require(rec["instance"]["parent"] == identity["genesis"], "IDENTITY_MISMATCH")
        if changed_records is None:
            for value, body in bodies.items():
                _require(type(value) is str and type(body) is dict)

    def _row(self, section, key, value):
        key, value = encode(key), encode(value)
        _require(len(key) <= MAX_KEY_BYTES and len(value) <= MAX_ROW_BYTES, "STORAGE_BOUND")
        return section, key, value

    def _initialize(self, initial):
        self._validate(initial)
        changes = [self._row("metadata", key, initial[key]) for key in sorted(META_FIELDS)]
        for section in ("records", "retained_bodies"):
            changes.extend(self._row(section, key, value) for key, value in initial[section].items())
        self._transact(changes, [], "genesis", genesis=True)
        self._bound_maps = {section: initial[section] for section in ("records", "retained_bodies")}

    def _fault(self, stage, reason):
        if self.fault is not None:
            self.fault(stage, reason)

    def _transact(self, changes, deleted, reason, *, genesis=False):
        generation = 0 if genesis else self._head["generation"] + 1
        previous = ZERO_DIGEST if genesis else self._head["digest"]
        changes.sort(key=lambda item: (item[0], item[1]))
        deleted.sort(key=lambda item: (item[0], item[1]))
        payload = encode({"version": SCHEMA_VERSION, "identity": self._identity if genesis else None,
                          "reason": reason, "set": [list(item) for item in changes],
                          "delete": [list(item) for item in deleted]})
        _require(generation < MAX_JOURNAL_ENTRIES and
                 self._journal_bytes + len(payload) <= MAX_JOURNAL_BYTES, "STORAGE_BOUND")
        count = len(self._rows) + sum((section, key) not in self._rows for section, key, _ in changes)
        count -= sum((section, key) in self._rows for section, key in deleted)
        _require(count <= MAX_ROWS, "STORAGE_BOUND")
        digest = self._hash(generation, previous, payload)
        head = {"generation": generation, "digest": digest}
        connection = self._connection
        try:
            self._check_file()
            self._fault("before_begin", reason)
            connection.execute("BEGIN IMMEDIATE")
            if genesis:
                connection.execute("CREATE TABLE state(section TEXT NOT NULL,key BLOB NOT NULL,value BLOB NOT NULL,PRIMARY KEY(section,key)) WITHOUT ROWID")
                connection.execute("CREATE TABLE journal(generation INTEGER PRIMARY KEY,previous_digest TEXT NOT NULL,digest TEXT NOT NULL,payload BLOB NOT NULL)")
                connection.execute("CREATE TABLE meta(name TEXT PRIMARY KEY,value BLOB NOT NULL) WITHOUT ROWID")
                connection.executemany("INSERT INTO meta VALUES (?,?)", (
                    ("version", encode(SCHEMA_VERSION)), ("identity", self._identity)))
            else:
                persisted = connection.execute("SELECT value FROM meta WHERE name='head'").fetchone()
                _require(persisted is not None and decode(persisted[0]) == self._head, "STALE_STATE")
            connection.executemany("DELETE FROM state WHERE section=? AND key=?", deleted)
            for row in changes:
                connection.execute("INSERT OR REPLACE INTO state VALUES (?,?,?)", row)
                if row[0] == "metadata" and row[1] == encode("snapshot"):
                    # Test a real transaction with the new snapshot row but
                    # before the later records/applied-marker row is written.
                    self._fault("after_snapshot_row", reason)
            self._fault("after_rows", reason)
            connection.execute("INSERT INTO journal VALUES (?,?,?,?)", (generation, previous, digest, payload))
            connection.execute("INSERT OR REPLACE INTO meta VALUES ('head',?)", (encode(head),))
            self._fault("before_commit", reason)
            connection.execute("COMMIT")
            self._fault("after_commit", reason)
            self._fault("before_fsync", reason)
            self._check_file()
            os.fsync(self._db_fd)
            os.fsync(self._dir_fd)
            self._fault("after_fsync", reason)
        except (OSError, sqlite3.Error, CodecError, StorageError) as exc:
            self._poisoned = True
            if connection.in_transaction:
                try:
                    connection.execute("ROLLBACK")
                except sqlite3.Error:
                    pass
            if isinstance(exc, StorageError):
                raise
            raise StorageError("STORAGE_IO") from None
        self._head = head
        self._journal_bytes += len(payload)
        for section, key in deleted:
            self._rows.pop((section, key), None)
        for section, key, value in changes:
            self._rows[section, key] = value

    def commit(self, old, candidate, reason):
        """Persist touched overlay rows before update_durable publishes memory."""
        self._ready()
        try:
            _require(type(reason) is str and len(reason.encode("utf-8")) <= MAX_REASON_BYTES,
                     "STORAGE_BOUND")
            _require(type(old) is dict and type(candidate) is dict, "STORE_CORRUPT")
            changes, deleted = [], []
            bound_maps = {}
            for key in META_FIELDS:
                old_row = self._row("metadata", key, old[key])
                _require(self._rows.get(old_row[:2]) == old_row[2], "STALE_STATE")
                if candidate[key] != old[key]:
                    changes.append(self._row("metadata", key, candidate[key]))
            touched_records = None
            for section in ("records", "retained_bodies"):
                after = candidate[section]
                if hasattr(after, "changed") and hasattr(after, "deleted"):
                    _require(after.original is old[section], "STALE_STATE")
                    if after.original is not self._bound_maps.get(section):
                        expected_rows = {encode(key): encode(value) for key, value in old[section].items()}
                        stored_rows = {key: value for (name, key), value in self._rows.items() if name == section}
                        _require(expected_rows == stored_rows, "STALE_STATE")
                    written, removed = after.changed, after.deleted
                    bound_maps[section] = after.original
                else:
                    _require(type(after) is dict and type(old[section]) is dict)
                    # Plain dictionaries do not carry the overlay's original
                    # map provenance. Verify their entire old section before
                    # deriving a delta; otherwise a stale untouched row could
                    # make persisted state differ from the returned candidate.
                    expected_rows = {encode(key): encode(value) for key, value in old[section].items()}
                    stored_rows = {key: value for (name, key), value in self._rows.items() if name == section}
                    _require(expected_rows == stored_rows, "STALE_STATE")
                    written = {key: value for key, value in after.items()
                               if key not in old[section] or value != old[section][key]}
                    removed = set(old[section]) - set(after)
                    bound_maps[section] = after
                if section == "records":
                    touched_records = written
                    _require(not removed)  # Never prune signing obligations.
                for key in set(written) | set(removed):
                    encoded_key = encode(key)
                    original = self._rows.get((section, encoded_key))
                    expected = encode(old[section][key]) if key in old[section] else None
                    _require(original == expected, "STALE_STATE")
                for key, value in written.items():
                    if section == "retained_bodies":
                        _require(type(key) is str and type(value) is dict)
                    changes.append(self._row(section, key, value))
                deleted.extend((section, encode(key)) for key in removed)
            self._validate(candidate, changed_records=touched_records)
            self._transact(changes, deleted, reason)
            self._bound_maps = bound_maps
        except (CodecError, KeyError, TypeError, ValueError, RecursionError) as exc:
            code = "STORAGE_BOUND" if isinstance(exc, CodecError) and exc.code == "CODEC_BOUND" else "STORE_CORRUPT"
            raise StorageError(code) from None

    def _verify(self, trusted_head=None):
        self._check_file()
        connection = self._connection
        _require(connection.execute("PRAGMA quick_check(1)").fetchall() == [("ok",)])
        schema = connection.execute("SELECT type,name FROM sqlite_master WHERE name NOT LIKE 'sqlite_%'").fetchall()
        _require(set(schema) == {("table", "state"), ("table", "journal"), ("table", "meta")})
        meta = connection.execute("SELECT name,value FROM meta LIMIT 4").fetchall()
        _require(len(meta) == 3 and {item[0] for item in meta} == {"version", "identity", "head"})
        meta = dict(meta)
        version = decode(meta["version"])
        _require(type(version) is int and version == SCHEMA_VERSION)
        _require(meta["identity"] == self._identity, "IDENTITY_MISMATCH")
        stored_head = decode(meta["head"])
        _require(_head(stored_head))
        count, total_bytes = connection.execute("SELECT count(*),coalesce(sum(length(payload)),0) FROM journal").fetchone()
        _require(0 < count <= MAX_JOURNAL_ENTRIES and total_bytes <= MAX_JOURNAL_BYTES, "STORAGE_BOUND")
        rows, previous, used = {}, ZERO_DIGEST, 0
        trusted_seen = trusted_head is None
        for expected, (generation, prev, digest, payload) in enumerate(connection.execute(
                "SELECT generation,previous_digest,digest,payload FROM journal ORDER BY generation")):
            _require(type(generation) is int and generation == expected and prev == previous and
                     _digest(digest) and type(payload) is bytes)
            _require(digest == self._hash(generation, previous, payload))
            entry = decode(payload)
            _require(type(entry) is dict and set(entry) == {"version", "identity", "reason", "set", "delete"} and
                     type(entry["version"]) is int and entry["version"] == SCHEMA_VERSION and
                     entry["identity"] == (self._identity if generation == 0 else None) and
                     type(entry["reason"]) is str and len(entry["reason"].encode("utf-8")) <= MAX_REASON_BYTES and
                     type(entry["set"]) is list and type(entry["delete"]) is list)
            _require(len(entry["set"]) + len(entry["delete"]) <= MAX_ROWS, "STORAGE_BOUND")
            seen = set()
            for item in entry["delete"]:
                _require(type(item) is list and len(item) == 2)
                section, key = item
                _require(section in SECTIONS and type(key) is bytes and len(key) <= MAX_KEY_BYTES)
                location = (section, key)
                _require(location not in seen and location in rows)
                seen.add(location)
                del rows[location]
            for item in entry["set"]:
                _require(type(item) is list and len(item) == 3)
                section, key, value = item
                _require(section in SECTIONS and type(key) is bytes and type(value) is bytes and
                         len(key) <= MAX_KEY_BYTES and len(value) <= MAX_ROW_BYTES)
                location = (section, key)
                _require(location not in seen)
                seen.add(location)
                rows[location] = value
            _require(len(rows) <= MAX_ROWS, "STORAGE_BOUND")
            previous = digest
            used += len(payload)
            if trusted_head is not None and generation == trusted_head["generation"]:
                _require(digest == trusted_head["digest"], "ROLLBACK_DETECTED")
                trusted_seen = True
        _require(stored_head == {"generation": count - 1, "digest": previous})
        _require(trusted_seen, "ROLLBACK_DETECTED")
        actual = {}
        for section, key, value in connection.execute("SELECT section,key,value FROM state LIMIT ?", (MAX_ROWS + 1,)):
            _require(section in SECTIONS and type(key) is bytes and type(value) is bytes and
                     len(key) <= MAX_KEY_BYTES and len(value) <= MAX_ROW_BYTES)
            _require((section, key) not in actual)
            actual[section, key] = value
        _require(len(actual) <= MAX_ROWS, "STORAGE_BOUND")
        _require(actual == rows)
        state = self._state(rows)
        self._rows, self._head, self._journal_bytes = rows, stored_head, used
        self._bound_maps = {section: state[section] for section in ("records", "retained_bodies")}
        return state

    def load(self):
        """Verify durable bytes again, returning entirely independent objects."""
        self._ready()
        try:
            return self._verify(self._head)
        except StorageError:
            self._poisoned = True
            raise
        except (OSError, sqlite3.Error, CodecError, KeyError, TypeError, ValueError):
            self._poisoned = True
            raise StorageError("STORE_CORRUPT") from None

    def close(self):
        """Release ownership without unlinking the stable lock inode or store."""
        self._closed = True
        if self._connection is not None:
            try:
                self._connection.close()
            except sqlite3.Error:
                pass
            self._connection = None
        for attr in ("_db_fd", "_lock_fd", "_dir_fd"):
            descriptor = getattr(self, attr, None)
            if descriptor is not None:
                try:
                    os.close(descriptor)
                except OSError:
                    pass
                setattr(self, attr, None)

    def __enter__(self):
        self._ready()
        return self

    def __exit__(self, *_):
        self.close()
