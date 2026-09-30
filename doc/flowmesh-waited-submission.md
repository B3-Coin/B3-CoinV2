# FlowMesh waited submission (one-round-trip trades)

Status: implemented on `feat/flowmesh-hl-learnings-claude`. Offered by
validators by default (`-flowmeshapisubmitwait=1`, inside the existing
`-flowmeshapiactionwait` policy), used by a client only when a caller asks for
a wait. This is a transport and latency feature only: no consensus rule,
signed format, fee, economic rule or FMN2 message changes, and nothing here
is evidence. Every inclusion is still verified by the client under its local
B3 authority. The validator default (on, which adds one ignored key to status
objects for older clients, as `action_wait_ms_max` did) is for owner review.

Before this change a remote trade needed, after signing, a `submit` request
and then a separate waited `action` read for the certified inclusion. Two
changes shorten that path:

1. **Lane borrow (client only).** The waited read uses its own TLS
   connection (the wait lane), which goes cold after 25 s without a waited
   read. A sporadic trader paid a new TCP and TLS setup, plus a fresh CA load,
   on every trade. The waited read now carries the ordinary connection when
   only that one is warm, and hands it back afterwards.
2. **Waited submission.** Where the endpoint offers it, the submission itself
   waits for the admitted action's certified inclusion and returns it: one
   request per trade instead of two.

Section 7 gives the expected effect. It is a model until measured on the
owner's WAN deployment.

## 1. Validator: `submit` with `wait_ms`

| Flag | Default | Meaning |
|---|---|---|
| `-flowmeshapisubmitwait` | 1 | Also offer the `-flowmeshapiactionwait` wait on `submit`, in the same `-flowmeshapiactionwaiters` slots. 0, or disabled action waits, keeps the previous `submit` contract byte for byte. |

When offered, every status object (`markets`, `snapshot`, `updates`)
advertises `submit_wait_ms_max` (the action-wait maximum, at most 2500), next
to `action_wait_ms_max`. A client must read it before sending `wait_ms` on
`submit`. An endpoint without it answers with the unchanged error "Unknown or
duplicate client API field", before decoding or admitting anything.

A waited submission is an ordinary `submit` plus `wait_ms` (any unsigned
integer, clamped to the advertised maximum) and, where `action_outcome_v1` is
advertised, optionally `"outcome": true`. `wait_ms` and `outcome` are checked
before the action is decoded, like every other `submit` check, so an error
reply still means that the request admitted nothing. `outcome` without
`wait_ms` is refused.

The reply is the unchanged submission receipt plus:

| Key | Meaning |
|---|---|
| `action` | Exactly the `action` reply for the same ActionId after the bounded wait (receipt, the durable `certified_payload` or `evidence_error`, the opt-in outcome parts, and its own `wait_status`). Present only when the submission was not refused. |
| `wait_status` | A latency hint, never evidence: the embedded reply's (`none`, `busy`, `deadline`, `untracked`, `terminal`, `timeout`, `interrupted`, `restarted`), `none` for a refused submission, `budget` when the extra request token was spent, or `failed`. |

The embedded read never fails the request: the client always learns what the
request admitted. It is the same code as `action` (`TradingApi::ActionReply`),
so a certified reply is built from the durable entry under the commit's market
lock, never from the event.

**Bounds (none loosened).**

- The wait uses the action-wait slot pool: at most 12 slots, 2 per IPv4
  address or IPv6 /64, taken without blocking (`busy` at once), with the HTTPS
  permit released while sleeping and the same thread count. It ends at least
  one second before the request's HTTPS deadline, and API stop ends it.
- A refused submission never waits. A waited submission only waits for the
  action this request just had queued (or finds certified), which is no more
  than an `action` wait on any public ActionId already allows.
- The embedded read costs one more read token after admission, like the
  separate `action` read it replaces; outcome parts cost what they cost on
  `action`. A trade therefore spends the same tokens as before, in one request.
- A waited submission holds a slot for about the time to certification after
  admission. A remote client's waited `action` read used to arrive one round
  trip later and often found the action certified already, so slot occupancy
  per trade is higher. With the default 4 waiters, about 4 / C waited
  submissions per second fit before `busy` (C = admission to certification,
  roughly 70-150 ms on the benchmark hosts).

## 2. Client: when a submission waits

`Submit` (and the wallet RPCs below) takes an optional wait, 0 by default.
With a wait:

