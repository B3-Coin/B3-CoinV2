# P2-FV recovery-evidence gate — research tests only

Successor work: [message/restart TEST model](MESSAGE_MODEL.md), separately
versioned from the evidence-only probe below. Neither is a native wallet.

This is **not** a native fast-path implementation, protocol ratification or
latency benchmark. The existing accounting/agreement/storage models and native
daemon are unchanged. The probe follows one narrow prerequisite to implementing
the supplied “FlowMesh V2 and PoS V2 — protocol research and recommendation”
(2026-09-25; SHA256 in [TEST_PROFILE.json](TEST_PROFILE.json)).

## Finding: private evidence is not shared evidence

Report §3.7 lets a replacement proposer use evidence “in hand” that the old
leader signed conflicting values. But §3.2 lists NEW_VIEW as carrying the
selected statuses and selected value; it does not explicitly identify a field
for conflicting leader proposals obtained outside those statuses.

Concrete case with four seats, quorum three and one faulty old leader:

1. Leader 0 signed original proposals x and y.
2. The new leader holds both originals, but its selected three reports are
   `seat1: voted x`, `seat2: no vote`, `seat3: no vote`.
   Specifically, a fourth authenticated status `seat0: voted y` was also in
   hand. It supplies y's original signature but is excluded from the selected
   quorum because step 3 excludes the equivocating old leader.
3. No prepared/decision certificate is known. With equivocation evidence,
   §3.7 step 3 sees only one x report, below threshold two: ANY permits y.
4. A follower receiving only those reports sees a single x; step 4 requires x.
   It rejects the leader's choice y.

This is a **specification/evidence-provenance ambiguity**, not proof of two
conflicting finalized results. There is no hidden fast quorum in this example,
and choosing x remains possible. It does not prove permanent unavailability
under every implementation or a defect in the currently running wallet.

The script preserves both interpretations and their different results.
It does not invent an E=true flag and assume followers know why it is true.
The separate bounded review confirmed this empty-PC case; its observation
about recording the extra, discarded status is reflected in the reproducer.
Its authentication-test coverage comment is addressed by testing an otherwise
valid, unissued report separately from reports with an invalid target view.
This narrow review is not a protocol security audit.

## Proposed clarification tested, not adopted

The TEST profile optionally carries the two original, same-instance/view/leader
signed proposals as explicit NEW_VIEW evidence. Every receiver derives the
selection from transmitted, authenticated evidence, not its private receipt
history. A receiver must not grant this authority to an asserted boolean.

The proof pair alone never counts as extra statuses or votes. The selection set
still has three distinct reports excluding the equivocating old leader.
If the required two originals already occur in the selected reports, no extra
pair is needed. No fast quorum or threshold is changed.

This is one possible completion of the report, not a new production wire format.
Before adoption, the full NEW_VIEW format, its authentication/digest binding,
proof bounds, missing-data rules and interaction with prepared/decided proofs
must be specified and reviewed. This probe intentionally has no prepared or
decision certificates; it does not resolve the separate global-exclusion versus
highest-PC precedence ambiguity.

## Tests actually run

15 focused unittest cases passed, including:

- preserved missing-evidence mismatch and the explicit-pair interpretation;
- report/proof ordering, private arrival history independence;
- original proof identity, context/view/signer checking;
- rejection of forged assertions, duplicate reporters and a reduced status set;
- embedded conflicting originals without an additional pair;
- all 27 assignments of x/y/no vote to the three honest nonleaders, checking
  selection under all six report permutations and preserving any possible
  hidden fast value in this bounded shape.

Authentication is an ideal issued-object registry. Report construction is a
test-fixture facility, not a real honest signer transition. The 27 assignments
are not a full state-space exploration of the protocol.

Reproduce from the repository root, Python 3.14:

```
python3.14 -B -m unittest discover -s test/flowmesh_v2_fastpath_research -p 'test_*.py' -v
python3.14 -B test/flowmesh_v2_fastpath_research/recovery_evidence_probe.py
```

This exploratory directory is not yet part of the existing complete model CI
runner; report these 15 checks separately from its existing suite totals.

## What remains before a real latency result

No messages traversed a socket, no trading execution or actual signature ran,
and no durable signing record was written by this probe. No time result follows.
The previously reproduced 161 ms is still a simulation, not a regtest result.

Required next gate: freeze the missing evidence/selection rules in a separately
reviewable TEST profile; build the complete message/view/crash state machine and
independent decision checker, including actual two-decision negative controls.
Only then compare isolated native slow and fast paths with real durability and
matched fills, before a private multi-machine regtest pilot with B3 staking.

No mainnet, wallet, signer-history, checkpoint or production consensus change.
