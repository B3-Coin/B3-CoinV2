# Proof of Disintegration — the B3 Fundamental Node creation mechanism

**Status: DESIGN DIRECTION (2026-08-16).** Historical mechanism authoritative
(traced from `master`); modern direction locked at the level below, with the
economics OPEN. This document supersedes any earlier description of FN
creation as a burn output. No modern FN economics, governance, rewards or
FlowMesh coupling are implemented; Modern PoS and the transition corridor
are unchanged by this design.

## 1. The historical mechanism — authoritative

Legacy B3 creates a Fundamental Node through **implicit destruction via the
transaction accounting gap** — Proof of Disintegration (PoD):

    total inputs  >  ordinary outputs

    ordinary_fee = total_inputs − ordinary_outputs − FN_collateral

Traced facts (`master`):

- The collateral schedule (`fn-activity.h GetFNCollateral`): 25,000,000 B3
  through height 85,000; 20,000,000 through 105,000; 15,000,000 after;
  testnet 15 B3 past height 60.
- The disintegrated amount **has no output**. It is not a fee: both the
  mempool path (`AcceptableFundamentalTxn`, `main.cpp:740`) and
  `ConnectBlock` (`main.cpp:1643`) compute
  `fee = in − collateral − out`, so the block producer cannot claim it and
  it permanently leaves the spendable supply. The only on-chain evidence is
  the gap itself.
- The wallet flow sends collateral **+ 1 B3**; the 1-B3 output is the FN's
  registration marker. `CFundamentalnode` identity is a `CTxIn vin` bound
  to operator pubkeys via **network-layer broadcasts** (fn-manager /
  fn-activity), masternode-style.

**Never describe historical FN creation as an explicit burn output.**
Historical bytes remain historical; replay reproduces the state effect
mechanically (the gap simply never re-enters the UTXO set), and the ported
fee rule (`legacy::GetLegacyTransactionFee`) plus the per-index
`m_legacy_fn_integrated` aggregate preserve the accounting during live
validation.

### What chain data alone cannot recover

The operator/pubkey registration lived in P2P messages, not blocks. From
chain data alone one can recover: every disintegration transaction, its gap
amount, its height, and its outputs (including the customary 1-B3 marker,
and therefore *whoever can spend that marker output*). One can NOT recover:
the historical operator pubkeys, service addresses, ping/activity history,
or any network-layer binding. Claim derivation (OD-5) must therefore be
defined over the recoverable facts — the natural candidate being
"beneficiary = controller of the disintegration transaction's marker
output" — and never over network-layer state.

## 2. Modern FN creation preserves PoD

Modern B3 keeps Proof of Disintegration as the FN creation mechanism — it
is NOT replaced by a generic BURN output. The economic signature is
preserved: **B3 is permanently sacrificed, and an FN right is created.**

    Modern FN creation transaction

    Inputs:   B3 being disintegrated
    Outputs:  ordinary change / payment outputs
              one explicit FN ownership (FN Coin) output
    Gap:      exactly the required PoD amount (plus the ordinary fee)

    invariant:  inputs − outputs − ordinary_fee = PoD amount

The PoD amount remains permanently excluded from B3 supply; the miner can
never claim it; a modern FN creation transaction is explicitly recognizable
by consensus (no ambiguity between fee and destruction).

### PoD is not BURN

    BURN  → generic, visible destruction of an asset (asset engine)
    PoD   → B3-specific economic transformation:
            B3 permanently sacrificed AND an FN right / FN Coin created

Both exist; they are never merged. Generic asset destruction uses the BURN
policy; FN creation uses PoD.

## 3. Modernized ownership: on-chain, no P2P registration

The historical weakness is fixed, not the mechanism: a modern FN creation
transaction explicitly creates an **on-chain FN ownership object** (the FN
Coin output) canonically identifying the FN creation identity, the owner
authority, policy/version, and any future consensus-required FN state. No
separate network-layer registration establishes ownership.

## 4. FN Coin is a separate asset/state from B3

If 25,000 B3 (example numbers) is disintegrated and 1 FN Coin is created:

    B3 supply      −= 25,000 (permanently)
    FN Coin supply += 1      (by FN rules, independently accounted)

