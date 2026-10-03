# B3 Bridge V2: regtest-first end-to-end plan

> **Status: PROPOSAL, no governing authority.** Below every owner ruling and
> every reviewed, committed governing document; nothing built, run,
> ratified or activated.
>
> Revision 7, 2026-10-04, the **simpler plan**; base `1d1d6da`, branch
> `feat/flowmeshV2-dev-claude`. Revision 6 (two vaults, LP; this file at
> `1d1d6da`) is withdrawn for launch; its Arbitrum-vault and LP milestones
> move to §11. Implements the owner's 2026-10-03 instruction "I need
> everthing to be working atleast on regtest" and the simpler-plan ruling
> (relayed). The [architecture doc](b3-bridge-v2-arbitrum-proposal.md)
> (revision 9) owns numbers, trust, failures, stages, decision IDs and
> governing text (§8, §10, §13-§16). No node or chain RPC was contacted.

**Labels.** [E] established or verified; [I] design choice; [H] estimate or
unverified. Repo facts are `path:line` at `1d1d6da`; uncommitted files carry
no lines. New files, flags and RPCs are [I].

**This document owns** the harness (§3), mocks (§4) and the test content of
RT0-RT10 (§5, §6); architecture §14 owns stages and RT numbering. **RT5 is
retired; RT7 is withdrawn for launch**, kept only for the later
Arbitrum-vault option (§11); neither is reassigned.

---

## 0. Summary

**"Working on regtest" [I].** Routes (a), (b), (d1) and (d3), the deposit and
burn gates, caps, per-epoch committee keys (B-37), the legacy closure and
every drillable failure and recovery row (architecture §13) run as automated
local-only tests: B3 regtest nodes linked only to each other; anvil 31337 as
Ethereum (`src/kernel/chainparams.cpp:1136`), the only chain B3 reads; and
anvil 31338 **only as the USDT0 source chain** standing in for Arbitrum.

**At `1d1d6da`** [E, grep]: unit, Foundry and relayer tests cover
parts of the bridge (`src/test/bridge_state_tests.cpp`,
`contracts/test/B3StakerBridge.t.sol`,
`contrib/b3bridge/test_b3_bridge_relayer.py`); no functional test ran it
end to end or used anvil; four blockers stopped route (a) (§2).

**Plan.** S2 builds the harness on today's V1 contracts (RT0, RT2, RT3), no
consensus change; the rest waits for S0 and the override of architecture §16
item 2 (§8). Until P3 the V1 signer produces digest V2 behind a regtest-only
knob (B-7). RT4 ((a), (d1)) and RT6 ((b), (d3), legacy closure) run first on
fixture I (V1 intact, no H1), so the bridge can go green if H1 slips, then
on fixture B (broken lineage, H1); RT8 re-runs both under P3.

**Largest risk:** stranding of up to **100% of backing** (architecture
§9.1); RT9 drills each cause (§6.2 rows 1b, 3, 4; §6.3 R6).

**Honest limit.** Mocks prove B3's rules, our contracts and tooling, not real
Ethereum, USDT0 or LayerZero behaviour, nor B3 finality liveness (§4.2).

---

## 1. Ground rules

**Local only:** helpers refuse hosts other than `127.0.0.1`/`localhost`;
anvil never forks; the framework disables discovery and seeds
(`test/functional/test_framework/util.py:566-576`) [E]. **"No peers"**
(RP-8): nodes link only to the test's own nodes, as
`test/functional/feature_b3_finality_soak.py:44-140`. **No installs:**
Python stdlib, BLS in C++, `forge --offline` (`contracts/foundry.toml`).
**Never mainnet:** knobs and pin file refused outside regtest; tools are
`B3_BRIDGE_TOOLS` executables outside `b3coind` (`src/CMakeLists.txt:535-555`);
mocks in `contracts/regtest/`, never imported by production code.

---

## 2. Starting point

Exists [E]: a synthetic MANAGED_V1 regtest bridge on chain 31337
(`src/kernel/chainparams.cpp:1130-1177`); a consensus light client
(`src/node/bridge_state.cpp:400-475`); a stdlib relayer
(`contrib/b3bridge/b3_bridge_relayer.py:1119-1177`).

