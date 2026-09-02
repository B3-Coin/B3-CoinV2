# B3 Hive v1.1.0-beta.3 — Transition Hardening Beta

Beta.3 is a new, immutable prerelease for the post-FN-Genesis audit fixes. It
does not replace the already published beta.2 binaries or checksums.

The sealed consensus schedule is unchanged: legacy height 810,000 and X,
historical FN Genesis at 810,001, Modern PoS at 811,001, modern FN issuance at
812,000, colored assets and FlowMesh seat preparation at 813,000, and FlowMesh
trading/vault activation at 815,000.

## Wallet and Assets fixes

- The Qt **Assets** page now reads wallet-owned FN and colored outputs instead
  of placeholder rows. It includes immature FN Genesis units, displays the full
  asset id, and lets an owner paste an asset id to filter the wallet's assets.
- Closing a wallet safely detaches its Assets data source, and balance refreshes
  preserve the asset the user selected instead of jumping back to native B3.
- Locked coins, wallet locks/unlocks, rescans, conflicts, and policy-output
  provenance now refresh the same spendability result in RPC and Qt.
- Watch-only keys, locked private keys, incomplete multisig, unsupported MuSig,
  and unrelated scripts are no longer shown as spendable merely because the
  wallet recognizes part of the owner script.
- B3 witness addresses are not active in this release. Default receive and
  change addresses remain legacy P2PKH, including after legacy-dump recovery or
  wallet migration, and explicit bech32/bech32m recipients are rejected with a
  clear error. Existing direct or P2SH-wrapped witness-owned native, colored,
  and FN outputs remain visible but are excluded from spendable balances and
  transaction selection. Ordinary legacy P2SH remains valid: a foreign P2SH
  address cannot be identified as witness-wrapped unless its redeem script is
  already known to the wallet, so operators should use an explicit legacy
  P2PKH address for new payments and asset ownership.
- BASIC-filter rescans no longer skip post-810,000 B3A1 policy outputs whose
  owner script is embedded in the carrier. A restored historical owner can
  discover its block-810,001 FN output. The manual fallback is
  `rescanblockchain 810001` after full synchronization.
- Pruned-fund imports use the proved block height when interpreting modern
  policy outputs. Fee bumping refuses asset, stake, and MPA transactions rather
  than rebuilding them without their required policy meaning.

Historical FN Genesis outputs are coinbase outputs. They appear confirmed but
immature first, and become wallet-selectable at depth 31.

## Transition and network hardening

- Pre-boundary 80008 peer connections are recycled at H so upgraded peers use
  the modern protocol. A lagging legacy node can still initiate a connection to
  an upgraded archival peer and download sealed history.
- Restart and reconnect paths repeat the deterministic boundary, codec, target,
  nonce, timing, FN Genesis, stake, payload, and finality-form checks instead of
  trusting an old in-memory result.
- Bitcoin-style non-B3 test/regtest networks retain their ordinary stored-header
  proof-of-work check during restart; the B3 phase-aware loader no longer
  bypasses that fail-closed corruption check.
- A one-time per-chainstate validation marker is written atomically only after
  every existing post-H block reconnects successfully under the repaired rules.
  Missing/pruned data, insufficient cache, interruption, or invalid data stops
  startup safely.
- A pre-pin database whose active tip lies on a branch now proven off X is
  stripped of any old marker and unwound with its stored undo data before the
  canonical branch is selected. A new marker is deliberately withheld until
  the next clean startup; an invalid active tip on the pinned-X branch still
  stops startup.
- Post-H coinbases cannot create STAKE outputs, including in the temporary PoW
  corridor. Ordinary corridor transactions can create the stakes needed for
  Set0.
