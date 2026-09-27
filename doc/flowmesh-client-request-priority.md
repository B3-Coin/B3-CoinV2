# FlowMesh client: trading priority with live refreshes retained

Scope: local client scheduling, based on `80ec1c1e789c53ca27d784adb913f01fe655da9e`.
No matching, consensus, fee, signature, persistence, polling-rate or TLS-policy
change. Existing Qt refresh/flicker and readiness protections remain in place.

## Why this change

Captured client work showed foreground market preflight and saved-action
ownership checks waiting behind Qt refreshes. Classifying only `Submit` as
foreground would miss those prerequisites. Separately, Qt's one worker finished
all remaining passive reads before servicing an explicit status/review click.

The deterministic before-fix regression holds market discovery, queues an
explicit status check, then releases discovery. The old panel performs another
market snapshot before the check (`snapshots == 1`, assertion expecting zero
fails). The repaired panel yields the unstarted snapshot and drains the captured
check next (`snapshots == 0`). This establishes request ordering, not a measured
end-to-end latency reduction.

## Rules

- Backend callers default to foreground. Only automatic Qt read jobs explicitly
  enter a thread-local passive scope; synchronous RPC dispatch preserves this
  context. Explicit review preflight, status, exact retry and submission remain
  foreground, including their market/outbox prerequisites.
- `FlowMeshClientWorkGate` preserves one owner across each complete backend
  method, including network, cache and durable outbox work. It is non-preemptive.
  Two FIFO queues prefer foreground. After eight foreground acquisitions while
  a passive caller waits, one passive caller gets a turn. `try_lock` cannot jump
  existing waiters. Queue heads/counters are constant-size; waiter nodes belong
  to blocked callers, whose number depends on the existing execution limits.
- Qt still has one worker. An explicit status/review click can yield **before**
  the next passive RPC, not abort an in-flight request. Repeated status clicks
  coalesce under the existing captured-wallet/market/ActionId rules.
- A yielded read does not publish partial market data, advance its auxiliary
  phase, renew snapshot age, or masquerade as a successful refresh. Any already
  completed, correctly attributed receipt observation/error is preserved.
  Actual auxiliary errors take precedence over a yield, including cached
  reconciliation-fallback errors.
- A deferred review gets a foreground fresh-market read before the existing
  identity, balance, units, freshness and confirmation checks. This is only an
  intent to open a review: it does not approve, unlock, sign, submit or replace an
  instruction. Unrelated optional refresh phases are left for later.
- After eight consecutive Qt yields, the next passive phase may finish despite
  further clicks. This preserves a refresh-completion opportunity under a stable
  schedule and eventually completing RPCs, not a wall-clock guarantee. Phase
  completion/reset replenishes that local allowance. These two eight-request
  limits are local scheduling policies, not protocol timers or quorum rules.

## Foreground linger

A trade reaches the backend as several separately gated calls: the signing
preflight (`Market`), `Submit`, then status reads. Before this rule a queued
passive refresh won each idle gap between them and held the gate for a whole
remote call. Now, when a foreground owner releases, the idle gate lingers for
`FlowMeshClientWorkGate::FOREGROUND_LINGER` (50 ms):

- a foreground caller arriving in the window takes the gate at once; linger
  never delays a foreground caller, and `try_lock` on an empty queue is still
  prompt;
- a queued passive caller waits for the window to expire (a deadline wait; no
  notification marks expiry);
- no window follows a passive owner;
- the eight-acquisition burst bound overrides the window, so a passive caller
  waits at most eight windows (400 ms) beyond the eight foreground holds. An
  isolated foreground call delays passive work by at most one window.

Under sustained back-to-back RPC trading with a visible Qt panel, a trade makes
about four contended acquisitions, so the forced passive turn lands inside
roughly every second trade. A gap longer than the window (for example a slow
process spawn between two CLI calls) still lets one passive call in, as before.
A passive call already running when a trade starts is not affected: the gate is
non-preemptive.

Gate spans (`flowmeshtiming`, default off) record `priority`, `linger_window`
(acquired inside a window) and the cumulative counters `linger_captures`
(foreground acquisitions inside a window while passive work waited),
`linger_expired_with_passive_waiting` and `burst_forced_passive_turns`, so the
window can be checked against captured inter-step gaps in the field.

## Signing preflight join

The wallet's signing preflight (`GetWalletActionContext` -> `Market`) used to
issue its own `updates` round trip even when it had just queued behind a
refresh of the same market for the same account, or directly followed a
balance read (the Qt order path reads `getflowmeshbalance` first). That
preflight may now reuse ("join") the refresh instead. Only a `Market` call made
inside the wallet's `FlowMeshSigningPreflightScope` may join: explicit
`getflowmeshbalance` reads and `Data` always refresh, and a join never
re-stamps the refresh, so joins cannot chain. The preflight span field
`preflight` is `joined` or `refreshed`. Like the work scope, the preflight
scope follows synchronous dispatch on one thread; across a process boundary
the preflight simply refreshes.

