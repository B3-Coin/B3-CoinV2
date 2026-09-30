# FlowMesh operator-observed time: time candles, a trades tape and times

Status: implemented on `feat/flowmesh-hl-learnings-claude`. Display data only:
no consensus rule, signed format, fee, economic rule, certificate, state root,
commitment or FMN2 message changes, and nothing here is read by signing,
admission, execution, verification, balances, expiry or any market decision.
It is the one path `doc/design/flowmesh-chart-time.md` allows: **operator-observed
time**, labelled as such and never presented as certified execution time.

V1 microblocks carry no execution time. Hyperliquid stamps each block with a
consensus time; B3 does not (see §8). What B3 can show honestly is when a node
applied each certified auction, by that node's own clock, with the node named.

## 1. What a node records

Right after each durable commit (next to the existing `local_observed_at`) the
node records a stamp for the entry: `{sequence, hash, mode, ms, flags}`.

| Mode | Meaning | What is shown |
|---|---|---|
| `live` | applied while this node was caught up | its time (local `HH:mm:ss`; UTC in candles) |
| `delayed` | applied late | no trade time; `applied_here_ms` is only an upper bound (tooltip) |
| `untimed` | the wall clock stood behind an earlier stamp | no time |

An entry is `delayed` when any of these holds: it came from a catch-up page; a
certificate, proposal, attestation or agreement frame of its slot waited for B3
reconciliation (a per-market watermark, raised at most 16 past the head by an
unverified deferred frame); this node holds a verified certificate, or a peer
hint announces a head, **more than one entry beyond** the one being committed
(a peer level with it is not ahead); its anchor is more than two blocks behind
the current acceptable anchor; or the node has not been caught up for the
warm-up, `max(2 s, 2 x round timeout)` (4 s by default). Caught up means ready,
not halted, chain gate open, a connected FlowMesh peer (or a local signing
quorum), no proven lag and no hint more than one entry ahead; an interruption
shorter than one round timeout (a brief B3 reconciliation, a lag resolved within
a round) does not restart the warm-up. Whether this node signed an entry is not
an input: nothing observable can reveal a seat.

Clocks: a backward wall-clock step of at most 1 s is held at the previous stamp
(`clock_clamped`); a larger one records entries `untimed` until the clock passes
the previous stamp again, so no time is ever pinned or invented. Wall and
steady clocks disagreeing by more than 1 s since the previous stamp flags
`clock_step`; the first stamp of a process is flagged `restart`. A B3 tip more
than three minutes ahead of this clock flags later stamps `clock_suspect`.

### Candles

The node keeps UTC-epoch-aligned candles for 1m, 5m, 15m, 1h, 4h and 1d with
exact OHLC and 128-bit base and quote (`price x volume`) sums, updated once per
commit. A `live` entry goes into the bucket of its time. A `delayed` entry goes
into a bucket only when its whole bound (from about the last live time before
it to when it was applied) lies inside that bucket (candle flag `late`);
otherwise it is placed nowhere and every candle it may belong to is flagged
`partial`. A candle whose first entry carries a clock step or a restart is
flagged `seam_before`. Empty buckets are never created: two candles whose
sequences are consecutive (and the later without `seam_before`) prove that no
auction existed between them; anything else is a gap.

### Storage and the FlowMesh critical path

- `Record` is memory only, O(1) (a 4096-stamp ring, the six rollups, a bounded
  queue of rows to persist) and `noexcept`; its mutex is a leaf taken inside the
  market lock. A failure costs a gap, never the commit (tested with a clock that
  throws).
- One separate LevelDB, `<flowmeshdatadir>/observed_time`, market-prefixed keys,
  outside every production store (its fail-closed checks can never be affected;
  older binaries never open it).
- **Only its own thread (`flowmesh-obstime`) writes it**: at most once a
  second, unsynced, one batch per market with rows and candles together, and
  once synced at shutdown after the runtime stopped. It never runs on the
  FlowMesh tick or worker thread, so a stalled disk delays only the flush
  (tested with a flush held inside its write while commits and reads go on).
- More than 8192 unflushed rows: the oldest are dropped from persistence only (a
  gap after a restart). A crash loses at most the unflushed second (a gap).
- At start, after the production store replays: rows at or after its next
  sequence are erased, a row whose hash contradicts the store's retained hashes
  (or another domain) clears the market's times, and the newest rows and each
  interval's memory window are reloaded. An unreadable database is wiped and
  recreated; if even that fails, times stay in memory.
