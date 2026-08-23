# The Modern Payload Area (MPA) — native carrier for large evidence bytes (design, revision 2)

**Status: design record, revision 2 (2026-08-23). Owner rulings applied: MPA concept
accepted; the permanent `policy_params ≤ 80 B` invariant stays; commitment path is the
B3-native **Path B** (`MODERN_PAYLOAD_ROOT` coinbase cell) — **not** the SegWit/witness-
reserved-value path, and SegWit is **not** a dependency of the MPA (its activation is a
separate modern-era audit question, untouched here); a full-payload transaction identifier
is specified without SegWit; the resource numbers are **not frozen** — worst-case effects
are reported first (§6). Revision 1's Path A text is withdrawn. The protocol amendment
(§7) is applied only after the non-circularity/determinism proof (§4). No implementation.**

Model lock (owner, 2026-08-23): `policy_params ≤ 80 B` = small typed live/derived state,
permanent; large evidence (BLS certificates, bridge / Merkle / ZK proofs) is bounded,
priced **historical** payload data committed to by the policy object — never policy state,
never an arbitrary-data policy, never solved by raising `MAX_POLICY_PARAMS_SIZE`.

---

## 1. Carrier format (transaction level)

```
version        int32
marker         0x00                     // BIP144 marker byte
flag           0x02                     // bit1 = Modern Payload Area present (bit0 = witness, if ever enabled; 0x00 never written)
vin            vector<CTxIn>
vout           vector<CTxOut>           // policy cells (32-B commitment, ≤ 80-B params)
[witness]      only if flag & 0x01      // out of scope here; not required
mpa            PayloadSection           // only if flag & 0x02
lockTime       uint32
```

`PayloadSection` = the existing strict creation-action section codec, byte-identical:
`count (CompactSize, 1..MAX_PAYLOAD_RECORDS_PER_TX)` then `count × PayloadRecord
{payload_type u16 LE, payload_version u16 LE, payload (CompactSize-prefixed bytes)}`.
Parsed only in the modern-era decode context; `flag & ~0x03` invalid; `flag & 0x02` with an
empty section invalid; unknown `(type, version)` invalid; every record must bind to exactly
one policy cell in the same transaction per its type's grammar (and every payload-backed
cell to exactly one record). Finality records: type 4 `FINALITY_CERTIFICATE` (bound to the
`FINALITY_CERT` cell by `commitment == TaggedHash("B3/FINALITY/CERT/V1", payload)`, coinbase
only, ≤ 1 per block) and type 5 `FINALITY_KEY_EVIDENCE` (bound to the `FINALITY_KEY` cell by
`commitment == validator_key`, `params == bls_pubkey ‖ seq`).

---

## 2. Path B — the `MODERN_PAYLOAD_ROOT` cell

| Field | Value |
|---|---|
| Policy type | `MODERN_PAYLOAD_ROOT` (next free number after `FINALITY_CERT = 6`, `FINALITY_KEY = 7` → **8**; never renumbered) |
| Placement | **coinbase only**; exactly **one** such cell **iff** at least one transaction in the block carries an MPA; **zero** such cells otherwise. A second one, one in a non-coinbase transaction, or one in a block without any MPA ⇒ block invalid |
| `amount` | 0, native asset |
| `commitment` | `payload_root` (§3) |
| `params` | **empty** (`n` is `vtx.size()`, already committed by the block; nothing is duplicated) |
| UTXO | **never added** — metadata cell rule (same rule as `FINALITY_CERT` / `FINALITY_KEY`; precedent: the legacy zero-value marker skipped in Connect and Disconnect) |
| Relay | never (coinbase) |

---

## 3. Exact Merkle construction and ordering

```
for i in 0 .. n−1, n = block.vtx.size(), in block order (i = 0 is the coinbase):
  section_hash_i = TaggedHash("B3/MPA/SECTION/V1", canonical PayloadSection bytes of tx i)   if tx i has an MPA
                 = 0x00…00 (32 zero bytes)                                                   otherwise
  leaf_i         = TaggedHash("B3/MPA/LEAF/V1", uint32 i (LE, standard serializer) ‖ section_hash_i)

payload_root = ComputeMerkleRoot([leaf_0, …, leaf_{n−1}])     // exactly the algorithm of hashMerkleRoot
```

Properties:
- **One leaf per transaction, positional.** The leaf carries the position `i`, not the
  txid; the block's own `hashMerkleRoot` already binds position → txid. Swapping sections
  between transactions, inserting, deleting or reordering sections changes some `leaf_i`
  and therefore the root.
- **No duplication ambiguity.** `ComputeMerkleRoot` duplicates the last node on odd
  levels (the CVE-2012-2459 shape); that ambiguity needs two equal adjacent subtrees, which
  needs equal leaves, which is impossible because every leaf embeds a distinct `i`. The
  block's transaction-tree `mutated` check rejects a duplicated tail independently.
