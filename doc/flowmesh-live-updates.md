# FlowMesh live updates (bounded `updates` watch)

Status: implemented on `feat/flowmesh-hl-learnings-claude`, **off by default
on validators** (`-flowmeshapiwatch=0`). Enabling it by default waits for an
independent review of the pending-checkpoint memo and for the cost of
concurrent watchers to be measured on the deployment hardware (section 6).
This is a transport and latency feature only: no consensus rule, signed format,
fee, economic rule or FMN2 message changes, and nothing here is evidence.

A remote trading client used to learn about a new certified microblock only on
its next 500 ms poll. With live updates, the client holds one bounded
`updates` read open at its endpoint, which answers as soon as something
changes for the watched market and account. Every change is still read and
verified exactly as before: a new head goes through the unchanged snapshot
path (certificate against locally anchored seats, whole-state bytes, canonical
anchor, B3 checkpoint non-regression, durable high-water). Balances and books
still come only from verified state.

This is event-driven long-polling on a kept-alive connection, not a stream,
and not the thread-free waiter of stage 3 in the
[persistent-session design](design/flowmesh-persistent-trading-session.md):
each sleeping watch occupies one worker thread (with no HTTPS permit), within
the API's existing 16-worker bound.

## 1. Validator: the watched `updates` read

| Flag | Default | Meaning |
|---|---|---|
| `-flowmeshapiwatch=<ms>` | 0 (off) | Longest time one `updates` read waits; at most 2500. 0 keeps the previous request and reply contract byte for byte. |
| `-flowmeshapiwatchers=<n>` | 8, reduced to the room left | Concurrent sleeping watches, at most 10, at most 2 per IPv4 address or IPv6 /64, in a pool separate from waited `action` reads. |

Action-wait slots and watch slots together never exceed 14 sleeping workers
(16 workers minus the 2 active permits). Without an explicit
`-flowmeshapiwatchers` the default shrinks to the room the action waiters
leave (for example 2 with `-flowmeshapiactionwaiters=12`), so every
configuration that started before watches existed still starts. Only an
explicit count beyond that room is refused, and only while watches are on.

When enabled, every status object (`markets`, `snapshot`, `updates`)
advertises `updates_watch_ms_max`, exactly like `action_wait_ms_max`. A client
must read it before sending `watch_ms`; an endpoint without the capability
answers `watch_ms` with the unchanged error "Unknown or duplicate client API
field".

A watched request is an ordinary `updates` request plus `watch_ms` (any
unsigned integer, clamped to the advertised maximum). It may not carry
`limit` or `before_sequence`. The endpoint returns at once, without taking a
slot, when the request has no cursor (`no_cursor`), `known_head` is no longer
the head (`head_changed`), an event for the filter or a gap already exists
after the cursor (`ready`, `gap`), or the market is unknown (HTTP 400).
Otherwise it takes a watch slot, releases its HTTPS permit and sleeps until an
event for the market and account filter is appended (a certified head is
relevant to every account), the cursor falls out of the event ring, the
runtime restarts, the API stops, or the deadline: the smaller of `watch_ms`
and one second before the request's HTTPS deadline. No free slot is an
immediate reply with `busy`; a deadline already too close is `deadline`.

The reply, read after the wait in the unwaited order, is the ordinary event
page (`cursor`, `gap`, `more`, `oldest_event_id`, `latest_event_id`,
`events`), the ordinary status object, and `watch_status`, one of `none`,
`no_cursor`, `head_changed`, `ready`, `gap`, `timeout`, `interrupted`,
`restarted`, `busy`, `deadline`. It never carries `reported_data`, and the
server refuses a watched reply over 512 KiB (256 worst-case events are about
410 KiB).

Cost: each watch is one request charged by the API's admission budget (as
every read); a wake costs two status-only market reads (no curves, account or
history) and the pending-checkpoint status, which is now retained per exact
store marker instead of scanning the store on every status object. The
event-log append signals only the watchers an event is relevant to and
notifies them after releasing the log mutex. On the development Mac a
six-event commit append takes about 0.3 us with 10 idle watchers, and about
9 us (p50; p90 13 us, max 24 us) when it wakes 10 of them.

## 2. Client: `getflowmeshmarketdata` with `watch_ms`

