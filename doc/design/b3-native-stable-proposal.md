# bUSD — B3-native overcollateralized stablecoin (superseded proposal)

**Status: SUPERSEDED by the owner's 2026-09-01 ruling. Production bUSD is
the `BRIDGE_BACKED` B3 asset minted 1:1 from proven canonical-USDT deposits
through the explicitly pinned Ethereum-mainnet vault. This file is retained
only as design history; none of its CDP, oracle, collateral, liquidation, or
parameter choices are release requirements or authorization to implement a
second asset named bUSD. Current rulings live in
[b3-flowmesh-dex-decisions.md](b3-flowmesh-dex-decisions.md) L-3/L-6 and the
bridge threat model.**

Written 2026-08-22 from the owner's direction in conversation: "a B3-native
overcollateralized stablecoin so FlowMesh needs no outside permission; people who hold
USDT buy B3 on external markets and bring it in as collateral." This document says how
that can be built inside the locked architecture, what it still depends on, what it
contradicts, and which decisions are the owner's.

Per [b3-master-handoff.md](b3-master-handoff.md) §0 the ruled direction governs through the DEX register; this document is its design record and acquires full documentary authority on owner review of this commit.

---

## 0. Not an "algorithmic" stablecoin

This is **not** a seigniorage / uncollateralized / rebasing design (Terra-class). It is an
**overcollateralized debt position (CDP) stablecoin**, Liquity-shaped: every unit of bUSD
is a liability of a specific vault that holds more than $1 of collateral, and the system
has no issuer, no admin key, and no outside dependency except the price oracle.

Contract §47 names the policy module slot `ALGORITHMIC`. Recommendation: implement it
under a name that says what it is — `CDP_BACKED` (or `COLLATERAL_BACKED`) — and keep
`ALGORITHMIC` as the contract's umbrella term. Naming is owner's call; semantics below.

## 1. What it buys, honestly

- FlowMesh can run with a dollar quote asset that **no issuer can freeze, delist, or
  withdraw**, and that needs **no bridge and no federation**.
- The fiat path is the Bitcoin model: USDT holders buy B3 on external exchanges (later
  Chainflip-class swaps), B3 arrives natively, they lock it and mint bUSD or fund the
  stability pool. Their dollars never touch B3; nothing outside the protocol gets a veto.

What it does **not** remove: a price oracle (§4) — replaced from "trust Tether" to
"trust the staked, slashable FN set", which is B3-internal but is not nobody.

## 2. Where it lives in the locked architecture

| Piece | Placement | Why |
|---|---|---|
| bUSD asset | modern colored asset, own `AssetId`, policy `CDP_BACKED` | handoff §3.3/§3.4; supply may change **only** through the CDP state machine (mint on borrow, burn on repay / redemption / liquidation); conservation: Σ vault debt == bUSD supply |
| vault/CDP state | deterministic FlowMesh state, FN-certified microblocks, anchored on B3 | handoff §5.1 (account-model execution), §6 (microblocks); same determinism rules as trading (§5.5) |
| collateral custody | the keyless `DEX_VAULT` | handoff §5.7; collateral is deposited like any FlowMesh deposit (§5.2), never leaves consensus-validated custody, no key can take it |
| accounting domain | a **third state domain**, "CDP", beside spot and futures | extends ruling L-5 (spot/futures separate); explicit `SPOT_TO_CDP` / `CDP_TO_SPOT` actions; CDP logic may touch only CDP balances |
| oracle | FN-certified price feed entering as ordered microblock actions | matches decision-register O-5 note ("oracle inputs entering as ordered microblock actions … a privileged actor to be specified"); same oracle later serves futures mark |
| liquidation engine | deterministic full liquidation (handoff §5.11 "first liquidation version") | one engine, later shared with futures |

No VM, no scripts, no new consensus language: the CDP is a typed policy + a typed
FlowMesh state machine, exactly like spot.

## 3. Mechanism (Liquity-shaped, integer arithmetic, all parameters owner-set)

```
open vault          : lock collateral C (B3 v1; BTC via SPV later), mint debt D
constraint          : C · price ≥ MCR · D           (MCR = minimum collateral ratio)
repay / close       : burn D, release C
redemption          : anyone burns bUSD → receives $1 of collateral from the riskiest
                      vaults (hard peg FLOOR without an issuer)
liquidation         : vault with C · price < MCR · D is closed in full; its debt is
                      absorbed by the stability pool, which receives the collateral
stability pool      : bUSD deposits that absorb liquidations; rewarded in B3/FN
recovery mode       : if system-wide CR < CCR, minting that lowers CR is refused and
                      liquidation threshold tightens toward CCR
debt ceiling        : formula-driven (see §5), never a hope
bad-debt order      : stability pool → redistribution to remaining vaults → (owner
                      decision) surplus buffer. Decided up front, never live.
fees                : one-time mint fee + redemption fee (both owner); no interest in v1
```

Deterministic: prices, ratios and fees are integers in fixed units; quantity quantization
reuses the spot `QTY_LOT` discipline (decision register D-2) so no rounding rule exists
beyond explicit floor divisions.

## 4. The oracle — the one real trust point

Requirements (all owner decisions, recommendations in brackets):

- **Source**: active FN seats each submit a signed price observation as an ordered
  microblock action; consensus price = **median** of the latest observation per seat
  within a staleness window [stale if older than N microblocks → no new mint, liquidations
  continue on last good price with a bounded move].
