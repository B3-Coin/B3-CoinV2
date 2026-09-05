# B3 Cross-Chain Finality Protocol — v1 (normative)

**Status: FINAL protocol specification, amended 2026-08-23 (revision 2) with the owner's
architectural rulings of the same day: the finality objects are Modern **policy cells**
(`FINALITY_CERT = 6`, `FINALITY_KEY = 7`, frozen numbers) whose large evidence travels in
the **Modern Payload Area** ([b3-modern-payload-area.md](b3-modern-payload-area.md),
Path B, `MODERN_PAYLOAD_ROOT = 8`); the BLS binding is **identity-authorized** (BIP340 by
`validator_key` + separate PoP, sequence-controlled); validator-set rotation is
**handover-gated**; F = M in the X-pin Modern-PoS release. Layouts, hashes, verification
algorithms and state machines are frozen as `V1`; numeric parameters in §9 are **FROZEN by owner ruling 2026-08-23** except the
Ethereum-side `MAX_EPOCH_LAG` (separate bridge activation B). Rationale and attack analysis: [b3-finality-to-ethereum.md](b3-finality-to-ethereum.md);
compatibility audit: [b3-finality-compatibility-report.md](b3-finality-compatibility-report.md);
Modern PoS amendment: ruling M7 in [b3-modern-pos-spec.md](b3-modern-pos-spec.md).
Deposits and the bridge vault are outside this document. The V1 implementation
exists, but Ethereum deployment and mainnet bridge activation are not approved
until the production pins, gas bounds, and independent reviews are complete.**

**Pre-M deployment amendment (2026-09-02):** `A3` names FlowMesh activation
only. Inbound bridge activation `B` is a separate later-build B3-chainparams input,
selected only after the complete audited deployment tuple and all safety gates
are reviewed. No Solidity contract hardcodes that release choice.
Because canonical Set_0 is unknowable until block 811,000, the immutable
Ethereum contracts may be deployed earlier with a synthetic four-member BLS
bootstrap header (equal weights, quorum three) and an immutable deadline. At
M-1, any three bootstrap identities attest exactly
`FinalizedBlock{811000, block_hash, 0, hash(Set_0), 0}` using the ordinary V1
digest and proof ABI. `initialize` accepts that handoff once, installs Set_0,
and permanently removes the bootstrap path; all later authority is the normal
B3 stake/finality lineage. The B3 decentralized burn/withdrawal-root path is
implemented but remains fail-closed on mainnet until its release pins are
complete.

Conventions: all integers are **big-endian, fixed width**; `‖` is byte concatenation;
`keccak` = Keccak-256; `sha256` = SHA-256; `TaggedHash(tag, m) = sha256(sha256(tag) ‖
sha256(tag) ‖ m)` (BIP340 convention, already in-tree); `ModernChainDomain` is the 32-byte
B3 modern chain domain.

---

## 1. Keys — two keys, one validator, never derived from each other

| | Identity key | Consensus (finality) key |
|---|---|---|
| Algorithm | BIP340 x-only secp256k1, 32 B | BLS12-381 G1 compressed, 48 B |
| Name in code | `validator_key` | `bls_pubkey` |
| Lives in | STAKE carrier `B3S1` (ratified v1, **unchanged**), coinbase declaration, block signature (M3), binding authorization | `FINALITY_KEY` cell params; validator-set leaves |
| Signs | blocks (`B3/MODERN/POS/SIG/V1`), bindings (`B3/FINALITY/BIND/V1`) | **only** `B3/FINALITY/V1` digests (and its own PoP) |
| Rotation | re-stake (M4) | next `seq` in a new `FINALITY_KEY` cell; effective at the next snapshot |

Ciphersuite: `BLS_SIG_BLS12381G2_XMD:SHA-256_SSWU_RO_POP_`; PoP DST
`BLS_POP_BLS12381G2_XMD:SHA-256_SSWU_RO_POP_`; public keys G1 (48 B compressed),
signatures G2 (96 B compressed); point encodings per the IETF BLS draft (ZCash/Ethereum
serialization). A BLS key is **never** derived from the identity key and an identity key
never signs finality. `bls_pubkey` MUST NOT be the point at infinity. **Stake never elects a
BLS key**: a validator's finality key exists only through the validator's own signature.

### 1.1 `FINALITY_KEY` — validator-binding state (policy type 7, frozen)

