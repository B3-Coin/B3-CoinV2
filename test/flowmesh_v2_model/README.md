# Isolated FlowMesh V2 accounting model

This is a standalone, deterministic, **TEST-ONLY** accounting model. It exercises
the seven R1 accounting assumptions with generated assets, accounts, authority
identifiers and custody/risk facts. Permission to build this model does not
ratify its economic choices or authorize production integration. In particular,
the fee dust and remainder rules below remain provisional.

The preserved R1 reference is revision
`2b32645e26f4ba41de8aa746bd04e84192fb3972`, including
[`v2-stage0-r1-accounting.md`](../../doc/design/v2-stage0-r1-accounting.md).
That historical package is unchanged. Its pre-model approval/status wording is
part of the preserved record; this separate directory records the bounded model
profile without rewriting that record.

Milestone 1's internal review and repairs are recorded in
[`MILESTONE1.md`](MILESTONE1.md). The original [`RESULTS.md`](RESULTS.md) remains
the unchanged 88-test baseline record, not the successor's qualification report.

## Profile and scope

[`TEST_PROFILE.json`](TEST_PROFILE.json) identifies
`flowmesh-v2-isolated-accounting-test/1`, with domain
`generated-accounting-model-not-a-network`. [`API.md`](API.md) defines the test
interface. The manifest, immutable asset/market configuration, frozen synthetic
recipient authorities and subaccount ownership are part of model identity and
snapshot state. They are not a production codec, registry or activation manifest.

The required eventual V2 includes **both spot and futures**, safe BFT and PoS,
shared balances by exact AssetId, genuine asset/asset markets, deposits credited
to spot first, and explicit authorized transfers into and out of futures.
Futures losses, funding, fees and positions receive no implicit access to spot.
Ordinary Qt trading must not require a local validator engine; no separate
process or deployment architecture is selected here. This model does not
implement BFT, PoS, that engine integration,
futures trading/margin, or a complete V2 release. Existing Qt, node and bridge
behavior remain unchanged.

The first model accepts only spot markets quoted in the manifest's approved
synthetic stable fee assets: the distinct 32-byte AssetIds displayed as 64 `a`
characters and 64 `b` characters. These are test registry entries, not named
live stablecoins or real approval decisions. Base assets remain distinct exact
AssetIds, including all-zero native B3 where a fixture configures it. Equal
symbols never merge assets; base and quote cannot be equal. Broader asset/asset
market support remains an eventual V2 requirement, not a claim that arbitrary
quote assets are supported by this first fee profile.

Prices, lots, decimals and atom multipliers are explicit fixture configuration.
Market identity binds the exact assets and units; a changed configuration creates
a different identity. All arithmetic is integer arithmetic under named manifest
bounds. The configured bounds, including maximum amounts and intermediate
products, are test limits, not mainnet amounts or proposed wire widths. No
display-unit conversion supplies spendable funds.

## The seven provisional R1 choices

| R1 choice | Model meaning and remaining qualification |
| --- | --- |
| A-D1 | One shared spot ledger per account and exact AssetId. Import settlements/risk facts, import eligible deposits, execute account sequences, clear every configured market by MarketId, then allocate fees and validate the candidate. Same-batch later clearing proceeds cannot fund an earlier instruction. |
| A-D2 | At most one open curve per account/market/side. Immutable OrderId comes from the opening action. Replacement names the expected revision and authorizes additional remaining lots; lifetime fills, notional and fees survive replacement. Cancellation/exhaustion never reopens that ID. |
| A-D3 | BUY reservations use the persistent staircase bound plus a fee cushion. Residual backing is initial revision bound minus actual spend; no speculative partial-fill release. Replacement adjusts the owned reservation atomically, and cancel/exhaustion releases the residual. SELL reserves base atoms and deducts its fee from quote proceeds. |
| A-D4 | Charge 50 ppm per side in the approved exact quote asset using cumulative per-order floors. Group collected fee atoms across markets by exact quote asset in one frozen recipient context. Treasury receives the per-batch floor of 20%; historical FN seats split the remainder equally, with canonical SeatId order assigning extra atoms. No treasury carry. All rounding/allocation choices remain provisional. |
| A-D5 | Authenticated deterministic state rejection consumes the expected sequence and retains its outcome. Gaps and new equivocation groups do not consume it. Exact completed replay returns its stored outcome. This profile explicitly gives completed replay precedence over grouping new conflicting content at its already-consumed sequence. |
| A-D6 | Per-asset custody equals available spot, owned reservations, backed futures cash, protocol balances and pending payouts. Capacity and authority inputs are synthetic. V1 custody/claims remain separate; only the modeled settled payout followed by a distinct new V2 deposit can move test funding across the fence. |
| A-D7 | Futures transfers require explicit account/subaccount ownership, exact asset and amount, a configuration identity and an explicit enabled synthetic risk result. Unknown/undefined/denied risk cannot authorize a transfer. This checks cash movement mechanics only; no futures economics or production risk authority is selected. |

