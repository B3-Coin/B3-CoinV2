# FlowMesh Linux native CI guardrails

Date: 2026-10-16. Profile: `native-guardrails-v4`.

This headless Linux profile checks selected native FlowMesh concurrency and
recovery regressions. Its source and runner controls are prepared; Linux native
and ThreadSanitizer execution remain pending. It is not a trading latency,
Qt, WAN, physical power-loss, full test-suite or release qualification.

## Two bounded Linux profiles

The [workflow](../../.github/workflows/flowmesh-native-guardrails.yml) runs on
`flowmeshV2-dev` pushes affecting its declared source paths, or manual dispatch
on that branch. It has `contents: read`, no stored checkout credentials and no
release, signing, deployment or production-node operation.

| Profile | Configuration | What it checks |
| --- | --- | --- |
| `lockorder` | Ubuntu 24.04, distro Clang 18, C++20, Debug | Bitcoin lock-wrapper ordering and compiled thread-safety annotations |
| `tsan` | Same platform, RelWithDebInfo, `SANITIZERS=thread` | Instrumented native-code accesses exercised by the selected tests |

Both build only `test_bitcoin`, with wallet support and without Qt, IPC, daemon,
CLI, benchmark or fuzz binaries. Compilation uses two jobs and preserves
warnings as errors. Each job is limited to 120 minutes; configuration to four
minutes, compilation to 70 minutes, all selected tests to 30 minutes and an
individual suite to ten minutes. Existing internal test deadlines are unchanged.
Failures are not retried until green.

The TSan job uses the existing Linux CI prerequisite
`vm.mmap_rnd_bits=28` on its disposable runner. A remaining sanitizer startup
failure fails the job; it does not select an unsanitized fallback.

## Exact test selection and evidence

The [strict runner](../../ci/check_flowmesh_native_guardrails.py) selects 27 exact
compiled Boost suites. They cover P2P queues, runtime and production storage,
service startup and policy, HTTPS and client wait/watch paths, request
scheduling, outcomes and readiness, local admission, health, timing, checkpoint
indexing and chainstate-manager lifecycle. The manifest names are authoritative;
this is not a wildcard selection of every FlowMesh test.

Required named cases include immutable read publication and retirement,
checkpoint advisory bounds and corruption refusal, all eight native mutation
fence cases, all eight accepted-original retention cases, the private P2P token
case and synchronous seat-key observation. Version 2 adds the native market-view
suite and thirteen named ordinary-read/freshness regressions: twelve watch-lane
cases and the stale tick-advertisement case. Version 3 additionally selects the
native agreement suite and requires all seven exact-publication runtime cases
plus both durable-local-callback cases. Version 4 additionally requires all
thirteen restricted-health overflow cases: the two original chain-lock and
saturation controls and eleven explicit reserve, budget and refusal controls.
Missing prerequisites fail discovery.
The workflow must contain their reviewed source revisions before qualification;
it cannot substitute a prototype or silently omit a missing case. Do not publish
this workflow ahead of its mutation-fence, retention and metadata prerequisite
commits.

Discovery reads the compiled Boost tree. Each exact suite then runs once with a
detailed XML report. Its executed leaf names and count must equal discovery,
with no selected skips, expected failures, warnings, failed assertions or missing
results. Unselected cases cannot execute secretly. This avoids CTest's existing
skip treatment for an empty Boost filter. Nonzero exits, sanitizer diagnostics,
timeouts, excessive diagnostic output and malformed reports all fail closed.

The runner checks the checkout path, configuration and native compilation flags,
and records source HEAD, compiler identity, executable SHA-256 and suppression
SHA-256. These are provenance observations, not a proof that arbitrary preexisting
binaries came from the checkout. Qualification requires the workflow's fresh
checkout and completed build, not a copied stale executable or a reused dirty
build. No build cache is used by this profile.

Every fixture uses a new private temporary directory, never an owner datadir.
Artifacts contain only the compact summary, discovery and XML results. Generated
wallets, TLS keys, signing journals, databases and debug-log directories are not
uploaded. Completed and runner-timed-out suite attempts retain their actual exit
and status. Hard runner cancellation can prevent final evidence; it never
qualifies as success.

## Local controls already executed

The initial runner candidate passed 30 lightweight rejection controls locally
and in separate review. Its extended retention, metadata and P2P manifest passes
31 controls locally. Version 2 passes 32 controls, including rejection when any
one of its thirteen new required cases is missing. Version 3 passes 33 controls,
including individual rejection of each of its nine added publication cases.
Version 4 passes **36 controls**, including per-case positive checks and
missing, disabled, unexecuted or skipped rejection for all thirteen health
cases. Root ran the shared suite with exit 0 (0.174 seconds); a separate
reviewer ran the private identical future files with exit 0 (0.176 seconds).
These exercise missing/disabled suites, missing regressions,
empty or inconsistent XML, selected skips, wrong build flags, timeout and
sanitizer failure, process-group termination and bounded output. They do not
execute native FlowMesh or a sanitizer.

A separate positive format control used a preserved ordinary macOS native
binary: real Boost discovery and the XML results for eleven timing cases passed
the parser. It verifies compatibility with that Boost report format, not Linux
execution, the complete 25-suite selection or TSan. No Linux or TSan success is
claimed here.

From the repository root, run the lightweight controls with:

