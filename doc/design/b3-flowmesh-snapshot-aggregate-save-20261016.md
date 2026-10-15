# Aggregate snapshot journal transition — 2026-10-16

Status: separately source-reviewed aggregate-save implementation; the count
and four preselected interruption/outcome cases passed locally, as did the
unchanged golden durability regression and **460 affected native cases across
27 suites**. This is not performance, power-loss or release qualification.
The previously qualified two-write client remains the comparison control.

## Scope and source identities

Base client: `src/node/flowmesh_client.cpp`, SHA256
`e08c5c3c36fd88e074d06b22da699a8c9095307db058196bc0e4ba42f104ee74`,
committed in `c0b9e9f`; repository HEAD during preparation was
`4aa9f813319214eb0ab0a5312580cc942f272827` (CI v2).

Frozen candidate identities:

| File | SHA256 |
| --- | --- |
| `src/node/flowmesh_client.cpp` | `fb16f124b285042011a54c85783052758602251ffe604e8915da498aa84bc0c5` |
| `src/test/flowmesh_client_crash_hook.h` | `eef23e1376ea785ce7d265521ac175406461f3ba2642c4dadb9a289217062706` |
| Final `test/functional/feature_flowmesh_snapshot_atomic_faults.py` | `03c16ac621f1b0ce69c4d83e6c061fe8d55873bc917f3a6b5fb66cb98de6ec8d` |
| Unchanged count fixture `test/functional/feature_flowmesh_snapshot_atomic.py` | `1bc66a4541b6bcf35db6249aa61fcce005bf93f3cc3203dbd53ce64687c860fd` |
| Unchanged r5 fixture `test/functional/feature_flowmesh_client_durability.py` | `dd3b00be465605de08ee25de91d9712e8ffae5037817167393faab8213d570c2` |

The first fault-fixture freeze `e24894c9…` and intermediate correction
`2fb7a355…` are retained with their failed runs. Only its committed-kill test
setup/observer changed; production client and hook hashes above did not change.

No HTTPS implementation, protocol, timer, quorum, account sequence, exact
instruction, pre-send synchronous barrier, verified-return guard, freshness
window, or endpoint trust rule is changed. Other authors' runtime/Qt/CI/test
changes and unrelated `src/flowmesh/v2` are outside this edit.

## Existing measured BEFORE control

The parent executed the frozen v1 fixture against the existing two-write
fault binary. Its private evidence directory is
`/private/tmp/b3-redesign-successor.gogiQI/snapshot-atomic-before-v1`.
I inspected its `client-snapshot-atomic-v1/RESULTS.json` and
`client-durability-child-exits.json`; I did not execute that run.

The run reached the intended count assertion with actual **2**, expected
**1**, harness exit 1 and qualification false. All exact restart-field and
original-byte checks preceding that assertion passed; unchanged read and
delayed original lane each added zero syncs; exactly one original submission
and one order effect were observed. All six owned child instances exited 0,
with no cleanup errors. An expected red control is not a passing successor
qualification and is not silently retried.

## Storage-only transition and exception boundary

`InstallSnapshot` retains the same local authority, checkpoint, anchor,
high-water, market and account checks. Under the existing work lock it now:

1. Collects every matching live `Pending*` by market and semantic ActionId
   before mutation. No wholesale Pending copy/swap can lose lane identity.
2. Privately prepares the map node needed for a new market high-water. This
   avoids allocating that node after irreversible obligations are bound.
3. Within one encompassing failure guard, sets **every** matched original's
   `previously_certified`, records the own-inclusion floor, and installs the
   new head. `FlowMeshOwnCertifiedThrough::Record` is noexcept; allocation
   failure marks the floor incomplete and disables all joins.
4. Constructs the still-private Cache candidate, then runs the existing
   `Inclusion` work on all matches. Candidate copies, receipt strings and
   proof retention can throw only after all matched flags/floors/head bind,
   including matches not yet processed by Inclusion.
5. Calls the existing `SaveRestartState` **once**. Its existing one-key
   `public-client-v1` blob contains both heads and actions. Only after that
   synchronous barrier returns does cache/endpoint/tick publication proceed.

The guard extends through cache insertion, endpoint learning and tick-page
handling. On any guarded exception it resets the successful restart digest,
withdraws inclusion/outcome labels for **all** collected matches, clears
cached freshness and advances the read epoch. Flags, exact bytes, own floor
and head never roll back. The epoch fence also covers unchanged-head failure
with an absent cache and unchanged own floor: otherwise `SameReadContext`
could remain true and `OrdinaryReadCall` could walk another endpoint over an
already-bound transition. Revision exhaustion remains fail-closed.

Forced pre-send `Save()` calls and `SaveVerifiedRestartState` stay unchanged.
Successful unchanged restart state and pure outcome text still require no
extra sync. This is storage scheduling, not new authority or accounting.

## Deliberately removed crash prefix

