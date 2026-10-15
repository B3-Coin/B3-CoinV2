# Bounded authenticated foreground client read admission

This local stage changes admission for direct remote `Market` and `Data`
calls that carry an account and run in a foreground work scope. They may
wait behind the existing ordinary read owner instead of immediately failing
busy. Passive, accountless and Watch reads retain immediate admission. The
candidate has a separate read-only source review PASS. Root execution passed
481 affected native cases; the author did not build or run those tests.

## Versioned scheduling policy

The node local profile is `authenticated-foreground-read-admission-v1`:

- One owner covers the ordinary TLS flight, bounded parsed reply,
  verification, durability and application. Up to eight foreground callers
  wait on their own stack frames, so the total is at most nine participating
  callers. The gate owns no queued request bodies, callbacks or authority.
- Eligible callers are FIFO. Immediate callers cannot steal an idle
  transition from the queue. A ninth waiter receives an explicit full refusal;
  passive, accountless and Watch contention keeps the existing busy refusal.
- The new absolute admission deadline is the original direct call entry plus
  five seconds. Spurious wakes and queue changes do not extend it. Expiry wins
  a simultaneous release. This is a queue deadline, not a whole call deadline.
- The existing five second timeout for each HTTPS attempt and its trust check
  remains unchanged. Owner work, endpoint attempts, local locks and durable
  writes can outlast the queue deadline. No owner is interrupted.

`FlowMeshClientReadAdmission` uses the repository's annotated `Mutex`,
`WAIT_LOCK`, `LOCK` and guarded state. It holds that mutex only for admission
state, never across `m_work`, chain locks, network, proof work or durability.
Each linked waiter is removed on grant, expiry or a thrown wait; release and
removal notify the remaining callers. The existing work gate, its priority,
linger and fairness policy are unchanged.

## Current context and unchanged freshness

`Market` and `Data` explicitly create their entry policy and timestamp.
`DataImpl` supplies the shared read path without changing the public backend
interface. Watch fallback and snapshot continuation explicitly use immediate
admission, even with a foreground authenticated caller; they do not inherit
the direct call policy while retaining a watch lane.

After admission, `RefreshRead` captures the then current pins, epoch, cache
revision, high water and own action floors under `m_work`. The existing
context checks still fence a later reset, checkpoint or authority change.
A reset while queued does not bind the caller to an obsolete context.

The ordinary observation's start timestamp remains the original direct call
entry, including queue time. Validation does not re-clock that timestamp.
Signing `Market` preserves its optimistic no-network join and rechecks the
same `JoinFresh` predicate after admission. A successful join preserves the
cache's original stamp and performs no Save. A refused join proceeds under
the same lease without recursive admission. Direct `Data` still requires its
own observation. Production join windows remain 50 ms since validation and
1000 ms since the original observation start.

The aggregate snapshot SyncWrite path, high water, no-resubmit obligations,
proof/authority validation and action delivery are byte-identical to the
frozen `5f717923` client. API handling and the HTTPS factory are also unchanged.
No action is automatically signed, submitted or resent because a read waited
or was refused.

## Retained control evidence

Root retained the predetermined ordinary-daemon control and capture arms
privately as `post-health-result-control` and `post-health-result-captured`.
The [sanitized diagnostic](b3-flowmesh-post-isolation-diagnostics-20261016.md)
records their exact provenance and timings. Each burst completed one of eight verified
results; seven requests failed before signing on ordinary read admission,
without ActionIds or submission attempts. Root reported all five final child
exits zero and drained threads. These small diagnostic arms establish the
observed availability failure, not a reliability, 200 ms or capture-overhead
claim.

The original private native fixture is retained separately as
`flowmesh_https_tests.cpp`,
SHA256 `0c655e427208f3c1716f7dcd1d366d82492ee2679c1f239c32c24b615065630b`.
Root execution `55139` returned 201: two failed cases, 1496 passing assertions,
nine failing assertions and one aborted leaf after the expected absent Data.
The BEFORE XML is retained separately. That is a failed old-client control,
not a passing model or native qualification.

The explicit old-test scheduling successor was frozen before production edits
as the private `adapted-before-v1.cpp`, SHA256
`ac7db08058cc0ef067e7398e9bd8c213b59f7538dfbd43793ca13ad4ac0ca841`.
Against the unchanged old client, root's corrected singular filters returned
201 for each adapted case: three failing assertions for expiry and eight for
the two reset variants. The wrapper's exit zero meant expected exits
`[201, 201]`, not that the cases passed. The initial combined selector returned
200 with zero selected cases; that selector failure is retained, not counted
as native coverage.

## Explicit test adaptations and focused controls

Only two existing test bodies change their scheduling expectation:

- `signing_preflight_can_join_only_the_original_stamp_during_passive_https`
  retains its initial join, fixture windows, original head and no-network
  counts. The expired foreground prerequisite must now remain pending while
  held, without using the expired original stamp or sending another body.
  After release it must obtain a verified result, either joining the newly
  validated passive observation with its unchanged stamp or performing one
  fresh read.
- `explicit_connect_and_reconnect_fence_an_original_preflight_join` retains
  both reset variants, the otherwise unexpired premise, no-body-before-release
  counts and the old reply's `superseded` refusal. The waiting foreground
  prerequisite must then perform exactly one current-context read and return
  verified data. It cannot join the reset original observation.

