# Regtest recovery from a retained historical certificate

A returning validator can hold a vote on an orphaned branch while the active
chain has already included a newer certificate from the exact same epoch and
signing committee. Later epochs can hide that recovery authority from a signer
which examines only the newest certificate. This bounded repair consults an
already-retained rotation handover; it does not invent authority to resolve a
split which has no qualifying certificate.

This follows the separate [canonical missed-epoch catch-up repair](finality-sequential-catchup-regtest.md).
Neither slice qualifies the whole V2 release.

## Failure sequence

On baseline `7c45b5a8f35f2ed573b79d892ba4cce6ac008363`, validator B signs
checkpoint G+5 in epoch 0 on branch X. The active chain switches to branch Y
before a certificate is included on X. A, C and D hold 33 of 34 stake weight
and three of four seats; they genuinely certify Y's G+10 in epoch 0 and then
certify later epochs. B's old vote remains in its durable journal.

The signer checks only the newest, later-epoch certificate and refuses because
its epoch differs from B's lock. The exact accepted epoch-0 rotation handover
is still available through `FreshLineageState::HandoverForEpoch(0)` but is not
consulted. The unchanged regression on the regression-only revision below
exited 201: 819 of 826 assertions passed and seven failed. The anchor stayed at
G+5 rather than G+10, later catch-up never advanced and no resumed vote was
issued. The failure reports the existing exact-epoch/committee refusal, not a
fixture or certificate-construction error.

## Existing authority and bounded correction

The existing newest-certificate recovery path is preserved. Only when newer
finality is in a later epoch does the fallback perform one direct historical
lookup for the orphan lock's exact epoch. The retained certificate must have:

- The same lineage, epoch and signing-set hash as the old lock.
- A strictly higher checkpoint, on the configured checkpoint grid and within
  that epoch, with the exact checkpoint index and canonical block hash.
- Its exact carrier block still on the active chain, after the checkpoint.
- The successor committed by the active chain's validated lineage.

Only consensus-validated certificate application creates these internal
records. A peer's assertion, timeout, online-peer count or successor committee's
certificate supplies no substitute authority. These checks do not authenticate
arbitrary imported records or detect general process-memory corruption.

The existing `CommitCertifiedAnchor` operation persists the same-epoch anchor
with its predecessor check and file and directory flushes. It preserves every
`last_signed_*` field. The canonical certificate supplies the successor; the old
branch's successor need not match it, just as in the existing recovery rule.

That first pass returns a transient no-vote state after the write. A subsequent
ordinary signer pass performs the existing sequential, at-most-16-step catch-up.
It cannot issue a multi-epoch vote before those steps are durable. No checkpoint
acceptance rule, quorum, signature domain, timing policy or journal format is
changed. V1 signing and the regtest-only fresh-lineage gate remain unchanged.

## Scope and remaining blockers

The fallback can use only a qualifying retained rotation handover. It does not
recover every historically included certificate. In particular, an archive at
G+5 cannot release a G+10 orphan lock even if a delayed canonical G+15 certificate
once existed after rotation but is no longer retained. Refusal in that case
remains intentional; recovering additional authenticated historical evidence is
a separate bounded task.

Without a newer same-epoch and same-committee certificate, a true split lock
still refuses. Later committees may be disjoint and old signatures may remain
usable externally; a newer committee's vote cannot silently revoke them. Full
native commitment, view-change and membership recovery remains a separate
protocol and activation gate.

The age-only IBD restart trap is also separate: the staking loop blocks all
work after a restart on a stale tip. Its guarded block-production exception
must retain the finality-signing IBD gate and establish actual synchronization
evidence. This repair makes no networking, global IBD or staking-policy change.

No VPS update, mainnet operation, activation height, wallet action or journal
deletion is part of this slice. Same-process journal reopening is not a child
process kill test or machine-power-loss qualification. Existing exclusive signer
ownership and external freshness requirements still apply.

## Qualification

