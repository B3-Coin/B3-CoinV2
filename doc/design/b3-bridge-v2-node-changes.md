# B3 Bridge V2: node changes

> **Status: PROPOSAL, no governing authority.** It ranks below every owner
> ruling and every reviewed, committed governing document. Nothing here is
> built, audited, deployed or activated, and no consensus parameter changes in
> code.
>
> - 2026-10-03, revision 10 (cross-document fixes). Base `b4b1de4`, branch
>   `feat/flowmeshV2-dev-claude`.
> - Follows the owner's 2026-10-03 direction as relayed (not yet in the repo).
>   The CLAUDE.md workspace lock (`CLAUDE.md:5-7`, `:11`) is unreconciled for this
>   branch (architecture document §15 item 2).

**Labels.** [E] established or verified; [I] design choice made here; [H]
estimate or unverified; [computed] arithmetic on cited inputs.
**Default** means a provisional choice under the solve-and-report rule. Repo
facts are `path:line` from the worktree root; external facts are URLs.

**Ownership.** The architecture document
([b3-bridge-v2-arbitrum-proposal.md](b3-bridge-v2-arbitrum-proposal.md)) owns
the architecture, the numbers (its §7), stages S0-S9, milestones RT0-RT10 and
decisions B-1 to B-24. The contracts spec
([b3-bridge-v2-contracts-spec.md](b3-bridge-v2-contracts-spec.md)) owns
**every byte layout a contract hashes or exposes** (its §2.1-§2.2, §3.2, §6.3,
§6.4, §6.6, §7.2). This document mirrors those bytes through shared vectors
and owns the B3 node side: record kinds, registry, gates, the LP rule and
caps. New decisions are N-1 to N-5 (§16).

---

## 0. Summary

B3 today knows one bridge asset, `busd_bridge` (`src/consensus/params.h:248`),
whose AssetId hashes the vault address (`src/modern/bridge_asset.h:24-32`).
This document adds a height-activated **bridge V2 regime** beside it:

1. **One bUSD** with a B3-native AssetId, backed by **vault entries** Vault_E
   (Ethereum, USDT) and Vault_A (Arbitrum One, USDT0), each with its own
   **reserve ledger** and caps (§3, §7, §9).
2. **Ethereum mints** reuse today's receipt path on a fresh light-client store
   (§4).
3. **Arbitrum mints** prove `Outbox.roots[sendRoot] ≠ 0` by L1 storage proofs
   (kind 7), then the deposit message by the Outbox Merkle path (kind 8) (§5).
4. **LP fast-fill** (kind 10): the first valid fill that paid the deposit's
   recipient fixes the payee when the deposit is proven; capped mints wait in
   a FIFO queue (kind 12) (§6).
5. **Every burn names a vault**; B3 refuses it above that vault's reserve,
   beyond what the vault can still pay (the **wind-down bound**), or while
   that vault's **burn gate** is closed (§7, §8). Conflict evidence
   (kind 13) closes both gates for good.
6. **Legacy closure** (§10): from activation every V1 record is invalid.

**Mintability rule [I].** Every check B3 applies to a deposit is implied by
what the vault enforces, so an accepted deposit always mints once proven
(§4, §5.3).

**Numbers.** Architecture §7 owns every latency and cost figure; this
document uses only its block legs (t_chain 2-4 blocks, t_fin 22-32, t_fm
31-32). Caveats: chaining (§11.3) counts inclusion, which a reorg can undo
until finality; FlowMesh deposits and burns need native B3
(`doc/design/b3-master-handoff.md:376-381`; N-3); without an LP (N-2) route
(c) means days, so (b) is the Arbitrum default at opening; inflow above a
vault's cap (~7,700 bUSD a day at 78 s [computed]) queues.

---

## 1. Rulings and conflicts this relies on

- **No signer set inside B3 consensus** (OD-8,
  `doc/design/b3-open-decisions.md:306`): mints come only from Ethereum
  light-client proofs, Arbitrum only through Ethereum-proven Outbox state.
- **One bUSD over L1 and L2 reserves** (owner, 2026-10-03; B-3) **conflicts**
  with OD-8 `:309` (an L2 asset is a distinct identity) and `:306` (a
  vault-bound `AssetId` for a later leg), architecture contract §21
  (`doc/design/b3-architecture-contract.md:373-375`) and CLAUDE.md rules 5-6
  (`CLAUDE.md:92-104`, the managed vault, stale against code pinning the
  keyless vault `0x0778…c64b`,
  `src/consensus/bridge_params.h:42-45`); all need the B-20 reconciliation
  commit (architecture §15 items 1, 3, 5).
- **Genesis, legacy history, H/X and Modern PoS V1 unchanged** (CLAUDE.md
  rules 3-4): V2 state is additive and replay below activation is identical
  (§15). Incomplete mainnet schedules fail closed (§3.4). The V2 regime is
  still a consensus change, so it needs the `CLAUDE.md:12-13` override
  (architecture §15 item 2).
- **Fresh lineage and digest V2** belong to the finality dependency (§7.4).

---

## 2. What the code does today

| Fact | Source |
|---|---|
| One envelope, `BridgeAssetParams`; AssetId binds chain, vault and token; registry id hashes every pin | `src/consensus/bridge_params.h:319-340`; `src/modern/bridge_asset.h:24-32`, `:40-89` |
| Record kinds 1-6 (BOOTSTRAP to BRIDGE_BURN) | `src/bridge/proof.h:74-81` |
| Limits: record 32 KiB; 64 MPT nodes / 11 KiB per proof; 1,024 B per node | `src/bridge/proof.h:37-47`, `:498` |
| Mint path; admission (deployment block, chain, vault, token, RECIPIENT_V1, exact units); nullifier (chain, vault, id) | `src/node/bridge_state.cpp:537-664`; `src/bridge/admission.h:242-274`, `:196-207` |
| Anchors keep `receipts_root` only; MPT verifier is inclusion-only | `src/node/bridge_state.h:63-76`; `src/bridge/mpt.h:17-25` |
| Receipt decoder accepts typed envelopes up to 0x04 only | `src/bridge/deposit.h:74` |
| Light-client updates and mints refused beyond `fork_schedule_valid_through_epoch` (mainnet V1: 479,999) | `src/node/bridge_state.cpp:130-137`, `:559-567`; `src/kernel/chainparams.cpp:243` |
| V1 leaf 128 bytes; root zero before activation, empty-tree root (non-zero) after | `src/modern/withdrawal_tree.h:33-49`; `src/node/bridge_state.cpp:1162-1171`; `src/node/bridge_state.h:370-376` |
| Burn readiness = finality sets and `lineage_broken` only, once per block | `src/node/finality_tracker.cpp:46-63`; `src/validation.cpp:3631-3657` |
| Regtest bridge: synthetic pins, MANAGED_V1 | `src/kernel/chainparams.cpp:1130-1177` |

