# FlowMesh exact signed publication recovery

Date: 2026-10-16. Local diagnostic repair on `flowmeshV2-dev`, based on
`f2fdfde3f041824bd7b3e1c5353dd8e88da38419`. Independent review passed within the
six-file repair's declared source scope; the successor's complete selected native
qualification also passed. No publication, production activation or speed claim is authorized by
this stage.

## Cause and correction

`FlowMeshAgreement::Sign` synchronously stores the exact signed object before
calling its publication callback. A chain reconciliation can begin between those
two operations. `PublishAgreement` then refuses admission; the signature remains
safe in the journal, but later calls to `Sign` return the existing signature without
sending it again. `ProcessTick` ordinarily waits for its one-second `Retry` interval.
This explains a delivery wait, not slow execution or expensive quorum mathematics.

The repair retains a disposable copy of that exact local signature only when its
publication actually observes an unavailable chain gate. The first ordinary tick
after the gate reopens can replay one bounded batch without waiting for the normal
one-second deadline. It never calls `Sign`, `Pump`, or the historical record scan
in `Retry`. Normal rebroadcast, view timeouts, synchronous signing records,
certificate rules and quorum remain unchanged.

## Ownership and authority

The optional `publish_local_signature` callback identifies locally signed journal
records after their signed sync, including exact records retransmitted by normal
`Retry`. Existing users without the optional callback retain their old callback
behavior. Received proposals, quorum decisions and peer forwarding use the ordinary
publication callback and cannot claim local cache ownership.

A retained copy binds the complete existing agreement context: domain, market,
epoch, seat set, log sequence, parent hash, previous state root and execution
configuration. Its hash and canonical encoded payload identify the exact original;
the cache never selects a new candidate or produces another signature. The retry
rechecks the normal publication context, chain gate, permanent halt and pending
handoff guards, after ordinary marker/anchor refresh and journal retries.

A PAUSED seat or missing local key alone neither inserts a copy nor resets retry
pacing. Existing exact retransmission eligibility is preserved: already issued
signatures may still be relayed while local signing is paused. Relaying those
bytes is not permission to sign, execute a candidate or disregard unavailable
candidate evidence. Peer verification, candidate validation and signing retain
their existing independent gates.

Normal successful publication removes a matching disposable copy, avoiding a
second same-tick send. A context change, detected permanent halt, pending handoff
or stopped runtime discards only the copy; signing records are never cleared.
An old view alone does not invalidate a record of the same full slot context.
Restart does not preserve this optimization: exact journal retries remain the
durable recovery mechanism.

## Retention and work bounds

| Resource | Bound |
| --- | ---: |
| Global retained copies | 64 |
| Retained copies per market | 16 |
| Global accounted bytes | `2 * (AGREEMENT_MAX_BYTES + FLOWMESH_WIRE_HEADER_SIZE)` |
| Accounted bytes per market | `AGREEMENT_MAX_BYTES + FLOWMESH_WIRE_HEADER_SIZE` |
| Records inspected per fast pass | 8 globally, including refused and stale records |
| Encoded input per fast pass | At most two maximum framed agreement objects |
| Open fast-pass interval | 250 ms |

`AGREEMENT_MAX_BYTES` is 4 MiB and the wire header is 50 bytes. A fresh supported
budget can fit at least one maximum legal frame. A flat global cursor prevents
per-market loops from multiplying the eight-record pass. Closed gate checks
inspect no cached objects and do not consume an open-pass interval. The first
reopening batch is also at most eight records; it cannot bypass an interval spent
by an earlier open batch. Repeated close/reopen events do not refill relay tokens.

Every attempt uses the unchanged market/global agreement relay budgets. Cache
capacity refusal is not a signing halt or a proven nonexecution result: the
journal's normal one-second retry remains available. The cache owns one encoded
payload per retained record. Accounted bytes include its framed size, not total
C++ heap usage, decoder temporaries, physical I/O or an absolute processing-time
bound. Each inspected input can also incur bounded decoding, canonical encoding
and hashing; those are not additional unaccounted historical traversals.

## Native regressions and qualification

The initial failing-before test translation unit was frozen before production
edits. Its test-only key-provider hook arms the existing synthetic chain's
one-shot reconciliation predicate at the post-validation publication check.
Assertions require two actual publication refusals, for the durable proposal and
PREPARE, rather than accepting an earlier pause with no signature. Reopening uses
`Tick(0)`, with no new client request, clock advance, new view or cleared obligation.
The successor must relay the same canonical signed objects and reach agreement.