For A-D4, an order's fee after lifetime quote notional `T` is
`floor(T * 50 / 1_000_000)`. Each fill charges only the increment. Replacement
does not reset it; a new OrderId starts a new accumulator. Thus ten partial
10,000-atom fills on one order charge 5 atoms, while ten distinct one-fill
orders can charge zero. There is no minimum fee or collectible fractional atom
at close. For a batch group with total fees `F`, treasury gets `floor(F/5)` and
FN seats get the remaining integer atoms. Small batches can allocate zero to
treasury, and canonical seat order can systematically receive remainder atoms.
These are disclosed test assumptions, not an exact fractional 80/20 payout or
an economic recommendation. Different AssetIds never share a rounding bucket.

## Synthetic input and authority boundary

Construct `Model(profile, assets, markets, seats, treasury_owner, subaccounts)`
with explicit generated identities. Each model has one frozen synthetic
historical seat/authority mapping and treasury owner. A seat's test authority is
not a real BLS key, and the model does not authenticate a committee or implement
epoch handover. Recipient claims use separate derived account sequence streams;
later ownership of an FN cannot supply the frozen historical claim authority.

`apply_batch(deposits=(), settlements=(), actions=(), capacities=None,
risk=None, fault=None)` consumes a complete synthetic candidate. Invalid
external evidence, bounded-arithmetic failure or invariant failure rejects the
whole candidate. An authenticated instruction's ordinary state failure changes
only its sequence/outcome. `fault` is an explicit test hook for interruption at
`after_actions` or `after_first_fill`; it is not an operational API.

- A deposit fact contains `chain`, `custody_version: 2`, `custody_domain`,
  `txid`, `vout`, `account`, `asset`, `amount`, `height`, `tx_index`,
  `output_index`, and `verified: True`. Its identity includes chain, version,
  custody domain and original outpoint. Identical duplicates credit once;
  contradictory facts reject the candidate. It credits spot first. The fields
  are already authenticated stand-ins supplied by the fixture; `verified`
  does not verify a chain proof or establish import completeness.
- A withdrawal action names `asset`, `amount`, and exact `destination`.
  The fixture supplies `capacities` as AssetId to atom amount. Admission requires
  existing pending claims plus this request to fit that synthetic capacity and
  requires available owned funds. It moves those funds into one pending claim;
  it is not a B3 payout authorization or transaction. A smaller later capacity
  blocks new requests without erasing existing claims. Silence or an unknown
  broadcast outcome cannot restore available funds.
- A settlement fact contains `receipt_id`, `asset`, `amount`, `destination`,
  `height`, `tx_index`, `event_index`, and `verified: True`. It must match a
  pending claim exactly and retires that liability and custody once. Identical
  duplicates have no further effect; conflicting evidence rejects the batch.
  No payout transaction is constructed, and B3 network fee funding is not
  modeled or silently deducted from the payout asset.
