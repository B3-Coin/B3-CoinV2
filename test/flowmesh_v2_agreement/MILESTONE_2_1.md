# Milestone 2.1 — adversarial admission, bounded retries, model CI

Isolated, synthetic model only. No production integration, live signer, V1
recovery, quorum change, economic change or performance qualification.
Reviewed baseline: `0247001a04588fccbadf63bb9d4a8704dcaf5b2c`.
The accounting dependency and R1 remain frozen at the revisions in
[TEST_PROFILE.json](TEST_PROFILE.json). Publication is **branch only; no PR**.

## Response to the three external findings

1. **Unsolicited DATA could durably halt signing.** The old DATA handler checked
   only ValueId, then `_store_body` admitted the object before checking shape,
   context, relevance or application validity. Object 257 raised
   `BODY_CACHE_LIMIT`; `pump` made that a durable halt. Restart emptied the
   disposable cache but correctly preserved the halt, leaving the attack's
   effect permanent. The unchanged 256-object bound is not the repair.
2. **Retry work grew with historical decisions.** `retry` walked every durable
   record and rebroadcast every completed certificate. Its `_resume_intents`
   path could additionally deep-copy the entire journal through `_persist`.
   A send-count limit alone would miss that second cost.
3. **Neither full model suite was explicitly qualified by CI.** The dedicated
   [model workflow](../../.github/workflows/flowmesh-models.yml) now invokes
   [one runner](../../ci/run_flowmesh_models.py) for discovery guards and both
   complete suites. Workflow configuration is not evidence of hosted execution;
   actual clean-tree/hosted results are recorded separately at closeout.

## Profile boundary

`flowmesh-v2-single-sequence-pbft-test/2` adds admission/delivery limits and
non-voting envelopes. The profile participates in configuration identity, so
`/1` and `/2` messages are not silently interchangeable. Old failure fixtures
remain `/1`, replayed against their stated historical revisions. No fixtures,
issued signatures or journals are converted to `/2`. A fresh synthetic run
starts with the new profile; supported restart preserves that run's own state.

PBFT-style phase meanings, PREPARE/COMMIT quorums, matching NEW_VIEW guards,
highest-prepared selection, anchor predicates, view deadlines, accounting
ordering and exact-application rules are unchanged. This is not HotStuff.

## Admission rules

`receive` bounds the encoded wire at the existing 2 MiB limit before enqueueing.
For a body, DATA then verifies its hash, exact structure, canonical batch,
instance and anchor shape. It requires a current tracked request or a fully
validated relevant PROPOSE/NEW_VIEW/PreparedQC/CommitQC reference. An individual
PREPARE or COMMIT vote does not make an arbitrary hash a data dependency.

Proposal and certificate retries may include the exact reference with their
body. Thus valid inline data does not require an earlier fetch round trip.
The receiver performs the existing nonmutating application/ancestry preview
before admitting a body to its validated cache. Missing ancestry defers the
body under bounded pending rules, not as validated cached data. No screening
operation credits deposits or executes the ledger.

References, outstanding requests and pending wires are each capped at 32.
Only two alternate references per `(sequence, view, proof-kind)` slot are
retained; this bounds authenticated Byzantine hash advertising. Disposable
reference/request leases are 30 abstract ticks. Repeated valid proof delivery
revalidates relevance and can renew a request; an expired unreferenced reply
does not itself renew authority. Strong validated quorum evidence can displace
disposable reference metadata when the global table is full. Existing durable
obligations do not depend on lease survival.

Unknown future-parent traffic is not retained as an unbounded pending proof.
It prompts bounded discovery of the receiver's own next missing certificate;
the sender must retry once preceding history is applied. No peer-supplied
integer advances local sequence, view or quorum.

Refusals have precise trace reasons, including `DATA_BODY_SHAPE`, `BODY_HASH`,
`WIRE_BYTES`, `UNREQUESTED_BODY_DATA`, `DATA_REFERENCE_LINK`,
`REFERENCE_SLOT_PRESSURE`, `REQUEST_PRESSURE`, `PENDING_PRESSURE`,
`INBOX_PRESSURE` and `MISSING_PARENT_NOT_RETAINED`. Exhausting verification work
on an incoming reference refuses that input, rather than durably halting the
signer. Genuine failure to retain indispensable state remains a safety stop.

## What may be discarded, and what may not

