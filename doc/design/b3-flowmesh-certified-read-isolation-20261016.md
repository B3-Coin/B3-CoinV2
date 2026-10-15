# FlowMesh certified read isolation

Date: 2026-10-16. Status: local native milestone, separately reviewed within its
declared scope. Parent: `4876f0c`. This stage separates certified reads from the
trading writer. It does not complete the V2 redesign or establish 200 ms trading.

## Ownership and publication

The single writer retains the existing execution, agreement, signing and
synchronous store rules. Its committed state is shared as immutable data, not
copied wholesale for each reader. Publication follows this order:

1. Complete the existing durable certified-entry append and application.
2. Install matching state, certified head, recent evidence and bounded history.
3. Publish one immutable read view.
4. Append the corresponding client events and wake observers.

All configured markets finish startup recovery validation before any initial
view or restored event is exposed. A failed startup does not publish a partially
validated collection of markets. Old view ownership is retired outside the
writer lock, including initialization and idempotent `Start` calls.

Current/recent certificate reads, state/account projections, books and frozen
history use the published view. They do not call the chain, key provider or
store, or acquire the market lock. A response captures one view, so a concurrent
commit cannot mix its state with another head. Book projection and encoding run
outside publication locks. Health and publication leaf locks use Bitcoin's
annotated `Mutex` with precise negative preconditions and guarded fields.
These annotations are not a claim that existing trading code cannot acquire
`cs_main`.

Internal historical evidence lookup retains its existing blocking fallback.
Public callers must explicitly request `allow_history=false`; older evidence
outside the retained window yields `history_busy`, not false nonexecution or
expiry. Public service wiring is a separate, unfinished integration stage.

## Reproduced failures and regressions

The new blocked-worker regression failed against clean baseline native code
at `d03f433`: exit 201, 33 of 34 assertions passed; the certified read waited
for the worker's market lock. The corrected implementation passes that case
without clearing an obligation or changing an authority predicate.

Separate review found a retirement race in the first draft. An idempotent
`Start` could drain a live worker's retired handles and cause large old-state
destruction under its market lock. The deterministic regression failed before
correction: exit 201, 34 of 35 assertions passed. Initialization now detaches
only its own handles before launching the worker and releases them after its
locks unwind. Idempotent `Start` never drains the live retirement list.

Regressions also cover actual held-`cs_main` health reads, immutable versions,
rejected append without false publication, view/event coherence, recent versus
explicit historical evidence, and reopening/replay of history. The runtime
restart fixture reuses an open store; it is not a power-loss test.

## Actual qualification

Clean scratch builds used AppleClang 17, Release, Unix Makefiles, C++20, wallet
and native tests enabled, GUI disabled, ordinary synchronous durability. The
unrelated native V2 prototype and its dirty CMake change were excluded.

- Targeted regressions: 7 tests, 333 assertions, exit 0.
- Complete affected suites: 122 runtime tests and 19 store tests, 187,513
  assertions, exit 0.
- Separate read-only review: accepted the final six source/test files, including
  retirement, startup publication and annotation corrections. Reviewer code
  inspection is distinct from the implementer's test execution.
- No source warnings in the final affected build; the existing duplicate-library
  linker warning remained.

Reproduce from a clean checkout and an optimized native test build:

```sh
./build/bin/test_bitcoin \
  --run_test=flowmesh_runtime_tests,flowmesh_production_store_tests \
  --report_level=short --log_level=error
```

## Bounds and next gates

Frozen history retains at most 256 entries with the existing 128-account fill
projection limit. Recent certificates/outcomes preserve existing count/byte
limits. These are serialized/structural bounds, not total heap measurements.
Publication copies bounded history and recent metadata; its overhead still
needs measurement. Startup buffering and unit-end publication scale with the
operator-configured market count. Post-durable allocation exceptions remain
fail-stop and require restart, not speculative continuation.

The public service candidate still has availability blockers: automatically
produced checkpoint advisory state, bounded operator queries and submission
permit starvation. It is not part of this accepted milestone. Neither this
stage nor its tests qualify lock-free chain facts, constant-time ancestry,
finality undo, full V2 accounting/futures, bridge compatibility, machine power
loss, WAN latency or mainnet deployment. Signing journals, quorum, economics,
wire validity and existing regtest/mainnet data are unchanged.
