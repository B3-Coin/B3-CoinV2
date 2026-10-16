# Qt passive catalog read isolation

Date: 2026-10-16. Status: bounded source review PASS and local Qt qualification
completed. This stage separates passive market discovery
from the approved-command owner. It changes no economic instruction, signing
gate, proof requirement, freshness deadline or automatic retry policy.

## Reproduced command dispatch failure

The actual local before control `passiveCatalogCannotHoldApprovedForegroundDispatch`
held the genuine offline Catalog dispatcher while starting a synthetically
approved command through the real Qt dispatch path. Its preflight count stayed
at zero instead of one. The run completed with two lifecycle passes, one intended
failure, no skipped cases, exit 1 and 3.733 seconds; cleanup completed.

The tested translation unit SHA-256 was
`c0759eae8dae7e7af6b282ed6a3fe87af149297854be39ab5a34bc33242cc9ae`.
The source basis is panel `de9ea9d0`, header `6d2185fd` and workspace tests
`4d6cc354`, with the separately reviewed default-off Catalog probe/control
patch applied. That exact test translation unit is retained privately before
the owner-specific adaptations. The original desired preflight assertion and
the full original control body remain unchanged.

This fixture swaps in an isolated read dispatcher and an offline mutation trap.
It exercises Qt scheduling, not production signing, durable signed bytes,
native certificate verification, successful HTTPS trading or a latency target.

## Bounded catalog owner

Passive Catalog has one separate QThread, result and monotone cancellation
token, plus at most one replaceable value-only successor scope. Repeated refresh
requests do not queue RPC bodies, economic actions or additional threads.
Selected Data and Watch remain mutually exclusive with passive Catalog; their
existing owners and cancellation rules remain. Foreground command dispatch
continues to use only `m_thread`, its result, unlock context and original
instruction.

The Catalog worker performs the same client-info, public saved-request and
market-list reads. It never performs a mutation or captures a wallet backend or
unlock owner. A single context-bound queued callback preserves the existing
early display of locally retained requests. That callback accesses panel state
only on the GUI thread. The worker holds the QObject address solely for the
thread-safe queued invocation; destruction joins it before that address can
become invalid.

The scope binds wallet/Node identity, wallet and Catalog generations, the
foreground-command observation, refresh epoch, selected market/base/vault/domain/
configuration/account, captured head and sequence, and asset-routing state.
GUI completion rechecks the whole scope. Old success and old failure are
discarded after a command, connection change, market/wallet switch or newer
selected observation. They cannot retire a command, reset its cancellation,
replace a newer account row, change its readiness/backoff or renew response/
certificate clocks. A superseded result does not fabricate a new retry; only
an already coalesced read request may start afterward.

Remote discovery retains only the exact previous account/checkpoint binding.
Discovery alone neither supplies an account proof nor refreshes the trading
account. A current Catalog failure keeps its original conservative gate and
operator metadata invalidation; it is not silently erased by a later selected
proof or reinterpreted as a successful yield.

## Existing read intents and fairness

Already-approved command dispatch bypasses Catalog without joining it. Opening
a Review and explicit saved-request Status checks retain their existing queued,
read-only intent workflow. Their private Catalog yield flag skips only
unstarted passive continuations; an in-flight RPC still drains. A real auxiliary
error retains precedence over that yield. No intent automatically approves,
signs or resends an instruction.

The existing eight-yield allowance and Catalog, Status, Effects, Receipt order
remain. At the exhausted Catalog fairness turn, its one selected Data
continuation uses the selected-read owner, not the command owner. The queued
Status intent waits for that bounded continuation to complete or fail.
Other auxiliary phases retain their original fairness implementation. No
allowance is reset early and no request or freshness deadline is extended.

Independent review marked the first private freeze changes required. Its
current Catalog-error path omitted the legacy selected read attempt; the
successor restores that attempt only when no command/review or yield owns the
continuation and the existing Watch/candles predicate permits it. The original
error and Catalog type remain attached, so that read cannot erase the error
or renew freshness. The existing next-phase and snapshot-count assertions are
retained.

Wallet/market switching cancels and fences the exact old read and drains it
asynchronously, preserving the already qualified nonblocking Watch-switch
behavior. Commands still join before backend teardown. Destruction, application
shutdown, Node lifetime end and scoped RPC-dispatcher restoration must join
every actor. Cancellation is not proof that any previously submitted instruction
did not execute.

## Test scope and preservation

The six identified Catalog/yield tests use explicit passive-owner observation
helpers. Those helpers select Catalog only while it exists and otherwise return
the unchanged legacy command/result/yield observation. Foreground command-owner
assertions remain direct. The phase-parameterized Review test uses the same
helper only to observe its passive owner. Sixty-four existing completed-refresh
waits now additionally await Catalog drainage; their original timeouts and
following assertions remain.