---

## 3. Unified bUSD and vault registry

### 3.1 Parameters [I]

`Consensus::Params` gains `std::optional<BridgeV2Params> bridge_v2`;
`busd_bridge` stays byte-for-byte unchanged, so its registry id `cc10…6b00`
and AssetId `ad615d31…9cc6` keep validating history
(`src/test/bridge_params_tests.cpp:216-226`).

| Field | Content |
|---|---|
| `activation_height` (**V**) | First height of V2 records and of the V2 withdrawal tree; **must equal each verifier's immutable `BRIDGE_ACTIVATION_HEIGHT`** (§7.4) |
| `burn_height` (**W2**) | First height of V2 burns; W2 ≥ V, default **W2 = V** (B-12), so no minted bUSD waits for a burn height |
| `light_client` | Fresh `EthereumLightClientPins` (`src/consensus/bridge_params.h:174-205`) with an initial horizon, plus an append-only, height-scoped fork-extension list (§12.3) |
| `verifiers` | Per chain: address, runtime code hash, lineage id, genesis set hash, mirrored immutables (expiry, deposit close, minimum epoch, `BRIDGE_ACTIVATION_HEIGHT`) |
| `vaults` | Ordered entries (§3.3) |
| `arbitrum` | Outbox `0x0B9857ae2D4A3DBe74ffE1d7DF045bb7F96E4840` (contracts spec Appendix A), `roots` slot 3, proxy code hash, pinned implementation, pinned rollup proxy (slot 0), pinned bridge (slot 1), `L1_SINK` (contracts spec §6.1), `max_rollup_delay` (§3.4 assertion 5) |
| `exit_forwarder` | F_x factory, `FORWARDER_INIT_CODE_HASH`, exit tag and `dstEid` (contracts spec §7.2, §8) |
| `gate`, `lp` | §8.6; §6 |
| Lineage bounds | 4-64 members, weight ≥ 900, as V1 (`src/kernel/chainparams.cpp:263-265`) |

### 3.2 Identities [I]

- **bUSD V2 AssetId** = `TaggedHash("B3/BRIDGE/BUSD/V2")` over the chain
  domain, a version byte and the decimals (6). It names no vault, so adding or
  retiring a vault never renames balances (consequence: §7.5). Both vaults'
  `B3_ASSET_ID` immutable is this value.
- **Vault entry id** (what records name) = `TaggedHash("B3/BRIDGE/VAULT/V2")`
  over domain, origin chain id, vault, token, decimals, vault runtime code hash
  and proof kind (ETHEREUM_RECEIPT or ARBITRUM_OUTBOX).
- **Registry id V2** (approval identity) = `TaggedHash("B3/BRIDGE/REGISTRY/V2")`
  over every §3.1 pin except V, W2 and the fork extensions, then the hash of
  the extension list. Records never name it, so a new build pins a new value
  without renaming anything; replay below an extension's height is unchanged.

### 3.3 Vault entries

| Field | Vault_E | Vault_A |
|---|---|---|
| Chain, token | 1, USDT `0xdAC17F958D2ee523a2206206994597C13D831ec7` | 42161, USDT0 `0xFd086bC7CD5C481DCC9C85ebE478A1C0b69FCbb9` |
| Decimals origin/asset | 6/6 | 6/6 [H until O11] |
| Vault, code hash | S7 deployment, two independent builds | same |
| Proof kind | kind 3 | kinds 7, 8 |
| Deployment block | Ethereum deployment block | none (check skipped, §5.3) |
| Caps; `MAX_DEPOSIT_RAW` mirror | 10,000 bUSD per block and per mint epoch; 10,000 | same |
| LP fills and mint queue | no | yes |

A V2 entry never stops admitting proven deposits; retirement closes burns
only (§7.5). No rule or release suspends its mints, which the vault cannot
see (architecture §12 row 14).

### 3.4 Records from V, and startup assertions

The type-10 wire is unchanged (`src/bridge/proof.h:24-31`). From V: kinds 1,
2, 4 use the **V2 store** (anchors also keep `state_root`); kind 3 must name a
Vault_E-type entry; kind 5 is invalid; kind 6 is invalid below W2 and from W2
uses §7.2; kinds 7-13 are new. The V1 store, nullifiers and tree freeze at V.
The V2 store starts empty; today's second-bootstrap guard
(`src/node/bridge_state.cpp:407-410`) applies per store. A fresh store is
needed because the V1 pins end at epoch 479,999 (about 2026-10-04T20:00Z)
and changing them would rename V1 ids.

Asserted with `throw` at startup, like `src/kernel/chainparams.cpp:271-273`,
and in unit tests:

1. Mainnet V1 ids unchanged; on mainnet with `bridge_v2` set, every pin
   non-null, distinct vaults, W2 ≥ V above every V1 height, and the build's
   registry id.
2. V equals both verifiers' mirrored `BRIDGE_ACTIVATION_HEIGHT`.
3. Each entry's `MAX_DEPOSIT_RAW` ≤ its block cap ≤ its epoch cap.
4. W2 ≥ R_f; on mainnet digest V2 comes from V2 P3, never staging (B-7).
5. `set_expiry − release_margin − burn_margin − arbitrum_skew >
   max_rollup_delay` (§8.4, §8.6), the delay pinned with `arbitrum` (§3.1):
   mainnet 1,813,632 s > 1,270,080 s (14.7 days); regtest profile R, the test
   rollup's pinned delay, 50,378 s > 35,280 s (regtest plan §3.3 check 3)
   [computed].

---

## 4. Ethereum deposits (Vault_E)

From V a kind-3 MINT naming Vault_E is verified as today
(`src/node/bridge_state.cpp:537-664`) except [I]: V2 store and anchors;
`ExtractDepositAt` on Vault_E; `b3_asset_id` = bUSD V2; per-vault caps; the
amount adds to `reserve_E`. Routes (a) and (b) both arrive here; (b) through
the user's F_d, whose flush makes one `Vault_E.deposit`.

**Mintability.** Vault V2 keeps V1's `Deposit` event and RECIPIENT_V1 check
(contracts spec §6.2), which match B3's decoder exactly (`contracts/src/B3StakerBridge.sol:101-104`;
`src/bridge/admission.h:58-80`); amounts are ≤ `MAX_DEPOSIT_RAW` (assertion
3). Two gaps remain [I]:

