# Native diagnostics after bounded original retention

Date: 2026-10-16. **Both predetermined workloads completed; the 200 ms target
remains unmet and unqualified.** This is native V1-based spot, not the isolated
V2 accounting/agreement model, a Qt session or a release qualification.

## Exact comparison boundary

Frozen source composition is equivalent to
`cb63cb85544f59d44831578b1e90a11cb88149c7`, excluding the unrelated uncommitted
V2 prototype/CMake changes. Runtime, client, service, HTTPS, P2P, validation and
fence source files were compared with the frozen compilation tree before the
pair. Ordinary daemon SHA-256:
`069226d05429fa7256f11b28074796b0dc63680676f209142f2bffed4dee7671`.
Harness SHA-256:
`f344f81341aa4d4fd568ea491ff7e7f0695ff84b25440fe4baffda5e77412121`.
This includes the separately tested client durability and accepted-original
repairs; no client ordinary-read refactor is present.

Same Mac/session and fixture as the
[earlier campaigns](b3-flowmesh-redesign-measurements-20261016.md): Apple M4 Max,
16 cores, 128 GiB, macOS 26.5.2, AppleClang 17 Release C++20; four generated
validators, two HTTPS operators, one engine-off client, four buyer wallets,
two markets and standing maker liquidity. Seed `24092026`, port seed `92401`.
Exactly one isolated matched fill, one second idle, then eight simultaneous
fills. B3 advances; normal cryptography, quorum and synchronous storage remain
enabled. No heavy build or test ran during either measured window.

The profile is `native-result-observation-v1`, ordinary zero-wait submit,
with identical bounded in-memory RPC observation and 5 ms local status polling.
It measures original submission RPC start to client-observed verified applied
outcome. It does not qualify durable restoration of outcome labels. Capture-off
then capture-on used distinct fresh generated identities and originals; matching
the seed does not make their ActionIds identical.

| Campaign | Isolated result | Burst completed | Burst result p50 | Burst result maximum | Actual exit |
| --- | ---: | ---: | ---: | ---: | ---: |
| Memory capture off | 290.188 ms | 8/8 | 1,300.271 ms | 2,433.510 ms | 0 |
| Memory capture on | 259.211 ms | 8/8 | 1,948.125 ms | 2,442.848 ms | 0 |

Each also completed its isolated fill: **9/9 per campaign**, not one combined
population. All five child daemons exited 0 in each. These nine-sample diagnostics
do not establish reliability, tail latency, a population median or capture
overhead. Earlier failed campaigns remain failures, not replaced by these rows.

## Captured request timelines

The capture contains 22,961 / 20,423 / 22,139 / 22,067 / 2,881 events on nodes
0 through 4. All dropped-event and stream-limit-marker counts are zero; RPC
capture also dropped zero. This is complete within the instrumented boundaries,
not a claim to have instrumented every prerequisite or scheduler decision.

Captured public synthetic identities:

- M0: `2a58e59fc8f73356bc6ce19dcd929781ed0a1e6e33d6535c6dc683e118387361`
- M1: `89753928497a9ffd7e2e679c5680a572582f85d9a8b1e305759a048767267255`
- A: `ade1a3732d9ed54b70e455034a0aa69e9c5153152ef08c3580e469afb7560232`
- B: `cdfc31638030b9faddfcf52523f998428941461e9a632fab0cfac3f5034c028a`
- C: `706e4ba5d271c455e24a542e92d467afa2ff283ad7312b4b77d2f0d45ffc2012`
- D: `abd06fb8b13219925ea64eb67eaad89a807725ea8d6acd73453345e35a833021`
- E: `8ccfe5ba023ebe1c46a4f9db409edb9664364992b4a86dbc45e1023b2e012894`

Values below are milliseconds from that original submission RPC's host start.
Native clock alignment uses recorded offset midpoints (at most ±0.085 ms
recorded alignment uncertainty). Every row retains **market and ActionId**;
the same semantic ActionId can occur in different markets.

