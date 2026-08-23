# Finality timing analysis and benchmark — 2026-08-23

**Status: measurement report + recommended constants (owner-authorized benchmark-only
work). Nothing here activates or implements consensus. Harness:
`src/bench/b3_finality_bench.cpp`, built only with `-DB3_FINALITY_BENCH=ON`; vendored
`blst` v0.3.17 (54e6e556) `__BLST_PORTABLE__`, single thread. Reference machine: Apple M4
Max, AppleClang 17, RelWithDebInfo. Raw table in §3.**

---

## 1. Finality timing parameters — blocks and wall-clock

Modern PoS V1 spacing: `block_interval_seconds = 60` (ratified floor), `round_seconds = 30`,
`f0 = 1`, ×2 relaxation. At full participation ≈ 63 % of blocks land in round 0, so the
**expected spacing is ≈ 78 s** (60 + E[rounds]·30, E[rounds] ≈ 0.59); at 50 % online
stake 95 % of blocks arrive by round 3 (≈ 150 s). Below, "nominal" = 60 s, "expected" =
78 s.

Definitions: a block at height `h` is final when a certificate for the first checkpoint
`c ≥ h` is included in a block. Time-to-finality = wait for the next checkpoint (`I` =
`CHECKPOINT_INTERVAL`, mean `I/2`) + `D` = `CHECKPOINT_DEPTH` + signature gossip (~1–2 s,
negligible) + inclusion in the next produced block (1 block).

| Candidate (`I` / `D`) | Mean time to finality | Worst case (liveness OK) | Bridge-visible (+ Ethereum inclusion ≈ 1–2 min) |
|---|---|---|---|
| 60 / 20 (rev-1 proposal) | 51 blocks ≈ 51 min nominal / **66 min** expected | 81 blocks ≈ 81–105 min (+1 block per missed inclusion) | ≈ 53–108 min |
| 30 / 12 | 28 blocks ≈ 28 / **36 min** | 43 blocks ≈ 43–56 min | ≈ 30–58 min |
| **10 / 12 (recommended)** | 18 blocks ≈ 18 / **23 min** | 23 blocks ≈ 23–30 min | ≈ **20–32 min** |
| 1 / 6 | 7 blocks ≈ 7 / **9 min** | 7 blocks ≈ 7–9 min | ≈ 9–11 min |

- **Epoch handover latency.** With `E = 1,440` there are `E/I` checkpoints per epoch (24
  at `I = 60`, **144 at `I = 10`**); rotation happens exactly at `epoch_start + E` as long as
  ≥ 1 certificate of the epoch exists — i.e. **zero added latency** unless every checkpoint
  of a whole day failed. Ethereum learns `Set_{e+1}` from the first certificate of the
  epoch (≈ `D + 1` blocks after `epoch_start`, ~13–17 min at `I = 10`) and rotates on the
  first certificate of `e+1`.
- **Missed certificates.** A missed checkpoint (no quorum, or no producer included it)
  costs `I` blocks of extra latency for the blocks it covered; the next checkpoint covers
  them. An epoch with no certificate at all extends by `I`-block steps until one
  certifies; the lineage breaks only after `MAX_EPOCH_EXTENSION` (7·E proposed = 7 days of
  total quorum absence).
- **Per-checkpoint network/BLS overhead** (n validators, `finsig` = 116 B):
  validator: `hash_to_G2` 88 µs + sign 95 µs ≈ **0.18 ms**; gossip per node ≈ n × 116 B
  (**0.4 MB at n = 3,500; 0.95 MB at 8,192**); if a node verifies every individual `finsig`
  before relay: n × 0.61 ms (**2.1 s at 3,500; 5 s at 8,192**, single core); aggregation
  1.3 ms (3,500) / 3.1 ms (8,192). Per day at `I = 10`: 58 MB / 5 min CPU at 3,500 —
  acceptable; at `I = 1`: 576 MB/day and 50 min CPU/day at 3,500 — **bandwidth, not
  cryptography, rules out per-block checkpoints** without a sub-aggregation relay design
  (future). Block validation cost of the certificate itself is ≤ 1.9 ms per block at any
  set size (≤ 1 per block).

