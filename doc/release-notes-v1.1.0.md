# B3 Hive v1.1.0 — Transition Release

B3 Hive v1.1.0 resumes the chain after the sealed legacy height and carries
the consensus rules for the temporary PoW corridor, Modern PoS, historical FN
Genesis, permissionless modern FN creation, simple-v1 colored assets, and
FlowMesh v1 spot trading. Every post-transition feature remains fail-closed
until its separately pinned activation height and required mainnet constants
are present. This stable release includes every transition-hardening fix from
the beta.3 audit; beta.1, beta.2, and beta.3 remain separate historical
prereleases and are not replaced in place.

## Important activation sequence

- Legacy B3 ends at height 810,000. The release pins that block's exact hash
  (`2413ba59476afb9a01b971c350b2c5a51494b37925055be42dde774f30d865c6`)
  and the supply-derived Modern PoS initial reward (19,836,712,254 base
  units).
- Block 810,001 starts the 1,000-block modern-format PoW corridor and must
  include the complete historical FN Genesis manifest in its coinbase.
- Historical FN units go directly to their legacy P2PKH owner scripts. There
  is no claim transaction, proof, deadline, or issuance fee. Ordinary
  30-block coinbase maturity applies.
- Modern PoS starts at height 811,001.
- Permissionless modern FN PoD activates at A1 = 812,000.
- Simple-v1 colored-asset issuance and FlowMesh seat/vault preparation activate
  at A2 = 813,000.
- FlowMesh trading, checkpoints, and vault settlement activate at A3 = 815,000,
  after a 2,000-block preparation runway. Each market's epoch-zero anchor is the earliest canonical
  block at or after `market.created_height` whose post-block FN seat set has at
  least four seats; sequence zero waits until that exact block is 30-deep.

## FN and asset behavior

- FN is one indivisible asset capped at 5,000 units. Modern PoD destroys
  native B3 at the ruled 15,000 / 30,000 / 60,000 B3 tier schedule.
- Simple-v1 asset genesis creates the full fixed supply exactly once and pays
  1,000 native B3 to the treasury. No later mint operation exists.
- FN and colored assets move under the recipient's ordinary owner script;
  native B3 pays transaction fees.
- Typed asset outputs use the modern B3A1 Policy Output envelope and do not use
  `OP_RETURN`. Burns use the unspendable typed BURN policy.

Wallet RPCs include `getassetstate`, `getwalletassets`, `issueasset`,
`sendasset`, `burnasset`, and `createfncoin`.

## Wallet and asset safety

- The Qt **Assets** page reads wallet-owned FN and colored outputs, includes
  immature FN Genesis units, displays the full asset id, and lets an owner
  paste an asset id to filter the wallet's assets. Closing a wallet safely
  detaches its Assets data source, and refreshes preserve the selected asset.
- RPC and Qt use the same exact spendability result across wallet locks,
  locked coins, rescans, conflicts, and policy-output provenance. Watch-only
  keys, locked private keys, incomplete multisig, unsupported MuSig, and
  unrelated scripts are not presented as spendable merely because the wallet
  recognizes part of an owner script.
- B3 witness addresses are not active in v1.1.0. Default receive and change
  addresses remain legacy P2PKH, including after legacy-dump recovery or wallet
  migration, and explicit bech32/bech32m recipients are rejected clearly.
  Existing direct or P2SH-wrapped witness-owned native, colored, and FN outputs
  remain visible but are excluded from spendable balances and selection.
  Ordinary legacy P2SH remains valid; use an explicit legacy P2PKH address for
  new payments and asset ownership.
- Ordinary and BASIC-filter rescans discover post-810,000 B3A1 policy outputs
  by their embedded owner script. Pruned-fund imports use the proved block
  height, and fee bumping refuses asset, stake, and MPA transactions rather
  than rebuilding them without their policy meaning.

Historical FN Genesis outputs are coinbase outputs. They appear confirmed but
immature first and become wallet-selectable at depth 31. After importing a
historical key, the manual recovery fallback is `rescanblockchain 810001` once
the node is fully synchronized.

## Transition and network hardening

- Pre-boundary 80008 peer connections are recycled at H so upgraded peers use
  the modern protocol. A lagging legacy node can still initiate a connection to
  an upgraded archival peer and download the sealed history.
- Restart and reconnect repeat the deterministic boundary, codec, target,
  nonce, timing, FN Genesis, stake, payload, and finality-form checks. Ordinary
  non-B3 test/regtest networks retain their stored-header proof-of-work check.
  The atomic post-H validation marker and off-X recovery behavior are described
  under **Upgrade safety** below.
- Post-H coinbases cannot create STAKE outputs, including in the temporary PoW
  corridor. Ordinary corridor transactions can create the stakes needed for
  Set0.
