# B3 FlowMesh economics v1 — consolidated (RULED vs PROPOSED)

Single source of truth for economic parameters, superseding scattered
notes. Owner rulings of 2026-08-26 incorporated. Anything marked PROPOSED
awaits an explicit owner ruling; nothing PROPOSED may be pinned into a
release.

## Supply — RULED
- Legacy B3 supply carried 1:1 across H; no premine, no re-issuance ever.
- Display unit kB3 (1 B3 shown = 1e9 base units); FN marker = 1 old COIN.
- FN Coin: lifetime cap 5,000 units (RATIFIED 2026-08-22); 3,500 issuable
  against historical PoD proof; Proof-of-Disintegration burns B3 -> FN.

## Treasury — RULED 2026-08-26
- **One single treasury wallet/address. No multisig, no complexity.**
  The address itself is an owner release input (never invented by the
  implementation; pinned like other release parameters).
- **Issuance fees pay to the treasury address** (supersedes the earlier
  burn recommendation).
- **FlowMesh trading fees pay to the same treasury address**, on the
  proposed 5 bps taker / 2 bps maker schedule (bps numbers themselves
  still PROPOSED until pinned).
- Reported trade-off, accepted by the ruling's simplicity mandate: a
  single key is a single point of compromise/loss; strong key hygiene
  (hardware-backed) recommended; the address can be rotated only by
  release.

## Modern PoS reward — PROPOSED (answering OD-2)

**Recommendation: constant tail emission with a consensus-enforced
treasury share.**

- `block_reward = round(S_H * r / BLOCKS_PER_YEAR)` where `S_H` is the
  MEASURED total supply at H (taken from the final U==U' capture at H --
  never an assumed number), `r` = **1.5%/year** (PROPOSED; sane band
  1-2%), `BLOCKS_PER_YEAR = 525,600` at the 60 s target spacing.
- Split per block, enforced in the coinbase: **90% block producer /
  10% treasury address** (PROPOSED). All transaction + payload fees to
  the producer.
- Corridor (1,000 PoW blocks): same formula, same split — one rule
  everywhere (PROPOSED).
- Properties: predictable security budget from day one (fee-only was
  rejected: too thin early); no halving cliffs or decay schedules to
  misimplement ("not perfect but working" — one constant, one split);
  effective staking APY = r / staked_fraction, which self-balances
  participation (low participation -> higher APY -> more staking);
  treasury funding needs no second mechanism — it rides every block plus
  issuance and FlowMesh fees into the one ruled wallet.
- Revisitable by future release without touching balances or history.

## Asset issuance fee — RULED destination, PROPOSED size
- Destination: the treasury address (RULED 2026-08-26).
- Size: PROPOSED flat 10 B3 per ASSET_ISSUANCE (anti-spam scale, not a
  gate for serious issuers) + the standard MPA payload costs. Activates
  with the asset phase; the consensus check is "coinbase-independent
  output of >= fee to the treasury script in the issuing transaction".
- User protection remains layered as designed: unforgeable AssetIdV1,
  registered-vs-permissionless tiers, FlowMesh listing gate against
  approved quote assets, wallet unknown-asset flagging.

## Bridge fees — OPEN
Stage-4 decision; a small bps on mint/withdraw to the treasury is the
natural shape; interacts with mint caps (threat model T1). Not designed.

## Still requiring owner pins before the relevant release
1. `S_H`-derived block reward + r + split (this proposal's numbers).
2. FlowMesh bps numbers.
3. The treasury address itself.
4. Issuance fee amount.
5. Bridge fee (stage 4).