`getflowmeshmarketdata <market> {"known_head": ..., "watch_ms": 1500, ...}`
waits at most `watch_ms` (0-2000; at most 1500 is used) for a change and then
returns the same verified data plus a `watch` object: `status` (the closed
set above plus this client's `not_eligible`, `lane_busy`, `superseded`,
`unsupported`, `failed`, `revalidate`, `invalid`, `ticks_catchup`, `other`),
`supported`, `waited`, `gap`, `endpoint` and `endpoint_live_ms`. When
`waited` is false, callers pace their next read themselves (at least
500 ms). `ticks_catchup` (an eligible watch replaced once per head by an
ordinary read for missing clearing ticks) has `waited` true and keeps the
watch connection.
`watch_ms` 0 is an ordinary read that also closes the live-update
connection. The wallet is held for at most `watch_ms` plus 5 s.

The remote client sends a watch only for a market whose verified snapshot it
already holds, to the endpoint that served it and advertised the capability,
while that cache is current under local B3 authority, in the same account
scope, outside transport cooldown and not left unanswered by an earlier
watch. The wait is at most 1500 ms and shrinks so that twice the measured
lane overhead still fits a 2.4 s cycle, keeping replies inside the 3 s
display freshness gate on slow links. The watch uses its own TLS connection,
so the client's work gate is never held while an endpoint sleeps and orders,
receipts and other reads proceed meanwhile.

After its wait the client checks local authority again (a reorganized anchor
or a newly connected B3 checkpoint means an ordinary re-verifying read), and
never applies a reply for a cursor another read has already moved. An
unknown-field error withdraws the capability and the same call reads
ordinarily; a transport failure marks the endpoint unanswered (no cooldown)
and returns the error; there is no failover walk inside a watch. After
`busy`, `deadline`, `interrupted`, unrecognized or failed replies, and before
any read that does not watch, the client closes the watch connection so it
does not count against the endpoint's connection limit (64). A watch never
signs, sends or resends an action and never changes a retained instruction.

`getflowmeshclientinfo` endpoint rows show `live_updates_ms`, the watch
maximum each endpoint last advertised (0: none).

The local engine (a validator's own wallet) waits on its own event log when
the caller names the head it holds, with at most four local watchers; without
`known_head` it answers at once.

## 3. Event gaps

`verification.event_gap` is now raised only when events may really have been
missed: an endpoint-reported gap, a snapshot from another or a restarted
endpoint's event log, a restart of this client, or a wallet or authority
change. A head change read through a snapshot no longer counts as a gap: the
client resumes from the page that reported it, so the events in between are
still delivered. The label stays sticky once raised. Events remain hints
only; balances, orders and action outcomes come from verified state.

## 4. Qt

The trading panel keeps one live-update read outstanding for the selected
market on a second worker while it is visible and idle (with the local
engine also while a review is open, for display only; see below). While live, the
500 ms tick keeps display ageing and the auxiliary Catalog, Status, Effects
and Receipt cycle (so checkpoint eligibility and saved requests still
refresh every 5 s); only the market read of a passive refresh is left to the
watch. Foreground, review and action reads are unchanged, and the trading
gates are unchanged (new orders stop after 3 s without a verified update).

Any ordinary read of the market moves the remote client's event cursor, and a
watch whose cursor moved while it slept is answered `superseded`. So a probe
(trying live updates, before the first waited reply) is armed on the tick
before its poll, only when no passive job is in flight, and holds the market
read of the passive jobs started while it is outstanding, for at most the
watch plus 3 s. Otherwise, on a busy validator every probe would be
superseded by the poll beside it and the panel would never reach live mode.

The connection line shows the mode: "live updates", "polling every 0.5 s
(endpoint offers no live updates)", "polling (live-update capacity full,
retry in N s)", "live updates reconnecting in N s" or "No live update for
15 s · reconnecting". Replies answered without waiting are never re-sent at
once (a `ticks_catchup` read is re-armed at once and counts as neither a live
reply nor a strike); three in a row fall back to polling for 10 s, busy for 10 s, an
endpoint without the capability for 60 s, failures back off 1-30 s, and the
panel never sends more than a burst of two watch reads and then one per
500 ms, to a remote endpoint or to the local engine. Ten local reads a
second (the provisional C7 default of 2026-09-30) is held back: each local
read copies the book, the account and the store's history under the node's
market mutex, which the FlowMesh worker's tick also takes, and the
validator A/B showed the tick waiting behind such reads, worst at ten a
second (maxima of 25.7 and 200.9 ms, and 1.1 s behind one stalled read,
against at most 7.1 ms with no reader; flowmesh-latency-definition.md has
the per-holder table). It needs a node change first: local reads served
from a snapshot published per certified head, outside that mutex. A reply
that names an endpoint marks a panel remote at once. The next read starts when the pacing allows it, on
a precise timer, not on the next 500 ms tick; on a busy market that means a
remote endpoint's watch reads now reach the full two a second (they were
held to the tick before, 500-1000 ms apart), and each changed head still
costs that client a whole verified state read. When replies the
client keeps its connection for (`superseded`, `revalidate`, `lane_busy`)
send the panel from live mode or from a probe to polling, it closes the
live-update connection with one `watch_ms` 0 read.

