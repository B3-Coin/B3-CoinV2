# Restricted health overflow admission — 2026-10-16

Status: separately source-reviewed bounded health stage. Root applied the
reviewed API wiring after aggregate commit `aa5e3bb`; its ordinary native build
passed (execution 70621). Root's complete affected native v4 selection passed
**473 cases in 27 suites**, execution 86917, every child exit 0 and no selected
skips. No production system, public release or deployment is involved.

## Frozen identities / API wiring provenance

| Artifact | SHA256 |
| --- | --- |
| `src/node/flowmesh_https.cpp` | `464ec6dead454f2287c83b0be68fe6c73170016aa488f4a3f24a4f840f17d2d9` |
| `src/node/flowmesh_https.h` | `fed884440f5438ea24152372a32fa7cf3324ef9380c93d2ce316a0682e4bf2db` |
| `src/test/flowmesh_https_tests.cpp` | `3e515f411a2837c790cf81c5d7b4e75fe453486e35fa23a2e9e2de3a532fc92e` |
| Private `/private/tmp/b3-health-api-stage.YJHMEa/api-regions.patch` | `16e10c0ebd7ee533563dd8efc84dc28c5861f70dd9615cb31d2f13dc78ea441e` |
| Applied `src/node/flowmesh_client.cpp` | `5f717923a5b9aa394b85367a6d44d2537df3600f33f2fca825368ffd6908ea08` |
| Applied `src/node/flowmesh_client.h` | `0a5ee8e6d0a4029064565ae7a0b409dac157ee4232956f479d29b68488e87bec` |

The prior shared client was `fb16f124…`, with header `fc0cde04…`.
The private patch passed applicability checks and separate source review before
root authorized its application. Its only implementation regions
are the existing TradingApi health branch/helper, a restricted health entry
point, and `MakeFlowMeshTradingApi`; header changes are API capacity comments.
Simulating the patch verified the entire client-local/RemoteBackend/aggregate
region byte-identical; the applied files exactly match the reviewed future
files. No runtime, Qt, client behavior, aggregate save, crash hook or functional
durability fixture was changed by this health stage. The separately reviewed
CI v4 successor requires all thirteen health regressions; it is not an actual
Linux or TSan execution result.

## Operational profile: restricted-health-overflow-v1

An optional third `FlowMeshHttpsServer` constructor handler enables exactly
one additional exclusive worker. Generic two-argument callers retain their
existing behavior. The ordinary ceiling remains **16**, ordinary API permits
remain **2**, and combined action-wait/watch capacity remains **14**. Only a
configured restricted health handler permits the explicit **17 total worker**
ceiling. This is an operational capacity change, not a protocol/quorum rule.

The reserve has its own one-owner TLS/read/write capacity and never consumes,
releases or waits for an ordinary application permit. It can dequeue only
under the existing queue mutex when `active + resuming >= permits`. Ordinary
workers retain the exact opposite availability predicate and resumer priority.
An idle ordinary-capacity request therefore is not randomly stolen by a
competing general worker on the reserve.

Eligibility is observed at dequeue. Ordinary capacity can become free while
the reserved connection subsequently performs TLS/read/classification. There
is no migration or automatic economic retry: a nonhealth request already
owned by the reserve is still explicitly refused before action admission.
The ordinary active count includes TLS, reading, writing and closing as well
as application work; this is not a promise of admission during any burst
whose transport owners momentarily fill both ordinary permits.

## Ownership, bounds and stop

The reserve takes the existing `unique_ptr<Connection>` intact. Its socket,
SSL, socket BIO, counted lifetime, original absolute deadline, request count
and expiry remain the same owners. No parsed handoff queue or cross-thread
stack callback is created. Reserved requests have empty suspend/resume
callbacks and are never sent to the ordinary handler.

Exactly one additional Request body and parsed DOM owner can exist, bounded
by the existing configured request limit (default 64 KiB, hard maximum 1 MiB)
and the same complete JSON parser/depth bound. This is not a claim that DOM
allocator overhead equals raw JSON bytes. One extra bounded reply owner is
also possible under the existing reply cap. Queued parsed-body bytes are zero.
All ordinary/idle/queued/reserved/closing connections still share the existing
global `max_connections`; the queue shares the existing `max_queue`. No slot
or descriptor cap is silently raised to seventeen connections or beyond the
configured limits. The private test-only admission getter observes counts;
it does not alter them or expose bodies.

The same TLS context, HTTP framing/body/header/pipeline rules and `TlsIo`
deadline/stop checks serve both lanes. No deadline is restarted at reserved
dequeue or classification. Successful health may park for ordinary sequential
keepalive; every subsequent readable request is classified anew. Explicit
before-admission busy replies close and do not gain sticky health privilege.
Stop refuses new work, wakes normal waits, clears queued/idle owners, and joins
both ordinary and reserved workers before destroying TLS context/listener.

## API dispatch, once-only budgets and refusal

The applied factory wiring uses the **same TradingApi instance** for both
handlers, and enables the reserve only when its health policy is enabled.
The ordinary handler is unchanged apart from extracting its existing health
branch into `HealthReply`.

`HandleReservedHealth` performs the existing ordinary pre-parse admission
charge exactly once, including malformed or nonhealth bodies. It then parses
the complete JSON body, applies duplicate/unknown-field-safe exact envelope
checks, requires decoded method `health` and an empty-object `params`, and
calls only `HealthReply`. Field order/whitespace are irrelevant. Missing,
null, array or nonempty params do not qualify; filtered health remains valid
on the ordinary API path, so no protocol parameter rule is removed.

