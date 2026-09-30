# FlowMesh order book: verified levels, ladder orders and the trades tape

Status: implemented on `feat/flowmesh-hl-learnings-claude` (UI round of
2026-09-30). Display data and a wallet convenience only: no consensus rule,
signed format, action, fee, economic rule, certificate, state root, FMN2
message or public API reply changes. Nothing here feeds signing, admission,
balances or any verification label, except that a ladder order is signed
through the existing SUBMIT_BID / SUBMIT_ASK action after an explicit review.

## 0. What the owner should know first

- **The engine keeps one standing curve per account and side**
  (`ClearingEngine::m_curves`). A new order replaces that side; so one test
  wallet always had one bid level and one ask level, which is why the old
  book "showed only the last order".
- **Provisional default (solve-and-report): ladder orders.** Several of one
  wallet's own prices on one side are placed as a *ladder*: ONE curve with up
  to four limit prices (a v1 curve holds eight breakpoints, two per price).
  No protocol change. Independent orders per side (separate cancels and
  fills) remain a V2 state change needing an owner ruling.
- **Certified trade time is not implemented** (V2 ruling); the tape shows
  operator-observed times (doc/flowmesh-observed-time.md) or, for servers
  without them, strictly limited receipt times.

## 1. Book levels

`getflowmeshmarketdata <market> {"levels": {...}}` adds a `book_levels`
section: the exact price levels of **every** verified standing curve (all
accounts, ladders and general curves; not only the 128-curve page), computed
by the node the wallet talks to.

### Math (exact integers, no floating point)

A curve's remaining quantity at price `p` is `R(p) = max(0, E(p) - filled)`,
with `E` the engine's floor-interpolated `EvaluateCurve` (fills take the
best-priced lots first). A **bid** bucket `[lo, hi]` holds `R(lo) - R(hi+1)`
of it, an **ask** bucket `R(hi) - R(lo-1)` with `R(-1) = 0` (an ask whose
first quantity is positive sells at price 0). `cumulative` is the running sum
from the best level and equals the side's total demand at `lo` (bids) or
supply at `hi` (asks). Each level has its curve count (one per account and
side), the caller's own share and, within a walk of 16 prices per curve, its
exact B3 `notional` (sum of quantity x price).

### Grouping

A display step `h = m x 10^e` (`m` 1, 2 or 5) for a base asset of `d`
decimals:

- **Canonical (B3 per token).** A canonical price `P` shows as
  `P x 10^(d-9)` B3 per token, so `h` is `g = m x 10^(e+9-d)` price ticks
  (the corrected sign; e.g. `d = 6`, `h = 0.01` is 10 ticks). Valid when
  `e+9-d` is 0..18 and `g <= MAX_MONEY`. Bids are floored and labelled with
  the lower edge, asks ceiled and labelled with the upper edge: every lot of a
  bucket accepts its label.
- **Inverse (token per B3).** `u(P) = 10^s / (m x P)` with `s = 9-d-e` in
  0..36. Canonical bids become displayed sells (`ceil(u)`), canonical asks
  displayed buys (`floor(u)`); each bucket is an exact contiguous range of
  canonical prices. Price 0 is its own `zero_price` level (the best displayed
  buy), so cumulative stays exact.

`LevelBucketRange` gives any bucket's canonical range, so a client checks
each reported level against its bucket.

### Bounds and where it runs

- Computed only for a reply whose head changed, or an unchanged `known_head`
  reply with `include_unchanged` (after a grouping change); a light poll
  computes nothing. `status_only` refuses levels; invalid options are
  refused, never clamped (depth 1-50).
- Its own work budget (`MARKET_VIEW_LEVELS_WORK_BUDGET`, 2^20 units over
  both sides): a book whose first pass does not fit has no levels
  (`work_budget`); afterwards a side stops early as an exact prefix
  (`truncated_reason`). A level or cumulative beyond `MAX_MONEY` stops its
  side (`overflow`), never saturated. A book over 16384 standing curves is
  not copied (`work_budget`).
