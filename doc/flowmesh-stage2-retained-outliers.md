# Retained four-validator run4: outlier forensics

## Outcome and limits

The original run succeeded: 54 measured actions, two recovery cycles, and 203 new B3 blocks during the workload. Ten actions exceeded one second. All ten were in fault phases with only FN0/FN1/FN2 configured to sign. None of the fourteen initial/recovered all-four samples exceeded one second.

The retained artifacts do **not** establish the cause of those ten delays. Their successful RPC admissions took 7.378–34.732 ms; the dominant remaining time was before the client observed a certified state. The additional checks of the selected replicas took only 1.581–17.508 ms after that client observation. These are local observation timings, not packet arrival, certificate-formation, or cross-machine latency measurements.

Every outlier's live-path action/proposal/execution/vote/certificate/durable event history has fallen out of the final FN0–FN2 128-event rings. FN3 retains two outliers' *later historical catch-up* events, which are not their original live-path timings. The RPC debug logs record method parse time, not arguments, action IDs, results, or completion time. There is no retained monotonic-to-UTC calibration.

This report is a read-only reconstruction of the original evidence. No runtime, harness, network, or consensus source was changed, and no new tests or nodes were run.

## Evidence and measurement definitions

- Original qualification JSON (private retained generated-fixture evidence), SHA-256 `30c020129fc91c0f223293d58bffa3bf3d5795d0f197b2f72c52268ceb86b87a`.
- Original run4 log (private retained evidence), SHA-256 `39a86ce625c16ac4745b60709fb544241ce65939386252b6ae466d987392cb19`. Its 54 `FMNET_ACTION_RESULT` records match the JSON samples by action ID.
- N0–N3 node logs remain private retained generated-fixture evidence.
- [Original sampling control flow](../test/functional/feature_flowmesh_independent.py:264): `t0` is taken after the initial account and block-height reads, before the admission attempt and any B3 clock pump. `Aobs` is recorded after a successful RPC response; `Cobs` after a certified-state response advances the account sequence; `Robs` is the latest subsequent successful selected-replica check.
- [Replica checks](../test/functional/feature_flowmesh_independent.py:204) occur serially in node-index order and verify the exact target hash/state, including certified history if a node has advanced further. The 25 ms polling interval is not a strict 25 ms latency bound: RPC service time, scheduling, and the B3 clock pump also contribute.
- [RPC logging](../src/rpc/request.cpp:243) precedes parameter parsing. Its timestamps below are request-parse observations, not completion timestamps.
- [Bounded event retention](../src/node/flowmesh_runtime.cpp:563) keeps the last 128 events per market; repeated certificates and transport events can displace older action stages. Final aggregate counters cannot reconstruct an individual action.

`Aobs−t0` includes explicit refused attempts, retry waiting, successful signing/admission RPC service, and response overhead. `Cobs−Aobs` is **not** measured admission-to-certificate formation: actual admission can precede Aobs, and actual certification precedes Cobs. It is the measured response-to-observation interval. `Robs−Cobs` is the extra observer-check tail, not pure replication delay. In fault phases Robs covers only nodes 0/1/2; it does not mean all four replicas had caught up.

## Phase distributions

Milliseconds; nearest-rank percentiles, `ceil(n*p)−1`, matching the original harness. Small samples make p95/p99 equal the maximum in several rows. “No explicit pause refusal” does not mean “no B3 reconciliation.”

