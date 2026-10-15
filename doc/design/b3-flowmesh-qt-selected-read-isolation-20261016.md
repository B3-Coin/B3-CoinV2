# Qt selected market read isolation

Date: 2026-10-16. Qualification status: locally qualified within the offline
Qt dispatcher scope below; the frozen successor received separate source
review. Initial isolation, late-command and full-suite failures are retained.
This stage separates the ordinary selected
market Data/candles actor from the trading command actor and fences results
that predate a foreground command. It does not establish a 200 ms trading
result or isolate every auxiliary RPC.

## Cause and measured before control

The panel used one `m_thread` for both a passive selected-market read and an
approved command. `startJob` returned immediately while that thread existed.
A slow Data or candles response therefore prevented the command from even
reaching its readiness preflight, irrespective of the native client's separate
ordinary-read lane.

The actual local Qt before run completed with **five intended failures**, two
fixture lifecycle passes and no skipped cases. In each failing row the selected
read had entered and remained held, but `probe.preflights` was zero instead of
one. Three rows held Data; two held candles. Both eligible and expired pure
preflight-policy cases were exercised, including a changed selected account.
These are offline dispatcher/ownership tests: synthetic prior approval reaches
the real Qt command dispatch, but all mutation RPCs are trapped before signing,
outbox writes or network. They do not independently exercise native TLS proof
verification or qualify live-wallet latency.

Before test translation-unit SHA-256:
`6bb5f206f983f34fc7467283c35099f210ec13eacb7ee6ca772612c9aa078535`.
Before Qt executable SHA-256:
`a9aefd551665f609fb995044211418fa6d0eb62b2e5badf08f690cfd85000469`.
The five-case shared assertion helper is unchanged byte for byte, SHA-256
`ff50fefe72a6c0a1fc0fa4931fb08f286dcdb7add4dc2edae37eaab8ab5bd02b`.
Its desired availability assertions have not been weakened.

## Late results after command completion

Independent source review identified a second ordering defect in the initial
separate-reader candidate. A Data read could start against head H, then a
foreground command could finish without changing the displayed head, wallet
generation or read generation. Once the command actor cleared, the old read
still appeared current. A late failure set the stale/backoff gate; an old
successful Data response delayed by candles could publish after the command.

The targeted local Qt before run reproduced both defects: **seven passes,
two intended failures, zero skips, exit 2, 1.645 seconds**. The five original
command-progress rows and fixture initialization/cleanup passed. The new
failure case set `m_read_failed`; the new success case published its old head
`0a` over the GUI's retained head `04`. Both cases had already completed the
real Qt command dispatcher, while the original selected-read actor remained
held and the displayed head/generations stayed unchanged.

These two controls use the offline dispatcher, pure preflight policy and a
synthetic terminal inclusion receipt with the exact captured account and
sequence. They do not invoke a production signer, durable outbox or network,
and do not qualify native proof verification. They additionally require one
coalesced post-command account read, preserved protected receipt identity,
unchanged old observation clocks/bookkeeping, and exactly one mutation-trap
dispatch. The before failures occur before those later recovery assertions;
their successor assertions passed in the targeted and final full runs below.

Late-result before test translation-unit SHA-256:
`7a906ebee883a5e9e08e20f8995caa5f6b41d0f99ee1a18a2ef3758c8d7e1182`.
Before Qt executable SHA-256:
`5b2fccee23f17d1ce84b5b4f2efc8d7dba9650fb5fac332c731c70a318081b28`.
Both late-result test bodies are retained unchanged for the corrected candidate;
no original five-case or Watch-switch assertions changed.

## Full workspace failures and corrections

The first full local workspace run completed with **343 passes, 13 failures,
three explicit opt-in visual/export skips, exit 13, 57.391 seconds**. Its
panel/header/test sources were `58ab2d60`, `6d2185fd` and `7a906ebe`;
the executable SHA-256 was
`ced70515ed629c8966ff0219c74a3879b9aa82a8536c3b692f3603bd79b72fb5`.
That failed run remains qualification history, not a pass.

Splitting the auxiliary phase from Data removed the next RPC's existing
yield check. A foreground status/review click or market switch during the
last auxiliary RPC could therefore consume the phase instead of yielding.
The correction checks the same yield flag at the end of an auxiliary-only
phase. Existing handling still preserves a real auxiliary error over yield,
retains an attributed receipt error and leaves the yielded phase due. No
yield allowance or priority policy changes. After the existing eight yields
are exhausted, one original combined auxiliary plus Data job remains on the
legacy owner, so repeated explicit status clicks cannot skip that fair
maintenance completion. This exceptional turn can still delay a command.