- Local engine: from the per-head book copy (one 128-curve page per market
  lock hold). Remote client: from its BLS-verified whole state, never the
  endpoint's reported data; the copy is made once per verified head under
  its work gate (about 0.34 ms for 16384 curves, measured in
  `book_levels_of_a_full_limit_book_fit_the_budget`), the levels are computed
  after it (about 1.2 ms for 16384 limit curves).
- Memoised per copy of the curves and query (head, grouping, depth,
  account; at most 8 entries).
- The section also carries the exact best prices and the **standing
  clearing**: the indicative without any draft (what the next auction would
  do with the standing curves alone). The ticket's draft indicative is never
  used for the book.

### Compatibility

Only the local wallet RPC asks for levels. The public HTTPS `snapshot` and
`updates` never set them: `levels` stays an unknown field there and every
public reply is byte-identical, so c10c952 and f8a3432 peers, servers and
clients are unaffected, and a new client computes levels from the state bytes
any of those servers already serves.

## 2. The Qt order book

- One ladder: displayed asks above a spread row and displayed bids below,
  best prices beside the spread, `Price | Size | Total` with Total
  cumulative, as many rows per side as the height holds (only non-empty
  levels), updated in place (no model reset or row churn).
- Layout (Hyperliquid's): the chart sits above the activity tabs (your
  orders, trade history, activity, saved requests) in the left column; the
  order book and the ticket each take the full height beside it, and the
  book's title and view selector share one row. At the standard 1320 x 940
  panel the ladder holds 12 levels a side in the exported screenshots (at
  least 10 is tested), where the tabs under all three columns used to leave
  it about 5.
- One number of decimal places per column, on every row (Hyperliquid's
  fixed precision; review of 2026-09-30). Price: the market tick's places in
  the canonical view (every on-grid price exact, e.g. `1.000` at 0.001 B3),
  the step's places when grouped (`1.00` at 0.01, `0.9900` at 0.0001). In
  the B3 view with one level per canonical price, token-per-B3 prices do not
  end, so the view has ONE rounding rule (`ViewPricePlaces`, second review
  of 2026-09-30): every price of a level or an order is rounded to the
  nearest value (half up), never by side, at the fewest places at which the
  market's best prices and their neighbouring ticks have three different
  values (the B3 view's tick precision at the market's price, even when a
  best price ends early: 1000 atoms is exactly 1, shown `1.000` beside
  799-801). The book's rows, the spread row and Your orders all use it, so a
  price reads the same wherever it appears: the viewer's sell of B3 at 997
  atoms is `1.003` in its row and `≈1.003 tUSD / B3` under Your orders (the
  earlier directed rule showed it on a row labelled `1.004`). Exact levels
  far from the market whose prices round to one value share ONE row: its
  size is their sum, its tooltip lists each exact price, size and order
  count, and a click offers its deepest price (the one that takes every
  level in it). "≈" is said once, in the Price header (its tooltip states
  the rule), never per row; each rounded level keeps its exact price in its
  tooltip and says "about" to a screen reader. Size and Total share one
  count: the places the rows need. The fallback projection follows the
  same rule. `ViewPricePlaces`, `ViewPrice`, `ViewSpread`,
  `NearestDisplayPrice`, `PadDecimals` and `NeededDecimals`
  (qt/b3flowmeshmarketdata.h) do the formatting; display only, never parsed
  into an action. Trade displays (last price, trade history, the trades
  tape, the chart, the clearing strip) keep `DisplayPrice`'s significant
  digits for now.
- Depth bars on ONE scale shared by both sides; the viewer's own orders
  marked; tooltips with order count, grouped range, average price.
