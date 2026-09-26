# Regtest client: tip-change deposit and refresh repairs

These are native wallet/client and test-build repairs, not new FlowMesh
consensus, V2 activation, a performance claim, or a production deployment.

## Demonstrated defects and corrections

1. **Windows policy-probe link:** the standalone Qt policy test links core
   version/license code without defining its translation callback. The
   Windows build failed with undefined `G_TRANSLATION_FUN`; the GUI itself
   had linked. The test now supplies the null callback, like other standalone
   core test executables, and exercises `LicenseInfo()` to prevent dead-code
   elimination from hiding the dependency. Native macOS also reproduced the
   link failure with the callback absent. After correction: 119 policy tests
   passed. This is not evidence of a successful new Windows CI run.

2. **Unchanged-chart repaint:** runtime-only reconciliation observations
   triggered candle reconstruction and repaint although no plotted data
   changed. The chart retains every new snapshot but rebuilds/repaints only
   when its rendered fields change. The regression observed six unnecessary
   paints before and zero after; changes to depth, prices, provenance, units
   and stale state still repaint. The workspace suite passed 97 checks, with
   three explicitly opt-in fixture checks skipped. Input text/focus, selected
   receipt and table-cell identity survive six reconciliation cycles. Action
   safety gates remain enabled; this is not proof that every owner-visible
   flicker or the historical accessibility crash is fixed.

3. **Ordinary deposit construction race:** wallet preparation saved the exact
   B3 tip, performed remote readiness reads, constructed a deposit, then
   rejected it if any new block had arrived. An isolated five-node regression
   advances one actual B3 block during the HTTPS readiness response. The old
   executable refuses with RPC -26, `The chain tip or FN slot changed while
   the transaction was being built; retry`.

   Ordinary deposits into an already established market now undergo a
   current-tip dry-run of the **same signed transaction**, with activation and
   current-chain market checks. Normal mempool consensus and policy checks
   remain mandatory. The dry-run does not insert, commit or broadcast the
   transaction. Market bootstrap, FN-slot operations and other callers retain
   the strict existing snapshot check. No nTime, signature or deposit identity
   is rewritten. A real paused market still refuses; its actual pause reason
   is no longer misreported as necessarily insufficient seats.

4. **Remote pause-reason loss:** the HTTPS snapshot/updates status omitted the
   service's typed `chain_reconciling` observation, and the remote projection
   always left it false. A temporary reconciliation error could therefore
   appear as a generic market halt in engine-off Qt. Snapshot/updates now
   carry this optional boolean and the client validates and projects it.
   Older endpoints remain compatible (absent means unknown/false, with all
   existing paused/error restrictions intact). This is an unauthenticated
   runtime observation, not certified state, proof of readiness, or permission
   to trade through an active pause. A real hard halt still takes precedence.

## Executed local checks

- New `feature_flowmesh_deposit_race.py`: passed on the patched native daemon;
  real tip advance, preparation without wallet/mempool insertion, exact-byte
  publication, conflicting input spend after construction refused, actual
  pause reason retained, validator engine off. All five child exits were 0.
  Predetermined seed: `5482936382661989032`.
- Before-fix run with the corrected same-node RPC fixture failed at the
  expected exact-tip refusal. Earlier fixture-only failures (shared HTTP
  connection and denied loopback bind) were retained separately, not counted
  as proof of the wallet defect.
- Existing `feature_flowmesh_release.py`: passed, including bootstrap,
  below-activation reorg refusal, deposit credit/sweep, partial custody flow,
  cancellation, exact-fee matched trade, withdrawal, restart and its existing
  generated-data reindex check. No live datadir was reindexed.
- `asset_amount_rpc_tests,wallet_rpc_tests`: 17 cases passed.
- New `feature_flowmesh_reconciliation_status.py`: failed before (true status
  observed as false), passed after, with five clean child exits. Tests actual
  server serialization and remote projection through snapshot and unchanged
  updates: true/false/missing/malformed field, unchanged certified head and
  proof flags, no submitted action. Qt still distinguishes a true hard halt.
- Native Qt workspace/policy tests: counts above. Actual screen-level behavior
  and same-wallet test-network use must be reported separately from these tests.

## Reproduce

Use an ordinary development build with daemon, wallet, CLI, core tests and Qt
workspace tests enabled. `BUILD_FLOWMESH_REGTEST_CLIENT=ON` and
`BUILD_FLOWMESH_REGTEST_POLICY_TESTS=ON` enable the separate guarded tester and
policy targets. Do not run these commands against a holder wallet.