- **Canonical inputs.** The section bytes are canonical (strict codec: one encoding per
  logical content), the tags are constants, the serializer is the standard one, the order
  is block order. `payload_root` is a deterministic function of the block's transaction
  list.

---

## 4. Non-circularity and determinism — proof

Define the dependency relation "A → B" = "B's bytes are a function of A's bytes".

```
record bytes (tx i)  →  section_hash_i  →  leaf_i  →  payload_root
record bytes (tx i)  →  cell.commitment (policy 6/7 cell of tx i)  →  vout_i  →  txid_i  →  hashMerkleRoot
payload_root         →  MODERN_PAYLOAD_ROOT cell commitment (coinbase)  →  vout_0  →  txid_0  →  hashMerkleRoot  →  header  →  block hash
```

1. **`payload_root` never depends on any txid.** Leaves are `(i, section_hash_i)`; no txid,
   no output, no commitment enters a leaf. So the coinbase's own `txid_0` — which depends on
   `payload_root` through the `MODERN_PAYLOAD_ROOT` cell — is not an input to `payload_root`.
2. **A section never depends on its own transaction's identity.** Record grammars are
   forbidden from referencing the containing transaction's txid/ptxid/outputs (consensus
   rule; they could not do so consistently anyway, since every payload-backed cell's
   commitment is a function of the record). The finality records reference only past
   data (an earlier checkpoint; validator/BLS keys and a counter).
3. **Non-coinbase transactions do not depend on the coinbase.** Their sections, cells and
   txids are fixed before the block is assembled; `payload_root` is computed from them.
4. Therefore the relation is a DAG: records → sections → leaves → root → coinbase cell →
   coinbase txid → merkle root → header. **No cycle.** The block builder computes in that
   order; a validator recomputes `payload_root` from the received sections and compares it
   to the coinbase cell; a mismatch or a missing/extra cell ⇒ block invalid.
5. **Determinism.** Every step is a fixed function of canonical bytes (§3); two honest
   nodes with the same block bytes compute the same root; two different ordered section
   lists yield different roots (positional leaves, strict codec).
6. **Transitive commitment by the ordinary block hash.** Every MPA byte of every
   transaction is in some `section_hash_i` ⊂ `payload_root` ⊂ coinbase `vout_0` ⊂
   `txid_0` ⊂ `hashMerkleRoot` ⊂ header ⊂ block hash — and therefore also under the M3 block
   signature. No witness machinery is used.

---

## 5. Full-payload transaction identifier (`ptxid`) — no SegWit required

Two identities per modern transaction:

| Id | Covers | Use |
|---|---|---|
| `txid` | version, inputs, outputs, locktime — **evidence-independent** | state identity, outpoints, block merkle tree, indexes |
| **`ptxid`** | the complete optional-data serialization: marker, flag, inputs, outputs, **MPA**, locktime (and witness if ever present) | relay, deduplication, fetching, compact-block short ids, orphan maps |

Definition: `ptxid = SHA256d(full serialization with all optional data present)`, and for a
transaction with no optional data `ptxid == txid`. This is precisely what the existing
`CTransaction::m_witness_hash` (`GetWitnessHash()`) computes once the MPA is part of the
optional-data serialization — the field exists and is computed regardless of any
deployment, so **no SegWit activation is involved**; the `wtxidrelay` plumbing (announce,
`getdata`, compact-block reconstruction keyed on the full hash) carries MPA-bearing
transactions unchanged. Docs and RPC name it `ptxid`; the code type remains the existing
full-hash slot. (Structural alternative, if the owner prefers an identifier independent of
serialization details: `ptxid' = TaggedHash("B3/MPA/PTXID/V1", txid ‖ section_hash)`; it
would need its own relay key and is not recommended.)

`ptxid` is **not** used in the Merkle leaves (the coinbase `ptxid` depends on
`payload_root`; §4 item 1 keeps the leaf free of any transaction identity).

---

## 6. Worst-case validation and storage effects (numbers deliberately not frozen)

Block caps in the tree: `MAX_BLOCK_WEIGHT = 4,000,000`, `MAX_BLOCK_SERIALIZED_SIZE =
5,000,000`, weight = `3 × size(txid-form) + size(full-form)`. Because the MPA is in the full
form only, **it weighs ×1 by default** (the witness discount) unless a B3 rule adds `3 ×
mpa_size` to make it ×4.