**Blockers [E]:** (1) nothing points regtest at anvil addresses or a
synthetic checkpoint (`src/chainparams.cpp:44-80`); (2) the relayer accepts
only decentralized mode (`contrib/b3bridge/b3_bridge_relayer.py:762-786`);
(3) outbound proof RPCs refuse MANAGED_V1 (`src/rpc/blockchain.cpp:4004-4012`,
`:4963-4970`): the pin file (§3.3) fixes all three; (4) deposits need a
ready verifier (`contracts/src/B3StakerBridge.sol:95-96`): certificates, or
a mock verifier (RT3a).

**Landed after `1d1d6da` [E, commits `8fbf0d6`-`e2a4b6c`, not reviewed
here]:** `-b3bridgepins`; `src/bridge/regtest_beacon.cpp`; helpers
`anvil.py`, `b3bridge.py`; RT0, RT2 and RT3a tests; `contracts/regtest/`
mocks; `contrib/b3bridge/regtest/mock_beacon.py`.

---

## 3. Harness

### 3.1 Topology

```
 anvil "Ethereum" (31337)                    anvil "USDT0 source" (31338)
   MockTetherToken; V1 stack (RT2, RT3)        MockUSDT0, MockOFT,
   Verifier V2, BlsDigestProver, Vault V2      MockLzEndpoint (eid 30110);
   F_d and F_x factories, B3RelayBatch         B3 reads nothing here
   MockOFTAdapter, MockLzEndpoint (eid 30101)
      |      ^------- LayerZero test relayer -------^
 mock beacon + b3-bridge-regtest-beacon
      |
 relayer V2 <---- JSON-RPC to the Ethereum anvil only
      |
 B3 regtest n0-n3 (validators) and n4 (sync check), linked only to each other
```

Processes bind free `127.0.0.1` ports. Mocks use the mainnet endpoint
ids, so F_x's `DST_EID` check runs unchanged [I] (RP-5).

### 3.2 Components [I unless *landed*, §2]

| Component | Path | Role |
|---|---|---|
| anvil helper (*landed*) | `test/functional/test_framework/anvil.py` | Start (`--enable-tx-gas-limit`), mine, warp, snapshot, forge deploy; skip if tools missing |
| Bridge helper (*landed*) | `test/functional/test_framework/b3bridge.py` | keccak/RLP/MPT; pin writer; clock; evidence |
| Certificate tool | `src/bridge/regtest_certify.cpp` | Digest V2 bootstrap attestations, conflicting or forged certificates with per-epoch keys; refuses non-test keys |
| Committee tool; mock beacon (*landed*) | `src/bridge/regtest_beacon.cpp`; `contrib/b3bridge/regtest/mock_beacon.py` | §3.4; finalize, rotate, k-of-512, fork switch |
| Submitter | relayer V2 | Certificates, `reportConflict`, releases, `flush`, `forward` |
| Deploy script; stack driver | `contracts/script/RegtestDeploy.s.sol`; `contrib/b3bridge/regtest/run_local_bridge.py` | Mocks and stacks, manifest; bring-up (§7) |

### 3.3 Startup knobs and the pin file

**Replay rule.** Restart replays every certificate under current parameters
(`src/node/finality_tracker.h:86-90`; `src/node/bridge_state.h:381-386`); a
failure resets the tracker (`src/node/finality_tracker.cpp:250-262`) and
pauses the staker (`src/node/miner.cpp:594-595`) [E]; switching MANAGED_V1 to
decentralized mode changes every withdrawal root
(`src/node/bridge_state.cpp:1162-1174`). So **no parameter may change how a
connected block is judged** [I].

