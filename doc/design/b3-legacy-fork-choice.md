# Legacy B3 Fork-Choice and Chain Weight

How a legacy B3 block earns chain-selection weight, traced from the historical
client on the `master` branch (commit `4aa1b16`, `src/main.cpp` and
`src/kernel.cpp`). This records the semantics the modern tree must preserve
for the legacy era and must not "improve" into a different protocol. It is the
reference for the anti-DoS work in [b3-implementation-status.md](b3-implementation-status.md)
(the D2 fix).

## Chain weight is target-derived and identical for PoW and PoS

The historical client scores a chain by `nChainTrust` on `CBlockIndex`, summed
as `pprev->nChainTrust + GetBlockTrust()`. The per-block trust
(`CBlockIndex::GetBlockTrust`, `master:src/main.cpp:2561`) is

    trust = (1 << 256) / (target + 1)

taken from the block's `nBits`, with **no proof-of-work / proof-of-stake
branch**. A PoW block and a PoS block at the same `nBits` earn exactly equal
trust. This is the same quantity Bitcoin Core computes as
`GetBlockProof(nBits)`, so the modern tree's `nChainWork` accumulation is
already faithful to the historical trust formula and must not be replaced with
a separate trust field.

Consequences to keep in mind:

- An easier claimed target earns strictly *less* trust (monotone in target), so
  a fake-easy block cannot dominate on trust.
- Trust ignores the coin-day weighting that actually gates the kernel, so trust
  does not measure stake-work; it measures claimed difficulty only.
- The best chain is chosen by `pindexNew->nChainTrust > nBestChainTrust`
  (`master:src/main.cpp:2273`), strict `>`, so ties keep the incumbent.

## A legacy block earns weight only after full validation, and blocks-only

The historical client is **blocks-only**: it serves `getheaders` but has no
`headers` message handler, so a header alone never creates a `CBlockIndex` and
never contributes trust. Sync is `inv` -> `getdata` -> full `block`.

Trust is assigned inside `AddToBlockIndex` (`master:src/main.cpp:2242`), which
is reached only from `AcceptBlock` (`master:src/main.cpp:2545`), i.e. **after**
these contextual checks have passed:

1. `nBits == GetNextTargetRequired(pindexPrev, IsProofOfStake())` — the exact
   deterministic retarget (`master:src/main.cpp:2496`, `DoS(100)`). An attacker
   cannot claim an arbitrary target to inflate trust; the target is fixed by the
   parent chain and the block type.
2. `Checkpoints::CheckHardened` — the 13 pinned mainnet heights
   (`master:src/checkpoints.cpp:50`).
3. `CheckProofOfStake()` — the stake kernel and coinstake signature
   (`master:src/main.cpp:2517`, `master:src/kernel.cpp:388`). The kernel target
   is `nBits` weighted by coin-days, checked as
   `hashProofOfStake <= coinDayWeight * targetPerCoinDay`
   (`master:src/kernel.cpp:303`).
4. `Checkpoints::CheckSync` — the rolling max-reorg-depth bound (below).

So the order is: receive full block -> `CheckBlock` (context-free shape,
including the block signature; no kernel) -> `AcceptBlock` (the four checks
above) -> `AddToBlockIndex` (trust assigned) -> `SetBestChain` -> `ConnectBlock`
(inputs, sigops, reward; it does **not** re-check the kernel). A header, or a
block whose kernel fails, never reaches the trust assignment.

## Spam is bounded by cost, depth, and duplicate-stake, not by proof-of-work

Because legacy blocks are proof-of-stake, there is no proof-of-work to gate
admission. The historical client bounds cheap block spam three ways:

- **Kernel cost.** A valid PoS block requires solving the stake kernel for a
  UTXO the miner controls; `CheckCoinStakeTimestamp` forces
  `nTimeBlock == nTimeTx` (`master:src/kernel.cpp:424`) and `nTimeTx` is hashed
  into the kernel, so each attempt costs a fresh kernel solve within a bounded
  time window.
