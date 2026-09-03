# Modern PoS V1 + BLS finality + MPA — implementation plan (tree-grounded)

**Status: plan only (2026-08-23). Constants frozen the same day (normative spec §9, MPA
doc §9, PoS spec §9). No consensus implementation begins until the owner approves this
plan. Every path below was verified against the current tree (`test/b3-clean-architecture`
at the plan commit); where the documents and the code disagree, the code is reported and
the plan follows the code.**

Target binary: the **X-pin Modern-PoS release** (F = M). The shipped v1 binary refuses
H+1 with X blank and never validates a modern-era block, so nothing here touches v1.

---

## A. Existing-code prerequisites — exact production paths

| Concern | Where it lives today (tree truth) | What must change |
|---|---|---|
| **Modern PoS activation at M** | `src/validation.cpp` ConnectBlock modern branch (~3045–3140): corridor vs modern-PoS split, `modern::CheckModernPosCodec`, `ModernChainDomain`, `no-modern-pos-rules` when `consensus.modern_pos` is unset, `ModernPosContext` → `modern::CheckModernPosBlock` (`src/modern/pos_v1.h`); phase from `Consensus::GetConsensusPhase` (`src/consensus/era.h`); params `Consensus::ModernPosParams` (`src/consensus/modern_pos_params.h`, mainnet **unset**, fixtures set it); horizon refusal at ~5316 (`modern-reorg-too-deep`). | Add finality fields to `ModernPosParams` (E, interval, depth, max-extension, min-set, cost budget, MPA limits) under the same **unset ⇒ fail-closed** discipline; `Valid()` extended; mainnet still unset until the X-pin release sets the whole block. |
| **Validator eligibility** | `modern::ModernPosEligible` (`pos_v1.h:108`) fed by `Chainstate::ModernStakeTracker().ActiveWeight(key, height)` (`src/node/stake_tracker.{h,cpp}`: `std::map<COutPoint, Entry>`, `Sync` = rebuild by walking the modern span, `BlockConnected` incremental, `MarkDirty` on disconnect at `validation.cpp:3573`). | From F: eligibility additionally requires a non-revoked binding for the declared key (`W` = Σ weight of **bound** validators). Implement by giving the tracker (or a sibling `FinalityTracker`) the binding index and having `ActiveWeight` take bindings into account at `eval_height`. |
| **BLS key binding** | No code. Script carriers are the live policy realization: `src/modern/stake.h` (`B3S1` push ‖ `OP_DROP` ‖ owner script), recognized by `ClaimsStakeMagic`, validated by `CheckStakeOutputs` at mempool (`validation.cpp:864`) and block (`validation.cpp:5391`) level; standardness carve-in in `src/script/solver.cpp:146`. `ModernOutput`/`PolicyType` model in `src/modern/policy.h` mirrors STAKE but is **not on the wire**. | New `src/modern/finality_key.h`: `FINALITY_KEY` cell carrier (`B3K1` ‖ `bls_pubkey` ‖ `seq`, commitment = `validator_key`, **unspendable metadata cell**), `CheckFinalityKeyOutputs` (tx level, same two call sites), `PolicyType::FINALITY_KEY = 7` + `IsActivatedPolicy` gating in `policy.h`; solver carve-in; evidence verification (BIP340 + PoP) from the MPA record. **Discrepancy reported:** docs say "policy cells"; the tree realizes cells as typed script carriers plus a model mirror — the plan follows the tree (as STAKE does). |
| **MPA serialization** | `src/primitives/transaction.h` `UnserializeTransaction`/`SerializeTransaction` (~225–290): BIP144 marker/flag, `flags & 1` witness, **any other flag bit throws** (fail-closed today); `TransactionSerParams{allow_witness, legacy_time}`; `CTransaction{hash, m_witness_hash}` computed in `src/primitives/transaction.cpp:94–104`. Section codec exists: `src/modern/creation_action.h` (`EncodeCreationActionSection` / `DecodeCreationActionSection`, bounds 4,000 / 64 / 20,000; `static_assert(MAX_CREATION_ACTION_PAYLOAD == MAX_TRANSITION_PROOF_SIZE)` in `proof.h:318`). | Add `std::vector<CreationAction> mpa` to `CMutableTransaction`/`CTransaction`; flag bit `0x02` read/written **only** under a modern-era serialization context (new `TransactionSerParams::allow_mpa`, selected exactly where `legacy_time` is selected today: per-peer wire codec / block codec / local construction); bounds raised to 32,768 / 65,536 with **per-type** maxima; decouple the `static_assert`. **Discrepancy reported:** the codec's type registry contains test-only types 1–3; they become "known, not activated" (fail closed) on the wire. |
| **`ptxid`** | `CTransaction::m_witness_hash` = hash of `TX_WITH_WITNESS` serialization (`transaction.cpp:94`); used by wtxid-relay, compact blocks, orphanage. | Normative `ptxid` = SHA256d(canonical full serialization incl. MPA) — implemented by making the full-form serialization include the MPA, so the existing full-hash slot *is* `ptxid`; RPC/docs name it `ptxid`; assert `ptxid == txid` when no optional data. |
| **`MODERN_PAYLOAD_ROOT`** | No code. Coinbase rules: M3 `scriptSig` declaration (`pos_v1.h:149`), M6 STAKE-magic check (`validation.cpp:3087`). Merkle: `ComputeMerkleRoot` (`src/consensus/merkle.h`). | New `src/modern/payload_root.h`: `section_hash`, positional leaves, `payload_root`; `PolicyType::MODERN_PAYLOAD_ROOT = 8` carrier (`B3R1` ‖ nothing, commitment = root); ConnectBlock/CheckBlock rule: present iff any MPA, coinbase-only, exactly one, root matches; miner emits it. |
| **Metadata-cell handling** | `src/coins.cpp:91` `AddCoin` skips only `IsUnspendable()`; `validation.cpp:2425–2440` DisconnectBlock exact-match skips the legacy zero-value marker (the precedent). | `modern::IsMetadataCell(script)` (types 6/7/8) → skipped in `AddCoins` (via a parameter or a pre-filter in ConnectBlock's `UpdateCoins`) **and** in DisconnectBlock's exact-match loop; amount must be 0; never in mempool as spendable. |
| **Payload cost accounting** | No code. Sigops analogue: `MAX_BLOCK_SIGOPS_COST` in `src/consensus/consensus.h`, `GetTransactionSigOpCost` in `src/consensus/tx_verify.cpp`, block sum in ConnectBlock. Weight: `GetTransactionWeight/GetBlockWeight` (`src/consensus/validation.h:132–139`). Policy vsize: `src/policy/policy.{h,cpp}` (`GetVirtualTransactionSize`). | `modern::PayloadCost(tx)` from the frame alone (registry `(type, version) → cost, max, per-block count`); per-tx check in `CheckTransaction`/mempool, per-block sum in ConnectBlock **before** any record verification; weight `+3×mpa_size`; policy `vsize = max(weight/4, cost×1)`. |
| **Certificate construction** | Aggregation/signing: no code (blst bench-only). Block production: `src/node/miner.cpp:150–260` (modern-PoS template, validator key, eligibility, `SignModernPosBlock`); staking loop `src/node/staking.{h,cpp}`; staking RPCs `src/wallet/rpc/staking.cpp`. | New `src/node/finality_signer.{h,cpp}` (sign checkpoints with the wallet's BLS key, gossip `finsig`), `src/node/finality_aggregator` (collect → certificate), miner includes the best certificate (coinbase `FINALITY_CERT` cell + MPA record) and the `MODERN_PAYLOAD_ROOT` cell. |
| **Certificate verification** | No code. | `src/modern/finality.h` (digest, bitmap rules, quorum) + `src/crypto/bls.{h,cpp}` (blst wrapper: decode/subgroup, PoP verify, FastAggregateVerify); ConnectBlock hook after cost/count checks; fail-closed reasons `bad-finality-cert-form` / `bad-finality-cert`. |
| **Validator-set snapshots** | `StakeTracker` can answer per-key weight at a height but has **no full-set enumeration**; rebuild-by-walk on reorg (no undo records). | Add `Snapshot(height)` enumeration (sorted members with bound BLS keys) and per-epoch header cache to the tracker; persisted as a derived, rebuildable index (same discipline as today). |
| **Certificate-gated epoch transitions** | No code. Block index already persists a per-block modern field (`CBlockIndex::m_modern_pos_digest`, `chain.h:193/415`). | Epoch state machine (`epoch_start[]`, `Set_e`, `Set_{e+1}`, extension counter) in the finality tracker; a small persisted per-index flag "carries certificate for epoch e / height h" enables cheap recomputation. |
| **Finality pin / reorg refusal** | Horizon refusal at `validation.cpp:~5316` (contextual header check, `!blockman.LoadingBlocks()` guard). | Add `modern-finality-violation` next to it: refuse any candidate whose fork point is below `finalized_tip`; pin updated on connect of a certificate-bearing block; skipped during reindex/import like the horizon. |
| **Chainstate persistence & undo** | Coins undo via `CBlockUndo`; derived indexes (stake tracker) are **rebuilt**, not undone (`MarkDirty` on disconnect). | Same discipline for bindings, epoch state, finalized tip and the certificate archive: `BlockConnected` applies, disconnect marks dirty, `Sync` rebuilds from blocks; persistence of the derived index is a cache (optional leveldb), never authoritative. This satisfies correct Connect/Disconnect semantics by reconstruction. |
| **Block assembly** | `src/node/miner.cpp` modern-PoS branch. | Include `MODERN_PAYLOAD_ROOT` cell when any MPA is in the template; include `FINALITY_CERT` cell + record when a quorum certificate is available; respect `MAX_BLOCK_PAYLOAD_COST` in selection; coinbase ordering unchanged. |
| **Mempool / relay / standardness** | `MemPoolAccept::PreChecks` era gate (status row "Mempool era rules"); `CheckStakeOutputs` at 864; solver carve-in; wtxid-relay. | `CheckFinalityKeyOutputs` + MPA cost cap at admission; `FINALITY_KEY` solver carve-in; MPA-bearing tx standard only for registered/activated types; `ptxid` relay unchanged in mechanism. |
| **Wallet / staking** | `createstake` (`wallet/rpc/staking.cpp:56`, legacy P2PKH owner — **no SegWit dependency**), staking loop with wallet validator key. | Wallet BLS key (new key record), `bindfinalitykey` / `revokefinalitykey` RPC building the `FINALITY_KEY` tx (cell + MPA evidence), staking status shows binding/seq; the staking loop refuses to produce when unbound from F (mirrors consensus). |
| **P2P** | `src/protocol.{h,cpp}`, `src/net_processing.cpp` (no custom modern messages today). | `finsig` message (116 B), relay bounded to indices of `Set_e`/`Set_{e+1}`, one per (index, height), misbehaviour on invalid; `getfinality*` RPCs in `src/rpc/`. |

**Other discrepancies found:** (i) `MIN_FINALITY_WEIGHT` has no counterpart — frozen as "none" (the floor is `MIN_FINALITY_SET`); (ii) `blst` is bench-only today — promoting it to a consensus library is a CMake change (link `b3_blst` into `bitcoin_consensus`), still `__BLST_PORTABLE__`; (iii) the wallet's default address type is `BECH32` (`wallet.h:148`) while mainnet `SegwitHeight = INT_MAX` — unrelated to finality, already flagged as the separate SegWit audit item.

---

## B. Implementation sequence — small reviewable commits

Dependency order adjusted from the owner's sketch in two places: the **blst wrapper** moves
before `FINALITY_KEY` (its evidence check needs PoP), and **metadata-cell UTXO exclusion**
moves before `FINALITY_KEY` (the first metadata cell needs it to keep the coin set clean).

| # | Commit | Builds on | Tests in the same commit |
|---|---|---|---|
| 1 | `consensus: finality + MPA constants and types` — `ModernPosParams` finality fields (frozen values, unset ⇒ fail closed), `PolicyType 6/7/8` enum numbers, frozen tags (`B3/FINALITY/V1`, `…/BIND/V1`, `…/CERT/V1`, `B3/MPA/*`), `FinalizedBlock`/`ValidatorSetHeader`/`Certificate` fixed-width codecs (`src/modern/finality_types.h`) | — | codec round-trips, fixed-width vectors, digest vectors |
| 2 | `crypto: blst consensus wrapper` — promote vendored blst from bench-only to `src/crypto/bls.{h,cpp}` (decode + subgroup, PoP verify, FastAggregateVerify, aggregate), CMake link into consensus | 1 | consensus-spec-tests BLS vectors (sign/verify/aggregate/fast_aggregate_verify/PoP), malformed encodings, infinity |
| 3 | `modern: metadata-cell rule` — `IsMetadataCell`, `AddCoins`/DisconnectBlock exclusion, amount-0 rule | 1 | cell never in UTXO; connect/disconnect symmetry; legacy marker unaffected |
| 4 | `modern: FINALITY_KEY cell + evidence` — carrier, `CheckFinalityKeyOutputs`, policy model 7, solver carve-in, binding index (derived, rebuildable) | 2,3 | BIP340 binding vectors, PoP, duplicate BLS key, seq replay/skip, revocation, wrong domain |
| 5 | `primitives: MPA serialization` — flag 0x02, `mpa` member, modern-context-only, bounds 32,768/65,536, per-type registry with maxima/costs/activation | 1 | canonical encoding, unknown type fail-closed, oversize, flag-without-section, legacy context rejects |
| 6 | `primitives: normative ptxid` — full-form includes MPA; `ptxid == txid` when no optional data; RPC field | 5 | txid unchanged by MPA; ptxid changes; vectors |
| 7 | `consensus: MODERN_PAYLOAD_ROOT` — `payload_root`, coinbase cell rule (iff any MPA, exactly one), miner emits | 3,5 | root vectors; missing/extra/mismatched cell; section swap/reorder detection |
| 8 | `consensus: payload cost + weight ×4 + policy vsize` — per-tx/per-block cost before crypto, `weight += 3×mpa`, `vsize = max(weight/4, cost)` | 5 | exhaustion (tx and block), ordering (cost rejects before any pairing) |
| 9 | `node: validator-set snapshot` — tracker enumeration, `Snapshot(h)`, set header/leaves/`members_root`, carry-over, epoch header cache | 4 | snapshot vectors (sorting, weights, excluded unbound/revoked, carry-over) |
| 10 | `modern: finality certificate validation` — digest, bitmap rules, weighted quorum, aggregate verify, record↔cell binding | 2,5,9 | quorum boundaries (= / −1), bitmap high bits, wrong set, wrong domain, invalid sig full-cost path |
| 11 | `consensus: checkpoint schedule + depth + epoch window` — checkpoint heights, depth, `{current, current−1}` with monotone height / set hash / epoch relation | 10 | timing tests, delayed `{current−1}` accept/reject matrix |
| 12 | `consensus: certificate-gated epoch transition` — `epoch_start`, extension, `MAX_EPOCH_EXTENSION`, lineage-broken state | 11 | handover success / failure / extension / broken lineage |
| 13 | `consensus: finality pin` — `modern-finality-violation`, pin update on connect, reindex/import skip | 12 | forbidden reorg refused, allowed reorg above pin, restart keeps pin |
| 14 | `consensus: eligibility requires binding from F` — `ActiveWeight` through bindings, `W` = bound weight | 4,9 | unbound validator ineligible; W equality test (production weight == finality weight) |
| 15 | `node: BLS signing/aggregation + finsig` — finality signer, aggregator, P2P message + relay bounds | 10 | gossip dedupe/bounds, equivocation evidence stored |
| 16 | `node: block assembly + staking integration` — certificate inclusion, payload-root emission, cost-aware selection, staking loop refuses when unbound | 7,8,13,15 | template validity under all rules |
| 17 | `wallet/rpc: BLS key + bind/revoke + finality RPCs` | 4,15 | RPC round trips |
| 18 | `activation: F = M plumbing` — regtest/fixture params, guard tests that mainnet block stays unset; chainparams hook for the X-pin release | all | regtest activation at M |
| 19 | `tests: vectors, cross-platform determinism, regtest campaigns` | all | §D matrix |

---

## C. Consensus invariants per stage

| Invariant | Guarded by stage(s) | How |
|---|---|---|
| No change to legacy consensus ≤ H | all | every new rule is reached only via `legacy_b3coin && phase ≥ corridor` branches; replay path untouched; `legacy_*_tests` unchanged |
| No activation before the Modern boundary | 1, 5, 18 | params unset ⇒ `no-modern-pos-rules`; MPA flag decodable only in modern context; guard test: mainnet `modern_pos` unset |
| No OP_RETURN fallback | 4, 7 | cells are typed carriers (`B3K1`/`B3F1`/`B3R1`), never `OP_RETURN`; test asserts scripts are not `IsUnspendable()`-by-OP_RETURN yet are excluded from UTXO by rule |
| No SegWit dependency | 5, 6, 7 | MPA and `ptxid` use the flag byte and the coinbase cell only; no `DeploymentActiveAt(SEGWIT)` in any new path; test runs with `SegwitHeight = INT_MAX` |
| 80-byte policy-state limit unchanged | 1, 4 | `MAX_POLICY_PARAMS_SIZE` untouched (static test); `FINALITY_KEY` params 52 B, `FINALITY_CERT` 16 B, `MODERN_PAYLOAD_ROOT` 0 B |
| No arbitrary-data policy | 4, 5 | every record type has a grammar + binding; unknown ⇒ invalid |
| No certificate in the spendable UTXO set | 3, 7 | metadata-cell exclusion tests (connect, disconnect, reindex) |
| No rotation without previous-set authorization | 12 | epoch advances only on an included epoch-`e` certificate; test: no certificate ⇒ extension |
| Production weight == finality weight from F | 14 | single `ActiveWeight` path through bindings; equality test |
| No block-hash/certificate circularity | 7 | leaves exclude txids; miner/validator compute in DAG order; test builds a block and re-derives |
| Deterministic digests | 1, 10 | fixed-width codecs, tagged hashes, vectors checked on every CI platform |
| Deterministic weights/quorum | 9, 10 | integer arithmetic, whole-B3 units, sorted by key; vectors |
| Deterministic payload cost across platforms | 8 | cost from frame alone; table-driven; vectors |

---

## D. Test matrix required before activation

Unit (`src/test/finality_*_tests.cpp`, `mpa_tests.cpp`, `bls_tests.cpp`): BLS PoP vectors (consensus-spec-tests + ours); BIP340 binding authorization vectors (valid, wrong key, wrong domain, wrong seq); BLS aggregate signature vectors at n ∈ {1, 4, 16, 512, 3,500, 8,192}; wrong-domain signatures (SIG vs POP DST, wrong chain domain); malformed pubkeys/signatures (bad flags, off-curve, wrong subgroup, infinity); duplicate BLS key across validators; seq replay / skip / revocation / rebind-after-revoke; validator-set snapshot (sorting, weights, unbound excluded, carry-over, `MIN_FINALITY_SET`); weighted-quorum boundaries (= quorum, −1, one whale); checkpoint timing (interval, depth, epoch-end); delayed `{current−1}` certificate (accept when monotone + correct set hash; reject lower height, wrong set hash, wrong epoch relation); epoch extension and `MAX_EPOCH_EXTENSION` lineage break; handover failure; finality pin / forbidden reorg; MPA canonical encoding (non-minimal CompactSize, trailing bytes, empty section with flag, unknown flag bits); unknown/unactivated `(type, version)` fail-closed; payload-root commitment (missing/extra cell, mismatch, section swap); `txid` vs `ptxid`; metadata-cell UTXO exclusion; ConnectBlock/DisconnectBlock state restoration (bindings, epoch state, finalized tip, certificate archive); payload-cost exhaustion (tx, block; rejects before any pairing — instrumented); invalid-proof cheap rejection (malformed bytes reject in µs, ordering asserted); cross-platform determinism vectors (digest, root, cost) run on macOS arm64 + Linux x86_64.

Functional/regtest (`test/functional/feature_b3_finality*.py`, `feature_b3_mpa.py`): activation at M with a short corridor (bindings in the corridor, `Set_0` non-empty); certificate production end-to-end with ≥ 4 validators; missed checkpoint and recovery; handover at `E` (small regtest E); extension when no quorum; pin refusal across a forced reorg; restart / `-reindex` / `-reindex-chainstate` persistence of bindings, epoch state and pin; mempool standardness of `FINALITY_KEY` txs and rejection of oversize/over-cost MPA; `ptxid` over RPC/P2P.

---

## E. Remaining blockers

**Before implementation starts:** the owner's approval of this plan (no other decision is open for Modern PoS V1 finality/MPA).

**Before X-pin activation (not before coding):**
1. Modern PoS spec §9 provisional rows: `sentinel_bits` (0x207fffff), `max_future_seconds` (120), `reward` (0 / OD-2).
2. **SegWit for the modern era** — separate audit item; current Modern PoS functionality (STAKE with legacy P2PKH owner, finality, MPA) does **not** require it, but the wallet default address type is BECH32 and mainnet `SegwitHeight = INT_MAX`.
3. Release operations: two further fixed seeds + DNS seed; Linux x86_64 build box/CI; the X-pin itself (block H buried, X recorded).
4. Bootstrap operations: owner validators stake and publish `FINALITY_KEY` bindings during the corridor so `Set_0 = Snapshot(M−1)` has ≥ `MIN_FINALITY_SET` members.

**Out of scope for this historical Modern-PoS plan:** separately activated
bridge parameters (including bridge security thresholds and `MAX_EPOCH_LAG`),
the bridge stage-4 work now implemented elsewhere in the transition tree, and
the ZK prover. `A3` now names FlowMesh activation and never activates bridge
minting or redemption.