| Kind | Content | When |
|---|---|---|
| Startup knobs | `-b3freshlineageheight=<R_f>` (staging signer, B-7); `-b3freshkeyheight=<R_key>` (B-37: fresh bindings start here and refuse any key ever bound; R_key < R_f − 1; default 241); `-b3freshmaxepochextension=<n>` (fresh lineage only; default B-23's value, 1,280 on profile R and 46,080 on M, asserted so (epoch + n) blocks at 60 s cover the verifier expiry plus three scaled days); `-b3h1height=<R_h>`; `-b3h1escaperounds=<n>`; `-b3regtestwithholdcertificates=<a>-<b>` | From the first start, like the existing `-b3*` knobs (`src/chainparamsbase.cpp:20-22`) |
| Pin file `-b3bridgepins=<path>` | External pins (addresses, code hashes, token, deployment block, caps, forwarder factories, light-client checkpoint, forks) and activation heights (V1 entry, V, re-pins) | Each added or changed pin takes effect above the tip; the node records the file's hash (`-reindex` is no cure) |

Both need `-b3modernregtest -b3flowmeshtest`; without the file MANAGED_V1
defaults stay (`src/test/bridge_params_tests.cpp:497-540`).

**Pin-writer checks [I]**, each also proposed as a node startup assertion:

1. Per-block cap ≥ the vault's `MAX_DEPOSIT_RAW`, or a larger deposit waits
   forever; today's deploy check compares two environment values only
   (`contracts/script/ProductionBridgeConfig.sol:129`).
2. Windows follow B-10. **Profile R** (default) scales every verifier window
   and B3 gate threshold by 1/36, a mainnet day to one 2,400 s regtest epoch
   (24 h → 2,400 s, 72 h → 7,200 s, 6 h → 600 s, 30 days → 72,000 s), and
   sets `max_finality_lag_blocks` to 30 (about three regtest t_fin, §6.4);
   **profile M** keeps mainnet values (drill ES, §6.2).
3. The verifier's `BRIDGE_ACTIVATION_HEIGHT` = V.
4. Deployment ≤ checkpoint execution block ≤ deployment + 20,000
   (`src/node/bridge_state.cpp:170-184`, `:420-426`; `src/bridge/proof.h:43`).

### 3.4 Synthetic Ethereum light client

No shortcut: the real light client and proof checks run on 31337.

- **Keys [I]:** 512 per period p, secret i =
  `FromIKM(sha256("B3-REGTEST-SYNC-COMMITTEE-V1" ‖ p ‖ i))` (`src/crypto/bls.h:101`),
  public.
- **Headers:** anvil's real blocks; B3 uses hash, number, receipts root,
  timestamp and (new, for the burn gate) state root
  (`src/bridge/exec_chain.h:41-99`) [E].
- **Signing:** slot = S0 + N (`src/bridge/eth_light_client.h:187-205`) [E];
  512, 342 or 341 signers; one update per period crossed (`:288-295`).
- **Output:** Beacon-API JSON for `src/bridge/lc_json.h:99-187`, read by the
  unchanged relayer. Tool and node refuse mainnet genesis roots and forks.

### 3.5 Clocks

One monotonic clock drives both anvils and B3 mock time [I]. Before a
handover it jumps at least `MIN_EPOCH_DURATION` plus one second (6 h on
profile M, 600 s on R); the harness relays the handover or a keep-alive
before the next deposit. Jumps stay under the light client's 27.3 h lag
bound. Gates use `now_eff = max(nTime, light-client timestamp)`; RT4 runs B3
behind and ahead of Ethereum.

### 3.6 Fixtures and bring-up

**Flags [I]:** `-b3epochlength=40 -b3checkpointinterval=5 -b3checkpointdepth=3
-b3maxepochextension=40 -b3minfinalityset=4` (`src/chainparams.cpp:70-74`),
shared by the fresh lineage except `-b3maxepochextension`, V1 only (fresh:
`-b3freshmaxepochextension`, §3.3); corridor 130, so M = 131
(`src/consensus/era.h:141-146`) and checkpoints fall at 131 + 5k
(`src/modern/finality_schedule.h:61-63`). R_f = 262 puts R_f − 1 on the grid;
the harness asserts it.

| Fixture | V1 lineage | H1 | Used by |
|---|---|---|---|
| **I** | Intact; V1 decentralized pins from height 0 | No | RT3; RT4 and RT6 first; legacy closure |
| **B** | Broken at 211 = 131 + 40 + 40 by withholding (`src/node/finality_tracker.cpp:133-137`) | `-b3h1height=220` | RT1; RT4 and RT6 repeat before S5 |

On fixture B the staker pauses once V1 breaks (`src/node/miner.cpp:558-560`);
blocks are produced manually until H1 switches
`PlanRegtestFinalityProduction` (`src/node/regtest_finality_policy.h:34-44`)
to the producer schedule [I].

**Bring-up [I]:**

1. Start the Ethereum anvil (from RT6 also 31338); deploy mocks and, for RT2
   and fixture I, the V1 stack. Seed MockOFTAdapter with locked
   MockTetherToken, as the real adapter holds locked USDT.
2. Build the synthetic bootstrap inside check 4's window; write the pin file.
3. Start n0-n3 with every knob and the pin file from height 0; mine the
   corridor; stake and bind.
4. V2 keys (B-37; finality §6.4): between R_key and height 261 (R_f − 1)
   each member binds one fresh genesis key under the new binding kind with
   proof of possession, shared by Set′_0 and Set′_1 (step 3's V1 bindings
   stay, so V1 keeps certifying on fixture I); for each k ≥ 1, between
   Snapshot(start_(k−1) − 1) and Snapshot(start_k − 1), a fresh key for
   epoch k+1. Deploy the V2 contracts against epoch k's set hash (k = 0, or
   k ≥ 2); `initialize` with a 3-of-4 attestation of an epoch-k statement
   plus `leaf0Path`.
