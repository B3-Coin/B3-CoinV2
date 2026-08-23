# The Modern Payload Area — native carrier for large evidence bytes (design report)

**Status: design report (2026-08-23), no implementation. Written under the owner's locked
model of the same day: `ModernOutput.policy_params ≤ 80 B` is a permanent invariant for
small typed live/derived state; large evidence (BLS certificates, bridge proofs, Merkle
proofs, future ZK proofs) is NOT policy state but bounded proof/payload data committed to
by the policy object; fees/block limits price and bound historical payload bytes, the
80-byte bound protects live state; `MAX_POLICY_PARAMS_SIZE` is never raised for proof
growth; no arbitrary-data policy exists. Payload bytes are historical blockchain data —
bounded and priced — that never become live spendable/state entries. Target release: the
X-pin Modern-PoS release (the first binary that validates any modern-era block).**

---

## 0. Idea in one paragraph

Core's transaction format already has a **segregated, optional, per-transaction area**
with a reserved extension mechanism: BIP144's flag byte (bit 0 = witness; **other bits
reserved**, and this tree already rejects unknown bits). Core's block format already has a
**32-byte slot reserved for future commitments**: the BIP141 *witness reserved value* in the
coinbase witness, hashed into the witness commitment → coinbase txid → merkle root → block
hash. The tree already has the **strict typed record codec** we need: the V2
creation-action section (`{type u16, version u16, payload}`, bounded 4,000 / 64 / 20,000,
unknown types invalid). The Modern Payload Area (MPA) is exactly those three things
plugged together: **flag bit 1 (0x02) = "this transaction carries a Modern Payload Area"**,
whose bytes are the existing strict section codec, committed into block identity through
the witness reserved value. No script trick, no scriptSig, no `vchBlockSig`, no OP_RETURN
of our own, no policy-params change.

---

## 1. Exact carrier format

### 1.1 Transaction wire (modern-era serialization only)

```
version        int32
marker         0x00                      // BIP144 marker
flag           0x01 | 0x02               // bit0 = witness present, bit1 = MPA present; 0x00 is never written
vin            vector<CTxIn>
vout           vector<CTxOut>            // policy cells live here (≤ 80-B params, 32-B commitment)
[witness]      per-input stacks          // only if flag & 0x01 — input authorization (the "witness-style" proof area)
[mpa]          PayloadSection            // only if flag & 0x02
lockTime       uint32
```

Rules: `flag == 0x00` is invalid (write the non-segwit form instead, as Core does);
`flag & ~0x03` invalid; `flag & 0x02` with an empty section invalid (omit the area
instead); the MPA is parsed **only under the modern-era decode context** (the existing
era-aware provenance: per-peer wire codec / block codec / local construction) — a pre-H
peer or block can never present one.

### 1.2 PayloadSection = the existing strict creation-action section, unchanged grammar

```
PayloadSection {
  count          CompactSize, 1 ≤ count ≤ MAX_PAYLOAD_RECORDS_PER_TX (64, ratified)
  records[]      PayloadRecord × count
}
PayloadRecord {                           // == modern::CreationAction frame, byte-identical
  payload_type     uint16 LE              // registry number (creation-action numbering continues: 1–3 exist)
  payload_version  uint16 LE
  payload          CompactSize-prefixed bytes, ≤ type-specific max ≤ MAX_PAYLOAD_RECORD_SIZE
}
```

Binding to policy objects is **inside each type's payload grammar** (as types 2 and 3
already do), not in the frame. For the finality objects:

| payload_type | bound object | payload | max | per-block |
|---|---|---|---|---|
| 4 `FINALITY_CERTIFICATE` | the unique `FINALITY_CERT` (policy 6) cell of the **same (coinbase) tx** with `cell.commitment == TaggedHash("B3/FINALITY/CERT/V1", payload)` | `FinalizedBlock(120) ‖ bitmap(⌈n/8⌉) ‖ sig(96)` | 1,240 B | ≤ 1 |
| 5 `FINALITY_KEY_EVIDENCE` | the unique `FINALITY_KEY` (policy 7) cell of the same tx with `cell.commitment == validator_key` and `cell.params == bls_pubkey ‖ seq` | `validator_key(32) ‖ bls_pubkey(48) ‖ seq u32 ‖ bip340_sig(64) ‖ pop(96)` | 244 B | unbounded (fee-priced) |

