# FlowMesh: bounded retention of accepted local originals

Date: 2026-10-16. Status: corrected local native qualification **PASSED within
the declared scope**; earlier failures are retained below. Source predecessor:
`680fe0d`. This repairs a local scheduling loss, not consensus, economic rules
or the 200 ms latency target.

## Reproduced loss and the exact boundary

A signed local action could be accepted into the runtime queue, then disappear
before entering the action pool when B3 reconciliation temporarily closed the
current-authority gate. A wrapper overtaken by a certified head needed the
ordinary authority and retained-interval recheck. A temporary failure was
reported as `POOL_REFUSED`, and subsequent ordinary maintenance no longer owned
the exact original. Recovery could therefore require another client request.

The new regression was run against the unchanged predecessor runtime through
its existing queue seam: one test, exit **201**, **101 of 102 assertions passed**.
It failed specifically because the accepted original was refused solely while
reconciliation was temporarily unavailable. Its private capture/reserve and
enqueue stages expose the same production scheduling boundary. The old-source
test adapter queues the captured original wrapper without a reservation. This
is an encoded-queue/staged-admission reproducer, **not a public HTTPS/RPC
end-to-end reproducer** or a new measurement of network latency.

The separate immediate seat-key observation control also reproduced stale
observer metadata: exit 201, 32 of 33 assertions passed. That administrative
metadata repair is distinct from accepted-action retention and does not
authorize a vote or prove trading progress.

The first integrated runtime suite then exposed four failures: 127 cases
passed, four failed, with 187,968 passing and five failing assertions; child
exit 201, qualification-wrapper exit 1. These failures are retained, not
reported as successful qualification. Two were real interactions introduced
by the draft; two were incorrect new fixture preconditions, detailed below.

## Ownership before acknowledgement

The [runtime](../../src/node/flowmesh_runtime.cpp) reserves an independently
owned source and its counted attribution before a potentially blocking
deposit-facts check, and before acknowledging the queue admission. Preflight
reservation is not acknowledgement: while `admitted=false`, maintenance may
expire the source but never submit it. Failure or exception releases that exact
source through its scoped owner; delayed facts cannot allocate a replacement
token or renew its deadline. It retains:

- The exact encoded signed action payload and original routing header.
- Domain, execution configuration, semantic ActionId and fixed original expiry.
- Original NODE/PUBLIC attribution and submitter; a PUBLIC frame's exact
  attribution serial and ownership count.
- A monotonically allocated in-process token carried by the queue copy.

The [queue metadata](../../src/flowmesh/p2p.h) token is never serialized, signed,
framed or relayed. Normal network ingress always uses token zero. A token grants
no admission, authentication or consensus privilege. Processing checks the
owned source's entire original header, payload and counted flag, not ActionId
alone: ActionId intentionally excludes credentials. Remote peers cannot claim
reservations; stale or aliased copies cannot execute or settle another source.

A higher-priority queue eviction can remove a disposable queue copy without
removing its retained original. Ordinary maintenance can enqueue another copy
of that same original. Once it is pooled or terminally handled, its source
retires; later copies are inert. No retry signs a new instruction or changes
the economic action. Existing pool duplicate checks remain in force.

## Count, byte, work and time bounds

| Resource | Bound |
| --- | --- |
| Retained local originals | At most 8 globally |
| Local originals plus existing deferred recovery input | At most 128 objects and 16 MiB accounted bytes globally; 16 objects per market |
| Existing deferred input from one peer | Unchanged: at most 8 |
| Original local processing eligibility | Fixed 60-second deadline from original capture, never renewed by slow preflight or retries |
| Ordinary deferred maintenance pass | At most 8 inspected records total, including at most 2 local sources |
| Real reopen/generation-change pass | At most 32 inspected records total, including at most 8 local sources |
| Ordinary open-gate retry spacing | At least 250 ms |

Local source bytes are `WireMessage::MemoryUsage()` (fixed wire-header size
plus payload size) plus retained submitter bytes; the queue's own existing byte
limits still apply separately. Transport overhead and object/container
allocation overhead are not included. This is
**not a measurement or bound of total C++ heap consumption**. Reserved sources
are charged even before their first queue copy is admitted, so incoming
recovery traffic cannot consume promised capacity after acceptance. Retention
does not evict durable signing obligations or clear their evidence.