5. Add the V2 section with V above the tip; restart; start the mock beacon
   and both relayers.
6. RT4 and RT6 each end with n4 syncing from genesis and n0 run with
   `-reindex`; all agree on `getbridgestatedigest` and `getfinalitystatus`.

### 3.7 Shared byte vectors

The contracts spec defines bytes (AssetId: node changes §3.2). **RT4 entry
criterion [I]:** C++, Foundry and the harness reproduce every
`gen_v2_vectors.sh` entry. Each test writes `bridge_evidence.json` (blocks
per step, gas, jumps, stranded amounts).

---

## 4. Mocks

### 4.1 Mock list

Written from scratch in `contracts/regtest/`, each Foundry-tested [I].

| Mock | Chain | Behaviour |
|---|---|---|
| MockTetherToken | 31337 | Tether's storage slots, pinned by a `vm.load` test (mainnet via O11); no-return transfers; pause; sender or `from` blacklist; from RT4 `destroyBlackFunds`, forwarding after `deprecate`, the fee switch (https://sourcify.dev/server/v2/contract/1/0xdAC17F958D2ee523a2206206994597C13D831ec7?fields=sources) [E]; a test-only partial seizure [I] |
| MockDepositVerifier (RT2, RT3a) | 31337 | The views the vault constructor checks (`contracts/src/B3StakerBridge.sol:75-89`) [E] |
| MockUSDT0 | 31338 | OFT-native token; blocklist (`isBlocked`, `destroyBlockedFunds`) |
| MockOFT; MockOFTAdapter | 31338; 31337 | `send` burns on 31338; the adapter pays the Ethereum `to` by plain transfer; the reverse for (d3); `quoteSend` fee |
| MockLzEndpoint ×2, test relayer | both | Delivery once; retryable failures, pause, delay (for the DVN wait); DVNs not modelled |

### 4.2 What regtest cannot prove

| Gap | Closed by |
|---|---|
| **B3 finality liveness** under real latency and churn: V1 passed a four-node soak (`test/functional/feature_b3_finality_soak.py:5-15`) yet deadlocked twice (`doc/release-notes-v1.1.3.md:58-63`; `doc/finality-quorum-deadlock-814191.md:16-20`) | **Required** owner-run multi-host regtest soak with outages, before the S5 mainnet gate (RP-10) |
| USDT0's OFT, adapter and executor retries; LayerZero fees and DVNs; real gas | Source review; O11; S8 small transfers |

---

## 5. Milestones RT0-RT10

| RT | Stage | Test (`test/functional/feature_b3_…`) | Needs first |
|---|---|---|---|
| RT0 | S2 | `bridge_rt0_toolchain.py` | anvil helper |
| RT1 | S1 | `h1_producer_rotation.py` + C++ | S0; H1 |
| RT2 | S2 | `bridge_rt2_light_client.py` | Pin file; tools |
| RT3 | S2 | `bridge_rt3_v1_route_a.py` (`--deposit-only` = RT3a) | RT2; submitter |
| RT4 | S3 | `bridge_rt4_v2_route_a.py --fixture=I\|B` | S0; vectors; lineage with B-22, B-23, B-37; contracts V2; node core V2 |
| RT5 | — | **Retired** (2026-10-03 ruling); cases moved to RT4, RT6 | — |
| RT6 | S4 | `bridge_rt6_route_b_d3.py`; `bridge_rt6_legacy_closure.py` | RT4; forwarders; OFT mocks |
| RT7 | — | **Withdrawn for launch**; returns only with §11 item 1 | — |
| RT8 | S5 | RT4 and RT6 `--finality=p3` | P3 |
| RT9 | S6 | `bridge_rt9_drills.py --drill=<name>` | RT4, RT6 |
| RT10 | S7 | `bridge_rt10_deploy.py` | V2 deploy scripts |

