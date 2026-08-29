# B3 Hive v1.0.0

B3 Hive is the new desktop wallet and full node for the B3 FlowMesh network.

## Important network boundary

This release preserves normal legacy-chain and wallet operation through height
**810,000**. It deliberately fails closed at height **810,001** until the
mandatory X-pin follow-up is distributed. Modern PoS, FlowMesh, FN issuance,
and bridge functionality remain inactive in this release.

## Highlights

- Redesigned B3 Hive desktop interface and corrected B3 branding.
- Existing B3 datadir and wallet migration support.
- Mainnet-tested legacy send and receive behavior.
- Hardened update notification and download framework.
- Cross-platform Linux x86-64, Windows x64, and macOS arm64 packages.

## Before upgrading

Back up `wallet.dat` and close the old B3 client before installing B3 Hive.
These v1.0.0 packages are not signed with a trusted publisher certificate, and
the macOS app is not notarized. Verify the downloaded package with the supplied
platform checksum file or the consolidated `SHA256SUMS` file.

The Trade page is a preview. Qt staking controls and all modern-era protocol
components remain inactive until their separately reviewed activation release.
