# B3 Bridge V2 launch plan: one keyless Ethereum vault, Arbitrum users through USDT0

> **Status: PROPOSAL, no governing authority.** Below every owner ruling and
> every reviewed, committed governing document; nothing ratified, built,
> audited, deployed or activated. Revision 9, 2026-10-04, the **simpler
> plan**, with review fixes (§20), base `1d1d6da`, branch
> `feat/flowmeshV2-dev-claude`. Revision 8 (two vaults, Outbox proofs, LP
> fast-fill; this file at `1d1d6da`) is withdrawn for launch, kept as a
> later option (§17). No node or chain RPC was contacted.

**Owner ruling, 2026-10-03** (relayed, not yet in the repo): one fresh
Ethereum vault, Arbitrum users through USDT0's LayerZero bridge, no
liquidity provider (LP), no signer minting: "ok then I think simpler
plan , and its without LP ? I have no issue from LP, but just doesnt know why
we need it". Earlier that day: "30 mintues to 1H is okay", no USDC, "I need
everthing to be working atleast on regtest".

**Labels.** [E] established or verified; [I] design choice; [H] estimate.
**Default** = provisional choice, open to owner ruling. Repo facts are
`path:line` at `1d1d6da` (another workflow's uncommitted edits shift
`src/kernel/chainparams.cpp` lines after 1080); `.../study/` means
`/Users/josh/development/ON/B3-FlowMesh-200ms-evidence/arbitrum-bridge-study-20261003/`.
"Codex review" is the external review relayed on 2026-10-03, not in the
repo.

**Map.** This document owns scope, trust, numbers (§8), stages (§14),
decisions (§15) and open issues (§20). Siblings:
[contracts spec](b3-bridge-v2-contracts-spec.md) (byte layouts, AssetId
preimage excepted), [node changes](b3-bridge-v2-node-changes.md),
[finality dependency](b3-bridge-v2-finality-dependency.md),
[regtest plan](b3-bridge-v2-regtest-plan.md), [README](b3-bridge-v2-README.md),
[staker-signed deposits](b3-bridge-v2-staker-signed-deposits.md).

---

## 0. Summary

**Shape.** One new keyless Ethereum stack: **Verifier V2**, a **BLS digest
prover** and **Vault V2**, whose USDT
(`0xdAC17F958D2ee523a2206206994597C13D831ec7`) alone backs bUSD V2. B3 mints
only from Ethereum light-client proofs; the vault pays only against
withdrawal roots certified by B3's staker committee (≤ 64 BLS keys, no
slashing) under a new finality lineage. The old stack expires; legacy bUSD
stays frozen. No signer mints, no LP, no Arbitrum vault, no USDC: OD-8
stands.

**Why no LP.** An LP only hid the ~6.4 days B3 needs to prove a deposit into
an *Arbitrum* vault. Arbitrum users instead send USDT0 over LayerZero to a
personal Ethereum forwarder that deposits for them. Deposits take 16-28 min
and withdrawals 29-50 min under §8.5's conditions [H].

**Finality.** If owner read O2 confirms the broken lineage, B3 needs **H1**
(scheduled producer rotation) and **H2** (a hardened checkpoint) for its own
health (§6); mainnet waits for **V2 P3**, regtest uses the V1 signer.

**Largest risk.** If the verifier can never accept another root, every
unreleased dollar is stranded for good (§9.1); B-23 narrows this; the
residual needs explicit owner acceptance. **Regtest first** (§14). Open
issues: §20.

---

## 1. Goals

| Owner goal | Launch plan |
|---|---|
| Deposits and withdrawals in 30-60 minutes | Only under §8.5's conditions; latency is a target, not a consensus fact (`doc/design/b3-master-handoff.md:1013`) |
| No signer set in B3 consensus (OD-8, `doc/design/b3-open-decisions.md:306`) | Met: mints need light-client proofs only |
| Simple, no LP | Met |
| Arbitrum users served | By (b) and (d3), paying forwarder gas within a capped fee (§8.3) |
| No USDC | Met (§18) |
| Base-chain liveness never depends on an issuer or bridge (`doc/design/b3-master-handoff.md:387`) | Met |

---

## 2. Why one Ethereum reserve at launch

1. **Proof latency.** B3's light client proves an Ethereum deposit once
   Ethereum finalizes it (13-19 min), but sees Arbitrum only through state
   confirmed on Ethereum: next assertion ≤ ~1 h plus 45,818 L1 blocks ≈
   6.36 days, ~14.7 if disputed [E] (`.../study/check.md:78-92`). An Arbitrum-vault deposit thus mints after
   ~6.4-6.5 days unless an LP fronts it or signers vouch (OD-8 forbids it);
   USDT0 over LayerZero reaches the Ethereum vault in minutes.
2. **Arbitrum risk stays out of the backing** (Codex review): the reserve is
   Ethereum USDT only; USDT0's 3-of-5 Safe, LayerZero's verifier networks
   (DVNs) and Arbitrum's 9-of-12 Security Council affect only coins in
   transit.
3. **Less to build and audit.** No Outbox proofs, heartbeats, LP rule, mint
   queue, per-vault ledgers or Arbitrum clock skew; no override of OD-8
   `:309` or architecture contract §21.
4. **Cost.** Arbitrum users pay the LayerZero fee and forwarder gas (§8.3),
   not a 6.4-day wait or an LP's 30 bps.

---

## 3. The old Ethereum stack lapses

Verifier `0xE72B3Fe73F0d42A6e964D33E7BB1cc2EA7a3F690`, prover
`0x8e612aE4D475d25940E2A2FC907F21b6813eedA7` and vault
`0x077839b12cebfbF163acAEAC3A59A015D100c64b` expire: the verifier refuses
certificates 30 days after its last rotation
(`contracts/src/B3FinalityVerifier.sol:211-213`), at
**2026-10-08T17:11:59Z** [E] (`.../study/check.md:110-118`). Its root is the
empty tree, so the vault's 3 test USDT are stranded for good, with no
rescue path (`contracts/src/B3StakerBridge.sol:95-160`) [E].

- **B-2 default: skip** relaying 814,051 (~$0.40), **unless O9** finds a
  burn leaf at or below 814,051: burns were valid from 811,001
  (`src/kernel/chainparams.cpp:270`), and only that relay could pay one.