All go in `EXTENDED_SCRIPTS` (`test/functional/test_runner.py:87`) until
measured.

**RT0 (S2).** Offline forge build and test; anvil `--hardfork osaka
--enable-tx-gas-limit` runs an EIP-2537 vector; headers re-encode to their
hash (`contrib/b3bridge/eth_live_test.py:70-92`); historical `eth_getProof`
verifies; the V1 stack deploys; B3 survives a one-day jump [H].

**RT1 (S1, after S0).** The finality dependency's five RT1 items, with
manual production, plus: burns stay invalid while broken
(`src/node/finality_tracker.cpp:46-63`); the escape uses
`-b3h1escaperounds`; the late upgrader runs with `-b3reorghorizon=40`
(`src/chainparams.cpp:75`) and gets `-b3h1height` only past R_h + 40 (§3.3's
one deliberate knob change).

**RT2 (S2).** Pins from height 0; bootstrap, updates, one rotation.
Rejected: 341 of 512, wrong genesis root, unpinned fork, two-period jump,
conflicting finalized header (`src/node/bridge_state.cpp:461`), no-op update
(`:472`), mint with the head 8,192 slots behind. Checkpoints at deployment
and +20,000 accepted, +20,001 refused. The unchanged relayer drives it
(`localhost` and `127.0.0.1` meet its distinct-origin rule,
`contrib/b3bridge/b3_bridge_relayer.py:93-106`); reindex agrees.

**RT3 (S2, fixture I).** RT3a, first green: mock verifier, deposit, mint,
replay refused. RT3: `signbridgebootstrap` ×3, `initialize`, certificates,
deposit, mint, `bridgewithdraw`, certificate, `release`. Default and wait
relayer variants (architecture §8.1); FlowMesh V1 credit
(`src/consensus/flowmesh_params.h:15`).

**RT4 (S3; fixture I, then B).**
- Entry: vectors green; lineage at R_f = 262 from Snapshot(261), staging
  signer only under the knob; an old-journal orphan lock does not block it.
- **Per-epoch keys (B-37, Codex review).** Each member binds a fresh key
  per epoch under the new kind (epochs 0 and 1 share one); a key ever bound,
  under either kind, is refused; on fixture I V1 keeps certifying; a key is
  erased once its last seated epoch's handover is certified and past the
  reorg limit; a committee below the minimum carries over with its keys.
  **Old-backup race:** an old wallet copy binds first for the next epoch,
  then the honest signer: the seat is void and an alert raised; with no
  honest binding the copy's key is seated (the disclosed limit). A member
  without a fresh binding sits out; certificates continue (a fifth staked
  wallet keeps 4 [I]).
- Bootstrap (B-11): K_OF_N with 3-of-4 regtest keys over an epoch-0
  statement, and k ≥ 2 on a second deployment; wrong set hash, bad
  `leaf0Path`, 2-of-3 and late calls revert; deposits wait for an ordinary
  certificate; watcher `cancel`; a third deployment runs START_CERTIFICATE.
  `getfinalitylineage` matches every pin before any deposit.
- Around V: a pre-V handover (root zero) rotates the verifier; a non-zero
  root below V or a zero root from V reverts; deposits open only after a
  certificate at or above V.
- Route (a) under digest V2 (`approve` + `deposit` from an EOA), then (d1):
  burn, certificate, `release` of a real B3 leaf; FlowMesh V1 credit.
- Deposit gate (B-12): a contract caller, an EIP-7702-coded account and a
  wrapped call revert; Tether paused, deprecated or blacklisting the vault
  refuses deposits.
- Burn gate (B-13): a kind-9 state observation opens it; it closes near
  expiry and reopens after rotation, in both clock orders, and on any status
  but LIVE; burns open at W2 = V. `deprecate` closes burns until a re-pin
  (B-24, R7). Wind-down: a burn plus unreleased leaves must fit the observed
  balance less the margin.
- Caps (B-14): with `MAX_DEPOSIT_RAW` equal to a small block cap, an
  exact-size deposit mints; others wait for later blocks and epochs.
- Refused by the verifier and by B3's carrier rule (as `bad-finality-cert`,
  `src/validation.cpp:3853`): V1 tag, ruleset-1 header, other lineage; by the
  verifier, a history missing its last accepted checkpoint.
