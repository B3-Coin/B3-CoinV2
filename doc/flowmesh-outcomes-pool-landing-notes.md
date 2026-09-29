# FlowMesh verified outcomes and pool admission: landing notes

These notes are for whoever lands branch `feat/flowmesh-hl-learnings-claude`.
They cover the outcomes-and-pool work (7f8d83b to 690f572) and the review
follow-ups made after it. They record what the branch history cannot fix,
what must be run by hand before landing, and the measured client load
against the tighter public API budgets.

## Commit 17d8cfc does not configure

Commit 17d8cfc ("flowmesh: add outcome evidence codec, verifier and
derivation") adds `flowmesh_action_pool_tests.cpp` to
`add_executable(test_bitcoin ...)` in `src/test/CMakeLists.txt`. That file
first appears in the next commit, 8027ac5 ("flowmesh: move the runtime action
pool into its own header"), whose message says so.

At exactly 17d8cfc the CMake generate step fails, so no target builds, not
only `test_bitcoin`:

```
CMake Error at src/test/CMakeLists.txt:7 (add_executable):
  Cannot find source file:
    flowmesh_action_pool_tests.cpp
CMake Error at src/test/CMakeLists.txt:7 (add_executable):
  No SOURCES given to target: test_bitcoin
CMake Generate step failed.
```

This was reproduced on 2026-09-29 by configuring a `git archive 17d8cfc`
export. From 8027ac5 on, the tree configures again.

The branch rules forbid rewriting history, so the commit stays as it is.

- **Bisecting:** use `git bisect skip 17d8cfc`. Its change is complete once
  8027ac5 adds the missing source file.
- **Landing with a squash or rebase:** move the
  `flowmesh_action_pool_tests.cpp` line from 17d8cfc into 8027ac5. Then build
  at least `b3coind b3coin-cli b3coin-wallet test_bitcoin` at every commit
  before landing. Builds were verified at HEAD and at each review follow-up
  commit, not at every earlier commit.

## Functional campaigns to run by hand

`feature_flowmesh_action_outcome.py` and `feature_flowmesh_pool_admission.py`
are registered in `NON_SCRIPTS`, next to the other opt-in latency-fixture
campaigns, so `test_runner.py` never runs them. They are the only functional
coverage of:

- lost-reply resubmission;
- proven not-executed across a client restart;
- the local sequence bound;
- the public API budgets;
- the read-rate gate below.

Run both before landing, each with its own temporary directory and port seed:

```
python3 test/functional/feature_flowmesh_action_outcome.py --configfile=build/test/config.ini
python3 test/functional/feature_flowmesh_pool_admission.py --configfile=build/test/config.ini
```

`feature_flowmesh_pool_admission.py` writes its measurements to
`flowmesh-pool-admission.json` in the test's temporary directory. Pass
`--nocleanup` to keep it.

## Read-rate gate (design I2) and 429 risk

The per-identity budgets are 28 reads, 16 writes and 32 in total per second.
The identity is an IPv4 address or an IPv6 /64. Opt-in outcome replies cost
one extra token per evidence part, and a waited submission costs one extra
token for its embedded read.

`feature_flowmesh_pool_admission.py` now drives the engine-off client like
the Qt trading panel, for 30 s in each of its two modes:

- **500 ms market poll:** one auxiliary phase per tick.
- **Live-update watch loop:** at most two watch reads per second, with the
  auxiliary refresh every 5 s.

In both modes a trader places and cancels a resting bid every 3 s, using
waited submissions and waited receipt reads. The test counts every forwarded
request, and every extra token the replies show, at validator 0's single
identity. The peak over any one second must stay at or below 24.

Results on the final binaries (f8577d5), with nothing else running on the
machine:

| Mode | Requests in 30 s | Extra tokens | Mean per s | Peak in 1 s | 429 | Budget-refused parts |
|---|---|---|---|---|---|---|
| 500 ms poll | 160 | 42 | 6.7 | 14 | 0 | 0 |
| live watch | 113 | 39 | 4.8 | 15 | 0 | 0 |

An earlier run at 1ede0cf gave peaks of 14 and 15. It overlapped local
builds and unit tests, so it was not run on an idle machine. That commit
message's "otherwise idle" is inaccurate.

Where 429s remain possible:

- **Shared identities.** Devices behind one IPv4 NAT, or on one IPv6 /64,
  share one budget. In f8a3432 each IPv6 address had its own 32 per second.
  - Two Qt-like clients whose peaks coincide reach about 30, which exceeds
    the 28-read cap.
  - Three or more will see 429s.
  - A 429 fails that one read or delivery attempt ("Unexpected HTTPS
    response status"), like any refused request. Retained signed
    instructions are kept.
- **Unpaced scripts.** A loop calling `getflowmeshactionstatus` without a
  wait is bounded by the client's automatic status budget (16 per second)
  plus its other reads.
  - The first gate run had a test bug that left its trader loop unpaced for
    about 20 s. It peaked at 21 per second and saw three 429s.
  - That run had no idle gap after the harness's own burst in the same
    second, so those 429s cannot be attributed to the client alone.
- **Old clients.** f8a3432 clients never opt in to outcome evidence, so they
  pay no extra tokens. They cannot adapt to the lower caps either.

The budget numbers are still provisional for the owner:

- global: 128 total, 64 writes, 112 reads;
- per identity: 32 total, 16 writes, 28 reads;
- orders: 48 global, 12 per identity.

The design's other gate, a passing `feature_flowmesh_latency.py` run with
the CommitCertified p50 delta, has still not been produced for this range.

## Local notice rings

Notices now carry the origin of the submission that caused them, and each
origin has its own ring of 2,048.

- **NODE:** this node's wallet RPC and GUI. This includes the worker's
  recheck notices and the pool-drop notices for actions they queued; the
  runtime remembers their last 1,024 queued ActionIds.
- **PUBLIC:** the public trading API. This is also the default for any other
  caller.

Public traffic can no longer evict the notices this node's own not-executed
proofs need.

Inside the public ring, each requesting identity (an IPv4 address, or an
IPv6 /64, as the API admission budgets count them) has a share of 64
notices, which applies only once the ring is full. Until then every notice
is kept, so one client, or every client behind one NAT address, /64 or
TLS-passthrough load balancer, can hold all 2,048. Once the ring is full,
each new notice evicts one:

- the submitting identity's own oldest, if it already holds 64 or more;
- otherwise the oldest notice of any identity that holds more than 64;
- otherwise the oldest notice overall.

Notices whose identity is unknown count as one identity.

The identity is the client that first queued the action, so the worker's
recheck and pool-drop notices count against it too. The runtime remembers it
for at most 1,024 public ActionIds per market and 64 per identity; an
identity at 64 replaces its own oldest, so one identity cannot push
another's out either. A forgotten action's notices use the unknown share.

A public deposit refused because its outpoint has no anchored facts appends
no notice: it carries no credential, and the refusal is already in the
submit reply.

One identity therefore cannot turn the public ring over. An identity that
holds at most 64 notices loses one to other identities only once every
identity holds at most 64, which takes at least 32 other identities
flooding at once. Under that pressure, clients sharing one identity also
share its 64: a busy NAT address, /64 or load balancer then keeps only its
newest 64 notices. A remote client's evicted proof reads "unknown", which
is safe. An already-included resubmission appends no notice at all.

## Foreground latency of outcome re-proofs

`ReproveNotExecuted` and `UpgradeOutcome` make their one bounded HTTPS
request inline, while holding the client work gate. Each can be reached from
a foreground `ActionStatus`, `QueryAction` or `Send`. The request timeout is
5 s.

Neither is scheduled through the automatic poll queue. Each is charged to
the automatic status budget and runs at most once per process:

- `ReproveNotExecuted`: once per retained row;
- `UpgradeOutcome`: once per row and endpoint.

The added foreground delay is therefore at most one request timeout per such
row per process.

## Review items left unchanged

- **Decisive inclusion.** For an executed action that a Byzantine proposer
  re-includes as BAD_SEQUENCE, the opt-in 'action' reply and the local
  backend prefer the event ring's newest certified inclusion. They do not
  prefer the retained window's decisive one, so such an order can show
  "included, not executed, not final".
  - This is not a false "rejected".
  - Reproducing it needs a certified malicious entry, which the unit
    harnesses cannot produce yet.
  - The INCLUDED_BAD_SEQUENCE text no longer advises a resend, and now warns
    that a new order could execute twice.
- **Local header proof.** The worker's header proof for a local frame one or
  two slots behind still records "already certified" as POOL_REFUSED when the
  retained interval includes the action. That event reads "rejected".
  - This happens only for an action included without executing, while its
    sequence or deposit is still open.
  - The action's own certified inclusion event outranks the refusal while
    the event ring retains it. The refusal is visible only after more than
    2,048 events in two slots.
- **Service gate refusals.** A refusal that happens before any decision
  (reconciling, not running, genesis pending) still reads "rejected", as in
  f8a3432.
- **Earlier deferrals.** G5 (exact resend of an included-not-executed
  action), H1 (head-priority selection) and the G4 same-sequence tightening
  remain deferred, as recorded in the implementation notes.
