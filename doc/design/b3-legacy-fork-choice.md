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

## Checkpoints and the depth bound (ported)

Both historical mechanisms are now ported faithfully (values and placement,
never reinvented):

- the hardened checkpoint list (13 mainnet heights, `legacy::MainnetCheckpoints`);
- the rolling `CheckSync` depth bound (`nCheckpointSpan = 500`,
  `legacy::ReorgDepthExceeded`), enforced in the live-legacy branch of
  `ContextualCheckBlockHeader` and skipped while importing our own blocks.

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

## Historical reward-rule exceptions: provenance

The port originally carried a single unsourced predicate
(`IsHistoricalStakeRewardCapException`) exempting heights 77447-77505 and
107488 from the coinstake reward cap. A provenance investigation against the
final client (`master`, commit `4aa1b16`) resolved it into **three distinct
sourced rules**, now ported faithfully. Chain data was not available offline;
every fact below is grounded in the final client's source, which is the
operative consensus for any node syncing that history (whatever the affected
blocks contain, the final client accepts it through these exact rules, so
per-block observed rewards are not needed for a faithful port).

1. **The repair window, heights 77447-77505 inclusive** (`master:src/main.cpp`,
   `if (!(pindex->nHeight > 77446 && pindex->nHeight < 77506))`, introduced by
   commit `e0f3256` "fix small sync bug and change the fork"). The final client
   skips its whole coinstake reward-cap check inside the window. Ported
   verbatim as `legacy::IsRepairWindowHeight`. The dense hardened checkpoints
   at 77900-78961 and rule 3 below are the rest of the same incident response.

2. **The superblock at height 107488** (`chainparams.cpp` `nSuperBlockHeight =
   107488`, `vSuperBlockPubKey = 0432160b...0522`; `main.h` `SUPERBLOCKPAYMENT
   = 75656908 * KILO_COIN` with `KILO_COIN = 1e9`; enforcement in
   `ConnectBlock`; introduced by commit `dafa714` "Added superblock on
   mainnet"). This was **not** a cap bypass: at exactly that height the general
   cap is replaced by a structured rule — the last coinstake output must pay at
   most 75,656,908,000,000,000 units to the P2PKH script of the pinned key
   (hash160 `1c49f78e1a406c64996da1bc5fda3b371bd33706`), else the block is
   rejected. The port's former blanket `height == 107488` bypass dropped that
   validation entirely; it is now enforced via
   `Consensus::Params::legacy_superblock_height/pubkey` and
   `legacy::LEGACY_SUPERBLOCK_PAYMENT` / `legacy::SuperblockPayeeScript`.

3. **The restricted staker, heights above 78000** (`ConnectBlock`,
   `if(pindex->nHeight > 78000)`; lineage includes commit `d10f4fd` "Change
   address to destroy address"). A proof-of-stake block whose second coinstake
   output pays the destination of address
   `ShJsVNBQMa2M7cfCVPzRMt8nVZxHitBp7v` (version byte 63, hash160
   `db8ca2a4493aaed6b7d2f30acb4467b823e0b0a5`) while earning a positive reward
   is rejected. This unconditional consensus rule was **missing** from the
   port and is now implemented (`legacy::StakeDestinationIsRestricted`),
   reproducing the 0.8-era `ExtractDestination` fold of pay-to-pubkey into the
   key's hash, which modern `ExtractDestination` no longer performs.

One further check in the same code region is deliberately **not** ported: the
Fundamental Node payment check (`foundfnpayment`) is gated on
`!IsInitialBlockDownload()` in the final client, so it never applied to a node
validating the historical chain from genesis — it is not a sync-consensus rule
— and porting it would also reintroduce FN consensus against the standing
scope decision.

Trusted replay is unaffected by all of the above: replay attests rewards via
the pinned X and never adjudicates them (`legacy/replay.cpp`), so these rules
exist only for faithful pre-X live legacy validation.
