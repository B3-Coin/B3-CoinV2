# B3 Open Decisions

Decisions that are **not locked**. Nothing here may be resolved by implementation choice.
If work appears to require settling one of these, **stop and request the decision**.

Locked material lives in [b3-architecture-contract.md](b3-architecture-contract.md).

---

## OD-1 — Modern PoS consensus specification — **V1 MECHANISM FROZEN (2026-08-20); numerics REVISABLE**

**Status: the V1 mechanism is FROZEN by explicit owner rulings (M1–M6,
2026-08-20) and implementation is authorized.**
[b3-modern-pos-spec.md](b3-modern-pos-spec.md) is the frozen V1 specification:
deterministic stake-weighted hash eligibility over a chained seed, exact
deterministic timestamps encoding recovery rounds, BIP340 validator block
signatures, PoS-native height/round fork choice with a fixed reorganization
horizon, and an unconditional modern coinbase cap. No VRF, epochs, committees,
slashing, finality gadget, or delegation in V1 (all V2 research, spec §10).

The V1 numerics are ratified: block interval 60 s, round length 30 s, f0 = 1,
×2 relaxation, sentinel bits 0x207fffff, future drift 120 s, horizon D = 1440,
minimum stake 333 modern B3, and the STAKE v1 carrier. Corridor subsidy is
fees-only and `transition_pow_bits = 0x1f008000`. The only monetary value that
cannot be known before the seal is integer R0, derived from S_H under OD-2.

Further RATIFIED 2026-08-21: the horizon D = 1440 (one day at the 60 s
interval) and `min_stake_amount` = 333 modern B3 (kB3; 333e9 base units),
both stated on mainnet and inert until H/X.

Resolved by the frozen spec (formerly listed as undefined): stake eligibility
(STAKE policy outputs, aggregated per key, 20-block depth); the selection
function (tagged-hash digest over the seed chain — no separate retarget: the
`w/W` normalization is the difficulty); reward cap (unconditional, outside the
validator); PoS block structure (coinbase key declaration + trailing BIP340
signature); block signature scheme (BIP340); validator set (the derived stake
registry; no committees; finality is fork-choice-plus-horizon only in V1).

Constraints already locked and not open for reinterpretation:

- The modern block header stays Bitcoin-style; it is **not** redesigned merely because
  consensus is PoS (contract §17).
- Modern PoS must be complete before FlowMesh (contract §53).
- Validators and FlowMesh FNs are separate roles (contract §52).

**Required to unblock mainnet activation:** observe and pin X/S_H/R0; reproduce
and pin the exact FN manifest/count/root; pin the ruled post-M A1/A2 activation
heights; pass the final-H equivalence, shadow-fork rehearsal, and release
verification gates. The consensus mechanism and remaining numeric policies are
closed.

**The transition model is AUTHORITATIVE design direction (2026-08-16):**
[b3-during-fork-transition.md](b3-during-fork-transition.md) — the
**temporary-PoW corridor**. Both earlier models (post-boundary
self-activating bootstrap; the 1,000-block legacy-PoS declaration window)
are SUPERSEDED. Timeline: Genesis…500 historical B3 PoW
(`LAST_POW_BLOCK = 500`); 501…H legacy PoS with X = hash(H) the immutable
anchor; H+1…H+1000 temporary PoW reusing B3's existing scrypt PoW primitive
in **modern-format blocks** with Policy Outputs active, in which holders
spend legacy UTXOs once (LEGACY_LOCK) into real modern STAKE outputs that
mature undisturbed (legacy coinstake churn is why declarations inside
legacy PoS were abandoned); M = H+1001 first modern-PoS block, starting
from the registry derived deterministically at the end of H+1000. Corridor
length 1,000 is a locked count; per-validator weight aggregation is LOCKED
(no per-UTXO lottery tickets); `STAKE_ACTIVATION_DEPTH = 20` (mature iff `h − b ≥ 20`) is the
preserved design number; after H, legacy PoS never resumes. Operator keys,
snapshots, committees and self-authorizing blocks remain forbidden.