```
cell:    policy_type = 7, policy_version = 1, amount = 0, asset = native
         commitment  = validator_key (32 B)                      // the identity the cell binds
         params      = bls_pubkey(48) ‖ seq u32 LE   (52 B ≤ 80)  // bls_pubkey = 0^48 means REVOKE
record:  MPA payload_type 5 `FINALITY_KEY_EVIDENCE`, version 1, 244 B exactly:
         validator_key(32) ‖ bls_pubkey(48) ‖ seq u32 ‖ bip340_sig(64) ‖ pop(96)
         bip340_sig = Schnorr_Sign(sk_identity,
               TaggedHash("B3/FINALITY/BIND/V1", ModernChainDomain ‖ validator_key ‖ bls_pubkey ‖ seq))
         pop        = BLS_Sign(sk_bls, bls_pubkey) under the PoP DST   (omitted/zero only when bls_pubkey = 0^48)
```

Rules (consensus from H+1, the corridor included, so `Set_0` can be non-empty at M):
exactly one record per cell and one cell per record in the same transaction
(`record.validator_key == cell.commitment`, `record.bls_pubkey ‖ seq == cell.params`);
`seq` must equal the validator's previous binding `seq + 1` (first binding `seq = 0`); a
non-zero `bls_pubkey` may be active under **one** `validator_key` at a time; `bip340_sig`
and `pop` verified at creation; the cell is a **metadata cell**: zero value, never added to
the UTXO set, no spend path. Derived consensus state `binding[validator_key] = (bls_pubkey,
seq, height)` (maintained on connect/disconnect, rebuilt on reindex, with its own undo).
Revocation = a binding with `bls_pubkey = 0^48`. **Effect:** a binding created in block `b`
is visible to snapshots at heights ≥ `b` (§4); it never changes a set already in force.
Stake contributes weight to a validator's finality membership **only** by resolving through
this validator-authorized active binding — never through any property of a STAKE output.

## 2. Validator set commitment — Merkle root, fixed depth

Ethereum stores **no member list**. A validator set is committed by:

```
SET_TREE_DEPTH = 13                             // 2^13 = 8,192 = MAX_FINALITY_SET
leaf_i   = keccak( u32 i ‖ bls_pubkey_i(48) ‖ u64 w_i )        // 60-byte preimage, i = 0..n-1
leaf_i   = 0x00…00 (32 zero bytes)                              // for n ≤ i < 8,192 (padding)
node     = keccak( left ‖ right )
members_root = root of the complete binary tree over the 8,192 leaves

ValidatorSetHeader (110 B = 8+2+4+8+8+48+32):
  u64  epoch
  u16  ruleset_version        // 1
  u32  validator_count        // n,  1 ≤ n ≤ 8,192
  u64  total_weight           // W = Σ w_i
  u64  quorum_weight          // ruleset 1: floor(2·W/3) + 1
  48 B aggregate_pubkey       // Σ_i bls_pubkey_i (G1 point addition, unweighted), compressed
  32 B members_root
validator_set_hash = keccak( ValidatorSetHeader )
```

Members are the bound validators with ACTIVE stake at the snapshot height, **sorted
ascending by `validator_key`** (bytes), `w_i = floor(active_stake_base_units_i / 10^9)`
(whole modern B3), members with `w_i = 0` dropped. B3 consensus computes the header; the
consistency `Σ leaves = total_weight`, `Σ pk = aggregate_pubkey`, `quorum_weight` by
rule are **B3 consensus rules**; Ethereum checks the rule for `quorum_weight` (§5.4) and
relies on the attesting quorum for the rest.

---

## 3. Finalized block, digest, certificate

```
FinalizedBlock (112 B = 8+32+32+32+8):
  u64  height
  32 B block_hash              // modern block hash at `height`
  32 B withdrawal_root         // §6; all-zero before inbound bridge height B
  32 B validator_set_hash      // keccak(header of Set_{epoch+1})  — the successor set, always
  u64  epoch                   // epoch of the signing set Set_epoch

finality_digest = TaggedHash("B3/FINALITY/V1", ModernChainDomain ‖ FinalizedBlock)   // 32 B
signature_i     = BLS_Sign(sk_bls_i, finality_digest)         // message = the 32-byte digest

Certificate:
  signer_bitmap   ⌈n/8⌉ bytes — bit i = (bitmap[i >> 3] >> (i & 7)) & 1  (LSB-first in byte);
                  bits ≥ n MUST be 0
  aggregate_sig   96 B = Σ_{i: bit i = 1} signature_i  (G2 addition)
```

---

### 3.1 `FINALITY_CERT` — block-consensus metadata (policy type 6, frozen)

