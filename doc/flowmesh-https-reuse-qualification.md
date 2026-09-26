# Bounded HTTPS connection reuse — local qualification

Date: 2026-09-27 (Asia/Kolkata). Branch: `flowmeshV2-dev`.

Implementation: `c81cdb2bc270db0f4721c55cb15e759c588d8202`.
Unmodified control: `e4f83139e637ff1d291118cd705130d2855ea5b7`.
This is a client/API transport change, not native V2 consensus integration.
No quorum, matching, signed format, accounting, fee or settlement rule changed.

## Implemented change

The remote backend retains one sequential, verified HTTPS connection under
its existing exclusive work gate. Per-request uncertainty is recorded before
dispatch, including on a warm connection. A lost response does not trigger a
hidden economic retry, replacement instruction or cancellation. Exact-action
and certified/no-resubmit handling remain in the existing backend.

Callbacks have session-owned lifetime; exceptions dispose of in-flight state
before stack contexts disappear. Reuse checks endpoint identity, pin, explicit
CA content, certificate expiry and connection state. Explicit reconnect drops
the session and verifies trust afresh. Reuse is bounded by idle time, age and
request count. Partial/ambiguous TLS state and peer-requested closure prevent
reuse. There is no redirect, plaintext fallback, TLS-verification bypass or
request pipelining.

The server parks idle sessions in a bounded readiness owner instead of tying
up a handler worker. Queued, active, idle and closing connections all count
toward admission. Each request still uses strict length/framing rules, bounded
rejection cleanup and an absolute deadline including ready/queue waiting.
Default worker/connection/queue limits are unchanged. Active TLS/read/write
and application handlers still use workers: this is not a fully asynchronous
server or a 100-client load qualification.

## Tests actually executed

- Before-fix two-request regression: three failed assertions (connection
  closed, second response missing, expected handler call absent). A separate
  initial sandbox socket denial is not counted as this reproduction.
- Native HTTPS/work-gate/poll/evidence: 49 cases and 5,700 assertions passed;
  1,682 unrelated cases skipped. Includes 24 HTTPS cases for warm response loss,
  exact request preservation, trust replacement, strict second-request bounds,
  malformed/pipelined input, deadlines, capacity, idle shutdown and reopening.
- Headless Qt workspace: 167 passed, 3 optional exports skipped. Trading:
  25 passed. Panel: 5 passed. Executable exits zero. These are not native-screen
  or attended application-Quit qualification.
- Isolated read-only reconnect fixture: passed with seed
  `8624845788586056180`; two daemon children and framework exited zero.
  Verified unavailable/busy/failover/malformed response, CA/pin rejection,
  locked generated wallet and same-wallet reopening. Only market reads sent.
- New matched-trade fixture smoke: one real unit fill, verified account deltas
  for both participants and matching execution history on all four native
  replicas. 24 of 24 captured HTTPS calls reused TLS, including the original
  buyer submission, with zero new handshakes. Five child exits zero. Concurrent
  compilation means this smoke is correctness evidence, not a latency arm.

The smoke first exposed two fixture assumptions: wallet-scoped RPCs after
loading a second generated wallet, and a history helper expecting a state-root
field absent from the target receipt. These were corrected in the test, not by
weakening wallet attribution or proof checks. The failed run and its clean
child exits remain in private evidence.

## Matched-trade comparison

Both arms passed with the same fixture SHA256
`02c467ceb0ccb265803d39b7aa6dad05c51a62fbfdef1bb162023a8dfd476700`
and seed `260927`. Baseline ran first, then persistent, without concurrent
compilation or another trade workload. All ten daemon children exited zero;
both frameworks exited zero. B3 heights advanced 201 to 218 in the baseline
and 201 to 215 in the persistent arm. Exact block/action interleavings differ;
the seed and price schedule do not force an identical operating-system schedule.

Host: Apple M4 Max, 16 CPUs, 128 GiB, macOS 26.5.2; arm64 Release build,
Apple Clang 17, C++ `-O2`. Existing desktop applications, including the retained
regtest Qt process, were not shut down. This is a sequential small-sample local
comparison, not a dedicated-host performance certification.

Executable SHA256s:

- Control: `c94c2e8fbc37111a2af4fc4a1c2640d44ebecacc98f88ea1c066853d104b4426`.
- Repaired: `08340765b3a30ebe6a5e0df6ff63b42d14f6e9e767fa01a85536213574d444b1`.

