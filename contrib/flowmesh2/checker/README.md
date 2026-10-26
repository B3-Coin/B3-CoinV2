# Independent FlowMesh V2 checker (M3-ENG-05)

An independent model of the FlowMesh V2 slot engine, written in standard-library
Python from `src/flowmesh2/ARCHITECTURE.md` section 11, the PROVISIONAL
decisions D4, D5 and D7 of `src/flowmesh2/TODO.md` and the open-question
register. It shares no code with the C++ engine. Its codecs, hashes, Merkle
trees and BIP340 are reimplemented too; they are checked against goldens the
C++ tests pin and against BIP340 test vector 0.

Scope: the DEX-1 regtest profile (`fm2-dex1-regtest-provisional/1`) and state
commitment v0. Withdrawals, payout and source-range facts, the stable-quote
and view-bound profiles, and futures are outside it.

## Files

- `fm2check/codec.py`: tagged hashes, Merkle roots (RFC 6962 shape), the wire
  codecs (actions, payloads, deposit facts, the 366-byte slot header,
  outcomes) and the commitment v0 leaves.
- `fm2check/bip340.py`: BIP340 sign and verify (test keys only, not constant time).
- `fm2check/engine.py`: the slot validity predicate, the canonical order and
  phases 1-9 (deposits, transfers, cancels, post-only, the A2 sweep, rest or
  release, fees, conservation), outcomes and the next header.
- `fm2check/batch.py`: seeded batches in five kinds: `random`, `adversarial`
  (25 mutations: bad signature, used nonce, duplicate ActionId, forked
  anchor, broken or out-of-range fact cursors, duplicate outpoint, stub and
  unknown fact kinds, oversize facts, wrong domain or window, unsupported or
  truncated originals, and typed action refusals), `dust` (one-lot,
  low-price orders and tiny transfers, so fee rounding decides), `caps`
  (more than 1,000 open orders on an account, more than 1,024 fills by one taker)
  and `slotcap` (17 makers rest 17,000 one-lot asks; then 17 takers of 1,024
  lots in one slot ask for more than the 16,384 fills of the slot budget. Pro-
  rata keeps each taker near 1,000 fills, under the per-order cap, so every
  REJ_FILL_CAP of that slot is the slot budget's; `fm2_check.py` prints the
  model's stops, `SLOT_FILL_CAP 17`. The slot changes about 17,000 order
  leaves, more than the 8,192 changed-leaf cap of D6, which nothing enforces
  yet: when M3-ENG-04 does, this slot must be refused and the batch changed).
- `fm2_check.py`: runs each batch through `fm2-trace` (the real engine, via
  `state::BuildCandidate`) and through the model. It requires byte equality
  on every slot: the verdict (error code and detail on refusal), the
  canonical order, every outcome, every account leaf and resting order, the
  five state roots, the header and its hash. `--skeleton` recomputes the
  walking skeleton's goldens (below).
- `test_fm2check.py`: self-tests of the codecs and BIP340.

CTests: `flowmesh2_checker_selftest`, `flowmesh2_checker_agreement`,
`flowmesh2_checker_caps` and `flowmesh2_checker_slotcap` (label `checker`;
the last takes about 3 minutes, mostly signing 17,000 orders in Python).

## The walking skeleton's goldens (O-87)

`fm2-trace --export-skeleton` runs the walking skeleton (seed 1) and prints
its committed slots as inputs only: genesis parameters, anchors, fact ranges,
facts, originals sorted by their bytes and certificate hashes. The model
recomputes from them the canonical orders of slots 2-4, the slot-5 header
hash and the final state root. `fm2_check.py --skeleton` compares them with
the values pinned in `src/flowmesh2/sim/scenario.cpp` (`GOLDEN_CANONICAL_ORDER`)
and `src/flowmesh2/test/sim_walking_skeleton_tests.cpp` (`GOLDEN_SLOT5_HEADER`,
`GOLDEN_FINAL_ROOT`), read from those files. Certificate hashes are inputs:
checking BLS certificates is not the engine's job.

## Spec gaps

Where the spec text leaves a choice open, the model marks it `GAP` in
`fm2check/engine.py`. The comparison with the engine settled each of them as
follows. None needs a ruling: each is a missing sentence, not a conflict.

1. Outcome deltas are netted per (account, asset), sorted by (account,
   asset) bytewise, and all-zero entries are dropped (`DeltaSet::Sorted`).
2. `remaining_lots` is non-zero only for RESTING and PARTIAL_RESTING. CANCELLED,
   IOC_DONE and REJ_FILL_CAP carry 0.
3. A taker's stop (FILLED, NO_CROSS, ORDER_FILL_CAP, SLOT_FILL_CAP) is fixed
   when its side's sweep ends, before phase 6 rests anything. matcher.h says
   so, but section 11 does not.
4. The residual cross is per market and only among GTC remainders that left
   the sweep with NO_CROSS (engine/residual.h).
5. Both sides of a fill add the quote to `cum_quote`; only the seller adds the fee.
6. A fee share of 0 (a 1-atom fee total gives the seat pool 0) creates no
   protocol account.
7. A fact kind whose codec is a stub (2, 3, 4, 6) fails the slot with that
   stub's NOT_IMPLEMENTED error and task id, not with INVALID_INPUT. Unknown
   kinds are "fact does not decode". `MAX_FACT_BYTES` is 1,024 (core/body.h).
8. A deposit fact is refused if its amount is not positive, it credits a protocol
   account, its outpoint is already credited, or the outpoint repeats in the slot.
9. A FACT outcome's subject is the FactId, with nonce 0. The SLOT outcome of
   phase 8 has the quote asset as subject and a zero account.
10. Canonical-order keys and every sorted id compare as raw bytes (memcmp order).
11. A fact range may end anywhere up to (anchor height + 1, 0).
12. `ArrivalRank(height, phase, rank)` = (2·height + phase block)·max_actions_per_slot
    + rank, with post-only in block 0 and rest in block 1.

A transfer of an asset nobody holds is REJ_INSUFFICIENT_AVAILABLE, as O-7
orders it. The registered REJ_UNKNOWN_ASSET is never emitted by DEX-1's
TRANSFER.

## Not covered

- Balances or totals above MAX_ATOMS. These are errors, not outcomes (O-34); the
  model raises NotImplementedError rather than guess their code, and no batch
  reaches them.
- Payout and source-range facts, withdrawals, CANCEL_ALL execution and futures
  (stubs or other profiles).
