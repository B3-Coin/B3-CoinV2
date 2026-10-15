# Client ordinary-read isolation

Date: 2026-10-16. This stage separates passive market HTTPS waits from the
native client's order/status work gate. It does not remove required durable
writes, change agreement or make Qt's sequential worker concurrent.
The final ordinary native, Qt workspace and unchanged crash/reopen successor
selections pass within their declared local test boundaries.

## Cause and bounded correction

The [captured native campaign](b3-flowmesh-retention-native-diagnostics-20261016.md)
found an action-status call waiting 259.148 ms for a market refresh. That refresh
spent only 4.021 ms in HTTPS dispatch and 255.717 ms in two synchronous outbox
writes. This is evidence of mixed network/durability work under one gate, not
proof that HTTPS, BLS or PBFT alone caused the overall latency.

[RemoteBackend](../../src/node/flowmesh_client.cpp) now splits ordinary snapshot
and updates reads into prepare, request and apply. Preparation captures owned
values under `m_work`; the HTTPS request and public-CA comparison run without
that gate; application reacquires it and verifies that the reply still belongs
to the captured context. No cache, pending-action or endpoint-container reference
escapes across the network operation. Watch's head-change snapshot uses the same
path rather than silently holding the work gate during its second request.

An atomic lease permits one entire ordinary-read operation, including its
received body awaiting application. A second read refuses explicitly rather
than queuing another retained body. A separate externally serialized HTTPS
session serves this lane. Its lock is released before reacquiring `m_work`.
Submission, exact-action retry and generic action/discovery paths retain their
existing semantics; this is not a general removal of all network work from
the client gate.

The original five-second deadline is **per HTTPS request**, not a new global
five-second bound. Existing endpoint count, reply size, failover and one
undecorated capability retry bounds remain. No plaintext fallback or weakened
certificate/hostname/pin validation is introduced.

## Publication and authority conditions

The reply must match the captured explicit configuration/trust generation,
unique cache revision, head and retained high-water/no-resubmit obligations.
Application rechecks local market pins, connected checkpoint, seat authority
and anchor. A global monotone cache revision prevents eviction/reinsertion ABA;
explicit Connect/Reconnect fence an in-flight old reply. Revision exhaustion
refuses the operation, preserving signing history.

Endpoint cooldown/capability bookkeeping has a separate observation revision.
An unrelated newer endpoint observation suppresses stale bookkeeping, but is
not a veto over otherwise valid certified evidence. Unknown-field withdrawal
retains its original revision through the bounded undecorated retry. A late
watch timeout cannot overwrite newer bookkeeping or shrink the next watch budget.

The [HTTPS trust stamp](../../src/node/flowmesh_https.h) identifies the configured
public CA material actually validated for the response's session. A bounded
read outside the work gate compares it with current configured material.
Default trust-store reload requires explicit reset. This is an observed
file-content check, not an atomic administrator lease or detection of arbitrary
filesystem ABA. Transport context is never an admission or consensus proof.

## Durability and preflight freshness

Snapshot verification stages the new cache, status, cursor and display data.
The **two original conditional synchronous writes remain in their original
order**: first the monotone head high-water, then any newly verified original
inclusion/no-resubmit obligations. The candidate cache is published only after
both succeed. Failure withdraws affected verified labels and any old join
permission; it preserves exact original bytes, monotone heads and in-memory
obligations. A successful first write is not rolled back. The earlier
[verified-return repair](b3-flowmesh-client-verified-return-durability-20261016.md)
continues to guard delayed verified returns.

Starting network I/O does not erase a still-valid original preflight stamp.
Foreground wallet `Market` preflight may join only under the existing exact
account/context/obligation checks and original deadlines: 50 ms completion
window and 1,000 ms observation-age limit. Waiting does not restamp or extend
either. A real failed read clears only its matching captured context; it cannot
clear a newer watch or reset. Explicit Connect/Reconnect invalidate join
permissions across at most eight volatile caches, without resetting history.
Cold or expired preflights still refuse busy; no new request coalescing or
account-scope policy is implemented.

## Preserved failing controls and source review

All native controls use generated disposable identities, wallets, TLS material
and directories. Failed runs are retained privately, not included in source.

| Control | Actual before result | Meaning |
| --- | --- | --- |
| Held snapshot and updates | Exit 201; 739 assertions passed, 4 failed | Local action status and direct submission waited on the held ordinary HTTPS read |
| Superseded watch timeout | Exit 201; 724 passed, 1 failed; 1,080 instead of 1,500 ms | Stale bookkeeping reduced a newer watch's budget |
| Tick rejection after newer advertisement | Exit 201; 723 passed, 1 failed; two instead of three tick requests | The undecorated retry laundered an old withdrawal into a newer observation revision |
| Real wallet preflight during passive HTTPS | Exit 201; 727 passed, 1 failed | An eager freshness reset unnecessarily made foreground signing preflight busy |
| Same-context failure invalidation | Exit 0; 724 assertions passed | Positive control: genuine refresh failure removes original join permission |
| Explicit Connect/Reconnect fencing | Exit 0; 739 assertions passed | Positive control: reset cannot reuse old trust-context permission |

