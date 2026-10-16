# Overnight redesign: implemented foundations and remaining gates

Date: 2026-10-16. Branch: `flowmeshV2-dev`.
Starting revision: `46e5cf927b66dadc37ddfac30dce20a7847baf1b`.
Qualified selected-preflight implementation:
`c87deb34b736cc4557e57a7c89836febe99bff77`.
Its v9 runner and actual diagnostic reports follow at `35de6684` and `4433acb6`.
The [fair outcome-read successor](b3-flowmesh-outcome-fifo-20261016.md),
`b42705b021cc9286b9eb230cc16c70c1528e4eec`, records the latest frozen source
hashes and v10 qualification. Its strict CI controls follow at
`049d90f2cf44da5894227588c4f05991b19946d2`. These are the successor review
targets, not the old measured daemon.

## Outcome, without overstating completion

The local work repairs concrete V1 availability, exact-action recovery and
durability defects while building reusable C++20/Qt redesign foundations.
It does **not** complete the new V2 protocol or make the wallet release-ready.
No whole D0–D7 roadmap gate is declared complete. The stronger **200 ms
durable, client-verified trading-result target remains unmet and unqualified**.

Everything stayed local on the existing branch. No mainnet, VPS, existing
tester data, live wallet/key, external contract, push, PR, release or activation
operation occurred. Unrelated native spot prototype/CMake work was preserved,
not adopted as implemented V2. Frozen predecessor commits and failed traces
remain evidence; no history, signing obligation or journal was reset.

## What actually changed

| Local sub-stage | Concrete result | Boundary |
| --- | --- | --- |
| [Lock contracts](../../ci/check_flowmesh_lock_contracts.py) and [chain timing](b3-flowmesh-chain-lock-measurement-20261016.md) | Annotated publication/admission boundaries, compiler positive/negative controls, opt-in chain/client timing | Not all plain mutexes converted; exclusion annotations do not forbid acquisitions hidden in a function body |
| [Immutable certified runtime reads](b3-flowmesh-certified-read-isolation-20261016.md) | One coherent durably committed state/head, bounded recent evidence/history, safe view retirement outside the writer lock | Public service wrappers and historical fallbacks still have blocking dependencies |
| [Exact originals](b3-flowmesh-accepted-original-retention-20261016.md) and [publication recovery](b3-flowmesh-exact-publication-recovery-20261016.md) | Retain issued objects and evidence across failures; recover exact publication identity instead of inventing a replacement | Existing agreement/authority rules, not a new native BFT protocol |
| [Verified return durability](b3-flowmesh-client-verified-return-durability-20261016.md) and [aggregate save](b3-flowmesh-snapshot-aggregate-save-20261016.md) | Client evidence/no-resubmit obligations become durable before verified return; related head/obligations use one synchronous atomic batch | Process-kill/injected failures are not machine power-loss or old-backup rollback qualification |
| [Health](b3-flowmesh-health-overflow-admission-20261016.md) and [client read admission](b3-flowmesh-bounded-client-read-admission-20261016.md) | Bounded restricted health reserve and foreground FIFO outside client work ownership; existing freshness ages remain binding | Health-or-busy is not signing readiness; no arbitrary-load availability claim |
| [Qt selected reads](b3-flowmesh-qt-selected-read-isolation-20261016.md), [Qt Catalog](b3-flowmesh-qt-catalog-read-isolation-20261016.md) and [native Catalog](b3-flowmesh-native-catalog-isolation-20261016.md) | Separate bounded passive owners; exact scope fences discard late reads without overwriting command evidence or renewing freshness | Native authority queries and some shared locks remain; no claimed throughput improvement from functional tests |
| [Selected final preflight](b3-flowmesh-selected-final-preflight-20261016.md) | An already-approved remote order/cancel/withdraw checks its exact current verified account without rediscovering unrelated Catalog rows | Explicit narrow availability-policy change; no stale-proof fallback, economic retry or bypass of native signing checks |
| [Transient outcome evidence](b3-flowmesh-outcome-budget-recovery-20261016.md) | Only an exact recognized standalone server BUDGET response can reopen an evidence read after its bounded cooldown | All other terminal failures remain one-shot; never resubmit the economic instruction |
| [Fair automatic result reads](b3-flowmesh-outcome-fifo-20261016.md) | Included-outcome reads join the same bounded FIFO as unresolved requests; verify each selected owner's authority and return only the caller's card | No larger traffic budget, durable pure-outcome cache, arbitrary-schedule fairness or measured speedup |

Each linked stage identifies its frozen source, counterexamples, actual tests,
bounded non-author source review and remaining assumptions. This collection is
not a fresh independent audit of the entire integrated tree or deployment approval.

## Actual tests and measurements

Final selected-preflight build succeeded. Ordinary local qualification on an
Apple M4 Max, 16 cores/128 GiB, macOS 26.5.2, AppleClang 17 Release:

- Latest native v10 selection: **500 cases / 28 suites**, all actual children
  exit 0, no selected skips, 227.968 seconds. Earlier v9's 497 cases retain
  their separate historical identity.
- Complete Qt trading helper: **82 passes**, zero failures/skips.
- Complete Qt workspace: **362 passes**, zero failures, three unchanged
  opt-in visual skips.
- Latest v10 runner controls: **56 passes** in separate reviewer/root runs;
  these are not native tests or an added combined population.
