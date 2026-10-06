# B3 Bridge V2: node changes

> **Status: PROPOSAL, no governing authority.** Below every owner ruling
> and every reviewed, committed governing document; nothing built, audited,
> deployed or activated; no consensus parameter changes in code. Revision
> 11, 2026-10-04, the **simpler plan**, base `1d1d6da`, branch
> `feat/flowmeshV2-dev-claude`; revision 10 (two vaults, Outbox proofs, LP;
> this file at `1d1d6da`) survives as later options (§17). It follows the
> owner's 2026-10-03 ruling (architecture document). The CLAUDE.md workspace
> lock (`CLAUDE.md:5-7`, `:11`) is unreconciled for this branch
> (architecture §16 item 2). No RPC was contacted.

**Labels.** [E] established or verified; [I] design choice; [H] estimate or
unverified; [computed] arithmetic or hashing on cited inputs. **Default** =
provisional choice under the solve-and-report rule. Repo facts are
`path:line` at `1d1d6da` (another workflow's uncommitted edits shift
`src/kernel/chainparams.cpp` lines after 1080).

**Ownership.** The [architecture document](b3-bridge-v2-arbitrum-proposal.md)
owns launch scope, trust, numbers (§8), stages and milestones (§14) and
decisions B-1 to B-37; the [contracts spec](b3-bridge-v2-contracts-spec.md)
every contract byte layout except the bUSD V2 AssetId preimage, normative
here (§3.2); the finality dependency H1/H2, the fresh lineage, digest V2
production, B-22 and B-37's binding rule. This document owns the node side:
registry, record kinds, gate, caps, reserve ledger, RPC, wallet and relayer;
node decisions are N-1 to N-5 (§16).

**Numbering.** Sections and record kinds keep revision 10's numbers; kinds
7, 8 and 10-12 stay reserved (§3.4).

---

## 0. Summary

B3 today knows one bridge asset, `busd_bridge` (`src/consensus/params.h:248`),
whose AssetId hashes the old vault `0x0778…c64b`
(`src/modern/bridge_asset.h:24-32`; `src/consensus/bridge_params.h:42-45`).
This document adds a height-activated **bridge V2 regime**
(architecture §4):

1. **One vault entry**: bUSD V2 gets a new AssetId bound to Ethereum, USDT
   and Vault V2, one reserve ledger and caps (§3, §7, §9); from activation
   height V legacy bUSD is frozen (§10).
2. **Mints** reuse the light-client receipt path on a fresh store with
   height-versioned pins (§4, §12.3); routes (a) and (b) both arrive as a
   Vault V2 `Deposit`; B3 never reads Arbitrum or LayerZero.
3. **Burns** append one leaf V2 each, refused above the reserve, beyond what
   the vault can still pay (the **wind-down bound**) or while the **burn
   gate**, fed by Ethereum storage proofs (kind 9), is closed; conflict
   evidence (kind 13) closes it, and deposits, for good (§7, §8). Route (d3)
   adds an exit block naming the user's F_x.
4. Forwarder-aware RPC, wallet and relayer (§11); activation (§12); tests
   (§13).

**Numbers.** Architecture §8 owns every figure; this document uses only its
block legs (t_chain 2-4 blocks, t_fin 22-32, t_fm 31-32). Burns need native B3
(B-17; N-3); inflow above the caps (~7,700 bUSD a day at 78 s [computed:
10,000 per 1,440 blocks]) waits.

---

## 1. Rulings and conflicts this relies on

- **OD-8** (`doc/design/b3-open-decisions.md:306`): mints only from Ethereum
  light-client proofs, no signer set; **not overridden**; its L2-identity
  point (`:309`) no longer applies (architecture §16 item 3).
- **A vault-bound AssetId** (B-3) is what OD-8 (`:306`) and the architecture
  contract (`doc/design/b3-architecture-contract.md:373-375`, `:637-638`) ask
  for; contract §21 needs no override.
- **CLAUDE.md rules 5-6** (`CLAUDE.md:92-104`), already stale (code pins
  `0x0778…c64b`), are replaced by the B-20 commit (architecture §16 item 1).
- **Genesis, legacy history, H/X and Modern PoS V1 unchanged** (rules 3-4):
  V2 state is additive, replay below V identical (§15), incomplete mainnet
  schedules fail closed (§3.4); the regime is still a consensus change under
  the single override (`CLAUDE.md:12-13`; rule 3, `:87-88`; architecture §16
  item 2).

---

## 2. What the code does today

| Fact | Source |
|---|---|
| `BridgeAssetParams`; AssetId binds chain, vault, token; registry id hashes every pin | `src/consensus/bridge_params.h:319-340`; `src/modern/bridge_asset.h:24-32`, `:40-89` |
| Record kinds 1-6; limits 32 KiB record, 64 MPT nodes / 11 KiB, 1,024 B per node | `src/bridge/proof.h:74-81`, `:37-47` |
| Mint path; admission; nullifier (chain, vault, id) | `src/node/bridge_state.cpp:537-664`; `src/bridge/admission.h:242-274`, `:196-207` |
| One per-block counter; epoch counters keyed by (AssetId, epoch), epoch = (height − activation) / 1,440 | `src/node/bridge_state.cpp:368`, `:210-215`; `src/node/bridge_state.h:358` |
| Anchors keep `receipts_root` only; inclusion-only MPT; receipt types ≤ 0x04 | `src/node/bridge_state.h:63-76`; `src/bridge/mpt.h:17-25`; `src/bridge/deposit.h:74` |
| Light client and mints stop after `fork_schedule_valid_through_epoch` 479,999, i.e. 2026-10-04T20:00:23Z [computed] | `src/node/bridge_state.cpp:130-137`, `:559-567`; `src/kernel/chainparams.cpp:243` |
| V1 leaf 128 bytes; root zero before activation, empty-tree root after | `src/modern/withdrawal_tree.h:33-49`; `src/node/bridge_state.cpp:1162-1171` |
| Burn readiness = finality sets and `lineage_broken`, once per block | `src/node/finality_tracker.cpp:46-63`; `src/validation.cpp:3631-3657` |

