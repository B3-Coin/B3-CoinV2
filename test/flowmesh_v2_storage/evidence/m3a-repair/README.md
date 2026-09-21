# Milestone 3A repair: retained failing-before evidence

These are the three review reproductions against frozen baseline
[`1d022dbf0da63636d6d43650d3864222761497a1`](https://github.com/B3-Coin/B3-CoinV2/commit/1d022dbf0da63636d6d43650d3864222761497a1).
They are **baseline counterexamples**, not after-repair qualification and not an
independent security audit.

## Run the frozen reproductions

Use Python 3.14 on a POSIX host supported by the storage harness. Keep this
repair checkout unchanged. Prepare a separate clean baseline checkout; no new
development branch is needed:

```sh
git clone --no-checkout https://github.com/B3-Coin/B3-CoinV2.git ../b3-m3a-frozen
git -C ../b3-m3a-frozen checkout --detach 1d022dbf0da63636d6d43650d3864222761497a1
python3.14 -B test/flowmesh_v2_storage/evidence/m3a-repair/probe_view_transition.py ../b3-m3a-frozen
python3.14 -B test/flowmesh_v2_storage/evidence/m3a-repair/probe_admission_interactions.py ../b3-m3a-frozen
```

Run the last two commands from this repair checkout's repository root.
The public launcher checks the supplied checkout's exact HEAD and clean status
before imports. It deliberately refuses the repaired tree: the reproductions'
assertions describe the defects at the old revision.

The original private review probes and evidence remain preserved separately.
The only public-probe adaptation is source selection: a positional checkout
argument and the read-only guard in `_frozen_baseline.py`. No fault schedule,
assertion, message, timer, quorum or model table has been changed. Both public
launchers were rerun against a clean detached frozen checkout; their outputs
matched the retained logs. A modified checkout was refused with exit 1.

## Observed failing-before results

| Check | Observed result |
|---|---|
| R1 no-kill control | All three honest nodes applied the decision; settled at tick 36. |
| R1 kill after durable view entry | Node0 exited -9, reopened its original generated store in view 1 without a VIEW_CHANGE intent. All three honest nodes remained sequence 0, CHANGING view 1, no deadline through tick 232. No new client request or injected recovery OFFER was used. |
| R2 exact 150-cycle churn | 300 received-vote buckets despite only 2 references, 2 requests, 2 pending entries and 0 bodies. Durable head unchanged; no permanent signing halt. |
| R3 remote OFFER while CHANGING | Zero reports and initially no deadline; OFFER created deadline 60, then expiry advanced to view 2. |

Both launchers returned **0** because their original assertions intentionally
verify these counterexamples. That is not a pass of the desired progress,
retention or timer invariants.

R1 uses separate child processes. The no-kill control exits were
`[0, 0, 0, 0]`; the fault scenario exits were `[-9, 0, 0, 0, 0]`, including
the reopened node. All test children were confirmed exited. R2/R3 use the
original same-process disk simulator; do not describe them as process-kill
tests.

## Evidence and limits

- `BEFORE.json`: commit/tree identities, original/public probe hashes, output
  hashes, exact observations, launcher and child exits.
- `view-transition-results.jsonl`: bounded R1 control/fault and cleanup output.
- `admission-interaction-results.jsonl`: bounded R2/R3 output.

The independent checker accepted the signing/accounting evidence in these
scenarios. Its safety result does not establish progress, timer eligibility or
cache bounds. The 600 Byzantine-issued signatures retained by the R2 auditor
are separate from a replica's disposable received-vote cache.

The unchanged probes use generated temporary test stores, which are cleaned
when each probe finishes. The retained JSON is their bounded stdout, **not**
the complete store contents or per-event runtime stream. Authentication,
membership and anchor evidence are synthetic. Process-kill recovery is not
machine-power-loss qualification. No live wallets, real keys or production
validators are involved.