- STAKE owner suffixes may not be P2SH, witness programs, or another B3 policy
  carrier. Asset owners likewise cannot nest a STAKE or metadata carrier,
  because an extra carrier layer could prevent the intended key authorization
  from running. The built-in Stake page and `createstake` use safe legacy P2PKH.
  Because the corridor was already live when this audit fix was made, every
  confirmed post-H STAKE and asset creation must pass the owner-shape scan in
  the release runbook before v1.1.0 is tagged.
- Witness-bearing transactions are refused before entering B3's mempool while
  witness commitments remain inactive. Block assembly repeats this guard so a
  stale entry restored from an older build cannot poison mining templates.
- `getfinalitystatus.active` means Modern PoS is actually active. At height
  811,000 it exposes the exact Set0 preview that will govern block 811,001.

## FlowMesh v1

- FN holders bind seats during the A2 preparation window. At least four active
  seats are required for a market; otherwise that market pauses safely without
  stopping the B3 chain. At least three seat operators must arm their FlowMesh
  validators. For immediate A3 operation, the first colored-market deposit and
  four seats must be present by height 814,970 so their anchor is 30 blocks
  deep.
- Ordinary user deposits are refused before A3 and whenever the matching
  market runtime is unavailable or paused. At A2, only an explicit
  `market_bootstrap` request may create the first colored deposit needed to
  establish a new market; B3 cannot use this bootstrap exception.
- A changed seat set takes control only after the outgoing set's exact handoff
  checkpoint is itself 30 blocks deep. A shallow reorg can therefore republish
  the handoff without stranding either committee.
- FlowMesh uses its dedicated authenticated peer messages for fast microblocks.
  DEX vault UTXOs are typed B3A1 Policy Outputs; checkpoints and vault proofs
  are typed MPA records. Neither uses `OP_RETURN`.
- Spot trades charge the ruled 0.01% native-B3 fee, split 80% equally across
  active FN seats and 20% to treasury. Native B3 pays B3 transaction fees;
  FlowMesh can trade B3 against registered colored assets.
- A validator returning after a long outage checkpoints certified withdrawal
  effects in deterministic chunks of at most 4,096, split only at B3 block
  boundaries. This is certified-log batching, not base-chain transaction
  batching; the withdrawal publisher remains strictly sequential.
- Withdrawal requests are admitted only while pending obligations fit the
  deterministic capacity of the largest 64 live pool UTXOs. Payouts select
  amount-descending UTXOs, then outpoint for ties. The publisher sends one
  withdrawal, waits for confirmation, refreshes capacity, rebuilds, and only
  then sends the next.
- After each ordinary slot, treasury fees flush by the largest positive amount
  that fits anchored native capacity after existing pending native withdrawals.
  A partial flush is valid; zero capacity never blocks trading.
- Before signing, a validator atomically stores the exact candidate and its
  authenticated action evidence with the permanent safety lock. After a
  restart it can revalidate and resume that same candidate without risking a
  second vote or waiting for users to resubmit actions.
- A valid signed proposal ahead of a returning validator's local head starts
  the existing bounded catch-up flow, so an honest three-of-four committee can
  recover even if its other up-to-date member is offline.
- v1 deliberately uses permanent per-sequence signing locks and has no
  view-change protocol. A malicious proposer that splits honest seats can halt
  that FlowMesh market, but cannot create two valid B3 checkpoints or fork B3.

## Known operational limitation

Normal relay accepts only the next confirmed modern FN PoD slot. Until a future
mempool state overlay safely tracks consecutive pending slots, only one FN PoD
issuance can wait in the public mempool at a time; another creator must retry
after that issuance confirms. Block consensus still enforces the complete
sequence, disintegration curve, and 5,000-FN cap.

The canonical bUSD identity is Ethereum-mainnet USDT
`0xdAC17F958D2ee523a2206206994597C13D831ec7` held by managed-v1 vault
`0x143F207e23e6aebD7E974be90ac6D434f4c7BFb6`, represented 1:1 in raw
six-decimal units. Managed-v1 withdrawals use the vault's immutable release
authority, but no arbitrary authority payment is a valid redemption. A
confirmed B3 burn request must bind canonical bUSD, the exact raw amount,
Ethereum recipient and unique request id. The exact B3 BURN/request record and
reorg/reindex tracking are implemented; the operator waits the pinned B3
finality depth, releases exactly once, durably consumes the id, and reconciles
reserves against supply. The operator release automation and durable
request-consumption database are not implemented. No confirmed burn means no
release.

