# FlowMesh client consistency and measured worker repair — 2026-09-24

## Status

REPAIRS COMPLETE FOR REVIEW; PERFORMANCE TARGET NOT MET. The client repairs
and separately reviewed narrow local-queue repair are implemented and tested.
The final healthy campaign completes 40/40 requests and the separate fault
campaign 30/30. The diagnostic campaign still fails 2/40 requests, and one
combined C++ suite has an unresolved intermittent idle-deadline failure.
Those failures remain part of this milestone, not replaced by later passes.
No deployment, 200 ms achievement or V2 performance result is claimed.

The preserved baseline is documented in
[the original report](flowmesh-performance-baseline-20260924.md). Failed runs,
original source, binaries, signed test instructions and raw evidence have not
been replaced. All operations use newly generated, isolated regtest identities.
No live wallet, production signer, contract or mainnet configuration was used.

## Exact identities and experiment

- Baseline executable source: `f3f2b86840df97c6666379c6c96d4fc26c786213`.
  SHA256 `a2737f4c3ff5b999ec61faa381c6fc724017e2de40af84ce463f0ac0db4921f9`.
- Initial repair executable and frozen final comparison harness:
  `6698766b6d5f66fb9a4fc74351df3b241c8f0ca4`.
  Daemon SHA256 `1dae8c59959d1b1a90ff4ca86754e0d14b0781d69068a1bde59b0215241efc5f`.
  This binary remains separately preserved after later builds.
- Successor queue-repair executable:
  `f5732576a53d459afdafd9396d07977330c2ecbb`.
  SHA256 `6e8615b90ab164e31b1ada13d84d3ef4225505656e8ee0b218cd57f62a71e5bb`.
  Its executable harness during the measurements remains identical to
  `6698766`; only the protocol document added the predeclared successor
  campaign. The later relay-capture helper correction and this report do not
  change the measured daemon or retrospectively repair retained captures.
- The earlier benchmark handoff `f57e500cc8aa33c175f69df15e1aac40515b3965`
  did not change the baseline C++ implementation.

The executable is experimental **V1 pre-agreement followed by V1 final BLS
certification**, not the isolated Python V2 models. Four independent validator
processes, four seats/3-of-4 quorum, real BLS and signed client actions, normal
synchronous persistence, independent authenticated plaintext FMN2 TCP and an
ordinary engine-off client are retained. B3 advances throughout the workload
using the existing regtest staking/mock-clock fixture. It is not mainnet timing.

The optimized build uses RelWithDebInfo (`-O2 -g`), AppleClang 17, wallet enabled,
GUI disabled, tests enabled, no sanitizer/ccache. Host: M4 Max, 16 cores,
128 GiB RAM, internal APFS SSD, macOS 26.5.2. The five processes share one Mac;
user applications were left untouched, so this is **not dedicated controlled-
load or WAN qualification**. Point load averages before baseline02, candidate01,
fault01 and diagnostic01 were respectively 3.49/4.27/4.35, 4.06/4.20/4.29,
3.70/3.93/4.15 and 1.82/3.05/3.74. No build ran during these measurements.

See the [predeclared protocol](../test/functional/flowmesh_performance_repair_protocol.md)
for commands, fixed seed/rates and observation bounds. Both sides of the initial
matched pair use the same frozen harness, generated HTTPS trust, two TLS relay
legs, fresh connections and full public-body capture. Public capture itself
perturbs scheduling. One client daemon serializes its generated wallets through
the existing backend; this is not many independent trading clients.

The fixed healthy campaign is 12 pilot actions at 0.5/s, ten idle seconds,
two 20-second windows at 0.5/s (ten actions each), then eight simultaneous
actions with a one-second offer window. Arrivals do not wait for completion.
There are two markets, four total taker identities shared across both markets
(eight account/market workers) and funded counterparty
orders. No offered-rate escalation follows a failed latency gate.

## Issue A — authenticated snapshot and reported history

**Cause demonstrated in source/regression:** the public snapshot response
previously fetched the certified payload/state, market status and reported
history under separate runtime-lock acquisitions. A certificate committed
between them could pair authenticated next-sequence N with history entries
whose sequence is N or newer, crossing
the existing client's strict history boundary. This is an endpoint assembly
race, not permission to weaken the client guard.