```
cell:    policy_type = 6, policy_version = 1, amount = 0, asset = native, coinbase only, ≤ 1 per block
         commitment  = TaggedHash("B3/FINALITY/CERT/V1", record payload)   // hash of the full certificate payload
         params      = epoch u64 ‖ height u64 (16 B)                        // the FinalizedBlock's epoch and height, duplicated for cheap lookup
record:  MPA payload_type 4 `FINALITY_CERTIFICATE`, version 1, payload = FinalizedBlock(112) ‖ signer_bitmap(⌈n/8⌉) ‖ aggregate_sig(96)
         type-specific maximum: 112 + 1,024 + 96 = 1,232 B (n ≤ 8,192)
```

The cell is a metadata cell (zero value, never in the UTXO set, no spend path); the record
is bounded, priced historical payload data committed by the cell's commitment and by the
block through `MODERN_PAYLOAD_ROOT` (MPA §2–§4). A later ZK proof of the same statement is
a **new record type behind the identical cell** (§7).

> **Implementation note (2026-08-24).** Sections 2–4 are implemented on
> `test/b3-clean-architecture` (see the addendum in
> [b3-implementation-status.md](b3-implementation-status.md)). Node-local
> mechanics chosen by the implementation, not consensus: the finality pin is
> additionally **persisted** (`<blocksdir>/finality_pin.dat`, atomic and
> monotone, surviving restart/-reindex — the in-process pin was already
> sticky across carrier-removing reorgs); wallets derive the BLS consensus
> key deterministically from the validator identity key and the binding
> sequence (`TaggedHash("B3/FINALITY/BLSKEY/V1", identity_secret || seq)`),
> so no second keystore exists; transactions relay under the full-payload
> codec (`TX_MODERN`) so FINALITY_KEY evidence survives the wire; the
> signer-bitmap width of a certificate is resolved from the certificate's
> own epoch. Activation is the single predicate
> `Consensus::ModernObjectRulesActive` (H + X + Modern-PoS rule set pinned):
> the F = M rule with no separate switch.

## 4. B3 rules (normative)