| State | Protection and lifetime |
| --- | --- |
| Unsolicited/invalid bodies | Never enter the validated cache |
| Body/anchor caches, inbox, pending/reference/request metadata | Bounded disposable state; refuse, expire or evict only unprotected entries |
| Current accepted proposal/body; prepared proof; exact signing intent; accepted NEW_VIEW; known decision | Durable records remain authoritative; referenced current bodies and their available ancestry are protected from cache eviction |
| Locally offered exact work | Retained durably under its original instance and protected while pending; never rebased into a replacement action |
| Historical decisions, bodies, issued votes, application markers | Not pruned by this repair; indexed retrieval serves catch-up, while the independent checker can still inspect every issued vote |

After advancing to the next applied sequence, the previous body's *cache*
protection may end; its durable record does not disappear. On restart, the
current record restores accepted/prepared headers and exact bodies. Lost
volatile requests/references are rebuilt through unfinished-intent checks,
proof retries and history discovery. Missing ancestry is fetched again before
first signing or application. Already-issued signatures are retransmitted
exactly, not regenerated with another value/view/sequence.

`fm_memory.update_durable` copies only touched records into a private overlay;
successful mutation publishes the changed entries atomically in a synchronous
simulator step. Exceptions before that boundary publish no changed entries.
The ordinary historical dictionaries remain intact and unpruned. This assumes
the same modelled stable-memory atomicity as before; it is not physical disk,
concurrent storage, power-loss or rollback protection. Aliases to the durable
maps observe a successful update, not a persistent historical snapshot.

## Bounded delivery schedule

`STATUS(config, next_sequence)` is a non-voting hint. A node behind it requests
its **own next** missing record with `GET_CERT(config, sequence)`. Source,
configuration and numeric bounds are checked. One coalesced history request
per configured peer is serviced fairly. Replies carry the existing full
certificate/body and pass the unchanged proof, context, ancestry and execution
checks. Neither message releases a lock, signs, certifies or applies anything.

Each retry interval reserves service for one requested historical record and
small missing-data/discovery messages before current proof traffic can use
the budget. It then retries current exact signatures, rotating older current-
instance slots and accepted bodies, a bounded sample of offers, and the most
recent commitment. Pending work is moved to the inbox without copying all its
payloads. A refused send is observable and leaves required durable evidence
available; unserved history requests remain queued.

| Per-retry-interval cap | Value |
| --- | ---: |
| Durable records selected | 4 |
| Scheduler objects selected/processed | 96 |
| Scheduled messages, including per-peer fanout | 256 |
| Encoded payload bytes | 33,554,432 |
| Scheduled payload bytes, including fanout | 33,554,432 |
| Current-instance signature slots sampled | 12 |
| Accepted views sampled | 4 |
| Offers / exact requests / pending wires sampled | 4 / 8 / 8 |

Counters are charged before history lookup/selection; encoding reserves a
conservative envelope before work, and fanout is counted before scheduling.
Repeated calls at the same simulated clock share the interval's budget.
Nested sends while resuming intents share it too. Normal fresh-message
handlers are not suppressed by a preceding retry's exhausted budget.

Tests substitute a history dictionary that raises on iteration, and exercise
the actual interrupted-intent retry path as well as the helper. Current-record
copying is bounded by views/proofs, not completed-sequence count. Current
application preview, ancestry checks and fixed-size admission-table scans are
separate subordinate costs, not hidden full-history scans. The counters are
**scheduler work units**, not every Python operation or wall-clock CPU time.

For one queued historical fetch plus idle-current/recent dissemination:

| Retained decisions | Records | Scheduler objects | Fanout messages | Encoded bytes | Fanout payload bytes |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 3 | 25 | 14 | 17,993 | 45,125 |
| 4 | 3 | 25 | 14 | 17,993 | 45,125 |
| 16 | 3 | 25 | 14 | 18,009 | 45,189 |
| 31 | 3 | 25 | 14 | 18,009 | 45,189 |

The old 31-certificate unconditional rebroadcast becomes one most-recent
broadcast plus any separately requested history response. A restarted laggard
eight sequences behind catches up without a new client action. Requested
history remains serviced during active traffic; tests also saturate byte and
same-tick budgets. Network loss after scheduling still requires later retry:
admission or publication is not receipt or durable application.