`e234938` adds an owning `ClientSnapshotView` containing certificate/state/cursor
and reported projection from one market-lock acquisition. Snapshot status is
built from that same projection; updates likewise pair reported data and status.
Service availability and pending-checkpoint observations remain explicitly
separate, unauthenticated observations. The service lock is released before
taking the runtime lock. Certificate, anchor, high-water, stale-history and
account-state verification are unchanged; market identities are never joined
by symbol or assumed to have equal sequences.

The deterministic generated-certificate regression retains snapshot0, commits
sequence1, demonstrates that the old split read has history outside snapshot0's
authenticated range, and checks that the atomic view and retained earlier
owning copy are coherent. This is a negative control of the old assembly
pattern, **not an actual HTTP interleaving capture of the original incident**.

The original `expanded02` error remains an observed defensive rejection whose
raw response was not retained. Its exact historical cause remains unknown.
New public-response analysis found no snapshot/history mismatch or stale-
history RPC rejection in baseline02, both healthy candidates and both fault
runs. Absence here does
not retrospectively identify the old incident or prove all races impossible.

## Issue B — HTTPS amplification

**Measured boundary:** `TradingApi::Admit` rejects before dispatch at the existing
32 requests per peer per second / 128 global requests per second limits. The
error is `Public trading request budget exhausted before admission`, HTTP429.
It is not a chain fee, pagination allowance, proxy quota or certification result.

`7676e5a` coalesces automatic unresolved-action status demand by exact
`(market, ActionId)`, with a bounded 512-key FIFO, rolling 16 actual endpoint
attempts/second, two-attempt burst and smooth replenishment. Failed endpoint
attempts count too. A retained failover cursor avoids repeatedly spending the
allowance on the first unhealthy endpoints. Current local authority is checked
before returning a cached certificate. Nonfresh observations say so explicitly.

Explicit retry still uses the existing bounded endpoint cycle. The redundant
preliminary status lookup in the wallet retry path was removed: one fresh query
precedes one retry of the **same signed object**, never a replacement instruction.
Previously certified actions retain no-resubmit protection. The policy does
not raise/reset server limits, sleep the worker, change signing history or
guarantee headroom when other methods/clients share the same IP. Fair service
requires continued demand; this is not a background progress guarantee.

Focused generated-regtest negative control `poll-baseline-01` failed the new
bound at **59 attempts belonging to RPC intervals wholly contained within a
one-second host window**.
The candidate passed: 244 local calls generated 37 HTTP status requests during
2.25 seconds, maximum 16 by that same bracket-count method. This is not an
exact dispatch-window measurement. Exact dispatch boundaries are also
covered by deterministic C++ tests. Eight-endpoint failover reached endpoint7;
unknown-action retry emitted exactly `action, submit` with unchanged bytes,
ActionId, sequence and initial time. Twenty cached reads and three certified
retry calls generated zero HTTP requests. A genuine cancellation released the
reservation with authenticated balances. Both fixtures' five child exits were
0; the negative-control launcher correctly exited1 and candidate exited0.

## Initial matched result — frozen before the newly found queue repair

`matched-baseline-02` versus `matched-candidate-01`, identical harness6698766.
Both launcher exits were 0, all five children exited 0; each completed 40/40
certificates, authenticated buyer checks, 40 exact signed-byte checks, 18 filled units and
final maker conservation. No follow-up read error occurred. Latency gates fail
even though the correctness fixtures exit successfully.

Milliseconds, empirical nearest-rank; each sustained side contains only 20
samples. p99 equals the sample maximum, **not a dependable tail guarantee**.

| Sustained observable | Baseline p50 / p95 / p99=max | Candidate01 p50 / p95 / p99=max |
|---|---:|---:|
| Original submission call → verified certificate | 484.298 / 2601.131 / 4536.280 | 487.486 / 556.210 / 2766.232 |
| Scheduled offer → verified certificate | 485.361 / 2602.893 / 4536.377 | 488.378 / 557.872 / 2767.193 |
| Admission observation → certificate observation | 373.898 / 483.958 / 4450.590 | 386.175 / 453.123 / 453.123 |
| Submission → authenticated expected balances | 521.895 / 2803.681 / 4689.189 | 532.553 / 753.198 / 2812.526 |
| Submission → every replica observed | 546.270 / 2827.653 / 4691.785 | 536.808 / 828.774 / 2837.610 |
| Last minus first replica observation | 1.686 / 39.356 / 48.960 | 1.633 / 74.859 / 75.196 |

