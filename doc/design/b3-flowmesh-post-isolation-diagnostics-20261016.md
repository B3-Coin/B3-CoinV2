# Native post-isolation trading diagnostic

Date: 2026-10-16. Status: **FAILED AVAILABILITY DIAGNOSTIC; NOT 200 ms
QUALIFICATION**. This retained pair establishes a client admission defect,
not a consensus failure. No load, retry rule, freshness window or assertion
was changed to make it pass.

## Exact scope and provenance

Both runs used the same preserved ordinary daemon, SHA-256
`fefac5e330d61a3631ab3e8f6caca5dfc19a8dd8bb9127932390c2d611f4c812`.
The relevant native source matches
`45d02971facc5f7f04951eff33ed544826365f44`: client SHA-256
`5f717923a5b9aa394b85367a6d44d2537df3600f33f2fca825368ffd6908ea08`,
HTTPS source `464ec6dead454f2287c83b0be68fe6c73170016aa488f4a3f24a4f840f17d2d9`.
The executable was preserved before later Qt/CI commits, not relabelled as a
newly built whole-branch executable. Apple M4 Max, 16 cores, 128 GiB,
macOS 26.5.2, AppleClang 17 Release/C++20; no crash-test hooks.

[The unchanged harness](../../test/functional/feature_flowmesh_performance.py)
has SHA-256
`f344f81341aa4d4fd568ea491ff7e7f0695ff84b25440fe4baffda5e77412121`.
[The measurement profile](../../test/functional/flowmesh_native_queue_protocol.md)
is `native-queue` plus `native-result-observation-v1`, ordinary zero-wait
submission, seed 24092026, port seed 92401, RPC tracing in both arms and memory
tracing only in the second. It is one isolated matched fill, one second idle,
then eight concurrent offers across four buyer accounts and two markets.
Four validators, two HTTPS operators and one engine-off client process use
generated local regtest identities, ordinary TLS, BLS, quorum and durability.
The client wallets share one backend. This is neither eight independent
clients nor Qt/WAN/sustained-load qualification.

## Actual outcomes

| Arm | Process exit | Isolated verified result | Burst completion | Sole successful burst result |
| --- | --- | --- | --- | --- |
| Control, memory trace off | 1 | 257.849 ms | 1 of 8; 7 failures | 258.832 ms |
| Capture, memory trace on | 1 | 239.626 ms | 1 of 8; 7 failures | 285.742 ms |

All five final child-daemon exits were observed as 0 in each run; both harnesses
reported all threads stopped. RPC capture drops were zero. Every successful
sample retained its exact signed bytes and had one HTTPS submission. Every
failed offer had **no ActionId, signed-action record or submission attempt**:
the prerequisite failed before signing. Failed captured RPCs took
1.012–1.199 ms and reported `Ordinary market read already in progress`.
Failed samples remain in the denominator. No combined median, capture-overhead
estimate, burst-throughput or reliability claim follows from these two arms.

## Captured timelines

Times below are milliseconds from each original submission, independently
midpoint-aligned from native clocks. Each native-to-host midpoint has
uncertainty up to ±0.077 ms; pairwise cross-node comparisons can have combined
uncertainty up to approximately ±0.153 ms. These are not sub-microsecond
ordering guarantees. Same-node duration measurements do not add that offset
uncertainty.

| Observation | Isolated fill | Sole successful burst fill |
| --- | --- | --- |
| Client Market prerequisite completed | 7.581 | 10.618 |
| Required pre-send synchronous writes, separate durations | 6.056 and 6.324 | 5.053 and 5.568 |
| Initial queued response | 23.187 | 24.597 |
| Node 0 local admission | 22.840 | 24.253 |
| Proposer selects original | 28.373 | 29.285 |
| Node 0 agreement-decision persistence | 113.986–124.191 | 147.125–159.316 |
| Node 0 candidate-lock persistence | 125.305–138.124 | 160.452–174.622 |
| Node 0 local V1 attestation | 138.589 | 175.087 |
| Node 0 quorum certificate | 201.958 | 209.625 |
| Node 0 durable application | 212.685 | 216.659 |
| Final actual status HTTPS | 229.503–231.990 | 273.186–275.444 |
| Client restart-state synchronous write | 235.294–239.476 | 278.734–285.619 |
| Client verified applied outcome | 239.626 | 285.742 |

