# Milestone 2 publication review and successor repair

This record supplements, rather than overwrites, [RESULTS.md](RESULTS.md).
Owner authorization covers this dedicated model branch and a **draft** PR,
not production integration, merge, release, activation or deployment.

## Preserved revisions and review boundary

- Original frozen Milestone 2: `81107bd82ffaec63472bc0825be0d58e48c64e6f`.
- Unchanged Milestone 1 dependency / PR base:
  `model/flowmesh-v2-accounting-test1`,
  `9d4fd53dc7d58d397680fd64065663703cbbdd17`.
- Preserved R1: `2b32645e26f4ba41de8aa746bd04e84192fb3972`.
- Test profile: [flowmesh-v2-single-sequence-pbft-test/1](TEST_PROFILE.json),
  the documented [single-sequence PBFT-style model](PROTOCOL.md), not HotStuff.

The earlier internal review was followed by a refused follow-up. That refusal
remains part of the historical record; its tests were not an independent
post-fix review. The owner subsequently requested **one** new bounded review.
A separate available context completed that read-only review of exact frozen
`81107bd`, without editing files or receiving a tool refusal. No alternative
route was used to continue the previously refused review operation.

The prior review did not record a single fully reviewed Git tree covering its
concurrent implementation/test work. Consequently this review conservatively
covered **all changes `a22b663..81107bd`**, not an invented prior approval SHA.
The post-finding implementation fixes that needed review were:

| Commit | Change and interaction reviewed |
| --- | --- |
| `d458ea5df6bff805af43d8da9f486f12cbdd8d9b` | Resume an already committed, unapplied decision only after recovering exact anchor evidence |
| `95c27f958ee92ca3efc417bc59e25e96706cdaf6` | Restore retained offers and proposal/prepared headers; enforce exact accepted NEW_VIEW; authenticate before pending retention; bounded exhaustion; record historical signing guards and initial synthetic audit anchors |
| `0b59a514616cce4fde8fd409168f42af13c0c775` | Disseminate/retry a sole nonleader offer; preserve intents on local/restart exhaustion; independently check temporal signing/publication and multi-height ancestry |
| `bb2b71b`, `5fc4b67`, `1cb2dc7`, `81107bd` | Adversarial regressions, explicit two-anchor competition, declared assumptions and sanitized counterexample evidence; included conservatively rather than assumed previously reviewed |

This covers matching NEW_VIEW/voting prerequisites, pre-signature evidence,
hidden/late certificates, anchor ancestry, restored work, bounded resources
without erasure, sparse/nonleader progress, and exactly-once application.

## New finding: first signature before local evidence recovery

**P2 / demonstrated profile discrepancy, not demonstrated equivocation.**
The frozen `fm_replica.py` `_resume_intents` path (lines 126–148) finished an
unsigned PREPARE or COMMIT immediately on restart. A prior genuine proposal,
body and (for COMMIT) PreparedQC had already been validated and persisted, but
the volatile anchor ancestry was lost. Local `_valid_body` still raised
`NeedData`, while restart created/published the first signature and left no
evidence request. This contradicts the existing missing-data transition in
[PROTOCOL.md](PROTOCOL.md). No forged journal or conflicting commitment was
needed or demonstrated.

The frozen checker accepted this schedule: it checked historical ancestry
against external audit evidence but did not establish its availability at the
signing replica. Global knowledge cannot establish that local precondition.

The parent reproduced the finding and expanded the same restart path to all
four candidate phases: PROPOSE, PREPARE, COMMIT and NEW_VIEW.

| Revision | Result |
| --- | --- |
| `11c9c87b67210b5d5ed26ec1131c013bc6bb6479` | Test-only successor of frozen 81107bd; all four candidate-phase subcases fail; missing-evidence retry and superseded-view tests also fail; existing-signature retransmission passes |
| `1642c4e55f47bf8923d609392cffd5be54f06fb3` | Model/checker repair, protocol clarification and retained failure evidence; all four recovery methods pass; checker suite has 35 passing methods |

[before-restart-unsigned-anchor.json](evidence/before-restart-unsigned-anchor.json)
retains the actual `success:false` output, four simulations, 134 events and all
issued synthetic messages. It contains no wallet, private key or runtime data.

### Small repair

- Preserve the exact durable unfinished intent. Before issuing its first
  candidate signature, revalidate that same body against local anchor evidence.
- Request exact missing data; resume through DATA handling and the existing
  bounded retry interval. Do not change timers, quorum, signed payloads or views.
- Recheck existing current-view/mode/proposal/prepared predicates when resuming.
  Evidence arriving after abandonment never authorizes a fresh old-view vote.