Admission observations cover 20 baseline /19 candidate actions. These are
client/host observations, not internal pool or disk timestamps. Replica reads
are serial upper bounds after the normal durable path: submission→last replica
is **not** additional replication delay. Last−first also includes observer
effects. Certificate inclusion still says `outcome_verified=false`; separate
authenticated expected balance deltas establish these controlled fills.

| Phase | N | Baseline certificate p50 / max | Candidate01 certificate p50 / max |
|---|---:|---:|---:|
| Pilot | 12 | 496.416 / 2842.923 | 442.394 / 2523.372 |
| Sustained repeat1 | 10 | 449.163 / 1532.584 | 485.903 / 2766.232 |
| Sustained repeat2 | 10 | 499.759 / 4536.280 | 520.360 / 556.210 |
| Simultaneous burst | 8 | 2449.878 / 2904.255 | 1953.219 / 4016.138 |

Both sustained windows delivered 0.5 completed actions/s and 0.1 authenticated
filled units/s, with no growing sustained backlog. Both bursts had eight
outstanding at their one-second boundary and zero in-window completions.
Burst offer-start through drain took 3.030194 s baseline /4.099620 s candidate
(2.030194 s /3.099620 s after the one-second offer window), not eight
completions/s.
Candidate repeat1 and burst tails worsened. A single matched pair does not
isolate causal timing effects of each patch or establish statistical tail gains.
**No sustainable rate meeting both p50≤200ms and p95≤600ms was established.**

| Sustained public method | Baseline | Candidate01 |
|---|---:|---:|
| action | 289 | 193 |
| snapshot | 40 | 40 |
| submit | 21 | 22 |
| updates | 19 | 20 |
| Total / offered action | 369 / 18.45 | 275 / 13.75 |

Counts include rejected calls but exclude setup from measured denominators.
Whole-run retained client RPC counts were 830→1594 while HTTP counts fell.
Those include all observed client RPCs, not only status polls; CPU cost and
causal attribution were not isolated. Local RPCs are distinct from public
HTTPS requests.
Whole-run HTTP429s fell 9→0 (baseline: 6 action, 2 snapshot, 1 updates). Both retained
three HTTP200 submit receipts rejected during B3 reconciliation and three
exact-action retries. Explicit pre-admission paused RPC refusals were2→1;
their elapsed time remains included. Success does not mean no intermediate error.

## Successor healthy result — f573257

`matched-candidate-02`, unchanged frozen669 executable harness: launcher and
all five child exits 0. All 40 offers completed certificate, authenticated
buyer-balance and exact signed-byte checks; 18 filled units and maker
conservation passed. No failed/dropped/unresolved action or follow-up read
error was filtered out. All sustained windows advanced B3 on every replica.
The host load sample during early startup was 3.09/3.02/3.02, not an idle-host
guarantee. No simultaneous build/test workload ran.

| Sustained observable, N=20 | Successor p50 / p95 / p99=max (ms) |
|---|---:|
| Original submission → verified certificate | 477.264 / 882.312 / 2953.499 |
| Scheduled offer → verified certificate | 481.349 / 882.761 / 2954.612 |
| Original submission → admission observed | 89.806 / 131.642 / 207.848 |
| Admission observed → certificate observed | 388.053 / 786.214 / 2864.824 |
| Submission → authenticated expected balances | 517.235 / 926.060 / 2989.885 |
| Submission → every replica observed | 527.635 / 954.617 / 2992.021 |
| Last minus first replica observation | 1.558 / 39.552 / 54.175 |

The target still fails both median and p95. Sustained offered/completed rate
is 0.5 actions/s, authenticated fills 0.1 units/s, with no sustained queue
growth. This is an observed rate, **not a rate qualified to the target**.
No higher-rate escalation was attempted. Repeat1 certificate p50/max is
505.769/2953.499 ms; repeat2 is 448.453/592.146 ms. The initial candidate01
had a better sustained p95; small sequential runs do not isolate patch causality.

