# B3 Bridge V2: contracts specification (Ethereum and Arbitrum)

> **Status: PROPOSAL, no governing authority.** It ranks below every owner
> ruling and every reviewed, committed governing document. Nothing here is
> ratified, built, audited, deployed or activated.
>
> - 2026-10-03, revision 8 (cross-document fixes). Base `b4b1de4`, branch
>   `feat/flowmeshV2-dev-claude`.
> - Nothing was compiled, deployed or run; no node or chain RPC was contacted.
>   Hash constants and vectors were computed locally with `cast` (Foundry 1.7.1).

**Labels.** [E] established technique or verified fact; [I] design choice made
here or in a sibling; [H] estimate or unverified. **Default** means a
provisional choice open to owner ruling. Repo facts are `path:line` from the
worktree root; external facts are URLs; `.../study/` means
`/Users/josh/development/ON/B3-FlowMesh-200ms-evidence/arbitrum-bridge-study-20261003/`.

**Ownership.** The architecture document
[b3-bridge-v2-arbitrum-proposal.md](b3-bridge-v2-arbitrum-proposal.md) owns
routes, trust, the single set of latency and cost numbers (its §7), stages
S0-S9, milestones RT0-RT10 and decisions B-1 to B-24. This document owns the
contracts and is **the only normative source for every byte layout a contract
hashes, stores or emits** (§2.1-§2.2, §3.2, §6.3, §6.4, §6.6, §7.2; §2.5
summarizes them and lists the shared vectors). The finality dependency owns
how B3 *produces* those values (R_f, grid, P3); node changes owns the
consensus rules that read them. A sibling text that differs is an error to
delete, not an alternative.

---

## 0. Summary

Six new keyless contracts; those deployed on both chains share code except
constructor immutables [I]:

| # | Contract | Chains | Job |
|---|---|---|---|
| 1 | `BlsDigestProver` | both | V1 BLS and dual-quorum code behind `verifyDigest` (§3.6) |
| 2 | `B3FinalityVerifierV2` | both | Digest-V2 certificates with ancestry; root history; conflict halt; windows (§3) |
| 3 | `B3VaultV2` | both (USDT / USDT0) | Holds the reserve; deposit gate; release against any accepted root, including a migration to a successor; Arbitrum messages (§6) |
| 4 | `B3DepositForwarderFactory` + `B3Forwarder` (F_d) | Ethereum | Per-user CREATE2 deposit address into Vault_E (§7) |
| 5 | `B3OftExitFactory` (F_x) | Ethereum | Vault_E release, then USDT0 OFT to Arbitrum (§8) |
| 6 | `B3RelayBatch` | both | Certificates, releases, forwards and a heartbeat in one transaction (§9) |

No owner, admin, pause, proxy, rescue key or fee switch. The only privileged
act is a one-time cancel by watchers while deposits are still closed (§5).

**Improvements over V1** [I]: digest V2 with lineage id and history
ancestry; accepted-root history; a provable conflict halt; catch-up windows;
a deposit gate on liveness, token status and the bridge activation height V;
bootstrap from an ordinary certificate of a pinned recent set, departing from
the owner-accepted k-of-n default and needing a ruling (§5); a MIGRATE leaf
(B-23, §6.4).

**Numbers.** Architecture §7 owns every latency and cost figure and the
conditions they need; this spec repeats none (gas: §11).

**Order.** Verifier, prover and vault in S3, forwarders in S4, audit S6,
deployment S7. RT4 runs them on anvil with the staging signer and the node
core V2 slice (both S3, architecture §13); RT5-RT7 add the Arbitrum vault and
forwarders; RT8 swaps in V2 P3 with contracts unchanged.

---

## 1. Scope

- **Untouched:** V1 sources in `contracts/src/` and the deployed stack, which
  lapses (B-1). V2 goes in `contracts/src/v2/` (types, prover, verifier,
  vault), `contracts/src/v2/periphery/` (forwarders, batch),
  `contracts/script/DeployV2.s.sol` and `contracts/test/v2/` [I], with the
  compiler settings of `contracts/foundry.toml:1-14` (solc 0.8.35, via-IR,
  `osaka`) and the EIP-170 check of `contracts/script/check-deployable-sizes.sh:9-30`.
- **Reused** [E]: the EIP-2537 path (`contracts/src/BlsCertificateProver.sol:52-121`,
  `:178-330`), the depth-13 member tree (`:145-156`), the 110-byte SetHeader
  (`contracts/src/IB3FinalityProver.sol:29-53`), the balance-delta deposit and
  tolerant token calls (`contracts/src/B3StakerBridge.sol:95-115`, `:176-196`),
  and B3's depth-32 ordered tree (`src/modern/withdrawal_tree.h:33-34`, `:100-131`).

---

## 2. Digest V2

### 2.1 Objects

```solidity
library B3TypesV2 {
    uint16 internal constant RULESET_V2 = 2;
    struct FinalizedBlockV2 {          // 184 bytes packed, big-endian, this field order
        bytes32 lineageId;
        uint64  height;
        bytes32 blockHash;
        bytes32 withdrawalRoot;        // §2.4: zero below BRIDGE_ACTIVATION_HEIGHT (= V), tree root from it
        bytes32 validatorSetHash;      // setHash of the successor SetHeader, as in V1
        uint64  epoch;                 // epoch of the signing set
        uint64  checkpointIndex;       // ≥ 1; this checkpoint's position in the history tree
        bytes32 checkpointHistoryRoot; // depth-32 root over leaves 0 .. checkpointIndex-1
    }
}
```

The finality dependency's §7 sources each field, in this order. SetHeader keeps V1's
110-byte layout with `rulesetVersion = 2`, so every V1 header is rejected [I].

### 2.2 Hashes (normative)

TaggedHash(t, m) = sha256(sha256(t) ‖ sha256(t) ‖ m), as both sides already
use (`contracts/src/BlsCertificateProver.sol:158-176`;
`src/modern/finality_types.h:64-89`). Integers are big-endian.

| Name | Definition |
|---|---|
| `setHash(H)` | keccak256 of the 110-byte SetHeader (V1 meaning) |
| `GENESIS_HEIGHT` | R_f − 1, the grid checkpoint that is history leaf 0 |
| `lineageId` | TaggedHash("B3/FINALITY/LINEAGE/V2", chainDomain ‖ u64 GENESIS_HEIGHT ‖ blockHash(R_f − 1) ‖ setHash(Set′_0)) |
| `digestV2` | TaggedHash("B3/FINALITY/V2", chainDomain ‖ encodePacked(FinalizedBlockV2)) |
| `historyLeaf(h, hash)` | keccak256(u64 h ‖ hash), 40 bytes, keyed by **height** |
| history node | keccak256(left ‖ right), zero-subtree padding, as withdrawals |
| `certId` | keccak256(encodePacked(FinalizedBlockV2)); equal for every aggregation of one statement |

`chainDomain` is the B3 modern domain pinned for V1
(`contracts/deployments/ethereum-mainnet-v1.1.1.json:5`) [E]. A V1 signature
cannot verify under V2 (tag, length and header version differ). The digest
names no destination chain, so one certificate serves both chains, as V1's
already do (`.../study/check.md:65-73`) [E]. There is **no genesis
statement**: bootstrap uses an ordinary certificate (§5).

