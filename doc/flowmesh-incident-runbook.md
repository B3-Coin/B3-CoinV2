# FlowMesh incident runbook

How operators detect, classify, act on and communicate FlowMesh incidents.
The codes come from this revision's health v1 object (`getflowmeshhealth`
over local RPC, or the public `health` method of the restricted HTTPS API)
and from the market status objects; codes marked reserved are never reported
by this revision. A code you do not recognise means not ready.

Health is a local observation of one node. It is never certified evidence,
never a proof of the network's freshest head, and never an input to
admission, relay, signing or verification. Clients display an endpoint's
advertised readiness (`getflowmeshclientinfo`); they do not switch endpoints
because of it.

## 1. Principles

- A FlowMesh pause or halt never stops B3. Block download, the mempool,
  mining, staking, finality and ordinary B3 transactions continue
  ([production contract, section 1](design/b3-flowmesh-v1-production.md)).
- Never delete or reset a signing journal, a production store or certified
  history. Never run two signers with one key, not even to test connectivity.
  A `signing-conflict` must never be cleared by deleting history
  ([operator RPCs](flowmesh-operator-rpcs.md)).
- Never roll back a certified log. Recovery from `anchor-invalidated` or
  `certificate-conflict` needs a separately versioned, owner-approved
  procedure. This runbook does not contain one.
- Never re-sign an action whose outcome is unknown. Users must not place a
  replacement for an order whose outcome is still unknown; the client keeps
  and retries the exact saved action.

## 2. Topology

- Seats keep the restricted API off (`-flowmeshapi=0`, the default) or on its
  default loopback bind.
- Public API traffic goes to observer nodes: `-enableflowmeshvalidator=1
  -flowmeshrole=observer -flowmeshapi=1 -flowmeshapibind=<address>
  -flowmeshapicert=<file> -flowmeshapikey=<file>`, with no armed keys. Run at
  least two per region.
- Ports: FMN2 (validator network) 5649, restricted HTTPS API 5650.
  *Reported conflict, not resolved here:* the production contract's section 1
  says "v1 opens no separate port". Both ports are separate listeners.
- `-flowmeshapihealth=1` (the default) serves `health` and the readiness keys.
  With `=0` the API keeps its previous contract exactly.

## 3. Monitoring and the status page

**Source of truth.** Poll `getflowmeshhealth` over local RPC on every API node,
or the public method on a loopback- or allowlist-bound API instance kept for
monitoring. The public method on an internet-facing listener shares the API's
admission budget, so anyone can push a public poller into HTTP 429.

```
b3coin-cli getflowmeshhealth
curl -sS --cacert ca.pem -H 'Content-Type: application/json' \
  -d '{"method":"health","params":{}}' https://127.0.0.1:5650/flowmesh/v1
```

The public method allows four calls per second per client address (IPv6 per
/64) and caches the all-markets reply for 250 ms. Poll every 15 s.

**Reading the object.**

- Route by `service_ready` and each market row's `ready`. Top-level `ready` is
  only a label: it is false whenever any one market is not ready, so it must
  not drain a node from a load balancer.
- `state` gives one label per market (section 6).
- `head_age_ms` of an idle market grows legitimately, because FlowMesh never
  produces empty microblocks. Do not alert on head age alone.
- `pause_reason` `b3_reconciliation_pending` is the normal pause at every B3
  block while the chain gate is closed. It does not change `state` and must
  not raise an alert. A gate that stays closed is reported as
  `reconciling_prolonged`.

**Poller rules.**

- HTTP 429, timeouts, TLS or connection errors are UNKNOWN, never not ready.
- Debounce: change a component only after 3 consecutive polls agree.
- Per market, across the API nodes:

| Component | Condition |
|---|---|
| Operational | At least one API node (target: two) reports the market ready with `state` `live` or `idle`. |
| Degraded | Any API node reports the market not ready, or `state` is `paused`, `handoff_pending` or `catching_up` on every reporting node. |
| Major outage | `halted` on a majority of API nodes, or no API node has reported the market ready for more than 2 minutes. UNKNOWN polls never count here. |
| Maintenance | An announced window. |

- Change the component automatically on detection. A trading freeze is never
  recorded as "no impact".

## 4. Severity and clock

| Severity | Examples |
|---|---|
| SEV1: integrity or security | `certificate-conflict`; `anchor-invalidated`; `store-failure` whose error is "durable execution result disagrees with independent execution"; any bridge incident. |
| SEV2 | A market halted, or not ready on every API node, for more than 10 minutes. |
| SEV3 | One API node not ready while others are ready. |
| SEV4 | Expected pauses: `genesis_checkpoint_pending`, `settlement_checkpoint_pending`, planned handoffs. |

