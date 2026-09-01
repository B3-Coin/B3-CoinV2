# B3 bridge contracts (Ethereum side)

Authored in-repo. The published transition-v1 deployment has been independently
observed on Ethereum; reproducible compilation, contract tests, and byte-for-byte
source/runtime matching remain release evidence. The B3 build never depends on
a Solidity toolchain.

## Contents

| Contract | Leg | Status |
|---|---|---|
| `B3DepositVault.sol` | ETH -> B3 deposits (owner ruling 2026-08-24: this leg first) | source complete; managed-v1 vault `0x143F207e23e6aebD7E974be90ac6D434f4c7BFb6` observed; the bounded type-10 B3 mint path is implemented, while reproducible source/runtime matching, adapter enforcement, audits, and production pins remain gates |
| `src/B3FinalityVerifier.sol` | future decentralized B3 -> ETH finality | staker-only scaffold with fail-closed public bootstrap and bridge thresholds; mock-prover tests pass; **not deployable until canonical Set_0 and real prover vectors are available** |
| `src/BlsCertificateProver.sol` | B3 certificate proof | EIP-2537 scaffold with ordered validator paths and the pinned negative generator; **not production-approved without target-fork cross-language vectors and an external cryptography audit** |
| `src/B3StakerBridge.sol` | future decentralized USDT release | keyless single-token/AssetId vault scaffold; mock-prover release tests pass; **not wired into deployment and must not be deployed yet** |

## Invariants

- The `Deposit` event shape is consensus-relevant on the B3 side and must
  byte-match `src/bridge/deposit.h` (`Deposit(uint64,address,uint256,bytes32)`,
  id and token indexed, amount = received balance delta).
- The vault has no mutable owner slot, proxy, pause, or upgrade path. Funds
  leave only via `release`, restricted to its immutable release authority. The deployed
  transition-v1 vault uses an owner-controlled managed authority; it is not
  the later §5 verifier stack.
- `rescue` (separate `rescueAuthority`, defaults to the release authority at
  deploy) can withdraw ONLY the surplus above the per-token `locked`
  liabilities -- strays, airdrops, force-sent ETH. `rescue` cannot spend
  tracked liabilities; this does not mitigate compromise of `releaseAuthority`
  and is not a production-readiness claim.
- The published mainnet smoke vault was promoted for transition v1 by owner
  ruling on 2026-09-01. Its managed authority cannot be changed in place. A
  later verifier therefore needs a new vault. Under the current B3 identity
  formula the vault address is part of `AssetId`, so the new vault creates a
  new asset identity: migration requires an explicit burn/swap/reissue of old
  bUSD, reserve migration, an old-vault cutoff, and handling/refunds for late
  deposits. It is not an in-place contract upgrade.
- Verified at Ethereum block 25,877,643: both immutable authorities are the
  EOA `0x76c7a245d0D2e4CF92403aF0144825df1cC614f1`; the runtime code hash is
  `0x1be220c18efa4e4cda0bb1c912c7c41346f5c04d49a36ec2c68f6ddcc5586233`.
  The vault is generic and held zero USDT at that observation, so B3 consensus
  must still enforce canonical USDT and every mint-security gate.

## Staker-verifier scaffold (not a deployment artifact)

The decentralized scaffold follows the stake-weighted Modern-PoS
`FINALITY_KEY` lineage. It is not controlled by FlowMesh FN seats and embeds no
real validator count, stake weight, BLS key, or private material. Those public
facts become knowable only from the canonical corridor snapshot at `M - 1`.

A small early set (including two validators) may be followed for chain-lineage
continuity, but it cannot open the bridge. Ethereum requires every accepted
certificate to carry both the B3 >2/3 stake-weight quorum and a >2/3 validator
headcount quorum, so a two-member bootstrap is 2-of-2 and one high-weight
validator cannot act alone. Deposits and releases remain closed until the
current signing set meets the separately configured minimum bridge validator
count and total stake, and that qualifying set has signed a post-activation
certificate. The stale-lineage timer begins with the first accepted
certificate, applies to current-epoch certificates as well as rotations, and
closes bridge readiness when it expires.

The verifier may be deployed after the public snapshot is known; no Ethereum
transaction is required at the exact B3 `M` block. Before any real deployment,
the canonical snapshot/root, threshold values, runtimes, bridge height, token,
AssetId, migration plan, and audit evidence must all be reviewed and pinned.
The BLS hash-to-curve/pairing path still requires real target-fork vectors. The
withdrawal leaf's literal fields encode to 128 bytes
(`8+8+32+20+20+32+8`); the older “164-byte” label was arithmetic, not padding.

The scaffold is intentionally **not** a migration implementation. The existing
bUSD AssetId commits the managed vault address, while a new staker vault has a
different address; deposits to the new vault therefore cannot mint the existing
bUSD. Directly moving old reserves also does not initialize the new vault's
`locked` liabilities. Before deployment, the protocol needs an explicit old
registry cutoff plus either a burn/swap/reissue migration or a versioned
asset/registry design, including reserve and withdrawal-id genesis. B3 must
also gate `BRIDGE_BURN` on live bridge-qualified finality so users cannot burn
while Ethereum is closed or stale. Finally, a managed-to-decentralized mode
change must be height-versioned so restart/reindex never reinterprets old
blocks. These are release blockers, not values to guess from a validator list.

## Toolchain (owner/CI)

```
forge build
forge test
forge create contracts/B3DepositVault.sol:B3DepositVault --constructor-args <releaseAuthority> <rescueAuthority>
```

The production evidence must pin the exact Solidity compiler version, EVM and
metadata settings, source commit, both constructor arguments, fetched runtime
bytes, and the byte-for-byte comparison that yields the published runtime
hash. The broad source pragma and an unversioned local Forge install are not a
reproducibility record.

Order of operations per the staged build plan: Sepolia/Holesky with test
tokens first; mainnet minting only after every independently reviewed bridge
proof/readiness pin. FlowMesh A3 does not activate the bridge.

## Managed-v1 withdrawal minimum

Managed v1 does not make an arbitrary authority payment a valid redemption.
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
forge script script/Deploy.s.sol --rpc-url $ETH_RPC --broadcast --interactive

# 3. deposit a tiny amount of real ETH (cast asks for the key interactively)
cast send <VAULT> "depositETH(bytes32)" 0x<32-byte-b3-recipient>     --value 0.001ether --rpc-url $ETH_RPC --interactive

# 4. wait ~15 minutes for Ethereum finality, then hand the tx hash to the
#    verification pipeline -- ALL checks run in the B3 C++ stack:
cd ../contrib/b3bridge
python3 eth_live_test.py /tmp/b3-deposit-proof     --tool ../../build/bin/b3-bridge-ethcheck     --tx 0x<deposit-tx-hash> --vault 0x<VAULT>
```

Success ends with `DEPOSIT PROVEN: id 0, token 0x00..00, amount 1000000000000000 wei,
b3_recipient 0x<yours>` followed by `ALL VERIFIED`.