The later balance reads (259.118/309.410 ms) are separate observations, not
part of these verified-outcome clocks. Do not add overlapping queue, storage,
network or agreement spans.

The first two accepted remote attestations that actually contributed to node
0's quorum waited 20.140/35.101 ms in the isolated worker queue and
44.781/41.741 ms in the burst queue. Their market-lock waits were approximately
1 microsecond. These queue waits overlap. The isolated first arrival at
163.740 ms followed a worker tick spanning 163.685–183.869 ms. Burst arrivals
at 156.528/164.810 ms overlapped a decision/candidate-lock worker turn spanning
138.947–175.671 ms and later messages. This identifies worker occupancy, not
an allocation of the entire residual to BLS or PBFT.

The largest individual agreement-journal write in each corresponding round
was 18.027/22.621 ms, on nodes 3/2. Node 0's measured append write was
8.376/4.673 ms. Client `m_work` acquisition waits were at most 1 microsecond
in both original outcome windows: the earlier long-client-lock campaign's
diagnosis must not be imported into this pair.

The burst has a distinct result-observation delay. HTTPS at
213.758–216.510 ms still returned admitted/unverified. Later local RPC calls
returned explicitly coalesced admitted observations; the next actual HTTPS
started at 273.186 ms, 56.527 ms after node 0's durable application. The
existing poll scheduler's 62,500-microsecond refill interval explains budget
pacing despite the harness requesting status every 5 ms. It is not proof of
the exact time another endpoint made the result available.

## Cause and bounded next correction

[RemoteBackend](../../src/node/flowmesh_client.cpp)'s atomic `ReadLease`
immediately refuses a second ordinary read, before preparing its body.
Signing Market can only bypass it through the existing narrowly scoped fresh
join. Different accounts fail the cache account-scope check. The wallet raises
the failed Market prerequisite before signing; the harness intentionally does
not retry this untyped busy error. Seven concurrent offers therefore fail
locally rather than reaching the agreement engine.

The smallest proposed repair is explicitly bounded authenticated foreground
read admission outside `m_work`, with one owner, eight waiting caller frames,
an original-entry five-second admission deadline and no queued bodies. Passive,
accountless and Watch recovery reads retain immediate refusal. Recheck current
context and unchanged fresh-join predicates after admission. This is new
node-local scheduling policy, not permission to sign, resend, clear an
obligation, extend freshness or lower quorum. Its test-first/source review
and actual qualification are separate from this failed diagnostic.

After that repair, repeat the **unchanged** predetermined pair once. Residual
latency needs matching quorum-contributing worker/journal events and the next
eligible status-budget time, not an unmeasured protocol replacement.

## Capture and evidence limits

Node 0–4 captured 9,395/8,962/9,510/8,889/221 events, respectively;
all reported zero dropped events and zero limit markers against each
32,768-event/16 MiB bound. Offset-bound widths were 144–153 microseconds.
Per-node `memory_capture.offset_us_bounds` are authoritative; the top-level
`clock_offsets` object is empty.

Raw control/capture reports, exact per-request records, memory timing, child
exits and generated data remain private under their distinct campaign names
`post-health-result-control` and `post-health-result-captured`. Do not upload
the generated wallets, keys or databases. The root ran the pair; a separate
read-only reviewer independently traced its captures and source without
claiming personal test execution. This diagnostic does not establish physical
power-loss recovery, full V2, production custody, WAN latency or a release gate.