The eight-action burst is explicitly worse: p50 5319.062 ms, p95=p99=max
7226.854 ms, eight outstanding at the one-second boundary, zero in-window
completions, queue-growth flag true. All eight eventually completed. Offer-start
through drain was 7.357928 s (6.357928 s after the offer window), or 1.087263
drained actions/s. This is not a demonstrated latency gain from the queue fix.

Sustained HTTPS counts: action203, snapshot40, submit20, updates22, total285
or **14.25/offered action** versus baseline18.45 and candidate01's13.75.
Whole-run656 HTTP rows were HTTP200, but two reconciliation-rejected receipts
each required one exact-action retry (pilot sample2, burst sample36). Two
pre-admission cancellation RPCs were refused as paused (samples14/26).
Their time remains included. HTTP429s0; local client RPC rows1638 are separately
counted. Public analysis found no coherence/stale-history issue in655 full
requests/54 entry identities. Recorded archive loss counters were zero and
finalization quiescent; the frozen pre-parse body-read caveat still applies.

## Separate fault result on 6698766

`fault-candidate-01`: 30/30 complete, 12 authenticated fills, 30 exact-byte checks,
zero failed/dropped/unresolved actions and zero follow-up read failures.
Launcher 0; all final children 0 and original node3 graceful-stop 0.

| Phase | N | Certificate p50 / p95=p99=max (ms) | Required replica observations |
|---|---:|---:|---|
| Before fault | 12 | 478.175 / 2982.529 | All4 |
| Node3 offline | 6 | 423.530 / 2511.806 | Nodes0/1/2 |
| BULK held | 4 | 449.027 / 2757.338 | Nodes0/1/2 |
| BULK throttled | 4 | 415.142 / 2527.969 | Nodes0/1/2 |
| Recovered/rearmed | 4 | 422.709 / 486.578 | All4 |

All phases completed 0.5 actions/s without growing backlog. HTTP429s 0; two
reconciliation refusals/two exact retries and one paused pre-admission RPC
refusal remain recorded. No observed stale-history mismatch.

Node3 reopened its same generated keys/journals, unarmed. Mesh authentication
was observed 841.263 ms after restart; held data still left the offline target
missing 8.995 s after restart. After BULK release, fourteen action certificates
(eleven distinct sequences 19..29) were observed together 12.355 s later. Exact
target state/catch-up readiness was observed 21.351 s after restart. Signing
eligibility followed explicit rearm 7.045 ms later. **An accepted returning
signing share was not proved** in this logging-off run. Connectivity, catch-up,
eligibility and actual signing are distinct. Failed proposer/hostile ingress,
crash/power-loss recovery and WAN behavior remain outside this run.

## Successor fault result — f573257

`fault-candidate-02`: launcher exit 0, all five final children exit 0, original
node3 graceful-stop exit 0. All 30 original requests completed certificate,
authenticated expected-balance and exact signed-byte checks. Twelve filled
units and final maker conservation passed. No failed, dropped, unresolved or
failed-follow-up-read action was excluded. This is deliberate fault testing,
not another healthy latency sample (`performance_pass=null`).

Milliseconds, empirical p50 / p95=p99=max; only 4–12 samples per phase:

| Phase | N | Certificate | Expected balances | Required replicas |
|---|---:|---:|---:|---:|
| Before fault | 12 | 415.794 / 489.096 | 459.395 / 521.943 | 463.737 / 658.328 |
| Node3 offline | 6 | 708.226 / 6294.780 | 753.950 / 6338.181 | 755.854 / 6340.342 |
| BULK held | 4 | 387.959 / 2518.498 | 427.094 / 2562.480 | 429.153 / 2564.587 |
| BULK throttled | 4 | 385.679 / 2518.207 | 461.082 / 2572.546 | 467.652 / 2574.743 |
| Recovered/rearmed | 4 | 458.781 / 536.787 | 499.874 / 577.970 | 507.328 / 580.888 |