---

## 3. bUSD V2 and the vault registry

### 3.1 Parameters [I]

`Consensus::Params` gains `std::optional<BridgeV2Params> bridge_v2`;
`busd_bridge` stays byte-for-byte unchanged, so its registry id `cc10…6b00`
and AssetId `ad615d31…9cc6` keep validating history
(`src/test/bridge_params_tests.cpp:216-226`). Fields:

- `activation_height` (**V**): first height of V2 records and tree; **equals
  the verifier's immutable `BRIDGE_ACTIVATION_HEIGHT`** (§7.4).
- `burn_height` (**W2**) ≥ V, default **W2 = V** (B-12). V1's W stays 811,001
  (`src/kernel/chainparams.cpp:270`), which drifts from
  `doc/design/b3-master-handoff.md:115-118` (architecture §16 item 12).
- `light_client`: fresh `EthereumLightClientPins`
  (`src/consensus/bridge_params.h:174-205`) plus an append-only fork-extension
  list (§12.3).
- `verifier`: address, code hash, lineage id, genesis set hash, mirrored
  windows, member bounds (4-64, weight ≥ 900, as
  `src/kernel/chainparams.cpp:263-265`) and `BRIDGE_ACTIVATION_HEIGHT`.
- `vaults`: one entry at launch (§3.3); a migration hard fork may append a
  successor (§7.6).
- `deposit_forwarder` (F_d factory, init-code hash): derivation only, never
  a mint condition; `exit_forwarder` (F_x factory, init-code hash, tag,
  `dstEid` 30110): checked in burns (§7.2).
- `gate`: §8.6.

### 3.2 Identities [I]

- **bUSD V2 AssetId** = `TaggedHash("B3/BRIDGE/ASSET/V2")` over the
  `BridgeAssetIdV1` serialization (`src/modern/bridge_asset.h:24-32`): chain
  domain, version byte 2, origin chain id 1 (little-endian u64), Vault V2,
  USDT, decimals 6 and 6; Vault V2 computes it from `address(this)` as V1
  does (`contracts/src/B3StakerBridge.sol:88`), so the S7 deployment fixes
  it. **Vector** [computed with Python `hashlib`, which reproduces V1's
  `c69c6bd5…61ad`, `src/test/bridge_params_tests.cpp:220`]: mainnet chain
  domain (`contracts/deployments/ethereum-mainnet-v1.1.1.json:5`), vault
  `0x2222…2222` (stand-in), USDT → raw
  `0xdb9e32397f3656fb2317c7829429f430cfefcf665c08a26774cb7b2fb3c76fcd`, a
  shared-generator entry (contracts spec §2.5). **Migration exception:** a
  B-23 successor keeps this id as a constructor argument checked against its
  predecessor (contracts spec §6.1), recorded by its hard fork
  (architecture §5).
- **Vault entry id** (what records name) = `TaggedHash("B3/BRIDGE/VAULT/V2")`
  over domain, chain id, vault, token, decimals, vault code hash and proof
  kind (1 = ETHEREUM_RECEIPT), so a successor differs and later entry types
  stay additive.
- **Registry id V2** (approval identity) =
  `TaggedHash("B3/BRIDGE/REGISTRY/V2")` over every §3.1 pin except V, W2 and
  the extensions, then the extension list's hash. Records never name it.

### 3.3 The vault entry

Chain 1; USDT `0xdAC17F958D2ee523a2206206994597C13D831ec7`; decimals 6/6;
vault and code hash from the S7 deployment (two builds agreeing);
AssetId per §3.2; proof kind 1 (ETHEREUM_RECEIPT), minted by record kind 3;
Vault V2's deployment block (the relayer's first log scan); caps 10,000 bUSD
per block and per mint epoch and `MAX_DEPOSIT_RAW` 10,000 (B-14). Nothing
suspends its mints, which the vault cannot see refused; a sticky conflict
stops deposits on Ethereum instead (§8.5); retirement by migration closes
burns and sweeps later deposits (§7.6).

### 3.4 Records from V, and startup assertions

The type-10 wire stays (`src/bridge/proof.h:24-31`). From V: kinds 1,
2 and 4 use the **V2 store** (anchors also keep `state_root`); kind 3 names
the vault entry and mints bUSD V2; kind 5 is invalid; kind 6 is invalid below
W2 and from W2 follows §7.2; kind 9 (§8.2) and kind 13 (§8.5) are new; kinds
7, 8 and 10-12 are reserved (§17) and invalid. The V1 store, nullifiers and
tree freeze at V; the V2 store starts empty, with the second-bootstrap guard
(`src/node/bridge_state.cpp:407-410`) per store. A fresh store is needed
because the V1 pins end today and changing them would rename V1 ids.

Checked at startup, failing closed like `src/kernel/chainparams.cpp:271-273`,
and in unit tests:

1. Mainnet V1 ids unchanged; with `bridge_v2` set on mainnet, every pin
   non-null, W2 ≥ V above every V1 height, the build's registry id; unset,
   V2 kinds are invalid.
2. V equals the verifier's mirrored `BRIDGE_ACTIVATION_HEIGHT`.
3. `MAX_DEPOSIT_RAW` ≤ block cap ≤ epoch cap.
4. W2 ≥ R_f; on mainnet digest V2 comes from V2 P3, never staging (B-7).
5. Mirrored windows keep the contracts spec §4 orderings and `set_expiry >
   release_margin + burn_margin` (E2 positive).
6. The launch entry's AssetId equals §3.2 recomputed; any other entry is a
   recorded successor carrying its predecessor's id.

---

## 4. Ethereum deposits

