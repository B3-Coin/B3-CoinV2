# B3 bridge contracts (Ethereum side)

Authored in-repo; **compiled and deployed by CI / the owner** — the B3 build
never depends on a Solidity toolchain, and none of this is reachable from
B3 consensus.

## Contents

| Contract | Leg | Status |
|---|---|---|
| `B3DepositVault.sol` | ETH -> B3 deposits (owner ruling 2026-08-24: this leg first) | source complete; compile/test/deploy pending toolchain |
| `B3FinalityVerifier.sol` + `BlsCertificateProver.sol` + release wiring | B3 -> ETH withdrawals | specified normatively in `doc/design/b3-cross-chain-finality-v1.md` §5–§7; to be authored when the release leg starts |

## Invariants

- The `Deposit` event shape is consensus-relevant on the B3 side and must
  byte-match `src/bridge/deposit.h` (`Deposit(uint64,address,uint256,bytes32)`,
  id and token indexed, amount = received balance delta).
- The vault has no owner, no pause, no upgrade path. Funds leave only via
  `release`, restricted to the release authority (the §5 verifier stack).
- Mainnet deployment waits for the release-leg contracts: the constructor
  refuses a zero release authority so deposits can never be trapped.

## Toolchain (owner/CI)

```
forge build
forge test
forge create contracts/B3DepositVault.sol:B3DepositVault --constructor-args <releaseAuthority>
```

Order of operations per the staged build plan: Sepolia/Holesky with test
tokens first; mainnet only after the four release gates analogue for A3.

## Mainnet smoke-test runbook (owner-executed)

The assistant cannot sign transactions or handle keys; every step below runs
on the owner's machine with the owner's wallet. Recommended: tiny amounts,
and an owner-controlled `releaseAuthority` (interim, funds recoverable).

```
# one-time
curl -L https://foundry.paradigm.xyz | bash && foundryup
cd contracts && forge install foundry-rs/forge-std --no-git

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