- Conflict (row 4): a conflicting certificate landing first makes the
  canonical one revert; `reportConflict` halts the verifier; accepted roots
  still pay (C-1). On a fresh deployment B3's kind 13 closes burns; wallets
  and relayers refuse deposits; the next certificate carries `bridgeFlags`
  bit 0, so the vault refuses a direct deposit while earlier leaves
  release; canonical, wrong-set or rotated-out-set evidence is refused.
- Ancestry (B-9): relaying every third certificate passes; a release against
  an older accepted root succeeds after a newer one; a root below the leaf,
  an unknown root and a second release revert; a release whose transfer
  fails (Tether paused) reverts whole and pays once after unpause.
- Migration leaf (B-23): with a successor v′ (same chain and token) pinned
  at h_m, the first block ≥ h_m with the gate open appends an **ordinary
  leaf V2** paying the reserve to v′, released once, counted by `absorb()`;
  a kind-9 observation of v′ before `absorb()` credits nothing and sets no
  conflict, one after it credits; none while the gate is closed; a later
  deposit into v is swept to v′; the relayer skips v's forwarders; a v′ on
  another chain or token fails startup [I].

**RT6 (S4; fixture I, then B).**
- (b): on 31338 MockOFT `send` to a fresh wallet-derived F_d address (a
  second call yields another) before F_d exists; the adapter pays F_d;
  relayer V2, given p by the wallet, `deploy`s and `flush`es within the fee
  cap, and a second relayer acts from `Published` alone; one `Deposit`, no
  other non-token log (D1); mint. Balances above `MAX_DEPOSIT_RAW` flush in
  parts; lifetime fees stay within `feeCap` (D5).
- Refused or waiting: a contract-called flush (PR-2); a flush while deposits
  are closed; a predicted receipt leaf above 7,168 bytes (PR-3). `refund`
  before custody; `returnToOwner` after `notAfter`; a failed delivery
  retries and credits once.
- (d3): burn naming F_x = `accountOf(p)`; one `B3RelayBatch` call runs
  `release` and `forward` within `feeCap` and `maxBridgeFeeBps`, the fee
  paying the relayer; arrival on 31338. With LayerZero paused or the fee
  short, `forward` reverts and funds wait; `withdrawOnEthereum` works.
- Blacklists (row 8): Tether blacklisting F_d makes flush and refund revert;
  a USDT0-blocked `to` receives frozen tokens; the wallet warns first.
- Through every failure, Vault V2's balance and B3's reserve move only by
  successful flushes and releases.
- Legacy closure (fixture I): V1 still certifies after the fresh bindings;
  a V1 burn valid at V − 1 is invalid at V; V1 updates and mints refused
  from V; label "bUSD (legacy V1)" (B-19).

**RT8 (S5).** RT4 and RT6 unchanged under P3, plus FlowMesh V2 credit once
V2 exists (RP-9).

**RT10 (S7).** Production V2 scripts and pin flow on the Ethereum anvil; two
builds agree on code hashes; a checkpoint within 20,000 blocks of deployment
survives the 7-day review. The suite on release candidates gates S8.

---

## 6. Coverage

### 6.1 Routes

(a) and (d1) RT3 (V1), RT4; (b) and (d3) RT6; (e) Tron guidance only; (c)
and (d2) withdrawn (§11).

### 6.2 Failure drills (RT9; architecture §13 rows)

Times are profile M's, scaled on profile R; block counts are profile R's.

| Row | Drill |
|---|---|
| 1 Finality stalls | Default fresh extension: withhold votes; burns close (C3, lag 30); deposits close within 24 h (72 h with keep-alives but no rotation); mints continue; fixture B producers rotate; no break |
| **1b Stall past the break** | At 1,280 the verifier expires (1,200 blocks after its last rotation) before the B3 break (1,320 after the epoch start); then `-b3freshmaxepochextension=40` breaks the lineage 80 blocks into a stall. Both: no further root; burns refused; stranded amount recorded |
| 2 No relay | Deposits close within 24 h; another relayer resumes, one rotation per 6 h |
| 3 Expiry | 30 days: certified leaves release; burns refused; stranded reserve recorded |
| 4 Conflicting certificates | After RT4's flag and halt: deposits closed, no further root; remainder recorded |
| 5 Tether blacklists the vault or deprecates USDT | Deposits refused; releases revert or follow the forwarded contract; burns wait for a re-pin (R7) |
| 6 Partial seizure | Burns pass up to the wind-down bound and fail one raw unit above; a zero balance refuses all |
| 7 USDT0 or LayerZero fails in (b) or (d3) | Delay, failure, pause, `destroyBlockedFunds`: that transfer waits or is lost; reserve unchanged |
| 8 Recipient or forwarder blacklisted | A blacklisted (d1) recipient gets frozen tokens; forwarder funds stuck; wallet warns |
| 9 Unscheduled Ethereum fork | Updates and mints stop; burns close within 2 days |
| 10 Committee out of bounds | Below the minimum of 4: roots and deposits frozen until a member restakes. Without B-22, 61 extra stakes: roots refused; with B-22 truncated to 64, roots continue |
| **10b Offline Sybil** (architecture §10) | Two keys below the W/128 floor withhold. Without B-22 (n = 6, q_h = 5) the honest four cannot certify and the vault strands at break or expiry, amount recorded; with B-22 the keys are excluded |
| 11, 12 | Contract bug: not drillable; false mint: R4 |
| ES Epoch-start wait | Profile M, epoch 1,440, 60 s blocks: a burn at an epoch's first checkpoint waits only for the rotation certificate |