From V a kind-3 MINT is verified as today (`src/node/bridge_state.cpp:537-664`:
sync-committee supermajority, execution ancestry, receipt proof, admission,
caps, nullifier) except [I]: V2 store and anchors, `ExtractDepositAt` on
Vault V2, `b3_asset_id` = bUSD V2, and the amount adds to the reserve (§7.1).
Route (a) is an EOA's `deposit` or a transfer to F_d, route (b) USDT0's
adapter paying F_d; a flush is one `deposit`, so B3 sees one `Deposit`.

**Mintability rule [I].** Every check B3 applies to a deposit is implied by
what Vault V2 enforces, since the vault never refunds (architecture §9): it
keeps V1's `Deposit` event and RECIPIENT_V1 check, matching B3's decoder
(`contracts/src/B3StakerBridge.sol:101-104`; `src/bridge/admission.h:58-80`),
and has no LP field. Two gaps remain:

- **Receipt size.** Allowed deposits and flushes are modelled at 629-byte
  receipt leaves [H], at most 1,133 (contracts spec §7.1), over today's
  1,024-byte node limit (`src/bridge/proof.h:45`); the caller rule (C-13)
  keeps out unbounded smart-wallet bundles. The V2 store allows a **terminal
  receipt node up to 8,192 bytes** (the rest margin) and a receipt proof up
  to 16 KiB, keeping 1,024 for other nodes and the 32 KiB record.
