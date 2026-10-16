# Native passive Catalog transport isolation

Date: 2026-10-16. Profile: `native-catalog-isolation-v1`.
Status: bounded source review PASS, six targeted native controls, complete
native selection and linked Qt suite passed locally.
This changes a client read path, not consensus, wire format, quorum, fees,
signed instructions, journal schema or production defaults.

## Reproducer and minimal correction

The actual test-first control held a real loopback TLS `markets` reply.
Local-only ActionStatus and a deliberately unsigned action's local refusal
both waited on the passive discovery transport. Its child exited 201 with
two failed assertions; the later exact request counts, unchanged genesis,
empty outbox and drainage assertions passed. It used client `7ec1034f` and
test translation unit
`84fc9bdb95f9399ef128f6f53a23fa35d3b35410a78f93c3de755fd9a2137b0d`.
BEFORE binary SHA256:
`0dafc5c5995baa5402d6a7e5130b5ba76262687ee4d6fb973e2d6e61daaf815b`.
That exact original case body remains in the successor, not a weaker
accepted/refused timing predicate.

Catalog now has one CAS owner acquired before constructing request/body
state and retained through reply validation, application and unwinding.
There is no caller or body queue. A second Markets/Reconnect returns an
explicit read-only busy result, never a fabricated empty success.
Catalog uses its own persistent TLS client. Its annotated Bitcoin `Mutex`,
`TRY_LOCK` and `GUARDED_BY` HTTPS owner are released before taking the client
work gate to apply the reply. Negative lock-entry contracts on ReadMarkets,
Connect, Markets and Reconnect describe actual lane ownership only; they
do not claim that internal chain locks are forbidden or removed.

Connect persists the existing trust/configuration selection under work
before its optional reachability read. A busy or superseded optional probe
cannot turn successfully persisted configuration into configuration failure.
Reconnect refuses a busy Catalog before resetting anything or incrementing
attempts. With ownership, it retains fresh TLS/current trust and explicit
cooldown bypass, and checks only bounded shape and unique nonzero market IDs.
It does not refresh accounts, parse labels, read actions or send an instruction.

## Bounds, fences and publication

There are at most eight sequential endpoints, each with the unchanged
five-second request deadline. This is not a global five-second operation or
shutdown guarantee. One serialized reply is at most 24 MiB; its parsed DOM,
up to 256 staged rows and bounded metadata candidates consume additional
heap memory. The serialized bound is not a total-memory measurement.
One additional retained idle TLS socket consumes existing connection
capacity, not an HTTPS worker/permit; idle/age/request limits are unchanged.
An early private claim that idle keepalive consumed workers was corrected
before compilation. No worker-starvation conclusion depends on it.

Endpoint values, trust/configuration epoch, preference and endpoint-observation
revision are copied under work and rechecked before applying a reply.
The configured CA file's content is rechecked after parsing, outside work,
including same-path replacement with restored mtime. This is not filesystem
ABA exclusion or an atomic administrator-change lease. All local pins/status,
unique identities, metadata candidates and display rows are staged before
publishing any rowset's capability/readiness/label observation. Invalid cosmetic
metadata remains nonfatal and never grants account authority. Labels name the
actual responding endpoint, not a stale selected URL.

Discovery remains paused/unverified until selected-account proof succeeds.
It cannot refresh certificate/response clocks, account cursors, highwater,
actions or restart-state obligations. Whole-rowset validation does not promise
a power-loss transaction or strong exception guarantee for all volatile
observations under allocator failure. Local Pins/SyncIndexes can still acquire
`cs_main` during application while the work gate is held; this stage isolates
transport, not every chain-index wait.

## Preserved controls and source-review history

