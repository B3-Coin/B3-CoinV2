# Coordinator-selection comparison — TEST only

This is a bounded selection/placement experiment, not a native performance
implementation. See the [predeclared boundary](../../doc/design/flowmesh-coordinator-experiment-20260925.md),
[profile](TEST_PROFILE.json), [measured results](RESULTS.md) and
[qualification record](QUALIFICATION.md).

It compares the existing per-sequence proposer selection with a separately
identified four-batch tenure. Both retain the same PREPARE/COMMIT protocol,
view-change rules, quorum, action identities, signing obligations and timers.
This does **not** implement the complete proposed longer-lived coordinator:
replacement authority does not persist into the next sequence.

## Run

From the repository root with Python 3.14 and its standard library:

```sh
python3.14 -B ci/run_flowmesh_models.py --suite coordinator
python3.14 -B ci/run_flowmesh_models.py
PYTHONHASHSEED=0 python3.14 -B test/flowmesh_v2_coordinator/experiment.py > coordinator-comparison.json
```

The first command runs 20 coordinator tests; the second includes the earlier
332 accounting/agreement/storage/runner tests. The comparison runs ten
predeclared cases and includes synthetic event/signature traces. Its schedule
is bounded to 180 logical ticks per attempted batch and by the existing model
resource limits. JSON is emitted on successful completion. No services, live
keys, wallet files, external network or native binaries are used.

## What is measured

- Four equal synthetic seats with an unchanged quorum of three.
- Healthy: eight consecutive batches, each with one BUY and one SELL that
  actually fill; one base atom for 20,000 quote atoms, with one quote atom fee
  per side under the existing accounting test profile.
- Burst: eight original instructions from eight accounts, four BUY and four
  SELL, already placed in **one** batch. This is not the earlier daemon's
  eight-request client/HTTPS burst and must not be compared as if it were.
- Faults: scheduled seat 0 unavailable, seat 3 unavailable, and two seats
  unavailable. All remaining seats must verify and apply the original batch
  for a case to complete. A missing quorum is an expected stop.

Elapsed ticks start immediately before the one original body offer. They end
when the observation harness verifies the complete COMMIT proof, exact body,
execution result and apply-count of one in the model's durable record.
First verified result and last required replica application are separate
fields; neither includes a real client request/response. The observation is
in-process and keyless, with synthetic authentication. Execution, proof
checking and persistence have **no wall-clock cost assigned in these ticks**.

Messages include deliveries to unavailable nodes and maintenance/catch-up
traffic, including leftovers from the previous sequence. They are not a count
of unique signatures, socket writes, or successful peer receptions.
Issued-phase counts and all issued-signature evidence are retained separately.
The independent checker remains mandatory; it does not reuse the proposer
helper for its schedule arithmetic.

## Storage and profile isolation

The optional TEST profile is included in the existing configuration hash.
Only spans 1 and 4 and the declared version/identity are accepted. The default
agreement profile file is unchanged. A case holds one exclusive in-process
profile context for all replicas, proof checking and SQLite reopening; no
replica may be operated after leaving that context. It is not a concurrent
multi-profile production API.

Two tests use the existing SQLite adapter on disposable generated directories.
They check exact pending COMMIT restoration, one-time application, duplicate
store ownership refusal and wrong-profile reopening refusal without modifying
the original store. These are same-process close/reopen tests. Existing
separate-process interruption tests are rerun by the full runner; this new
policy is not independently power-loss qualified.

Other tests cover equivocation, partition/heal, missing data/certificates,
nonleader sparse ingress, duplicates, memory restart, seven seats under
predetermined loss/duplication seeds 45312 and 45313, and independent checker
negative controls.

## Optional hosting

Placement is modelled as correlated failure of existing identities. A host
adds neither a vote nor spending authority. One operator controlling many
keys remains one correlated control domain, even across several machines.
Hosting is not a mandatory route for a self-run validator.

No delegation, key-transfer workflow or commission code is added. In
particular, `-xbps` is **not an available command-line flag**. The proposed
commission basis is a share of earned FN rewards with explicit owner consent,
not principal, deposits, treasury funds or an extra trading fee. Its rules
require separate approval before implementation.

## Qualification limits

No native 200 ms result, real BLS, WAN, HTTPS/Qt, B3 settlement, bridge verifier,
changing membership, production futures risk, hosting security or deployment
is qualified here. The existing native latency baseline is unchanged.
Passing tests are not an independent security audit.
