# FlowMesh client verified return durability

Date: 2026-10-16. One generated regtest campaign reproduced an unsaved
verification label escaping through a delayed submission response. The repaired
client passes the same frozen fixture: failed persistence withdraws verified
labels, successful persistence retains the original instruction across a real
process kill and reopening, and an explicit retry does not resubmit it.

This is client outbox durability qualification within the tested failure model,
not production consensus, a release gate or a latency measurement.

## Cause and correction

An ordinary snapshot can verify that a retained action is included while its
original waited submission reply is still in flight. Verification sets the
in-memory `previously_certified` marker. If the subsequent synchronous outbox
write fails, the old delayed-reply path could return that in-memory verified
receipt without first persisting the marker. The displayed verification and the
restart protection then disagreed.

The minimal correction in
[RemoteBackend](../../src/node/flowmesh_client.cpp) is conditional
`SaveVerifiedRestartState()` before an already-verified return. It calls the
existing `SaveRestartState()`, which writes synchronously only when
`RestartDigest()` differs from the last successful write. The digest includes
the original bytes' identity, market/domain/configuration/account binding,
submission uncertainty, head high-water marks and no-resubmit protections.
Receipt text and a pure outcome-label upgrade are not restart inputs.

The snapshot path also records each affected pending instruction before
verification. If its inclusion-marker write fails, it withdraws the affected
verified labels together. Other relevant status and delayed-reply returns use
the same conditional durability guard. Failure preserves exact original bytes
and the in-memory `previously_certified` obligation; it does not clear the
obligation, sign a replacement or authorize resubmission. Fresh successful
persistence must precede exposing verified inclusion again.

In the exercised sequence, the action itself had a valid quorum certificate.
The defect was the client's missing durable no-resubmit marker, not a forged
certificate, a duplicate execution or invalid accounting.

## Frozen fixture and executable identities

The fixture is
[feature_flowmesh_client_durability.py](../../test/functional/feature_flowmesh_client_durability.py),
cleanup successor r5, SHA-256
`dd3b00be465605de08ee25de91d9712e8ffae5037817167393faab8213d570c2`.
Its declared bootstrap profile is `generated-legacy-minimal-genesis-v1`.
It reuses only the existing generated PoS transition, market genesis and
checkpoint setup, with no preliminary-agreement flag. It does not run or weaken
the separate full release/reindex assertions. There are four native validators,
two HTTPS-serving operators and one validator-off client; all keys, wallets,
funds and TLS material are newly generated regtest fixtures.

Both arms use seed `24092026` and the same ordinary operator daemon:
`77da7f9bdec6a28c50b63ed6fb735c28d5a53d8102d73c9cfbb6d9a1ed252d87`.
Only client4 uses a separately compiled fault-hook daemon.

| Client executable | SHA-256 | Fixture exit | Delayed response during save failure |
| --- | --- | ---: | --- |
| Instrumented control before the repair | `7ab7a76e270c4c2c55a5ed6c9d5d5a68307570e17265a536fe8a81db70873b04` | 1 | Incorrectly verified inclusion and outcome |
| Instrumented repaired client | `a0725e0cf743858ea165efd828d21ab799f68cfdf9013d726af548c6a894ffbc` | 0 | Unknown, with both verified labels false |

Source freezes identify the tested client and hook independently of platform
binary reproducibility:

- Repaired `flowmesh_client.cpp`:
  `d0e1f63082697c8c3154d5cf1401ad27897b0bca0a4ad1bea152a2e35374e281`.
- Actual assembled control `flowmesh_client.cpp` compiled into the before
  executable:
  `344bc379b42dd2d9c34d54dfbd54dc67cf32f5ac653ffa6b78ad39e2a6ed8adc`.
- [Compile-time hook](../../src/test/flowmesh_client_crash_hook.h):
  `e778d4195be6fe26e15bb8b9bf45fa17e89bebf3950c6e73a403ab0ab06e46db`.
- [Generated crash fixture dependency](../../test/functional/feature_flowmesh_client_crash.py):
  `39aa84dd9daf1320cd1308578c808ce0d40cbde27da20119814575dd6a661c89`.

The control combines the clean in-tree API prefix with the frozen
instrumentation-only `RemoteBackend` suffix. The archived whole-file hash
`f7206d6d264ae9d9b916c720ddfc43f3d0ee275b0642ec72dc7fe5f8c2edca88`
identifies that suffix's donor, not the complete compiled source: its
quarantined upper API candidate was excluded. Reconstructing the clean prefix
plus that exact suffix reproduces the compiled control source hash above.

## Actual failure and recovery sequence

1. Submit one original signed order through the normal client waited lane,
   requesting 2,000 ms. Hold its returned HTTPS reply before it reacquires the
   client work lock. Require the operator's certified receipt and durable
   payload through the public read-only API, not another wallet's private RPC.
