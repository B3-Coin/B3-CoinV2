# Foreground read-admission diagnostics

This is a measurement-only successor to the bounded account-read admission
stage at `6da690f180ff7319bfaaca452b88e1d9cfe388e4`. It does not change
agreement, signing, admission, freshness, TLS, execution, fees or durable writes.
The [admission policy](b3-flowmesh-bounded-client-read-admission-20261016.md)
and [retained failed trading controls](b3-flowmesh-post-isolation-diagnostics-20261016.md)
remain the applicable context. This is not 200 ms qualification.

## Capture contract

The diagnostic profile is `client-read-admission-timing-v1`. Existing bounded
in-memory capture is default OFF. A new `client_read_admission` span is created
only for a direct authenticated FOREGROUND_ACCOUNT read when capture is ON
and BENCH debug logging is OFF at entry. For an ordinary capture campaign,
BENCH must stay OFF through completion, drain and capture stop: the inherited
emitter could log synchronously if that category is enabled later. The entry
guard alone is not a global no-logging guarantee.

The original request start, reservation requested and reservation acquired
timestamps use the same process-local monotonic microsecond clock. The
requested-to-acquired interval measures the reservation call, including its
mutex, FIFO wait, scheduling and return overhead; it is not pure sleeping,
transport, proof verification or total RPC latency. A missing acquired marker
requires a complete capture and the outer RPC error to classify refusal.
An admitted read may fail later while still having an acquired marker.

Public market/account identities, policy and join flags provide correlation;
no signed payload, key, wallet content or credential is added. Existing
32,768-event and 16 MiB serialized-byte limits and drop/error accounting are
unchanged. These limits do not measure total allocator heap use.

The optional span is declared before the admission lease. The lease and inner
work/lane/chain scopes unwind before emission. This order is a source-level
finding, not a claim that all unlock instants are measured. Watch and passive
IMMEDIATE paths emit no new admission span. An optimistic preflight join that
never enters the reservation path likewise has no new span.

## Test-first evidence

The prospective two-test fixture is pinned by SHA256
`17b7a96302fd7ef2725017cf52780509d9e7c729e03c6792b60be1608ac65a34`.
All 156 earlier native HTTPS case bodies are preserved. Against the previous
client, the positive test failed after all actors drained: zero new admission
spans instead of three. The negative control passed. Actual child exit was
201, with 1,499 passing and one failing assertion; this was the expected
missing measurement, not a changed trading-rule counterexample.

The successor client is SHA256
`7ec1034fbbd2079878d055aef52704471dae1912f2b1ef35ad718f2169b91755`;
the admission header remains
`7333dff18859e25e6fac6a5b2eb33e084250d45a9be6cc9ab00987c67b243f56`.
The first successor run failed during local HTTPS bind setup before either
control. That failure is retained. With normal loopback approval, the exact
same binary passed both controls: actual exit zero, 1,649 passing assertions.

The positive control holds a genuine passive TLS snapshot and queues a
different authenticated account. It checks original age, marker ordering,
parent/thread/account correlation, an admitted local Pins failure and a
healthy successor. Passive and Watch controls remain immediate. There are
exactly three snapshots, no action/submit/update traffic, unchanged genesis
and an empty outbox. All actors drain before capture stops; drops/errors are
zero. Recorded descendant chain-release events were zero, so this execution
does not supply chain-unlock instrumentation coverage.

The negative control checks default-OFF capture and a separate deliberate
BENCH-ON entry/drain case. Legacy/root events confirm that capture really is
ON in the latter, but no new admission span is created. It does not claim
legacy synchronous logging is disabled in that deliberate negative profile.

## Qualification status and limits

The full affected native host selection passed: 483 cases in all 27 selected
suites, every actual child exit zero, no selected skips, 219.058 seconds. The
linked offline Qt workspace passed 356 checks, no failures, three existing
opt-in visual-export skips, 52.188 seconds. Discovery and detailed XML were
checked using the unchanged v5 manifest; these are macOS host results, not
Linux CI or sanitizer qualification. Native binary SHA256 is
`f8a979c85dcbf933c2e087df7964dcbee713e6533c6ef5cf705ebcc11d58db31`;
daemon SHA256 is
`c8e4c86b22ad0d6191d4b8bb5fcdf67991ed4fe4b3159d5af97172a6eff165a8`.

The first build command returned two only because its final wallet target
was misspelled `bitcoin-qt`, after the native/test targets completed. The
correct `b3coin-qt` and workspace targets then completed with exit zero.
That command error and the failed listener setup remain retained separately.
The real backend FULL/EXPIRED diagnostic mappings and all possible exception
schedules have not been executed by these two controls.
The earlier pure admission tests remain separate evidence for queue rules.
No Linux, TSan, Windows, WAN, power-loss or production qualification is implied.

Reproduce the focused controls from a fresh native test build with generated
test data, using the prefix
`flowmesh_client_watch_lane_tests/client_read_admission_trace_*`, detailed XML
and a hard external deadline. The [native guardrails](b3-flowmesh-native-linux-guardrails-20261016.md)
describe discovery and rejection checks; a macOS selection is not a Linux
sanitizer run. Raw generated fixture directories remain private.
