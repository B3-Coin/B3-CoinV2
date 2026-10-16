# Clean-core FlowMesh foundations — C1

New C++ implementation beside V1, not a wrapper around the old runtime.
This first milestone is **infrastructure only**, disabled by default and not
linked into the node, wallet or Qt. It does not execute trades.

- `BoundedMailbox`: opaque FIFO, count and accounted-payload limits, rejection
  preserves input, close refuses new work while retaining queued originals.
- `ReadSlot`: deeply owned one-head opaque snapshots and move-only bounded
  reader leases; readers do not acquire publisher, chain or market locks.
- Actual Bitcoin `Mutex`/`LOCK` and compiler-enforced lock annotations.

Neither component authenticates an action, certifies a result, grants spending
authority or acknowledges disk durability. Read publication is memory visibility
only. Accounted payload length is not a total-heap/DoS bound; caller allocations,
capacity, concurrency and CPU budgets remain integration requirements.

See the [replacement roadmap](../../doc/design/b3-flowmesh-clean-core-20261016.md),
[versioned test profile](TEST_PROFILE.json) and [C1 results](C1_RESULTS.md).

## Reproduce

Use a clean committed checkout, not the unrelated dirty native spot prototype.
Qualified source: `1f05fc748c1c74ad7afff4e63ab21dc81bfb8c88`.
No node, wallet, network, keys or existing datadir are used.

```sh
cmake -S . -B build-flowmesh2-c1 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DBUILD_FLOWMESH2_FOUNDATION_TESTS=ON \
  -DBUILD_TESTS=ON -DBUILD_GUI=OFF -DBUILD_DAEMON=OFF -DBUILD_CLI=OFF \
  -DBUILD_TX=OFF -DBUILD_UTIL=OFF -DBUILD_WALLET_TOOL=OFF \
  -DENABLE_WALLET=OFF -DENABLE_IPC=OFF -DWITH_CCACHE=OFF
cmake --build build-flowmesh2-c1 --target test_flowmesh2_foundations --parallel 4
ctest --test-dir build-flowmesh2-c1 -R '^flowmesh2_foundations$' --no-tests=error --verbose
```

Expected: one CTest executable, **19 internal cases** (12 mailbox + 7 read-slot),
no skips. The executable rejects a changed case count and runs checks even under
`NDEBUG`. Each CTest execution has a 60-second deadline. Compilation uses
`-Wthread-safety -Werror=thread-safety` on supported Clang compilers.

This run qualifies neither full V1 nor full V2. The 200 ms target must later
include original preflight, actual crypto/agreement, durable execution and
durable client verification on isolated regtest. Test runtime is not trading
latency.

## Rebirth outline (branch `rebirth`)

The C1 text above is unchanged and C1 stays frozen. On branch `rebirth` the
whole new core is being laid out as a walking skeleton (slices R1 to R14) that
Codex then completes:

- [ARCHITECTURE.md](ARCHITECTURE.md): module layout, data flow, lock and
  determinism rules, durability barriers, authority versioning, what is real
  versus stubbed, how to build and test, and the walking-skeleton acceptance.
- [TODO.md](TODO.md): the task ledger. Every `// TODO(rebirth:<ID>)` tag in the
  source names a row there, with its status, acceptance test, evidence path and
  the provisional decisions D1 to D9.

Landed so far: **R1**, the `BUILD_FLOWMESH2` gate (default OFF; it requires
`BUILD_TESTS` and `BUILD_FLOWMESH2_FOUNDATION_TESTS`), the `[[nodiscard]]`
`Result`/`Error` model and code registries in `core/`, the Boost.Test binary
`test_flowmesh2` with its case inventory `test/INVENTORY.txt`, and the lint
scripts `contrib/flowmesh2/check_inventory.py` and `check_todo.py`; and
**R2**, the seven guards `contrib/flowmesh2/check_guards.py` with their
fixture self-test, the compile-fail controls in `test/compile_fail/`, the
evidence bundle tool `contrib/flowmesh2/evidence.py`, the sanitizer canaries
(`-DFLOWMESH2_SANITIZER_CANARIES=ON`, default OFF) and the owner-protected CI
workflow `.github/workflows/flowmesh2.yml`, which the outline never runs.
Nothing is linked into the node, wallet or Qt.

```sh
git -C /Users/josh/development/ON/B3-FlowMesh-hl branch --show-current   # must print: rebirth
cmake -S . -B build-rebirth -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTS=ON -DBUILD_FLOWMESH2_FOUNDATION_TESTS=ON -DBUILD_FLOWMESH2=ON \
  -DBUILD_GUI=OFF -DBUILD_DAEMON=OFF -DBUILD_CLI=OFF -DBUILD_TX=OFF -DBUILD_UTIL=OFF \
  -DBUILD_WALLET_TOOL=OFF -DENABLE_WALLET=OFF -DENABLE_IPC=OFF -DWITH_CCACHE=OFF
cmake --build build-rebirth --target fm2_all --parallel 4
ctest --test-dir build-rebirth -L flowmesh2 --no-tests=error --output-on-failure
```

Expected after R2: 8 CTests pass (`flowmesh2_foundations` with its 19 C1
cases, `flowmesh2_unit` with 16 cases, `flowmesh2_inventory`,
`flowmesh2_todo_lint`, `flowmesh2_guards`, `flowmesh2_guards_selftest`,
`flowmesh2_evidence_selftest` and, with Clang, `flowmesh2_negative_compile`).
Sanitizer builds and their canaries are described in ARCHITECTURE.md §13.2. Open gates (workspace lock reconciliation, decision
D3) are listed in ARCHITECTURE.md §0. All outline values are provisional and
REGTEST-only.
