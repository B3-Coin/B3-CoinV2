# Proof of Disintegration — the B3 Fundamental Node creation mechanism

**Status: DESIGN DIRECTION (2026-08-16).** Historical mechanism authoritative
(traced from `master`); modern direction locked at the level below. The
modern creation-cost curve was pinned by owner ruling 2026-08-28; reward and
remaining FlowMesh economics stay OPEN. This document supersedes any earlier
description of FN
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

## 2. Modern FN creation preserves PoD — economic intent LOCKED, encoding LOCKED (2026-08-17)

Modern B3 keeps Proof of Disintegration as the FN creation mechanism in its
ECONOMIC lineage: **B3 is permanently sacrificed, and an FN right is
created**; the destroyed amount is permanently excluded from B3 supply and
never claimable by the block producer; the creation is explicitly
recognizable by consensus.

**The modern ENCODING is LOCKED** by owner ruling 2026-08-17 (§10),
closing the question the master handoff §4.6 recorded — implicit
accounting gap vs explicit visible burn primitive — **in favor of an
implicit on-chain gap with a validation-only hypothetical disintegration
output**. Modern conservation's objection to hidden gaps is answered
because the gap is not hidden: validation explicitly recognizes and
enforces the destroyed amount `D` (§10.1). This lock is unrelated to
legacy claims, which mint FN Coins for destruction that already happened
historically and follow §8 unchanged.

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

    PoDId = a deterministic, non-self-referential modern creation
            identifier; exact derivation remains OPEN.
            (Neither the issuing transaction's identity nor a creation
            outpoint can serve: both depend on the transaction that
            contains the FN output, which itself serializes the PoDId.)

    one valid PoD event  → at most one FN creation
    same PoD reused      → INVALID / no second FN

Replay, restart, reindex and branch handling must never double-create FN
state.

## 6. Lineage

    LEGACY FN                          MODERN FN
    =========                          =========
    historical PoD                     modern PoD
    implicit input/output gap          implicit gap, validated through the
                                       hypothetical disintegration output
                                       (§10.1)
    historical collateral schedule     RequiredDisintegration curve
                                       (§11.2; values PINNED 2026-08-28)
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

## 8. Legacy FN claims — SUPERSEDED scan-and-claim MVP (kept for the surviving definitions; see banner)

> **SUPERSEDED (owner ruling 2026-08-17/18; conflict register C-R4).**
> The scan-and-claim mechanism of this section — funding-key claim
> signatures (§8.3), the per-node claim activation flow, and any
> production PodDB integration into sync/reindex — is replaced by the
> **archival-builder / stateless-proof issuance model**, the owner-locked
> direction:
> [b3-legacy-fn-issuance-proposal.md](b3-legacy-fn-issuance-proposal.md).
> One archival wallet builds proof-carrying issuance transactions from
> the sealed prefix (deferred privately until M); every node verifies
> the embedded evidence statelessly against the H/X anchor; issuance is
> deduplicated per PoDId. What SURVIVES from this section as governing:
> the qualifying-PoD definition and detector, the §8.2 derivation
> machinery (classifier, `DerivePodRecords`, `PodDB`, sync helper) as
> **builder-side tooling only**, the capacity-report machinery, and the
> one-issuance-per-PoDId uniqueness principle (the future
> `issued[pod_id]` state — the corrected terminology: this is ISSUANCE,
> not a user claim protocol; `claimed[pod_id]` below reads as its
> superseded name). Semantic reversals under the corrected FN model
> (owner 2026-08-18): the 1-B3 **P2PKH** output is no longer audit-only
> — it is the historical FN identity and designates the issuance
> beneficiary (lowest index; a PoD without one is IGNORED, no
> fallback); funding-script claimability (`claimable` /
> `UNSUPPORTED_FUNDING_SCRIPT`) no longer gates issuance, because no
> fresh signature is ever checked; and **FN Coin is ONE global
> fungible-but-indivisible asset** (`FN_ASSET_ID`, decimals 0, cap
> `MAX_FN_EVER_ISSUED = 5000`, ratified 2026-08-22) — no per-PoD FN objects, no PoDId in
> ordinary FN outputs, and the §10.2 "same-PoDId successor" transfer
> lifecycle is superseded with it (transfers move whole units of the
> one asset; extinguishment reduces live supply, never issuance
> capacity).

Approved direction (2026-08-16, revised; **superseded as annotated
above**): FN eligibility is derived entirely during node synchronization
from on-chain history, and claims are authorized by **fresh signatures
from the historical funding keys** — the keys that actually paid the
disintegration. There is no external scanner, no external claim registry,
and no marker-based ownership. This section is retained as the record of
the superseded design.

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