| Observed arithmetic median, milliseconds | Control | Persistent |
| --- | ---: | ---: |
| Original buyer HTTPS submit only | 2.071 | 0.695 |
| Original buyer RPC to verified certified inclusion | 381.0015 | 377.274 |
| Original buyer RPC to both authenticated account observations | 427.741 | 428.238 |
| Original buyer RPC to all four native history observations | 431.485 | 430.1725 |

These boundaries overlap; do not add them. Arithmetic median averages the
middle two observations. The retained harness's `p50_ms` uses nearest rank
instead (certification: 368.588 and 359.875 ms). Do not interchange definitions.
Certification observations in actual request order:

- Control: 368.588, 357.822, 393.415, 466.614, 352.084, 416.511 ms.
- Persistent: 359.875, 398.768, 350.917, 352.311, 394.673, 460.990 ms.

For six samples, nearest-rank p95 is simply the maximum: 466.614 / 460.990 ms.
All samples exceeded 200 ms. **A meaningful end-to-end improvement is not
established.** Eliminating local handshakes saved roughly 1.38 ms on the original
HTTPS submission, not hundreds of milliseconds on the complete trade path.

All 170 captured control HTTPS requests performed fresh handshakes; all 146
persistent requests reused verified TLS with zero new handshakes. Every
measured original buyer submission was individually linked to its HTTPS span.
Polling counts varied, so total request counts are not a fixed-load throughput
comparison. Each arm matched six units and accrued 47,500 B3 fee atoms under
the unchanged test market rules. No duplicate fills, credits or sequences.

One control maker setup request was refused during B3-tip reconciliation and
used one explicit exact-ActionId retry: initial response at 58.679 ms, retry
completion at 1,087.197 ms, certification at 1,382.225 ms from its original RPC.
It preceded that pair's measured buyer RPC. All measured buyers had no retries
or pre-admission refusals; all twelve persistent-arm orders submitted once.

Same-node admission-to-certification medians remained 305.7325 / 291.336 ms,
the largest currently observed interval. Three sequential client outbox syncs
nested within original submission totalled median 17.7855 / 16.270 ms; final
action proof verification was about 3.24 ms. These are overlapping subspans,
not an additive explanation of the end-to-end median. No persistence or quorum
rule was weakened. Remaining runtime/wake-up attribution is not resolved merely
by observing a large certification interval.

### Representative runtime trace: where time actually went

In persistent pair 0, on node0 only, admission-to-certification was 271.936 ms.
Offsets below share that node's monotonic clock and the exact buyer ActionId;
they are not subtracted from Python or other-node timestamps.

| Offset from admission, ms | Observation |
| ---: | --- |
| 46.665 / 46.669 | First proposal enqueued / dequeued |
| 49.618–50.181 | Candidate execution, 0.563 ms |
| 202.047 | Pre-agreement decision durable |
| 219.440 / 219.893 | Candidate lock durable / local attestation signed |
| 247.738 / 259.045 | Two remote attestations verified |
| 264.077 | Certificate formed |
| 271.929 / 271.936 | Execution durably applied / action certified |

Eight disjoint agreement-journal synchronous-write spans totalled 97.550 ms.
Their reasons were candidate, proposal, PREPARE intent, signed PREPARE,
prepared evidence, COMMIT intent, signed COMMIT and decision persistence.
Separate candidate-lock and execution-append writes took 14.537 and 5.512 ms.
Their union covers **117.599 ms** of this one 271.936 ms interval. This is
synchronous API wall time, not a measurement of kernel fsync alone.

An incoming agreement message waited 52.606 ms from enqueue to dequeue while
the worker was busy. Attestations waited 29.871 and 28.058 ms. These overlap
the writes and other worker activity; adding them would double-count delay.
The first proposal's local queue wait was only 0.004 ms. The preceding remote
work/transport gap is unattributed, not evidence of a local polling defect.

Code anchors: `src/node/flowmesh_agreement.cpp::Persist` builds the journal
batch and uses synchronous `CDBWrapper::WriteBatch`; `src/dbwrapper.cpp`
delegates to LevelDB. Agreement signing preserves intent before signing and
the exact signed object before publication. The runtime worker uses a wakeable
condition-variable predicate; its ticker constant alone is not causal evidence.

