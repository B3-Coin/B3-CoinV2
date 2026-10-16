# Ancestry-proved committed-anchor frontier

Date: 2026-10-16. Production/source freeze:
`b8136220f550ebd6a7a3c41b720b71a4337171ea`.
Fixture-only successor: `c920fcf4a841d47ecb7de7905f5a15de81c7b574`.
Strict v11 profile: `2c99c038ff88b71897cb55c925d978d34231f754`.
Fixture-maintenance and strict v12 successor:
`0d119032046da1e1dca557583cb691fd70299ee6`.

This is a local, validity-preserving runtime optimization, not new consensus,
finality, bridge authority or a completed chain-facts adapter. No live data,
keys, node, VPS, push, release or activation operation occurred.

## Cause and smallest correction

Every runtime anchor recheck walked the complete retained map of distinct
committed anchors. That repeats historical canonicality queries even when
immutable block-index ancestry can prove the entire prefix is below one anchor.

For retained anchors S and a retained frontier F, if every A in S is a proven
ancestor of F in an immutable blocktree, then at one canonical chain view:

`all A canonical <=> F canonical`.

The new optional `AnchorPolicy::ProvesImmutableAncestry(A, F)` defaults **false**.
A true result must prove exact non-null hash/height identity, stable ancestry
and an ancestor-closed `StillCanonical` predicate. It says nothing about current
canonicality, depth, freshness, generation or permission to sign/spend. The
production adapter resolves both exact indexes and checks
`descendant.GetAncestor(ancestor.height) == ancestor` under one `cs_main` hold.
It deliberately does not cache a canonicality answer or return chain pointers.

The [runtime](../../src/node/flowmesh_runtime.cpp) keeps the original full map.
Its nonpersisted derived state distinguishes EMPTY, COMPLETE and DISABLED:

- Both original insertion sites use the same helper: successful durable commit
  and the existing startup traversal. No received assertion, candidate or vote
  independently grants coverage.
- First valid anchor initializes coverage. A distinct subsequent anchor extends
  it only with a positive previous-frontier-to-new-anchor proof and complete
  accounted map coverage. Exact duplicate keys preserve the old sequence update
  and do not add a proof or re-enable the cache.
- Derived coverage is invalidated before map/proof work. False, unavailable or
  throwing proof permanently disables this lifecycle's fast path. A proof
  exception after append does not become a new publication failure. Later proof
  availability or duplicates cannot re-enable it.
- Startup stages coverage privately. All original full-map startup checks,
  retained-candidate restoration and agreement checks still run before enabling
  it. Failed initialization publishes nothing; restart/replacement rebuilds it.
- A covered runtime recheck freshly tests the active-seat anchor, then the exact
  committed frontier with depth-free `StillCanonical`. Bad coverage falls back;
  an invalid frontier halts with the original committed-anchor error. Unsupported
  providers keep the original full scan and its error behavior.

Height monotonicity, private arrival order, timeout, online peers or the current
tip alone never establish the relation. No acceptance, quorum, signing domain,
certificate, journal, fee, serializer or old-conflict rule changed. Lower or
unproved relations cost the optimization, not an additional validity veto.

## Bounds, deliberately limited

The covered `RecheckAnchors` body uses two fresh canonicality queries—seat and
frontier—independently of history length. That is **not** two total queries per
tick or universal constant-time work. Other validation still performs its own
checks. Distinct insertion retains map lookup/allocation and ancestry lookup
costs; `cs_main` can still block. Startup and the authoritative map remain
history-sized. No obligation or historical entry is pruned.

This does not solve the inherited chain-during-sign/write authority-lease gap,
public service facade stalls, finality dirty replay, old-backup rollback, Linux
lock-order/TSan, WAN, sustained load or the 200 ms goal. The subsequent source
was not trading-latency benchmarked; the [earlier FIFO comparison](b3-flowmesh-outcome-fifo-measurement-20261016.md)
remains a separate exact-source observation.

## Test design and retained failures

One real indexed-chain proof case checks exact identity/heights, null/unknown,
reverse and competing-fork refusal, plus unchanged ancestry after orphaning.
True ancestry does not grant burial or canonical authority.

Four runtime cases use genuine generated BLS certificates and native durable
appends. The positive provider has an explicitly fixed synthetic immutable
lineage with ancestor-closed canonical prefixes; it is not claimed as real B3
validation or custody. An additional default-false provider deliberately has
independently mutable canonical hashes: A200 becomes invalid while B201 and
the seat remain valid. The original scan must still halt. This is the direct
counterexample to an unconditional highest-height replacement.