- Retransmit already-issued signatures byte-for-byte; do not create another
  signature merely because the local cache was lost.
- Snapshot bounded local body/anchor evidence in the test-only `pre_sign` guard.
  Independently walk those copied links, without global-cache fallback or the
  implementation's body validator. A later cache cannot repair an earlier
  missing-evidence signature. The unchanged agreed anchor is already durable.

Four new recovery methods cover all candidate phases, no-data retry, abandonment,
and exact already-signed retransmission. Four new checker methods cover absent
candidate/intermediate headers, wrong local header, missing local body, and the
already-agreed-anchor case. Fair recovery then applies exactly once everywhere.

### Other probe: alternate proof subset

A valid transferable PreparedQC may contain a different valid NEW_VIEW report
subset for the same I/view/ValueId. The review did **not** classify that alone
as a defect: R1 C3 binds votes to this tuple, and the local accepted proposal
and NEW_VIEW remain unchanged. No new byte-identical transferable-QC restriction
was added. Direct substitution of an already accepted proposal's NEW_VIEW
continues to be rejected by the existing guard and regression.

### Review status, without an endless loop

The separate read-only reviewer ran all **79 frozen agreement tests**, exit 0,
52.733 seconds, and completed the requested review. It identified the above gap.
The successor repair and new tests received implementation self-review and the
independent checker implementation, **not a second fresh-context audit**.
They remain visible for draft-PR review. Automated passes do not substitute for
that remaining source-review boundary. This is internal AI-assisted review,
not an external security audit or formal protocol proof.

## Reproduction

New clean-tree qualification used a full `git archive` export of exact repair
commit `1642c4e55f47bf8923d609392cffd5be54f06fb3`, with no working-tree overlays:

| Suite | Result | Host runtime | Process exit |
| --- | --- | --- | --- |
| Agreement, including all new regressions | 87 methods PASS | 56.546 seconds | 0 |
| Unchanged Milestone 1 accounting | 113 methods PASS | 65.801 seconds | 0 |

Only publication documentation follows that tested implementation revision.
The model source, tests, profile, accounting dependency and R1 are unchanged
by those documentation commits. These host runtimes are not latency results.

Python 3.14.6, standard library only. From a clean checkout of this branch:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s test/flowmesh_v2_agreement -p 'test_*.py' -v
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s test/flowmesh_v2_model -p 'test_*.py' -v
```

The focused post-publication-review regression:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s test/flowmesh_v2_agreement -p test_publication_recovery.py -v
```

For the retained before-fix result, use a separate clean checkout at
`11c9c87b67210b5d5ed26ec1131c013bc6bb6479` and run that focused command. Its
nonzero exit is expected. Do not change the current checkout or frozen history
to reproduce it. All required R1/accounting source is in branch ancestry; no
developer-local snapshots or private evidence directories are prerequisites.

## Publication checks and explicit limits

The approved repository is `https://github.com/B3-Coin/B3-CoinV2`.
Publication is only `model/flowmesh-v2-agreement-test1`, without force or tags.
The PR base is the published M1 model branch, keeping the review diff separate
from V1 client work while preserving its full dependency history. No squash,
rewrite, production-branch push, release or deployment is authorized here.

Checked-in workflow inspection: `ci.yml` runs on PR/manual dispatch;
`release-build.yml` runs on manual dispatch or `v*` tag pushes, and its publish
job requires a tag-push event plus qualification/build gates. This branch-only
push does not trigger a checked-in deployment. The draft PR can run normal CI;
model test results do not predict the outcome of that separate broad CI.

This model uses **fixed synthetic membership, synthetic authentication and
anchor evidence, modeled durable memory, bounded schedules and fault campaigns**.
Its 48 enumerated splits and 16 seeded campaigns are not arbitrary-schedule
proofs. Timers are abstract ticks; test duration is not certification latency.

It does **not** qualify live V1 lock recovery, real cryptographic integration,
physical disk/power-loss recovery, undetectable restored-old signing backups,
changing membership/mainnet bootstrap, stake-weighted PoS V2, unchanged
bridge-verifier compatibility, futures margin/liquidation, or 200 ms/WAN
performance. Existing accounting fee-dust/reward-rounding assumptions remain
provisional and unchanged. No live wallet, key, validator or contract was used.

Proposed next boundary, **not authorization**: an isolated storage-boundary
harness for explicit journal encoding, interrupted-write/reopen behavior,
exact-message replay and freshness fencing, still fixed-membership and with
no production networking/node wiring. Undetectable rollback needs an explicit
external trust/fencing assumption; a disk-format test cannot solve it.
