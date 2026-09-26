# Boundary before a full-node regtest trial

The native healthy probe and native recovery gate are NOT engine-off node or
Qt integration. Running them beside a mining B3 process would be coexistence,
not evidence that B3 authorizes their state or that a wallet verifies their proof.

## Existing native seams

* `src/node/flowmesh_runtime.cpp`: `SubmitLocalAction` / `HandleAction` admit
  the exact signed action; `EvaluateCandidate` / `ValidateAgreementCandidate`
  bind execution to the current parent/configuration/roster/anchor.
* `FinalizeAgreement` currently transitions into V1 attestation/certification;
  `HandleCertificate` / `CommitCertified` and the production store require
  the existing certificate type.
* `src/flowmesh/client_evidence.cpp::VerifyClientStateEvidence` and the node
  client's snapshot/inclusion verification protect engine-off clients.
  Inclusion alone is not proof of a matched fill or its complete outcome.
* `BuildProductionCheckpointRecord`, service checkpoint handling and B3
  custody/withdrawal validation enforce existing settlement lineage/formats.

## Required next bounded adapter

Progress: [the read-only regtest bootstrap prerequisite](REGTEST_BOOTSTRAP.md)
now verifies a pinned existing V1 base against local B3 authority on four real
operators and an engine-off client. It authorizes neither a protocol cutover
nor execution, and is not completion of the remaining runtime/store/client
proof path below. Earlier full V1 regtest trading remains preserved separately.

1. A default-off regtest-only, explicitly versioned experimental protocol
   profile. It must not masquerade as V1 or alter default/mainnet operation.
2. Bootstrap from a verified regtest B3 state: exact domain, market, parent,
   balances, seats, anchor and effect cursor. The current hard-coded generated
   fixture is insufficient authority. Preserve any original client instruction.
3. A separately reviewed native multi-instance/recovery store and test proof
   verifier. Research proof bytes are not existing V1 certificates. Keep V1
   rejection tests; never bypass validation to get the prototype accepted.
4. A real engine-off client path with exact signed-action outbox, certified
   execution/result verification and durable no-resubmit state. Neither the
   current stdio observer nor the earlier standalone client is Qt qualification.
5. Use the existing isolated functional fixtures: latency-market bootstrap,
   independent-network B3 workload and engine-off assertions. Exercise one
   matched fill, coordinator loss, a recovered fill and returning-replica
   catch-up while real B3 blocks advance. Capture actual process exits.
6. Measure one original signed action through client durable result on the
   exact integrated binary. Keep healthy/fault distributions separate; record
   all outliers. Only then compare that boundary to the 200 ms local objective.

No external tester handoff follows automatically. Finite-view/single-instance
research recovery, economic-conflict cases, cross-instance pipelining, admission
under hostile traffic, anchor transitions and bootstrap authority require their
own reviewed implementation. The unchanged bridge, PoS V2, shared-account V2
execution and futures remain separate requirements, not completed by this gate.
