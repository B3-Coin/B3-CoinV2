# B3 Hive v1.1.1 — Final Transition Release

This release follows the already-published v1.1.0 tag; v1.1.0 and its artifacts
remain immutable. It pins the finalized decentralized bridge deployment and
ships the complete transition code needed before Modern PoS. The public
deployment record deliberately retains `production_approved: false`: the
owner accepted an expedited launch, but no independent external cryptography
or contract audit has been completed.

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
  start M = 811,001 for this release. It is no longer coupled to FlowMesh A3
  = 815,000. Generic colored assets remain gated at A2 = 813,000.
- Irreversible bridge burns use a second consensus height W. The owner ruling
  requires the complete two-way bridge and sets mainnet `W = 811,001`, equal to
  inbound `B`. This removes an intentional inbound-only interval, but does not
  bypass the code-enforced verifier initialization, validator-set qualification,
  or finality-certificate gates. External audit and rehearsal are separate
  release risks which the code cannot detect. W remains deliberately excluded
  from the stable bridge registry id.
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

The decentralized stack is finalized on Ethereum mainnet at origin block
25,898,729. It represents canonical USDT
`0xdAC17F958D2ee523a2206206994597C13D831ec7` 1:1 in raw six-decimal bUSD units.
The public release record is
`contracts/deployments/ethereum-mainnet-v1.1.1.json`; it retains
`production_approved: false` because a finalized deployment is evidence, not
activation or audit approval. The four public bootstrap identities and their
ownership proofs are in
`contracts/deployments/ethereum-mainnet-bootstrap-members-v1.1.1.json`
(SHA-256
`1af9ed3227213d5d02a6c7b84e7392b2a252091f95f954ec122939fb54cd8de3`);
that public file contains no wallet or BLS private keys.

| Component | Address | Runtime code hash |
|---|---|---|
| `B3StakerBridge` vault | `0x077839b12cebfbF163acAEAC3A59A015D100c64b` | `0xdb267712887568bffd394e46538bddba01da11cefc38e32b2428c00911237f8d` |
| `B3FinalityVerifier` | `0xE72B3Fe73F0d42A6e964D33E7BB1cc2EA7a3F690` | `0xafdba8befb1aacc832bff4e08dcd92e6645a012ea8a8088b0f2811d916022902` |
| `BlsCertificateProver` | `0x8e612aE4D475d25940E2A2FC907F21b6813eedA7` | `0x77d2aea2d2a6842fae8b29e64a146622e2f45e772a6c351640ffe8362211a959` |

The vault's immutable single-deposit ceiling is 10,000 USDT
(`10,000,000,000` raw units). The initial B3 risk envelope is also capped at
10,000 bUSD minted per 1,440-block epoch (nominally one day), with the
per-block ceiling no higher than the same amount. Inbound height `B` and
outbound height `W` are both 811,001, reflecting the requirement to launch a
complete two-way bridge rather than an inbound-only bridge. Reaching those
heights alone does not make either direction ready.

The release light-client checkpoint is raw Ethereum root
`0xf6744774a1bcfe910c643e447cd09fe8443cc2edc25d9ae65155b3cbbef3b646`
at slot 15,136,512. Its reviewed fork-schedule validity ends at epoch 479,999,
with an operational horizon of 2026-10-04 20:00:23 UTC. Operation must stop or
ship a separately reviewed checkpoint/fork-schedule extension before crossing
that horizon. The captured 54,406-byte bootstrap payload has SHA-256
`8ddc57324951849dd0dbe272540325cb9b2e1d975cbc2c871a35cf83d40f7996`;
the tracked record also binds the capture provenance and normalized bootstrap
data hashes.

The historical managed smoke vault
`0x143F207e23e6aebD7E974be90ac6D434f4c7BFb6` is superseded and must never be
used as this deployment's vault or AssetId. It held zero observed USDT and no
B3 bridge minting activated against it; both facts must be rechecked before
activation. Any discovered liability stops activation and requires an explicit
migration.

The deployed vault rejects deposits until the verifier is initialized and a
fresh certificate proves qualified current and successor validator sets with
enough exit time remaining. Four validator stakes intended for Set0 must each
be included no later than height 810,980, with confirmed non-revoked finality
bindings; the one-time 3-of-4 Ethereum handoff must land before its immutable
2026-09-10 18:52:50 UTC deadline. Both current and successor sets must have
4–64 validators and at least 900 B3 total weight. There is no corridor-deposit
or pre-readiness custody phase. Once those gates and B3's separate activation
pin are satisfied, the Linux relayer proves finalized Ethereum headers,
receipt roots, and the exact vault event through B3's Ethereum light client.
With `B = W = 811,001`, deposits, B3 burns, and Ethereum releases form one
two-way launch envelope. Each operation still fails closed until its verifier,
qualified-current-and-successor-set, certificate, proof, and release-policy
requirements are actually satisfied.

The withdrawal machinery is implemented and scheduled by `W = 811,001`, but a
height pin is not a readiness claim. `bridgewithdraw` burns exact bUSD and
commits the Ethereum recipient without `OP_RETURN`. `getbridgefinalityproof`
builds the B3 certificate proof and `submitCertificate` calldata;
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
fixture. No independent external cryptography or contract audit has been
completed, and the end-to-end production procedure has not been rehearsed.
Those are unresolved operator/release risks, not on-chain conditions: neither
contract can detect an audit report or rehearsal. Technically, the contracts
open once the verifier is initialized and a fresh accepted certificate proves
qualified current and successor sets; releases additionally require the exact
withdrawal proof. `production_approved: false` therefore records an explicit
release-policy stop, not an extra contract-enforced predicate.

AssetId has two deliberate hex presentations. Wallet asset lookup uses B3's
conventional reverse `uint256` display; Ethereum manifests/calldata use raw EVM
`bytes32` order. RPC output labels the EVM form explicitly, and a shared
C++/Solidity vector pins both forms.

## P2P compatibility

- Post-boundary B3 peers advertise protocol `80009`, the monotone successor to
  historical B3 protocol `80008`. Pre-boundary connections and explicit
  historical-peer replies remain exactly `80008`. The new number is a B3 wire
  identity only: Core feature negotiation remains capped at `70016`, and
  non-B3 networks continue to advertise `70016`.

## Wallet, GUI, and operator packages

- The default `sendrawtransaction`/`testmempoolaccept` fee-rate guard now uses
  B3's modern display unit. The inherited legacy-unit value was 1,000 times
  smaller than its documented 0.1 B3/kvB limit and could reject a correctly
  signed wallet-built finality-key transaction during manual rebroadcast.
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
  relayer/operating guide. The static archive remains headless. This release
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