The auxiliary completion also started an unnecessary selected snapshot even
when a live Watch already supplied that read. Its handoff now uses the
existing Watch/candles-due predicate. The full-run live-mode counterexample
had four snapshots where the existing assertion required zero; the assertion
and auxiliary/receipt cycle remain unchanged.

Three failing test cases contained four destructive wait expressions:
`QTRY_VERIFY_WITH_TIMEOUT(probe.entered.tryAcquire(), ...)`. Qt evaluates that
expression repeatedly, including its final assertion, consuming a single
correct entry signal before checking it again. Those four rendezvous now
wait for `available() > 0` and acquire exactly once. Their original timeouts,
held RPCs, subsequent counts, receipt clocks, editor checks and no-mutation
assertions are unchanged. This fixture correction is separate from the
production yield/live-mode defects; no extra signal or retry was added.

The original explicit-status, repeated-priority, automatic-receipt-error,
order-review and market-switch assertions are retained. Their complete
successor execution, including the bounded fairness turn, passed below.

## Ownership and bounds

[The panel](../../src/qt/b3flowmeshtradingpanel.cpp) retains one selected
Data/candles actor and one replaceable value-only successor scope. Repeated
refresh requests do not queue bodies, commands or threads. The command actor
retains its own `m_thread`, active result, busy state, unlock and original
instruction. Each selected read and Watch has a private cancellation token;
starting a command cannot revive an old cancelled read.

A scope binds the wallet model identity, node, wallet/read generations, market,
base asset, vault, domain, execution configuration, selected account, captured
certified head/sequence and a separate foreground-command observation. An
actual non-passive job advances that observation only after its existing start
guards pass. If a selected read is still outstanding, one value-only read scope
is retained under the new observation. Neither a body nor an economic action
is queued, and no actor is synchronously joined.

Completion rechecks the scope on the GUI thread. A conflicting or older
captured head is refused. A result predating the command is obsolete even
after that command's actor disappears; it cannot set read backoff, apply
balances, acknowledge uncertainty or renew clocks. Only an otherwise current
read within the same observation may follow the existing display-only path
while a review owns the panel. Candles never authorize trading. The fence
changes no freshness deadline, receipt identity, cancellation token or
uncertain-action rule.

Watch and selected reads exclude one another. A coalesced successor first
services any due existing auxiliary/receipt phase, preventing a sustained
stream of Data requests from starving the tracked original's status checks.
Existing auxiliary order, yield allowance, read backoff and request limits
remain. No economic instruction is automatically retried or re-signed.

## Explicit read only review handoff

A user clicking Order Review may bind its one read-only review intent to an
already outstanding, exact selected **Data-only** read. There must be no
command/result owner or uncertain instruction. This preserves the existing
single-response review workflow rather than issuing a duplicate snapshot.
The existing foreground confirmation path consumes the response with its
original receipt clock and checks the captured inputs, pins, account,
readiness and age. It does not approve the order for the user.

This exception does not promote candles, Watch, an auxiliary response, a late
or superseded read, or a completion while another command owns the panel.
Cancellation, wallet/market switch, explicit Connect or a new command invalidates
the handoff. A failed handoff consumes the review intent without an automatic
replacement request. No handoff acknowledges an uncertain submission.

For ordinary idle completion, only a full fresh verified proof for the same
captured uncertain instruction/account may set its existing `refreshed`
marker. The uncertain instruction itself remains retained until the user's
existing explicit acknowledgment. Busy, display-only, omitted-account,
superseded or different-instruction reads cannot set that marker.

## Lifecycle and compatibility

Read actors capture value requests/results and the longer-lived `interfaces::Node`;
they never dereference a wallet model, wallet backend, unlock context or panel
from their worker thread. Wallet RPC dispatch owns the wallet it resolves for
the call. Ordinary wallet/market switching cancels and fences old read scopes
without joining on the GUI thread; the one old actor drains before its
coalesced successor may start. Commands still join before wallet/backend
teardown. Panel destruction and application shutdown join all read actors
before their panel/node lifetime ends.