- Retention: 262,144 rows per market; candles 1m 14 days, 5m 60 days, 15m 180
  days, 1h 2 years, 4h 5 years, 1d 10 years (swept at most once a minute, a
  bounded number of erases per sweep). Memory windows: 1m one day, 5m three
  days, 15m a week, 1h 30 days, 4h 90 days, 1d 500 days.

## 2. Local RPC

`getflowmeshmarketdata <market> {"observed_time": true, ...}` adds an
`observed_time` section (present only when requested, also on an unchanged
`known_head` poll, refused with a status-only read): `available`,
`unavailable_reason`, `source` (`this_node`), `source_kind`
(`operator_observed`), `certified` (always false), `clock_suspect`,
`recorded_since_sequence`, and `rows` for the head and exactly the reply's
history and tick entries whose hash matches: `sequence`, `microblock_hash`,
`mode`, `observed_ms` (live) or `applied_here_ms` (delayed), `flags`, and
`delay_reason` (this node's own reply only). History rows and ticks are
unchanged: times live beside them, so tick equality (which drives clients'
conflict checks) can never change because of them.

`getflowmeshcandles <market> {"interval": "1m|5m|15m|1h|4h|1d", "before_ms": n,
"limit": 1-500}` returns the candles with open in
`[before_ms - limit x interval, before_ms)`, oldest first, only buckets holding
placed entries: `open_ms`, `first_sequence`, `last_sequence`, `entries`,
`clearings`, `open/high/low/close` (with a clearing), `base_volume` and
`quote_volume` as exact decimal strings, `flags`, plus `more` and
`endpoint_reported`. The local engine pages its database beyond the memory
window; a remote client asks its endpoint.

## 3. Public trading API

| Flag | Default | Meaning |
|---|---|---|
| `-flowmeshapiobservedtime` | 1 | `observed_time_v1` advert; `observed_time: true` on `snapshot` and unwatched `updates`; 0 keeps the previous contract exactly |
| `-flowmeshapicandles=<n>` | 500 | `candles` method and `observed_candles_max` advert; 0 keeps the previous contract exactly |

The section rides inside `reported_data` (endpoint-reported display data,
never evidence), labelled `source: endpoint_operator`, with only the flags
`clock_clamped`, `clock_step`, `clock_suspect` and `restart`: no delay reason
and nothing about seats. A watched page refuses it, as it refuses `ticks`.

`candles {market_id, interval, before_ms?, limit?}` is served from the memory
window only, never the disk. It does not loosen the public DoS bound the way
server-side book buckets would: candles are computed once per commit, never
per request, and involve no curve evaluation; a page is a copy of at most 500
memory-resident candles, the limit clamped to the advert. Each request passes
the ordinary pre-parse read admission, then costs one more read token (as
outcome evidence does) and at most two pages per second per client address
(IPv6 /64), answered with HTTP 429 beyond, like `health`.

Privacy: published times reveal this operator's clock and how late it applies
entries, at millisecond resolution (`local_observed_at` was already public at
whole seconds). An operator who does not want that runs with
`-flowmeshapiobservedtime=0 -flowmeshapicandles=0`.

## 4. Remote client

The engine-off client learns both adverts leniently. A market-data read that
asks for observed time and ticks adds `observed_time: true` to its ticks
request, only for an endpoint that advertises it, never on a signing preflight;
an unknown-field refusal withdraws both for ten minutes. Rows are kept beside
the tick ring, only for held ticks whose hash they carry, one endpoint's series
only (another endpoint starts again), with only the public flags taken. They
are flagged `implausible`, never corrected, when a live time goes backwards
with sequence, when any time is more than 5 s after it was received, or when
the head's time is before the block that made its anchor final less 120 s.
Replies label the section with the endpoint's URL or say why there is none
(`endpoint_does_not_advertise_observed_time`, `observed_time_not_yet_received`).

`Candles` asks the endpoint serving the market's data, only when it
advertises candles. The work gate is held only to read that endpoint and the
verified head, never across the request, which uses a lane with its own TLS
connection (busy fails at once), so an order never waits for a candles page.
The page is structure-checked (interval, window, aligned ascending opens,
non-overlapping sequences none past the verified head, consistent prices,
exact volumes) and stays labelled endpoint-reported: its prices, volumes and
times are the endpoint's claims.

## 5. Qt

- **Chart.** With observed times the chart draws UTC candles (1m…1d, default
  5m): the source's page, continued with the held ticks' own times through the
  node's bucketing (never recomputed, never double-counted). A proven quiet
  stretch is a dotted line at the earlier close; a gap of auctions without an
  observed time is hatched and counted; a clock seam is marked. Partial candles
  are hollow. After the newest candle the axis is blank. Wheel zoom (10-500
  buckets), drag pan, double-click for the latest; painting covers visible
  buckets only. Headers: locally "Certified execution · times observed by this
  node (not certified)", remotely "Prices, volumes and times reported by
  <host> · not verified"; the axis names the observer and says UTC. Without
  times (an older server) the chart keeps auction order ("Every 5 auctions")
  with the note "This server does not report times (older version)".
- **Candle reads.** A 300-interval window after a market, interval or source
  change (or at most every 2 s while the held ticks cannot continue it), else
  the newest three intervals about every 10 s. A failed candle read never fails
  the market read.
- **Trades tape** (liquidity card, "Trades"): the newest 50 matched auctions,
  Price | Size | Time. A uniform auction has no taker side: a price is coloured
  by its change from the previous auction. Time is local `HH:mm:ss` only for a
  live observation; late, untimed or unrecorded auctions show "—" (catch-up
  imports stay without a time); tooltips give the observer, UTC to the
  millisecond (or the late upper bound), flags and the auction number.
- **Older servers.** The tape may show when this app received an auction, only
  for a reply that brought exactly one new auction while the link was Live and
  without an event gap; labelled "received by this app", never a chart input,
  dropped as soon as the source reports times.
- **Wording.** The clearing strip ("Last auction 12:34:56 · …"), the trade
  history's Time column, the progress line, toasts and gap notes no longer show
  microblock numbers; each keeps the auction number in its tooltip.

## 6. Compatibility

| Client / server | c10c952 / f8a3432 | new, both flags 0 | new (default) |
|---|---|---|---|
| older client | unchanged | byte-identical | status objects add `observed_time_v1` and `observed_candles_max` (read leniently); never sends either |
| new client | no adverts: never sends them; "does not advertise" reasons; auction-order chart, receipt-time tape | same | times and candles |

Wallet RPC callers that pass neither option get byte-identical replies.

## 7. Provisional defaults (owner may revise)

`-flowmeshapiobservedtime=1`; `-flowmeshapicandles=500`; UTC chart with a
local-time tape; tape colour by price change; the receipt-time fallback for
older endpoints; millisecond public stamps; warm-up `max(2 s, 2 x round
timeout)` with an interruption tolerance of one round timeout; anchor margin 2
blocks; deferral reach 16; retention and windows as in §1; default interval 5m.

## 8. Not included: V2 certified time

A proposer-set `time_ms` in a new, activation-gated entry version would change
the commitment layout, entry hash, certificate digest, type-8 checkpoint
reconstruction, store and wire; V1 history would stay without it. Checkable on
replay: `time_ms >= parent.time_ms` and `time_ms >= nTime(anchor) x 1000 -
120 s`; a signing gate could refuse a time more than a small bound ahead of the
signer's clock ("held, not invalid"). There can be no past bound: locked
candidates are re-proposed under the same hash, so a Byzantine proposer could
hold "certified" time arbitrarily far behind (only `>= parent` is enforced),
and the anchor bound is about 30 minutes loose, so calling it execution time
would overstate it. It touches contract §37 (the gate reads a wall clock), §40
(a bound encodes a clock assumption) and handoff §5.5 (execution depends on it
once expiry or funding reads it), and must be co-designed with V2 funding and
oracle time. It is a separate protocol decision; nothing here implements it.

## 9. Tests

Unit: `flowmesh_observed_time_tests` (UTC alignment, exact OHLC and 128-bit
volumes, late placement only inside a bound, partial and seam flags, quiet
proof, liveness matrix including a level peer, warm-up tolerance, paging,
retention, client extension), `flowmesh_observed_time_log_tests` (clock clamp,
step, untimed, restart; persistence and reload; lost tail is a gap; corrupt
database; rows past the store head; hash mismatch; pending overflow; flush
failure; a stalled flush never blocks Record or reads; candle JSON),
`flowmesh_runtime_tests` (recorded after commit with its liveness, catch-up and
deferral late, level peer live, throwing clock, the market-data section),
`flowmesh_client_ticks_tests` (adverts, row binding and plausibility, candle
page checks, round trip), `flowmesh_client_market_view_tests` (public contract
on and off for both flags, rate limit, remote labels and candles, older
endpoint reasons). Qt: `test_b3_flowmeshworkspace-qt` (parsing, requests,
time candles, viewport, fallback, tape, wording, receipt times). Functional
(opt-in, run directly): `feature_flowmesh_observed_time.py`.
