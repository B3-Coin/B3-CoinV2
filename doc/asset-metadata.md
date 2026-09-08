# Wallet asset names and verified precision

This is a wallet/display feature, not a consensus upgrade. It does not change
asset IDs, supply, spending rules, transaction amounts, or the issuance fee.
Existing wallets and existing assets remain compatible. Updating display
metadata does not issue another asset or broadcast a transaction.

## What is verified

Simple-v1 issuance commits the maximum supply, display decimals, and fixed
issuance mode. Together with the first input's outpoint and the pinned chain
domain, that genesis record determines the asset ID. The wallet verifies this
commitment before using a custom asset's precision. Decimals must be 0–18.

Names and tickers are **display labels**, not consensus metadata. They are not
proof of an issuer's identity, dollar backing, redemption rights, or ticker
uniqueness. Always identify an asset by its full ID on the intended chain.
Importing another asset under the same display name does not change its ID.
Configured native/FN/bridge identities and bundled registry names cannot be
overridden through custom metadata.

The metadata proof establishes which genesis belongs to the ID. It does not
establish that the issuance is currently confirmed or finalized. Wallet
balances and confirmations still come from normal wallet/chain processing.

## Display behavior

- Native B3, FN, and the configured bridge asset retain their existing names
  and precision.
- A bundled registry resolves known assets by exact chain-bound ID, including
  the tUSD test asset below.
- Wallet-known issuance transactions supply verified precision automatically,
  even if no human-readable name is registered. These show as **Unnamed asset**.
- Imported metadata supplies a local name and ticker plus ID-verified precision.
- An asset without a matching genesis proof remains **Unknown asset**, showing
  exact raw units. Unknown precision is not treated as verified zero decimals.

Qt and `getwalletassets` share the same resolver. RPC balances and all asset
RPC amount arguments remain integer base units; only the GUI formats decimal
amounts. `getwalletassets` retains its existing `decimals` field (zero fallback
when unknown) and adds `precision_known`, `name`, `metadata_source`, and
`test_only`. Clients must check `precision_known` before scaling an unknown
asset. Metadata sources are `consensus`, `bundled-registry`, `local-registry`,
`wallet-issuance`, and `unknown`.

## Register a custom asset locally

Use the full issuance transaction's hex, not a later transfer transaction:

```text
setassetmetadata "ASSET_ID" "Example Asset" "EXAMPLE" "ISSUANCE_HEX"
getwalletassets "ASSET_ID"
clearassetmetadata "ASSET_ID"
```

`setassetmetadata` verifies the issuance proof, saves the label in the selected
wallet, and refreshes the GUI without requiring a new block or restart. It
requires no wallet unlock and spends no coins. `clearassetmetadata` removes
the local label; it does not delete coins or change the immutable genesis.

Metadata survives wallet reload and is included in wallet backups. It is
local to that wallet: it is not automatically published or relayed to other
wallets. A recipient whose wallet never saw the issuance can import the same
public proof and label. There is no new whole-chain scan or network metadata
service. Automatic global name discovery is outside this change.

Names are limited to 64 safe ASCII characters and tickers to 12; configured
names and their normalized aliases are reserved. A proof may be at most
1,000,000 bytes. These limits also prevent display markup and control characters
from becoming wallet labels.

## Send and receive in Qt

Selecting a supported non-native asset enables **Send** for a signing-capable
wallet with a mature confirmed balance. A locked wallet can open the
form, but preparing/signing requires its normal spending unlock; staking-only
operation does not authorize a payment. Watch-only and immature-only holdings
cannot send through this form.

The asset form keeps the full ID and selected wallet visible. Known precision
allows decimal amounts; unknown precision explicitly uses integer raw units.
It prepares once, shows the exact recipient, asset amount and native B3 fee,
then asks for confirmation before submitting the same signed transaction.
An uncertain submission is not retried automatically: check its displayed
transaction ID before making another payment.

**Receive** creates an ordinary B3 owner address and displays the selected
asset's full ID. Give the sender both; do not send a similarly named token on
another chain or native B3 instead. Address creation costs no fee. No new
asset-specific payment URI is introduced that older wallets could misread.

Changing or unloading the selected wallet cancels open asset operations.
Cancellation cannot undo a transaction already submitted. If an operation
temporarily unlocked spending, its lock is restored even when the Qt wallet
model closes before the operation finishes. A wallet that was already fully
unlocked retains that prior state.
FlowMesh deposit and withdrawal controls remain disabled on this page.

The CLI now correctly converts numeric, boolean, and object arguments for
`issueasset`, `getwalletassets`, `sendasset`, `burnasset`, and `createfncoin`.
For example, use `-named getwalletassets minconf=1` to omit the optional asset
filter. Asset IDs, addresses, metadata names, tickers, and proof hex remain
strings, even when they contain only digits.

## Bundled tUSD test asset

- Chain: B3 mainnet.
- Name / ticker: **Test USD / tUSD**.
- Asset ID: `43d4555d04fdb78726381db4e8340c6f59e5761f2945d634aef0a5d4a3c3a299`.
- Fixed supply: `1000000000000` raw units; **1,000,000 tUSD** at six decimals.
- Issuance transaction: `7f9ef293989bc1f335755cdd3783cbd866d84538477148ca8128765b3776608d`.
- Issuance anchor (first input): `59b3897ca1ce201118644247833cb4eba68cc70849a3f593b94d61da46ba150c:0`.
- Serialized genesis: `0010a5d4e8000000060000`.
- Status: **unbacked test asset**, not USDT, not the bridge asset, and not a
  promise of dollar redemption.

The exact-ID entry lets recipient wallets display this existing asset after
upgrading, without reissuing it or paying another issuance fee. A test asset's
display label does not enable FlowMesh trading/deposit buttons or change any
bridge permission.
