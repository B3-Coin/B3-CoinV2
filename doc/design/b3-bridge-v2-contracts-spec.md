# B3 Bridge V2: contracts specification (Ethereum launch)

> **Status: PROPOSAL, no governing authority**; below every owner ruling and
> reviewed governing document; nothing ratified, built, audited or deployed.
> Revision 9, 2026-10-04, the **simpler plan**, base `1d1d6da`, branch
> `feat/flowmeshV2-dev-claude`. Revision 8 (two vaults, LP field; this file
> at `1d1d6da`) is withdrawn for launch; its Arbitrum parts are in Appendix
> B. Nothing compiled or run; no RPC contacted; hashes recomputed with
> `cast` 1.7.1.

**Labels.** [E] verified; [I] design choice; [H] estimate. **Default** =
provisional. Repo facts are `path:line` at `1d1d6da`; `.../study/` is
`/Users/josh/development/ON/B3-FlowMesh-200ms-evidence/arbitrum-bridge-study-20261003/`;
"Codex review" is the external review relayed on 2026-10-03.

**Ownership.** The [architecture document](b3-bridge-v2-arbitrum-proposal.md)
owns scope, routes, trust, every latency and cost number (§8), stages and
milestones (§14) and decisions B-1 to B-37 (§15). This document is **the
only normative source for every byte layout a contract hashes, stores or
emits** (§2.1-§2.2, §3.2, §6.4, §6.6, §7.2; summary §2.5), except the bUSD
V2 AssetId preimage (node changes §3.2); a differing sibling text is an
error. Section numbers are revision 8's; §6.3 is a pointer.

---

## 0. Summary

**Owner ruling, 2026-10-03:** one fresh Ethereum vault, Arbitrum users
through USDT0's LayerZero bridge, no liquidity provider (LP), no signers
minting. Six new keyless contracts, **all on Ethereum** (chain 1; anvil
31337 on regtest) [I]:

| # | Contract | Job |
|---|---|---|
| 1 | `BlsDigestProver` | V1 BLS and dual-quorum code behind `verifyDigest` (§3.6) |
| 2 | `B3FinalityVerifierV2` | Digest-V2 certificates with ancestry; root history; conflict halt; windows (§3) |
| 3 | `B3VaultV2` | Holds the USDT reserve; deposit gate; release against any accepted root, including a migration (§6) |
| 4 | `B3DepositForwarderFactory` + `B3Forwarder` (F_d) | Per-user CREATE2 address flushing USDT delivered from Arbitrum into the vault (route (b), §7) |
| 5 | `B3OftExitFactory` (F_x) | Takes a release and sends it through the USDT0 OFT adapter to Arbitrum (route (d3), §8) |
| 6 | `B3RelayBatch` | Certificates, releases and exit forwards in one transaction (§9) |

No owner, admin, pause, proxy, rescue key or fee switch; the only privileged
acts are the one-time 3-of-4 bootstrap attestation and a watcher cancel
before deposits open (§5). bUSD V2 is
backed 1:1 by this vault only.

**Not at launch** (Appendix B): an Arbitrum vault and its Outbox pins, an LP
field, multi-vault accounting, or any signer-attested mint (OD-8,
`doc/design/b3-open-decisions.md:306`, stays intact).

**Improvements over V1** [I]: ancestry (B-9); claimable historical roots and
failed transfers that consume nothing (Codex review, §6.4); a conflict halt;
catch-up windows (B-10); a deposit gate (B-12) that B3's conflict flag
closes for good; the k-of-n bootstrap (B-11, accepted in conversation,
uncommitted); a MIGRATE leaf
(B-23). B-37 needs no contract change (§3.7).

---

## 1. Scope

- **Lapsing, untouched (B-1):** V1 sources in `contracts/src/` and the
  deployed verifier, prover and vault
  (`contracts/deployments/ethereum-mainnet-v1.1.1.json:45`, `:99`, `:101`;
  addresses in architecture §3), expiring 2026-10-08T17:11:59Z; legacy bUSD
  stays frozen (B-19).
- **New** [I]: `contracts/src/v2/`, `contracts/src/v2/periphery/`,
  `contracts/script/DeployV2.s.sol`, `contracts/test/v2/`; settings of
  `contracts/foundry.toml:1-14`; EIP-170 check of
  `contracts/script/check-deployable-sizes.sh:9-30`.
- **Reused** [E]: the EIP-2537 path (`contracts/src/BlsCertificateProver.sol:52-121`),
  the member tree (`:145-156`), the SetHeader
  (`contracts/src/IB3FinalityProver.sol:29-53`), the balance-delta deposit
  and tolerant token calls (`contracts/src/B3StakerBridge.sol:95-115`,
  `:176-196`), B3's ordered tree (`src/modern/withdrawal_tree.h:100-131`).

---

## 2. Digest V2

### 2.1 Objects

```solidity
library B3TypesV2 {
    uint16 internal constant RULESET_V2 = 2;
    struct FinalizedBlockV2 {          // 185 bytes packed, big-endian, this field order
        bytes32 lineageId;
        uint64  height;
        bytes32 blockHash;
        bytes32 withdrawalRoot;        // §2.4: zero below BRIDGE_ACTIVATION_HEIGHT (= V), tree root from it
        bytes32 validatorSetHash;      // setHash of the successor SetHeader, as in V1
        uint64  epoch;                 // epoch of the signing set
        uint64  checkpointIndex;       // ≥ 1; this checkpoint's position in the history tree
        bytes32 checkpointHistoryRoot; // depth-32 root over leaves 0 .. checkpointIndex-1
        uint8   bridgeFlags;           // bit 0: B3 closed burns for good (§3.3 step 11); others zero
    }
}
```

The finality dependency's §7 sources each field. SetHeader keeps V1's
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
cannot verify under V2 (tag, length and header version differ). As in V1
(`.../study/check.md:73`) [E], the digest names no destination chain, so a
later Arbitrum verifier could read the same certificates (Appendix B.1).