- A **new** action (never delivered before) whose ordinary submission endpoint
  advertises `submit_wait_ms_max`, and that is neither in transport cooldown
  nor left unanswered by a lane request, is submitted with `wait_ms` on the
  wait lane. The lane is try-locked before `m_work`; a busy lane means the
  ordinary path. One automatic status attempt is charged, as for the waited
  read it replaces.
- Anything else is delivered by the ordinary path, followed by at most one
  waited `action` read of the delivered action (what a caller's
  `getflowmeshactionstatus ... wait_ms` would send right after), which already
  helps against endpoints that only offer action waits.
- If the endpoint did not wait (`busy`, `deadline`, `budget`, `failed`), one
  waited `action` read may follow in the same call.

A previously delivered, certified or proven-not-executed action never takes
the lane, and nothing is ever signed again.

### Invariants of the lane submission

- **Write-ahead.** Before any byte can leave, under `m_work`, the delivery
  record (`may_have_been_sent`, `unknown`) is made durable by a synchronous
  outbox write, exactly as the ordinary path does; the row itself was saved
  when it was retained. Nothing after that write throws.
- **Never held across the network.** `m_work` is released during the request
  and the endpoint's wait; ordinary reads, submissions and watches continue.
- **In flight.** The row stays retained, so a different action for its
  account sequence is still refused. It is never evicted and never sent again
  while in flight (a concurrent resend returns its receipt with an "in
  flight" reason), and it is absent from the saved view until the call
  returns, so the view still only shows completed methods.
- **Always released.** The flight is released under `m_work` on every path,
  including an exception (a guard takes `m_work`): the row is published
  again, the carried connection handed back and the lane unlocked. An
  exception escaping the application becomes `unknown`, never a refusal.
  A unit test injects an unwind after the request (a test-only hook that
  nothing in the node sets) and checks both: the row is back in the saved
  view as possibly sent, a different action for its sequence is refused, its
  status and exact retry work, and a later waited read takes the lane. The
  guard starts once the request is prepared; an unwind between marking the
  row in flight and that point needs two failed allocations in a row and is
  not covered.
- **Applied by the existing rules.** The acknowledgement is applied as the
  ordinary submission applies it: an inclusion label becomes `unknown`; a
  refusal of this first and only delivery is definite, unless a concurrent
  read already observed the action queued or admitted, which one
  acknowledgement never undoes. The embedded `action` reply is applied by the
  same code as a waited read: the certificate is verified under local B3
  authority, a verified label is saved before it is returned, and a failed
  save returns it unverified. A verified or proven receipt from a concurrent
  read is never replaced.
- **Resends.** Only the exact retained bytes, and only where the ordinary
  path would send them: without `wait_ms` after an unknown-field refusal (the
  capability is withdrawn until advertised again; a refusal of that resend
  stays `unknown` because the waited attempt may have been delivered), or to
  the other endpoints after an unusable or missing reply. A single endpoint
  that did not answer leaves the action `unknown` and marked unanswered for
  the lane, with no resend; resolve it with `getflowmeshactionstatus` or
  `retryflowmeshaction`, never a replacement.

The bounded cost of a silent single endpoint is `wait + 5 s` (7.5 s at the
cap), then `unknown`, against 5 s on the ordinary path.

### Lane borrow

Under `m_work` with the lane held, before a lane request: a pending
Connect/Reconnect trust reset is consumed first; then, when the lane's own
connection would not be reused for the target but the ordinary one would, the
two sessions swap. After the reply, under `m_work` and still holding the lane,
the connection is handed back unless an explicit reconnect reset trust
meanwhile (the lane then drops it before its next use) or ordinary work
already opened its own connection to the selected endpoint (both stay warm).
It is transport only: the same bytes go to the same endpoint, and `Request`
re-checks origin, pin and CA content before writing. While the lane carries
the connection, ordinary work that needs one opens its own, so for a client
with concurrent callers the setup cost can move rather than vanish.

## 3. Wallet RPC and Qt

`submitflowmeshorder` (argument 6), `cancelflowmeshorder` (4) and
`requestflowmeshwithdrawal` (6) take an optional trailing `wait_ms`, 0-2500,
default 0 (unchanged). It is parsed before anything is signed. The result
schema is unchanged; a receipt that is not verified in time is returned as
without a wait. The local engine ignores it. The wallet drops its copy of the
signing key right after signing, so no key copy is kept across the wait.

```
b3coin-cli submitflowmeshorder <market_id> bid 10000 500 7 2000
```

Qt passes `wait_ms` 2000 only for a remote order, cancel or withdrawal from a
wallet that was **already unlocked**. A wallet the panel unlocks for the
request is relocked when the action job ends, and waiting inside the
submission would keep it open longer; those trades keep the submission plus
one waited status read, which the lane borrow now makes cheaper. A receipt
not verified by the submission still gets exactly one waited status read.

## 4. Compatibility

| Client, server | Behaviour |
|---|---|
| c10c952 or f8a3432 client, any server | Unchanged requests and replies. Status objects may carry one more key (`submit_wait_ms_max`), which they ignore, as they ignore `action_wait_ms_max`. `-flowmeshapisubmitwait=0` restores f8a3432 status bytes exactly. |
| New client without a wait | Request bytes identical to f8a3432 on every server. The lane borrow is transport only. |
| New client with a wait, c10c952 server | Plain `submit` only (nothing advertised). |
| New client with a wait, f8a3432 server | Plain `submit`, then one waited `action` read (the f8a3432 Qt request bytes). |
| New client with a wait, new server | One waited `submit`. With `-flowmeshapisubmitwait=0`: plain `submit` plus one waited `action`. |
| Stale advertisement (downgraded server) | The server refuses `wait_ms` before admission; the client withdraws the capability and resends the exact bytes without it. |

Peers, FMN2, consensus and the outbox format are unchanged.

## 5. Tests

- Unit (`flowmesh_https_tests`): `Warm`/`Swap`; the lane borrow, hand-back,
  reconnect and concurrent-ordinary-work cases (`flowmesh_client_wait_lane_tests`);
  the server contract (`flowmesh_api_submit_wait_tests`); the client paths
  (`flowmesh_client_submit_wait_tests`): one request that verifies, outcome
  opt-in only where offered, unknown-field fallback, no reply on one and on
  two endpoints, the hidden in-flight row, a refusal after a concurrent
  observation, a failed outbox save, a busy endpoint, and retained actions
  never on the lane. Helper rules: `flowmesh_client_action_wait_tests`.
- Functional (`feature_flowmesh_action_wait.py`, real validators): advert and
  refusal, malformed wait, timeout with seats disarmed, `busy` shared with
  waited reads, a certified resubmission answered at once, shutdown ending a
  waiting submission, and `submitflowmeshorder ... wait_ms` making exactly one
  `submit` request (no `action` request) and returning `certificate_verified`.
- Qt: `Parameters` and `SubmissionWaitMs`; a verified submission receipt starts
  no follow-up read.
- Not covered: a crash between the lane write-ahead save and the request. The
  existing `ClientCrashBeforeFirstSend` hook still sits before both paths; no
  separate crash-test build was run for this change.

## 6. Measuring it

`feature_flowmesh_persistent_trades.py --submit-wait-ms=2000` runs the
benchmark with waited submissions and reports `submit_wait` and
`summary.transport.measured_window_waited_submissions`. Like a waited read,
the submission's wait is cut to end when the next B3 pump tick is due, so on
regtest it is often shorter than certification and the ordinary status loop
finishes the sample; pair it with `--status-wait-ms=2000` to compare against
the two-request arm. A three-sample co-located smoke run on this branch made
exactly one waited submission per order and no separate submit request.
Co-located runs show no gain by construction. The claims below need the owner's WAN deployment:
trades spaced more than 30 s apart (the borrow) and back to back (the waited
submission), each before and after, reading `client_action_wait` and
`client_submit_wait` spans (`borrowed`, `connection_reused`,
`tls_handshake_performed`, `wait_status`, `result`).

## 7. Expected effect (model, not measured on WAN)

With R the client-to-endpoint round trip and C admission to certified
inclusion on the endpoint:

| Case | Before | Lane borrow | Waited submission |
|---|---|---|---|
| Sporadic trade (lane cold) | a + max(4R + q + s, R + C) + v | a + max(2R + q, R + C) + v | a + R + C + v |
| Frequent trade (lane warm) | a + max(2R + q, R + C) + v | same | a + R + C + v |

(a = admission, q = client turnaround between the two requests, s = cold
TLS/CA setup, v = client verification and outbox write.) At R = 220 ms and
C = 70 ms the borrow saves about 2R + s (roughly 450 ms) for sporadic trades,
and the waited submission a further max(0, R + q - C), about 150 ms. Nothing
changes co-located. The observed co-located functional run returned a
verified inclusion from `submitflowmeshorder ... 2000` in about 190 ms with
one HTTPS request for the action.
