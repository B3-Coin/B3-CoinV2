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
