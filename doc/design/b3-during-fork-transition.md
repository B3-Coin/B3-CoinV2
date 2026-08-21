# B3 transition corridor — temporary PoW between legacy PoS and modern PoS

**Status: AUTHORITATIVE DESIGN DIRECTION (2026-08-16).** This document
supersedes BOTH earlier transition models: the post-boundary
"self-activating bootstrap" and the **1,000-block legacy-PoS declaration
window (SUPERSEDED)** — the corridor is no longer legacy blocks carrying
declarations; it is temporary PoW with modern semantics. Specification and
inspection only: no consensus code is implemented by this mission, and the
`no-modern-pos-rules` fail-closed gate is untouched. OPEN items must not be
closed by implementation choice.

## 1. Timeline

    Genesis … 500            historical legacy B3 PoW   (LAST_POW_BLOCK = 500)
    501 … H                  legacy B3 PoS
    P0 = H+1 … PF = H+1000   TEMPORARY B3 PoW corridor  (1,000 blocks, locked count)
                             modern block/tx/output semantics, Policy Outputs
    M = H+1001 onward        modern B3 PoS

    H = FINAL_LEGACY_POS_HEIGHT      X = hash(H)     (unchanged legacy anchor)

B3 already ran PoW for its first 500 blocks; the corridor's default
direction is to **reuse that historically proven PoW mechanism** — not
RandomX, not a novel algorithm, not a new hash function — inside a new
bounded phase with modern semantics. The 1,000-block corridor length is a
locked design direction; numeric subparameters inside the corridor remain
OPEN unless stated otherwise.

## 2. Why PoW returns temporarily: legacy coinstake churn

Under legacy B3 PoS, staking consumes the staking outpoint: the coinstake
spends it and creates a new output, which must rebury before staking again.
Measured from `master`: `nCoinbaseMaturity = 30` (coinstake outputs unusable
for ~30 confirmations; the "~20 confirmations" churn description is this
same friction) plus `nStakeMinAge = 3600 s` before an output can stake.
A declaration bound to a specific legacy outpoint therefore forces a staker
to choose between continuing to stake (churning the outpoint, voiding the
declaration) and freezing the outpoint (going offline as a staker). The
corridor removes the dilemma:

    legacy PoS stops at H
        ↓ temporary PoW produces blocks (stake is not consumed by anyone)
        ↓ holders spend old UTXOs ONCE into real modern STAKE Policy Outputs
        ↓ STAKE outputs sit unchanged and mature
        ↓ modern PoS begins at M with a ready validator registry

The corridor is not a permanent mining economy; it exists to establish
modern ownership + modern stake + the initial validator registry. Nothing
more.

## 3. Three independent dimensions — do not collapse into one boolean

    dimension                LEGACY_POS        TRANSITION_POW      MODERN_POS
    ─────────────────────    ──────────────    ────────────────    ────────────
    block format             legacy codec      MODERN codec        modern codec
    tx/output format         legacy CTxOut     MODERN + Policy     modern + Policy
    block production         legacy PoS        existing B3 PoW     modern PoS

The corridor does **not** revert to the 2016 legacy format: transition
blocks are modern-family blocks with modern transactions, Policy Outputs and
LEGACY_LOCK spending support, produced under the existing B3 PoW mechanism.
Recommended future abstraction (NOT implemented in this mission):

    enum class ConsensusPhase { LEGACY_POS, TRANSITION_POW, MODERN_POS };
    height <= H              → LEGACY_POS
    H+1 <= height <= H+1000  → TRANSITION_POW
    height >= H+1001         → MODERN_POS

Existing two-state assumptions that will eventually need refactoring for
three phases are catalogued in §11.

## 4. Historical B3 PoW — traced from `master` (not from memory)

