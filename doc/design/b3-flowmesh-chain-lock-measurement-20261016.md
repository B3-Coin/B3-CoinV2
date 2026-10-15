# B3 chain lock timing diagnostics

Date: 2026-10-16. This default-off diagnostic stage makes selected B3 chain-lock
waits visible without formatting or logging inside the measured critical
sections. It changes neither chain validity nor FlowMesh voting rules.

## Measurement boundary

The two explicitly instrumented `ActivateBestChain` scopes are
`b3_chain_activation_lock` and `b3_chain_periodic_flush_lock`. The latter uses
`m_chainman.GetMutex()`, which is the same chain mutex. This is not coverage of
every acquisition in validation or finality.

Each scope records three raw monotonic timestamps: requested, acquired and
releasing. Acquisition is observed just after the real guard locks; releasing
is observed just before it unlocks. Their difference is an in-lock bracket,
not an exact kernel mutex-hold measurement. Request-to-acquired includes small
instrumentation/acquisition overhead. Neither duration distinguishes CPU work
from descheduling or nested waits.

Only numeric clock/scalar operations run inside the measured scopes. An outer
object emits after the real guard unwinds, including returns and exceptions.
JSON construction, capture locking and optional logging occur there. Emission
can be deferred beyond other work: use the raw timestamps, never the span's
emission timestamp as a substitute for hold duration. Captures must remain
active until measured work drains; boundary-crossing spans may be absent.

## Kernel and production boundaries

The header-only kernel observer depends on `chrono` and `cstdint`, not node
logging or JSON. `ChainstateManagerOpts` defaults to null callbacks. Node init
installs the static-lifetime, nonthrowing callbacks only on regtest; clocks and
emission stay disabled until the regtest-only timing capture is started.
Ordinary mainnet initialization does not install them.

The existing bounded capture still accounts serialized bytes and event count,
not total heap. Overflow invalidates complete attribution. Its allocation and
short capture mutex can affect measurements, so use an identical capture-off
control and do not merge results from different campaigns.

## Actual qualification

AppleClang 17 Release builds completed for the native test binary, daemon and
CLI in a clean scratch source with baseline trading code plus this diagnostic
delta. Eleven timing tests and 72 assertions passed, exit 0. They exercise
disabled/null callbacks, nested capture, count/byte overflow, deferred emission,
early return, exception unwind and callback-after-unlock behavior with a real
test mutex. The validation scopes were inspected separately; the unit tests do
not synthesize the entire `ActivateBestChain` path.

Separate read-only review accepted the diagnostic seam. The full experimental
standalone kernel target, Linux CI, thread sanitizer and power-loss behavior are
not qualified by these tests. One unrelated existing finality-test lock warning
and the existing duplicate-library linker warning appeared during the broader
scratch rebuild.

```sh
./build/bin/test_bitcoin --run_test=flowmesh_timing_tests \
  --report_level=short --log_level=error
```
