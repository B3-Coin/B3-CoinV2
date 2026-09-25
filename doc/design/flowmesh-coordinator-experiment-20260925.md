# Coordinator and optional-host experiment — predeclared boundary

Status: isolated TEST experiment, not a production protocol or deployment.
Baseline: `82ebbb2e31ae8df09b575d379117c55490461617`, `flowmeshV2-dev`.
The existing native performance captures and all frozen model profiles remain
preserved. Work stays on this branch; no PR, push, live wallet or signer work.

## Question and scope

Does keeping a proposer for several batches improve progress compared with
changing the proposer at each sequence? What happens when an optional operator
host takes several otherwise distinct validator identities offline together?

This first experiment isolates **selection policy**, not the complete proposed
native redesign. It reuses the existing accounting, prepared/commit, recovery,
admission and durability models. It does not remove native V1 certification,
replace a trading backend, add pipelining or implement native V2 execution.
The native callback called `leader` is not a complete scheduling override:
native ingress and proof verification independently enforce their schedule.
Changing that callback alone would be an invalid experiment.

## Versioned alternatives

The experiment commits a new TEST profile identity and selection parameters
into the existing configuration/instance hashes. Existing profiles are not
edited. One profile is active during a case; it must not be switched while
that case's stores/replicas are used.

- Control: `floor(sequence / 1) + view`, modulo the fixed roster size.
- Candidate: `floor(sequence / 4) + view`, modulo the fixed roster size.

These are deterministic, bounded four-batch tenures, not a local performance
score, online-peer filter, lease, membership change or permanent trusted host.
The view-change, PREPARE/COMMIT, quorum and timer predicates are unchanged.
Selection is checked in proposal generation, proof acceptance, disk reopening
and independently in the historical safety checker. Exact signatures and all
issued-signature evidence are retained.

**Known limitation to measure, not conceal:** the view starts at zero at each
new sequence in this existing model. If the scheduled tenure leader remains
unavailable, the next sequence can have to fail over again. Remembering a
replacement across sequences would require additional agreed authority; a
private local preference or the particular proof subset first received is not
an acceptable shortcut. This experiment does not silently add such a rule.

## Predeclared cases and measurements

Use generated synthetic accounts, fixed 4/3 or 7/5 rosters and original test
timeouts. Compare identical workloads/delivery schedules under both policies:

- healthy consecutive batches, including a matched trade and an eight-request
  batch, client proof verification and exactly-once accounting;
- an unavailable scheduled coordinator and an unavailable noncoordinator;
- equivocation, split delivery and later recovery with existing instructions;
- missing data, restart, duplicate proofs and exact retransmission;
- one host versus multiple independent hosts and mixed self-hosted placement;
- insufficient quorum must stop, not silently remove hosted identities.

Record per-case logical elapsed ticks, first verified durable result, all
required replicas applied, messages and issued phases, alongside checker
results. Ticks are configured simulator delivery steps, **not milliseconds**.
Python elapsed time, and SQLite time within a single-process simulator, are
not native trading latency. No 200 ms claim may follow from this experiment.
Healthy and fault results must remain separate, including negative results.

## Optional hosting proposal (not economic implementation)

Validators need not use a host. A host can operate one or more authorized
identities; it receives no extra vote merely by being called a host. Placement
affects correlated availability/control, not certificate thresholds.

`-xbps` is the owner's suggested operator fee setting, **not an implemented
flag**. A proposed future interpretation is a commission in basis points on
the delegating owner's earned FN rewards, not deposits, principal, treasury
share or an extra trader fee. For example, 100 bps on 100 reward units is one
unit for the host before a separately approved integer-rounding rule.
This basis, rounding, AssetId-aware payouts, owner authorization, rate changes,
revocation/handoff and anti-double-sign fencing require an approved protocol.
A local operator flag cannot authorize deductions by itself. Spending keys
must remain separate from validator-signing authority. No key delegation,
commission, funds movement or live hosting is implemented by this experiment.

## Interpretation and next gate

Stable selection is a hypothesis, not evidence that rotation caused the
retained 477 ms native result. Equal healthy results or worse fault results
must be reported as such. After this bounded result, review the selection and
handoff rules before a separately identified native experiment. Native proof
replacement must preserve executed-state verification, durable application,
client safety and independently specified B3 settlement compatibility.
