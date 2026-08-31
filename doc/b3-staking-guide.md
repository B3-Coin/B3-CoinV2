# B3 Staking Guide — the transition and your first stake

This guide is for every B3 holder who wants to stake when the modern era
begins. It covers what happens at the boundary, when and how to deposit
your stake, and how to confirm you are in the first validator set.

Everything here is enforced by consensus code; nothing is a promise.
File references point at the source for readers who want to verify.

## The timeline in one picture

```
  LEGACY (PoS, today)      PAUSE          POW CORRIDOR            MODERN PoS
 ──────────────────────┬──────────┬──────────────────────────┬──────────────────►
                   810,000    (days: X is   810,001 .. 811,000   811,001 (= M)
                   final     observed and   1,000 CPU-mined      60-second blocks,
                   legacy    the X-pin      blocks, fees only    stake-weighted
                   block     release ships)  << STAKE HERE >>
```

1. **Block 810,000 (H)** — the final legacy block. Every B3 Hive v1 node
   accepts the chain through H and refuses anything above it. The chain
   pauses on purpose: the boundary hash X cannot be known before the
   block exists, and everything modern derives from X.
2. **The pause** — the project observes X, measures the supply, and
   ships the **X-pin release**. Nothing you hold changes; every balance
   carries over exactly. Install the X-pin release when it is announced.
3. **The corridor (blocks 810,001–811,000)** — one thousand temporary
   proof-of-work blocks at a trivial fixed difficulty, paying no subsidy
   (fees only, so there is nothing to profit-mine). **This is the
   staking window.** The corridor exists so that stake deposits can
   confirm and mature *before* the first PoS block needs a validator
   set.
4. **Block 811,001 (M)** — modern proof of stake begins. The validators
   of the very first PoS block are whoever holds an ACTIVE stake in the
   snapshot at block 811,000.

## What a stake is

A stake is a special output you create from your own wallet
(`src/modern/stake.h`):

- Your **principal stays yours**. The stake output is owned by a normal
  address in your wallet; unstaking is simply spending that output back
  to yourself. Nothing is sent to anyone.
- The wallet attaches your **validator key** (created automatically on
  first use) — the key that signs your blocks when you are selected.
- The **minimum stake is 333 B3** (consensus constant, ratified).
  There is no maximum; multiple stakes from one wallet aggregate onto
  the same validator.
- Your weight is your principal: the chance of producing a block is
  proportional to your stake over the total staked.

During the corridor there is **no cooldown, no slashing, and no
duties** — if you change your mind before M, spend the output and the
stake is removed. Slashing and validator duties belong to the modern
era, after M.

## How to stake

The current Qt **Stake** page is monitoring-only. It shows wallet state and
reward history, but its staking controls have no backend in this release and
cannot create a stake.

Create the stake with RPC / command line:

    b3coin-cli createstake 1000

The reply shows the transaction id, your validator key, the owner
address that holds the principal, and the activation depth. Check
progress any time with:

    b3coin-cli getstakinginfo

## Confirmations: UNCONFIRMED → PENDING → ACTIVE

A stake created in the block at height **b** becomes **ACTIVE at height
b + 20** (`STAKE_ACTIVATION_DEPTH`, src/modern/stake.h). Twenty corridor
blocks is about twenty minutes at the corridor's 60-second minimum
spacing — but do not aim for the deadline:

- To be in the FIRST validator set (the snapshot at block 811,000),
  your stake must be **included in a block no later than 810,980**.
- **Recommendation: stake in the first half of the corridor** (blocks
  810,001–810,500). The corridor lasts 17+ hours at minimum; staking
  early leaves hundreds of blocks of margin for anything — a slow
  corridor, a missed transaction, a wallet issue.
- A stake that misses the first snapshot is not lost: it activates 20
  blocks after inclusion and joins the validator set from then on.
  Stakes can be created at any time in the modern era too; the corridor
  is only special because it feeds the *first* set.

## Checklist

1. **Before block 810,000:** if you do NOT stake with the old client,
   upgrade to B3 Hive v1 now. If you DO stake with the old client,
   **keep it staking until block 810,000 is produced** — B3 Hive
   validates legacy blocks but cannot produce them
   (src/node/miner.cpp), so the old clients must carry the chain to
   the boundary — and upgrade during the pause instead (there will be
   days). Either way, decide how much you will stake and keep it in
   your wallet, confirmed. Old clients left running past 810,000 build
   a dead branch the network ignores; harmless, but pointless.
2. **At the pause:** wait. Do not panic; the chain stopping AT 810,000
   is the designed behavior, not an outage. Watch the official channels
   for the X-pin release.
3. **Install the X-pin release** when announced.
4. **When the corridor starts:** run `createstake` for 333 B3 or more,
   early in the corridor.
5. **Verify:** use `getstakinginfo` as the authoritative status and wait for
   it to show your stake ACTIVE. Once it is, you are in the set at M — leave
   the node running and it stakes.

## Questions people will ask

**Is my balance safe through the transition?** Yes. The transition
rewrites no history and touches no balance. Coins that never stake are
simply ordinary coins in both eras.

**Do I have to stake?** No. Staking is optional; unstaked coins work
normally.

**What are the rewards?** From M, blocks pay a subsidy (announced with
the X-pin release, computed as 1% per year of the measured supply,
halving yearly) plus fees, weighted by stake. Corridor blocks pay the
staker nothing — the corridor is plumbing, not a reward phase.

**Can I lose my stake?** In the corridor: no — no slashing exists
there, and you can withdraw at will. After M, validator rules apply to
active validators.

**What hardware do I need?** A machine that can run B3 Hive and stay
online. Block production is a signature, not mining; there is no
hashpower race.