> **CORRECTIVE + HARDENING (recorded and implemented 2026-08-17):**
> (a) the H/X anchor is enforced BEFORE any PodDB mutation — H and X
> configured together or not at all, H nonnegative, and once height H
> exists on the active chain its hash must equal X, else sync fails
> before any write, with the logical marker and records unchanged;
> below H the derivation is
> the LOCAL PREFIX only, never described as an anchored claim set;
> (b) a persisted marker ABOVE a newly pinned H is atomically rewound —
> reconnect/restart then holds exactly the prefix through H inclusive,
> and a qualifying PoD exactly AT H survives the rewind; (c) missing,
> truncated, malformed or mismatched undo data — wrong entry counts,
> wrong per-input coin counts, spent/null coins, out-of-range amounts
> or an out-of-range input-value sum — FAILS CLOSED: `DerivePodRecords`
> leaves the caller's output completely UNMODIFIED (structural and
> obviously-invalid-Coin validation only; the provenance rule is stated
> once, below); (d) PodDB STRUCTURAL/CANONICAL damage fails closed —
> the checked conditions are: noncanonical or trailing-byte keys and
> values (exact-consumption decoding, deobfuscation applied),
> key/record identity mismatches, negative record heights, records
> above the marker height, an undecodable marker including an
> 'm'-prefixed key with trailing bytes (CORRUPT, never mistaken for
> missing), a missing marker over a nonempty record namespace, and
> iterator I/O errors; a valid marker never bypasses the
> full-namespace scan; rewinds validate the existing marker and the
> namespace before writing anything, and reads fail rather than return
> a partial prefix;
> (e) `-podreport` publication is TRANSACTIONAL and EQUIVALENCE-GATED —
> records accumulate privately during replay and publish only after
> BOTH the complete H/X-anchored replay succeeds AND `U_port ==
> U_replay`; a late failure or an equivalence mismatch returns no
> partial records and no report, so the activation-gate numbers are
> never presented as authoritative without equivalence; (f) input
> summation uses a PRE-ADD MoneyRange guard (never add first, check
> after), and PodDB decoding is STRICTLY CANONICAL — the complete
> record namespace is scanned from the raw key prefix with
> exact/full-consumption key and value decoding, key/record identity
> checks and an iterator-status check at termination; a missing marker
> is valid only over an empty record namespace; a damaged marker or key
> fails closed (rewinds abort before writing, reads throw rather than
> return partial data). The undo provenance rule, stated once: disk
> checksums detect stored-byte corruption; validated ConnectBlock undo
> data or TrustedReplay reconstruction supplies SEMANTIC provenance.
> Normal production sync/reindex derivation is the REQUIRED FUTURE
> INTEGRATION — not present behavior; the sync helper and the offline
> replay paths are what exist and are tested today. Tested with the regtest evolution method:
> genuine legacy PoW, genuinely validated legacy PoS with the authentic
> PoD, the pinned H/X boundary and the transition-PoW corridor — the
> chain then reaches the modern-PoS height and verifies the current
> `no-modern-pos-rules` FAIL-CLOSED gate (no modern-PoS block is
> produced yet) — plus the fn_pod chain fixture (undo-file truncation,
> at-H boundary survival, anchor matrix, controlled marker/key
> corruption, transactional replay, restart after rewind, deterministic
> recovery; genuine undo copied and deliberately mutated throughout).

