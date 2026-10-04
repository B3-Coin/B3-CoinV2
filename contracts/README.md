# B3 bridge contracts (Ethereum side)

Authored in-repo. The decentralized prover/verifier/vault stack was mined and
finalized on Ethereum mainnet at block 25,898,729. It is not active or
production-approved merely because it is deployed: the tracked release record
retains `production_approved: false`, no independent external cryptography or
contract audit has been completed, and the production procedure has not been
rehearsed. Those are operator/release risks, not conditions the contracts can
detect. Reproducible compilation, contract tests, and byte-for-byte
source/runtime matching remain release evidence. The B3 build never depends on
a Solidity toolchain.

## Contents

| Contract | Leg | Status |
|---|---|---|
| `B3DepositVault.sol` | historical managed ETH -> B3 prototype | retained only to reproduce/audit managed-v1 vault `0x143F207e23e6aebD7E974be90ac6D434f4c7BFb6`; superseded and excluded from the decentralized deployment |
| `src/B3FinalityVerifier.sol` | decentralized B3 -> ETH finality | deployed at `0xE72B3Fe73F0d42A6e964D33E7BB1cc2EA7a3F690`; time-bounded one-time 3-of-4 BLS handoff to canonical Set_0; no owner/admin path |
| `src/BlsCertificateProver.sol` | B3 certificate proof | deployed at `0x8e612aE4D475d25940E2A2FC907F21b6813eedA7`; EIP-2537 implementation with ordered validator paths and a 64-validator gas bound; **no completed external cryptography or contract audit** |
| `src/B3StakerBridge.sol` | decentralized mainnet USDT deposit/release target | deployed at `0x077839b12cebfbF163acAEAC3A59A015D100c64b`; keyless single-token/AssetId vault; deposits remain fail-closed until verifier initialization, qualified current and successor sets, and a fresh accepted certificate |

## Finalized Ethereum mainnet evidence

The sanitized public record is
[`deployments/ethereum-mainnet-v1.1.1.json`](deployments/ethereum-mainnet-v1.1.1.json).
It binds the ignored local finalizer output, source/build and bootstrap
provenance, and the independently captured Ethereum light-client evidence. The
four public bootstrap identities and their ownership proofs are published in
[`deployments/ethereum-mainnet-bootstrap-members-v1.1.1.json`](deployments/ethereum-mainnet-bootstrap-members-v1.1.1.json),
whose SHA-256 is
`1af9ed3227213d5d02a6c7b84e7392b2a252091f95f954ec122939fb54cd8de3`.
It contains no wallet or BLS private keys. The
finalized contracts are:

| Artifact | Address | Runtime code hash |
|---|---|---|
| Vault | `0x077839b12cebfbF163acAEAC3A59A015D100c64b` | `0xdb267712887568bffd394e46538bddba01da11cefc38e32b2428c00911237f8d` |
| Verifier | `0xE72B3Fe73F0d42A6e964D33E7BB1cc2EA7a3F690` | `0xafdba8befb1aacc832bff4e08dcd92e6645a012ea8a8088b0f2811d916022902` |
| Prover | `0x8e612aE4D475d25940E2A2FC907F21b6813eedA7` | `0x77d2aea2d2a6842fae8b29e64a146622e2f45e772a6c351640ffe8362211a959` |

The vault pins canonical mainnet USDT
`0xdAC17F958D2ee523a2206206994597C13D831ec7`, raw/EVM AssetId
`0xc69c6bd581c3188fe80d97cf9946f34c79ec502ccf204cd597dc9366315d61ad`,
and a 10,000-USDT single-deposit ceiling. The pilot B3 cap is 10,000
six-decimal bUSD per 1,440-block epoch (nominally one day), with the per-block
cap no higher than 10,000. The full two-way owner ruling sets both inbound
`B = 811,001` and outbound `W = 811,001`.

The release checkpoint is raw Ethereum root
`0xf6744774a1bcfe910c643e447cd09fe8443cc2edc25d9ae65155b3cbbef3b646`
at slot 15,136,512. The reviewed fork schedule is valid through epoch 479,999
and has a 2026-10-04 20:00:23 UTC operational horizon. Crossing that horizon
without separately reviewed replacement pins is a stop condition.

Deployment is not activation. Machine-enforced gates require the exact pinned
deployment and rules, successful verifier initialization before its immutable
deadline, qualified current and successor sets of 4–64 validators and at least
900 B3 total weight, a fresh accepted certificate, and each operation's exact
proof. Equal B/W heights do not bypass them.

Separately, operators must include the four intended Set0 stakes by B3 height
810,980 with confirmed current finality bindings, and must require independent
execution providers plus successful relayer runtime/state checks before
accepting funds. Those are operational procedures, not predicates the
contracts can observe.

External audit and end-to-end operational rehearsal are still incomplete.
Neither the Ethereum contracts nor B3 consensus can observe them. Operators
must treat them as unresolved release risks and keep
`production_approved: false` until they are completed, but they are not another
on-chain fail-closed predicate.

## Invariants

