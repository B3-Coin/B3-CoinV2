# Coordinator experiment — local qualification, 2026-09-25

## Exact source and disposition

- Branch: `flowmeshV2-dev`; no new branch, PR, push or deployment.
- Preserved baseline: `82ebbb2e31ae8df09b575d379117c55490461617`.
- Predeclared boundary: `0e070cf195d63bae15d23d79c13e2a8e064ee47e`.
- Tested implementation: `233e4f6df41338d3772d12361bfa4909a6cfd76b`.
- This closeout adds documentation only; the executable model/test tree is
  unchanged from that tested implementation.
- The native `src/` tree and all three existing accounting/agreement/storage
  profile files are unchanged from the preserved baseline.

## Personally executed checks

On Darwin arm64, Python 3.14.6, standard library only:

```sh
python3.14 -B ci/run_flowmesh_models.py
```

The committed-tree runner completed with exit status **0**:

| Suite | Executed | Result | Test-runner wall time |
| --- | ---: | --- | ---: |
| Runner discovery guards | 8 | PASS | 0.015 s |
| Accounting | 113 | PASS | 65.452 s |
| Agreement/recovery | 142 | PASS | 75.566 s |
| Storage/interruption | 69 | PASS | 46.778 s |
| Coordinator/placement | 20 | PASS | 72.939 s |
| **Total** | **352** | **PASS** | |

These durations are test execution costs, **not trading latency**. No skipped
or expected-failure test is counted as passing. The existing storage suite
retains its intentional killed-child exits alongside successful recovery
and clean exits; a deliberate kill is not described as a clean shutdown.

The separate new-suite development run passed 20 tests in 72.136 s.
The ten-case comparison completed with exit 0, preserving complete synthetic
issued-signature/event evidence. Its executable source matches the tested
implementation above. All ten committed compact-result rows were compared
with the raw JSON and matched exactly.

Full suite log retained outside the repository:
`full-run-233e4f6.log`, SHA256
`c7a8375509e506f0b2ca3c2ed83d1c34bf16e93dd634fd0610a28413a07c7d58`.
The comparison capture identity is in [RESULTS.md](RESULTS.md).
No GitHub CI run is claimed for these unpushed local commits.

## Result and preserved invariants

[The comparison](RESULTS.md) is a negative result for selection-only speed:
both policies take three logical delivery ticks under healthy conditions.
The four-batch policy repeatedly waits for an absent scheduled leader on each
new sequence. It has not become the approved native selection rule.

Checks preserve exact original instruction identities, complete issued-vote
history, existing quorum/timer predicates, independently verified COMMIT
proofs, matching and quote-fee accounting, one-time application, protected
recovery evidence and refusal to reinterpret SQLite history under another
configuration. No native certificate phase or durability step was removed.

Optional host placement preserves independent seat identities and quorum.
One shared host can remove several votes at once. The test does not prove
host independence or secure delegation. Commission terms and `-xbps` remain
proposed, not implemented or approved economic defaults.

## Review and next boundary

A separate read-only native-feasibility inspection established why changing
only a native leader callback is insufficient: native ingress, proof
verification and production proposal paths also enforce selection, and
native advancement relies on durable V1 certification.

This is not a completed independent implementation audit. A further reviewer
context was unavailable due to the agent-thread limit; no alternate route
was used to bypass that limit. Implementation review remains pending.

Next: review the experiment and specify agreed coordinator-replacement
authority across batches, including restart and competing proof delivery.
Then use a separately identified native pipeline experiment to measure the
actual client-to-durable-result path. Do not derive leader authority from a
private last-seen preference or silently remove V1 certification.

The complete coordinator architecture, native 200 ms target, BLS/network
integration, client proof delivery, B3/bridge compatibility, changing
membership, futures risk and paid delegation remain unqualified here.
