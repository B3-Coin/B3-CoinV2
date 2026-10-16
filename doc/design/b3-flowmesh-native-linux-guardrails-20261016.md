# FlowMesh Linux native CI guardrails

Date: 2026-10-16. Profile: `native-guardrails-v12`.

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

The [strict runner](../../ci/check_flowmesh_native_guardrails.py) selects 29 exact
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
Version 5 additionally requires all eight foreground read-admission cases:
three pure-gate cases for eight-waiter FIFO/capacity, the original absolute
deadline under repeated wakes and exception-safe ownership; and five native
watch-lane cases for queued signing/explicit reads, reconnect fencing, a new
post-wake original-stamp join, queue time in the original freshness age and
immediate Watch refusal plus failed-read recovery. The two earlier named
original-stamp and explicit Connect/Reconnect tests remain mandatory. Their
authorized foreground-wait scheduling adaptation does not replace the age,
no-extra-body, reset-fencing or no-submit assertions.
Version 6 additionally requires two diagnostic admission-trace cases in the
watch-lane suite: a genuinely held foreground read plus an admitted failure,
and explicit default-off and BENCH-entry guards. The positive capture keeps
BENCH disabled through caller, client and service drainage; the separate
BENCH-enabled refusal control keeps that setting unchanged through its drain
and requires no new admission span. Tracing stays opt-in and off by default.
These cases check capture boundaries and attribution, not production timing
or trading latency. All version 5 admission, header, suite and bound requirements
remain unchanged.
Version 7 adds only `flowmesh_client_outcome_evidence_tests`, compiled from the
already selected HTTPS translation unit. All six cases are mandatory: truthful
capability parsing, advertising-endpoint outcome verification, inclusion
preservation on evidence failure, permanent `EXPIRED` one-shot behavior,
transient standalone `BUDGET` recovery without resubmission, and unknown-field
withdrawal of both capabilities. The new recovery case is
`transient_outcome_evidence_budget_recovers_without_resubmitting_included_action`.
Independent controls freeze every earlier suite name and source mapping, so
changing the count from 27 to 28 cannot conceal an omitted or replaced suite.
Every version 6 required case, source, header dependency, build check and time
or output bound remains. No catalog-isolation names are added to the mandatory
requirements in this version.
The complete **version 7 native 28-suite execution was pending** at the private
preparation freeze and subsequently passed locally as recorded below. Earlier
27-suite campaigns do not qualify the new outcome source or complete version 7
selection; Linux, DEBUG_LOCKORDER, TSan and remote execution remain pending.
Version 8 retains those 28 suites and adds the six native Catalog regressions
to the mandatory `flowmesh_client_watch_lane_tests` selection: held passive
TLS versus local status/refusal/health, one owner and busy reconnect, stale
trust/endpoint observations, same-path CA replacement, whole-rowset staging,
and fresh shape-only reconnect with unchanged outbox. Independent controls
freeze all six actual case names and reject each if missing, disabled,
unexecuted or skipped. Every version 7 requirement, source, header, checker
algorithm, build/time/output bound and the nonpublishing workflow remain.
Complete native version 8 qualification was pending at the private source
freeze and subsequently passed locally, as recorded below. The earlier
489-case result does not qualify the new Catalog source.
Version 9 keeps all 28 suites and adds only two mandatory genuine selected-proof
cases in `flowmesh_client_watch_lane_tests`: a validator-certified account
remains verifiable while Catalog TLS is held, and corrupted serialized state
is refused under that hold. Exact names are independently frozen in controls
with per-case missing/disabled/unexecuted/skipped rejection. The Qt helper's
five offline mock cases are not counted as native C++, and this headless
GUI-off workflow does not qualify the Qt policy change. All version 8
requirements, algorithms, sources, headers, bounds and workflow remain.
Complete version 9 native host qualification was pending at the private source
freeze and subsequently passed locally, as recorded below.

Version 10 retains all 28 suites and adds three mandatory automatic-polling FIFO
cases: two integrated TLS client cases in `flowmesh_client_outcome_evidence_tests`
and one pure scheduler case in `flowmesh_client_poll_tests`. They require an
included caller to service an older unresolved demand, an unresolved caller to
service an older included outcome without misattributing its result, and the
unchanged scheduler's coalescing, bounds and nonrefunding removal. Independent
literal controls reject each case when missing, disabled, unexecuted or skipped.
The six earlier outcome-evidence names and all their negative controls remain
independently required. The earlier exact-set assertion expands only to the
union of those six names and the separately frozen two integrated FIFO names.
Every version 9 suite, source, header dependency, checker algorithm, execution
bound and workflow remains unchanged. This is headless native coverage; Qt
qualification remains separate.