The old implementation could durably write the newer head alone, then die
before saving newly included originals' no-resubmit markers. The aggregate
implementation deliberately removes that separately durable head-only
prefix. A prewrite failure/death can restore the **old complete blob**, with
its old head and old marker. There is no promise that a newly observed
in-memory marker survives a write that never committed. In that case the
fixture obtains fresh independent proof before relying on a new durable
marker; it does not replay the original to obtain the proof.

After an actual committed one-blob write, restart must restore the new head
and new marker together, with exact bytes and unknown verification labels.
For a genuinely ambiguous device/storage error, durable state is unknown;
memory remains binding while the process lives. Neither this fixture nor
the implementation claims arbitrary power-loss behavior or recovery from a
corrupt store. Existing unreadable/malformed journal refusal is unchanged.

## Guarded post-write seam

The separate test binary alone calls
`ClientCrashAfterJournalSyncBeforeDigest` immediately after
`m_db->Write(..., true)` returns, before the timing completion mark, digest
update, saved-row publication or snapshot cache publication. The ordinary
build has no call, guard/environment processing or added file access.

The existing exact generated-regtest path/token guard is reused. A new
fault targets an exact `(market_id, action_id)` with an already-true marker:

- `throw`: record `committed_journal_test_throw_before_digest`, then throw.
- `hold`: record `committed_journal_before_digest_held`, then wait at most
  30 seconds for the owned parent to kill the child or explicitly release.

The marker is intentionally persistent while a delayed lane/status read is
tested under the same fault. Each recorded post-write event proves an
actual successful write return; it is distinct from the existing successful
digest/save record. This is a **COMMITTED-write followed by test error/kill**
experiment, not an uncertain-write or power-loss simulation.

## Frozen functional profiles to execute after review

All profiles use the unchanged r5 generated legacy minimal genesis bootstrap:
four normal operators, one new engine-off client, no preagreement protocol
flag, no preserved inputs, one exact signed order. Each fault phase must use
a separate fresh harness directory and the separately compiled hook binary.
The inherited exact-Popen cleanup captures every owned child instance in
`client-durability-child-exits.json`, including the intentional SIGKILL and
replacement client; unexpected forced cleanup cannot pass.

| Fixture / preselected phase | Required result |
| --- | --- |
| Frozen v1 default count fixture | BEFORE red 2-vs-1; AFTER exactly 1; identical restart fields/exact original; unchanged read and already-durable delayed lane each 0 extra syncs |
| v2 `prewrite` | New aggregate root offered but 0 actual target sync returns; labels withdrawn, live marker binding, delayed old lane unverified, no endpoint walk/replay; kill/reopen exact old whole blob with unknown labels, then fresh proof durably renews marker |
| v2 `committed-throw` | Exactly 1 actual target sync return but 0 successful-digest records; snapshot refuses publication; live lane/status stay unverified and original is never replayed; restart equals last actual complete committed root and labels are unknown |
| v2 `committed-kill` | Actual full write followed by guarded hold with snapshot RPC still pending and the exact prior leaf saved view unchanged; exact client SIGKILL before digest/publication; restart restores full new blob/unknown labels and expiry cannot cause replay |
| v2 `outcome-only` | Successful one-save snapshot plus delayed lane 0 extra syncs; exact new-blob restart/unknown labels; fresh inclusion without outcome, then one actual `outcome_evidence` request upgrades outcome with 0 extra syncs |

The v2 marker is constructed completely and replaced **once**, including
fault and lane-release fields. A two-replacement draft was rejected during
independent source review because it could briefly disarm the fault while
releasing the old lane. The frozen fixture contains the correction.
The final retry accepts a still-current locally verified receipt despite
endpoint expiry, while checking exact identity, retained flags and no replay;
earlier unknown-label restart/error checks remain strict. Marker/evidence
I/O errors cannot bypass draining owned RPC threads and resetting relays;
any such cleanup error still fails qualification.

Request capture must report zero drops. Only exact signed originals already
created in this new fixture may be submitted, and every phase asserts total
original submission delta one and one order effect. The join failure fence
is source-reviewed; the fixture does not sign another instruction merely to
probe a join. Baseline v1's explicit expected-count 2 option is only a
positive control, never successor qualification.

## Actual local execution and retained fixture failures

The root worker, not the independent source reviewers, executed these cases.
All used the same ordinary operator daemon SHA256
`3e7b20ea9928edd4a3ffe62145f076732218ff8948f63a887d5ce81f4abed8de`
and aggregate fault-client SHA256
`5716ed27cd15f7b3ed61cd2aadd57bc3958ab2dc580a29dcdd83123d35b8c028`.
The ordinary aggregate daemon is
`da30d35376576a293ce7cca3f36a3cb8d2a13b9b80e8586be7cf2f5b21fe8ce6`;
its ordinary native test binary is
`a06b3084bfb9fbbadf925aa102e01532d4e75802e05d53bf699fda4d3878eafc`.
Ordinary binaries contain no crash-hook seam. The combined source freeze
includes the separately committed exact-publication recovery stage; it excludes
the concurrent unqualified HTTPS/Qt candidates and unrelated V2 prototype.

