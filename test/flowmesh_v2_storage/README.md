# FlowMesh V2 Milestone 3A — disk-backed TEST recovery

This isolated test adapter persists the existing synthetic agreement model.
It does not change accounting, agreement, authentication or economic rules,
and does not integrate storage into a live node. The prior contract is
[PLAN.md](PLAN.md); the versioned scope and bounds are
[flowmesh-v2-disk-recovery-test/3](TEST_PROFILE.json). The original `/1` and `/2`
remains in frozen reviewed history; the store schema and codec remain version 1.
The [R1/R2/R3 repair supplement](REPAIR_R1_R2_R3.md) records the successor
implementation, tests and external re-review gate. See also the existing
[agreement model](../flowmesh_v2_agreement/README.md),
[protocol](../flowmesh_v2_agreement/PROTOCOL.md) and
[accounting model](../flowmesh_v2_model/README.md).

R1/R2/R3 were subsequently accepted within their tested scope. The separate
[header-retention/aggregation successor](HEADER_REPAIR.md) retains those
repairs and adds bounded disposable headers and candidate-local aggregation.
Its external re-review remains pending; the earlier reports stay frozen.

## Run from the repository root

Python 3.14 and the standard library are sufficient:

```sh
python3.14 -B ci/run_flowmesh_models.py
```

The complete runner includes discovery guards and the accounting, agreement
and storage suites. For the storage suite alone:

```sh
python3.14 -B ci/run_flowmesh_models.py --suite storage
```

The complete runner fixes `PYTHONHASHSEED=0` and isolates suite imports.
Tests generate temporary stores and synthetic fixtures; they require no
wallet, key, running node, contract, external service or third-party package.
The implementation requires POSIX facilities available on macOS and Linux:
`flock`, directory file descriptors, `O_NOFOLLOW`, `fsync` and `SIGKILL`.
Qualification applies only to the tested local filesystem. Windows and
network filesystems are outside this profile.

## Components and APIs

- [fm_codec.py](fm_codec.py): `encode(value) -> bytes` and `decode(bytes)` use
  canonical versioned typed JSON. Integer/tuple map keys and bytes survive
  round trips; unsupported types, malformed encodings and bounds raise
  `CodecError`. No pickle is used for stored data.
- [fm_disk_store.py](fm_disk_store.py):
  `DiskStore(directory, identity, *, initial=None, trusted_head=None, fault=None)`
  owns `store.sqlite3` and holds an exclusive `store.lock` for its lifetime.
  `initial` explicitly permits fresh provisioning; reopening never replaces
  an existing invalid database. `load()` verifies and returns private state;
  `commit(old, candidate, reason)` persists changes; `head` returns
  `{generation, digest}`; `close()` releases resources without unlinking the
  lock file. `StorageError.code` gives an explicit refusal reason.
- [fm_disk_replica.py](fm_disk_replica.py): `DiskReplica` takes the ordinary
  `Replica` arguments plus `directory`, `create`, `trusted_head`, `fault` and
  `cut_hook`. Its identity binds signer index, population, configuration and
  genesis. The optional `update_durable(..., before_commit=callback)` hook
  commits before memory changes become visible. Recovery validates protected
  evidence using existing validators; missing volatile ancestry still follows
  normal GET/DATA. `close()` requires constructing a new adapter to reopen;
  `restart()` on an available adapter is a same-process volatile restart.
- [fm_process_harness.py](fm_process_harness.py):
  `ProcessSimulator(snapshot, directory, **kwargs)` spawns real child processes
  with separate connections and stores. `node.call("set_cut", point, phase)`
  arms a replica boundary; `node.call("set_fault", {"stage": ..., "reason": ...,
  "kind": "kill"})` arms a storage kill (`"error"` injects I/O failure).
  `node.restart()` spawns a replacement using the same directory and parent
  witness. The parent retains all synthetic issued signatures, including
  unpublished ones, and records actual exits in `STORAGE_PROCESS_RESULT`
  output. `DiskSimulator` uses the same disk adapter with same-process
  scheduling for bounded-load checks; those checks are not process-kill tests.

SQLite uses rollback journaling (`DELETE`) and `synchronous=FULL`. Materialized
rows and the checksummed change chain commit together. Storage hooks are
`before_begin`, `after_snapshot_row` (when written), `after_rows`,
`before_commit`, `after_commit`, `before_fsync` and `after_fsync`.
Write/flush failure makes the running adapter unavailable; reopening reconciles
the committed outcome. Explicit file/directory fsync fault injection does not
simulate SQLite's internal VFS or a device's write cache.

## Bounded evidence

The frozen baseline contained the following 55 storage test methods; the
repair supplement records added regressions and current complete counts.
Subcases and seeded schedules are not counted as additional test methods.

| File | Tests | Coverage |
| --- | ---: | --- |
| [test_disk_store.py](test_disk_store.py) | 34 | Codec, corruption, identity, bounds, rollback, SQLite/fsync errors, memory isolation and separate-process ownership |
| [test_process_recovery.py](test_process_recovery.py) | 14 | A–G kill/reopen boundaries, PREPARE/COMMIT publication cuts, exact signature replay, application atomicity, ancestry fetch, I/O refusal and seeds 31031/31032 |
| [test_storage_availability.py](test_storage_availability.py) | 5 | 257 unsolicited bodies, 256 remote offers, retries at 1/4/16/31 decisions, eight-decision catchup and protected-evidence refusal |
| [test_storage_profile.py](test_storage_profile.py) | 2 | Versioned assumptions and bounds match implementation |

Limits stop writes without pruning obligations: 100,000 journal entries,
256 MiB journal payload, 512 MiB database, 20,000 rows and 8 MiB per row.
The codec permits at most 16 MiB encoded data and depth 64; child calls have a
20-second deadline. [TEST_PROFILE.json](TEST_PROFILE.json) records these
limits; source constants impose additional key, scalar and container bounds.

A trusted generation/digest retained outside a child detects a rollback behind
that observation or a mismatching chain. The parent witness is a test-harness
assumption, not a production witness service. A coherently restored old backup
without that observation remains undetectable. Advisory ownership covers
cooperating processes on one local filesystem, not clones, hostile owners or
multiple machines sharing a real key.

## Qualification and review gate

Files, fsync calls, locks, process creation, SIGKILL and exit statuses are real.
Authentication, membership, custody, anchors, clock and network delivery remain
synthetic. These bounded tests do not establish machine power-loss durability,
device cache behavior, real BLS/B3 verification, changing membership, live V1
recovery, bridge validity, futures margin, WAN performance or 200 ms latency.

Historical publication recorded a pending DATA/OFFER/retry review at
`eb73e12d8b042f3ccdfd1e0d96701460e07f20d5`; use the
[commit-pinned review map](../../doc/design/v2-milestone-2-1-publication.md).
The subsequent review of `1d022db` found R1/R2/R3. The repair supplement
preserves its counterexamples and marks the completed correction for external
re-review. Local/hosted automated tests do not close that gate. Publication is
for review only; no next milestone or production deployment is approved here.
