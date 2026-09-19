# FlowMesh V1 integrated fixes — review scope

This source integration starts at published commit
`b061401252ab46b0abaa6f098dfe5ce3b233de33`. It preserves V1 as the baseline;
it is **not FlowMesh V2**, a hard-fork activation or a production deployment.
No live wallet, signer, market or historical signing journal is changed by
source publication.

## Included changes

1. Bounded exact-envelope delivery retry and retired-slot duplicate suppression;
   old-view candidate-evidence recovery without new old-view votes; typed
   pre-write reconciliation deferral; interrupted catch-up page ownership;
   eligibility-time deadline accounting; explicit checkpoint barrier diagnostics.
2. Coalesced fresh-action wake-up and single-entry reuse of identical successful
   seat construction. Canonical-anchor, synchronized-index and anchored live
   eligibility checks still run before cache lookup. Signing and publication
   remain synchronously durable. The older eight-entry cache prototype is not
   included.
3. Ordinary-client HTTPS endpoint connection/selection, bounded transport
   failover/backoff, catalog visibility after a selected-market read fails and
   Qt connection controls. `flowmeshclientconnect` and the existing
   `reconnectflowmeshclient` are distinct APIs. See
   [trader connection instructions](../flowmesh-trader-connections.md).
4. A generic bounded public-asset-metadata parser, with compatible FlowMesh
   wrappers. This is only a refactor/foundation: **creator-authenticated
   publishing, automatic P2P distribution and persistent public labels are not
   implemented by it**.
5. An opt-in generated-regtest latency fixture and focused regressions.

Quorum, signature/application formats, economic execution and B3 settlement
validity are preserved. Configured round timeout and resource limits are
unchanged; local eligible-time accounting is a behavioral correction, not a
claim that timer behavior is identical. The pre-agreement operator protocol
still requires compatible operator versions; mixed-version operation is not
qualified.

## Retained evidence, before this combined publication

These results were executed against their individual preserved source stages.
They are not new executions against the combined source.

- Final resilience/performance source: 339 FlowMesh cases / 168,450 assertions
  and 7 FN seat-index cases / 300 assertions passed. Failing-before controls
  cover the delivery/reconciliation repairs, action wake-up and seat reuse.
- Four independent headless regtest operators and one ordinary HTTPS client
  completed the recovery/settlement fixture, including trade, cancellation,
  withdrawal payout, constructed-block acceptance/rejection parity, advancing
  B3, bulk interference, same-store restart and rejoining-seat attestation
  acceptance. Final five child exit statuses were zero. These are isolated
  generated-data tests, not recovery of a deployed conflicted market.
- Trader stage: 81 Qt workspace passes (one optional visual-export skip),
  29 combined C++ cases / 1,209 assertions and generated connection/reopen
  functional checks passed. The Qt results use a minimal platform, not attended
  native screen qualification.
- Generic metadata parser: 8 focused cases / 208 assertions passed at component
  scope; no complete metadata-publishing feature was qualified.

### Latency: 200 ms remains unmet

The retained same-Mac optimized-build workload used four separate operators,
one ordinary HTTPS client, normal logging and advancing B3. Each sample has
20 sequential bid/cancel actions, not matched-fill throughput or WAN load.

| Submission to client-verified durable certification | Baseline | Wake-up | Wake-up + seat reuse |
| --- | ---: | ---: | ---: |
| Median | 695.897 ms | 577.015 ms | 419.386 ms |
| p95 | 862.421 ms | 707.056 ms | 744.989 ms |
| Maximum | 863.563 ms | 2113.025 ms | 1859.363 ms |
| Above 200 ms | 20/20 | 20/20 | 20/20 |

The performance gate failed in all three samples. The median improvement is
not a production guarantee or a proven causal improvement for every workload.
Original submission timing survives admission refusals and identical-action
retries. All-replica observations start at original submission; they are not
additional replication delay. Exact disk-append/first-admission times were not
captured in this normal-logging comparison. A separate fault/safety campaign
had longer tails and must not be blended with this table.

## Opt-in benchmark

Build daemon, CLI and wallet support with the usual generated functional-test
configuration. Run directly, using only its generated regtest directories:

```sh
python3 test/functional/feature_flowmesh_latency.py \
  --configfile=/absolute/path/to/build/test/config.ini \
  --latency-warmup-pairs=2 --latency-measured-pairs=10 \
  --latency-production-logging --latency-observe-only
```

Without `--latency-observe-only`, the default 200 ms performance assertion
remains enforced. Observe-only records a failed performance gate without
relaxing certificate, account-state or signed-byte correctness. The benchmark
and full pre-agreement qualification are deliberately opt-in rather than
unconditional default-CI latency requirements. Do not compare runs with
different logging/observation modes or concurrent build load as equivalent.

## Remaining limits

- Original native accessibility SIGSEGV: **OPEN / UNRESOLVED**. No recurrence
  and minimal-platform Qt passes are not a crash repair.
- Existing split final signatures, invalidated prepared/final-signed anchors
  and indefinitely missing quorum are not automatically recoverable. Do not
  reset signing history or force rearming to make them appear healthy.
- FMN2 is authenticated plaintext TCP, not encrypted transport. QUIC,
  independent-machine/WAN performance, crash/power-loss recovery and sustained
  concurrent/matched-fill latency remain unqualified.
- Fresh platform packages, native Qt qualification of merged changes, Windows
  trust-store packaging and publicly reachable verified HTTPS endpoints are
  separate work. Loopback examples are not remote tester infrastructure.
- Asset-to-asset markets, shared balances, stable-asset fees, new consensus
  phases, inactive-validator policy and V2 transition rules are design work,
  not features of this integration.

New combined-tree results are recorded separately when run. Earlier evidence
and failed-run histories remain preserved outside the public source tree;
private wallets, keys, outboxes and runtime archives are not publication inputs.