Rotating maintenance cursors count inspected records even when they are idle,
expired, ineligible or refused by queue admission. The local and existing
deferred loops share the same 8/32 record budget; the separate existing bounded
catch-up request loop is unchanged. A closed gate expires owned sources but
does not enqueue them. Ordinary open retries are paced; a real gate reopen or
delivery-generation change may trigger the existing immediate pass. This is
bounded maintenance work, not an arbitrary-load availability guarantee.
Cleanup occurs when a source is processed or reached by bounded maintenance;
delayed timers can retain expired objects longer in memory, still under the
same count/byte caps. An expired source is not authorized to enter the pool.

## Authority and exact-original checks remain mandatory

Deferral is only an internal scheduling decision for already accepted local
originals. It does not bless a cached-ready state. On processing, ordinary rules
still require the exact domain/configuration, current epoch and permitted
header interval. An old local header can advance only through the unchanged
complete retained certified interval, at most two slots behind, with no prior
inclusion and a still-pending action. Current authority and anchored deposit
facts are rechecked where previously required. Missing interval/facts, invalid
credentials, wrong epoch, actual halt, signing/store conflict and incompatible
handoff are not made valid by retention. Remote ACTION handling is unchanged.
Equal-header ACTION admission to the pool retains its original behavior while
B3 authority is unavailable, so missing action bodies can supply agreement
recovery evidence. Pool admission is not signing authority. Retention applies
where the existing behind-header or candidate-restoration path actually needs
unavailable authority; every existing signing gate remains mandatory.

Successful pool admission transfers ongoing ownership to the existing pool.
It is not a matched-fill guarantee. Certification, execution, quorum, durable
signing records and accounting application remain unchanged.

PUBLIC attribution is pinned to its original source. Source retirement owns
one frame settlement; queue copies do not repeatedly increment or decrement
it. A recycled ActionId attribution cannot make an old source settle a newer
owner's count. Source tokens never reset across same-object Stop/Start;
overflow refuses before acceptance. Stop retires sources only after joining
the worker. Startup retains its queue-only idempotent fast return and uses
market-then-queue initialization order, with a second started-state check;
it does not reinitialize a stopping worker during join.

## Findings retained from separate source review

1. **Fabricated reopen edge in the first draft.** A PAUSED seat transition can
   coexist with an open B3 gate. Unconditionally marking the shared gate closed
   after that pause would make every tick appear to reopen it, bypassing the
   existing pacing. The successor marks a closed observation only when the
   B3 gate was actually observed closed. The zero-clock PAUSED regression checks
   processing work and unchanged expiry separately.
2. **Malformed reward claims could shelter behind unavailable authority.**
   Transport shape permits empty or wrong-sized reward credentials. Deferring
   all claims before authentication could retain such impossible signatures.
   The successor applies the existing 96-byte canonical, subgroup-valid,
   non-infinity BLS decoding requirement before deferral. Empty, 95-byte and
   96-zero credentials are rejected. A well-encoded signature is **not proof
   of historical membership or authorization**; an unknown claim remains
   bounded and unpooled until its unchanged historical verification succeeds
   or refuses it.
3. **Integrated held-facts attribution regression.** Moving counted-frame
   ownership after the facts check broke two unchanged assertions in
   `only_a_counted_frame_settles_its_public_submitter`. Reservation now occurs
   before facts and owns exactly one frame through success, refusal, exception,
   expiry or Stop. The original test is unchanged; the successor adds cleanup
   controls for failed and throwing preflight.
4. **Integrated old-view recovery regression.** Blanket B3 gating of local
   ACTION pool admission delayed an equal-header body until after the reopen
   tick had consumed its existing 250 ms evidence-retry turn. The unchanged
   old-view test reproduced the loss: 264 of 265 assertions passed, exit 201;
   the expected proposal-forward count stayed at eight. The successor restores
   original equal-header pool admission, not permission to vote. No clock
   advance, budget reset, quorum change or weakened assertion obtains progress.
5. **New fixture errors, not authority repairs.** The capacity fixture used an
   invalid AGREEMENT version prefix; it now supplies the existing cheap framing
   prefix and explicitly checks framing before admission. Limits are unchanged.
   The token Stop/Start fixture attempted to reuse a completed preagreement
   bootstrap-creation capability. Its token-only lifecycle check now uses the
   ordinary non-preagreement fixture with an unchanged marker. Production
   bootstrap permission and replay checks are not relaxed.

Integration failures reopened separate review of the exact correction delta.
The successor source review passed at the runtime/test file identities recorded
below, with the two original failing test functions independently confirmed
byte-identical to their predecessor. Reviewer inspection did not execute native
tests and is distinct from the actual implementer qualification below.