- **Smoothing**: a TWAP over a long window [hours, not minutes] for mint/redemption
  checks; a shorter window for liquidation triggers (owner decides whether they differ).
- **Bounded move**: per-update maximum deviation [x%]; larger jumps apply over several
  updates — makes a single-venue wick unable to liquidate anyone instantly.
- **Circuit breaker**: if realized volatility over the window exceeds [y%], **pause new
  minting only**. Redemptions and liquidations never pause (pausing them is how pegs die).
- **Misbehaviour**: an FN whose observations deviate from the median beyond [z%] over
  [k] rounds is penalized (FN penalty rules are already an open item; this adds one case).
- **Circularity note**: early on, the only B3/USD prices are on external exchanges, which
  is what FNs observe. Once FlowMesh bUSD markets are deep, their TWAP may join the median
  — but never as the sole input (that is the "last_trade_price as oracle" anti-pattern,
  handoff §9).

## 5. Low-volume safety — size by rule

The danger of a thin coin as collateral is real (oracle pushing, liquidations without
buyers, reflexivity). Every one of those scales with **debt ceiling ÷ liquidity**, so:

- **Debt ceiling = f(observed liquidity)**: e.g. a small fraction of the 30-day median
  external B3 liquidity the oracle observes, lagged, monotone in both directions (grows
  with depth, shrinks when depth vanishes). Numbers are owner's; the *formula* is the
  safety property.
- **MCR high at launch** [250–400%], CCR higher; both may fall by explicit owner ruling as
  depth and BTC collateral arrive — never automatically.
- **Launch size**: tens of thousands of dollars, not millions. Labelled as an early native
  stable in every UI.
- **Collateral set**: B3 only at v1 (capped); **BTC via SPV peg-in** (trust-minimized on
  the mint leg) added when the bridge layer (A3) exists — the least reflexive collateral
  in crypto, and the thing that lets the ceiling grow.
- **Chain safety is unaffected**: base chain never depends on any stable asset
  (handoff §3.5); the vault is keyless; a bUSD failure costs bUSD holders and vault
  owners, not B3, staking, FN or other markets.

## 6. Contradictions and conflicts — reported, not resolved

1. **Decision-register L-3 (LOCKED, 2026-08-19): "DEX fees are denominated in USDT (a
   USDT-backed colored coin) and collected by FN Coin holders."** Using bUSD as a quote
   or fee asset is a change to that ruling. The owner must either re-rule ("fees in an
   approved stable AssetId, including bUSD") or keep USDT fees and run bUSD as a quote
   asset only. Not decided here.
2. **Sequencing**: handoff §5.3 excludes "production oracle economics" from the initial
   spot target and §5.11 locks "leverage only after spot". A CDP is margin machinery
   (collateral, mark, liquidation). Building it before futures **pulls the oracle and
   liquidation engine ahead of step 11**. Proposed order (owner's call):

   ```
   H+1 → TEST_USDT spot harness → vault → microblocks
       → FN oracle + deterministic liquidation engine (decided once, shared)
       → bUSD, B3-collateralized, small formula ceiling   ← first real mainnet quote asset
       → futures (same engine, same oracle)
       → BTC via SPV → bUSD adds BTC collateral, ceiling grows
       → bridged USDT/USDC as extra liquidity, not as the base
   ```
3. **Release v1 scope** ([b3-release-v1.md](b3-release-v1.md)): B3 Hive v1 ships FlowMesh
   activation-inert. bUSD is a FlowMesh-era feature and cannot be in that release; "v1"
   in this proposal means "first FlowMesh quote asset", not the hard-fork client.
4. **Colored-asset open items** (handoff §10): final AssetId preimage, policy abstraction,
   activation heights — bUSD inherits all of them.

## 7. Owner decisions required before any code

- policy name (`CDP_BACKED` vs keep `ALGORITHMIC`);
- oracle: source set, median/TWAP windows, bounded move, staleness, breaker, penalties;
- MCR, CCR, debt-ceiling formula and its inputs, mint/redemption fees, stability-pool
  rewards (asset + schedule);
- bad-debt order and whether a surplus buffer exists;
- CDP as a third state domain (extension of L-5) — confirm;
- re-ruling or not of L-3 (fee asset);
- resequencing (§6.2) — confirm;
- launch collateral set (B3 only) and the rule by which BTC is added.

## 8. Build shape when approved (mirrors the spot approach)

1. **Standalone deterministic CDP harness** (header-only, test-only, like `src/flowmesh/`):
   vaults, oracle input model, liquidation, stability pool, redemption, recovery mode,
   ceiling formula — with adversarial tests: oracle wick, mass liquidation, empty stability
   pool, redemption run, rounding/overflow, determinism across input orderings.
2. **Simulation** of crash scenarios against the chosen parameters (the same discipline
   the PoS v1 brief applies to its numerics).
3. **Consensus wiring** only after (1)–(2) and after the spot/vault/microblock stack is
   live: policy module, CDP state domain, oracle actions, anchoring — behind an activation
   height, activation-inert until then.
4. **Whitelist entry** for bUSD as an approved quote (and, if re-ruled, fee) `AssetId`.

Nothing in steps 1–2 touches consensus; nothing in step 3 starts before the owner rules
on §7.