Every node synchronizes from block 1; the deterministic PoD scan MUST
run inside normal sync/reindex/trusted-replay (input values and funding
scripts from the connect/undo path — the proven mechanism; no separate
scanning pass, no optional index, no local configuration). That
production wiring is the REQUIRED FUTURE INTEGRATION (see the note
above); today the checked sync helper and the offline replay paths
implement and test the same derivation.

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
                               || fn_output_value
                               || owner_v1_policy_commitment)

    (Native-modern revision, 2026-08-17: FN Coin exists only in the
    modern era and its ownership is the MODERN POLICY SYSTEM, never a
    legacy destination script. The digest binds the complete
    economically relevant FN output — network, PoDId, underlying B3
    value and the modern owner — all fixed-width; no CScript appears.)

    fn_output_value = the FN output's underlying modern B3 value:
                      CAmount, fixed-width 8 bytes little-endian,
                      validated nonnegative and within MoneyRange. A
                      copied authorization can only recreate the same
                      PoDId with the same value under the same owner;
                      claimed[pod_id] prevents a second issuance.
    chain_domain  = ModernChainDomain — the contract's immutable
                    anti-replay network identifier:
                    TaggedHash("B3/MODERN/CHAIN", genesis_hash || X),
                    both hashes as their 32 raw internal-order bytes;
                    no defaults, no globals; fail-closed when either
                    hash is unset (src/modern/fn.h), so an
                    authorization is valid on exactly one network.
    pod_id        = 32 raw bytes, internal hash order.
    owner_v1_policy_commitment
                  = the FN output's modern OWNER v1 policy commitment
                    (32 bytes): one party, a threshold group or an
                    organization alike — control is entirely the modern
                    OWNER mechanism.

    authorization = { funding_script_index (compact size),
                      form (1 byte: P2PKH | P2PK),
                      P2PKH only: pubkey (33/65 bytes, fully valid),
                      signature (bare strict DER, LOW-S, over
                                 claim_message; no sighash byte) }

    (P2PK refinement, same ruling: a P2PK authorization carries the
    SIGNATURE ONLY — the key is the funding script's embedded key.)

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

### 8.4 The claim transaction — NATIVE MODERN FORM (owner ruling, 2026-08-17)

**SUPERSEDED WIRE FORMS.** The earlier script-level carriers — the
"B3F1" `PUSH36(envelope) OP_DROP <owner locking script>` ownership
scriptPubKey and the "B3FP" `OP_RETURN` authorization proof output —
are **superseded in full** and must not appear in code, tests, tooling
or documentation. FN Coin exists only in the modern era; its objects
are native modern policy objects, not legacy scripts, and an OP_RETURN
carrier has no role in the modern codec (auxiliary data lives in the
typed segregated proof area). The prior script-policy analysis
(datacarrier budgets, NULL_DATA classification, script standardness) is
obsolete with them. Implementation: `src/modern/fn.h`.

A claim is a MODERN transition (height ≥ M) carrying:

1. **One FN v1 policy output** — the FN Coin (modern/policy.h
   `PolicyType::FN = 5`, consensus-stable, v1):

        asset              = native B3
        amount             = the underlying modern B3 value
        policy_type        = FN (5), policy_version = 1
        policy_commitment  = the modern OWNER v1 owner binding
        policy_params      = exactly the 32-byte PoDId (zero invalid):
                             a legacy claim carries the historical
                             disintegration's txid (raw bytes, internal
                             hash order); modern issuance will carry the
                             still-OPEN non-self-referential identifier

   FN v1 is a wrapper around modern OWNER authorization: color and
   PoDId live in the FN policy; control is entirely the committed OWNER
   mechanism, so one person, a threshold group or an organization are
   all naturally supported and NO legacy destination script exists.
   Future FN spend validation delegates owner authorization to OWNER,
   then enforces transfer-vs-extinguishment (§10.2). FN v1 is NOT an
   activated policy on any network in this stage (fail closed by
   modern/policy.h IsActivatedPolicy).