Captured error strings and promises in these two adapted cases now outlive
their futures. All other original case bodies remain byte-identical to `3e515f41`.
The original two private prospective case bodies and their helper body remain
byte-identical to `0c655e42`; only the separate introductory comment now names
that historical BEFORE control. The entire successor TU is not claimed to
match the BEFORE TU.

Eight added cases occupy existing native suites, without workflow or suite
manifest expansion: the original two real TLS/proof controls plus three pure
gate controls and three further integration controls. They cover actual
eight-waiter insertion and FIFO grants, full refusal and no stealing, absolute
deadline expiry and spurious notifications, owner exception release, a new
post-wake join, original queue-age freshness, authenticated Watch immediate
refusal, and recovery after a queued unknown-market exception. The original
two also require responsive status, locally rejected unsigned Submit and real
TLS health, passive immediate busy, reconnect fencing, exactly two snapshots
and no action network request. The accountless default-foreground busy cases
remain unchanged and still exercise the declared immediate policy.

## Frozen source and qualification gates

The initial candidate identities are:

- `src/node/flowmesh_client.cpp`: SHA256
  `461838346366b9264f5ee0d5e597f6a52f96375d7adc606fa8e39837814d16f0`.
- `src/node/flowmesh_client_read_admission.h`: SHA256
  `7333dff18859e25e6fac6a5b2eb33e084250d45a9be6cc9ab00987c67b243f56`.
- `src/test/flowmesh_https_tests.cpp`: SHA256
  `aef686468e9d683471ec4fdc6edf3338bb07b42f929b743de1e022f3a3e6c664`.
- Unchanged `src/node/flowmesh_client.h`: SHA256
  `0a5ee8e6d0a4029064565ae7a0b409dac157ee4232956f479d29b68488e87bec`.

Author checks were read-only source comparison, hashes and whitespace checks.
Static extraction found 148 original case bodies, all preserved except the
two explicitly adapted cases, and 156 successor case bodies. No builds,
native tests, daemons or commits were performed by the author. A separate
non-author review returned scoped source and fixture PASS at these three
candidate hashes, with no builds or execution by that reviewer.

Root's exact AppleClang 17 Release C++20 build completed with exit 0. The new
annotated gate and its callers introduced no thread-safety/alias warnings;
the pre-existing duplicate-library linker warnings remain. Preserved native
test executable SHA-256:
`c5791f10958be7b0ec58a6d2c82338333e9ccfaa626820bd899448c6f38343cb`;
daemon SHA-256:
`5cd64c6733dd5f1d131b429a6d27a7894a37a0cf01be1ab11b52286a1c4f4085`.
No crash-test macros or unrelated native prototype were included.

The full host-local native run exited 0: **481 discovered and executed cases
across 27 suites**, every actual child exit 0 and no selected skips. This
includes all eight new cases, both adapted cases and the previous health,
agreement, retention, client/work-gate, storage, authority and timing controls.
The runner imported `native-guardrails-v4` at startup and labels its result
`host-local-macos-ci-selection-v4`. Subsequently the frozen v5 checker
rechecked the same discovery and all retained XML against its strengthened
mandatory-case rules: 27 suites, 481 cases, exit 0. That is a local result,
not Linux CI or sanitizer execution.

The linked Qt workspace and wallet were also compiled. The full offline Qt
workspace exited 0: **356 passes, 0 failures and the same 3 explicit opt-in
visual skips**, in 44.685 seconds. Linked test SHA-256:
`b5a9d7d25e4513e948979db2bfed2d765ce59ade623fad05638bffd524253891`;
wallet SHA-256:
`38c1301c426dda142777efb0fe04d0598059da0771cdc3b66e50c13a6960ce36`.
The wallet was compiled, not opened with live data. The predeclared successor
trading diagnostic is separate. DEBUG_LOCKORDER, Linux and TSan remain unexecuted.
Functional tests and a separate source review are not an independent security
audit or production/release qualification.

## Reproduction

After a normal test-enabled build, run these separately (not as a comma-joined
Boost selector). Both use disposable generated state, not a running wallet:

```sh
admission_test_dir="$(mktemp -d)"
./build/bin/test_bitcoin --run_test=flowmesh_https_tests -- -testdatadir="$admission_test_dir/https"
./build/bin/test_bitcoin --run_test=flowmesh_client_watch_lane_tests -- -testdatadir="$admission_test_dir/watch"
```

For the complete guarded Linux profile, use the exact build and execution
commands in [native guardrails](b3-flowmesh-native-linux-guardrails-20261016.md).
Its validation must not be bypassed or relabelled as a host-local macOS run.

The exception tests exercise owner unwinding and a real unknown-market read
failure. A condition-variable wait throwing is source-inspected, not injected
or observed. No power-loss behavior is newly qualified; the unchanged
aggregate durability stage retains its own distinct fault evidence. The
strict default-window positive join control may fail if the environment
cannot establish its timing premise; it must not widen windows or skip the
assertion to obtain a pass.

Market and Data have no per-call cancellation token, and RemoteBackend has no
new Stop protocol. A retired caller may fence publication elsewhere but must
keep backend ownership alive until its synchronous call drains. The queue
deadline does not cancel its owner, TLS, local proof work or fsync. Concurrent
backend destruction is not a valid shutdown test. Native fixture guards
release held replies before joining futures, but an incorrectly implemented
gate that waits while holding `m_work` could deadlock, so hard external native
process timeouts remain necessary. No bounded shutdown, WAN, Linux, throughput
or durable-client-outcome qualification follows from this candidate.
