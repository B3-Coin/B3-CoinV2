# FlowMesh chart time — investigation, not a protocol change

2026-09-27; inspected baseline `e4f83139e637ff1d291118cd705130d2855ea5b7`.

## Why the current chart says microblocks

A FlowMesh microblock is a certified batch of actions. Its sequence orders
batches; it is not a fixed duration. `B3FlowMeshChart::AggregateCandles` in
`src/qt/b3flowmeshchart.cpp` groups actual clearings into sequence buckets of
1, 5 or 20. `src/qt/b3flowmeshtradingpanel.cpp` explicitly labels these as
microblocks, not minutes. This is a current data limitation, not a general
limitation of blockchain trading or Qt.

`src/flowmesh/market_data.h::MarketHistoryEntry` carries sequence, hash, anchor,
price, quantity, notional, fee and fills, but no execution timestamp. The
snapshot's optional `local_observed_at` is explicitly local observation,
absent after restart until new certification, not a certified per-trade time.
Neither multiplying sequence by a nominal interval nor using the B3 anchor
timestamp reconstructs exact historical execution time. Receipt arrival at a
reconnecting client would incorrectly bunch old trades into the reconnect time.

## What Hyperliquid exposes

Its official public API exposes timestamped trades and candle records with
millisecond open/close times, OHLC, trade count and volume. Candle intervals
include 1m, 3m, 5m and 1h. WebSocket subscriptions provide trades and candles.
These documents establish its public data interface, not a proof of its exact
internal timestamp-consensus rule or a speed guarantee for this project.

- [Official trade/candle subscription schemas](https://hyperliquid.gitbook.io/hyperliquid-docs/for-developers/api/websocket/subscriptions).
- [Official candle snapshot API](https://hyperliquid.gitbook.io/hyperliquid-docs/for-developers/api/info-endpoint#candle-snapshot).

## Recommended separate chart stage

Retain canonical sequence/hash for identity, deduplication and ordering, while
adding an explicitly sourced time field to chart records. Keep transport
reuse qualification separate from this stage.

For a display-only, non-consensus path, an operator can retain the first local
observation time of an applied certified clearing, keyed by exact market and
microblock hash. Label it **operator-observed time**, never signed execution
time. It is not authoritative input to balances, order expiry, funding or
liquidations. Historical catch-up import must remain missing-time unless an
explicitly attributed historical source supplies it. Do not backdate guessed
times or treat different operators' clocks as interchangeable.

Time candles can then use fixed UTC intervals, actual chronological prices and
exact rUSD turnover. Define tie ordering, backward clock steps, late records,
duplicate delivery, branch changes and missing intervals before implementation.
Missing history is a gap, not evidence of zero trading. Do not silently sum
notional at a closing price or change fee currency to match a volume label.

If V2 instead requires validator-certified execution time, specify its signed
field and deterministic validation/clock bounds as a separate reviewed protocol
decision. Do not add it to existing signed objects or reinterpret V1 history
as a chart-only patch. Futures oracle/funding time also needs its own approved
rules, not the display clock.

No chart data, timestamps, signed formats, consensus or running application
were changed by this investigation. Time-based candles remain unimplemented.
