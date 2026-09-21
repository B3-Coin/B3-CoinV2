# Milestone 3A adapter-development findings

These two defects occurred while writing the new TEST-only disk adapter. They
are not findings against baseline `eb73e12d8b042f3ccdfd1e0d96701460e07f20d5`.
This document records implementation debugging and bounded regression evidence;
it is not an independent algorithm/security audit and does not close the
separate review gate described in `PLAN.md`.

## Evidence provenance

The initial implementation patch, the repair patch, and the first test failure
excerpts are retained in this task's tool history. A standalone, untruncated
original test log was **not** saved. The original tool response was truncated,
but it retained the exact assertions quoted below, including the three
malformed-database lengths. No missing log content is reconstructed here.

To verify the observations, a fresh temporary directory received copies of
`fm_disk_store.py`, `fm_codec.py`, and `test_disk_store.py`. Only the two relevant
pre-repair branches were restored in that copy. This was a branch-level
regression replay against the current adapter snapshot, not a claim to have
recovered the complete original development checkout. In particular, later
overlay map-provenance checks stayed present and do not participate in the
plain-dictionary failure case. The final workspace implementation was never
reverted or patched for the replay.

The fixed replay snapshot had these SHA-256 values, both before reverting the
two branches and after restoring their fixes:

| File | SHA-256 |
| --- | --- |
| `fm_disk_store.py` | `539bcf48a21f87d57718c93dcf77cea6c7fc92643b1685a2c048fbbf9f8fd3f5` |
| `fm_codec.py` | `b68cdc2fb41a36853cd0b8f414826ba2857c7dc9b8f06f98baf04db3c907cd77` |
| `test_disk_store.py` | `592ae3ecf014fb7759639d8c76acdc0376e565130980cf41aef4dc55e9f4230e` |

These identify the replay snapshot, not later changes made elsewhere during
Milestone 3A. The replay environment reported Darwin, Python 3.14.6, and SQLite
3.53.3. All fixtures used generated synthetic state in temporary directories.
No real key, wallet, node, contract, credential, hostname, or user path appears
in the evidence below.

## 1. Stale plain-dictionary state was accepted

The failing test was
`test_disk_store.DiskStoreTests.test_stale_writer_state_cannot_replace_current_obligations`.

The store first committed a change from view 0 to view 1. A second call supplied
the original view-0 state as `old`, with a candidate differing from that old
state only in its `halt` metadata. The original plain-dictionary path derived
record deltas by comparing `candidate` with `old` and checked only touched
record rows. Because that second call appeared not to touch its record, it
never detected the stale record supplied by the caller.

The observed original assertion was:

```text
AssertionError: StorageError not raised
```

The exact relevant repair hunk was:

```diff
                 else:
                     _require(type(after) is dict and type(old[section]) is dict)
+                    # Plain dictionaries do not carry the overlay's original
+                    # map provenance. Verify their entire old section before
+                    # deriving a delta; otherwise a stale untouched row could
+                    # make persisted state differ from the returned candidate.
+                    expected_rows = {encode(key): encode(value) for key, value in old[section].items()}
+                    stored_rows = {key: value for (name, key), value in self._rows.items() if name == section}
+                    _require(expected_rows == stored_rows, "STALE_STATE")
                     written = {key: value for key, value in after.items()
                                if key not in old[section] or value != old[section][key]}
                     removed = set(old[section]) - set(after)
```

The isolated replay reproduced both the failed assertion and the concrete
state mismatch. Before the fix, the second commit returned successfully,
advanced the journal to generation 2, and persisted `STALE_CANDIDATE` in `halt`
while retaining the already committed view 1. That persisted state differed
from the caller's view-0 candidate. The probe printed:

```json
{"refusal": null, "head_generation": 2, "candidate_view": 0, "loaded_view": 1, "loaded_halt": "STALE_CANDIDATE", "candidate_equals_loaded": false}
```

After restoring the fix, the same call refused with `STALE_STATE`, retained
generation 1, and kept `halt` unchanged:

```json
{"refusal": "STALE_STATE", "head_generation": 1, "candidate_view": 0, "loaded_view": 1, "loaded_halt": "", "candidate_equals_loaded": false}
```

`candidate_equals_loaded` remains false after the fix because the stale
candidate was correctly rejected. This probe does not establish an observed
equivocation or signature loss; it establishes acceptance of an inconsistent
caller state before the repair. Normal overlay commits continue to serialize
touched rows. An overlay from an unrecognized old map additionally receives a
full old-section comparison before it can use that path.

## 2. Malformed SQLite files received the wrong refusal code

The failing test was
`test_disk_store.DiskStoreTests.test_malformed_and_truncated_database_refuse_even_if_initial_supplied`.