Do not publish version 10 before the reviewed fairness implementation, narrow
generated-regtest test seam and all three test cases are committed together.
The non-RPC seam advances the existing scheduler monotonically to a bounded
future instant, invoking its ordinary refill and rolling-window expiry, and
may spend its existing burst tokens. It does not install a custom refill,
reset or refund a budget, alter signed originals or expose an operator flag.
The pure scheduler case is a complementary contract control, not a substitute
for either integrated failing-before counterexample. Version 9's completed
native host result below does not qualify this new source or version 10.

Version 11 preserves those 28 suites in their original order and adds the full
`flowmesh_sync_tests` suite as the 29th, with one mandatory real indexed-chain
ancestry case. Four new runtime cases require the ancestry-proved frontier,
permanent false/throw fallback, catch-up/restart with full startup validation,
and the default-false older-anchor/MAX-only falsifier. The production ancestry
translation unit gains the same strict compiler-flag checks; exact hashes of
`src/flowmesh/deposit.h` and `src/node/flowmesh_anchor.h` join the existing
admission-header dependency. All earlier required cases, mappings, algorithms,
time/output bounds, workflow and permissions remain. The implementation and
fixture correction are frozen at `b8136220` and `c920fcf4`; the v11 checker at
`2c99c038`. Do not qualify or publish this profile without those prerequisites.
See the [frontier source/test report](b3-flowmesh-proved-anchor-frontier-20261016.md).
The v10 500-case result below is historical and does not qualify this successor.

Version 12 adds one exact runtime case for an admitted original whose native
certificate is deliberately withheld from one replica. The fixture releases
only the first publisher's exact retained certificate, supplies bounded normal
250 ms maintenance, and forbids substitute attestations/catch-up. Three new
literal discovery/execution controls preserve the previous 61 controls, all
29 suites, algorithms, source/header checks, budgets and workflow permissions.
This is a fair-maintenance fixture correction and declared packet-loss control,
not a production timing/rate-limit change or proof of arbitrary-loss progress.
The first v11 full-suite failure remains in the
[frontier report](b3-flowmesh-proved-anchor-frontier-20261016.md). Version 12's
complete native qualification is reported separately below; v10 passes are not
its qualification. Root ran the 64 stdlib controls once, exit 0 (0.199 seconds);
the separate reviewer ran the frozen private copy once, exit 0 (0.204 seconds).

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

Version 5 separately requires and records the SHA-256 of
`src/node/flowmesh_client_read_admission.h` in
`profile_header_dependencies_sha256`. Headers do not have standalone compile
commands, so this dependency does not replace or relax any translation-unit
flag check. A missing declared header fails before native discovery. Its
recorded hash is a source observation, not proof of compiler inclusion in an
arbitrarily reused binary. Do not publish version 5 before its admission
implementation, header and all eight test cases are committed together.

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
Version 5 passes **40 controls**, actual local exit 0 (0.448 seconds). Its added
controls require all eight admission cases and both adapted old cases, and
reject each one if missing, disabled, unexecuted or marked skipped. A separate
header control verifies exact hashing, changed-content attribution and refusal
when the dependency is missing. The unchanged full-profile mock checks retain
the header hash in the compact result. All version 4 health/runtime/source and
build-flag requirements remain, as do the 27-suite selection and workflow.
These exercise missing/disabled suites, missing regressions,
empty or inconsistent XML, selected skips, wrong build flags, timeout and
sanitizer failure, process-group termination and bounded output. They do not
execute native FlowMesh or a sanitizer.

Version 6 passes **43 lightweight controls**, actual private local exit 0 in
0.317 seconds. Independent exact names protect both trace cases with per-case
positive, missing, disabled, unexecuted and skipped rejection controls. The
private runner was imported from its disposable directory; only the workflow
contract and child-process working directory used the unchanged repository.
This does not execute native C++ or qualify production timing. The version 5
native campaign below remains a separate historical result, not qualification
of the new tracing source. At that runner-control preparation freeze, native
version 6, Linux, DEBUG_LOCKORDER, TSan and remote CI execution were pending.