The preserved baseline actually failed this native regression with exit **201**:
**99 assertions passed, one failed**. Both durable proposal/PREPARE publication
refusal preconditions passed. After fixed-clock reopening, the decisive failure
was `originals.size() == 0`, rather than the required two signed originals. No
timeout, new request or view advance was used to manufacture that failure.
The exact before binary SHA-256 is
`76c337cf9094d9eddeb34d6726c975734b9dbf1111a0b9f8e228606d05dc1fdc`;
its added test translation unit was frozen as
`2a1d0224eae8e6f09324133d9130f10a4dd88ae95b3288fca8b49b55093841da`.
Production runtime/agreement source was still the baseline, not the repair.

Additional runtime tests exercise global/per-market count bounds, repeated exact
references, large encoded-copy byte bounds, peer nonownership, cursor fairness,
same-clock repeated ticks, the 250 ms boundary, relay-budget refusal, actual
reclose/reopen, normal-send retirement, PAUSED-only behavior, stale context,
permanent anchor failure and signer-journal reopening. Resource tests using the
private cache seam are explicitly synthetic copy/accounting tests, not evidence
that arbitrary fixture objects were issued by the native signer. The end-to-end
and restart tests use real native signing and generated disposable journals.

Two agreement tests separately check local-source callback classification and
that publication cannot reach it before the signed-record sync. Existing
agreement/recovery, runtime, handoff and independent issued-vote checks must
remain enabled; a source-only review is not native qualification.

Reproduce the focused runtime cases from the exact successor binary:

```sh
build/bin/test_bitcoin '--run_test=flowmesh_runtime_tests/preagreement_local_signed_publications_resume_exactly_without_clock_advance' --report_level=detailed
build/bin/test_bitcoin '--run_test=flowmesh_runtime_tests/local_agreement_publication_*' --report_level=detailed
build/bin/test_bitcoin '--run_test=flowmesh_agreement_tests/local_signature_*' --report_level=detailed
```

| Evidence | Current result |
| --- | --- |
| Initial before test on the preserved baseline | Intended failure reproduced: exit 201, 99 passed assertions and one failed |
| Successor focused native tests | All nine added runtime/agreement cases pass within the full selection; fixed-clock reproducer passes 122 assertions |
| Complete native runtime and agreement suites | All 138 runtime and 25 agreement cases pass |
| Independent source review of the frozen successor | PASS within declared source scope; reviewer ran no tests or builds |
| Native trade campaign after repair | Not executed for this stage |

The complete `native-guardrails-v3` selection passed **460 exact native cases
across 27 suites**, all children exit 0, with no selected skips, missing cases
or expected failures. The explicitly local runner profile was
`host-local-macos-ci-selection-v3`, on the same AppleClang 17 Release C++20
macOS build described in the [diagnostic report](b3-flowmesh-retention-native-diagnostics-20261016.md).
Test executable SHA-256:
`e1747eefa91f1836a38282218c68b1d146a8b393a974e5777c73003b34f5b4f6`.
Ordinary daemon SHA-256:
`9c4dc50ae299e5d25a5b4b6bff6a49197c02c532d88958e0631830193e72b136`.
The compilation tree combines the committed client read-isolation source
`e08c5c3c...`/test `fdbf57e8...` with this exact six-file recovery repair; the
separate aggregate-save, Qt and public-health test candidates are excluded.
No crash-hook macro, unrelated native V2 prototype or user CMake edit is included.

Frozen source hashes, unchanged from separate review:

- Runtime source/header: `85ace68aad0fbab5067e6f409a33029d391def0acfb0acf6d4dd6d16a3cc1204` / `e6ea4e7b8abd6826681430476914d0e7ee62611d99cecc10ad25f33d0e4c42e0`.
- Agreement source/header: `fe8ea9f4fd32b21f59e62339a6cd91746d681445564ba294b43742e0b01fc1e5` / `5e917ba6c133cf80e21d2c52f23761c3c1b99bcdcb6d05fa87cf06f3847fe3f5`.
- Runtime/agreement tests: `3bd0c0dd4018463f8d1e1287c3a03205da66faacf1fa1947561f033a782be950` / `ff201860d7d282510ba984c6be25d376fe268a9f0c1f572ec50cfb2c67a0cc0f`.

The strict runner's 33 lightweight controls also passed locally and in separate
read-only review. These are not additional native tests. Linux, lock-order CI
and TSan execution remain pending; no remote workflow was run or published.

## Remaining boundaries

This stage covers a local publication callback that actually observes the closed
chain gate. A transport that refuses later, after that gate check, still has the
existing normal journal retry; this repair does not claim every network admission
race is eliminated. Admission still does not prove peer receipt, a fill or client
verification. Finite cache/work bounds do not prove arbitrary-load availability,
200 ms trading, WAN latency, Qt behavior or production release readiness.