- The `Deposit` event shape is consensus-relevant on the B3 side and must
  byte-match `src/bridge/deposit.h` (`Deposit(uint64,address,uint256,bytes32)`,
  id and token indexed, amount = received balance delta).
- The current production target has no mutable owner slot, release/rescue key,
  proxy, pause, or upgrade path. Funds leave only through a withdrawal proof
  under a bridge-qualified B3 staker-finality root.
- The published managed smoke vault is historical, superseded, and cannot change its
  authority in place. Because it held zero USDT at the recorded observation and
  B3 bridge minting has not activated, the current new vault is a pre-activation
  replacement, not a user-balance migration. Recheck those facts and exclude the
  old vault/AssetId before activation; any discovered liability requires the
  explicit migration process described below.
- Verified at Ethereum block 25,877,643: both immutable authorities are the
  EOA `0x76c7a245d0D2e4CF92403aF0144825df1cC614f1`; the runtime code hash is
  `0x1be220c18efa4e4cda0bb1c912c7c41346f5c04d49a36ec2c68f6ddcc5586233`.
  The vault is generic and held zero USDT at that observation, so B3 consensus
  must still enforce canonical USDT and every mint-security gate.

## Adding assets later

The finality verifier is reusable, while each vault is deliberately
single-token and immutable. The current V1 vault and AssetId are specifically
the six-origin-decimal to six-B3-decimal, one-raw-unit-to-one-raw-unit profile.
A separate six-decimal ERC-20 vault (for example USDC, after its own token and
adapter review) can reuse the same verifier without changing the USDT vault.
The shared-verifier test proves that the two vaults keep custody, AssetIds, and
withdrawal proofs isolated.

That does **not** make canonical WETH (18 decimals) or common wrapped-BTC
tokens (typically 8 decimals) drop-in V1 assets. They need a later converted-
vault version that immutably pins both decimal counts, rejects deposits that
cannot be represented exactly on B3, and converts withdrawals back to exact
origin-token units. They also need separately reviewed caps and adapter/token
commitments. The current B3 release has one `busd_bridge` entry, so every
additional asset—including a second six-decimal vault—also requires a later B3
registry activation before deposits are offered to users. Native ETH and
native Bitcoin require different deposit-proof paths and are never silently
treated as ERC-20 tokens.

B3 uses two heights for the first vault. Inbound `B` starts light-client and
verified deposit-mint rules. Outbound `W` starts irreversible `BRIDGE_BURN`
withdrawal leaves and must be at least `B`. This deployment requires a full
two-way bridge and pins both to 811,001; it has no intended inbound-only
interval. The Ethereum contract cannot observe the B3-only `W` pin, and equal
heights do not make `bridgeReady()` true or replace finality and proof checks.

## Staker-verifier delayed bootstrap

The decentralized implementation follows the stake-weighted Modern-PoS
`FINALITY_KEY` lineage. It is not controlled by FlowMesh FN seats. Deployment
pins a synthetic header containing the four already-confirmed bootstrap BLS
identities with equal weight and quorum three. The real validator count,
weights and canonical Set_0 become knowable only at `M - 1`.

A small early set (including two validators) may start and finalize B3 and may
initialize the Ethereum verifier for chain-lineage continuity, but it cannot
authorize bridge withdrawals. B3 consensus and the Ethereum
verifier both require every accepted certificate to carry >2/3 stake weight
and >2/3 validator headcount, so a two-member bootstrap is 2-of-2 and one
high-weight validator cannot act alone. The four-key bootstrap may authorize exactly one
statement: the real block-M-1 hash and canonical Set_0 header. The same BLS
prover verifies that 3-of-4 statement. Initialization then permanently disables
the bootstrap path and starts the Set_0 weak-subjectivity clock; only normal B3
finality certificates and verified set handovers have authority afterward.
Bridge authorization stays closed until both the current and successor
canonical sets have at least four members, meet the pinned minimum total
weight, and satisfy the verifier's freshness requirements.
The initialization transaction must land before its immutable deadline.
Initialization also pins `GENESIS_TIME`. Every epoch certificate must fall
inside the deployment-pinned absolute window
`GENESIS_TIME + epoch * MIN_EPOCH_DURATION <= now <= GENESIS_TIME +
(epoch + 1) * MAX_EPOCH_LAG`, in addition to the per-handover lag checks. This
is paired with a relative rule requiring at least `MIN_EPOCH_DURATION` between
accepted handovers; absolute lower bounds already in the past do not by
themselves stop batch epoch-walking. Catch-up after missed relays therefore
takes one real interval per rotation. A genuinely stalled chain that misses
the maximum window freezes
fail-safe and requires a new reviewed deployment; wall-clock bounds do not
remove the normal PoS weak-subjectivity assumption or replace live relaying.

The contracts may be deployed before M, but USDT deposits remain disabled until
the verifier is initialized and a fresh valid certificate proves qualified
current and successor validator sets. A certificate/readiness gap closes
deposits. There is no corridor-deposit or pre-readiness custody phase. The later
B3 build must also pin and match the complete reviewed deployment tuple before
any deposit is presented as mintable.