The existing nonblocking Watch wallet-switch/closing control is byte-identical
to the before fixture, SHA-256
`1bb3118dcee838839b282b93f0171390d770032a7c99cfc943b4a9ced40f9447`.
Old completed-refresh waits now include the selected actor, so they cannot
mistake a drained auxiliary thread for a completed account refresh. The old
single-read review test retains its RPC-count, approval, freshness, priority
and no-mutation assertions; only owner-specific observations distinguish the
new selected actor. The delayed-GUI clock fixture now uses a proper successor
head instead of two fabricated conflicting hashes at one sequence.

Status/effects failure can be superseded only by the existing full fresh
verified remote selected-account evidence. Operator metadata is invalidated,
the original error remains visible, and missing payouts remain unknown.
The exceptional Effects reconciliation fallback remains on its **legacy
auxiliary owner** and reuses its original single Data response/receipt clock;
its existing error/yield behavior is preserved rather than issuing a second
snapshot. The existing exhausted-yield fairness turn also retains one combined
auxiliary plus Data completion on that owner. Neither exception is described
as fully isolated command scheduling.

## Successor qualification and remaining gaps

The corrected source is frozen at
panel SHA-256
`de9ea9d027908ce91dab486ae1bea2eb61b6c5b6c6b6cae8cd0e2bc209fe21ad`
and header SHA-256
`6d2185fd8939636308ff74b02785b0588ef1050e306fb1be616dfc2156823cf5`.
The test translation-unit SHA-256 is
`4d6cc354944319c265b33c547c31ca0004410bb08dee78c4ddd203f4c92cdf9f`;
its only changes after the full failed run are the four wait expressions
described above. The command-observation delta matches the independently reviewed private
patch `f025a7b2d8086f4935cc3156a23c9d9fc4c3e3fe0e02a00b3a675811b7dc1bb0`,
with only explicit empty auxiliary aggregate initializers added to preserve
their defaults and remove the new missing-initializer warning. The subsequent
source delta contains only the auxiliary yield check, live-Watch handoff
guard and exhausted-yield compatibility turn described above.

Added controls cover one in-flight
owner/one coalesced successor, private cancellation, wallet-switch lifetime,
destructor joining, tracked-receipt fairness, six uncertainty conditions and
review handoff supersession/cancellation/actual command ownership, plus the two
late-after-command counterexamples above. The original five failing-before
controls, both unchanged late-result tests and full existing workspace suite
passed in the final full run.

Actual local execution, on the same macOS/Qt 6.11.1 offscreen fixture:

- The command-observation successor's targeted run exited 0: 9 passes,
  0 failures and 0 skips in 1.740 seconds. Its first full run then exited
  13: 343 passes, 13 failures and the three existing explicit opt-in visual
  skips in 57.391 seconds. Neither that full failure nor the original five
  and two counterexamples was discarded or counted as qualification.
- After the separately reviewed A/B/C corrections and four non-consuming
  semaphore-barrier fixes, the full workspace run exited 0: **356 passes,
  0 failures and 3 explicit opt-in visual skips in 44.579 seconds**. This
  includes the original five progress rows, both late-result controls,
  receipt fairness, original Watch-switch controls and all formerly failed
  scheduling cases. No failing case was disabled or converted to a skip.
- Final workspace executable SHA-256:
  `8dac086e4a646dd13177a23c8fb3818cb5a50c0912eb59a5ee030cf8d260adee`.
  Final Qt wallet executable SHA-256:
  `4a1c9bb6be686f2a6705bbde072638890f7b86ca4251c6c5d1135aee38aadaf7`.
  The latter was compiled, not used with a live wallet.

The author did not run these tests; the root executed them. The independent
reviewer checked the frozen source/deltas, not native execution. Builds
completed successfully; an initial command naming a nonexistent build target
failed before compilation and was corrected without changing source. The
pre-existing unused lambda-capture and duplicate-library warnings remain.

Catalog, operator status/effects, generic saved-request discovery and explicit
status calls still use the command owner. If one of those auxiliary RPCs has
already started, it can still delay a command. The exceptional Effects
fallback and exhausted-yield fairness turn above are also not isolated.
Native cold/expired signing preflights
may explicitly refuse busy; a selected-read worker does not grant cached
signing authority or extend the existing native join windows.

The existing five-second HTTPS deadline is per request, not a five-second
bound on actor draining, a complete failover sequence or native lock waits.
This stage changes no consensus, certificate, fee, durability, request-original
or account-scope policy. These offline tests do not qualify end-to-end native
signing/TLS, Windows, live-node use, a tester release or a latency claim.
