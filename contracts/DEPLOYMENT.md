# Decentralized bridge deployment

`script/Deploy.s.sol` now deploys the immutable production-shaped stack in the
only valid dependency order:

1. `BlsCertificateProver`
2. `B3FinalityVerifier`, pinned to the prover runtime hash
3. `B3StakerBridge`, pinned to the verifier runtime hash and one origin token

It does not deploy the historical managed vault. That old helper remains under
the deliberately explicit name `script/DeployManagedVault.s.sol`.

This deployment procedure is intentionally USDT-only: origin and B3 asset
decimals are both six, and the vault moves raw units one-for-one. The verifier
can later be reused by another separate six-decimal ERC-20 vault, but this
script deploys only the first vault and the current B3 release exposes only the
single `busd_bridge` registry entry. Do not use this V1 bytecode for WETH or
wrapped BTC. Those assets require a later converted-vault contract plus a
separate B3 registry release; deploying a contract alone does not activate an
asset or make its deposits mintable.

No live deployer/contract address, key, validator root, cap, deadline, or
deployment coordinate is supplied by the repository. The example carries a
few already-ruled candidate schedule/ceiling values, but every value below must
still be compared with the reviewed release record.
Do not put a private key in this environment file; use Foundry's normal hardware
wallet, keystore, or interactive signer support.
Copy `.env.example` separately as `.env.testnet` and `.env.mainnet`; never reuse
one network's token, chain id, candidate manifest, or mined coordinates for the
other.

`BRIDGE_ACTIVATION_HEIGHT` is independent of FlowMesh A3. It is not selected by
deploying the contracts and must not be inferred as M. A later reviewed B3
build may pin it only after the final mined manifest and every other bridge
gate are complete. It is inbound height `B`; irreversible B3 burns use a
separate B3-only height `W` that is not a contract constructor input. The
script deliberately does not decide either release gate.

`foundry.toml` pins Solidity 0.8.35, post-Fusaka Osaka bytecode, IR compilation, optimizer
settings, and metadata mode. Preserve the matching Foundry version, source
commit, build-info output, and broadcast record with the release evidence.

## Required deployment inputs

```text
EXPECTED_DEPLOYER
EXPECTED_CHAIN_ID
B3_CHAIN_DOMAIN
ORIGIN_TOKEN
EXPECTED_ORIGIN_TOKEN_CODE_HASH
MAX_DEPOSIT_RAW
B3_MAX_PER_BLOCK_RAW
BOOTSTRAP_AGGREGATE_PUBKEY
BOOTSTRAP_MEMBERS_ROOT
EXPECTED_BOOTSTRAP_SET_HASH
BOOTSTRAP_MEMBERS_MANIFEST_SHA256
SOURCE_BUILD_PROVENANCE_SHA256
MODERN_START_HEIGHT
BRIDGE_ACTIVATION_HEIGHT
MIN_BRIDGE_VALIDATORS
MAX_BRIDGE_VALIDATORS
MIN_BRIDGE_TOTAL_WEIGHT
MIN_EPOCH_DURATION_SECONDS
MAX_EPOCH_LAG_SECONDS
MAX_CERTIFICATE_AGE_SECONDS
MIN_DEPOSIT_EXIT_WINDOW_SECONDS
BOOTSTRAP_DEADLINE_UNIX
CANDIDATE_MANIFEST_PATH
```

The bootstrap public key is the reviewed 48-byte aggregate for the four
synthetic equal-weight bootstrap members. The set hash must be the independently
computed hash of that exact header. Generate all three bootstrap values from
the four preserved public identity exports with:

```sh
build/bin/b3-bridge-bootstrap-proof bootstrap-manifest.json \
  > bootstrap-deployment-pins.json
```

Use only the three exact values under `deployment_env`; the tool validates all
four proofs of possession and BIP340 binding signatures and deterministically
sorts the committee. Because it is offline, separately verify each reported
binding transaction is confirmed and current on B3 before deploying. The
four operators must not rotate or revoke these immutable manifest bindings
until the verifier's one-time initialization succeeds; otherwise rebuild and
review the deployment pins before deployment. The
script rejects a mismatch between the supplied header and set hash. Set
`BOOTSTRAP_MEMBERS_MANIFEST_SHA256` to the SHA-256 of the exact unmodified
`bootstrap-manifest.json` bytes and preserve/publish that file. The deployment
manifest commits this digest without copying any private material. It also
rejects the wrong Ethereum chain, any noncanonical token address when chain ID
is Ethereum mainnet, an origin-token runtime mismatch, any token whose
`decimals()` is not six, invalid height/threshold relationships, and a
bootstrap deadline that is expired or beyond the weak-subjectivity window.
`MAX_DEPOSIT_RAW` is the immutable maximum actual token balance delta accepted
by one deposit, in raw six-decimal token units. `B3_MAX_PER_BLOCK_RAW` is the
reviewed B3 chainparams value for the same release. The deployment fails closed
unless both are nonzero, `MAX_DEPOSIT_RAW <= B3_MAX_PER_BLOCK_RAW`, and neither
exceeds B3 consensus `MAX_MONEY` (`662200000000000000` raw six-decimal units);
do not guess either value. This prevents Ethereum from accepting one custody
event whose amount B3 can never represent or mint.