Remaining scaling limits: N=4/7, 8 views, 32 sequences, 256 objects, bounded
proofs/events and finite campaigns. Restart reconstruction, ledger preview,
full independent checking and test-harness event selection can grow with their
respective retained state. No guarantee under unlimited ingress, exhausted
genuine safety storage or permanent quorum loss. No 200 ms, WAN or
5,000-validator production-readiness claim.

## Regressions and reproducibility

Run from a clean checkout using Python 3.14, standard library only:

```sh
python3.14 -B ci/run_flowmesh_models.py
```

The runner discovers every test module, preserves per-file minimums, compares
declared methods with collected tests, rejects skips/expected failures, and
isolates suite imports. It fixes `PYTHONHASHSEED=0`; campaign seeds remain the
declared ones in the two profiles. Per-process timeouts are 30 seconds for
runner guards and 300 seconds per model; hosted job limit is 15 minutes.
Routine CI includes all original hidden/late-certificate, view-change, restart,
checker-negative-control and bounded fault campaigns.

The exact 257-object regression is frozen in
`48b684a0ed154d8681e861eb68ff84526ed7fc9d`. Its test body is unchanged in the
repair. [Before DATA trace](evidence/before-unsolicited-data.json): N=4 and N=7,
256 cached bodies then durable `BODY_CACHE_LIMIT`; after restart zero cached
bodies but the same halt. No accounting change or issued vote. The repaired
test requires zero admitted junk, no halt, identical durable state and a later
valid decision after restart.

The retry-growth assertion is frozen in
`8e1cc9652b48c7d2350b8557e4eaf2415290c944`.
[Before retry trace](evidence/before-history-retry.json) records 31 genuine
decisions followed by failure of the at-most-two-certificate retry assertion.
To reproduce either failing baseline, check out its revision in an isolated
directory and use its retained runner (expected exit 1):

```sh
python3 -B test/flowmesh_v2_agreement/replay_case.py --source . \
  --revision 48b684a0ed154d8681e861eb68ff84526ed7fc9d \
  --case test_admission.AdmissionTests.test_exact_257_unsolicited_invalid_bodies_do_not_halt_signing \
  --output /tmp/m21-before-data.json
python3 -B test/flowmesh_v2_agreement/replay_case.py --source . \
  --revision 8e1cc9652b48c7d2350b8557e4eaf2415290c944 \
  --case test_retry_bounds.RetryBoundsTests.test_retry_does_not_rebroadcast_all_retained_decisions \
  --output /tmp/m21-before-retry.json
```

Do not run both against one checkout and merely change the revision label:
each requires the actual indicated source. The current runner can replay the
same case names against the successor, using its actual revision and separate
output files. Earlier failures remain failures, not overwritten green traces.

New regression coverage includes plausible unrequested bodies, wrong hashes,
malformed/oversized data, duplicates, forged/irrelevant references, bounded
Byzantine advertisements, expired/refreshed requests, missing ancestry,
inline proposals, nonmutating invalid application preview, prepared/COMMIT
evidence under pressure, restart and delayed data after view change. Genuine
resource-stop, hidden-certificate and exactly-once tests are retained.

Failed attempts retained during development:

- First full intermediate agreement run: 98 tests, one failure. The old
  `authenticated_missing_data_exhaustion_halts_without_erasure` expected a
  permanent `PENDING_LIMIT` halt from Byzantine future hashes. Profile `/2`
  deliberately replaces that policy. Its successor asserts bounded refusal,
  unchanged exact obligations through restart, then progress. The genuine
  `AUTH_AUDIT_LIMIT` stop regression is unchanged.
- First expanded admission-only run: 12 passes, one fixture failure. The
  harness had already provided every anchor, so the intended missing-ancestry
  path was not reached. The fixture now uses supported restart to lose only
  volatile ancestry, then tests actual deferral and recovery; all 13 pass.

## Qualification and review record

Working-tree and final clean-tree tests, the one focused fresh-context internal
review, any successor repair, publication identity and hosted CI evidence must
be distinguished. See the separate closeout record added after those checks;
this document does not predeclare their result.

Publication must preserve all frozen ancestors. Only this dedicated model
branch is authorized; no PR, tag, merge, production branch or deployment.
After this milestone, stop for external review before real storage,
cryptography or transport integration. Future economics, custody/bridge,
changing membership, live V1 locks and actual PoS V2 remain outside this model.
