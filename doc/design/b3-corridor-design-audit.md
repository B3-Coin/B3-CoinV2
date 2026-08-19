# Corridor design audit — gap list (pre-Modern-PoS)

**Scope: design audit only (2026-08-16).** No implementation. Reviews the
regtest-complete corridor against the accepted design; enumerates gaps.
Modern PoS (VRF, proposer selection, rewards, fork choice) is untouched.

## 1. STAKE `B3S1` carrier — ambiguities; finalize-vs-redesign

**Verdict: KEEP the carrier shape (spendable-by-owner prefix-drop envelope);
do NOT redesign. It must not be called final until the gaps below are
resolved.** The shape delivers the required properties — deterministic
recognition, strict rejection of malformed claims, owner-spendable principal
(self-policing cancel), opaque validator key deferring PD-2 — and the
alternatives (a new transaction field: breaks the no-new-tx-fields lock; an
OP_RETURN companion output: separates data from principal and reintroduces
the pairing/ambiguity problems the declaration-window model had) are worse.

Gaps, in decreasing severity:

- **S-1 Non-canonical push encodings.** The 38-byte payload can be encoded
  as a direct push, PUSHDATA1/2/4 — all recognized, all valid, producing
  byte-different scripts for identical stake data. Two "identical" stakes
  can have different scriptPubKeys (different owner-binding hashes,
  different row bytes in equivalence tooling). v1 must REQUIRE the minimal
  (direct) push encoding and reject the rest as malformed claims.
- **S-2 No minimum stake amount.** Only `amount > 0` is enforced;
  `MIN_STAKE_AMOUNT` (OPEN, economics) is unwired. Dust-level stakes can
  bloat the registry unboundedly cheaply. The constant must exist (even if
  its mainnet value stays OPEN) before the carrier is called final.
- **S-3 No unstake-cooldown representation.** Today cancel == instant
  ordinary spend. Correct for the corridor (pre-M there are no duties), but
  the locked modern design requires exit through a COOLDOWN with no weight
  and no spendability. The carrier has no way to express "unstake intent at
  height h"; before M the exit path needs a defined form (e.g. a spend into
  a designated unbonding envelope) or Modern PoS must define it. Decide
  before finalizing, or explicitly scope v1 = corridor-only semantics with
  v2 at M.
- **S-4 Validator-key validity is deferred, un-postured.** 32 opaque bytes
  aggregate weight regardless of whether they are a valid key under the
  eventual PD-2 scheme. What happens at M to weight bound to an invalid
  key (dead weight? refused at derivation? ignored for eligibility but
  counted in W?) is undecided and changes W. Needs an explicit rule when
  PD-2 locks.
- **S-5 Nested/degenerate owner scripts.** The owner suffix may itself be a
  claiming script (recognition reads only the first push — outer wins,
  deterministic, but worth pinning in a test), may be unspendable
  (OP_RETURN owner: permanently burned principal that still carries
  registry weight — decide: forbid, or allow as owner's risk but then an
  unreachable validator holds weight forever), and under modern flags a
  witness-program-shaped owner suffix is witness-enforced (footgun, not a
  consensus flaw — document).
- **S-6 No relay-policy story.** STAKE scripts are nonstandard to stock
  policy: creation transactions do not relay without `-acceptnonstdtxn`.
  Corridor participation via ordinary wallets needs a standardness carve-in
  (policy change, not consensus).
- **S-7 No canonical ModernOutput mapping function.** policy.h documents the
  STAKE view (commitment = owner binding, params = key||reserved) but no
  code produces a `modern::ModernOutput` from a stake CTxOut. Later policy
  machinery (proofs, conservation) will need the one canonical mapping.

## 2. Transition PoW networking/headers — SHA256d assumptions found

Every site inspected; findings with required phase-aware changes:

- **N-1 `LoadBlockIndexGuts` (`node/blockstorage.cpp:162`) — REAL DEFECT.**
  `needs_proof_of_work = (era == MODERN)` runs `CheckProofOfWork(SHA256d
  identity hash, nBits)` over every modern-era index entry at restart.
  Corridor blocks never mined that hash: with a real (non-trivial) corridor
  difficulty, **a node restart fails to load its own block index**. Unseen
  on regtest only because the scaffolding target is ~2^255. Required:
  phase-aware check — TRANSITION_POW entries verify scrypt eligibility (or
  defer to stored-validity flags), MODERN_POS entries keep the placeholder.
- **N-2 `HasValidProofOfWork` (`validation.cpp:4871`) / `CheckHeadersPoW`
  (`net_processing.cpp:2879`).** Chain-wide `return true` for
  `legacy_b3coin` — corridor headers are not *blocked* (good), but the
  cheap header-spam pre-filter is disabled for the whole B3 chain (a
  pre-existing legacy-era accommodation now covering the modern era too).
  Corridor headers are header-only verifiable (scrypt needs only the 80
  bytes); once anchored, per-header heights are derivable inside a batch.
  Required: phase-aware filter — corridor-range headers checked by scrypt
  eligibility against the corridor bits; post-corridor headers per the
  eventual modern PoS header rule; only legacy-era headers keep the
  full-block deferral.
