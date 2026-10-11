# Regtest validator catch up repair

A validator with an intact journal can miss several certified fresh-lineage
epochs while its old locked checkpoint remains on the active chain. This
repair lets it follow the included handovers one epoch at a time without
erasing its actual last vote. It does not recover an orphaned lock or introduce
a new finality protocol.

## Reproduced failure

Baseline `760888dfb5ec95e54e6672706603b2ae1b39380c` plus the unchanged
[retained counterexample](bridge-v2-counterexamples/two-epoch-signer-restart.patch)
failed again: exit 201, 627 of 629 assertions passed, two failed. The returning
weight-1 validator could not issue its epoch-2 checkpoint-137 vote even though
its epoch-0 lock was still canonical. The other three validators held 33 of 34
stake weight and three of four seats and had genuinely certified the intervening
epochs. This was not a missing-quorum or orphaned-lock case.

The error was `checkpoint does not follow the durable validator-set lineage`.
The old staging signer never advanced a canonical old lock; fresh-key mode
instead allowed a later-epoch jump without preserving each intermediate step.

## Recovery authority and durability

[Fresh lineage state](../../src/node/finality_fresh_lineage.h) retains one exact
accepted statement per completed epoch, its signing-set hash and the carrier
block height and hash. Only certificate application after consensus judgement
creates this evidence. Rotation archives the current-epoch statement before
changing committee pointers. A separate latest statement retains delayed
previous-epoch certificates without rewriting the handover which authorized
rotation. Rebuilds and reorgs derive the same evidence from connected blocks.

The [signer](../../src/node/finality_signature.cpp) checks the old checkpoint is
canonical and matches the historical epoch and committee lineage. Each step
checks checkpoint grid, epoch, exact checkpoint and carrier hashes on the
active chain, and the predecessor's committed successor. The final step must
match the exact newest included certificate. Missing or inconsistent evidence
refuses voting; a certificate learned from a peer without validated chain
inclusion is not sufficient.

Retained statements are trusted internal derived state, not an imported or
unauthenticated recovery file. Application already checks every statement field
and its BLS certificate. Catch-up rechecks ancestry, epochs, grid and committee
continuity; it does not repeat historical BLS verification or detect arbitrary
corruption of process memory. The corruption tests target the named missing,
carrier and committee inconsistencies, not all possible memory faults.

The [journal](../../src/node/finality_signer_store.cpp) independently requires
exactly the next epoch, a strictly higher checkpoint and the previous lock's
exact successor set. It reuses the existing predecessor check and atomic
write, file flush, rename and directory flush path. Catch-up changes only
`lock_*`; every `last_signed_*` field survives unchanged. A restart resumes
from the durably stored lock, not from the epoch of the last actual vote.

There are at most 16 catch-up writes per signer pass. An incomplete pass emits
no votes and reports a transient catch-up state until a subsequent ordinary
pass. Missing evidence is also a transient safe refusal; persistence failure
retains the existing permanent local signing fence. No timer, quorum, vote,
signature-domain, certificate-placement or on-chain acceptance rule changes.

## Scope and limits

The existing staging gate restricts this signer path to explicit regtest fresh
lineage configuration. Mainnet and testnet parameters, V1 journals and the
separate exact-same-epoch and committee orphan-lock recovery rule are unchanged.
No live validators, wallets, signing journals, bridge contracts or fork height
are changed.

History is immutable and shared between projections, copied when an epoch
rotates, and directly indexed for catch-up. Its size grows with certified
epochs. The 16-write limit bounds the local catch-up walk, not total memory,
historical-state maintenance, filesystem latency or arbitrary-load availability.

This is not full PoS V2, BFT view-change recovery, recovery of permanent split
locks, or proof of machine-power-loss safety. It cannot create a quorum when
too many validators are offline. A coherently restored old journal is not
detectable without separate trusted freshness evidence. Existing exclusive
signer/datadir ownership assumptions still apply; a stale sequentially reopened
writer test is not a proof against simultaneous independent signers. None of
these gaps is removed by deleting journals or lowering quorum.

## Reproduction commands

Use a clean checkout containing the repair and a native test build. Do not
apply the old counterexample patch again; its case is now part of the suite.

```sh
cmake -S . -B build-native -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_GUI=OFF -DBUILD_TESTS=ON -DENABLE_WALLET=ON
cmake --build build-native -j6 --target test_bitcoin
build-native/bin/test_bitcoin \
  --run_test=finality_fresh_lineage_tests \
  --report_level=detailed
build-native/bin/test_bitcoin \
  --run_test=finality_fresh_key_tests,finality_fresh_transport_tests \
  --report_level=detailed
env RANDOM_CTX_SEED=0000000000000000000000000000000000000000000000000000000000000011 \
  build-native/bin/test_bitcoin \
  --run_test=finality_signer_store_tests,finality_fresh_params_tests,finality_signing_policy_tests \
  --report_level=detailed
build-native/bin/test_bitcoin \
  --run_test=finality_signature_tests/durable_signer_survives_restart_locks_forks_and_rebases_only_on_included_certificate \
  --report_level=detailed
```

## Qualification status

Code and regression target:
`23a07dd7ef3a08a2d309bb41bd378a90ca59fc01`, following implementation commit
`b6519b151115f1d2ae09a8b2341058dec722c0dc`. Only local commits were made on
`flowmeshV2-dev` in this task; no publication or deployment was performed.

The native macOS arm64 RelWithDebInfo build used AppleClang 17 with wallet
support and GUI disabled. It reused a clean scratch checkout of the baseline
with the nine modified code/test files mirrored exactly. `git diff` against
the frozen target over all `src` and the top-level build file was empty;
generated version metadata still names the baseline dirty checkout, not a
clean checkout of the frozen target. The tested executable SHA-256 is
`27613fc90afda26815c144917cba72258736a314efa17fdef6a8a4c5cae35718`.
Unrelated unfinished market-model and build-file edits were excluded and
preserved in the development worktree.

| Completed native run | Cases | Assertions | Exit |
| --- | ---: | ---: | ---: |
| Signer store, fresh params and signing policy | 28 | 681 | 0 |
| Full fresh lineage suite | 12 | 7278 | 0 |
| Full fresh-key and transport suites | 33 | 71218 | 0 |
| V1 durable restart and orphan-lock control | 1 | 281 | 0 |

All 74 selected distinct cases passed, with 79458 assertions. This is targeted
qualification, not an execution of all 2400 available native cases.

The new outage case also passed separately with 4240 assertions, and the
existing fresh-journal helper case passed separately with 32. These duplicate
runs are not additional distinct test cases. The 28-case group passed again
with the explicit seed above. The broader combined V1-signature/key/transport
attempt was interrupted with exit 130 and is not counted as a pass; the
selected V1 safety control replaced the broad unchanged-V1 campaign.

A separate read-only implementation and test-design review of the frozen
target found no blocking defect within this bounded regtest scope. It did not
execute tests; the execution results above are separate evidence. The completed
verdict is **PASS WITHIN THE BOUNDED REGTEST RECOVERY SCOPE**, not deployment
approval. During the long fresh-key run, a bounded stack sample confirmed its
slow legacy proof-of-work fixture setup; this was not a performance benchmark.

These are native connected-chain fixtures and actual filesystem journal
reopens in the same OS process, not a deployed tester network, child-process
kill campaign, machine-power-loss test or full-node release qualification. A
passing regression is not production deployment approval or completion of all
validator recovery work.