2. **One FN claim creation action** (modern/creation_action.h `CreationAction`;
   registry: action_type `CREATION_ACTION_FN_CLAIM = 1`, version 1).
   Payload:

        compactSize(fn_output_index)      -- the FN output it creates
        compactSize(n_authorizations)     (n >= 1)
        n × { compactSize(len) authorization-record }   (8.3 records,
              ascending, index_i == i, canonical compact sizes,
              bounded reads, no trailing bytes)

   Structural rules: actions sorted by ascending output index, no
   duplicates, referenced output must exist and be FN v1, exactly one
   action per FN output — no FN output without an action, no action
   without an FN output.

   **Scope of the action rules (owner clarification, 2026-08-17):**
   these are the LEGACY-CLAIM rules; `CheckFnCreationActions` is a
   legacy-claim-context helper only. Modern FN ISSUANCE stays the
   simple path: **no FN claim action and no historical funding-key
   authorization** — one ordinary issuance transaction whose
   authorization is paying the required disintegration `D` through the
   locked §10.1 hypothetical-output arithmetic, alongside its ordinary
   modern input proofs and whatever outer transaction format is
   selected later. **The exact modern issuance identifier and the
   exact issuance-vs-claim classifier remain OPEN owner decisions.**
   (An issuance PoDId cannot be the issuing transaction's own id: the
   FN output serializes the PoDId while the transition id hashes the
   outputs — self-referential and impossible. §5's "exact identifier
   OPEN" stands.)

   **Integration status (exact, updated for the v2 envelope):**

   - ENCODED AND TESTED: the standalone claim-action codec (Commit 3)
     AND the inactive **v2 transition envelope** that carries the
     action collection — `uint16 version (LE, = 2 only)` followed by
     the v2 body (the v1-shaped inputs/outputs/proofs plus the
     creation-action section; modern/proof.h, with the neutral frame
     and bounds in modern/creation_action.h). The v2 identities are
     version-domain-separated tagged hashes ("B3/MODERN/TX/ID/V2",
     "…/PROOFAREA/V2", "…/FULL/V2"): `TransitionIdV2` commits to the
     version and the canonical inputs/outputs and is blind to proofs
     and actions; `ProofAreaCommitmentV2` and `FullTransitionIdV2`
     commit to the action collection. Identical v1/v2 economic bodies
     never share an id.
   - UNCHANGED: raw modern-transition v1 remains the sole v1 format,
     byte-frozen — serialization, `TransitionId`,
     `ProofAreaCommitment`, `FullTransitionId` and every frozen vector.
     v1 and v2 byte patterns CAN overlap — discrimination is never by
     inspection: the future outer context explicitly selects one
     decoder, with no sniffing or fallback and exact exhaustion. The v1
     `ProofAreaCommitment` does not commit FN actions. The input-proof
     invariant (input i ↔ proof i) is untouched in both versions.
   - DECODE BOUNDS (enforced before allocation, for EVERY collection —
     inputs, outputs incl. the 80-byte policy-params cap, proofs incl.
     the 4,000-byte payload cap, and actions): per-action payload ≤
     4,000 bytes (the existing proof-area bound); action count ≤ 64 and
     aggregate action-section ≤ 20,000 bytes — after framing that
     permits FOUR maximum-size actions — (**both owner-ratified
     2026-08-17**); envelope ≤ 1,000,000 bytes (**owner-ruled a
     TEMPORARY defensive parser ceiling while the codec is inactive —
     NOT the final production consensus, relay or weight limit, which
     must be reconciled with the future outer transaction/block codec
     before activation**). Unknown action (type, version) pairs are
     INVALID at decode — never silently ignored — and any future
     consensus validation must keep that rule.
   - NOT YET TRUE: **the active chain does not carry the v2 envelope**
     — no production outer transaction/block codec selects it; defining
     the format is not activating it. Claim validation (eligibility,
     signature verification against PodRecords, `claimed[pod_id]`,
     counters, supply), activation, state, wallet, RPC, mempool,
     mining, rewards and economics remain untouched and FOR LATER
     commits. **FN cannot activate until a production context selects
     the envelope through its own reviewed change.**

        Size bound: a creation-action payload shares the segregated
        proof area's limit, MAX_TRANSITION_PROOF_SIZE = 4,000 bytes.
        SUPERSEDED DIAGNOSTIC (owner correction 2026-08-18): the
        -podreport payload arithmetic described here measures the
        worst-case payload of THIS superseded type-1 encoding and is
        NON-AUTHORITATIVE for activation — it is NOT the capacity gate
        for the live type-2 issuance carrier (LegacyFnIssuanceActionV1,
        b3-legacy-fn-issuance-proposal.md), whose real encoded sizes
        over actual history remain unmeasured future work.

   Status note (owner rulings 2026-08-17/18): this encoding is a
   RESERVED/SUPERSEDED frozen record, consensus-inactive (codec +
   vectors only, unreachable from validation/mempool/wallet/RPC/mining;
   FN v1 unactivated everywhere; no FN semantic checker accepts it).
   The real-chain `-podreport` QUALIFYING COUNT (`R` vs the 5,000 cap,
   §11.1) remains the mandatory pre-activation gate; the height-807,709
   result of 3,500 is a floor and the through-H result is final. The
   payload figures do not gate anything. H is pinned; X remains blank.

3. Ordinary inputs paying fees and any ordinary outputs. The claim spends
   NO historical outpoint; the marker, if it still exists, is untouched
   and irrelevant.

Validation at connected height ht, for every FN v1 output (native form):

    ht < M or no corridor            → INVALID "fn-claim-inactive"
    FN output malformed (v1 rules)   → INVALID "bad-fn-claim"
    duplicate pod_id among outputs   → INVALID "fn-claim-duplicate"
    pod_id ∉ PodRecords or !claimable→ INVALID "fn-claim-ineligible"
    pod_id already claimed           → INVALID "fn-already-claimed"
    creation action missing/mismatched (8.4 structural rules)
                                     → INVALID "fn-claim-missing-auth"
    any authorization fails 8.3      → INVALID "fn-claim-bad-auth"
    else: mint exactly one FN {pod_id, FN output, owner commitment,
          claim height}; FN Coin supply += 1; claimed[pod_id] = true

**FN TRANSFER** is distinguished by the authorizing INPUT: spending an
existing FN v1 output (modern OWNER authorization) with exactly one
successor FN v1 output with the SAME pod_id transfers the FN; transfers
never mint and never split/merge, and several independent FNs may move
in one transaction under the both-directions pod_id bijection. A
transfer carries no creation action and no funding signatures. A
transaction that emits an FN v1 output for a pod_id without either
spending that FN object or carrying a valid claim creation action is
INVALID "fn-claim-missing-authority". Wallets present one FN asset
balance with per-object PoD provenance.

> **Revised 2026-08-17 (§10.2):** the earlier rule that a spend of the FN
> object without a same-PoDId successor is INVALID
> ("fn-transfer-no-successor") is superseded. An ordinary B3 spend of the
> FN output is VALID and permanently **extinguishes** the FN — the color,
> the PoDId association and all future rewards/perks end, and the FN can
> never be recreated from the same PoDId. Wallets MUST separate "Transfer
> FN" from "Spend as ordinary B3" (§10.2).

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
| Claim with malformed FN output / creation action | INVALID bad-fn-claim / fn-claim-missing-auth |
| Wrong pubkey for P2PKH funding script | INVALID fn-claim-bad-auth |
| Transfer chain after claim | same pod_id, new outpoints, supply unchanged |
| Ordinary spend without a same-PoDId successor | VALID; the FN is permanently extinguished (§10.2 — supersedes the earlier fn-transfer-no-successor rejection) |
| Transfer with changed pod_id | fails its authority check (fn-claim-missing-authority) |
| One tx, several independent FN transfers | valid; bijection both directions |
| Reorg across the claim | FN removed + claimed flag cleared atomically; reconnect identical |
| Reorg across claim + transfers | ownership chain rolls back and replays identically |
| Restart / reindex / trusted replay | identical PodRecords, audit set, claimed flags, FN state, supply |
| Activation-boundary behavior | claim-form invalid at M−1, valid at M, on two independently synced nodes |

## 9. Decision status

**LOCKED / DESIGN DIRECTION:** historical FN creation = PoD; historical PoD
is implicit destruction through the accounting gap; PoD value is never a
miner fee; PoD permanently reduces B3 spendable supply; modern FN creation
preserves PoD's ECONOMIC lineage (PoD remains semantically distinct from
generic BURN); **the modern ENCODING is LOCKED (2026-08-17)** to the
implicit gap with a validation-only hypothetical disintegration output
(§2, §10.1); the FN lifecycle rule — FN-preserving transfer vs
ordinary-spend extinguishment — is LOCKED (§10.2); modern FN ownership is
explicit and on-chain; FN Coin is separate from B3 supply; one PoD event
creates at most one FN; mainnet historical collateral rules unchanged;
**FN economic direction LOCKED (2026-08-17, §11):** limited total supply
(`MAX_FN_EVER_CREATED = 5,000`) plus a deterministically nondecreasing
creation cost (`RequiredDisintegration`, values PINNED 2026-08-28) — FN as a scarce,
freely transferable market asset; all historical rights reserved before
modern issuance and never crowded out; scarcity counted on
total-ever-created; extinguishment never reopens a creation slot.