### 2.3 Ancestry rule [I]

B3 appends one history leaf per grid checkpoint, certified or not; leaf 0 is
block R_f − 1. A certificate at index k ≥ 1 signs the root over leaves
0..k−1; B3 never signs index 0. The verifier stores the last accepted (index,
height, blockHash, historyRoot, certId) and accepts a new certificate only if
`latestIndex < checkpointIndex < 2^32`, `height > latestHeight`, and a
32-sibling path places `historyLeaf(latestHeight, latestBlockHash)` at
`latestIndex` under the new root (index bits order the hashing, as
`contracts/src/B3StakerBridge.sol:142-148`). A previous-certificate pointer
would force relaying all ~111 certificates a day (10-block grid,
`src/kernel/chainparams.cpp:293`).

### 2.4 Withdrawal root and BRIDGE_ACTIVATION_HEIGHT [I]

`BRIDGE_ACTIVATION_HEIGHT` is **exactly V**, node changes'
`activation_height`, pinned in verifier and vault and mirrored by B3, which
refuses to start on a mismatch. B3 signs `withdrawalRoot = 0` below V and
the V2 tree root from V, non-zero even when empty
(`src/modern/withdrawal_tree.h:116-131`), as V1 does
(`src/node/bridge_state.cpp:1162-1171`). Certificates in [R_f, V) rotate the
verifier before V.

Burns are valid from W2 ≥ V, unseen by the contracts; default **W2 = V**
(B-12), avoiding the custodial wait of
`doc/design/b3-master-handoff.md:115-118`. If V must move, deploy anew and
`cancel()` or outlive the old deployment, which could otherwise open
deposits B3 never mints.

### 2.5 Summary table and shared vectors

| Item | Normative here |
|---|---|
| History leaf | keccak256(u64 height ‖ blockHash) |
| Bootstrap statement | an epoch-k statement attested 3-of-4 (§5) |
| Withdrawal root | zero below V (= `BRIDGE_ACTIVATION_HEIGHT`), tree root from V |
| Bridge flags | bit 0 closes deposits for good (§3.3 step 11) |
| Leaf V2; migration leaf | 180-byte tagged form; an ordinary leaf paying the successor (§6.4) |
| Deposit event | V1's `Deposit(uint64,address,uint256,bytes32)` (§6.2) |
| Verifier liveness | `status == 1` |

`contracts/test/vectors/gen_v2_vectors.sh` [I] writes one JSON file for the
Foundry and B3 unit tests (lineage id, digest, `certId`, history leaf and
path, leaf V2, the bUSD V2 AssetId with the preimage of node changes §3.2,
F_d and F_x addresses); RT4 waits until C++ and Solidity agree.

---

## 3. Verifier V2

### 3.1 Immutables

| Immutable | Default | Constructor check |
|---|---|---|
| `CHAIN_DOMAIN`, `GENESIS_HEIGHT`, `GENESIS_BLOCK_HASH`, `GENESIS_SET_HASH` | Fresh lineage (§2.2) | non-zero |
| `LINEAGE_ID` | §2.2 | equals the recomputed value |
| `START_EPOCH`, `START_SET_HASH` | Newest epoch at deployment and its set hash (§5) | `START_SET_HASH` non-zero; if `START_EPOCH` is 0 it equals `GENESIS_SET_HASH` |
| `BOOTSTRAP_MODE`, `BOOTSTRAP_SET_HASH` | K_OF_N; four published bootstrap keys (§5) | K_OF_N: 4-member, weight-4, quorum-3 header (`contracts/src/B3FinalityVerifier.sol:117-121`); START_CERTIFICATE: zero |
| `PROVER`, `PROVER_CODE_HASH` | `BlsDigestProver` | code hash equal |
| `BRIDGE_ACTIVATION_HEIGHT` | V (§2.4) | > `GENESIS_HEIGHT` |
| `MIN_BRIDGE_VALIDATORS`, `MAX_BRIDGE_VALIDATORS`, `MIN_BRIDGE_TOTAL_WEIGHT` | 4, 64, 900 (V1 manifest `:32-34`) | 4 ≤ min ≤ max ≤ 64 |
| Windows | §4 | §4 |
| `INIT_DEADLINE`, `REVIEW_WINDOW`, `WATCHERS` (≤ 5) | deploy + 7 days; 7 days; owner-chosen | §5 |

The 64 maximum (V1's, within the gas cap, §11) only stops roots from larger
sets; making B3's committee the top 64 is B-22 and **needs explicit owner
approval** (Codex review).

### 3.2 State (normative slots)

Storage keeps set hashes; callers pass the signing header in calldata (V1
stored headers, `contracts/src/B3FinalityVerifier.sol:55-58`) [I]. B3 proves
these slots:

| Slot | Content |
|---|---|
| 0 | `Liveness {uint64 currentEpoch; uint40 lastRotationTime; uint40 lastCertificateTime; uint64 latestBridgeHeight; uint8 status; bool depositsClosed}` |
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
    `setHashByEpoch`, summary, `lastRotationTime = now`, `nextSetHash = 0`).
    Then, on **every** certificate: if `nextSetHash` is zero pin `successor`;
    otherwise revert `SuccessorMismatch` unless `fb.validatorSetHash ==
    nextSetHash`, so a same-epoch certificate naming another successor fails
    (V1 `:259-274`).
11. Store latest (height, index, blockHash, historyRoot, certId) and
    `lastCertificateTime`; if `fb.bridgeFlags` bit 0 is set (B3 closed burns
    for good, node changes §8.5), set `depositsClosed` for good; emit
    `Finalized(certId, epoch, height, withdrawalRoot, bitmap)`.

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