- **Notice:** plain transfers to `0x0778…c64b` are lost.
- **Legacy bUSD** (raw `0xc69c6bd5…61ad`, `src/test/bridge_params_tests.cpp:216-228`)
  stays distinct, "bUSD (legacy V1)", burns closed
  (`src/node/finality_tracker.cpp:46-63`; B-19); supply at most those 3 USDT
  [I; O9]. If O9 finds holders beyond the owner's test wallets, the owner
  swaps off consensus, depositing by route (a) what each sends to a
  published address (OD-8 `doc/design/b3-open-decisions.md:306`).
- **Smoke vault** `0x143F207e23e6aebD7E974be90ac6D434f4c7BFb6`: recheck for
  USDT liability or consensus bUSD before activation
  (`doc/release-notes-v1.1.1.md:88-93`; `doc/design/b3-open-decisions.md:253-256`).

---

## 4. The launch model

```
ETHEREUM L1 (USDT)                                     B3
(a) EOA -> Vault.deposit ---------- Deposit log ---->  light client (sync committee)
(b) Arbitrum USDT0 -> OFT/LayerZero -> F_d -> flush      receipt proof -> mint bUSD V2
        -> Vault.deposit                               reserve = minted - burn leaves
WITHDRAWAL: burn bUSD V2 -> leaf {id, chain 1, vault, USDT, recipient, amount, height}
  -> fresh-lineage certificate, digest V2 (P3 on mainnet) -> relayed to Verifier V2
  -> anyone: Vault.release -> recipient (d1)  |  -> F_x -> USDT0 OFT -> Arbitrum (d3)
```

Contracts: **Verifier V2** (dual quorum: stake ≥ ⌊2W/3⌋+1 **and**
headcount ≥ ⌊2n/3⌋+1); **BlsDigestProver**, V1's EIP-2537 code redeployed
to take a digest (the deployed prover hard-codes the V1 tag,
`contracts/src/BlsCertificateProver.sol:158-176`); **Vault V2**; per-user
CREATE2 forwarders **F_d** and **F_x**, payable before deployment and
refundable to the user's Ethereum key; optional **B3RelayBatch**.

### 4.1 Improvements over V1 [I unless marked]

1. **Digest V2** binds `lineageId` and a cumulative checkpoint-history root;
   a certificate proves the last accepted checkpoint is in its history.
2. **Accepted roots stay claimable; failed transfers consume nothing**
   (Codex review; V1 checks only the latest root,
   `contracts/src/B3StakerBridge.sol:149-151`, and reverts failed transfers,
   `:154-156`).
3. **Windows (B-10).** Deposits close **24 h without a certificate or 72 h
   without a rotation** (V1: 23 days); expiry 30 days; rotation spacing
   **6 h** for catch-up (contracts spec §4).
4. **Deposit gate (B-12).** Custody needs a live verifier with a root at or
   above activation height V, and USDT not paused, deprecated or
   blacklisting the vault; deposits only from a codeless EOA's top-level
   call or an F_d account (contracts spec §6.2); burns open at **W2 = V**;
   releases are never gated.
5. **Bootstrap (B-11)** by a **time-bounded k-of-n attestation, default
   3-of-4** (as V1, `contracts/src/B3FinalityVerifier.sol:93-190`; accepted
   in conversation 2026-10-03, uncommitted, conditional on no more
   trust-minimized option): four published keys sign one statement of a
   pinned recent epoch, bound to the verifier address, the EVM chain id and
   the pinned start set (OI-12, resolved by owner ruling R9); watchers may
   only cancel during review. Recommended alternative, needing a ruling
   (perhaps the option that condition anticipates): any ordinary
   certificate of that epoch (contracts spec §5; OI-1, OI-9).
6. **Migration (B-23)** by an ordinary leaf paying a pinned successor
   (§9.1).

### 4.2 Gas

