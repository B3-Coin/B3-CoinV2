# Arbitrum Sepolia contract settlement review

This is a separate testnet source verifier for Ethereum Sepolia (11155111) and
Arbitrum Sepolia (421614). It cannot enable Arbitrum One or native mainnet.
The existing production verifier continues to return `UNREVIEWED_DEPLOYMENT`.

The native authority starts with `peer::gloas::FinalizedExecution`, minted by
the separate Ethereum Sepolia light client. The raw execution header must hash
to the execution block hash authenticated by the finalized beacon header. The
Arbitrum verifier then checks Ethereum account and storage Merkle proofs under
that header's state root. Provider assertions, JSON roots, transport success,
and this provenance file cannot mint authority.

`pins::Registry()` is the compiled review record. Seventeen account witnesses
are required in two groups, exactly the first 16 and last 1 registry entries.
Each group retains the existing 16-account bound. The aggregate budget stays
32 MiB and 8192 trie nodes, with 64 storage words per account, 64 nodes per witness,
and 8192 bytes per node. The existing `B3ARPR01` format and limits are unchanged.

The registry binds the BoLD rollup proxy, its two implementations and admin,
the governance upgrade executor proxy and implementation, the challenge,
bridge, inbox, outbox and sequencer proxies and implementations, and both
protocol and governance ProxyAdmins. The ProxyAdmin owner checks stop at the
reviewed governance executor address. The records bind the L2 chain ID,
wasm root, challenge and confirmation configuration, rollup backlinks,
pause state, validator whitelist and its five members, and zero AnyTrust fast
confirmer. Changed or upgraded configuration refuses verification until a
new source review and compiled record are supplied.

The current BoLD rollup is
`0x042b2e6c5e99d4c521bd49beed5e99651d9b0cf4`; older documentation tables also
list the superseded `0xd808...` rollup. The maintained Nitro chain configuration
and exact deployed code/layout were used to disambiguate this deployment.

The authenticated rollup `latestConfirmed` must equal the assertion preimage's
native hash. Its assertion mapping entry must have status Confirmed and a
nonzero creation block no newer than the finalized Ethereum block. The
assertion machine status must be FINISHED. The committed L2 block header is
checked against the assertion and cannot be newer than its Ethereum anchor.

The separate `arbitrum_sepolia_availability.h` supplies bounded discovery
helpers. `RequiredWitnessesByHash(candidate)` derives requests before a
preimage is known. `QueryLatestConfirmed(owner, anchor, slot, rollupWitness)`
checks the rollup account, compiled static slots, latest hash and Confirmed
mapping word against the native Ethereum state root. Its private result has
`Hash()`, `CreatedBlock()` and `IsCurrent()`, so the exact creation block can
be requested from execution peers. This one-account query is not the full
seventeen-account settlement check. `DecodeAssertionCreated(log, hash)` checks
the exact event address, three topics, 800-byte static ABI, integer/address
padding and native preimage hash. Decoding does not prove receipt inclusion;
the full `AuthenticateHead` check still binds any candidate to finalized L1
storage before settlement authority is created.

This establishes canonical **contract settlement**. It retains these explicit
assumptions:

- Authorized governance may upgrade or force-confirm the rollup. Governance
  controller role membership and every upstream governance institution are
  outside this verifier's dependency closure.
- At least one authorized validator honestly challenges invalid execution
  through the reviewed configured challenge path.
- Correctness of the configured fraud-prover contracts beyond the pinned
  challenge manager is assumed. Their full code and proxy dependency graph
  is outside this verifier's closure.
- The verifier does not execute the Nitro state transition or independently
  prove the complete fraud-prover graph, preimage availability, batch data
  availability, or all L2 transactions.

`SettledHead` is privately constructed after these checks. `BeginAncestor` and
`WalkBack` extend its hash-authenticated ancestry in bounded 128-header pages.
Each page begins with the current endpoint's parent and checks every hash,
height, and timestamp. An arbitrary older history chunk can then be checked
against its authenticated endpoint without requiring the chunk to end at the
latest confirmed tip. `ConfirmedRange` retains every checked header, complete
receipt, transaction hash, and requested account/storage result. All three
capabilities recheck the exact native owner, policy, journal, sequence and
freshness through `IsCurrent`. Serialized cursors are availability data and
cannot reconstruct these private types without replaying validation.

Primary review sources:

- [Pinned Nitro chain configuration](https://github.com/OffchainLabs/nitro/blob/70fa99a6e711f09eedd96a907290c3df0f3bcf7d/cmd/chaininfo/arbitrum_chain_info.json)
- [Pinned Nitro contracts](https://github.com/OffchainLabs/nitro-contracts/tree/4341b132cfbdcc980ead03765ca5224ff6cb5d97)
- [Rollup core layout](https://github.com/OffchainLabs/nitro-contracts/blob/4341b132cfbdcc980ead03765ca5224ff6cb5d97/src/rollup/RollupCore.sol)
- [Assertion layout](https://github.com/OffchainLabs/nitro-contracts/blob/4341b132cfbdcc980ead03765ca5224ff6cb5d97/src/rollup/Assertion.sol)
- [Verified deployed rollup sources](https://repo.sourcify.dev/contracts/full_match/11155111/0x042b2e6c5e99d4c521bd49beed5e99651d9b0cf4/)
- [Verified governance executor sources](https://etherscan.io/address/0x3d745b8815f9be5bf053858165f8ab1f58c77932#code)
- [Verified governance ProxyAdmin sources](https://sepolia.etherscan.io/address/0x3729b0b3999fb46aa71386d1016e155d55ccd282#code)
- [Official Solidity compiler checksums](https://github.com/ethereum/solc-bin/blob/gh-pages/linux-amd64/list.json)

`provenance.json` records the static account/slot pins, source and compiler
hashes and review assumptions. Public RPC was used only for read-only discovery
of deployed code/config and is not runtime authority. Missing full source
records were independently compiled using checksum-pinned Solidity binaries.
Governance executor and outbox runtime matched byte for byte (including
compiler-declared outbox self-address immutables). Both ProxyAdmin executable
bytecodes matched; only compiler CBOR auxiliary metadata differed, and their
complete actual runtime hashes remain pinned.

The native unit tests use explicitly synthetic trust roots, real Gloas
bootstrap validation and Merkle tries. They cover accepted 17-account proofs,
298-block ancestry pagination, complete deposit/payout receipts, absence
proofs, changed configuration/code, missing/reordered/duplicate witnesses,
invalid assertion status, stale owners, and bounded requests. They establish
implementation behavior; they are not evidence of a live Sepolia deposit or
an independent execution proof. Public testnet acquisition and owned Nitro
end-to-end execution must be reported separately.

## File-driven native checker

`b3-bridge-sepolia-arbitrumcheck` replays an existing Gloas journal in the same
process that checks the Arbitrum proofs. It reads the policy and journal through
the safe native file reader, never creates or appends a journal, bounds replay
to 64 MiB and 8192 records, and checks wall-clock freshness again after history
verification. Windows calls fail closed; use Linux or WSL.

```sh
b3-bridge-sepolia-arbitrumcheck \
  --policy /absolute/sepolia-policy.json --journal /absolute/proofs.journal \
  --execution-header execution-header.rlp \
  --head-proof head-group-0.bin --head-proof-1 head-group-1.bin \
  --ancestor-pages ancestor-pages.json --objects objects \
  --history-proof history-proof.bin \
  --previous-height 100 --previous-hash 0xPREVIOUS_HASH \
  --to-height 110 --to-hash 0xENDPOINT_HASH --vault 0xVAULT_ADDRESS
```

The two head files use the existing `B3ARPR01` envelope. They carry exactly
16 and 1 L1 accounts, identical assertion preimages and the identical settled
L2 header, with empty unrelated receipts, transactions and L2 accounts. The
history file carries a neutral assertion, complete transactions and receipts,
and any requested endpoint account proofs. Ancestry JSON uses
`b3-arbitrum-sepolia-ancestor-pages/1`, a `pages` array, and SHA-256 object
references named `header_sha256`; each referenced file contains the raw parent
header. At most 64 pages of 128 headers and 32 MiB are accepted. Hashes, heights
and parent links are derived from the raw headers.

For the earlier discovery step, supply the same three common inputs with
`--query-latest --rollup-witness rollup-witness.json`. That JSON contains one
`address`, `nodes`, and `storage` array of `{slot,nodes}` objects, in the exact
compiled rollup slot order. It is bounded to 262 KiB. A successful query emits
the native root-bound assertion hash and creation block; full settlement still
requires all seventeen account witnesses.

Success reports `sepolia_contract_settlement_verified=true`, the native
Ethereum policy/journal identity, settlement/range metadata and complete
candidate events/account evidence. `mainnet_enabled`, `arbitrum_authority`,
`execution_verified` and `writes_enabled` remain false. There is no caller
`verified` flag, fake clock or JSON source-root override.

`qualify_cli.py` runs strict process controls with a genuine Gloas journal and
matching peer-fetched execution preimage. Its Arbitrum account/head inputs are
explicit negative controls. Both normal and sanitized builds must reach
`BAD_ACCOUNT` for those inputs and reject changed execution bytes, corrupt or
missing journals, network mismatch, inconsistent head groups, duplicate JSON
and attempted authority/clock flags. It also checks original input bytes are
unchanged. These checks do not establish a live public Arbitrum settlement.

Public end-to-end qualification still needs actual raw L1 account/storage
witnesses for the current native finalized root, the assertion preimage and
L2 ancestry/history. Public peer availability or pruning may refuse those
inputs. The file-driven checker keeps that acquisition boundary explicit;
the owned Nitro fullstack test has a separate trust model and result.