- STAKE owner suffixes may not be P2SH, witness programs, or another B3 policy
  carrier. Asset owners likewise cannot nest a STAKE or metadata carrier.
  Behind a carrier prefix, those special forms do not execute their intended
  key authorization and could make a custom-built output spendable without
  the expected signature. The built-in Stake page and `createstake` already
  use a safe legacy P2PKH owner. Because the corridor is live, every confirmed
  post-H STAKE and asset creation must pass the owner-shape scan in the release
  runbook before beta.3 is tagged.
- Witness-bearing transactions are refused before entering B3's mempool while
  witness commitments remain inactive. Block assembly repeats the check, so a
  stale entry restored from an older build cannot make every mining template
  invalid.
- `getfinalitystatus.active` means Modern PoS is actually active, not merely
  that corridor state was readable. At height 811,000 it exposes the exact Set0
  preview that will govern block 811,001.

An incompatible legacy/pre-transition block index needs one full `-reindex`.
Do not substitute `-reindex-chainstate`. A current transition-beta data
directory uses the automatic post-H safety pass and does not needlessly rebuild
the whole block index. See `doc/b3-wallet-migration.md` and
`doc/b3-transition-release-runbook.md`.

## FlowMesh funds safety

Normal user deposits now fail closed until A3 and until the matching FlowMesh
runtime is live and unpaused. At A2, only the explicit colored-market bootstrap
path can create the first deposit needed to establish that market; callers must
request that bootstrap mode deliberately. This prevents ordinary users from
silently placing funds in keyless vaults before enough anchor-final FN seats can
certify a sweep.

FlowMesh finality seats are separate from Modern-PoS validator bindings. A
market needs four anchor-final FN seats, and at least three seat operators must
arm their FlowMesh validators. For immediate A3 operation, the first colored
market deposit and four seats must be present by height 814,970 so their anchor
is 30 blocks deep. Missing this gate pauses that market; it does not stop B3.
The bUSD bridge remains fail-closed in beta.3.

## Known operational limitation

Normal relay accepts only the next confirmed modern FN PoD slot. Until a future
mempool state overlay safely tracks consecutive pending slots, only one FN PoD
issuance can wait in the public mempool at a time; another creator must retry
after that issuance confirms. Block consensus still enforces the complete
sequence, disintegration curve, and 5,000-FN cap.

## Mandatory Set0 operator gate

The four confirmed public BLS bindings are identities, not stakes. By height
810,980, at least two independently controlled bound validators must each have
an unspent, confirmed stake of at least 333 B3. This gives the required 20-block
activation depth for the block-811,000 snapshot. If this is missed, there is no
eligible Set0 and the chain stops safely at 811,000.

Before attempting block 811,001, require:

1. each operator's `getstakinginfo` to show an ACTIVE stake;
2. each operator's `getfinalityinfo` to show `binding.bound: true`; and
3. `getfinalitystatus.set0_preview.ready` to be true at height 811,000.

Do not spend or revoke bootstrap stakes until a qualifying successor validator
set is visibly in force. With exactly two validators, B3 finality requires both
only while their weights remain balanced; a validator above two-thirds of total
weight can finalize alone.

## Release integrity

Beta.3 packages and the displayed client version carry the explicit
`-beta.3` suffix. The release workflow refuses to overwrite any published
release, safely retries partial private drafts, and gates publication on the
full unit, Qt, FN restore/import, finality, and four-node FlowMesh suites.
The macOS arm64 and Intel downloads are GUI-app packages and require macOS 15
or newer. Command-line operator binaries are provided in the Linux and Windows
packages; the static Linux package is headless.

All beta.3 packages are unsigned. The published SHA-256 list detects a damaged
or substituted download only when users obtain that list through a trusted
project channel; it is not a developer signature and cannot by itself prove
the GitHub release account was uncompromised.

This remains a prerelease. It must not be tagged or published until the live
post-810,001 corridor is scanned, the exact-tag build passes, at least two Set0
stakes satisfy the deadline, and the mandatory real-history shadow-fork
rehearsal is complete. Back up `wallet.dat`, shut down cleanly, and verify every
download against its published SHA-256 checksum.
