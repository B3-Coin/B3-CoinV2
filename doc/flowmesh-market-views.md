# FlowMesh market views: clearing ticks and the indicative clearing

Status: implemented on `feat/flowmesh-hl-learnings-claude`. Clearing ticks are
**served by default** on the restricted trading API
(`-flowmeshapiclearingticks=256`); the indicative clearing is computed only by
the node a wallet talks to and is never served publicly. Display data only: no
consensus rule, signed format, fee, economic rule, certificate, state root or
FMN2 message changes, and nothing here feeds signing, admission, balances or
any verification label.

FlowMesh clears one uniform-price auction per microblock. Its equivalent of a
top-of-book feed is therefore a **clearing tick** per microblock (price,
volume, and which side was left unfilled at that price), plus an
**indicative next clearing**: what the next auction would do if the verified
standing orders, optionally with the order in the ticket, were cleared now.

## 1. Clearing ticks

A tick is one certified microblock of the bounded (256-entry) market-history
cache: `sequence`, `microblock_hash`, `kind` (`execution`, `settlement` or
`handoff`), `cleared`, `price`, `volume`, `imbalance` and `imbalance_side`
(`bid`, `ask`, `none`, `unknown`). There is no time field: microblocks carry
no execution timestamp.

`imbalance` is the committed `|demand - supply|` at the clearing price
(saturated at the monetary maximum, exactly as `ClearingResult::imbalance`).
The committed result loses its sign; the node recovers it, for display only,
from the book right after the execution (`ObserveImbalanceSide`): Allocate
lists every curve with positive quantity at the price on both sides, and the
clearing is the last book mutation of an ordinary execution, so the short side
has nothing left at the price and the long side's residual sums to the
committed imbalance. Anything else is `unknown`, never a guess. Handoffs,
settlements and executions without a match are side `none`. Appends observe
the side on the post-execution state; startup replay does so only for the
entries the cache retains. The commit path pays one lookup per curve the
clearing's own allocation already listed.

### Local read: `getflowmeshmarketdata ... {"ticks": {...}}`

`ticks` takes `after_sequence` (optional) and `limit` (1-256, default 64).
Without `after_sequence` it returns the newest ticks; with it, the ticks after
that sequence, oldest first. `gap` says the tick right after `after_sequence`
was already evicted (a missing record, never a quiet market) and `more` that
the page was truncated. Ticks are read together with the head and also for an
unchanged `known_head`, so a light poll still receives new ones. The local
engine labels them `local_engine`.

### Public API

| Flag | Default | Meaning |
|---|---|---|
| `-flowmeshapiclearingticks=<n>` | 256 | Largest tick page on `snapshot` and unwatched `updates`; 0 keeps the previous request and reply contract byte for byte. |

When enabled, every status object advertises `clearing_ticks_max`, exactly
like `action_wait_ms_max`; older clients read named status fields only and
never send `ticks`. `snapshot` and `updates` accept a `ticks` parameter
`{after_sequence, limit}` (the limit clamped to the maximum, 0 refused), whose
page rides in `reported_data.clearing_ticks`. A snapshot's ticks are read
under the same market lock as its certificate and state. A watched `updates`
page has no `reported_data` and refuses `ticks`. Disabled, `ticks` is the
unchanged "Unknown or duplicate client API field".

Cost: an in-memory ring read of at most 256 entries under the store lock, the
same as the existing history read; a full page adds about 64 KB of JSON to a
reply. Admission budgets are unchanged and no new request type exists; the
largest possible reply (a `snapshot`, up to 8 MiB of state) and the per-client
byte rate it bounds are unchanged.

### Remote client

The engine-off client asks an endpoint for ticks only when that endpoint's
status advertised them, only on reads that asked for ticks (never a signing
preflight or any other read), and only for the ticks after the last one it
holds. Pages are checked after the read completes, never inside reply
validation, so a bad page costs only the ticks, never the read:

- well-formed values the engine can have produced (bounded amounts, no match
  for a handoff or settlement, nothing but side `none` without a match);