- Spread row: `Spread <difference> · <pct>%`. The difference is that of the
  two prices beside it as they are shown, at the view's places (`≈0.002`
  between `1.001` and `0.999`; `0.010` B3 between `1.010` and `1.000`),
  "≈" when either is rounded, the exact spread in its tooltip. The share,
  200 x (ask - bid) / (ask + bid), is exact and the same in both views. A
  crossed or locked book, legal after a pro-rata auction, shows
  `Crossed · next auction ≈P for V (indicative)` from the standing clearing,
  P at the view's places.
- Flash: a newly appearing price (not a size change) flashes 800 ms,
  staggered; "Animate order book" (context menu, default on) and the
  platform's UI effects setting turn it off. The timer runs only while a
  flash does.
- Click a level (Hyperliquid's click-to-price): its price **as shown** goes
  into the ticket whenever the ticket reads it back, for its side, as exactly
  the level's canonical price (the edge every order of a grouped level
  accepts): always in the canonical view, and in the B3 view whenever the
  nearest value happens to lie on the side the ticket rounds toward.
  Otherwise the fewest more places, rounded toward the ticket's side, that
  do and still round to the label shown (buy B3 at the row `1.003`: `1.0031`,
  never the next row's `1.004`; a grouped edge may take the same places);
  the exact form only if none does. The executable limit is therefore always
  exactly the level's canonical price: a rounded B3-view text is adjusted
  only in the customer's favour, the ticket shows "Safely adjusted
  executable limit: <exact>", the review shows the exact canonical limit
  with the entered text and the adjustment, and that exact limit is what
  would be signed. The price is re-derived from the gates' current verified
  data; the side is kept, the marketable limit unchecked, nothing is
  reviewed or signed. "Set size on click" (default off, unlike Hyperliquid)
  also sets the cumulative size within the available balance.
- Grouping selector: Exact, then `1, 2, 5 x 10^(k-4)` and `10^(k-3..k-1)`
  around the displayed price's magnitude `k` (integer digit counts), each
  valid for the orientation; kept per market and orientation for the
  session; a choice reads at once with `include_unchanged`.
- Without the node's levels (an older node, levels unavailable, or another
  grouping still being read) the book falls back to the limit and ladder
  orders of the first curve page, labelled.

## 3. Ladder orders

- RPC: `submitflowmeshladder <market> bid|ask [{"price", "quantity"}, ...]
  [sequence] [wait_ms]`, 1-4 distinct positive prices (a bid below
  MAX_MONEY), validated before any key is read, signed through the same
  outbox as `submitflowmeshorder` (write-ahead save, unknown stays unknown,
  retry by ActionId).
- Curve: `MakeLadderBidCurve` / `MakeLadderAskCurve`, a staircase with
  one-tick drops; one price is exactly the limit order. Reservation
  (`LadderReservation`): a bid reserves exactly the sum of quantity x price
  B3 (exact only because every drop is one tick wide), an ask the sum of its
  quantities.
- Qt: "Ladder · up to 4 prices" in the ticket (the price and quantity fields
  are the first price); the review lists every price best first with the
  exact reservation; Edit prefills what remains at each price; Your orders
  and saved requests name ladders.
- What users must be told (review S9): a ladder replaces the account's order
  on that side and its quantity counts from zero; a price cannot be cancelled
  or edited alone; clients before this version (c10c952, f8a3432) show a
  ladder only as a general curve under Curve depth, and their marketable-limit
  helper refuses to price a market that holds one. This client prices against
  ladders (each price is a limit order) and never crosses the viewer's own.

## 4. Trades tape times

The tape (next to the order book, "Trades") shows operator-observed times
where the source serves them. For a server without them, a "received by this
app" time says only that the auction came in a live update and was certified
at or before then. It is stamped only for a live update whose source held the
displayed head when its wait began and saw it move during the wait (watch
status `ready`, head moved, no gap), bringing exactly one new auction, with
the link Live and the endpoint not advertising that it is not ready, and only
when it is not earlier than a time already held for an older auction (after
a backward clock step the newer auction shows "—"); every other reply leaves
"—" (review B2). A `head_changed` reply never stamps one:
the local engine and the public endpoint answer it at once, without waiting,
for a stale known head. An endpoint without live updates therefore gets no
receipt times.

