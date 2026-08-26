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

## Modern PoS reward — RULED 2026-08-26 (mechanism AND numbers)

Owner pins (2026-08-26, second ruling of the day):

- **Initial annual rate: 1% of S_H** — `R0 = floor(S_H * 1% / 525,600)`
  atomic units per block, with `S_H` = the MEASURED total supply at
  H = 810,000 from the final U==U' capture (never assumed).
- **Halving interval: 525,600 blocks = one year** at the 60 s target
  spacing ("halving every year"): `reward(h) = R0 >> floor((h - M)/525,600)`.
- **Corridor reward: 0 + fees.** The 1,000 PoW corridor blocks carry no
  subsidy at all — fees only. No new coins exist until Modern PoS begins
  at M = 811,001.
- Split: 90% producer / 10% treasury per block (standing proposal,
  applied unless the owner objects); all tx + payload fees to the
  producer; treasury share pays to the single ruled treasury address.

Consequences, reported: total lifetime emission converges to ~**2% of
S_H ever** (1% + 0.5% + 0.25% + ...) — an exceptionally scarce schedule;
the staking security budget decays quickly (year 1 = 1%, year 3 = 0.25%),
so transaction + FlowMesh fee flow must become the dominant validator
income within a few years. This is the accepted trade-off of the ruling.

## (superseded earlier note) mechanism RULED 2026-08-26: HALVING SCHEDULE

The owner ruled a Bitcoin-style halving emission (rejecting the constant
tail-emission recommendation). Mechanism locked; NUMBERS still PROPOSED:

- `block_reward(height) = R0 >> floor((height - M) / HALVING_INTERVAL)`
  (integer atomic units; emission ends when the shift reaches zero).
- PROPOSED `HALVING_INTERVAL` = **2,102,400 blocks** (~4 years at the
  60 s target spacing).
- PROPOSED `R0` = sized from the MEASURED supply at H so that first-epoch
  issuance is ~3%/year of `S_H` (R0 = S_H * 3% / 525,600); total lifetime
  emission then converges to ~24% of S_H over all halvings. `S_H` comes
  from the final U==U' capture at H — never an assumed number.
- Split per block, enforced in the coinbase: **90% block producer /
  10% treasury address** (PROPOSED, carried over). All transaction +
  payload fees to the producer.
- Corridor (1,000 PoW blocks): same R0, same split (PROPOSED).
- Halving properties, stated honestly: strong scarcity narrative and
  familiar economics; the security budget decays by design, so fees (and
  FlowMesh volume feeding the treasury) must grow into the gap across
  epochs — this is the accepted trade-off of the ruling.
- Owner pins still needed: R0 (or the 3% sizing rule), HALVING_INTERVAL,
  the 90/10 split, and the treasury address.

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