Other controls require permanent fallback on false/throwing proof, exact
duplicate behavior, requested catch-up, restart with explicit verified store
Replay, the original full startup scan, fresh frontier reorg refusal and
unchanged durable marker/head/sequence. Observer-only empty entries do not
qualify new financial accounting or a new agreement protocol.

The instrumented BEFORE has the new proof capability/adapters and identical
final test bodies, but original runtime `85ace68a`. It is not a byte-identical
old production binary. Final BEFORE native SHA256:
`dfa3c9049b23f3f50df5edd88dbfbe9c5ac87339e015d121bcbad145cee7629b`.
Actual primary child exit **201**, 78 assertions passed, two failed: the old
runtime made no extension proofs and still queried the interior prefix. The
exact intended prefix failure message was observed. Production-proof and
default-false controls passed separately with actual exits 0 (24 and 35
assertions). The expected-red collection wrapper's 0 is not native qualification.

Preparation failures remain separate evidence:

1. The old v10 discovery helper excluded the new sync suite despite its case
   being present in the binary's listing. It rejected before native execution;
   v11 adds the complete suite with mandatory discovery/execution controls.
2. The first proof fixture assumed heights 90/100 because of the inherited
   `TestChain100Setup` name. B3 actually mines `COINBASE_MATURITY=30`. Its child
   aborted with 1 passed/1 failed assertion before the proof predicates. The
   separately reviewed fixture-only successor derives positions from the actual
   tip and requires at least ten blocks; every original assertion remains.
   Original binaries, patches and outputs were preserved, not overwritten.

## Qualification and source review

The final five-target native/daemon/Qt build exited 0. The seven source files
listed below match the committed files. The scratch test CMake file exactly
matches committed HEAD, excluding preserved unrelated prototype edits.
The complete source-tree comparison found two other disclosed differences:
three comment-only lines in the scratch top-level `src/CMakeLists.txt`, and an
archive-expanded historical `GIT_COMMIT_ID` (`d03f4331d253de1966c583ad07c2b097db782bcb`)
in `src/clientversion.cpp` instead of the repository's format placeholder.
That embedded version label is not proof of the tested source revision or
fresh-checkout provenance. Preserved executable hashes and the explicit source
comparison identify this local campaign; no such binary is approved for distribution.
All other source differences were the excluded unrelated V2 prototype/test rows.
Final native SHA256 before the fixture-only successor:
`ebe8bb8a942f895b47f19b1a78afa2abf9e290470769883eb55d085d9c61ccae`.
All five targeted AFTER children exited 0, with respectively 24, 80, 166, 81
and 35 assertions passed. No timeouts, output overflows, warnings or expected
failures masked those results. Complete Qt helper/workspace tests subsequently
passed 82 and 362 cases, with zero failures and the same three opt-in visual
skips. These are functional tests, not a performance comparison.

The first full v11 native attempt **failed**: P2P's 14 cases passed; runtime
had 141 passes and one aborted existing
`accepted_local_original_waits_for_authority_then_resumes_without_client_resend`
case, at its `AllAt(3)` wait (189,975 passed/one failed assertion). The strict
runner stopped; the remaining 27 selected suites were not executed. Its failed
summary/XML/logs remain evidence. A predetermined isolated original-runtime
control also failed that same wait (266 passed/one failed assertion); the
single planned isolated frontier-runtime arm passed (291 assertions). These
schedule-dependent observations do not establish harmless flakiness, frontier
causality or complete qualification.

The retained original-runtime trace subsequently pinned the missing delivery:
the final certificate's relay to replica 3 was refused with `RATE_LIMITED`,
while its relays to replicas 1 and 2 succeeded. Simulated time remained exactly
102 seconds. All four had admitted the original and reached the same agreement
decision; three durably appended, while replica 3 lacked the final certificate.
The in-memory network ignores that enqueue result and reports untracked relay.
The committee's unchanged 32-token burst/refill and existing one-second retry
need ordinary maintenance; wall-clock `WaitUntil` does not advance `FixedClock`.
Real service maintenance uses 250 ms ticks. The failure did not demonstrate
lost admission or a source-specific frontier regression.

The separately reviewed **fixture-only** successor keeps the closed-phase
zero-time assertions and first same-clock reopening edge. It then allows at
most five ordinary 250 ms ticks, below the fixture's ten-second round timeout,
and checks unchanged views. No production timer, quota, timeout, journal or
quorum changed; no original was renewed, replaced or submitted again.