**Mainnet activation stays gated** on the corridor document's remaining
OPEN list — updated 2026-08-29 (the earlier version of this paragraph
went stale and contradicted the ratifications recorded above): the
corridor difficulty VALUE is PINNED (0x1f008000, 2026-08-23), the
minimum stake amount and horizon D are RATIFIED (333 modern B3 / 1440,
2026-08-21, stated on mainnet and guard-tested), and X-distribution is
RULED (PAUSE, fail closed, 2026-08-23). Actually still open: the
corridor reorg-depth bounds and §7 mitigations, the spec-§9 provisional
values (sentinel bits, future drift — shipped but revisable until the
X-pin ratification), and the numeric R0 (measured at H). Ruled out of existence on 2026-08-21: cutoff C
(the 20-block activation depth alone governs), the readiness/minimum-stake
consensus gate (operational, options A+C), and the corridor
reward/capture question (ratified 0, fees-only, fail-closed). The earlier
PD-1..17 register is superseded by the frozen V1 spec; the initial seed at
M is resolved (spec §3).

---

## OD-2 — Modern B3 monetary policy — **RULED 2026-08-26**

Treasury = ONE single wallet/address (no multisig). At M, the native block
subsidy is `R0 = floor(S_H × 1% / 525,600)`, halves every 525,600 blocks, and
is split 90/10 between producer and treasury. The PoW corridor has zero
subsidy and pays native transaction fees only. The exact integer R0 remains a
seal-derived release pin, not an open policy choice. Simple-v1 asset issuance
pays 1,000 native B3 to the same treasury. FlowMesh charges 100 ppm of matched
native-B3 notional and routes 20% to the treasury. Native B3 is never issued by
the colored-asset engine.

---

## Colored assets simple-v1 — RULED 2026-08-22 (formerly audit Decision 11)

Owner rulings: (1) asset identity unified with the FN convention —
`AssetId = TaggedHash("B3/ASSET/V1") ‖ ModernChainDomain ‖ issuance_outpoint
‖ H(genesis record)` (chain-bound, rule-bound; supersedes the untagged
outpoint-only derivation and reconciles contract §21 by strengthening it);
(2) the v1 asset-wide rule set is exactly `max_supply`, `decimals` and
`mint_authority = NONE`, stated once in the issuance transaction's creation
action and never repeated on outputs; (3) the genesis mints exactly
`max_supply` in one transaction and no later mint exists, so the cap holds
by construction without a registry. ROOM FOR EXPANSION (owner direction,
same day): the genesis record carries an `issuance_mode` byte (v1 accepts
only GENESIS_FIXED; AUTHORITY_MINT, POW_MINED — PoW-minable colored assets
— and BRIDGE_BACKED are RESERVED numbers) plus a bounded `mode_params`
blob (empty in v1), so a future mode ships its parameters inside the same
record layout and the same AssetId preimage with no format change;
`max_supply` is the hard cap in every mode. Programmable schemes remain V2
research. The owner separately activated the narrowly specified
`BRIDGE_BACKED` bUSD design on 2026-09-01; it is not simple-v1 issuance and
cannot be selected by a ticker or generic mode parameters. Implemented in
`src/modern/asset.h` and the production transaction path: B3A1 parsing,
fixed-supply conservation, A2 fail-closed activation, and the 1,000 B3 treasury
payment are wired. The 2026-08-31 owner correction requires explicit burns to
be B3A1 `PolicyType::BURN` outputs; no asset/FN carrier uses `OP_RETURN`.
Exact A2 remains a transition-release pin.

## OD-3 — Consensus governance / upgrade mechanism