| Rule | Value |
|---|---|
| Heights | **M** = first modern-PoS block, epoch 0 starts; **F = M** — the finality rules ship in the X-pin Modern-PoS release; inbound bridge activation **B ≥ F**; irreversible burn activation **W ≥ B** and fail-closed while unset |
| Epoch start | `epoch_start[0] = M`; at the first block of epoch `e`, `Set_{e+1} := Snapshot(epoch_start[e] − 1)` (so `Set_1 = Set_0 = Snapshot(M−1)`); `Set_{e+1}` is known for all of epoch `e` and every epoch-`e` certificate carries `hash(Set_{e+1})` |
| Snapshot(b) | every `validator_key` with ACTIVE STAKE weight `w > 0` at height `b` (STAKE v1, 20-block maturity, aggregation per key — unchanged) **and** a non-revoked `binding[validator_key]` at height `b` → member `(bls_pubkey, w)`; sorted by `validator_key`; `w` in whole modern B3. **One stake universe (FINAL, owner ruling 2026-08-23): from F (= M) a validator key without a non-revoked binding is NOT eligible to produce blocks** (M1 eligibility requires the binding), so the block-production weight `W` and the finality weight are the same quantity |
| **Handover-gated rotation** (**FINAL**, owner ruling 2026-08-23) | epoch `e+1` begins at the first height `h ≥ epoch_start[e] + E` such that the chain below `h` contains a valid certificate with `epoch = e` (it necessarily carries `hash(Set_{e+1})`). Until then epoch `e` **extends**: checkpoints continue, signed by `Set_e`, all with `epoch = e`. A set never signs before the previous set has attested it on-chain — B3 and the Ethereum verifier follow the identical `e → e+1` rule |
| Carry-over | if `Snapshot` yields `n < MIN_FINALITY_SET` or `W < MIN_FINALITY_WEIGHT`: `Set_{e+1} = Set_e` re-stamped with `epoch = e+1` |
| `MAX_EPOCH_EXTENSION` | an epoch extended beyond it (no quorum certificate at all) declares the lineage **broken**: no further certificate is valid until a consensus re-bootstrap pins a new genesis set (rule, not improvisation) |
| Checkpoints | heights `h` with `(h − M) mod CHECKPOINT_INTERVAL = 0` |
| Signing (validator behaviour) | after `tip − h ≥ CHECKPOINT_DEPTH`; one signature per height; strictly increasing heights; only descendants of the latest certified checkpoint known |
| Signer journal (validator behaviour) | durable per-(chain domain, validator key) record: `last_signed_*` (never sign the same or a lower height with a different object) plus an ancestry `lock_*` (never sign a higher checkpoint on a branch that does not descend from the lock). The lock moves ahead of the vote only by (a) a strictly newer quorum certificate of the exact same epoch and signing set included on the active chain, or (b) a **chain-pinned one-time recovery** (`Consensus::FinalitySignerRecovery`, compiled beside a hardened modern checkpoint): a journal holding exactly the pinned orphaned vote as both its last vote and its lock, under the pinned epoch and sets, and nothing newer, moves only its lock to the pinned anchor on the current chain after normal `CHECKPOINT_DEPTH`, while the pinned epoch is still inside the `{current, current − 1}` window with the exact pinned sets (whether or not a newer certificate of another epoch has meanwhile been included); the recorded vote is kept and the next signature must be strictly above the anchor, the old vote and the finalized checkpoint. Journals are never deleted or recreated; there is no operator unlock, option or timeout. Mainnet pins one incident: vote 811,631 (`86297c10…26be0`, epoch 0) → hardened anchor 811,641 |
| Certificate carrier | `FINALITY_CERT` cell + `FINALITY_CERTIFICATE` record in the **coinbase**, ≤ 1 per block (§3.1); the checkpoint must be an ancestor at that height on this chain; **epoch window `{current, current − 1}` (FINAL)** accepted only with: `FinalizedBlock.height` strictly greater than the highest certified height (monotone — an old-set certificate can never overwrite newer finalized state), the certificate's `validator_set_hash` equal to `hash(Set_{epoch+1})` as derived on this chain, and the epoch relation `epoch(height) == FinalizedBlock.epoch` |
| Validation order | MPA frame/lengths → type activation → cell↔record binding → verification-cost budget and per-block counts → epoch window / ancestry → bitmap rules → quorum by weight **and headcount** → `FastAggregateVerify` last. Failure ⇒ `bad-finality-cert` / `bad-finality-cert-form`, block invalid |
| Finality pin (from F) | on connect of a block carrying a valid certificate for checkpoint `C`: `finalized_tip = C`; a reorganization that would disconnect `finalized_tip` is refused (`modern-finality-violation`, no peer penalty, skipped during reindex/import). Pins apply on the active chain only |
| No certificate | the chain continues under V1 (blocks never depend on certificates); the epoch extends; withdrawals wait; beyond `MAX_EPOCH_EXTENSION` → lineage broken (above) |
| Archive | every node keeps the highest certificate per epoch it has seen (included or not) and serves it by RPC |
| Transport | `finsig {u64 epoch, u64 height, u32 index, 96 B sig}`; relayed only for indices of `Set_epoch` / `Set_{epoch+1}`, one per (index, height) |
| Untouched | M1 eligibility/seed, M3 block signature and wire format, M4, M5 fork choice, reorg horizon, STAKE v1 carrier, `MAX_POLICY_PARAMS_SIZE = 80` |

## 5. Ethereum — `B3FinalityVerifier` exact verification

### 5.1 Storage

```solidity
bytes32 immutable CHAIN_DOMAIN;
bytes32 immutable BOOTSTRAP_SET_HASH; SetHeader BOOTSTRAP_SET; uint256 immutable BOOTSTRAP_DEADLINE;
bool initialized; uint256 GENESIS_TIME; bytes32 GENESIS_SET_HASH;
uint64  currentEpoch;     bytes32 currentSetHash;   SetHeader currentSet;
bytes32 nextSetHash;      SetHeader nextSet;         // zero until learned
mapping(uint64 => bytes32) setHashByEpoch;            // append-only
FinalizedBlock latest;                                 // height strictly increasing
uint256 lastCertificateTime;                           // live-finality signal; never set lifetime
uint256 lastRotationTime;
uint32 immutable MIN_BRIDGE_VALIDATORS; uint32 immutable MAX_BRIDGE_VALIDATORS;
uint256 immutable MIN_EPOCH_DURATION; uint256 immutable MAX_EPOCH_LAG;
uint256 immutable MAX_CERTIFICATE_AGE;
uint256 immutable MIN_DEPOSIT_EXIT_WINDOW;
IB3FinalityProver prover;
```

Constructor: `CHAIN_DOMAIN`, the exact synthetic four-key `BOOTSTRAP_SET`
(`epoch=0`, `n=4`, `W=4`, `quorum=3`), its hash, proof runtime/hash, B/M and
security windows. It leaves `currentSetHash = 0`, `initialized = false` and
`lastRotationTime = 0`. `BOOTSTRAP_DEADLINE` must be after deployment but no
more than `MAX_EPOCH_LAG` later.

