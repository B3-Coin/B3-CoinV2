# B3 Era Architecture

Status: architectural spike, 2026-08. Describes the era boundary of the
continuous B3 chain and the module boundaries introduced ahead of it.

## One chain, two eras

B3 remains one continuous chain with the same genesis and history. A final
legacy boundary will eventually be pinned as

    LEGACY_FINAL_HEIGHT = H
    LEGACY_FINAL_HASH   = X

- **Legacy era, height <= H** — historical B3Coin blocks. Downloaded with
  legacy-compatible networking, decoded from the legacy formats, and (once
  the boundary is pinned) *mechanically replayed*: hash linkage, checkpoints
  and Merkle/data integrity are checked; transaction IDs, outpoints, values
  and scriptPubKeys are preserved; the UTXO set is reconstructed with
  crash-safe forward replay. Old PoW, the PoS kernel, stake modifiers,
  rewards, difficulty, timestamps, chainwork and historical script/signature
  validation are all *skipped* — the pinned hash X attests the prefix.
- **Modern era, height > H** — begins with the block at H + 1 referencing X.
  New PoS and full validation, clean modern protocol, and a permanent
  prohibition on any reorganization crossing H.

There is no new genesis and no UTXO snapshot migration.

## Era selection: the single source of truth

`Consensus::GetB3Era(int height, const Consensus::Params&)` in
[`src/consensus/era.h`](../../src/consensus/era.h) is the only era selector:

- `Params::hard_fork_height` stores the **first modern height**, i.e. H + 1.
  While unset, every height of a legacy-B3Coin chain is `B3Era::LEGACY`.
- Chains without a legacy B3Coin history (`legacy_b3coin == false`; Bitcoin
  test chains, plain regtest) are `B3Era::MODERN` at every height.
- `legacy::IsActive()` and `Consensus::IsHardForkActive()` are thin wrappers
  kept for compatibility; new code calls `GetB3Era()` directly.

`GetB3Era()` is only valid where the height is already unambiguous: a
connected `CBlockIndex`, or a height derived from a known parent. Callers in
`validation.cpp`, `net_processing.cpp`, `node/blockstorage.cpp` and
`primitives/block.cpp` all satisfy this.

## Decoder selection: a header marker plus an authoritative height

A block header is fixed at 80 bytes, so a node can parse its `nVersion` before
it parses the transaction body. B3 permanently reserves VersionBits bit 27
(`0x08000000`) as the modern block-body codec marker. A modern B3 block must
have the normal BIP9 top pattern plus that bit: `0x28000000`, serialized as
the little-endian bytes `00 00 00 28`. Lower versionbits remain available for
later deployments; bit 28 remains Core's testdummy bit.

This gives two separate, mutually checked decisions:

1. **Raw format selection** — the marker chooses the legacy body codec
   (transaction `nTime` plus trailing PoS block signature) or the exact Core
   body codec (no transaction `nTime`, no trailing signature).
2. **Consensus-era selection** (`GetB3Era`) — after the previous block is
   known, its height determines the only valid codec. A marker/height mismatch
   is invalid. The marker is not a miner signal and cannot override height.

This makes header-first parsing possible even for an unknown-parent block;
when the parent arrives, the height check confirms the preliminary codec and
hash choice. Standalone `tx` messages have no header, so they must instead be
accepted only in the active transaction era; the mempool is flushed at the
boundary and on a prohibited cross-boundary reorg.

The current `src/legacy/codec.h` interfaces remain a skeleton. Before a fork
height is configured, the implementation must restore stock Core primitives
and make the legacy codec explicit; merely adding the marker does not make
global `CTransaction` serialization height-aware.

## What stays global today, and what can move behind src/legacy

**Remains global (network-scoped, height-independent).** These uses of
`Params::legacy_b3coin` describe the whole network, not an era, and stay at
their call sites:

- P2P identity and capability pinning: legacy protocol version 80008 /
  compatibility cap 70011, headers-first sync disabled, witness service bits
  and v2 transport disabled (`net_processing.cpp`, `init.cpp`).
- Hash selection for lookups on a legacy chain (`index/txindex.cpp`,
  `index/txospenderindex.cpp`, `rpc/txoutproof.cpp`).
- Header PoW exemptions for hybrid PoW/PoS headers
  (`node/blockstorage.cpp`, `validation.cpp`).

**Can later move behind src/legacy.** The `use_legacy_b3coin` branches in
`ConnectBlock`/`DisconnectBlock` (`validation.cpp`), the legacy structural
block checks, kernel/stake-modifier/reward validation (`src/legacy/pos.cpp`,
`src/legacy/consensus.cpp` — already there), and the legacy serial sync
state machine (`net_processing.cpp`). Once (H, X) is pinned, the whole
legacy validation path collapses into
[`src/legacy/replay.h`](../../src/legacy/replay.h)'s `TrustedReplay`:
linkage + integrity checks + mechanical UTXO application, crash-safe
resumable, no rule validation.

## Modern validation and future Policy Outputs

[`src/modern/validation.h`](../../src/modern/validation.h) sketches the
modern-era boundary: context-free checks, contextual checks against the
parent, and `CanReorgTo()` — the permanent prohibition on rewinding across H.

Future consensus features (coloured assets, DEX custody) attach through
**typed Policy Outputs** validated by **generic transition proofs** carried
in the spending context:

- A policy output commits to a policy type and its parameters; spending it
  requires a proof that the state transition is valid for that policy.
- DEX fills are not UTXO spends. FlowMesh maintains internal balances,
  reservations, positions, persistent demand curves, matching and
  settlement; UTXOs are touched only when assets enter or leave DEX custody.
- A future `DEX_VAULT` policy needs: finalized withdrawal transition proofs,
  partial withdrawal, forced change back to the approved vault policy, exact
  per-asset conservation, one-time withdrawal receipts, permissionless
  relay, no DEX private key, and sharded vault outputs.

The accommodation is structural: proofs and policies live in script/witness
context and dedicated validators — **never as new fields on `CTxIn` or
`CTxOut`** — so `TrustedReplay` and txid/outpoint stability are unaffected.
None of this is implemented in the spike.

## Legacy sync pipelining

Legacy peers cannot serve headers-first sync (a hybrid header proves
neither PoW nor a stake kernel), so sync is getblocks/inv/getdata over full
blocks. The downloader keeps a window (`LEGACY_BLOCK_DOWNLOAD_WINDOW`) of
getdata requests in flight per peer instead of one block per round-trip;
queue entries are only popped when their block is accepted, so a timed-out
request is retried exactly and the batch survives (old peers never
re-announce inventory on the same connection). Message formats are
unchanged.

## Prototype status

- `era.h` semantics, the call-site migration, and the sync window are live
  behavior (consensus outcomes unchanged).
- `legacy/codec.h`, `legacy/replay.h`, `modern/validation.h` are
  compile-checked interface skeletons with no implementations or callers.
- No real (H, X) is configured; mainnet parameters are untouched.
