# Selected-market final order preflight

Date: 2026-10-16. Local C++20/Qt stage; no publication or live operation.
Basis: `885ba6adcde2b25b0d03833bc2e2ac6876250aae`.

## Cause and deliberately narrow policy change

The [Qt helper](../../src/qt/b3flowmeshtrading.cpp), `DispatchApproved`, used
`ReadMarket` for its final preflight. That rediscovered and validated the whole
Catalog before reading the selected account. Separating passive Catalog's
transport and thread did not remove this final submission dependency: an
unrelated busy Catalog could still veto an otherwise verifiable selected market.

`ReadApprovedMarket` now performs exactly one selected `getflowmeshbalance`
read for an already-reviewed **remote market with an existing account**, only
for Order, Cancel and Withdraw. It parses the ordinary native verified result,
explicitly binds `remote` to the reviewed value, then retains `CheckSameMarket`
identity, base asset, vault, domain, configuration, account, sequence and readiness
checks. `Parameters` still checks current funds, amounts, destination and the
unchanged trading semantics. Approval and cancellation are checked before the
read and again before the one mutation. Exceptions have no fallback or retry.

This is an explicit **client availability-policy change**, not byte-for-byte
behavior equivalence: membership in a newly fetched unrelated Catalog rowset
is no longer an additional final veto for these eligible reviewed actions.
It does not make an unverified or stale selected proof acceptable. Native
`Market` independently checks pinned context, certificate, canonical anchor,
validator eligibility, verified account state and retained signing obligations.
The established market is resolved at the certificate anchor, not authorized by
advertised Catalog membership. The new explicit reviewed-remote binding also
closes a gap left by comparing only fresh discovery and fresh account values.

General discovery/opening review, local engine actions, accountless/first
deposits, Deposit/Admit and Checkpoint/Vault remain on their existing paths.
Operator checkpoint comparison, unlock, native signing preflight, exact saved
originals, durable return and no-resubmit protection are unchanged. No wire,
consensus, fee, quorum or storage rule changes.

## Failing-before and passing-after evidence

Five additive pure-helper tests cover seven order/cancel/withdraw parameter
variants; 24 changed selected gates across three operations; approval and both
cancellation points; unavailable proof without fallback; one unknown mutation
outcome without retry; excluded paths; insufficient current funds; and the
unchanged operator checkpoint check. These use offline RPC parser inputs, not
real certificates or a live wallet.

With the same new test body and old production source `a29a24dd`, the exact
`remoteApprovedDispatchUsesOnlyExactSelectedProof` leaf exited **1**: two
lifecycle passes, one failure, zero skips, Qt-reported 1 ms. It failed at the
unexpected refusal from Catalog busy; subsequent assertions were not executed.
After the repair, all five new leaves passed: seven passes including lifecycle,
zero failures/skips, 3 ms, child exit 0. The old local cancellation test remained
byte-identical. Two existing workspace Catalog success/error cases received
only their shared scheduling-premise adaptation: exactly two passive Catalog
calls, zero foreground Catalog calls and exactly one selected preflight. Their
head/freshness, cancellation, mutation, unlock, receipt and no-resubmit checks
remain intact.

Two separate native TLS controls use a generated indexed B3 fixture and genuine
BLS-certified account state while one Catalog TLS exchange is held:

- `selected_certified_account_proof_remains_available_while_catalog_tls_is_held`
  verifies the exact selected account, certificate, pins, sequence and balances.
- `selected_corrupted_state_is_refused_while_catalog_tls_is_held` corrupts the
  serialized state bytes and requires refusal of verified/running status.

Both actual native children exited 0. They were already expected to pass
before the Qt helper change; they are not represented as failing-before Qt
integration. Each performs one Catalog and one account snapshot, without a
Submit or ActionStatus call. Client high-water persistence may legitimately
occur; no no-storage-write claim is made. The fixture releases held TLS before
destroying asynchronous futures, including failure unwind.