`initialize(snapshot, genesisSet, proof)` requires: not initialized; deadline
not passed; `snapshot = {M−1, nonzero block_hash, 0,
keccak(genesisSet), epoch 0}`; a structurally valid epoch-zero Set_0; and
`prover.verify(CHAIN_DOMAIN, snapshot, BOOTSTRAP_SET_HASH, BOOTSTRAP_SET,
proof)`. The pinned equal-weight/header-count rules make this exactly 3-of-4.
Success installs Set_0, records its hash, sets `latest=snapshot`, starts
`GENESIS_TIME=lastRotationTime=block.timestamp`, and irreversibly closes
`initialize`.

### 5.2 `submitCertificate(FinalizedBlock fb, SetHeader successor, bytes proof)`

```
1  require fb.epoch == currentEpoch
        || (fb.epoch == currentEpoch + 1 && nextSetHash != 0)            // EPOCH TRANSITION
2  require block.timestamp − lastRotationTime ≤ MAX_EPOCH_LAG
3  require GENESIS_TIME + fb.epoch * MIN_EPOCH_DURATION ≤ block.timestamp
        ≤ GENESIS_TIME + (fb.epoch + 1) * MAX_EPOCH_LAG
   if transition: require block.timestamp − lastRotationTime ≥ MIN_EPOCH_DURATION
4  select signingSet = currentSet, or precommitted nextSet for a transition
5  require fb.height > latest.height                                       // monotone finality
6  require successor.epoch == fb.epoch + 1
        && checkHeaderRules(successor)                                    // §5.4
        && keccak(encode(successor)) == fb.validatorSetHash
7  require prover.verify(CHAIN_DOMAIN, fb, signingSetHash, signingSet, proof)   // §5.3
8  if transition, install the verified precommitted nextSet as currentSet,
       increment currentEpoch, clear nextSet, and record setHashByEpoch
9  if nextSetHash == 0: nextSet = successor; nextSetHash = fb.validatorSetHash
   else require nextSetHash == fb.validatorSetHash                          // one successor per epoch
10 latest = fb; lastCertificateTime = block.timestamp
   if transition: lastRotationTime = block.timestamp
   emit Finalized(fb.epoch, fb.height, fb.blockHash, fb.withdrawalRoot)
```

**Epoch transition, exactly:** the verifier accepts certificates only from `Set_currentEpoch`
(step 1, first clause) or — once some certificate of `currentEpoch` has disclosed the
successor header (step 8) — from `Set_{currentEpoch+1}`, which performs the rotation
only *after* signature verification against that precommitted set. Epochs can never be
skipped: a certificate of epoch `e+2` is
rejected until one of `e+1` has rotated the verifier. Within an epoch any number of
checkpoints may be skipped. A carry-over epoch is an ordinary rotation (same members,
new `epoch` in the header, different hash).

The absolute window prevents an epoch from appearing before B3 could have
reached it and expires old epoch numbers. The relative minimum is independently
required: after a relay outage, several absolute lower bounds may already be
in the past, so without per-handover spacing a withheld lineage could batch-walk
them and repeatedly renew `lastRotationTime`. Honest catch-up consequently
takes one real minimum epoch per missed rotation. These wall-clock bounds fail
closed but do not eliminate PoS weak subjectivity if compromised historical
keys keep a competing lineage current in real time; live honest relaying and a
reviewed deployment checkpoint remain security assumptions.

`bridgeReady()` reports live finality only: both `currentSet` and the known,
attested `nextSet` meet the minimum stake/headcount thresholds and the immutable
`MAX_BRIDGE_VALIDATORS` ceiling, the newest certificate is no older than
`MAX_CERTIFICATE_AGE`, and more than `MIN_DEPOSIT_EXIT_WINDOW` remains before
`MAX_EPOCH_LAG`. The immutable vault uses this readiness for inbound deposits:
before initialization, without a fresh valid certificate, or while either set
is unqualified, deposits fail closed. Deployment before M creates no
corridor-deposit window. Irreversible burns use the separate B3 consensus
height `W >= B`. If B is enabled while W is unset, verified deposits can mint
bUSD after readiness but every withdrawal record is rejected; this custodial
waiting period must be disclosed. An accepted root alone is not outbound
activation.

### 5.3 `BlsCertificateProver.verify(domain, fb, setHash, set, proof)` — today's prover

