# B3 Bridge V2: regtest-first end-to-end plan

> **Status: PROPOSAL, no governing authority.** It ranks below every owner
> ruling and every reviewed, committed governing document. Nothing here is
> built, run, ratified or activated.
>
> - 2026-10-03, revision 6 (cross-document fixes). Base `b4b1de4`, branch
>   `feat/flowmeshV2-dev-claude`.
> - Implements the owner's 2026-10-03 instruction (relayed, not yet in the
>   repo): "I need everthing to be working atleast on regtest". Follows the
>   [architecture doc](b3-bridge-v2-arbitrum-proposal.md), which owns every
>   latency number (§7), the stages (§13) and decision ids (§14). No node,
>   chain RPC or public network was contacted.

**Labels.** [E] established technique or verified fact; [I] design choice made
here; [H] estimate or unverified. Repo facts are `path:line` from the worktree
root; external facts are URLs. New files, flags and RPCs are [I].

**This document owns** "Harness" (§3), "Mocks" (§4) and the test content of
milestones RT0-RT10 (§5); architecture §13 owns the stages and RT numbering.

---

## 0. Summary

**"Working on regtest" [I].** Every route (a)-(d), the LP rule, both burn
gates, per-vault caps, the legacy closure and every drillable failure and
recovery row (architecture §12) run as automated tests that start only local
processes: B3 regtest nodes linked only to each other, and anvil chains for
Ethereum (31337, `src/kernel/chainparams.cpp:1136`) and Arbitrum (31338).

**Today none of it runs** [E, grep]: no functional test touches the bridge or
uses anvil, and four blockers stop even route (a) (§2).

**Plan.** S2 builds the harness on today's V1 contracts (RT0, RT2, RT3)
without consensus changes and can start now; the rest waits for S0 and the
override of architecture §15 item 2 (§8). Until P3 the V1 single-vote signer
produces digest V2 behind a regtest-only knob (B-7). RT4-RT7 run first on
fixture I (V1 lineage intact, no H1), so the bridge can go green if the H1
ruling slips, then on the mainnet-shaped fixture B (broken lineage, H1); RT8
re-runs all under P3. Mainnet opens only after H1 and P3.

**Largest risk.** A fresh-lineage break strands the uncertified reserves of
**both** vaults for good (contracts spec §10; finality §10.1). B-23
(architecture §8.1) puts any break past the 30-day expiry and adds a
migration leaf, but thirty days without a relayed rotation, or a conflict
halt, still strands, a residual the owner must accept. RT9 drills the break,
expiry, conflict halt and migration leaf (§6.2 rows 1b, 3, 4b; §6.3 R6).

**Honest limit.** Mocks prove B3's rules, our contracts and tooling, not real
Ethereum, Arbitrum, USDT0 or LayerZero behaviour, nor B3 finality liveness on
a real network, which failed in V1 (§4.2).

---

## 1. Ground rules

| Rule | Enforcement |
|---|---|
| Local only | Helpers refuse hosts other than `127.0.0.1`/`localhost`; anvil never forks; the framework disables discovery and seeds (`test/functional/test_framework/util.py:566-576`) [E] |
| "No peers" | Nodes link only to the test's own nodes, as `test/functional/feature_b3_finality_soak.py:44-140` (RP-8) |
| No installs | Python stdlib; BLS in C++; `forge --offline` (`contracts/foundry.toml`) |
| Never mainnet | Knobs and pin file refused outside regtest; tools are `B3_BRIDGE_TOOLS` executables outside `b3coind` (`src/CMakeLists.txt:535-555`); mocks in `contracts/regtest/`, barred from production scripts |

---

## 2. Starting point

Exists [E]: a synthetic MANAGED_V1 regtest bridge on chain 31337
(`src/kernel/chainparams.cpp:1130-1177`); a consensus light client
(`src/node/bridge_state.cpp:400-475`); BLS, SSZ and deterministic keys
(`src/crypto/bls.h:101-167`; `src/bridge/ssz.h:149-253`); a stdlib relayer
(`contrib/b3bridge/b3_bridge_relayer.py:1119-1177`); bridge and key RPCs
(`src/wallet/rpc/assets.cpp:1753`; `src/wallet/rpc/staking.cpp:385-420`);
4-node mock-time tests (`test/functional/feature_flowmesh_release.py:41-58`).

| Blocker [E] | Where | Fix |
|---|---|---|
| 1. Nothing points regtest at anvil addresses or a synthetic checkpoint | `src/chainparams.cpp:44-80` | Pin file (§3.3) |
| 2. Relayer accepts only decentralized mode | `contrib/b3bridge/b3_bridge_relayer.py:762-786` | Pin file sets it |
| 3. Outbound proof RPCs refuse MANAGED_V1 | `src/rpc/blockchain.cpp:4004-4012`, `:4963-4970` | Same |
| 4. Deposits need a ready verifier | `contracts/src/B3StakerBridge.sol:95-96` | Certificates, or a mock verifier (RT3a) |

