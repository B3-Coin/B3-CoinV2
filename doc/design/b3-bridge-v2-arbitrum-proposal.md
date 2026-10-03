# B3 Bridge V2: Ethereum and Arbitrum vaults with no admin key, released by B3 staker certificates

> **Status: PROPOSAL, no governing authority.** It ranks below every owner
> ruling and every reviewed, committed governing document; nothing is
> ratified, built, audited, deployed or activated. Revision 8, 2026-10-03:
> a fresh start (the USDC and keep-the-old-contract plan is withdrawn).
> Base `b4b1de4`, branch
> `feat/flowmeshV2-dev-claude`. It follows the owner's 2026-10-03 direction
> (relayed, not yet in the repo): "30 mintues to 1H is okay", no USDC, both
> chains, defaults accepted ("you should do it … I think thats the best
> approch"). No node or chain RPC was contacted.

**Labels.** [E] established or verified; [I] design choice; [H] estimate or
unverified. **Default** = provisional solve-and-report choice, open to owner
ruling. Repo facts are `path:line` from the worktree root; external facts are
URLs; `.../study/` means
`/Users/josh/development/ON/B3-FlowMesh-200ms-evidence/arbitrum-bridge-study-20261003/`.

**Map.** This document owns architecture, trust, the numbers (§7), stages
(§13) and decisions B-1 to B-24 (§14). Siblings:
[contracts spec](b3-bridge-v2-contracts-spec.md) (contracts, bootstrap,
windows, gas, and the only normative byte layouts);
[node changes](b3-bridge-v2-node-changes.md) (bUSD, reserves, gates, Outbox
proofs, LP rule, RPC);
[finality dependency](b3-bridge-v2-finality-dependency.md) (H1, H2, fresh
lineage, P3); [regtest plan](b3-bridge-v2-regtest-plan.md) (harness, mocks,
RT0-RT10); [README](b3-bridge-v2-README.md).

---

## 0. Summary

**Shape.** Two new vaults, USDT on Ethereum and USDT0 on Arbitrum One, have
no admin, pause or rescue key; each pays only against roots certified by B3's
staker committee (at most 64 BLS keys, no slashing) under a **new finality
domain** with ancestry. B3 has **one bUSD** and a reserve ledger per vault,
and refuses burns a vault cannot pay. The old Ethereum stack expires. No
signer set enters B3 consensus; no USDC.

**Speed** (§7 owns every number; only with healthy finality, prompt relays,
inflow under caps and the computed ~78 s blocks at full participation [H]):
bUSD on B3 in 16-24 min from Ethereum (a), 17-28 [H] by LayerZero (b), and
2-4 or 15-26 [H] directly from Arbitrum with a funded LP (c), ~6.4-6.5 days
without one; withdrawals arrive in 29-50 min, up to 80 when Ethereum relays
are batched; FlowMesh V1 adds 40-42 min each way, plus a checkpoint wait on
exit.

**Finality.** If owner read O2 confirms the broken lineage, B3 needs **H1**
(scheduled producer rotation) and **H2** (a hardened checkpoint) for its own
health; H1 is advisable anyway (§5). The mainnet bridge waits for **V2 P3**,
with no Ethereum deadline; regtest runs it earlier with the V1 single-vote
signer behind a regtest-only gate.

**Largest risk.** A verifier that can never accept another root strands its
vault's unreleased reserve; a fresh-lineage break strands both, 100% of
backing (§8.1). B-23 (resumable lineage, migration leaf) narrows this, but 30
days without a relayed rotation, or a conflict halt, still strands for good:
the price of no admin key, for explicit owner acceptance.

**Regtest first.** Every code stage has a no-peer regtest milestone on local
anvil chains (§13); no end-to-end bridge test exists today.

---

## 1. Goals

Owner goals (2026-10-03 unless cited) and how far they are met:
deposits and withdrawals in 30-60 min (only under §7.5's conditions); no
signer set inside B3 consensus (OD-8, `doc/design/b3-open-decisions.md:306`;
met for B3's rules, but route (c) inherits Arbitrum governance, §6); no USDC
(met, §16); Arbitrum cheap for
users and Ethereum supported (partly, §7.3); base-chain liveness never
depends on an issuer (`doc/design/b3-master-handoff.md:1012-1016`, `:387`;
met); everything on regtest first ("I need everthing to be working atleast
on regtest"; §13).

---

## 2. The old Ethereum stack lapses

Verifier `0xE72B3Fe73F0d42A6e964D33E7BB1cc2EA7a3F690`, prover
`0x8e612aE4D475d25940E2A2FC907F21b6813eedA7` and vault
`0x077839b12cebfbF163acAEAC3A59A015D100c64b` expire: the verifier refuses
certificates 30 days after its last rotation
(`contracts/src/B3FinalityVerifier.sol:211-213`), at
**2026-10-08T17:11:59Z** [E] (`.../study/check.md:110-118`). Its root is the
empty tree, so the vault's 3 test USDT are stranded for good, with no owner or
rescue path (`contracts/src/B3StakerBridge.sol:95-160`) [E].

- **B-2 default: skip** relaying 814,051 (~$0.40), **unless O9** finds a
  burn leaf at or below 814,051: burns were valid from 811,001
  (`src/kernel/chainparams.cpp:270`), and only that relay could pay one.
- **Notice:** plain transfers to `0x0778…c64b` are lost.
- **Legacy bUSD** (raw `0xc69c6bd5…61ad`, `src/test/bridge_params_tests.cpp:216-228`)
  stays distinct, labelled "bUSD (legacy V1)", burns closed
  (`src/node/finality_tracker.cpp:46-63`; node changes §10).
- **Smoke vault** `0x143F207e23e6aebD7E974be90ac6D434f4c7BFb6`: recheck for
  USDT liability or consensus bUSD before activation
  (`doc/release-notes-v1.1.1.md:88-93`; `doc/design/b3-open-decisions.md:253-256`).

---

## 3. The fresh dual-chain model

```
ETHEREUM L1 (USDT)                      B3                         ARBITRUM ONE (USDT0)
(a) user -> Vault_E.deposit ---Deposit log--> light client      (c) user -> Vault_A.deposit
(b) USDT0 OFT send -> F_d -> Vault_E          (sync committee)        + ArbSys.sendTxToL1(msg)
                                              receipt proof           |  ~6.4 d BoLD confirm
    Outbox.roots[sendRoot] (slot 3) <---------------------------------+
         \--- L1 account + storage proof ---> kind 7 root, kind 8 mint (LP fill first)
                                              one bUSD; reserve_E, reserve_A
WITHDRAWAL: burn names vault -> leaf {id, chain, vault, token, recipient, amount, height}
  -> fresh-lineage certificate, digest V2 (P3 on mainnet) -> relayed to BOTH verifiers
  -> Vault_E.release (or F_x -> LayerZero -> Arbitrum) | Vault_A.release
```

Six new contracts (contracts spec §0), with no owner, admin, pause, proxy or
rescue key, and the same code on each chain they use except immutables:
**Verifier V2** (dual quorum, stake ≥ ⌊2W/3⌋+1 **and** headcount ≥
⌊2n/3⌋+1); **BlsDigestProver**, a new deployment built from the V1 EIP-2537
code and taking a precomputed digest (the deployed prover
`0x8e612aE4D475d25940E2A2FC907F21b6813eedA7` hard-codes the V1 tag,
`contracts/src/BlsCertificateProver.sol:158-176`); **Vault V2** (USDT
`0xdAC17F958D2ee523a2206206994597C13D831ec7`, USDT0
`0xFd086bC7CD5C481DCC9C85ebE478A1C0b69FCbb9`); **B3RelayBatch** (certificates,
releases, forwards and a heartbeat in one transaction); and, on Ethereum
only, forwarders **F_d** and **F_x** (per-user CREATE2 addresses, refundable
to the user's key before custody).

### 3.1 Improvements the new contracts allow [I unless marked]

1. **Digest V2** binds `lineageId` and a cumulative checkpoint-history root;
   a certificate must prove the last accepted checkpoint is in its history (a
   previous-certificate pointer would force relaying every certificate,
   `src/node/finality_signing_policy.h:16`).
2. **Accepted-root history:** releases prove against any accepted root at or
   above the leaf height, so newer certificates stop breaking proofs in flight
   (V1: `contracts/src/B3StakerBridge.sol:149-151`).
3. **Windows (B-10).** Deposits close after **24 h without a certificate or
   72 h without a rotation** (V1: 23 days). Expiry is 30 days, **never below
   24**: the Arbitrum gate needs 14.7 d dispute + 7 d release + 1 d burn
   margin + 1.01 d skew ≈ 23.7 d (node changes §3.4). Rotation spacing is
   **6 h**: V1's day, against B3's ≥ 24 h epochs (`src/modern/pos_v1.h:74-79`),
   recovers an epoch of gap only every ~4.3 days. Arbitrum timestamps lag up
   to 24 h and lead 768 s (`.../study/check.md:31-34`); `block.number` is
   unused [E].
4. **Deposit gate tied to a live release path (B-12).** Custody is refused
   unless the verifier is live with a root at or above the bridge activation
   height V and the token is not paused, deprecated or blocking the vault
   (contracts spec §3.5, §6.2). Burns open at **W2 = V**, so no minted
   bUSD waits for a burn height. Vault_E takes deposits only as the
   top-level call of a codeless EOA or from an F_d account (C-13): B3's V2
   store accepts an 8,192-byte terminal receipt node (node changes §4; today
   1,024, `src/bridge/proof.h:45`), but a Safe, ERC-4337 or EIP-7702 bundle
   adds logs to the same receipt without bound, so its deposit could be
   custodied and never minted; such wallets pay their F_d by plain transfer.
   Releases are never gated.
5. **LP fee ceiling.** The vault's `maxLpFeePpm` ceiling is an immutable equal
   to B3's consensus ceiling (10,000 ppm), never lowered; kind 8 is valid
   whatever the proven value (B-5).
6. **Bootstrap (B-11)** from a **recent certified checkpoint** (contracts
   spec §5, normative): the deployer pins the lineage and a start epoch k
   with its set hash; `initialize()` accepts **any ordinary epoch-k
   certificate** whose history root contains leaf 0. Watchers may only
   cancel during a review window; reviewers check every pin before a B3
   release pins the vaults. This **departs from the k-of-n default the owner accepted on 2026-10-03
   (accepted in conversation, not yet a committed ruling), so it needs a ruling**: it
   removes the attesters, but proves only that the pinned keys are live and
   reach a quorum, not that they are B3's committee, so the trust root stays
   the deployer pins plus public review (finality §11). A recent epoch also avoids months of catch-up trusting
   old, possibly unstaked keys. Fallback: k-of-n
   (`contracts/src/B3FinalityVerifier.sol:93-190`).
7. **Migration (B-23)** by an ordinary leaf V2 paying a pinned successor
   vault, §8.1.

### 3.2 Gas

[E, V1 measured] Certificate relays 1,976,420 and 1,872,058; `verify()`
1,618,093 on both chains; 64 members with 21 absent 5,513,351 (6,283,311
conservative), under Ethereum's 16,777,216 per-transaction cap
(https://eips.ethereum.org/EIPS/eip-7825) and Arbitrum's 32M
(`.../study/check.md:30-59`; `doc/design/b3-cross-chain-finality-v1.md:334-342`).
Each absent member adds ~0.22M [I computed], so ~70 absent exceed Ethereum's
cap, while B3 allows sets up to 8,192 (`src/modern/finality_types.h:42`);
B-22 bounds the set. EIP-2537 is live on Arbitrum (ArbOS 51,
https://docs.arbitrum.io/run-arbitrum-node/arbos-releases/arbos51). V2 gas
is [H] (contracts spec §11).

**Byte layouts.** Only the contracts spec defines them (summary and shared
C++/Solidity vectors in its §2.5, gating RT4).

---

## 4. One bUSD, per-vault reserves

- **Identity [I].** One B3-native id bound to the chain domain and a version
  tag, not a vault (V1's bound the vault, `src/modern/bridge_asset.h:24-32`);
  vaults are registry entries, overriding architecture contract §21 and
  OD-8 `:309` (§15).
- **Mint, reserve, burn.** Either vault's proven deposit mints bUSD
  (nullifier per chain, vault and id, `src/bridge/admission.h:196-207`) [E];
  `reserve_v` = minted from v − burn leaves naming v [I]. A burn names a
  vault, whose chain, address and token its leaf carries; B3 refuses it
  beyond `reserve_v` or the wind-down bound, or while v's gate is closed.
- **Gates [I]** (node changes §8). *Ethereum:* EIP-1186
  proofs of verifier, vault and USDT state against a light-client-proven
  `stateRoot` (https://eips.ethereum.org/EIPS/eip-1186), which B3 proves but
  does not store today (`src/bridge/ssz.h:167-203`;
  `src/node/bridge_state.h:63-76`). *Arbitrum:* heartbeats **from the vault
  only** via the Outbox (on every release, after every rotation, at least
  daily), one 416-byte payload of verifier, token and vault state (contracts
  spec §6.3). Being 6.4-14.7 days stale, the gate needs the verifier live 7
  days past the burn after a 24 h 13 min skew allowance.
- **Wind-down [I].** A burn naming v is accepted only while its amount plus
  v's unreleased leaves stays within v's last observed balance less a margin,
  so after a partial USDT0 burn or seizure the survivor stays redeemable.
- **Adapter gate (B-24).** USDT upgrades by `deprecate`, which forwards
  calls; B3 proves `deprecated` and the code hash (kind 9), and deprecation
  closes Vault_E burns until a ruled re-pin. USDT0 is a proxy under a 3-of-5
  Safe (`.../study/check.md:99-103`) whose upgrades B3 cannot see; default, a
  recorded owner waiver.
- **Contagion.** Every bUSD claims both reserves. A USDT0 failure (seized or
  frozen balance, lost backing) is everyone's loss, seen a week or more late:
  a run, where those exiting first through the healthy vault are paid in
  full. A Tether action against Vault_E mirrors it.
- **Rebalancing.** Reserves never move. Ethereum exits come only from
  Vault_E and wait while `reserve_E` is short; a user may instead take
  (d2) and send USDT0 to Ethereum through its OFT, unlocking USDT from the
  Ethereum adapter [E/H]
  (https://docs.usdt0.to/technical-documentation/developer/).
- **USDT fee switch** (below 20 bps): releases deliver the net [E].

---

## 5. Finality: what the bridge needs, and B3's own repair

**Facts [E].** Under V1 producers are the finality set
(`src/validation.cpp:2574`), which changes only after a same-epoch
certificate (`src/node/finality_tracker.cpp:123-131`, `:141-158`);
`lineage_broken` is never cleared (`:133-137`). V1 voting deadlocked at
811,631 (`doc/release-notes-v1.1.3.md:58-106`) and 814,191
(`doc/finality-quorum-deadlock-814191.md:16-20`); mainnet is expected broken
since ~826,841, finalized at 814,051, producers frozen at Set_3 [H; O2].

**Answer.** Yes if O2 confirms `lineage_broken`, and advisable anyway: **H1**
with **H2**, a hardened checkpoint (`src/kernel/chainparams.cpp:326-329`);
the bridge drives neither. Frozen
producers exclude new stakers, let an unstaked Set_3 member keep producing,
and halt the chain while no Set_3 key is online, permanently if those keys
are lost (`src/modern/pos_v1.h:109-123`). A fresh node is protected only to
the hardened 811,641 until it validates the block carrying the 814,051
certificate (`src/validation.cpp:2511-2535`), hence H2; only a working
lineage restores finality, which FlowMesh V2 needs
(`doc/design/b3-flowmesh-v2-stage0.md:496-505`).

| Item | Default |
|---|---|
| **H1 + H2** | Hard fork: producers rotate every 1,440 blocks with today's snapshot rules and a 24 h escape (finality §5.1, FD-2); a hardened checkpoint; no BLS or bridge change. Needs a rule-3 override, a timing ruling (`doc/design/b3-cross-chain-finality-v1.md:172-173`), Set_3 coordination |
| **Fresh lineage** | Genesis Snapshot(R_f − 1), tag `B3/FINALITY/V2`, lineage id, separate journals (old orphan locks cannot block it), checkpoint tracker separate from producers; resumable, so it cannot break before the contracts expire (B-23, §8.1) |
| **Committee (B-22)** | From R_f, the stake-ranked top 64 among keys above a per-key weight floor; producers stay the full snapshot. Keeps rotations within gas (§3.2); blunts headcount attacks (§9) |
| **Digest V2 producer** | Mainnet: **V2 P3** (PBFT-style CommitQC; D1 pending): the exporter must never produce two conflicting certificates, which V1 voting can across an epoch boundary (finality §8). Regtest: the V1 signer, new domain |
| **Order** | H1 before any mainnet fresh lineage, or producers following it freeze after 11,520 blocks of quorum failure (`src/kernel/chainparams.cpp:292-295`) |

Stage 0's bootstrap (`doc/design/v2-stage0-r1-consensus.md:409-415`) and
P3-R1 producers (`:370-376`) need amendments (§15).

---

## 6. Who verifies, signs and authorizes

**B3 block validity alone authorizes mints and burns.** An Ethereum-vault
mint needs a sync-committee supermajority (342 of 512), execution ancestry,
a receipt proof of the pinned vault's Deposit, caps and a nullifier
(`src/node/bridge_state.cpp:537-664`) [E]. An Arbitrum-vault mint needs kind
7, `Outbox.roots[sendRoot] ≠ 0` by L1 storage proof with the Outbox
implementation, slot 0 (`rollup`) and slot 1 (`bridge`) equal to pins
(https://raw.githubusercontent.com/OffchainLabs/nitro-contracts/main/src/bridge/AbsOutbox.sol),
then kind 8: a Merkle path to the pinned vault's message, caps, nullifier and
LP lookup [I]. A burn needs a named vault, the reserve and wind-down bounds
and an open gate. **The committee** (≤ 64) accepts a root by dual-quorum BLS
over digest V2 (lineage, ancestry, epoch e or e+1, spacing, expiry, monotone
height); **anyone** may then `release` a leaf that binds chain, vault and
token, is unreleased and sits under a root accepted at or above its height.

B3 never takes a signer set's word for a mint; stakers certify only
withdrawal roots [I]. Transit for (b) and (d3) trusts LayerZero DVNs and
USDT0. Route (c) inherits Arbitrum governance: kind 7 fails closed on an
Outbox implementation, rollup or bridge change, but Rollup upgrades are
**not** detected, and the 9-of-12 Security Council can force-confirm a false
root that mints on B3, within caps (§11); the OD-8 record must say so (B-3).

---

## 7. Latency and cost: the single set of numbers

### 7.1 Method

B3 legs use the **computed ~78 s at full participation**
(`src/modern/pos_v1.h:74-79`; `doc/design/b3-finality-benchmark-2026-08-23.md:12-16`),
unmeasured (O8); at 50% online stake 95% of blocks arrive within ~150 s
(same source), with one 1/34-weight key online ~240 s (finality §4). The
**default relayer** chains dependent B3 transactions (a reorg undoes both;
the nullifier stops double mints); the **wait variant**, today's relayer,
waits for B3 finality per step (`contrib/b3bridge/b3_bridge_relayer.py:1744-1761`).

| Code | Step | Blocks | Minutes | Basis |
|---|---|---|---|---|
| t_ef | Ethereum finality after inclusion | — | 13-19 | [E/H] https://ethereum.org/en/developers/docs/consensus-mechanisms/pos/#finality |
| t_lz | USDT0 LayerZero delivery | — | 0.5-3; +15-20 if DVNs wait for L1 [H] | https://docs.usdt0.to/technical-documentation/developer/ |
| t_fl | Relayer check and F_d flush | — | 0.5-1 | [I] |
| t_batch | Arbitrum batch carrying the deposit posted to Ethereum | — | 0-3 | [H] one sample, ~1 batch per 3.1 min (`.../study/arb-facts.md:138-140`) |
| t_lp | LP sees the deposit (sequencer feed ~250 ms) and its fill lands on B3 | — | 2-4 | [H] `.../study/inbound.md:374` |
| t_chain | Default relayer: update then mint (or kind 7 then 8) | 2-4 | 2.6-5.2 | [I] |
| t_fin | B3 finality: inclusion 1 + checkpoint 0-9 + export depth 20 + carrier 1-2 | 22-32 | 29-42 | [E] `src/kernel/chainparams.cpp:293-294`; `src/node/finality_signing_policy.h:16` |
| t_wait | Wait variant: two waits plus mint (best 23-34 blocks, 30-44 min) | 45-66 | 59-86 | [I/H] |
| t_fm | Into FlowMesh V1: USER_DEPOSIT plus 30-block anchor | 31-32 | 40-42 | [E] `src/node/flowmesh_anchor.h:31-41` |
| t_rel | Relay certificate and release, prompt | — | 0.2-5 | [I] |
| t_arb | Arbitrum: next assertion ≤ ~1 h + 45,818 L1 blocks ≈ 6.36 days; disputes to ~14.7 | — | days | [E] `.../study/check.md:31-34`, `:78-92` |

Route figures add their steps' minutes, rounded half up; other spacings
recompute the B3 legs. **Sensitivity** [I computed]:

| Spacing | t_fin | Withdrawal (d1) | (a) usable on FlowMesh V1 |
|---|---|---|---|
| 78 s (default) | 29-42 min | 29-47 | 56-66 |
| 100 s | 37-53 | 37-58 | 68-79 |
| 150 s (95th pct at 50% online) | 55-80 | 55-85 | 96-109 |

At the 60 s floor B3 legs shrink 23%. Withdrawals pass 60 minutes at the
upper bound above ~103 s per block, at the lower bound above ~163 s. **O8 and
O2's online fraction gate publishing any user-facing time.** Prices (2026-10-03): ETH $2,677.62; Ethereum at 0.08, 1 and 10 gwei (measured
0.063-0.077); Arbitrum 0.02 gwei (`.../study/check.md:125-132`). Under
FlowMesh V2 the FlowMesh leg becomes about one t_fin [H].

### 7.2 Deposits

| Route and steps | bUSD on B3, default | Wait variant | Usable on FlowMesh V1 |
|---|---|---|---|
| **(a)** Ethereum USDT: `approve` + `deposit` from an EOA, or a transfer to F_d; t_ef, t_chain | **16-24 min** | 72-105 (best 43-63) | 56-66 |
| **(b)** Arbitrum USDT0 FAST: OFT `send` to F_d (dstEid 30101); t_lz, t_fl, t_ef, t_chain | **17-28 [H]** (32-48 if DVNs wait for L1) | 73-109 [H] | 57-70 [H] |
| **(c)** Arbitrum USDT0 DIRECT to Vault_A; a funded LP pays on B3: t_lp, or t_batch, t_ef, t_lp | **2-4** trusting the sequencer; **15-26** after Ethereum finality of the batch [H] | same | 42-46 or 55-68 |
| (c) without an LP: t_arb, t_ef, t_chain | **~6.4-6.5 days**; ≤ ~14.7 disputed; unbounded while paused | + ~1 h | +40-42 min |

**(e) Tron:** guidance only, the Legacy Mesh to Arbitrum (0.03%,
https://docs.usdt0.to/overview/the-legacy-mesh) or an exchange.

**LP fast-fill rule (B-5) [I]; node changes §6 is normative.** The
Arbitrum deposit message carries a user-set `maxLpFeePpm` (0 disables fills;
ceiling 10,000). A fill claims exactly the deposit key, amount, recipient and
`maxLpFeePpm`, and pays the recipient at least amount × (1 − maxLpFeePpm/10⁶)
in bUSD. At the kind 8 mint, B3 pays the earliest such fill (strictly
earlier block, within `fill_max_age_blocks` = 40,320), else the recipient. A
fill short of its own claim, or a second fill into an occupied bucket
(B-5a), is invalid; a wrong claim never matches and is a gift. Fills and
mints undo together. **The user pays exactly their cap**: LPs compete on
speed, not price.

Float costs 0.09/0.18/0.36% for 6.5 days at 5/10/20% APR (0.20/0.40/0.81%
for 14.7 days), so 30 bps covers ~17% APR. **LP risks:** never-confirmed deposits; wrong claims; slower confirmation (the 2026-10-02
emergency upgrade warned of delays, `.../study/check.md:94`); and pruning, where a kind 8 more than 40,320 blocks
(~36 days) after the fill pays the recipient. **Float = Arbitrum delay + cap
queue:** Vault_A mints at most ~$7,700 a day (10,000 per ~31.2 h), so $5,000
a day needs ~$32,500 [I computed]; above the cap the queue grows without
bound, delaying repayment to the payee fixed at proof time (node changes §6). **No LP exists today**; the treasury LP (N-2) holds B3, so its
float must be bought as USDT off-chain and enter through (a) or (b) [H].

### 7.3 User cost

| Route | Ethereum gas the user pays, 0.08 / 1 / 10 gwei | Total at $100 / $1,000 / $10,000 |
|---|---|---|
| (a), ~216-221k gas [H]: V2 deposit 170-175k (contracts spec §11) plus a USDT `approve` of ~46k; V1's deposit measured 161,975 (`.../study/check.md:24`) | $0.05 / $0.58-0.59 / $5.78-5.92 | Same at every amount (plus the USDT fee if on) |
| (b) first use: F_d deploy + flush 320-410k [H], inside the capped relayer fee | $0.07-0.09 / $0.86-1.10 / $8.6-11.0 | Same at every amount, plus the LayerZero fee [H] |
| (b) later: flush 200-250k [H] | $0.04-0.05 / $0.54-0.67 / $5.4-6.7 | ditto |
| (c) + LP | none (Arbitrum ~$0.01-0.02 [H]) | $0.31 / $3.02 / $30.02 at the 30 bps cap |
| (c) without an LP | none | ~$0.01-0.02, after 6.4 days |

The LayerZero fee also pays Ethereum `lzReceive` gas and the DVNs [H, O11].
(c)+LP beats (a) only below ~$10, ~$190 and ~$1,950 at 0.08, 1 and 10 gwei
[I computed].

### 7.4 Withdrawals

| Route | Prompt relays | Ethereum batched (> 1 gwei) | User pays |
|---|---|---|---|
| **(d1)** → Ethereum from Vault_E | **29-47 min** | 29-77 | B3 fee (native B3) |
| **(d2)** → Arbitrum from Vault_A | **29-47** | unaffected | B3 fee |
| **(d3)** → Arbitrum via Vault_E + F_x + LayerZero | **30-50 [H]** | 30-80 [H] | Plus LayerZero fee and `forward` gas from the amount, capped |


Starting in FlowMesh V1 adds t_fm and a checkpoint wait (69-92+ min [H]).
Wallets pick (d2) when `reserve_A` covers the amount, else (d3); Ethereum
exits use (d1) only. Burns need native B3 for fees
(`src/wallet/rpc/assets.cpp:1985-2020`; B-17). At an epoch start a burn
needs the chain's rotation, normally within one t_fin [H]; after a relay
outage, catch-up runs ~3.2 epochs a day. "Prompt" assumes the treasury pays
every Ethereum certificate (up to $53 at 10 gwei).

### 7.5 Against the 30-60 minute target

| Journey | Default relayer | Wait variant |
|---|---|---|
| Deposits (a), (b) | **Yes** (16-28; (b) up to 48 if DVNs wait) | **No** (72-109) |
| Deposit (c) + LP | **Yes only with a funded, online LP** (2-26) | same |
| Deposit (c) without an LP | **No** (days) | **No** |
| Deposits into FlowMesh V1 | Borderline (42-70) | **No** |
| Withdrawals to arrival | **Yes at ≤ 1 gwei or with P-fast** (29-50); **No** when batched (to 80) | same |
| Withdrawals from FlowMesh V1 | **No** (69-92+) | same |

Every row assumes full participation, a working lineage, prompt relays and
inflow under caps.

---

## 8. Exactly-once, no refund, and stranding

**Deposits.** One `deposit()` emits one Deposit with an increasing id
(`contracts/src/B3StakerBridge.sol:95-114`) [E]; a flush is one `deposit()`;
the nullifier is undone only with its mint [E]; a LayerZero message credits
once (https://docs.layerzero.network/v2/developers/evm/composer/overview)
[E]; `Outbox.roots` is never pruned, so Arbitrum deposits stay claimable
(AbsOutbox source above) [E]. **No refund once tokens are in a vault**.

**Withdrawals.** One burn, one leaf with a global id and a vault tag
(`src/node/bridge_state.cpp:767-776`) [E]; each vault keeps `released[id]`,
and only the named vault computes the leaf [E pattern,
`contracts/test/B3StakerBridgeMultiVault.t.sol:89-159`]. Ancestry puts every
accepted root on **one** B3 branch, canonical only while faulty keys stay
below one third (§9). **No refund on B3.** A blacklisted recipient receives
frozen tokens, since USDT and USDT0 check only the sender or `from`
(contracts spec §6.5); wallets pre-check. A leaf stays unpaid if the vault is
blacklisted, seized or short; a blacklisted forwarder holds funds stuck.

### 8.1 Stranding

Vault tokens leave only by release of a certified leaf (contracts spec §10).
If a verifier can never accept another root, everything not covered by an
accepted root is stranded for good: the backing of all unburned bUSD plus
uncertified leaves. A new deployment cannot reach it.

Causes (§12 rows 1-4, 10, 12): a **fresh-lineage break**, by default (B-23)
1,440 + ≥ 46,080 blocks after an epoch start (≥ 33 days at 60 s), not V1's
1,440 + 10,080 (`src/kernel/chainparams.cpp:292-295`), strands **both vaults,
100% of backing**; **30 days without a relayed rotation** strands that vault,
both if quorum is lost; a **conflict halt** (§9) strands each chain receiving
the pair; a set beyond the gas cap (§3.2) strands that chain, which B-22
prevents. The surviving vault pays first come, first served.

**Survivable path (B-23) [I].**

- **Resumable lineage (FD-7 as default).** The fresh lineage cannot break
  before the contracts expire, even at the 60 s floor: 1,440 + extension ≥
  33 days (30-day expiry plus 3 for relay delay and skew) of 60 s blocks, so
  extension ≥ 46,080. Any quorum recovery while a verifier lives revives the
  same contracts. Only the owner can set this new constant
  (`doc/design/b3-master-handoff.md:1160`); the cost is a stale checkpoint
  set holding authority longer.
- **Migration leaf** (contracts spec §6.4; node changes §7.6). A coordinated
  hard fork (node changes §12.3) pins a successor vault (same chain, token
  and bUSD id); B3 appends an **ordinary leaf V2** paying `reserve_v` to it,
  only while the old vault's gate is READY, before a known expiry, a USDT0
  change, a re-pin or a bug fix. It adds no trusted party and cannot help
  after expiry or a halt.
- **Residual, for explicit owner acceptance.** Thirty days without a relayed
  rotation, or a conflict halt, still strands. A 60-day expiry would widen
  the window but keep rotated-out keys dangerous longer; default 30 (B-10).

---

## 9. Trust disclosure

| Party | Powers | Affects |
|---|---|---|
| **Tether** (USDT) | Pause; blacklist (sender or `from` only); `destroyBlackFunds`; `deprecate` forwards calls, invisible to code-hash pins; a fee [E, Sourcify] | Vault_E; USDT0 backing |
| **USDT0 operators** (one 3-of-5 Safe owns token, ProxyAdmin and OFT) | Blocklist (sender, `from`, spender) and `destroyBlockedFunds`; `setOFTContract` re-points a mint/burn authority that can burn from any address; proxy upgrades invisible to B3 (B-24); the upgradeable Ethereum adapter holds all backing [E] (`.../study/check.md:96-103`) | Route (b); **Vault_A**; (d3) |
| **LayerZero DVNs** (LayerZero, USDT0, Canary; 3/3; reconfigurable) | Colluding DVNs can forge deliveries or unbacked USDT0 [E/H] (https://usdt0.to/ecosystem/arbitrum) | Transit; Vault_A's token |
| **Arbitrum** | Timestamps −24 h / +768 s; force inclusion ~24 h; the 9-of-12 Security Council acts instantly, three times in six months, including a 2026-04-21 seizure of ~30,766 ETH and a 2026-10-02 emergency upgrade (`.../study/check.md:93-94`; https://forum.arbitrum.foundation/t/security-council-emergency-action-21-04-2026/30803); force-confirmation; upgradeable contracts; BoLD to ~14.7 days [E] | Vault_A; (c) mints; LP float |
| **Committee ≥ 2/3** of weight and headcount (≤ 64, no slashing) | Certify a false root and drain any vault; caps do not bound it | All withdrawals |
| **Committee > 1/3** of weight and headcount, with network-timing control | Two conflicting quorum certificates (equivocation is free); relayed to both chains they halt both verifiers and strand both reserves (§8.1); an orphan relayed first gets its leaves paid | Everything |
| **Headcount Sybil** (without B-22) | Keys need only 333 B3 each (`src/kernel/chainparams.cpp:274-277`); with h honest members, k ≥ h/2 offline keys block the ⌊2n/3⌋+1 headcount: for Set_2's 34, 17 keys = 5,661 B3 against 126,108,063 B3 of weight (`.../study/check.md:29`) [I computed; H until O2]; 31 keys push n past 64. Finality halts and reserves strand; recovery is a hard fork. B-22 raises the cost to ~(h/2) × floor | Everything |
| **Ethereum sync committee** (512) | ≥ 2/3 dishonest feed false headers: false mints or Outbox roots, ≤ 10,000 per epoch per vault, unbounded in total until a B3 release; false bUSD burns against real reserves (§12 row 14) | All reserves |
| **Bootstrap** | Deployer pins a recent epoch and its set hash; any certificate of that epoch initializes; watchers only cancel; k-of-n attesters only in the fallback [I] | Initial state |
| **LPs**; **owner relayer**; **contracts** | Own capital only; liveness, but 30 days with no relayer strands; new and unaudited | Route (c); survival; everything |

**Required wording [I].** bUSD is a claim on two pooled vaults with no admin
key, USDT on Ethereum and USDT0 on Arbitrum, released only by B3 staker
certificates (at most 64 signers, no slashing) and minted only from Ethereum
light-client proofs, which for Arbitrum deposits also trust Arbitrum
governance. It inherits Tether's powers over both reserves, plus USDT0,
LayerZero and Arbitrum powers over the Arbitrum reserve; a failure of either
reserve is shared by all holders, first come, first served. If B3 finality
stops long enough, or the committee signs conflicting certificates, the
affected reserve becomes permanently unredeemable. It is not a
protocol-native dollar.

---

## 10. Relayers, economics and standing operations

All roles are permissionless; censorship only delays, until §8.1's 30 days.
The owner relayer posts B3 records (per deposit, at least daily),
certificates to both chains (rotations, daily keep-alives, withdrawals),
releases and Vault_A heartbeats (each rotation, at least daily).

| Cost item | Ethereum 0.08 / 1 / 10 gwei | Arbitrum 0.02 |
|---|---|---|
| Certificate relay, ~1.98M gas (V1; V2 [H]) | $0.42 / $5.30 / $53 | $0.11 |
| `release` [H]; (d3) `forward` gas: contracts spec §11 | ≤ $0.03 / $0.32 / $3.21 | < $0.01 |
| Baseline per day, ~2 relays [H] | $0.85 / $10.60 / $106 | $0.21 |
| P-fast ceiling, ~111 relays/day; P-batched, 48/day | $47 / $588 / $5,885; $20 / $254 / $2,545 | $11.8; $5.1 |

**Policy (B-16).** Arbitrum: relay certificates covering pending burns within
~5 minutes. Ethereum: P-fast at or below 1 gwei, batched (≥ 30 min apart)
above (§7.4). **Baseline gas, both chains: ~$32 / $324 / $3,190 a month** at
0.08 / 1 / 10 gwei [computed], excluding withdrawal relays.

**Not in that figure [H, unestimated]:** infrastructure (Ethereum and
Arbitrum endpoints, a B3 node); B3 fees for kinds 1-4 and 6-13; ETH on both
chains, bought by hand with the treasury's B3 (10% of the block reward,
`src/kernel/chainparams.cpp:288-290`); LP float.

**Effort [H].** 23-39 person-weeks to RT7 (25-43 to RT9) excluding P3 and
audit (regtest plan §8), 3-5 engineer-months of it node work, plus a 4-8
week node audit (node changes §14). **No audit cost figure exists** for the
six contracts, node kinds 7-13, H1/H2, the fresh lineage and P3.

**Funding (B-15):** user-capped fees, taken only inside a successful step,
plus the treasury relayer.

**Standing operations [I], owner-coordinated.** Each of these loosens a
validity rule, so each is a **coordinated hard fork**, not a routine release
(node changes §12.3, N-5): an Ethereum fork beyond the light-client schedule
(~yearly [H]), shipped before it; an Outbox implementation, rollup or bridge
change (§12 row 6); a migration pin (§8.1). A USDT0 adapter, OFT or endpoint
change needs new forwarder factories (immutable settings); a USDT
`deprecate`, a re-pin.

---

## 11. Caps

- **Pilot (B-14):** per vault, 10,000 bUSD per block and per 1,440-block epoch
  (~31.2 h), the mainnet V1 values (`src/kernel/chainparams.cpp:221-225`),
  and 10,000 per deposit (`MAX_DEPOSIT_RAW`); per-vault counters
  (`src/node/bridge_state.cpp:368`); no supply ceiling. Raise only after
  audit and rehearsal (`contracts/deployments/ethereum-mainnet-v1.1.1.json:77-78`).
- **Effect:** (a) and (b) share Vault_E's cap; $50,000 into one vault takes
  5-6.5 days.
- **Bound:** caps rate-limit mint-side losses (broken light client, false
  sync committee, forced Arbitrum confirmation) to 10,000 per epoch per vault,
  **unbounded in total** until a B3 release. They do not bound a forged root.

---

## 12. Failure and human-intervention cases

| # | Failure | Effect | Funds lost? | Recovery (human?) |
|---|---|---|---|---|
| 1 | Fresh-lineage finality stalls | Deposits close within ~24 h; burns close; mints continue; producers rotate | None if quorum returns before expiry (B-23); else **all unreleased reserve of both vaults** | Restore quorum (yes) |
| 2 | No relay to one chain | Deposits close after ~24 h; catch-up ~3.2 epochs a day | After 30 days, that vault's unreleased reserve | Anyone relays (someone must) |
| 3 | A verifier expires | Certified leaves payable; burns refused | **That vault's unreleased reserve** | Migrate earlier, B-23 (yes) |
| 4 | Conflicting certificates (> 1/3 faulty with network control; V1 voting on regtest) | First wins; each receiving chain halts | Each halted vault's unreleased reserve; an orphan relayed first is paid | None (yes) |
| 5 | Arbitrum paused, disputed or `setOutbox` | (c) mints wait; Vault_A burns close; LP float lengthens | No | Wait (if permanent) |
| 6 | Outbox implementation, rollup or bridge changed | (c) mints stop; Vault_A burns close ~2 weeks later; LP float locked | No | Coordinated hard fork (yes) |
| 7 | USDT0 seizes or blocks Vault_A | Seen ≥ a week later; burns only within the surviving balance | Yes, shared | Wind-down (ruling) |
| 8 | Tether blacklists Vault_E; or deprecates USDT | Frozen; or Vault_E burns wait for a re-pin | Yes unless reversed; deprecation no if balances are honoured | Issuer; re-pin (owner) |
| 9 | Recipient or forwarder blacklisted; LayerZero or USDT0 paused | Frozen tokens or stuck forwarder funds; pauses only delay | That item; pauses no | Wallet pre-check; retry |
| 10 | Owner relayer stops | Deposits close within ~24 h; mints wait | Stranding if nobody relays for 30 days | Anyone relays (yes) |
| 11 | Ethereum fork beyond the schedule | Mints stop; Vault_E burns close within 2 days, Vault_A burns within ~14.6 days | No | Coordinated hard fork first (yes) |
| 12 | Committee below 4 or weight 900; without B-22, above 64 or beyond gas | Roots and deposits frozen until a qualified set returns; rotations continue (contracts spec §3.3) | None, unless rotations stop (a set beyond gas) for 30 days: stranding | A qualified set returns; else hard fork (yes) |
| 13 | Sequencer drops an LP-filled deposit; fill pruned; contract bug | LP loses the fill; immutable code | LP only; a bug possibly all | Migration leaf while live (yes) |
| 14 | False mint (sync committee, forced confirmation, proof bug) | ≤ 10,000 per epoch per vault, unbounded total; burnable against real reserves | Yes, shared | Monitors match mints to vault events; a coordinated B3 release fixes the fault, never suspending mints, which vaults cannot see (node changes §3.3) (yes) |
| 15 | `reserve_E` short | Ethereum exits wait (§4) | No | Vault_E deposits refill it |

---

## 13. Phasing and regtest milestones

Milestones run B3 regtest with no peers and local anvil chains, 31337 for
Ethereum (`src/kernel/chainparams.cpp:1136`) and 31338 for Arbitrum [I].
Today's regtest bridge is synthetic MANAGED_V1 (`:1130-1177`), refused by the
relayer (`contrib/b3bridge/b3_bridge_relayer.py:762-786`).

| Stage | Content | Regtest milestone | Mainnet gate |
|---|---|---|---|
| **S0 Now** | Old-vault notice; O2, O8-O12; commit §15; B-2 after O9 | — | None |
| **S1 H1 + H2** | Scheduled producer rotation with a 24 h escape (finality §5.1, FD-2); hardened checkpoint | **RT1**: H1 rotates every E; an escape block restarts the schedule; reindex, miner, staking, RPC agree | Rule-3 override; timing ruling; Set_3 coordination |
| **S2 Harness** | anvil/forge helpers; pin file; synthetic light client | **RT0** smoke; **RT2** light client; **RT3** V1 round trip | None |
| **S3 Lineage, contracts V2, core node slice** | Digest V2; lineage with B-22, B-23; verifier, prover, vault V2, migration leaf; node core: bUSD V2 id, Vault_E entry, V2 store, kinds 1-4, 6 and 9, leaf V2, Vault_E's reserve and wind-down bound, the Ethereum gate (C1-C4, E1-E8) | **RT4**: route (a) under digest V2 from the staging signer, through the Ethereum gate; bootstrap; byte vectors; conflict halt; migration | None until S5 |
| **S4 Node bridge V2, forwarders** | Vault_A with its reserve and gate (A1-A5), kinds 7, 8, 10-13 (12 mint queue, 13 conflict), LP rule, legacy closure; F_d, F_x | **RT5** two vaults (Vault_A refuses at `reserve_A` = 0); **RT6** (b), (d3); **RT7** (c) with and without an LP, heartbeats, a Vault_A release | With the S7 pins |
| **S5 V2 P3** | CommitQC replaces the V1 signer; mainnet lineage at R_f | **RT8**: RT4-RT7 re-run under P3 | D1, D3, D5, D10, FD-7, FD-12 (C8 split), FD-14, FD-16 ruled; model check; liveness rehearsal; multi-host soak (RP-10); hard fork |
| **S6 Audit, drills** | External audit; gas at 64 members, maximum absent | **RT9**: the §12 drills (regtest plan §6) | Audit; drills green |
| **S7 Deploy** | Both chains from a recent checkpoint; B3 pins entries; review window | **RT10**: production deploy scripts and pin flow on both anvils | Two builds agree on code hashes |
| **S8, S9** | Small transfers per route; open at pilot caps | S8 entry: the RT10 suite on release candidates | Disclosure |

S3 carries the Ethereum gate because RT4's burn and migration need it (node
changes §7.2 checks 2-4, §7.6); no regtest-only stand-in gate [I].

---

## 14. Owner decisions (README B-n)

Defaults in brackets.

- **B-1** Old stack lapses [yes, owner]. **B-2** Relay 814,051 before
  2026-10-08T17:11:59Z [skip unless O9 finds a leaf ≤ 814,051].
- **B-3** One bUSD over both vaults [yes, owner; record overrides of OD-8
  `:309` and contract §21, and (c)'s Arbitrum-governance trust].
- **B-4** Routes (a)-(d3), Ethereum exits only from Vault_E; Tron guidance
  only [yes].
- **B-5** LP fill as §7.2: exact claimed tuple, so a claimed fee unequal to
  the proven fee is invalid (B-5b); short fills and a second fill into an
  occupied bucket invalid (B-5a); vault ceiling equals B3's
  `max_fee_ppm_ceiling`, 10,000 ppm, never lowered, above-ceiling deposits
  mint to the recipient (B-5c); kind 8 always valid [yes; wallet cap 30 bps, H].
- **B-6** H1 with its 24 h escape (FD-2) plus H2 [yes; rule-3 override,
  timing ruling]. **B-7** Mainnet digest V2 only from P3 [yes]. **B-8** Fresh
  lineage from Snapshot(R_f − 1) [yes]. **B-9** Ancestry and root history [as
  §3.1].
- **B-10** Deposits close 24 h without a certificate or 72 h without a
  rotation; expiry 30 days (≥ 24); spacing 6 h; `MAX_CERTIFICATE_AGE`
  86,400 s on both chains, with relayer re-certification (contracts spec §4,
  C-14) [yes; alternative ~48 h on Arbitrum splits B-10 per chain].
- **B-11** Bootstrap from a recent certified checkpoint [start certificate;
  departs from the owner-accepted k-of-n default, so needs a ruling; k-of-n
  fallback]. **B-12** Deposit gate on a live verifier, healthy token
  and Vault_E caller rules; burn height **W2 = V** [yes; W2 > V would make
  bUSD minted in [V, W2) wait, a custodial wait against the goal of a live release
  path, to be disclosed (§15 item 12)].
- **B-13** Burn gates, one heartbeat payload, wind-down [node changes].
  **B-14** Caps 10,000 per block and epoch per vault [yes].
- **B-15** Capped fees plus treasury relayer [yes]. **B-16** Relay policy,
  chaining [§10]. **B-17** Native B3 for burns [disclose].
- **B-18** Outbox implementation, `rollup`, `bridge` pins; a change stops (c)
  mints, then Vault_A burns, until a hard fork [yes].
- **B-19** Legacy bUSD frozen [yes]. **B-20** Reconciliation commit [yes].
  **B-21** Separate S5 and S7 releases [yes].
- **B-22** Committee: stake-ranked top 64 above a per-key weight floor
  (V2-D5) [yes; floor W/128, ~0.78% of weight, as FD-14, checked against
  O2; or accept and disclose §9's Sybil risk].
- **B-23** Extension ≥ 46,080 blocks (FD-7); migration leaf; residual
  stranding explicitly accepted [yes; needs owner acceptance].
- **B-24** Adapter gate: USDT by kind 9; USDT0 upgrades unobservable
  [recorded waiver for USDT0].
- **N-1 to N-5** (node changes §16) [node defaults; N-1 (new deposits
  backstop a dead vault's losses) needs owner acceptance].

---

## 15. Governing text to reconcile before activation

Each needs a reviewed commit before the related part activates.

1. `CLAUDE.md:92-104` (rules 5-6: managed vault, adapter review) → V2,
   honest labels, B-24.
2. `CLAUDE.md:87-88` (rule 3), `:12-13` (no consensus change) → **one
   override: H1 with its escape, H2, the fresh lineage, the B-22 committee
   and the bridge V2 consensus regime** (node changes §3-§10: one bUSD, vault
   registry, kinds 7-13, per-vault caps, burn gates, LP rule; finality §16
   and regtest §8 cite it); `:5-7`, `:11` (workspace lock) → own-branch
   authorization if wanted.
3. OD-8, `doc/design/b3-open-decisions.md:306-309` (managed release; an L2
   asset is distinct; L1 "is the stable origin") → one bUSD over L1 and L2;
   Outbox proofs inheriting Arbitrum governance; signer-set ban unchanged.
4. `doc/design/b3-open-decisions.md:251-256` (adapter gate; smoke-vault
   recheck) → B-24; recheck kept.
5. Architecture contract §21 (`doc/design/b3-architecture-contract.md:367-375`,
   L1 and L2 versions "can never collapse into the same asset") and
   `:636-643` (vault-derived AssetId) → one B3-native bUSD id; §22
   unaffected.
6. `doc/design/b3-master-handoff.md:101-123` and contract §45 (`:596-610`):
   single USDT; 3-of-4 Set_0 bootstrap (handoff `:106`, contract `:608-610`)
   → dual reserve, contagion, stranding; recent certified checkpoint (B-11).
7. `doc/design/b3-bridge-threat-model.md` T8 (`:213-221`) and the stale
   comment `src/kernel/chainparams.cpp:1146-1148` → dual reserve, LP,
   Outbox, stranding, Sybil.
8. `doc/design/b3-flowmesh-v2-stage0.md:43`, `:69` (preserve the deployed
   bridge), `:709` (new digest "Rejected"), `:711` (over 64 members) → fresh
   start, digest V2, B-22.
9. `doc/design/v2-stage0-r1-consensus.md:409-415`, `:407`, `:370-376`,
   `:466-467` → release-fixed genesis (FD-12); H1 in V2 (D3); B-22 (D5); old
   locks scoped to the old lineage.
10. `doc/design/b3-cross-chain-finality-v1.md:3-11` (V1 frozen), `:172-178`
    (one stake universe; handover-gated producers) → V2 beside a frozen V1;
    H1, B-22, per-lineage journals, B-23.
11. Owner 2026-09-29, "bridge need to be fix without changeing the contract"
    (`.../study/check.md:27`, memory only) → superseded by the 2026-10-03
    fresh start.
12. W drift (`src/kernel/chainparams.cpp:270` vs
    `doc/design/b3-master-handoff.md:115-118`, W unset with a disclosed
    custodial wait) → record V1's W (811,001) and V2's W2 = V (B-12).
13. Stable-quote ruling (memory only) vs `doc/design/b3-master-handoff.md:170-175`
    → commit it.
14. `doc/design/b3-bridge-bls-proposal.md:178-179` (Arbitrum rejected) →
    Outbox proofs.
15. This 2026-10-03 direction → commit it.

---

## 16. Considered and rejected

- **USDC**: Circle "will never mint on our chain" (owner).
- **Keeping the old contract**: broken lineage, no ancestry
  (`contracts/src/B3FinalityVerifier.sol:199-224`).
- **A signer set attesting Arbitrum deposits**: forbidden by OD-8.
- **An admin or rescue key against stranding**: a new trusted party; the
  migration leaf covers planned moves.
- **Arbitrum receipts by header ancestry** (~14,400 headers per assertion
  against 32, `src/bridge/proof.h:37-47`) and **Rollup storage** (changed at
  BoLD, https://github.com/OffchainLabs/nitro-contracts/blob/main/src/rollup/BOLDUpgradeAction.sol#L418-L420).

---

## 17. Open items and owner reads

**Owner reads:** **O2** `getfinalitystatus`, `getfinalityset 3` (lineage;
Set_3 members, online and unstaked); **O8** block spacing; **O9** legacy
minted balance and any burn leaf ≤ 814,051; **O10** Outbox implementation and
slots 0-1, `confirmPeriodBlocks`, assertion period, fast confirmer, admin
owners; **O11** USDT fee, owner and slots, USDT0 implementation, Safe and DVN
configuration; **O12** Outbox `eth_getProof` sizes.

**Open across documents:**

- **FD-15:** whether a conflict halt should stop both verifiers for good,
  and a replacement-successor path (finality §10.2; contracts spec §3.4).
- **Re-credit** of burns above a frozen root (contracts spec §10; not taken
  up).
- **Owner acceptance:** B-11's departure from the accepted k-of-n default, B-23's residual, N-1, and
  the wind-down margin (node changes §8.6).

**Unverified [H]:** V2, `sendTxToL1` and USDT `approve` gas; LayerZero fees
and DVN depths; anvil EIP-2537; every minute figure until O8.
Addresses: contracts spec Appendix A.