```
proof = abi.encode(bytes bitmap,
                   bytes signature /*256-byte EIP-2537 G2*/,
                   bytes aggregatePubkey /*128-byte EIP-2537 G1*/,
                   Absent[] absent)
Absent = { uint32 index; bytes pubkey /*48-byte compressed G1*/;
           uint64 weight; bytes uncompressedPubkey /*128-byte EIP-2537 G1*/;
           bytes32[13] siblings /*leaf level first*/; }

A  SIGNER BITMAP
   n = set.validatorCount
   require bitmap.length == (n + 7) / 8
   require (n % 8 == 0) || (bitmap[n/8] >> (n % 8)) == 0          // high bits zero
   require absent indices strictly increasing, each < n, each with bit == 0
   require popcount(bitmap) + absent.length == n                    // bitmap ≡ complement(absent)
   require popcount(bitmap) >= floor(2n/3) + 1                       // bridge headcount quorum

B  VALIDATOR WEIGHTS (ordered membership paths; no member list stored)
   for each absent[k]:
       require compressed/uncompressed public-key encodings describe the same G1 point
       node = keccak(abi.encodePacked(absent[k].index, absent[k].pubkey, absent[k].weight))
       for level in 0..12:
           node = ((absent[k].index >> level) & 1) == 0
                ? keccak(node ‖ absent[k].siblings[level])
                : keccak(absent[k].siblings[level] ‖ node)
       require node == set.membersRoot
   absentWeight = Σ absent[k].weight                                   // u64 arithmetic in u256
   require absentWeight <= set.totalWeight

C  QUORUM
   signedWeight = set.totalWeight − absentWeight
   require signedWeight >= set.quorumWeight                            // quorumWeight validated by §5.4

D  AGGREGATE PUBLIC KEY (subtraction over the committed aggregate)
   require compressed/uncompressed aggregatePubkey encodings describe the same point
   require compressed(aggregatePubkey) == set.aggregatePubkey
   aggPk = aggregatePubkey
   for each absent[k]: aggPk = G1ADD(aggPk, NEG(absent[k].uncompressedPubkey)) // EIP-2537 G1ADD
   require aggPk != INFINITY

E  MESSAGE
   digest = sha256(sha256(TAG) ‖ sha256(TAG) ‖ domain ‖ encode(fb)),  TAG = "B3/FINALITY/V1"
   Hm     = hash_to_curve_G2(digest, DST)    // expand_message_xmd(sha256) → 2 × MAP_FP2_TO_G2 → G2ADD

F  SIGNATURE
   require BLS12_PAIRING_CHECK([ (aggPk, Hm), (NEG(G1_GENERATOR), sig) ]) == 1
   // the precompile performs on-curve/subgroup checks; B3's certificate carries
   // a canonical 96-byte compressed signature, while this Ethereum witness
   // supplies the equivalent 256-byte EIP-2537 point
   return true
```

Signer identities are fully public: `bitmap` is emitted in the `Finalized` event data for
accountability; the contract never learns or stores member lists. Verification cost and
calldata grow linearly with the number of absent validators, so a target-chain gas bound
is a production activation gate rather than an assumption hidden in the relayer. V1 pins
`MAX_BRIDGE_VALIDATORS=64`. Its post-Fusaka Osaka vector uses 64 distinct PoP-verified keys,
43 signers and 21 absent paths: 18,144 proof bytes, 18,724 submit calldata bytes,
5,513,351 measured gas for the complete successful verifier call, and a
6,283,311 conservative total after charging all calldata at 40 gas/byte. This is below
EIP-7825's 16,777,216 per-transaction cap. Larger sets remain structurally valid
lineage headers but cannot make `bridgeReady()` true in this deployment.

### 5.4 `checkHeaderRules(h)` — enforced on every successor header and on genesis

```
require h.rulesetVersion == 1                                  // unknown ruleset ⇒ reject (fail closed; a new
                                                                //   ruleset needs a verifier upgrade)
require 1 <= h.validatorCount && h.validatorCount <= 8192
require h.totalWeight > 0
require h.quorumWeight == (2 * h.totalWeight) / 3 + 1           // ruleset 1, integer division
require h.aggregatePubkey != INFINITY (48-byte encoding with the infinity flag unset)
```

### 5.5 Read API

`latest()`, `currentSetHash()`, `setHashByEpoch(e)`, `isFinalized(height, hash)` (= `latest.height ≥ height` is
**not** sufficient — the verifier only attests `latest`; historical (height, hash) pairs are
proven by the B3 side via the checkpoint chain, out of scope for the vault, which needs
only `latest.withdrawalRoot`).

---

## 6. Withdrawal root — exact