Version 7 passes **47 lightweight controls**, actual private local exit 0 in
0.190 seconds, with zero failures or errors. All 43 earlier controls remain;
only their profile-version and suite-count literals change. Four added controls
independently preserve the frozen 27-suite source mapping and require all six
outcome-evidence cases to be discovered and executed. Each named case is also
rejected individually when missing, disabled, unexecuted or skipped; the new
suite is separately rejected when missing or disabled. The private checker was
imported from its disposable directory, with the unchanged repository used only
for the workflow contract and Python-child working directory. Runner functions,
earlier required-case sets, native sources, header dependencies and execution
bounds were compared unchanged. A non-author reviewer separately ran these 47
controls once: exit 0, zero failures/errors, 0.190 seconds. These are runner
controls, not native execution; the later native host result is separate.

Version 8 passes **50 lightweight controls** in a private author execution
(exit 0, zero failures/errors, 0.196 seconds) and one separate reviewer
execution (exit 0, zero failures/errors, 0.200 seconds). All previous 47
controls remain; three new controls cover the exact Catalog names and each
negative discovery/execution fault. Source mapping was checked against the
actual future C++ translation unit. These are stdlib runner tests, not native
C++, Linux lock-order, TSan, remote CI or performance qualification.
Version 9's author ran 53 private stdlib controls once: exit 0, zero failures
or errors, 0.196 seconds. The non-author reviewer independently ran them once:
53 passed, exit 0, zero failures/errors, 0.198 seconds. All previous 50 controls
remain; three additions protect the two actual C++ names and their negative
discovery/execution cases. Native, Qt, Linux and TSan execution are separate.
Root also executed those exact 53 controls once after integration: exit 0,
zero failures/errors, 0.185 seconds. These three executions are not combined
into a larger test population.

Version 10 adds three lightweight controls for its three exact FIFO names,
including per-case missing, disabled, unexecuted and skipped rejection. All 53
version 9 control bodies remain, apart from the version literal and the exact
outcome-set union adaptation described above. At the private preparation freeze
execution was pending. Subsequently root ran all 56 once with exit 0,
zero failures/errors, 0.189 seconds; the non-author reviewer independently ran
the frozen private suite once with exit 0, 0.201 seconds. Its unchanged private
workflow copy was used without a source-root override. The author executed
none. These are separate stdlib controls, not native execution; the complete
host selection is recorded below. Linux, DEBUG_LOCKORDER, TSan and remote CI
remain unexecuted.

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

The [bounded client-admission candidate](b3-flowmesh-bounded-client-read-admission-20261016.md)
passed 481 native cases in 27 suites locally, with all actual children exiting
0 and no selected skips. That host runner imported v4 at startup and retains
the label `host-local-macos-ci-selection-v4`; its complete discovery and XML
were then rechecked with the frozen v5 mandatory rules (27 suites, 481 cases,
exit 0). It is not relabelled as Linux/TSan CI.

Separately, the author ran v5's 40 lightweight controls with exit 0 in 0.448
seconds; the non-author reviewer ran them once with exit 0 in 0.186 seconds.
These do not execute the C++ gate, TLS integration, Linux lock-order build or
TSan instrumentation. No remote CI run, push or release qualification follows
from this update; Linux, DEBUG_LOCKORDER and TSan remain unexecuted.

The admission-trace stage subsequently passed 483 cases in 27 suites on
ordinary macOS, with all actual children exiting 0 and no selected skips.
Its runner imported v5; preserved XML was independently rechecked under v6,
not relabelled as a Linux run. Native executable SHA256 was
`f8a979c85dcbf933c2e087df7964dcbee713e6533c6ef5cf705ebcc11d58db31`.

Version 7's exact full local `host-local-macos-ci-selection` imported manifest
`native-guardrails-v7` and passed **489 cases in 28 suites**, all actual child
exits 0, no selected skips, 224.590 seconds. Native executable SHA256 was
`24424b0deb86aab7a7d7a8afb857ff95f052c35e1e0e8f8e0973cb3d6e72f718`.
Its client source is the separately reviewed
[transient evidence-budget repair](b3-flowmesh-outcome-budget-recovery-20261016.md).
This ordinary AppleClang 17 Release run does not validate Linux flags,
DEBUG_LOCKORDER, TSan, remote CI, production timing or 200 ms performance.
No native Catalog successor is included in this qualification.

