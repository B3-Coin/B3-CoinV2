# B3 bridges — BLS as the keystone (design proposal)

**Status: historical design record (2026-08-23). Sections 1–8 preserve the
original proposal and are not current operating instructions. The BLS direction
is now implemented as the keyless `BlsCertificateProver` /
`B3FinalityVerifier` / `B3StakerBridge` stack, with a one-time 3-of-4 handoff to
canonical Set0, normal certificate/withdrawal proof-calldata RPCs, and B3's
Ethereum light-client deposit path. Mainnet remains fail-closed pending the
new deployment manifest, later-build chainparams pins, independent audits, and
rehearsal. The independent bridge height is selected only after those gates;
A3 is FlowMesh activation and never activates this bridge.**

## 1. Why BLS is the keystone (the one-paragraph version)

A bridge is two one-way proofs: **mint leg** (origin chain → B3: "this lock really
happened and is final on the origin chain") and **release leg** (B3 → origin: "this burn
really happened and is final on B3"). OD-8 already rules the mint leg must be a **light
client**, never a signer set. The only finality signal Ethereum L1 exports that a foreign
chain can verify cheaply is the **sync committee**: 512 validators who, every slot, sign
the beacon header with an **aggregated BLS12-381 signature** (one 96-byte signature +
a 512-bit participation bitfield). Verifying that signature *is* the Ethereum light
client; without BLS12-381 there is no light client, only a federation.

The same primitive solves the release leg: if B3's validators (or FN seats) carry BLS
keys, a finalized B3 checkpoint is **one aggregate signature** that an Ethereum contract
verifies with the **EIP-2537** BLS12-381 precompiles (live on L1 since Pectra, 2025).
Ethereum then runs a light client *of B3* in exactly the pattern B3 runs of Ethereum.
One primitive, both directions, **no trusted signer set in either consensus**.

    mint leg:    ETH sync committee --BLS agg sig--> B3 node verifies (blst)     -> BRIDGE_MINT
    release leg: B3 committee       --BLS agg sig--> ETH contract verifies (2537) -> release()

Bitcoin stays SPV (most-work headers + merkle proof); no BLS needed there.

## 2. What we need — inventory

### 2.1 Cryptography (new in-tree)

| Need | Why | Proposal |
|---|---|---|
| **BLS12-381**, Ethereum variant: pubkeys in G1 (48 B), signatures in G2 (96 B), hash-to-curve RFC 9380 `BLS_SIG_BLS12381G2_XMD:SHA-256_SSWU_RO_POP_`, proof-of-possession scheme | verify `SyncAggregate`; later sign/verify B3 checkpoints | vendor **`blst`** (supranational; the library every Ethereum client uses; C + asm, portable C fallback, Apache-2) under `src/blst/` at a pinned commit, like `secp256k1`/`minisketch`. Build with `__BLST_PORTABLE__` **on by default** (reproducible, no CPU-feature divergence); the ADX path is opt-in. CMake target `b3_bls`, behind `-DWITH_BRIDGE=ON`, OFF in v1 release builds. |
| **Keccak-256** | Ethereum addresses, receipt/log hashing, MPT node hashing | ~40 lines: `Keccak256` class over the existing `KeccakF` in `src/crypto/sha3.cpp` (padding `0x01`, same rate). In-tree `SHA3_256` is *not* usable (padding `0x06`). |
| SHA-256 | SSZ `hash_tree_root`, beacon domains | already in-tree |
| SSZ Merkle branch verification (generalized indices) | finality branch, next-sync-committee branch, execution-payload branch | small header-only `modern/ssz.h` |
| RLP decoder + Merkle-Patricia proof verifier | prove a receipt (Lock event) under `receipts_root` of the finalized execution payload | header-only `bridge/mpt.h`; bounded sizes, fail-closed |

No other new cryptography. No zk circuits in v1 of the bridge (see §7 alternatives).

### 2.2 Consensus objects (modern era, separate bridge activation)

All of these are new **ACTIONs / policy state**, never `CTxOut` fields, never a change to
block hashing, era selection, genesis, or anything ≤ H.

1. **`BRIDGE_BACKED` issuance mode** (genesis record `issuance_mode = 3`, already
   RESERVED). `mode_params` = `{origin_chain_id u64, origin_contract 20 B,
   bridge_instance 32 B, light_client_id u8, max_mint_per_block u64?}` (cap field is
   OPEN). `AssetIdV1` formula unchanged (tagged hash ‖ chain domain ‖ outpoint ‖
   H(genesis)) so the origin domain is part of the id per contract §21 — Ethereum-USDT
   and Arbitrum-USDT can never collide. Supply under this mode may exceed the genesis
   `max_supply` **only** via `BRIDGE_MINT` below; `max_supply` acts as the hard ceiling.
2. **Light-client state** in chainstate (rebuildable, undo on disconnect, like the
   asset registry): per `light_client_id` — `{current_committee (512×48 B + root),
   next_committee?, finalized_header, period, fork_version_table}`. Bootstrapped from a
   **hard-coded trusted checkpoint** in chainparams at bridge activation (block root + committee root),
   exactly as every Ethereum light client does. Re-bootstrap rule = OPEN.
3. **`LIGHT_CLIENT_UPDATE` action** carrying an Ethereum `LightClientUpdate`
   (attested header, sync aggregate, finalized header + branch, next committee + branch
   at period boundaries). Rules: finalized-only advancement; participation ≥ threshold
   (Ethereum safety argument needs > 2/3 of 512; exact number OPEN); monotone slots;
   period handover only with a valid next-committee branch. **Size:** a handover is
   ~24.6 KB, above `MAX_CREATION_ACTION_PAYLOAD` (4,000 B) — this needs its **own
   block section with its own cap** (e.g. ≤ 1 handover per block, ≤ 32 KB), not the
   creation-action section. Anyone may relay (permissionless); a valid update is not a
   privilege.
4. **`BRIDGE_MINT` action** (implemented as type-10 `MINT`): the proof binds a finalized
   Ethereum execution header to the receipt trie, the canonical receipt key,
   and one exact log index. The node decodes the deployed vault event
   `Deposit(uint64 indexed depositId, address indexed token, uint256 amount,
   bytes32 b3Recipient)`, requires the exact approved vault and token, converts
   raw units exactly, and records the replay key
   `(origin_chain_id, vault_address, deposit_id)`. The older
   `H(origin_chain_id ‖ tx_hash ‖ log_index)` sketch is superseded.
5. **Managed withdrawal burn** (implemented as type-10
   `MANAGED_WITHDRAWAL`): one exact canonical bUSD BURN output binds the raw
   amount and Ethereum destination. Its unique request id is the full
   evidence-bearing transaction id plus burn-output index, tracked with
   connect/undo/reindex replay. Transition-v1 managed release uses the
   normative workflow in the threat model; the operator-side finality wait,
   Ethereum call, durable consumption database, and reconciliation service are
   not implemented here.
6. **B3 BLS committee (release leg)** — superseded in detail by
   [b3-finality-to-ethereum.md](b3-finality-to-ethereum.md) (the full B3 → Ethereum
   communication layer: finality gadget, validator-set handover, `B3FinalityVerifier.sol`,
   withdrawal accumulator, ZK-forward prover seam). Original sketch kept for history: a registration binding a
   validator/FN-seat key to a 48-byte BLS pubkey **with proof-of-possession** (rogue-key
   defence). STAKE v1 params are 32 + 2 B, so this is a **separate registration action /
   STAKE v2 params — a Modern-PoS ruling is required** (the V1 spec M1–M6 is frozen;
   this document does not alter it). Per-epoch committee root committed in the modern
   block (FN seat sets are already recorded per epoch — Decision 7 — so FN seats are the
   natural committee). Members BLS-sign `TaggedHash("B3/BRIDGE/CHECKPOINT") ‖
   ModernChainDomain ‖ epoch ‖ block_hash ‖ exit_root`; the aggregate is **off-chain
   data** relayed to Ethereum, not a B3 consensus rule.

### 2.3 Off-consensus / other-chain components

- **Relayer**: stateless, permissionless; pulls `LightClientUpdate`s from any beacon
  API, builds proofs, submits B3 transactions. Reference implementation in
  `contrib/bridge/` (Python), not in the node.
- **Ethereum contracts** (`contrib/bridge/eth/`): `B3LightClient.sol` — stores the B3
  committee (pubkeys), verifies aggregate checkpoints with EIP-2537, rotates committees on
  a signed handover (mirror of the sync-committee rule); `B3Bridge.sol` — `lock(token,
  amount, b3_recipient)` emits the Lock event, `release(proof of burn under exit_root of a
  verified checkpoint)` pays out, nullifiers by burn id. Gas estimate: two pairings +
  up to 512 G1 adds ≈ 0.3–0.5 M gas per checkpoint — acceptable at one checkpoint per B3
  epoch; can be reduced later by proving the aggregate in a SNARK (out of scope).
- **Transition-v1 supersession**: the published vault's immutable EOA is the
  disclosed managed release authority. It is not rotatable in the vault and
  cannot become the future verifier in place. Moving to a verifier means a new
  vault and, under the current identity formula, a new `AssetId` plus explicit
  burn/swap/reissue and reserve migration.

## 3. How we do it — staged build order

Each stage is a small, independently buildable commit series; stages 1–3 are
**test-only / header-only** (like FlowMesh) and can land at any time without touching
consensus or the v1 release.

| Stage | Content | Gate |
|---|---|---|
| 0 | Owner rulings on §8 (at minimum: blst dependency yes/no, Ethereum L1 as origin, committee = FN seats) | — |
| 1 | Vendor `blst` (pinned), `Keccak256`, CMake `WITH_BRIDGE` (OFF by default); unit tests against Ethereum **consensus-spec-tests** BLS vectors (sign/verify/aggregate/fast-aggregate-verify, PoP) and Keccak KATs | `ctest` green, release build unchanged |
| 2 | `modern/ssz.h`, `bridge/mpt.h`, `bridge/rlp.h`; tests with captured mainnet fixtures (one finalized block: header, receipts-root branch, a USDT `Transfer`/`Lock` receipt proof) — captured **offline**, checked in as hex | tests only |
| 3 | `bridge/eth_light_client.h` (pure functions: `ProcessUpdate`, `VerifyFinality`, `VerifyCommitteeHandover`) + fixture-driven tests across one period boundary and one fork-version boundary | tests only |
| 4 | Consensus objects of §2.2 items 1–5 behind a separate bridge activation, light-client/anchor/nullifier/cap/withdrawal-request state, undo/reindex, and mempool/miner/asset integration | **implemented in the transition tree**; state is rebuilt in memory from activation, pruning is refused, and production pins/review remain gates |
| 5 | BLS committee registration (needs PoS ruling), checkpoint signing in the staking loop, `B3LightClient.sol` + `B3Bridge.sol`, Sepolia/Holesky end-to-end with TEST tokens | owner ruling + testnet only |
| 6 | Separate mainnet bridge activation pin, trusted bootstrap checkpoint in chainparams, first bridged asset registration (canonical USDT-ETH-L1) | all bridge readiness, audit, and managed-redemption gates green |

## 4. What this changes, and what it does not

- Does **not** change FlowMesh's "no BLS" decision (DEX register D-3; `certificate.h`):
  action auth and seat certificates stay BIP340. With `blst` in-tree, certificate
  aggregation becomes *possible* later — that would be a separate owner decision, not a
  side effect. **Reported, not resolved.**
- Does **not** make any bridge a dependency of FlowMesh (L-6 stands: bUSD first).
- Does **not** touch the legacy era, genesis, block hashing, or anything ≤ H.
- Adds one consensus-critical third-party library (`blst`). Determinism is the risk:
  pinned commit, portable build by default, vectors in CI on every platform we ship.

## 5. Threat notes (to keep §45 "explicit")

- **Sync-committee trust**: 512 sampled validators, ~27 h rotation; a > 1/3 corrupt
  committee can sign a false header. Mitigations: finalized-only, high participation
  threshold, per-block mint caps, optional watcher veto window (OPEN).
- **Long-range / bootstrap**: the hard-coded checkpoint is a trust root like a Core
  `assumevalid`; re-bootstrap after a long outage must be a rule, not a manual patch.
- **Ethereum forks/upgrades**: fork-version/domain table must be updatable by B3
  release (consensus constant), or the light client freezes at the next hard fork.
- **Issuer freeze (USDT/USDC blacklist)**: locked tokens can be frozen at the origin
  contract; the B3 asset then trades against a frozen reserve. Policy OPEN (OD-8).
- **Rogue-key attack on the B3 committee**: proof-of-possession mandatory at
  registration.

## 6. Cost/size sanity

- `SyncAggregate` verify: 2 pairings + ≤ 512 G1 adds ≈ 2–3 ms with `blst` portable on
  an M4 core; one per relayed update, so negligible for block validation.
- Chainstate: light-client state ≈ 50 KB per origin; nullifiers 32 B each.
- Block space: handover ≤ 32 KB once per ~27 h; mint proof ≈ 1–3 KB.

## 7. Alternatives considered

- **Federation / MPC signer set in consensus** — rejected by OD-8 ruling.
- **zk light client (SNARK of the sync-committee verification)** — smaller on-chain
  cost, far larger engineering and a prover dependency; reasonable *later* optimization
  for the Ethereum side, not a v1 path.
- **Pure-C++ BLS instead of `blst`** — fewer build surprises, much higher correctness
  risk on consensus-critical pairing code; rejected.
- **Arbitrum / L2 origin** — adds rollup-state proof + BoLD delay or sequencer trust;
  OD-8 already recommends L1.

## 8. Historical open list — current status is governed by OD-8

1. Vendor `blst` (portable default) — yes/no.
2. Origin chain for the first bridged stablecoin: Ethereum L1 (recommended) — confirm.
3. First assets: USDT, USDC, both? Names/decimals are display only; ids are `AssetIdV1`.
4. Release-leg committee: FN seats per epoch (recommended) vs stake-weighted validators;
   and the Modern-PoS ruling that adds a BLS-key registration (STAKE v2 params or a new
   registration action).
5. Sync-committee participation threshold (Ethereum spec minimum is > 2/3; recommend
   ≥ 2/3 + finalized-only).
6. Mint caps (per block / per epoch), watcher veto yes/no and window.
7. Bootstrap checkpoint procedure and re-bootstrap rule.
8. Issuer-freeze handling (contract §45).
9. Separate block section + cap for `LIGHT_CLIENT_UPDATE` (proposed ≤ 32 KB, ≤ 1
   handover per block) — confirm.
10. Separate bridge activation height — never implied by FlowMesh A3.

---

## 9. Execution status addendum (2026-08-24)

This 2026-08-24 table is itself historical. The current decentralized
supersession is summarized in the status notice above and in
`b3-finality-to-ethereum.md`; managed-v1 statements below record the path that
was reviewed at that date rather than the current production target.

Owner ruling 2026-08-24: **deposit legs first — ETH → B3, then BTC → B3**
(recorded in b3-open-decisions.md OD-8). Against the §3 staged order:

| Stage | Status |
|---|---|
| 1 (blst, Keccak-256) | **COMPLETE** — landed earlier by the Modern PoS finality work (`src/blst` v0.3.17, `crypto/bls.{h,cpp}` with the Ethereum ciphersuite DST, `crypto/keccak256.{h,cpp}`) |
| 2 (RLP/MPT; SSZ) | **EXECUTED 2026-08-24** — `bridge/rlp.h` (canonical-strict), `bridge/mpt.h` (inclusion-only), `bridge/ssz.h`; mainnet-anchored fixtures captured by `contrib/b3bridge/` (receipts trie of block 25811248 rebuilt in full and matched to the header root) |
| 3 (eth_light_client.h) | **EXECUTED 2026-08-24** — `bridge/eth_light_client.h` (InitStore/VerifyUpdate/ProcessUpdate; finalized-only; supermajority default 342/512; proven execution payload headers; Altair/Electra gindices by fork epoch) verified END TO END on real mainnet data: bootstrap + full 512-member aggregate + committee rotation across periods 1836→1837 (fork Fulu), plus `bridge/deposit.h` (strict receipt decode + vault Deposit extraction) and `contracts/B3DepositVault.sol` (source; compile/deploy = owner/CI). The one-block cross-anchor: the receipts fixture block IS the light-client-proven finalized block, so the tests exercise signature → finality proof → execution proof → receipts_root → MPT proof → receipt → event extraction as one chain. |
| 4 (consensus wiring, separate bridge activation) | **IMPLEMENTED / MAINNET GATED** — canonical bounded type-10 bootstrap, update, mint, execution-backfill, and managed-withdrawal records feed consensus validation; each has one exact zero-value policy-9 `BRIDGE_RECORD` metadata output so standard `SIGHASH_ALL` binds the canonical record without `OP_RETURN` or a custom sighash. Exact OWNER mint, nullifier/caps, exact bUSD BURN request, undo/reindex replay, mempool, miner, and asset conservation are wired. State is in-memory and rebuilt from activation; configured bridge nodes refuse pruning because no durable sidecar exists. Adapter enforcement, operator release automation/request-consumption storage, audits, and all production checkpoint/fork/cap/activation/rules/X pins remain gates. |

BTC → B3: inbound SPV verification is designable on the same pattern, but the
BTC **custody** model for a two-way peg (threshold-Schnorr committee vs
federation) has no ruling yet and nothing was built (OD-8 note of 2026-08-24).
