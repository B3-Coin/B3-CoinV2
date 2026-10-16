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
