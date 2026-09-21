# Milestone 2.1 — review and qualification handoff

## Frozen history

- Reviewed predecessor: `0247001a04588fccbadf63bb9d4a8704dcaf5b2c`.
- Exact 257-DATA regression: `48b684a0ed154d8681e861eb68ff84526ed7fc9d`.
- Full-history retry regression: `8e1cc9652b48c7d2350b8557e4eaf2415290c944`.
- Initial repair implementation: `b1a95c3` (ancestor in this branch).
- Frozen internal-review target: `8edd2c084b63818e528cc43b1b6f30263f74d48b`.
- Additional remote-OFFER regression: `99e9b5dc3038e43689c6c771f7831ae72c7d43d1`.

All are preserved ancestors. No squash, rewritten failure fixture, journal
conversion, production branch update, PR, tag, release or deployment.

## One fresh-context internal review — INCOMPLETE

A separate reviewer context was asked to inspect actual source and interactions
at `8edd2c0`, not merely the implementation summary. It reported a concrete
public-ingress bypass: valid-looking remote `OFFER` bodies were retained as if
each was an acknowledged local client obligation. A Byzantine peer could fill
the durable body table; the first decision completed, but the next legitimate
local offer permanently halted with `RETAINED_BODY_LIMIT`.

The review then terminated with this tool error:

> This content was flagged for possible cybersecurity risk. If this seems wrong,
> try rephrasing your request. To get authorized for security work, join the
> Trusted Access for Cyber program: https://chatgpt.com/cyber

The request was **not** rephrased, routed to another reviewer or otherwise
retried. There is no completed fresh-context review verdict. Its bounded
partial finding is retained separately from tests. The parent reproduced it
through public ingress, froze the failing regression/trace, and made the narrow
successor repair authorized by this milestone:

- Remote offers enter a 32-entry disposable validated pool, not global durable
  client retention. Refusal is explicit backpressure, not permanent signing halt.
- Local synthetic client offers keep their durable original bodies and sparse
  retry behavior. No new client authentication is claimed.
- A remotely learned body selected for a first signature is saved atomically
  with the exact intent in the current durable record. Supported restart can
  recover the original body/intent and retransmit existing signatures.
- Current candidate bodies remain protected through signing/commit obligations;
  completed records and checker evidence are not pruned.
- Regressions cover the reproduced flood followed by two genuine decisions,
  each of four proposal-publication crash boundaries, and a full disposable
  inbox with an accepted vote followed by restart and progress.

**Remaining review gap:** the whole Milestone 2.1 interaction review did not
finish. In particular, this successor's remote/local offer separation and
`intent_bodies` durability have not received a completed separate-context
review. Passing automated tests do not replace that review. External review of
the exact published branch is still required before the next integration stage.

## Newly executed local evidence before final freeze

| Execution | Observed result |
| --- | --- |
| Original exact DATA regression, N=4 and N=7 | Failed: cache 256, durable halt; restart cache 0, same halt |
| Repaired DATA regression at `8edd2c0`, N=4 and N=7 | Passed: cache 0, no halt, unchanged durable state, later valid decision |
| Original 31-decision retry assertion | Failed: all 31 certificates rebroadcast |
| Repaired retry assertion at `8edd2c0` | Passed; full historical evidence remains available |
| Combined runner at `8edd2c0` source | 8 runner guards, 113 unchanged accounting, 113 agreement tests passed |
| New remote-OFFER regression at `99e9b5d` | Failed with `RETAINED_BODY_LIMIT` at sequence 1 |
| Successor intermediate agreement run | 114 passed in 70.171 s |
| Successor focused admission/crash tests | 16 passed in 4.527 s |

Intermediate failures and fixture corrections are enumerated in
[MILESTONE_2_1.md](MILESTONE_2_1.md), not overwritten. Full private execution
logs remain outside publication. Repository evidence files contain only
synthetic traces, not wallets, production keys or real signing journals.

The final combined command discovers **8 runner + 113 accounting + 116
agreement tests**. It must pass again from the exact final committed source
before push:

```sh
python3.14 -B ci/run_flowmesh_models.py
```

The same command is the hosted model workflow. At this document's freeze:
**CI configured; remote execution pending.** Final handoff must identify the
actual tested/published full commit and distinguish clean-tree results from
subsequent hosted results. Neither this document nor an existing YAML file is
evidence of a completed GitHub run. No broad native-wallet or release CI result
is implied by isolated-model success.

## Review boundaries

Source prerequisites are in the branch; the frozen accounting, application,
proof verifier, independent checker and R1 source were not changed. Only
synthetic agreement admission/delivery/memory, tests, documentation and the
unprivileged dedicated model CI are in the milestone delta.

The model still assumes fixed synthetic membership, ideal authentication,
synthetic finalized anchors and atomic acknowledged stable memory. Tests do not
qualify live V1 lock recovery, real cryptography, disk/power loss, coherent old
backup detection, changing membership, mainnet bootstrap, stake-weighted PoS V2,
unchanged bridge compatibility, futures margin/liquidation or 200 ms/WAN speed.
Bounded schedules and finite traffic are not a universal liveness proof.

This remains a PBFT-style isolated model, not HotStuff or production FlowMesh.
Publish only the existing `model/flowmesh-v2-agreement-test1` branch and stop
for external milestone review. Do not start real-storage/signature/network work.