Ethereum withdrawals and B3 burns use a separate, stronger code path. The
contracts technically permit them after the configured heights, verifier
initialization, qualified current and successor sets, accepted finality
certificate, and exact withdrawal proof. Independent audit, canonicality and
liveness review, and rehearsal remain necessary release-risk controls, but the
contracts cannot test whether those off-chain activities occurred.

Each deposit is also bounded by immutable `MAX_DEPOSIT_RAW`, checked against
the token balance actually received. The reviewed deployment value must be no
greater than B3's pinned `max_per_block`, because one deposit event is one
indivisible B3 mint/nullifier and cannot be split across blocks. Both caps must
also be no greater than B3 consensus `MAX_MONEY`; with the fixed six-to-six
decimal mapping, this is the same raw-unit boundary. RECIPIENT_V1 also rejects
an all-zero P2PKH key hash so custody cannot be directed to an effectively
unspendable B3 output.

The prover, delayed verifier and vault may be deployed before Set_0 is known so
their exact addresses and runtime hashes can be collected for a later B3 build.
At M-1, three bootstrap operators sign the ordinary `FinalizedBlock` digest
whose successor hash is Set_0, and anyone relays it to `initialize`. Before any
real-fund use, the later build must pin the reviewed Ethereum chain ID,
verifier/vault addresses and runtime hashes, vault deployment block, canonical
USDT address, derived AssetId, bootstrap manifest, threshold values, bridge
height, and audit evidence, and every value must match live code/state. The
deployment candidate commits every config/artifact field plus exact public
bootstrap-manifest and source/build-provenance SHA-256 values; the finalizer
requires that independently approved candidate commitment before it can mark
deployment inputs complete. `production_approved` remains false.
The C++-generated vectors exercise the target-fork EIP-2537 hash-to-curve,
pairing, 3-of-4 aggregation, ordered absent-member path, proof ABI, and full
`initialize` calldata. The largest bridge-authorizing set is immutably capped
at 64 members. Its post-Fusaka Osaka benchmark uses 64 distinct PoP-verified keys, the
minimum 43 signers, and all 21 absent-member paths: 18,144 proof bytes, 18,724
`submitCertificate` calldata bytes, and 5,513,351 measured gas for the complete
successful verifier call (checks, prover, storage, and event). Charging every
calldata byte at the conservative EIP-7623 40-gas nonzero rate gives a
6,283,311-gas transaction bound,
below EIP-7825's 16,777,216 per-transaction cap. The fixture and executable
assertion are `test/vectors/bls64_proof.bin` and
`test/BlsCertificateGasBound.t.sol`. Production still requires independent
cryptography and contract review. The
withdrawal leaf's literal fields encode to 128 bytes
(`8+8+32+20+20+32+8`); the older “164-byte” label was arithmetic, not padding.

The new vault has a new address and therefore a new B3 AssetId. In the current
pre-activation case this is a replacement, not a user-balance migration: the
managed smoke vault held zero USDT at the recorded observation and B3 has not
activated consensus bridge minting, so no consensus bUSD balance exists to
move. The release must recheck those zero-state facts, pin only the new
vault/AssetId before first activation, and permanently exclude the smoke vault
from the production registry. If a managed vault were ever activated or gained
liabilities first, replacement would no longer be safe: it would require an
explicit cutoff, reserve/liability reconciliation, and burn/swap/reissue or a
versioned asset migration. The implemented B3 path gates `BRIDGE_BURN` on the projected
current and next bridge-qualified validator sets and an intact finality
lineage, but that is not sufficient for activation. Although the owner ruling
sets `W = 811001`, burns and releases still require the initialized verifier,
qualified current and successor sets, an accepted finality certificate, exact
proofs, and their configured B3 rules. External audit and operational rehearsal
remain incomplete operator/release risks; they are not code-enforced gates.

## Toolchain (owner/CI)

```
forge build
forge test
script/check-deployable-sizes.sh
forge script script/Deploy.s.sol:Deploy --rpc-url "$ETH_RPC_URL"
```

Do not use raw `forge build --sizes` as the release verdict: Foundry includes
the local deployment script runtime in that report even though it is never
deployed. The checked size gate inspects the three actual Ethereum runtimes and
enforces EIP-170 on each one.

The default script is now the immutable prover -> verifier -> staker-vault
stack. Its mandatory inputs, post-broadcast block proof, and machine-readable
manifest flow are documented in [`DEPLOYMENT.md`](DEPLOYMENT.md). The managed
vault helper is retained only as `DeployManagedVault.s.sol` for reproducing the
historical deployment.

The production evidence must pin the exact Solidity compiler version, EVM and
metadata settings, source commit, both constructor arguments, fetched runtime
bytes, and the byte-for-byte comparison that yields the published runtime
hash. The broad source pragma and an unversioned local Forge install are not a
reproducibility record.

`B3BridgeAssetId.sol` is the byte-for-byte Solidity mirror of
`modern::BridgeAssetIdV1`. The deliberately non-symmetric fixture in
`test/BridgeAssetIdVector.t.sol` and `src/test/bridge_asset_vector_tests.cpp`
pins the uint64 byte order and both AssetId presentations. Ethereum manifests
use raw/EVM bytes32 order; B3 wallet asset lookup retains B3's conventional
reverse `uint256` display. Do not substitute one spelling for the other.

