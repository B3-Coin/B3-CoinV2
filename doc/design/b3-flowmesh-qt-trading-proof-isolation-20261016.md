# Qt: trading proofs and operator settlement reads are separate

Date: 2026-10-16. Scope: the existing Qt trading controller; no protocol,
matching, fees, signature, timer or automatic-retry change.

## Defect and correction

A passive payout/connected-settlement discovery failure previously failed the
whole refresh, even when the same job subsequently obtained a full fresh,
locally verified proof for the exact selected trading account and market.
That could disable new order reviews because an unrelated operator read failed.

The existing narrow operator-Status exception now also covers the passive
Effects phase. It requires a full, remote, certified, certificate-verified,
account-state-verified response for the exact selected account, market, base,
domain and configuration, with a nonregressing/nonconflicting head and a fresh
captured response age. Omitted/UNCHANGED, stale, local-only, failed or unverified
responses do not qualify. No response becomes fresh merely because another
operation failed.

The original settlement error is retained in Activity. Checkpoint/payout
eligibility and cached payout choices are cleared; pending payouts remain
unknown, not proven absent. A separate uncertain instruction retains its exact
identity, no-resubmit protections and new-order gate. A modal review continues
to freeze the data policy it already used.

For example, a new verified proof may show 200 test units available for trading
while settlement discovery is unavailable. That permits the normal review
checks to use those proved trading funds; it does not authorize a payout,
acknowledge an uncertain order or claim that settlement has completed.

## Native qualification

The unchanged predecessor panel plus the final new regression produced eight
expected failing positive rows; all eight negative-proof controls passed
(10 passes including fixture setup/cleanup, eight failures). After correction,
the affected selection passed all 29 rows, covering full-proof separation,
fallback errors, freshness, foreground review priority and uncertain/review
guards. The complete workspace suite passed 336 tests, zero failures, with
three opt-in render/export cases skipped. No assertions or test discovery were
weakened.

The source received a separate read-only review before qualification; the
implementer did not count its own work as an independent audit. Tests used Qt
6.11.1 on macOS
with the minimal platform plugin, generated mock wallet/settings fixtures and
synthetic proofs. This is controller behavior testing, not a cryptographic or
live-network proof test. Existing unrelated unused-capture/link warnings remain
visible in the build evidence.

## Reproduce and limits

```sh
cmake -S . -B build -DBUILD_GUI=ON -DBUILD_GUI_TESTS=ON -DENABLE_IPC=OFF
cmake --build build --target test_b3_flowmeshworkspace-qt -j 8
QT_QPA_PLATFORM=minimal build/bin/test_b3_flowmeshworkspace-qt
```

No real wallet, server, VPS or existing regtest data is opened by these fixtures.
This does not fix the shared serial RPC wait, all HTTPS refresh failures, inverse
V1 market semantics or the 200 ms trading target. Windows packaging and a real
engine-off Qt trading campaign remain separate qualification gates.