Required replicas during the middle three phases are nodes0/1/2, not offline
node3. All fourteen middle-phase action sequence/hash pairs were subsequently
observed in node3's exact applied history, so all thirty actions eventually
reached all four replicas. Those separate return observations are not invented
simultaneous four-replica latency: offline actions were observed at node3
56.829–66.830 s after their original calls, held-phase actions 45.921–49.501 s,
and throttled-phase actions 37.225–43.230 s.

The offered rate was 0.5 actions/s throughout. Before/after phases completed
all offers in-window. Offline completed five of six during its 12 s window;
held and throttled each completed three of four during their eight-second
windows. All drained without failures; elapsed-through-drain rates were
0.367/s, 0.467/s and 0.466/s respectively. The declared sustained queue-growth
metric remained false, but the outstanding actions at window boundaries are
not erased by that flag. B3 advanced during every phase.

The returning same-datadir node was initially unarmed. Restart→mesh observation
was 850.460 ms; restart→exact catch-up readiness was 52.878 s, including
43.331 s after BULK release. Throttling was 2048 bytes/s per direction and
recorded 106642 delayed bytes; path attribution remains unproved. Explicit
rearm→eligible observation took 22.064 ms. No accepted returning signing share
was proved (`signing_share_observed=false`); do not equate eligibility or
replication with an accepted vote. No journal reset or replacement key was used.

There were 584 HTTP observations: action417, markets11, snapshot73, submit37,
updates41, vault_operation5. One markets call preceded capture; 583 complete
public body pairs were retained. One HTTP200 submission refusal during B3
reconciliation required one exact-byte retry (sample14), with the original
clock unchanged. Two explicit local pre-admission RPC refusals remain recorded
(samples1/22, market paused). No HTTP429/non-200 response was reported. Capture
loss counters were zero and finalization quiescent; the unchanged frozen
pre-parse body-read limitation still applies. Public analysis found no
coherence/stale-history mismatch. Account checks and exact-byte checks are
separate from this non-cryptographic offline analysis.

## Retained diagnostic failure and worker evidence

`diagnostic-candidate-01` on 6698766 deliberately enables BENCH logging and is
not pooled into headline latency. Forty offers produced 38 certificates and
38 authenticated account checks; two burst actions missed the 60 s deadline.
Seventeen buyer-side fills were proven. Final maker conservation was skipped
after the failure; that is not an observed balance mismatch. Launcher 1;
all five children 0 and harness threads stopped. Three intermediate
reconciliation refusals and one paused pre-admission RPC refusal remain
recorded. HTTP200 did not mean every application request succeeded.

Samples 33/35 each submitted one 279-byte signed action. Their public traces
contain 426/427 subsequent status reads, all HTTP200 and still `queued`, never
observed admitted or certified. No retry/re-sign was observed. They lack a
successful final wallet-outbox byte comparison, so that check is not claimed.

On node0, both wrappers carried head27. Sequence27 durably applied and the
runtime entered28 before these messages were dequeued. Queue waits were
638.238 ms and 932.758 ms. The logged message gate was open, but ACTION is
noncritical and this marker does not prove B3 authority was available.
Processing lasted tens of microseconds; `HandleAction`'s stale-header return occurs before decoding
or pool admission. The return site was not separately logged: this is a
source-backed reconstruction from exact wire identity, entry/head events and
dequeue timestamps, not a directly logged rejection reason. No pool admission
or certificate for either exact action appears in retained replica evidence.
This demonstrates a local queue/head race, not a BFT timeout or missing quorum.

`f573257` repairs that measured dependency, without adding a wake-up or changing
worker scheduling. Only a trusted local queue item overtaken by exactly one
certified head can refresh its outer routing sequence. The complete intervening
body must be the retained, authenticated durable head and must not already
include this action. The original signed payload/ActionId/account sequence are
unchanged. Pending-state, current authority/anchor, deposit facts, signature,
pool capacity and sequence-conflict checks still apply. Remote messages do not
gain stale-header acceptance. Wrappers spanning multiple heads or missing
required evidence receive explicit refusal: automatic recovery is deliberately
not claimed there; existing exact-action status/recovery is required.