Create and independently review a source/build provenance manifest before
deployment. It must identify the exact source commit/tree, preserved source
changes if any, Foundry and solc versions, compiler settings, and build-info
evidence used for the candidate. Set `SOURCE_BUILD_PROVENANCE_SHA256` to the
SHA-256 of that exact file's bytes and preserve/publish the file. The digest is
part of the deployment commitment; changing it changes the commitment. A bare
hash without the corresponding reviewed provenance file is not approval.

`B3_CHAIN_DOMAIN` must be the `chain_domain` returned by `getfinalityset`, which
is already labeled and encoded in Ethereum/wire byte order. Do not replace it
with an ordinary reverse-display B3 `uint256` string. Likewise,
`EXPECTED_B3_ASSET_ID` is the vault's raw EVM `bytes32`; compare it with the
explicit `asset_id_evm` RPC field. The wallet-facing `asset_id` spelling is
intentionally reversed for normal B3 asset lookup. Shared C++/Solidity vectors
fail if either form is substituted for the other.

`MAX_BRIDGE_VALIDATORS` is also an immutable release-authority ceiling, not a
B3 consensus validator limit. The reviewed V1 ceiling is 64: the post-Fusaka
Osaka test
fixture proves the worst admitted headcount case (43 signers and 21 absent
paths) in 18,144 proof bytes / 18,724 transaction calldata bytes, with a
5,513,351-gas complete successful verifier call and a 6,283,311-gas conservative
transaction bound below EIP-7825's 16,777,216 transaction cap.
A larger current set or known successor stays valid B3 lineage state but closes
`bridgeReady`; deposits remain disabled while lineage recovery is in progress.
The vault also rejects new deposits after
an uninitialized bootstrap deadline or irreversible initialized-lineage expiry.
Raising the ceiling requires a new benchmark, review, and deployment.

`MIN_EPOCH_DURATION_SECONDS` must equal the reviewed minimum wall-clock duration
of one B3 epoch (86,400 seconds for 1,440 one-minute blocks in this release).
Initialization records `GENESIS_TIME`; epoch `e` is accepted only between
`GENESIS_TIME + e * MIN_EPOCH_DURATION_SECONDS` and `GENESIS_TIME + (e + 1) *
MAX_EPOCH_LAG_SECONDS`; accepted handovers must additionally be separated by
at least `MIN_EPOCH_DURATION_SECONDS`. The relative rule is necessary because
absolute lower bounds already in the past do not themselves stop batch
epoch-walking. Catch-up after missed relays therefore takes one real interval
per rotation. The contract freezes fail-safe if the genuine chain misses its
maximum wall-clock window. These bounds do not remove the normal PoS
weak-subjectivity assumption if compromised historical keys maintain a fake
lineage in real time.

`CANDIDATE_MANIFEST_PATH` must be inside `contracts/deployments/`, the only
directory for which the Foundry profile grants write access.

## Dry run, review, then broadcast

From `contracts/`:

```sh
mkdir -p deployments
forge script script/Deploy.s.sol:Deploy --rpc-url "$ETH_RPC_URL"
```

Review the complete trace and the candidate JSON. The JSON intentionally says:

```json
{
  "deployment_inputs_complete": false,
  "chainparams_ready": false,
  "origin_deployment_block": 0,
  "production_approved": false
}
```

It also carries `deployment_config_hash`, a domain-separated commitment to
every deployment config field, every deployed artifact address/runtime hash,
the derived AssetId, the bootstrap-manifest digest, and the source/build
provenance digest. A reviewer independent from the finalizer operator must
compare the candidate with the preserved build and bootstrap evidence and
approve that exact hash. Do not calculate a replacement hash from finalizer
inputs.

Zero is not a guessed block. It is an explicit stop marker because Foundry
simulates the transactions before Ethereum mines them. If the dry run and
candidate are approved, broadcast the same inputs with the approved signer:

```sh
forge script script/Deploy.s.sol:Deploy \
  --rpc-url "$ETH_RPC_URL" --broadcast
```

Save `broadcast/Deploy.s.sol/<chain-id>/run-latest.json`. It is transaction
evidence, not the B3 chainparams manifest.

## Finalize the mined coordinates

Read the vault-creation transaction receipt and copy its exact `blockNumber`.
Copy the three addresses, three runtime hashes, and AssetId from the reviewed
candidate into these additional variables:

```text
ETH_RPC_URL
PROVER_ADDRESS
EXPECTED_PROVER_RUNTIME_CODE_HASH
VERIFIER_ADDRESS
EXPECTED_VERIFIER_RUNTIME_CODE_HASH
VAULT_ADDRESS
EXPECTED_VAULT_RUNTIME_CODE_HASH
EXPECTED_B3_ASSET_ID
EXPECTED_DEPLOYMENT_CONFIG_HASH
ORIGIN_DEPLOYMENT_BLOCK
DEPLOYMENT_CONFIRMATIONS_REQUIRED
FINAL_MANIFEST_PATH
```

