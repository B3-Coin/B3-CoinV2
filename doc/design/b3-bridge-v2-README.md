# B3 Bridge V2: overview

> **PROPOSAL, no governing authority.** 2026-10-04, the **simpler plan**,
> base `1d1d6da`, branch `feat/flowmeshV2-dev-claude`. Nothing built,
> audited, deployed or activated; no node or chain contacted. Minutes assume
> computed ~78 s blocks (O8); [H] = estimate.

**Owner ruling, 2026-10-03** (relayed, uncommitted): "ok then I think
simpler plan , and its without LP ? I have no issue from LP, but just doesnt
know why we need it".

## The idea

One new Ethereum vault with no admin key holds USDT and backs every bUSD
1:1. B3 mints bUSD only when its Ethereum light client proves a deposit; no
signer can. Arbitrum users send USDT0 over LayerZero to a personal Ethereum
forwarder that deposits for them. To withdraw, burn bUSD; B3's stakers (at
most 64 keys, no slashing) certify it under a fresh finality lineage,
anyone relays it, and the vault pays on Ethereum or forwards to Arbitrum. The old contracts expire; legacy bUSD stays frozen;
regtest comes first.

**Why no LP.** An LP only hides the ~6.4 days B3 needs to prove a deposit
into an *Arbitrum* vault; without such a vault it adds cost and risk for
nothing (architecture §18).

## Routes

From architecture §8: prompt relays; gas at 0.08/1/10 gwei; *FlowMesh
V1* = until usable there, or withdrawing from there. All routes trust B3's
stakers, Ethereum's sync committee and Tether.

| Route | Minutes | FlowMesh V1 | User cost | Also trusted |
|---|---|---|---|---|
| (a) Ethereum USDT → vault | 16-24 | 56-66 | $0.05/$0.58-0.59/$5.78-5.92 gas [H] | — |
| (b) Arbitrum USDT0 → LayerZero → F_d → vault | 17-28 [H]; 32-48 if its verifiers wait for L1 | 57-70 [H] | LayerZero fee plus a fresh F_d's capped gas, $0.07-0.09/$0.86-1.10/$8.6-11.0 [H] | USDT0, LayerZero, Arbitrum, in transit only |
| (d1) vault → Ethereum | 29-47; to 77 batched | 69-92+ [H] | B3 fee (native B3) | — |
| (d3) vault → F_x → USDT0 → Arbitrum | 30-50 [H]; to 80 batched | 69-92+ [H] | Plus LayerZero fee and release + forward gas, capped | As (b) |

Tron: guidance only. **Plain reading:** with full participation and a
working lineage, deposits meet the 30-60 minute target; withdrawals do at
≤ 1 gwei or with fast relays; at 150 s blocks, up to 85 minutes. Caps
spread a lone $50,000 deposit over about 3-5.2 days, longer when routes
compete.

## Timeline

- **Now (S0):** warn that transfers to old vault `0x0778…c64b` are lost;
  owner reads O2, O8, O9, O11.
- **2026-10-04 20:00:23 UTC:** V1 light-client pins end; V1 mints stop.
- **2026-10-08 17:11:59 UTC:** old verifier lapses (B-1), stranding 3 test
  USDT.
- **S1, once reviewed:** **H1 + H2** hard fork for B3's own health
  (producer rotation with a 24 h escape; a hardened checkpoint).
- **S2-S4:** regtest harness (in progress), lineage, contracts, node
  rules, forwarders; every route on regtest.
- **S5, no date:** **V2 P3** starts the mainnet lineage (RT8).
- **S6-S9:** audit, drills, deploy, 7-day review, pilot caps.

**When usable, honestly.** No mainnet bridge until opening: V1 mints stop
today; V1 burns close if the lineage broke. H1 on regtest: ~2-4
person-weeks [H]; full regtest: not re-estimated; P3 to mainnet: ~3-9
months [H] (finality §14). No date is promised.

## Ruled (owner, 2026-10-03; B-1, B-3, B-4)

Fresh start; one bUSD, backed 1:1 only by the Ethereum vault; the four
routes above; no USDC; no LP at launch; no signer minting (OD-8 stands);
regtest first; 30-60 minutes acceptable.

## Decisions

Defaults in brackets (architecture §15).

| IDs | Decision [default] |
|---|---|
| B-2 | Relay 814,051 before Oct 8? [skip unless O9 finds a burn leaf] |
| B-6 | H1 + H2 first [yes; rule-3 override, timing ruling] |
| B-7 to B-9 | Digest V2 from P3; fresh lineage; accepted roots stay claimable, failed transfers consume nothing [yes] |
| B-10, B-12 | Deposits close 24 h without a certificate or 72 h without a rotation; 30-day expiry; deposit gate [yes] |
| B-11 | Bootstrap by a time-bounded k-of-n attestation, default 3-of-4 [accepted in conversation, uncommitted, conditional]; **a recommended start-certificate alternative needs a ruling** |
| B-13, B-14, B-24 | Burn gate; caps 10,000 per block and per epoch; USDT adapter gate [yes] |
| B-15, B-16 | Capped forwarder fees; treasury pays certificates and (d1) releases (1 gwei: ~$318/month, ~$5.62 per lone withdrawal) [yes] |
| B-17 | Burns pay native B3 [disclose] |
| B-19 to B-21 | Legacy bUSD frozen, owner swap if O9 finds third-party holders; reconcile governing text, OD-8 untouched; separate releases [yes] |
| **B-22** | Committee: top 64 by stake above a W/128 floor [**needs explicit owner approval**] |
| **B-23** | Resumable lineage; migration leaf credited once absorbed, later deposits swept [**residual stranding needs owner acceptance**] |
| **B-37 (new)** | Fresh per-epoch BLS keys under their own binding kind, erased after a deep handover; past epochs only, as the identity key still binds [yes] |

**Withdrawn (2026-10-03 ruling):** B-5, B-18, routes (c) and (d2), N-1,
N-2, Arbitrum parts of B-10, B-13, B-24.
**Deferred:** B-25 to B-36.

## Later options

Each needs a ruling (architecture §17): an **Arbitrum vault** (~6.4-6.5
days to prove; optional LP fast-fill); **staker-signed fast deposits**
(needs that vault); an **overcollateralized coin**, never bUSD.

## Biggest risks

- **Chain health:** frozen producers halt B3 if no Set_3 key is online [H,
  O2]; H1 fixes it.
- **Stranding:** a lineage break, 30 days without a relayed rotation, or a
  conflict halt strands up to 100% of backing, the price of no admin key; a
  conflict closes deposits a certificate later.
- **Forgery:** two-thirds of ≤ 64 unslashable keys can drain the vault,
  beyond caps.
- **Tether** can freeze the reserve; deposits are not refunded.
- **Unproven:** unaudited; figures estimated.
- **Open issues:** OI-1 to OI-12 (architecture §20): OI-8 resolved by owner
  ruling R10 (sponsored native-B3 fees), OI-12 by R9 (bootstrap binding);
  the rest unresolved, including a bootstrap that could install a
  permanent root (OI-1).

## Documents

[Architecture](b3-bridge-v2-arbitrum-proposal.md) (scope, numbers,
decisions); [contracts](b3-bridge-v2-contracts-spec.md);
[node](b3-bridge-v2-node-changes.md);
[finality](b3-bridge-v2-finality-dependency.md);
[regtest](b3-bridge-v2-regtest-plan.md);
[staker-signed deposits](b3-bridge-v2-staker-signed-deposits.md).