2. Inject a failure immediately before a synchronous journal write containing
   that order's new `previously_certified=true` marker. An ordinary client
   snapshot performs real certificate verification and receives the injected
   save error. The last successful journal still has the order's marker false.
3. Release the exact original delayed reply while the failure remains active.
   The control incorrectly returns `certificate_verified=true` and
   `outcome_verified=true`; the assertion fails. The repaired client returns
   unknown with both labels false and retains its binding in-memory obligation.
4. Remove the prewrite fault. Read-only status recovery persists the marker
   before returning verified inclusion. Exact signed bytes remain identical;
   the original order has exactly one HTTPS submission. Its account sequence
   advances once, from 0 to 1, with one resting-order reservation effect.
5. Kill only the checked generated client process with SIGKILL, after the
   successful marker save. Reopen the same test store and compare the complete
   retained journal, exact bytes and binding flags. Receipt labels deliberately
   restore as unknown rather than borrowing old authority.
6. Simulate endpoint receipt expiry. Invoke the explicit exact-action retry RPC;
   the restored no-resubmit marker blocks transmission, despite the unknown
   current receipt. No replacement or second submission occurs.
7. First return fresh inclusion without outcome evidence, then restore valid
   outcome evidence. The client upgrades the verified outcome with zero extra
   synchronous journal writes because restart-relevant state is unchanged.

The r5 control failed at the intended verified-label assertion, not setup. Its
original order ActionId was
`842e92793c10f2993347ff2d4785e7c1bc8e793c744f06f7078a36b712d603eb`.
The repaired fresh fixture used
`10efd92a357ab65e4f1840b244fd5a9d553d3930e4dfa9a8f6c9c695d6b2117a`.
These are separate generated campaigns, not the same request replayed across
test stores. Each also has a distinct setup deposit; that deposit is not a
second order submission.

### Child exits and retained evidence

The fixture records exact owned `Popen` objects, not processes discovered by
name, port or a PID search. Cleanup runs on failure as well as success. Forced
cleanup would fail qualification; only the existing checked intentional crash
has an expected nonzero exit.

| r5 campaign | Tracked child instances | Actual exits | Cleanup errors | Harness result |
| --- | ---: | --- | ---: | --- |
| Control before repair | 6 | All 6 exited 0 | 0 | Expected assertion failure, exit 1 |
| Repaired client | 7 | 6 exited 0; intentional client crash exited -9 | 0 | Pass, exit 0 |

The extra instances are the ordinary bootstrap client, its fault-hook
replacement and, in the passing arm, its reopened replacement. There are still
only five node roles. The original submission thread stopped in both arms.

The ordinary, non-hook macOS build also passed 133 selected client, admission
and HTTPS tests with 30,134 assertions, exit 0. Its native test executable
SHA-256 was `be37384508008d2fb04eabe42872ad520e0649dca144634bb3062dced1f5fdc1`;
ordinary daemon SHA-256 was
`0c1bbd45b79a3f21c52ecdf7a4fbf6780270edeb2f35a25464cada9478bc3c66`.
Positive/negative executable-string controls confirmed the guarded injected
failure path was present in the separate fault build and absent from that
ordinary daemon. This is selected client qualification, not a pass of the
entire integrated redesign: a separate retention draft failed four runtime
cases and remains under correction.

Fresh test output includes `client-verified-return-durability/RESULTS.json`,
`D-before-lane-release.json`, `D-reopened.json` and
`client-durability-child-exits.json`. Preserve complete failed runs privately.
This report publishes only sanitized identities and results, not generated
wallets, signing material, TLS private keys, RPC credentials or private logs.

## Reproduce with separate ordinary and fault builds

Install the normal [build prerequisites](../build-unix.md), or the
[macOS prerequisites](../build-osx.md), and run from a clean review checkout.
The two build directories must be distinct. The ordinary build must not define
`FLOWMESH_CLIENT_CRASH_TEST_HOOKS`; never ship or package the fault build.
On macOS, add the local dependency prefix required by the normal build guide.

