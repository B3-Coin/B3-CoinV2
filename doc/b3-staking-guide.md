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
4. **Block 811,001 (M)** — modern proof of stake begins. A validator belongs
   to the first usable set only when its stake is ACTIVE **and** its confirmed,
   non-revoked `FINALITY_KEY` binding is present in the snapshot at block
   811,000. Mainnet chain bootstrap requires at least two such validators.
   FlowMesh markets separately require at least four active FN seats, and the
   decentralized Ethereum bridge has its own stricter four-staker gate.

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

### Recommended: B3 Hive beta.2

1. Open the wallet that will own the validator, then open **Stake**.
2. Select **Generate & bind BLS key** and unlock the wallet when asked.
   Binding needs a small transaction fee but does not require an existing
   stake. Wait until the page shows the binding as confirmed.
3. Copy the displayed public `validator_key`, public `bls_pubkey`, and binding
   transaction id for coordination. Select **Back up wallet** immediately and
   store the backup offline. Never share the BLS secret, wallet file, password,
   or seed phrase.
4. Enter at least **333 B3** and select **Create stake**. The principal remains
   in a special wallet-owned output, but it is excluded from ordinary spending
   while staked. Beta.2 does not provide a simple GUI unstake button.
5. Wait for the Stake page to show the deposit as **ACTIVE** and the binding as
   confirmed. Then select **Start staking**. The page must show that staking is
   running for this wallet and that the finality signer is armed.

During the corridor, **Start corridor mining** is an optional separate action.
It uses one CPU thread, makes bounded attempts, pays transaction fees only, and
stops automatically when Modern PoS begins. Starting B3 Hive never starts
mining or staking by itself.

Binding does not require an existing stake. After the transition client is
synced to the pinned block 810,000, an operator may broadcast the binding and
stake transactions for inclusion from block 810,001 onward. Do not broadcast
them from an earlier legacy tip.

### Console fallback

The same workflow remains available in the debug console or with
`b3coin-cli`:

    walletpassphrase "YOUR_WALLET_PASSWORD" 120
    bindfinalitykey
    createstake 333
    getstakinginfo
    getfinalityinfo
    startstaking
    walletlock

For an unencrypted wallet, omit `walletpassphrase` and `walletlock`. Never paste
the wallet password anywhere except your own local debug console.

Use a larger amount in `createstake` if desired. Wait for the binding and stake
transactions to confirm and for the stake to become active before relying on
the validator at M. `startstaking` must return `running: true` and
`finality_signing: true`. The producer keeps an in-memory copy of the required
keys when the wallet is re-locked; select **Stop staking**, run `stopstaking`,
or shut down B3 Hive to stop it and clear those copies.

## Confirmations: UNCONFIRMED → PENDING → ACTIVE

A stake created in the block at height **b** becomes **ACTIVE at height
b + 20** (`STAKE_ACTIVATION_DEPTH`, src/modern/stake.h). Twenty corridor
blocks is about twenty minutes at the corridor's 60-second minimum
spacing — but do not aim for the deadline:

- To be in the FIRST validator set (the snapshot at block 811,000),
  your stake must be **included in a block no later than 810,980**.
- **Recommendation: stake in the first half of the corridor** (blocks
  810,001–810,500). Its 1,000 required 60-second timestamp steps span about
  16 hours 40 minutes of chain time (the future-time allowance can make
  observed wall time slightly shorter); staking
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
4. **When the corridor starts:** use the beta.2 **Stake** page to bind the BLS
   key and create a stake of 333 B3 or more, early in the corridor. Back up the
   wallet, then wait for both transactions to confirm. The console commands
   above remain available as a fallback.
5. **Verify:** `getstakinginfo` must show the stake ACTIVE;
   `getfinalityinfo` must show a live binding and set membership. Operators
   must coordinate at least two independent bound validators before M.
6. **Start:** select **Start staking** (or run `startstaking`) and require both
   staking and finality signing to be active. Keep the node running after that.

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
