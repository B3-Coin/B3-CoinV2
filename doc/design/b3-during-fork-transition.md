# B3 transition window — the 1,000-block pre-modern process

**Status: AUTHORITATIVE DESIGN DIRECTION (2026-08-15).** This document
supersedes every earlier transition interpretation, in particular the
post-boundary "self-activating bootstrap" analysis that previously appeared
here and in the modern PoS specification §10. Specification only: nothing in
this document is implemented, no consensus code changes with it, and the
`no-modern-pos-rules` fail-closed gate stays in force. Items marked OPEN are
undecided and must not be closed by implementation choice.

## 1. Terminology (use these names; never use "H" ambiguously)

    T = TRANSITION_START_HEIGHT        first block of the transition window
    W = TRANSITION_LENGTH = 1,000      legacy blocks in the window
    F = FINAL_LEGACY_HEIGHT = T+W−1    last legacy block  (= T+999)
    M = MODERN_START_HEIGHT = F+1      first modern block (= T+1,000)
    X = FINAL_LEGACY_HASH              hash of block F
    C = INITIAL_STAKE_CUTOFF_HEIGHT    declaration cutoff for initial ACTIVE

If the transition is colloquially said to "start at H", the exact meaning is:
`H … H+999` are the 1,000 legacy transition blocks and `H+1000` is the first
modern block. The existing code's `hard_fork_height` (first modern height)
and `LegacyFinalHeight()`/`legacy_final_hash` map to **M**, **F** and **X**
respectively — the existing boundary value is **F**, not T. Until
implementation is deliberately revised, every "H"/"H+1" in existing code and
documents reads as **F**/**M**. The only block-format/PoS switch is `F → M`;
`T` is **not** a format switch of any kind.

## 2. Timeline

    ─────────────┬──────────────────────────────┬──────────────────────────
     Phase A     │  Phase B — "during the fork" │  Phase C — modern era
     pure legacy │  T ………… C ………………………… F        │  M = F+1  onward
     genesis…T−1 │  1,000 ordinary legacy blocks│  modern codec + modern PoS
                 │  legacy PoS keeps producing  │  mandatory from M
                 │  stake declarations allowed  │  initial validator set
                 │  ≤C → initial ACTIVE at M    │  already exists
                 │  >C → enters M as PENDING    │
    ─────────────┴──────────────────────────────┴──────────────────────────
                                        └ X = hash(F) attests everything ≤ F

At the legacy six-minute interval, W = 1,000 blocks ≈ 6,000 minutes ≈ 100
hours ≈ 4 days 4 hours. The public software-upgrade notice begins well
before T; the window is the on-chain validator-declaration period, not the
whole operator-notification period.

## 3. The three phases

**Phase A — pure legacy era (genesis … T−1).** Legacy block codec, legacy
transaction codec, legacy `CTxOut`, legacy script rules, legacy PoS.
Existing stakers operate normally. No modern validator registry, no modern
duties of any kind.

**Phase B — the transition window (T … F, exactly 1,000 blocks).** Blocks
remain ordinary legacy-format blocks, produced and validated by legacy PoS;
legacy nodes follow the same historical consensus throughout; legacy stakers
keep the chain operating. There is no modern block production, no modern
rewards, and no modern duties (§9). What is new: **upgraded wallets may
create legacy-compatible modern-stake declarations** (§4) inside ordinary
legacy transactions. These prepare the initial modern validator set and are
committed by the legacy history ending at X. This window is the "meantime
between legacy and modern" that the transition has always meant.

**Phase C — modern era (M = F+1 onward).** Block M is the first modern
block. Modern block codec and modern PoS are mandatory from M; a
legacy-format block at any height ≥ M is invalid; a modern-format block at
any height ≤ F is invalid. The initial modern validator set already exists,
prepared during T…F. Old undeclared legacy UTXOs remain spendable through
`LEGACY_LOCK`; new outputs use the modern Policy Output system.

There is **no** post-F self-activating bootstrap phase, **no** temporary
operator committee, **no** trusted validator list, **no** snapshot-created
validator set, and **no** administrator key producing the first modern block.

## 4. Declarations are legacy-compatible — NOT ModernOutput serialization

Mandatory distinction: blocks T…F must remain valid to legacy software, so
they cannot contain any transaction or output serialization that legacy
consensus cannot decode. Before M, a stake registration is a
**legacy-compatible STAKE declaration**, not a serialized
`modern::ModernOutput`.

Working model:

    ordinary legacy transaction
            │
            ├── principal output
            │     value        = locked B3 principal
            │     scriptPubKey = owner's ordinary legacy lock
            │
            └── canonical declaration data
                  version / domain
                  principal output index
                  validator public key

One encoding **candidate** is an `OP_RETURN`-style companion output carrying
`"B3STAKE/V1" || principal_vout || validator_pubkey`. This is a candidate
only — the exact encoding is **OPEN** until analyzed against: legacy
transaction consensus; legacy script limits; standard relay policy on the
legacy network; transaction-size limits; canonical parsing requirements; and
ambiguity/duplicate-declaration handling. Convenience does not lock it.

The non-negotiable requirement: **old nodes see an ordinary valid legacy
transaction; upgraded nodes additionally recognize a deterministic stake
declaration.**

## 5. Who must use the window (and who need not)

Only a holder who wants **eligibility in the initial modern validator set**
must create a qualifying declaration during T…F. Ordinary holders may leave
legacy UTXOs untouched:

    undeclared legacy UTXO survives F
            ▼
    modern era begins at M
            ▼
    modern transaction spends the old outpoint
            ▼
    frozen legacy script rules validate ownership
            ▼
    new OWNER / STAKE / DEX_VAULT output

There is no forced balance migration, no new genesis allocation, no
rewritten txid, no rewritten outpoint, no mandatory wallet sweep. Only
*immediate* modern-validator eligibility requires declaring before the
cutoff.

## 6. Cancellation is self-policing

The declaration's principal output remains an **ordinary spendable legacy
output** during the window. Spending it before F automatically voids the
declaration; at F, only declarations whose identified principal is still
unspent qualify. No special legacy lock and no pre-F consensus restriction
exists (and none may be created unless separately approved):

    declare stake → change mind before F → spend principal → not qualifying

## 7. Deterministic initial-stake derivation at (F, X)

At the exact final legacy state (F, X), upgraded nodes derive the initial
modern validator registry from the attested legacy history. A declaration
qualifies only if **all** applicable conditions hold:

 1. It appears within the window T … F.
 2. Its transaction is part of the exact chain ending at X.
 3. Its encoding is canonical and version-recognized.
 4. It identifies exactly one principal output without ambiguity.
 5. The principal output contains native B3.
 6. The principal output is still unspent at F.
 7. The amount meets the (eventual) minimum stake requirement.
 8. The validator-key encoding is valid and canonical.
 9. It satisfies the immediate-activation cutoff rule (§8).
10. It is not made ambiguous by conflicting duplicate declarations.
11. Any additional anti-DoS limits are satisfied.

Malformed or ambiguous declarations **never** retroactively invalidate an
otherwise valid legacy block — legacy blocks remain governed by legacy
consensus through F. Instead:

    malformed / ambiguous declaration
            ↓ ignored for initial stake eligibility
            ↓ its principal remains an ordinary legacy UTXO

## 8. Eligibility cutoff C inside the window

A declaration in the final legacy blocks must not automatically control the
first modern block:

    declaration height ≤ C → initial ACTIVE validator set at M
    declaration height > C → valid, but enters M as PENDING and
                              activates under normal modern maturity rules

The mechanism is part of the design; the exact depth `F − C` is **OPEN**
(candidates such as 100 or 200 may be simulated but are not locked).
`W = TRANSITION_LENGTH = 1,000` is the currently selected transition length.

## 9. No modern duties during the window

Declarations are pending preparations only. For every block before M:
no modern proposer eligibility, no VRF evaluation, no modern rewards, no
modern penalties, no slashing, no uptime requirement, no heartbeat, no
committee duty. **Legacy PoS exclusively secures and produces the chain
through F.** Modern duties begin at M.

## 10. Who produces the first modern block

The bootstrap circularity is removed by construction. At F the chain already
contains the deterministic set of qualifying declarations:

    initial active stake registry (derived at F/X)
            ↓ modern eligibility/randomness rule
            ↓ eligible modern proposer
            ↓ first modern block M

The first proposer is selected from the initial validator set prepared
during the window. There is no self-activating block that creates its own
authority, and no "prove ownership and become proposer in the same block."
The exact initial randomness/VRF seed at M is **OPEN** and must be specified
separately.

## 11. Readiness versus consensus fallback

Operational readiness before finalizing the mainnet transition should
require adequate: total declared B3 stake; number of operational validators;
client adoption; validator-key availability; network reachability; software
readiness. Exact thresholds are **OPEN**. A validator-count threshold is a
liveness metric, not Sybil resistance — economic weight comes from declared
stake.

Explicitly forbidden fallback: "F reached, stake inadequate → quietly
continue accepting legacy blocks." Once F/X are final, block M is modern and
legacy continuation is invalid. Any postponement decision happens
*operationally, before* the boundary becomes irreversible (choose a later T
and re-run the window). If declaration participation proves inadequate, the
correct outcomes are: postpone before finalization, or proceed knowingly
with a small (but economically weighted) initial set — never an automatic
legacy-extension rule. No such rule may be invented.

## 12. Crossing mapping at M

A qualifying declaration is never rewritten inside historical storage. At M,
the modern validator derives a **policy view** over the original principal
outpoint:

    stake identity        = original declaration principal outpoint
    stake amount          = exact original legacy output amount
    owner authorization   = exact original legacy scriptPubKey,
                            interpreted under the frozen legacy script rules
    validator key         = canonical key from the declaration
    initial state         = ACTIVE or PENDING per the cutoff rule (§8)

The mapping is **exclusive**: a qualifying declared principal takes the
STAKE policy view; an ordinary undeclared pre-F output takes the
LEGACY_LOCK policy view; no outpoint is ever both. Historical transaction
bytes, txids and outpoints are unchanged. When a bootstrap-origin stake
eventually exits, the owner authorizes under the original frozen legacy
owner script and directs the value into a normal modern `OWNER` output
(exact authorization flow: OPEN). `STAKE` remains the appended modern policy
type; existing serialized policy enum values are never renumbered.

## 13. The X-distribution problem — OPEN

The window solves initial-validator preparation. It does not solve
distribution of the exact final hash X = hash(F), which is unknown until
block F exists. This remains an explicit unresolved operational choice:

- **Option A — wall-clock pause.** F is produced → legacy production stops
  operationally → X recorded and independently verified → the X-pinned
  release/configuration is distributed → the first accepted next-height
  block is modern M. No extra consensus height exists between F and M; only
  a possible wall-clock pause.
- **Option B — on-chain precommit/handoff.** An automatic commitment
  mechanism before F letting M begin without a release pause. Not yet
  designed.

Neither option is chosen. Mainnet T/F/X must never become an ordinary
user-controlled runtime override; regtest may use explicit overrides for
testing.

## 14. Compatibility with completed work

This design keeps every completed piece conceptually intact: one continuous
chain; same genesis; same historical txids/outpoints; TrustedReplay of
legacy history (the window is ordinary legacy history and replays
mechanically, so the initial registry derivation is itself attested by X);
the three-way UTXO equivalence framework; marker-based era enforcement; the
final anchor (F, X); the cross-boundary reorg prohibition once X is active;
the mempool flush when F connects; legacy transaction rejection after F;
legacy block rejection at M; `LEGACY_LOCK` compatibility; the modern PoS
fail-closed gate; D1–D4 hardening; checkpoint and live-legacy `CheckSync`
behavior; the historical reward-rule reconstruction.

The primary future code-level change (not in this mission) is additive:

    TRANSITION_START_HEIGHT = T
    TRANSITION_LENGTH       = 1,000
    INITIAL_STAKE_CUTOFF    = C

plus deterministic recognition of declarations and derivation of the
initial registry. The existing boundary values remain
`FINAL_LEGACY_HEIGHT = F` and `FINAL_LEGACY_HASH = X`.

## 15. Decision status

**LOCKED / DESIGN DIRECTION**

- One continuous chain.
- Legacy PoS continues through F.
- Transition preparation happens during T…F.
- W = 1,000 blocks for the current design.
- Pre-F declarations must remain legacy-compatible.
- Initial modern stake derives deterministically from pre-F facts committed
  by X.
- No operator keys and no trusted validator committee.
- No validator snapshot granting authority from passive historical balances.
- No post-F self-activating bootstrap blocks.
- Only qualifying unspent declarations participate.
- Spending the principal before F voids the declaration.
- No modern duties or rewards before M.
- Ordinary undeclared legacy UTXOs remain spendable after M via LEGACY_LOCK.
- M is the first modern block.
- Once F/X is active, no legacy fallback is allowed.

**OPEN**

- Exact declaration wire encoding (OP_RETURN companion is a candidate only).
- Declaration versioning.
- Validator-key type.
- Minimum stake amount.
- Cutoff depth F − C.
- Minimum total declared stake / readiness thresholds.
- Duplicate-declaration resolution.
- Declaration indexing / state representation in the node.
- Initial randomness/VRF seed at M.
- Exact owner-to-modern exit authorization flow for bootstrap-origin stake.
- X distribution: wall-clock pause vs. on-chain precommit.
- Relay-policy handling of declarations on legacy nodes and upgraded wallets.

No OPEN item may be closed silently by an implementation choice.

## 16. Forbidden interpretations (do not return to these)

- "F reached with no stake → M creates stake and authority in the same
  block."
- "Foundation/operator keys produce the first modern blocks."
- "A passive balance snapshot automatically becomes validator authority."
- "Actual ModernOutput bytes appear in blocks legacy nodes must decode."
- "All holders must migrate all UTXOs during the window."
- "A malformed declaration invalidates an otherwise valid legacy block."
- "Legacy blocks continue after F if modern readiness is insufficient."
- "T is itself the modern block-format switch."

The only block-format/PoS switch is F → M. The 1,000 blocks before it are
the preparation process.

## 17. Destination (unchanged; not blocked by this design)

After the first valid modern blocks: modern PoS → typed Policy Outputs →
asset registry → bridge policies → TEST_USDT and test colored assets →
FlowMesh DEX → spot trading → TEST_USDT/approved USDC-USDT fees → isolated
leverage up to 10× → positions, PnL and liquidation → deterministic
epochs/state roots → microblocks → real bridge and FN systems. FlowMesh
remains account-model execution; UTXOs remain custody/deposit/withdrawal
boundaries.
