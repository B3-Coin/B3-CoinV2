# Bounded derived checkpoint advisory

Date: 2026-10-16. Branch: `flowmeshV2-dev`. Prerequisite:
`1d268c4629534931bd34a166a964e893f62baf6d` (immutable certified reads).
Status: native store primitive; public service integration remains a separate gate.

## What changed

The production store now publishes one memory-only advisory, without requiring
a display reader to acquire the durable-store mutex or scan history. It contains
the complete validated store marker, a local revision and connection watermark,
a resumable cursor, and at most one exact existing FlowMesh checkpoint payload
(at most 1,264 encoded bytes). It does not grant signing, custody, execution,
publication or chain authority. No durable schema, signature domain, economic
rule, certificate threshold or checkpoint eligibility changed.

The explicit states are `INVALID`, `REBUILDING`, `KNOWN_NONE` and
`KNOWN_CANDIDATE`. Unavailable evidence never means that no checkpoint is pending.
Only successful existing replay/append/connection/reconciliation boundaries
publish derived results. The first mandatory verified replay derives advice
alongside its existing work; a failed replay cannot publish a partial result.
Genesis, handoffs, effects and settlement-only entries retain the legacy
eligibility rules. Complete marker matching prevents a same-head context change
from silently reusing old advice.

`ReadCheckpointAdvisory()` copies fixed metadata and an immutable shared payload
under a separate leaf mutex. It does not use the chain lock, durable-store lock,
disk, history traversal or cryptography. Payload lifetime is bounded; this is
not a formally lock-free algorithm or a measurement of total heap use.

`AdvanceCheckpointAdvisory()` is an explicit maintenance operation, not a hidden
fallback from the getter. Each default step inspects at most eight records and
decodes at most 8 MiB of encoded key/value input. These counts include marker and
refused records. Raw iterator lengths are checked before copying/deserializing.
The bounds do not measure LevelDB I/O, total allocation, cryptographic wall time
or the existing full replay/reconciliation work. A valid maximum entry and its
marker fit a fresh default budget. The implementation retains a cursor and one
candidate, not an unbounded backlog map.

## Qualification and retained counterexample

The native store and DB suites passed 35 cases and 4,548 assertions. They cover
bounded suffix scanning and resume, exact legacy-result equivalence, leaf reads
while the writer mutex is held, connection/reorg/replay behavior, oversized and
corrupt records, missing seat evidence, and an injected synchronous-write failure.
The test-only fault adapter is defined in the test translation unit; there is no
new production fault option.

The integrated runtime/store/DB/HTTPS/client-evidence/health/service/timing
selection then passed all 265 cases and 243,153 assertions. The source used the
ordinary service/client, not the quarantined public-API candidate. The separate
reviewer inspected the frozen source but did not execute these native suites;
the milestone owner executed them. This is not external security qualification.

Separate read-only source review found a stale-advice defect in the first draft:
after a known candidate, reconciliation could observe a completely emptied
namespace and retain the old candidate while returning its legacy empty-store
success. The correction invalidates the advisory on that observed branch,
without changing the legacy readiness/return contract. Reconstructing that exact
predecessor branch against the final regression failed two of 79 assertions;
the corrected store passes. This was a defect in the new draft, not a claim that
V1 previously violated an advisory API it did not have.

The first test compile also rejected a `std::string` filesystem-path operand;
the test-only successor uses `fs::PathFromString`. Production source was unchanged
by that compile correction. Existing unrelated lock-analysis/link warnings remain
visible in qualification evidence.

## Reproduce

Configure the ordinary native build with wallet and tests enabled. Then run:

```sh
cmake --build build --target test_bitcoin -j 8
build/bin/test_bitcoin --run_test=flowmesh_production_store_tests,dbwrapper_tests \
  --report_level=short --log_level=error
```

Use only the generated temporary fixtures. No live wallet, signer, VPS, bridge
transaction or existing regtest data was used.

## Remaining gates

This primitive alone does not wire public status/checkpoint routes, replace
blocking vault-proof discovery, guarantee bounded restart time, or establish the
zero-`cs_main` chain-fact architecture. Those require a coherent separately
qualified service stage. The unfinished service candidate was retained outside
the source tree rather than committed with unavailable funding routes.

Corruption is invalidated when observed; a memory-only getter does not instantly
detect external disk changes. A coherently restored old backup requires an
external trusted freshness mechanism. These tests do not qualify machine power
loss, production bridge compatibility, V2 consensus or 200 ms latency.