The original held-read control executable SHA-256 is
`f6acb8a8482e678c0f66c39bc6bb650c22739ee21f607666e729cba940d6ae9c`.
The stale-bookkeeping control is
`dbc9146d302d0d25cd505ac56d152451f14af84e5a7795417100eca822ff6e88`.
The freshness and publication-reopen before executable is
`76c337cf9094d9eddeb34d6726c975734b9dbf1111a0b9f8e228606d05dc1fdc`.
These are different frozen source compositions, not one combined population.

A separate read-only source review accepted the ordinary-read successor and,
after the independently identified preflight gap, its narrow freshness
successor. Review inspected context, trust, outbox and locking interactions;
the reviewer did not run native tests. The earlier successor passed 425 exact
native cases before the final freshness correction; that run does not qualify
the final source.

Final source freeze:

- `flowmesh_client.cpp`: `e08c5c3c36fd88e074d06b22da699a8c9095307db058196bc0e4ba42f104ee74`
- `flowmesh_https.cpp`: `c6bd9dc939470968f2689b47630432ac234437ef1f736060ec88e4d29a5f7018`
- `flowmesh_https.h`: `ac2652b6456be668acb01416cf37b2e7aeb5e47b54f5ac7df98c853b5b391469`
- `flowmesh_https_tests.cpp`: `fdbf57e8939610427b0bb54bdc38e4885bbeb5502f6e4bf7bac1e3c03976dff8`

The final ordinary macOS campaign, `host-local-macos-ci-selection-v3` with
`native-guardrails-v2`, discovered and executed **428 exact cases across 26
suites**. Every suite child exited 0; there were no selected skips or missing
cases. Test executable SHA-256:
`2f2c97c544d1d5d52d989f5c82243b9ed3fe3c7f8bcb5a50919536bd4266fb28`.
Ordinary daemon SHA-256:
`3e7b20ea9928edd4a3ffe62145f076732218ff8948f63a887d5ce81f4abed8de`.
This source composition adds only the four frozen client/HTTPS files to the
committed native baseline; the separate runtime publication repair and unrelated
V2 prototype/CMake work are excluded. No crash hooks are in the ordinary build.
The 32 lightweight runner controls also pass; Linux/TSan were not executed.

The rebuilt Qt workspace executable
`ee4174d2078908a839cc0466a2f8e30131ca241975c71503e2ecd4931695e6a1`
passed **336 cases**, with zero failures and three explicitly opt-in visual
render/export skips, in 36.495 seconds; actual exit 0. It uses isolated mock
wallet/settings state. This is neither a live trading session nor proof of
independent Qt command scheduling. The ordinary Qt application also rebuilt,
SHA-256 `101a730dfee763da2b9692c3043c17bef43fd53614b2a8f7d249d2962b494f01`.

The unchanged r5 [durability fixture](../../test/functional/feature_flowmesh_client_durability.py),
SHA-256 `dd3b00be465605de08ee25de91d9712e8ffae5037817167393faab8213d570c2`,
also passed, actual exit 0, seed `24092026` and port seed `92401`. The ordinary
operators/bootstrap client used the daemon above; the generated fault client
used a separately configured daemon with SHA-256
`a83ac992d7b80fa1e6b0fb60e2495ef684dc17e8e45065fbef6d403613bc1c4b`.
Its exact original order was
`22ddf2d81fdcdbaedfbfcd4d74d3f1d07b71ecb45276c1044b8dbc9a837b5f7f`.
All seven tracked child instances matched their required exits: six zero exits
and the single intentional generated-client SIGKILL exit -9. Cleanup reported
zero errors, and the original submission thread stopped.

The fixture exercised prewrite failure, a crossing delayed original response,
successful persistence, kill/reopening, retained exact bytes/no-resubmit flags
and no extra synchronous write for an outcome-only upgrade. It did not inject
an ambiguous write or machine power loss. Positive/negative executable-string
controls confirmed the injected-failure hook in the separate fault binary and
its absence in the ordinary daemon. These are new correctness results, not
latencies combined with the earlier historical campaign.

## Reproduction and remaining boundaries

Use a fresh ordinary native build with wallet/tests enabled and no crash hooks.
The [strict native manifest](../../ci/check_flowmesh_native_guardrails.py)
requires all thirteen named ordinary-read/freshness regressions across the
watch-lane and market-view suites. Run those compiled suites, then the full
manifest; absence of a required case is a failure, not a successful empty filter.
The [durability fixture](../../test/functional/feature_flowmesh_client_durability.py)
requires its separate guarded fault build and unchanged generated crash profile.

Proof verification, local chain/index checks and synchronous persistence still
hold `m_work`. The inherited possibility of a chain change during synchronous
writes is not converted into an atomic authority lease by this stage; a full
typed chain-facts publication adapter remains a separate gate. Qt still has a
sequential trading worker, so backend isolation alone does not establish that a
button click bypasses that worker. Server read isolation, ordinary API capacity,
fixed-interval runtime servicing and aggregate snapshot persistence are separate
workstreams. This stage is not a 200 ms, WAN, Windows, machine-power-loss,
production bridge, full V2 or release qualification.