The FN Coin never "recreates" the destroyed B3; destroyed B3 never returns
to circulating supply. Two ledgers, one event.

## 5. One PoD event, at most one FN

    PoDId = the modern FN creation transaction identity
            (or its designated creation outpoint — exact identifier OPEN)

    one valid PoD event  → at most one FN creation
    same PoD reused      → INVALID / no second FN

Replay, restart, reindex and branch handling must never double-create FN
state.

## 6. Lineage

    LEGACY FN                          MODERN FN
    =========                          =========
    historical PoD                     modern PoD
    implicit input/output gap          implicit input/output gap
    historical collateral schedule     modern PoD amount (OPEN)
    historical marker + P2P binding    explicit on-chain FN ownership
    aggregate-only chain accounting    FN Coin / FN policy object
                                       deterministic replay protection

Related, distinct, same economic signature.

## 7. Testing

The evolution test (`b3_evolution_tests`) exercises the AUTHENTIC
historical mechanism in its legacy phase: a real disintegration transaction
(gap = collateral + fee) inside a legacy PoS block, with the collateral
recognized exactly once (`m_legacy_fn_integrated`), the destroyed amount
excluded from the claimable fee (a coinstake claiming it is refused),
spendable supply reduced by exactly the collateral, an
insufficient-gap/fake-marker transaction creating no FN state, and the same
facts reproduced after restart, chainstate reindex, and on a
trusted-replay-mode synced node from raw block + undo data alone. The
regtest fixture uses a small collateral via
`Consensus::Params::legacy_fn_collateral_test_override`; **the mainnet
historical schedule is untouched and must never change.**

Future modern-FN tests (not yet implementable): valid PoD → exactly one FN;
insufficient PoD → reject; same PoD reused → reject; FN ownership transfer
deterministic; restart/reindex → identical FN state; B3 supply permanently
reduced; FN Coin supply independently accounted.

## 8. Legacy FN claims — NORMATIVE MVP (integrated scan-and-claim)

Approved direction (2026-08-16, revised): FN eligibility is derived
entirely during node synchronization from on-chain history, and claims are
authorized by **fresh signatures from the historical funding keys** — the
keys that actually paid the disintegration. There is no external scanner,
no external claim registry, and no marker-based ownership. This section is
normative.

### 8.1 Activation and anchor — deterministic network values

**Activation height = `M = TransitionPowFinalHeight(params) + 1`** — a
pure function of the existing consensus parameters
(`hard_fork_height + transition_pow_length`, `src/consensus/era.h`). Not a
node-local setting, command-line option, or independently configurable
value; no new parameter exists. Regtest moves it only by overriding the
parameters it derives from. Without a corridor-configured boundary the
function has no value and claims are inactive everywhere (fail closed).

**Anchor — stated precisely:** eligible PoD events occur in the historical
prefix ending inclusively at `H = LEGACY_FINAL_HEIGHT`; their metadata
(including funding-input scripts) is reconstructed from that prefix;
`X = hash(H)` attests and seals the prefix and resulting state. The
eligibility set is DERIVED from the attested prefix and reproducible by
every node; X does not itself contain or commit to it as a structure.

**Node behavior around activation (no divergence surface):** claim rules
ship in the same release as the corridor rules (the H/X-pinning release).
Legacy era: modern transaction forms cannot appear (era codec gate) — no
rule fires. Corridor (H+1 … M−1): every corridor node uniformly rejects
claim-form outputs (`fn-claim-inactive`); no older corridor population
exists. Legacy clients never follow past H (dead fork). From M, claims
validate per 8.4.

### 8.2 Eligibility — derived during sync, reindex and replay

Every node synchronizes from block 1; the deterministic PoD scan runs
inside normal sync/reindex/trusted-replay (input values and funding
scripts from the connect/undo path — the proven mechanism; no separate
scanning pass, no optional index, no local configuration).

A **qualifying PoD** is a transaction `P` at height `h ≤ H` on the chain
ending at X: non-coinbase, non-coinstake, with
`Σinputs(P) − Σoutputs(P) ≥ GetFNCollateral(h)`. The disintegrated value
determines WHETHER `P` qualifies; **every qualifying PoD creates exactly
one FN eligibility event** regardless of the amount above the tier.

