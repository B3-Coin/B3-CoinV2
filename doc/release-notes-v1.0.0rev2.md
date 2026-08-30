# B3 Hive v1.0.0rev2

This is the second packaging revision of B3 Hive 1.0.0. The numeric client
version remains 1.0.0, and the P2P protocol versions are unchanged; the `rev2`
suffix distinguishes these packages from the earlier release builds.

## Legacy-wallet recovery

This release adds `importlegacywalletdump`, a recovery command for the
human-readable private-key file produced by the old B3 client's `dumpwallet`
command. It is intended for the uncommon case where direct wallet migration is
blocked by a damaged legacy transaction record.

The importer validates the complete dump before adding any key, rejects damaged
or wrong-network records without printing private keys, preserves both legacy
P2PKH and bare-P2PK ownership, and performs one full-chain rescan. Re-importing
the same file is safe and does not duplicate keys.

Recommended recovery procedure:

1. Back up the old `wallet.dat` and close B3 Hive.
2. Open the old B3 client, fully unlock the wallet (not staking-only), and run
   `dumpwallet "<safe-path>"` in its debug console.
3. Close the old client. In B3 Hive, create a new descriptor wallet and unlock
   it if encrypted.
4. Run `importlegacywalletdump "<safe-path>"` in the B3 Hive debug console and
   keep B3 Hive open until the rescan completes.
5. Confirm the recovered balance and make a new encrypted wallet backup. Securely
   archive or destroy the plaintext dump because it contains private keys.

Direct migration remains preferred when it works. A legacy dump contains keys
and labels, but not transaction comments, HD derivation, watch-only or multisig
scripts, locked-coin state, or the original keypool ordering.

## Packages

- Linux x86-64
- Windows x86-64 (`win64`)
- Windows 32-bit x86 compatibility build (`win32`, Windows 10 or newer)
- macOS Apple Silicon (`arm64`, macOS 15 or newer)
- macOS Intel (`x86_64`, macOS 15 or newer)

The Win32 compatibility pipeline builds every payload with the i686 toolchain,
verifies the PE architecture, and runs all five command-line programs under
Windows WoW64 before publication. Qt does not list 32-bit x86 as an officially
supported Windows target, so use Win64 when the computer supports it. Install
either Win32 or Win64, not both against the same B3 data directory.

## Important network boundary

This release preserves normal legacy-chain and wallet operation through height
**810,000**. It deliberately fails closed at height **810,001** until the
mandatory X-pin follow-up is distributed. Modern PoS, FlowMesh, FN issuance,
and bridge functionality remain inactive in this release.

Automatic update installation is not active in these packages. Users must
manually install the mandatory X-pin follow-up from the official B3-CoinV2
GitHub release before the network can advance beyond height 810,000.

## Before upgrading

Back up `wallet.dat` and close every old B3 client before installing B3 Hive.
These packages are not yet signed with a trusted Windows publisher certificate,
and the macOS app is not notarized. Verify the downloaded package with the
supplied platform checksum file or the consolidated `SHA256SUMS` file.

The Trade page is a preview. Qt staking controls and all modern-era protocol
components remain inactive until their separately reviewed activation release.
