# B3 Cross-Chain Finality Protocol — v1 (normative)

**Status: FINAL protocol specification (2026-08-23), owner direction accepted. Layouts,
hashes, verification algorithms and state machines below are frozen as `V1`; the
numeric parameters in §9 carry their own status. Rationale, attack analysis and
alternatives live in [b3-finality-to-ethereum.md](b3-finality-to-ethereum.md); the
Modern PoS amendment is ruling M7 in [b3-modern-pos-spec.md](b3-modern-pos-spec.md).
Deposits and the bridge vault are outside this document (see
[b3-bridge-bls-proposal.md](b3-bridge-bls-proposal.md)); this document defines only how
B3 finality is produced and how Ethereum verifies it. Implementation not yet authorized
("before implementation").**

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
| Lives in | STAKE carrier `B3S1` (ratified v1), coinbase declaration, block signature (M3), binding authorization | `VALIDATOR_BLS_BINDING` action; validator-set leaves |
| Signs | blocks (`B3/MODERN/POS/SIG/V1`), bindings (`B3/FINALITY/BIND/V1`) | **only** `B3/FINALITY/V1` digests (and its own PoP) |
| Rotation | re-stake (M4) | rebind; effective at next snapshot |

Ciphersuite: `BLS_SIG_BLS12381G2_XMD:SHA-256_SSWU_RO_POP_`; PoP DST
`BLS_POP_BLS12381G2_XMD:SHA-256_SSWU_RO_POP_`; public keys G1 (48 B compressed),
signatures G2 (96 B compressed); point encodings per the IETF BLS draft (ZCash/Ethereum
serialization). A BLS key is **never** derived from the identity key and an identity key
never signs finality. `bls_pubkey` MUST NOT be the point at infinity.

**Binding** — creation action type **5**, version **1**, payload 240 B exactly:

```
validator_key 32 ‖ bls_pubkey 48 ‖ pop 96 ‖ bip340_sig 64
pop        = BLS_Sign(sk_bls, bls_pubkey)                                  under the PoP DST
bip340_sig = Schnorr_Sign(sk_identity,
               TaggedHash("B3/FINALITY/BIND/V1", ModernChainDomain ‖ validator_key ‖ bls_pubkey))
```

Valid in any modern-era block from H+1. Consensus state `binding[validator_key] =
(bls_pubkey, height)`, latest wins; a `bls_pubkey` may be bound by one `validator_key` at
a time (a second binding of the same BLS key while the first stands is invalid); undo on
disconnect; rebuilt on reindex.

---

## 2. Validator set commitment — Merkle root, fixed depth

Ethereum stores **no member list**. A validator set is committed by:

