# Qt candle volume in the B3/asset view

This is a display-only correction on top of the client request-priority fix.
The native market remains asset/B3; inverse price display does not reverse its
configuration, orders, fees or settlement rules.

Candle volume now always sums the actual canonical asset quantity. For the
test rUSD market, B3/rUSD candles therefore show rUSD turnover, not B3 gross
notional. The unit label, formatted number and volume-bar normalization all
use that same asset. Depth/order quantities remain unchanged.

Example: two clearings each exchanging 1 rUSD at 0.1 and 0.2 B3 respectively
have equal 1 rUSD volume bars, or 2 rUSD in one bucket. Do not convert total
B3 notional at the closing price. Reciprocal OHLC prices and their colors are
unchanged. Volume uses retained 128-bit integer totals and the exact configured
asset precision; ticker text is a label, not a currency selector.

## Regression

Before the fix, `candleVolumeKeepsAssetUnitsWhenPriceIsInverted` failed with
actual `0.1`, expected `1`. It now covers unchanged asset units under inverse
prices, differing decimals and invalid precision. Existing tests cover
multi-clearing aggregation, totals beyond signed 64 bits, unchanged refreshes,
inverse extrema, real clearing versus unmatched auction, and up/down painting.

```sh
cmake --build build --target test_b3_flowmeshworkspace-qt b3coin-qt -j 8
QT_QPA_PLATFORM=minimal build/bin/test_b3_flowmeshworkspace-qt
```

Optionally set `B3_FLOWMESH_CHART_GALLERY` to an output directory for explicitly
synthetic visual fixtures. These generated images are not wallet screenshots
or evidence of new network trades. Local macOS arm64/Qt6.11.1 Release test with
gallery export: 168 passed, zero failed, two optional exports skipped.

## Fee and qualification boundary

Actual native V1 fees remain B3, with deterministic 80% active-seat reward and
20% treasury allocation. Accrued reward-account balances are not completed
on-chain payments and do not pay every FN token holder. Stable-asset fees and
asset-aware payouts need the separately approved V2 accounting integration;
they cannot be implemented by changing this chart label.

This patch changes no economic state and proves no trade-latency target.
