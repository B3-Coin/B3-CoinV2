# C1: clean-core infrastructure result

Date: 2026-10-16. Branch: `flowmeshV2-dev`.
Verdict: **PASS WITHIN DECLARED FOUNDATION SCOPE; STOP FOR MILESTONE REVIEW**.
No publication or deployment.

Exact tested source: `1f05fc748c1c74ad7afff4e63ab21dc81bfb8c88`.
Architecture contract began at `aed0f449`; implementation begins `1e7fc498`.
Later documentation commits do not change the qualified implementation.

## Implemented and executed

Fresh disposable source export/build, followed by the reviewed incremental
corrections below: AppleClang 17.0.0.17000013, C++20, RelWithDebInfo,
wallet/GUI/daemon/CLI/IPC disabled. Unchanged dependencies were reused for the
final build; this was not a second full clean rebuild. Only the isolated target
was built; the final link contains Bitcoin utility/consensus/crypto dependencies,
not `bitcoin_node`, wallet or Qt.

| Check actually executed | Result |
| --- | --- |
| Fresh configuration without the new option | Option OFF; no foundation target |
| Final target compilation, unchanged strict lock-warning flags | Exit 0 |
| CTest with no-empty-discovery guard and 60 s child deadline | 1 executable passed, 19 internal cases, no skips |
| Direct executable with exact 19-name output validation | Actual child exit 0; 12 mailbox + 7 read-slot |
| Correct mailbox source, same compile flags, syntax-only control | Actual compiler exit 0 |
| Disposable copy removing only `LOCK` in the real `Stats()` function | Actual compiler exit 1; three required-lock errors and no unrelated error |

Binary SHA-256:
`bbcf47b199d42bce70f8fbf5e676a0d534243e83d1cac9ae1dd15227c3bfb90c`.
CTest runtime was approximately 0.74 seconds for the whole fixture, **not** an
order/certification/UI latency measurement.

The annotation control was run against `2af654a9` core source. The final
`1f05fc74` changes only test-worker ownership; mailbox source/header and compiler
flags are identical. The initial negative compilation had unrelated positive-
source errors and is retained as **invalid control**, not passing evidence.

## What the cases establish

Mailbox: exact count/byte edges, empty-body count, FIFO, rejected originals
unchanged, accepted-buffer ownership, idempotent close with exact drain,
independent capacity, maximum configured counters without giant allocation,
fixed-seed 512-operation reference replay, concurrent producers/consumers and
close races retaining every accepted message.

Read slot: invalid/empty configuration, exact payload edge, deep ownership,
one claimed head per immutable envelope, permit exhaustion/reuse, moved leases,
leases surviving slot destruction, 1,000 publications with bounded retained
reader ownership and concurrent coherent publication (four readers with
2,000 fixed iterations each).

Scope details: concurrent mailbox producers and consumers run in separate
phases, not simultaneous push/pop. Publication-counter saturation at UINT64_MAX
and subsequent refusal are source-reviewed, not dynamically exercised. Empty or invalid
proof/parent claims are not validated by this opaque transport primitive.

## Findings and preserved corrections

Pre-execution source review found and corrected old-view deallocation under the
publisher lock, truncated/narrow test identities, partial thread-launch cleanup
and missing mailbox inventory enforcement.

Actual builds then found three portability/compiler-contract defects:

1. `1e7fc498` build exit 2: Bitcoin's actual `Mutex` requires negative capability,
   not only `LOCKS_EXCLUDED`. `d34f18e9` adopts the existing Bitcoin thread-pool
   annotation pattern; no warning suppression or runtime change.
2. `d34f18e9` build exit 2: current macOS libc++ lacks the
   `atomic<shared_ptr>` specialization. `2af654a9` uses standard shared-pointer
   atomic free functions, preserving ordering and outside-lock retirement.
3. `2af654a9` build exit 2: this libc++ lacks `std::jthread`.
   `1f05fc74` uses joined `std::thread` ownership and a launch/drain guard;
   operation counts and assertions are unchanged.

All revisions and failed logs are preserved. The final positive build/tests are
not inferred from source review. Independent bounded read-only source reviews
covered components/tests and each correction; reviewers did **not** execute
these tests. The primary session executed the commands above. This is not an
external security audit or GitHub CI run.

## Limits and next gate

No accounting, matching, authentication, BFT, signer/store, custody, PoS V2,
bridge, futures, client/Qt integration, physical durability, regtest or 200 ms
qualification is supplied by C1. No mainnet/VPS/test-network data was touched.
No GitHub push, PR, workflow run, tag or release.

Atomic shared-pointer internals and permit contention can wait/starve; this is
not a formal lock-free or hard real-time guarantee. Payload length bounds do not
bound total heap, spare vector capacity or concurrent caller allocation.
Publication is not proof, freshness, readiness or a disk barrier.

The earlier Qt repair at `863a30cd` remains parked: strict p95 **503.645 ms**
failed its 50 ms target. It is not qualified by these unrelated passing tests.

**Next bounded milestone C2:** implement the shared spot application in the new
C++ core, with two stable-quoted markets sharing one exact AssetId. Differential
replay uses the frozen accounting model as oracle, retaining provisional fee,
order and risk profile labels. No new model is substituted for native code and
no production economic rule is silently selected. Stop here for review before C2.

Private build/log/command/hash evidence is retained outside Git in the
Development `operations/redesign-milestones-20261016/c1-clean-core.GxqCtk`
directory. It contains no live wallet/key/network material. Historical Qt
failure evidence is in the sibling `m1-qt-switch.NnYJYN` directory.