For each qualifying PoD the node records:

    PodRecord {
        pod_id            = txid of P            (the PoDId)
        height            = h
        disintegrated     = Σinputs − Σoutputs   (the gap)
        tier              = GetFNCollateral(h)   (the satisfied tier)
        funding_scripts   = the DISTINCT set of prevout scriptPubKeys of
                            P's inputs, reconstructed from undo data
        marker            = audit metadata only: the customary 1-B3
                            output(s), if any. Markers determine NOTHING —
                            not ownership, not beneficiary, not
                            eligibility.
        claimable         = whether every funding script is an
                            MVP-supported key form (below)
    }

**MVP-supported funding script forms:** P2PKH and P2PK (the key-based
forms — "proving control of the original funding keys"). A qualifying PoD
with any funding script outside these forms is recorded with
`claimable = false` and audit reason `UNSUPPORTED_FUNDING_SCRIPT`; no
fallback is invented in the MVP. Identity is NEVER inferred from marker
outputs, change heuristics, or input ordering — only the literal set of
funding scripts.

### 8.3 Claim authorization — fresh signatures from the funding keys

A wallet identifies its claim rights by recognizing funding scripts it
controls. To claim, it produces — for **every distinct funding script** of
the PoD (one authorization per distinct script, NOT per input) — a fresh
authorization signature over the canonical claim message:

    claim_message = TaggedHash("B3/FN/CLAIM/V1",
                               chain_domain || pod_id
                               || canonical_fn_destination_script)

    chain_domain  = ModernChainDomain — the contract's immutable
                    anti-replay network identifier,
                    H("B3/MODERN/CHAIN" || genesis_hash || X)
                    (b3-architecture-contract.md; derived from the genesis
                    hash and X per the existing project convention), so an
                    authorization is valid on exactly one network.
    pod_id        = 32 raw bytes, internal hash order.
    canonical_fn_destination_script
                  = the exact owner-suffix bytes of the FN ownership
                    output, serialized with a compact-size length prefix.

    authorization = { funding_script_index (compact size),
                      pubkey (33-byte compressed or 65-byte uncompressed,
                              must parse as a valid public key),
                      signature (strict DER, LOW-S required, over
                                 claim_message) }

The distinct funding scripts are canonically ordered lexicographically by
script bytes and indexed 0..n−1; authorization records must appear in
ascending index order, exactly one per index — none omitted, none
duplicated, none extra. The signature **binds the PoD right to one exact
FN destination** on one exact network. Copying an authorization into
another transaction is harmless: it can only mint the same PoDId to the
authorized destination, and `claimed[pod_id]` prevents a second successful
claim. A PoD funded by several owners requires a valid authorization for
every distinct funding script; one wallet holding several historical keys
may produce all of them.

Consensus verifies each authorization against the RECONSTRUCTED historical
funding script: P2PKH — HASH160(pubkey) must equal the script's 20-byte
hash; P2PK — the pubkey must equal the script's embedded key byte-for-byte;
then the signature must verify over the claim message. Missing,
duplicated, mis-ordered, or invalid authorizations invalidate the claim.

### 8.4 The claim transaction

A claim is a MODERN transaction (height ≥ M) carrying:

1. **One FN ownership output** — the spendable ownership object:

        scriptPubKey = PUSH36(envelope) OP_DROP <owner locking script>

        envelope (exactly 36 bytes):
          bytes 0..3    magic "B3F1" = 0x42 0x33 0x46 0x31 (v1; future
                        versions change the magic)
          bytes 4..35   pod_id: the 32 raw bytes of the PoD txid in
                        transaction-serialization (internal hash) order

        push rule: minimal direct push (opcode 0x24 = 36). PUSHDATA
        variants, wrong lengths, zero txid → INVALID "bad-fn-claim".
        The owner locking script suffix is the FN destination; its
        satisfaction under modern rules is how the owner later proves
        authority. The fn_destination_script signed in 8.3 is EXACTLY
        this suffix.