The test-only `3ada2ed` regression failed before the repair at the intended
missing pool-admission assertion (exit 201). A bounded worker barrier lets the
other three real seats certify, then releases a previously queued local action
behind the certificate-priority lane. The final test requires unchanged bytes,
all-four durable certified application, and no new request/timer advancement.

Review found a second edge in the uncommitted draft repair: `StillPending`
alone does not exclude a certified action whose execution was rejected. The
final head-body guard refuses it regardless of consumed-deposit/nonce state.
A separate genuine quorum-BLS certified-recovery fixture checks this with an
overflow-rejected deposit; it is not a claim that the honest leader normally
selects that body. An earlier attempt timed out before obtaining its certificate
and remains failed fixture evidence, **not** a reproduced edge assertion.
The final test also checks a valid multi-head-old instruction is refused;
the missing-head branch is source-reviewed, not directly injected because
normal initialization/head advancement establishes that invariant.

The new worker, runtime, agreement and store spans separate queue admission,
notification, dequeue, market-lock acquisition, reconciliation gate, execution,
nested agreement validation/sign/persist/publication and synchronous store work.
Spans are inclusive/overlapping: do not sum parent and child durations. A CV
notification→return bracket cannot separate OS scheduling from mutex reacquisition.
Synchronous `WriteBatch(true)` is not a pure device-fsync measurement.
The original baseline's 1.282932 s interval remains historically unattributed;
new analogues cannot retroactively supply missing timestamps.

Representative diagnostic brackets, in milliseconds; these overlap rather
than partition total latency. Node 0's queue joins use the exact local wire
identity, and pool-to-durable joins use the exact included action.

| Sample | Client certificate | Local queue residence | Node 0 pool → durable application |
|---|---:|---:|---:|
| Normal 0 | 525.745 | Not selected for this example | 471.606 |
| Slow 23 | 6217.024 | 530.011 | 3761.796 |
| Slow 26 | 1363.954 | 373.656 | 772.100 |
| Burst 37 | 4017.733 | 787.020 | 2355.404 |
| Failed 33 | No certificate within 60 s | 638.238 | No admission observed |
| Failed 35 | No certificate within 60 s | 932.758 | No admission observed |

Normal sample 0's measured tick was 112.853 ms, containing candidate submission
87.651 ms and its nested pump 67.477 ms. Its later durable append took 8.424 ms,
including execution 0.402 ms, certificate validation 0.629 ms and synchronous
database call 7.367 ms. These are elapsed scopes, not exclusive CPU costs.
For sample 23, 1043.573 ms elapsed between first execution completion and first
observed stage-1 publication, with explicit unavailable/reconciling refusals
inside that interval. Its largest retained tick was 80.364 ms, containing
timeout 75.148 ms and pump 63.405 ms; append was 8.526 ms including database
7.566 ms. Neither observation justifies assigning the entire gap to BLS,
disk, the network or a protocol timeout. The worker was already busy at the
two failed enqueues; another notification would not repair their stale wrapper.

Service timing spans exhausted their fixed diagnostic cap before the pilot
on all four nodes. The diagnostic's local-RPC archive hit its 256 MiB reserved
capacity, retaining 7710 rows and dropping 7646: complete capture correctly failed.
Public HTTP bodies retained 1592 requests without recorded loss; the loss of RPC
rows is not erased by complete HTTP indexing. Worker/runtime/agreement/store
count caps were not hit, but no marker alone proves an uncensored log. Missing
segments remain unknown rather than zero. No limit was increased to get a pass.

## Regression/review and evidence limits

### Successor diagnostic: safety refusal is not automatic progress

`diagnostic-candidate-02` on f573257 also exits 1, all five children exit 0:
38/40 actions certified and balance-checked; burst samples36/37 missed their
original 60 s deadline. No failed follow-up balance read occurred. Final maker
conservation was skipped after failure, not observed violated. The RPC archive
dropped 6220 rows at the unchanged bound; the diagnostic capture is incomplete.
No failed sample is replaced or removed from denominators.
There were 1551 HTTPS observations, 1550 with full body pairs, and no recorded
public-body loss or HTTP429. Forty original measured submissions plus one
exact retry were retained, including both failed actions. Three local
pre-admission RPC errors remain recorded. Offline public analysis found no
coherence/stale-history mismatch; it cannot make the incomplete RPC archive
complete or convert those two deadline misses into successes.

