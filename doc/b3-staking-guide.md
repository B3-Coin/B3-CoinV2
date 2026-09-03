# B3 Staking Guide — the live corridor and your first stake

This guide is for every B3 holder who wants to stake when the modern era
begins. It covers what happens at the boundary, when and how to deposit
your stake, and how to confirm you are in the first validator set.

The chain and staking rules described here are enforced by consensus code.
The Ethereum bridge is deliberately disabled in v1.1.0 and is mentioned only
to distinguish its future gate from B3 staking. File references point at the
source for readers who want to verify.

## The timeline in one picture

```
  810,000        810,001             810,980          811,000       811,001
  legacy final → FN Genesis →  last Set0 stake  → Set0 snapshot → Modern PoS
                     └──────── POW CORRIDOR LIVE; STAKE NOW ────────┘
```

1. **Block 810,000 (H)** — the final legacy block, already confirmed.
2. **The pause** — the project observed X, measured the supply, and shipped
   the X-pin transition release. Nothing held by users changed.
3. **The corridor (blocks 810,001–811,000)** — now live. It consists of temporary
   proof-of-work blocks at a trivial fixed difficulty, paying no subsidy
   (fees only, so there is nothing to profit-mine). **This is the
   staking window.** The corridor exists so that stake deposits can
   confirm and mature *before* the first PoS block needs a validator
   set.
4. **Block 811,001 (M)** — modern proof of stake begins. A validator belongs
   to the first usable set only when its stake is ACTIVE **and** its confirmed,
   non-revoked `FINALITY_KEY` binding is present in the snapshot at block
   811,000. Mainnet chain bootstrap requires at least two such validators.
   FlowMesh markets separately require at least four active FN seats. The
   decentralized Ethereum bridge is disabled in v1.1.0; a later activation
   would have its own stricter four-staker gate.

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

There is **no slashing or unstake cooldown in V1**, either during the corridor
or after Modern PoS begins. Before M there are no validator duties; if you
change your mind, spend the output and the stake is removed. After M an active
validator has block-production and finality-availability duties, but this
release does not confiscate its principal.

## How to stake

### Recommended: B3 Hive v1.1.0

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
   while staked. B3 Hive v1.1.0 does not provide a simple GUI unstake button.
5. Wait for the Stake page to show the deposit as **ACTIVE** and the binding as
   confirmed. Then select **Start staking**. The page must show that staking is
   running for this wallet and that the finality signer is armed.

During the corridor, **Start corridor mining** is an optional separate action.
It uses one CPU thread, makes bounded attempts, pays transaction fees only, and
stops automatically when Modern PoS begins. Starting B3 Hive never starts
mining or staking by itself.

Binding does not require an existing stake. Sync B3 Hive v1.1.0 to the current
live corridor tip, then broadcast both the binding and stake transactions now.

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
- A stake that misses the first snapshot is not lost: it becomes **ACTIVE**
  20 blocks after inclusion, then becomes eligible for a future snapshot and
  joins only when that successor set completes its handover-gated rotation.
  It does not enter the frozen current set immediately. Stakes can be created
  at any time in the modern era too; the corridor is only special because it
  feeds the *first* set.

Do not spend a bootstrap stake or revoke its BLS binding merely because a new
epoch has started. Keep both in place until `getfinalitystatus` shows a
qualifying successor set in force. The V1 set is fixed for an epoch and can be
carried forward when a replacement has fewer than two validators.

Finality requires both more than two-thirds of stake weight and more than
two-thirds of validator headcount. Two validators require both signatures;
three validators require all three; four validators require three. A validator
with more than two-thirds of the stake cannot finalize alone after the set has
more than one member. This does not enable the Ethereum bridge, which has its
own four-validator readiness floor.

## Checklist

1. **Install v1.1.0** and let it sync to the live corridor tip.
2. Use the B3 Hive v1.1.0 **Stake** page to bind the BLS key and create a stake of
   333 B3 or more now. Back up the wallet, then wait for both transactions to
   confirm. The console commands above remain available as a fallback.
3. **Verify before M:** `getstakinginfo` must show the stake ACTIVE and
   `getfinalityinfo` must show `binding.bound: true`. `validator_set.member`
   remains false before the block-811,000 snapshot is installed; that does not
   mean the confirmed binding or stake is missing. Operators must coordinate
   at least two independent bound validators before M. At height 811,000,
   `getfinalitystatus.set0_preview.ready` must be true before attempting M.
4. **Start:** select **Start staking** (or run `startstaking`) and require both
   staking and finality signing to be active. Keep the node running after that.
   If you bind a replacement BLS key later, wait for it to confirm, then stop
   and restart staking so the in-memory signer uses the new key.

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

**Can I lose my stake?** V1 has no slashing or confiscation, before or after
M, and no unstake cooldown. After M, active validators are expected to stay
online for block production and finality, but the principal remains controlled
by its owner key.

**What hardware do I need?** A machine that can run B3 Hive and stay
online. Block production is a signature, not mining; there is no
hashpower race.