| Configured phase | n | >1 s | Aobs→Cobs p50 / p95 / p99 | t0→last selected replica p50 / p95 / p99 | Extra check tail p50 / p95 / p99 |
|---|---:|---:|---|---|---|
| All four, no injected fault (initial + recovered) | 14 | 0 | 205.436 / 319.382 / 319.382 | 222.489 / 322.394 / 322.394 | 9.610 / 35.303 / 35.303 |
| All configured faults, three eligible signers | 40 | 10 | 226.176 / 2121.727 / 2315.432 | 241.837 / 2141.190 / 2334.775 | 8.853 / 35.770 / 43.181 |
| FN3 offline (both cycles) | 20 | 5 | 227.049 / 2215.760 / 2315.432 | 243.322 / 2227.287 / 2334.775 | 7.473 / 38.097 / 43.181 |
| FN3 restarted/unarmed; bulk held (both cycles) | 8 | 2 | 229.743 / 2084.787 / 2084.787 | 266.445 / 2102.829 / 2102.829 | 10.315 / 25.884 / 25.884 |
| FN3 unarmed; bulk limited to 2 KiB/s (both cycles) | 12 | 3 | 193.135 / 2121.727 / 2121.727 | 214.340 / 2139.995 / 2139.995 | 9.099 / 34.824 / 34.824 |
| Explicit pre-admission paused-text refusal (not an interval classification) | 4 | 1 | 172.137 / 2104.877 / 2104.877 | 218.392 / 2141.190 / 2141.190 | 9.404 / 32.829 / 32.829 |
| No explicit pre-admission pause refusal (not proof of no reconciliation) | 50 | 9 | 226.176 / 2121.727 / 2315.432 | 239.274 / 2139.995 / 2334.775 | 9.099 / 35.770 / 43.181 |

The offline/held/slow subsets partition the 40 fault samples. The last two rows cut across phases and must not be added to them. True admission-to-certificate distributions and distributions partitioned by complete B3-reconciliation intervals cannot be computed from this evidence.

## Every action exceeding one second

All times below are measured offsets from that action's `t0`, except the final difference column. “0” refusals means the sample's complete recorded refusal array is empty. Every row subsequently returned `accepted: true`. Four seats and a three-seat quorum were unchanged; available signers were configured FN0/FN1/FN2, not a claim that each one's vote was individually observed.

| Target seq | Scenario | Client | FN3 state | Pre-admission refusals | Aobs ms | Cobs ms | Robs ms (nodes 0/1/2) | Aobs→Cobs ms | Extra Robs−Cobs ms | B3 height before→after |
|---:|---|---:|---|---|---:|---:|---:|---:|---:|---|
| 20 | offline_fn_0 | 1 | offline | 1 at +0.200 ms (P) | 34.732 | 2139.608 | 2141.190 | 2104.877 | 1.581 | 266→269 |
| 24 | offline_fn_0 | 1 | offline | 0 | 7.488 | 2100.821 | 2108.690 | 2093.333 | 7.869 | 269→272 |
| 28 | held_bulk_0 | 1 | online, unarmed; bulk held | 0 | 7.727 | 2092.514 | 2102.829 | 2084.787 | 10.315 | 273→276 |
| 32 | slow_bulk_0 | 1 | online, unarmed; bulk 2 KiB/s | 0 | 9.038 | 2102.117 | 2105.395 | 2093.079 | 3.277 | 277→280 |
| 36 | slow_bulk_trade_0_ask | 0 | online, unarmed; bulk 2 KiB/s | 0 | 7.541 | 1990.235 | 1992.240 | 1982.695 | 2.004 | 282→285 |
| 40 | offline_fn_1 | 1 | offline | 0 | 7.378 | 2223.138 | 2227.287 | 2215.760 | 4.149 | 362→365 |
| 44 | offline_fn_1 | 1 | offline | 0 | 7.583 | 2323.016 | 2334.775 | 2315.432 | 11.760 | 366→369 |
| 48 | offline_trade_1_ask | 0 | offline | 0 | 7.524 | 2085.073 | 2102.581 | 2077.549 | 17.508 | 370→373 |
| 52 | held_bulk_trade_1_ask | 0 | online, unarmed; bulk held | 0 | 7.416 | 1577.575 | 1579.680 | 1570.160 | 2.105 | 375→377 |
| 56 | slow_bulk_1 | 1 | online, unarmed; bulk 2 KiB/s | 0 | 7.619 | 2129.346 | 2139.995 | 2121.727 | 10.649 | 377→380 |