With the local engine, the order book, chart and trades tape keep following
live updates while an order review is open. Those reads are display only:
the gates, balances, the review and what is signed keep the verified data of
before the review (the review shows exactly the values it signs), and a click
on a book level is refused while the book shows newer data than the gates.
Once the review is approved and the order is being sent (or while any other
job is in flight, such as the read that opens a review), no display read is
armed and one that returns meanwhile is dropped: a local read takes the
node's market mutex, which the submission also needs and which nothing can
preempt. At most the one live read already outstanding when the order is
sent overlaps it, as in live mode without a review. A remote client keeps
these views frozen during a review or an action, as before: its reads hold
the client's work gate, which is not preemptive, so a display read could
delay the order being sent.

A remote endpoint without live updates is polled on the 500 ms tick, never
back to back: a slow link's read (longer than 500 ms) is followed by the next
tick's, so the passive share of the client's work gate does not rise.

**A remote client (a trader's computer reading a VPS endpoint).** What
the panel does for it: a verified reply with a new head is applied and the
book repainted at once, whether it came from a live update or a poll; a
same-head reply (a quiet watch timeout, a same-head poll, including the
first one after each new head) updates status only, with no view rebuild,
book install or paint; cells change in place. What it does not change:
commit-to-screen latency (U1-remote) is the same as before this round,
because it is set outside Qt. In order of effect:

1. `-flowmeshapiwatch` stays off by default, so a remote trader polls at two
   reads a second and a new head waits for the next poll. An operator who
   serves their own trading client (the owner's VPS) can enable it now: first
   measure the `state_bytes` each new head makes the client download on that
   market, then the items of section 6. With it the next read is armed at
   once after a changed head, at most two a second.
2. Every new head costs the remote client an `updates` round trip that
   reports the head, then a `snapshot` round trip that downloads the whole
   market state (`state_bytes`, up to 8 MiB) with its certified microblock,
   BLS verification of the certificate, decoding the whole state and checking
   its root against the certified one, and a durable save of the client's
   high-water mark, all under the client's work gate, which is not
   preemptive (an order waits for a passive read already holding it). The book, levels, account and
   own-order rows come from that verified state. Removing this per-head
   download is the deferred *verified incremental state apply*: the client
   would fetch only the new certified entries (from the endpoint's
   recent-certified window, through a capability-advertised, bounded,
   admission-charged method), re-execute them with the consensus code on the
   state it already holds, check the resulting state root against the
   certificate, and fall back to a snapshot on any gap or mismatch, with no
   cold reads or BLS work under the endpoint's market mutex. It needs its own
   design and review (owner go/no-go); no consensus or signed-format change.
3. The remote book stays frozen while a review is open or an order is in
   flight (a display read would hold the work gate the order needs), and
   the remote client never gets the local engine's display-only reads.

A reported gap adds a log line, a separator row in the trade history and an
"event gap" suffix for 30 s, and the tracked request is reconciled first.
A wallet or market switch never waits for an outstanding watch; closing the
panel joins it, bounded by that one read.

## 5. Known limits

- Status-only transitions (reconciliation, pause, halt, handoff) are not
  events: while live they surface with the next watch reply, at most about
  the watch plus one round trip later (display only; the endpoint still
  refuses orders). Waking watchers on such transitions is not implemented.
- More than 10 simultaneous watchers per validator would need a higher worker
  bound or the thread-free waiter; excess clients get `busy` and poll.
- A hidden panel closes its watch connection with one `watch_ms` 0 read; a
  client that simply stops watching leaves an idle connection until the
  endpoint's 30 s idle timeout.

## 6. Before enabling by default

1. Independent review of the pending-checkpoint memo
   (`FlowMeshService::PendingCheckpointStatus`): its key, and that every
   connection and disconnection writes the connection records and the
   marker's `last_b3_checkpoint` in one batch.
2. Measure on deployment hardware with 1, 4, 8 and 10 watchers at 0 and
   5 commits/s: reply cost, extra queue delay of a concurrent submit,
   market-mutex hold at commit, accept refusals with 20-30 clients, and
   certified-to-displayed latency against the polling control.
3. Then set `-flowmeshapiwatch=2000` on the API nodes (or flip
   `FLOWMESH_API_UPDATES_WATCH_DEFAULT`).