**LOCKED for the legacy-claim MVP (2026-08-16, integrated scan-and-claim) — MECHANISM SUPERSEDED 2026-08-17 (archival-builder issuance); the qualifying-PoD DEFINITION below survives verbatim:**
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
pre-activation claim forms invalid; safe post-claim conservation (the
same-PoDId successor rule preserves the FN; no split/merge; per the
2026-08-17 revision an ordinary spend without a successor is valid and
permanently extinguishes the FN, §10.2); no administrator list or
off-chain ownership anywhere in consensus.

**OPEN (after the 2026-08-28 cost-curve ruling):** reward amount and
schedule, reward ownership cutoff on
transfer/extinguishment, legacy claim expiry policy (none by default) —
`MAX_FN_EVER_CREATED` is no longer open: its current owner-selected
value is 5,000 (D-1, ratified 2026-08-22 after the real-history report found R = 3,500; supersedes the 1,000 selection), revisable before activation through the reviewed
process, gated on the real-chain reservation count;
final FN ownership serialization beyond the MVP claim form; the modern
FN-creation transaction form's exact detection/serialization (§10.1); FN
reward economics beyond the §11.5 parameters (OD-4). Implementation must
not close these silently. *Resolved since the earlier list:* excess-gap
treatment and modern ordinary-fee calculation (`fee = I − O − D`, excess
is ordinary fee — §10.1); FN lifecycle beyond transfer (ordinary-spend
extinguishment defined, §10.2; still no split/merge); dynamic-vs-fixed
pricing (deterministic nondecreasing curve, §11.2); FN issuance-rate
structure (capped total and the pinned tranche curve, §11.5).

## 10. Modern FN accounting and lifecycle — LOCKED (owner ruling, 2026-08-17)

**Status: LOCKED.** This section records the owner's hypothetical-output
ruling in its intended scope: **modern FN-creation accounting only**. It
does not touch the legacy issuance mechanism — which, since the same-day
ruling recorded in §8's banner, is the **archival-builder / stateless-proof
model** of b3-legacy-fn-issuance-proposal.md (no funding-key claims, no
production PodDB, issuance deduplicated per PoDId); §8's scan-and-claim
carrier and per-node `claimed[pod_id]` flow are superseded, not pending.
(A same-day proposal that misapplied the hypothetical-output idea to the
legacy claim anchor — virtual claim outpoints materialized into the UTXO
set at M — was **rejected in full by the owner and must not be revived**.)

### 10.1 The validation-only hypothetical disintegration output

For a transaction recognized as a **modern FN-creation transaction**
(the exact detection/serialization of that form is OPEN, §9):

    I = real input value (sum of spent prevouts)
    O = the sum of EVERY real serialized B3 output — including the real
        FN-colored ownership output the creation emits. The hypothetical
        disintegration amount D is separate from and never conflated
        with the FN ownership output.
    D = the required disintegration for this creation — a
        consensus-determined atomic amount (its determination is the
        economics direction, recorded separately)

    During validation ONLY, the transaction is treated as though its
    output total also contained a hypothetical output of value D.

    Mathematical rule:  I >= O + D,  fee = I − O − D

    Overflow-safe evaluation (normative — `O + D` must never be computed
    directly in a type where it could overflow; subtraction first):

        validate I and O with MoneyRange
        if I < O:      reject
        gap = I − O
        if gap < D:    reject
        fee = gap − D
        validate D and fee with MoneyRange