The cell carries only the 32-byte commitment (+ ≤ 80-B typed params); the evidence lives in
the record; the record is invalid without its cell and the cell is invalid without its
record (both directions checked in the same transaction).

---

## 2. How it is committed into transaction and block identity

| Identity | Covers MPA? | Mechanism |
|---|---|---|
| `txid` | **no** (by design — state identity excludes evidence, contract §26) | txid serializes without marker/flag/witness/MPA, exactly as Core's witness rule |
| `wtxid` | yes | `wtxid` = hash of the full optional-data serialization; the MPA sits after the witness, so Core's existing wtxid covers it with no new code |
| block hash, non-coinbase tx | yes | wtxid → `BlockWitnessMerkleRoot` → BIP141 commitment output → coinbase txid → merkle root → header |
| block hash, **coinbase** tx | yes, via the reserved slot | BIP141 sets the coinbase wtxid to 0, so its MPA is not in the witness root; B3 rule: **the 32-byte witness reserved value = `TaggedHash("B3/PAYLOAD/COINBASE/V1", canonical coinbase PayloadSection bytes)`**, or `0^32` if the coinbase has no MPA. The reserved value is hashed into the commitment (`SHA256d(witness_root ‖ reserved)`), so the coinbase's certificate is committed by the block hash and covered by the M3 block signature |
| `FullTransitionId` analog | yes | `TaggedHash("B3/PAYLOAD/SECTION/V1", canonical section bytes)` is the per-tx payload commitment, reusable by any rule that wants to reference a record set |

Consequences: a block must carry the witness commitment whenever any transaction has a
witness **or** an MPA (`no-mpa-commitment` otherwise); a mutated MPA changes the wtxid /
reserved value and fails the commitment (`bad-witness-merkle-match`), the same treatment
Core gives witness mutation (BLOCK_MUTATED, no peer penalty for the block hash).

**Coupled decision (pre-existing gap, now load-bearing):** this commitment path requires
BIP141 to be active in the modern era — `SegwitHeight = H + 1` pinned in the X-pin release
(B3 main currently has `INT_MAX`, which also makes every witness-program output unspendable
post-H). If the owner declines to activate witness at H+1, the fallback commitment is a
coinbase **`PAYLOAD_ROOT` metadata cell** (policy type, `amount 0`, commitment = merkle root
over `TaggedHash("B3/PAYLOAD/TX/V1", txid ‖ section_hash)` per tx in block order,
params empty, never in the UTXO set) — same MPA bytes, one B3-native commitment instead of
Core's; larger, offered only as the fallback.

---

## 3. Size and resource limits

| Limit | Value | Note |
|---|---|---|
| `MAX_PAYLOAD_RECORD_SIZE` (global hard ceiling) | 32,768 B | ceiling for any type; replaces the provisional 4,000 `MAX_CREATION_ACTION_PAYLOAD` as the *ceiling* (types 1–3 keep their own ≤ 4,000 maxima) |
| per-type max | declared in the registry, ≤ ceiling | `FINALITY_CERTIFICATE` 1,240; `FINALITY_KEY_EVIDENCE` 244; future bridge receipt proof ~3,000; a sync-committee handover (~24.6 KB) fits under the ceiling as one record |
| `MAX_PAYLOAD_RECORDS_PER_TX` | 64 | ratified number, unchanged |
| `MAX_PAYLOAD_SECTION_SIZE` | 65,536 B | per-transaction section cap (was 20,000 for the test-only envelope) |
| per-block, per-type counts | registry | `FINALITY_CERTIFICATE` ≤ 1 per block and coinbase-only; light-client handover ≤ 1; others unbounded |
| weight / fees | MPA bytes count as **witness-weight (×1)** | historical, pruneable like witness, still bounded by block weight and priced by fee rate — the owner's "bounded and priced" rule; coinbase records have no fee and are bounded by the per-block counts instead |
| decode memory | read length before allocating; reject any length > its cap before reading the bytes | closes status-matrix row 147 ("bounded reads before cryptography") for this codec |