One recommended next measurement: separate database/WAL work from actual
storage-sync time inside the existing agreement write span, and record encoded
batch size plus queued-message age. That distinguishes disk-barrier cost from
serialization/growing-state work before a repair is proposed. Do not remove
durability barriers, shorten protocol timers or parallelize state mutation to
make a latency result look better.

The fixture uses four generated, colocated regtest operators plus one ordinary
engine-off client process with two separate generated wallets. Six standing
asks are individually matched by six equal-price bids through direct native
HTTPS, while B3 advances. There is no close-after-each-request TLS relay. Both
arms use the same fixed price schedule and seed. Original buyer submission time
is retained across any pre-admission or exact-action retry.

Measurement boundaries are reported separately: initial response; verified
inclusion; later authenticated account observations; all-native-replica history
observation. The last is not an extra replication delay. Inclusion alone does
not prove a fill; endpoint-reported history remains labelled as such. Native
clearing history and both authenticated account changes establish these test
fills. Later reads and polling are part of the observed client path, not a
consensus-only stopwatch. Six samples cannot establish a tail-latency SLA.

## Transport-only observation

Sixteen paired loopback, 128-byte echo requests: cold median 1.147708 ms;
warm median 0.213 ms; sixteen cold handshakes and zero warm handshakes.
This is not trading, consensus, WAN or Qt latency. It cannot explain the prior
4.398-second Mac-to-VPS six-fill median by itself.

## Reproduce in disposable directories

From a configured build, with test dependencies and loopback sockets permitted:

```sh
cmake --build build --target b3coind test_bitcoin -j 6
build/bin/test_bitcoin --run_test=flowmesh_https_tests,flowmesh_client_work_tests,flowmesh_client_poll_tests,flowmesh_client_evidence_tests --report_level=detailed
python3 test/functional/feature_flowmesh_client_reconnect.py --configfile=build/test/config.ini --randomseed=8624845788586056180
python3 test/functional/feature_flowmesh_persistent_trades.py --configfile=build/test/config.ini --transport-arm=warm --randomseed=260927 --nocleanup
```

For the control, build the exact unmodified control revision separately and
run the **same new fixture** with its `--configfile` pointing to that build and
`--transport-arm=baseline`. Do not modify the control transport or compare a
different historical workload. Run arms sequentially without concurrent builds.
The fixture records binary/fixture hashes, arguments, timings, ActionIds,
retries, proof/account/replica assertions and actual child exits. Retained
directories contain generated test wallets and private test TLS material;
do not publish them as source artifacts.

## Existing remote regtest price exercise

Six additional 0.25-rUSD fills produced prices 5, 6.6667, 9.0909, 8, 6.25,
5 rUSD/B3. All four native replicas and account changes were checked. The
first standing maker instruction survived a temporary client-readiness refusal
and was used unchanged; no replacement deposit or instruction was created.
The existing running binaries were not upgraded. This campaign overlapped
build work and is explicitly **not** a performance comparison.

These six fills add 1.5 rUSD turnover, not six new deposits. Actual fees remain
23,625 B3 atoms in total (0.000023625 B3), under existing rules. Accrual is not
proof of reward payout. rUSD is an unbacked, valueless regtest asset; this was
not market-making or price manipulation on mainnet.

## Review and remaining limits

A separate bounded read-only review checked callback lifetime, exception
cleanup, libevent current-request retry behavior, warm-request uncertainty,
server ownership and shutdown. It found no concrete blocker in that scope;
it ran no tests and is not a formal independent security audit.

No tester/VPS/Qt upgrade, GitHub push, release or deployment is part of this
stage. Native screen/Quit/reopen and Windows runtime checks remain pending.
No 200 ms, WAN, high-concurrency or physical crash/power-loss guarantee.

The backend still serializes full methods. Two request lanes, asynchronous
state ownership and live result delivery are separate unimplemented stages.
OS trust-store replacement needs reset/reconnect or connection age expiry;
filesystem reads and admitted application handlers are not interruptible.
One request deadline is not a whole-application shutdown guarantee.

Time candles remain a separate [investigation](design/flowmesh-chart-time.md).
The current records lack verified per-trade execution timestamps; sequence
buckets must not be relabelled as minutes. The historical accessibility crash
also remains unresolved, unrelated to these transport test passes.