Each proves the quorum signed two incompatible histories; the third lets
anyone halt with an older conflicting certificate seen off Ethereum, for
example on B3. Effect: `status = HALTED_CONFLICT`, permanently; certificates
and deposits stop. **Default (C-1): releases against accepted roots
continue**, since an attacker can release in the transaction that installs a
false root. A halt freezes the remaining reserve (§10).

### 3.5 Views and deposit gate

```
depositsOpen() = status == LIVE && !depositsClosed && now >= initTime + REVIEW_WINDOW
              && now - lastRotationTime    <= DEPOSIT_CLOSE_AFTER
              && now - lastCertificateTime <= MAX_CERTIFICATE_AGE
              && currentQualified && nextSetHash != 0 && nextQualified
              && latestBridgeHeight >= BRIDGE_ACTIVATION_HEIGHT
setExpiresAt()    = lastRotationTime + MAX_EPOCH_LAG
depositsCloseAt() = min(lastRotationTime + DEPOSIT_CLOSE_AFTER,
                        lastCertificateTime + MAX_CERTIFICATE_AGE)
```

Plus getters for every slot. Qualified means 4-64 members and weight ≥ 900
(V1 `:375-378`).

### 3.6 Prover V2 entry

`BlsDigestProver.verifyDigest(bytes32 digest, bytes32 signingSetHash,
SetHeader signingSet, bytes proof)` is the body of V1 `verify`
(`contracts/src/BlsCertificateProver.sol:59-121`) with the digest as an
argument (not `_finalityDigest`, `:118`) and the shape rule on `RULESET_V2`.
Witness ABI, the proof-of-possession signature suite (`:42`) and dual quorum,
⌊2n/3⌋+1 and ⌊2W/3⌋+1 (`:82-107`), are unchanged [E]. A new deployment is
needed because the deployed prover hard-codes the V1 tag (`:158-176`).

### 3.7 Per-epoch signing keys (B-37, C-15) [I]

Under B-37 (Codex review) each member signs with a fresh key under its own
binding kind, with proof of possession (`src/modern/finality_types.h:207-214`),
erased once its handover is certified and 1,440 blocks deep (node changes
§7.7), not a key an old backup yields (`src/wallet/wallet.cpp:3937-3949`;
`src/wallet/wallet.h:1051-1067`); it protects past epochs only (finality
§6.4). **The contracts do not change**: the verifier sees
only set hashes, so B3 consensus refuses a reused key.

---

## 4. Time windows

| Immutable | V1 | V2 default (B-10) | Constructor rule (mainnet ids) |
|---|---|---|---|
| `MIN_EPOCH_DURATION` (between rotations) | 86,400 s | 21,600 s (6 h) | ≥ 3,600 s |
| `MAX_EPOCH_LAG` (expiry after the last rotation) | 2,592,000 s | 2,592,000 s (30 days) | ≥ `DEPOSIT_CLOSE_AFTER` + 7 days |
| `DEPOSIT_CLOSE_AFTER` (rotation age) | 23 days | 259,200 s (72 h) | > `MAX_CERTIFICATE_AGE` |
| `MAX_CERTIFICATE_AGE` | 86,400 s | 86,400 s (24 h) | > 0 |
| Absolute epoch window | yes (`contracts/src/B3FinalityVerifier.sol:380-389`) | **dropped** | — |

V1 values: `contracts/deployments/ethereum-mainnet-v1.1.1.json:29-36` [E]. `DeployV2.s.sol` pins the B-10 values
on chain 1. Reasons [I]:

- **Catch-up.** Only epoch e or e+1 is accepted. B3 epochs (1,440 blocks,
  `src/kernel/chainparams.cpp:292`) last ≥ 24 h (`src/modern/pos_v1.h:74-79`),
  ~31 h at 78 s: a 24 h minimum gains ~0.23 epochs a day, 6 h ~3.2 (3 at
  60 s). Each rotation still needs the pinned next set's dual quorum.
- **Expiry.** With no unbonding or slashing (`src/modern/stake.h:30-40`),
  expiry only bounds how long rotated-out keys stay dangerous. The 30 days
  fix B-23's extension (≥ 46,080 blocks, architecture §9.1).
- **72 h close.** About two missed rotations at 78 s; a daily keep-alive
  certificate satisfies `MAX_CERTIFICATE_AGE`. On chain 1 `DeployV2.s.sol`
  requires `DEPOSIT_CLOSE_AFTER ≥ 1,472 × S_MAX + 21,600 s` (a 1,440-block
  epoch, t_fin's 32 blocks, 6 h of relay margin; `S_MAX` the slowest
  sustained mean spacing, O8 and O2) [I]. 72 h holds to ~160 s [computed];
  slower, deposits close late in every epoch, immutably.
- **Regtest (C-12).** On chain 31337 any non-zero windows keeping the
  orderings are accepted; `DeployV2.s.sol` refuses them elsewhere.

---

## 5. Bootstrap

**Default K_OF_N (B-11)** [I]: a time-bounded k-of-n attestation, 3-of-4 as
V1 (`contracts/src/B3FinalityVerifier.sol:93-190`) and governing text
(`doc/design/b3-master-handoff.md:106-108`); the direction accepted in
conversation on 2026-10-03 (uncommitted) named k-of-n unless research found
a more trust-minimized option:

1. The deployer pins `START_EPOCH = k` (default: newest), `START_SET_HASH`,
   `GENESIS_*` and `LINEAGE_ID` from `getfinalitylineage` (finality §6.5,
   §11; `getfinalityset` serves only V1 sets,
   `src/rpc/blockchain.cpp:3707-3716`), and `BOOTSTRAP_SET_HASH` over four
   published, PoP-verified keys.