- Unchanged isolated models: **446 cases / seven suites** in their separate
  earlier execution. Fixed synthetic membership/authentication and modelled
  assumptions remain; no native V2 integration follows from those passes.

Functional native and Qt runs overlapped, so elapsed times are not controlled
performance comparisons. [Linux CI guardrails](b3-flowmesh-native-linux-guardrails-20261016.md)
are committed locally but have not run remotely. Linux/DEBUG_LOCKORDER/TSan,
Windows, WAN, practical desktop scaling, sustained load and soak remain open.

The one [bounded result-decision experiment](b3-flowmesh-outcome-slice-20261016.md)
used four generated validators, two TLS endpoints, one engine-off client,
two markets, a maker and eight burst originals. All nine fills/results and
follow-up balances verified; maker conservation passed, all harness threads
stopped and all five final daemon statuses were 0. The isolated result was
**307.772 ms**. Eight burst results had conventional median **1,462.619 ms**,
nearest-rank p50 **1,364.825 ms**, maximum **2,274.240 ms**. The performance flag
was false. These are not a controlled before/after speed comparison or a
successful 200 ms campaign.
That measurement used the old native client `bff8dac1`, before the final
`f6610edb` fairness correction. No trading-latency campaign was run after it;
functional regression success is not a measured speedup.

The selected capture had zero drops and proved one **local client read-budget
deferral**, before HTTPS. It did not prove that budgeting caused the whole
192.570 ms inclusion-to-result interval. The subsequent uncaptured interval
remains unattributed. A separate older capture lost data and timed out on five
of eight results; those causes remain unproven. Neither failure was hidden by
rerunning until green or mixing successful subsets into a headline median.

## Biggest remaining structural gap

Fast runtime leaves do not yet make the public path independent of chain work:

1. Service `ClientSnapshotView`, `MarketData` and `MarketStatus` still overlay
   `RulesActiveAtTip`/`ReconciledAtTip`, which acquire `cs_main`.
2. Service handle lookup shares `Impl::mutex` with market installation that can
   wait on the chain. Older public certificate lookup can also use the blocking
   market/store/chain history fallback.
3. `ProcessTick`/`ProcessMessage` still make chain queries under the market
   mutex. Anchor checking traverses retained distinct committed anchors;
   constant-time historical ancestry is not implemented or qualified.
4. The [mutation fence](b3-flowmesh-native-mutation-fence-20261016.md) is a tested,
   default-null prerequisite, not a production chain-facts publisher/consumer.
   It does not authorize signing, and finality dirty recovery still permits
   modern-span replay. No finality undo was implemented.

The next structural read stage must first freeze a genuine-certified
service-versus-runtime counterexample while real `cs_main` is held. Its safe
contract is coherent historical display **or explicit bounded busy/unknown**,
never a renewed freshness label or signing permission. It must define initialized
registry publication/lifetime and advisory UNKNOWN before wiring a facade.
Simply returning inactive when a lock is busy is not a valid authority decision.

That private test-first counterexample was subsequently reviewed and executed
once, without a production repair. It used the **same initialized service**,
real indexed four-seat/three-signature BLS genesis, production certificate
ingress/append and independently verified exact state/head. Its existing
`ClientSnapshot` passthrough answered while another thread owned real `cs_main`;
`ClientSnapshotView` did not finish in the shared one-second held-lock window.
After release both readers drained and exact evidence remained unchanged.

The build exited 0. The **native child exited 201: 714 assertions passed, one
intended availability assertion failed**, zero warnings/expected failures, no
timeout or output overflow. The collection wrapper's 0 means it captured the
predetermined red result, not that the test passed. The intentional red case
remains private/off the default suite; no assertion was inverted or hidden by
a skip. Source-only author and separate non-author review preceded root's run.
Its prospective historical-or-UNKNOWN marker is unratified test data, not an
implemented wire field, fresh-authority rule or signing permission. The fixture
uses indexed synthetic blocks, not full B3 block/custody/TCP/Qt qualification.
Its certified genesis remained checkpoint-paused; the test did not demonstrate
healthy/current-ready signing availability.

Private patch SHA256:
`a84f944ffc473b44c9503764135aaac2a9e3764fc0d3de424a6926a64fc74f10`;
preserved native executable SHA256:
`003529f3adcfa86282c52120289aaacfe338c6e17ba2d0e9442160e87ced0ff6`.
Production service/runtime/client were unchanged; this proves a remaining
dependency, not a completed facade or a normal-load latency measurement.

## Full V2 still needs explicit design and qualification

The [roadmap](b3-flowmesh-redesign-plan-20261015.md) retains decisions R-D1–R-D7.
This session did not ratify a new BFT variant, producer/finality/membership
authority, price-time/IOC semantics, fee AssetIds/rounding, futures economics,
oracle/backstop, bootstrap/bridge lineage or live activation height.

Native shared asset balances, atomic cross-market reservations, genuine generic
pairs, stable-asset fees/rewards and futures remain required. Their isolated
models or unrelated uncommitted prototypes are not a delivered native product.
The complete deposit/order/fill/cancel/custody-withdrawal slice, authoritative
migration, bridge round trips and distributed tester qualification remain open.

No claim is made that critical failures are impossible. The next review must
inspect the exact frozen source and adversarial cases, not merely this report
or the total number of passing tests.