Mainnet minting remains gated on every independently reviewed bridge
proof/readiness pin. FlowMesh A3 does not activate the bridge. The finalized
deployment records `BRIDGE_ACTIVATION_HEIGHT = 811001`, which must match the
explicit B3 inbound `B` pin. The full two-way release also sets `W = 811001`;
the contracts do not infer either activation or readiness from M.

## Historical managed-v1 withdrawal minimum

This section records the safety minimum for the historical managed vault; it is
not the current decentralized activation path. Managed v1 does not make an
arbitrary authority payment a valid redemption.
The B3 consensus path now requires one exact bUSD `BURN` output and a type-10
managed-withdrawal record binding its raw six-decimal amount and Ethereum
recipient; the resulting request is replayed and undone with the active chain.
The operator must wait the pinned B3 finality depth, call `release` exactly
once, durably consume the request id, and reconcile released reserves against
burned supply. **No confirmed burn means no release.** The operator-side
release automation and durable request-consumption database are not implemented
in this repository. The authority remains trusted because the vault itself
enforces only caller, token, recipient, and amount.

## Historical mainnet smoke-test runbook (owner-executed; not activation)

The assistant cannot sign transactions or handle keys; every step below ran on
the owner's machine with tiny amounts and an owner-controlled authority. It is
retained to reproduce the historical smoke test, not to select or activate a
new production vault.

```
# one-time: install Foundry itself (the repository has no Solidity library
# downloads)
curl -L https://foundry.paradigm.xyz | bash && foundryup
cd contracts

# 1. prove the contract behaves (mock USDT incl. the no-return-value quirk,
#    fee-on-transfer, release authority, rejecting receivers)
forge test -vv

# 2. deploy (RELEASE_AUTHORITY = an address you control, for the interim)
export RELEASE_AUTHORITY=0xYourRecoveryAddress
forge script script/DeployManagedVault.s.sol:DeployManagedVault \
    --rpc-url $ETH_RPC --broadcast --interactive

# 3. deposit a tiny amount of real ETH (cast asks for the key interactively)
cast send <VAULT> "depositETH(bytes32)" 0x<32-byte-b3-recipient>     --value 0.001ether --rpc-url $ETH_RPC --interactive

# 4. wait ~15 minutes for Ethereum finality, then hand the tx hash to the
#    verification pipeline -- ALL checks run in the B3 C++ stack:
cd ../contrib/b3bridge
python3 eth_live_test.py /tmp/b3-deposit-proof     --tool ../../build/bin/b3-bridge-ethcheck     --tx 0x<deposit-tx-hash> --vault 0x<VAULT>
```

Success ends with `DEPOSIT PROVEN: id 0, token 0x00..00, amount 1000000000000000 wei,
b3_recipient 0x<yours>` followed by `ALL VERIFIED`.

## Bridge V2 (S3, regtest-first; not deployed)

Stage S3 of the bridge V2 plan (owner ruling R8 in
[`doc/design/b3-bridge-v2-owner-rulings-2026-10-03.md`](../doc/design/b3-bridge-v2-owner-rulings-2026-10-03.md))
adds the launch contracts under `src/v2/`. They implement
[`doc/design/b3-bridge-v2-contracts-spec.md`](../doc/design/b3-bridge-v2-contracts-spec.md)
§2-§6 and §9, together with the directions of open issues OI-1 and OI-10
(architecture §20, `doc/design/b3-bridge-v2-arbitrum-proposal.md`). No V2
contract is deployed on any public network, no B3 pin names one, and nothing
here activates the bridge. The V1 contracts, tests, deployment record and
scripts described above are unchanged.

### Contracts

| Contract | Role |
|---|---|
| `src/v2/B3TypesV2.sol` | Library for digest V2, lineage id, `certId`, history leaf and root-from-path, the ruleset-2 header shape, AssetId V2 and leaf V2. The tag hashes are hard-coded; the vector test recomputes each one from its string. |
| `src/v2/BlsDigestProver.sol` | V1's EIP-2537 prover (`src/BlsCertificateProver.sol`) behind `verifyDigest(digest, signingSetHash, signingSet, proof)`. It takes a precomputed digest and accepts only ruleset-2 headers. The witness ABI, POP DST, ordered depth-13 absent-member paths and dual quorum are V1's: headcount `floor(2n/3)+1` and stake `quorumWeight`. |
| `src/v2/B3FinalityVerifierV2.sol` | Keyless mirror of B3's fresh finality lineage. It covers the one-time bootstrap, certificates checked by checkpoint-history ancestry (so relayers may skip certificates), epoch rotation, the expiry and spacing windows, accepted-root history, the conflict halt, the watcher `cancel` and the deposit gate. |
| `src/v2/B3VaultV2.sol` | Keyless single-token vault. It keeps V1's `deposit(uint256,bytes32)` ABI and V1's `Deposit` event byte for byte, and applies the caller rule (a codeless top-level EOA, or an account of `FORWARDER_FACTORY`). It releases a leaf V2 under any accepted root at or above the leaf's height, exactly once per id. It also has `absorb()`/`absorbedTotal` and a B-23 successor mode. |
| `src/v2/B3RelayBatch.sol` | Stateless batch. It tries the certificates in order, then each release not yet released, and reports each failure in an event without stopping the batch. Each try forwards at most a caller-chosen gas cap (`certGas`, `releaseGas`), because a malformed BLS witness makes an EIP-2537 precompile burn all gas forwarded to it. It has no storage, no owner and no payable entry. |
| `src/v2/IBlsDigestProver.sol`, `src/v2/IB3FinalityVerifierV2.sol` | The interfaces the verifier, vault and batch use. |
| `script/DeployV2.s.sol` | Regtest-only deployment of the four contracts, with post-checks and an optional gitignored manifest. |

