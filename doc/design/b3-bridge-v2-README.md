# B3 Bridge V2: overview

> **PROPOSAL, no governing authority.** 2026-10-03, base `b4b1de4`, branch
> `feat/flowmeshV2-dev-claude`. Nothing is built, audited, deployed or
> activated; no node or chain was contacted. Minutes assume B3's computed
> ~78 s blocks, unmeasured on mainnet (O8). [H] = estimate.

## The idea

Two new keyless vaults hold USDT on Ethereum and USDT0 on Arbitrum One. B3
mints one bUSD only when its Ethereum light client proves a deposit
(Arbitrum's through Arbitrum's confirmed state on Ethereum). To withdraw,
burn bUSD naming a vault; B3's stakers certify it under a new finality
lineage, anyone relays that, and the vault pays. B3 refuses burns a vault
cannot pay. No signer group enters B3 consensus; no USDC; the old contracts
expire; regtest comes first.

## Routes

Numbers copied from architecture §7. *Minutes*: to bUSD on B3, or arrival.
*FlowMesh V1*: until usable there, or withdrawals starting there. All routes
also trust B3's stakers, Ethereum's sync committee and Tether. Gas at
0.08/1/10 gwei.

| Route | Minutes | FlowMesh V1 | User cost | Extra trust |
|---|---|---|---|---|
| (a) Ethereum USDT → Ethereum vault | 16-24 | 56-66 | ~216-221k gas [H]: $0.05/$0.58-0.59/$5.78-5.92 | — |
| (b) Arbitrum USDT0 by LayerZero → Ethereum vault | 17-28 [H]; 32-48 if LayerZero's verifiers wait for L1 | 57-70 [H] | LayerZero fee; capped relayer fee for F_d gas, first use $0.07-0.09/$0.86-1.10/$8.6-11 [H] | USDT0, LayerZero (transit) |
| (c) Arbitrum USDT0 → Arbitrum vault, LP fill | 2-4 (LP trusts the sequencer) or 15-26 (after Ethereum finality) [H] | 42-46 or 55-68 | ~$0.01-0.02 gas + an LP fee equal to the user's cap (default 30 bps) [H] | USDT0, LayerZero, Arbitrum (reserve) |
| (c) without an LP | ~6.4-6.5 days (~14.7 disputed) | +40-42 | Gas only | Same |
| (d1) Ethereum vault → Ethereum; (d2) Arbitrum vault → Arbitrum | 29-47; (d1) to 77 if batched | 69-89 + checkpoint wait [H] | B3 fee in native B3 | (d2): USDT0, LayerZero, Arbitrum (reserve) |
| (d3) Ethereum vault + LayerZero → Arbitrum | 30-50 [H]; to 80 if batched | 70-92+ | Plus LayerZero fee and gas, capped | USDT0, LayerZero |

**Plain reading.** The 30-60 minute target holds only with full
participation, healthy finality, prompt relays and amounts under caps. Then
deposits (a), (b) and (c) with a funded, online LP (none exists yet) meet it;
withdrawals meet it only at ≤ 1 gwei or with fast relays (batched Ethereum
relays take up to 77-80 minutes). At 100 s blocks withdrawals take up to 58
minutes, at 150 s up to 85. FlowMesh V1 is borderline; withdrawals starting
there, and the waiting relayer (72-109), miss it. Caps make $50,000 into one
vault take 5-6.5 days.

## Timeline

- **Now (S0):** warn users off the old vault `0x0778…c64b`; owner reads O2,
  O8-O12; commit this direction.
- **~2026-10-04 20:00 UTC:** the V1 light-client pins end (epoch 479,999):
  legacy V1 updates and mints stop (node changes §3.4).
- **2026-10-08 17:11:59 UTC:** the old Ethereum verifier lapses (accepted),
  stranding its 3 test USDT; relaying 814,051 first is optional (B-2).
- **Once reviewed (S1):** a hard fork for B3's health: **H1** rotates
  producers (Set_3 since ~815,321, stuck since ~826,841 [H, O2]) every 1,440 blocks with a 24 h
  escape; **H2** hardened checkpoint (RT1).
- **In parallel (S2):** harness on local anvil chains (RT0, RT2, RT3).
- **Then (S3-S4):** fresh lineage, new contracts, node rules, forwarders;
  every route runs on regtest with today's signer before P3 (RT4-RT7), then
  drills (RT9).