- **Rolling depth bound.** `CheckSync` with `nCheckpointSpan = 500`
  (`master:src/checkpoints.cpp:15`, `:88`) rejects in `AcceptBlock` any block
  whose height is `<= pindexBest->nHeight - 500`. Maximum reorg depth is ~500
  blocks; a block forking deeper never gets a `CBlockIndex` and never earns
  trust.
- **Duplicate-stake.** `setStakeSeen` / `setStakeSeenOrphan`, keyed on
  `(kernel prevout, coinstake nTime)` (`master:src/main.cpp:2618`, `:2668`),
  reject reusing the same stake.

Orphans are bounded separately (40 MiB, random eviction,
`master:src/main.cpp:1117`) and carry no trust.

## What the modern tree preserves, and where it differed

The modern tree keeps the trust formula (`GetBlockProof(nBits)`), the kernel,
the retarget, and the reward schedule. The security gap the D2 fix closes is
that the modern tree has a headers-first path and, on a legacy chain, admitted
legacy headers to the index (with `nChainWork` from unvalidated `nBits`) before
any legacy validation — exactly what the historical blocks-only design never
allowed. The fix makes the legacy era blocks-only again: a legacy-codec header
is admitted only as part of a full block, so it earns no chain weight until the
block is present to be validated.

## Post-X synchronization is membership, not recomputed trust

A decision that scopes all of the above. Once H/X is finalized:

- **Legacy history (height <= H) is reconstructed by trusted replay**, not by
  header-first consensus. Full legacy blocks are downloaded only to rebuild
  state and to verify their **membership in the pinned chain ending at X**
  (previous-hash linkage, checkpoints, Merkle roots). The stake kernel and the
  legacy trust competition are **not** recomputed. A proof-of-stake block
  cannot be validated without its full block, and there is no need to: the
  checkpoints up to H and the pinned X are what establish trust.
- **Headers-first synchronization begins at H+1** under modern consensus.
- **Live full legacy consensus is required only before the transition is
  finalized**, for blocks at height <= H while the chain is still being
  extended and selected under the historical rules.

The consequence for anti-DoS: do **not** build accept-time legacy
proof-of-stake trust machinery to keep unvalidated chainwork out of fork
choice. That job is done by membership. A full legacy block that lies off the
X-anchored chain is anchor-ineligible regardless of its claimed `nBits` or the
validity of its kernel, so it never enters `setBlockIndexCandidates` and never
influences tip selection -- even though `AddToBlockIndex` still credits its
`GetBlockProof(nBits)` (a block off the anchor keeps its stored trust, exactly
as the historical client left a `ConnectBlock`-failed block in its index). This
is exercised adversarially in `legacy_transition_tests`: a signed PoS block
with absurd `nBits` and a bogus kernel, forking below H, is stored with
chainwork exceeding the tip yet is marked `BLOCK_ANCHOR_INELIGIBLE`, kept out
of the candidate set, never connected, and leaves the tip unmoved.

## Follow-on hardening

Two historical mechanisms are noted but **not yet ported**, tracked in the
status document:

- the rolling `CheckSync` depth bound (`nCheckpointSpan = 500`); and
- the hardened checkpoint list.

Both are legacy-era consensus rules and must be ported faithfully (values and
placement) rather than reinvented.

**Scope for `CheckSync`.** The 500-block rolling depth bound belongs to **live
legacy consensus** — the regime where the node still extends and selects the
legacy chain under the historical rules, i.e. before H/X is pinned. It must
**not** become a trusted-replay rejection rule after X. Once X is finalized the
legacy prefix is fixed and reconstructed deterministically by following X's
linkage backward (`TrustedReplay`), which does no competitive chain selection;
imposing a rolling-depth reorg limit there would be a new rule the historical
client never applied to already-settled history. After X, alternate-branch
exclusion is the job of the anchor-ineligibility check, not of `CheckSync`.

H/X chain eligibility — once X is pinned, no alternate legacy branch may compete
regardless of claimed trust — is thus a separate concern from `CheckSync`,
handled by the anchor-ineligibility check, not by trust.
