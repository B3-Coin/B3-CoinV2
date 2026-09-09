# FlowMesh exact duplicate-evidence forwarding

Status: implemented; focused execution and full functional qualification pending.

## Scope

An informed intermediary previously stopped an incoming retained ACTION retry at
the action-pool duplicate check. It can now finish forwarding that received copy
to other peers. This does not create actions, resubmit locally originated actions,
change headers, recover conflicting permanent locks, or change consensus rules.

Only an authenticated remote duplicate whose complete encoded payload equals the
stored action is eligible. Equality includes the credential: semantic action IDs
alone are insufficient. A different valid Schnorr signature for the same action
is not an exact copy and is rejected by this path.

## Bounds and lifetime

- One ID-only FIFO obligation per existing pool entry, carrying the original
  header and excluded incoming peer; duplicate arrivals do not reorder it.
- Global and per-market ceilings of four additional frames and 16 KiB per 250 ms.
  No idle accumulation or refill from backward clock movement.
- A 250 ms per-action cooldown after completing a forwarding attempt, including
  one suppressed by an existing relay gate. An exhausted batch keeps the FIFO
  obligation for a later tick; it does not create another obligation.
- At most 16 markets and 16 action preparations per drain tick, with a rotating
  market cursor. A market that consumes the global batch yields the next batch
  to its successor; FIFO ordering prevents a fixed action-prefix starvation.
- No duplicate payload storage and no periodic re-enqueue. Completing one
  received obligation cannot itself schedule another. A later authenticated
  incoming repeat can admit a new obligation after the cooldown.
- Pool removal also removes its obligation. Certified advancement clears all
  obligations instead of refreshing their headers. Halt, pause, or handoff
  prevents draining and clears pending obligations.

Before sending, the runtime rechecks ready/anchor/seat-transition/halt state,
the exact original epoch and sequence against the current header, authentication,
and certified account nonce or deposit consumption. It removes the obligation
and charges both budgets before calling the existing relay callback. The incoming
peer is excluded.

The additional sustained ceilings are 16 frames/s and 64 KiB/s, separately from
retained-evidence retry's 32 frames/s and 128 KiB/s. These count application frames
before peer fan-out, not total network traffic or guaranteed admitted throughput.
Ordinary first-action relay, committee traffic, and existing receiver limits are
unchanged. Incoming cycles remain rate-limited rather than impossible.

## Regression coverage

`duplicate_action_forwarding_recovers_missing_tail_through_informed_middle`
uses a three-runtime line with three of four committee seats. The middle already
has nine actions while the far endpoint lacks the ninth and cannot lock the
proposal. The original proposer retries its retained evidence in eight-plus-one
chunks; the middle drains exact FIFO copies in four/four/one batches. A later
proposal retry certifies the original locked candidate and executes all actions
once. Invalid credentials, a different valid credential for the same semantic
ID, and a conflicting same-nonce action admit no retry obligation. An old-header
copy after certification is not rewritten or forwarded.

`duplicate_action_forwarding_fifo_market_fairness_and_cleanup` covers two-market
rotation, exact copies and origin exclusion, coalescing, same-clock and byte/count
bounds, no autonomous recurrence, pause cleanup, and anchor-invalidated halt.

## Remaining limits

This is a transport repair opportunity before incompatible final locks exist.
The unchanged protocol cannot repair an already split irreversible lock. It also
does not relax stale ACTION headers or solve a locally originated action that
never reaches any peer. Passing unit tests alone does not qualify burst trading.
