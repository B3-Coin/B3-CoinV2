# Native recovery gate — TEST profile 1

This is a separate native port of the bounded
[P2-FV message/restart profile v2](../flowmesh_v2_fastpath_research/MESSAGE_PROFILE.json).
It is not the production FlowMesh agreement, a new wallet, or a latency result.
It does not rename or replace the earlier PBFT-style agreement model.

## Exact boundary

* Four separate C++ replica processes, fixed generated membership, at most one
  Byzantine seat, one instance/sequence and views 0 through 2.
* Real BLS signatures, proof-of-possession membership validation, Schnorr
  authorization, the existing native curve-auction execution/receipt checks,
  and actual synchronous LevelDB writes.
* Test scheduler carries bounded messages over child-process pipes. Delivery
  ordering, loss and abstract timer steps are controlled. This is NOT a TCP,
  HTTPS, Qt, real-clock latency or WAN campaign.
* Two deterministically generated, individually valid request envelopes use
  different Schnorr auxiliary randomness. They have the SAME semantic ActionId,
  execution entry and result. The native tests distinguish certified exact-body
  identities; they do not yet test two economically different native batches.
* Custody, initial liquidity, anchors and identities are synthetic. No B3 node,
  live wallet, production signing key, bridge or stake mechanism is involved.
* Individual sender-bound BLS proofs differ from the earlier healthy probe's
  same-message aggregate signature. Its approximately 79 ms median cannot be
  attributed to this new recovery implementation.

## Agreement and proof rules

Leader is view modulo four. View zero FAST requires three distinct PREPAREs
including seat zero. A prepared certificate (PC) requires three PREPAREs;
later-view final SLOW requires three COMMITs. COMMIT requires an accepted
matching proposal, the exact verified NEW_VIEW and matching PC. CHANGING is
not ACTIVE. Original votes and complete decisions remain binding across views.

REPORT carries the original vote/leader evidence, highest PC and any decision.
Its target must exceed the PC view. NEW_VIEW carries exactly three reports,
plus any old-leader equivocation proof. Reported decisions are finalized rather
than bypassed. Highest PC determines the selected body; otherwise the profile's
original-vote/equivocation selection applies. Proven equivocation excludes old
leader zero from the report quorum. An individual PC never authorizes ACTIVE.
View-bound exhaustion is an explicit safe wait, not an unlimited recovery claim.

Proofs bind a distinct TEST signing domain, instance, membership hash, phase,
view, author and exact carried evidence. They cannot be repackaged as a V1
certificate. Codec limits: 32 KiB, 64 nodes, depth 8, four direct children.
These are serialized limits, not measurements of total heap use.

## Durability and publication

1. Validate the exact executed body, current mode, proposal, NEW_VIEW, PC and
   retained obligations as applicable.
2. Synchronously store an unsigned signing intent and its complete guard.
3. Compute the BLS signature.
4. Synchronously store the exact signed packet, immutable guard and associated
   local obligation before any output containing that signature.
5. Publish through the flushed response. Retrying uses the identical object.

The full store snapshot and identity are one synchronous LevelDB batch.
Decision recording precedes application. The exact resulting state, decision
and one-time application count are atomically committed before in-memory apply.
Reopening independently executes retained bodies and checks the stored state,
proofs, local signature slots, guards and view authority. Missing indispensable
evidence fences the process. A pending durable intent can be completed exactly.

Any persistence failure fences further work. Failed-store responses must not
expose proof-bearing mutable state: diagnostics are also a publication channel.
Missing remote bodies remain ordinary NEED/DATA recovery, not journal reset.
Only test-owned stores are opened, and LevelDB exclusive ownership is retained.

Cut labels distinguish before intent completion, before signature computation,
computed-undurable, durable-unpublished, flushed publication, decision-before-
application, before the atomic application batch, and after application.
`during_apply_before_atomic_commit` is deliberately BEFORE the batch call; it
does not simulate a torn filesystem/device write inside that call. Process
exits and injected failures do not qualify machine power loss. Filesystem and
device flush guarantees, undetectable coherent old backups, tamper-proof
external freshness and production anti-rollback remain unqualified.

## Observer and finite resource scope

The Python observer imports neither the native verifier nor the model replica.
It retains all observed signatures, including withheld/late votes, checks
immutable slots/guards, restored obligations, certificate-backed application
and hidden quorums. It trusts native issuance/guard observations as test facts;
it is not a second BLS implementation or a production security proof. Retained
slot counts are only a monotonic lower-bound check, not proof of record identity.

The worker accepts only the declared two-body universe and finite views. It
retains bounded pending evidence, votes, reports and local obligations rather
than dropping them to obtain progress. This does NOT carry forward the earlier
general DATA/OFFER pressure qualification into arbitrary native network traffic.
Neither unlimited hostile input nor multi-instance/multi-batch liveness is proved.

## Reproduce

Use the repository's normal supported native build dependencies. The probe
option is default off and only added on UNIX with native tests enabled:

```sh
cmake -S . -B build -DBUILD_TESTS=ON -DBUILD_GUI=OFF -DBUILD_FLOWMESH_FASTPATH_PROBE=ON
cmake --build build --target flowmesh-p2fv-proof-check flowmesh-p2fv-worker -j 4
build/bin/flowmesh-p2fv-proof-check
python3.14 -B -m unittest discover -s test/flowmesh_v2_fastpath_native -p 'test_native_checker.py' -v
python3.14 -B test/flowmesh_v2_fastpath_native/run_recovery_campaign.py --binary build/bin/flowmesh-p2fv-worker --evidence /tmp/flowmesh-native-recovery
python3.14 -B test/flowmesh_v2_fastpath_native/test_store_refusal.py --binary build/bin/flowmesh-p2fv-worker --evidence /tmp/flowmesh-native-recovery
python3.14 -B ci/run_flowmesh_models.py --suite fastpath_research
```

Each campaign creates a new disposable subdirectory and retains generated
fixtures, exact responses, schedule, actual child exits and results. No existing
wallet or store should be supplied. No full-node/Qt/WAN or public tester-ready
claim follows from this gate.