- **No date (S5):** **V2 P3** starts the fresh lineage on mainnet (RT8);
  needs V2-D and FD rulings, a model check, a soak, a hard fork (finality §14).
- **After S5:** audit; deploy both chains (RT10); 7-day review; rehearsal;
  open at pilot caps.

## Decisions

Owner-ruled 2026-10-03 (relayed, not yet in the repo): fresh start, one
bUSD, no USDC, both chains, 30-60 minutes, regtest first, and acceptance of
Claude's defaults; OD-8's signer-set ban stands. **B-11 departs from an
owner-accepted default and needs a ruling;** other bracketed defaults stay
open to ruling.

| IDs | Decision [default] |
|---|---|
| B-1, B-2 | Old stack lapses (ruled); relay 814,051? [skip unless O9 finds a burn leaf ≤ 814,051] |
| B-3, B-20 | One bUSD; record the OD-8 and architecture contract §21 overrides first [yes] |
| B-4, B-5 | Routes (a)-(d), Tron guidance only; LP fast-fill in consensus, LP fee = user's cap, one fill per bucket [yes; wallet cap 30 bps] |
| B-6 | H1 plus a hardened checkpoint, before the bridge [yes; needs the CLAUDE.md override, timing, Set_3 coordination] |
| B-7-B-9 | Mainnet digest V2 only from P3; fresh lineage, new journals; each certificate extends the last; older roots stay payable [yes] |
| **B-11** | Bootstrap from a recent certified checkpoint by one ordinary certificate [**departs from the default the owner accepted on 2026-10-03, a time-bounded k-of-n attestation: needs a ruling**; k-of-n fallback] |
| B-10, B-12 | Deposits close 24 h without a certificate or 72 h without a rotation; 30-day expiry; deposits need a live verifier and healthy token; burns open at V [yes] |
| B-13, B-14, B-18, B-22 | Per-vault burn gates; caps 10,000 per block and epoch per vault; Outbox pins; committee: top 64 by stake above a W/128 floor [yes; raise caps after audit] |
| B-15-B-17 | Capped user fees plus owner relayer (baseline ~$32/$324/$3,190 a month at 0.08/1/10 gwei); chaining relayer; native B3 to burn [yes] |
| B-19, B-21 | Legacy bUSD frozen (O9); separate P3 and pin releases [yes] |
| B-23 | Resumable lineage (extension ≥ 46,080 blocks); migration leaf; residual stranding [yes; needs explicit owner acceptance] |
| B-24 | Adapter gate: USDT by kind 9; USDT0 upgrades unobservable [recorded USDT0 waiver] |
| C-n, FD-n, N-n, RP-n | Contract (contracts §13), finality (finality §15), node (node §16) and regtest (regtest §9) choices [yes; N-1 (new deposits backstop dead-vault losses) needs acceptance] |

## Biggest risks

- **No mainnet date**: no bridge before V2 P3 and the node V2 regime (a
  consensus change the size of V1's bridge code), both unbuilt.
- **Chain health now**: frozen producers exclude new stakers; B3 halts
  while no Set_3 key is online, permanently only if those keys are lost; H1
  ends this.
- **Permanent stranding**: a fresh-lineage break strands both vaults, 100% of
  backing. B-23 makes the lineage resumable, but 30 days without a relayed
  rotation, or a conflict halt, still strands: the price of no admin key, for
  explicit owner acceptance.
- **Staker forgery**: two-thirds of 4-64 unslashable keys can drain a vault,
  beyond any cap. **Conflicting certificates** halt each receiving verifier
  for good; P3 prevents them only while faulty keys stay below one third.
- **Shared losses**: a Tether, USDT0, LayerZero or Arbitrum failure hits
  every holder, first come first served; Arbitrum trouble shows a week late.
- **No refund** once funds reach a vault; a withdrawal is lost if the
  recipient is blacklisted or the verifier expires first.
- **Unproven**: unaudited contracts; mocks are not real chains; gas and
  minutes are estimates.

## Documents

[Architecture](b3-bridge-v2-arbitrum-proposal.md) (trust, numbers, stages,
B-n); [contracts spec](b3-bridge-v2-contracts-spec.md);
[node changes](b3-bridge-v2-node-changes.md);
[finality dependency](b3-bridge-v2-finality-dependency.md) (H1, P3);
[regtest plan](b3-bridge-v2-regtest-plan.md).
