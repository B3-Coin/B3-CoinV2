# One V2 development branch — Milestone 2.1 publication

Ongoing branch: **`flowmeshV2-dev`**. No pull request is required or authorized
for this handoff. Make logical local commits and publish completed milestones
to this branch only. Preserved model/design branches are frozen reference
history, not additional ongoing development lanes.

## History-preserving integration

The existing local `flowmeshV2-dev` tip was
`0b930e303e4c2c6bc28beb3bf656488b636ad49d`, an ancestor of the tested milestone
`10e00a47563809c83d28756ef005c325a9988641`. Integration fast-forwarded the actual
existing branch through all 28 prerequisite and milestone commits. No
cherry-picks, duplicate implementations, squash, reset or rewritten history.
The original dirty developer checkout was not switched or altered; integration
uses an isolated worktree of that same branch.

The frozen accounting milestone `9d4fd53dc7d58d397680fd64065663703cbbdd17`,
agreement milestones, R1 and all failing-before traces remain ancestors.
The two model directories and both CI runner files are identical to `10e00a4`.
The only successor changes are this publication document and the literal
`flowmeshV2-dev` push trigger in the dedicated model workflow. There is no
production source, model implementation, test assertion or test-profile change.

Verify the exact equivalence from any checkout of this integration commit:

```sh
git merge-base --is-ancestor 10e00a47563809c83d28756ef005c325a9988641 HEAD
git diff --exit-code 10e00a47563809c83d28756ef005c325a9988641 HEAD -- \
  test/flowmesh_v2_model test/flowmesh_v2_agreement \
  ci/run_flowmesh_models.py ci/test_flowmesh_models_runner.py
```

## Reproducible qualification

The exact `10e00a4` committed-tree export passed **237 tests** with Python
3.14.6: 113 accounting, 116 agreement/recovery and 8 runner/discovery checks.
The corresponding command is unchanged:

```sh
python3.14 -B ci/run_flowmesh_models.py
```

The dedicated [model workflow](../../.github/workflows/flowmesh-models.yml)
now runs on pushes to `flowmeshV2-dev`, not only PRs or historical model branch
names. It retains read-only contents permission, checkout credential
non-persistence, bounded execution, fixed seeds and discovery/rejection guards.
There is no service exposure, secret dependency or deployment step. Existing
wallet/build/release workflows are unchanged; no tag or manual release trigger
is part of this task.

A committed workflow is not a hosted pass. Report the actual final remote
commit and workflow run independently. If the branch is published but the run
has not executed, the status is **Published; remote CI pending.** No unrelated
wallet/Qt campaign or performance qualification is implied by these model tests.

## Direct-commit external review map

Review the full published 40-character commit, not a moving branch tip. Replace
`REVISION` in `https://github.com/B3-Coin/B3-CoinV2/blob/REVISION/` with that
commit, followed by the repository paths linked below:

- [DATA admission and bounded references](../../test/flowmesh_v2_agreement/fm_admission.py).
- [Remote/local OFFER separation, durable signing and restart](../../test/flowmesh_v2_agreement/fm_replica.py).
- [Bounded retry, status discovery and requested catch-up](../../test/flowmesh_v2_agreement/fm_delivery.py).
- [Atomic model-memory updates](../../test/flowmesh_v2_agreement/fm_memory.py).
- [Independent checker](../../test/flowmesh_v2_agreement/fm_checker.py).
- [Admission regressions](../../test/flowmesh_v2_agreement/test_admission.py),
  [retry regressions](../../test/flowmesh_v2_agreement/test_retry_bounds.py),
  [restart regressions](../../test/flowmesh_v2_agreement/test_publication_recovery.py).
- [Protocol/profile](../../test/flowmesh_v2_agreement/TEST_PROFILE.json),
  [milestone explanation](../../test/flowmesh_v2_agreement/MILESTONE_2_1.md),
  [preserved findings and review gap](../../test/flowmesh_v2_agreement/MILESTONE_2_1_REVIEW.md).
- [Test runner and reproducible instructions](../../ci/run_flowmesh_models.py).

## Review gap remains open

The separate Milestone 2.1 review reported the remote-OFFER retention bypass
but did not complete. Its tool terminated, and the denial was not bypassed.
The reproduced failure and successor repair are preserved. In particular, the
final remote/local offer separation and `intent_bodies` durability still need
a completed independent review. Local tests and GitHub CI are not a security
audit; this publication task does not close that gap.

Scope remains fixed synthetic membership, authentication, anchor evidence and
modelled stable memory. No real-storage, cryptographic or transport integration,
live V1 lock recovery, membership/PoS V2/bridge activation, futures liquidation,
or 200 ms/WAN qualification. Stop for review; do not start Milestone 3.