```sh
python3 -B ci/test_flowmesh_native_guardrails.py -v
```

After the workflow's exact Linux configuration and fresh build, its native
command is:

```sh
python3 -B ci/check_flowmesh_native_guardrails.py \
  --profile lockorder --build-dir build-native \
  --output-dir /tmp/b3-native-lockorder-results-new
```

Use a nonexistent result directory. The TSan profile requires its own fresh
TSan build and `--profile tsan`, not the ordinary executable.

## Coverage limits and build prerequisites

`DEBUG_LOCKORDER` follows Bitcoin's instrumented lock wrappers. Existing plain
`std::mutex` paths, including market, queue, store and client locks, are not
automatically converted or covered by that checker. Correct wrapper ordering
also does not rule out a one-direction wait, scheduling starvation or slow I/O.

TSan uses ordinary system libraries, not the instrumented libc++ built by the
[broader upstream profile](../../ci/test/00_setup_env_native_tsan.sh). Its
[existing suppression file](../../test/sanitizer_suppressions/tsan) suppresses
`Chainstate::ConnectTip` deadlock reports and includes other declared library
suppressions. This profile adds none. Its clean result would cover exercised
native accesses under those exclusions, not prove every dependency, plain-mutex
order, schedule, signing obligation or durability boundary safe.

One existing compiler warning blocked a strict local syntax check:
`finality_pin_persist_tests.cpp`, in
`block_flush_refusal_does_not_publish_pin`, calls `Tip()->GetBlockPos()` without
holding `cs_main`, although `CBlockIndex::GetBlockPos` requires it. AppleClang 17
reported it during the preserved scratch build; a separate syntax-only check
with `-Werror=thread-safety` reproduced exit 1. Selecting other tests does not
avoid compiling that translation unit. The source-reviewed two-line test
correction copies the block position while holding `cs_main`, then resolves its
filename outside the lock. The same strict local syntax check after correction
exited 0. All seven finality-pin persistence cases, including this corrected
case, subsequently passed in the separate 30-case ordinary macOS campaign
(3,596 assertions across its three suites; actual exit 0). This profile does not
suppress the warning, weaken warnings as errors or change production finality.

Two existing range-loop copies in
[flowmesh_asset_metadata_tests.cpp](../../src/test/flowmesh_asset_metadata_tests.cpp),
for `bad_id` and `contents`, also failed a strict local syntax check with
`-Werror=range-loop-construct`: two errors, exit 1. The test-only correction binds
each initializer-list string by const reference instead of copying it. The
elements outlive each loop body, and all catalog inputs and assertions remain
unchanged. The same strict syntax check after correction exited 0. All four
asset-metadata cases subsequently passed in that 30-case ordinary macOS
campaign; no production metadata rule or warning policy changes.

The exact 25-suite manifest was also executed locally under the explicitly
separate `host-local-macos-ci-selection-v2` profile: 398 discovered and executed
cases passed, every child exited 0, with no selected skips. The ordinary Release
test executable SHA-256 was
`fbd7949f09fafade9b3544a1b2786ed4194593de59391633ff39bd2e06d0b676`.
That host runner reused discovery and XML checks, not Linux build/sanitizer
qualification. That historical run used version 1; it does not qualify version
2's client source or added regressions. Linux Clang 18, DEBUG_LOCKORDER and TSan
remain unexecuted.

The exact version 2 manifest was subsequently executed under the explicit local
`host-local-macos-ci-selection-v3` profile against the final client read-isolation
source: **428 cases across 26 suites**, every child exit 0, no selected skips.
Executable SHA-256:
`2f2c97c544d1d5d52d989f5c82243b9ed3fe3c7f8bcb5a50919536bd4266fb28`.
This includes all thirteen ordinary-read/freshness regressions and preserves the
earlier required cases. It is ordinary AppleClang 17 Release qualification,
not Linux lock-order or sanitizer execution. The source, failing controls and
remaining client boundaries are recorded in the
[client stage report](b3-flowmesh-client-read-isolation-20261016.md).

Version 3's complete local selection subsequently passed **460 exact cases
across 27 suites**, all child exits 0, no selected skips, using ordinary macOS
native executable `e1747eefa91f1836a38282218c68b1d146a8b393a974e5777c73003b34f5b4f6`.
This includes all 138 runtime and 25 agreement cases. The prior 398- and 428-case
campaigns remain separate historical results, not a combined population. Its
nine added mandatory cases depend on the
[exact-publication repair](b3-flowmesh-exact-publication-recovery-20261016.md);
do not publish this manifest without that implementation and its tests.

Version 4's exact local selection passed **473 cases in 27 suites**, all child
exits 0, no selected skips, using ordinary macOS native executable
`b2fa60f95d76fc5019362ab86fefbac5f966b511e8abc656a9ebb5f47d3000ec`.
This includes the [health-overflow implementation and all thirteen controls](b3-flowmesh-health-overflow-admission-20261016.md)
plus the separately qualified [aggregate-save stage](b3-flowmesh-snapshot-aggregate-save-20261016.md).
The native host profile is explicitly `host-local-macos-ci-selection-v4`;
it neither validates Linux build flags nor spoofs sanitizer execution. The
workflow remains unchanged: no push, remote CI run, Linux/DEBUG_LOCKORDER or
TSan success is claimed. Preserve the earlier 398/428/460-case campaigns as
their own historical freezes, not one combined result.