[E, V1 measured] 64 members with 21 absent 5,513,351 (6,283,311
conservative), under Ethereum's 16,777,216 per-transaction cap
(https://eips.ethereum.org/EIPS/eip-7825)
(`.../study/check.md:30-59`; `doc/design/b3-cross-chain-finality-v1.md:334-342`).
At ~0.22M per absent member [H], ~70 absent would exceed it, while B3
allows sets up to 8,192 (`src/modern/finality_types.h:42`); B-22 bounds the
committee. V2 gas is [H].

---

## 5. bUSD V2 and its reserve

- **Identity [I].** A new AssetId bound to the Ethereum origin, USDT, Vault
  V2 and six decimals, as OD-8 (`doc/design/b3-open-decisions.md:306`) and
  `doc/design/b3-architecture-contract.md:636-643` require; distinct from
  legacy V1 (`src/modern/bridge_asset.h:24-32`). Node changes owns the
  preimage; a B-23 migration keeps the id under an exception its hard fork
  records (contracts spec §6.1).
- **Mint** once per nullifier (chain, vault, id)
  (`src/bridge/admission.h:196-207`) [E]; `reserve` = minted − burn leaves.
- **Burn gate (B-13).** EIP-1186 proofs of verifier, vault and USDT state
  against a light-client-proven `stateRoot`, which B3 proves but does not
  store today (`src/bridge/ssz.h:167-203`; `src/node/bridge_state.h:63-76`).
- **Wind-down.** A burn is accepted only while it plus unreleased leaves
  fits the last observed USDT balance less a margin, so after a partial
  seizure earlier leaves keep first claim.
- **Adapter gate (B-24).** USDT upgrades by `deprecate`, which forwards
  calls; B3 proves `deprecated` and the code hash, and deprecation closes
  burns until a ruled re-pin.
- **USDT fee switch** (below 20 bps): releases deliver the net [E]. A plain
  `burnasset` leaves no leaf; wallets refuse it without an explicit flag.

---

## 6. Finality: what the bridge needs, and B3's own repair

**Facts [E].** Under V1 producers are the finality set
(`src/validation.cpp:2574`), changing only after a same-epoch certificate
(`src/node/finality_tracker.cpp:123-131`); `lineage_broken` is never cleared
(`:133-137`). V1 voting deadlocked at
811,631 (`doc/release-notes-v1.1.3.md:58-106`) and 814,191
(`doc/finality-quorum-deadlock-814191.md:16-20`); mainnet is expected broken
since ~826,841, finalized at 814,051, producers frozen at Set_3 [H; O2],
which excludes new stakers, lets an unstaked member produce and halts the
chain while no Set_3 key is online (`src/modern/pos_v1.h:109-123`); fresh
nodes are protected only to 811,641 (finality §0 item 2).

| Item | Default |
|---|---|
| **H1 + H2** (B-6) | Hard fork: producers rotate every 1,440 blocks with a 24 h escape; a hardened checkpoint; no BLS or bridge change. Needs a rule-3 override, a timing ruling (`doc/design/b3-cross-chain-finality-v1.md:172-173`), Set_3 coordination |
| **Fresh lineage** (B-8) | Genesis Snapshot(R_f − 1), tag `B3/FINALITY/V2`, own journals and tracker; resumable (B-23) |
| **Committee** (B-22) | Stake-ranked top 64 above a W/128 floor; producers keep the full snapshot. **Needs explicit owner approval** (Codex review; `doc/design/b3-flowmesh-v2-stage0.md:711`) |
| **Signing keys** (B-37) | Per-epoch keys (below) |
| **Producer** (B-7) | Mainnet **V2 P3** (PBFT-style CommitQC), never yielding conflicting certificates while faulty keys stay below one third, unlike V1 voting; regtest the V1 signer under a new tag and journals |
| **Order** | H1 before any mainnet fresh lineage, or producers following it freeze after 11,520 blocks of quorum failure (`src/kernel/chainparams.cpp:292-295`) |

**Independent per-epoch keys (B-37, Codex review).** Today's finality BLS
key comes from the identity secret or one imported wallet key
(`src/wallet/wallet.cpp:3937-3949`, `:4030-4031`;
`src/wallet/wallet.h:1051-1067`), so an old backup yields it. Default [I]: a
fresh key per committee epoch under a **binding kind of its own**, kept
outside the wallet file and erased once the handover out of its last seated
epoch is certified and 1,440 blocks deep; V1 bindings, signing and producer
eligibility stay unchanged (finality §6.4; node changes §7.7). **Limit:**
past epochs only, since the identity key in every backup
(`src/wallet/wallet.h:1037-1038`) can bind a key for a coming epoch; a
double binding voids the seat and raises an alert, and a leaked identity
secret calls for restaking. A member without a fresh binding sits out (the
S5 rehearsal must show no quorum cost).

---

## 7. Who verifies, signs and authorizes

**B3 block validity alone authorizes mints and burns.** A mint needs a
sync-committee supermajority (342 of 512), execution ancestry, a receipt
proof of Vault V2's `Deposit`, caps and a nullifier
(`src/node/bridge_state.cpp:537-664`) [E]; a burn, the wind-down bound and
an open gate. **The committee** (≤ 64) accepts roots by dual-quorum BLS
over digest V2. **Anyone** may `release` an unreleased leaf under a root
accepted at or above its height, or `flush` and `forward` within fee caps.
Nobody's signature mints bUSD.

---

## 8. Latency and cost: the single set of numbers

### 8.1 Method

B3 legs use the **computed ~78 s at full participation**
(`src/modern/pos_v1.h:74-79`; `doc/design/b3-finality-benchmark-2026-08-23.md:12-16`),
unmeasured (O8); at 50% online stake 95% of blocks arrive within ~150 s. The
**default relayer** chains dependent B3 transactions (a reorg undoes both;
the nullifier stops double mints); the **wait variant**, today's relayer,
waits for B3 finality per step
(`contrib/b3bridge/b3_bridge_relayer.py:1744-1761`).

| Code | Step | Blocks | Minutes | Basis |
|---|---|---|---|---|
| t_ef | Ethereum finality after inclusion | — | 13-19 | [E/H] https://ethereum.org/en/developers/docs/consensus-mechanisms/pos/#finality |
| t_lz | USDT0 LayerZero delivery | — | 0.5-3; +15-20 if DVNs wait for L1 [H] | https://docs.usdt0.to/technical-documentation/developer/ |
| t_fl | Relayer check and F_d flush | — | 0.5-1 | [I] |
| t_chain | Default relayer: light-client update then mint | 2-4 | 2.6-5.2 | [I] |
| t_fin | B3 finality: inclusion 1 + checkpoint 0-9 + export depth 20 + carrier 1-2 | 22-32 | 29-42 | [E] `src/kernel/chainparams.cpp:293-294`; `src/node/finality_signing_policy.h:16` |
| t_wait | Wait variant: two waits plus mint (best 23-34 blocks, 30-44 min) | 45-66 | 59-86 | [I/H] |
| t_fm | Into FlowMesh V1: USER_DEPOSIT plus 30-block anchor | 31-32 | 40-42 | [E] `src/node/flowmesh_anchor.h:31-41` |
| t_rel | Relay certificate and release, prompt | — | 0.2-5 | [I] |

Routes add step minutes, rounded half up. **Sensitivity** [I computed]:

| Spacing | t_fin | Withdrawal (d1) | (a) usable on FlowMesh V1 |
|---|---|---|---|
| 78 s (default) | 29-42 min | 29-47 | 56-66 |
| 100 s | 37-53 | 37-58 | 68-79 |
| 150 s (95th pct at 50% online) | 55-80 | 55-85 | 96-109 |

At the 60 s floor B3 legs shrink 23%. Withdrawals pass 60 minutes at the
upper bound above ~103 s per block, at the lower above ~163 s. **O8 and O2's
online fraction gate any user-facing time** and `DEPOSIT_CLOSE_AFTER` (72 h
holds to ~160 s, contracts spec §4). Prices
(2026-10-03): ETH $2,677.62; gas 0.08, 1 and 10 gwei (measured 0.063-0.077:
`.../study/check.md:125`; `.../study/impact.md:53`, `:318`;
`.../study/arb-facts.md:214`).

### 8.2 Deposits

| Route and steps | bUSD on B3, default | Wait variant | Usable on FlowMesh V1 |
|---|---|---|---|
| **(a)** Ethereum USDT: `approve` + `deposit` from an EOA, or a transfer to F_d; t_ef, t_chain | **16-24 min** | 72-105 (best 43-63) | 56-66 |
| **(b)** Arbitrum USDT0: OFT `send` to a fresh F_d (dstEid 30101), which a relayer flushes (contracts spec §7.4); t_lz, t_fl, t_ef, t_chain | **17-28 [H]** (32-48 if DVNs wait for L1) | 73-109 [H] | 57-70 [H] |

**(e) Tron:** guidance only, the Legacy Mesh to Arbitrum (0.03%,
https://docs.usdt0.to/overview/the-legacy-mesh) and then (b), or an exchange.

### 8.3 User cost

| Route | Ethereum gas the user pays, 0.08 / 1 / 10 gwei | Total at $100 / $1,000 / $10,000 |
|---|---|---|
| (a), ~216-221k gas [H]: V2 deposit 170-175k plus a USDT `approve` of ~46k; V1's deposit measured 161,975 (`.../study/check.md:24`) | $0.05 / $0.58-0.59 / $5.78-5.92 | Same at every amount (plus the USDT fee if on) |
| (b), fresh F_d per deposit (wallet default): deploy + flush 320-410k [H], in the capped relayer fee | $0.07-0.09 / $0.86-1.10 / $8.6-11.0 | Same at every amount, plus the LayerZero fee, which also pays `lzReceive` and DVNs [H, O11] |
| (b) reused F_d with cap headroom: flush 200-250k [H] | $0.04-0.05 / $0.54-0.67 / $5.4-6.7 | ditto |

### 8.4 Withdrawals

| Route | Prompt relays | Ethereum batched (> 1 gwei) | User pays |
|---|---|---|---|
| **(d1)** vault → Ethereum recipient | **29-47 min** | 29-77 | B3 fee (native B3) |
| **(d3)** vault → F_x → USDT0 OFT → Arbitrum | **30-50 [H]** | 30-80 [H] | Plus LayerZero fee and `release` + `forward` gas from the amount, capped |

Starting in FlowMesh V1 adds t_fm and a checkpoint wait (69-92+ min [H]).
Burns need native B3 fees (`src/wallet/rpc/assets.cpp:1985-2020`; B-17;
OI-8). At an epoch start a burn needs the new rotation relayed, normally
within one t_fin [H]; after a relay outage, catch-up runs ~3.2 epochs a day.
"Prompt" assumes the treasury relays every certificate and (d1) release,
and a relayer batches (d3)'s `release` and `forward` for the F_x fee (§11).

### 8.5 Against the 30-60 minute target

| Journey | Default relayer | Wait variant |
|---|---|---|
| Deposits (a), (b) | **Yes** (16-28; (b) up to 48 if DVNs wait) | **No** (72-109) |
| Deposits into FlowMesh V1 | Borderline (56-70) | **No** |
| Withdrawals (d1), (d3) | **Yes at ≤ 1 gwei or with P-fast** (29-50); **No** when batched (to 80) | same |
| Withdrawals from FlowMesh V1 | **No** (69-92+) | same |

Every row assumes full participation, a working lineage, prompt relays and
inflow under caps.

---

## 9. Exactly-once, no refund, and stranding

**Deposits.** One `deposit()` (or flush) emits one `Deposit` with an
increasing id (`contracts/src/B3StakerBridge.sol:95-114`) [E]; the nullifier
is undone only with its mint [E]; a LayerZero message credits once
(https://docs.layerzero.network/v2/developers/evm/composer/overview) [E].
F_d funds refund to the user's Ethereum key before custody, **none once in
the vault**, hence the caller rule: every accepted deposit must be mintable.

**Withdrawals.** One burn, one leaf with a global id
(`src/node/bridge_state.cpp:767-776`) [E], paid once (`released[id]`);
failed transfers leave it payable; accepted roots stay claimable, even after
a halt, and lie on one B3 branch while faulty keys stay below one third.
**No refund on B3**; a blacklisted recipient receives frozen tokens (§10),
so wallets pre-check.

### 9.1 Stranding

Vault tokens leave only by release of a certified leaf. If the verifier can
never accept another root, everything not under an accepted root, **100% of
backing** with one vault, is stranded for good. Causes (§13): a
**fresh-lineage break**, **30 days without a relayed rotation**, a
**conflict halt**, or a set beyond the gas cap (B-22 prevents it).

**Survivable path (B-23) [I].**

- **Resumable lineage.** The lineage cannot break before the contract
  expires, even at the 60 s floor: 1,440 + extension blocks ≥ 33 days
  (expiry plus 3 days of relay delay), so extension ≥ 46,080 (V1:
  10,080, `src/kernel/chainparams.cpp:292-295`); quorum recovery while the
  verifier lives revives it. The owner sets this constant
  (`doc/design/b3-master-handoff.md:1160`); a stale set holds authority
  longer.
- **Migration leaf.** A coordinated hard fork pins a successor vault (same
  chain and token); B3 appends an ordinary leaf paying it the reserve while
  the gate is open, and sweeps each later deposit into the old vault by a
  further leaf while its verifier lives (node changes §7.6); one whose sweep
  is never certified is stranded. B3 credits the successor only once a state
  proof shows it absorbed the payment. Useless after expiry or a halt.
- **Residual, for explicit owner acceptance.** Thirty days without a relayed
  rotation, or a conflict halt, still strands; a 60-day expiry would keep
  rotated-out keys dangerous longer (default 30).

---

## 10. Trust disclosure

| Party | Powers | Affects |
|---|---|---|
| **Tether** (USDT) | Pause; blacklist (sender or `from`); `destroyBlackFunds`; `deprecate` forwards calls, invisible to code-hash pins; a fee [E, Sourcify] | The reserve |
| **USDT0 operators** (one 3-of-5 Safe owns token, ProxyAdmin and OFT) | Blocklist, `destroyBlockedFunds`, re-point the OFT, upgrade proxy and Ethereum adapter [E] (`.../study/check.md:96-103`) | Coins in transit on (b), (d3); **not the reserve** |
| **LayerZero DVNs** (3/3, reconfigurable) | Colluding, forge deliveries [E/H] (https://usdt0.to/ecosystem/arbitrum) | Coins in transit; a forged delivery drains USDT0's adapter, not the vault |
| **Arbitrum** (sequencer, Security Council) | Delay, reorder or, by emergency action, alter state [E] (`.../study/check.md:93-94`) | Arbitrum-side coins; not the reserve |
| **Committee ≥ 2/3** of weight and headcount (≤ 64, no slashing) | Certify a false root and drain the vault; caps do not bound it | Everything |
| **Committee > 1/3** of weight and headcount, with network-timing control | Two conflicting certificates: B3 closes burns for good, its next certificate closes deposits, and a relayed halt strands the remainder; a rotated-out set's evidence is refused (contracts spec §3.4; node changes §8.5) | Everything |
| **Headcount Sybil** (without B-22) | Keys need 333 B3 each (`src/kernel/chainparams.cpp:274-277`); with h honest members, h/2 offline keys block headcount: for Set_2's 34, 17 keys = 5,661 B3 against 126,108,063 B3 of weight (`.../study/check.md:29`) [I computed; H until O2]; B-22 raises it to ~(h/2) × floor | Everything |
| **Ethereum sync committee** (512) | ≥ 2/3 dishonest: false mints, ≤ 10,000 per epoch, unbounded in total | The reserve |
| **Bootstrap; owner relayer; contracts** | Deployer pins, four keys (holders: OI-9) attest once (3-of-4) for one verifier on one chain (OI-12), watchers only cancel; liveness (30 days with no relayer strands); new and unaudited | Everything |

**Slashing is not dollar insurance** (Codex review): B3 has none
(`src/modern/stake.h:33-37`), and a B3-paid penalty could deter, never
refund.

**Required wording [I].** bUSD is a claim on one Ethereum vault holding USDT
with no admin key, released only by B3 staker certificates (at most 64
signers, no slashing) and minted only from Ethereum light-client proofs. It
inherits Tether's powers over USDT. Arbitrum users also trust USDT0's
operators, LayerZero's verifiers and Arbitrum while coins are in transit,
not once they are in the vault. If B3 finality stops long enough, or the
committee signs conflicting certificates, the reserve becomes permanently
unredeemable. The contracts are unaudited. It is not a protocol-native
dollar.

---

## 11. Relayers, economics and standing operations

All roles are permissionless; censorship only delays, until §9.1's 30 days.
The owner relayer posts B3 records, certificates, releases and forwarder
calls.

| Cost item | Ethereum 0.08 / 1 / 10 gwei |
|---|---|
| Certificate relay, ~1.98M gas (V1; V2 [H]) | $0.42 / $5.30 / $53 |
| `release`, 90-120k gas [H] | ≤ $0.03 / $0.32 / $3.21 |
| (d3) batched `release` + `forward`, 310-540k gas (contracts spec §11), F_x fee | ~$0.07-0.12 / $0.83-1.45 / $8.3-14.5 [H] |
| P-fast ceiling, ~111 relays/day; P-batched, 48/day | $47 / $588 / $5,885; $20 / $254 / $2,545 |

**Policy (B-16).** P-fast at or below 1 gwei, batched (≥ 30 min apart)
above. **Baseline ~$25 / $318 / $3,180 a month** (~2 relays a day) [I
computed] without withdrawal relays.
**Excluded [H]:** an Ethereum endpoint, a B3 node, B3 record fees, ETH
bought with treasury B3 (10% of the block reward,
`src/kernel/chainparams.cpp:288-290`).

**Funding (B-15) [I].** User-capped fees cover F_d's deploy and flush and
(d3)'s batched `release` plus `forward` (contracts spec §8). Leaf V2 has no
fee field and burn fees stay on B3, so the treasury relayer pays every
certificate and (d1) release; users may relay their own. A withdrawal
relayed alone costs ~$0.45 / $5.62 / $56; ten a day add ~$135 / $1,690 /
$16,900 a month [I computed], less when sharing a certificate. Later
option: a capped exit forwarder for (d1).

**Effort [H]:** revision 8's 23-39 person-weeks to RT7 (excluding P3 and
audit) included removed Arbitrum parts and omitted B-37's signer store;
**not re-estimated**; no audit cost figure.

**Standing operations**, each a **coordinated hard fork**: an Ethereum fork
beyond the light-client schedule (~yearly [H]), shipped before it; a
migration pin; a USDT `deprecate` re-pin; a USDT0 adapter, OFT or endpoint
change (new F_x factory, `exit_forwarder` re-pin). The F_d factory, a Vault
V2 immutable receiving plain transfers, keeps working; replacing it needs a
new vault and migration, as an updatable pointer would be an admin key.

---

## 12. Caps

- **Pilot (B-14):** 10,000 bUSD per block and per 1,440-block epoch (~31.2
  h), the mainnet V1 values (`src/kernel/chainparams.cpp:221-225`), and
  10,000 per deposit; no supply ceiling. Raise only after audit and
  rehearsal (`contracts/deployments/ethereum-mainnet-v1.1.1.json:77-78`).
- **Effect:** (a) and (b) share it: a lone depositor's $50,000 needs five
  epochs, so its last mint follows the first by 3-4 epoch lengths, about
  3-5.2 days at 60-78 s [computed], longer when routes compete; (b) above
  20,000 per F_d: OI-2.
- **Bound:** mint-side losses (light-client bug, false sync committee) are
  rate-limited to 10,000 per epoch, **unbounded in total** until a B3
  release; caps do not bound a forged root.

---

## 13. Failure and human-intervention cases

| # | Failure | Effect | Funds lost? | Recovery (human?) |
|---|---|---|---|---|
| 1 | Fresh-lineage finality stalls | Deposits close within ~24 h; burns close; mints continue | None if quorum returns before expiry; else **all unreleased reserve** | Restore quorum (yes) |
| 2 | No relay, or the owner relayer stops | Deposits close after ~24 h; catch-up ~3.2 epochs a day | After 30 days, all unreleased reserve | Anyone relays (yes) |
| 3 | Verifier expires | Certified leaves payable; burns refused | **All unreleased reserve** | Migrate earlier (yes) |
| 4 | Conflicting certificates (current or next set) | Burns close, then deposits (B3's flag); a reported conflict halts the verifier; accepted roots pay | The unreleased remainder; deposits before the flag mint unredeemable bUSD | None (yes) |
| 5 | Tether blacklists the vault or deprecates USDT | Frozen; or burns wait for a re-pin | Yes unless reversed; deprecation no if honoured | Issuer; re-pin (owner) |
| 6 | Partial seizure | Burns only within the surviving balance | Yes, shared | Wind-down (ruling) |
| 7 | USDT0, LayerZero or Arbitrum fails during (b) or (d3) | That transfer stuck, delayed or lost; reserve untouched | That transfer | Retry; F_d refund; F_x `withdrawOnEthereum` |
| 8 | Recipient or forwarder blacklisted | Frozen tokens or stuck forwarder funds | That item | Wallet pre-check |
| 9 | Ethereum fork beyond the schedule | Mints stop; burns close within 2 days | No | Hard fork first (yes) |
| 10 | Committee below 4 or weight 900; without B-22, above 64 or beyond gas | Roots and deposits frozen | None, unless rotations stop 30 days | Qualified set returns; else hard fork (yes) |
| 11 | Contract bug | Immutable code | Possibly all | Migration leaf while live (yes) |
| 12 | False mint | ≤ 10,000 per epoch; burnable against the real reserve | Yes, shared | Monitors; coordinated release (yes) |

---

## 14. Phasing and regtest milestones

B3 regtest reads one local anvil, chain 31337
(`src/kernel/chainparams.cpp:1136`); RT6 adds 31338 only as the USDT0
source (RP-5). RT5 is retired; RT7 is withdrawn for launch
(regtest plan §5); S10 and RT7b are reserved (§17 item 2).

| Stage | Content | Regtest milestone | Mainnet gate |
|---|---|---|---|
| **S0 Now** | Old-vault notice; O2, O8, O9, O11; commit the ruling; B-2 after O9 | — | None |
| **S1 H1 + H2** | Producer rotation with a 24 h escape; hardened checkpoint | **RT1**: rotation, escape, all paths agree | Rule-3 override; timing ruling; Set_3 coordination |
| **S2 Harness** | anvil/forge helpers; pin file; synthetic light client | **RT0** smoke; **RT2** light client; **RT3** V1 round trip | None |
| **S3 Lineage, contracts, node core** | Digest V2; lineage with B-22, B-23, B-37; verifier, prover, vault; bUSD V2, V2 store, burn gate, wind-down | **RT4**: (a) and (d1) on the staging signer; bootstrap; vectors; per-epoch keys; conflict halt; migration | None until S5 |
| **S4 Forwarders, legacy closure** | F_d, F_x; legacy bUSD frozen | **RT6**: (b) and (d3); refunds; blacklisted forwarder | With the S7 pins |
| **S5 V2 P3** | CommitQC replaces the V1 signer; mainnet lineage at R_f | **RT8**: RT4 and RT6 under P3 | V2-D and FD rulings; model check; liveness rehearsal; soak; hard fork |
| **S6 Audit, drills** | External audit; gas at 64 members, maximum absent | **RT9**: the §13 drills | Audit; drills green |
| **S7 Deploy** | Ethereum, 3-of-4 bootstrap of a recent epoch; B3 pins it; review window | **RT10**: deploy scripts and pin flow on anvil | Two builds agree on code hashes |
| **S8, S9** | Small transfers per route; open at pilot caps | RT10 on release candidates | Disclosure |

---

## 15. Owner decisions

Defaults in brackets. IDs are kept; removed ones are **withdrawn by the
2026-10-03 simpler-plan ruling**.

- **B-1** Old stack lapses [ruled]. **B-2** Relay 814,051 before
  2026-10-08T17:11:59Z [skip unless O9 finds a leaf ≤ 814,051].
- **B-3** One bUSD V2, backed only by the Ethereum vault, vault-bound id
  [yes, owner; no OD-8 or §21 override; but see OI-5].
- **B-4** Routes (a), (b), (d1), (d3); Tron guidance only [yes, owner]; (c)
  and (d2) withdrawn. **B-5** LP fast-fill: withdrawn.
- **B-6** H1 with its escape plus H2 [yes; rule-3 override, timing ruling].
  **B-7** Mainnet digest V2 only from P3 [yes]. **B-8** Fresh lineage from
  Snapshot(R_f − 1) [yes]. **B-9** Ancestry; accepted roots stay claimable;
  failed transfers leave leaves payable [yes].
- **B-10** Deposits close 24 h without a certificate or 72 h without a
  rotation (checked against O8, §8.1); expiry 30 days; spacing 6 h [yes];
  the per-chain split: withdrawn.
- **B-11** Bootstrap by a time-bounded k-of-n attestation, default 3-of-4
  [k-of-n accepted in conversation 2026-10-03, uncommitted, conditional on
  no more trust-minimized option]; the recommended start-certificate
  alternative **needs a ruling**.
- **B-12** Deposit gate and caller rule; **W2 = V** [yes].
- **B-13** Burn gate by Ethereum state proofs; wind-down; a sticky conflict
  also closes deposits by a certified flag [yes]; Arbitrum heartbeats
  withdrawn.
- **B-14** Caps 10,000 per block and per epoch [yes].
- **B-15** Capped forwarder fees (F_x's also pay (d3)'s release); treasury
  pays certificates and (d1) releases (§11) [yes; owner confirms the
  budget]. **B-16** Relay policy, Ethereum only [yes]. **B-17** Native B3
  for burns [disclose].
- **B-18** Outbox pins: withdrawn. **B-19** Legacy bUSD frozen; an
  owner-funded swap if O9 finds third-party holders (§3) [yes]. **B-20**
  Reconciliation commit [yes]. **B-21** Separate S5 and S7 releases [yes].
- **B-22** Committee: top 64 by stake above a W/128 floor [**needs explicit
  owner approval**; or accept and disclose the Sybil risk].
- **B-23** Extension ≥ 46,080 blocks; migration leaf, credited to the
  successor only once absorbed; residual stranding [yes; **needs explicit
  owner acceptance**].
- **B-24** Adapter gate on USDT [yes]; the USDT0 waiver: withdrawn.
- **B-25 to B-36** Staker-signed fast deposits: **deferred** (§17); B-25
  needs an explicit ruling if revived.
- **B-37 (new)** Per-epoch committee BLS keys under their own binding
  kind (§6) [yes, Codex review; replaces FD-6's binding reuse].
- **N-1 and N-2** (node changes §16): withdrawn.

---

## 16. Governing text to reconcile before activation

Each needs a reviewed commit first.

1. `CLAUDE.md:92-104` (rules 5-6: managed-v1 vault and withdrawal leg) →
   Vault V2, releases only by staker certificates, honest labels.
2. `CLAUDE.md:87-88` (rule 3) and `:12-13` → **one override: H1 with its
   escape, H2, the fresh lineage, B-22, B-37 and the bridge V2 regime**;
   `:5-7`, `:11` (workspace lock) → own-branch authorization if wanted.
3. OD-8 (`doc/design/b3-open-decisions.md:306-309`): **not overridden**
   (B-23's AssetId exception: OI-5). Light-client mint leg, signer-set ban
   and L1 origin kept; the L2-identity point (`:309`) no longer applies; the
   transition-v1 managed release (`:306`) was already superseded
   (`doc/design/b3-master-handoff.md:101-123`); its old-vault expectations
   meet the lapse notice and B-19's swap.
4. `doc/design/b3-open-decisions.md:251-256` (adapter gate; smoke-vault
   recheck) → B-24; recheck kept.
5. `doc/design/b3-master-handoff.md:101-123` and contract §45
   (`doc/design/b3-architecture-contract.md:596-610`): single immutable USDT
   vault kept; 3-of-4 bootstrap (handoff `:106`, contract `:609`) kept by
   B-11 for a recent fresh-lineage epoch, not Set_0; V2 contracts; stranding
   disclosure.
6. `doc/design/b3-bridge-threat-model.md:213-221` (T8) and the stale comment
   `src/kernel/chainparams.cpp:1146-1148` → adapter gate, stranding, Sybil.
7. `doc/design/b3-flowmesh-v2-stage0.md:43`, `:69` (preserve the deployed
   bridge) → **overridden by the fresh start**; `:709` (new digest
   rejected) → digest V2; `:711` → B-22.
8. `doc/design/v2-stage0-r1-consensus.md:370-376`, `:407`, `:409-415`,
   `:466-467` → release-fixed genesis; H1; B-22; old locks scoped to the old
   lineage.
9. `doc/design/b3-cross-chain-finality-v1.md:3-11`, `:172-178` → V2 beside a
   frozen V1; H1, B-22, per-lineage journals, B-23, B-37.
10. `src/wallet/wallet.h:1039-1067` (derived and imported keys) → B-37.
11. Owner 2026-09-29, "bridge need to be fix without changeing the contract"
    (`.../study/check.md:27`, memory only) → superseded by the fresh start.
12. W drift (`src/kernel/chainparams.cpp:270` vs
    `doc/design/b3-master-handoff.md:115-118`) → record V1's W (811,001) and
    V2's W2 = V.
13. Stable-quote ruling (memory only) vs `doc/design/b3-master-handoff.md:170-175`
    → commit it.
14. `doc/design/b3-bridge-bls-proposal.md:178-179` (Arbitrum origin
    rejected) → consistent at launch; revisit with §17 item 1.
15. The 2026-10-03 simpler-plan ruling → commit it.

---

## 17. Later options (not at launch)

Each needs its own ruling; revision 8 (at `1d1d6da`) has the full design.

1. **Arbitrum vault V2: routes (c) and (d2)** (contracts spec Appendix
   B.1). Vault_A holds USDT0; Outbox-proven deposits mint after **~6.4-6.5
   days** (≤ ~14.7 disputed). Two reserves under one bUSD override OD-8
   `:309` and contract §21 and put USDT0's Safe and Arbitrum's Security
   Council behind every holder. An **optional LP fast-fill** hides the
   delay.
2. **Staker-signed fast deposits** ([FSD](b3-bridge-v2-staker-signed-deposits.md),
   B-25 to B-36): committee attestations of Arbitrum deposits, ~16-29 min
   [H], settled by the Outbox proof. Needs option 1, penalties, P3 and a
   B-25 ruling (a bounded OD-8 exception). Reserved: stage **S10**, regtest
   **RT7b** (after RT7).
3. **An overcollateralized coin** (owner remark): another product, never
   bUSD; [`b3-native-stable-proposal.md`](b3-native-stable-proposal.md) is
   superseded history.

---

## 18. Considered and rejected

- **USDC**: Circle "will never mint on our chain" (owner).
- **Keeping the old contract**: broken lineage, no ancestry
  (`contracts/src/B3FinalityVerifier.sol:199-224`).
- **A small rotating multisig (3-of-4 or 2-of-3)**: a signer set in
  consensus, which OD-8 forbids; 2-of-3 tolerates no Byzantine signer
  (Codex review; one needs four), and small sets are captured over many
  rotations (staker-signed deposits §3).
- **An LP at launch**: with no Arbitrum vault it buys nothing (§0) and adds
  a consensus rule, float capital and Arbitrum trust in the backing.
- **An admin or rescue key**: a new trusted party; the migration leaf covers
  planned moves.

---

## 19. Open items and owner reads

**Owner reads:** **O2** `getfinalitystatus`, `getfinalityset 3` (lineage;
Set_3 members, online and unstaked); **O8** block spacing; **O9** legacy
minted balance, holders and any burn leaf ≤ 814,051; **O11** USDT fee, owner
and slots, USDT0 adapter, Safe and DVNs. O10 and O12 return only with §17
item 1.

**Open:** FD-15 (permanent conflict halt; a replacement-successor path);
re-credit of burns above a frozen root; a ruling on B-11's alternative;
owner acceptance of B-22, B-23's residual, the wind-down margin and the
(d1) relay budget; B-37's rebinding schedule (finality dependency);
`DEPOSIT_CLOSE_AFTER` against O8; OI-1 to OI-7 and OI-9 to OI-11 open
(OI-2 and OI-6 have implementation choices pending owner review); OI-8 and
OI-12 resolved (§20).

**Unverified [H]:** V2 and `approve` gas; LayerZero fees and DVN depths;
anvil EIP-2537; minutes until O8.

---

## 20. Open issues register

From the 2026-10-04 review, unresolved unless marked resolved (mechanical
errors were fixed in place). **Critical**: can lose reserve; **High**: loses
user funds or blocks a milestone.

- **OI-1** (Critical; contracts spec §5 step 3). Unlike V1
  (`contracts/src/B3FinalityVerifier.sol:167-171`), nothing keeps the
  bootstrap statement below V, so three bootstrap keys, or a pinned
  START_CERTIFICATE set, could install a permanently claimable root. *Direction:* require fb.height < V in both modes;
  apply only step 11's latest/history part.
- **OI-2** (High; contracts spec §7.4; RT6). A fresh F_d's `feeCap` pays two
  flushes, so a (b) delivery above 20,000 strands its remainder.
  *Direction:* size `feeCap` per ceil(amount / `MAX_DEPOSIT_RAW`), or cap
  (b) at 20,000 per F_d.
  *Implementation choice (regtest forwarder slice, 2026-10-08; pending
  owner review):* the second direction, in the wallet.
  - `bridgeforwarderaddress` refuses an expected delivery above
    `MAX_DEPOSIT_RAW` and keeps the spec's `feeCap = 4 × fee_per_tx`. One
    flush then moves the whole delivery, whatever fee within the cap its
    flusher takes. No contract changed.
  - The first direction was implemented first and dropped: `flush` lets
    any codeless EOA take the whole remaining cap at once, so a ceil-sized
    cap protects only against cooperative flushers. Bounding each flush's
    fee in p would need a salt-layout ruling.
  - Residuals: an over-delivery the wallet did not derive for, and a repeat
    arrival after a greedy first flush, wait for a zero-fee flush, a refund
    or `returnToOwner(p, 0)`; a front-runner can take the first flush fee
    from the relayer that paid for the deploy.
  - Details are in contracts spec §7.4. Tests: `test_OI2_*` in
    `contracts/test/v2/ForwarderDepositV2.t.sol`.
- **OI-3** (High; regtest plan §3.3, §3.6, RP-1). `815bcbf` refuses a
  changed pin file on reindex or once blocks reach bUSD activation, ~162
  (`src/init.cpp:1409-1415`, `:2107` at `e2a4b6c`;
  `src/kernel/chainparams.cpp:1122-1128`, `:1145`); V2 pins need
  blockHash(261). *Direction:* per-section activation with an append-only
  hash record, or all V2 pins before first start. *Regtest approach taken*
  (`1ef5212`, `9b5faad`, `834101a`; RT4a `af70f03`): a pin file with only
  a `v2` section, written before first start, names the CREATE addresses
  `DeployV2.s.sol` will use (deployer nonces 0-3). The Vault V2 and
  Verifier V2 runtime code hashes and the lineage id, which commit to
  blockHash(R_f − 1), are left unpinned on 31337 only:
  `getbridgev2info.unpinned` lists them, kind 9 code-hash checks waive the
  two hashes only on 31337 (`src/bridge/state_observation.h`), and
  `BridgeV2Params::Valid()` requires them on Ethereum mainnet. V ≥ A3 is
  asserted at startup, so `init.cpp`'s pin record refuses any later change
  of the file (RT4a step 12). The mainnet direction stays open.
- **OI-4** (Medium; node changes §3.1, §7.6). One `verifier` pin: a
  successor inherits its expiry and bugs, yet §13 rows 3 and 11 offer
  migration. *Direction:* per-entry verifier pins; narrow those rows.
- **OI-5** (Medium; §15 B-3, §16). B-23 keeps the AssetId against contract
  §21 (`doc/design/b3-architecture-contract.md:373-375`) and OD-8
  (`doc/design/b3-open-decisions.md:306`). *Direction:* a §16 item and
  ruling, or a new AssetId plus swap.
- **OI-6** (High; §3, §10; contracts spec §6.2). Plain transfers or (b)
  sends to Vault V2 are lost, undisclosed. *Direction:* disclose; wallets
  refuse `to` = Vault V2; no F_d derivation yields it.
  *Implementation choice (regtest forwarder slice, 2026-10-08; pending
  owner review):* wallet policy, plus one consensus refusal.
  - **Reserved addresses.** `modern::BridgeV2ReservedEthAddressReason`
    lists every address with a bridge V2 role: the launch vault, the
    successor vault, the origin token, the verifier, and the F_d and F_x
    factories.
  - **What the wallets refuse.** `bridgewithdraw` and
    `bridgeforwarderaddress` refuse a reserved address as:
    - the (d1) burn recipient;
    - the (d3) Arbitrum `to`;
    - a `refundEOA`.
  - **Derivations.** An F_d or F_x derivation that yields a reserved address
    fails closed.
  - **Warnings.** `bridgeforwarderaddress` warns that USDT or USDT0 sent by
    plain transfer to the vault, its token or a factory mints nothing and is
    lost.
  - **On-chain checks.** The F_d factory refuses such a `refundEOA` (vault,
    token, factory). It also refuses a `refund` to the vault, the account or
    the factory (contracts spec §7.3).
  - **Consensus.** A route (d3) exit block with a reserved `refundEOA` is
    invalid (check 5): F_x's `withdrawOnEthereum` escape needs a caller.
  - **Still missing.** The §10 disclosure text is not written yet. Details
    are in contracts spec §6.2.
- **OI-7** (Medium; finality §6.4, §9.2). B-37's windows use
  Snapshot(start − 1); P3-R1 snapshots at the terminal checkpoint
  (`doc/design/v2-stage0-r1-consensus.md:336-347`). *Direction:* restate
  them for P3-R1; amend BuildV1Snapshot in §9.2.
- **OI-8** (**Resolved 2026-10-05, owner ruling R10**, option B, sponsored
  fees; node changes §16; §8.4). bUSD-only users lack native B3 for burns
  until N-3 is ruled. *Resolution:* the user signs the burn (or exit) with a
  small bUSD tip to a relayer, under a signature mode that lets others only
  add inputs; the relayer adds the native-B3 fee input and later buys B3
  with the accumulated bUSD (e.g. on FlowMesh). No consensus fee-in-bUSD
  change: burns still pay native B3 (B-17). Wallet and relayer support is
  not built yet.
- **OI-9** (Medium; §10; contracts spec §5). Bootstrap key holders and their
  independence are unstated. *Direction:* name them or the rule; if
  owner-held, drop the independent-check argument.
- **OI-10** (Medium; contracts spec §4). Profile R's 72,000 s breaks
  `MAX_EPOCH_LAG` ≥ `DEPOSIT_CLOSE_AFTER` + 7 days. *Direction:* scale
  fixed terms on 31337, or list regtest orderings.
- **OI-11** (Low; regtest plan §3.3, §6.2). Profile M's 46,080 with 40-block
  epochs gives 32 days, not 33; rows 10 and 10b ignore RT4's fifth wallet;
  their knob is unlisted. *Direction:* 47,480 or 1,440-block epochs; state
  the count; list the knob.
- **OI-12** (**Resolved 2026-10-05, owner ruling R9**, "yes OI-12";
  contracts spec §2.2, §5 step 3). The K_OF_N attestation signed
  `digestV2(fb)` only, so it bound neither `START_SET_HASH` for k > 0 nor
  the deployment: a published attestation could initialize another
  verifier, on any chain, whose deployer pinned a different epoch-k set.
  *Resolution:* the bootstrap keys sign `bootstrapDigestV2 =
  TaggedHash("B3/BRIDGE/BOOTSTRAP/V2", CHAIN_DOMAIN ‖ u256 block.chainid ‖
  address(this) ‖ START_SET_HASH ‖ encodePacked(fb))`, 301 bytes, so one
  attestation initializes exactly one verifier on one chain for one start
  set; the distinct tag keeps attestations and certificates apart, and
  START_CERTIFICATE still signs `digestV2(fb)`. Implemented in
  `contracts/src/v2` with replay tests (`contracts/test/v2/BootstrapV2.t.sol`)
  and shared vectors (`bootstrap_attestation_v2`). The message binds no
  other immutable (windows, watchers, prover, bounds), so the keys sign only
  after deployment and after checking the deployed code hash and immutables
  against the manifest, never for a predicted address.