Contract §43 and §55 require a deterministic activation mechanism and a governed
`AcceptedFeeAssets` registry, but the governance mechanism itself is undefined: who may
propose and ratify later registry changes. The transition schedule itself is
ruled: FN Genesis at 810,001 with ordinary coinbase maturity, modern FN PoD at
post-M A1, simple-v1 assets plus FN seat pre-binding at A2, and working
FlowMesh spot trading at A3 after at least a 30-block runway. The exact
A1/A2/A3 heights remain pending owner pins for the sealed transition release;
the production height-gating framework fails closed while any value is absent
or contradictory.

---

## OD-4 — FN supply economics — **CLOSED except later rewards/bond policy**

Contract §51 originally left the issuance curve outside consensus. The old
"every 25 FN → price doubles" scheme remains **rejected**. Owner rulings
through 2026-08-31 pin the lifetime cap at 5,000, final modern capacity at
`5,000 - R`, and the modern price table to 15,000 / 30,000 / 60,000 B3
over successive 500-unit tiers. The height-807,709 report proves at least
3,500 historical rights; the seal-pause run fixes R. Production type-6
modern-PoD validation, branch-local counting, capacity enforcement, tiered
destruction, and fee separation are implemented; the exact A1 pin remains
pending. Bond size and any reward/revenue policy beyond the ruled spot-fee
allocation remain OPEN. Spot-v1 revenue is closed: 100 ppm
of matched native-B3 notional, split 80% equally across active FN seats and
20% to treasury.

**Creation mechanism locked (2026-08-16, [b3-fn-pod.md](b3-fn-pod.md)):** modern
FN creation preserves the historical **Proof of Disintegration** — implicit
destruction through the transaction accounting gap, never claimable as a fee,
never a generic BURN output — modernized with an explicit on-chain FN
ownership (FN Coin) output. FN Coin is a separate asset/state from B3; one
PoD event creates at most one FN. The modern PoD amount and maximum issuance
quantity are now selected by the pinned table. Remaining OPEN items are the
exact A1 height, bond/reward policy, and revenue distribution. Ownership,
transfer authorization, and excess-gap treatment are implemented from the
later owner rulings.

---

## OD-5 — historical FN derivation — **CLOSED 2026-08-31**

The final rule is the independently reproduced canonical through-H manifest:
non-coinbase, non-coinstake, gap at least the historical tier, and a 1-old-COIN
byte-exact P2PKH designation output; the lowest-index match is the owner. The
transition release pins the full manifest, R, and Merkle root. Block 810,001
coinbase issues one amount-1 FN output per row. There are no later claims or
holder proofs. The codec, deterministic builder, mandatory coinbase
production/validation, and B3A1 owner authorization are implemented;
independent sealed-history reproduction, the exact manifest pins, review, and
rehearsal remain release gates tracked by the runbook.

---

## OD-6 — FlowMesh epoch ↔ B3 finality relationship — **RULED 2026-08-22**

`FLOWMESH_ANCHOR_DEPTH = 30` blocks (~30 min at the 60 s interval) governs
deposit recognition, anchor acceptance, and receipt redeemability alike;
distinct from `MODERN_REORG_HORIZON = 1440`. Certificate finality (yes,
FlowMesh-state only, conflicting certificate = evidence + fail-safe halt),
the keyless receipt-vault (ratified, redeemability = certified ∧ anchor
canonical ∧ buried ≥ depth ∧ not consumed), and the DEX_VAULT v2 beneficiary
binding (USER_DEPOSIT vs VAULT_POOL_CHANGE) are recorded in
[b3-flowmesh-dex-decisions.md](b3-flowmesh-dex-decisions.md) §3b. The text
below is the pre-ruling statement of the question.

## OD-6 (original statement)

Contract §39 requires microblocks/epochs to be anchored to B3 rather than forming an
independent history, but the binding is unspecified: the relationship between a FlowMesh
`slot`/epoch and block height, the anchoring cadence, the finality rule a deposit must
satisfy (contract §30), and what happens to a finalized receipt if its anchoring block is
reorged. Today `flowmesh::Ledger::m_slot` is a bare counter with no defined relationship
to the chain.

