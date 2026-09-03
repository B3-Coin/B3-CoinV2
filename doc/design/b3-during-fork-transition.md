# B3 transition corridor — temporary PoW between legacy PoS and modern PoS

**Status: AUTHORITATIVE CORRIDOR DESIGN (updated 2026-09-01).** This document
supersedes BOTH earlier transition models: the post-boundary
"self-activating bootstrap" and the **1,000-block legacy-PoS declaration
window (SUPERSEDED)** — the corridor is no longer legacy blocks carrying
declarations; it is temporary PoW with modern semantics. The corridor and
modern consensus paths are implemented. At the pre-pin checkpoint, mainnet
remained fail-closed until the seal-derived X/R0/FN-manifest pins and the ruled
A1/A2/A3 activation heights were set and the transition-release gates in the
runbook were complete.

> **Current pin supersession (2026-09-01).** Those consensus pins are now set:
> H/X = 810,000/
> `2413ba59476afb9a01b971c350b2c5a51494b37925055be42dde774f30d865c6`,
> M = 811,001, R0 = 19,836,712,254 base units, the 3,592-row FN artifact has
> SHA-256
> `c80470eec785600f33fa2e69c520ff331c2b354ebf6e0a9bf8cae7d1eb5f9dca`,
> and A1/A2/A3 = 812,000/813,000/815,000. The preceding sentence records the
> pre-pin gate; it no longer describes the current transition binary. Release
> rehearsal/verification remain required, and bUSD remains independently
> fail-closed.

Later sections retain historical investigation where useful; `FINALIZED.md`
and the FN/assets and FlowMesh production designs govern any conflict.

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
modern-PoS block. Modern-family rules begin at H+1; modern PoS production
begins only at M = H+1001.

**Policy surface during the corridor (minimal):** OWNER, LEGACY_LOCK, STAKE,
BURN (where needed), basic native B3 transfers, and the mandatory FN Genesis
outputs in the H+1 coinbase. FN units become ordinarily transferable after
their 30-block coinbase maturity. Permissionless modern FN PoD creation waits
for the separately pinned post-M height A1; simple-v1 colored-asset issuance
waits for A2. During the corridor itself, FlowMesh, DEX, bridge, real
USDT/USDC, microblocks, leverage, and general smart contracts are not active.

**Legacy UTXO spending:** old outputs keep original txid/vout/nValue/
scriptPubKey; a modern corridor transaction spends them through
ViewLegacyCoin/LEGACY_LOCK under frozen legacy script rules into modern
OWNER or STAKE outputs. No rewrite, forced migration, snapshot, or chain
restart occurs; non-stakers may leave UTXOs untouched forever.

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

## 6. Corridor difficulty (MECHANISM RULED 2026-08-21; value RULED 2026-08-23; PINNED on mainnet 2026-08-27)

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
earlier retarget analysis above is superseded.

**Value policy RULED 2026-08-21: LOW, with stall-safety dominant.** With
a fixed target the failure modes are asymmetric: too easy is recoverable
(a fast, noisy corridor — harmless, since staking has no deadline and
zero reward removes the attack economics), while too hard is the one
catastrophic case (hashpower below calibration and no retarget to
rescue it). The constant is therefore calibrated against the WEAKEST
GUARANTEED PARTICIPANT, not expected participation: one ordinary CPU
core alone must sustain roughly a block per 30–60 seconds, so any single
machine can always push the corridor through and a stall is impossible
by construction; every calibration uncertainty resolves toward EASIER.
The exact compact-bits number is measured against a reference CPU's real
scrypt rate at mainnet H/X pinning time (inert before then) and recorded
with the pin.

**Value RULED 2026-08-23: canonical compact bits `0x1f008000`** — target
2^239, i.e. 2^17 expected scrypt hashes per block (the measured single
core does ~7,466 H/s, so ~18 s per block alone). `0x20000080` encodes the
SAME target non-canonically (mantissa 0x000080, exponent 0x20); the
consensus constant is the canonical form that `arith_uint256::GetCompact`
produces, a canonical round-trip test pins it, and a configured
non-canonical value fails closed like an unset one. NOT PINNED in mainnet
chainparams until the release pin gates pass.