```
SET_TREE_DEPTH = 13                             // 2^13 = 8,192 = MAX_FINALITY_SET
leaf_i   = keccak( u32 i ‖ bls_pubkey_i(48) ‖ u64 w_i )        // 60-byte preimage, i = 0..n-1
leaf_i   = 0x00…00 (32 zero bytes)                              // for n ≤ i < 8,192 (padding)
node     = keccak( left ‖ right )
members_root = root of the complete binary tree over the 8,192 leaves

ValidatorSetHeader (126 B):
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
FinalizedBlock (120 B):
  u64  height
  32 B block_hash              // modern block hash at `height`
  32 B withdrawal_root         // §6; all-zero before bridge activation A3
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

## 4. B3 rules (normative summary; full text in the design record)

| Rule | Value |
|---|---|
| Epoch `e` | heights `[M + e·E, M + (e+1)·E)`, `E` per §9 |
| Snapshot | `Set_{e+1} = Snapshot(last block of epoch e−1)` for `e ≥ 1`; `Set_0 = Set_1 = Snapshot(M−1)` |
| Carry-over | if `n < MIN_FINALITY_SET` or `W < MIN_FINALITY_WEIGHT`: `Set_{e+1} = Set_e` re-stamped with `epoch = e+1`; more than `MAX_CARRY_OVER` consecutive carry-overs ⇒ lineage broken (no certificate valid until a consensus re-bootstrap) |
| Checkpoints | heights `h` with `(h − M) mod CHECKPOINT_INTERVAL = 0`, plus the last block of every epoch |
| Signing (validator) | after `tip − h ≥ CHECKPOINT_DEPTH`; one signature per height; strictly increasing heights; only descendants of the latest certified checkpoint known |
| Certificate carrier | creation action type **4**, version **1**, payload `FinalizedBlock ‖ Certificate`, **coinbase only, ≤ 1 per block**, checkpoint must be an ancestor and `epoch ≤ current epoch` |
| From M | syntactic validity of the action (placement, lengths, bitmap width, zero high bits) is consensus |
| From F | `signed_weight ≥ quorum_weight` and `FastAggregateVerify` are consensus; **finality pin**: a reorganization disconnecting the highest certified checkpoint is refused (`modern-finality-violation`) |
| Archive | every node keeps the highest certificate per epoch it has seen (included or not) and serves it by RPC |
| Transport | `finsig {u64 epoch, u64 height, u32 index, 96 B sig}`; relayed only for indices of `Set_epoch` / `Set_{epoch+1}`, one per (index, height) |

---

## 5. Ethereum — `B3FinalityVerifier` exact verification

### 5.1 Storage

```solidity
bytes32 immutable CHAIN_DOMAIN;
bytes32 immutable GENESIS_SET_HASH;  SetHeader GENESIS_SET;  uint64 immutable GENESIS_EPOCH; // = 0
uint64  currentEpoch;     bytes32 currentSetHash;   SetHeader currentSet;
bytes32 nextSetHash;      SetHeader nextSet;         // zero until learned
mapping(uint64 => bytes32) setHashByEpoch;            // append-only
FinalizedBlock latest;                                 // height strictly increasing
uint256 lastRotationTime;
IB3FinalityProver prover;
```

Constructor: `CHAIN_DOMAIN`, `GENESIS_SET = header of Set_0` (must have `epoch == 0`,
`ruleset_version == 1`, pass §5.4 header rules), `currentEpoch = 0`,
`currentSetHash = setHashByEpoch[0] = keccak(GENESIS_SET)`, `latest = {M−1, 0, 0, 0, 0}`,
`lastRotationTime = block.timestamp`, `prover = BlsCertificateProver`.

### 5.2 `submitCertificate(FinalizedBlock fb, SetHeader successor, bytes proof)`

```
1  require fb.epoch == currentEpoch
        || (fb.epoch == currentEpoch + 1 && nextSetHash != 0)            // EPOCH TRANSITION
2  if fb.epoch == currentEpoch + 1:
        currentSet = nextSet; currentSetHash = nextSetHash; currentEpoch += 1
        nextSet = ∅; nextSetHash = 0
        setHashByEpoch[currentEpoch] = currentSetHash
        require block.timestamp − lastRotationTime ≤ MAX_EPOCH_LAG;  lastRotationTime = block.timestamp
3  require fb.height > latest.height                                       // monotone finality
4  require successor.epoch == fb.epoch + 1
        && checkHeaderRules(successor)                                    // §5.4
        && keccak(encode(successor)) == fb.validatorSetHash
5  require prover.verify(CHAIN_DOMAIN, fb, currentSetHash, currentSet, proof)   // §5.3
6  if nextSetHash == 0: nextSet = successor; nextSetHash = fb.validatorSetHash
   else require nextSetHash == fb.validatorSetHash                          // one successor per epoch
7  latest = fb;  emit Finalized(fb.epoch, fb.height, fb.blockHash, fb.withdrawalRoot)
```

**Epoch transition, exactly:** the verifier accepts certificates only from `Set_currentEpoch`
(step 1, first clause) or — once some certificate of `currentEpoch` has disclosed the
successor header (step 6) — from `Set_{currentEpoch+1}`, which performs the rotation
(step 2) *before* signature verification, so the certificate is checked against the set
that actually signed it. Epochs can never be skipped: a certificate of epoch `e+2` is
rejected until one of `e+1` has rotated the verifier. Within an epoch any number of
checkpoints may be skipped. A carry-over epoch is an ordinary rotation (same members,
new `epoch` in the header, different hash).

### 5.3 `BlsCertificateProver.verify(domain, fb, setHash, set, proof)` — today's prover

```
proof = abi.encode(bytes bitmap, bytes sig /*96*/, Absent[] absent, bytes32[] multiproof, bool[] flags)
Absent = { uint32 index; bytes pubkey /*48*/; uint64 weight; }

A  SIGNER BITMAP
   n = set.validatorCount
   require bitmap.length == (n + 7) / 8
   require (n % 8 == 0) || (bitmap[n/8] >> (n % 8)) == 0          // high bits zero
   require absent indices strictly increasing, each < n, each with bit == 0
   require popcount(bitmap) + absent.length == n                    // bitmap ≡ complement(absent)

