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
```

The functional tests need loopback network permission and generated disposable
directories; their inherited fixtures do not measure ordinary WAN propagation
or trading latency.

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