Not in this stage: the deposit forwarder F_d, the exit forwarder F_x (spec
§7-§8), and the batch's `ExitForward` items and ETH refund (spec §9). The
vault already pins `FORWARDER_FACTORY`. Until the forwarder slice lands,
DeployV2 sets it to the deployer's predicted, still codeless next CREATE
address (nonce n+4), so only top-level EOAs can deposit. This is a latent
deployer privilege, not only an empty slot: whatever `B3V2_DEPLOYER` later
deploys at nonce n+4 answers the vault's `isAccount()` caller check. A
factory answering true for every address would let contract wallets make
deposits whose receipts B3 cannot prove, so those deposits could never be
minted; spending nonce n+4 on anything else loses the slot for good. The
vault only staticcalls the factory, so no theft path exists. The manifest
records the state (`forwarder_factory_code_hash`,
`forwarder_factory_latent_deployer_privilege`). Before any non-regtest
deployment (S7) the reviewed F_d factory must be deployed in the same
sequence and its code hash pinned in the manifest and by B3. Tokens sent to
the vault by plain transfer create no B3 claim (OI-6); `absorb()` only
counts them as reserve.

### Byte layouts

The normative definitions are the contracts spec §2.2 (hashes) and §6.4
(leaf V2 and the migration leaf). Spec §3.2 and §6.6 fix the storage slots
that B3 proves, and `test/v2/StorageLayoutV2.t.sol` checks them. Here,
TaggedHash(t, m) = sha256(sha256(t) ‖ sha256(t) ‖ m), and integers are
big-endian unless stated otherwise.

- `FinalizedBlockV2` packs to 185 bytes in field order: `lineageId`,
  `height`, `blockHash`, `withdrawalRoot`, `validatorSetHash`, `epoch`,
  `checkpointIndex`, `checkpointHistoryRoot`, `bridgeFlags`.
- `digestV2 = TaggedHash("B3/FINALITY/V2", chainDomain ‖ encode(fb))` and
  `certId = keccak256(encode(fb))`.
- `lineageId = TaggedHash("B3/FINALITY/LINEAGE/V2", chainDomain ‖ u64
  GENESIS_HEIGHT ‖ genesisBlockHash ‖ genesisSetHash)`.
- `historyLeaf(h, hash) = keccak256(u64 h ‖ hash)`. The depth-32 history
  and withdrawal trees hash `keccak256(left ‖ right)`, and bit l of the
  index orders level l.
- Leaf V2 is the keccak256 of a 180-byte preimage: `LEAF_TAG_V2 ‖
  B3_ASSET_ID ‖ u64 withdrawalId ‖ u64 chainid ‖ vault ‖ token ‖ recipient ‖
  u256 amount ‖ u64 b3Height`. A B-23 migration leaf is an ordinary leaf V2
  naming the old vault, with the successor vault as recipient.
- AssetId V2 is V1's `BridgeAssetIdV1` serialization with version byte 2:
  a 147-byte tagged preimage that carries the origin chain id
  little-endian.
- SetHeader keeps V1's 110-byte layout with `rulesetVersion = 2`. Every V1
  header, and therefore every V1 signature, is refused.

### Bootstrap and certificates

The verifier constructor pins:

- the lineage: `GENESIS_*` and `LINEAGE_ID`, which must match the library
  value;
- `START_EPOCH = k` and `START_SET_HASH`;
- the bootstrap mode;
- the prover and its code hash;
- `BRIDGE_ACTIVATION_HEIGHT = V`;
- the bridge qualification bounds (4-64 members and a minimum total
  weight);
- the windows, `INIT_DEADLINE` and `REVIEW_WINDOW`;
- up to five watchers.

The bootstrap runs in one of two modes:

- **K_OF_N (default, B-11).** The constructor takes a ruleset-2 bootstrap
  header with four members, weight 4 and quorum 3, hashing to
  `BOOTSTRAP_SET_HASH`. It refuses any other shape, including 2-of-3.
  Before `INIT_DEADLINE` anyone may call `initialize(fb, signingSet,
  startSet, successor, leaf0Path, proof)` with an epoch-k statement that
  the bootstrap keys sign 3-of-4 over digest V2.