B  VALIDATOR WEIGHTS (membership + weights via the Merkle root; no member list stored)
   leaves[k] = keccak(abi.encodePacked(absent[k].index, absent[k].pubkey, absent[k].weight))
   require MultiProofVerify(multiproof, flags, set.membersRoot, leaves)   // OpenZeppelin multiProofVerify
                                                                             // semantics, leaves in
                                                                             // ascending index order,
                                                                             // depth SET_TREE_DEPTH
   absentWeight = Σ absent[k].weight                                   // u64 arithmetic in u256
   require absentWeight <= set.totalWeight

C  QUORUM
   signedWeight = set.totalWeight − absentWeight
   require signedWeight >= set.quorumWeight                            // quorumWeight validated by §5.4

D  AGGREGATE PUBLIC KEY (subtraction over the committed aggregate)
   aggPk = set.aggregatePubkey
   for each absent[k]: aggPk = G1ADD(aggPk, NEG(absent[k].pubkey))       // EIP-2537 BLS12_G1ADD
   require aggPk != INFINITY

E  MESSAGE
   digest = sha256(sha256(TAG) ‖ sha256(TAG) ‖ domain ‖ encode(fb)),  TAG = "B3/FINALITY/V1"
   Hm     = hash_to_curve_G2(digest, DST)    // expand_message_xmd(sha256) → 2 × MAP_FP2_TO_G2 → G2ADD

F  SIGNATURE
   require BLS12_PAIRING_CHECK([ (aggPk, Hm), (NEG(G1_GENERATOR), sig) ]) == 1
   // precompile performs on-curve and subgroup checks on every input point; sig decoded
   // from 96-byte compressed G2 in-contract (decompression) or supplied uncompressed
   // with an equality check against the compressed bytes — implementation choice,
   // the compressed 96 bytes are the canonical Certificate field
   return true
```

Signer identities are fully public: `bitmap` is emitted in the `Finalized` event data for
accountability; the contract never learns or stores member lists.

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

One cumulative tree for all `BRIDGE_BURN` records on B3 (from A3):

```
WITHDRAWAL_TREE_DEPTH = 32
leaf(w) = keccak( u64 withdrawal_id ‖ u64 origin_chain_id ‖ 32 B asset_id ‖ 20 B origin_token
                 ‖ 20 B recipient ‖ u256 amount ‖ u64 b3_height )          // 164-byte preimage
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
state machine (§5.2) are unchanged; `prover` is the single governance-changeable slot.

---

## 8. Required test vectors (both implementations must pass the same files)

1. BLS: Ethereum consensus-spec-tests (sign, verify, aggregate, fast_aggregate_verify,
   PoP) — `blst` in-tree and the Solidity `hash_to_curve_G2` + pairing path.
2. `finality_digest` for a fixed `FinalizedBlock` and domain (hex preimage + digest).
3. `ValidatorSetHeader` hash and `members_root` for n ∈ {1, 4, 5, 8,192}, including
   zero-leaf padding and a multiproof for absentees at indices {0, n−1, middle}.
4. Bitmap cases: n mod 8 ∈ {0, 1, 7}; high-bit violation; popcount mismatch.
5. Quorum boundary: `signedWeight = quorumWeight` (accept) and `−1` (reject).
6. Epoch transition: e→e+1 accept, e→e+2 reject, successor-hash mismatch reject,
   carry-over header accept, `MAX_EPOCH_LAG` exceeded reject.
7. Withdrawal tree: first leaf, leaf 2^32−1 boundary proof, stale-but-valid proof against
   a newer root (accept), double release (reject), wrong chain id (reject).

---

## 9. Parameters

| Name | Value | Status |
|---|---|---|
| `SET_TREE_DEPTH` / `MAX_FINALITY_SET` | 13 / 8,192 | FINAL (layout) |
| `WITHDRAWAL_TREE_DEPTH` | 32 | FINAL (layout) |
| `ruleset_version` 1 quorum | `floor(2W/3) + 1` by weight | FINAL |
| Weight unit | whole modern B3 (`/10^9`) | FINAL |
| `E` (epoch blocks) | 1,440 | proposed |
| `CHECKPOINT_INTERVAL` / `CHECKPOINT_DEPTH` | 60 / 20 | proposed |
| `MIN_FINALITY_SET` / `MIN_FINALITY_WEIGHT` / `MAX_CARRY_OVER` | 4 / owner / 7 | proposed |
| `MAX_EPOCH_LAG` | 30 days | proposed |
| `F` | M (target) or pinned by the X-pin follow-up release | **owner** |
| `A3` | later | owner |
| Binding required for block eligibility from F | yes | proposed |