Rows 10 and 10b select the full-snapshot committee by a regtest-only knob
[I]. **Reorgs:** a reconnected mint mints once; a B3 reorg undoing a chained
update undoes its mint; an anvil deposit reverted before synthetic finality
never mints. Staging journals keep ancestry locks
(`src/node/finality_signature.cpp:607-618`), so reorg drills stay above the
last voted checkpoint or run last on a throwaway fixture.

### 6.3 Recovery drills [I]

Each is a pin change above the tip, or a redeploy.

| # | Recovery | Asserted |
|---|---|---|
| R1 | Withdrawn (Outbox re-pin, §11) | — |
| R2 | New fork version (row 9) | Updates and mints resume |
| R3 | Replacement verifier and vault (rows 3, 4) | Old entry burn-closed; stranded amount reported |
| R4 | False mint: the committee tool signs a header with a fabricated `Deposit` | At most 10,000 per epoch; a monitor flags the missing vault event; burns draw on the real reserve |
| R5 | FD-8 re-bootstrap after 1b | New lineage works; old reserve stays stranded |
| R6 | Migration before a predicted expiry (B-23) | Leaf released once and absorbed; a later deposit into the old vault swept; certified old leaves still pay; impossible after expiry or a halt |
| R7 | USDT `deprecate` re-pin (B-24) | Burns reopen only above the re-pin height |

**Retries:** a killed relayer restarts without duplicates; a reverted
`release`, failed delivery or refused flush succeeds once the cause clears.

### 6.4 Latency structure

Block counts only; minutes are architecture §8's (unmeasured, O8).

| §8 step | Regtest assertion |
|---|---|
| t_chain | Update then mint within 2-4 blocks |
| t_fm | FlowMesh V1 credit 31-32 blocks after the mint |
| t_fin | Burn to certificate in 5-10 blocks: 1 + 0-4 + depth 3 + 1-2 [computed]; mainnet's export depth is 20 (22-32 blocks) |
| t_wait | 11-22 blocks [computed] |
| t_ef, t_lz, t_fl, t_rel | Set by the harness; recorded, never asserted as time |

---

## 7. Commands

Once the files exist; all on 127.0.0.1
(`test/functional/test_runner.py:510-519` expands the wildcard).

```sh
cmake -B build -DB3_BRIDGE_TOOLS=ON && cmake --build build -j"$(sysctl -n hw.ncpu)"
(cd contracts && forge build --offline && forge test --offline)
build/test/functional/test_runner.py build/test/functional/feature_b3_bridge_rt* \
  build/test/functional/feature_b3_h1_producer_rotation.py
D="$(mktemp -d)"   # scratch only
python3 contrib/b3bridge/regtest/run_local_bridge.py up --dir "$D" --bin build/bin --fixture I
python3 contrib/b3bridge/regtest/run_local_bridge.py route b --amount 25
```

Nodes get `-regtest -bind=127.0.0.1 -connect=127.0.0.1:<peer> -dnsseed=0
-fixedseeds=0 -discover=0 -b3modernregtest -b3flowmeshtest`, §3.6's flags,
`-b3freshlineageheight=262 -b3freshkeyheight=241
-b3freshmaxepochextension=1280` and the pin file;
fixture B adds `-b3h1height=220 -b3regtestwithholdcertificates=131-211`.

---

## 8. Stage gating, effort and critical path