### 2.3 Ancestry rule [I]

B3 appends one history leaf per grid checkpoint, certified or not; leaf 0 is
block R_f − 1. A certificate at index k ≥ 1 signs the root over leaves
0..k−1; B3 never signs index 0. The verifier stores the last accepted (index,
height, blockHash, historyRoot, certId) and accepts a new certificate only if
`latestIndex < checkpointIndex < 2^32`, `height > latestHeight`, and a
32-sibling path places `historyLeaf(latestHeight, latestBlockHash)` at
`latestIndex` under the new root (index bits order the hashing, as
`contracts/src/B3StakerBridge.sol:142-148`). The leaf binds the height, so no
grid constant is needed. A root, not a previous-certificate pointer, because
certificates are signed at depth 20 (`src/node/finality_signing_policy.h:16`)
over a 10-block grid (`src/kernel/chainparams.cpp:293`): a pointer would force
relaying all ~111 certificates a day.

### 2.4 Withdrawal root and BRIDGE_ACTIVATION_HEIGHT [I]

`BRIDGE_ACTIVATION_HEIGHT` is **exactly V**, node changes'
`activation_height` (first height of V2 records and tree), pinned in both
contracts and mirrored by B3, which refuses to start on a mismatch (node
changes §3.4, assertion 2). B3 signs `withdrawalRoot = 0` below V and the V2
tree root from V, non-zero even when empty
(`src/modern/withdrawal_tree.h:116-131`), as V1 does
(`src/node/bridge_state.cpp:1162-1171`).
Certificates in [R_f, V) rotate both verifiers before V.

Burns are valid from W2 ≥ V (node changes §3.1), which the contracts never
see; the default is **W2 = V** (B-12). Deposits open after a certificate at or
above V (§3.5), so a W2 > V would make bUSD minted in [V, W2) wait for W2:
the disclosed custodial wait of `doc/design/b3-master-handoff.md:115-118`.

Order: choose V, deploy, ship the release, confirm the committee runs it
before V (node §12.1, S7). If V must move later, deploy anew. The old verifier
rejects certificates in [old V, new V) and cannot rotate, but if unexpired
at the new V it would accept the new roots and could open deposits that B3
never mints; so pick a new V reached after its expiry, or `cancel()` it in
its review window (§5).

### 2.5 Summary table and shared vectors

| Item | Normative here |
|---|---|
| Lineage-id height | `u64 (R_f − 1)` |
| History leaf | keccak256(u64 height ‖ blockHash) |
| Root at index k | leaves 0..k−1; index ≥ 1 |
| Bootstrap statement | an ordinary certificate (§5); no genesis statement |
| Withdrawal root | zero below V (= `BRIDGE_ACTIVATION_HEIGHT`), tree root from V |
| Leaf V2 | 180-byte tagged form (§6.4) |
| Migration leaf | an ordinary leaf V2 paying the successor vault (§6.4) |
| Heartbeat | 416 bytes (§6.3) |
| Verifier liveness | `status == 1` |

`contracts/test/vectors/gen_v2_vectors.sh` [I] (installed `cast`) writes one
JSON file read by the Foundry tests and B3 unit tests: lineage id, digest V2,
`certId`, history leaf, root and a leaf-0 path, leaf V2, both payloads and
salts, the F_d and F_x addresses, and an Outbox item and root. RT4 waits until
C++ and Solidity agree on every entry.

---

## 3. Verifier V2

### 3.1 Immutables

| Immutable | Default | Constructor check |
|---|---|---|
| `CHAIN_DOMAIN`, `GENESIS_HEIGHT`, `GENESIS_BLOCK_HASH`, `GENESIS_SET_HASH` | Fresh lineage (§2.2) | non-zero |
| `LINEAGE_ID` | §2.2 | equals the recomputed value |
| `START_EPOCH`, `START_SET_HASH` | Newest epoch at deployment and its set hash (§5) | `START_SET_HASH` non-zero; `START_EPOCH` may be 0, and then `START_SET_HASH` = `GENESIS_SET_HASH` |
| `BOOTSTRAP_MODE`, `BOOTSTRAP_SET_HASH` | START_CERTIFICATE; zero | K_OF_N: 4-member, weight-4, quorum-3 header (`contracts/src/B3FinalityVerifier.sol:117-121`) |
| `PROVER`, `PROVER_CODE_HASH` | `BlsDigestProver` | code hash equal |
| `BRIDGE_ACTIVATION_HEIGHT` | V, node `activation_height` (§2.4) | > `GENESIS_HEIGHT` |
| `MIN_BRIDGE_VALIDATORS`, `MAX_BRIDGE_VALIDATORS`, `MIN_BRIDGE_TOTAL_WEIGHT` | 4, 64, 900 (V1 manifest `:32-34`) | 4 ≤ min ≤ max ≤ 64 (B-22) |
| Windows | §4 | §4 |
| `INIT_DEADLINE`, `REVIEW_WINDOW`, `WATCHERS` (≤ 5) | deploy + 7 days; 7 days; owner-chosen | §5 |

### 3.2 State (normative slots)

Storage keeps set hashes; callers pass the signing header in calldata (V1
stored headers, `contracts/src/B3FinalityVerifier.sol:55-58`) [I]. B3 proves
these slots on Ethereum:

| Slot | Content |
|---|---|
| 0 | `Liveness {uint64 currentEpoch; uint40 lastRotationTime; uint40 lastCertificateTime; uint64 latestBridgeHeight; uint8 status}` |
| 1, 2 | `currentSetHash`, `nextSetHash` (zero until known) |
| 3 | `SetSummary {uint32 currentCount; uint64 currentWeight; uint32 nextCount; uint64 nextWeight}` |
| 4 | `Latest {uint64 height; uint64 index; uint40 initTime}` |
| 5, 6, 7 | `latestBlockHash`, `latestCertId`, `latestHistoryRoot` |
| 8 | `latestBridgeRoot`, written with `latestBridgeHeight` (as V1 `:69`, `:256`) |
| 9 | `mapping(bytes32 root => uint64 height) acceptedRootHeight` |
| 10 | `mapping(uint64 epoch => bytes32 setHash) setHashByEpoch` |

`status`: 0 UNINITIALIZED, 1 LIVE, 2 HALTED_CONFLICT, 3 CANCELLED. B3 must test
`status == 1`; "initialized and not halted" would read CANCELLED as live.

### 3.3 `submitCertificate(fb, signingSet, successor, ancestryPath, proof)`

Anyone may call (pattern of V1 `:192-288`):

1. `status == LIVE`.
2. `transition = (fb.epoch == currentEpoch + 1 && nextSetHash != 0)`;
   otherwise `fb.epoch == currentEpoch`.
3. `now − lastRotationTime ≤ MAX_EPOCH_LAG` (`SetExpired`); on a transition
   also `≥ MIN_EPOCH_DURATION`.
4. `fb.lineageId == LINEAGE_ID`; ancestry (§2.3).
5. `blockHash`, `validatorSetHash` non-zero; `withdrawalRoot` zero if
   `fb.height < BRIDGE_ACTIVATION_HEIGHT`, non-zero otherwise.
