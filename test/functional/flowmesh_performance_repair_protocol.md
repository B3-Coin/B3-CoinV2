# Predeclared matched performance-repair experiment

Freeze this harness revision before either run. Run the existing frozen
baseline executable once, then the candidate executable once, sequentially on
the same host. Both runs use this **same corrected harness**, fresh generated
regtest directories/keys/wallets, the same seed, and identical options. Never
modify the frozen baseline executable, evidence, or its private directory.
Record each executable SHA-256, source revision, build configuration, harness
revision, actual command, host/load context, complete stdout/stderr and process
exit. A harness-only commit is not evidence that the executable was rebuilt.

## Production-log matched pair

The runner substitutes the absolute paths below from the existing build and
artifact manifests; each fixture directory must be new. `BITCOIND` selects the
daemon directly; `BITCOINCLI` selects its matching CLI. Ensure `BITCOIN_CMD` is
unset, because that optional framework wrapper otherwise overrides binaries.

```sh
env -u BITCOIN_CMD BITCOIND=/absolute/frozen-baseline/bin/b3coind BITCOINCLI=/absolute/frozen-baseline/bin/b3coin-cli \
  python3 -B test/functional/feature_flowmesh_performance.py \
  --configfile=/absolute/frozen-baseline/test/config.ini \
  --tmpdir=/absolute/new-evidence/matched-baseline-02 --nocleanup \
  --randomseed=24092026 --portseed=92401 \
  --performance-profile=matched-repair --performance-action-timeout=60 \
  --performance-public-trace --performance-read-recovery

env -u BITCOIN_CMD BITCOIND=/absolute/candidate/bin/b3coind BITCOINCLI=/absolute/candidate/bin/b3coin-cli \
  python3 -B test/functional/feature_flowmesh_performance.py \
  --configfile=/absolute/candidate/test/config.ini \
  --tmpdir=/absolute/new-evidence/matched-candidate-01 --nocleanup \
  --randomseed=24092026 --portseed=92401 \
  --performance-profile=matched-repair --performance-action-timeout=60 \
  --performance-public-trace --performance-read-recovery
```

Do not run the commands concurrently. The profile fixes two fresh markets,
four buyer accounts (eight independently queued account/market workers), and
one funded maker account. It retains actual fills, standing bids, signed
cancels, exact signed-byte retry checks and buyer/maker conservation checks.
One engine-off client daemon still serializes its wallets through its existing
backend; this is not a multi-client-process or WAN experiment.

The plan is exactly 40 offered actions: 12-action pilot at 0.5/s over 24 seconds;
10-second idle; two 20-second windows at 0.5/s, ten actions each; eight-action
simultaneous burst with a one-second offer window. Arrivals are scheduled from
the original monotonic plan, not prior completion. Pilot and burst are labeled
separately from the 20 sustained actions. There is no automatic rate escalation
in this profile, no rerun-until-green, and no exclusion of failed/dropped offers.
Do not add a higher-rate run unless a separate plan is approved after this
pair meets the target without correctness failures or persistent queue growth.

All five daemons must report zero enabled debug categories. BENCH logging is
not a production headline sample. Primary latency remains original wallet RPC
call to first client-verified certificate inclusion, with p50 <= 200 ms and
p95 <= 600 ms. Also report scheduled-offer-to-certificate latency, worker queue,
admission observation, authenticated expected-balance time, exact replica
observations, all offered/completed/failed counts, in-window versus drained
throughput, and useful authenticated fills. Certificate inclusion alone is
not execution-result verification (`outcome_verified=false` remains explicit).
Nearest-rank p99 from 20 sustained samples is a small-sample maximum, not a
reliable tail guarantee. Performance misses are reported even when the default
process exit permits a latency miss; correctness failures always fail exit.

### Predeclared finalization amendment

`matched-baseline-01` began with frozen harness `8a4be3a` before independent
review identified a finalization gap: active relay handlers/body writes could
be omitted from the final archive, and a failure first observed in the final
report could leave the process exit or fault pass flag green. Preserve that
run, all original failures, raw artifacts, commands and exit status unchanged.
It is a pre-finalization observation, **not** the baseline of the final matched
pair; do not delete it or use it to select a favorable sample.

Freeze the corrected harness revision before running `matched-baseline-02`
and `matched-candidate-01` once each, with the same predeclared workload and
options above. This replacement is for the identified observation-integrity
defect, not a latency/result retry. After workers stop, corrected finalization
closes relay admission and waits at most 15 seconds for active handlers,
unarchived/pending records, body writes and wallet RPC observations to drain.
It reports the actual pending counts and retains every loss/error counter.
Live reports are explicitly provisional; complete capture requires a finalized
quiescent archive. A timeout, late write failure, surviving worker or read
consistency failure clears both healthy and fault pass flags, writes the final
report, and fails exit without replacing an earlier workload exception.

## Explicit read-recovery policy