| # | Item | Finding (file:line in `master`) |
|---|---|---|
| 1 | Hash algorithm | scrypt, N=1024 r=1 p=1, 80-byte header as both input and salt (`scrypt.cpp:136,193`; `SCRYPT_BUFFER_SIZE = 131072+63`) |
| 2 | Where computed | `CBlock::GetPoWHash() = scrypt_blockhash(&nVersion)` (`main.h:665`); **`GetHash()` is version-dependent: `nVersion > 6` → SHA256d, else the scrypt hash** (`main.h:658`) |
| 3 | `CheckProofOfWork` | `main.cpp:1329` — `CBigNum::SetCompact(nBits)`; reject if target ≤ 0 or > `ProofOfWorkLimit()`; reject if `hash > target`. Called from `CheckBlock` only for `IsProofOfWork()` blocks (`main.cpp:2302`) and on disk reads (`main.h:808`) |
| 4 | Target interpretation | Compact-bits big-number encoding via `CBigNum::SetCompact/GetCompact` (OpenSSL BN) — same encoding the port already handles |
| 5 | Difficulty adjustment | `GetNextTargetRequired(pindexLast, fProofOfStake)` (`main.cpp:1286`): PPC-style per-block exponential move toward target spacing over last two same-type blocks; `bnNew *= ((n−1)·S + 2·A) / ((n+1)·S)` with `n = nTargetTimespan/nTargetSpacing`; clamps: negative spacing → 1, spacing capped by `nTargetTimespan` and `10 × nTargetSpacing`; result capped at the limit. PoW spacing in the hybrid era: `min(nTargetSpacingWorkMax, 360·(1+height−lastPoWheight))` — it *stretches* as PoW blocks thin out. Constants: `nStakeTargetSpacing = 360 s`, `nTargetTimespan = 7200 s`, `nTargetSpacingWorkMax = 1080 s` (`main.h:56-58`). Enforced exactly at accept: `nBits != GetNextTargetRequired(...) → reject` (`main.cpp:2496`) |
| 6 | Initial difficulty | `bnProofOfWorkLimit = ~uint256(0) >> 20` mainnet; genesis `nBits = limit` (`chainparams.cpp:58,73`); retarget returns the limit for the first two same-type blocks |
| 7 | Timestamp constraints | `FutureDrift = +600 s`, `FutureDriftPOW = +100000 s` (`main.h:66-67`); PoW blocks: `GetBlockTime() > FutureDriftPOW(coinbase.nTime)` rejected while `height ≤ LastPOWBlock` (`main.cpp:2481`); block vs prev: `> GetPastTimeLimit()` and drift bound (`main.cpp:2500`); network future bound `FutureDrift(GetAdjustedTime())` (`main.cpp:2306`) |
| 8 | PoW reward | `GetProofOfWorkReward` (`main.cpp:1146`): 10 COIN + fees; height 1 = 260,000 COIN; **fees-only above `LastPOWBlock()`** — the schedule assumes the first-500 context and must NOT be reused as-is |
| 9 | Miner | `miner.cpp`: `CreateNewBlock(reservekey, fProofOfStake, &fees)` (line 105) sets `nNonce = 0` (line 459); `IncrementExtraNonce` (line 466); PoW scan = in-process loop hashing with `GetHash()`/`GetPoWHash()` (lines ~490-560); FN-payment logic embedded — not reusable |
| 10 | PoW→PoS activation | `nLastPOWBlock = 500` mainnet, 100 testnet (`chainparams.cpp:118,174`); enforced: `IsProofOfWork() && nHeight > LastPOWBlock() → reject` (`main.cpp:2475`) |
| 11 | Shared header fields | Yes — one `CBlockHeader` for both; PoW/PoS distinguished by transaction shape (`IsProofOfStake()` = coinstake at vtx[1]), not header bits |
| 12 | `nNonce` | PoW grinding counter; PoS blocks carry `nNonce = 0` (miner sets 0; PoS validity never inspects nonce) |
| 13 | Chain trust | `CBlockIndex::GetBlockTrust() = 2^256/(target+1)` from `nBits`, identical formula for PoW and PoS blocks (`main.cpp:2561`); accumulated in `nChainTrust` (`main.cpp:2242`). The port's `GetBlockProof` already reproduces this |
| 14 | Scrypt implementation | `scrypt.cpp` generic C + `scrypt-x86.S`, `scrypt-x86_64.S`, `scrypt-arm.S` (32-bit ARM) assembly |
| 15 | Platform assumptions | x86/x86_64/ARMv7 asm; no arm64 asm. The port (`src/legacy/scrypt.cpp`, 143 lines) is generic C only — portable everywhere, already used by all legacy tests |
| 16 | Test coverage | `master`: none. Port: real scrypt PoW grinding against the ported `legacy::GetNextTargetRequired` in `legacy_transition_tests`, `legacy_checkpoint_tests`, reward/fork-choice suites (5 test files reference the retarget) — historical PoW validation is already exercised end-to-end in the port's legacy era |

### Reuse verdicts