The regression-only revision is
`47e45a1ad7849c1d4f6c32117a9cbf81c5e7e5d8`. The implementation target is
`01cb294a7791b769b33b56c932bddac61cc0f0b0`. These are separate local commits on
`flowmeshV2-dev`, following the preserved baseline above; no history was rewritten.

The new case is
`finality_fresh_lineage_tests/staging_orphan_recovers_from_archived_same_epoch_authority`.
It uses genuine BLS votes on X, real block invalidation and replacement to form
Y, and genuinely connected A/C/D certificates. A second isolated journal scenario
has an X/G+15 lock; Y's only epoch-0 authority at G+10 is genuinely insufficient.
These are alternative test histories, not simultaneous production signers.

The case also damages only internal derived evidence to test rejection of
missing history, wrong or missing carrier, signing committee, lineage, epoch,
checkpoint hash, successor, height/grid and index. It inspects the first durable
anchor-only write, reopens the journal, interrupts the later catch-up by removing
required derived evidence, reopens again and checks one resumed vote without a
duplicate. The original orphan vote remains unchanged until a legitimate later
vote is actually issued.

From a clean checkout with the native build dependencies installed, build and
reproduce the selected case with:

```sh
cmake -S . -B build-native -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_GUI=OFF -DBUILD_TESTS=ON -DENABLE_WALLET=ON
cmake --build build-native -j6 --target test_bitcoin b3coind
env RANDOM_CTX_SEED=0000000000000000000000000000000000000000000000000000000000000012 \
  build-native/bin/test_bitcoin \
  --run_test=finality_fresh_lineage_tests/staging_orphan_recovers_from_archived_same_epoch_authority \
  --report_level=detailed
```

For the qualification groups, use that same fixed seed and executable with
each of these selectors separately:

```text
--run_test=finality_fresh_lineage_tests
--run_test=finality_signer_store_tests,finality_fresh_params_tests,finality_signing_policy_tests
--run_test=finality_fresh_key_tests/fresh_journal_lineage_catch_up
```

The macOS arm64 RelWithDebInfo build uses AppleClang 17, wallet support enabled
and GUI disabled. It reuses the existing scratch build with source mirrored to
the frozen target; `git diff` against that target over `src` and the top-level
build file is empty. Generated version metadata still identifies the baseline
dirty scratch checkout, not a clean checkout of the target. Both the native test
executable and daemon build successfully. Test executable SHA-256:
`a9514b67457dda988ae416bfca082e65e7e4e1ad63cbcf5b0ccb75971750bf28`.

| Completed native run | Cases | Assertions | Exit |
| --- | ---: | ---: | ---: |
| Signer store, fresh parameters and signing policy | 28 | 681 | 0 |
| Fresh-key journal catch-up control | 1 | 32 | 0 |
| Full fresh-lineage suite, including the new regression | 13 | 8111 | 0 |

All 42 selected distinct cases passed, with 8824 assertions. These runs use
the same fixed seed as the command above. The new regression passed all 833
assertions, including insufficient-proof refusal, damaged derived evidence,
anchor-only persistence, journal reopening, interrupted sequential catch-up
and one resumed vote without duplication. Its before/after result is separate
from the 74-case qualification of the preceding canonical catch-up slice;
unchanged broader suites were not rerun or added to this turn's totals.

The source/test target remained frozen at the implementation revision above
throughout qualification. The report is recorded in a later documentation-only
commit; it does not change the tested source. The native daemon SHA-256 is
`e49413cb335e66de1ddbcc5c5997a7019169b3a140b90218cef611fcc76bffe4`.

A separate read-only review of the exact frozen source and regression found no
new concrete safety defect within this bounded scope. It inspected revision
diffs and whitespace checks but did not execute native tests. Its code-review
verdict is separate from the execution results and is not deployment approval.

No full native, Qt, WAN, contract or performance campaign is claimed. The
unfinished market-model and build-list changes in the development worktree are
preserved and excluded from both this repair and its scratch build.
