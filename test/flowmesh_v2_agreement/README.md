# FlowMesh V2 Milestone 2 — isolated agreement and recovery model

**Local model for review, not production consensus or deployment.**
Branch: `model/flowmesh-v2-agreement-test1`. Frozen Milestone 1 remains unchanged
at `9d4fd53dc7d58d397680fd64065663703cbbdd17`; R1 remains unchanged at
`2b32645e26f4ba41de8aa746bd04e84192fb3972`.

The exact [protocol/transition table](PROTOCOL.md) and
[versioned profile](TEST_PROFILE.json) were committed before the transitions.
[Results and counterexamples](RESULTS.md) distinguish tested recovery from
safe stalls and later production work. No PR, push, release or activation is
part of this milestone.

## Reproduce from repository root

Python standard library only; executed using Python 3.14.6. There is no node
build, running service, wallet, key, network connection or third-party package.

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s test/flowmesh_v2_agreement -p 'test_*.py' -v
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s test/flowmesh_v2_model -p 'test_*.py' -v
```

Use a clean checkout of the milestone revision. Its parent history contains
all accounting and R1 prerequisites; no untracked snapshots are needed.
CPU duration of the Python audit is not certification latency. Simulation
deadlines are abstract ticks, never milliseconds.

## Files and trust boundaries

- `fm_application.py`: narrow nonmutating preview, exact ValueId and synthetic
  anchor check over the unchanged accounting model.
- `fm_protocol.py`: ideal authentication capabilities and proof vocabulary.
  Only the external harness/checker sees all issued votes; replicas receive
  their own signing capability and verification callback.
- `fm_replica.py`: per-replica inbox, timers, durable/volatile state, safe view
  change, replay, exact-data retrieval and exactly-once committed application.
- `fm_simulator.py`: deterministic scheduling, partitions, losses, delays,
  duplication and supported crashes. No live FlowMesh transport modification.
- `fm_checker.py`: separately written proof/ancestry and temporal-state audit.
  Counts every issued vote, including signatures no peer received. It does not
  call the replica's proof-verification or acceptance functions.
- `test_*.py`: targeted cases, checker negative controls, bounded enumeration
  and fixed-seed campaigns.
- `replay_case.py` and `evidence/`: replayable, sanitized synthetic pre-fix
  failure records. Failing outcomes are retained, not renamed successful.

Model signature tokens are deterministic authentication labels backed by a
trusted issuance registry, not secret keys, BLS or practical authentication.
The test driver can deliberately corrupt state for **checker negative
controls**; those mutations are not authorized adversarial abilities in normal
consensus scenarios.

## Review gates still open

The model assumes trusted synthetic genesis, fixed equal-weight membership,
exclusive signer ownership, stable acknowledged memory and eventual timely
delivery for progress. It does not establish disk/power-loss durability or
detect an undetectably restored coherent old backup. Resource exhaustion
stops the relevant model run/signing path; it never resets safety state.

Mainnet bootstrap/cutover, lineage expiry, membership transitions, genuine
stake-weighted PoS V2, authenticated B3 custody, bridge AssetId/RegistryId and
unchanged-verifier compatibility, complete futures margin/oracle/liquidation,
production codecs/storage/fencing and WAN performance remain separate gates.
Fixed-set success neither resolves old V1 split locks nor revokes signatures.
The existing independent FN network and ordinary Qt client are untouched.
