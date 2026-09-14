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
  the tUSD and cUSD test assets below.
- Wallet-known issuance transactions supply verified precision automatically,
  even if no human-readable name is registered. These show as **Unnamed asset**.
- A node-wide background cache discovers other simple-v1 issuance proofs from
  contiguous locally available validated modern blocks. New and existing
  wallets use the same verified precision even when the issuance is not in
  their own history.
- Imported metadata supplies a local name and ticker plus ID-verified precision.
- A configured FlowMesh HTTPS endpoint may provide an explicitly published
  display catalog for its listed assets. Trading discovery and market refreshes
  automatically verify and cache this metadata; recipients need no import RPC.
- An asset without a matching genesis proof remains **Unknown asset**, showing
  exact raw units. Unknown precision is not treated as verified zero decimals.

Qt and `getwalletassets` share the same resolver. RPC balances and all asset
RPC amount arguments remain integer base units; only the GUI formats decimal
amounts. `getwalletassets` retains its existing `decimals` field (zero fallback
when unknown) and adds `precision_known`, `name`, `metadata_source`, and
`test_only`. Clients must check `precision_known` before scaling an unknown
asset. Metadata sources are `consensus`, `bundled-registry`, `local-registry`,
`operator-public-catalog`, `endpoint-label: <configured HTTPS origin>`,
`wallet-issuance`, `node-issuance`, and `unknown`.

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

Imported labels survive wallet reload and are included in wallet backups.
They remain local to that wallet: arbitrary peer names/tickers are not trusted
or automatically relayed. The separate explicit public operator catalog below
never exports this wallet-local registry. A recipient can import the same public
proof and label if desired. Reviewed bundled identities include tUSD and cUSD below.

Names are limited to 64 safe ASCII characters and tickers to 12; configured
names and their normalized aliases are reserved. A proof may be at most
1,000,000 bytes. These limits also prevent display markup and control characters
from becoming wallet labels.

## Automatic precision discovery

The node scans available **validated modern blocks**, starting at asset
activation, in a separate interruptible background worker. It stores compact
chain-scoped genesis preimages and a resumable block cursor outside wallet
databases. There is no per-wallet historical scan, no requirement to enable
`txindex`, and no block read or chain synchronization inside a balance lookup.
With contiguous available history from asset activation to the scan cursor,
the cache also discovers later issuances regardless of whether their outputs
belong to a loaded wallet. Qt refreshes when cached precision becomes available,
including when the chain is idle.

Every cached proof is checked against its full asset ID and chain domain.
The scanner pauses at the first missing or pruned block. Until that gap is
restored, it cannot discover issuances in later blocks either, including new
issuances. Previously verified cached precision remains usable; unknown
precision is never guessed.
Cache failures must not stop the blockchain or prevent the wallet from opening.
Manual import and bundled proofs remain available as fallbacks on pruned nodes.
Discovery does not assert that a retained proof's issuance is still confirmed
after a reorg; balances and spendability continue to use normal chain rules.

This discovery concerns immutable **precision**, not arbitrary names, dollar
backing, redemption, or an issuer's identity. Unnamed assets keep their full
asset identity and can display correctly scaled prices without inventing a
ticker. Other wallets need a build containing this feature; it does not change
the behavior of already-running older binaries or the network consensus rules.

## Automatic public labels for FlowMesh clients

Existing bundled tUSD/cUSD identities need no operator catalog. For other
listed simple-v1 assets, the operator may set `flowmeshassetmetadata=assets.json`
in the appropriate network section of `b3coin.conf`. This requires
`enableflowmeshvalidator=1`; sharing uses the existing restricted HTTPS API and
its existing certificate verification. No new public wallet RPC is exposed.
Relative file paths are resolved under the network datadir.

The file is an explicitly PUBLIC JSON array, at most 256 records and 256 KiB.
Each record has exactly these eight fields (replace placeholders with real
public genesis data; integer amounts are atomic units):

```json
[
  {
    "domain": "<64-hex chain domain>",
    "asset_id": "<64-hex AssetId>",
    "name": "Example Token",
    "ticker": "EXM",
    "issuance_txid": "<64-hex first input transaction id of the issuance>",
    "issuance_vout": 0,
    "max_supply": 1000000000,
    "decimals": 6
  }
]
```

`issuance_txid`/`issuance_vout` identify the issuance transaction's FIRST INPUT
outpoint, not the issuance transaction itself or a later transfer. The proof
must reconstruct the exact AssetId on this chain. The entire file is checked
before any entry is published; malformed or duplicate records fail the explicit
startup configuration with a bounded diagnostic. Updating the file takes effect
on the next normal operator restart, not through signing-history resets.

Recipients automatically learn the optional `asset_display` field during
market discovery/snapshot/status reads. Only metadata for the locally established
market's exact base AssetId is accepted. Names/tickers remain endpoint assertions,
not issuer identity, current issuance confirmation, reserves or redemption proof.
Unsafe labels, reserved configured aliases, malformed integers, wrong-chain
proofs and mismatched AssetIds are rejected. Cosmetic rejection does not stop
valid market data, change balances, or alter a signed instruction.

Local and bundled names take precedence. Remote labels are bounded, in-memory
and re-fetched during trading reads after restart. The displayed source identifies
the configured HTTPS origin; previously learned metadata is retained if a later
response omits it. Opening only Assets does not itself make a trading-network
request. This is not general B3 P2P gossip of all token labels.

Metadata changes can appear on an unchanged certified market head without
clearing order inputs or signed request history. Wallet metadata lookups are
cache-only and never wait for HTTP under a wallet lock. The optional wire field
is separate from all signed actions, certificates and consensus commitments;
older operators can omit it and older clients ignore it.

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

## Bundled cUSD test asset

- Name / ticker: **cUSD Unbacked Test / cUSD**.
- Asset ID: `929d3345f4bc08683dabce49cbca6f79cf646621e1f18342713f975dc2111ade`.
- Fixed supply: `10000000000` raw units; **10,000 cUSD** at six decimals.
- Issuance transaction: `eed79c52111c0d8faf367afb8899d83c33c66434cad28602e900ccd1d6e7e260`.
- Issuance anchor: `97976c28709507dc443ecf07d6bb8caf1202688f18e102a5004acf34fb130800:1`.
- Serialized genesis: `00e40b5402000000060000`.
- Status: **unbacked test asset**, not bridge-backed USDT or a redemption promise.

Its reviewed label and verified precision are available even on a fresh wallet
or one without historical issuance block data. A wallet that already imported
the same asset still loads successfully; the bundled identity takes precedence
over its older, independently verified local display record.