---

## OD-7 — Per-market FlowMesh precision

Contract §36 requires each market to define consensus precision (price ticks, quantity
lots, fee units, funding units, margin precision). No values or governing mechanism are
specified.

---

## OD-8 — Bridge design (current supersession, then labeled history)

Historically, contract §21 required an origin domain while bridge verification
and issuer-freeze handling were still unspecified. Current bridge activation is
a separate fail-closed gate after H+1; FlowMesh A3 alone never enables minting.

**CURRENT OWNER SUPERSESSION (2026-09-01):** the first production dollar asset
is bridge-backed bUSD, minted 1:1 from proven canonical USDT deposits on
Ethereum mainnet. The fixed identity tuple starts with chain id 1, vault
`0x143F207e23e6aebD7E974be90ac6D434f4c7BFb6`, token
`0xdAC17F958D2ee523a2206206994597C13D831ec7`, and exact 6-to-6 decimal
conversion. FlowMesh ships in the transition release at A3, but bridge minting
has its own fail-closed activation and does not turn on merely because A3 is
reached. Remaining release pins are the reviewed Ethereum bootstrap/fork
schedule, adapter commitment/version, approval interval, block/epoch caps,
sync-lag rule, durable light-client/nullifier state and proof carrier. The
vault's immutable owner-controlled withdrawal authority was explicitly
accepted for transition v1 on 2026-09-01. That release leg is managed, not
decentralized; activation must pin the independently observed authority,
runtime code hash, and versioned withdrawal rules. Because the authority is
immutable, a later verifier requires a new approved vault and explicit reserve
migration. Dated statements below are design history wherever they conflict
with this paragraph.

**Verified deployment facts (Ethereum block 25,877,643):** independent
PublicNode and dRPC reads agree that `releaseAuthority()` and
`rescueAuthority()` are both
`0x76c7a245d0D2e4CF92403aF0144825df1cC614f1`, an EOA with no bytecode. The
3,135-byte vault runtime has Keccak-256 hash
`0x1be220c18efa4e4cda0bb1c912c7c41346f5c04d49a36ec2c68f6ddcc5586233`;
`owner()` reverts and no proxy is present. The vault is generic, not a USDT
allowlist, and held zero USDT at that block, so exact canonical-token admission
remains a B3 consensus responsibility.

**Historical 2026-08-22 direction (superseded where the current paragraph
above conflicts):**

- The 2026-08-22 native-CDP bUSD direction is superseded; bUSD is the exact
  bridge-backed USDT asset described above. FlowMesh fees remain native B3.
- Any protocol-level bridge is **light-client / SPV on the mint leg**, never a signer set inside B3 consensus: Ethereum via the sync-committee light client (finalized headers only; the owner ruled the light client as "the solution"), Bitcoin via SPV proofs of the most-work chain. For transition v1 only, the deployed Ethereum vault's immutable owner authority performs managed releases. It is not rotatable and cannot become a verifier in place; a later decentralized release leg needs a newly approved vault/migration.
- Tron and other non-light-client chains are served **off-consensus** (Chainflip-class swaps into native B3, managed on-ramps); no Tron bridge enters consensus.
- End state: an issuer taking mint authority in place (native issuance) — a business outcome, not a protocol dependency.
- **Ruled for canonical bUSD:** Ethereum L1 (chain id 1), not an L2, is the stable origin. Any future L2 asset is a distinct registry identity and proof design.
- Historical open list: exact light-client verification rules and the
  `blst`/keccak/RLP/MPT dependency decision, sync-committee participation
  threshold, mint caps, watcher veto, fork-upgrade procedure, re-bootstrap
  rule, issuer-freeze handling. The verification stack and dependencies are
  now implemented; only the current paragraph's reviewed mainnet pins and
  operating policies remain release gates.