- **START_CERTIFICATE (alternative).** `BOOTSTRAP_SET_HASH` must be zero.
  The same `initialize` call takes an ordinary certificate signed by the
  pinned start set itself, and a bootstrap-key proof is refused.

**What the K_OF_N attestation does not cover (reported to the owner).** The
bootstrap keys sign `digestV2(fb)` only, as spec §5 step 3 says. That binds
the lineage (and through it `GENESIS_SET_HASH`), epoch k and the
epoch-(k+1) successor that `fb` names. It does not bind `START_SET_HASH`
when k > 0, nor the verifier's address or Ethereum chain id. The epoch-k
start set, which certifies until the first handover, is therefore pin-only:
reviewers must check it against `getfinalitylineage`. A published
attestation can also initialize any other deployment with the same lineage,
bootstrap header and `START_EPOCH` but a different `START_SET_HASH`
(`test_KOfN_AttestationDoesNotBindStartSetOrDeployment`). V1's attestation
did bind the start set. No funds are at risk unless B3 pins such a vault.
Closing the gap changes the signed message (for example a bootstrap-specific
digest over chain id, verifier address, `START_SET_HASH` and `digestV2(fb)`)
and with it the C++ bootstrap signer, so it waits for an owner ruling on
spec §5.

In both modes `initialize` checks that:

- `startSet` hashes to `START_SET_HASH` and has epoch k;
- `successor` is the epoch-(k+1) set that `fb.validatorSetHash` names;
- `leaf0Path` places `historyLeaf(GENESIS_HEIGHT, GENESIS_BLOCK_HASH)` at
  index 0 under `fb.checkpointHistoryRoot`;
- **(OI-1)** `GENESIS_HEIGHT < fb.height < V`, with a zero withdrawal root
  and zero flags. Bootstrap therefore never installs a claimable root; it
  sets only the latest checkpoint and the history.

Initialization starts the rotation clock and the review window. Deposits
open only after the review window, once a bridge-qualified set has
certified a root at or above V (`latestBridgeHeight` ≥ V) and every term of
`depositsOpen()` (spec §3.5) holds: status LIVE, deposits never closed by
B3's flag, a qualified current and next set, a rotation younger than
`DEPOSIT_CLOSE_AFTER` and a certificate younger than `MAX_CERTIFICATE_AGE`
(`lastCertificateTime` stays 0 until the first ordinary certificate). A
watcher may `cancel()` before initialization or during the review window.
CANCELLED is final; it strands nothing, because deposits never opened.

After initialization, only certificates count:

- A certificate must be signed by the current set (same epoch) or by the
  pinned next set (handover to epoch+1), and it must extend the stored
  checkpoint through its ancestry path.
- A root certified at or above V by a bridge-qualified set stays claimable
  for good. `acceptedRootHeight` is written once per root.
- `bridgeFlags` bit 0 closes deposits for good. Later certificates are
  still accepted.
- `reportConflict` halts the verifier on a certificate from the current or
  next set that contradicts the stored history. Rotated-out sets cannot
  halt it. Evidence is refused after the current set expires
  (`MAX_EPOCH_LAG` after the last rotation), like certificates (I-4); an
  expired verifier that never halted still reads status 1. The vault keeps
  paying under roots accepted before the halt.
- `nextSetHash` is never zero while LIVE: `initialize` and every rotation
  pin a non-zero successor, and nothing clears it. The zero-next-set
  branches of spec §3.3 steps 2 and 10 and the `nextSetHash != 0` term of
  §3.5 are therefore always settled and not coded.

### Time windows

| Immutable | Chain 1 (B-10; `DeployV2.validateMainnetPins`) | Regtest profile R (DeployV2 defaults) |
|---|---|---|
| `MIN_EPOCH_DURATION` | 21,600 s | 600 s |
| `MAX_EPOCH_LAG` | 2,592,000 s | 72,000 s |
| `DEPOSIT_CLOSE_AFTER` | 259,200 s | 7,200 s |
| `MAX_CERTIFICATE_AGE` | 86,400 s | 2,400 s |
| `REVIEW_WINDOW` | 7 days | 16,800 s |
| `INIT_DEADLINE` | at most 7 days after deployment | deployment + 16,800 s |

Profile R is B-10 scaled by 1/36 (regtest plan §3.3).

The verifier constructor checks the windows by chain id. On every chain id
except 31337 it requires:

- `MIN_EPOCH_DURATION` ≥ 3,600 s and `MIN_EPOCH_DURATION` ≤
  `MAX_EPOCH_LAG`;
- `MAX_EPOCH_LAG` ≥ `DEPOSIT_CLOSE_AFTER` + 7 days;
- `DEPOSIT_CLOSE_AFTER` > `MAX_CERTIFICATE_AGE` > 0;
- `REVIEW_WINDOW` ≥ 7 days, and at least one watcher in K_OF_N mode. The
  K_OF_N trust argument rests on the review window plus a watcher `cancel`,
  so neither is left to manifest review alone. START_CERTIFICATE may run
  without watchers.

Profile R breaks the fixed 7-day term (OI-10), so on 31337 only the
orderings apply:

- 0 < `MAX_CERTIFICATE_AGE` < `DEPOSIT_CLOSE_AFTER` < `MAX_EPOCH_LAG`;
- 0 < `MIN_EPOCH_DURATION` ≤ `MAX_EPOCH_LAG`.

Both cases also need now < `INIT_DEADLINE` ≤ now + `MAX_EPOCH_LAG` and
0 < `REVIEW_WINDOW` < `MAX_EPOCH_LAG`. The tests show that profile R is
accepted on 31337 and refused on chain ids 1 and 11155111, and that the
B-10 values are accepted on chain 1.

### Shared vectors

`test/vectors/gen_v2_vectors.py` writes `test/vectors/bridge_v2_vectors.json`
(schema `b3-bridge-v2-vectors/1`). It uses only the python3 standard library
plus its own Keccak-256, never touches the network, and always produces the
same output. The file feeds `test/v2/VectorsV2.t.sol` and the B3 C++ tests.
It holds:

- lineage ids for three chain domains;
- ruleset-2 SetHeaders;
- `FinalizedBlockV2` encodings with digest V2 and `certId`;
- history leaves, roots and paths;
- leaf V2 with a withdrawal tree;
- the bUSD V2 AssetId.

The generator asserts the documented leaf V2 (`0x1cb905…ad64`) and AssetId
V2 (`0xdb9e32…6fcd`). The F_d and F_x address vectors will be added with the
forwarder slice.

BLS vectors (deviation from spec §12). The spec asks for real blst vectors
from an extended `generate_bls64_proof.cpp` (ruleset 2, 64 members, 43
signers). Stage S3 has none signed over digest V2. `test/v2/ProverV2.t.sol`
instead runs the unedited V1 blst vectors (`bls64_proof.bin`,
`bls64_bootstrap_proof.bin` and the 1-member vector) through `verifyDigest`
with their V1 digests and the headers relabelled to ruleset 2, and drives
digest V2 through the in-EVM signer (`test/v2/utils/BlsTestSignerV2.sol`).
The composition is sound: a BLS signature covers only the 32-byte message
and hash-to-G2 does not depend on what the message encodes, while the
digest-V2 bytes themselves are pinned by the Python generator and the C++
`finality_v2_tests`. A C++-signed digest-V2 proof is left to RT4, where the
node's staging signer produces real digest-V2 certificates.

```
test/vectors/gen_v2_vectors.sh            # rewrite bridge_v2_vectors.json
test/vectors/gen_v2_vectors.sh --check    # exit 1 unless the committed file is current
```

### Build, test and gas

`--offline` keeps forge from downloading a compiler; solc 0.8.35 must
already be cached. `foundry.toml` sets `src = "."`, so every command
compiles every `.sol` file under `contracts/`, V1 included. Add `--out
<dir> --cache-path <dir>` to keep build artifacts outside the tree.

```
cd contracts
forge build --offline
forge test --offline                                   # V1 and V2 suites
forge test --offline --match-path 'test/v2/*'          # V2 suites only
forge test --offline --match-contract GasV2Test -vv    # gas table and runtime sizes
```

Gas measured at this revision on Osaka, by `test/v2/GasV2.t.sol` except
the batch row, which `test/v2/RelayBatchV2.t.sol` measures. The called
contracts are cooled first; certificates run at the minimum headcount
quorum (1, 11 and 21 absent members) with member weights of 250, 100 and
100.
Each cell gives execution gas, then a conservative transaction figure of
21,000 + 40 × calldata bytes + execution. Every conservative figure is
asserted below EIP-7825's 16,777,216.

| Call | n = 4 | n = 34 | n = 64 |
|---|---|---|---|
| `submitCertificate`, same epoch, new root | 792,225 / 956,745 | 3,070,280 / 3,567,600 | 5,360,761 / 6,190,881 |
| `submitCertificate`, transition, new root | 823,308 / 987,828 | 3,104,374 / 3,601,694 | 5,391,175 / 6,221,295 |
| `submitCertificate`, first transition after bootstrap | 840,438 / 1,004,958 | 3,118,493 / 3,615,813 | 5,408,974 / 6,239,094 |

| Call | Execution | Conservative |
|---|---|---|
| `initialize`, K_OF_N 3-of-4 | 935,986 | 1,114,586 |
| `release` (cold slots, fresh recipient) | 86,468 | 153,708 |
| `deposit` | 62,457 | 86,177 |
| `absorb` (first, after a migration leaf) | 55,679 | 76,839 |
| batch: one 64-member transition plus one release (`RelayBatchV2.t.sol`) | 5,497,294 | 6,385,014 |

Relayers size the batch caps from this table: `RelayFixtureV2` uses
`certGas` = 6,000,000 (above the 64-member worst case) and `releaseGas` =
250,000. An off-curve absent-member witness under a 6,000,000 cap costs the
batch 5,845,906 gas and the later items still apply; with all gas forwarded
the same 12,000,000-gas batch runs out of gas
(`test_GasCapContainsAPrecompileGasBurn`).