## 5. Owner rulings still open

1. Independent multiple orders per account and side (V2 state change).
2. Certified execution time (V2 protocol change).
3. Product defaults set provisionally here: ladder of up to 4 prices per
   side; "Set size on click" off; "Animate order book" on; display cap 16384
   standing curves.
4. **Before merge or release (review of 2026-09-30, process note).** Two
   points need the owner's explicit ruling; nothing in code was changed for
   them:
   - **Workspace and branch.** This work is on `feat/flowmesh-hl-learnings-claude`
     in `/Users/josh/development/ON/B3-FlowMesh-hl`. The checked-in CLAUDE.md
     scope lock (owner ruling 2026-09-03) names only
     `/Users/josh/development/ON/B3-FlowMesh` on branch `FlowMesh`. The later
     instructions this round relied on (2026-09-24 "make your own branch",
     2026-09-29 "implement and improve this enough") are not reconciled with
     that tracked document; under its own precedence rules the
     reconciliation must land in a reviewed commit before this branch is
     merged or released.
   - **Ladder orders shipped as a provisional default.** The order-book
     design held ladder orders (L1: the wallet RPC; L2: the Qt flow) back
     "pending an owner ruling"; this branch ships both
     (`submitflowmeshladder` and "Ladder · up to 4 prices"). Once any user
     of this client places a ladder, every c10c952 and f8a3432 wallet in
     that market shows it only under Curve depth, its marketable-limit
     helper refuses to price the whole market (it requires a book of limit
     orders only), and it cannot Edit a ladder. The ladder review says only
     that wallets before this version show it as a curve. Options for the
     ruling: accept as is; keep the Ladder checkbox behind an opt-in setting
     until the ruling; and/or add to the review that older wallets'
     marketable-limit helper will refuse this market. The RPC help and §3
     already state the market-wide effect.

## 6. Tests

- Unit (`flowmesh_market_view_tests`): levels against an integer scan in
  exact, canonical and inverse groupings with random fills; the canonical
  step sign; the inverse partition; ladders, slopes, zero prices, fills; a
  crossed residual; budget prefixes and overflow; bucket ranges; memoised
  views; full-book cost. `flowmesh_microblock_tests`: ladder builders and a
  signed ladder executed. Runtime and HTTPS: levels only when wanted, one copy
  per head, refusals, both backends from verified state, never public; and
  (`flowmesh_client_wide_book_tests`) a remote client reading 150 verified
  standing curves (fifteen of them four-price ladders) through a relay that
  rewrites the endpoint's `reported_data` to an empty book with made-up
  levels: its levels in exact, canonical and inverse groupings equal
  `ComputeBookLevels` over every verified curve paged from the serving node,
  never the reported projection (critique S7).
- Functional (`feature_flowmesh_market_views.py`): option errors, ladder RPC
  errors, a four-price bid ladder and a three-price ask ladder, levels equal
  to those computed from every paged curve (exact and grouped) and equal
  between the local engine and the remote client. Every grouping is
  compared at one head (a read that sees the head move is taken again, never
  skipped), the light-poll and `include_unchanged` checks always run, and
  the test asserts that all four comparisons happened.
- Qt (`test_b3_flowmeshworkspace-qt`, `test_b3_flowmeshtrading-qt`): parsing
  fails closed per section, the unchanged merge, requests, the ladder layout,
  shared depth, own marker, no churn, flash, fallback, spread texts, grouped
  labels, click-to-fill, grouping choices and selection, the ladder ticket,
  review, edit and marketable pricing, receipt times.
- Opt-in PNGs: `FLOWMESH_UI_SNAPSHOT_DIR=<abs> QT_QPA_PLATFORM=offscreen
  test_b3_flowmeshworkspace-qt exportOptInSyntheticVisualFixtures`.