| Sample / pair | Backend start → work acquired | HTTPS dispatch → done | Operator enqueue → dequeue → market acquired → pool | Sync start → done → durable apply | Client inclusion → outcome observed |
| --- | --- | --- | --- | --- | --- |
| 0 / M0,A | 8.470 → 8.473 | 21.030 → 23.823 | 23.608 → 23.617 → 23.651 → 23.736 | 210.835 → 218.519 → 218.536 | 259.211 → 259.211 |
| 1 / M0,B | 6.954 → 13.682 | 25.872 → 28.355 | 28.125 → 28.134 → 28.157 → 28.232 | 273.375 → 292.930 → 292.949 | 408.743 → 408.743 |
| 2 / M0,C | 224.704 → 230.448 | 251.470 → 254.002 | 253.809 → 365.661 → 365.661 → 368.476 | 1,927.449 → 1,934.351 → 1,934.370 | 1,948.125 → 1,948.125 |
| 3 / M0,D | 13.220 → 27.904 | 62.178 → 99.977 | 99.725 → 318.259 → 318.260 → 321.087 | 1,536.239 → 1,547.978 → 1,548.033 | 1,768.961 → 1,768.961 |
| 4 / M0,E | 116.085 → 165.942 | 184.673 → 217.867 | 217.582 → 347.950 → 347.952 → 350.704 | 1,927.159 → 1,934.060 → 1,934.079 | 2,025.659 → 2,442.848 |
| 5 / M1,A | 230.387 → 253.965 | 285.524 → 293.122 | 292.906 → 440.351 → 440.353 → 440.421 | 1,973.437 → 1,987.341 → 1,987.396 | 2,022.755 → 2,022.755 |
| 6 / M1,C | 107.302 → 116.174 | 155.460 → 166.012 | 165.702 → 336.204 → 336.204 → 336.264 | 602.304 → 609.206 → 609.226 | 948.931 → 948.931 |
| 7 / M1,D | 300.545 → 306.161 | 325.373 → 336.389 | 336.063 → 440.637 → 440.637 → 440.697 | 1,973.311 → 1,987.215 → 1,987.270 | 2,300.119 → 2,392.406 |
| 8 / M1,E | 305.861 → 336.091 | 366.512 → 382.716 | 382.533 → 440.815 → 440.817 → 440.873 | 1,972.965 → 1,986.869 → 1,986.924 | 2,295.635 → 2,295.635 |

Correlation uses exact client Submit pair and parent spans through client_call
and https_request, the operator API pair, original local-peer wire hash,
message_processing dequeue/lock marks, and certified entry hash through append
and durably_applied. All submissions used operator 0. HTTPS dispatch is not a
kernel socket-write timestamp. Pre-Submit time includes wallet/preflight/signing
work without a dedicated complete parent span. Final client columns are host
observation upper bounds, not exact cryptographic-verification timestamps.

## Proved waits versus remaining attribution

- **259.148 ms client work wait:** node 4 ActionStatus span 2759 (M1,E) waited
  behind Data span 2752 and its snapshot. HTTPS dispatch took only 4.021 ms;
  two synchronous outbox writes took 29.669 and 226.048 ms. Durability work,
  not HTTPS waiting alone, explains most of that holder. Do not remove required
  persistence or expose a result before it is durable.
- **218.533 ms original ACTION queue wait:** sample 3 waited before dequeue;
  subsequent market-lock acquisition was 0.001 ms. During that interval the
  same worker processed 24 agreement, nine attestation, three certificate,
  two tick and one other ACTION iterations. Counts overlap the interval;
  they are not an additive CPU-time decomposition.
- **1,583.374 ms pool-to-apply interval:** M0,E's measured delay initially lacked
  full attribution. The subsequent phase trace below resolves its dominant
  parent-slot delay; its final append sync was 6.901 ms. The residual alone
  proves neither BLS nor PBFT is slow.