- `SPOT_TO_FUTURES` and `FUTURES_TO_SPOT` name `subaccount`, `asset`, `amount`,
  and `risk_config`. The `risk` map entry for that subaccount is
  `{config, enabled: True, asset, status, withdrawable}` with status one of
  `PASS`, `DENY`, `UNKNOWN`, or `NOT_DEFINED`. The configuration must match,
  ownership and collateral must match, and no missing or undefined profile
  permits either transfer. Transfer-out is bounded by both explicitly
  withdrawable cash and backed cash. A `PASS` fixture establishes no real
  margin, oracle, solvency or liquidation safety.
  The per-batch withdrawable allowance is consumed by all transfer-outs;
  transferring more cash in does not increase the supplied allowance.

The action `authorized` flag and claim `authority` parameter exercise test
predicates only; they are not signatures. Action identity excludes the synthetic
authentication wrapper, so an equivalent wrapper does not create another
economic instruction. No production keys, signing, RPC or network input is used.

An omitted risk/capacity entry here means the complete supplied **test input**
does not authorize that action. It must not be interpreted as a replica's local
failure to obtain required execution data. R1 requires that real missing parent
state, candidate body or proof witnesses cause `DEFER` and data recovery, not a
certified state rejection or consumed account sequence. This model does not
implement that distributed data-availability protocol.

`seed_v1`, `settle_v1`, and `redeposit_v1` create explicitly synthetic V1 cases.
Settlement debits only an existing V1 pending liability and records external
funding. A separate V2 deposit consumes that funding once. No snapshot credit,
local release of a V1 reservation, replay of active V1 orders, or unknown old
payout can manufacture V2 funds. These methods neither execute nor authorize
live migration.

## Run and interpret the model

From the `accounting-model-source` repository root, run:

```sh
python3 -m unittest discover -s test/flowmesh_v2_model -p 'test_*.py' -v
```

`snapshot()` returns canonical model state, configuration and bounded synthetic
input history; `Model.restore()` replays that history and compares all resulting
state, `digest()` identifies that snapshot, and `assert_invariants()` checks
current internal accounting. The latter alone does not establish historical
ownership or authorized lifecycle changes. Serialization/replay
qualifies model persistence only, not durable production signing journals or
power-loss recovery. Snapshots are not authenticated: internal-consistency
checks do not prove protection against a coherently forged history or external
rollback. Test results must be read from the actual run; this README
does not assert that a suite has passed.

The corrected test-only envelope is `TEST-MODEL-SNAPSHOT/2`; it does not change
the accounting profile, ActionIds, MarketIds or economics. Storage is bounded to
the existing record-count limit, 8 MiB input history and 32 MiB complete snapshot.
Limit exhaustion is atomic, not permission to delete history. Old `/1` snapshots
remain preserved and fail closed under the new reader: replay requires their
original synthetic inputs, not invented replacement events. See `API.md`.

The evidence from this directory is bounded to deterministic accounting,
reservations, replay, integer bounds, fee ownership and the synthetic fences.
It supplies no network experiment, performance proof, consensus safety proof,
production custody-selection proof, or real futures-margin validation.

## Gates retained from R1

Safe BFT/PoS implementation, producer/checkpoint authority separation, outgoing
bootstrap/cutover authorization, exact historical preservation and freshness,
lineage/expiry handling, actual custody and full futures economics remain
separate gates. No activation height, registry, recipient key or authority is
created by this model. An inherited broken lineage, expired verifier or old
signature conflict does not become valid through a model reset or a V2 label.

The Ethereum P2P light-client replacement is **deferred**. That deferral does not
waive supported-fork, freshness, proof-validation, historical replay or unchanged
bridge compatibility requirements. The deployed bridge and its pins are
unchanged. The preserved R1 main specification's §9.2, including its flagged
inaccurate sentence claiming that AssetId commits the adapter, remains unchanged.
The inspected source excludes the adapter from `BridgeAssetIdV1` and includes it
in the registry identity; the R1 bridge appendix correctly distinguishes these.
This accounting model does not adopt the flagged sentence as an implementation
fact or claim to resolve it. Bootstrap/cutover, lineage/expiry and bridge compatibility
require their own explicit review and evidence before integration.