- **Future transaction types.** The decoder accepts EIP-2718 type bytes
  0x01-0x7f with the same four-field receipt RLP
  (https://eips.ethereum.org/EIPS/eip-2718); a different body fails closed and
  needs a fork extension (§12.3).

**State root in anchors.** `BridgeExecutionAnchor` gains `state_root`
(`header.execution.state_root`, or RLP field 3 when backfilled) for kind 9.
**No age limit [E]:** anchors leave only by reorg undo
(`src/node/bridge_state.cpp:1667-1671`), so a deposit stays provable while the
light client is fresh (`:140-153`) and inside its schedule.

---

## 5. Arbitrum deposits at launch

None reach B3 directly: USDT0 crosses by its LayerZero OFT to the user's F_d
(architecture §4, §10). Kinds 7, 8 and 11 are reserved (§17 item 1).

---

## 6. No LP at launch

No fill, bucket, payee rule or mint queue; kinds 10 and 12 are reserved (§17
item 3; why, architecture §18). A kind 3 over a cap is refused and
resubmitted later (§9).

---

## 7. Reserve, burns and leaf V2

### 7.1 Reserve ledger [I]

Per entry v, under undo: `minted_v`, `burned_v` (leaves naming v), `C_v`
(migration amounts credited to v, §7.6), `transit_v` (amounts paid toward v,
not yet credited), `reserve_v = minted_v + C_v − burned_v`, and an index of
proven deposit amounts by deposit id (invariant R, wind-down). Supply ≤
Σ (reserve_v + transit_v) (plain burns lower supply only); at launch there is
one v.

### 7.2 Burn record (kind 6 from W2)

V1 fields (`src/bridge/proof.h:138-145`): `registry_id` names the vault entry;
the 20-byte recipient is an Ethereum address. Checks, per record (today once
per block, `src/validation.cpp:3631-3657`):

1. The output is a bUSD V2 BURN of exactly `raw_amount`.
2. `raw_amount ≤ reserve_v` after earlier burns in the block.
3. **Wind-down bound:** `raw_amount + unreleased_v ≤ observed_balance_v −
   margin_v`, earlier burns in the block included.
4. The gate is READY (§8).
5. **Optional exit block** (route d3): `exit_version` 1 and the F_x
   parameters p in contracts spec order (dstEid, to, maxBridgeFeeBps, feeCap,
   refundEOA). The recipient must equal the CREATE2 address from the pinned
   exit factory, salt `keccak256(abi.encode(TAG, p…))` and the pinned
   init-code hash, and p must pass `isValid` except EOA-ness. p is stored with
   the leaf (bytes unchanged) and returned by `getbridgewithdrawals`, so a
   lost wallet cannot lock F_x funds (C-7).

**Wind-down terms [I]**, from the latest RESERVE_E observation (kind 9
subject 2, required by E8): USDT balance B, `locked` L, cut =
`nextDepositId`; `P_pre` and `P_post` sum proven deposits below and from the
cut.

- `observed_balance_v = B + P_post`, so fresh deposits redeem at once.
- `unreleased_v = max(0, burned_v − R_v)`, `R_v = max(0, P_pre + C_v − L)`: a
  lower bound on releases by the observation, since the vault adds deposits to
  `locked` and subtracts releases. Unproven deposits and `absorb()` surplus
  only lower R_v (strict); late proofs below the cut loosen it.
- `C_v` (§7.1) counts only credited amounts, never `transit_v`.
- `margin_v`: §8.6.

After a partial seizure, appended leaves keep first claim (architecture §5,
§13 row 6).

### 7.3 Leaf V2

One global id space (< 2³²) and one depth-32 tree, as V1
(`src/modern/withdrawal_tree.h:33-34`). The leaf is keccak256 of the
180-byte contracts spec §6.4 preimage; tag and length separate it from V1
leaves and tree nodes. B3 must reproduce
`0x1cb905899ca0c488e158585c0208497d5415d1645035fbca3ca98ee2efb5ad64`
[computed: re-derived with local `cast`; its stand-in asset id makes it
independent of §3.2]. RT4 releases a real leaf.

### 7.4 What this document gives the fresh lineage

`FinalityWithdrawalRootV2(h)` = **zero for h < V**, the V2 tree root from V
(non-zero even when empty), mirroring `src/node/bridge_state.cpp:1162-1171`,
and `BridgeFlagsV2(h)`, bit 0 once a sticky conflict at or below h covers
an entry of that verifier (§8.5). Verifier V2 demands that root (contracts
spec §3.3 step 5), so certificates in [R_f, V) rotate it before V. If the
release carrying V cannot activate by V, the verifier is abandoned before
deposits open and redeployed (human intervention). In return this document
reads a `LineageV2View` (bootstrapped, broken, epoch, sets by epoch,
finalized height, handover status) and `CertIdAt(index)`, B3's own `certId`
(contracts spec §2.2) for a grid checkpoint. *Realized:* `CertIdAt(index)`
is no view member but `FinalityCertIdV2(ExpectedFinalizedBlockV2(e, G +
index × interval))` with e the epoch of that height (B3 signs a checkpoint
only in that epoch, so a statement of any other epoch is not B3's), built
inside `VerifierVerdictV2` (E5) and `JudgeFinalityConflictV2` (§8.5);
`BridgeFlagsV2` is `FinalityBridgeFlagsV2` (`src/node/bridge_state.h`).

### 7.5 Plain burns and impairment

A plain `burnasset` (`src/wallet/rpc/assets.cpp:1090`) destroys supply without
a leaf; the wallet refuses bUSD V2 without `allow_unredeemable_burn=true`.
The entry is **impaired** when a sticky conflict covers it, the lineage is
broken, a proven observation shows its verifier not LIVE or past
`setExpiresAt` (contracts spec I-4), or a migration retires it: burns stay
refused and `getbridgereserves` shows the reserve frozen.

### 7.6 Migration leaf (B-23) [I]

A coordinated hard fork (§12.3) may append `(v, v′, h_m)`, pinning v′ on v's
chain and token with v's AssetId. At the first block ≥ h_m with the gate
READY, ConnectBlock appends one leaf V2 naming v, recipient v′'s vault,
amount `reserve_v`, which empties `reserve_v` and adds the amount to
`transit_v′`; v retires. Nothing is burned. Useless after expiry or a halt;
RT4 drills it.

**Credit on proven arrival [I].** A transit amount counts in none of
`reserve_v′`, invariant R or R_v′ until Ethereum counts it: each RESERVE_E
observation of v′ moves `min(transit_v′, absorbedTotal − C_v′)` into C_v′
(contracts spec §6.6), which needs the release and then `absorb()`
(anyone). An observation anchored before `absorb()` thus credits nothing and
cannot fail invariant R; R fails only when `locked` falls below what B3
credited, which needs a release beyond the leaves, so nobody can freeze
v′'s burns this way. Credit never exceeds what `absorb()` counted, so R_v′
stays a lower bound on releases; absorbed strays only credit earlier, at
the sender's cost. A USDT fee's loss stays in transit (`getbridgereserves`).

**Retired vault [I].** v's deposit gate reads only its verifier and USDT
(contracts spec §3.5, §6.2), and its F_d factory is fixed, so v keeps taking
deposits while its verifier lives. B3 mints them (v cannot refund) and
**sweeps** them: at each block where v's gate would be READY but for
retirement, ConnectBlock appends one ordinary leaf naming v that pays v′
the amount proven since the last sweep, adding it to `transit_v′`. Wallets
derive F_d against v′; relayers stop flushing into v's forwarders, whose
funds stay refundable. A deposit whose sweep leaf is never certified before
v's verifier stops is stranded, disclosed in the release notes.

### 7.7 Per-epoch committee keys (B-37)

Today's finality key comes from the identity secret or one imported wallet
key, preferred on a fresh bind (`src/wallet/wallet.cpp:3937-3949`,
`:4030-4031`; `src/wallet/wallet.h:1051-1067`). B-37 (Codex review) adopts
per-epoch keys under their own binding kind; binding rules, including the
double-binding void, are finality §6.4's. Node side [I]:

- **Signer records** per (lineage, epoch): a key from fresh randomness, its
  proof of possession and state, in a signer store outside the wallet file,
  so `backupwallet` copies hold none; the V1 signer keeps wallet keys for its
  unchanged bindings.
- **Erasure:** once the handover out of the committee's last seated epoch
  (carry-overs extend it, finality §6.4) is certified on B3 and 1,440 blocks
  deep, or, if never seated, once its epoch's committee is fixed; no
  verifier condition, since a lagging verifier catches up from certificates
  on B3 and kept keys only widen the forgery window.
- **No bridge-state rule:** C2 counts the lineage's sets, which omit members
  without a fresh binding.

---

## 8. Burn liveness gate

### 8.1 Why

A burn is irreversible and paid only if the verifier lives on B3's lineage
and the vault holds the token; today B3 checks only its own sets. The gate
never affects mints or past burns.

### 8.2 Kind 9, ETH_STATE_OBSERVATION [I]

Body: `subject` (u8), `anchor_hash`, a `verdict` byte, then account and
storage proofs (EIP-1186) against one retained V2 anchor's `state_root`;
`t_obs` is its execution timestamp, and each subject's anchor block must
exceed the stored one. Account code hashes are pinned (OI-3: the verifier's
and vault's may stay unset on 31337 only).

**Wire [E, `src/bridge/proof.h`].** `09 00 || subject u8 || anchor_hash 32
(raw EVM order) || verdict u8 || account_count u8 ||` per account: its
account-proof node list `(u8 count || (u16 len || node)*)`, 1-64 nodes,
≤ 1,024 B each, ≤ 11,264 B in all, then `u8 slot_count` and per slot a
storage-proof node list of 0-64 nodes under the same bounds; lengths are
big-endian and the record is at most 32 KiB. The subject fixes the shape:
VERIFIER_E [verifier: 5], RESERVE_E [vault: 3, token: 1], TOKEN_FLAGS
[token: 3]; any other subject, account count or slot count is malformed.
Values, addresses and slot keys never travel: the pins and the subject fix
the accounts and slots, the proofs carry the values. The **verdict** byte
applies to VERIFIER_E only: bit 0 SETS_MISMATCH (E4), bit 1 CERT_CONFLICT
(E5), any other bit malformed. Consensus recomputes it and it must match
(replay trusts the recorded byte); it is zero for the other subjects and
for a verifier that is not LIVE. A LIVE verifier is judged for e_E in
[e_B3 − 2, e_B3], the fresh lineage retaining Set′_{e−2} for E3's
`max_epoch_gap` of 2; one outside that window is refused, so the gate stays
closed. Kind 9 decodes only under the V2 record set: below V and without
`bridge_v2` it is malformed.

| Subject | Slots |
|---|---|
| 1 VERIFIER_E | Verifier V2 slots 0 (epoch, rotation and certificate times, `latestBridgeHeight`, status, `depositsClosed`), 1-2 (set hashes), 4 (Latest), 6 (`latestCertId`) (contracts spec §3.2) |
| 2 RESERVE_E | Vault slots 0 (`nextDepositId`), 1 (`locked`), 3 (`absorbedTotal`) (contracts spec §6.6) and USDT `balances[vault]` (slot 2), one record |
| 3 TOKEN_FLAGS | USDT code hash (B-24); slots 0 `paused`, 10 `deprecated`, 6 `isBlackListed[vault]` |

USDT slots come from the verified source
(https://sourcify.dev/server/v2/contract/1/0xdAC17F958D2ee523a2206206994597C13D831ec7?fields=sources)
[H until O11]. Zero values need absence proofs: a strict `VerifyStorageValue`
in a new `src/bridge/state_proof.h` returns zero only for the empty-trie root
with no nodes, an empty branch child, or a diverging path; every node must be
consumed and a present zero is malformed.

### 8.3 No Arbitrum heartbeat at launch

Kind 11 and revision 10's rows A1-A5 are reserved (§17 item 1).

### 8.4 Gate rules

`now_eff = max(nTime, light-client finalized execution timestamp)`: PoS V1
block time is exact (`src/modern/pos_v1.h:74-79`), proven L1 time covers a
lagging B3 clock, and a dishonest sync committee can only close the gate
early. `e_B3` is the lineage's projected epoch. **Invariant R:** observed
`locked` ≥ P + C_v − `burned_v`, P summing proven deposits below the observed
`nextDepositId` and C_v only credited amounts (§7.6); it holds honestly, in
transit too, and fails after a forged drain beyond the unproven-deposit
slack.

| # | Condition (all must hold) | Prevents |
|---|---|---|
| C1 | height ≥ W2; entry not impaired | — |
| C2 | Lineage bootstrapped, not broken; current and next sets 4-64 members, weight ≥ 900 | Burns no quorum can certify |
| C3 | `height − finalized ≤ 90` | Burns during a stall |
| C4 | No sticky conflict | — |
| E1 | VERIFIER_E within 2 d (`now_eff − t_obs`), status = LIVE | Acting blind; halted or cancelled verifier |
| E2 | `now_eff − lastRotationTime ≤ 30 d − 7 d − 1 d` = 1,900,800 s | Expiry before a covering root is relayed |
| E3 | `0 ≤ e_B3 − e_E ≤ 2` | Release delay beyond ~6 h |
| E4 | Observed set hashes equal B3's for `e_E` and `e_E + 1` (next may be zero) | A verifier on another lineage |
| E5 | `latestCertId` = `CertIdAt(Latest.index)`, a mismatch setting the sticky conflict | A non-canonical accepted statement |
| E6 | If `e_B3 > e_E`, the handover into `e_B3` is certified on B3 | Needing a certificate that never formed |
| E7 | TOKEN_FLAGS within 2 d: code hash pinned, not paused, not deprecated, vault not blacklisted | Frozen releases; an upgrade by `deprecate` |
| E8 | RESERVE_E within 2 d; invariant R, a failure setting the sticky conflict | Forged drain; stale balance |

**Shortfalls bound burns, not the gate.** A balance below `locked` raises
`margin_v` and check 3 of §7.2 limits burns to what the vault can pay. A
deprecation (E7) closes burns until a ruled re-pin (architecture §5).

**Catch-up.** A relay backlog shrinks ~3.2 epochs a day at 78 s (contracts
spec §4); a withdrawal waits about (gap − 1) × 6 h, capped at ~6 h by E3;
a larger gap closes burns ~7.5-8 h per excess epoch [computed].

**Detectors.** E5, E8 and kind 13 set the sticky conflict; off consensus,
monitors match every mint to a Vault V2 `Deposit` (architecture §13 row 12).
A forged root not yet paid out, or a drain within the slack, rests on the
committee assumption and P3 (architecture §10).

### 8.5 Kind 13, FINALITY_CONFLICT [I]

One digest-V2 quorum certificate signed by the lineage's **current or
next** committee at the including block, valid and differing from B3's
canonical statement at its index (a checkpoint above the parent block is
invalid until reached). Older sets are refused, as `reportConflict`
refuses them (contracts spec §3.4): with no unbonding or
slashing a rotated-out quorum can be cheap to buy and B-37 erasure is
unenforceable; an older conflict that matters reaches Ethereum, where E1 and
E5 catch it. Effect: a sticky conflict closing the gate.

**Wire [E, `src/bridge/proof.h`].** `0d 00 || u16 n || FinalizedBlockV2
(185) || bitmap (ceil(n/8)) || aggSig (96)`, n big-endian and the signing
committee's size, the rest exactly `EncodeCertificatePayloadV2`'s bytes:
4 + 185 + ceil(n/8) + 96, so **286 bytes at n = 4**, at most 293 within the
bridge bound of 64 and 1,309 at n = 8,192 [computed] (replacing revision
11's ~5.4 KB [H] estimate). The codec checks shape only (n in 1-8,192,
exact length, zero bitmap padding, no `bridgeFlags` bit above bit 0) and
decodes only under the V2 record set, so kind 13 is malformed below V and
without `bridge_v2`.

**Judge order [E, `JudgeFinalityConflictV2`, `src/node/bridge_state.cpp`]**,
cheap checks before BLS and BLS before the O(k) canonical rebuild:
(1) modern PoS checkpoint parameters and a pinned chain domain; (2) a
bootstrapped, unbroken fresh lineage; (3) the certificate decodes; (4) its
lineage id is this lineage's; (5) its epoch is e (current committee) or
e + 1 (next): e − 1 and older, and e + 2 or later, are refused; (6) n is
that committee's size; (7) 1 ≤ index < 2³² and H = G + index × interval
fits and is at or below the parent block; (8) a quorum of that committee
signed it (`VerifyFinalityCertificateV2`; the successor field is not
checked, a wrong one being one more way to differ); (9) it differs from
B3's statement (`ExpectedFinalizedBlockV2` at H): an epoch other than that
of H differs by construction; otherwise height, block hash, withdrawal
root, successor and `bridgeFlags` are compared first, and only a statement
equal in all of them reads B3's history root (the finalized checkpoint's
recorded root at its index, else the O(k) rebuild), an equal root being
B3's own statement and refused. The record is valid from V only while no
sticky conflict is set (the first of kind 13, E5 and E8 wins, so a chain
holds at most one) and only with a lineage context (mempool, ConnectBlock,
wallet prevalidation, block assembly); replay trusts the recorded record as
it trusts kind 9 verdicts. The conflict records kind 13, its `certId`,
checkpoint index and epoch, and C4 refuses every later burn, in the same
block too.

**Deposits stop too [I].** B3 never refuses a proven deposit (the vault
cannot refund), so a sticky conflict (kind 13, E5 or E8) stops deposits:
(1) B3's next checkpoint statement sets `bridgeFlags` bit 0 (contracts spec
§2.1, §3.3), and a verifier accepting it closes deposits for good while
certificates and releases go on, so earlier leaves still pay; (2) until
then, about one t_fin plus a relay, wallets refuse deposits and new F_d
addresses, relayers refuse flushes (F_d funds stay refundable) and
`getbridgeinfo` warns; a deposit still made mints unredeemable bUSD,
disclosed; (3) the owner relayer submits `reportConflict` once every earlier
leaf is under an accepted root, or at once if the verifier can accept no
canonical certificate (permanence: FD-15). Residual: a captured quorum may
sign neither flag nor evidence and keep deposits open while it relays; it
could drain the vault anyway (architecture §10). *Realized on regtest:* bit
0 from the first checkpoint at or after the closing block
(`FinalityBridgeFlagsV2`); `getbridgev2info.deposits` reads
`CLOSED_FOR_GOOD`, the wallet's `getbridgedepositrecipient` refuses and
`contrib/b3bridge/b3_bridge_deposit_guard_v2.py` exits non-zero; RT4c
drills it. The closing block also evicts every pending V2 burn and kind 13
record (with descendants) from the mempool; a wallet then frees a burn's
bUSD V2 input with `abandontransaction`. The F_d paths come with S4.
*Implementation choices pending owner review (S3 slice n2b):* bit 0 at a
checkpoint at or after, not strictly after, the closing block (block c's
statement already reflects c's state); wallets and relayers also refuse
deposits on a broken fresh lineage, an impaired verifier (halted,
cancelled or expired) and Verifier V2's own `depositsClosed`, each as
permanent as the sticky conflict; the Wire and Judge order paragraphs above
record the implementation, not an owner ruling.

### 8.6 Where evaluated, and thresholds

`EvaluateBridgeBurnGateV2(entry, state, lineage, height, now_eff) → READY |
NOT_READY(reason) | UNAVAILABLE` replaces `BridgeWithdrawalValidatorSetsReady`
for V2 burns in the mempool (`src/validation.cpp:1244-1279`), ConnectBlock
(`:3631-3657`), the wallet (`src/node/interfaces.cpp:998-1003`) and block
assembly (`src/node/miner.cpp:236-262`), with the wind-down bound per burn in
all four. The mempool refuses burns whose gate closes within
`-bridgeburnmarginsecs` (3,600).

Defaults (B-13): `set_expiry` 2,592,000 s; `release_margin` 604,800;
`burn_margin` 86,400; `observation_max_age` 172,800; `max_epoch_gap` 2;
`max_finality_lag_blocks` 90; `wind_down_margin_ppm` 0 while balance ≥
`locked`, else 100,000 (10% of B, rounded up) [I], covering appended leaves if
the issuer takes more before B3 sees it. Regtest profile R divides time
thresholds by 36 and sets `max_finality_lag_blocks` to 30, scaled to its
5-10 block t_fin (regtest plan §3.3).

### 8.7 What failures cost

**Closing** (no loss by itself): 2 days without an observation; 22 days
without a relayed rotation (E2); finality lag above 90 blocks (~2 h
[computed]). **Permanent loss:** once the verifier can accept no more roots,
the uncertified reserve, with one vault **all unreleased backing**, is
stranded (architecture §9.1): after 30 days without a relayed rotation, a
fresh-lineage break (B-23: 1,440 + ≥ 46,080 blocks after an epoch start, so
after expiry), a conflict halt or a forged root (up to the balance);
accepted but uncertified burns too. Mitigation: B-23 (residual for owner
acceptance). Tether actions can make up to 2 days of burns unpayable before
B3 sees them.

---

## 9. Caps (B-14)

10,000 bUSD per block, per 1,440-block mint epoch (raw 10,000,000,000, the
mainnet V1 values, `src/kernel/chainparams.cpp:221-225`) and per deposit;
epoch = `(height − V) / 1,440`. Epoch counters are keyed by AssetId
(`src/node/bridge_state.h:358`); the per-block counter
(`src/node/bridge_state.cpp:368`) stays shared, as V1 mints are invalid
from V. Routes (a) and (b) share the caps, which bound mint-side losses
only.

---

## 10. Legacy bUSD closure (B-19)

From V, V1-semantics kinds 1-6 are invalid (`legacy bridge registry closed`).
Old bUSD (`ad615d31…9cc6`) is **frozen** (architecture §3): no mint, burn or
redemption; labelled "bUSD (legacy V1)"; unbacked; never migrated;
third-party holders found by O9 get an owner-funded swap off consensus
(B-19). Transfers stay valid as colored-asset outputs from A2
(`src/modern/asset_validation.h:83-88`; 813,000,
`src/kernel/chainparams.cpp:310`); invalidating them would be a further
consensus change nobody asked for [I]. Before V, V1 burns already fail while
the old lineage is broken (`src/node/finality_tracker.cpp:50-51`); if O2
shows it intact, a minor release should make `bridgewithdraw` refuse them;
nothing may clear the V1 `lineage_broken` flag before V. The smoke vault
`0x143F…BFb6` is never registered and is rechecked before activation
(`doc/design/b3-architecture-contract.md:641-644`).

---

## 11. RPC, wallet and relayer

### 11.1 Node RPCs

- `getbridgeinfo` (`src/rpc/blockchain.cpp:4094`): a `v2` object (V, W2,
  ids, caps and headroom, reserve, impairment, gate decision, reason,
  `closes_at`, latest observations, light-client horizon and extensions,
  forwarder pins) and a frozen `legacy` object.
- `getbridgeproofstatus` (`:4639`), `getbridgewithdrawalproof`
  (`:4887-5144`), `getbridgefinalityproof` (`:4782`): V2 deposits; V2 leaves
  against any accepted root at or above the leaf; digest-V2 calldata.
  Light-client RPCs (`:4410`, `:4518`): V2 store.
- New: `getbridgereserves` (minted, burned, reserve, transit, balance,
  `locked`, invariant R margin, wind-down headroom); `getbridgewithdrawals` (leaves,
  exit p); `getbridgestatedigest` (reindex check).

### 11.2 Wallet

- `bridgewithdraw` (`src/wallet/rpc/assets.cpp:1985-2190`): `ethereum` (d1)
  or `arbitrum` (d3: recipient F_x plus exit block); shows gate reason,
  `closes_at`, catch-up delay; refuses legacy bUSD.
- `claimbridgedeposit` (`:1845`): kind 3, V2 store. `submitbridgecarrier`
  (`:1750`): kinds 1-4, 6, 9, 13, V1 or V2 by height (`:780-795`).
- New `bridgeforwarderaddress`: F_d or F_x address and p by the contracts
  spec derivation and vectors; F_d's `refundEOA` proven by EIP-191 (§7.3
  there); **a fresh F_d per call**, `feeCap` per §7.4 there, never re-shown
  once flushed; p sent to the configured relayer endpoints, optional
  `publish(p)`; refused while the entry is impaired (§7.5).
- Display: both ids are bridge assets, V1 as "bUSD (legacy V1)"
  (`src/wallet/interfaces.cpp:189`; `src/qt/b3assetmodel.cpp:89-94`).
- Recipient pre-checks (contracts spec §6.5) need an Ethereum or Arbitrum
  endpoint the node lacks; the wallet takes an optional one, else warns [I].

### 11.3 Relayer (`contrib/b3bridge/b3_bridge_relayer.py`, not consensus)

- Mode `decentralized-verifier-v2` (today V1 only, `:784-785`); one Ethereum
  provider; deposits keyed `UNIQUE(chain, vault, deposit_id)` (today
  `UNIQUE(deposit_id)`, `:358`).
- Light-client updates, backfill and kind 3 promptly; kind 9 after each
  relayed certificate and at least daily.
- F_d: learns p from wallets or `Published` events (contracts spec §7.4)
  and recomputes the address; deploys and flushes within the fee cap after
  checking the entry (never while impaired), freshness, horizon and cap
  headroom. F_x: `release` and `forward` in one `B3RelayBatch` call, the
  F_x fee paying both (B-15).
- Certificates and releases: P-fast at or below 1 gwei, batched above;
  **default chaining**: dependent records go out once each source is on the
  active chain; `--wait-b3-final` keeps today's behaviour (`:1744-1761`)
  (B-16).

---

## 12. Activation, stages, regtest and standing operations

### 12.1 Stages (architecture §14)

| Stage | Node work | Regtest | Mainnet |
|---|---|---|---|
| S0-S1 | None (H1 + H2 in the finality dependency) | RT1 | — |
| S2 | Regtest pin file, V1 decentralized mode first | RT0, RT2, RT3 | — |
| S3 | `FinalityWithdrawalRootV2`, `BridgeFlagsV2`, `LineageV2View`, `CertIdAt`; AssetId, entry, V2 store, kinds 1-4, 6, 9, 13, leaf V2, migration, reserve and wind-down, gate C1-C4 and E1-E8, B-37 signer records, RT4's RPC, wallet and relayer paths | RT4 | None until S5 |
| S4 | Exit block, forwarder paths, legacy closure, remaining RPCs | RT6 | Code ships, `bridge_v2` unset |
| S5, S6 | None; audit, fuzzing | RT8; RT9 | — |
| S7 | Choose V with upgrade lead time, deploy, release `bridge_v2` with every pin | RT10 | Release (B-21) |
| S8, S9 | — | — | Small transfers; pilot caps |

Burns stay closed until the gate passes; deposits stay closed on the contract
until the verifier accepts a root at or above V (contracts spec §3.5).

### 12.2 Regtest pin file [I]

`-b3bridgepins=<path>` (regtest plan §3.3) works only with `-b3modernregtest
-b3flowmeshtest` (`src/chainparams.cpp:44-80`). S2 commit `815bcbf` (after
`1d1d6da`) parses its `format` and `v1_decentralized` sections; V2 adds a
`v2` section with every §3.1 field for anvil 31337. Without the file
MANAGED_V1 defaults stay (`src/test/bridge_params_tests.cpp:497-540`).

### 12.3 Height-versioned light-client pins and standing operations [I]

- **Horizon.** The fresh pins end at the last epoch of the fork schedule
  known at S7; each extension appends (B3 height, fork version, fork epoch,
  new horizon) and applies from its height.
- **Each extension, migration pin or `exit_forwarder` re-pin loosens a
  validity rule, so it is a coordinated hard fork**: at least one per
  Ethereum fork, shipped 60 days ahead (N-5); the others as architecture §11
  lists. A USDT `deprecate` needs a ruled re-pin (B-24).
- **During a lapse** mints and kind 9 stop and burns close within 2 days; the
  vault still takes deposits, which mint after the fork. Wallets and relayers
  refuse deposits within 7 days of the horizon; F_d `notAfter` stops 24 h
  before it (contracts spec §7.4).

---

## 13. Tests

**Unit (C++):** id vectors; kind 9 and 13 codecs and fuzzing; kind 13 from a
rotated-out set refused; kinds 5, 7, 8, 10-12 and V1-semantics records
refused from V; absence-proof forms; root **zero at V−1, empty at V,
non-empty after a burn**; receipt node at 8,192 bytes and one above; receipt
type 0x05; exit CREATE2 vector; every gate condition, invariant R,
`now_eff`; **wind-down**: a burn at the bound accepted and one raw unit above
refused (same-block burns counted), a late proof below the cut, a deposit
after it, margin 0 then 10%, migration and sweeps in `burned_v` and
`transit_v′`, **an observation of v′ between the leaf and `absorb()`
crediting nothing and setting no conflict**, credit after it; the
`bridgeFlags` bit from the first checkpoint at or after a sticky conflict;
legacy bUSD transfers after V; fork extension from its height; incremental
equals reindex.

**Cross-language vectors** (contracts spec §2.5): AssetId V2, leaf V2,
`certId`, F_d and F_x addresses.

**Functional (anvil):** RT4: (a) and (d1) under digest V2 from the staging
signer, a pre-V rotation, a real leaf released, the gate, a reused BLS key
refused, kind 13 then deposits closed by the flag, a migration credited
after `absorb()` and a later deposit into v swept; RT6: (b) and (d3), p from
`getbridgewithdrawals`, an F_d refund, a blacklisted forwarder; RT8 both
under P3; RT9 architecture §13 drills, a partial seizure bounding burns to
the unit; RT10 deploy and pin flow.

---

## 14. Files and effort

V1 bridge consensus code is about 6,700 lines plus 4,760 of unit tests
[computed in revision 10, `wc -l`]. Revision 10's 8,500-13,200 new or
changed lines [H] and 3-5 engineer-months included the removed Outbox
codec, kinds 7, 8, 10-12 and fill and queue state, but not B-37's signer
store and ever-bound key index (§7.7); **not re-estimated** (architecture
§11). New files: `src/bridge/state_proof.h`, `src/node/bridge_gate.*`. Add a
4-8 week external node audit [H].

---

## 15. Replay safety

Below V `busd_bridge`, its ids, the V1 store and rules are untouched. Bridge state is rebuilt by replay with no sidecar
(`src/node/bridge_state.h:381-386`); extensions apply from their heights;
`getbridgestatedigest` checks reindex.

---

## 16. Owner decisions

Carried with node defaults: B-3, B-10, B-12 to B-14, B-16, B-17, B-19, B-22
to B-24 and B-37. Withdrawn by the 2026-10-03 simpler-plan ruling: B-5,
B-18, the B-10 per-chain split, B-13's Arbitrum heartbeats, B-24's USDT0
waiver, N-1 (dead-vault backstop; one vault, §17 item 2) and N-2 (launch
LP; none, §6).

| ID | Status | Node default |
|---|---|---|
| **N-3** | Ruled 2026-10-05 (R10; architecture OI-8) | Native B3 for a bUSD-only user's burn: sponsored fees, a bUSD tip to a relayer that adds the native-B3 fee input (off-consensus; no fee-in-bUSD change), chosen over a starter drop (the earlier default) and fees in bUSD; R10 did not address the FlowMesh USER_DEPOSIT mint (architecture §16 item 13) |
| **N-4** | Narrowed | Kind 13 in consensus (yes); the kind 12 queue withdrawn |
| **N-5** | Open | At least one fork-extension hard fork per Ethereum fork, 60 days ahead |
| **B-37** (new) | Default yes | Signer records outside the wallet file; erasure per §7.7 |

---

## 17. Later options (not at launch)

Each needs its own ruling (architecture §17); revision 10 holds the full rules.

1. **Arbitrum vault V2, routes (c) and (d2):** entry Vault_A (42161, USDT0
   `0xFd086bC7CD5C481DCC9C85ebE478A1C0b69FCbb9`, proof kind ARBITRUM_OUTBOX);
   **kind 7** proves an Outbox root by EIP-1186 under pinned code; **kind 8**
   mints by the Outbox path of Vault_A's Deposit message, nullifier (42161,
   Vault_A, id), ~6.4-6.5 days after the deposit; **kind 11** heartbeats
   feed gate rows A1-A5. Needs O10, O12.
2. **One bUSD across several vaults:** an id naming no vault, per-vault
   ledgers and N-1; overrides OD-8 `:309` and contract §21 (architecture §2
   item 3, §17 item 1); leaving the launch id needs another recorded
   exception or a swap [I].
3. **LP fast-fill (kind 10) and mint queue (kind 12)**, only with item 1.
4. **Staker-signed fast deposits** ([FSD](b3-bridge-v2-staker-signed-deposits.md),
   B-25 to B-36): item 1, penalties, P3 and a B-25 ruling.
5. **An overcollateralized coin:** another product, never bUSD; no node
   change.

---

## 18. Open items

1. Owner adoption: the wind-down margin (B-13), B-22's approval, B-23's
   residual, N-4, N-5 (N-3 ruled by R10).
2. **F_d intake** (contracts spec §7.4): a user without Ethereum ETH needs a
   relayer to accept p; endpoints are relayer configuration, not consensus.
3. Recipient pre-check endpoint (§11.2); re-credit of burns above a frozen
   root (contracts spec §10), not adopted.
4. B-37's rebinding schedule and §7.7's signer store (finality dependency).
5. Owner reads O2, O8, O9, O11 (O10, O12 only with §17 item 1).
6. Unverified [H]: anvil `eth_getProof` and EIP-2537; old-block proof
   retention; kind 9 sizes (kind 13: 286 bytes at n = 4, §8.5); effort.
