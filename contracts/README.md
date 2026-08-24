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
