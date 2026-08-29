# B3 Hive v1.0.0rev

This is a packaging revision of B3 Hive 1.0.0. The numeric client and protocol
versions remain 1.0.0; the `rev` suffix distinguishes these rebuilt packages
from the original release.

## Corrections

- Recover automatically when an existing legacy B3Coin `peers.dat` cannot be
  read. The regenerable address database is moved to `peers.dat.bak` and a
  clean one is created instead of preventing the wallet from starting.
- Correct the Windows File, Settings, Window, and Help popup menus so their
  actions remain visible with the B3 Hive dark theme.

## Important network boundary

This release preserves normal legacy-chain and wallet operation through height
**810,000**. It deliberately fails closed at height **810,001** until the
mandatory X-pin follow-up is distributed. Modern PoS, FlowMesh, FN issuance,
and bridge functionality remain inactive in this release.

## Before upgrading

Back up `wallet.dat` and close the old B3 client before installing B3 Hive.
These packages are not yet signed with a trusted Windows publisher certificate,
and the macOS app is not notarized. Verify the downloaded package with the
supplied platform checksum file or the consolidated `SHA256SUMS` file.

The Trade page is a preview. Qt staking controls and all modern-era protocol
components remain inactive until their separately reviewed activation release.