The runtime sizes are 7,512 bytes (BlsDigestProver), 13,451 bytes
(B3FinalityVerifierV2), 4,315 bytes (B3VaultV2) and 1,884 bytes
(B3RelayBatch). All four are below EIP-170's 24,576 bytes and asserted in
`GasV2`. `script/check-deployable-sizes.sh` still covers only the three V1
runtimes. Spec §12's `check-no-number-opcode.sh` is not created in this
stage; `GasV2.test_RuntimesHaveNoNumberOpcode` stands in for it and scans all
four runtimes (skipping PUSH data and metadata) for NUMBER, DELEGATECALL,
CALLCODE, CREATE, CREATE2 and SELFDESTRUCT, and the prover and batch for
storage writes.

### Regtest deployment (`DeployV2.s.sol`)

`DeployV2` deploys, in order, the prover, the verifier (pinned to the
prover's code hash), the vault (pinned to the verifier's code hash) and the
batch. All four are CREATEs by `B3V2_DEPLOYER` at nonces n to n+3. The
script then reads back every immutable. `run()` and `deploy()` refuse every
chain id except 31337 before they read the environment. Integers may be
decimal or 0x-hex.

| Variables | Meaning |
|---|---|
| `B3V2_DEPLOYER`, `B3V2_CHAIN_DOMAIN`, `B3V2_GENESIS_HEIGHT`, `B3V2_GENESIS_BLOCK_HASH`, `B3V2_GENESIS_SET_HASH`, `B3V2_START_EPOCH`, `B3V2_START_SET_HASH`, `B3V2_BOOTSTRAP_MODE` (0 = K_OF_N, 1 = START_CERTIFICATE), `B3V2_BRIDGE_ACTIVATION_HEIGHT`, `B3V2_ORIGIN_TOKEN` | Required. |
| `B3V2_BOOTSTRAP_AGGREGATE_PUBKEY` (48 bytes), `B3V2_BOOTSTRAP_MEMBERS_ROOT`, `B3V2_BOOTSTRAP_SET_HASH` | Required for K_OF_N only. The 4-member header they form must hash to `B3V2_BOOTSTRAP_SET_HASH`. START_CERTIFICATE refuses a non-zero `B3V2_BOOTSTRAP_SET_HASH`. |
| `B3V2_LINEAGE_ID` | Optional cross-check; by default it is recomputed. |
| `B3V2_MIN_BRIDGE_VALIDATORS` (4), `B3V2_MAX_BRIDGE_VALIDATORS` (64), `B3V2_MIN_BRIDGE_TOTAL_WEIGHT` (900) | Optional bridge qualification bounds. |
| `B3V2_MIN_EPOCH_DURATION` (600), `B3V2_MAX_EPOCH_LAG` (72000), `B3V2_DEPOSIT_CLOSE_AFTER` (7200), `B3V2_MAX_CERTIFICATE_AGE` (2400), `B3V2_INIT_WINDOW` (16800; `INIT_DEADLINE` = now + window), `B3V2_REVIEW_WINDOW` (16800) | Optional windows; the defaults are profile R. |
| `B3V2_WATCHERS` | Optional comma-separated list of up to five addresses (default none). |
| `B3V2_TOKEN_CODE_HASH` (default: the token's code hash), `B3V2_MAX_DEPOSIT_RAW` (10,000,000,000), `B3V2_FORWARDER_FACTORY` (default: the deployer's nonce n+4 CREATE address) | Optional vault pins. |
| `B3V2_MANIFEST_PATH` | Optional. It must be `deployments/local-<name>.json`, which `.gitignore` covers. The manifest records addresses, code hashes, the AssetId and every pin, with `status: "regtest_candidate"` and `production_approved: false`. |

Against a local anvil only (no public RPC):

```
anvil --host 127.0.0.1 --chain-id 31337
# in contracts/, with the B3V2_* variables exported:
forge script script/DeployV2.s.sol:DeployV2 --offline \
    --rpc-url http://127.0.0.1:8545 --broadcast --unlocked --sender "$B3V2_DEPLOYER"
```

Forge writes its broadcast record under `broadcast/` and its cache under
`cache/`; both are gitignored. Set `FOUNDRY_BROADCAST` and `--cache-path` to
keep them outside the tree.

### Not enabled

- **Mainnet deployment and activation are not enabled.** `DeployV2` refuses
  every chain id except 31337. Its chain-1 policy (`validateWindows`,
  `validateMainnetPins`) is exercised by unit tests only. A chain-1
  deployment waits for a later stage and an explicit owner ruling.
- No V2 address, code hash or AssetId is pinned in any B3 build. These
  contracts do not change B3 consensus.
- There has been no external cryptography or contract audit of V2.
  `BlsDigestProver` reuses V1's BLS code, which is also unaudited.
- The open issues in architecture §20 still apply, notably OI-6
  (plain transfers to the vault) and OI-9 (the bootstrap key holders are
  not yet named).
- Open for an owner ruling: the K_OF_N attestation scope (see "Bootstrap
  and certificates") and the forwarder-factory deployer privilege until the
  F_d slice (see "Contracts").