- **2026-08-23 owner direction: "BLS is the key to bridge" — future end state, superseded for the transition-v1 release leg.** BLS12-381 remains the Ethereum sync-committee primitive for proof-verified deposits and the intended later decentralized withdrawal verifier. Transition v1 instead uses the existing immutable managed authority as ruled on 2026-09-01; it must not be marketed as the no-trusted-signer end state.
- **2026-08-23 owner brief: the critical problem is B3 → Ethereum (withdrawals), not deposits.** Design record: [b3-finality-to-ethereum.md](b3-finality-to-ethereum.md) — a V2 finality gadget layered on Modern PoS V1 (BLS key binding, one-epoch-lookahead validator-set snapshot committed by keccak header + Merkle members, `FinalizedBlock{height, block_hash, withdrawal_root, validator_set_hash(successor), epoch}` + `Certificate{signer_bitmap, aggregate_sig}` verified on B3 in-block with an absolute finality pin, `B3FinalityVerifier.sol` with set handover + non-signer-subtraction BLS verification via EIP-2537, cumulative withdrawal tree, vault release only on finality ∧ inclusion, `IB3FinalityProver` seam for a future ZK prover with frozen data structures). **Superseded the same day by the owner's correction:** the finality gadget and BLS validator keys are **V1-reserved from M** (PoS spec ruling M7), enforcement at F, bridge at A3 ≥ F; the verifier follows genesis set → epoch certificates → rotation → withdrawal roots. Revision 2 of the same document is the specification (exact gadget, key lifecycle, quorum, epoch transitions, permanent Ethereum state, attack table). Still design only ("do not implement yet"); owner decisions in its §9.
- **2026-08-23 direction ACCEPTED by owner; protocol FINALIZED:** [b3-cross-chain-finality-v1.md](b3-cross-chain-finality-v1.md) (normative) — BLS finality is part of Modern PoS V1; BIP340 identity key and BLS consensus key strictly separate; Ethereum verifier built on the fixed-depth (13) keccak `members_root` + 126-byte set header, never a member list; exact verification of epoch transition (strict e→e+1 after successor disclosure), weights (absentee leaves + multiproof), bitmap (LSB-first, complement-of-absentees check), quorum (`floor(2W/3)+1`, rule enforced on-chain for ruleset 1), withdrawal root (depth-32 cumulative keccak tree, single tree with `origin_chain_id` in the leaf). Remaining: parameters in its §9 (F, E, intervals, set bounds, lag) and the implementation go-ahead.
- **2026-08-23 owner: ZK deferred** — BLS certificate prover only for v1; `IB3FinalityProver` seam retained, no ZK work scheduled.
- **2026-08-23 historical compatibility audit, superseded carrier recommendation:** [b3-finality-compatibility-report.md](b3-finality-compatibility-report.md). Its proposed `B3F1` coinbase `OP_RETURN`/`B3B1` carriers were not adopted; the MPA/metadata-cell ruling below replaced them. Its handover-gated rotation finding remains historical input to the later finality design.
- **2026-08-23 owner LOCK (80-byte model):** `policy_params ≤ 80 B` is a permanent invariant for small typed live/derived state; large evidence (BLS certificates, bridge/Merkle/ZK proofs) is bounded, priced payload data committed to by the policy object — never policy state, never an arbitrary-data policy, never solved by raising `MAX_POLICY_PARAMS_SIZE`. `FINALITY_CERT` = small typed metadata (commitment = hash of the full payload, strict type max, ≤ 1 where required); `FINALITY_KEY` = small binding state (BIP340 identity authorizes the BLS key + separate PoP, sequence-controlled rotation/revocation, effective at next snapshot). Native carrier: [b3-modern-payload-area.md](b3-modern-payload-area.md) rev. 2 — MPA **accepted** (BIP144 flag 0x02 + the existing strict creation-action section), commitment **Path B ruled**: coinbase-only zero-value `MODERN_PAYLOAD_ROOT` metadata cell (policy 8, never in UTXO) committing `payload_root = ComputeMerkleRoot(TaggedHash("B3/MPA/LEAF/V1", i ‖ section_hash_i))`; non-circularity proven (leaves carry position + section hash, never a txid); `ptxid` = full-serialization hash for relay (no SegWit dependency; SegWit activation stays a separate audit question); resource numbers NOT frozen — worst case shows a per-block verification-cost budget is required (PoP spam ≈ 18 s/block at 4 MB) and the MPA weight factor must be chosen first.
- **2026-08-23 rulings applied to the normative documents:** MPA weight ×4; consensus payload verification-cost budget (per-tx and per-block, checked before cryptography, deterministic per `(type, version)`); relay vsize includes verification cost; `ptxid` defined normatively over the canonical full serialization; policy numbers 6/7/8 frozen (contract §23 updated); byte ceilings NOT frozen — framework frozen, numbers after benchmark. Finality spec rev. 2 now carries the cells + MPA records, identity-authorized `FINALITY_KEY`, handover-gated rotation, F = M in the X-pin release. Remaining owner decisions before Modern PoS implementation: see the session report of 2026-08-23 (finality spec §9, MPA §9, PoS spec §9 provisional rows, SegWit audit).
- **2026-08-23 Tier-1 rulings + benchmark:** gated rotation FINAL; epoch window `{current, current−1}` FINAL under monotone-height / set-hash / epoch-relation conditions; BLS binding mandatory for block eligibility from F = M (one stake universe); `FINALITY_KEY` semantics FINAL. Benchmark-only work authorized and done: pinned `blst` v0.3.17 vendored (`src/blst`, build-off-by-default), harness `b3-finality-bench`; results + recommended constants in [b3-finality-benchmark-2026-08-23.md](b3-finality-benchmark-2026-08-23.md) (PoP verify ≈ 0.6 ms; certificate ≈ 1.1 ms @3,500 / 1.9 ms @8,192; recommend I/D = 10/12, E = 1,440, cost budget 120,000 / 12,000 units, 1 vbyte/unit, ceilings 32,768 / 65,536). Consensus implementation still awaits a separate go-ahead.
- **2026-08-23 CONSTANTS FROZEN (owner):** E = 1,440; CHECKPOINT_INTERVAL = 10; CHECKPOINT_DEPTH = 12; MAX_EPOCH_EXTENSION = 7·E; MIN_FINALITY_SET = 4 (chain bootstrap floor only — bridge security thresholds are an A3 decision); verify_cost FINALITY_KEY_EVIDENCE 700 / FINALITY_CERTIFICATE 2,000; MAX_BLOCK_PAYLOAD_COST 120,000; MAX_TX_PAYLOAD_COST 12,000; COST_TO_VBYTES 1; MPA record 32,768 B / section 65,536 B / weight ×4. All earlier Modern PoS / finality / MPA rulings remain in force. Implementation plan: [b3-modern-pos-v1-implementation-plan.md](b3-modern-pos-v1-implementation-plan.md) — **implementation awaits explicit plan approval.**
- **2026-08-24 owner ruling — bridge sequencing: deposit legs first.** "We should have a
  real working bridge first from ETH, then from BTC." The mint (inbound) legs lead the
  bridge program: **Ethereum → B3 first, Bitcoin → B3 second**; the release leg
  (B3 → Ethereum withdrawals, [b3-cross-chain-finality-v1.md](b3-cross-chain-finality-v1.md)
  §5–§6) remains normative and follows. Consequences reported, not silently resolved:
  (a) this supersedes the 2026-08-23 chain-of-chains *sequencing* note ("skip the ETH
  light client in B3 if the hub is coming") — a real working ETH→B3 bridge requires the
  inbound verifier now, and per the standing "BLS is the key" ruling that verifier is the
  **sync-committee light client in B3** (finalized headers only, `blst`-verified); a
  future hub verifier may replace it as a later in-place transition. (b) The BTC leg is
  inbound-only at this stage: B3 verifies Bitcoin SPV (most-work headers + tx inclusion
  at depth); the BTC *custody* model for a two-way peg (threshold-Schnorr committee vs
  federation) is expressly NOT designed yet and needs its own ruling. (c) Bridge
  proposal stages 1–3 (test-only / header-only: Keccak-256, RLP/MPT, SSZ, pure
  light-client functions) are in execution per the committed staged order; consensus
  wiring (stage 4+, `BRIDGE_MINT`, nullifiers, chainstate) is authorized for
  the transition branch but remains fail-closed until the current production
  pins above are supplied and tested.

---

## OD-9 — Encrypted order flow

Contract §41 defers threshold encryption pending an MEV/front-running justification. Open
whether it is ever adopted; it must not become a dependency of H+1.

---

## OD-10 — Operational H/X coordination

Contract §62 states the preferred model (choose an already-buried block, record X, ship the
release) and requires H/X be known before the enforcing binary activates. The concrete
activation mechanism — how nodes agree to begin modern validation once H/X are baked in —
is not finalized and must not be improvised late.

**RE-RULED 2026-08-26 (owner, after explicit timeline review): H = 810,000**
(corridor 810,001..811,000, M = 811,001), superseding H = 820,000. Basis:
legacy stake spacing is 360 s (src/legacy/consensus.h STAKE_TARGET_SPACING),
~240 blocks/day; tip ~807,95x on 2026-08-26, so the chain reaches 810,000
around 2026-09-03/04 — an ~8.5-day runway the owner judged sufficient for
their operator coordination. Path: distribute the fail-closed v1 binary
(H set, X blank) before the height is reached (pause model); if
distribution slips past the height, block 810,000 is then a buried
observable fact and the §62 single-release H+X model applies instead.
Post-H legacy-client activity is abandoned by the modern ledger — the
cutoff must be announced to all stakers/operators immediately.

**RULED 2026-08-23 (owner, superseded above):** H = 820,000 (corridor 820,001..821,000,
M = 821,001); X distribution = PAUSE, FAIL CLOSED — a release ships with H
set and X blank, accepts through H and refuses every block at H+1
(`legacy-boundary-unpinned`, a no-penalty header refusal, plus a
production guard), and a mandatory follow-up release pins X and resumes
the corridor; nodes with blank X never enter the corridor. Corridor bits =
canonical `0x1f008000`. Seeds: `176.31.13.198` plus at least two further
independently hosted fixed seeds and an owner-controlled DNS seed before
final release. **None of these values is pinned in mainnet chainparams
until the four pin gates in [b3-release-v1.md](b3-release-v1.md) pass.**

---

## Ratified deviations (closed — recorded so they are not "fixed" later)

- **Policy enum numbering.** `LEGACY_LOCK = 0`, `OWNER = 1`, `BURN = 2`, `DEX_VAULT = 3`
  are consensus-stable as serialized. `BURN` occupying slot 2 ahead of `STAKE`/`BRIDGE` is
  **accepted as-is**; existing numbers must never be renumbered (contract §23). New policy
  types take new numbers.
- **`policy_params` field.** `ModernOutput` carries a sixth field (`policy_params`, ≤ 80
  bytes) beyond the five-field conceptual model, used by `DEX_VAULT` v1 for a shard id. It
  is already in the frozen wire vector and is retained.
- **No tagged modern block hash.** Modern block hashing stays the existing modern domain;
  domain tags apply only to newly introduced hashing, not block or existing transaction
  identity (contract §7).
- **No transaction-family byte.** Block-context codec selection plus trusted per-connection
  context for standalone transactions is the locked mechanism (contract §8).
- **Sync policy.** "Outbound-only sync ownership" is **not** locked. The requirement is
  bounded, progress-safe sync (contract §59); any design meeting it is acceptable.
