# FlowMesh verified committee forwarding

Date: 2026-09-10. Status: **implemented; syntax checked, focused execution and full functional qualification pending**. The earlier `af543e4` production candidate failed two real-P2P 64-action bursts and is not trading-ready. This change does not recover existing conflicting locks or identify the earlier mainnet stall's cause.

## Finding and scope

The four-node functional fixture uses the ordinary line topology `0—1—2—3`. The previous runtime broadcast locally created proposals and attestations to direct peers, but did not forward received proposals or verified attestations. The P2P relay does not implicitly connect every committee seat. Actions and certified entries already have separate propagation paths.

The retained first fixture has durable two-plus-two locks at `(epoch 0, sequence 46)`: nodes 0/1 selected 53 actions; nodes 2/3 selected 26. The second fixture reproduced a two-plus-two split at sequence 45 with 56 versus 10 actions and captured a `signing-conflict` halt. Its packet log shows an endpoint proposal reaching the adjacent validator, but no onward proposal to the other half before a different endpoint proposed. The stored candidates contain their corresponding credentials. Exact evidence retry cannot reconcile different permanent lock hashes.

This patch repairs multi-hop reachability within the existing protocol. The binding decisions in [the decision register](b3-flowmesh-dex-decisions.md), including O-9a/O-9b permanent locks and safe halt, remain unchanged. It does not change wire bytes, proposer schedule, quorum, action authentication, auction matching, anchors, certificates, signer journals or checkpoint rules. It does not introduce an early-attestation cache or claim general BFT lock recovery.

## Bounded forwarding policy

In `src/node/flowmesh_runtime.cpp`, `HandleProposal` forwards an incoming proposal only after existing proposer signature, current/next round, anchor/transition, authenticated action evidence, deterministic candidate evaluation and successful local durable retention checks. Observers do not create a signing lock. Forwarding precedes local votes, although the existing priority queue can still process an attestation before a proposal. The local original proposal is already broadcast normally and its loopback does not add a forwarding broadcast.

`HandleAttestation` forwards only after matching the BLS signature to an already evaluated candidate and an in-range active seat, and rejecting a seat's conflicting candidate vote. A same-candidate duplicate must contain the same compressed signature as the stored verified vote. The exact incoming payload is forwarded; no new signature is produced. Unknown-candidate, invalid and conflicting votes do not relay or count as verified votes.

Both paths exclude the incoming peer and apply:

| Limit | Scope |
|---|---|
| At least one second between attempts | Each candidate's proposal, shared across all authenticated rounds |
| At least one second between attempts | Each existing candidate/seat verified vote |
| 8 additional frames and 2 MiB + 4 KiB per 250 ms | Each market |
| 32 additional frames and 4 MiB per 250 ms | Entire runtime, across markets |

Byte accounting includes the FlowMesh header and can accommodate a maximum legal proposal. Batch capacity does not accumulate after inactivity or refill on a backward clock. These are additional application-forwarding ceilings before peer fan-out, **not total bandwidth or promised admitted throughput**. Existing local originals, scheduled proposer retries and separately bounded action-evidence retries are outside this new budget. The existing per-peer/market committee gate remains **8 messages/s, burst 32**, unchanged.

The identity cooldown is stamped on an attempted forwarding opportunity even if either budget denies the frame. Successful-budget attempts are charged before the relay callback, including a downstream gate-suppressed send. Budget/cooldown no-send does not prohibit otherwise eligible signing. A fresh failed safety/anchor/transition check is a different result and stops subsequent signing or certification in that handler.

Retry opportunities require a later authenticated incoming repeat. There is no autonomous retransmit timer, payload queue or per-round allocation. Copies returning through a cycle are rate limited, not deduplicated forever. An incoming retry after the cooldown may forward the same proposal or stored verified vote again, allowing a lost initial onward message to recover. Metadata lives inside the existing bounded candidate map and per-seat vote identities, disappears on certified progress, and is not journaled. Market/global budgets persist across progress, preventing sequence churn from refilling them.

## Focused regression coverage

Four new cases in `src/test/flowmesh_runtime_tests.cpp`:

- `committee_forwarding_budget_bounds_count_bytes_and_clock`: maximum legal proposal, global and per-market count/byte limits, shared-market global cap, no backward-clock or idle accumulation refill.
- `committee_forwarding_line_recovers_exact_dropped_vote`: endpoint proposer, observer intermediary and two more remote seats; the first distant vote is dropped and side certificates are prevented. After a one-second same-round proposal retry, the identical cached vote crosses two intermediaries and all nodes certify the same hash without changing any signing position. Same-clock copies and malformed/signature-invalid messages cannot amplify.
- `committee_forwarding_paces_verified_repeats_and_denied_budget`: denied-attempt cooldown survives a batch refill, next-round churn does not reset pacing, verified repeated votes preserve exact bytes, and a known seat's conflicting vote for a second evaluated candidate does not relay or count.
- `committee_forwarding_fresh_pause_prevents_local_vote`: initial validation and durable retention succeed, but the new fresh policy check sees a pause; no local vote or proposal forwarding occurs.

The existing `honest_divergent_round_candidates_preserve_permanent_locks_and_halt` regression remains unchanged and must continue to pass. Successful forwarding does not authorize signing a different candidate after a permanent lock. A full real-P2P burst qualification is still required after focused tests pass; do not replace that test with a fully connected topology to hide propagation defects.