Clock, as an operator policy. **The public-notice targets below need the
owner's sign-off before they are published as a commitment.**

- T+0: automatic detection changes the status component.
- T+5 min: on-call acknowledges.
- T+10 min: first public notice (templates in section 7).
- Updates every 30 minutes for SEV1 and SEV2, every 60 minutes for SEV3.
- A resolution notice, then a postmortem within 5 days for SEV1 and SEV2.

## 5. Diagnostics bundle

Collect, for the incident's UTC interval:

- `getflowmeshhealth`; `getflowmeshdeliveryinfo <market_id>`;
  `getflowmeshnetworkinfo`; `getblockchaininfo`; `getchaintips`;
  `listflowmeshmarkets`;
- the FlowMesh lines of `debug.log`;
- the version, the source commit and the platform.

Never collect or share passphrases, wallet files, private or BLS keys, RPC
cookies, TLS keys or complete signing journals. Do not post a full
configuration or a full debug log publicly.

## 6. Code table

### Service (`service.state`, `not_ready`)

| Code | Meaning | Operator action | User message |
|---|---|---|---|
| state `dormant` | The A2/A3 schedule is incomplete; FlowMesh never starts. | Expected before activation. | "Trading is not yet active." |
| state `stopped`, `service_stopped` | The service is not running on this node. | Check the process and `debug.log`; restart. | none (SEV3) |
| `initial_block_download` (gate reason too) | B3 is still syncing. | Wait for the initial block download. | none |
| `below_activation` (gate reason too) | The B3 tip is below FlowMesh activation. | Expected before A3. | "Trading is not yet active." |
| `reconciling_prolonged` | The chain gate has been closed longer than `reconcile_grace_ms` (10 s). `chain_gate_reason` says why; see below. | See the gate reasons. | "Trading is delayed while the node catches up with B3." |
| `worker_stalled` | No runtime observation for `worker_stall_ms` (5 s): the worker is busy or wedged. | Take the bundle, then a clean restart. Escalate if it recurs. | none unless on every node |
| `no_flowmesh_peers` | No FlowMesh peer for `no_peers_grace_ms` (30 s). | Check `getflowmeshnetworkinfo` targets, the firewall on 5649 and `-flowmeshconnect`; `flowmeshconnect` adds a target at runtime. | none unless on every node |

Chain gate reasons (`service.chain_gate_reason`). Every one of them retries at
the next B3 tip. If the gate stays closed for three or more blocks, take the
bundle; a clean restart is permitted (the durable store reconciles again at
start); then escalate.

| Reason | Meaning |
|---|---|
| `starting` | Start-up reconciliation is running. |
| `reconciling` | Ordinary reconciliation after a B3 tip or a block disconnection. |
| `idle_wait_timeout` | The runtime did not go idle within 5 s for checkpoint reconciliation. |
| `store_reconciliation_failed` | Durable checkpoint reconciliation failed, or the B3 tip changed after checkpoint connection. |
| `market_refresh_failed` | Market discovery failed (see `debug.log`). |
| `checkpoint_reconciliation_failed` | Connected-checkpoint reconciliation failed. |
| `tip_changed` | The B3 tip changed during reconciliation. |

### Market readiness and state

| Code or state | Meaning | Operator action |
|---|---|---|
| `market_not_installed`, state `not_installed` | The service knows the market but the runtime has not installed it (for example a store that failed to open). | Check `debug.log` for the market; take the bundle; escalate. |
| `market_halted`, state `halted` | The market halted on this node; see `halt` below. | Per halt code. |
| `catching_up` | This node has been provably behind for `lag_grace_ms` (10 s): it holds a BLS-verified certificate for a sequence it has not applied (`catchup.proven_target`, `catchup.proven_behind_ms`). That cannot be faked or hidden. | Normal after a restart. Escalate if it lasts more than 10 minutes. |
| state `catching_up` | Proven or speculative catch-up is under way. `catchup.speculative` means a verified page arrived without tail slack; it is not a proof and never makes the market not ready. | none unless `catching_up` is reported |
| state `insufficient_seats` | Fewer than four anchor-final seats (`PAUSED_INSUFFICIENT_SEATS`). | Expected until seats bind; B3 continues. |
| state `handoff_pending` | A committee handoff waits for its checkpoint to be 30 blocks deep. | ETA: max(0, 30 - (tip height - handoff checkpoint inclusion height)) x the observed B3 block interval. Publish the checkpoint first if it is not yet included. |
| state `paused` | Paused for `pause_reason`; see below. | Per pause reason. |
| states `live`, `idle` | Ready; `idle` only means no head in the last 60 s. | none |
| `stalled_with_pending_work`, state `stalled` | Reserved. This revision never reports them. | none |

### Halts (`halt`)