- **N-3 Anti-DoS work accounting is hash-agnostic but corridor-shaped.**
  `CalculateClaimedHeadersWork`, `GetAntiDoSWorkThreshold`, compact-block
  work gating (`net_processing.cpp:5030`) and `nChainWork` all derive from
  nBits (correct by design — the trust formula is uniform). But an easy
  constant corridor difficulty means ~zero claimed work for 1,000 blocks:
  header-spam thresholds, `nMinimumChainWork` and assumevalid around the
  corridor are an input into the mainnet corridor-difficulty decision
  (OPEN), not an independent code change.
- **N-4 `getblocktemplate` is not corridor-aware.** External miners get a
  template whose `target`/`bits` describe SHA256d semantics; nothing tells
  them to grind scrypt over the header. Required: GBT corridor annotation
  (or a documented corridor-only mining RPC) before any public corridor.
- **N-5 Post-corridor header rules are the modern-PoS placeholder**
  (stock `GetNextWorkRequired` + deferred SHA256d): already recorded; the
  regtest min-difficulty walk-back can legitimize corridor bits past the
  corridor (found by the e2e test). Superseded wholesale by the modern PoS
  header spec; no interim change needed beyond awareness.
- **N-6 Verified fine (no change):** `ReadBlock` header-PoW guard skips B3
  modern blocks (identity checked against the indexed hash);
  `GetBlockProof`/fork choice (nBits-uniform by design); compact-block
  relay (identity-based); `headerssync` commitments (identity-based);
  the corridor connect/validation pipeline itself (stages 1–8).

## 3. Corridor reward economics — options only (NO decision)

Base reward model for the 1,000 blocks:
(a) fees only (current scaffolding: `transition_pow_reward = 0`) — no new
issuance, but near-zero incentive for outside hashrate: corridor security
rests on stakeholder self-mining;
(b) fixed small subsidy + fees — simple, predictable, bounded issuance
(≤ 1,000 × subsidy), attracts marginal hashrate;
(c) declining subsidy across the corridor (step or linear) — front-loads
hashrate when the registry is forming (the censorship-sensitive phase),
tapers toward the handoff;
(d) subsidy calibrated as a fraction of expected modern-PoS reward — prices
the corridor consistently with OD-2, but couples two open decisions.

Orthogonal miner→validator-capture rules (any base model):
(i) corridor rewards mature only beyond H+1000 (cannot fund initial-set
stake at all);
(ii) rewards spendable but their value ineligible for the initial validator
set (needs provenance tracking — heavier);
(iii) rewards stake normally only after M (equivalent to (i) via maturity ≥
corridor remainder);
(iv) no restriction (accept that a dominant miner can convert subsidy to
stake — with C well before PF, subsidy earned late cannot enter the initial
set anyway; interaction with cutoff C is the key parameter).

Constraints on all options: the contract's issuance-cap invariant requires
explicit cap accounting for any subsidy; reward maturity for corridor
coinbases is currently the stock rule and needs an explicit corridor value;
fees-only corridors make fee sniping/empty-block mining rational (nothing
to lose by mining empty).

## 4. H+1000 registry state — sufficient for Modern PoS?

**Verdict: informationally sufficient at the UTXO level — every input the
modern design base needs is derivable from the chainstate — but the current
`DeriveStakeRegistry` output and mechanism have four gaps before Modern PoS
can consume them.**

Sufficient today: per-validator aggregated weight (locked rule implemented);
total weight W; maturity classification at any height (heights live in the
coins); owner authorization (owner script inside the carrier); determinism
(pure function of the UTXO set, hence reorg-safe by recomputation and
attested by the corridor's connected history).

- **R-1 Per-output detail is discarded.** The registry keeps only
  aggregates; Modern PoS needs per-output records (outpoint, key, amount,
  creation height) for the cutoff-C split (initial ACTIVE at ≤ C vs PENDING
  after), for later unbonding, and for any evidence/attribution. The
  information exists in the UTXO set; the derivation must retain it.
- **R-2 Full-scan derivation does not scale.** `DeriveStakeRegistry`
  enumerates the entire UTXO set (fine at regtest scale; a full DB scan per
  evaluation on mainnet). Modern PoS evaluates per slot/epoch: the registry
  needs incremental connect/disconnect maintenance with crash-safe
  persistence (coinstatsindex-style), with the full scan retained as the
  audit/recovery path.
- **R-3 Epoch snapshotting is absent.** The design base freezes the active
  set and W at epoch boundaries; nothing represents "the registry as of
  height h_snapshot" except recomputation at that height (impossible from
  the current UTXO set alone once spends occur after the boundary — it
  requires the incremental history of R-2 or undo-walking). Must be part of
  the registry design, not bolted on.
- **R-4 Cutoff C is unplumbed.** No parameter, no ACTIVE/PENDING split in
  the registry output (only mature/immature at the evaluation height). The
  initial-set rule "created ≤ C, unspent, mature" needs C on params and the
  split in the derivation — blocked only on the OPEN C decision.
- Cross-reference: S-3 (unbonding state) and S-4 (invalid-key posture)
  also land in registry semantics at M; W changes depending on S-4's rule.

## Summary counts

Blocking-before-mainnet-corridor: N-1 (restart defect), S-1, S-2, S-6, N-4,
plus the OPEN economics (§3) and corridor difficulty. Blocking-before-
Modern-PoS-implementation: R-1..R-4, S-3, S-4, N-2 (post-corridor part),
N-5. Nothing found that invalidates the corridor architecture itself.