2. Prover, verifier and vault are deployed on Ethereum.
3. Before `INIT_DEADLINE` anyone calls `initialize(fb, startSet, successor,
   leaf0Path, proof)` with an epoch-k statement the **bootstrap keys sign
   3-of-4** over `digestV2(fb)`: `startSet` hashes to `START_SET_HASH`;
   lineage, index ≥ 1 and §3.3 steps 5-6 hold; `leaf0Path` places
   `historyLeaf(GENESIS_HEIGHT, GENESIS_BLOCK_HASH)` at index 0 under
   `fb.checkpointHistoryRoot`. Result: LIVE at epoch k, next set
   `fb.validatorSetHash`, latest = fb, steps 9 and 11 applied,
   `lastRotationTime = initTime = now`; `lastCertificateTime` stays 0, so
   deposits also wait for an ordinary certificate (V1 `:187`, `:310`). The
   path is then unreachable.
4. Review window: certificates accepted, deposits closed; a watcher may
   `cancel()` before it ends (CANCELLED for good, strands nothing).
5. A B3 release pins the vault entry after reviewers check every immutable;
   two independent builds agree on code hashes (S7).

Never 2-of-3: any two quorums share a signer, so one Byzantine key can sign
conflicting statements (Codex review).

**Recommended alternative, START_CERTIFICATE (needs an owner ruling):** step 3 takes an
ordinary epoch-k certificate of `startSet` (step 8); no bootstrap keys
exist. For: no extra keys to publish, guard and retire; the pinned
committee proves itself live before review. Against: it drops the
attesters' independent check; in either mode the trust root is the pins
plus public review. The owner chooses (C-2).

---

## 6. Vault V2

### 6.1 Immutables

| Immutable | Value |
|---|---|
| `ORIGIN_TOKEN` | USDT `0xdAC17F958D2ee523a2206206994597C13D831ec7` |
| Token status checks | `paused()`, `deprecated()`, `isBlackListed(vault)` |
| `FORWARDER_FACTORY` | F_d factory (§7.3), predicted address; its constructor requires `VAULT.FORWARDER_FACTORY() == address(this)` |
| `VERIFIER`, `VERIFIER_CODE_HASH`, `B3_CHAIN_DOMAIN` | §3 |
| `B3_ASSET_ID` | bUSD V2 (preimage: node changes §3.2), computed by the launch vault from `address(this)` as V1 (`contracts/src/B3StakerBridge.sol:88`). A B-23 successor takes `predecessor` and `assetId` as constructor arguments pinned by the migrating hard fork, reverting unless `assetId == predecessor.B3_ASSET_ID()` and `predecessor.ORIGIN_TOKEN() == ORIGIN_TOKEN` |
| `MAX_DEPOSIT_RAW` | 10,000,000,000 (10,000 units, B-14, C-9), equal to B3's per-block and per-epoch caps |
| `EXPECTED_TOKEN_CODE_HASH` | constructor check (§6.5) |

Regtest uses MockTetherToken on anvil 31337 [I]. The token
functions exist as named
(https://sourcify.dev/server/v2/contract/1/0xdAC17F958D2ee523a2206206994597C13D831ec7?fields=sources) [E].

### 6.2 `deposit(uint256 amount, bytes32 b3Recipient)`

V1's signature (`contracts/src/B3StakerBridge.sol:95`); no LP field.

1. `VERIFIER.depositsOpen()`, and every token check returns exactly 32 bytes
   meaning false; otherwise `DepositClosed`.
2. **Caller rule (C-13):** `(msg.sender == tx.origin &&
   msg.sender.code.length == 0) || FORWARDER_FACTORY.isAccount(msg.sender)`.
   B3's V2 receipt node limit is 8,192 bytes (today 1,024,
   `src/bridge/proof.h:45`); Safe, ERC-4337 or EIP-7702 bundles add logs
   without bound, so such wallets fund their F_d by transfer (§7).
3. `amount > 0`; `b3Recipient` is RECIPIENT_V1
   (`doc/design/b3-bridge-recipient-encoding.md:13`; V1 `:101-104`).
