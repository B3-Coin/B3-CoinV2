# B3 Hive v1.1.1-beta.2 — Corrected Validator Bootstrap Beta

This prerelease replaces v1.1.1-beta.1, which must not be distributed or used.
Beta.1 could spend an impractical amount of time at `Loading block index` when
opening an existing full B3 chain database.

Beta.2 rebuilds the block index's existing ancestry shortcuts before checking
the fixed transition anchors. The block-810,001 checkpoint remains a direct
height/hash check against
`913fb38c75e0f12d8d5e6ea65a0ffce33a22a6908392a94661eab7c8506f6014`.
There is no new checkpoint mechanism, consensus change, database conversion,
or reindex requirement in this correction.

The corrected macOS Qt build was tested against the existing mainnet database
through block 810,001 and reached `Done loading`. Focused block-index restart
and skip-list tests also passed. GitHub builds every downloadable platform
package, verifies checksums, and runs the bridge contract gates; the duplicate
extended test-build job is skipped for this exact beta tag.

## Bootstrap operators

Each selected operator should open and unlock the wallet containing the
confirmed finality-key binding, then run this in the Qt debug console:

```text
exportbridgebootstrapidentity
```

The equivalent command-line call is:

```text
b3coin-cli -rpcwallet=<validator-wallet> exportbridgebootstrapidentity
```

Return only the complete public JSON result. Never share a wallet file, wallet
password, seed phrase, validator private key, or BLS private key. After
exporting, do not rotate or revoke the binding until the bootstrap manifest is
either initialized successfully or explicitly abandoned.

This remains an unsigned testing prerelease. It does not enable bridge deposits
or withdrawals and does not replace v1.1.0 as the latest stable release. The
full transition and bridge notes are in `doc/release-notes-v1.1.1.md`.
