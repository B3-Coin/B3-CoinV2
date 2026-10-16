# FlowMesh clean-core replacement: milestone contract

Date: 2026-10-16. Branch: `flowmeshV2-dev`.
Starting revision: `863a30cdbb5f1fc8dc36200ad1dd1fbb9a27cdd6`.
Status: **replacement implementation authorized; first infrastructure milestone,
not a working exchange, protocol approval or release**.

## Direction

The owner explicitly clarified: redesign completely, from scratch, using useful
Bitcoin components and a modern C++ architecture. This supersedes treating a
sequence of old-runtime repairs as the redesign deliverable. Build the new core
alongside the baseline; do not delete history, wallets, orders, claims, original
instructions or signer stores. Do not rewrite useful cryptography or chain
validation simply to call the result new.

The replacement lives in `src/flowmesh2/`, not the unrelated uncommitted
`src/flowmesh/v2/` prototype. No new branch or PR. Small local commits; completed
milestones freeze for separate review before dependent work. No push, activation,
VPS change or live data operation is authorized by this milestone.

The earlier [redesign plan](b3-flowmesh-redesign-plan-20261015.md) supplies product
requirements and unresolved protocol/economic decisions. Its repair-first order
is no longer the implementation sequence. Historical reports remain unchanged.

## Keep versus replace

| Keep as components | Replace as architecture |
| --- | --- |
| Bitcoin bounded serialization, hashing, exact asset identifiers and tested amount utilities | New V2 domains, canonical action/state/outcome formats; no reinterpretation of V1 bytes |
| secp256k1 and reviewed BLS dependency | Explicit signer/store interface and one approved agreement-to-commit path |
| Bitcoin `Mutex`/`LOCK`, annotations and applicable lock-order tooling | One state owner, bounded message interfaces, no chain/network/disk waits under a shared trading lock |
| Database/filesystem primitives and recovery counterexamples | Versioned commit/application journal, durable publication barrier and external rollback fencing |
| Wallet ownership, original-action outbox and no-resubmit protection | Typed engine-off trading client and result verifier |
| Useful peer/TLS plumbing and certificate checks | Independently bounded command, custody, recovery, control and display service |
| Qt wallet integration, exact formatters, chart/book widgets | New small controller/reducer/models and ordinary exchange-style ticket |
| Existing chain and finality verification evidence | Explicit PoS V2/finality/authority adapters, bounded derived-state recovery and transition rules |
| Bridge proof/contract/vector work that passes review | Separate bridge manager with no power to invent mint/payout authority |

Reuse algorithms, primitives and adversarial vectors. Do not wrap the V1
per-market ledger and call it a shared V2 account. Language stays C++20; Qt stays
the desktop framework. Neither choice promises latency.

## New ownership and commit boundaries

1. **Admission:** bounded parsing/authentication of the original instruction.
   Local queue acceptance is distinct from server admission, committed inclusion
   and verified execution. Busy reads must not create an uncertain economic send.
2. **Application:** one deterministic shared state owner for all exact AssetIds
   and markets. Pure preview returns a candidate, outcomes and commitments;
   it cannot call wallet, Qt, filesystem, network or chain state.
3. **Agreement:** certify the exact parent, ordered originals and resulting
   commitments once. Existing PBFT-style models and native work are references,
   not permission to substitute a toy certificate or change quorum.
4. **Storage/signing:** evidence and intent durable before first signature;
   exact signed object durable before publication; decision/application records
   reconcile exactly once. A queue acknowledgement is not a disk acknowledgement.
5. **Read publication:** actual verified committed content becomes visible only
   after the required durable application barrier. One reply carries one complete
   scope/head. Historical proof, current authority and transport freshness are
   separate facts.
6. **Chain adapter:** coherent authenticated custody/authority facts, explicit
   busy/need-data/invalid states and verified ancestry/import cursors. No hidden
   `cs_main` fallback in application or display. The exact sign-versus-reorg
   linearization must be proved before native signing integration.
7. **Client/Qt:** locally retained original bytes and verification of the actual
   applied result/fill/fees/deltas. Widgets emit intents and render models, not
   maintain another ledger or journal. Reconnect never re-signs a replacement.

The intended first useful vertical slice is two stable-quoted markets sharing
one quote AssetId: original action -> shared application -> durable decided result
-> coherent result proof -> engine-off verification. One market would fail to
test the main cross-market property.

## Milestone sequence

These identifiers are new clean-core milestones, not replacements for the
historical accounting/agreement/storage milestone numbers.