| Code | Severity | Operator action |
|---|---|---|
| `store-failure` | SEV2; SEV1 when the error is "durable execution result disagrees with independent execution" | Check disk and I/O. Stop the node, take a read-only copy of the FlowMesh datadir, fix the cause and restart. For the SEV1 variant, compare `state_root` at the same sequence across operators and notify the owner. |
| `signing-conflict` | SEV2 | Disarm with `stopflowmeshvalidator` (safe). Verify that only one signer uses the key. Never clear it by deleting history. The market continues while quorum remains. Escalate. |
| `certificate-conflict` | SEV1 | Possible equivocation by at least a third of the seats. Preserve all evidence, change nothing, notify the owner immediately. |
| `anchor-invalidated` | SEV1 | A reorg removed a committed anchor. Check `getchaintips` and finality. Owner-approved procedure only. |
| `invalid-config` | reserved | Never emitted in this revision. |

### Pauses (`pause_reason`)

| Reason | Operator action |
|---|---|
| `b3_reconciliation_pending` | Normal at every B3 block; no action. |
| `genesis_checkpoint_pending`, `settlement_checkpoint_pending` | Publish the pending checkpoint: `createflowmeshcheckpoint <market_id>`. |
| `handoff_checkpoints_pending` | Publish the pending checkpoints. How long this pause may last (about 30 minutes or more) is an open owner ruling; this runbook documents the current behaviour only. |
| `settlement_plan_unavailable`, `next_seat_set_unavailable`, `seat_context_mismatch`, `seat_epoch_exhausted`, `seat_transition_paused` | Take the bundle and escalate. |
| `retained_candidate_restore` | A restarted signer is restoring its retained candidate; clears after reconciliation. Escalate if it persists. |
| `insufficient_seats` | See state `insufficient_seats`. |

The 4,096-effect checkpoint backlog pauses deposits and withdrawals, not
trading (production contract, section 9). Publish a checkpoint.

### Public API

| Observation | Meaning | Action |
|---|---|---|
| HTTP 429 "Public trading request budget exhausted before admission" | The API's admission budget, by design. | Add observer API nodes. |
| HTTP 429 "Public health request budget exhausted" | More than four `health` calls per second from one address. | Slow the poller; monitoring should use local RPC. |
| `wait_status` `busy` | All bounded action-wait slots are taken (waited `action` reads and waited submissions share them). Clients fall back to one waited status read or the passive cycle. | Add waiters (`-flowmeshapiactionwaiters`) or API nodes. |
| Waited submissions suspected (for example after an upgrade) | `-flowmeshapisubmitwait=0` stops offering them; clients withdraw the capability on the next status object and use a plain submission plus one waited read. | See [waited submission](flowmesh-waited-submission.md). |
| `watch_status` `busy`; Qt shows "live-update capacity full" | All bounded `updates` watch slots are taken (at most 10 per node, 2 per address). Clients poll every 0.5 s meanwhile; nothing else is delayed. | Add API nodes; see [live updates](flowmesh-live-updates.md). |
| Qt shows "No live update for 15 s · reconnecting" | No verified live update for 15 s; the panel polls again. | Check the endpoint's health and the client's connection line. |
| TLS or certificate-pin errors | Client trust configuration. | Check the CA bundle and `-flowmeshendpointpin`. |

## 7. User messages

Keep every message factual. Never promise an outcome the operators have not
verified.

- **First notice (SEV2 to SEV4):** "Trading on <market> has been
  <paused/degraded> since <UTC>. A pause does not change certified balances
  or orders; your app shows each order's outcome as soon as it is verified.
  Do not place a replacement for an order whose outcome is still unknown.
  Next update by <UTC + 30 min>."
- **First notice (SEV1):** "Trading on <market> has been halted since <UTC>
  while we investigate an integrity alert. Do not place new orders on this
  market. Next update by <UTC + 30 min>." Do not state that balances or orders
  are safe until the owner confirms it.
- **Update:** "<market>: <what changed>. Next update by <UTC>."
- **Resolved:** "Trading on <market> resumed at <UTC>. Your app shows the
  outcome of every saved order as soon as it is verified."

## 8. Planned maintenance

- Announce at least 24 hours ahead.
- Restart one seat at a time. With k seats, never have more than
  k - (floor(2k/3) + 1) of them down at once; for k = 4 that is one.
- Before the next restart, wait until the restarted node's
  `getflowmeshhealth` reports `service_ready` and the market rows ready.

## 9. Waiting on owner rulings

These are referenced here and not acted on:

- how long `seat_transition_paused` and `handoff_checkpoints_pending` may last;
- bridge incident response (R6);
- the slow-seat proposer skip (R13);
- the public-notice targets in section 4.