This section freezes the implemented data path; it does not enable it.
Withdrawals and B3 burns remain disabled while outbound height W is unset. W
may be pinned only after round-trip canonicality and liveness safety is solved,
audited, rehearsed, and explicitly enabled by a later B3 build.

Once enabled, one cumulative tree covers all `BRIDGE_BURN` records on B3:

Before admitting a burn, B3 projects finality from the parent state to the
candidate height. Both projected current and next validator sets must exist,
have validator counts within the deployment-pinned inclusive minimum/maximum,
meet the deployment-pinned minimum total weight, and have an unbroken finality
lineage. Candidate-block stake changes and the withdrawal root produced by the
burn cannot satisfy this gate; they take effect only through later projected
state. Ethereum wall-clock freshness remains an additional wallet/relayer
preflight because it is not a deterministic B3 consensus input.

```
WITHDRAWAL_TREE_DEPTH = 32
leaf(w) = keccak( u64 withdrawal_id ‖ u64 origin_chain_id ‖ 32 B asset_id ‖ 20 B origin_token
                 ‖ 20 B recipient ‖ u256 amount ‖ u64 b3_height )          // 128-byte preimage
index   = withdrawal_id  (0-based, strictly sequential per burn in block/tx order)
Z_0 = 0x00…00;  Z_{k+1} = keccak(Z_k ‖ Z_k)                                  // zero hashes
incremental (deposit-contract) insertion; root after the last burn at height h
   = FinalizedBlock.withdrawal_root at h
```

Ethereum (`B3Bridge.release(w, bytes32[32] path)`):

```
require w.origin_chain_id == block.chainid
node = leaf(w); idx = w.withdrawal_id
for k in 0..31: node = (idx >> k) & 1 == 0 ? keccak(node ‖ path[k]) : keccak(path[k] ‖ node)
require node == verifier.latest().withdrawalRoot
require !released[w.withdrawal_id]; released[w.withdrawal_id] = true
require tokenOf[w.asset_id] == w.origin_token; transfer(w.origin_token, w.recipient, w.amount)
```

The tree is cumulative, so any burn proves against any later root; the verifier stores
only the latest root. Release requires exactly: an accepted finality certificate (§5.2)
whose `withdrawal_root` includes the burn (§6).

---

## 7. ZK seam (structures frozen; only the prover changes)

**Owner ruling 2026-08-23: ZK is deferred ("we will do it later").** v1 ships only
`BlsCertificateProver`; no circuit, prover infrastructure, or ZK-specific testing is in
scope. The interface below is kept exactly so the later swap changes only the `prover`
slot — no B3 rule, layout, or verifier state-machine change.

```solidity
interface IB3FinalityProver {
  function verify(bytes32 chainDomain, FinalizedBlock calldata fb,
                  bytes32 setHash, SetHeader calldata set, bytes calldata proof)
           external view returns (bool);
}
```

`ZkFinalityProver.verify` accepts `proof` = a SNARK/STARK whose public inputs are
`(chainDomain, keccak(encode(fb)), setHash)` and whose statement is exactly §5.3 A–F
against the set committed by `setHash`. B3 signing, headers, bitmaps and the verifier
state machine (§5.2) are unchanged. The deployed verifier pins its prover and runtime
hash immutably, so adopting a ZK backend requires a separately reviewed verifier/vault
deployment and explicit asset migration; it is not an in-place governance swap.

---

## 8. Required test vectors (both implementations must pass the same files)

1. BLS: Ethereum consensus-spec-tests (sign, verify, aggregate, fast_aggregate_verify,
   PoP) — `blst` in-tree and the Solidity `hash_to_curve_G2` + pairing path.
2. `finality_digest` for a fixed `FinalizedBlock` and domain (hex preimage + digest).
3. `ValidatorSetHeader` hash and `members_root` for n ∈ {1, 4, 5, 8,192}, including
   zero-leaf padding and ordered depth-13 paths for absentees at indices
   {0, n−1, middle}; reversing any left/right step must fail.
4. Bitmap cases: n mod 8 ∈ {0, 1, 7}; high-bit violation; popcount mismatch.
5. Quorum boundary: `signedWeight = quorumWeight` (accept) and `−1` (reject).
6. Epoch transition: e→e+1 accept, e→e+2 reject, successor-hash mismatch reject,
   carry-over header accept, `MAX_EPOCH_LAG` exceeded reject.
7. Withdrawal tree: first leaf, leaf 2^32−1 boundary proof, stale-but-valid proof against
   a newer root (accept), double release (reject), wrong chain id (reject).