The hypothetical disintegration output is:

- **never serialized**, stored, or indexed;
- **never given an outpoint**;
- **never added to the UTXO set or any other persistent state**;
- **never spendable**;
- without any effect on the txid;
- present **only** in the validator's arithmetic, to account for the
  permanently destroyed creation cost.

**Fee calculation vs fee distribution — kept distinct:**

    ordinary fee amount = I − O − D

The equation defines the ordinary fee AMOUNT only. **Who receives that
amount remains OPEN** under the modern reward/fee policy; no statement
that a miner or block producer necessarily receives it may be made until
that policy is locked. `D` itself is **never distributable and never
recoverable** B3, to anyone, under any policy — the exact continuation of
the historical rule (§1: the disintegrated amount is not a fee). Value
beyond `O + D` is part of the ordinary fee amount and creates no
additional FN (owner-approved resolution of the excess-gap question).

**Arithmetic discipline:** consensus arithmetic uses raw atomic
`CAmount` units exclusively — integer only, no floating point.
Human-facing modern prices may be expressed in modern B3 (= kB3, the
locked denomination model in `b3-test-baseline.md`); validation must
never use display-unit arithmetic.

### 10.2 FN lifecycle — FN-preserving transfer vs ordinary-spend extinguishment

An FN Coin is a colored, normally spendable B3 output, and it must remain
**freely transferable as FN** — never locked permanently to its creator:

- An **FN-preserving spend** — exactly one successor FN ownership output
  with the same PoDId (§8.4) — transfers the FN, its PoDId and its
  future rewards/perks to the new owner.
- An **ordinary B3 spend is VALID** and permanently **extinguishes** the
  FN: the color is removed and its future rewards/perks end. This
  supersedes the earlier `fn-transfer-no-successor` invalidity rule.
- An extinguished FN **cannot be recreated from the same PoDId** — by
  any party, under any rule. The same PoDId can never create FN twice.
- No FN split or merge (MVP, unchanged).
- **Wallets MUST clearly separate "Transfer FN" from "Spend as ordinary
  B3"**, and MUST exclude FN-colored outputs from ordinary automatic
  coin selection by default — extinguishment must be an explicit act,
  never an accident of coin selection.

## 11. FN scarcity and market economics — LOCKED direction (owner ruling, 2026-08-17)

**Status: LOCKED direction. D-1 is `MAX_FN_EVER_CREATED = 5,000`
(2026-08-22), and D-2 through D-4 were pinned on 2026-08-28 as the
15,000 / 30,000 / 60,000 B3 three-tier curve. Reward parameters remain
OPEN (§11.5).**
FN has **both a limited total supply and a deterministically increasing
creation cost**. The combination is intentional: FN is to become a
scarce, transferable market asset whose future creation becomes
progressively more expensive. The accounting mechanics (§10.1) and the
lifecycle rules (§10.2) are already locked; this section locks the
economic structure above them. The pure price helper is implemented but
no activated consensus path calls it.

### 11.1 Limited supply — `MAX_FN_EVER_CREATED`

A consensus constant **`MAX_FN_EVER_CREATED`** caps FN creation.

**Owner parameter decision (2026-08-17): `MAX_FN_EVER_CREATED = 1,000`; SUPERSEDED 2026-08-22: `MAX_FN_EVER_CREATED = 5,000`** — the equivalence-verified real-history -podreport found R = 3,500 qualifying historical PoDs (all claimable), and per the never-truncate-rights rule the cap was raised to honor every historical right with headroom for modern issuance.
The cap includes every qualifying historical FN right, claimed or
unclaimed, and every modern FN ever created; therefore
`remaining modern capacity = 5,000 − historical_reserved − modern_created`
(the `C − R − M` invariant below with `C = 5,000`). Revision rules:

- **Until activation**, the owner may revise the numerical value through
  the reviewed protocol process (a reviewed documentation commit, as
  here).
- **After activation**, changing the cap requires an explicit versioned
  consensus upgrade at a defined activation height. It is never
  node-local or independently configurable.

Supply rules, all consensus-enforced:

- The cap applies to **total FN creation rights ever**, not merely
  currently active FN.
- **All qualifying historical PoDs have their FN rights reserved before
  any modern FN creation is allowed.** Historical rights can never be
  crowded out by modern issuance.
- **Modern mint capacity = `MAX_FN_EVER_CREATED` − reserved historical
  FN rights.**