Version 8's exact local host selection passed **495 cases in 28 suites**,
all actual children exit 0, no selected skips, 226.365 seconds. The preserved
native executable SHA256 was
`8c6164da311b159816a0849b71f8874e7d4a9b0a5b91116d80d57eb3f5b5eb96`.
It includes the [annotated Catalog transport stage](b3-flowmesh-native-catalog-isolation-20261016.md)
and its six required regressions, all four retained transport-premise adaptations,
and the unchanged evidence-budget recovery control. The linked offline Qt suite
also passed 362 cases with three existing opt-in visual skips. Native and Qt
elapsed times are functional qualification under concurrent test execution,
not controlled trading-performance comparisons. Linux, DEBUG_LOCKORDER, TSan,
remote CI, Windows, WAN, release and 200 ms qualification remain unexecuted.

Version 9's exact local host selection passed **497 cases in 28 suites**,
all actual children exit 0, no selected skips, 225.172 seconds. The preserved
native executable SHA256 was
`8386b179bb4742483f67ba3c6f9fdff1832490a2938a16ce714b61015c7aff19`.
This adds the two genuine selected-account proof/refusal controls without
removing any previous case. The separately executed offline Qt helper passed
82 tests, and workspace passed 362 with the same three opt-in visual skips.
Their [selected final-preflight policy and limits](b3-flowmesh-selected-final-preflight-20261016.md)
are not qualified by a headless GUI-off workflow. These ordinary macOS runs
do not establish Linux, DEBUG_LOCKORDER, TSan, remote CI, Windows, WAN,
production availability or 200 ms performance. The workflow remains unpublished.

Version 10's exact local host selection passed **500 cases in 28 suites**,
all actual children exit 0, no selected skips, 227.968 seconds. The preserved
native executable SHA256 was
`52a27a3d7bdc21d5c365e6dd86758486605cb2e2c0df0b3f003704727d3a0fe1`.
It includes both integrated FIFO cases and the complementary pure case, with
all earlier outcome-budget, Catalog, health, persistence and recovery cases
retained. The [fairness stage report](b3-flowmesh-outcome-fifo-20261016.md)
records actual failing-before evidence, fixture failures, scope and hashes.
The separate Qt helper/workspace execution passed 82 and 362 cases, with the
same three opt-in visual skips. Functional runs overlapped, not a controlled
latency comparison. Ordinary macOS, linked non-RPC regtest-only poll seam,
crash-test macros off: not Linux/DEBUG_LOCKORDER/TSan, remote CI, Windows/WAN,
200 ms, production or release qualification. Earlier v9 and all predecessor
results retain their own source/binary identities rather than being relabelled.

Version 11's 61 lightweight controls passed once in the separate private
reviewer context (exit 0, 0.221 seconds) and once against the committed root
files (exit 0, 0.200 seconds). All 56 previous control methods remain, with
only the disclosed version/count, added-suite union and expanded-header fixture
adaptations. Version 11's first complete native host attempt failed an existing
local-original resumption wait in the runtime suite: P2P 14 passed, runtime
141 passed/one aborted; the strict runner stopped before the remaining 27 suites.
Its failure is retained, not replaced by an isolated pass. The later trace
diagnosis and fixture-only fair-maintenance correction are recorded in the
[frontier report](b3-flowmesh-proved-anchor-frontier-20261016.md).
The separate Qt helper/workspace passed 82/362 with three unchanged visual skips.
That first v11 attempt remains failed. The v12 successor has a separate
qualification record; controls do not qualify native behavior or Linux/TSan.

Version 12's complete local host selection passed **533 cases in 29 suites**
once against source `0d119032046da1e1dca557583cb691fd70299ee6`. Actual wrapper
and every native child exited 0; exact discovered/executed names and counts
matched, no selected skips, warnings/expected failures, timeouts or output
overflows. Elapsed functional campaign time: 253.963 seconds. Native SHA256:
`3c55c0c2544f395e1fa08d034697ee30993125f9514308c4bb54583c99e60a1b`.
Checker SHA256:
`7af2bac39d0c8f2e55ab302731ccbf69f8d83330ec1ead0a3c732c5ff0f9a369`.
The [frontier report](b3-flowmesh-proved-anchor-frontier-20261016.md) discloses
local archive build-ID metadata, exact source equivalence, the retained v11
failure and the test-first declared-loss control. Ordinary macOS Release,
crash hooks off, narrow non-RPC regtest poll seam linked: **not** Linux,
DEBUG_LOCKORDER/TSan, remote CI, WAN, speed, full V2 or release qualification.
