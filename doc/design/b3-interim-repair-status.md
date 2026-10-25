# B3 interim repair: finality and frozen staking, status

Status: draft for the project owner's public status update. Proposal only;
it sets no date and no activation height. Activating H1/H2 on any network
other than regtest is an owner ruling (CLAUDE.md, "Do not change consensus
or release parameters").

## 1. What is broken on mainnet

Read-only `getfinalitystatus` on Mohamed's own mainnet node:

| Field | Value |
|---|---|
| finalized height | 827,601, not moving |
| `lineage_broken` | true |
| `handover_certified` | false |
| V1 epoch | 10 (the validator set in force never changes again) |
| tip at that reading | 856,533, still advancing |

The frozen epoch-10 set has 47 members, and about 20 of them still sign.
The member count and the active count come from the task brief, not from a
`getfinalityset` reading in this session; the per-member weights were not
read. To refresh them, Mohamed can run these read-only calls himself:

```
b3coin-cli -rpcport=38647 -rpcuser=moham -stdinrpcpass getfinalitystatus
b3coin-cli -rpcport=38647 -rpcuser=moham -stdinrpcpass getfinalityset
```

These readings differ from section 2 of
`doc/design/b3-bridge-v2-finality-dependency.md`. That section expected the
last finalized checkpoint at 814,051, producers frozen at Set_3 and the
break near 826,841. Those values are marked there as hypotheses until a
read of the node (its O2). The node readings above replace them. The
mechanism the document describes is the same.

**The mechanism** (`src/node/finality_tracker.cpp`). Under the V1 rules a
new epoch starts only after a certificate for the current one: signatures
from more than 2/3 of the set's weight and more than 2/3 of its members
(`src/modern/finality_certificate.h`). If no certificate lands within the
epoch plus its extension, `lineage_broken` is set, and it is set for good.
From then on the placement check refuses every certificate as
`lineage-broken` before it looks at any signature or quorum
(`src/modern/finality_schedule.h`, `CheckCertificatePlacement`). Block
producers come from the set in force, so they freeze with it:

- a new stake is accepted on chain but never produces or earns;
- a member that has spent its stake keeps producing;
- if no member of the frozen set is online, the chain halts.

This is the "new stakes never participate" complaint. It has no separate
staking bug; the frozen producer set explains it.

## 2. What H1 and H2 change

Both exist in the tree as regtest-only knobs. No network sets
`consensus.h1`, and the knobs are refused outside regtest
(`-b3h1height`, `-b3h1escaperounds`, `-b3h2checkpoint`; they need
`-b3modernregtest -b3flowmeshtest`).

**H1, producer rotation from stake snapshots** (dependency document
section 5). From an activation height R_h:

- producer epochs start at R_h and every E blocks after, with no
  certificate needed;
- ProducerSet_0 = ProducerSet_1 = Snapshot(R_h − 1), and at each later
  epoch start the next set is the snapshot just below it, so a stake
  change takes force within E to 2E blocks;
- a snapshot is the ACTIVE stake with a live finality-key binding, the
  same rule V1 uses; a too-small snapshot carries the previous set over;
- **escape**: a block whose round is at least R_escape takes its weights
  from the parent's snapshot and restarts the schedule, so an offline
  producer set cannot halt the chain.

H1 changes only who may produce. The V1 finality tracker is unchanged and
stays broken; BLS, digests, journals, the bridge and Ethereum code are
untouched. Below R_h validity is byte for byte V1.

**H2, a hardened checkpoint** (dependency document section 5.3). A pinned
height and hash that a node refuses to reorganise below. A fresh node is
otherwise protected only to an old hardened checkpoint, so H2 is required
in the same release as H1. It adds no new mechanism: it is an entry in the
existing pin list.

**Adoption.** H1 is a hard fork. A node that upgrades more than the reorg
horizon after R_h stays on the old chain until it runs `-reindex`
(RT1 fixture B, item 7, shows this).

## 3. What is confirmed, and by which tests

All on regtest or in unit tests; nothing here touched a mainnet node except
the read-only status call above.

| Claim | Test |
|---|---|
| A broken V1 lineage keeps a newly bound stake ineligible; from R_h the new stake produces; a revoked member leaves after its residual epochs | `finality_h1_tests/broken_lineage_frozen_then_rotates` |
| A certificate signed by every member of the frozen set, valid one block before the break, is refused as `lineage-broken` after it; so no lowered threshold can unfreeze the set, while H1 lets the new stake produce at R_h | `finality_h1_tests/unanimous_certificate_after_break_refused_h1_unfreezes` (new) |
| With 47 members and 20 signing, neither the V1 rule nor a rule lowered to 2-of-3 or 1-of-2 of the members is met, whatever the weights; if the 20 hold most of the weight, the member count still refuses | `finality_certificate_tests/forty_seven_members_twenty_signing_meet_no_lowered_quorum` (new) |
| Below R_h everything is V1, bit for bit | `finality_h1_tests/below_rh_bit_identical` |
| Carry-over below the minimum set; bootstrap fallback | `finality_h1_tests/carry_over_and_bootstrap_fallback` |
| Escape restarts the schedule, also at activation and below the floor; restart and reindex reproduce it | `finality_h1_tests/escape_restarts_schedule`, `escape_at_activation`, `escape_takes_snapshot_below_floor`, `rebuild_and_reload_reproduce_escape`, `assembler_escape_round` |
| H2 pin above R_h refuses a competing branch | `finality_h1_tests/h2_pin_above_rh_refuses_competitor` |
| Knobs refused outside regtest and range checked | `finality_h1_tests/h1_knobs_refused_outside_regtest`, `h1_knobs_require_flowmesh_regtest`, `h1_knob_values_are_checked` |
| Staking, signing and carrier behaviour with a broken lineage | `finality_production_tests` (for example `regtest_carrier_rejects_previous_epoch_and_broken_lineage`) |
| Whole nodes: the frozen symptom (a bound new stake reported "not in the active validator set"), H1 unfreezing it at R_h, rotations, escape, restart, `-reindex`, a fresh sync with H2, and a late upgrader | functional `feature_b3_h1_producer_rotation.py --fixture=B` (broken lineage) and `--fixture=I` (intact lineage) |
| The later fresh lineage (finality restarted under new keys) | `finality_fresh_lineage_tests`, `finality_fresh_params_tests`, `finality_fresh_key_tests`, `finality_fresh_transport_tests` |

The frozen-epoch reproduction the task asked for already exists as RT1
fixture B. It binds a new stake (n4) after the V1 set froze, shows its
staking loop refusing ("not in the active validator set"), and shows it
producing block R_h alone once H1 starts. No second functional test was
added for the same thing.

Release-mode run logs: `~/B3-evidence/interim-repair/` on the build host,
from a Release build of this branch on 2026-10-25. Every manifest
verified.

| Run | Result |
|---|---|
| `new-cases`: the two new unit cases | both pass |
| `functional`: `feature_b3_h1_producer_rotation.py`, fixtures B and I | both pass |
| `suites`: the 26 finality suites, `legacy_transition_tests` and `transition_proof_tests` | all 28 pass |
| `mutants`: placement ignores the broken lineage; headcount lowered to two fifths | each fails its new case, so each is caught |

## 4. Why lowering the quorum does not help

Two independent reasons, each a test above:

1. The break is permanent. Once `lineage_broken` is set, placement refuses
   every certificate before the quorum check runs. A lowered threshold is
   never consulted. Even a certificate signed by every member is refused.
2. The numbers do not reach it anyway. 20 of 47 members is 42.6 %. The V1
   rule needs 32 members, a 2-of-3 rule 32, and a 1-of-2 rule 24.

Lowering the quorum would also weaken the safety of every future
certificate, and it still leaves the producer set frozen at epoch 10.

## 5. Still open

- **The activation height and date.** Owner only. Nothing in this branch
  sets either.
- **Live set data.** The member weights of the epoch-10 set and who still
  signs should be read with `getfinalityset` before the update is
  published.
- **H2's pinned block.** A reviewed recent block, chosen at release time.
- **Operator coordination.** Every producer and exchange must upgrade
  before R_h; a late upgrader needs `-reindex`.
- **Finality itself.** H1 restores block production for new stakes; it
  does not finalize anything. Finality resumes only with the fresh lineage
  (dependency document section 6) or V2, both later work.
- **The escape race.** Section 5.2 of the dependency document leaves a
  race between an escape block and a late in-force block on the same
  parent to a model check (its section 13). That check has not been run.