2. **One authorization proof output** (unspendable, zero value):

        scriptPubKey = OP_RETURN PUSH("B3FP" || pod_id)
                       PUSH(auth_1) [PUSH(auth_2) …]

        one push per authorization, canonical serialization, ascending
        funding_script_index order, exactly one authorization per
        distinct funding script — nothing missing, nothing extra,
        nothing duplicated.

        Size limits: each push ≤ MAX_SCRIPT_ELEMENT_SIZE (520 bytes; one
        authorization ≈ 107–139 bytes, fits easily); the whole script ≤
        MAX_SCRIPT_SIZE (10,000 bytes) ⇒ ≥ ~70 authorizations per proof
        output — far beyond any plausible historical PoD. Modern-era
        mempool policy must accept well-formed B3FP proof outputs (a
        policy carve-in, commit 6; stock datacarrier limits would refuse
        them). CAPACITY GATE: commit 2 must report the maximum number of
        distinct funding scripts across all eligible historical PoDs; if
        every real claim fits this encoding it is retained, otherwise it
        is revised before serialization is implemented so no
        otherwise-valid PoD is stranded.

3. Ordinary inputs paying fees and any ordinary outputs. The claim spends
   NO historical outpoint; the marker, if it still exists, is untouched
   and irrelevant.

Validation at connected height ht, for every FN-claiming output:

    ht < M or no corridor            → INVALID "fn-claim-inactive"
    envelope malformed               → INVALID "bad-fn-claim"
    duplicate pod_id among outputs   → INVALID "fn-claim-duplicate"
    pod_id ∉ PodRecords or !claimable→ INVALID "fn-claim-ineligible"
    pod_id already claimed           → INVALID "fn-already-claimed"
    proof output missing/mismatched  → INVALID "fn-claim-missing-auth"
    any authorization fails 8.3      → INVALID "fn-claim-bad-auth"
    else: mint exactly one FN {pod_id, ownership outpoint, owner suffix,
          claim height}; FN Coin supply += 1; claimed[pod_id] = true

**FN TRANSFER** is unchanged from the approved conservation rule and is
distinguished by the authorizing INPUT: spending an existing FN ownership
output (owner suffix, modern rules) requires exactly one successor FN
ownership output with the SAME pod_id — else "fn-transfer-no-successor";
transfers never mint, never split/merge, never burn (MVP), and several
independent FNs may move in one transaction under the both-directions
pod_id bijection. A transfer carries no proof output and no funding
signatures. A transaction that neither spends the FN object nor carries a
valid claim proof for a pod_id it emits is INVALID
"fn-claim-missing-authority". Wallets present one FN asset balance with
per-object PoD provenance.

**Explicit invariants:**

- Ordinary B3 value accounting is fully independent of FN issuance: a
  claim neither creates nor destroys B3; the historical destruction
  already happened at the PoD.
- Every qualifying PoD creates exactly one FN, regardless of the
  disintegrated amount or which tier it satisfied.
- PoDId uniqueness: the gap is a per-TRANSACTION property, so at most one
  PoD event exists per transaction and the txid alone is a sufficient
  PoDId. If any future rule ever introduced sub-transaction PoD events,
  the PoDId would have to gain a canonical event index — under the MVP
  definition no such case exists.
- One authorization per DISTINCT funding script, never per input.
- Authorization records have a deterministic (lexicographic-script,
  ascending-index) order and can be neither duplicated nor omitted.
- Exactly one FN ownership output must match each initial claim.
- Marker ownership, marker spending and marker destination have no effect
  whatsoever on eligibility or beneficiary selection.

### 8.5 State: eligibility, claimed flags, reorganization

Persisted node state, all reorg-managed and reconstructible:

- **PodRecords** — derived once during sync/reindex/replay (8.2),
  persisted crash-safely, immutable after H is final, auditable
  bit-for-bit by recomputation over the X-anchored prefix.
  Pruning-compatible: derived while blocks/undo ≤ H are available and
  persisted before pruning may discard them; a node that pruned without
  deriving must reindex.
- **claimed[pod_id]** — set on claim connect, cleared on claim disconnect
  (standard undo discipline), so no disintegration event can EVER yield
  two FNs: any second claim on any branch fails "fn-already-claimed"
  while the first is connected, and reorganizations deterministically
  reverse FN creation together with the flag.
- **FN registry + FN Coin supply** — as before: connect/disconnect
  symmetric, byte-identical across restart, reindex and trusted replay.
  No administrator list, manual allocation or off-chain ownership
  database affects consensus.