`--performance-read-recovery` is optional and identical for both builds. It
does not change the default fail-fast fixture. After a certificate, the first
follow-up read exception is preserved with its error and timestamp. Only that
account's authenticated read may retry, at 0.5, 1, 2, and 4-second waits, with
at most four such retries per action and within the original 60-second action
deadline. Existing normal status polling remains unchanged. This policy never
signs or resubmits an action, resets a signing store, changes a quorum, or
changes original submission/certificate clocks. Existing exact-action retry
before certification is separately labeled and still checked byte-for-byte.

A successful read alone is not recovery: expected account sequence and all
balance deltas must match before `account_read_recovered=true` and action
completion. A recovered action may permit the remaining fixed windows/fault
phases, but its first error, `read_consistency_pass=false`, and final failing
exit remain. Invalid proof/balance assertions are never retried. An unresolved
account fails; its later queued offers remain recorded as dropped, and the
scenario does not advance. The report distinguishes certificate count,
completed proof-checked actions, first/total read errors, recovered actions,
and unrun phases. Eventual recovery must not be described as initial success.

## Public trace and interpretation

`--performance-public-trace` retains full PUBLIC request and upstream response
bytes under `public-http-trace/endpoint*/`, starting after generated client
startup checks and before funding/market workload. Identical delivered bytes
reference the upstream artifact; changed delivered bytes get their own file.
No HTTP headers, RPC credentials, wallet secrets, or TLS private keys are added
to these public artifacts. Generated fixture directories remain private test
data. Full public bytes preserve exact failure context even when inline
summaries are capped. JSON rows contain method, endpoint, request ID, request
and response hashes/sizes/paths, market/account/config/domain where present,
cursor/instance/known-head, reported sequence/hash/root/history ranges, HTTP
status, public/runtime rejection errors, and host monotonic timing boundaries.
The public API may omit context on a particular method; never invent it.

Body capture caps are 256 MiB and 32,768 files per endpoint, at most 32 MiB per
body; normal request forwarding remains limited to 1 MiB. The combined compact
HTTP archive is capped at 64 MiB/32,768 records; wallet RPC contexts at 256 MiB
reserved/32,768 records and 32 KiB per context. Relay in-flight records retain
the existing 4 MiB/4,096-record bound. Captures are opt-in, bounded, and not
fsynced as consensus data. Lost records, body-write failures, bound exhaustion
and RPC observation failures are explicit counters/errors and fail complete
capture/correctness; forwarding behavior is not adjusted to hide them.
Both builds pay the same observer overhead. Primary certificate timestamps
are taken immediately on RPC return, before archive/summary work; observer
cost may still influence later scheduling and must not be called zero.

Count every captured public method, including rejected replies. Per-window
counts include requests started from offer start through drain, with offered
actions as the stated denominator; setup requests are not divided by measured
actions. For per-action attribution, exact `(market_id, action_id)` wins.
Otherwise a request must match a unique tagged wallet-RPC interval and any
account/market fields present. This is labeled interval attribution, not proof
of causality. Overlapping scope-free requests remain shared/ambiguous; setup
and unmatched requests remain unattributed. Do not allocate them to whichever
action makes a favorable requests/action result. RPC failures are separately
retained because a client consistency rejection can follow HTTP 200.

## Separate bounded fault and diagnostic runs

After the matched pair, run one candidate fault scenario in a fresh directory,
using the same binary selection, seed, trace and read-recovery flags but script
`feature_flowmesh_performance_faults.py`, `--performance-markets=1`, and no
`--performance-profile=matched-repair`. Its predetermined phases are baseline
12, node3 offline 6, BULK held 4, BULK throttled 4, and recovered 4 actions, all
at 0.5/s. Keep live-replica certificate progress (0/1/2), returned exact target
application, catch-up readiness, signing eligibility and actual accepted share
proof distinct. `fault_coverage_completed` does not erase a failing read or
make `fault_scenario_pass` true. Failed proposer/hostile ingress remain
unqualified unless separately proven. Never reset original node3 keys/journals.

One candidate diagnostic run repeats the healthy matched profile and adds
`--performance-diagnostic` in another fresh directory. Keep BENCH results in a
separate table. New worker/store/service/agreement trace streams require their
own parser interpretation; the prior analyzer does not automatically explain
their new lock/wait/subspan fields, and nested spans must not be summed twice.
Any additional baseline fault/diagnostic run must be declared before execution
and must not be used to select a favorable healthy sample.

## Honest deterministic mismatch reproduction boundary

The existing TLS relay can hold a complete snapshot response for up to six
seconds while another generated account advances the market. Release retains
exact original bytes: this tests a genuinely older response, not the server's
internal assembly race. Do not fabricate a mismatched JSON projection and
call it reproduction. The original race must be exercised in a bounded server
unit/test seam that advances state between certified snapshot retrieval and
reported-data retrieval, then verifies the atomic evidence/projection pair.
That server-side regression is separate from this observational harness.

Offline harness check (no daemons, mocked calls, no performance qualification):

```sh
cd test/functional
python3 -B -m unittest -v flowmesh_public_trace_test flowmesh_performance_metrics_test flowmesh_performance_fault_metrics_test
```