A join is allowed only when all of these hold (`src/node/flowmesh_client_join.h`):

- the caller is a foreground signing preflight and names an account;
- the reused refresh completed at most 50 ms ago and its request started at
  most 1000 ms ago (local policy constants, injectable in the pure predicate);
- every local check a refresh runs before its network call still passes
  (account scope, B3 checkpoint, seat authority, canonical anchor), and
  `Project`'s B3 checkpoint check runs as before;
- the cached entry is at or after every microblock at which this process
  verified one of its own inclusions on the market, a volatile per-market mark
  that survives eviction of certified actions from the outbox;
- the account has no unresolved retained action on the market, and every
  certified one is known to be included at or before the cached entry (an
  action certified before a restart refuses the join).

Otherwise the preflight refreshes as before. A join therefore never produces an
older own account sequence than the refresh would, and it only removes a
request.

What changes: the preflight is no longer a liveness probe of the endpoint. If
the endpoint fails within 50 ms of the joined response, the order is signed,
retained and reported unknown (protected, resolvable by exact retry) instead of
failing before signing, and its sequence stays blocked until that is resolved.
Endpoint status gates (paused, halt) and a same-key action from another device
may be up to 50 ms staler than on the refresh path. For the Qt order path the
preflight's "fresh market readiness" is the balance read made just before it.

## Preservation

No outbox, sequence/high-water, exact-action retry, uncertainty, no-resubmit or
durable-publication logic is changed. Wallet/market/generation changes continue
to invalidate deferred intent. Shutdown still drains the owned worker before
releasing backend/wallet references. Ending UI work never cancels a market order.
No second network worker or parallel mutable backend is introduced.

## Reproduction

From a configured build of this source, build the following targets (replace
`build` with your own isolated build directory):

```sh
cmake --build build --target test_bitcoin test_b3_flowmeshworkspace-qt test_b3_flowmeshtrading-qt test_b3_flowmeshtradingpanel-qt b3coind -j 8
build/bin/test_bitcoin --run_test=flowmesh_client_work_tests,flowmesh_client_poll_tests,flowmesh_client_evidence_tests --report_level=detailed
QT_QPA_PLATFORM=minimal build/bin/test_b3_flowmeshworkspace-qt
QT_QPA_PLATFORM=minimal build/bin/test_b3_flowmeshtrading-qt
QT_QPA_PLATFORM=minimal build/bin/test_b3_flowmeshtradingpanel-qt
python3 test/functional/feature_flowmesh_client_reconnect.py --configfile=build/test/config.ini
```

The functional test creates disposable wallets and local TLS endpoints; it
checks read-only reconnect, busy behavior, trust rejection and reopening without
submitting an economic action. Do not point it at a holder or operator datadir.
The Qt tests are synthetic, offscreen interaction/lifecycle regressions, not an
attended screen qualification or a new matched-fill benchmark.

## Limits

An acquired backend method may still perform multiple HTTPS calls and bounded
endpoint failover before releasing ownership. It is not interrupted by this
patch. The existing HTTPS connection lifecycle and server/validator latency are
unchanged. There is no new 200 ms claim, WAN qualification, Windows runtime
qualification or deployment approval from these scheduling tests.

## Executed qualification

macOS arm64, Qt 6.11.1, Release build, generated/mock wallets only:

- Work-gate, existing poll-budget and client-evidence suites: 25 passed
  (10 new gate tests, 8 polling, 7 evidence).
- Qt workspace: 166 passed, zero failed, three opt-in image/capture exports
  skipped. Includes unchanged-data refresh, phased updates, foreground review,
  coalesced status, receipt attribution, readiness, wallet switching and teardown.
- Qt trading rules: 25 passed; panel lifecycle: 5 passed.
- Focused loopback reconnect fixture: passed, final two daemon child exits zero.
  Private CA and pin rejection, unavailable/busy endpoints, locked-wallet and
  same-wallet reopen protections remain tested; only market-read requests sent.

The before-fix status-ordering failure is retained. A new wallet-switch test
initially failed because its offline attachment bypassed `setWalletModel`'s
display reset. The test now uses the real detach path first; production ownership
checks were not relaxed. Separate read-only code review identified error masking
around yielding; the final tests cover ordinary auxiliary failures, a cached
effects-fallback yield and receipt-error attribution. This bounded review is not
an independent protocol/security audit.

The running wallet executable was not relinked, replaced or restarted. No VPS
configuration, live wallet, signed instruction or validator was changed.