**Recommendation:** `CHECKPOINT_INTERVAL = 10`, `CHECKPOINT_DEPTH = 12`, `E = 1,440`
(one day; 144 checkpoints per epoch, huge handover redundancy, one set rotation per day),
`MIN_FINALITY_SET = 4`, `MAX_EPOCH_EXTENSION = 7·E`. Mean bridge-visible finality
≈ 20–30 min; tunable later (lowering `I` is a parameter change, the protocol does not
care). `D = 12` is chosen from the V1 fork-choice analysis (a 12-deep honest-majority
reorg is negligible under the timestamp-density argument; the horizon is 1,440) —
**owner may prefer 20 for extra margin; cost is +8 blocks of latency.**

---

## 2. Measured costs → verification-cost budget and byte ceilings

Unit: **1 cost unit ≈ 1 µs on the reference machine (portable blst, single core)**; a
node 3× slower sees 3 µs/unit. Declared costs are rounded **up**.

| `(payload_type, version)` / operation | measured | declared `verify_cost` |
|---|---|---|
| `FINALITY_KEY_EVIDENCE` (BIP340 15 µs + PoP verify 615 µs + decode/subgroup 70 µs) | ≈ 700 µs | **700** |
| `FINALITY_CERTIFICATE` (aggregate ≤ 8,192 pks + pairing; 1,908 µs worst) | ≈ 1.2 ms @3,500 / 1.9 ms @8,192 | **2,000** |
| MPA decode + section hash (per KB) | ≈ 1.6 µs/KB | folded into byte weight (×4); no cost units needed |
| `payload_root` (per leaf) | ≈ 0.6 µs | per block, not per record; 10,000 leaves = 6.3 ms |
| worst-case invalid rejection | = valid cost (full pairing then reject); malformed encodings reject in 7–14 µs | budget must assume full cost per record |

Worst case without a budget (×4 weight ⇒ ≤ ~1 MB of transaction bytes per block):
≈ 400 B per binding ⇒ ≈ 2,500 `FINALITY_KEY_EVIDENCE` records ⇒ **≈ 1.75 s of
verification per block**, ≈ 42 min extra per day of IBD. Hence:

| Constant | Recommended | Rationale |
|---|---|---|
| `MAX_BLOCK_PAYLOAD_COST` | **120,000** | ≈ 0.12 s reference / ≈ 0.36 s on a 3×-slower validator per block; ≈ 170 bindings or 60 certificates-equivalent per block — bindings are rare events (joins/rotations), 170 per block = 245k per day of capacity |
| `MAX_TX_PAYLOAD_COST` | **12,000** | ≈ 17 bindings per transaction (batch registration) |
| `COST_TO_VBYTES` | **1 vbyte per unit** | a 400-B binding tx (weight 1,600 ⇒ 400 vB) prices as 700 vB; CPU-heavy records can never be cheaper than their CPU; consistent with the block cost cap ≈ 12 % of a 1 MB block |
| `MAX_PAYLOAD_RECORD_SIZE` | **32,768** | parsing/hashing is 1.6 µs/KB — bytes are bounded by ×4 weight, not CPU; 32 KB leaves room for the ≈ 24.6 KB Ethereum sync-committee handover record later |
| `MAX_PAYLOAD_SECTION_SIZE` | **65,536** | same reasoning; per-tx cap |
| `MAX_PAYLOAD_RECORDS_PER_TX` | 64 | ratified, unchanged |
| `FINALITY_CERTIFICATE` max / per block | 1,240 B / 1 | layout facts |

These are measured-data recommendations; they remain **owner decisions**.

---

## 3. Raw measurements (min / median µs; 200 iterations except where noted)

