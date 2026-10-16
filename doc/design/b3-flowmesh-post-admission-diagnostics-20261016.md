# Post-admission trading controls: retain the failure

Date: 2026-10-16. Source: `777241f5d26ae3c48025979992b8388815dc4b76`.
This is a small local diagnostic, not a passing 200 ms, reliability, capture
overhead, Qt or WAN qualification. The successful control and failed capture
are both retained. Do not combine their medians or discard failed originals.

## Exact experiment

One predetermined control/capture pair ran sequentially, without concurrent
task builds or test campaigns. Two unrelated, 34-day-old orphaned test
processes were sleeping at zero reported CPU and were left untouched. The
machine was the same Apple M4 Max, 16-core/128 GiB, macOS 26.5.2 host. It was
not a VPS or distributed tester run.

The unchanged workload used four independent validator processes, two HTTPS
endpoints through the existing TLS relays, one engine-off client process,
four buyer accounts, two liquid test markets, one isolated matched-fill
request, one idle second, then eight burst requests. B3 advanced during the
workload. Real local TLS, quorum certificates and ordinary synchronous writes
remained enabled; membership and all assets/keys were generated regtest data.

Daemon SHA256:
`c8e4c86b22ad0d6191d4b8bb5fcdf67991ed4fe4b3159d5af97172a6eff165a8`.
The [unchanged harness](../../test/functional/feature_flowmesh_performance.py)
SHA256 was
`f344f81341aa4d4fd568ea491ff7e7f0695ff84b25440fe4baffda5e77412121`.
Both arms used `native-queue`, seed `24092026`, port seed `92401`, RPC capture,
strict execution-outcome observation and the default zero submit wait. Only
the second enabled bounded native memory capture. BENCH logging was OFF.

The principal clock is original submission RPC call to the client's verified
execution result. Follow-up balances and serial replica observations are
different clocks. Inclusion alone is not a fill. These nine requests cannot
establish population tail latency or sustained throughput.

## Actual results

| Arm | Isolated verified result | Burst verified results | Burst result p50 (nearest-rank) | Actual exit |
| --- | ---: | ---: | ---: | ---: |
| Control | 272.478 ms | 8/8 | 1,195.123 ms | 0 |
| Native capture | 300.517 ms | 3/8 | 739.208 ms, successful subset only | 1 |

The control's slowest burst result was 2,208.161 ms. Its functional correctness
flag was true, but its performance flag was false. It is not a 200 ms pass.

The captured burst verified inclusion for all eight originals. Five reached
the unchanged 60-second action deadline without a verified execution result.
Its three-success p50 must not hide those five failures. For the eight control
results, the harness uses nearest-rank p50, not the average of the two middle
observations; that conventional sample median would be 1,290.432 ms. The harness's
functional and performance flags were both false. No replacement action was
created for an included original; exact identities and retry observations
remain in the retained synthetic records.

All five final daemon exits were zero in each arm, and all harness threads
stopped. The final four validator exits are in the explicit shutdown log;
earlier `native-child-exits.jsonl` validator rows describe setup restarts and
must not be mislabelled final shutdown observations.

## Capture limitations and the next counterexample

The failed arm dropped 11,127 RPC observations. Every node retained the full
32,768 native-event limit; dropped-event counts for nodes 0 through 4 were
38,312 / 34,426 / 37,614 / 35,406 / 41,034. Serialized bytes remained within
16 MiB per node. The harness correctly rejected this incomplete capture.
Its long deadline wait does not justify increasing limits or calling missing
events proof that a particular action, check or request never happened.

Retained relay method counts contain no standalone `outcome_evidence` request.
The compact RPC projection omits `reason_code`, and generic HTTPS spans omit
the method. These records do not reveal whether an upgrade was unadvertised,
lacked a retained proof, was budget-ineligible or failed before forwarding.
The cause of the five observed missing results is therefore unresolved.

Separately, source inspection found a deterministic liveness counterexample:
`UpgradeOutcome` permanently marks an endpoint attempted before its read. A
temporary standalone BUDGET refusal therefore prevents a later healthy proof
from being requested in that process. A dedicated real-TLS test-first control
subsequently failed with three assertions against the old implementation and
passed against the [bounded recovery successor](b3-flowmesh-outcome-budget-recovery-20261016.md).
This separate deterministic reproducer is not the established explanation
for this captured burst.

Publications and deployments remain outside this local session. The
[previous diagnostics](b3-flowmesh-post-isolation-diagnostics-20261016.md),
[bounded admission stage](b3-flowmesh-bounded-client-read-admission-20261016.md)
and [new diagnostic controls](b3-flowmesh-client-admission-timing-20261016.md)
remain separate evidence.

Retained report SHA256 values are
`3a0256bd7aeeb84acc62e6f012cbeb6417f2ca5d6274dd837c5c5496c2147984`
(control),
`1d0745d851210287243c4b5ff8e3e5e5f8213ea27c5fe257a2e35823ee051fcc`
(capture), and
`b1b197bc02b039106582ac9d6a241456860953d624fc29c283157e57068eb368`
(pair actual-exit manifest). Generated wallets, journals and raw directories
remain private, not repository publication content.
