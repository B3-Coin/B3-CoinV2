# B3 Hive v1.1.1-beta.1 — Validator Bootstrap Beta

This public prerelease provides downloadable desktop and Linux operator
packages containing the finalized `exportbridgebootstrapidentity` RPC and the
offline `b3-bridge-bootstrap-proof` tool. Its immediate purpose is to collect
the four complete public validator identities required before deploying the
decentralized Ethereum bridge contracts.

This is an unsigned testing prerelease. It does **not** approve an Ethereum
deployment, enable bridge deposits or withdrawals, set the mainnet withdrawal
height, or replace B3 Hive v1.1.0 as the latest stable release. The production
bridge remains fail-closed until the four public exports, deterministic
bootstrap pins, deployment inputs, mined contract addresses and runtime hashes,
Ethereum light-client pins, and final B3 chain parameters have been reviewed.

## Bootstrap operators

Each of the four selected operators should install the package for their
platform, open and unlock the wallet containing their confirmed finality-key
binding, and run this in the Qt debug console:

```text
exportbridgebootstrapidentity
```

The equivalent command-line call is:

```text
b3coin-cli -rpcwallet=<validator-wallet> exportbridgebootstrapidentity
```

The offline `b3-bridge-bootstrap-proof` tool is included in both Linux
packages; operators on other platforms may send their public identity export
to the coordinator who runs that tool.

Return the complete JSON result. It contains public identity and binding proof
data. Never share a wallet file, wallet password, seed phrase, validator private
key, or BLS private key. After exporting, do not call `bindfinalitykey` again
and do not rotate or revoke the binding until the one-time Ethereum verifier
initialization succeeds or the bootstrap manifest is explicitly abandoned.

## Safety and qualification

The underlying transition-hardening candidate passed the complete local node,
wallet, Qt, bridge/finality, relayer, Solidity, and four-node FlowMesh suites.
This prerelease build therefore skips the duplicate long GitHub qualification
job while retaining every platform compilation/package check, checksum, the
full Solidity contract suite, and deployed-contract size gate. The eventual
stable v1.1.1 release still requires the complete stable-release policy.

The detailed transition and bridge changes remain documented in
`doc/release-notes-v1.1.1.md`. Contract deployment must follow
`contracts/DEPLOYMENT.md`; possession of these binaries alone is not deployment
approval.