| Case / execution identity | Actual result |
| --- | --- |
| Count AFTER, session 10602 | Exit 0; exactly one sync, exact restart fields/original bytes; unchanged read and delayed lane each zero extra syncs; one original and one effect; six child exits 0 |
| Prewrite, session 87742 | Exit 0; zero target sync returns; live obligations bind; restart restores exact old whole blob, then fresh proof persists the new marker; one original; seven child exits match six 0 and intentional -9 |
| Committed throw, session 96970 | Exit 0; one target sync return, zero successful digest records; no verified label escapes the injected error; restart restores exact new whole blob; one original; seven expected child exits |
| First committed-kill fixture, session 94068 | Exit 1 **before kill**: invalid assumption that an already retained unknown original must be absent from the leaf saved view; six child exits 0, cleanup clean; not a kill-recovery pass |
| Intermediate kill fixture, session 34223 | Exit 1 **before kill**: inherited two RPC workers were occupied by two intentional holds, so the third observer timed out after 30 seconds; primary cleanup connection was poisoned; six actual child exits 0 but cleanup recorded errors, therefore not a pass |
| Corrected kill fixture, session 68979 | Exit 0; one actual sync, zero successful digest records; whole prior saved view unchanged while snapshot remains held; exact owned SIGKILL/reopen restores new whole blob, unknown labels and no-resubmit marker; one original; seven expected child exits, cleanup clean |
| Outcome only, session 74445 | Exit 0; successful one-save snapshot/restart; exactly one real outcome-evidence request, zero extra syncs for its outcome upgrade; one original; seven expected child exits, cleanup clean |
| Unchanged golden r5, session 70410 | Exit 0; original prewrite-failure, delayed-lane, save/reopen/no-resubmit and one-effect assertions pass; outcome upgrade zero extra syncs; seven expected child exits, cleanup clean |
| Ordinary native selection, session 20258 | Exit 0; all 460 discovered cases in 27 suites executed, no selected skips, every child exit 0; ordinary AppleClang17 C++20 Release/macOS, not Linux/TSan |

The corrected kill profile explicitly uses **three test RPC workers**: two
intentional held calls plus one independent two-second leaf-view observer.
Other phases keep the inherited two-worker profile. This is not a production
capacity repair. The observer checks zero HTTPS traffic and exact whole-view
equality, then rechecks that the snapshot is still held and has returned
nothing. A prior private work-lock release may legitimately have published an
unknown original; no new verified marker or receipt may escape the held save.
The independent observer's timeout cannot poison the primary cleanup proxy.
Both fixture corrections received separate read-only source review before
their successor execution. No kill, exact-byte, restart, no-resubmit,
single-submission or accounting assertion was weakened.

This qualifies the declared storage stage within the tested local scope. It
does not qualify the concurrent HTTPS/Qt candidates, bridge, native V2 product
integration, large-load reliability or the 200 ms target.

## Reproduce the declared profiles

Follow the [separate ordinary/fault build instructions](b3-flowmesh-client-verified-return-durability-20261016.md#reproduce-with-separate-ordinary-and-fault-builds).
From the review checkout, keep those builds separate and use a new, initially
nonexistent directory for each command. Run sequentially, retain failures and
never package the hook build. For example, after setting `normal_build` and
`fault_build` as in those instructions:

```sh
fixture_parent="$(mktemp -d)"
BITCOIND="$normal_build/bin/b3coind" \
BITCOINCLI="$normal_build/bin/b3coin-cli" \
python3 -B test/functional/feature_flowmesh_snapshot_atomic.py \
  --configfile="$normal_build/test/config.ini" \
  --crash-client-binary="$fault_build/bin/b3coind" \
  --tmpdir="$fixture_parent/count" --nocleanup \
  --randomseed=24092026 --portseed=92401

for phase in prewrite committed-throw committed-kill outcome-only; do
  BITCOIND="$normal_build/bin/b3coind" \
  BITCOINCLI="$normal_build/bin/b3coin-cli" \
  python3 -B test/functional/feature_flowmesh_snapshot_atomic_faults.py \
    --configfile="$normal_build/test/config.ini" \
    --crash-client-binary="$fault_build/bin/b3coind" \
    --snapshot-atomic-phase="$phase" \
    --tmpdir="$fixture_parent/$phase" --nocleanup \
    --randomseed=24092026 --portseed=92401 || exit "$?"
done
```

The local measured arms pin the ordinary operators to the prior qualified
daemon above to isolate the client change; rebuilding both binaries from the
review checkout is an integrated regression, not the historical paired arm.
Private generated identities, keys, logs and databases must not be published.

## Evidence limits / remaining work

Independent source reviewers performed scoped source/hash, AST and
whitespace/diff checks, not daemon execution. They found no blocking production
counterexample within this storage scope and caught the earlier marker-release
race. Actual execution belongs only to the root results listed above.

The inherited possibility of B3 authority changing during an existing
synchronous write is separate: this change does not add an authority lease,
and does not claim to resolve that gap. Existing post-read local checks and
all signing/certificate barriers remain unchanged. The later RefreshRead
completion/project boundary is also unchanged. No small fixture result is
a WAN, Qt, performance, 200 ms, or reliability qualification.