### 8.6 Test matrix (implementation commits must cover)

| Case | Expected |
|---|---|
| Valid claim, single-input PoD, at height ≥ M | 1 FN minted, claimed flag set, supply +1 |
| Claim exactly at M | valid (first active height) |
| Claim at M−1 (corridor) | INVALID fn-claim-inactive |
| Second claim of the same PoDId (any branch) | INVALID fn-already-claimed |
| Two FN outputs with one PoDId in one tx | INVALID fn-claim-duplicate |
| Multi-input PoD, all funding owners sign | valid; 1 FN |
| Multi-input PoD, one distinct funding script missing | INVALID fn-claim-missing-auth |
| Authorization signed for a different destination | INVALID fn-claim-bad-auth |
| Authorization replayed into a different tx, same destination | mints only to the signed destination; harmless (documented) |
| Non-qualifying gap (below tier) | not in PodRecords; claim INVALID fn-claim-ineligible |
| Qualifying PoD with unsupported funding script | audit UNSUPPORTED_FUNDING_SCRIPT; claim INVALID fn-claim-ineligible |
| Marker spent at any time, any way | irrelevant to the right; claim still valid |
| Claim with malformed envelope / proof output | INVALID bad-fn-claim / fn-claim-missing-auth |
| Wrong pubkey for P2PKH funding script | INVALID fn-claim-bad-auth |
| Transfer chain after claim | same pod_id, new outpoints, supply unchanged |
| Transfer omitting successor | INVALID fn-transfer-no-successor |
| Transfer with changed pod_id | fails its authority check (fn-claim-missing-authority) |
| One tx, several independent FN transfers | valid; bijection both directions |
| Reorg across the claim | FN removed + claimed flag cleared atomically; reconnect identical |
| Reorg across claim + transfers | ownership chain rolls back and replays identically |
| Restart / reindex / trusted replay | identical PodRecords, audit set, claimed flags, FN state, supply |
| Activation-boundary behavior | claim-form invalid at M−1, valid at M, on two independently synced nodes |

## 9. Decision status

**LOCKED / DESIGN DIRECTION:** historical FN creation = PoD; PoD is
implicit destruction through the accounting gap; PoD value is never a miner
fee; PoD permanently reduces B3 spendable supply; modern FN creation
preserves PoD rather than generic BURN; modern FN ownership is explicit and
on-chain; FN Coin is separate from B3 supply; one PoD event creates at most
one FN; historical and modern mechanisms share lineage but not encoding;
mainnet historical collateral rules unchanged.

**LOCKED for the legacy-claim MVP (2026-08-16, integrated scan-and-claim):**
every qualifying historical PoD (gap ≥ tier, through H) → exactly one FN
eligibility event and at most one indivisible FN Coin; eligibility derived
during normal sync/reindex/replay from the attested prefix only — no
external scanner, no external registry, no optional index; recorded per
PoD: PoDId/txid, disintegrated value, satisfied tier, distinct
funding-input locking scripts; the customary marker is audit metadata only
and never determines ownership or beneficiary; claims authorize by fresh
signatures from the historical funding keys binding PoDId + FN destination,
verified against the reconstructed funding scripts — one valid
authorization per distinct funding script, all required, identity never
inferred from markers, change heuristics or input ordering; explicit claim
intent + FN destination required; one-claim-per-PoD enforced by a
reorg-managed claimed flag keyed by PoDId; perpetual claims; activation
exactly at M = TransitionPowFinalHeight + 1 (derived, never node-local);
pre-activation claim forms invalid; safe post-claim conservation (same-
PoDId successor rule, no split/merge/burn); no administrator list or
off-chain ownership anywhere in consensus.

**OPEN (unchanged by the MVP):** modern (post-legacy) FN PoD amount; FN
Coin issuance rate for NEW modern PoDs; dynamic vs fixed pricing;
excess-gap treatment; ordinary-fee calculation for modern FN creation;
final FN ownership serialization beyond the MVP claim form; FN Coin
lifecycle beyond the MVP same-PoDId transfer rule; FN reward economics
(OD-4); claim expiry (none unless a future consensus change introduces
one). Implementation must not close these silently.