The generated store file was replaced, in separate subcases, with the literal
21-byte `not a SQLite database`, the first 100 bytes of a valid file, and the
first half of that valid file (26,624 bytes in both observed runs). The original
constructor grouped all SQLite exceptions with ordinary OS availability
errors. It refused to open the files, but reported `STORE_UNAVAILABLE` instead
of the expected `STORE_CORRUPT`.

The original output and the replay both showed this assertion for all three
lengths:

```text
AssertionError: 'STORE_UNAVAILABLE' != 'STORE_CORRUPT'
- STORE_UNAVAILABLE
+ STORE_CORRUPT
```

The exact constructor repair hunk was:

```diff
-        except (OSError, sqlite3.Error):
+        except sqlite3.Error as exc:
+            self.close()
+            code = getattr(exc, "sqlite_errorcode", 0) & 255
+            unavailable = (sqlite3.SQLITE_CANTOPEN, sqlite3.SQLITE_PERM,
+                           sqlite3.SQLITE_READONLY, sqlite3.SQLITE_IOERR,
+                           sqlite3.SQLITE_BUSY, sqlite3.SQLITE_LOCKED, sqlite3.SQLITE_FULL)
+            raise StorageError("STORE_UNAVAILABLE" if code in unavailable else "STORE_CORRUPT") from None
+        except (KeyError, TypeError, ValueError, RecursionError):
+            self.close()
+            raise StorageError("STORE_CORRUPT") from None
+        except OSError:
             self.close()
             raise StorageError("STORE_UNAVAILABLE") from None
```

The repair preserves specific SQLite availability/error classes as
`STORE_UNAVAILABLE`, classifies malformed database failures as `STORE_CORRUPT`,
and safely classifies structural decoding exceptions. It does not change the
transaction algorithm. The fixed test checks both normal reopening and an
attempt supplying `initial` again, and confirms that the damaged bytes are not
replaced. The pre-fix subcases failed at their first code assertion, so that
first failed run alone is not evidence for later assertions in each subcase.

## Replay outcomes and constructor cleanup

The temporary-copy command selected only the two tests above. With their
original branches restored, its actual exit code was 1:

```text
Ran 2 tests in 0.014s

FAILED (failures=4)
```

The four failures were one stale-state assertion and the three malformed-file
subcases. Restoring the repairs in the same temporary source copy produced
exit code 0:

```text
Ran 2 tests in 0.015s

OK
```

The two selected tests were also rerun against the workspace implementation,
with exit code 0 (`Ran 2 tests in 0.016s`, `OK`). A portable equivalent from the
repository root is:

```sh
PYTHONPATH=test/flowmesh_v2_storage:test/flowmesh_v2_agreement:test/flowmesh_v2_model \
python3 -m unittest -v \
  test_disk_store.DiskStoreTests.test_stale_writer_state_cannot_replace_current_obligations \
  test_disk_store.DiskStoreTests.test_malformed_and_truncated_database_refuse_even_if_initial_supplied
```

A separate temporary probe wrapped `os.open` and `sqlite3.connect` while
running the fixed malformed-file test. After the test and fixture cleanup,
all 24 captured descriptor observations raised `EBADF` on `fstat`, and all
seven captured SQLite connections refused queries as closed connections:

```json
{"test_successful": true, "captured_fd_opens": 24, "closed_fd_observations": 24, "sqlite_connections": 7, "closed_sqlite_connections": 7}
```

The descriptor count includes fixture/cleanup opens and may include reused
descriptor numbers; it is not a claim of 24 distinct store descriptors. The
constructor calls `close()` on classified failures. That call was already
present in the original combined SQLite/OS handler: no historical descriptor
leak is claimed by these two findings. The added structural exception handler
also closes before refusing. Closing releases handles without unlinking the
store or its lifetime ownership lock file.

## Maximum supported durability claim

The implementation relies on SQLite DELETE rollback journaling and
`synchronous=FULL`, plus an explicit database-file/directory `fsync` barrier.
Its injected barrier failures do not exercise SQLite's internal VFS flushes
or simulate device write-cache behavior. A failed or unacknowledged write
poisons the running adapter; reopening the same store reconciles the committed
state before the replica may resume.

The milestone's process-kill and injected-I/O tests support only bounded
recovery on the tested local filesystem. They do not establish machine power
loss survival, remote-filesystem guarantees, hostile-owner resistance, real
signer custody, or multi-machine ownership. Lifetime advisory locking applies
to cooperating local processes. `O_NOFOLLOW` and file checks are practical
protections, not a hostile filesystem sandbox.

The journal checksums and replay/materialization comparison do not detect a
coherently restored older backup by themselves. Detecting a previously
observed rollback requires an external trusted generation/digest observation;
without it, that rollback remains an explicit gap. Storage bounds refuse new
writes instead of authorizing obligation pruning. None of these storage
results supplies the missing independent review of the pinned agreement
baseline or authorizes a later milestone.