- A separate 399.016 ms operator-2 signing-journal sync concerns the next slot 9,
  after measured slot 8 was already durable. Assigning its entire duration to
  these trades would be incorrect.

### Durable agreement messages missed the reopened publication opportunity

M0,D is certified in slot 9, entry
`f2092be9d81f6a13880767d6e197d426ada2a7eda8df19d698331e9fd454e6c4`.
M0,E and M0,C follow in slot 10, entry
`5e337f6ea37620caf04b7d887a178020d7ca7b19394ca7681cc214b8058822dd`.
This table uses **D's RPC start for every cell**, unlike the per-request clocks
above; do not interchange the two origins.

| Recorded phase (ms) | D / slot 9 | E / slot 10 |
| --- | ---: | ---: |
| Operator-0 pool admission | 321.087 | 350.594 |
| Proposer selects candidate | 338.411 (node 2) | 1,520.722 (node 1) |
| Proposal signing path completes | 366.962 | 1,534.072 |
| First actual proposal delivery attempt | **1,359.411** | 1,534.022 |
| First PREPARE delivery attempt | 1,359.478 | 1,557.142 |
| First COMMIT signing begins | 1,393.128 | 1,616.040 |
| First decision persistence completes | 1,457.932 (node 1) | 1,787.249 (node 3) |
| First V1 attestation signed | 1,464.565 (node 1) | 1,808.562 (node 3) |
| Operator 0 durably applies | 1,548.033 | 1,933.968 |

Node 2's slot-9 proposal publication at 366.945 ms and PREPARE publication at
391.399 ms both recorded `agreement_refused: market_unavailable_or_reconciling`.
Its recorded gate reopened at 395.591 ms, but actual proposal delivery began
**963.820 ms later**. Numerous intervening ticks attempted the cached PREPARE
signing path in approximately zero time, without publishing again. The worker
also explicitly waited from 1,116.876 to 1,347.944 ms: this was not continuous
busy execution, BLS work or a held chain lock.

The frozen [agreement Sign path](../../src/node/flowmesh_agreement.cpp) returns
existing signed bytes without another Send. New signatures do attempt Send.
The [runtime ProcessTick](../../src/node/flowmesh_runtime.cpp) calls Retry only
when `next_agreement_retry` is due, then schedules its next turn with the
one-second `DELIVERY_REPEAT`. Gate reopening does not advance that deadline.
The service ticker is 250 ms. Actual first deliveries follow the existing
Retry path, and peers receive the proposal within 0.5 ms once attempted.

E reached node 2's pool after slot 9's proposal and PREPARE were already signed.
Its operator-0 pool-to-apply interval spans 1,155.975 ms until node 1 applied
slot 9, 14.153 ms until node 1 selected slot 10, then 413.246 ms until operator
0 applied slot 10. These are successive observed dependency boundaries, not
added concurrent replica medians. Child-slot queue, signing and persistence
intervals still overlap across replicas.

The capture does not record the exact retry deadline, planned ticker wake,
individual predicate behind the combined refusal reason, every vote-count
transition or an explicit parent_hash field. The parent dependency follows
recorded slot/context advancement and the frozen source checks. A deterministic
repair proof must reproduce durable signed bytes, publication refusal, reopening
and exact retransmission under all unchanged authority guards. No quorum,
view-change timeout, normal repeat interval or journal rule is changed here.

A separate read-only context traced these correlations and inspected the frozen
source without running the workload itself. Actual workload execution was by
the local qualification owner; reported executions are not an independent rerun.

Next gates are a held-response contention regression for ordinary refreshes,
separate current-state/durability review, and a bounded exact-object recovery
test for the demonstrated publication/retry gap. A staged aggregate outbox transaction may be worth a
separate proof and regression, but no synchronous barrier has been removed in
this measured source. Raw generated wallets, signatures, TLS keys and private
captures remain outside the repository.

No mainnet/VPS data, live keys, protocol, fees, signing obligations, deployment
or activation was changed. The full V2 product and 200 ms goal remain open.