P is the exact refusal text: “FlowMesh market is paused (at least four active seats are required)”. For seq20 it occurred at +0.199833 ms, followed by a successful second attempt, with Aobs at +34.731666 ms. The other nine outliers had one successful attempt and no recorded pre-admission refusal. This text is also used for transient pause; it does not prove seat loss or establish a reconciliation interval.

### RPC wall-clock bounds

Date is 2026-09-11 UTC throughout. Initial `t0` lies after the preceding account-balance request was parsed and before the first mutation request was parsed. These are deliberately loose, source-supported bounds. Successful RPC admission/response lies after the successful mutation's parse and before the first following market-data query's parse. The successful certified query's parse is shown separately; the response/observation is later, at the relative Cobs above. UTC RPC-to-action mapping is inferred from the serial harness call order and matching result record, not a debug-log action-ID field.

| Seq | Initial t0 UTC bracket (N#: log lines) | Successful mutation parse → first observation request parse | Successful certified query parse | Result-log line |
|---:|---|---|---|---:|
| 20 | 15:13:53.271619 → 15:13:53.271954 (N1:18439,18443) | 15:13:53.299112 → 15:13:53.306745 (N1:18451,18453) | 15:13:55.409786 (N1:18738) | 32 |
| 24 | 15:13:56.047032 → 15:13:56.054666 (N1:18822,18826) | 15:13:56.054666 → 15:13:56.062305 (N1:18826,18828) | 15:13:58.105485 (N1:19106) | 36 |
| 28 | 15:13:59.597271 → 15:13:59.604725 (N1:19228,19232) | 15:13:59.604725 → 15:13:59.612505 (N1:19232,19234) | 15:14:01.646864 (N1:19529) | 40 |
| 32 | 15:14:02.385881 → 15:14:02.394884 (N1:19651,19659) | 15:14:02.394884 → 15:14:02.402378 (N1:19659,19661) | 15:14:04.427098 (N1:19963) | 44 |
| 36 | 15:14:05.269547 → 15:14:05.277230 (N0:32800,32804) | 15:14:05.277230 → 15:14:05.284787 (N0:32804,32806) | 15:14:07.245015 (N0:33083) | 48 |
| 40 | 15:15:02.395175 → 15:15:02.402618 (N1:24337,24341) | 15:15:02.402618 → 15:15:02.409997 (N1:24341,24343) | 15:15:04.625311 (N1:24647) | 53 |
| 44 | 15:15:05.334220 → 15:15:05.341869 (N1:24765,24769) | 15:15:05.341869 → 15:15:05.349460 (N1:24769,24771) | 15:15:07.626066 (N1:25060) | 57 |
| 48 | 15:15:08.366333 → 15:15:08.373913 (N0:37191,37195) | 15:15:08.373913 → 15:15:08.381438 (N0:37195,37197) | 15:15:10.439269 (N0:37459) | 61 |
| 52 | 15:15:12.402790 → 15:15:12.410195 (N0:37599,37603) | 15:15:12.410195 → 15:15:12.417610 (N0:37603,37605) | 15:15:13.917328 (N0:37811) | 65 |
| 56 | 15:15:14.656354 → 15:15:14.665183 (N1:25713,25717) | 15:15:14.665183 → 15:15:14.672852 (N1:25717,25719) | 15:15:16.744857 (N1:26014) | 69 |

For seq20, N1:18443 is the refused first request and N1:18451 the successful second request. Do not subtract the result log's UTC timestamp from a monotonic duration to manufacture an exact submission timestamp: the result is logged after additional replica and block-height RPCs.

### Stage coverage for each outlier

U = unknown/unretained, not zero or a failed stage. D≤Robs means exact durable target state was observed by the listed replica-check offsets; it does not identify the durable-apply instant. The live execution and certificate steps necessarily succeeded by the observations, but their individual timestamps and signer identities are absent.

| Seq | Proposal created / received | Round entered / timed out | Execution begin / end | Verified attestations / certificate formed | Live durable application upper observations N0 / N1 / N2 ms | Client observed ms |
|---:|---|---|---|---|---|---:|
| 20 | U / U | U / U | U / U | U / U | 2140.188 / 2140.674 / 2141.190 | 2139.608 |
| 24 | U / U | U / U | U / U | U / U | 2101.464 / 2106.268 / 2108.690 | 2100.821 |
| 28 | U / U | U / U | U / U | U / U | 2100.308 / 2100.905 / 2102.829 | 2092.514 |
| 32 | U / U | U / U | U / U | U / U | 2104.279 / 2104.834 / 2105.395 | 2102.117 |
| 36 | U / U | U / U | U / U | U / U | 1990.909 / 1991.587 / 1992.240 | 1990.235 |
| 40 | U / U | U / U | U / U | U / U | 2226.009 / 2226.649 / 2227.287 | 2223.138 |
| 44 | U / U | U / U | U / U | U / U | 2333.409 / 2334.096 / 2334.775 | 2323.016 |
| 48 | U / U | U / U | U / U | U / U | 2085.827 / 2101.862 / 2102.581 | 2085.073 |
| 52 | U / U | U / U | U / U | U / U | 1578.297 / 1578.999 / 1579.680 | 1577.575 |
| 56 | U / U | U / U | U / U | U / U | 2136.212 / 2137.033 / 2139.995 | 2129.346 |

There is no retained action-ID or target-hash match for any of these ten outliers in the final FN0/FN1/FN2 event arrays, and no match in any of the four debug logs. Matching a historical `sequence` alone would be unsafe: the final rings contain repeated older certificates. Full action IDs and exact target hashes are listed below.

Only FN3 retains these exact target-hash matches, during its later catch-up:

| Seq | Event | FN3 local monotonic µs | Meaning |
|---:|---|---:|---|
| 52 | certificate_verified | 292286844329 | Historical certificate verified from peer −4; not the original quorum event |
| 52 | durably_applied | 292286857688 | FN3 applied historical target; 13.359 ms after that local verified event |
| 56 | certificate_verified | 292286894836 | Historical certificate verified from peer −4; not the original quorum event |
| 56 | durably_applied | 292286905649 | FN3 applied historical target; 10.813 ms after that local verified event |

These timestamps cannot be converted to the harness offsets or another node's timeline without a retained clock bridge. FN3 was not part of the outlier samples' replica-observation set.

## B3 reconciliation evidence

All samples ran while the harness advanced B3; block-height changes in the table do not identify when a FlowMesh gate closed or reopened.

Exactly four of all 54 samples recorded one pre-admission paused-text refusal: seq17 at +3.943375 ms (Aobs 38.888750), seq20 at +0.199833 ms (Aobs 34.731666), seq34 at +0.238750 ms (Aobs 35.978291), and seq59 at +0.250458 ms (Aobs 38.206083). None recorded the separate “service is reconciling the B3 tip” RPC text. Those four refused calls establish points at which a pause check rejected admission, not full intervals or their causes.

The debug logs do retain explicit FN3 reconciliation-failure/pause points during its restart: 15:13:59.296459–15:13:59.348595 (N3:16021 through 16205) and 15:15:11.088966–15:15:11.166432 (N3:23600 through 23798). These are the first/last logged points, **not proven continuous interval boundaries**; no matching completion marker closes them. They occur on restarted, unarmed FN3 and do not prove that the three signing nodes were paused during an outlier. No complete gate-open/closed interval history was retained for FN0–FN2 in the measured workload.

Consequently, a “during reconciliation” versus “outside reconciliation” latency distribution is unavailable. The explicit-refusal distribution above is the only supported narrower classification, and is named accordingly.

## Interpretation: measured versus inferred

Measured: ten outliers, all fault-phase targets 20,24,28,32,36,40,44,48,52,56; prompt successful RPC admission; a long response-to-certified-observation interval; short subsequent observer-check tails; successful eventual exact-state convergence. The original recovery summaries separately report fixed-target catch-up in 62,640.480 ms and 64,781.952 ms from restart, each 20 entries behind. They are not part of the per-action Robs values above.

Inferred hypothesis only: the four-sequence periodicity is consistent with a committee/proposer scheduling effect when one of four signers is unavailable. It is **not proof** of which proposer was selected, a round timeout, its duration, a missing vote, a network refusal, or a B3 pause. In particular the 1,577.575 ms seq52 delay and the roughly two-second delays must not be relabeled as measured two-second round timeouts.

Needed for attribution in a new run: continuously drained action/hash-correlated events with a monotonic cursor and explicit lost-range detection; RPC start/end and node-clock alignment; proposal round/election/receipt markers; execution start/end/failure; per-seat verified votes and certificate formation; durable application; typed reconciliation enter/leave; and client observation request/response timestamps. Per-action traces must distinguish retry admission, local socket writes, peer validation, and actual application. This report requests no protocol or quorum changes.

## Exact correlation identifiers

### Sequence 20

- Action ID: `6085281f99518ab433d4771375289a31bea647488fe25c35fa3deb02655e4632` (client 1, account sequence 16).
- Certified target hash: `acc3575f793cb1c331059ad07e0df0f765a48f179163ce1e836958c901f0e4c0`.

### Sequence 24

- Action ID: `c73c2f5c3d13697bf49c0b522d23ccd69da6e2946c9763ed669a59b8020d4827` (client 1, account sequence 20).
- Certified target hash: `feed13bdbd8fac79ef1b04187ec1b9931d0f87ddfde7d81a5cfcda130c2bb8a7`.

### Sequence 28

- Action ID: `45b9c31a680d9441379b66c98cd5b223a23b14762a71de7d21fab8efc9f4e77e` (client 1, account sequence 23).
- Certified target hash: `b4bd9fabae41321dcbc2c56eb2a4636fe997278060a48ecaa43508b1a33c22a7`.

### Sequence 32

- Action ID: `6e94b23c02c87bc5a396fdf23c4caa1a7cb09f7e5d359a03a7ce9152da6ccd34` (client 1, account sequence 26).
- Certified target hash: `fc9959ede39a007304ea43ed1e7905cde68e2d810ae654b646a5b3bbd09921f4`.

### Sequence 36

- Action ID: `b91ec3b68e54dd51d6f23214d6cb6023715949254926c35f0a1d8f8ebe8d10b7` (client 0, account sequence 4).
- Certified target hash: `439503e0794f38e6d84dbca0b18845b39a99271bcc8db93e1005d248509ad17f`.

### Sequence 40

- Action ID: `12ebff712ec3f816eb2d19941b476e1bca5b76e104d7723ab74a686946e8112a` (client 1, account sequence 33).
- Certified target hash: `1e820cbce80eeb00e45bb2589ab0ac6f93b59940bbaa2353b1098f5874a9f6d8`.

### Sequence 44

- Action ID: `94d9f4586b815ea4dbea397c373ea82d58788f7bd7ab13da296352681a7a9190` (client 1, account sequence 37).
- Certified target hash: `e4a5c8700c79ec52341b95e62744de97d16cb8626d6b748012347cc9a636dde8`.

### Sequence 48

- Action ID: `723b5fece1599247bee0191eeba36ecb143586ada9122e0cf131d00ba5ba1ac3` (client 0, account sequence 5).
- Certified target hash: `0eada2f892fa01d877bcebe554d7270f6ddd09e405f7cdc6eadba7c1691fe52f`.

### Sequence 52

- Action ID: `2d4d975b758efe7879305633344c79b5a80c5a8e86c621066d9a9781e284081e` (client 0, account sequence 6).
- Certified target hash: `be673aef0edafcdaac474fa38d1604ee9b577189676472b3dd98644f4149cf17`.

### Sequence 56

- Action ID: `c6662cc81c3295e7fe0d3c0c4a6a5e9d702bbee3dc5881169d032ae6da494098` (client 1, account sequence 47).
- Certified target hash: `7cfe91b12c2c32694d89816ac1859a6f9511af578afadad90caef045cd21139a`.