- consecutive sequences after the requested one (a later start only with the
  endpoint's `gap` flag), none beyond the verified head;
- ticks already held are unchanged, across endpoints;
- the tick at the verified head carries that head's hash.

The client holds at most 256 ticks per cached market (erased with the cache)
and reports them as available only when they reach its verified head. They
stay labelled `endpoint_reported`: only the head tick is bound to verified
state. An endpoint that refuses `ticks` as an unknown field (a stale or
flapping advertisement) is read once more at once without them, is not marked
unavailable, and its advertisement is ignored for ten minutes. A live-update
read whose held ticks stop short of the verified head (a signing preflight's
snapshot asks for none) gives up its watch once per head so that an ordinary
read fetches them. Held ticks more than one advertised page behind the
verified head are not continued (they would creep forward a page per read):
the read asks for the newest page, which replaces them. Its watch status is `ticks_catchup` with `waited` true:
the watch connection is kept and the caller may watch again at once. Qt
counts it as neither a live reply nor a strike and re-arms at once.

## 2. Indicative next clearing

`getflowmeshmarketdata ... {"indicative": {}}` or
`{"indicative": {"draft": {"side": "bid"|"ask", "price": n, "quantity": n}}}`.
The draft is bound to the wallet's existing account and built with the
wallet's own limit-curve builders; submitted, it would replace that account's
standing curve on the same side.

The reply's `indicative` section is labelled `certified: false` and
`excludes_pending_actions: true`, names the `head` whose verified curves it
used, and reports `cleared`, `price`, `volume`, `imbalance`,
`imbalance_side`, the best standing bid and ask over every verified curve,
and for a draft its fill (`draft_fill_min`/`draft_fill_max`: exact
largest-remainder rationing when the account is known). A draft also says
whether it replaces a standing curve and that its admission (balance and
reservation) is assumed.

The computation is exactly ClearSlot's price selection (maximum volume, then
minimum imbalance, then lowest price over every breakpoint price) and
Allocate's rationing, over copies of the curves; tests compare it with the
engine's own ClearSlot on a state copy with the draft submitted. Work is
bounded (2^20 units: a breakpoint read or a curve evaluation); beyond it, or
for a malformed curve or invalid draft, the result is unavailable with a
reason, never truncated.

Where it runs:

- local engine: the standing curves are copied at most once per certified
  head, and only when asked, one curve-paging page (128 curves) per hold of
  the market lock, exactly as a paged liquidity read takes it; no hold copies
  the whole book. A head that moves between holds ends the copy (unavailable,
  `head_changed`; the next read copies the new head). The copy is kept only
  while its head is current and a view used it in the last 30 s; any
  market-data read drops (and frees, after the lock) older ones. The view is
  computed outside every runtime lock. On a validator whose wallet shows the
  indicative this still adds one page's copy per hold to the market lock the
  commit path also takes (the timing span records `book_copy_holds`,
  `book_copy_curves` and `book_copy_longest_hold_us`);
- remote client: from its BLS-verified whole state, copied once per cached
  head under the work gate (a signing preflight or action poll behind it
  waits for that copy, at most 16384 curves), computed after the gate is
  released;
- a book over 16384 standing curves is never copied: the view is unavailable
  (`work_budget`, as the computation would exceed its budget on such a book);
- the public API neither accepts nor serves it.

## 3. Qt

Every selected-market read (refresh and live update) asks for the ticks after
the last one held and, while the ticket holds an order that parses, for its
indicative clearing. When the held ticks reach the displayed head:

- a strip under the chart shows the latest auction, for example
  "Last auction 12:34:56 · 0.25 tUSD at 1 B3 / tUSD · buyers left 0.05 tUSD
  unfilled" ("B3 sellers" in the B3 view; "endpoint-reported" for a remote
  client; the time only when the serving node observed it live, the auction
  number in the tooltip; see `flowmesh-observed-time.md`);
- without observed times, the chart draws the same actual clearings in
  auction order over up to 256 auctions instead of the 100-row history page;
  with them it draws UTC time candles.

Under the ticket, the indicative line appears only for the order it was
computed for ("Indicative, not certified: would clear at ... filling ... if
submitted now (other pending orders excluded)"). After an edit it says it is
updating until a read for the new order returns; with live updates that can
take up to one watch cycle (about 1.5 s on a quiet market).

## 4. Compatibility

| Client / server | older server | new server, `-flowmeshapiclearingticks=0` | new server (default) |
|---|---|---|---|
| older client | unchanged | byte-identical | status objects add `clearing_ticks_max`; never sent `ticks` |
| new client | no advertisement: never sends `ticks`; ticks unavailable with a reason | same | incremental ticks |

Wallet RPC callers that pass neither option get byte-identical replies.

## 5. Not included

- A persisted last-verified view for an instant first paint after restart:
  not implemented (the review's race, stale-restore and flush blockers need a
  separate persistence and Qt restore path).
- Aggregated book buckets, on the node or the public API.
- Indicative clearing including uncertified pool actions, certified chart time
  and per-tick verified outcomes for remote clients. (Operator-observed chart
  time is in `flowmesh-observed-time.md`.)

## 6. Tests

Unit: `flowmesh_market_view_tests` (rationed side against brute-force
demand/supply, saturation, tick pages, indicative against ClearSlot, best
prices against a scan, budgets, a 10,000-curve book copied one 128-curve
page per read, over-bound and moved-head copies),
`flowmesh_production_store_tests` (sides across a handoff and replay),
`flowmesh_runtime_tests` (ticks, the curve copy on an unchanged head, and
dropping copies of an old head or unused for 30 s), `flowmesh_client_ticks_tests` (page checks and
merging), `flowmesh_client_market_view_tests` (public contract on and off,
incremental remote ticks bound to the verified head, stale advertisement,
contradicting page, live-update catch-up, indicative on both backends).
Qt: `test_b3_flowmeshworkspace-qt` (section parsing, held ticks, texts, read
options and display). Functional (opt-in, run directly):
`feature_flowmesh_market_views.py`.
