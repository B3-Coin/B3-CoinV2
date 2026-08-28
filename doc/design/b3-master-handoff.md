# B3 / B3FlowMesh Master Architecture and Claude Handoff

Status: reconstructed source-of-truth draft, 2026-08-16
Purpose: give Claude the complete project concept without authorizing a giant all-at-once implementation
Scope: B3 transition context, modern colored assets, FN Coin/Proof of Disintegration, FlowMesh DEX, and microblocks

> **Authority note (added when this file was placed in the repository, 2026-08-16).**
> This document is the **top authority** among the design documents in `doc/design/`.
> Where it disagrees with [b3-architecture-contract.md](b3-architecture-contract.md) or any
> other document in this directory, **this document governs**, subject only to the precedence
> order in §0 below (the project owner's later explicit corrections outrank it).
> The older documents are **not** rewritten to match; they remain accurate records of what was
> decided when. Known disagreements are catalogued in
> [b3-master-handoff-conflicts.md](b3-master-handoff-conflicts.md).

> **Owner reconciliation (2026-08-16, recorded after commit `750d983`).** Per this
> document's own precedence order, the project owner's later explicit corrections
> outrank it. The owner has ruled on the FN-claim conflicts:
> 1. **Legacy FN claims:** the integrated funding-key **scan-and-claim** specification
>    (commit `750d983`, [b3-fn-pod.md](b3-fn-pod.md) §8) governs. Funding-key
>    controllers authorize claims; markers are audit metadata only. The
>    **marker-spend design in §4.5 below is SUPERSEDED and must not be implemented.**
> 2. **Modern PoD encoding:** remains OPEN exactly as §4.6 states (unrelated to
>    legacy claims; the owner will lock it separately).
>    *[Since resolved: see the 2026-08-17 reconciliation below.]*
> 3. **Activation:** FN legacy claims activate exactly at the derived height
>    `M = TransitionPowFinalHeight(params) + 1`; there is no independently
>    configured FN activation parameter (supersedes item 13's "configured" wording).
> 4. **§4.4 verification completed:** the historical PoD test correction is commit
>    `d6a30ae`; the evolution suite is green — see the §4.4 note below.

> **Owner reconciliation (2026-08-17).** The owner has since ruled:
> 1. **Modern PoD encoding is LOCKED** (§4.6 below, updated): an implicit
>    on-chain gap with a **validation-only hypothetical disintegration
>    output** — `I >= O + D` enforced at validation, `fee = I − O − D`,
>    the hypothetical amount never serialized, stored, indexed, spendable,
>    or given an outpoint, and never miner-claimable. Full normative text:
>    [b3-fn-pod.md](b3-fn-pod.md) §10.1.
> 2. **FN lifecycle:** an FN-preserving spend (same-PoDId successor)
>    transfers the FN with its rewards/perks; an ordinary B3 spend is
>    valid and permanently extinguishes the FN (never recreatable from the
>    same PoDId). [b3-fn-pod.md](b3-fn-pod.md) §10.2.
> 3. A same-day proposal applying the hypothetical-output idea to the
>    LEGACY claim anchor (virtual claim outpoints materialized into the
>    UTXO set) was **rejected in full** and must not be revived; §8 of
>    b3-fn-pod.md remains the governing legacy claim design
>    (conflict register C-R3).
>    *[Since superseded in turn — see the 2026-08-17/18 note below.]*

> **Owner reconciliation (2026-08-17/18, second legacy-FN supersession).**
> The owner's later explicit ruling replaces the legacy FN mechanism again.
> The **OWNER-LOCKED direction** is the **archival-builder /
> stateless-proof issuance model**
> ([b3-legacy-fn-issuance-proposal.md](b3-legacy-fn-issuance-proposal.md),
> conflict register C-R4): legacy disintegrations stay plain confirmed
> legacy transactions; ONE archival wallet (no special authority) builds
> proof-carrying FN issuance transactions from the sealed prefix, holds
> them privately before M, and broadcasts them at or after M; every node
> verifies the embedded merkle/value evidence STATELESSLY against the H/X
> anchor (the chain view's block at H must be exactly X before any root is
> trusted). No funding-key claim signatures, no user claim process, no
> every-node legacy rescan, no production PodDB. The recipient rule is the
> historical client's own FN registration rule: the lowest-index 1-B3
> P2PKH output of the disintegration designates the recipient, and a
> disintegration without one is IGNORED (no fallback). Issuance
> uniqueness is per PoDId. The scan-and-claim affirmation in the
> 2026-08-16 note above is superseded to that extent — its rejection of
> the §4.5 marker-spend mechanism stands.

## 0. How this document must be used

This document is the canonical context for future Claude sessions. It is not an instruction to implement every section at once.

Every statement has one of these statuses:

- **LOCKED** — an architectural invariant. Claude must not change it.
- **ACCEPTED MILESTONE** — work or test behavior reported as accepted, but the exact repository state must still be verified.
- **DESIGN DIRECTION** — the intended direction, but details remain to be specified before consensus implementation.
- **OPEN** — unresolved. Claude must not choose an answer while coding.
- **SUPERSEDED** — an older prompt or model that must not be revived.
- **VERIFY IN REPOSITORY** — reported history, commit, symbol, or behavior that must be checked against the current checkout.

Precedence when sources disagree:

1. Explicit later corrections by the project owner.
2. Frozen historical B3 behavior proven from the legacy source and real chain data.
3. The latest approved architecture/specification documents.
4. This reconstructed handoff.
5. Older brainstorms and old Claude prompts.

If the current source code conflicts with a LOCKED rule, Claude must report the conflict. It must not silently redefine the rule to match the code. Existing code describes implementation state; it is not automatically the desired protocol.

## 1. One system, three economic roles

### 1.1 One continuous B3 chain — LOCKED

B3 remains one continuous Bitcoin-derived chain.

- No new genesis for the modern era.
- No balance snapshot that replaces historical ownership.
- No rewriting historical transaction IDs or outpoints.
- Historical `CTxOut` data remains historical `CTxOut` data.
- Legacy UTXOs remain spendable after the modern transition through frozen legacy ownership rules.
- The modern era adds typed assets, policies, modern PoS, FlowMesh, and microblocks on top of the same economic history.

### 1.2 Separation of roles — LOCKED / DESIGN DIRECTION

```text
B3 Coin + STAKE outputs
    -> secure the canonical B3 base chain through Modern PoS

FN Coin + active FundamentalNodes
    -> operate/certify the FlowMesh microblock layer

Approved USDC/USDT-like assets
    -> trading settlement, accounting, and FlowMesh fee denomination
```

The B3 base chain and FlowMesh are not competing chains.

- B3 Modern PoS is the canonical custody, settlement, and finality layer.
- FlowMesh is the fast deterministic execution layer.
- Microblocks are fast temporary/certified execution updates inside the B3 system, not a sovereign second blockchain.
- B3 PoS validators do not automatically receive FlowMesh fees.
- FN Coin must not silently become ordinary B3 PoS weight.

## 2. Chain transition context that FlowMesh must inherit

This document is not reopening the transition corridor, but Claude needs the context so it does not design assets or microblocks against the wrong chain model.

### 2.1 Consensus timeline — LOCKED current direction

```text
Historical B3:
    heights 0..500
    legacy B3 PoW

Legacy era after height 500 through H:
    legacy B3 PoS

Transition corridor:
    H+1 .. H+1000
    modern block/transaction/output family
    temporary historical-B3 scrypt PoW eligibility

Modern PoS:
    M = H+1001 onward
```

Important separation during the corridor:

```text
block identity/hash domain = modern block identity
PoW eligibility hash       = historical B3 scrypt against nBits
```

The corridor does not revert to the old legacy block codec, legacy output model, or original early-chain reward schedule.

### 2.2 H and X — LOCKED

- `H` is the final legacy-PoS height.
- `X` is the exact final legacy block hash at H.
- The prefix through X is the historical anchor.
- Mainnet H/X must remain unset until the real-history equivalence gate is passed.
- No runtime mainnet override may invent H/X.
- There is no post-H fallback to legacy PoS.
- The corridor must not silently extend or switch consensus because participation is low.

### 2.3 Modern ownership crossing — LOCKED

Historical outputs remain byte-for-byte historical:

```text
legacy outpoint
legacy nValue
legacy scriptPubKey
```

A modern transaction may spend a legacy output through a non-mutating `LEGACY_LOCK` view:

```text
legacy CTxOut
    -> B3_NATIVE amount
    -> LEGACY_LOCK policy view
    -> frozen legacy script validation
    -> modern OWNER / STAKE / other allowed output
```

No historical transaction or outpoint is rewritten.

### 2.4 STAKE rules already established — LOCKED

- A STAKE output is a modern output.
- Owner/cold authority and validator/hot authority are separate.
- Multiple qualifying STAKE outputs assigned to one validator identity are aggregated.
- Splitting one economic stake into many UTXOs must not create extra validator identities or lottery attempts.
- `STAKE_ACTIVATION_DEPTH = 20` means exactly:

```text
current_height - creation_height >= 20
```

- Before that boundary the stake is PENDING and contributes zero active weight.
- Spending/cancelling a stake removes it from the active registry according to the defined lifecycle.

### 2.5 Modern PoS v1 boundary — CURRENT PRIORITY

The transition corridor and full evolution scenario are accepted except for critical bugs. Do not keep polishing the corridor indefinitely.

Modern PoS v1 should be deliberately small:

1. Produce one genuine Modern PoS block at H+1001 from the existing mature `ValidatorRecord` registry.
2. Have an independent second node validate it.
3. Add only the liveness machinery necessary for a usable v1 chain.

Mandatory liveness behavior:

- variable PoS eligibility target/difficulty;
- deterministic round-based recovery;
- one validator identity gets one deterministic eligibility evaluation per round, never nonce grinding;
- failed rounds relax eligibility;
- recovery does not instantly reset to an impossible target;
- a stale best-known tip alone does not disable staking;
- a node must distinguish being behind from being synchronized to a stalled network;
- restart and independent sync reproduce the same PoS state.

Do not put committees, BFT finality, automatic slashing, delegation, multiple schemes, or speculative cryptography into the first release merely because they might be useful later.

## 3. Modern colored assets

### 3.1 Typed ModernOutput — LOCKED

Modern colored assets belong in the modern output model, not by adding fields to historical `CTxIn` or `CTxOut`.

Conceptually:

```text
ModernOutput {
    asset;
    amount;
    policy_type or scheme_type;
    policy_version or scheme_version;
    commitment;
}
```

Existing prototypes have used policy names including:

- `OWNER`
- `LEGACY_LOCK`
- `STAKE`
- `DEX_VAULT`
- `BRIDGE`
- `BURN`

Some later discussions introduced a hierarchy where a larger Scheme contains coordinated policies. That hierarchy is **OPEN** for future stateful objects. It must not cause already serialized enum numbers or output encodings to be casually renumbered.

### 3.2 TransitionProof separation — LOCKED direction

Modern spending proves that the current object/state exists and that the requested transition is authorized. It does not replay the object's complete ancestry.

The transition identity should exclude large/changeable proof material, while a full/witness identity may commit to the full serialization.

Authorization must bind at least:

- chain/domain;
- policy or scheme type;
- version;
- operation;
- inputs/current-state commitment;
- outputs/new-state commitment;
- asset;
- amount where applicable;
- anti-replay data.

Prototype proofs that accept any nonempty OWNER payload or only compare a public legacy-script preimage are not production authorization.

### 3.3 Asset identity and conservation — LOCKED direction

Each asset needs a stable deterministic `AssetId` derived from a canonical issuance fact. The exact preimage/encoding must be verified from the current implementation before it is treated as final.

For every asset independently:

```text
authorized issuance
+ inputs
= outputs
+ visible authorized burns
```

Required behavior:

- no silent asset loss;
- no unauthorized reissuance;
- no cross-asset value substitution;
- no overflow or negative amount behavior;
- splits and merges preserve exact units;
- explicit burn is visible and auditable;
- activation is deterministic and height/deployment based, not a production runtime switch.

Reported model tests used distinct stable IDs for `TEST_TOKEN` and `TEST_USDT`, and covered transfer, three-way split, three-input merge, exact burn, conservation failure, and unauthorized mint. **VERIFY IN REPOSITORY.**

### 3.4 Different assets may have different issuance mechanisms — DESIGN DIRECTION

Not every colored asset must use one generic minting rule.

Examples:

- ordinary authorized/fixed-supply asset;
- bridge-backed USDC/USDT asset;
- algorithmically backed asset;
- an asset with its own PoW issuance policy;
- FN Coin created/recognized only through B3-specific PoD rules.

FN Coin is not an ordinary "enter a name and supply in a wizard" token.

### 3.5 Fees — LOCKED separation

```text
normal B3 base-chain transaction fee -> native B3
FlowMesh trading fee                 -> approved stablecoin AssetId
```

The FlowMesh whitelist must identify exact approved `AssetId` values. A fake asset with the symbol `USDC` or `USDT` must never qualify merely because its display metadata matches.

Base-chain liveness must not depend on Circle, Tether, a bridge, or any stablecoin issuer.

### 3.6 Test assets before real bridges — LOCKED sequencing

Initial FlowMesh development uses test-only assets, for example:

```text
TEST_USDT
TEST_B3 or native-B3 test accounting
later TEST_BTC / TEST_ETH if useful
```

`TEST_USDT` may use a faucet/configurable supply in regtest. It has no mainnet meaning and does not define the future real USDT bridge.

### 3.7 Issuance interface and GUI — LOCKED sequencing

Protocol and validation come first, then wallet/RPC or a shared issuance API, then the GUI.

The eventual non-technical GUI should:

- use the same canonical transaction builder/API as CLI/RPC clients;
- keep private keys in the user's wallet;
- show network, issuer, asset ID, amounts, permissions, and fees;
- clearly mark permanent choices;
- prevent duplicate submission;
- recover from interruption;
- export an issuance/claim record.

FN historical claims and modern PoD creation require their own guided flow. They must not be disguised as generic asset issuance.

## 4. FN Coin and Proof of Disintegration

### 4.1 Historical FN creation — LOCKED FACT

Historical B3 FundamentalNode creation used **Proof of Disintegration (PoD)**. It was not an OP_RETURN burn-output mechanism.

Historical collateral tiers reported from legacy source:

```text
height <= 85,000       -> 25,000,000 B3
85,001 .. 105,000      -> 20,000,000 B3
height > 105,000       -> 15,000,000 B3
legacy testnet          -> 15 B3
```

Exact code boundaries and amount units must remain verified against frozen `master` source and real transactions.

The transaction accounting was:

```text
ordinary_fee
= total_inputs
- required_FN_disintegration
- ordinary_outputs
```

Equivalently:

```text
total_inputs - ordinary_outputs
= required_FN_disintegration + ordinary_fee
```

The disintegrated amount:

- had no output;
- was not an ordinary transaction fee;
- could not be claimed by the block producer;
- permanently disappeared from B3 spendable supply.

The customary GUI flow asked for collateral plus 1 B3. The 1-B3 output acted as the on-chain marker/outpoint used by the masternode-style identity flow.

### 4.2 Historical identity limitation — LOCKED FACT

The historical chain can prove the PoD transaction, gap, tier, height, and marker output. The operator key/public-key binding was partly carried through P2P FundamentalNode broadcasts, not fully committed on-chain.

Therefore a purely chain-derived migration cannot reliably recover every historical operator registration. It must use a deterministic on-chain ownership anchor. No off-chain administrator list may silently become consensus.

### 4.3 Historical supply accounting — LOCKED

Historical disintegrated B3 remains destroyed.

```text
historical B3 destroyed by PoD
    -> historical FN right/lineage
    -> possible modern FN claim
```

Minting or recognizing FN Coin must never recreate the destroyed B3 or count it as spendable B3 supply.

### 4.4 Historical PoD test correction — ACCEPTED MILESTONE

The old evolution-test convention that used an exact OP_RETURN burn was historically wrong and is superseded.

The corrected test must exercise authentic PoD accounting with a small regtest-only collateral parameter while preserving mainnet tiers. It must prove:

- the required gap exists;
- the collateral is not a miner fee;
- spendable supply falls by the destroyed amount;
- invalid gap/wrong amount/fake marker fails;
- restart, reindex, replay, and independent derivation agree;
- duplicate processing does not create duplicate FN recognition.

The user reported that Claude fixed this. The exact commit and current test result must be verified in the repository.

> **Verified (2026-08-16).** The correction is commit `d6a30ae` ("test: authentic
> Proof of Disintegration in the evolution scenario"), with the collateral test
> override in `Consensus::Params::legacy_fn_collateral_test_override` and mainnet
> tiers untouched (pinned by `legacy_pos_tests`). Test command:
> `./build/bin/test_bitcoin --run_test=b3_evolution_tests` — green ("*** No errors
> detected"), 2026-08-16, macOS arm64 (Darwin 25.5.0), ~5m15s runtime; every listed
> property is asserted, including restart/reindex/replay-mode agreement and
> derivation via raw block + undo data.

### 4.5 Historical PoD -> FN claim — APPROVED IMPLEMENTATION DIRECTION

> **SUPERSEDED IN PART (owner reconciliation, 2026-08-16).** The marker-based
> items below (4, 6, 7, 8, 11) describe the FIRST approved mechanism, which the
> owner subsequently replaced with the integrated funding-key scan-and-claim
> (commit `750d983`, [b3-fn-pod.md](b3-fn-pod.md) §8): eligibility requires no
> marker rule, markers are audit metadata only, funding-key signatures authorize
> claims, uniqueness is the reorg-managed `claimed[pod_id]` flag, and activation
> is exactly the derived M (item 13's "configured" height is likewise
> superseded). The marker-spend mechanism must not be implemented. The remaining
> items (1, 2, 5, 9, 10, 12, 14, 15, 16) stand as written and are encoded in the
> governing specification.

The claim set is derived deterministically from the attested historical prefix using raw blocks and the required undo/spend information. X attests the prefix; the exact claim-set anchor parameter must be verified/locked in code and specification.

MVP policy decisions:

1. One eligible historical PoD creates exactly one indivisible FN Coin.
2. Eligibility comes only from the attested historical prefix.
3. A PoD must satisfy the exact historical tier and canonical PoD accounting.
4. The MVP requires exactly one canonical 1-B3 marker under the approved marker rule.
5. Ambiguous/non-standard PoDs are recorded for audit but excluded from MVP issuance; no fallback is invented.
6. The marker must be unspent at the claim-set anchor and when claimed.
7. Markers already spent before the anchor have no MVP claim.
8. Control of the eligible marker's frozen legacy script authorizes the claim.
9. A claim transaction explicitly identifies FN-claim intent and supplies the FN destination.
10. An ordinary marker spend must never accidentally mint FN.
11. The valid claim consumes the eligible marker through `LEGACY_LOCK` and creates one FN ownership object.
12. Claims do not expire unless a future consensus activation adds expiry.
13. Claim validation activates only at a configured FN activation height at or after M.
14. Before activation, claim-shaped data cannot mint FN.
15. Connect/disconnect/reorg/replay must reverse or reproduce marker consumption and FN creation atomically.
16. No administrator allocation, manual list, or off-chain ownership database affects consensus.

Still verify/specify before coding the consensus portion:

- exact eligible-PoD scanned height range and inclusive endpoints;
- exact claim-set anchor;
- whether X directly commits a claim root or merely commits the prefix from which it is derived;
- exact canonical marker-selection rule;
- exact activation height;
- exact FN `AssetId`/object identifier and serialization;
- fee input/change behavior for the marker's 1 B3;
- mempool conflicts, replacement policy, indexes, DoS bounds, and compatibility behavior.

### 4.6 Modern FN creation — ENCODING LOCKED (2026-08-17), LOCKED ECONOMIC INTENT

The intended economic lineage is:

```text
destroy B3 through a B3-specific modern PoD operation
    -> create an explicit on-chain FN ownership object/FN Coin
    -> activate/lock FN Coin to operate FlowMesh infrastructure
```

Modern ownership must be fully on-chain, unlike the historical P2P operator binding.

The contradiction this section originally recorded — an implicit PoD
accounting gap vs an explicit visible modern BURN policy (because modern
conservation forbids hidden gaps) — is **RESOLVED by owner ruling
2026-08-17** in favor of the **implicit on-chain gap with a
validation-only hypothetical disintegration output**
([b3-fn-pod.md](b3-fn-pod.md) §10.1; conflict register C-R3): for a
recognized modern FN-creation transaction, validation temporarily
includes the required disintegration `D` in the output total, enforces
`I >= O + D`, and computes the ordinary fee as `I − O − D`. The
hypothetical amount is never serialized, stored, indexed, spendable, or
given an outpoint; it never enters the UTXO set or any persistent state;
and it is never miner-claimable. Conservation's objection is answered
because the gap is consensus-validated, not hidden.

Generic `BURN` and FN-specific `PoD` remain semantically distinct; the
locked encoding does not reuse the BURN primitive.

### 4.7 FN Coin's FlowMesh role — DESIGN DIRECTION

FN Coin is intended to grant the right to operate/certify FlowMesh microblocks.

An active FundamentalNode may perform:

- FlowMesh action propagation;
- microblock proposal;
- microblock certification;
- state-root agreement;
- data-availability service/certification;
- withdrawal-receipt certification.

Current simple direction:

```text
one active FN Coin -> one microblock seat
```

This avoids making `100 FN Coins` automatically equal one node with `100x` vote weight. Whether an operator may run multiple seats, how seats are sampled, committee size, certification threshold, rotation, liveness penalties, and activation/exit rules are **OPEN**.

FN Coin does not silently add B3 PoS weight.

### 4.8 FN economics — DIRECTION LOCKED / CREATION CURVE PINNED (2026-08-28)

```text
demand for FlowMesh capacity
    -> operator obtains/creates FN Coin through PoD
    -> B3 is destroyed
    -> FN is activated
    -> operator serves microblocks
    -> operator may earn approved stablecoin trading fees
```

**Locked direction (owner rulings 2026-08-17 through 2026-08-28; full
normative text in [b3-fn-pod.md](b3-fn-pod.md) §11):** FN has both a
**limited total supply** (`MAX_FN_EVER_CREATED = 5,000`) and a
**deterministically increasing creation cost**. The equivalence-gated
report at height 807,709 established a floor of 3,500 historical rights;
the mandatory through-H report fixes final R before FN activation, and
every final right is reserved perpetually. The price table allows at most
1,500 modern slots: 15,000 B3 for slots 1–500, 30,000 B3 for 501–1,000
and 60,000 B3 for 1,001–1,500. Additional final-H rights reduce the
reachable modern suffix one-for-one. Evaluation is nondecreasing integer
base-unit arithmetic; historical issuance does not advance M, and
extinguishment never reopens capacity. Supply invariants remain
`0 <= H <= R`, `R + M <= C`, `H + M = A + X`, remaining modern capacity
`= C − R − M` — [b3-fn-pod.md](b3-fn-pod.md) §11.1.
The combination is intentional: FN
is a scarce, freely transferable market asset whose future creation
becomes progressively more expensive. Legacy rights come first: every
qualifying historical PoD's right is reserved before modern creation
opens, legacy claimants pay no modern cost, modern issuance never
consumes a reserved slot, and unclaimed legacy rights remain reserved
perpetually absent a future explicit expiry policy. Historical claims do
not advance the modern cost curve. Scarcity counts total-ever-created;
extinguishment (§4.6 lifecycle, b3-fn-pod.md §10.2) reduces active
supply only and never reopens a creation slot.

Exact fee distribution remains OPEN. Possible destinations include active
FNs, insurance, treasury, or another approved allocation. B3 PoS
validators do not automatically receive the FlowMesh fee pool. The
remaining OPEN numerical decisions (reward amount/schedule and reward
cutoff) carry decision tables
in [b3-fn-pod.md](b3-fn-pod.md) §11.5.

## 5. FlowMesh DEX

### 5.1 FlowMesh is account-model execution — LOCKED

Do not represent each trade as a UTXO mutation.

```text
UTXO layer
    -> deposit
DEX_VAULT policy output(s)
    -> finalized/idempotent credit
FlowMesh internal state
    -> available balances
    -> reserved balances
    -> demand/order intent
    -> trades
    -> margin and positions later
    -> fees and PnL later
    -> withdrawal receipt
DEX_VAULT transition
    -> UTXO withdrawal
```

UTXOs provide custody entry and exit. Trading happens in deterministic FlowMesh state.

### 5.2 Deposits — LOCKED requirements

Every deposit has an immutable identity, normally derived from the exact outpoint or modern transition identity plus output index.

- Credit only after the required B3 finality condition.
- Deposit ingestion is idempotent.
- Replaying the same deposit cannot credit twice.
- Asset, amount, destination FlowMesh account, vault/shard, and finality reference must be unambiguous.

### 5.3 Spot trading first — LOCKED sequencing

Initial production target:

- deposits;
- internal balances;
- one spot market such as B3/TEST_USDT;
- submit/update/cancel intent;
- deterministic matching/clearing;
- partial fills;
- trading fees in TEST_USDT;
- settlement;
- withdrawal receipts;
- UTXO withdrawal.

Do not start with cross margin, perpetual funding, ADL, portfolio margin, real bridge assets, or production oracle economics.

### 5.4 Demand functions and deterministic clearing — DESIGN DIRECTION

The original B3FlowMesh model uses trader demand/holding functions:

```text
d_i(p) = desired holding of trader i at price p
```

The engine clears a canonical intent set deterministically, using integer/fixed-point arithmetic and explicit tie-breaking/rounding.

UI actions such as buy, sell, update, and cancel may compile into this canonical intent model. The exact production choice between a demand-curve-native wire format and a more conventional order interface is **OPEN**. Claude must not build two incompatible matching engines accidentally.

For superseding intent such as a demand curve, the newest valid version for the same account/market may replace older versions. Additive, consumptive, and sequential actions may require different normalization rules. Those action-semantics rules must be specified before they become consensus.

### 5.5 Determinism — LOCKED

Consensus/execution output must not depend on:

- arrival order;
- `unordered_map`/`unordered_set` iteration;
- wall clock;
- locale;
- filesystem order;
- pointer identity;
- thread scheduling;
- nondeterministic randomness;
- floating point;
- non-canonical serialization.

Use integer fixed-point values with defined price ticks, quantity lots, fee units, margin units, checked wide intermediates, explicit overflow rejection, and explicit rounding direction.

Authentication must occur before equivocation/dedup decisions can let an invalid action suppress a valid one.

Action IDs must commit to every field required to distinguish semantically different authorized actions.

### 5.6 Canonical action processing — DESIGN DIRECTION

Where ordering matters:

- each account uses a strict nonce/version discipline;
- per-account sequence is deterministic;
- cross-account ordering uses an explicitly canonical rule;
- set sealing/certification yields the same action set on every executor.

Where order does not matter, normalize by semantics rather than preserving arbitrary arrival history. Examples under consideration:

- `REPLACE` — highest valid version wins;
- `ACCUMULATE` — combine exact additive values;
- `CONSUME` — exactly one consumer of an object/value;
- `SEQUENCE` — retain an explicit ordered workflow.

These classes are a design direction, not yet permission to create a general-purpose VM.

### 5.7 Keyless DEX vault — LOCKED

No private key, multisig committee, or administrator may arbitrarily withdraw the FlowMesh custody pool.

```text
finalized valid FlowMesh withdrawal receipt
    -> permissionless transaction construction
    -> DEX_VAULT consensus validation
    -> exact user payment
    -> forced remainder back to a valid vault successor
```

The certification network certifies state; it does not possess custody keys.

### 5.8 Withdrawal receipts — LOCKED requirements

Conceptual receipt:

```text
WithdrawalReceipt {
    account;
    asset_id;
    amount;
    destination;
    nonce;
    epoch;
    shard;
}
```

Conceptual immutable ID:

```text
H(
  "B3/FLOWMESH/WITHDRAWAL"
  || shard
  || epoch
  || account
  || nonce
  || asset
  || amount
  || destination
)
```

Consensus must prove/check:

- receipt validity;
- certification/finality;
- correct vault/shard;
- ownership/destination;
- exact asset and amount;
- one-time consumption;
- exact payment;
- forced vault change/remainder;
- atomic connect/disconnect/replay behavior.

One-time consumption cannot remain an in-memory caller convention.

### 5.9 Vault sharding — LOCKED direction

Do not force the entire DEX through one globally contended vault UTXO.

Shard custody deterministically, potentially by asset, shard number, and epoch rotation. The user cannot choose a convenient shard to bypass rules. Exact sharding and aggregation rules are OPEN.

### 5.10 FlowMesh fee assets — LOCKED direction

FlowMesh trading fees are charged in the approved quote/settlement stable asset, initially TEST_USDT in regtest. The fee asset and exact `AssetId` are part of market configuration/consensus state.

Maker/taker schedules, insurance allocations, rebates, and FN distributions remain OPEN.

### 5.11 Leverage follows spot — LOCKED sequencing

> **Correction (2026-08-20, per the owner's Codex repair directives —
> recorded, not silently rewritten):** the ONLY locked futures facts are
> "FlowMesh will support futures" and "maximum leverage = 10×". Every
> mechanic below — isolated margin, the 10% initial-margin figure, the
> Position shape, the 5% test threshold — is HISTORICAL BRAINSTORM and
> OPEN, not approved design. The sequencing rule itself (leverage only
> after deterministic spot works) remains locked.

LOCKED here is only the SEQUENCING: futures work of any kind begins only
after deterministic spot works. The locked futures facts remain exactly
two — futures will be supported, and maximum leverage is 10×.

*(A historical sketch that previously stood in this section — an
isolated-margin test harness with a Position shape, a 10% initial-margin
figure, a 5% maintenance test threshold, and a `MarkPriceProvider`
interface — was NEVER approved design and was removed from normative
text on 2026-08-20 per the Codex repair directive. Every futures
mechanic — margin mode, margin parameters, funding, mark/oracle price,
liquidation, insurance, ADL — is an OPEN owner decision recorded in
[b3-flowmesh-dex-decisions.md](b3-flowmesh-dex-decisions.md).)*

First liquidation version is deterministic full isolated liquidation. Production insurance and liquidation economics remain OPEN.

### 5.12 Existing prototype risks — VERIFY AND FIX BEFORE WIRING

Earlier code audits reported:

- signed overflow in `EvalCurve`;
- arrival-order-dependent batch deduplication;
- authentication/equivocation ordering bug;
- stale curve reservation after partial fills;
- empty-curve crash/undefined behavior;
- no idempotent deposit identity/finality gate;
- receipt finality not actually checked;
- receipt consumption not persisted/consensus-atomic;
- bare slot counter not anchored to B3;
- FlowMesh fees hardcoded to native B3;
- no stablecoin whitelist.

These reports are not substitutes for a fresh audit of the current checkout.

## 6. Microblocks and epochs

### 6.1 Not a second blockchain — LOCKED

Microblocks do not create a separately sovereign history or custody system.

They are the fast execution/certification path inside B3FlowMesh. B3 remains the final canonical chain.

### 6.2 Ordered temporary-state chain — DESIGN DIRECTION

Let `S[e]` be the last B3/macro-finalized state for epoch e.

```text
T0 = S[e]
T1 = F(T0, M1)
T2 = F(T1, M2)
...
Tn = F(Tn-1, Mn)
```

A later B3 macro/finality commitment finalizes the current valid microblock tip:

```text
S[e+1] = Tn
```

This means microblocks are not merely unordered action buckets followed by one execution at the end. They may create a rapidly evolving temporary ordered state.

Before the final B3 anchor, that state is certified/provisional according to the eventual microblock rules. User-facing receipts must distinguish inclusion, temporary execution, certification, and B3 finality.

### 6.3 DEX superseding/recomputed semantics — DESIGN DIRECTION

FlowMesh demand-state updates may use a special batch-native rule.

Let `I[i]` be the canonical intent state after microblock i. A candidate DEX settlement may be recomputed from the fixed finalized epoch base:

```text
Candidate[i] = Clear(S[e], I[i])
```

Each newer microblock can supersede the previous candidate rather than incrementally applying trades to the previous candidate.

At finality:

```text
FinalDEXState = Clear(S[e], final_intent_state)
```

The latest valid candidate and the final deterministic recomputation must agree.

This is compatible with a single ordered microblock chain having two kinds of state transition:

```text
incremental:
    T[i+1] = F(T[i], action[i])

superseding/recomputed:
    T[i+1] = G(S_epoch_base, I[i+1])
```

The exact production scope of each semantic class is OPEN and must be specified narrowly. Do not generalize this into an unbounded VM.

### 6.4 FN-operated microblock layer — DESIGN DIRECTION

Expected lifecycle:

```text
trader actions
    -> propagated to active FNs
    -> canonical action set / intent state
    -> deterministic execution
    -> microblock proposal
    -> FN certification and data-availability evidence
    -> temporary state/receipt root
    -> periodic commitment/finality on B3
```

The exact producer selection, number of active seats, committee sampling, certification threshold, failure recovery, leader rotation, view change, data-availability certificate, and penalty rules are OPEN.

### 6.5 Minimum microblock commitments — OPEN SPECIFICATION

Before implementation, specify whether a microblock commits to:

- B3 anchor/parent final state;
- epoch and microblock number;
- previous microblock ID;
- canonical action/input root;
- resulting temporary state root;
- trade/receipt/withdrawal roots;
- fee totals;
- producer seat;
- certification bitmap/signature/proof;
- data-availability certificate;
- protocol version and domain.

Claude must not choose an encoding or signature system just to make code compile.

### 6.6 Finality and withdrawals — LOCKED direction

- Microblock inclusion is not automatically the same as final B3 settlement.
- The UI/API must describe provisional/certified/final states accurately.
- A DEX withdrawal is redeemable from the keyless vault only after the required finality rule.
- B3 reorganization/rollback rules must atomically agree with FlowMesh deposit credits, state commitments, receipt validity, and receipt consumption.

### 6.7 Build execution before networking consensus — LOCKED sequencing

First prove deterministic FlowMesh execution independently:

```text
FlowEpoch {
    epoch_id;
    previous_state_root;
    canonical_actions_or_intents[];
    deterministic test inputs;
}

FlowEpochResult {
    trades;
    fees;
    withdrawals;
    liquidations later;
    new_state_root;
}
```

The same input on independent nodes must produce byte-identical outputs and state roots.

Only then wrap the proven execution engine in the actual FN microblock proposal/certification/networking protocol.

### 6.8 Latency is a target, not a consensus fact — LOCKED

Hyperliquid-class responsiveness is a product/performance objective. Do not hard-code a latency claim as a consensus constant without measurement, simulation, and a separately approved protocol decision.

### 6.9 Initial cryptographic restraint — LOCKED sequencing

Do not start v1 by inventing threshold encryption, speculative BFT, new signature aggregation, or a general smart-contract VM.

Public actions and deterministic certification are acceptable for the first functional version. Encryption/commit-reveal may later be required for market fairness after a concrete last-look/MEV analysis, but it must be introduced as its own reviewed protocol upgrade.

## 7. Current build order

The present execution order is:

```text
1. Freeze the accepted transition corridor except for critical bugs.

2. Complete minimal usable Modern PoS v1.

3. Production-activate typed Asset Policies and conservation.

4. Finalize historical PoD -> FN claim specification and implementation.

5. Lock modern FN PoD encoding and implement modern on-chain FN ownership.

6. Build the active FN registry/seat lifecycle needed by microblocks.

7. Build and adversarially test a standalone deterministic FlowMesh spot harness
   with TEST_USDT.

8. Integrate deposits and keyless DEX_VAULT withdrawals with the B3 UTXO layer.

9. Specify and implement FN-produced/certified microblocks and B3 anchoring.

10. Enable TEST_USDT fee accounting/distribution under explicit test rules.

11. Add futures support within the locked 10× maximum (every mechanic —
    margin mode, PnL treatment, mark price, liquidation — is an OPEN
    owner decision to be made before this step).

12. Design/activate real stablecoin bridges and approved real fee AssetIds later.
```

This order provides the full end-state as context while giving Claude only one current implementation task at a time.

## 8. Reported implementation state — verify before relying on it

Reported milestones from earlier project sessions include:

- legacy consensus port and transition machinery;
- modern block identity/codec separation;
- `LEGACY_LOCK` crossing;
- STAKE carrier, 20-block activation, and validator aggregation;
- 1,000-block transition corridor on regtest;
- a 2,300-block full evolution test covering 300 legacy PoW, 1,000 legacy PoS, and 1,000 transition-PoW blocks;
- restart, reindex, and independent-node equality for that synthetic scenario;
- asset models and tests behind test-only activation;
- corrected authentic historical PoD testing;
- FlowMesh ledger/curve/batch/vault models, not fully wired to production validation.

Reported reference commits included `a8ad010` for an earlier integrated stack and `5de6b75` for the full evolution scenario. These references may be stale. The current branch, HEAD, later commits, working tree, build, and test status must be audited from the repository before new work.

Do not equate "model exists" with "production feature is wired."

## 9. Superseded ideas — do not revive

The following are superseded or explicitly rejected:

- Historical FN creation as an OP_RETURN or ordinary BURN output.
- Treating destroyed historical FN collateral as a miner fee.
- Recreating destroyed historical B3 during FN migration.
- Recovering historical FN ownership from an administrator-maintained operator list.
- FN Coin automatically adding B3 PoS weight.
- Making every DEX trade a UTXO transaction.
- A private key or committee multisig controlling the DEX vault.
- FlowMesh trading fees silently paid in native B3.
- Accepting an asset as USDC/USDT by display name or ticker alone.
- Starting with real USDT/USDC before TEST_USDT and deterministic asset rules.
- Starting leverage before spot trading works.
- Starting with cross margin, funding, ADL, or portfolio margin.
- Choosing `last_trade_price` or clearing price as the production mark oracle without a protocol decision.
- Treating microblocks as an independent blockchain.
- Treating microblocks only as unordered action collection with no temporary state.
- A giant Claude prompt that asks it to implement the entire end-state.
- Allowing Claude to invent OPEN consensus rules while solving implementation details.
- Reopening the accepted transition corridor for optional redesign rather than a demonstrated critical bug.
- A pre-H mixed-transaction/declaration bootstrap model; it was superseded by the 1,000-block modern-format transition-PoW corridor.
- Calling the first valid H+1001 block the whole of Modern PoS v1 while omitting liveness recovery.

## 10. Open decision register

Claude must keep these visible and unresolved until the project owner approves exact answers:

### FN / PoD

- modern PoD wire/accounting encoding: implicit gap vs explicit visible destruction primitive;
- modern PoD cost/tier/curve;
- exact historical claim-set anchor and activation height;
- exact canonical marker rule and handling of all audited exclusions;
- FN `AssetId`, object serialization, activation, exit, and transfer rules;
- whether one operator may run multiple seats;
- committee/seat selection, certification threshold, rotation, and penalties;
- stablecoin fee allocation among FNs, insurance, treasury, or other destinations.

### Colored assets

- final AssetId preimage and issuance serialization;
- generic issuer authority and revocation rules;
- metadata commitment/update rules;
- which policy/scheme abstraction is finalized;
- activation heights/deployments;
- real stablecoin bridge and redemption design;
- governance/update procedure for the accepted fee-asset whitelist;
- PoW-issued asset policy, if retained.

### FlowMesh spot

- demand-curve-native wire format vs order interface compiled to canonical intent;
- exact uniform/batch clearing algorithm and tie-breakers;
- canonical action normalization and cross-account ordering;
- market precision/tick/lot configuration;
- maker/taker schedule and fee distribution;
- deposit finality depth and vault sharding algorithm;
- state commitment data structure and light-client proof plan.

### Microblocks

- exact microblock serialization and ID;
- cadence/epoch length;
- active FN registry snapshot rule;
- proposer and committee selection;
- quorum/certificate format;
- data-availability proof/certificate;
- view change and stalled-layer recovery;
- equivocation evidence and penalties;
- exact B3 anchoring cadence and rollback/finality rule;
- which state transitions are incremental versus superseding/recomputed.

### Leverage

- production mark/oracle construction;
- maintenance-margin curve;
- liquidation pricing/penalty;
- insurance and bad-debt rules;
- funding/perpetual model, if ever enabled;
- later cross-margin or portfolio-margin design.

## 11. Claude's operating contract

Claude acts as implementation engineer and adversarial reviewer, not autonomous protocol architect.

Rules:

1. Read this entire document and the repository's authoritative design files before touching code.
2. Audit the current branch/HEAD/worktree/build/tests before relying on reported history.
3. Treat this whole document as read-only context unless a section is explicitly named as the current task.
4. Implement one bounded milestone at a time.
5. Do not refactor unrelated stable code.
6. Do not invent an OPEN parameter, hash preimage, serialization, activation height, fee split, cryptographic primitive, or economic rule.
7. If a LOCKED rule conflicts with code, stop and report the exact conflict with files/symbols/tests.
8. Preserve historical serialization, transaction IDs, outpoints, supply accounting, and frozen legacy behavior.
9. Do not use a test bypass, hard-coded privileged key, mutable production-global flag, or runtime consensus switch to make a test pass.
10. Every consensus change needs activation boundaries, connect/disconnect behavior, restart/reindex/replay behavior, independent-node agreement, and adversarial rejection tests.
11. Show planned files and consensus impact before editing.
12. Keep each commit focused, buildable, documented, and reviewable.
13. Run focused tests plus the relevant regression suite.
14. Show the complete diff and test results before moving to another milestone.
15. Never automatically start the next phase.

## 12. Alignment-pass prompt

This authorizes no code changes. Use it when a fresh session needs to re-establish ground truth.

```text
Read doc/design/b3-master-handoff.md completely.

This is the canonical project context, not an instruction to implement the
whole project. Do not edit files, create commits, switch branches, or start a
feature yet.

First inspect the current repository and return an evidence-based alignment
report with these sections:

1. Current branch, HEAD, worktree state, and recent relevant commits.
2. Build and relevant test status.
3. For every LOCKED invariant in the master document:
   - matches current code;
   - partially implemented;
   - not implemented;
   - conflicts with current code.
4. Exact current state of:
   - Modern PoS v1;
   - typed assets and conservation;
   - corrected historical PoD testing;
   - historical marker-spend FN claims;
   - modern FN creation;
   - active FN registry;
   - FlowMesh ledger/clearing/vault;
   - microblocks and B3 anchoring;
   - TEST_USDT fees;
   - leverage.
5. Every older implementation or document that reflects a SUPERSEDED idea.
6. Every OPEN decision that currently blocks safe implementation.
7. The smallest next implementation milestone that follows the locked build
   order, with exact files, tests, consensus impact, and acceptance criteria.

Reference actual files, symbols, tests, and commit hashes. Do not infer that a
feature is production-ready because a header/model/unit test exists. Do not
choose any OPEN decision. Stop after the report and wait for approval.
```

After that report is returned, review it against this document. Only then send a
second, narrow prompt for the single current milestone.

## 13. Reconstructed history used for this handoff

This document was reconciled from the project conversations titled:

- `B3 Blockchain Architecture`
- `Modern Era CTxOut`
- `Claude's Role in Migration`
- `Coloured Coins Hard Fork`
- `UTXO Spend Proof Design`
- `Bitcoin Renaming and PoS`
- `Smart Contracts with Epochs`
- `Upgrading Old Chain`
- `Old Chain Upgrade`
- `Clarify colored coin issuance GUI`

The latest explicit corrections were given priority. Older prompts remain useful evidence, but they are not allowed to override the locked/superseded classifications above.
