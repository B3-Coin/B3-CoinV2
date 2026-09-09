# Windows FN operator test build (1.1.5-dev)

This is an unsigned development artifact, not a release or a consensus upgrade.
GitHub's `release-build` workflow with `windows_build_only=true` compiles and
packages Windows only; `desktop_build_only=true` builds Windows plus both Macs.
Neither mode compiles tests, runs tests or publishes a release. A successful cross-build is not proof of Windows
runtime testing. Record the source commit and check the downloaded hashes.

## Included scope

- Verified asset names, tickers and precision, with the full asset ID retained.
- Asset Send/Receive and exact integer asset RPC amounts supplied as JSON
  numbers or strings. Native B3 deposit amounts retain decimal B3 semantics.
- FN public-key/market diagnostics and reviewed binding, arm and disarm actions.
- Confirmation of node-global FN controls, stale-state guards and persistent
  warnings if a temporary spending unlock cannot be restored.
- A selected-wallet FlowMesh workspace for reviewed deposits, limit orders,
  cancellation, admission, withdrawal requests and certified checkpoint/vault
  publication. See [the test workflow and units](flowmesh-qt-test.md).

The new recovery prototypes, relayer changes, automatic FN peer discovery and
any changes to validator membership/quorum are excluded. Existing recovery
features inherited from v1.1.4 are unchanged. Market-readiness checks remain
mandatory: enabling the forms does not create an FN quorum or counterparty
liquidity. The old chart preview is not a live price feed.

## Initial Windows checks

1. Back up the wallet and preserve all signing journals. Close the old wallet
   cleanly before using the same data directory. Do not run two installations
   with the same validator keys.
2. Confirm the version and source build, then check that Assets displays the
   expected balances, full IDs, names and decimals. tUSD must say it is unbacked
   test money; a ticker is not proof of backing.
3. Check FN status with no wallet loaded, with a watch-only wallet, and with an
   encrypted spending wallet. Public status must not prompt for a passphrase.
4. On isolated regtest wallets, exercise Send review/cancel, FN binding
   review/cancel, wallet switching and window closure. A previously locked
   wallet must be locked again after cancellation or failure. Keep any relock
   warning visible and investigate before continuing.
5. Test node-global FN start/stop confirmation and stale-state rejection.
   `armed` is not proof that a seat is mature, selected or producing signatures.

Creating a new FN permanently destroys B3; binding an existing FN is a separate
fee-paying transaction. Binding preparation creates a stored BLS key, so make
a fresh wallet backup even if the prepared binding is not broadcast. Merely
installing this package authorizes no mainnet transfers or FN creation.

## Focused local qualification

Build and run the focused FN operator, FN panel lifecycle, asset transfer,
staking-unlock and UI preview tests, plus the exact-amount wallet RPC and FN
service key-control tests. Use an isolated test environment, never the live
wallet data directory. Record failures and the exact tested source tree.