---

## 3. Harness

### 3.1 Topology

```
 anvil L1 :8545 (31337)                     anvil L2 :8546 (31338)
   MockTetherToken, Vault_E, Verifier_E       MockUSDT0, MockOFT, MockLzEndpoint
   F_d/F_x, MockOFTAdapter, MockLzEndpoint    Vault_A, Verifier_A, MockArbSys 0x..64
   MockOutbox (EIP-1967 proxy)
      |   ^---- test rollup, LayerZero test relayer ----^
 mock beacon :5052 + synthetic committee tool
      |
 relayer V2 <---- JSON-RPC to both anvils ---->
      |
 B3 regtest n0-n3 (validators) and n4 (sync check), linked only to each other
```

### 3.2 Components (all new) [I]

| Component | Path | Role |
|---|---|---|
| anvil helper | `test/functional/test_framework/anvil.py` | Start (the Ethereum anvil with `--enable-tx-gas-limit` for EIP-7825's cap, contracts spec §12), mine, warp, snapshot, `anvil_setCode`, forge deploy; skip if tools missing |
| Bridge helper | `test/functional/test_framework/b3bridge.py` | keccak/RLP/MPT from `contrib/b3bridge`; pin writer (§3.3); clock (§3.5); evidence |
| Committee tool | `src/bridge/regtest_beacon.cpp` | §3.4 |
| Certificate tool | `src/bridge/regtest_certify.cpp` | Signs digest V2 start, conflicting or forged certificates; refuses non-test keys and mainnet domains (unit-tested) |
| Mock beacon | `contrib/b3bridge/regtest/mock_beacon.py` | Serves the tool's JSON; finalize, rotate, k-of-512, conflicting header, fork switch |
| Test rollup; LayerZero relayer | `contrib/b3bridge/regtest/` | §4.1 |
| Outbound submitter | relayer V2 (node changes §11.3) | Certificates, `reportConflict` and releases to both anvils |
| Deploy script | `contracts/script/RegtestDeploy.s.sol` | Mocks and V1 or V2 stacks; manifest |
| Stack driver | `contrib/b3bridge/regtest/run_local_bridge.py` | The tests' bring-up, left running (§7) |

### 3.3 Startup knobs and the pin file

**Replay rule.** Restart replays every certificate under current parameters
(`src/node/finality_tracker.h:86-90`; `src/node/bridge_state.h:381-386`); one
that fails resets the tracker (`src/node/finality_tracker.cpp:250-262`) and
pauses the staker (`src/node/miner.cpp:594-595`) [E], and switching MANAGED_V1
to decentralized mode changes every withdrawal root
(`src/node/bridge_state.cpp:1162-1174`). So **no parameter may change how a
connected block is judged** [I].

| Kind | Content | When |
|---|---|---|
| Startup knobs | `-b3freshlineageheight=<R_f>` (staging signer, B-7); `-b3h1height=<R_h>`; `-b3h1escaperounds=<n>` (finality dependency §5.5 item 4); `-b3regtestwithholdcertificates=<a>-<b>` | From the first start, like the existing `-b3*` knobs (`src/chainparamsbase.cpp:20-22`) |
| Pin file `-b3bridgepins=<path>` (node changes §12.2) | External pins only (addresses, code hashes, tokens, deployment blocks, caps, LP ceiling, light-client checkpoint and forks, Outbox pins, sink) and activation heights (V1 entry, V, W2, re-pins) | Only if every added or changed pin takes effect above the tip; the node records the file's hash (`-reindex` re-judges history, so it is no cure) |

Both are regtest-only (`-b3modernregtest -b3flowmeshtest`, unit-tested);
without the file MANAGED_V1 defaults stay
(`src/test/bridge_params_tests.cpp:497-540`); the file must pass the node's
startup assertions (node changes §3.4).

**Pin-writer checks [I]**, each also proposed as a node startup assertion:

1. Per-block cap ≥ the vault's deployed `MAX_DEPOSIT_RAW`, or a larger
   deposit waits forever (node changes §9); today's deploy check compares two
   environment values only (`contracts/script/ProductionBridgeConfig.sol:129`).
2. Vault LP fee ceiling = B3 `max_fee_ppm_ceiling` (B-5c).
3. Windows follow C-12 (contracts spec §4). **Profile R** (default) scales
   every verifier window and B3 gate threshold by 1/36 (a mainnet day to
   2,400 s, one regtest epoch at 60 s: 6 h → 600 s, 30 days → 72,000 s, skew
   → 2,422 s); **profile M** keeps mainnet values (E1, skew drills). B3
   mirrors the deployed immutables; node assertion 5 already takes the test
   rollup's pinned delay (50,378 s > 35,280 s, node changes §3.4). Relayer
   cadence and `fill_max_age_blocks` may shrink, each named by its test.
4. Each verifier's `BRIDGE_ACTIVATION_HEIGHT` = V.
5. Deployment ≤ checkpoint execution block ≤ deployment + 20,000
   (`src/node/bridge_state.cpp:170-184`, `:420-426`; `src/bridge/proof.h:43`).

### 3.4 Synthetic Ethereum light client

The real light client and proof checks run against anvil, no shortcut.

- **Keys [I]:** 512 per period p, secret i =
  `FromIKM(sha256("B3-REGTEST-SYNC-COMMITTEE-V1" ‖ p ‖ i))` (`src/crypto/bls.h:101`);
  public and worthless elsewhere.
- **Headers [I]:** anvil's real blocks; B3 uses hash, number, receipts root,
  timestamp and (new) state root (`src/bridge/exec_chain.h:41-99`) [E].
  Beacon trees are sparse, so every branch is genuine.
- **Slots and signing:** slot = S0 + N (`src/bridge/eth_light_client.h:187-205`)
  [E]; sync-committee domain (`src/bridge/ssz.h:235-253`); 512, 342 or 341
  signers; one update per period crossed (`:288-295`).
- **Output:** Beacon-API JSON for `src/bridge/lc_json.h:99-187`, read by the
  unchanged relayer and `b3-bridge-ethcheck`.
- **Guard:** tool and node refuse mainnet genesis roots and fork versions.

Mainnet data stays in `src/test/bridge_eth_light_client_tests.cpp:5-15`.

### 3.5 Clocks

One monotonic clock drives L1, L2 and B3 mock time [I].

- Before a handover the clock jumps at least the deployed
  `MIN_EPOCH_DURATION` plus one second (6 h on profile M, 600 s on R); as
  `MAX_CERTIFICATE_AGE` without a certificate closes deposits, the harness
  relays the handover or a keep-alive before the next deposit.
- Jumps stay under the light client's 27.3 h lag bound; a fresh update
  precedes any mint.
- Gates use `now_eff = max(nTime, light-client timestamp)` (node changes
  §8.4); RT4 runs B3 behind and ahead of L1.
- Skew drills (profile M) set L2 24 h behind or 768 s ahead; E1 runs
  without jumps.

### 3.6 Fixtures and bring-up

**Flags [I]:** `-b3epochlength=40 -b3checkpointinterval=5 -b3checkpointdepth=3
-b3maxepochextension=40 -b3minfinalityset=4`, corridor 130, so M = 131
(`src/consensus/era.h:141-146`) and checkpoints fall at 131 + 5k
(`src/modern/finality_schedule.h:61-63`). R_f = 262 puts R_f − 1 on the grid
(finality dependency §6.2, §7); the harness asserts it.

| Fixture | V1 lineage | H1 | Used by |
|---|---|---|---|
| **I** | Intact; V1 decentralized pins from height 0 | No | RT3; RT4-RT7 first; legacy closure |
| **B** | Broken at 211 = 131 + 40 + 40 by withholding (`src/node/finality_tracker.cpp:133-137`) | `-b3h1height=220` | RT1; RT4-RT7 repeat before S5 |

On fixture B the automatic staker pauses once V1 breaks
(`src/node/miner.cpp:558-560`), so blocks are produced manually; H1 should
switch `PlanRegtestFinalityProduction` (`src/node/regtest_finality_policy.h:34-44`)
to the producer schedule via H1-R2's helper (finality dependency §5.2) [I].

**Bring-up [I]:**

1. Start anvils; deploy mocks and, for RT2 and fixture I, the V1 stack
   (addresses follow deployer nonces) [E].
2. Build the synthetic bootstrap inside check 5's window; write the pin file.
3. Start n0-n3 with every knob and the pin file from height 0; mine the
   corridor; import deterministic finality keys; stake and bind.
4. V2: past R_f, read Set′_0, block R_f − 1 and start epoch k's set hash
   (k = 0, or k ≥ 2 after two rotations); deploy verifiers and vaults V2;
   `initialize` each with an ordinary epoch-k certificate plus `leaf0Path`
   (contracts spec §5).
5. Add the V2 checkpoint and section with V and W2 above the tip; restart.
6. Start mock beacon, test rollup, LayerZero relayer and relayer V2.
7. RT4-RT7 end with n4 syncing from genesis and n0 run with `-reindex`; all
   agree on `getbridgestatedigest` and `getfinalitystatus`.

### 3.7 Shared byte vectors

Only the contracts spec defines bytes (vectors: its §2.5); this plan defines
none. **RT4 entry criterion [I]:** C++, Foundry and the harness
reproduce every entry of `gen_v2_vectors.sh`. Each test also writes
`bridge_evidence.json`: blocks per step, indicative gas, jumps, assertions,
stranded amounts.

---

## 4. Mocks

### 4.1 Mock list

Written from scratch in `contracts/regtest/`, each with a Foundry test;
nothing vendored (nitro-contracts is BUSL-1.1,
https://github.com/OffchainLabs/nitro-contracts/blob/main/src/bridge/AbsOutbox.sol#L3) [E].

| Mock | Behaviour |
|---|---|
| MockTetherToken | Tether's storage layout (slots 0 `paused`, 2 `balances`, 6 `isBlackListed`, 10 `deprecated`), pinned to the node's constants by a `vm.load` test (node changes §8.2; mainnet via O11); no-return transfers; `whenNotPaused` and sender blacklist on `transfer`; `destroyBlackFunds`; `deprecate` forwards; fee switch (https://sourcify.dev/server/v2/contract/1/0xdAC17F958D2ee523a2206206994597C13D831ec7?fields=sources) [E] |
| MockDepositVerifier (RT2, RT3a) | `CHAIN_DOMAIN()`, `BRIDGE_ACTIVATION_HEIGHT()`, `depositViable()`; code hash passed to the vault, whose constructor checks them (`contracts/src/B3StakerBridge.sol:75-89`) [E] |
| MockArbSys at `0x…64` | Arbitrum's sendHash and leaf (node changes §5.3) and `L2ToL1Tx` (https://github.com/OffchainLabs/nitro/blob/master/precompiles/ArbSys.go#L113-L173) [E] |
| MockOutbox, EIP-1967 proxy | `rollup` slot 0, `bridge` slot 1, `roots` slot 3; `updateSendRoot` only from `rollup`; `updateRollupAddress()` by the rollup's owner re-points it; upgradable (AbsOutbox.sol `#L28-L32`, `#L100-L111`) [E]; a test-only `setBridge()` re-points slot 1 [I] |
| Test rollup | Root from `L2ToL1Tx` posted after a set delay; pause, double post, forced root (https://github.com/OffchainLabs/nitro-contracts/blob/main/src/libraries/MerkleLib.sol#L29-L57) |
| MockUSDT0 | Blocklist (`isBlocked`, `destroyBlockedFunds`); a burn authority over any address (architecture §9) |
| MockOFT, MockOFTAdapter, MockLzEndpoint, LayerZero relayer | Burn/lock and packet; mint/release on delivery; retryable failures, pause, delay; DVNs not modelled (the relayer is trusted) |

Kind 7 pins the Outbox proxy code hash, implementation, slot 0 and slot 1
(node changes §5.2), so RT7 re-points each and expects it to fail closed.

### 4.2 What regtest cannot prove

| Gap | Closed by |
|---|---|
| **B3 finality liveness** under real latency, churn and validator counts: V1 passed a four-node soak (`test/functional/feature_b3_finality_soak.py:5-15`) and still deadlocked twice (`doc/release-notes-v1.1.3.md:58-63`; `doc/finality-quorum-deadlock-814191.md:16-20`) | **Required** owner-run multi-host soak (regtest chain type, never mainnet) with induced outages, before the S5 mainnet gate (RP-10) |
| Real Outbox layout and leaf | Owner-run read-only capture as a C++ vector (RP-7, O12) |
| USDT0 OFT behaviour; real gas | Source review; contracts spec §11; S8 |

---

## 5. Milestones RT0-RT10

| RT | Stage | Test (`test/functional/feature_b3_…`) | Needs first |
|---|---|---|---|
| RT0 | S2 | `bridge_rt0_toolchain.py` | anvil helper |
| RT1 | S1 | `h1_producer_rotation.py` + C++ | S0; H1 |
| RT2 | S2 | `bridge_rt2_light_client.py` | Pin file; tools |
| RT3 | S2 | `bridge_rt3_v1_route_a.py` (`--deposit-only` = RT3a) | RT2; submitter |
| RT4 | S3 | `bridge_rt4_v2_route_a.py --fixture=I\|B` | S0; shared vectors; lineage; contracts V2; node core V2 slice incl. kind 9, Vault_E reserve and Ethereum gate |
| RT5 | S4 | `bridge_rt5_two_vaults.py` | Vault_A entry, per-vault caps, kind 13, legacy closure |
| RT6 | S4 | `bridge_rt6_route_b_d3.py` | Forwarders; OFT mocks |
| RT7 | S4 | `bridge_rt7_route_c_lp.py` | Kinds 7-13; B-5a-c |
| RT8 | S5 | RT4-RT7 `--finality=p3` | P3 |
| RT9 | S6 | `bridge_rt9_drills.py --drill=<name>` | All |
| RT10 | S7 | `bridge_rt10_deploy.py` | V2 deploy scripts |

All go in `EXTENDED_SCRIPTS` (`test/functional/test_runner.py:87`) until
measured (runtimes [H], minutes each).

**RT0 (S2).** Offline forge build and test; anvil `--hardfork osaka
--enable-tx-gas-limit` runs an EIP-2537 vector; headers re-encode to their
hash (`contrib/b3bridge/eth_live_test.py:70-92`); historical `eth_getProof`
verifies; the V1 stack deploys; B3 survives a one-day jump [H].

**RT1 (S1, after S0).** Finality dependency §5.5's five items, with manual
production (withholding breaks V1), plus: burns stay invalid while broken
(`src/node/finality_tracker.cpp:46-63`); the escape uses
`-b3h1escaperounds`; the late upgrader runs with `-b3reorghorizon=40`
(`src/chainparams.cpp:75`) and gets `-b3h1height` only past R_h + 40 (§3.3's
one deliberate knob change).

**RT2 (S2).** Pins from height 0. Bootstrap, updates, one rotation. Rejected:
341 of 512, wrong genesis root, unpinned fork, two-period jump, conflicting
finalized header (`src/node/bridge_state.cpp:461`), no-op update (`:472`),
mint with the head 8,192 slots behind. Checkpoints at deployment and +20,000
accepted, +20,001 refused. The unchanged relayer drives it (`localhost` and
`127.0.0.1` meet its distinct-origin rule,
`contrib/b3bridge/b3_bridge_relayer.py:93-106`); reindex agrees.

**RT3 (S2, fixture I).** RT3a, first green: mock verifier, deposit, mint,
replay refused. RT3: `signbridgebootstrap` ×3, `initialize`, certificates,
deposit, mint, `bridgewithdraw`, certificate, `release`. A deposit of exactly
`MAX_DEPOSIT_RAW` under the smallest cap mints. Both relayer modes (B-16);
FlowMesh V1 credit (`src/consensus/flowmesh_params.h:15`).

**RT4 (S3; fixture I, then B).**
- Entry: shared vectors green. Lineage at R_f = 262 from Snapshot(261); the
  staging signer runs only under the knob; an old-journal orphan lock does
  not block signing.
- Bootstrap (B-11): `initialize` by an ordinary start certificate of epoch 0,
  and of epoch k ≥ 2 on a second deployment; wrong start-set hash, bad
  `leaf0Path` and late calls revert; watcher `cancel` → CANCELLED; deposits
  closed through review. A third deployment initializes through the K_OF_N
  fallback (3-of-4 regtest attester keys, contracts spec §5), then has no
  callable path. Before any deposit, `getfinalitylineage` matches every
  deployed pin on both anvils (finality §6.5, §11).
- Certificates on both sides of V (contracts spec §3.3 step 5): a pre-V
  handover certificate (root zero) rotates both verifiers; from V the root
  is the non-zero empty-tree root; a non-zero root below V or a zero root
  from V reverts; deposits open only after a certificate at or above V.
- Migration leaf (B-23; node changes §7.6): pin-file entry (v = Vault_E, v′
  on its chain and token, h_m); at the first block ≥ h_m with v's gate READY
  (kind 9 and the Ethereum gate, S3 code), B3 appends an **ordinary leaf V2**
  paying `reserve_v` to v′; Vault_E releases it once, v′'s `absorb()` counts
  it and v′ pays its own leaves; no leaf while the gate is closed; a v′ on
  another chain or token fails startup [I].
- Route (a) under digest V2, releasing a real B3 leaf from Vault_E (node
  changes §7.3); FlowMesh V1 credit.
- Ethereum gate (node changes §8.4, C1-C4, E1-E8): opens on a kind 9
  observation, closes near expiry, reopens after rotation; both clock
  orders; any status but LIVE closes it.
- Refused by the verifiers: V1 digest, ruleset-1 header, other lineage,
  history missing the last accepted checkpoint. Refused by B3's V2 carrier
  rule, the block invalid as for `bad-finality-cert` (`src/validation.cpp:3853`):
  a V1-tagged signature, a ruleset-1 header, an other-lineage certificate. A
  conflicting certificate landing first makes the canonical one revert;
  `reportConflict` to both chains halts the verifiers.
- Relaying every third certificate still passes ancestry. A release against
  an older accepted root succeeds after a newer one; a root below the leaf,
  an unknown root and a second release revert.
- Deposit gate closes after `MAX_CERTIFICATE_AGE` without a certificate, or
  `DEPOSIT_CLOSE_AFTER` without a rotation; releases continue. Tether
  paused, deprecated or blacklisting refuses deposits; a release while paused
  or blacklisted reverts until cleared.
- n4 and `-reindex` agree.

**RT5 (S4).**
- One start certificate initializes both stacks; certificates to both; one
  bUSD id.
- Vault_E only for releases: a real Vault_E leaf pays against Verifier_E's
  root and reverts at Vault_A. With `reserve_A` = 0 a burn naming Vault_A is
  refused; Vault_A's own release waits for RT7 (kinds 7, 8 and 11 create
  `reserve_A` and its gate). A burn naming Vault_E above `reserve_E` is
  refused until a route (a) deposit refills it (architecture §12 row 15).
- Caps: small `MAX_DEPOSIT_RAW` equal to the block cap; an exact-size deposit
  mints; others are refused, then mint in later blocks and epochs (no Vault_E
  queue).
- Legacy closure (fixture I): a V1 burn valid at V − 1 is invalid at V; V1
  updates and mints refused from V; label "bUSD (legacy V1)" (B-19).
- Kind 13 (node changes §8.5): a conflicting digest-V2 certificate at a
  reached index sets a sticky lineage conflict closing both gates; above B3's
  tip it waits; canonical or wrong-set evidence is refused.

**RT6 (S4).** (b): MockOFT `send` to F_d, delivery, flush within the fee cap,
mint; failed delivery retries; with deposits closed F_d refunds or waits; an
over-cap flush reverts. (d3): burn to F_x, release, `forward`, arrival on L2;
with LayerZero paused, funds wait and refund.

**RT7 (S4).**
- Vault_A deposit; root posted; update, kind 7, kind 8; mint.
- Payee (node changes §6): no fill or `maxLpFeePpm` = 0 → recipient; matching
  fill in a strictly earlier block → LP payout.
- Invalid (balances unchanged): short of its own claim; claimed fee 0 or
  above ceiling; fill in or after the mint's block; a second fill into an
  occupied bucket (B-5a).
- Valid but unmatched (the recipient is paid): wrong claimed recipient or
  amount; claimed fee unequal to the proven fee (B-5b); fill older than a
  shortened `fill_max_age_blocks`.
- Fees at the ceiling, ceiling + 1 and the contract maximum each revert at
  the vault or mint exactly once, above the ceiling to the recipient (B-5c).
- Refused: other L2 sender, wrong index, unposted root, replayed id; a
  sendRoot posted twice works.
- An Outbox upgrade, a re-pointed rollup and, separately, bridge
  (`setBridge()`) each fail kind 7 closed: kinds 8 and 11 on new roots stop,
  and Vault_A burns close once the last heartbeat ages past A2 (B-18).
- Heartbeats open the gate; (d2) within `reserve_A` releases a real B3 leaf
  from Vault_A (node changes §7.3); above it is refused; a stale heartbeat
  closes the gate; FlowMesh V1 credit.
- Queue (kind 12, node changes §6): over-cap kind 8 mints queue
  (`output_index` 0xFFFFFFFF); kind 12 issues the head to its stored payee as
  caps allow; an empty queue or no headroom refuses it; a reorg undoes pops;
  Vault_E at cap does not delay Vault_A.

**RT8 (S5).** RT4-RT7 unchanged under P3, plus FlowMesh V2 credit once V2
exists (RP-9).

**RT10 (S7).** Production V2 scripts and pin flow on both anvils; two builds
agree on code hashes; a checkpoint within 20,000 blocks of deployment is held
through the 7-day review. The full suite on release candidates gates S8.

---

## 6. Coverage

### 6.1 Routes

(a) RT3-RT5; (b) and (d3) RT6; (c) with and without an LP, and (d2), RT7;
(d1) RT3-RT5; (e) Tron is guidance only.

### 6.2 Failure drills (RT9; architecture §12)

Times are profile M's; profile R scales them (§3.3 check 3).

| Row | Drill |
|---|---|
| 1 Finality stalls | With `-b3maxepochextension=200`, so the lag rule (C3) acts before any break: withhold votes; deposits close within 24 h of each chain's last certificate; burns close; mints continue; fixture B producers rotate |
| **1b Stall past the break** | Extension 40 (B-23's 46,080 is 32 epochs, 1,280 here); stall past epoch start + E + extension: lineage broken; neither verifier accepts or rotates again; burns refused for both vaults; stranded amount recorded |
| 2 No relay to L2 | Vault_A deposits close within 24 h of Verifier_A's last certificate; Vault_E unaffected |
| 3 Expiry | 30 days: certified leaves release; new burns refused; no further root; that vault's stranded uncertified reserve recorded |
| **4b Split** | C_A first on L1, C_B first on L2: one bUSD burned naming Vault_E on branch A and Vault_A on branch B is paid twice, payable after any halt (C-1); cross-posted `reportConflict` halts both; assert the halts, the double-paid amount and window, and burns closed for both vaults (C4) |
| 5 Arbitrum paused | (c) mints wait; heartbeats stale, Vault_A burns close |
| 7a USDT0 blocks Vault_A | Block only: release reverts; `vaultBlocked` closes burns (A1) |
| 7b USDT0 burns its balance | After a partial burn of Vault_A's balance, burns pass up to the wind-down bound and fail one raw unit above, the gate staying open (A5, node changes §7.2 check 3); a zero balance with no later deposit refuses all |
| 7c Verifier_A on a non-canonical root, LIVE | A3 sets the lineage conflict; burns close, sticky (C4) |
| 8 Tether blacklists or deprecates Vault_E | Deposits refused; releases revert or follow the forwarded contract |
| 9 Recipient or F_x blacklisted | Unpaid; wallet warns |
| 10 Relayer stops | Deposits close within 24 h of the last certificate; another instance resumes |
| 11 Unscheduled Ethereum fork | Updates refused; mints stop; Vault_E gate stale |
| 12 Committee above 64 | Without B-22, 61 extra stakes: burns close, roots refused, rotations continue; with B-22 the committee is truncated to 64 and roots continue |
| **12b Offline Sybil** (architecture §9; finality §13 NC3) | Two extra keys below the W/128 floor withhold. Without B-22 (n = 6, q_h = 5) the honest four cannot certify, gates close as row 1, the precommitted successor never certifies, and at break or expiry both vaults strand, both amounts recorded (finality §10.2). With B-22 the keys are excluded and certificates continue |
| 13 Sequencer drops a filled deposit; fill pruned | No mint; the fill stays a payment; an expired fill stays unmatched (RT7); a contract bug cannot be drilled |
| 14 False mint | R4 (§6.3) |
| 15 `reserve_E` short | RT5 (§5) |
| E1 Epoch-start wait | Profile M, no jumps, `-b3epochlength=1440`, +60 s per block: a burn at an epoch's first checkpoint waits only for the rotation certificate; after two epochs of withheld relays, catch-up runs one rotation per 6 h; record burn-to-release delay |

Rows 4 and 6 run in RT4 and RT7, row 9's LayerZero pause in RT6; the
without-B-22 cases of rows 12 and 12b select the full-snapshot committee by
a regtest-only knob [I]. **Reorgs:** invalidating an LP fill's block also
disconnects the later mint, and reconnection pays exactly one mint to the
right payee; an anvil deposit reverted before synthetic finality never mints.
Staging journals keep ancestry locks (`src/node/finality_signature.cpp:607-618`)
and breaks are not healed, so reorg drills stay above the last voted
checkpoint or run last on a throwaway fixture.

### 6.3 Recovery drills [I]

Each is a pin change effective above the tip, or a redeploy.

| # | Recovery | Asserted |
|---|---|---|
| R1 | Re-pin the Outbox implementation (row 6) | Kinds 7, 8, 11 resume; paused deposits mint once |
| R2 | New fork version (row 11) | Updates and mints resume |
| R3 | Replacement verifier and vault entry (rows 3, 4) | Old entry burn-closed; stranded amount reported |
| R4 | Forced false root | Unbacked mint bounded only by Vault_A's caps |
| R5 | FD-8 re-bootstrap after 1b | New lineage works; old reserves stay stranded |
| R6 | Migration before a predicted expiry (B-23) | Successor pinned above the tip; the leaf released once and absorbed; certified old leaves still pay; impossible after expiry or a halt |

**Retries:** a killed relayer restarts from sqlite without duplicates; a
repeated kind 7 is a no-op; a reverted `release` succeeds once its cause
clears; failed deliveries and refused flushes succeed later.

### 6.4 Latency structure

Block counts only; minutes belong to architecture §7 (unmeasured, O8).

| §7 step | Regtest assertion |
|---|---|
| t_chain | Update then mint (or kinds 7, 8) within 2-4 blocks |
| t_fm | FlowMesh V1 credit 31-32 blocks after the mint |
| t_fin | Burn to certificate in 5-10 blocks: 1 + 0-4 + depth 3 + 1-2 [computed]; mainnet uses the 20-block export wait in place of depth 3 (22-32 blocks) |
| t_wait | 11-22 blocks [computed] |
| (c) + LP | Recipient paid in the fill's block |

---

## 7. Commands

Once the files exist; everything stays on 127.0.0.1 (`test_runner.py`
expands the wildcard, `test/functional/test_runner.py:510-519`).

```sh
cmake -B build -DB3_BRIDGE_TOOLS=ON && cmake --build build -j"$(sysctl -n hw.ncpu)"
(cd contracts && forge build --offline && forge test --offline)
build/test/functional/test_runner.py build/test/functional/feature_b3_bridge_rt* \
  build/test/functional/feature_b3_h1_producer_rotation.py   # RT9 runs every drill
D="$(mktemp -d)"   # scratch only
python3 contrib/b3bridge/regtest/run_local_bridge.py up --dir "$D" --bin build/bin --fixture I
python3 contrib/b3bridge/regtest/run_local_bridge.py route c --amount 25 --lp
```

Each node gets `-regtest -bind=127.0.0.1 -connect=127.0.0.1:<peer>
-dnsseed=0 -fixedseeds=0 -discover=0 -b3modernregtest -b3flowmeshtest`, the
§3.6 flags, `-b3freshlineageheight=262` and the pin file; fixture B adds
`-b3h1height=220 -b3regtestwithholdcertificates=131-211` and manual
production.

---

## 8. Stage gating, effort and critical path

**Rule [I].** A stage's mainnet gate is reviewed only after its milestone
passes from a clean build of the exact commit; regtest is necessary, never
sufficient.

**Governance [E].** Work conflicting with tracked governing documents waits
for a reviewed reconciliation commit (`CLAUDE.md:30-32`); H1, the fresh
lineage and the bridge V2 consensus regime need the one override of
architecture §15 item 2 (`CLAUDE.md:12-13`, `:87-88`).

| Stage | Regtest | May start | Effort [H] |
|---|---|---|---|
| S0 Rulings, direction commit, the override | None (governance) | Now | Owner |
| S1 H1 | RT1 | After S0 | 2-4 pw |
| S2 Harness | RT0, RT2, RT3 | **Now** (contracts and modes exist) | 3-5 pw |
| S3 Lineage, contracts V2, node core slice | RT4 | After S0, S2 | 12-20 pw |
| S4 Node bridge V2, forwarders | RT5-RT7 | After S3 | 8-14 pw |
| S5 V2 P3 | RT8; multi-host soak (§4.2) before its mainnet gate | V2 program | Not estimated |
| S6 Audit, drills | RT9 | Drills after S4; audit after S5 | 2-4 pw + audit |
| S7 Deploy | RT10 | After S5, S6 | 1-2 pw |
| S8 Rehearsal | Entry: RT10 suite on release candidates | — | Owner-run |
| S9 Opening | None (mainnet notices) | — | — |

**Basis [H]:** one experienced engineer, no measured velocity; split as
architecture §13. S3: lineage tracker, journals, carrier, vote message,
history accumulator, verifier, prover and vault, and the node core slice
(node changes §12.1), tests. S4: Vault_A and its gate, kinds 7, 8 and 10-13,
legacy closure, the rest of RPC and wallet, forwarders, batch and two-chain
relayers, Arbitrum and LayerZero mocks.

**Critical path [H].** S2 (alongside S0) → S3 → S4: 23-39 person-weeks to
RT7, 25-43 to RT9; the cut RT3a → RT4 → RT5 → RT6 covers routes (a), (b),
(d1) and (d3) before Arbitrum-direct RT7.

---

## 9. Owner decisions

| ID | What regtest needs | Default |
|---|---|---|
| B-5 | **a:** a fill into an occupied bucket is invalid (as a gift, a producer ordering a rival fill first would take an honest LP's payment); **b:** exact claimed tuple, or any fill meeting the threshold; **c:** vault ceiling equals B3's, kind 8 minting to the recipient above it | a yes; b exact; c both |
| B-6 to B-8, B-11, B-16, B-18, B-19 | The override and S0 before RT1 and RT4, staging signer regtest-only; B-11, B-16, B-18, B-19 tested in RT4, RT3-RT4, RT7, RT5 | Yes (B-11 needs a ruling) |
| C-1 | Releases continue after a halt, so row 4b's covered leaves pay on both chains | Re-read C-1 with 4b |
| FD-7, FD-8 | Row 1b drills the break at regtest scale, R6 the migration leaf | B-23: extension ≥ 46,080, migration leaf, residual accepted explicitly; FD-8: a break fails closed, disclosed |

Regtest-plan choices, default yes: **RP-1** knobs
from first start, external pins effective above the tip, file hash recorded;
**RP-2** committee tool outside the node; **RP-3** regtest-only lineage, H1
and withholding knobs; **RP-4** mocks from scratch; **RP-5** Arbitrum 31338;
**RP-6** `EXTENDED_SCRIPTS` until measured; **RP-7** Outbox capture (O12);
**RP-8** "no peers" = local nodes linked to each other; **RP-9** bridge bUSD
as regtest rUSD in FlowMesh V2 rehearsals (architecture §15 item 13);
**RP-10** multi-host soak before S5; **RP-11** fixture I first, B before S5.

---

## 10. Open items

- **[H]:** anvil 1.7.1 osaka, EIP-2537, historical proofs; one `b3coind`
  staking four wallets; every runtime, effort and gas figure.
- **Rulings** that block milestones: S0 and the override (RT1, RT4), B-5a-c
  (RT7), B-23's acceptance, RP-1 to RP-11 (§9).