6. `successor` has epoch `fb.epoch + 1`, ruleset 2 and hash `fb.validatorSetHash`.
7. `signingSet` hashes to `nextSetHash` on a transition, else `currentSetHash`.
8. `PROVER.verifyDigest(digestV2(fb), signingSetHash, signingSet, proof)`.
9. If `signingSet` is bridge-qualified and `fb.height ≥ BRIDGE_ACTIVATION_HEIGHT`:
   set `latestBridgeHeight`, `latestBridgeRoot`, and
   `acceptedRootHeight[root] = fb.height` if still zero.
10. On a transition rotate (current ← next, `currentEpoch += 1`,
    `setHashByEpoch`, summary, `lastRotationTime = now`), then pin `successor`
    as next or revert `SuccessorMismatch` (V1 `:259-274`).
11. Store latest (height, index, blockHash, historyRoot, certId) and
    `lastCertificateTime`; emit `Finalized(certId, epoch, height, withdrawalRoot, bitmap)`.

Same-epoch certificates never renew `lastRotationTime` (V1 `:204-213`).

### 3.4 `reportConflict(fb, signingSet, leafHeight, leafHash, path, proof)` [I]

Anyone may submit a certificate with index ≥ 1 that passes the lineage check
and step 8 under the **current or next** set (rotated-out sets cannot halt
anything) and contradicts the stored history:

- same index as latest, different `certId`; or
- higher index, and `path` places `L = historyLeaf(leafHeight, leafHash)` ≠
  `historyLeaf(latestHeight, latestBlockHash)` at `latestIndex` under
  `fb.checkpointHistoryRoot`; or
- lower index j, and `path` places L ≠ `historyLeaf(fb.height, fb.blockHash)`
  at j under `latestHistoryRoot`.

Each proves the quorum signed two incompatible histories. Because one
certificate serves both chains, a branch relayed to one chain can be taken to
the other, and the third case halts a verifier that got ahead on one branch.

Effect: `status = HALTED_CONFLICT`, permanently; certificates and deposits
stop. **Default (C-1): releases against accepted roots continue**, since an
attacker can release in the transaction that installs a false root. A halt
freezes the vault's remaining reserve (§10).

### 3.5 Views and deposit gate

```
depositsOpen() = status == LIVE && now >= initTime + REVIEW_WINDOW
              && now - lastRotationTime    <= DEPOSIT_CLOSE_AFTER
              && now - lastCertificateTime <= MAX_CERTIFICATE_AGE
              && currentQualified && nextSetHash != 0 && nextQualified
              && latestBridgeHeight >= BRIDGE_ACTIVATION_HEIGHT
setExpiresAt()    = lastRotationTime + MAX_EPOCH_LAG
depositsCloseAt() = min(lastRotationTime + DEPOSIT_CLOSE_AFTER,
                        lastCertificateTime + MAX_CERTIFICATE_AGE)
```

Plus getters for every slot. Qualified means 4-64 members and weight ≥ 900
(V1 `:375-378`). The gate cannot see epoch lag; §4 keeps lag short.

### 3.6 Prover V2 entry

`BlsDigestProver.verifyDigest(bytes32 digest, bytes32 signingSetHash,
SetHeader signingSet, bytes proof)` is the body of V1 `verify`
(`contracts/src/BlsCertificateProver.sol:59-121`) with the digest as an
argument (not `_finalityDigest`, `:118`) and the shape rule on `RULESET_V2`.
Witness ABI, POP DST (`:42`) and dual quorum, ⌊2n/3⌋+1 and ⌊2W/3⌋+1
(`:82-107`), are unchanged [E].

---

## 4. Time windows and Arbitrum skew

| Immutable | V1 | V2 default | Constructor rule (mainnet ids) |
|---|---|---|---|
| `MIN_EPOCH_DURATION` (between rotations) | 86,400 s | 21,600 s (6 h) | ≥ 3,600 s |
| `MAX_EPOCH_LAG` (expiry after the last rotation) | 2,592,000 s | 2,592,000 s (30 days) | ≥ 2,073,600 s (24 days) |
| `DEPOSIT_CLOSE_AFTER` (rotation age) | 23 days | 259,200 s (72 h) | ≤ `MAX_EPOCH_LAG` − 7 days |
| `MAX_CERTIFICATE_AGE` | 86,400 s | 86,400 s | < `DEPOSIT_CLOSE_AFTER` |
| Absolute epoch window | yes (`contracts/src/B3FinalityVerifier.sol:380-389`) | **dropped** | — |

V1 values: `contracts/deployments/ethereum-mainnet-v1.1.1.json:29-36`; V1's
one-day floor is a library constant (`contracts/src/IB3FinalityProver.sol:15`)
that V2 does not reuse [E]. Reasons [I]:

- **Catch-up needs a short minimum.** Only epoch e or e+1 is accepted, so
  lag closes at 1/MIN − 1/E_real. B3 epochs (1,440 blocks,
  `src/kernel/chainparams.cpp:292`) last at least 24 h at the 60 s floor
  (`src/modern/pos_v1.h:74-79`), ~31 h at 78 s: V1's 24 h minimum gains
  ~0.23 epochs a day at 78 s and nothing at 60 s; 6 h gains ~3. A burn on a
  chain k epochs behind waits about (k−1) × 6 h after relays resume. Speed
  is not a safety parameter: each rotation needs a dual quorum of the pinned
  next set.