bUSD minting remains disabled on mainnet. The current tree has a strict bounded
type-10 bootstrap/update/mint/backfill/managed-withdrawal carrier, exact OWNER
mint and bUSD BURN transitions, light-client/anchor/nullifier/cap state,
undo/reindex replay, and mempool/miner/asset integration. Each record requires
one zero-value policy-9 `BRIDGE_RECORD` metadata output committing its exact
canonical bytes. Managed-withdrawal consensus allows only exact ECDSA
`SIGHASH_ALL` or Schnorr `SIGHASH_DEFAULT`/`SIGHASH_ALL` on every input, so the
withdrawal-recipient commitment is signed without `OP_RETURN` or a custom
sighash; `NONE`, `SINGLE`, and `ANYONECANPAY` are rejected.
State is rebuilt in memory from bridge activation; configured bridge nodes
refuse pruning and snapshots that skip history because no durable sidecar
exists. Activation still requires adapter enforcement, independent
review/audits, reproducible runtime evidence, the operator withdrawal service,
and production checkpoint/fork/cap/approval/activation/rules/X pins.

The deployed vault is immutable. A later decentralized withdrawal design
requires a new audited vault, and the current identity formula makes that a
new `AssetId`. The announced migration must include old-registry cutoff,
late-deposit handling/refunds, burn/swap/reissue of old bUSD, reserve movement
without a duplicate mint claim, and the new vault/verifier/identity pins.

## Upgrade safety

Four confirmed BLS bindings do not bootstrap Modern PoS by themselves. At
least two independently controlled bound validators must each have an unspent
stake of at least 333 B3 included by height 810,980, so it is active in the
block-811,000 Set0 snapshot. At height 811,000,
`getfinalitystatus.set0_preview.ready` must be true before block 811,001 is
attempted. Bootstrap operators must not spend or revoke these stakes until a
qualifying successor set is in force. With exactly two validators, both are
required only while their weights remain balanced; a validator above two-thirds
of total weight can finalize alone.

Back up `wallet.dat` and shut down the old client cleanly before upgrading.
The release uses the existing B3 datadir and supports legacy wallet import and
rescan. A data directory written by the legacy client or an incompatible
pre-transition build needs one full **`-reindex`** (`b3coind -reindex` or
`b3coin-qt -reindex`). Do not substitute `-reindex-chainstate`: the transition
release must rebuild the unversioned block index as well as chainstate. Current
transition-beta indexes do not need a reindex solely for this update. Existing
block files are reused, but the rebuild can take time.

For an existing transition-beta chainstate already past height 810,000, first
startup performs a mandatory one-time level-4 reconnect of only blocks
810,001 through the current tip under the repaired consensus checks. It writes
its versioned marker only after success; this is not a full reindex. Startup
fails safely if those blocks are pruned/missing, the cache cannot complete the
pass, or a block fails the repaired rules. The marker is tied to each coins
database's exact best block and advances atomically with patched writes; an
older beta advancing or rebuilding that database makes it stale, so returning
to this release safely repeats the check.

If an old transition-beta database is actively following a branch now proven
to be off pinned X, the first startup with this release revokes any old pre-pin
marker and safely unwinds that branch using its stored undo data. The canonical
branch is connected under the repaired rules; restart once more to complete the
full post-H pass and write a new marker. This narrow recovery does not apply to
an invalid tip on the pinned-X branch, which stops startup.

The Qt Assets page lists wallet-owned FN and colored assets and allows lookup
by a pasted asset id. A historical FN Coin is initially shown as confirmed but
immature because it was created in the block-810,001 coinbase. If a legacy key
was imported but its FN output is absent after the upgrade is fully synced, run
`rescanblockchain 810001` once in that wallet. Verify every downloaded package
against the published SHA-256 sums.

The seal packet now contains independently verified X and R0, the byte-identical
3,592-row FN manifest (root
`e8f282a7dcaa9a8fbcfcc5c22ba4f456e5b50968fcf899aaacdaca65bef898ec`,
SHA-256 `c80470eec785600f33fa2e69c520ff331c2b354ebf6e0a9bf8cae7d1eb5f9dca`),
the A1/A2/A3 heights, and the final-H three-way equivalence result. The release
must not be tagged or distributed until the isolated shadow-fork rehearsal also
passes. Any bridge activation additionally requires every bUSD security pin
named above; bUSD remains fail-closed independently of native/colored FlowMesh.

## Release integrity

The v1.1.0 tag, package filenames, release notes, and displayed client version
must all carry the exact stable `v1.1.0` version with no prerelease suffix. The
release workflow refuses to overwrite a published release, safely retries a
partial private draft, and gates publication on the complete unit suite, Qt
suite, FN restore/import, finality, and four-node FlowMesh tests.

The macOS arm64 and Intel downloads are GUI-app packages and require macOS 15
or newer. Command-line operator binaries are provided in the Linux and Windows
packages; the fully static Linux package is headless.

All v1.1.0 packages are unsigned. The published SHA-256 list detects a damaged
or substituted download only when users obtain that list through a trusted
project channel; it is not a developer signature and cannot by itself prove
the GitHub release account was uncompromised.

Do not tag or publish v1.1.0 until the live post-810,001 owner-shape scan, exact
tag build, Set0 stake deadline, and mandatory real-history shadow-fork rehearsal
all pass. Back up `wallet.dat`, shut down cleanly, and verify every download
against its published SHA-256 checksum.
