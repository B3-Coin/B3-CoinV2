# B3 bridges — BLS as the keystone (design proposal)

**Status: committed design record (2026-08-23). The DIRECTION is an owner statement
("BLS is the key to bridge"); this document turns it into a concrete plan. Nothing here
is consensus-active, nothing is authorized for `src/consensus` / `src/modern` wiring
until (a) the v1 release (clean H+1) has shipped and (b) the OPEN items in §8 are ruled.
Governed by OD-8 ([b3-open-decisions.md](b3-open-decisions.md)), contract §21/§45/§47,
handoff §3.4–3.6. Where this conflicts with any of those, they govern and the conflict
is reported, not resolved here.**

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

### 2.2 Consensus objects (modern era, activation **A3**)

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
   **hard-coded trusted checkpoint** in chainparams at A3 (block root + committee root),
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
4. **`BRIDGE_MINT` action**: `{asset_id, light_client_id, slot ≤ finalized_slot,
   SSZ branch beacon_block → execution_payload_header.receipts_root, MPT proof of the
   receipt, log_index}`. The node decodes the `Lock(recipient, amount, nonce)` log,
   checks `origin_contract` matches the asset's genesis, mints **exactly** `amount`
   (decimals-normalized) to `recipient` (a B3 policy output the log names), and records
   the nullifier `H(origin_chain_id ‖ tx_hash ‖ log_index)` in chainstate. Double mint is
   a consensus failure. Caps/watcher veto = OPEN.
5. **`BRIDGE_BURN` action**: destroys `amount` of the asset and names an origin-chain
   destination (20 B). Each block commits a **bridge exit root** (Merkle root of its
   burns) so the release leg has something to prove.
6. **B3 BLS committee (release leg, later sub-phase)**: a registration binding a
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
- **Until the B3 committee exists**: the OD-8 interim (rotatable `signer_set` in the
  bridged asset's mutable state) remains the release-leg fallback; the `AssetId` does not
  change when the leg upgrades.

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
| 4 | Consensus objects of §2.2 items 1–5 behind `A3`, chainstate for light-client state + nullifiers, undo/reindex, activation regtest functional tests with a **mock origin** (fixture updates fed through RPC) | after v1 release; gate script green |
| 5 | BLS committee registration (needs PoS ruling), checkpoint signing in the staking loop, `B3LightClient.sol` + `B3Bridge.sol`, Sepolia/Holesky end-to-end with TEST tokens | owner ruling + testnet only |
| 6 | Mainnet A3 pin, trusted bootstrap checkpoint in chainparams, first bridged asset registration (USDT-ETH-L1 recommended) | four release gates analogue |

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

## 8. OPEN — owner decisions needed before code beyond stage 3

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
10. A3 activation height — after v1, never before a clean H+1.