| Vector | Worst case | Effect / needed bound |
|---|---|---|
| Bytes per block | ×1: up to ≈ 4 MB of MPA per block (weight budget), ≈ 5.7 GB/day at 60-s blocks if every block is stuffed; ×4: ≈ 1 MB/block, ≈ 1.4 GB/day | historical only (block files, prunable), zero UTXO growth. Choose the weight factor deliberately: **starting ×4 and relaxing later is a hard fork; starting ×1 and tightening later is a soft fork** |
| `FINALITY_CERTIFICATE` CPU | ≤ 1/block: ≤ 8,192 G1 adds + hash-to-G2 + 2 pairings ≈ 5–10 ms (`blst` portable, one core) | negligible; bounded by the per-block count |
| `FINALITY_KEY_EVIDENCE` CPU | each record = BIP340 (~60 µs) **+ BLS PoP verify (~1.5 ms)**; ≈ 320 B per binding incl. cell and amortized tx overhead ⇒ ≈ 12,500 records in a 4 MB block ⇒ **≈ 18 s of pairing work per block** on one core; IBD of a stuffed chain becomes infeasible | **bytes are not a sufficient bound — a per-block payload verification-cost budget is required** (the `MAX_BLOCK_SIGOPS_COST` pattern): each record type declares a cost (e.g. PoP 1,500, BIP340 60, certificate 10,000 units ≈ µs), `Σ cost ≤ MAX_PAYLOAD_VERIFY_COST` per block (e.g. 200,000 ≈ 0.2 s) and per transaction; counted before any cryptography runs |
| Invalid-evidence DoS at relay | an attacker sends transactions with bad PoPs: 1.5 ms to reject each, free to send | cheap checks first (structure → binding → BIP340 → PoP), per-tx cost cap, misbehaviour scoring on invalid evidence exactly as for invalid signatures |
| Decode memory | section cap × transactions | bounded by block size; lengths read and bounded **before** allocation (closes status row 147 for this codec) |
| Merkle | `n = vtx.size()` leaves | same cost class as `hashMerkleRoot` (µs–ms) |
| Chainstate | no UTXO entries for metadata cells; derived state: binding index (~100 B per validator), finalized tip, light-client state later | small; needs its own undo records per block (previous binding per changed validator) since no coin undo exists for these cells |
| Indexes | `txindex` unchanged (txid); `ptxid` lives in the mempool/relay maps only | no new disk index |
| Future records | `LIGHT_CLIENT_UPDATE` ≈ 24.6 KB, ≤ 1/block, ≈ 3 ms verify; bridge receipt proofs ≈ 3 KB, keccak-cheap | per-type max + per-block count + cost budget cover them |

**Conclusion for the numbers:** the record ceiling / section cap (proposed 32,768 /
65,536) are *byte* bounds and are safe for disk only if paired with (a) a deliberate MPA
weight factor and (b) a **verification-cost budget per block and per transaction**. Freeze
all three together, after the owner picks the weight factor. Until then the finality types'
own maxima (1,240 / 244) and counts (≤ 1 certificate per block) are the only figures needed
for design.

---

## 7. Protocol amendment (applied after §4)

Replaces revision 1 §2 "commitment" and all Path-A text:

1. **Carrier:** BIP144 flag bit `0x02` + the strict creation-action section codec (§1),
   modern-era decode context only; SegWit not involved.
2. **Commitment:** `MODERN_PAYLOAD_ROOT` coinbase metadata cell (policy type 8, `amount 0`,
   commitment = `payload_root`, params empty, present iff any MPA in the block, never in the
   UTXO set), with `payload_root` exactly as §3; proven non-circular and deterministic (§4).
3. **Identities:** `txid` evidence-independent; `ptxid` = full-serialization hash (§5) for
   relay/dedup/fetch; leaves use position + section hash only.
4. **Fail-closed rules:** as revision 1 §4, plus: `MODERN_PAYLOAD_ROOT` presence/uniqueness
   /coinbase-only/root-match checks; a record may not reference its own transaction's
   identity; per-block verification-cost budget checked before cryptography.
5. **Resource numbers:** OPEN — byte ceilings, MPA weight factor and the verification-cost
   budget are frozen together (§6).
6. **Reuse:** unchanged — a cell holds a 32-B commitment (+ ≤ 80-B typed params) and points
   at a bounded, priced record; ZK proofs, light-client updates, bridge/SPV proofs are new
   record types behind existing or new small cells; `MAX_POLICY_PARAMS_SIZE` untouched.

## 8. Rulings still needed

- MPA weight factor (×1 default vs ×4) — decides the byte ceilings.
- Verification-cost budget shape and per-type costs.
- `ptxid` definition: full-serialization hash (recommended) vs structural `H(txid ‖ section_hash)`.
- Policy number `8` for `MODERN_PAYLOAD_ROOT` (6/7/8 assignment to be recorded in the contract's "never renumber" list).