Copy `deployment_config_hash` from the independently approved candidate into
`EXPECTED_DEPLOYMENT_CONFIG_HASH`. Keep all original deployment inputs in the
environment too. Then run the
read-only finalizer without `--broadcast`. The RPC must provide historical
state for the declared block and its predecessor:

```sh
forge script script/FinalizeDeployment.s.sol:FinalizeDeployment \
  --rpc-url "$ETH_RPC_URL"
```

The finalizer fails unless:

- the vault address had no code at `ORIGIN_DEPLOYMENT_BLOCK - 1`;
- the exact vault first appears at `ORIGIN_DEPLOYMENT_BLOCK`;
- every config/artifact field recomputes the independently approved candidate
  `EXPECTED_DEPLOYMENT_CONFIG_HASH`;
- every runtime hash and every readable constructor/security pin matches at
  that block and at the RPC's latest block; and
- the requested confirmation depth has elapsed.

Only then does it write a manifest with `deployment_inputs_complete: true`.
`chainparams_ready` and `production_approved` deliberately remain false: a
correct deployment is not a cryptography audit, contract audit, light-client
checkpoint, mint-cap ruling, or activation approval.

The deployment is deliberately two-stage. First deploy the exact audited
verifier and USDT vault and collect the Ethereum chain ID, verifier/vault
addresses, their runtime code hashes, the vault deployment block, canonical
USDT token address, and derived B3 AssetId. Only a later B3 build may pin that
complete reviewed tuple. Missing or mismatching fields keep the bridge closed.
The vault may exist before M, but deposits remain disabled until verifier
initialization and a fresh valid certificate prove that both the current and
successor sets satisfy the pinned bridge qualifications. Contract deployment,
initialization, and pinning do not enable Ethereum withdrawals or B3 burns;
those remain disabled until the canonicality and liveness safety of the full
round trip has been solved, audited, rehearsed, and explicitly enabled by
pinning the separate B3 outbound height `W`. If inbound `B` is enabled first,
deposits can mint bUSD while withdrawals remain unavailable.

## B3 chainparams mapping

The final manifest directly supplies these reviewed facts:

| Manifest field | B3 use |
|---|---|
| `solc_version` and compiler-setting fields | reproducible runtime evidence |
| `ethereum_chain_id` | bridge asset origin chain |
| `origin_deployment_block` | first allowed vault log block / relayer start |
| `vault` | bridge asset vault address |
| `origin_token` | bridge asset token address |
| `origin_token_runtime_code_hash` | `BridgeAssetParams::implementation_or_adapter` for direct-token adapter version 1 |
| `origin_token_decimals`, `asset_decimals` | exact raw-unit conversion |
| `max_deposit_raw` | vault cap; B3 `max_per_block` must be at least this value |
| `b3_max_per_block_raw` | reviewed B3 mint cap used to validate `max_deposit_raw` |
| `b3_asset_id` | cross-language AssetId check |
| `deployment_config_hash` | independently approved exact Deploy-to-Finalize commitment; Finalize must recompute it |
| `bootstrap_members_manifest_sha256` | exact public four-member bootstrap input manifest evidence |
| `source_build_provenance_sha256` | exact reviewed source/build provenance evidence |
| `vault_runtime_code_hash` | exact `BridgeAssetParams::vault_runtime_code_hash` registry pin for the reviewed immutable vault implementation |
| `verifier`, `verifier_runtime_code_hash` | decentralized withdrawal pins |
| `bootstrap_set_hash` | `decentralized_withdrawal.bootstrap_validator_set_hash`; never unknown pre-M Set_0 |
| threshold/timing fields | verifier security pins; map the immutable committee floor/cap to B3 `min_bridge_validators` / `max_bridge_validators`, and express `min_bridge_total_weight` in whole modern B3 |
| `deposit_event_topic0` | deposit adapter/event commitment input |

The manifest does **not** invent the remaining B3 consensus inputs: Ethereum
light-client checkpoint/fork schedule, inbound B3 activation height `B`,
outbound burn height `W`, mint caps, adapter version, withdrawal-rules
commitment, or minimum bridge validator weight expressed in whole modern B3.
The deployment-time `bootstrap_set_hash` is the chainparams
trust root; canonical Set_0 does not exist yet and is deliberately installed
later by the verifier's one-time 3-of-4 handoff. This avoids requiring another
B3 binary after M merely to learn Set_0. Mainnet must remain fail-closed until
the inputs above are separately reviewed and pinned in the later B3 build.
There is no corridor-deposit phase.

After the later build pins the tuple, `getbridgeinfo` exposes
`vault_runtime_code_hash` alongside the vault/token registry identity. Operators
must compare it byte-for-byte with the final manifest. The automatic relayer
then verifies the vault, verifier, and direct-token runtime hashes at both the
pinned deployment block and `latest` through every configured execution
provider before changing durable state; absence or mismatch is a hard stop.