- **24-day floor, one formula** shared with node changes assertion 5: the
  Arbitrum gate must open for a heartbeat seen as late as the rollup allows,
  so `MAX_EPOCH_LAG − 7 d − 1 d − 87,168 s > max_rollup_delay` (14.7 d =
  1,270,080 s on mainnet, i.e. `MAX_EPOCH_LAG` > 2,048,448 s; the test
  rollup's pinned delay under C-12).
- **No economic anchor.** B3 stake has no unbonding or slashing
  (`src/modern/stake.h:30-40`), so expiry only bounds how long rotated-out
  keys stay dangerous (IBC ties trusting periods to unbonding,
  https://raw.githubusercontent.com/cosmos/ibc/main/spec/client/ics-007-tendermint-client/README.md);
  prompt rotation relays are the real control (§10).
- **72 h deposit close.** About two missed rotations at 78 s; above ~180 s
  spacing deposits would close between rotations (owner read O8). A daily
  keep-alive certificate satisfies `MAX_CERTIFICATE_AGE`.
- **Regtest relaxation (C-12).** On `block.chainid` 31337 or 31338 the
  constructor accepts any non-zero windows that keep the orderings, so drills
  need minutes of warp, not days; `DeployV2.s.sol` refuses relaxed values on
  any other id. B3 regtest mirrors them (regtest plan §3.3).

**Arbitrum skew** [E facts, I consequences]. Live SequencerInbox bounds let
`block.timestamp` lag by up to 86,400 s and lead by up to 768 s
(`.../study/check.md:31`; docs still say one hour,
https://docs.arbitrum.io/build-decentralized-apps/arbitrum-vs-ethereum/block-numbers-and-time);
force inclusion takes up to 24 h (`check.md:32`); `block.number` is an L1
number (`check.md:48`). Elapsed time on Arbitrum can be off by ~24 h 13 min,
so nothing depends on rotation spacing there; contracts read only
`block.timestamp` (`check.md:74`), and the 27-day margin between deposit
close and expiry covers force inclusion, skew and a relay margin.

**`MAX_CERTIFICATE_AGE` = 86,400 s on both chains (C-14, B-10)** [I].
Timestamps never decrease (same docs) [E], so skew moves the gate two ways. A
jump from 24 h behind to current closes it, but only until the next
certificate; the owner relayer relays one whenever Verifier_A shows
`depositsOpen()` false or `depositsCloseAt()` near, at about $0.11
(architecture §10). A drift from current to 24 h behind keeps it open up to
~48 h of real time. **Alternative:** ~48 h on Arbitrum only, giving a daily
relayer time to react; it stretches the drift case to ~72 h and splits B-10
per chain (a value just above 87,168 s, node changes A2, only avoids the
instant close).

---

## 5. Bootstrap

**Default START_CERTIFICATE (B-11 as revised here)** [I]:

1. The deployer picks a start epoch k (default: the newest) and pins
   `START_EPOCH = k`, `START_SET_HASH` (epoch k's committee set hash), and
   `GENESIS_*` and `LINEAGE_ID` (block R_f − 1, its hash, setHash(Set′_0)),
   all read from `getfinalitylineage` (finality §6.5, §11) and cross-checked
   with `getblockhash`. `getfinalityset` serves only V1 sets
   (`src/rpc/blockchain.cpp:3707-3716`); its hash would make `initialize()`
   fail.
2. Prover, verifier and vault are deployed on each chain.
3. Anyone calls `initialize(fb, startSet, successor, leaf0Path, proof)` before
   `INIT_DEADLINE` with **any ordinary certificate of epoch k**: `startSet`
   hashes to `START_SET_HASH`; lineage, index ≥ 1 and steps 5, 6, 8 of §3.3
   hold; `leaf0Path` places `historyLeaf(GENESIS_HEIGHT, GENESIS_BLOCK_HASH)`
   at index 0 under `fb.checkpointHistoryRoot`. Result: LIVE at epoch k; next
   set `fb.validatorSetHash`; latest = fb, including `latestCertId`; bridge
   fields per step 9; `lastRotationTime = initTime = now`.
4. Review window: certificates accepted, deposits closed. A watcher may
   `cancel()` before it ends (CANCELLED for good); a cancel strands nothing.
5. A B3 release pins the vault entries after reviewers check every immutable
   against B3; two independent builds agree on code hashes (S7).

**Why, and what changes.** The direction the owner accepted on 2026-10-03
(in conversation, not yet a committed ruling) named a time-bounded k-of-n attestation as default unless research found a
more trust-minimized option; this default departs from it and **needs an
owner ruling**. A start certificate removes the k-of-n attesters, needs no new B3
message, and starts at any epoch, so verifiers deployed at S7 need not walk
every epoch since R_f (S5). It does not prove the pinned set is B3's real
committee (a deployer pinning a fake set can sign with it), so the trust root
stays the deployer pins plus public review against B3 (finality §11).

**Fallback K_OF_N (the owner-accepted default):** V1's pattern
(`contracts/src/B3FinalityVerifier.sol:93-190`): an immutable 4-key header
signs `digestV2(fb)` 3-of-4 in place of the start set's quorum, then has no
callable path. The owner chooses (C-2). Rejected: inheriting the old verifier
(it lapses) and a ZK proof of B3 history (out of scope).

---

## 6. Vault V2

### 6.1 Immutables

| Immutable | Ethereum | Arbitrum One |
|---|---|---|
| `ORIGIN_TOKEN` | USDT `0xdAC17F958D2ee523a2206206994597C13D831ec7` | USDT0 `0xFd086bC7CD5C481DCC9C85ebE478A1C0b69FCbb9` |
| Token status checks | `paused()`, `deprecated()`, `isBlackListed(vault)` | `isBlocked(vault)` |
| `L2_MESSENGER` | zero | ArbSys `0x…64` |
| `L1_SINK` | — | `address(uint160(uint256(keccak256("B3/BRIDGE/ARB/SINK/V2"))))` |
| `FORWARDER_FACTORY` | F_d factory (§7.3) | zero |
| `VERIFIER`, `VERIFIER_CODE_HASH`, `B3_CHAIN_DOMAIN` | that chain's verifier | same |
| `B3_ASSET_ID` | unified bUSD id (node changes §3.2) | same |
| `MAX_DEPOSIT_RAW` | 10,000,000,000 (10,000 units, B-14, C-9), equal to node §3.3's block and epoch caps, so one maximum deposit fills an epoch | same |
| `MAX_LP_FEE_PPM` | 0 | 10,000, equal to B3's `max_fee_ppm_ceiling` (C-5) |
| `EXPECTED_TOKEN_CODE_HASH` | constructor check (§6.5) | same |

Regtest uses anvil 31337 / 31338 with MockTetherToken, MockUSDT0 and
MockArbSys at `0x…64` [I]. The token functions exist as named
(https://sourcify.dev/server/v2/contract/1/0xdAC17F958D2ee523a2206206994597C13D831ec7?fields=sources;
https://sourcify.dev/server/v2/contract/42161/0x3263cd783823d04a6b9819517e0e6840d37ca3f4?fields=sources)
[E]. `FORWARDER_FACTORY` is the predicted address; the factory constructor
requires `VAULT.FORWARDER_FACTORY() == address(this)`.

### 6.2 `deposit(uint256 amount, bytes32 b3Recipient, uint32 maxLpFeePpm)`

1. `VERIFIER.depositsOpen()`, and every token check returns exactly 32 bytes
   meaning false; otherwise `DepositClosed`.
2. **Ethereum caller rule (C-13):** `(msg.sender == tx.origin &&
   msg.sender.code.length == 0) || FORWARDER_FACTORY.isAccount(msg.sender)`.
   B3's V2 store accepts a terminal receipt node of up to 8,192 bytes
   (node changes §4; today 1,024, `src/bridge/proof.h:45`), but a Safe,
   ERC-4337 or EIP-7702 bundle adds logs to the same receipt without bound,
   and with no refund the funds would be lost; such wallets pay their F_d by
   plain transfer (§7). Not applied on Arbitrum, where B3 proves the message.
3. `amount > 0`; `b3Recipient` is RECIPIENT_V1
   (`doc/design/b3-bridge-recipient-encoding.md:13`; V1 `:101-104`).
4. `maxLpFeePpm ≤ MAX_LP_FEE_PPM`. B3 must treat a proven Vault_A deposit as
   **always mintable**: a fee above its ceiling means "no fills" and mints to
   the recipient, and its ceiling may never drop below `MAX_LP_FEE_PPM`.
5. Pull by balance delta; `0 < received ≤ MAX_DEPOSIT_RAW`; `locked += received`.
6. Emit **V1's `Deposit(uint64 indexed depositId, address indexed token,
   uint256 amount, bytes32 b3Recipient)` unchanged**, so B3's parser
   (`src/bridge/deposit.h:130-149`) needs only the new vault pin.
7. On Arbitrum, one `sendTxToL1(L1_SINK, depositMessage)` with value 0.

`absorb()` (anyone) sets `locked = max(locked, tokenBalance)`. It is how a
successor counts a migration it receives (§6.4); elsewhere it only counts
stray transfers, which creates no claim, since B3 leaves alone authorize
payment. A transient reentrancy lock guards every entry point.

### 6.3 Arbitrum messages (normative)

ArbSys `sendTxToL1` records sendHash = keccak(caller ‖ destination ‖ l2Block ‖
l1Block ‖ timestamp ‖ value ‖ data); keccak(sendHash) is the Outbox leaf
(https://github.com/OffchainLabs/nitro/blob/master/precompiles/ArbSys.go#L113-L173)
[E]. B3 pins caller = Vault_A and destination = `L1_SINK`, which has no code,
so the message can never execute on L1
(https://github.com/OffchainLabs/nitro-contracts/blob/main/src/bridge/AbsBridge.sol#L195-L212)
[E].

| Message | `data = abi.encode(...)`, one word per field |
|---|---|
| Deposit, 192 bytes, once per deposit | `DEPOSIT_TAG`, uint64 depositId, address token, uint256 amount, bytes32 b3Recipient, uint32 maxLpFeePpm |
| Heartbeat, 416 bytes, every `release` and every `heartbeat()` | `HEARTBEAT_TAG`, uint64 seq, uint8 verifierStatus, uint64 currentEpoch, bytes32 currentSetHash, bytes32 nextSetHash, uint64 lastRotationTime, uint64 setExpiresAt, uint64 latestBridgeHeight, bytes32 latestBridgeRoot, bool vaultBlocked, uint256 tokenBalance, uint256 locked |

`DEPOSIT_TAG = keccak256("B3/BRIDGE/ARB/DEPOSIT/V2") =
0xa3aaee59ff13350b00c4dbf4adfdc28af6efbb084704d321f9b3f14501588c81`;
`HEARTBEAT_TAG = keccak256("B3/BRIDGE/ARB/HEARTBEAT/V2") =
0x12170327ed9ae7f4b022c19f61cf35dbd0a182b819452b4f5362e4aae15b58ac`
[computed]. Verifier fields are read at send time; `vaultBlocked` is
`isBlocked(vault)`, and a failed read reports true. `seq` rises by one. The
heartbeat carries everything B3's Arbitrum gate reads (node changes A1-A5).
It has no `nextDepositId`: B3 cuts invariant R (A4) and the wind-down bound
at the heartbeat's own Outbox `leaf_index`, since each `sendTxToL1` takes the
next position and every deposit counted in the heartbeat's `locked` sent its
message earlier (node changes §7.2, §8.3-§8.4).

**Who sends (C-3).** The verifier sends nothing and the vault cannot notice a
rotation, so `B3RelayBatch` calls `heartbeat()` after every Arbitrum
transition (§9), plus at least daily (architecture §4, §10).

### 6.4 `release`, leaf V2 and the migration leaf (normative)

180-byte preimage, fixed-width big-endian, in this order:

| Field | Bytes |
|---|---|
| `LEAF_TAG_V2 = keccak256("B3/BRIDGE/WITHDRAWAL/V2")` = `0xc250a4de311e4369a5d75a6fdd6351f1cc8ec35929170d1374a5367a5edd10f5` | 32 |
| `B3_ASSET_ID` (unified bUSD; binds the chain domain) | 32 |
| `withdrawalId` | 8 |
| destination chain id (`block.chainid`: 1 or 42161) | 8 |
| vault (`address(this)`) | 20 |
| token (`ORIGIN_TOKEN`) | 20 |
| recipient | 20 |
| amount (u256) | 32 |
| `b3Height` | 8 |

```solidity
struct WithdrawalV2 { uint64 withdrawalId; address recipient; uint256 amount; uint64 b3Height; }
leafV2 = keccak256(abi.encodePacked(LEAF_TAG_V2, B3_ASSET_ID, w.withdrawalId,
             uint64(block.chainid), address(this), ORIGIN_TOKEN, w.recipient, w.amount, w.b3Height));
```

**Vector** [computed]: asset id 32 bytes of `0x11` (stand-in), id 7, chain 1,
vault `0x2222…2222`, token USDT, recipient `0x3333…3333`, amount 1,000,000,
height 900,000 → `0x1cb905899ca0c488e158585c0208497d5415d1645035fbca3ca98ee2efb5ad64`.
Node changes §7.3 has this layout; both must reproduce the vector.

`release(WithdrawalV2 w, bytes32[32] path)`: `withdrawalId < 2^32`;
`b3Height ≥ BRIDGE_ACTIVATION_HEIGHT`;
recipient, amount non-zero; not `released`; root from `path` (V1 `:142-148`);
`h = VERIFIER.acceptedRootHeight(root)`, `h != 0`, `h ≥ b3Height`;
`amount ≤ locked`; mark, decrement, transfer, emit `Withdrawn`; on Arbitrum
send a heartbeat. Release ignores verifier status and token checks: a frozen
root keeps paying (V1 principle, `contracts/src/B3StakerBridge.sol:123-127`).
Ids are global; each vault keeps its own `released`, and the other vault
computes a different leaf, so it cannot pay
(`contracts/test/B3StakerBridgeMultiVault.t.sol:89-159` pattern).

**MIGRATE leaf (B-23)** [I], as node changes §7.6: an ordinary leaf V2
naming the old vault v, with the successor v′ as `recipient` and `amount =
reserve_v`, paid once by `release` under the next global id; no new
function, tag or vector. Only a coordinated B3 hard fork (node changes
§12.3) creates it, pinning v′ on v's chain and token with the same
`B3_ASSET_ID`; B3 moves the reserve to v′ and retires v. B3 appends the leaf
only while v's burn gate is READY (verifier LIVE, at least 8 days before
expiry, node changes §8.4 E2, A2), and its root must then be accepted, which
I-4 forbids after expiry, a halt or a cancel. A plain transfer does not raise
v′'s `locked`, so the relayer calls `v′.absorb()` right after the release
(anyone may); v′ then pays its own leaves, emits no `Deposit`, and B3 mints
nothing. A tagged MIGRATE leaf with on-contract successor checks was
rejected: it adds a function and a vector for checks B3's pin already makes.

### 6.5 Token behaviour

Low-level calls accept an empty or `true` return (V1 `:182-196`). USDT's
optional fee: deposits record the received amount; a release lowers `locked`
by the gross amount [E]. **Neither token checks the recipient**: USDT checks
only `msg.sender` (`transfer`) or `_from` (`transferFrom`); USDT0 checks only
`from`, and its owner can `destroyBlockedFunds` (sources above) [E]. A release
to a blacklisted or blocked recipient succeeds and the funds arrive frozen, so
wallets check `isBlackListed(recipient)` on Ethereum and `isBlocked(recipient)`
on Arbitrum before a burn. A blocked **vault** makes `release` revert until
reversed; a USDT0 burn of the vault's balance shows in the next heartbeat.

**Upgrades are invisible.** USDT0 on Arbitrum is an EIP-1967 proxy
(`.../study/check.md:99`) whose implementation no contract can read.
Committed text admits upgradeable or blacklistable tokens only through
per-token adapters (`doc/design/b3-bridge-threat-model.md:365-369`;
`doc/design/b3-master-handoff.md:121-122`); this spec has none, so the
default (C-10, architecture B-24) watches USDT's `deprecated` flag and code
hash by kind 9 (node changes §8.2) and records an owner waiver for USDT0.

### 6.6 Storage layout (normative for B3 storage proofs)

Vault: slot 0 `{uint64 nextDepositId; uint64 heartbeatSeq}`, slot 1 `locked`,
slot 2 `mapping(uint64 => bool) released`. Verifier: §3.2. B3 proves Ethereum
slots by EIP-1186 (https://eips.ethereum.org/EIPS/eip-1186); Arbitrum state
arrives by heartbeat. Packing follows declaration order, first member in the
low-order bytes
(https://docs.soliditylang.org/en/latest/internals/layout_in_storage.html).

---

## 7. Deposit forwarder F_d

### 7.1 Receipt-leaf budget

Modelled leaf sizes [H]: a direct deposit or a flush 629 bytes; 786 with
USDT's fee on; a relayer-fee transfer in the same transaction 1,100 and two
deposits 1,133, all within the V2 store's 8,192-byte terminal receipt node
and 16 KiB proof (node changes §4). Rules [I]: **PR-1** a flush
makes exactly one `vault.deposit` and emits nothing else; **PR-2** `flush`
runs only as the top-level call of a codeless EOA (`msg.sender == tx.origin
&& tx.origin.code.length == 0`; EIP-7702 accounts carry code,
https://eips.ethereum.org/EIPS/eip-7702), so nobody can wrap it in junk logs;
**PR-3** relayers skip a flush whose predicted receipt leaf exceeds 7,168
bytes, 8,192 less 1,024 for error in the [H] size model (a skipped flush
stays refundable, §7.3); **PR-4** direct deposits obey the vault caller rule (§6.2).

### 7.2 Forwarder and address derivation (normative)

`B3Forwarder` has only `exec(target, value, data)`, callable by its factory,
no constructor arguments, no `receive()` [I]. Its address is CREATE2
(https://eips.ethereum.org/EIPS/eip-1014) from the factory, `salt` and the
manifest's `FORWARDER_INIT_CODE_HASH` (metadata is embedded,
`contracts/foundry.toml:13-14`), so it can be paid before deployment.
`salt = keccak256(abi.encode(TAG, fields…))`:

| Factory | `TAG` | Fields, in order |
|---|---|---|
| `B3DepositForwarderFactory` | `keccak256("B3/FWD/DEPOSIT/V2")` | address vault, bytes32 b3Recipient, address refundEOA, uint96 feeCap, uint64 notAfter |
| `B3OftExitFactory` | `keccak256("B3/FWD/EXIT/OFT/V2")` | uint32 dstEid, bytes32 to, uint16 maxBridgeFeeBps, uint96 feeCap, address refundEOA |

`feeCap` is a **lifetime** cap per account. Wallet RPCs derive exactly this.

### 7.3 Factory

Immutables: `VAULT` (Vault_E, code hash checked), `TOKEN`, `MAX_DEPOSIT_RAW`,
`MIN_FLUSH_RAW` (1,000,000 raw [I]), `FORWARDER_INIT_CODE_HASH`. Storage:
`isAccount`, `reserved` (accrued fees), `feesCharged` (lifetime) and
`owed[account][payee]`. `free = balance − reserved`.

- `deploy(p)` (anyone): CREATE2, `isAccount = true`, `approve(VAULT, max)`.
- `flush(p, fee)` (anyone, PR-2): deployed; `now ≤ p.notAfter`;
  `feesCharged + fee ≤ p.feeCap`; `amount = min(free − fee, MAX_DEPOSIT_RAW)
  ≥ MIN_FLUSH_RAW`; accrue `fee`; `account.exec(VAULT, 0, deposit(amount,
  p.b3Recipient, 0))`. Any revert undoes the accrual. **The caller picks only
  the fee, never the amount**: each flush takes the whole free balance up to
  the vault maximum, so a balance cannot be split into many fee-paying
  flushes, and all fees ever taken stay within `feeCap`.
- `collectFee(account, payee)`: pays accrued fees separately.
- `refund(p, token, to, amount)`: only `p.refundEOA`, any time; at most `free`
  of `TOKEN`; `to` is not the vault, the account or the factory.
- `returnToOwner(p, fee)` (anyone, after `notAfter`): pays `free − fee` to
  `p.refundEOA`, fee inside the lifetime cap, so a user with no Ethereum ETH
  is not stuck. An OFT return to Arbitrum is an open option.

`refundEOA` must be an Ethereum EOA the user controls, proven to the wallet by
an EIP-191 signature; Safes and ERC-4337 accounts are refused.

### 7.4 Route (b) and flush policy

The user calls the Arbitrum OFT `0x14E4A1B13bf7F943c8ff7C51fb60FA964A298D92`:
`send(SendParam{dstEid: 30101, to: F_d, amountLD, minAmountLD: amountLD,
extraOptions, composeMsg: "", oftCmd: ""}, fee, refundAddress)`
(https://github.com/LayerZero-Labs/devtools/blob/main/packages/oft-evm/contracts/interfaces/IOFT.sol)
[E]. The Ethereum adapter credits F_d by plain transfer; no compose. A
reverted delivery stays retryable [E for EndpointV2; H for USDT0's executor].
Before a flush the relayer checks the registry entry, light-client freshness
and Vault_E cap headroom counting unminted deposits (node changes). Wallets
size `feeCap` for the credits they expect. Default `notAfter`: creation +
30 days, capped at the light-client horizon minus 24 h [I].

**Invariants.** D1 a flush yields one `Deposit` and no other non-token log;
D2 tokens leave F_d only to the vault, a payee (its accrued fees) or as
`refundEOA` directs; D3 `balance ≥ reserved`; D4 a flush and a refund never
spend the same unit; D5 `feesCharged ≤ feeCap` for ever; D6 a flush deposits
`min(free − fee, MAX_DEPOSIT_RAW)`.

---

## 8. Exit forwarder F_x

Route (d3): the wallet burns bUSD naming Vault_E with recipient
`F_x = accountOf(p)`. After `release` pays it, anyone calls `forward(p, fee,
feeRecipient, nativeRefundTo)` with `msg.value ≥ quoteSend(...).nativeFee`:
deploy if needed; `fee ≤ p.feeCap`; `amount = balance − fee > 0`;
`SendParam(30110, p.to, amount, amount − amount·maxBridgeFeeBps/10⁴,
EXTRA_OPTIONS, "", "")`; pay `fee`; approve the adapter through zero (USDT's
rule); `send`; reset the allowance. Immutables: `USDT`, adapter
`0x6C96dE32CEa08842dcc4058c14d3aaAD7Fa41dee` (checks `token() == USDT`),
`DST_EID = 30110`, `EXTRA_OPTIONS` [H, O11]. `isValid(p)`: `dstEid == 30110`;
`to` a non-zero address word; `maxBridgeFeeBps ≤ 50`; a valid `refundEOA`.

`withdrawOnEthereum(p, token, to, amount)`, only by `refundEOA`, is the other
way out. The leaf stores only the address, but the burn's exit block carries
p, checked against it and returned by `getbridgewithdrawals` (node changes
§7.2 item 5); `publish(p)`, emitting p on Ethereum, is an optional
convenience [I]. A LayerZero or USDT0 pause or a short native fee makes
`send` revert and funds wait. **A delivery to a USDT0-blocked `to`
succeeds and arrives frozen** (§6.5), so wallets check `isBlocked(p.to)`
first. Wallets prefer (d2) whenever `reserve_A` covers the amount.

---

## 9. Batch relayer

`B3RelayBatch.relay(CertInput[] certs, ReleaseItem[] releases, ExitForward[]
forwards, address heartbeatVault, address nativeRefundTo)` holds no tokens,
ETH or state [I]: `try` each certificate in order; skip released ids and `try`
each release; `try` each forward within `msg.value`; call `heartbeat()` on
`heartbeatVault` if set, and always after an Arbitrum rotation; return unspent
ETH. Root history means a release no longer needs its certificate in the same
transaction (V1 did, `contracts/src/B3StakerBridge.sol:149-151`). Relayers
split batches above 15,000,000 gas on Ethereum (cap 16,777,216,
https://eips.ethereum.org/EIPS/eip-7825); Arbitrum allows 32,000,000
(`.../study/check.md:59`).

---

## 10. Invariants, exactly-once and escapes

**Verifier.** I-1 every accepted certificate carries a dual quorum of the
lineage's set for its epoch; I-2 accepted checkpoints form one chain in the
history tree; I-3 `lastRotationTime` changes only on a transition at least
`MIN_EPOCH_DURATION` after the previous one; I-4 nothing is accepted after
expiry, a halt or a cancel; I-5 `acceptedRootHeight` is written once per root.

**Deposits, exactly once.** One successful `deposit` emits one `Deposit` and,
on Arbitrum, one message in the same transaction; a revert emits neither. B3
mints at most once per nullifier `(chain, vault, id)`
(`src/bridge/admission.h:196-207`) [E]. A flush is one `deposit`. There is no
refund once tokens are in a vault, so every accepted deposit must be mintable:
hence the caller rule and the fee rule of §6.2.

**Withdrawals, exactly once.** One burn makes one leaf with a global id and a
vault tag; only the named vault computes it, at most once. Accepted roots lie
on one B3 branch (I-2) and B3's tree is append-only, so under the committee
assumption an id maps to one leaf in every accepted root. A failed `release`
changes nothing.

**Escapes.**

| Funds at | Way out |
|---|---|
| F_d, before flush | `refund` by `refundEOA`; `returnToOwner` after `notAfter` |
| A vault with a live verifier | `release` of certified leaves, including a migration to a successor (§6.4) |
| A vault whose verifier halted, expired or lost its lineage | Only leaves under roots accepted before the stop. **The whole remaining reserve is frozen for ever**: no new root is possible, the verifier pins one lineage, and no key or new deployment reaches the tokens |
| Burned on B3 above the frozen `latestBridgeHeight` | Unpayable. B3 can prove it (status by kind 9 or heartbeat; expiry by anchor time or heartbeat `l2_timestamp` past `setExpiresAt`), so node changes could re-credit such burns [proposal] |

**What a stop costs, plainly.** A conflict halt (even when the first
certificate was canonical) or 30 days without a relayed rotation freezes that
vault's unreleased reserve, or both if quorum is lost; with one bUSD, holders
exiting first through the other vault are paid in full and the rest carry
the loss. A cancel costs nothing. Mitigations (B-23, C-11; architecture
§8.1): a resumable lineage that breaks only after the 30-day expiry, the
MIGRATE leaf (§6.4) and monitored relays to both chains. **Residual, for
explicit owner acceptance:** 30 days without a relayed rotation, or a
conflict halt, still strands. Not default: a per-vault reserve ceiling
(B-14).

**Contract-level threats** (the architecture doc §9 owns the full disclosure):

| Threat | Effect | Mitigation |
|---|---|---|
| A dual quorum signs a false root | Drains a vault up to its balance | Committee assumption, P3; caps do not bound it |
| Conflicting quorum certificates | First wins, then halt; reserve frozen | §3.4; only P3 exports on mainnet; C-11 |
| Relays withheld | Deposits close after 24 h without a certificate (72 h without a rotation); expiry at 30 days freezes the reserve | Permissionless relaying; monitored relayer |
| Set above 64 or below the weight floor | Roots stop and deposits close until a qualified set returns; rotations continue (§3.3); likely without B-22, as Set_2 has 34 members (`.../study/check.md:29`) and stake splits cheaply | Bounded committee (V2-D5, B-22) |
| Outbox upgraded or its `rollup` (slot 0) or `bridge` (slot 1) re-pointed, forged roots | bUSD minted with no Vault_A deposit, burnable against Vault_E | Node pins of implementation, slot 0 and slot 1 (Appendix A; kind 7 fails closed, B-18); caps |
| Issuer pause, blacklist, burn or upgrade | Deposits refused; releases revert or pay frozen addresses | Gate; heartbeat; wallet pre-checks; C-10 |
| Junk-log flush; smart-wallet deposit; fee-splitting flushes | Unmintable deposit; balance lost to fees | PR-2; caller rule; whole-balance flush, lifetime cap |
| Wrong init-code hash, salt or lost exit parameters | Funds unreachable | Manifest pin; vectors; p carried by the B3 burn (node §7.2); optional `publish(p)` |
| Contract bug | Immutable code | Audit (S6), drills (RT9); MIGRATE to a fixed successor while live |

---

## 11. Gas

| Item | Gas | Basis |
|---|---|---|
| V1 certificate relays (17 members, 5 absent) | 1,976,420 and 1,872,058 | [E] `.../study/check.md:30` |
| V1 `verify()`, identical on both chains; precompiles ~153k of it | 1,618,093 | [E] `check.md:38`, `:55-56` |
| V1 64-validator worst case | 5,513,351 measured; 6,283,311 conservative | [E] `doc/design/b3-cross-chain-finality-v1.md:336-341` |
| V2 certificate relay (ancestry path, extra writes) | ~1.9-2.1M; worst case ~6.3-6.4M | [H] |
| V2 deposit Ethereum / Arbitrum | ~170-175k / unmeasured | [H] |
| V2 release; Arbitrum adds a heartbeat | 90-120k; plus `sendTxToL1` | [H] |
| F_d `deploy` / `flush` / `collectFee`, `refund`, `returnToOwner` | 120-160k / 200-250k / 50-70k | [H] |
| F_x `forward` first / later; `publish` | 300-420k / 220-330k; ~35k | [H] |

The 64-validator cap (B-22) keeps the worst case under Ethereum's
16,777,216 cap. Route (b)'s F_d deploy and flush and route (d3)'s `forward`
come from the user's fee cap or amount; architecture §7 and §10 own every
dollar figure.

---

## 12. Foundry test plan and regtest milestones

Offline, no forge-std, no new dependencies; nothing has run. Tests extend
`contracts/test/TestBase.sol:7-36` with `etch`, `load`, `recordLogs` and
`prank(address,address)`. Gas tests assert `gasUsed < 16,777,216`, because
anvil 1.7.1 enforces EIP-7825 only with `--enable-tx-gas-limit` (local
`anvil --help`) [E]; the regtest plan's Ethereum anvil must use that flag.

| File in `contracts/test/v2/` | Key tests |
|---|---|
| `ProverV2.t.sol` | Real BLS vectors (extended `contracts/test/vectors/generate_bls64_proof.cpp`: ruleset 2, 64 members, 43 signers); tampering and ruleset-1 headers fail; V1's `bls64_proof.bin` fails |
| `VerifierV2.t.sol` | Ancestry after skipped certificates; index 0, bad path, other lineage rejected; 6 h spacing and k-epoch catch-up; expiry; `SuccessorMismatch`; root history; `latestBridgeRoot`; roots on both sides of V; all three conflict forms, none from a rotated-out set; relaxed windows only on 31337/31338 |
| `BootstrapV2.t.sol` | Start certificate at epoch 0 and k; wrong set hash; bad leaf-0 path; deadline; K_OF_N; cancel inside but not after the window |
| `VaultV2.t.sol` | Gate states; caller rule (EOA and F_d pass; Safe-like, bundler-like, 7702 fail); `Deposit` identical to V1; fee 10,000 accepted, 10,001 rejected; payload, heartbeat and leaf vectors; **release against an older root**; cross-vault and double release rejected; blacklisted recipient paid; vault burn then heartbeat; `absorb`; **migration**: a leaf paying a second vault, whose `absorb()` then lets it release its own leaves; a second release of that id reverts |
| `StorageLayoutV2.t.sol` | Every slot of §3.2 and §6.6 |
| `ForwarderDepositV2.t.sol` | Only Transfer and Deposit logs; leaf size against §7.1's model; contract callers rejected; **repeated max-fee flushes never exceed `feeCap`**; `returnToOwner`; OFT credit before deployment; fuzz over D1-D6 |
| `ForwarderExitV2.t.sol`, `RelayBatchV2.t.sol` | Address vectors; fee and native refund; allowance reset; batch skips released ids, isolates failures, heartbeats after an Arbitrum rotation, fits the cap with a 64-member certificate |

Also `contracts/script/check-no-number-opcode.sh` fails on `NUMBER` in any V2
runtime. Commands (not run): `cd contracts && forge test --offline
--match-path 'test/v2/*' -vvv`, then `--gas-report` and `forge snapshot`.

**Regtest.** The regtest plan owns harness, mocks and RT0-RT10 content.
Contracts appear in RT0 and RT3 (V1 baseline via
`contracts/script/Deploy.s.sol:19-58`), then RT4 (`DeployV2.s.sol` on both anvils,
31337 and 31338, with relaxed windows; start certificates and the K_OF_N fallback;
route (a) releasing a real B3 leaf; conflict halt; migration; needs the S3
node core slice), RT5 (a Vault_E leaf paying and the same leaf reverting at Vault_A),
RT6 (forwarders), RT7 (Vault_A messages, a mock Outbox behind an EIP-1967
proxy, a Vault_A release), RT8 (P3, contracts unchanged), RT9 (drills) and
RT10 (production scripts and pin flow on both anvils).

---

## 13. Owner decisions

The owner accepted the direction on 2026-10-03; every B- and C- default
stays open to ruling, and B-11's start certificate needs one (§5).
Contract-level choices [I]:

| ID | Decision | Default |
|---|---|---|
| C-1 | Releases continue after a conflict halt | Yes |
| C-2 | Bootstrap mode; watchers, review window, init deadline | START_CERTIFICATE (k-of-n fallback); ≤ 5; 7 days; 7 days |
| C-3 | Vault sends heartbeats (release, public call, batch after an Arbitrum rotation) | Yes |
| C-4 | F_d factory on Arbitrum | Later |
| C-5 | `MAX_LP_FEE_PPM` | 10,000 = B3's ceiling, which may never go lower |
| C-6 | `notAfter`; `refund` and `returnToOwner` | Creation + 30 days; yes |
| C-7 | `publish(p)` for exit parameters | Optional; p is carried by the B3 burn (node §7.2) |
| C-8 | Signing headers in calldata, hashes in storage | Yes |
| C-9 | `MAX_DEPOSIT_RAW` | 10,000 units (architecture §11); a lower value changes it, node §3.3 and the LP fill bound together |
| C-10 | Admit USDT and USDT0 without per-token adapters, against the token-semantics gate | As architecture B-24: kind 9 watches USDT; recorded waiver for USDT0 |
| C-11 | Mitigations for a stopped verifier freezing its reserve (§10) | B-23: resumable lineage (extension ≥ 46,080), MIGRATE leaf, monitored relays; owner accepts the residual |
| C-12 | 6 h minimum, 24-day expiry floor, relaxed windows on 31337/31338 | Yes |
| C-13 | Vault_E deposits only from codeless EOAs or F_d accounts | Yes |
| C-14 | `MAX_CERTIFICATE_AGE` (§4, B-10) | 86,400 s on both chains, the owner relayer re-certifying when Verifier_A's gate nears closing; alternative ~48 h on Arbitrum, splitting B-10 per chain |

Choosing V (= `BRIDGE_ACTIVATION_HEIGHT`, fixed before deployment) and W2
(default **W2 = V**, B-12) are explicit rulings
(`doc/design/b3-master-handoff.md:115-118` keeps W unset until enabled and
discloses the custodial wait before it; W2 = V has none).

---

## 14. Open items

- **FD-15** (finality §10.2): whether a conflict halt should stop both
  verifiers for good (C-1 keeps releases), and whether to add a
  replacement-successor or succession path the live set signs before expiry.
- **Re-credit** of burns above a frozen `latestBridgeHeight` (§10), not
  taken up by node changes.
- **Governing text** for the reconciliation commit (architecture §15, B-20):
  one `B3_ASSET_ID` across both vaults against
  `doc/design/b3-architecture-contract.md:373-375` and `:636-643`; the
  adapter gate (C-10); V and W2; the start certificate against the 3-of-4
  bootstrap text.
- **Unverified [H]:** V2 gas; `sendTxToL1` gas; EIP-2537 on anvil 1.7.1; USDT0
  options and DVNs (O11); LayerZero fees; the leaf-size model; live USDT fee
  and pause state.
- **Before activation:** the reconciliation commit, audit (S6), drills (RT9).

---

## Appendix A. External addresses

| Item | Value | Source |
|---|---|---|
| USDT (Ethereum) | `0xdAC17F958D2ee523a2206206994597C13D831ec7` | `src/consensus/bridge_params.h:46-49` |
| USDT0 token / OFT (Arbitrum) | `0xFd086bC7CD5C481DCC9C85ebE478A1C0b69FCbb9` / `0x14E4A1B13bf7F943c8ff7C51fb60FA964A298D92` | https://docs.usdt0.to/technical-documentation/deployments |
| USDT0 Ethereum adapter | `0x6C96dE32CEa08842dcc4058c14d3aaAD7Fa41dee` | same |
| LayerZero endpoint ids | Ethereum 30101, Arbitrum 30110 | same |
| ArbSys | `0x0000000000000000000000000000000000000064` | https://github.com/OffchainLabs/nitro-precompile-interfaces/blob/main/ArbSys.sol |
| Arbitrum Outbox (Ethereum) | `0x0B9857ae2D4A3DBe74ffE1d7DF045bb7F96E4840`, EIP-1967 proxy. B3 pins its code hash, implementation slot `0x360894a13ba1a3210667c828492db98dca3e2076cc3735a920a3ca505d382bbc`, `rollup` slot 0 (only writer of `roots`; `updateRollupAddress` re-points it to `bridge.rollup()`) and `bridge` slot 1; proves `roots` slot 3 (node changes §5.2; architecture B-18) | https://docs.arbitrum.io/for-devs/useful-addresses; https://raw.githubusercontent.com/OffchainLabs/nitro-contracts/main/src/bridge/AbsOutbox.sol; https://eips.ethereum.org/EIPS/eip-1967 |