- **Historical-count activation gate — a PRE-ACTIVATION release gate,
  not a block-validation rule.** The real-chain `-podreport` run through
  H (§8.4 capacity gate, counting every qualifying PoD including rights
  whose script form is not yet supported) is mandatory **before any
  FN-activating mainnet release is produced**.
  The height-807,709 report established a floor of `R = 3,500`; the
  mandatory through-H report fixes final R. Every final historical right
  is reserved under the 5,000 cap before FN activation, so reachable
  modern capacity is at most 1,500 and may be smaller. If final `R > 5,000`,
  FN activation cannot proceed under the current cap and returns to the
  owner. Historical rights are never truncated, discarded or reprioritized. This gate is **not**
  an ordinary block-validation rule that unexpectedly halts an
  otherwise-running chain at M. A defensive implementation MAY fail node initialization
  loudly if committed activation parameters contradict the derived
  historical count, but that is protection against an invalid release,
  not the normal activation mechanism.
- Burning or extinguishing an FN (§10.2) **reduces active supply but
  never reopens a creation slot**.
- The same PoDId can **never** create FN twice (§10.2, §8.5).
- Once all permitted creation rights are used, modern FN creation
  **permanently stops** unless a future explicit consensus upgrade
  changes the cap.

**Supply accounting — normative counters, invariants and transitions.**
(Within this subsection the symbols `H` and `M` denote COUNTERS per the
owner's definition — not the chain heights `H` = LEGACY_FINAL_HEIGHT and
`M` = activation height used elsewhere in this document. Where the
prose could be ambiguous it says "the counter" explicitly.)

    R = complete historical FN rights reserved: EVERY qualifying
        historical PodRecord through the final legacy height, fixed at
        activation — INCLUDING records whose funding-script form is not
        currently supported. "Claimable" controls redemption support,
        never reservation: an unsupported-script right still consumes
        its slot under the cap.
    H = historical FN rights successfully claimed        (a counter)
    M = modern FN Coins ever created on the active chain (a counter)
    A = currently active FN
    X = permanently extinguished FN
    C = MAX_FN_EVER_CREATED

Required invariants, holding at every block on the active chain:

    0 <= H <= R
    R + M <= C
    H + M = A + X
    remaining modern capacity = C − R − M

Terminology, so nothing can be double-counted:

- `R + M` is the **cap-consuming rights count**.
- `H + M` is the number of **FN Coins actually issued**.
- Unclaimed historical reservations (`R − H`) consume cap capacity but
  are not yet active FN Coins.
- A historical claim moves one right from unclaimed to claimed; it does
  **not** consume modern capacity.
- Extinguishment reduces active supply but changes neither `H`, `M`,
  nor the remaining modern capacity.

State transitions (the only ones that exist):

    Historical claim:  H += 1;  A += 1
    Modern creation:   M += 1;  A += 1
    FN transfer:       no counter changes
    FN extinguish:     A −= 1;  X += 1

Reorganizations reverse the corresponding transitions exactly —
disconnecting a block undoes its transitions in reverse order, and every
counter is reorg-managed and restart/reindex/replay-deterministic.

### 11.2 Increasing creation cost — `RequiredDisintegration`

The required disintegration `D` of §10.1 is determined by a
**nondecreasing consensus function** of the counter `M` (modern FN Coins
ever created, §11.1):

    D = RequiredDisintegration(M) -> CAmount

The owner pinned an explicit three-tier table on 2026-08-28. The input is
the number of modern creations ever completed before the candidate:

| `M` before creation | modern slot | required disintegration |
|---:|---:|---:|
| 0..499 | 1..500 | 15,000 B3 |
| 500..999 | 501..1,000 | 30,000 B3 |
| 1,000..1,499 | 1,001..1,500 | 60,000 B3 |
| 1,500+ | none | creation refused permanently |

The table uses integer base-unit arithmetic. Historical issuance never
advances `M`, and extinguishment never reduces it or reopens capacity.
The final through-H reservation can make a suffix of the 1,500-slot price
table unreachable under the independent 5,000-unit lifetime cap.

Locked properties of the function and its evaluation:

- **Cost never decreases.** The function is nondecreasing in its
  argument, and the argument (the counter `M`) never decreases:
  extinguishing FN does not reduce `M` and does not lower the cost.
- **Historical FN claims do not advance the curve.** Their owners
  already paid through historical disintegration; only modern creations
  increment `M` (§11.1 transitions: a historical claim increments `H`,
  never `M`).
- **Multiple modern creations in one block use serialized block
  transaction order.** Each creation transaction's `D` is calculated
  using the **current** `M`; after each successful creation, `M` is
  incremented **before the next creation transaction is validated** —
  within one block and across blocks alike.
- **A transaction paying the previous tranche's price after the boundary
  has been crossed is rejected** (its `D` is evaluated at its own
  position on the curve; the §10.1 subtraction-first validation fails if
  it paid for a cheaper slot).