Unlike the first diagnostic, the server did not silently drop these local
actions. Both seq19 wrappers were dequeued after seq19 committed. The retained
head check passed, but the new guard explicitly refused them while B3 authority
was unavailable: `local queued action cannot refresh its header while current
authority is unavailable; retry exact instruction`. One action had 428 rejected
status replies after two queued polls; the other had 429. The service gate
closed at node0 monotonic1408935115067, covering both dequeues
1408935120789/1408935121146, and reopened at1408935144057. Processing was
49/46 microseconds, not a 60-second worker wait or BFT timer.

The existing client deliberately preserves queued/admitted uncertainty after
a possibly delivered instruction when one endpoint says rejected/unknown
(`RemoteBackend::QueryAction`). It updates the reason but does not treat this
as global proof of non-inclusion. Retained client RPC rows2032/2034 confirm
queued receipts carrying that exact endpoint-refusal reason (the latter also
says the status was coalesced rather than fresh). The harness's automatic exact-retry predicate
does not retry a receipt that remains queued. No automatic resubmission or new
instruction occurred. This is an explicit boundary: **the one-head repair is
not general autonomous liveness across reconciliation**. Future bounded
deferral or explicit exact-instruction recovery needs its own policy/tests;
neither uncertainty, authority checks nor original timeout was weakened here.
The previously tested explicit retry RPC remains separate from this failed
passive-polling scenario. A helper note or smaller error count is not completion.

- The initial diagnostic patch crowded the operational event ring and broke
  `normal_and_restarted_proposer_round_deadlines`; that failed test/log remains.
  `1350d14` moved added spans into a separate bounded BENCH stream instead of
  increasing operational retention. Two new regressions protect the separation.
- Eight focused C++ suites passed, 138 cases, after that correction. Later
  commits through 6698766 changed only Python analysis/tests/docs, not these
  tested C++ sources. Seventy offline trace/finalization/parser checks passed.
  With the final relay helper regression at `30001bc`, all 77 offline checks
  pass; this helper-only successor leaves C++ source identical to `f573257`.
- Successor `f573257` adds three runtime cases (141 total focused C++ cases).
  A sandbox-only run passed runtime72 but could not bind test HTTPS sockets;
  it remains an environment-blocked result. The authorized combined run passed
  seven of eight suites, with one existing
  `normal_and_restarted_proposer_round_deadlines` two-second `WaitForIdle`
  failure. Its isolated repeat passed with less output logging, so that is not
  a matched-condition rerun. All three new queue cases passed.
  The combined suite is **not reported green**: the intermittent idle deadline
  remains unresolved. Logs show current-header deposit admission and eventual
  durable certification on all four replicas; the new stale-header path was
  not needed. Store-call elapsed spans reached roughly 218–892 ms in that run;
  logger emission lag is also visible (two append completion clocks 5 µs apart
  have printed wall-time prefixes 224.452 ms apart). Disk, logger and host
  scheduling contributions were not separated. Neither
  the timeout nor production timers were loosened to pass.
- `matched-baseline-01` preceded a detected finalization gap. It is retained
  as pre-finalization evidence, not substituted for the final baseline02.
  `3526d64` closes relay admission, waits for bounded capture quiescence,
  preserves primary failures and makes late capture failures fail exit.
- Review found another narrow frozen-harness limit: `rfile.read` may raise
  before recording an incomplete body. Thus 6698766's closure claim applies
  to recorded/parsed requests, not every partially received HTTP connection.
  No such fault was observed in the matched pair. Later fixes cannot repair
  its missing observations retrospectively.
  The separate final helper correction records an incomplete body on
  `OSError` and rethrows the identical exception; it does not forward a partial
  request, invent a response or change the request limit. Seven new offline
  tests (15 subcases) fail before with 12 failing subcases and pass after.
  They cover healthy/fault finalization, sanitized errors, unchanged exception
  identity and disabled tracing. A quiescent archive with that failure is
  still incomplete and must fail qualification. This scope begins after a
  valid Content-Length; earlier TLS/header failures are not newly captured.