```sh
build/bin/test_bitcoin --run_test=asset_amount_rpc_tests,wallet_rpc_tests
build/bin/test_b3_flowmeshclosed-policy
QT_QPA_PLATFORM=offscreen build/bin/test_b3_flowmeshworkspace-qt
python3 -B test/functional/feature_flowmesh_deposit_race.py \
  --configfile=build/test/config.ini --randomseed=5482936382661989032
python3 -B test/functional/feature_flowmesh_release.py \
  --configfile=build/test/config.ini --randomseed=5482936382661989032
python3 -B test/functional/feature_flowmesh_reconciliation_status.py \
  --configfile=build/test/config.ini --randomseed=5482936382661989032
```

The functional tests need loopback network permission and generated disposable
directories; their inherited fixtures do not measure ordinary WAN propagation
or trading latency.

## Deposit draft and measured passive-refresh follow-up

The owner subsequently identified a specific symptom: Deposit was enabled,
then disabled. Commit `a5fdea3e2902c29e39cce6df3692d8049a3869fd`
separates opening a deposit draft from authorization to prepare or send it.
Opening retains the verified wallet/market binding, spending capability and
security/uncertainty gates, but permits a temporary pause or stale read.
Continue performs one fresh read before normal review; changed identity,
precision, unavailable service or failed readiness refuses without unlock,
preparation or submission. No automatic retry is introduced. Amounts survive
temporary refusal but are cleared when wallet, market or units change.
The regression failed before at the disabled Deposit button; the full Qt
workspace suite then passed 108 checks, with three opt-in skips.

A separate screenshot showed `Updates delayed · trading paused` despite
HTTPS reachability. A bounded 24-second memory trace of the actual generated
regtest Qt at `6d8fe5c1792354d45f963759fe66d6dc7c05479e` captured 122 events,
zero dropped, with synchronous BENCH logging disabled and no extra market
polls or economic requests. Two complete periodic refresh sequences took
approximately 4470 and 4014ms. The first contained sequential Markets
(1179ms), Market (1097ms), Data (1147ms), VaultOperations (1043ms), and a
cached ActionStatus (3ms). These are nonoverlapping calls, not added nested
spans. The GUI applied its selected snapshot only after the complete bundle,
while its unchanged staleness threshold is 3000ms.

Client-work lock waits in that capture rounded to 0.0ms. HTTPS spans included
connection/TLS and remote-response waiting; no claim distinguishes network
transit from server queuing/execution. This evidence establishes avoidable
read bundling, not a consensus bottleneck or every possible flicker cause.
The Deposit-draft correction alone does not repair that separate delay.

The follow-up splits passive support work into bounded catalog, status,
effects and saved-receipt phases. Each job runs at most one auxiliary RPC,
then reads the selected market snapshot. This is an RPC scheduling bound,
not a bound on HTTPS attempts inside proof recovery. Receipt checks get a
slot even if the catalog becomes overdue during a slow cycle. Explicit
status/retry and economic-action paths retain their existing checks.

Discovery preserves account/checkpoint information only for the same exact
market binding. Successful selected proof data determines readiness;
discovery alone does not. Effects recovery reuses its typed-reconciliation
snapshot if one was already requested, including a failed attempt, rather
than silently requesting a second snapshot. An auxiliary failure remains a
failed read and does not refresh stale data. Wallet, selection and endpoint
changes invalidate the previous scheduling context. The received snapshot's
monotonic age is retained through GUI delivery, rather than reset when queued
work finishes. The 3000ms stale threshold, 500ms UI timer, 5000ms catalog
cadence and action-preflight rules remain unchanged.

The new before-fix phase regression observed a balance read during the
catalog pass and failed its one-auxiliary bound. After correction the full
workspace suite passed **115 checks, zero failures, three opt-in skips**.
It covers phase order and receipt fairness, auxiliary failure without renewed
freshness, exactly one effects-fallback snapshot attempt, discovery versus
verified checkpoint state, delayed GUI delivery beyond 3000ms, obsolete
scheduling contexts, and the prior draft/receipt/shutdown regressions.
One intermediate fixture assertion saw the automatically started status
read finish before it inspected first discovery. That failure was retained;
the fixture now holds the status read, asserts the discovery boundary, then
releases it and separately asserts verified status. The production initial
selection refresh and rejection assertion were not weakened.

Actual same-wallet reopening and subsequent native refresh timings must be
reported separately from these synthetic Qt checks. No constant WAN latency
or absence of all visible flicker follows from the scheduling bound.

## Remaining reconciliation boundary

The service still closes new-action admission while it reconciles each B3 tip.
Existing exact-tip/anchor checks, checkpoint connections, signing obligations
and quorum rules are unchanged. Source inspection found repeated store
validation in that path, including work before detecting an obsolete callback.
Its contribution to wall-clock delay has not been measured here. This patch
does **not** qualify uninterrupted trading under arbitrarily fast B3 growth.
Do not remove the reconciliation gate or slow block production to hide it.

No mainnet operations, journal resets, duplicate signers, fee/rule changes,
public release, or completed full-V2 claim is part of this repair.
