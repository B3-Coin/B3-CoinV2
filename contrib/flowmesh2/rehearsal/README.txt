Arbitrum real-token custody rehearsal (mainnet disabled)

This directory is deliberately outside contracts/, whose Foundry src="."
would otherwise discover test functions during the ordinary contract suite.
No test here opens an RPC connection or writes to a real chain. The runner
must first validate and own the literal-loopback Anvil chain31337 process,
its explicit Arbitrum finalized origin/hash, and the registered asset pins.

The runner should give only the Forge child these environment overrides:
  FOUNDRY_PROFILE=default
  FOUNDRY_TEST=<absolute repository path>/contrib/flowmesh2/rehearsal

Use the pinned local Forge and solc executables, with this argument list:
  forge test
    --root <repository>/contracts
    --offline
    --use <pinned local solc executable>
    --via-ir
    --match-contract ^RebirthArbitrumForkTest$
    --match-path **/RebirthArbitrumFork.t.sol
    --fork-url <validated owned literal-loopback Anvil RPC URL>
    --fork-block-number <explicit captured local block height>
    --chain-id 31337
    --out <fresh evidence>/contracts-rehearsal-out
    --cache-path <fresh evidence>/contracts-rehearsal-cache
    --json

This is an argument list, not a shell string. Keep the glob/regex as literal
individual arguments. Do not pass the remote archive URL to Forge. Validate
the local block hash immediately before and after the child runs, and retain
the finalized origin, local checkpoint and executable/configuration hashes.
Require exactly the fourteen named test functions in this file to report
success, with zero failures/skips; exit status alone is not a qualification.
Retain the actual resolved Forge child configuration, compiler executable
hash, artifact compiler/version and settings (EVM target, via IR, optimizer),
and checked-in foundry.toml hash. The inspected regtest configuration selects
solc0.8.35, via_ir=true and evm_version="osaka"; this sentence is not evidence
of the settings actually used by a run. An owned Anvil accepting the resulting
bytecode does not establish Arbitrum One opcode/hardfork compatibility. A
future deployment release must separately verify Arbitrum execution rules at
its approved target, choose/review a compatible compiler EVM target, rebuild,
and repeat contract/integration qualification with those exact artifacts.
This rehearsal neither changes that deployment policy nor authorizes a live
build, transaction or deployment.
The parent integration run additionally proves actual local transactions and
the four-operator synthetic-Q protocol path; these Solidity tests simulate
custody/token behavior and do not qualify native chain42161 or mainnet.

Funding/issuer faults are artificial: only local authorized-role minting and
public test-key signatures are used, never overwritten token code/storage.
The source EOA has zero ETH and grants no approval for sponsored deposits.
Native gas is paid by the sponsor. Deposit sponsorship service fee is zero
only in the fixture; production pricing/funding remains unqualified.

The current sponsored-deposit helper accepts only source accounts with empty
code at its exact validated source block. The four-operator fixture uses fixed
public mnemonic indices10008/10009 and checks empty code before funding and
after deposit; it never deletes existing delegation or wallet code. A familiar
test address can already have an EIP7702 delegation on the actual fork. USDT0
can then apply ERC1271 rather than ordinary EOA signature recovery, so proving
possession of the underlying EOA key alone does not qualify that wallet. Both
EIP7702 delegated wallets and ERC1271 contract-wallet/bytes-signature submission
require separate production compatibility and end-to-end qualification. They
remain refused by this helper; this rehearsal does not claim their support.

New offline Python modules (run with repository regtest directory on sys.path):
  test_arbitrum_assets
  test_custody_compliance
The unchanged automatic custody protocol regression module is:
  test_automatic_custody
All of these are standard unittest modules; no RPC or Linux helper is needed.