- **Integer atomic-unit arithmetic only** — the §10.1 discipline applies
  to the curve's definition and evaluation: raw atomic `CAmount` units,
  no floating point; human-facing modern prices may read in modern B3
  (= kB3, the locked denomination model), historical tiers remain in
  legacy B3 and are never reinterpreted; validation never uses
  display-unit arithmetic.

### 11.3 Market effect — intended economic behavior

Recorded accurately, with no stronger claims than the mechanism supports:

- Increasing creation cost raises the **marginal price of new FN
  supply**.
- The hard cap prevents **unlimited dilution**.
- Existing FN can trade **below or above** the current creation cost,
  depending on demand, liquidity, provenance and expected perks — the
  curve sets the cost of *new* supply, not the price of existing supply.
- Reward eligibility creates **utility but does not guarantee market
  value**.
- Unique PoDId history may give individual FN Coins **different market
  valuations** (provenance is on-chain and per-coin).
- Scarcity is based on **total-ever-created** supply, so intentional FN
  destruction (§10.2 extinguishment) makes the active market **more
  scarce** — without ever reopening a creation slot (§11.1).

### 11.4 Legacy protection — legacy FN rights come first

- Derive the **complete eligible historical count through H** (the §8.2
  PodRecord set; the real-chain `-podreport` run is the authoritative
  count).
- **Reserve one FN right for every qualifying historical PoD** before
  modern creation opens.
- Legacy claimants **do not pay the modern creation cost** — no
  `RequiredDisintegration` applies to a legacy claim; the price was paid
  historically.
- Modern issuance must **never consume a reserved legacy slot**
  (`modern_capacity_remaining` excludes the full reservation, claimed or
  not).
- **Unclaimed legacy rights remain reserved perpetually** unless the
  owner later establishes an explicit expiry policy (OPEN, §11.5 D-7 —
  nothing expires by default).

### 11.5 Numerical decisions — selected and open

D-1 through D-4 are selected. D-5 through D-7 remain OPEN.

**D-1: `MAX_FN_EVER_CREATED` — SELECTED: 5,000 (owner, 2026-08-22; supersedes 1,000 of 2026-08-17)**

| Item | Status |
|---|---|
| Current owner-selected value | **5,000** (2026-08-22) — includes every qualifying historical right (measured floor R = 3,500 at height 807,709) and every modern FN ever created; modern headroom ceiling 1,500, finalized by the through-H report |
| Activation gate | height-807,709 report proves `R >= 3,500`; the mandatory through-H report fixes final R before FN activation; all final rights are reserved, so modern capacity is at most 1,500 and may be smaller; `R > 5,000` returns the cap decision to the owner; not a block-validation halt |
| Revision before activation | permitted, through the reviewed protocol process only |
| Revision after activation | only by an explicit versioned consensus upgrade at a defined activation height; never node-local or independently configurable |
| FlowMesh sizing | separate from issuance capacity; the seat lifecycle remains governed by the FlowMesh decision record |

**D-2 through D-4 — SELECTED (owner, 2026-08-28):** explicit 500-slot
tiers costing 15,000, 30,000 and 60,000 B3 respectively, exactly as
§11.2 and `modern::RequiredDisintegration` define. There is no rounding
rule or floating-point evaluation.

**D-5: reward amount and schedule**

| Consideration | Constraint / note |
|---|---|
| Source | FlowMesh fee flows (handoff §4.8) — distribution OPEN there as well; these decisions should land together |
| Guarantee level | rewards are utility, not guaranteed value (§11.3) — the schedule must not promise yield |

**D-6: reward ownership cutoff on transfer / extinguishment**

| Consideration | Constraint / note |
|---|---|
| Transfer | rewards/perks follow the FN to the new owner (§10.2) — the cutoff moment (block height of transfer? epoch boundary?) is the open detail |
| Extinguishment | rewards/perks end permanently (§10.2) — the cutoff moment is the open detail |
| Determinism | whatever the cutoff, it must be a pure function of chain state |

**D-7: legacy claim expiry policy** — none exists; perpetual reservation
unless the owner later establishes one explicitly (§11.4).

### 11.6 Future implementation tests — the cap (owner-required, 2026-08-17)

Implementation commits for the cap must cover, in addition to the §8.6
matrix:

| Case | Expected |
|---|---|
| Historical reservation | final through-H `R` is enforced; modern capacity = `5,000 − R` and is at most 1,500 |
| At modern counter `M = 1,499` when `R = 3,500` | one additional modern creation succeeds (`M` reaches 1,500) |
| At modern counter `M = 1,500` | every further modern creation is rejected permanently |
| Additional through-H rights | reduce reachable modern slots one-for-one; no historical right is discarded |
| Extinguishment | does not decrease `R + M`; at the cap, capacity remains 0 — no slot reopens |
| Reorganization | reverses canonical-chain modern creations and restores capacity exactly; re-connection reproduces the counters identically |

The general invariant is `R + M <= 5,000`; the pinned price table is also
bounded by `M <= 1,500`.