Valid health is charged by the same tighter per-peer health budget and uses
the same cached advisory local observation. The reserve never calls general
`Call`/`Handle`, action decoding/authentication/admission, a wait or a watch.
Every other complete, budget-admitted reserved body receives HTTP 503 with
`Public trading handler capacity busy before admission` and closes. Existing
ordinary/health budget refusals remain their exact HTTP 429 contracts and
also close on this lane. Unexpected restricted-handler exceptions fail to an
explicit before-admission busy/close, not to general dispatch.

This refusal concerns this request, not the outcome of an earlier possibly
delivered original. Client signing, uncertainty, exact-byte retention and
economic retry rules are untouched; no hidden retry was added.

## Retained BEFORE and successor controls

The exact original two-case TU is retained at
`/private/tmp/b3-redesign-successor.gogiQI/public-health-before-tests.cpp`,
SHA256 `a1abd251159dbbc60bb0d0531c9cac3bbc20247f12d9b0b3384011b5b01964aa`.
Root reported its real-health-under-held-cs_main positive control passing and
its generic synthetic two-parsed-handler saturation counterexample failing
with exit 201. This author verified the retained source identity, not that run.

The real-health leaf-lock positive case remains byte-identical. The active
successor saturation case changes only the synthetic server's explicit
restricted-handler configuration, explanatory profile comments, and stronger
success counts (three TLS connections, one health handler). Its original real
lock barriers, one-second health-success assertion, pending slow requests,
failure diagnostics and post-release assertions remain. Its health response
still comes from the actual restricted API, not a fabricated health result.
This synthetic transport holder is not evidence that two production API
handlers reached the same real chain-validation stage. No intentionally red
case remains active or is silently excluded: a separately named unconfigured
generic case now asserts the expected refusal honestly.

Eleven new enabled cases are registered, with no old registrations removed:

| Suite | Controls |
| --- | --- |
| `flowmesh_https_tests` | Generic unconfigured refusal; 24 precisely unloaded submit-body ordinary-dispatch controls; same-TLS health→submit busy/close→fresh health; fourteen synthetic suspended owners plus two active ordinary owners; slow reserve plus full queue/connection refusal and recovery; small-reply profile/fallback bounds; unchanged HTTP bounds/TLS pin authentication; ordinary resumer priority; Stop with a partial reserved body and clean restart |
| `flowmesh_api_health_tests` | Actual API reserved strict decoded-envelope rejection/valid reordered health; shared reserve/ordinary four-per-peer health budget and exactly-once ordinary read budget at28 |

The shared-budget case must complete inside its one preselected one-second
window; crossing it fails as insufficient evidence, rather than skipping or
retrying a counterexample. The unloaded control waits for zero active/resuming
ordinary owners before each synthetic submit body; it proves dispatch routing,
not production signing/execution or arbitrary concurrent burst admission.
The fourteen sleepers are a physical transport-capacity model, not a fresh
qualification of every action-wait/watch economic state. Existing wait/watch,
TLS and ordinary-read cases are unchanged. The selected native count must be
discovered from the actual compiled source, not inferred from an earlier460.

Independent review caught a generic configured-profile edge before any build:
the existing 64-byte minimum reply cap could not fit the explicit busy body,
and a generic out-of-bounds fallback dropped its close flag. The correction
requires a configured reserve's cap to fit that minimal body and makes every
restricted invalid/oversized reply fall back to bounded busy/close. Generic
unconfigured 64-byte profiles remain valid; production API's 24 MiB cap is
unaffected. The new small-bound regression proves both boundaries.

## Retained first build failure

The first root build, execution 57441, exited 2 before any tests. An
unterminated raw-string literal in the new rejection-input vector swallowed
the next test boundary and produced misleading missing-variable/name-collision
compiler diagnostics. The first test source `5adbe84c…` and failure evidence
are retained. The final source differs by exactly one independently reviewed
line: valid health JSON plus explicit `" trailing"`, preserving that malformed
input and the separate `not-json` case. No assertion, budget, production code,
redundant declaration or rename changed. Successor build 70621 exited 0.

Ordinary test binary SHA256:
`b2fa60f95d76fc5019362ab86fefbac5f966b511e8abc656a9ebb5f47d3000ec`.
Ordinary daemon SHA256:
`fefac5e330d61a3631ab3e8f6caca5dfc19a8dd8bb9127932390c2d611f4c812`.
The freeze includes committed aggregate/publication recovery and excludes
unqualified Qt work and the unrelated V2 prototype. Root's complete native
v4 selection passed 473 discovered/executed cases across 27 suites, every child
exit 0 and no selected skips. Independent reviewers performed source checks
and lightweight CI mocks, not native execution. This is ordinary AppleClang17
Release C++20/macOS, not native Linux, lock-order or TSan execution.

## Explicit availability limits and remaining gate

The stage addresses health access when ordinary handlers occupy both permits,
subject to an available reserve, admitted queue/connection slot, existing API
budgets and completion within the original deadline. A sole reserved worker
can itself be occupied by slow TLS/body input. Full admission rejects before
classification; no new protected health connection slot exists. Health still
uses existing leaf mutexes/cache construction, so bounded data is not a hard
wall-clock scheduling guarantee. The refusal/recovery controls deliberately
exercise these limits; no DDoS/full-max availability claim is made.

Personally performed checks: scoped whitespace/source inspection, hashes,
private patch applicability and in-memory scope simulation, retained original
case-byte comparisons, and enabled-case inventory comparison. All old cases
are byte-identical except the explicitly configured synthetic successor.
No build/test/daemon/network/commit was run by the implementation author.
Independent source review and root's authorized API application are complete;
actual bounded native qualification passed as recorded above. This does not
establish health availability when the declared admission/deadline/budget
conditions fail.
No WAN, performance, Qt, 200 ms, reliability, power-loss, protocol or release
qualification follows from this frozen local candidate.