| operation | min us | median us | note |
|---|---:|---:|---|
| BLS PoP verify (valid) | 593.8 | 614.6 | 1 pairing check, incl. subgroup checks |
| BLS PoP verify (INVALID sig, well-formed) | 560.1 | 614.9 | full pairing then reject = worst case |
| BLS pubkey uncompress (bit-flipped input; still decodable) | 7.0 | 7.1 | decode cost; flag-bit flips can yield valid x coordinates |
| BLS pubkey uncompress (valid) + in_g1 subgroup check | 27.8 | 28.5 | cost of accepting a key's bytes |
| BLS signature uncompress (valid) + in_g2 subgroup check | 37.9 | 42.3 | |
| BLS signature uncompress (MALFORMED) | 14.1 | 14.3 | decode rejects |
| hash_to_G2(32-byte digest) | 84.6 | 87.8 | validator + verifier both pay this |
| BLS sign (given H(m)) | 85.2 | 95.2 | per validator per checkpoint |
| BIP340 verify (FINALITY_KEY_EVIDENCE identity sig) | 13.3 | 14.9 | secp256k1 in-tree |
| cert verify n=16 100 %: aggregate 16 pks + verify | 595.5 | 608.7 | FastAggregateVerify, node side |
| cert verify n=128 100 % | 567.9 | 617.5 | |
| cert verify n=512 100 % | 677.7 | 692.3 | |
| cert verify n=1024 100 % | 690.1 | 740.9 | |
| cert verify n=2048 100 % | 873.6 | 904.4 | |
| cert verify n=3500 100 % | 1121.8 | 1143.6 | |
| cert verify n=3500 INVALID (one bad sig) | 1129.1 | 1164.6 | worst-case rejection = full cost |
| cert verify n=3500 95 % (3325 signers) | 1101.2 | 1125.5 | |
| cert verify n=3500 67 % (2345 signers) | 940.6 | 967.8 | |
| cert verify n=4096 100 % | 1222.9 | 1247.4 | |
| cert verify n=8192 100 % | 1862.7 | 1907.9 | |
| cert verify n=8192 INVALID (one bad sig) | 1892.9 | 1910.7 | |
| cert verify n=8192 95 % (7782) | 1820.5 | 1852.0 | |
| cert verify n=8192 67 % (5488) | 1434.3 | 1459.8 | |
| pairing only (pks pre-aggregated), any n | ≈ 560–606 | ≈ 596–618 | the constant part |
| aggregate signatures (aggregator): 16 / 128 / 512 / 1024 / 2048 / 3500 / 4096 / 8192 | 9.9 / 47.9 / 179.9 / 347.7 / 746.6 / 1281.6 / 1511.1 / 3016.5 | | G2 additions |
| MPA encode 1 × 1,240-B certificate (1,248 B) | 0.1 | 0.1 | |
| MPA strict decode, same | < 0.1 | < 0.1 | incl. registry check |
| MPA section TaggedHash, same | 2.2 | 2.3 | |
| MPA encode 64 × 244-B key evidence (15,937 B) | 0.9 | 1.1 | |
| MPA strict decode, same | 1.3 | 1.5 | |
| MPA section TaggedHash, same | 25.5 | 25.8 | |
| payload_root: 1 / 100 / 1,000 / 5,000 / 10,000 leaves | 0.3 / 61.6 / 607.6 / 3074.6 / 6227.8 | | build tagged leaves + ComputeMerkleRoot |

Notes: a 5 × 4,000-B section was (correctly) refused by the current 20,000-B section cap
including frame bytes — the strict codec enforces its bound. The "bit-flipped pubkey"
row did not produce a malformed encoding (flipping low bits of byte 0 can still give a
valid x); malformed-signature decoding is the representative fast-reject path.

---

## 4. Outcome

**All recommended constants were accepted and FROZEN by owner ruling 2026-08-23** (E = 1,440, I/D = 10/12, MAX_EPOCH_EXTENSION = 7·E, MIN_FINALITY_SET = 4 as chain bootstrap floor only, costs 700 / 2,000, budget 120,000 / 12,000, COST_TO_VBYTES = 1, ceilings 32,768 / 65,536, weight ×4). Historical list below.

### 4.1 (historical) Remaining owner decisions at report time

`CHECKPOINT_INTERVAL` / `CHECKPOINT_DEPTH` (10 / 12 recommended), `E` (1,440),
`MIN_FINALITY_SET` (4), `MIN_FINALITY_WEIGHT`, `MAX_EPOCH_EXTENSION` (7·E); cost budget
(120,000 / 12,000), `COST_TO_VBYTES` (1), byte ceilings (32,768 / 65,536); then the
go-ahead for consensus implementation. SegWit and A3 parameters stay out of scope.