4. Pull by balance delta; `0 < received ≤ MAX_DEPOSIT_RAW`; `locked += received`.
5. Emit **V1's `Deposit(uint64 indexed depositId, address indexed token,
   uint256 amount, bytes32 b3Recipient)` unchanged**, so B3's event decoder
   (`src/bridge/deposit.h:130-149`) needs only the new vault pin; its receipt
   size limit and accepted transaction types (`:74`) change as node changes
   §4 says.

Nothing is refunded once tokens are in the vault, so every accepted deposit
must be mintable. `absorb()` (anyone) sets `locked = max(locked,
tokenBalance)` and adds the increase to `absorbedTotal`: a successor uses it
to count a migration, which B3 credits only from that slot (§6.4); stray
transfers it counts create no claim. A transient reentrancy lock guards every
entry point.

### 6.3 Arbitrum messages: not at launch

The launch vault sends none; revision 8's Deposit message, Heartbeat and
tags are in Appendix B.1.

### 6.4 `release`, leaf V2 and the migration leaf (normative)

180-byte preimage, fixed-width big-endian, in this order:

| Field | Bytes |
|---|---|
| `LEAF_TAG_V2 = keccak256("B3/BRIDGE/WITHDRAWAL/V2")` = `0xc250a4de311e4369a5d75a6fdd6351f1cc8ec35929170d1374a5367a5edd10f5` | 32 |
| `B3_ASSET_ID` (bUSD V2; binds the chain domain) | 32 |
| `withdrawalId` | 8 |
| destination chain id (`block.chainid`: 1; 31337 on regtest) | 8 |
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

**Vector** [recomputed 2026-10-04]: asset id 32 bytes of `0x11` (stand-in),
id 7, chain 1, vault `0x2222…2222`, token USDT, recipient `0x3333…3333`,
amount 1,000,000, height 900,000 →
`0x1cb905899ca0c488e158585c0208497d5415d1645035fbca3ca98ee2efb5ad64`.
Node changes must reproduce it.

`release(WithdrawalV2 w, bytes32[32] path)`: `withdrawalId < 2^32`;
`b3Height ≥ BRIDGE_ACTIVATION_HEIGHT`; recipient, amount non-zero; not
`released`; root from `path` (V1 `:142-148`);
`h = VERIFIER.acceptedRootHeight(root)`, `h != 0`, `h ≥ b3Height`;
`amount ≤ locked`; mark, decrement, transfer, emit `Withdrawn`. Release
ignores verifier status and token checks: a frozen root keeps paying (V1
principle, `contracts/src/B3StakerBridge.sol:123-127`). Codex review [I]:
**accepted roots stay claimable** (V1 checks only the latest, `:149-151`),
and **failed transfers consume nothing**: a failing or false-returning
transfer reverts, leaving `released[id]` unset, as in V1 (`:154-156`). A successor vault computes a different leaf for the same
global id (pattern of `contracts/test/B3StakerBridgeMultiVault.t.sol:89-159`).

**MIGRATE leaf (B-23)** [I]: an ordinary leaf V2 naming the old vault v, with
successor v′ as `recipient` and `amount = reserve_v`; no new function, tag or
vector. Only a coordinated B3 hard fork creates it, pinning v′ (same chain,
token and `B3_ASSET_ID`, §6.1), while v's burn gate is READY (at least 8 days
before expiry); I-4 forbids its root after expiry, a halt or a cancel. The
relayer then calls `v′.absorb()`; v′ emits no `Deposit`, and B3 credits v′
only once a state proof of `absorbedTotal` covers the amount. Later deposits
into v are minted and swept to v′ by further such leaves (node changes §7.6).

### 6.5 Token behaviour

Low-level calls accept an empty or `true` return (V1 `:182-196`). With
USDT's optional fee on, deposits record the received amount and a release
lowers `locked` by the gross amount, delivering the net [E]. **USDT checks
only `msg.sender` or `_from`, never the recipient** [E], so a release to a
blacklisted recipient arrives frozen; wallets check
`isBlackListed(recipient)` before a burn. A blacklisted vault makes
`release` revert until reversed, and `destroyBlackFunds` can take its
balance (architecture §10).

USDT upgrades by `deprecate`, which forwards calls and leaves the code hash
unchanged. Committed text wants per-token adapters for such tokens
(`doc/design/b3-bridge-threat-model.md:365-369`;
`doc/design/b3-master-handoff.md:121-122`); instead (C-10, B-24) B3 watches
`deprecated` and the code hash by state proof, and a deprecation closes burns
until a ruled re-pin.

### 6.6 Storage layout (normative for B3 storage proofs)

Vault: slot 0 `{uint64 nextDepositId}`, slot 1 `locked`, slot 2
`mapping(uint64 => bool) released` (revision 8's positions, without
`heartbeatSeq`), slot 3 `absorbedTotal` (lifetime sum `absorb()` added to
`locked`). Verifier: §3.2. B3 proves slots by EIP-1186
against a light-client-proven state root. Packing follows declaration order,
first member in the low-order bytes
(https://docs.soliditylang.org/en/latest/internals/layout_in_storage.html).

---

## 7. Deposit forwarder F_d

### 7.1 Receipt-leaf budget

Modelled leaf sizes [H]: 629 bytes for a deposit or flush, at most 1,133
in the cases modelled, within the 8,192-byte terminal receipt node. Rules
[I]: **PR-1** a flush makes exactly one `vault.deposit` and emits nothing else; **PR-2** `flush`
runs only as the top-level call of a codeless EOA (`msg.sender == tx.origin
&& tx.origin.code.length == 0`; EIP-7702 accounts carry code), so nobody can
wrap it in junk logs; **PR-3** relayers skip a flush whose predicted leaf
exceeds 7,168 bytes (the skipped balance stays refundable); **PR-4** direct
deposits obey the caller rule (§6.2).

### 7.2 Forwarder and address derivation (normative)

`B3Forwarder` has only `exec(target, value, data)`, callable by its factory,
no constructor arguments, no `receive()` [I]. Its address is CREATE2 from the
factory, `salt` and the manifest's `FORWARDER_INIT_CODE_HASH` (metadata is
embedded, `contracts/foundry.toml:13-14`), so it can be paid before
deployment. `salt = keccak256(abi.encode(TAG, fields…))`:

| Factory | `TAG` | Fields, in order |
|---|---|---|
| `B3DepositForwarderFactory` | `keccak256("B3/FWD/DEPOSIT/V2")` | address vault, bytes32 b3Recipient, address refundEOA, uint96 feeCap, uint64 notAfter |
| `B3OftExitFactory` | `keccak256("B3/FWD/EXIT/OFT/V2")` | uint32 dstEid, bytes32 to, uint16 maxBridgeFeeBps, uint96 feeCap, address refundEOA |

`feeCap` bounds F_d's **lifetime** fees (`feesCharged`, D5) and **each** F_x
`forward` (§8), which empties the account, so a reused F_x address pays at
most `feeCap` per withdrawal. Wallet RPCs derive exactly this.

### 7.3 Factory

Immutables: `VAULT` (code hash checked), `TOKEN`, `MAX_DEPOSIT_RAW`,
`MIN_FLUSH_RAW` (1,000,000 raw [I]), `FORWARDER_INIT_CODE_HASH`. Storage:
`isAccount`, `reserved` (accrued fees), `feesCharged` (lifetime),
`owed[account][payee]`. `free = balance − reserved`.

- `deploy(p)` (anyone): CREATE2, `isAccount = true`, `approve(VAULT, max)`;
  emits `Published(account, p)`, as does `publish(p)` (anyone, no state).
- `flush(p, fee)` (anyone, PR-2): deployed; `now ≤ p.notAfter`;
  `feesCharged + fee ≤ p.feeCap`; `amount = min(free − fee, MAX_DEPOSIT_RAW)
  ≥ MIN_FLUSH_RAW`; accrue `fee`; `account.exec(VAULT, 0, deposit(amount,
  p.b3Recipient))`. Any revert undoes the accrual. **The caller picks only
  the fee, never the amount**, so a balance cannot be split into many
  fee-paying flushes.
- `collectFee(account, payee)`: pays accrued fees.
- `refund(p, token, to, amount)`: only `p.refundEOA`, any time; at most `free`
  of `TOKEN`; `to` is not the vault, the account or the factory.
- `returnToOwner(p, fee)` (anyone, after `notAfter`): pays `free − fee` to
  `p.refundEOA` within the lifetime cap, so a user with no Ethereum ETH is
  not stuck. An OFT return to Arbitrum is an open option.

`refundEOA` must be an Ethereum EOA the user controls, proven to the wallet by
an EIP-191 signature; Safes and ERC-4337 accounts are refused.

### 7.4 Route (b) and flush policy

The user calls the Arbitrum OFT `0x14E4A1B13bf7F943c8ff7C51fb60FA964A298D92`:
`send(SendParam{dstEid: 30101, to: F_d, amountLD, minAmountLD: amountLD,
extraOptions, composeMsg: "", oftCmd: ""}, fee, refundAddress)`
(https://github.com/LayerZero-Labs/devtools/blob/main/packages/oft-evm/contracts/interfaces/IOFT.sol)
[E]. The Ethereum adapter pays F_d in USDT by plain transfer; no compose. A
reverted delivery stays retryable [E for EndpointV2; H for USDT0's executor].

**Intake (default) [I].** The wallet hands p to its configured relayer
endpoints (default: the owner relayer) and may `publish(p)`; other relayers
read `Published`. A relayer seeing the address funded checks light-client
freshness and cap headroom, then deploys and flushes in one block, within
t_fl (architecture §8.1). An unserved user flushes or refunds from an own
Ethereum EOA, or awaits `returnToOwner`.

Default `notAfter`: creation + 30 days, capped at the light-client horizon
minus 24 h [I]. **One F_d per deposit (default) [I]:** the lifetime
`feeCap` would starve relayers of a reused address, so each derivation takes
a fresh `notAfter` (hence a fresh address) and a `feeCap` covering deploy,
two flushes and a `returnToOwner` at the wallet's gas ceiling; one repeat
arrival still flushes, and the wallet never shows a flushed address again.

**Invariants.** D1 a flush yields one `Deposit` and no other non-token log;
D2 tokens leave F_d only to the vault, a payee (its accrued fees) or as
`refundEOA` directs; D3 `balance ≥ reserved`; D4 a flush and a refund never
spend the same unit; D5 `feesCharged ≤ feeCap` for ever; D6 a flush deposits
`min(free − fee, MAX_DEPOSIT_RAW)`.

---

## 8. Exit forwarder F_x

Route (d3): the wallet burns bUSD V2 with recipient `F_x = accountOf(p)`.
Anyone may call `forward(p, fee, feeRecipient, nativeRefundTo)` after
`release` pays it; by default a relayer runs both in one `B3RelayBatch`
transaction, the capped fee paying both; wallets size `feeCap` for that
(B-15) [I]. With `msg.value ≥ quoteSend(...).nativeFee`: deploy if needed;
`fee ≤ p.feeCap`; `amount = balance − fee > 0`; `SendParam(30110, p.to,
amount, amount − amount·maxBridgeFeeBps/10⁴, EXTRA_OPTIONS, "", "")`; pay
`fee`; approve the adapter through zero (USDT's rule); `send`; reset the
allowance. Immutables: `USDT`, adapter
`0x6C96dE32CEa08842dcc4058c14d3aaAD7Fa41dee` (checks `token() == USDT`),
`DST_EID = 30110`, `EXTRA_OPTIONS` [H, O11]. `isValid(p)`: `dstEid == 30110`;
`to` a non-zero address word; `maxBridgeFeeBps ≤ 50`; a valid `refundEOA`.

`withdrawOnEthereum(p, token, to, amount)`, only by `refundEOA`, is the other
way out. The burn record carries p (`getbridgewithdrawals`); `publish(p)` is
optional [I]. A LayerZero or USDT0 pause or a short native fee makes `send`
revert and funds wait. USDT0 checks only `from`, and its owner
can `destroyBlockedFunds`
(https://sourcify.dev/server/v2/contract/42161/0x3263cd783823d04a6b9819517e0e6840d37ca3f4?fields=sources)
[E], so **a delivery to a USDT0-blocked `to` arrives frozen**; wallets check
`isBlocked(p.to)` first. An adapter, OFT or endpoint change needs a new F_x
factory and a B3 `exit_forwarder` re-pin by coordinated hard fork; F_d, paid
by plain transfer, is unaffected, its factory fixed for the vault's life
(§6.1; architecture §11).

---

## 9. Batch relayer

`B3RelayBatch.relay(CertInput[] certs, ReleaseItem[] releases, ExitForward[]
forwards, address nativeRefundTo)` holds no tokens, ETH or state [I]: `try`
each certificate in order; skip released ids and `try` each release; `try`
each forward within `msg.value`; return unspent ETH. With root history a
release no longer needs its certificate in the same transaction (V1 did,
`contracts/src/B3StakerBridge.sol:149-151`). Relayers split batches above
15,000,000 gas (EIP-7825).

---

## 10. Invariants, exactly-once and escapes

**Verifier.** I-1 every accepted certificate carries a dual quorum of the
lineage's set for its epoch; I-2 accepted checkpoints form one chain in the
history tree; I-3 `lastRotationTime` changes only on a transition at least
`MIN_EPOCH_DURATION` after the previous one; I-4 nothing is accepted after
expiry, a halt or a cancel; I-5 `acceptedRootHeight` is written once per root.

**Exactly once.** One successful `deposit` (or flush) emits one `Deposit`;
B3 mints once per nullifier `(chain, vault, id)`
(`src/bridge/admission.h:196-207`) [E]. One burn makes one leaf with a global
id and a vault tag, paid at most once by the named vault; accepted roots lie
on one B3 branch (I-2) over an append-only tree. A failed `release` changes
nothing.

**Escapes.**

| Funds at | Way out |
|---|---|
| F_d, before flush | `refund` by `refundEOA`; `returnToOwner` after `notAfter` |
| F_x, before `send` | `withdrawOnEthereum` by `refundEOA` |
| The vault, verifier live | `release` of certified leaves, including a migration (§6.4) |
| The vault, verifier halted, expired or lineage lost | Only leaves under roots accepted before the stop. **The rest, 100% of unreleased backing with one vault, is frozen for ever**: no key or new deployment reaches it |
| Burned above the frozen `latestBridgeHeight` | Unpayable; B3 can prove the stop, so node changes could re-credit such burns [proposal] |

Mitigations (B-23, C-11): a lineage that breaks only after expiry, the
MIGRATE leaf, monitored relays. **The residual needs explicit owner
acceptance.**

**Contract-level threats** (architecture §10 owns the disclosure):

| Threat | Effect | Mitigation |
|---|---|---|
| A dual quorum signs a false root | Drains the vault; caps do not bound it | Committee assumption, P3; no slashing (Codex review) |
| Conflicting certificates | B3's flag closes deposits; a halt freezes the reserve | §3.3-§3.4; only P3 exports on mainnet |
| Set above 64 or below the weight floor | Roots stop; likely without B-22, as Set_2 has 34 members (`.../study/check.md:29`) | B-22 (needs explicit owner approval) |
| Leaked identity secret or old backup | Today every finality key; under B-37 future bindings only (§3.7) | B-37; a double binding voids the seat |
| Tether actions (§6.5) | Deposits refused; releases revert or pay frozen; burns close | Gate; B-24; wallet pre-checks |
| Junk-log flush; smart-wallet deposit; fee-splitting | Unmintable deposit; fees drain a balance | PR-2; caller rule; whole-balance flush |
| Contract bug | Immutable code | Audit (S6), drills (RT9); MIGRATE while live |

---

## 11. Gas

| Item | Gas | Basis |
|---|---|---|
| V1 certificate relays (811,181, 813,771; 17-member sets) | 1,976,420 and 1,872,058 | [E] `.../study/check.md:29-30` |
| V1 `verify()`; precompiles ~153k of it | 1,618,093 | [E] `check.md:38`, `:55-56` |
| V1 64-validator worst case | 5,513,351 measured; 6,283,311 conservative | [E] `doc/design/b3-cross-chain-finality-v1.md:336-341` |
| V2 certificate relay | ~1.9-2.1M; worst case ~6.3-6.4M | [H] |
| V2 deposit (plus a USDT `approve` of ~46k) | ~170-175k | [H] |
| V2 release | 90-120k | [H] |
| F_d `deploy` / `flush` / `collectFee`, `refund`, `returnToOwner`; `publish` | 120-160k / 200-250k / 50-70k; ~35k | [H] |
| F_x `forward` first / later; `publish` | 300-420k / 220-330k; ~35k | [H] |
| (d3) `release` + `forward`, one batch (the F_x fee covers it) | 310-540k | [H] sum of the rows above |

The 64 cap keeps the worst case under 16,777,216 (architecture §4.2);
architecture §8 and §11 own every dollar figure.

---

## 12. Foundry tests and regtest milestones

Offline, no new dependencies, not run. Tests extend
`contracts/test/TestBase.sol:7-36` with `etch`, `load`, `recordLogs` and
`prank(address,address)`. Gas tests assert `gasUsed < 16,777,216`, because
anvil 1.7.1 enforces EIP-7825 only with `--enable-tx-gas-limit` [E].

| File in `contracts/test/v2/` | Key tests |
|---|---|
| `ProverV2.t.sol` | Real BLS vectors (extended `contracts/test/vectors/generate_bls64_proof.cpp`: ruleset 2, 64 members, 43 signers); tampered and V1 proofs fail |
| `VerifierV2.t.sol` | Ancestry after skipped certificates; bad index, path or lineage rejected; spacing, catch-up, expiry; root history; roots on both sides of V; all three conflict forms; **a same-epoch certificate naming another successor reverts**; **a flagged certificate closes deposits for good, releases continue**; **rotations through sets sharing no key (B-37)**; relaxed windows only on 31337 |
| `BootstrapV2.t.sol` | K_OF_N 3-of-4 at epoch 0 and k, 2-of-3 refused; wrong set hash or leaf-0 path; deadline; deposits wait for an ordinary certificate; the start-certificate alternative; cancel only in the window |
| `VaultV2.t.sol` | Gate states; caller rule (Safe-like, bundler-like, 7702 fail); `Deposit` identical to V1; leaf vector; **release against an older root**; **a failing transfer reverts and the leaf stays payable**; double release rejected; `absorb` raises `absorbedTotal` by the increase; **migration** to a successor carrying its predecessor's id (a wrong id or token reverts) |
| `StorageLayoutV2.t.sol` | Every slot of §3.2 and §6.6 |
| `ForwarderDepositV2.t.sol`, `ForwarderExitV2.t.sol`, `RelayBatchV2.t.sol` | Only Transfer and Deposit logs; leaf size; **repeated max-fee flushes never exceed `feeCap`**; an F_x fee above `feeCap` reverts and each forward empties the account; `Published` from `publish` and `deploy`; refunds; mocked OFT credit before deployment; fuzz over D1-D6; address vectors; allowance reset; batch isolates failures and fits the cap with 64 members |

`contracts/script/check-no-number-opcode.sh` fails on `NUMBER` in any V2
runtime, so the code can later run on Arbitrum (Appendix B.1). Commands (not
run): `cd contracts && forge test --offline --match-path 'test/v2/*' -vvv`.

**Regtest** (regtest plan §5): RT3 is the V1 baseline
(`contracts/script/Deploy.s.sol:19-58`); RT4 and RT6 use `DeployV2.s.sol`.

---

## 13. Owner decisions

The contracts implement B-9 to B-12, B-14, B-23 (residual needs explicit
acceptance) and B-24, and need no change for B-22 (only the 4-64 bound,
§3.1; selection needs explicit owner approval) or B-37 (§3.7); B-11's
alternative needs a ruling (§5). Contract-level choices [I]; removed ones
are **withdrawn by the 2026-10-03 simpler-plan ruling**:

| ID | Decision | Default |
|---|---|---|
| C-1 | Releases continue after a conflict halt | Yes |
| C-2 | Bootstrap mode; watchers, review window, init deadline | K_OF_N 3-of-4 (k-of-n accepted in conversation, uncommitted), never 2-of-3 (recommended START_CERTIFICATE alternative needs a ruling); ≤ 5; 7 days; 7 days |
| C-6 | `notAfter`; `refund` and `returnToOwner` | Creation + 30 days; yes |
| C-7 | `publish(p)` for F_d and F_x parameters | Optional; relayers also take p from the wallet (F_d) or the burn record (F_x) |
| C-8 | Signing headers in calldata, hashes in storage | Yes |
| C-9 | `MAX_DEPOSIT_RAW` | 10,000 units (B-14); changes only with B3's caps |
| C-10 | USDT without a per-token adapter | As B-24 |
| C-11 | Mitigations for a stopped verifier (§10) | B-23: extension ≥ 46,080, MIGRATE leaf, monitored relays; owner accepts the residual |
| C-12 | 6 h minimum; relaxed windows on 31337 | Yes |
| C-13 | Deposits only from codeless EOAs or F_d accounts | Yes |
| C-14 | `MAX_CERTIFICATE_AGE` | 86,400 s (B-10) |
| C-15 (new) | Per-epoch keys (B-37) need no contract change; B3, not the verifier, refuses reuse | Yes (§3.7) |
| C-16 (new) | `bridgeFlags` bit 0 closes deposits for good; `absorbedTotal` lets B3 credit a migration | Yes (§3.3, §6.2) |

Withdrawn: C-3 (vault heartbeats), C-4 (an Arbitrum F_d factory; the OFT
reaches F_d) and C-5 (`MAX_LP_FEE_PPM`) (Appendix B.1). V and W2 (default
**W2 = V**, B-12) are explicit rulings.

---

## 14. Open items

- **FD-15** (finality §10.2): permanent conflict halt; a successor path.
- **Re-credit** of burns above a frozen `latestBridgeHeight` (§10).
- **Governing text** (architecture §16): adapter gate; V and W2; a 3-of-4
  bootstrap installing a recent fresh-lineage set, not Set_0; B-37 against
  `src/wallet/wallet.h:1039-1067`.
- **Unverified [H]:** V2 gas; EIP-2537 on anvil; USDT0 options and DVNs
  (O11); LayerZero fees; the leaf-size model; live USDT fee state.

---

## Appendix A. External addresses (launch)

| Item | Value | Source |
|---|---|---|
| USDT (Ethereum) | `0xdAC17F958D2ee523a2206206994597C13D831ec7` | `src/consensus/bridge_params.h:46-49` |
| USDT0 token / OFT (Arbitrum) | `0xFd086bC7CD5C481DCC9C85ebE478A1C0b69FCbb9` / `0x14E4A1B13bf7F943c8ff7C51fb60FA964A298D92` | https://docs.usdt0.to/technical-documentation/deployments |
| USDT0 Ethereum adapter | `0x6C96dE32CEa08842dcc4058c14d3aaAD7Fa41dee` | same |
| LayerZero endpoint ids | Ethereum 30101, Arbitrum 30110 | same |

---

## Appendix B. Later options (not at launch)

Each needs its own ruling (architecture §17); revision 8 holds the full text.

### B.1 Arbitrum vault V2: routes (c) and (d2), optional LP fast-fill

**Shape.** The same prover and verifier on Arbitrum One (42161), where V1
certificates verify (`.../study/check.md:65-73`). Vault_A holds USDT0
`0xFd086bC7CD5C481DCC9C85ebE478A1C0b69FCbb9` and ends each deposit with one
`ArbSys.sendTxToL1`, which B3 proves through the Outbox
(`Outbox.roots[sendRoot]`, slot 3 unverified, `:89`), so mints follow in
**~6.4-6.5 days** (≤ ~14.7 disputed) unless an LP fronts them. Two reserves
override OD-8 `:309` and contract §21 and put USDT0's Safe and Arbitrum's
Security Council behind every holder; Arbitrum clock skew floors expiry at
24 days (`:31-32`, `:48`).

**Tags** [recomputed 2026-10-04; fields in revision 8 §6.3]:
`keccak256("B3/BRIDGE/ARB/DEPOSIT/V2") =
0xa3aaee59ff13350b00c4dbf4adfdc28af6efbb084704d321f9b3f14501588c81`;
`keccak256("B3/BRIDGE/ARB/HEARTBEAT/V2") =
0x12170327ed9ae7f4b022c19f61cf35dbd0a182b819452b4f5362e4aae15b58ac`.

**LP fast-fill.** The deposit carries `maxLpFeePpm` (wallet default 30 bps);
the earliest fill of the exact tuple is repaid at the mint; float ≈ $32,500
for $5,000 a day. Returning with it: B-5, B-18, C-3, C-4, C-5, C-14's ~48 h
alternative, RT7 (regtest plan §11).

### B.2 Staker-signed fast deposits

[FSD](b3-bridge-v2-staker-signed-deposits.md), B-25 to B-36: committee
attestations of Vault_A deposits, ~16-29 min [H], capped and settled by the
Outbox proof; needs B.1, penalties, P3 and an explicit B-25 ruling.

### B.3 An overcollateralized coin

An owner remark: a separate product under another name, never bUSD.