---

## 4. Parsing and fail-closed rules (normative intent)

1. **Strict grammar.** Minimal CompactSize everywhere; exact `count`; no trailing bytes in
   the section; section bytes are canonical (one encoding per logical content) so the
   section hash is well-defined.
2. **Unknown `(payload_type, payload_version)` ⇒ transaction invalid**, never ignored
   (the creation-action registry rule). Activation-gated types are "unknown" before their
   height (`IsKnownPayload(type, version, height)`), so nothing can be smuggled in early.
3. **Every record must bind.** A record whose type names a bound policy cell and finds none
   (or more than one, or a commitment mismatch) ⇒ invalid; a cell of a payload-backed
   policy type (6, 7, future bridge cells) without exactly one binding record ⇒ invalid.
4. **Era/context gating.** MPA parsed only in the modern decode context; present in a
   legacy-context message ⇒ decode failure (existing provenance model); present in a block
   below the activation height ⇒ `mpa-not-active`.
5. **Commitment presence.** Any MPA in the block ⇒ witness commitment required and the
   reserved value must equal the coinbase payload hash (or `0^32`).
6. **Validation order per record:** frame/lengths → type activation → binding to cell →
   cheap semantic rules (epoch window, `seq` continuity, per-block counts) → cryptography
   last (BLS pairing, BIP340, PoP, Merkle/MPT proofs).
7. **Per-block counters** are checked before any cryptography (a second
   `FINALITY_CERTIFICATE` record fails on count, not after a pairing).
8. **No reinterpretation, ever.** Type numbers are never reused; a superseded type stays
   decodable as "known but invalid/retired" (the type-1 precedent).
9. Relay policy (not consensus): MPA-bearing transactions are standard only for registered
   types under their maxima; wtxid-relay carries them unchanged.

---

## 5. Reuse by future proof types — without touching the 80-byte invariant

The pattern is fixed: **the policy cell holds the 32-byte commitment (+ ≤ 80-B typed
params) and points at a bounded, priced record; the record holds the evidence.** Adding a
proof type = one registry row `{payload_type, version, max_size, per-block count,
activation height, verifier}` plus, if new state is involved, one policy type with
≤ 80-B params. Nothing else moves:

| Future evidence | Cell (≤ 80-B params) | Record |
|---|---|---|
| ZK proof of B3 finality (deferred) | **the same `FINALITY_CERT` cell, unchanged** — commitment = hash of whatever record backs it | new type `FINALITY_ZK_PROOF`, max e.g. 2,048 B; verifier swaps, `FinalizedBlock` untouched |
| Ethereum light-client update / sync-committee handover | light-client state cell (small) | `LIGHT_CLIENT_UPDATE` ≤ 32 KB, ≤ 1 per block |
| Bridge mint (receipt / storage proof) | `BRIDGE_BACKED` asset cell | `BRIDGE_MINT_PROOF` ≤ ~4 KB |
| Merkle / SPV proofs (Bitcoin leg) | asset cell | `SPV_PROOF` |
| Any future typed proof | its cell | its record |

`MAX_POLICY_PARAMS_SIZE` is never consulted for evidence; evidence growth is absorbed by the
record ceiling and per-type maxima, paid for by weight, bounded per block by counts.

---

## 6. What must be ruled before coding

1. Adopt the MPA as the native proof/creation-action carrier (BIP144 flag 0x02 + strict
   section codec) — yes/no.
2. Commit path: **A** witness reserved value (requires `SegwitHeight = H + 1` in the X-pin
   release) — recommended — or **B** coinbase `PAYLOAD_ROOT` metadata cell (segwit-free).
3. Numbers: record ceiling 32,768; section 65,536; MPA weight ×1.
4. Finality type maxima 1,240 / 244 and the `FINALITY_CERT` ≤ 1 per block, coinbase-only.
5. The pre-existing `SegwitHeight` decision for the modern era (independent of 2, but
   decisive for which path is smallest).