- Read-only code reviews found no additional production client/disabled-
  instrumentation blocker. They are scoped engineering review, not independent
  security qualification of the whole protocol.

Raw generated wallets, TLS private keys, databases and private traces stay out
of Git. Reports preserve scoped hashes/counts and repository-relative commands.
All previously frozen history remains intact.

The final read-only engineering review checked the report against retained
results and the newly reachable source/history through `30001bc`; no additional
publication blocker was found. This is not a full protocol security audit.
Publication is limited to the existing `flowmeshV2-dev` branch, no PR, tag,
release or deployment. Its existing GitHub workflow runs the isolated Python
models only. A green model CI result would not replace the C++/real-process
results above or erase their failures. The pushed revision and actual CI run
are reported separately after remote verification.

## Reproduce the scoped checks

Configure an optimized, wallet-enabled, GUI-disabled build with C++ tests, then
build `b3coind`, `b3coin-cli`, `b3coin-wallet` and `test_bitcoin`. The HTTPS
tests require permission to open loopback sockets.

```sh
ctest --test-dir /absolute/build \
  -R 'flowmesh_(client|runtime|agreement|production_store|https)' \
  --output-on-failure -j4

(cd test/functional && python3 -B -m unittest -v \
  flowmesh_public_trace_test flowmesh_performance_metrics_test \
  flowmesh_performance_fault_metrics_test \
  flowmesh_performance_worker_analyze_test flowmesh_public_consistency_analyze_test \
  flowmesh_relay_read_failure_test)
```

The [experiment protocol](../test/functional/flowmesh_performance_repair_protocol.md)
contains exact fixed healthy/fault/diagnostic commands and observation limits.
The focused polling regression is
[`feature_flowmesh_client_poll.py`](../test/functional/feature_flowmesh_client_poll.py);
select the matching daemon/CLI/config using the same environment pattern and
a fresh generated directory. The regression before the queue fix is
`flowmesh_runtime_tests/preagreement_local_queued_action_survives_certified_head_advance`
at test-only revision `3ada2ed`, where failure at the named admission assertion
is expected. Never run these fixtures against an existing or live datadir.

Run the offline analyzers from the repository root. They consume retained
generated-fixture evidence and refuse to overwrite their output files:

```sh
python3 -B test/functional/flowmesh_public_consistency_analyze.py \
  /absolute/retained-fixture --output /absolute/new-consistency.json
python3 -B test/functional/flowmesh_performance_worker_analyze.py \
  /absolute/retained-diagnostic --output /absolute/new-worker.json
```

Their public entry-prefix parser is not a BLS verifier. Client acceptance and
authenticated result checks come from the actual client/runtime, not these
offline summaries. Diagnostic spans are nested; missing samples are not zero.

## Architectural boundary

Reusable results are coherent client snapshots, bounded automatic polling,
exact-action recovery/no-resubmit protection, safe one-head local rerouting,
and worker observability. Remaining
V1 agreement/final-certificate layering, serial worker validation/persistence,
multi-client scaling and genuine V2 integration are not redesigned here.
No timer/quorum/signing/fee rule has been changed to improve a number.

The concrete V1 dependency is `FlowMeshRuntime::FinalizeAgreement`: even after
the pre-agreement module has durably decided, the runtime retrieves and validates
the candidate, retains it before signing, takes the separate unchanged V1
signing lock, emits V1 BLS attestations, assembles the V1 certificate and
durably applies the entry. These are two agreement/certification layers, not
one socket round trip. Removing or fusing them would alter the V1 boundary and
requires the separately approved V2 cryptographic/commit/storage integration.
The present measurements do **not** prove that every remaining millisecond
is fundamental or establish a lower bound for another design. Reconciliation
deferral/exact-action recovery and serial worker cost remain separate concerns.
No further legacy protocol redesign or future V2 performance claim is made.

First transmission time, isolated wallet-sign/outbox cost, pure fsync, isolated
client proof verification and Qt presentation remain unmeasured. This milestone
does not qualify V2 real crypto/storage integration, membership/PoS V2, custody/
unchanged bridge, futures margin/oracles/liquidation, live V1 lock recovery,
power-loss safety or 200 ms WAN operation.