All 158 original HTTPS translation-unit cases remain, with only four narrowly
reviewed transport-premise adaptations: Catalog uses a separate TLS session;
two carried/reconnect controls add one genuine nonwaited status read to warm
the ordinary action lane; cold waited Submit now truthfully reports no borrowed
ordinary connection and one TLS handshake. Original exact request/action bytes,
certification, durable markers, reuse/hand-back and no-resend assertions remain.
The separately committed transient-BUDGET case is preserved byte-for-byte.

The private v2 patch was generated from immutable source copies after an
invalid live-left-side patch accidentally included reverse BUDGET hunks.
The invalid patch was retained but never applied. Narrow corrected patches
first produced exact reviewed rebases `d742fa7b` / `6c22a214`; the subsequent
root delta adds only the annotated lane/entry contracts and one test path
conversion. Separate read-only reviews inspected both deltas; reviewers did
not build or execute native tests.

The first build exited 2: production source compiled, but the new fixture used
the repository's deliberately deleted path/string append overload. Its two
async diagnostics were cascading errors; no test executed. The retained
fixture successor wraps the same ASCII generated leaf in `fs::PathFromString`.
No assertion, generated identity or protocol behavior changes.

## Actual targeted qualification

The corrected ordinary AppleClang 17 Release C++20 native, daemon, Qt workspace
and wallet targets built successfully. Frozen source SHA256 values:

- Client: `bff8dac180fae191bf256c41c061b6a82a5ba97b412f7b57501c65e3790f5f1e`.
- HTTPS tests: `9865dfd54893226899da984fe0adc78b487743cc9af23ee269e6e312bdd18ba6`.
- Native executable: `8c6164da311b159816a0849b71f8874e7d4a9b0a5b91116d80d57eb3f5b5eb96`.

Six separately selected actual native children exited 0; detailed XML contains
exactly each requested passed case. Unselected tests are not counted. They cover:

1. Original held TLS versus local status, unsigned local refusal and health.
2. Single owner, immediate competing read/busy reconnect and genuine empty success.
3. Stale same/other endpoint Connect and newer endpoint-action observation.
4. Same-path generated CA replacement, refusal and healthy recovery.
5. Whole-rowset invalid/duplicate/oversized/bad-domain refusal with no partial labels.
6. Fresh counted shape-only reconnect, fallback and exact unchanged outbox bytes.

The six-child run completed in 3.165 seconds; this is functional testing, not
a trading latency measurement. The complete linked offscreen Qt workspace
also exited 0: **362 passes, zero failures, three existing opt-in visual skips,
48.545 seconds**. Its executable SHA256 is
`14c0d01f4fd5e59796f67ddef806bef053346475919de4b3edee31fe067b00bf`.
The full native host selection and this Qt integration campaign ran concurrently;
their elapsed times are not controlled before/after performance comparisons.
The exact version 8 native host selection subsequently passed **495 cases in
28 suites**, every actual child exit 0, no selected skips, 226.365 seconds.
It used that same preserved native executable and includes the four adapted
transport controls and all six outcome-evidence cases. Its manifest checker
SHA256 is `79b89e352436eddf6db28969e3c4004822b13f4f0db2e71ce572bbf7e3fd85f1`.
This is ordinary macOS execution, not Linux/TSan or remote CI.

Generated TLS keys, wallets, databases, journals and raw directories remain
private disposable evidence. The preserved earlier failures remain failures.

## Boundaries

Qt `ReadMarket` still repeats Catalog discovery during approved-command
preflight. A held Catalog can therefore produce explicit busy instead of
permitting a real foreground order. Neither this native control's local
unsigned refusal nor Qt's offline preflight counter proves a successful signed
trade during held discovery. No stale label or cached availability replaces
the foreground proof gate.

The unchanged action, wait/watch, generic Call, verified-return durability,
evidence budget and aggregate-save paths retain their rules. Legacy plain
locks remain; the new annotated lane does not imply all FlowMesh locking is
compiler checked. No Linux/DEBUG_LOCKORDER/TSan, Windows, WAN, power-loss,
arbitrary-load, live recovery, release or 200 ms qualification is claimed.
