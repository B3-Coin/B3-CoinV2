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
detectable without separate trusted freshness evidence. None of these gaps is
removed by deleting journals or lowering quorum.

## Reproduction commands

Use a clean checkout containing the repair and a native test build. Do not
apply the old counterexample patch again; its case is now part of the suite.

```sh
cmake --build build-native -j6 --target test_bitcoin
build-native/bin/test_bitcoin \
  --run_test=finality_fresh_lineage_tests/staging_signer_restarts_after_two_certified_epochs \
  --report_level=detailed
build-native/bin/test_bitcoin \
  --run_test=finality_signer_store_tests,finality_fresh_key_tests,finality_signature_tests,finality_fresh_params_tests,finality_fresh_transport_tests \
  --report_level=detailed
```

## Qualification status

Repair qualification and independent implementation review are pending. A
passing regression is not production deployment approval or completion of all
validator recovery work.