## Qualification and reproduction

The [runtime regressions](../../src/test/flowmesh_runtime_tests.cpp) cover the
reported queue loss, subsequent agreement without another client request,
reservation count/bytes, priority-copy loss, aliases and remote tokens, expiry,
Stop/Start, delayed original preflight, token exhaustion, forged credentials,
open-chain seat-pause pacing, and Start during Stop/join. The
[P2P regression](../../src/test/flowmesh_p2p_tests.cpp) checks private metadata
compatibility and unchanged wire/admission behavior.

Actual failing-before and first integrated failures are reported above. The
corrected ordinary macOS Release native build passed the exact 25 suites
selected by the [native guardrail manifest](../../ci/check_flowmesh_native_guardrails.py):
**398 executed cases, all passed, exit 0**. Discovery and XML execution names
and counts matched, with no selected skips. The affected runtime suite passed
all **131 cases and 188,286 assertions**; P2P passed all 14 cases, production
storage all 24 cases, and chainstate-manager validation all 19 cases. This is
the selected native campaign, not every repository test or a release campaign.

The retained local result profile is `host-local-macos-ci-selection-v2`.
Summary and XML evidence identify ordinary Darwin Release execution, with
`linux_ci=false` and `sanitizer_qualified=false`. No Linux CI, TSan, performance
or live-trading result is inferred from this run. The clean scratch build
excluded unrelated local V2 prototype/CMake changes and client-only crash hooks;
neither baseline-only test adapter was compiled into the successor.

Exact tested identities (SHA-256):

| Object | Identity |
| --- | --- |
| Runtime source file | `c1f57d9858685b63769b077c6f1c75f536ceef68ddec0aa4f1c8ab62979e7ead` |
| Runtime test source file | `4844e9926659e8c329d8c8248f6239e4e00e9990b208e791d788f7c9e6271e66` |
| Native test executable | `fbd7949f09fafade9b3544a1b2786ed4194593de59391633ff39bd2e06d0b676` |
| Companion ordinary daemon build | `069226d05429fa7256f11b28074796b0dc63680676f209142f2bffed4dee7671` |

The file identities bind the compiled source candidate; they are not Git commit
IDs. The integrated campaign also contained the separately reviewed metadata
and mutation-fence candidates. The companion daemon identity is build evidence,
not evidence of launching it, trading latency or VPS deployment.

The first integrated successor compilation exited 2: three nonnegative
`count_if` results required explicit signed-to-`size_t` conversions, and the
separate metadata helper required its exact `cs_main` declaration include.
Those compile corrections do not change a policy, predicate or assertion.
The corrected rebuild and successful subsequent execution are recorded above;
the first failed integrated runtime run remains part of the evidence.

From a clean ordinary native build, reproduce the complete affected runtime
and P2P suites from the repository root:

```sh
./build/bin/test_bitcoin \
  --run_test=flowmesh_runtime_tests,flowmesh_p2p_tests \
  --report_level=short --log_level=error
```

The old-source reproducer additionally uses the test-TU-only
`FLOWMESH_LOCAL_RETENTION_BASELINE_TEST` adapter, not a production option.
Its decisive case is
`accepted_local_original_waits_for_authority_then_resumes_without_client_resend`.
The separate metadata control uses `FLOWMESH_SEAT_OBSERVATION_BASELINE_TEST`.
Neither adapter belongs in successor qualification binaries.

## Explicit limitations and next gates

This original-source cache is **memory only**. Expiry, process restart and Stop
do not prove global nonexecution. Unknown expiry/binding changes emit diagnostic
events, not a false `POOL_REFUSED` execution verdict. Exact saved-client-outbox
reconciliation remains necessary; no automatic client resend, fresh signature,
sequence replacement or obligation clearing is introduced.

An unchanged two-slot or same-epoch proof limit can still refuse a delayed
original after too much certified progress or an epoch change. A syntactically
valid but unknown reward claim can occupy bounded space while authority is
unavailable. Source retention does not repair unavailable required evidence,
permanent safety halts, all HTTPS permit starvation or base-chain lock waits.

No preserved wallet, order, signing journal, live validator, VPS, mainnet chain
or bridge contract is changed by this local qualification. Machine power loss,
full public-API integration, Qt/WAN performance, 200 ms trading, V2 futures,
bridge compatibility and release readiness remain separate gates. Same-object
token lifecycle coverage uses ordinary mode, not preagreement bootstrap
qualification; existing preagreement reopen tests retain their separate scope.