8. Bootstrap handoff: four PoP-verified identities, canonical validator-key ordering,
   bitmap `0x0b` (members 0, 1 and 3 sign), one ordered absent-member path, aggregate
   signature/public-key witnesses, exact proof ABI, and full `initialize` calldata must
   byte-match between C++ and Solidity.
9. Maximum bridge gas: 64 distinct PoP-verified members, exactly 43 signers,
   21 ordered absent paths, fixture checksum, exact submit calldata size,
   post-Fusaka Osaka prover gas, and a conservative total below the EIP-7825 transaction
   cap.

---

## 9. Parameters

| Name | Value | Status |
|---|---|---|
| `SET_TREE_DEPTH` / `MAX_FINALITY_SET` | 13 / 8,192 | FINAL (layout) |
| `WITHDRAWAL_TREE_DEPTH` | 32 | FINAL (layout) |
| `ruleset_version` 1 quorum | `floor(2W/3) + 1` by weight | FINAL |
| Weight unit | whole modern B3 (`/10^9`) | FINAL |
| `E` (epoch blocks) | **1,440** | **FINAL (owner ruling 2026-08-23)** |
| `CHECKPOINT_INTERVAL` / `CHECKPOINT_DEPTH` | **10 / 12** | **FINAL (owner ruling 2026-08-23)** |
| `MIN_FINALITY_SET` / `MAX_EPOCH_EXTENSION` | **2 / 7·E = 10,080** | **UPDATED 2026-09-01** — two real corridor stakers may bootstrap the chain; this is NOT a bridge security threshold |
| Finality signer quorum on B3 and Ethereum | `floor(2W/3) + 1` by weight **and** `floor(2n/3) + 1` by headcount | **ALIGNED 2026-09-02** — both consensus layers enforce the identical dual quorum; 2 members require 2 signatures, 4 require 3, and one high-weight validator cannot certify or release alone |
| `MIN_FINALITY_WEIGHT` | none in V1 (the floor is `MIN_FINALITY_SET`; stake economics are the min-stake rule) | FINAL (no separate weight floor) |
| `FINALITY_CERTIFICATE` `verify_cost` / `FINALITY_KEY_EVIDENCE` `verify_cost` | **2,000 / 700** units | **FINAL** |
| `MAX_BLOCK_PAYLOAD_COST` / `MAX_TX_PAYLOAD_COST` / `COST_TO_VBYTES` | **120,000 / 12,000 / 1** | **FINAL** |
| MPA record max / section max / weight factor | **32,768 B / 65,536 B / ×4** | **FINAL** (MPA doc §8–§10) |
| Certificate epoch window | `{current, current − 1}` with the §4 monotonicity/set-hash/epoch-relation conditions | FINAL |
| Policy numbers 6 / 7 / 8 | `FINALITY_CERT` / `FINALITY_KEY` / `MODERN_PAYLOAD_ROOT` | **FINAL, never renumbered** |
| `FINALITY_CERTIFICATE` record max / `FINALITY_KEY_EVIDENCE` size | 1,232 B / 244 B | FINAL (layout) |
| `MAX_EPOCH_LAG` (Ethereum verifier) | 30 days | proposed — bridge activation B decision, out of scope for Modern PoS V1 |
| `MIN_EPOCH_DURATION` (Ethereum verifier) | 86,400 seconds | deployment pin matching the 1,440 one-minute-block epoch; absolute epoch window plus minimum spacing between accepted rotations |
| `MAX_BRIDGE_VALIDATORS` (Ethereum release authority) | 64 | Post-Fusaka Osaka benchmark cap; larger current/successor sets close `bridgeReady`, without changing B3's 8,192 consensus layout cap |
| `MAX_CERTIFICATE_AGE` / `MIN_DEPOSIT_EXIT_WINDOW` | 1 day / 7 days | proposed deployment pins for live-finality readiness; deposits fail closed when readiness is absent |
| `F` | **= M**, in the X-pin Modern-PoS release | FINAL |
| Gated rotation; epoch extension on failure | rule of §4 | FINAL |
| Binding mandatory for block eligibility from F | yes | FINAL |
| `FINALITY_KEY` semantics (seq-controlled, `0^48` = revoke, one active validator per BLS key) | §1.1 | FINAL |
| `B` (inbound bridge activation) | later B3 build only | pending complete audited deployment pins and inbound safety review; never implied by A3 |
| `W` (irreversible B3 burn activation) | unset | later reviewed release only after canonicality/liveness safety; must be at least B |
| Binding required for block eligibility from F | yes | FINAL |