```sh
normal_build="$PWD/build/flowmesh-client-normal"
fault_build="$PWD/build/flowmesh-client-fault"

cmake -S . -B "$normal_build" -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS= \
  -DBUILD_DAEMON=ON -DBUILD_CLI=ON -DENABLE_WALLET=ON \
  -DBUILD_GUI=OFF -DBUILD_TESTS=ON -DENABLE_IPC=OFF \
  -DBUILD_FUZZ_BINARY=OFF -DBUILD_BENCH=OFF
cmake --build "$normal_build" --target b3coind b3coin-cli -j4

cmake -S . -B "$fault_build" -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS=-DFLOWMESH_CLIENT_CRASH_TEST_HOOKS \
  -DBUILD_DAEMON=ON -DBUILD_CLI=ON -DENABLE_WALLET=ON \
  -DBUILD_GUI=OFF -DBUILD_TESTS=ON -DENABLE_IPC=OFF \
  -DBUILD_FUZZ_BINARY=OFF -DBUILD_BENCH=OFF
cmake --build "$fault_build" --target b3coind -j4

fixture_parent="$(mktemp -d)"
BITCOIND="$normal_build/bin/b3coind" \
BITCOINCLI="$normal_build/bin/b3coin-cli" \
python3 -B test/functional/feature_flowmesh_client_durability.py \
  --configfile="$normal_build/test/config.ini" \
  --crash-client-binary="$fault_build/bin/b3coind" \
  --tmpdir="$fixture_parent/run" --nocleanup \
  --randomseed=24092026 --portseed=92401
```

The fixture directory must not exist before launch. Run predetermined fresh
attempts sequentially and retain failures; do not loop until green. These
commands exercise the repaired checkout. Replaying the historical failing arm
uses the explicit reverse-regression control below, not the repaired source
relabeled as the control.

The fault build requires both a generated regtest marker and matching exact
path/token environment guards. An ordinary daemon contains no fault-hook
environment processing, marker reads, delayed-lane barriers or injected-save
failure path.

### Historical failing control in a disposable copy

The small [test-only reverse-regression patch](../../test/functional/data/flowmesh_client_verified_return_before.patch)
has SHA-256
`b31a194c655a726110b2ea6bf69eb28f5870e6fc4c90eadb2ea4ddfb87b38da2`.
It changes only the lower client durability repair, preserving the clean upper
API and guarded fault instrumentation. Applying its six hunks to the exact
repaired source `d0e1f630...` reproduces control source `344bc379...` byte for
byte. It does not require the private donor file.

Apply it only in a new disposable control copy, never the ongoing worktree,
live node, ordinary deployment or release source. From the clean repaired
review revision containing these files:

```sh
control_parent="$(mktemp -d)"
mkdir "$control_parent/source"
git archive HEAD | tar -x -C "$control_parent/source"
cd "$control_parent/source"
python3 -c 'import hashlib,pathlib; assert hashlib.sha256(pathlib.Path("src/node/flowmesh_client.cpp").read_bytes()).hexdigest() == "d0e1f63082697c8c3154d5cf1401ad27897b0bca0a4ad1bea152a2e35374e281"'
git apply --check test/functional/data/flowmesh_client_verified_return_before.patch
git apply test/functional/data/flowmesh_client_verified_return_before.patch
python3 -c 'import hashlib,pathlib; assert hashlib.sha256(pathlib.Path("src/node/flowmesh_client.cpp").read_bytes()).hexdigest() == "344bc379b42dd2d9c34d54dfbd54dc67cf32f5ac653ffa6b78ad39e2a6ed8adc"'
```

Run the separate-build and frozen-fixture commands above from that disposable
directory to reproduce the intended control assertion failure. The repaired
qualification never applies this patch. The recorded executable hashes identify
the historical runs; a new platform or surrounding source revision need not
produce identical binary bytes.

## Earlier setup failures remain separate

Before the meaningful durability runs, three distinct fixture/observation issues
were preserved and corrected or isolated:

- The functional-test metaclass required an explicit concrete `run_test`
  override; the first import failed before any durability experiment.
- A later fixture incorrectly queried an operator wallet for a client-owned
  action. The wallet correctly returned RPC -4. The precondition now uses the
  public operator action API; client proof verification still occurs in the
  actual snapshot path under test.
- The inherited full release/reindex campaign observed stale immutable
  `observer_only` metadata immediately after validator arming. That publication
  race is a separate advisory-metadata repair. The minimal bootstrap neither
  changes nor weakens the old release assertion.

Those failures are not durability counterexamples or passing qualification.
The r4 minimal fixture subsequently reproduced the actual unsaved verified
return, and its repaired arm passed. r5 adds explicit child-exit accounting;
its workload, fault and assertion bodies are unchanged from r4.

## Qualification limits

- The injected write failure occurs before the database write. Unknown or
  partially completed writes, corruption, disk loss and machine power loss
  are not qualified. The real SIGKILL occurs only after successful persistence.
- Restored labels intentionally require fresh certificate/authority evidence.
  The persisted no-resubmit marker is protection against retransmission, not
  permission to treat an old receipt as current verified state.
- A coherently restored old signing/outbox backup cannot be detected without
  an external trusted freshness commitment. This repair does not solve rollback.
- The campaign checks one predetermined order/failure/recovery sequence. It is
  not exhaustive concurrency testing, Qt or Windows qualification, production
  BFT/PoS V2, bridge verification, futures risk, live migration, WAN performance,
  a 200 ms result or full release/reindex qualification.