**Reusable unchanged:** the scrypt primitive (port's generic implementation);
compact-bits target encoding and the `hash ≤ target` check shape; the
trust/chainwork formula (already era-uniform in the port); the shared-header
approach (no header change; corridor blocks are modern-format blocks whose
work check hashes the header with scrypt while identity stays the modern
SHA256d marker domain — the two-hash separation already exists in the port).

**Reusable with transition-specific parameters (OPEN):** the PPC-style
per-block exponential retarget as the *candidate* difficulty policy —
corridor spacing, timespan, starting target at P0, min/max target and clamp
behavior must be corridor-specific (§8); timestamp bounds (the 600 s modern
drift, not the 100000 s early-PoW drift).

**Must NOT be reused:** the historical reward schedule (10 COIN + the
260,000-COIN height-1 block and the fees-only-after-500 branch); the
`LastPOWBlock = 500` early-chain activation switch and its hybrid
PoW-spacing stretch (`nTargetSpacingWorkMax` logic assumes PoW/PoS
interleaving — the corridor is PoW-only); `FutureDriftPOW = +100000 s`;
the miner's FN-payment and wallet coupling; the version-dependent
`GetHash()` quirk (identity in the corridor is the modern marker domain,
never scrypt).

## 5. Corridor semantics

**Block P0 = H+1** is: the first block after the immutable legacy anchor X;
the first modern-format block; the first modern-transaction block; the first
Policy Output block; the first temporary-PoW block. It is NOT the first
modern-PoS block. The existing integration expectation "H+1 →
`no-modern-pos-rules`" is recorded as a contradiction (§11) and will
eventually move to the first attempted M = H+1001; tests are not modified in
this mission.

**Policy surface during the corridor (minimal):** OWNER, LEGACY_LOCK, STAKE,
BURN (already part of the model, where needed), basic native B3 transfers.
NOT activated: FlowMesh, DEX, bridge, FN, real USDT/USDC, advanced
colored-asset issuance, microblocks, leverage, general smart contracts.

**Legacy UTXO spending:** old outputs keep original txid/vout/nValue/
scriptPubKey; a modern corridor transaction spends them through
ViewLegacyCoin/LEGACY_LOCK under frozen legacy script rules into modern
OWNER or STAKE outputs. No rewrite, no migration, no snapshot, no new
genesis; non-stakers may leave UTXOs untouched forever.

**STAKE outputs are real modern outputs** committing to: native B3 principal
amount, owner authorization, validator public key, creation height, policy
version, and lifecycle state where needed. Owner authority (exit/withdrawal/
ownership changes) and validator authority (PoS operations) stay separate;
the validator machine never needs the owner's cold key. One STAKE policy
type; any number of STAKE outputs.

**Validator aggregation — LOCKED:** consensus weight is
`validator_weight = SUM(qualifying ACTIVE STAKE principal assigned to the
validator key)`. Splitting 100,000 B3 across 10,000 outputs must confer
exactly the proposer opportunity of one 100,000 B3 output: eligibility is
evaluated per validator identity with aggregated weight, never one lottery
ticket per UTXO. (This supersedes the earlier per-output eligibility
wording in the PoS spec, which is corrected to per-validator aggregation.)

**Maturity — `STAKE_ACTIVATION_DEPTH = 20` (precise consensus rule):**
a STAKE output created in a block at height `b` is MATURE at height `h`
iff `h − b ≥ STAKE_ACTIVATION_DEPTH`, i.e. from height `b+20` onward.
Conventional confirmation-count terminology is deliberately NOT used —
"20 confirmations" creates an off-by-one ambiguity; the depth comparison
above is the rule. It matches the existing maturity-comparison shape
(`spend_height − create_height ≥ depth`) used by both codebases. The
constant is a new modern one, independent of the legacy
`nCoinbaseMaturity = 30`. During the corridor, mature STAKE
contributes to the future registry only — it produces no corridor blocks;
temporary PoW is the corridor's only block-production mechanism.

**Initial validator cutoff C (RULED 2026-08-21): there is NO separate
cutoff.** The 20-block STAKE activation depth alone determines the initial
ACTIVE set: a STAKE output is ACTIVE at M iff it was created at least
STAKE_ACTIVATION_DEPTH blocks earlier (created at-or-before M−20), exactly
the maturity rule the implementation already applies uniformly at every
height. Later stake stays valid, enters PENDING, and activates under the
same rule. The earlier registration/stabilization split is superseded; no
second number exists.

**Registry derivation:** at the end of block PF = H+1000, every node derives
the identical initial modern validator registry from qualifying STAKE
outputs in the modern UTXO state — no legacy reinterpretation, no snapshot,
no administrator list, no operator committee, no self-authorizing first PoS
block. Then M: registry → modern VRF/eligibility → eligible proposer(s) →
first modern-PoS block.

## 6. Corridor difficulty (MECHANISM RULED 2026-08-21; value OPEN)

**Owner ruling (2026-08-21): NO retarget — one fixed constant difficulty
for the whole corridor.** The corridor is a transition, not a mining era,
and must not be complicated to optimize it. This is exactly what the
implementation enforces (`transition_pow_bits`, compared for equality on
every corridor header, fail-closed while unset), so the ruling locks the
shipped mechanism. A retargeting alternative was considered and rejected
by the owner.

Consequences, accepted with the ruling: corridor wall-clock duration is
set by real hashrate against the constant target (high hashrate
compresses it, low hashrate stretches it); there is no adjustment. The
earlier retarget analysis above is superseded. **Still OPEN: the constant
value itself** — one compact-bits number, chosen at mainnet H/X pinning
time (inert before then), balancing "cheap enough that the corridor
cannot stall" against "expensive enough that its duration gives holders a
real staking window".

## 7. Corridor security (concrete questions; mitigations OPEN)

Unequal hashpower is accepted ("more computing power → more expected
blocks"); scrypt is memory-touched but has mature ASIC/rental markets —
it is not CPU-only and is not claimed to be. The concrete questions:
Can one miner censor STAKE creation? Can corridor blocks be cheaply
reorganized (rented hashrate vs. the corridor's history that defines the
registry)? Can a miner shape the initial validator set (censorship +
reorg near C)? Can the chain stall if hashrate disappears? Can difficulty
be manipulated (timestamp gaming the exponential filter)? Can timestamp
manipulation affect the handoff at PF? Can the final corridor blocks be
withheld to grief the M transition? Candidate mitigations (none locked):
responsive difficulty; cutoff C well before H+1000 with a large burial
period; bounded corridor reorg depth; corridor timestamp rules; broad
mining-client availability; a temporary block reward attracting honest
hashrate.

## 8. Corridor mining reward (RATIFIED 2026-08-21: 0 — fees only)

**Owner ruling: the corridor pays transaction fees only; no subsidy.** The
historical schedule does not return, no new issuance occurs during the
corridor, and the miner→validator-capture question dissolves — with zero
subsidy there is no reward B3 to convert into initial-set weight; corridor
miners bootstrap their validator position with coins they already hold,
like every other holder. The parameter is stated explicitly on mainnet and
FAILS CLOSED when unset on any network (never an accidental default).

## 9. Insufficient stake at the end of the corridor (RULED 2026-08-21)

**Owner ruling: NO minimum-total-stake consensus gate — options A + C.**
M begins with whatever valid mature stake exists; the V1 recovery rounds
make any nonzero active stake sufficient for liveness (even ~1% online
produces blocks within minutes). The only true failure mode is exactly
zero stake at M — a chain that can never produce a modern block — and
that is prevented OPERATIONALLY: H is not finalized until adequate
participation is evident (option A), never by consensus machinery. The
corridor-extension mechanism (B) is rejected as complexity; abort (D)
remains possible only before H is final, as before.
**FIRM: after H, legacy PoS never resumes.** There is no silent fallback
from transition PoW to legacy PoS under any circumstances.

## 10. The legacy anchor and the activation gate are unchanged

Genesis…H remains the legacy prefix: TrustedReplay ends at H; X = hash(H)
is the immutable anchor; corridor blocks H+1…H+1000 are ordinary
modern-format blocks under normal (PoW) validation, and M onward is normal
modern-PoS validation. The mandatory mainnet activation gate
`U_master(H) == U_port(H) == U_replay(H)` on real history stands; regtest
does not replace it. All completed work retains its value: D1–D4
hardening, legacy checkpoints and live CheckSync, mempool boundary work,
historical reward-rule reconstruction, TrustedReplay, the three-way
equivalence framework, legacy UTXO identity, LEGACY_LOCK, Policy Output
models, marker/codec work, boundary finality, and the modern PosValidator
interface. What changes is only the phase immediately after H.

## 11. Contradiction register — current code/tests encoding "H+1 = modern PoS"

Recorded only; nothing modified in this mission.

1. `src/test/legacy_transition_tests.cpp` —
   `non_empty_transition_fails_closed_at_h_plus_one` expects H+1 →
   `no-modern-pos-rules`; under the corridor, H+1 should eventually
   validate under TRANSITION_POW and the fail-closed expectation moves to
   the first attempted H+1001.
2. Same file — `full_legacy_to_modern_transition` installs the test PoS
   validator for H+1 and treats H-connect as "next block is modern PoS";
   under the corridor the installed validator would first bind at M.
3. `src/modern/pos.h` — `SelectStakeRules` is two-state (LEGACY/MODERN by
   codec marker × era); a third TRANSITION_POW phase must dispatch
   modern-codec corridor blocks to PoW validation, not `CheckModernStake`.
4. `src/consensus/era.h` — `GetB3Era` is boolean (LEGACY/MODERN); correct
   for format/tx dimensions but not for block-production phase; the future
   `ConsensusPhase` abstraction (§3) supersedes it as the production
   selector; `hard_fork_height` remains the format boundary = H+1.
5. `src/validation.cpp` modern branch of `CheckBlock`/
   `ContextualCheckBlockHeader` — checks stock SHA256d
   `CheckProofOfWork(block.GetHash(), nBits)` and Bitcoin
   `GetNextWorkRequired`; corridor blocks need scrypt-hash work checks and
   the corridor difficulty policy while identity stays SHA256d.
6. `src/modern/policy.h` — no STAKE policy type exists yet (types 0–3),
   and OWNER/BURN/DEX_VAULT activation is test-only
   (`test_only_asset_policies_active`); the corridor requires a production
   activation story for the minimal surface OWNER/LEGACY_LOCK/STAKE(/BURN)
   from H+1.
7. Mempool era gate (`MemPoolAccept::PreChecks`) — era-of-next-block logic
   is correct for the corridor (modern txs from H+1) but "next block
   modern" currently implies modern-PoS context in tests; corridor-phase
   awareness will be needed for miner/relay policy, not admission.
8. `Consensus::Params` — no corridor constants exist
   (`TRANSITION_LENGTH`, cutoff C, corridor difficulty/reward params);
   `legacy_last_pow_block = 500` exists and must remain untouched by the
   corridor (it governs the historical era only).
9. The PoS spec's per-output eligibility wording (threshold "split-
   invariant" per STAKE output) conflicts with the now-LOCKED
   per-validator aggregation rule; corrected in the PoS spec by this
   mission (documentation only).
10. No mining/block-production path exists for any era (miner/submitblock
    are stock Bitcoin); corridor mining needs the marker-aware production
    path already listed as missing in the status matrix.

## 12. Decision status

**LOCKED / DESIGN DIRECTION:** corridor model `legacy PoS → temporary
existing-B3 PoW → modern PoS`; corridor length 1,000 blocks; corridor
blocks are modern-format/modern-tx/Policy Output blocks (never legacy
codec); reuse of B3's existing scrypt PoW primitive as the default
direction; H/X as the immutable legacy anchor with TrustedReplay ending at
H; the three-way real-history gate; minimal corridor policy surface;
LEGACY_LOCK crossing; owner/validator key separation; per-validator
weight aggregation (splitting confers no advantage);
`STAKE_ACTIVATION_DEPTH = 20` (mature iff `h − b ≥ 20`, never
"confirmations"); mature-stake-produces-no-corridor-
blocks (PoW is the only corridor production); deterministic registry
derivation at H+1000 with no snapshot/committee/administrator/self-
authorizing block; after H, legacy PoS never resumes; unequal hashpower
accepted; no new hash function.

**RULED 2026-08-21 (no longer open):** corridor difficulty MECHANISM
(fixed constant, no retarget — §6); corridor reward (0, fees only,
fail-closed — §8, capture question dissolved); cutoff C (none — the
20-block activation depth alone, §5); insufficient-stake handling
(options A + C, no consensus gate — §9); the STAKE v1 carrier
(RATIFIED as tested); the modern-PoS mechanism set and its timing
numbers (frozen V1 spec: 60 s interval, 30 s rounds, f0 = 1, ×2
relaxation — the VRF/slots/epochs items are superseded; the seed at M
derives from the corridor-exit block).

**STILL OPEN:** the corridor difficulty VALUE (one compact-bits number at
H/X pinning); minimum stake amount; the modern reorganization horizon D;
the modern-PoS sentinel-bits and future-drift values (provisional);
corridor reorg-depth bounds and other §7 mitigations; X-distribution
operations (pause vs. precommit); modern reward schedule (OD-2).

## 13. Destination unchanged

After modern PoS: asset registry → bridge → TEST_USDT → FlowMesh → spot DEX
→ USDT/USDC fees → futures (only "supported, max leverage 10×" is locked;
margin mode, funding, liquidation and every other mechanic are OPEN owner
decisions) → microblocks → real bridges → FN system. (This roadmap line
originally read "isolated leverage ≤10× → PnL/liquidation → deterministic
epochs"; corrected 2026-08-20 per the Codex directive — no futures
mechanics beyond the 10× cap are approved, and microblocks, not epochs,
are the execution unit.) The corridor must not couple to or redesign any
of these.