### 6.1 Pacing — VERIFIED COMPRESSIBLE by hashpower; RULED 2026-08-23: min spacing 60 s, future bound 120 s

Owner instruction (2026-08-23): do not assume fixed difficulty implies
fixed elapsed time. Verified: a corridor header is checked for the
constant bits, the scrypt eligibility, `time > MedianTimePast(prev)` and
`time <= now + 2 h` — nothing else. Consequences:

- Expected corridor duration at honest low hashrate is ~1000 × 18 s ≈ 5 h
  per CPU core; more cores/machines divide it.
- A large scrypt miner (Litecoin-class hardware does GH/s; 2^17 hashes is
  sub-millisecond) can mine all 1,000 blocks in seconds to minutes,
  advancing timestamps 1 s per block, all inside the 2 h future window.
  Operators get no wall-clock time to create STAKE outputs; the 20-block
  maturity is satisfied inside the burst; the initial ACTIVE set at M is
  whatever that miner chose to include.
- Nothing in the corridor's stated purpose survives that case. The
  "zero-stake-at-M prevented operationally" ruling (§9) assumed hours of
  corridor, not seconds.

**RULED 2026-08-23 (owner): "add 60 s to 120 s for the corridor, since
someone with a large miner should not compress that window."** Implemented
as consensus for every corridor: `transition_pow_min_spacing = 60` — a
corridor block's timestamp must be ≥ parent time + 60 s
(`time-too-early-corridor`, invalid) — and `transition_pow_max_future = 120`
— at most 120 s ahead of the validating node's clock (`time-too-new`, held
like the modern-PoS rule). Consequence: the corridor takes at least
1000 × 60 s − 120 s ≈ 16.6 h of real time regardless of hashpower; a lone
CPU (~18 s per block at the ruled target) is slowed to one block per
minute, never stalled; a large miner is paced, not empowered. Block
production sets the template time to max(now, parent + 60 s). Test
`corridor_pacing_enforced` (1 s and 59 s refused, 60 s accepted, a burst
advances chain time ≥ 60 s per block, +121 s held / +120 s accepted,
template ≥ parent + 60 s); the corridor fixtures build at 60 s spacing.

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

## 11. Former contradiction register — resolved

The earlier implementation gaps that treated H+1 as immediate modern PoS are
closed: production phase is distinct from codec era; the scrypt corridor,
difficulty/reward rules, Policy Output surface, mempool/miner dispatch, and
marker-aware block production are implemented and covered by transition and
evolution tests. `hard_fork_height` retains its exact meaning as the first
modern-family height H+1, while M = H+1001 selects modern PoS production.
Mainnet activation still fails closed until the seal-derived pins are set.

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

**RULED 2026-08-23 in addition:** corridor difficulty VALUE = canonical `0x1f008000` (§6; PINNED on mainnet 2026-08-27 with H); H = 810,000 / M = 811,001 (re-ruled 2026-08-26, superseding 820,000) and X-distribution = pause-fail-closed (open-decisions OD-10).

**RULED 2026-08-23 in addition:** corridor PACING — minimum spacing 60 s and future bound 120 s (§6.1).

**Later resolution:** Modern-PoS sentinel bits, future drift, horizon, stake,
and reward rules are governed by the reviewed Modern-PoS specification and
`FINALIZED.md`; they are not open corridor choices.

## 13. Destination unchanged

After modern PoS: A1 activates modern FN PoD; A2 activates simple assets plus
FlowMesh seat/vault preparation; A3 activates FlowMesh spot trading and
microblocks after the preparation runway. Bridge-backed bUSD remains
independently fail-closed until every proof/readiness pin passes. The later
FlowMesh release expands the working spot product; futures follow spot, with
only support and the 10× maximum currently locked. The corridor must not couple
to or redesign any of these.