## Final local qualification

The final five-target build completed with actual exit 0. Against its frozen
outputs, executed once:

| Selection | Actual result |
| --- | --- |
| Complete trading helper | 82 passes, 0 failures/skips; child 0; Qt 24 ms |
| Complete workspace | 362 passes, 0 failures, 3 unchanged opt-in visual skips; child 0; Qt 50.597 s |
| Exact native v9 selection | 497 cases / 28 suites; every child 0, no selected skips; 225.172 s |

The native host runner is ordinary AppleClang 17 Release/macOS 26.5.2 on an
Apple M4 Max, 16 cores/128 GiB. It imported the v9 manifest and enforced actual
discovery/execution equality. It is not Linux/DEBUG_LOCKORDER/TSan qualification.
Qt and native functional tests overlapped; their elapsed times are not a
controlled latency comparison. The unchanged seven isolated model suites had
separately passed 446 cases; they were not rerun or added to the native count.

Frozen source SHA256:

- Qt helper: `b14dd62f3592343cf3bbd18d4d10cf7a47b4d503a235da85db779cef8425af97`.
- Helper tests: `e33778b3fc7335cdba8caaf881a204907a414bba98283a3ee1725d0638e53ea2`.
- Workspace tests: `751c9887c50e7eec9c6d2e6dd6cfa1b784fac17ed8ff648e77096062830a27e8`.
- Native HTTPS tests: `968badb7073bdd959b824fb29008da5fb3feaf95b7ee3e31fd691c28d53ed609`.

Preserved executable SHA256:

- Before helper: `9ae11659067ba426e8825a268f607ca0c4401593df75a5edc697daf9e3724f84`.
- After helper: `0590fbe96c619f599e0581c230dad34273842ab00605d4fab8d1d7f6af2e25f4`.
- After workspace: `cb96d715c1abf75093f6416b45f67ea86721e3b4391c65cd7dc406871fd7ea09`.
- Native: `8386b179bb4742483f67ba3c6f9fdff1832490a2938a16ce714b61015c7aff19`.
- Qt wallet: `3ee7047264ade1805e718b13f0a59c3751b41cfc2e62f873238f759d134d8edb`.

After an ordinary matching build, reproduce the offline helper/workspace tests
with the two binaries `bin/test_b3_flowmeshtrading-qt` and
`bin/test_b3_flowmeshworkspace-qt` under `QT_QPA_PLATFORM=offscreen`; leave
visual opt-in settings disabled. Native selected cases are in
`flowmesh_client_watch_lane_tests` in `bin/test_bitcoin`. The
[strict Linux runner instructions](b3-flowmesh-native-linux-guardrails-20261016.md)
are a separate profile and must not be applied to a macOS binary as Linux proof.

## Review and remaining boundaries

A separate non-author source review accepted the exact test-first candidate,
production helper and narrow workspace adaptation. The reviewer inspected
native selected-market authority and unwind ordering; it did not execute these
native/Qt tests. Root performed the actual builds and runs above. CI runner
controls were independently reviewed and executed separately. No tests are
described as an independent security audit or deployment approval.

This stage does **not** demonstrate a real Qt-signed trade during held Catalog,
200 ms latency, arbitrary-load availability, Windows/WAN behavior or physical
power-loss recovery. Native chain-lock dependencies and the inherited
authority-during-durable-write window remain. Full native V2 asset-to-asset
accounting, futures risk, BFT/PoS V2 and bridge transition remain separate
unratified or unwired work. The
[bounded outcome diagnostic](b3-flowmesh-outcome-slice-20261016.md) is a different
experiment; its measured times are not attributed to this unbenchmarked helper.

Generated identities, TLS keys, journals, raw traces and disposable databases
remain private. Existing orders/data and unrelated source changes were preserved.
No push, release, live wallet, VPS, mainnet, bridge or activation operation.