A new deterministic control deliberately withholds native sequence-2
attestations and certificates from one replica, also forbidding catch-up
ENTRIES. It pins the first actual publisher and entire exact certificate,
checks that publisher's durable entry and the observer's durable candidate,
then releases only that publisher's subsequent certificate relay. The BEFORE
omits only post-release maintenance: actual child exit 201, 337 assertions
passed/one intended final-wait failure, no warnings/expected failures. This
declared packet-loss control is **not** a claimed reproduction of the actual
single rate-limited packet. The identical future adds bounded normal ticks:
the existing and new cases passed separately, actual exits 0, respectively
295 and 503 assertions. Exact wire, no additional origin ACTION/catch-up,
unchanged views and one accounting sequence effect are checked.

Frozen BEFORE executable SHA256:
`16dca7601de06298b5c620e75adccf3896901e32bf961eccfa570502a454f460`.
Frozen v12 AFTER executable SHA256:
`3c55c0c2544f395e1fa08d034697ee30993125f9514308c4bb54583c99e60a1b`.
Both builds exited 0; their unchanged duplicate-library linker warning remains.
The complete v12 native selection then ran **once** against the committed
successor: **533 cases in 29 suites**, actual wrapper exit 0 and every native
child exit 0, discovered names equal executed names, no selected skips,
warnings/expected failures, timeouts or output overflows. Elapsed functional
campaign time was 253.963 seconds, not a trading-latency measurement. The first
v11 failure and instrumented BEFORE remain separate failed evidence.
The final native executable is the `3c55c0c2` SHA256 above; only the runtime test
translation unit changed after the earlier five-target build. Daemon and both
Qt test executables remain byte-identical to their previously tested copies:
82 trading-helper passes and 362 workspace passes/three opt-in visual skips.
No new native/Qt product or performance guarantee follows from those passes.

Separate non-author source review passed the seven-file R2 implementation,
its scope, the root fixture-only successor and CI v11. Review inspected source,
hashes, invariants and counterexamples, not merely this summary. The implementation
author executed no build/test. The CI reviewer ran 61 private stdlib controls
once, exit 0 (0.221 seconds); root ran the identical committed controls once,
exit 0 (0.200 seconds). These are not native tests or Linux CI execution.
The fixture-maintenance successor received separate non-author source review
of complete BEFORE/AFTER/phase patches, callback lifetimes, durable evidence
and alternate-path exclusion. CI v12 received its own bounded review; all 61
previous controls remain, plus three. Actual reviewer/root stdlib runs each
passed 64 controls once, exit 0 (0.204/0.199 seconds). The reviewers did not run
the native qualification and did not equate root's results with their own.

## Reproduction and exact source

After a normal dedicated native build, run the full runtime and sync suites
separately against fresh generated test directories. For the complete strict
Linux profiles and their bounded commands, use the
[guardrail workflow/report](b3-flowmesh-native-linux-guardrails-20261016.md).
Do not pass an owner wallet/datadir to a test or substitute a live endpoint.
The five exact mandatory names are in the
[versioned manifest](../../ci/check_flowmesh_native_guardrails.py).
Lightweight discovery/execution rejection controls:

```sh
python3 -B ci/test_flowmesh_native_guardrails.py -v
```

Final source SHA256:

| File | SHA256 |
| --- | --- |
| `src/flowmesh/deposit.h` | `10ced8cf90bd4dd7110b39021625bb8f28a2efbc45bb8b7c75125a3ae137e432` |
| `src/node/flowmesh_anchor.h` | `b1dd4ddec06a94b941e3d7b768849234554e8796e89261d08485308a2653083c` |
| `src/node/flowmesh_anchor.cpp` | `6fd41e1b58dd1311ca37c4b2f0f89602521ea9e19968be2a5adab2373d226ba0` |
| `src/node/flowmesh_runtime.cpp` | `004b7f9e9116b55aa798fa1e827c9b6aa541eab8e8a0daecf8c01506b08a51ff` |
| `src/node/flowmesh_service.cpp` | `976b8102d049eaf87af92a99912742c1777ee0417b58d515ca4c102c69f7fd6e` |
| `src/test/flowmesh_sync_tests.cpp` | `776edd85ed9012ba96aa1d9ea2a48df39a6010d54c3b5bc924eb8adeb884dcde` |
| `src/test/flowmesh_runtime_tests.cpp` | `eb3ee22fbd3cebe2903fa046d89498c5a5526c351e4261e982e42ebfa111540b` |

Generated fixture keys, databases, journals, logs and raw outputs stay private.
No release or deployment approval follows from this stage.