| Milestone | Bounded deliverable | Exit gate |
| --- | --- | --- |
| C1 native infrastructure | Default-off, separately testable bounded mailbox and bounded read-view ownership, actual annotated Bitcoin locks and reproducible build | Source review, exact test inventory and compiler negative control; no trading/performance claim |
| C2 shared application | Native shared spot ledger/reservations, exact asset/pair units, explicit cash transfers and duplicate outcomes | Differential replay against frozen accounting model; two-market/withdrawal/transfer conflicts, rounding/overflow/ownership conservation |
| C3 order lifecycle | Independent order IDs/revisions, fills/cancels and stable-asset fee/reward accounting | Explicit versioned matching/fee test profile; adverse partial fills, replacement, dust and full result commitments |
| C4 agreement and storage | Approved PBFT profile, actual crypto, exclusive signer ownership, durable intent/signature/decision/application and recovery | Separate-process fault campaign and independent issued-signature checker; no old-backup rollback claims |
| C5 chain/PoS authority | Native authenticated inputs, immediate invalidation, bounded finality undo and required PoS V2 improvement | Reorg/epoch/restart/full-replay comparison and safe membership/bootstrap; producer election is not silently left as completed V2 |
| C6 API and Qt | Independent bounded service lanes, exact outcome recovery, typed controller/models, direct base/quote UI | Held chain/writer locks leave reads/status responsive; market/wallet switch and original-action safety regressions |
| C7 complete spot regtest | Generated custody/assets, four operators, engine-off Qt, deposits, matched trades, cancels and actual withdrawals | Real cryptography/fsync/proofs, live block growth, restarts and predetermined load; measure stronger R1/R2 |
| C8 full product | Approved futures margin/oracle/funding/liquidation/backstop; bridge routes and authoritative V1/V2 cutover | Conservation/liability, stale-oracle, no implicit spot use, anvil+regtest and no duplicate mint/payout/migration |
| C9 release decision | Distributed regtest/ordinary-PC/Windows/macOS soak and separate security review | Exact CI/artifacts and operator rehearsal; only then decide mainnet activation height/hash/rules |

Safe independent work may later overlap, but no dependent gate is skipped.
Spot-first is implementation order: **a spot-only product is not full V2**.
Deposits credit spot first; only explicit authorized transfers move futures cash.
Stable trading fees and FN payouts are separate from B3 network fees. Wider
asset-to-asset markets remain required; non-stable-quote fee valuation still needs
its own rule. Existing bridge restrictions must be reconciled with the later
owner-approved V2 route/contract rulings, not copied blindly from old Stage 0.

No unresolved economic parameter becomes a mainnet default. Matching priority,
fee/dust/recipient rules, futures risk, membership, bootstrap and authoritative
cutover remain explicit ratification gates from the prior plan. Infrastructure
can proceed without choosing them. Ethereum light-client transport replacement
remains deferred, not its verification/freshness requirements.

## C1 implementation boundary

`BoundedMailbox` is an opaque local FIFO, bounded by message count and accounted
payload length. Closing refuses new messages and drains accepted originals;
there is no drop-to-progress operation. Mutexes are Bitcoin annotated types.
Allocation/destruction is kept outside the queue lock where possible. These
limits are not a measurement of total heap, CPU or wire bytes; the future
decoder must also cap frame allocation, spare capacity, per-peer input and work.

`ReadSlot` publishes deeply owned opaque envelopes with claimed scope/parent/
commit/state identities. Move-only permits bound retained readers/old views.
Readers never acquire its publisher mutex or a chain/market lock.
Atomic shared-pointer operations are not promised formally lock-free. Counter contention
does not establish an arbitrary-load latency bound.

Crucially this primitive supplies **no verified, fresh, ready, spendable or final
label** and no certificate verifier or durability barrier. It must not be exposed
as an authoritative API until C4/C5 supply those gates. Publication here means
memory visibility only. The library is not linked to the node, wallet or Qt.
The explicit build option defaults OFF.

Tests use synthetic IDs/bytes, fixed operation counts and disposable build
directories. They cover exact capacity, rejection preserving input, FIFO,
closing/draining, independent queues, concurrency, one-head ownership, moved
permits, slot lifetime and bounded retained readers. No wallet/key/network,
filesystem durability, agreement or real regtest qualification follows.

## Evidence and performance

Keep the historical UI regression evidence. Baseline strict market-switch
20-sample p50/p95: **1258.341/1305.071 ms**. The local `863a30cd` candidate:
**33.491/503.645 ms**; it failed the unchanged 50 ms p95 gate. One bounded
test-only causal trace confirmed obsolete same-market Watch ownership caused
the returning-market wait. It is parked, not declared qualified or full redesign.
The full six-suite after run was not executed. No untested follow-up is applied.
Private evidence is retained in the Development operation directory, not uploaded.

The actual product objective remains **200 ms local median**, first on isolated
regtest, for original preflight through durable client-verified applied/open
result (R1), and separately actual matched fill/fees/deltas (R2).
Admission, inclusion, UI paint and bridge finality are different boundaries.
Foundation throughput and model timings cannot qualify this target.

Freeze profile, hardware, build, durability, crypto, topology, load, liquidity,
sample count and failures before comparison. Report queue/lock/phase/wake-up
attribution and p50/p95/p99 with all requests in the denominator. No WAN target
is inferred by adding a guessed delay to a local median.

## Reference oracles, not production dependencies

- [Frozen accounting profile](../../test/flowmesh_v2_model/TEST_PROFILE.json)
  and [native integration gaps](../../test/flowmesh_v2_fastpath_native/INTEGRATION_GATE.md).
- [Application seam](../../test/flowmesh_v2_agreement/fm_application.py),
  [agreement protocol](../../test/flowmesh_v2_agreement/PROTOCOL.md) and
  [disk recovery plan](../../test/flowmesh_v2_storage/PLAN.md).
- [C1 test profile](../../src/flowmesh2/TEST_PROFILE.json).

C2 ports the relevant semantics into the future C++ core and compares against
existing models; it does not create another Python model or silently approve
their provisional fee/risk assumptions.