**Rule [I].** A mainnet gate is reviewed only after its milestone passes on
a clean build of the exact commit; regtest is necessary, not sufficient.

**Governance [E].** Conflicting work waits for a reviewed reconciliation
commit (`CLAUDE.md:30-32`); H1, the fresh lineage, B-22, B-37 and the bridge
V2 regime need architecture §16 item 2's one override (`CLAUDE.md:12-13`,
`:87-88`); OD-8 is not overridden (item 3).

| Stage (architecture §14) | Regtest | May start | Effort [H] |
|---|---|---|---|
| S0 Now | None | Now | Owner |
| S1 H1 + H2 | RT1 | After S0 | 2-4 pw |
| S2 Harness | RT0, RT2, RT3 | **Now** (in progress) | 3-5 pw |
| S3 Lineage, contracts, node core | RT4 | After S0, S2 | Revision 6: 12-20 pw, before B-37 |
| S4 Forwarders, legacy closure | RT6 | After S3 | Revision 6: 8-14 pw, with Vault_A and the LP |
| S5 V2 P3 | RT8; multi-host soak first | V2 program | Not estimated |
| S6 Audit, drills | RT9 | Drills after S4; audit after S5 | 2-4 pw + audit |
| S7 Deploy | RT10 | After S5, S6 | 1-2 pw |
| S8, S9 | RT10 suite on release candidates | — | Owner-run |

**Critical path [H].** S2 (with S0) → S3 → S4 → S6 drills; **not
re-estimated** (architecture §11).

---

## 9. Owner decisions

IDs are architecture §15's; removed ones are withdrawn by the 2026-10-03
ruling.

| ID | What regtest needs | Default |
|---|---|---|
| B-5, B-18, B-24's USDT0 waiver | Withdrawn; tests move to §11 | — |
| B-6 to B-8, B-11, B-16, B-19 | Override and S0 before RT1, RT4; B-11 RT4, B-16 RT3-RT4, B-19 RT6 | Yes (B-11's alternative needs a ruling) |
| B-9, B-12 to B-14, B-24 | RT4 (B-12 also RT6; B-24 also R7) | Yes |
| B-37 (new) | RT4, RT8 | Yes (Codex review) |
| B-22 | Rows 10 and 10b, with and without | **Needs explicit owner approval** |
| B-23, FD-7, FD-8 | Row 1b, R5, R6 | Extension ≥ 46,080, migration leaf, residual explicitly accepted; a break fails closed |
| C-1 | Releases continue after a halt (row 4); one vault, no double payment | Yes |

Regtest-plan choices, default yes: **RP-1** knobs from first start, pins
above the tip, file hash recorded; **RP-2** committee tool outside the node;
**RP-3** regtest-only lineage, H1 and withholding knobs; **RP-4** mocks from
scratch; **RP-5** 31338 only as the USDT0 source chain, or both endpoints on
31337; **RP-6** `EXTENDED_SCRIPTS`; **RP-7** withdrawn
(§11); **RP-8** "no peers" = local nodes linked to each other; **RP-9**
bridge bUSD as regtest rUSD in FlowMesh V2 rehearsals (architecture §16 item
13); **RP-10** multi-host soak before S5; **RP-11** fixture I first.

---

## 10. Open items

- **[H]:** anvil osaka, EIP-2537 and historical proofs (RT0); one `b3coind`
  staking four wallets; all runtimes, effort and gas.
- **Rulings** blocking milestones: S0 and the override (RT1, RT4); B-11's
  alternative, B-22, B-23's residual; RP-1 to RP-11.

---

## 11. Later options (not at launch)

Each needs its own ruling; revision 6 (this file at `1d1d6da`) has the test
lists.

1. **RT7, Arbitrum vault V2: routes (c) and (d2), optional LP fast-fill**
   (architecture §17 item 1): Vault_A on 31338 with from-scratch Arbitrum
   mocks (nitro-contracts is BUSL-1.1,
   https://github.com/OffchainLabs/nitro-contracts/blob/main/src/bridge/AbsOutbox.sol#L3
   [E]); Outbox-proof mints, heartbeats, LP cases (B-5), skew drills (RP-7).
2. **RT7b, staker-signed fast deposits**
   ([FSD](b3-bridge-v2-staker-signed-deposits.md),
   `feature_b3_bridge_rt7b_fsd.py`, after RT7): needs option 1, penalties,
   P3 and a B-25 ruling.
