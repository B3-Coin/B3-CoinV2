# B3 Hive v1.1.1 — Transition Hardening Candidate

These notes describe the candidate built after the already-published v1.1.0
tag. v1.1.0 and its artifacts remain immutable. Do not tag or publish v1.1.1
until the owner approves the exact candidate, the production bridge
manifest/pins are complete, and the candidate passes the release runbook.

## Consensus and upgrade safety

- Restarts now rebuild the block index's existing ancestry shortcuts before
  applying the fixed transition anchors. This removes a quadratic startup loop
  which could leave a fully synced wallet at `Loading block index` for an
  impractical time. It does not change the checkpoint, accepted chain, or disk
  format.
- The block-810,001 checkpoint is verified independently at startup even when
  an older transition validation marker exists, closing an inherited-database
  path that could otherwise skip the new checkpoint check.
- Inbound bridge activation is an independent height B, intended as Modern-PoS
  start M = 811,001 for this candidate. It is no longer coupled to FlowMesh A3
  = 815,000. Generic colored assets remain gated at A2 = 813,000.
- Irreversible bridge burns use a second consensus height W. Mainnet W remains
  unset in this candidate: Ethereum light-client maintenance and verified
  deposits may operate after B, while `bridgewithdraw` fails closed until a
  later reviewed release pins W at or after B. W is deliberately excluded from
  the stable bridge registry id.
- B3 finality certificates now require a greater-than-two-thirds validator
  headcount as well as the existing greater-than-two-thirds stake weight. This
  exactly matches the Ethereum prover and prevents one very large staker from
  certifying alone after the validator set grows.
- Finality votes are written to an atomic, checksummed signer journal before
  relay. Restart, corruption, a deleted live journal, or a competing fork now
  stop signing safely instead of allowing an accidental conflicting vote;
  genuine newcomers can establish an empty journal before entering a future
  validator set.
- Finality gossip now discards obsolete branch slots after a permitted reorg,
  prunes finalized work on every submission, and replaces its oldest bounded
  slot with a newer verified checkpoint. Per-peer and whole-node limits also
  bound costly BLS verification without treating honest late signatures as
  peer misbehavior.
- Before a decentralized bUSD burn is accepted, both projected current and
  successor B3 validator sets must satisfy the pinned validator-count floor,
  64-member gas-benchmarked ceiling, minimum total weight, and intact finality
  lineage.

## Decentralized bUSD bridge (production gated)

The current production target represents Ethereum-mainnet USDT
`0xdAC17F958D2ee523a2206206994597C13D831ec7` 1:1 in raw six-decimal bUSD units
through a new immutable `B3StakerBridge` keyless vault and
`B3FinalityVerifier`. The historical managed smoke vault is not this target.
It held zero observed USDT and no B3 bridge minting activated against it; both
facts must be rechecked before replacement. Any discovered liability stops
activation and requires an explicit migration.

The contracts may be deployed before B, but the new vault rejects deposits
until the verifier is initialized and a fresh certificate proves qualified
current and successor validator sets with enough exit time remaining. There is
no corridor-deposit or pre-readiness custody phase. Once those gates and B3's
separate activation pin are satisfied, the Linux relayer proves finalized
Ethereum headers, receipt roots, and the exact vault event through B3's
Ethereum light client. If inbound B is enabled while W remains unset, users can
deposit and receive bUSD but cannot yet burn it for Ethereum release; this is
an explicit custodial waiting period and must be disclosed before deposits are
offered.

The withdrawal machinery is implemented, but production B3 burns and Ethereum
releases remain disabled by the unset W pin until certificate canonicality and
round-trip liveness are solved, audited, and explicitly enabled. When W is
pinned,
`bridgewithdraw` burns exact bUSD and commits the Ethereum
recipient without `OP_RETURN`. `getbridgefinalityproof` builds the B3
certificate proof and `submitCertificate` calldata;
`getbridgewithdrawalproof <confirmed_txid> <burn_vout> [certificate_block]`
resolves that confirmed active-chain burn to its consensus-assigned withdrawal
id, builds the finalized depth-32 leaf path, and emits `release` calldata. The
wallet does not report a provisional id or leaf before confirmation. Any funded
Ethereum account may submit these permissionless calls and pays Ethereum gas.
B3 does not ask for or store an Ethereum private key, and the vault rejects a
duplicate withdrawal id.

The verifier uses a time-bounded one-time 3-of-4 bootstrap statement to install
canonical Set0, then accepts only normal B3 finality and signed set handovers.
Its 64-validator worst case is covered by a real EIP-2537 proof/calldata/gas
fixture. Production remains fail-closed until the mined deployment manifest,
runtime hashes, vault/AssetId, Ethereum checkpoint/forks, caps, approval
interval, adapter/rules commitments, bootstrap handoff, audits, and rehearsal
are all reviewed and pinned.

AssetId has two deliberate hex presentations. Wallet asset lookup uses B3's
conventional reverse `uint256` display; Ethereum manifests/calldata use raw EVM
`bytes32` order. RPC output labels the EVM form explicitly, and a shared
C++/Solidity vector pins both forms.

## Wallet, GUI, and operator packages

- Finality-key binding and revocation now stop cleanly when their 32-bit
  sequence is exhausted instead of wrapping, and the staking status reports
  the actual last handled signing height even when bounded old gossip is not
  relayed.
- Active validators must preserve the network datadir's `finality_signer`
  journal with their wallet and must never run the same validator/BLS key from
  two independent machines or datadirs. The local journal cannot coordinate
  cloned keys.
- A wallet-owned configured bridge asset is shown as `bUSD` with six decimals
  in Qt and by `getwalletassets`. Metadata recognition does not claim bridge
  activation.
- `transferasset` and `burnasset` now follow the bridge's independent B gate
  for the exact configured bUSD AssetId, so bridged funds can move between B
  and the later generic colored-asset activation at A2.
- Both Linux archives include `b3-bridge-ethcheck`,
  `b3-bridge-bootstrap-proof`, and the dependency-free POSIX Python deposit
  relayer/operating guide. The static archive remains headless. This candidate
  packages the bridge operator bundle only on Linux; Windows and macOS remain
  wallet/node packages and do not claim those operator tools.
- Release CI runs the full Solidity suite and an EIP-170 gate over only the
  three contracts actually deployed. The oversized local Foundry deployment
  script is not incorrectly treated as an Ethereum runtime.
- Extended GitHub release qualification remains the default. After the exact
  tag has completed the full local qualification, an administrator may use the
  explicit, visibly warned `B3_SKIP_EXTENDED_RELEASE_TESTS_SHA=<exact commit>`
  repository variable for a packaging-only tag run. The exception cannot
  carry over to a later commit; every platform build and checksum remains
  mandatory.