- **Receipt size.** Safes, ERC-4337 bundles and EIP-7702 batches add logs to
  the same receipt (modelled 1,100-1,133 bytes with one extra transfer or
  deposit, contracts spec §7.1), above today's 1,024-byte node limit. The V2 store allows the
  **terminal receipt node up to 8,192 bytes** and a receipt proof up to
  16 KiB, keeping 1,024 for branch and extension nodes and the 32 KiB record.
  Contracts spec §6.2 step 2 (C-13) admits only a codeless top-level EOA
  **or an F_d account**, so route (b) still works. A shared test mints a
  receipt at the limit and refuses one byte above.
- **Future transaction types.** The decoder accepts any EIP-2718 type byte
  0x01-0x7f followed by the same four-field receipt RLP
  (https://eips.ethereum.org/EIPS/eip-2718), instead of ≤ 0x04
  (`src/bridge/deposit.h:74`). A future type with a different body fails
  closed and needs a fork extension (§12.3).

**State root in anchors.** `BridgeExecutionAnchor` gains `state_root` (from
`header.execution.state_root`, or RLP field 3 for backfilled anchors). V1
outcomes are unchanged.

**No age limit [E].** Anchors are removed only by reorg undo
(`src/node/bridge_state.cpp:1667-1671`), so any deposit stays provable while
the V2 light client is fresh (`:140-153`) and inside its schedule.

---

## 5. Arbitrum Outbox proofs (Vault_A)

### 5.1 Why [E]

The Outbox maps confirmed `sendRoot → L2 block hash` in `roots` (slot 3),
written only by its `rollup` (slot 0) through `updateSendRoot` and never
deleted; `updateRollupAddress` lets the rollup owner re-point slot 0
(https://github.com/OffchainLabs/nitro-contracts/blob/main/src/bridge/AbsOutbox.sol).
Arbitrum receipts are impractical (up to ~14,400 headers per assertion
against B3's 32; types 0x64-0x6A), so Vault_A calls `ArbSys.sendTxToL1(L1_SINK, data)` once per
deposit (contracts spec §6.3), and B3 proves the message.

### 5.2 Kind 7, ARB_CONFIRMED_ROOT [I]

Body: `anchor_hash` (32), `send_root` (32), the Outbox account proof, and
storage proofs of `roots[send_root]`, slot 0 (`rollup`), slot 1 (`bridge`)
and the EIP-1967 implementation slot `0x3608…2bbc`
(https://github.com/OffchainLabs/nitro-contracts/blob/main/src/bridge/AbsOutbox.sol#L24-L28) [E]. Checks:

1. The V2 light client is fresh and inside its schedule.
2. `anchor_hash` is a retained V2 anchor; its `state_root` is the proof root
   (no ancestry headers).
3. The account exists and its `codeHash` is the pinned proxy code hash.
4. `roots[send_root]` (key `keccak(send_root ‖ uint256(3))`) is non-zero
   (https://eips.ethereum.org/EIPS/eip-1186).
5. Slot 0 equals the pinned rollup proxy, slot 1 the pinned bridge, and the
   implementation slot the pinned implementation
   (https://eips.ethereum.org/EIPS/eip-1967). A mismatch rejects the record:
   route (c) pauses, fail closed, until a hard fork re-pins (B-18).
6. `send_root` is new; store it with (L1 block, B3 height) under undo.

Size about 9-10 KB [H until O12].

**Limits.** Roots are permanent, so a root written under another rollup or
implementation stays acceptable once the pins match again; the rollup's own
implementation is unpinned [H until O10]. The trust root is Arbitrum's
upgrade authority (architecture §9), not the pin.

### 5.3 Kind 8, ARB_MINT [I]

Body: `registry_id` (32), `output_index` (u32; 0xFFFFFFFF when queued),
`send_root` (32), `leaf_index` (u64), `path` (≤ 64 nodes), `l2_sender`, `to`,
`l2_block`, `l1_block`, `l2_timestamp`, `value`, `data` (≤ 512 bytes). Checks:

1. `send_root` is confirmed (kind 7 in an earlier record).
2. `l2_sender` = Vault_A, `to` = `L1_SINK`, `value` = 0.
3. `item = keccak(l2_sender ‖ to ‖ l2_block ‖ l1_block ‖ l2_timestamp ‖ value
   ‖ data)`, `leaf = keccak(item)`; fold the path by the bits of `leaf_index`
   (`leaf_index < 2^len(path)`) to `send_root`
   (https://github.com/OffchainLabs/nitro-contracts/blob/main/src/bridge/AbsOutbox.sol#L247-L294) [E].
4. `data` is the contracts spec §6.3 Deposit message, exactly 192 bytes.
   `maxLpFeePpm` is **never** a reason to reject: `fee_eff = maxLpFeePpm`
   when it is ≤ `max_fee_ppm_ceiling`, else 0, so no fill can match and the
   mint goes to the recipient (contracts spec §6.2 step 4; regtest B-5c).
5. Admission with origin chain 42161 and bUSD V2; the deployment-block check
   (`src/bridge/admission.h:250-252`) is skipped for ARBITRUM_OUTBOX entries;
   nullifier `(42161, Vault_A, depositId)`.
6. **Caps never refuse a kind 8.** B3 consumes the nullifier, resolves the
   payee (§6) and deletes the key's fills. If the mint queue is empty and caps
   have headroom, `output_index` is exactly the bUSD OWNER output to the payee
   and the amount adds to `reserve_A`; otherwise `output_index` is
   0xFFFFFFFF and (payee, amount) joins the queue (§6).

Vault_A's own code implies every rejection reason above, so its accepted
deposits always mint. `l2_timestamp` and `l1_block` are display-only (the
sequencer can skew time −24 h / +768 s). Size ≤ ~2.8 KB.

---

## 6. LP fast-fill (kind 10) and the mint queue (kind 12) [I]

**Fill record.** `registry_id` (Vault_A), `deposit_id`, `claimed_amount`,
`claimed_recipient` (RECIPIENT_V1), `claimed_fee_ppm`, `paid_output_index`,
`payout_recipient`. Valid only if:

1. The entry allows fills, height ≥ V, and the key is not nullified.
2. `1 ≤ claimed_fee_ppm ≤ max_fee_ppm_ceiling`, and `min_fill_raw ≤
   claimed_amount ≤ MAX_DEPOSIT_RAW`.
3. The paid output is a bUSD V2 OWNER output to the claimed recipient with
   `paid × 10⁶ ≥ claimed_amount × (10⁶ − claimed_fee_ppm)`. A fill short of
   its own claim is invalid.
4. **Its bucket is empty**: no stored fill in an earlier block or earlier in
   this block (B-5a). A second fill never confirms, so its payment never
   happens.
5. No kind 8 for the key in the same block.
6. At most `max_new_fills_per_block` fills per entry in the block.

**Buckets.** `TaggedHash("B3/BRIDGE/LPFILL/V2")(key ‖ recipient ‖ amount ‖
fee_ppm)` stores the payout script and height. A fill with a wrong tuple
occupies only its own bucket, so nobody can block a real LP without knowing
the real tuple; anyone who does know it and fills first is simply the LP.

**Payee at kind 8.** If `fee_eff` = 0 the payee is the recipient. Otherwise
B3 computes the bucket from the proven tuple with `fee_eff`; a stored fill
from a **strictly earlier block** within `fill_max_age_blocks` is the payee,
else the recipient. The LP fee is therefore **exactly** `fee_eff` (default
30 bps): LPs compete on speed, not price. Fills and the payee undo together on
reorg.

**Queue.** Each LP-enabled entry keeps a FIFO of (key, payee, amount) under
undo. Kind 12 ARB_ISSUE (`registry_id`, `output_index`) is valid when the
queue is non-empty and caps cover its head; its output is exactly the head's
bUSD OWNER output to the stored payee; the head is popped and the amount adds
to `reserve_A`. Anyone may submit it. Because the payee is fixed at proof
time, a cap backlog delays an LP's repayment but cannot cancel it.

**Defaults:** `max_fee_ppm_ceiling` 10,000 (1%, covering a 14.7-day float at
20% APR, 0.81%); wallet `maxLpFeePpm` 3,000 (B-5); `min_fill_raw` 1,000,000
(1 bUSD); `max_new_fills_per_block` 32; `fill_max_age_blocks` 40,320 (~36
days at 78 s, 28 at 60 s). Worst case 32 × 40,320 ≈ 1.29M stored fills per
entry, ~100 MB [computed; H per bucket size], each paying a B3 fee.

**Policy.** The mempool refuses fills into occupied buckets, for nullified
keys or with a pending kind 8, and evicts them once a block fills the bucket.

**LP risk.** A deposit that never confirms (a sequencer drop), a wrong claim,
and **fill expiry while route (c) proofs are paused**: kind 8 needs only the
deposit's own root, so expiry needs confirmations or kind 7 stopped for ~13
(disputed, 60 s) to ~30 days (normal, 78 s) [computed]; a re-pin should
extend fill ages by the pause.

---

## 7. Reserves, vault-tagged burns and leaf V2

### 7.1 Reserve ledger [I]

Per entry v, under undo: `minted_v` (issued mints), `burned_v` (leaves naming
v), `reserve_v = minted_v − burned_v`, and an ordered index of proven deposit
amounts, by deposit id for Vault_E and by Outbox `leaf_index` for Vault_A, for
invariant R (§8.4). Queued mints count only when issued, so supply ≤
Σ reserve_v.

### 7.2 Burn record (kind 6 from W2)

V1 fields (`src/bridge/proof.h:138-145`): `registry_id` names a vault entry,
the 20-byte recipient is on that vault's chain. Added checks, evaluated per
record (today once per block, `src/validation.cpp:3631-3657`):

1. The output is a bUSD V2 BURN of exactly `raw_amount`.
2. `raw_amount ≤ reserve_v` after earlier burns naming v in the block.
3. **Wind-down bound:** `raw_amount + unreleased_v ≤ observed_balance_v −
   margin_v`, earlier burns naming v in the block included.
4. v's gate is READY (§8).
5. **Optional exit block** (route d3, Vault_E only): `exit_version` (u8 = 1)
   and the F_x parameters p in the contracts spec §7.2 order (dstEid, to,
   maxBridgeFeeBps, feeCap, refundEOA). B3 requires the recipient to equal
   the CREATE2 address of the pinned exit factory, salt
   `keccak256(abi.encode(TAG, p…))` and the pinned init-code hash, and p to
   pass the contracts' `isValid` rules except EOA-ness. p is stored with the
   leaf (whose bytes do not change) and returned by `getbridgewithdrawals`, so
   a lost wallet cannot lock F_x funds (C-7).

**Wind-down terms [I].** All come from v's latest stored observation, which
the gate already requires (E8, A1): for Vault_E the kind 9 RESERVE_E (USDT
balance B, `locked` L, cut = `nextDepositId`); for Vault_A the latest
heartbeat (`tokenBalance` B, `locked` L, cut = its Outbox `leaf_index`).
`P_pre` and `P_post` sum v's proven deposits before and after the cut (the
§7.1 index).

- `observed_balance_v = B + P_post`: deposits proven after the cut reached
  the vault after the observation, so a fresh deposit is redeemable before
  the next observation.
- `unreleased_v = max(0, burned_v − R_v)`. `burned_v` counts every leaf
  naming v, a migration leaf included (which also keeps invariant R true for
  a retired vault). `R_v = max(0, P_pre + C_v − L)` is a
  lower bound on what v had released by the observation: the vault adds each
  received deposit to `locked` and subtracts each release (contracts spec
  §6.2 step 5, §6.4). Unproven deposits and `absorb()` surplus only lower
  R_v, so the bound errs strict; late proofs below the cut loosen it. Leaves
  appended after the observation count in full.
- `C_v` is the amount migrated into v (§7.6), else 0, because a successor's
  `locked` includes it after `absorb()`. A seizure between the migration
  release and that `absorb()` is under-counted by up to C_v; the relayer
  absorbs right after the release (contracts spec §6.4).
- `margin_v` follows §8.6.

Every input is already kept under undo. After a partial seizure, leaves
already appended keep first claim on the surviving balance and new burns take
only what remains (architecture §4, §12 row 7).

### 7.3 Leaf V2

One global sequential id space (< 2³²) and one depth-32 tree certified to
both chains, as V1 (`src/modern/withdrawal_tree.h:33-34`). The leaf is
keccak256 of the contracts spec §6.4 preimage verbatim (180 bytes: tag, bUSD
V2 AssetId, withdrawal id, destination chain id, vault, token, recipient,
amount, B3 height). Tag and length separate it from V1 leaves (128
bytes) and tree nodes (64 bytes); chain id and vault separate the vaults.
B3 must reproduce the contracts spec §6.4 vector
`0x1cb905899ca0c488e158585c0208497d5415d1645035fbca3ca98ee2efb5ad64`
[computed: re-derived here with `cast` from its inputs]; the shared generator
(contracts spec §2.5) pins it before any code. RT4 releases a real leaf from
Vault_E, RT7 from Vault_A.

### 7.4 What this document gives the fresh lineage

`FinalityWithdrawalRootV2(h)` = **zero for h < V**, and the V2 tree root for
h ≥ V (the empty depth-32 root, which is non-zero, until the first burn),
mirroring V1 (`src/node/bridge_state.cpp:1162-1171`). Verifier V2 requires
zero below its `BRIDGE_ACTIVATION_HEIGHT` and non-zero from it (contracts spec
§3.3 step 5), so certificates in [R_f, V) can rotate both verifiers before V.
**V must equal that immutable exactly** and is chosen before the S7
deployment (§12.1). If the release carrying V cannot activate by V, the
verifiers reject every later certificate, so they are abandoned before
deposits open and redeployed with a new V (human intervention).

In return this document reads a `LineageV2View` (bootstrapped, broken, epoch,
set sizes, weights and hashes by epoch, finalized height, handover status) and
`CertIdAt(index)`: B3's own `certId` (contracts spec §2.2) for a grid
checkpoint on its active chain.

### 7.5 Invariant, plain burns and dead vaults

With honest roots each vault holds at least its reserve plus its unreleased
leaves. A plain `burnasset` of bUSD (`src/wallet/rpc/assets.cpp:1090-1122`)
destroys supply without a leaf; the wallet refuses it without
`allow_unredeemable_burn=true`.

**Dead vaults [I].** An entry is **impaired** when a sticky conflict covers
it, the lineage is broken, a proven observation or heartbeat shows its
verifier past `setExpiresAt` (contracts spec I-4), or a release retires it. Burns naming it stay refused, and
`getbridgereserves` reports its reserve as frozen and impaired. Under one
bUSD id, bUSD backed by a dead reserve can still be burned naming a live
vault up to that vault's reserve, so **new depositors' funds backstop past
losses**, first come first served. N-1 asks the owner to accept this with
plain disclosure (default) or to bind bUSD to a backing generation.

### 7.6 Migration leaf (B-23) [I]

A coordinated hard fork (§12.3) may append `(v, v′, h_m)` to a `migrations`
list, v′ being a new entry it pins on v's chain and token. At the first block ≥ h_m with v's gate READY, ConnectBlock appends one leaf V2
naming v, recipient v′'s vault, amount `reserve_v`; then `reserve_v′ +=
reserve_v`, `reserve_v = 0`, and v is retired (§7.5). Nothing is burned, so
supply ≤ Σ reserve_v holds. The contract sees an ordinary leaf; v′'s
`absorb()` counts the tokens (contracts spec §6.2, C-11). It cannot help after
expiry or a halt; RT4 drills it.

---

## 8. Burn liveness gates

### 8.1 Why

A burn is irreversible, and its leaf is paid only if that vault's verifier
is alive and on B3's lineage and the vault still holds the token; today B3
checks only its own sets. Gates never affect mints or past burns.

### 8.2 Kind 9, ETH_STATE_OBSERVATION [I]

Body: `subject` (u8), `anchor_hash`, then account and storage proofs in a fixed
order, all against one retained V2 anchor's `state_root` (as kind 7). `t_obs`
is that anchor's execution timestamp; each subject's anchor block must exceed
the stored one.

| Subject | Accounts (code hash pinned) and slots |
|---|---|
| 1 VERIFIER_E | Verifier V2 slots 0 (Liveness: epoch, rotation and certificate times, `latestBridgeHeight`, status), 1-2 (set hashes), 4 (Latest: height, index), 6 (`latestCertId`) (contracts spec §3.2) |
| 2 RESERVE_E | Vault_E slots 0 (`nextDepositId`) and 1 (`locked`) (contracts spec §6.6) **and** USDT `balances[Vault_E]` (slot 2), in one record |
| 3 TOKEN_FLAGS | USDT slot 0 (`paused`), slot 10 (`deprecated`), `isBlackListed[Vault_E]` (slot 6) |

USDT slots are computed from the verified source
(https://sourcify.dev/server/v2/contract/1/0xdAC17F958D2ee523a2206206994597C13D831ec7?fields=sources)
[H until O11]. Zero values need absence proofs: a new strict
`VerifyStorageValue` in `src/bridge/state_proof.h` returns zero only for the
empty-trie root `0x56e8…b421` with no nodes, an empty branch child, or a
diverging leaf or extension path; every node must be consumed and a present
zero is malformed. `VerifyMptProof` is unchanged.

### 8.3 Kind 11, ARB_HEARTBEAT [I]

Body and checks 1-3 as kind 8, without `output_index`. `data` is the
contracts spec §6.3 Heartbeat (416 bytes: verifier status, epoch, set
hashes, rotation time, expiry, latest bridge height and root, `vaultBlocked`,
`tokenBalance`, `locked`). `seq` must
exceed the stored one, and B3 stores the message's `leaf_index`. Vault_A sends
one on every release and whenever anyone calls `heartbeat()`; the relayer
calls it after each Arbitrum rotation and **at least daily** (architecture
document §4, §10). Heartbeats are 6.4-14.7 days old when B3 sees them.

**Recorded choice [I]:** no `nextDepositId` is added; A4 cuts invariant R by
Outbox position (§8.4), so the payload and its vectors stay unchanged.

### 8.4 Gate rules

`now_eff = max(nTime, V2 light-client finalized execution timestamp)`: PoS V1
block time is exact (`parent + 60 + 30·round`, `src/modern/pos_v1.h:74-79`),
the proven L1 time covers a lagging B3 clock, and a dishonest sync committee
can only close gates early. `e_B3` is the lineage's projected epoch. **Invariant R**
for vault v with observed `locked` L: `L ≥ P_v − burned_v`, P_v summing v's
proven deposits before the observation: Vault_E ids below the observed
`nextDepositId`; Vault_A messages below the heartbeat's Outbox `leaf_index`
(each `sendTxToL1` takes the next position,
https://github.com/OffchainLabs/nitro/blob/master/precompiles/ArbSys.go [E];
a deposit raises `locked` before sending, contracts spec §6.2). It holds
honestly (releases never exceed burned_v) and fails after a forged drain
beyond the unproven-deposit slack.

| # | Condition (all must hold) | Prevents |
|---|---|---|
| C1 | height ≥ W2; entry active and not impaired | — |
| C2 | Lineage bootstrapped, not broken; current and next sets 4-64 members, weight ≥ 900 | Burns no quorum can certify |
| C3 | `height − finalized ≤ 90` | Burns during a stall |
| C4 | No sticky conflict for the lineage or this vault | — |
| E1 | VERIFIER_E observation with `now_eff − t_obs ≤ 2 d` and status = LIVE | Acting blind; halted or cancelled verifier |
| E2 | `now_eff − lastRotationTime ≤ 30 d − 7 d − 1 d` = 1,900,800 s | Expiry before a covering root is relayed |
| E3 | `0 ≤ e_B3 − e_E ≤ 2` | Release delay beyond ~6 h (below) |
| E4 | Observed current and next set hashes equal B3's for `e_E` and `e_E + 1` (next may be zero) | A verifier on another lineage |
| E5 | `latestCertId` = `CertIdAt(Latest.index)`; a proven mismatch sets the **lineage** conflict | A verifier that accepted a non-canonical statement |
| E6 | If `e_B3 > e_E`, the handover into `e_B3` is certified on B3 | Needing a certificate that never formed |
| E7 | TOKEN_FLAGS within 2 d: not paused, not deprecated, not blacklisted | Frozen or redirected releases |
| E8 | RESERVE_E within 2 d; invariant R, a failure setting the lineage conflict | Forged drain; a stale wind-down balance |
| A1 | A heartbeat exists; verifierStatus = LIVE; vaultBlocked false | Halted or cancelled verifier; USDT0 block of Vault_A |
| A2 | `now_eff − lastRotationTime ≤ 1,900,800 − 87,168` = 1,813,632 s (about 20.99 d; 87,168 s is the 24 h + 768 s skew) | Arbitrum expiry before release |
| A3 | `currentEpoch ≤ e_B3`; set hashes equal B3's for `currentEpoch` and `currentEpoch + 1` (next may be zero); a non-zero `latestBridgeHeight` carries B3's root at that height. Else the lineage conflict | Impossible state; other lineage; non-canonical root |
| A4 | Invariant R on the heartbeat (cut by `leaf_index`), a failure setting the lineage conflict | Forged drain |
| A5 | The heartbeat's `tokenBalance` and `locked` feed the wind-down bound (§7.2 check 3) | Burns a seized or burned USDT0 reserve cannot pay |

**Shortfalls bound burns; they do not close the gate.** A balance below
`locked` (E8, A5) raises `margin_v` (§8.6) and §7.2 check 3 limits burns to
what v can still pay; a zero balance with no later deposit refuses them
all. Not sticky.

No epoch-gap rule applies to Arbitrum (heartbeats are a week old); safety
rests on A2. A heartbeat seen at T + 6.4 d (T + 14.7 d disputed) after a
rotation at T keeps the gate open until about T + 21 d.

**Catch-up delay.** A verifier rotates at most once per 6 h (contracts spec
§4) and B3 epochs last ≥ 24 h (~31.2 h at 78 s), so after a relay outage the
gap shrinks ~3 epochs a day at 60 s, 3.2 at 78 s [computed: 4 − 24/31.2], and
a withdrawal waits about (gap − 1) × 6 h once relays resume. E3 = 2 [I] caps
that at ~6 h; a larger gap closes Vault_E burns during catch-up, ~7.5-8 h per
excess epoch [computed]. `bridgewithdraw` shows the delay.

**Residual.** E5, A3 and invariant R do not catch a forged root that has not
yet paid out, or a drain within the slack; that risk rests on the committee
assumption and P3 (architecture document §9).

### 8.5 Kind 13, FINALITY_CONFLICT [I]

Body: one digest-V2 quorum certificate (statement, signing set header,
bitmap, aggregate signature, about 5.4 KB [H]). Valid when it verifies under
the lineage's set for its epoch with B3's own BLS code and its statement
differs from B3's canonical statement at that checkpoint index (an index above
B3's tip is invalid until reached). Effect: a sticky lineage conflict that
closes both gates. Anyone holding the evidence can halt each verifier where
that set is current or next by `reportConflict` (contracts spec §3.4),
stranding uncertified burns (§8.7); owner policy decides only whether the
owner relayer submits it; whether a halt should be permanent stays FD-15
(contracts spec §14).

### 8.6 Where evaluated, and thresholds

`EvaluateBridgeBurnGateV2(entry, state, lineage, height, now_eff) → READY |
NOT_READY(reason) | UNAVAILABLE` replaces `BridgeWithdrawalValidatorSetsReady`
for V2 burns in the mempool (`src/validation.cpp:1244-1279`), ConnectBlock
(`:3631-3657`), the wallet (`src/node/interfaces.cpp:998-1003`) and block
assembly (`src/node/miner.cpp:236-262`). The mempool re-checks after each
block and refuses burns whose gate closes within `-bridgeburnmarginsecs`
(3,600). The wind-down bound (§7.2 check 3) is checked per burn in all
four.

Defaults (B-13): `set_expiry` 2,592,000 s; `release_margin` 604,800;
`burn_margin` 86,400; `arbitrum_skew` 87,168; `max_rollup_delay` 1,270,080
(assertion 5); `observation_max_age` 172,800
(E1, E7, E8); `max_epoch_gap_ethereum` 2; `max_finality_lag_blocks` 90;
`wind_down_margin_ppm` 0 while v's latest observation shows balance ≥
`locked`, else 100,000 (margin_v = 10% of B, rounded up) [I]. A healthy
vault's whole reserve stays redeemable; after a seizure a tenth of what
survives covers appended leaves if the issuer takes more before B3 sees it
(observations are up to 2 days old for Vault_E, 6.4-14.7 for Vault_A). Regtest profile R divides every time threshold here by 36,
including `max_rollup_delay` (35,280 s; regtest plan §3.3 check 3).

### 8.7 What failures cost

**Closing** (no loss by itself): 2 days without an Ethereum observation; no
rotation relayed for ~21-22 days; no newer heartbeat ~14.6 days after the
last seen one (also after a paused rollup or an Outbox re-pin); finality lag
above 90 blocks (~2 h [H]). A stall gives holders no exit window: no burn can
be certified without finality anyway.

**Permanent loss.** Funds leave a vault only by release of a certified leaf
(contracts spec §10), so once a verifier can accept no more roots its
**uncertified reserve is stranded for good**, a loss all bUSD holders share,
first come first served at the other vault (architecture §8.1). Triggers: no
rotation relayed to a chain for 30 days (that vault); a fresh-lineage break,
by default (B-23) 1,440 + ≥ 46,080 blocks after an epoch start, ≥ 33 days at
60 s, so after each verifier's expiry (both vaults; finality §6.3); a
conflict halt (that vault, both if relayed to both); a forged root under a
dishonest quorum (up to the vault's balance). Burns accepted but not yet
certified are lost in each case.

Mitigations (B-23; the residual needs owner acceptance): prompt relays, the
resumable lineage and the migration leaf (§7.6) before a known expiry.
Tether or USDT0 actions can make up to 2 days of Vault_E burns, or a week or
more of Vault_A burns, unpayable before B3 sees them.

---

## 9. Caps (B-14)

Per entry, 10,000 bUSD per block and per mint epoch of 1,440 blocks (raw
10,000,000,000), the mainnet V1 values (`src/kernel/chainparams.cpp:221-225`);
epoch = `(height − V) / 1,440`. The shared counter
(`src/node/bridge_state.cpp:368`) and epoch keys (`src/node/bridge_state.h:358`)
become per entry. A kind 3 over a cap is refused and waits; a kind 8 queues
(§6). Caps bound mint-side losses only, never a forged root.

---

## 10. Legacy bUSD closure (B-19)

From V, kinds 1-6 under V1 semantics are invalid (`legacy bridge registry
closed`). Old bUSD (`ad615d31…9cc6`) stays a valid colored asset
(`src/modern/asset_validation.h:66-69`), labelled "bUSD (legacy V1)", unbacked
and never migrated (O9). Before V, V1 burns are
already invalid while the old lineage is broken
(`src/node/finality_tracker.cpp:50-51`); if O2 shows it intact, a minor
release should make `bridgewithdraw` refuse legacy burns. Fresh-lineage work
must not clear the V1 `lineage_broken` flag before V.

---

## 11. RPC, wallet and relayer

### 11.1 Node RPCs

| RPC | Change |
|---|---|
| `getbridgeinfo` (`src/rpc/blockchain.cpp:4094`) | `v2` object: V, W2, ids, per vault (caps, headroom, reserve, impairment, gate decision, reason, `closes_at`, latest observation or heartbeat), light-client horizon; `legacy` object |
| `getbridgeproofstatus` (`:4639`), `getbridgewithdrawalproof` (`:4887-5144`), `getbridgefinalityproof` (`:4782`) | Vault key and Arbitrum status; V2 leaves per vault against any certified height at or above the leaf; digest-V2 calldata for both chains |
| Light-client RPCs (`:4410-4417`, `:4518-4527`) | V2 store; `require_b3_final` flag |
| New | `getbridgereserves` (minted, burned, reserve, observed balance, `locked`, invariant R margin, headroom, impairment); `getbridgewithdrawals` (leaves and exit p); `getbridgearbitrum` (roots, heartbeat, **mint queue backlog**); `getbridgelpfills`; `getbridgestatedigest` |

### 11.2 Wallet

| RPC | Change |
|---|---|
| `bridgewithdraw` (`src/wallet/rpc/assets.cpp:1985-2190`) | `destination` `ethereum` or `arbitrum`; Arbitrum takes (d2) when Vault_A's reserve, headroom and gate allow, else (d3) with an exit block (§7.2); shows gate reason, `closes_at`, catch-up delay, B3-final timing |
| `claimbridgedeposit` (`:1845`) | Kinds 3, 8 and 12 |
| `submitbridgecarrier` (`:1750`) | Kinds 7-13; V1 or V2 by height (`:780-795`) |
| New `bridgelpfill`, `bridgeforwarderaddress` | Kind-10 builder with §6 policy; F_d/F_x addresses by the contracts spec derivation, checked against its vectors |
| `burnasset`; display | Refuses bUSD without the override; both ids are bridge assets (`src/wallet/interfaces.cpp:189`, `src/qt/b3assetmodel.cpp:89-94`) |

### 11.3 Relayer (`contrib/b3bridge/b3_bridge_relayer.py`, not consensus)

- Mode `decentralized-verifier-v2` (today V1 only, `:784-785`); providers for
  Ethereum and Arbitrum; deposits keyed `UNIQUE(chain, vault, deposit_id)`
  (today `UNIQUE(deposit_id)`, `:354-358`).
- Ethereum: updates, backfill, kind 3; kind 9 after each relayed certificate
  and at least daily (subject 2 at one anchor).
- Arbitrum: Merkle paths (`NodeInterface.constructOutboxProof`, rebuilt from
  logs on regtest); `eth_getProof` at the newest anchor; kinds 7, 8, 11, 12,
  minting every proven deposit promptly; daily `heartbeat()` calls.
- Rotations and burn certificates to both chains (architecture §10).
- **Default chaining (B-16):** dependent B3 records go out once each source is
  on the active chain; a reorg undoes them together. `--wait-b3-final` keeps
  today's behaviour (`:1744-1761`).

---

## 12. Activation, stages, regtest and standing operations

### 12.1 Stages (architecture document §13)

| Stage | Node work | Regtest | Mainnet |
|---|---|---|---|
| S0-S1 | None (owner reads; H1 in the finality dependency) | RT1 | — |
| S2 | Regtest pin file, first for V1 decentralized mode | RT0, RT2, RT3 | — |
| S3 | `FinalityWithdrawalRootV2`, `LineageV2View`, `CertIdAt`; core V2 slice behind the pin file: bUSD V2, Vault_E, V2 store, kinds 1-4, 6 and 9, leaf V2, migration leaf, Vault_E's reserve ledger and wind-down bound (§7.2 checks 2-3), gate rules C1-C4 and E1-E8, and the RPC, wallet and relayer paths RT4 uses | RT4, including a pre-V rotation, a burn through the Ethereum gate and a migration | None |
| S4 | Vault_A and A1-A5, kinds 7, 8 and 10-13, queue, legacy closure, the rest of RPC, wallet and relayer | RT5, RT6, RT7 | Code ships, `bridge_v2` unset |
| S5, S6 | None; then audit, fuzzing | RT8; RT9 drills | — |
| S7 | Choose V (with upgrade lead time), deploy both chains with it, then release `bridge_v2` with every pin, V and W2 | RT10: deploy scripts and pin flow on both anvils | Release (B-21) |
| S8-S9 | — | — | Rehearsal; opening |

Burns to each vault stay closed until that vault's own burn gate passes
(§8.4: Vault_E C1-C4 and E1-E8; Vault_A A1-A5); deposits stay closed until
each verifier accepts a root at or above V.

### 12.2 Regtest pin file [I]

`-b3bridgepins=<path>` (regtest plan §3.3) is honoured only with
`-b3modernregtest -b3flowmeshtest` (`src/chainparams.cpp:44-80`): JSON with
optional `v1_decentralized` (RT3) and `v2` sections carrying every §3.1 field
for anvil (Arbitrum 31338 [I]) and shortened thresholds. Without it, MANAGED_V1 defaults stay
(`src/test/bridge_params_tests.cpp:497-540`). The real light client checks a
synthetic 512-key committee over real anvil blocks; a warped anvil clock
raises `now_eff` and closes gates early.

### 12.3 Standing operations [I]

- **Horizon.** The fresh pins end at a `fork_schedule_valid_through_epoch`
  chosen at S7 as the last epoch of the Ethereum fork schedule then known.
  Each extension appends (B3 activation height, fork version, fork epoch, new
  horizon), applying only to records at or after its height.
- **Each extension, Outbox re-pin or migration pin (§7.6) loosens a validity
  rule, so it is a coordinated hard fork**, not a routine release: at least
  one per Ethereum fork, shipped at least 60 days [I] before the horizon or
  the fork.
- **During a lapse:** Vault_E mints and kinds 7, 8, 9, 11 stop; Vault_E burns
  close within 2 days, Vault_A burns within ~14.6 days. Vaults still take
  deposits, which mint after the fork.
  Wallets and relayers refuse deposits within 7 days of the horizon; F_d
  `notAfter` stops 24 h before it (contracts spec §7.4).

---

## 13. Tests

**Unit (C++):** id vectors; codecs and fuzz targets for kinds 7-13;
`state_proof.h` absence forms, present zero, unconsumed node; kind 7 pin
mismatches (slots 0, 1, implementation); kind 8 checks, fee above the ceiling
minting to the recipient, queue sentinel; kind 10 earliest wins, **second fill
rejected** (earlier and same block), short fill, fee 0, per-block cap, expiry,
eviction; kind 12 FIFO and caps; kind 13 evidence; root **zero at V−1, empty
at V, non-empty after a burn**; receipt node at 8,192 bytes and one above;
receipt type 0x05; exit CREATE2 vector; each gate condition, invariant R by id
and by `leaf_index`, one-anchor RESERVE_E, `now_eff`; **wind-down**: a burn at
the bound accepted and one raw unit above refused (same-block burns counted),
R_v from `locked` and the index, a late proof below the cut loosening it, a
deposit after the cut adding to the balance, margin 0 then 10% after a
shortfall, a migration leaf in `burned_v` and C_v on the successor; fork
extension from its height; incremental equals reindex.

**Cross-language vectors** (contracts spec §2.5): leaf V2, Heartbeat, Deposit
`data`, `certId`, F_d and F_x addresses, Outbox `item` and root.

**Functional (anvil):** RT4 pre-V rotation, route (a) with a real Vault_E
leaf released, the Ethereum gate, migration; RT5 two vaults, burns above
reserve refused (Vault_A at `reserve_A` = 0); RT6 (b) and (d3) with p from `getbridgewithdrawals`; RT7 (c)
with and without an LP, a refused second fill, a backlog repaid to the LP,
heartbeats, a real Vault_A leaf released, and an Outbox upgrade or re-pointed rollup or bridge failing kind
7 closed; RT9 row 7b: after a partial MockUSDT0 burn of Vault_A, burns pass
up to the bound, not one unit above.

---

## 14. Files and effort

V1 bridge consensus code is about 6,700 lines plus 4,760 of unit tests
[computed: `wc -l` over `src/bridge/*.h`, `src/node/bridge_state.*`, the
bridge params, asset and tree headers, and `src/test/bridge_*_tests.cpp`];
the V2 regime is a consensus change of similar size, separate from P3.

| Area (files) | New or changed lines [H] | Review |
|---|---|---|
| Params, ids, assertions (`bridge_params.h`, `bridge_asset.h`, chainparams) | 600-900 | Consensus |
| Storage proofs with exclusion (`src/bridge/state_proof.h`), receipt limits (`deposit.h`, `proof.h`) | 400-600 | Consensus |
| Outbox and kinds 7-13 codecs (`src/bridge/arb_outbox.h`, `proof.h`) | 700-1,000 | Consensus |
| V2 state: store, reserves, fills, queue, conflicts, tree, migration, undo (`src/node/bridge_state.*`) | 2,000-3,000 | Consensus |
| Gates (`src/node/bridge_gate.*`, validation, miner) | 600-900 | Consensus and policy |
| RPC, wallet, Qt | 1,200-1,800 | Policy |
| Relayer (`contrib/b3bridge/`) | 1,500-2,500 Python | Off-consensus |
| Regtest pin file, functional tests | 1,500-2,500 | Test |

Rough effort [H]: 3-5 engineer-months of node consensus work within the
23-39 person-weeks to RT7 (regtest plan §8), plus a 4-8 week external node
audit, separate from the contracts audit; no cost figure without quotes.

---

## 15. Replay safety

Below V nothing changes: `busd_bridge`, its ids, the V1 store and V1 rules are
untouched. Bridge state is rebuilt by replay
with no disk sidecar (`src/node/bridge_state.h:381-386`); fork extensions
apply only from their heights; `getbridgestatedigest` checks reindex. Genesis,
H/X and Modern PoS V1 are not touched.

---

## 16. Owner decisions

B-3, B-5, B-13, B-14, B-16 to B-19 and B-23 take the node defaults in §3,
§5.2, §6, §7.6, §8.6, §9, §10 and §11.3. New decisions:

| ID | Node default |
|---|---|
| **N-1** | Dead-vault losses: keep one bUSD id and disclose that new deposits backstop past losses (default), or bind bUSD to a backing generation |
| **N-2** | Launch LP: an owner/treasury LP seeded through route (a) with ~50,000 bUSD (6.5 days of Vault_A's capped inflow [computed]); until then route (b) is the Arbitrum default |
| **N-3** | First native B3 for a bUSD-only newcomer: a small treasury drop with the mint (default, off-consensus), or minting into a FlowMesh USER_DEPOSIT output (consensus; the uncommitted stable-quote "no-gas onboarding", architecture §15 item 13) |
| **N-4** | Kind 13 conflict evidence and kind 12 mint queue in consensus |
| **N-5** | Fork-extension hard forks at least one per Ethereum fork, 60 days' lead |

---

## 17. Open items

1. **Owner adoption:** the wind-down margin (§8.6) under B-13; N-1 to N-5,
   N-1 needing explicit acceptance.
2. **Re-credit** of burns above a frozen root (contracts spec §10) is not
   adopted.
3. **Owner reads:** O2, O8 (block spacing behind every minute figure), O9,
   O10 (Outbox slots 0-1, implementations, proxy code hash, admins), O11
   (USDT slots; USDT0 decimals, `isBlocked`), O12 (proof sizes).
4. **Unverified [H]:** anvil `eth_getProof` and EIP-2537; old-block proof
   retention; kind 7, 9 and 13 sizes; bucket memory; effort and runtime
   estimates.