The original freshness, auxiliary-error precedence, receipt attribution,
signed-object identity, no-resubmit, no-write and unlock assertions are retained.
The original five selected-read dispatch rows, their late-command controls and
the nonblocking Watch-switch control are not rewritten. The pristine actual
before translation unit and a separate owner-adaptation patch are retained
for comparison.

Four additional controls check late Catalog success and failure after an
approved command has completed without changing the displayed head, one-owner/
one-scope bounds under repeated refreshes, and destruction while a real Catalog
read is outstanding. The late-result controls hold the one new coalesced Catalog
independently, so a subsequent valid account response cannot conceal the old
completion's effect. These are offline dispatcher controls with a mutation trap.

The first late-result fixture also held foreground discovery unintentionally.
The successor holds and counts passive reads separately: two passive Catalogs
and the one mandatory foreground discovery must all be observed. The foreground
gate remains unchanged. The destruction control withholds its reply until
it observes the Catalog cancellation flag set by destruction; its one-second
bounded release thread always drains and joins, including a failed assertion.
This closes the first fixture's possible release-before-destruction race.

The second source freeze also required a correction at the exhausted-yield
turn when Catalog fails and an explicit Status is queued. Draining Status first
claimed the command thread and skipped the original Data attempt. The third
candidate starts that one error-tagged Data read and binds its exact fairness
marker before draining Status. Completion or failure then releases only that
marker and resumes the existing Status intent. The ordinary yielded-error
guards, original Catalog error and prohibition on freshness renewal remain.

A focused test-first control holds the failed Catalog at the eight-yield limit,
queues Status without a yield, and requires Data to enter before Status. It
returns a full newer remote account response but requires the old GUI head,
response/certificate clocks and Catalog failure gate to remain. Finally it
requires one Data followed by one Status, the exact retained request hash and
no-resubmit marker, and no write or unlock. Its test body is identical for the
preserved second source and the corrected third source. Actual before and
after execution of this control were performed separately from source review.

## Actual qualification and retained failures

The original dispatch control failed on the old owner, as described above.
The focused error-ordering control also failed against the retained second
production freeze `e056a0f3`: Data never reached its barrier before queued
Status. Its actual child exited 1, with two lifecycle passes, one failure,
zero skips and 3.714 seconds. The original assertion and corrected-third
test body are identical. That BEFORE executable SHA256 is
`e2b7484fb65c643b84d84a5d2a88dc84c9576c447234f28463c2e63504097e7d`.
The two rejected private source candidates and their findings remain retained;
they are not represented as qualified successors.

The corrected third source hashes are panel
`fa70c3a7fffaed03d7278a9f6b49a5531a515544d514fedae6faa8be13c98f60`,
header `10dd7a6d405cf15557d4568f944607770455c9f0d2263e47adcc00b46b8ba15f`,
and workspace tests
`7ff8e054cef63c288766e1c2df511a74a2b1ed14488e6ea046a66df214c786a0`.
Its separate read-only review found no concrete remaining defect within this
bounded scope; the reviewer did not build or run native tests.

Root built the ordinary AppleClang 17 Release Qt workspace and wallet targets
successfully. The frozen test binary SHA256 is
`d7c6e18a238ce8acf5c2ca05e9adddea4cd78da8225e26c97d1a49568cc22c44`.
All six new Catalog controls passed: **eight Qt passes including lifecycle,
zero failures, zero skips, exit 0, 1.186 seconds**. The complete workspace
suite then passed **362 cases, zero failures and three unchanged opt-in visual
skips, exit 0, 46.055 seconds**. These were separate actual children, not summed
into one population. They used offscreen generated regtest setup and offline
dispatchers/mutation traps, not a live wallet or VPS.

The linked native client is the separate BUDGET recovery candidate `b026337e`;
its six evidence cases also passed. The complete version 7 native host selection
passed 489 cases in 28 suites, all actual child exits 0 and no selected skips.
No Linux/TSan, Windows, WAN, successful real
order under stalled native discovery, release or 200 ms qualification follows.

## Remaining boundaries

Native `B3FlowMeshTrading::ReadMarket` repeats `listflowmeshmarkets` during a real
approved-command preflight. Native catalog discovery still uses its current
client work gate. A separate native discovery-lane change could instead return
explicit busy while another Catalog flight is held. Therefore a Qt preflight
counter of one proves dispatch isolation only; this stage does not prove that
a real native order succeeds during held discovery. The foreground catalog/
proof gate is preserved, not skipped or replaced with stale cached authority.

Foreground Catalog/preflight, Connect, Status, Effects, Receipt, the exceptional
Effects reconciliation fallback and non-Catalog exhausted-yield turns retain
their legacy ownership. They can still delay another command. The backend's
existing per-request deadline, endpoint/failover bounds and cancellation-before-
next-call behavior remain; an entire endpoint walk is not guaranteed to drain
within one request's five-second limit. This stage is not a complete Qt queue
redesign, a zero-lock-wait authority lease, a 200 ms result or release approval.
