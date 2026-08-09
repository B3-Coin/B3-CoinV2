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

## Decoder selection is NOT consensus-era selection

The era selector cannot pick a wire decoder: the height of a
not-yet-decoded blob is unknown until after decoding. These are two distinct
problems:

1. **Consensus-era selection** (`GetB3Era`) — given a *connected* block's
   height, which rules govern it.
2. **Format selection** — given raw bytes, which decoder parses them. Today
   this is resolved **per chain**, not per height: on a legacy-B3Coin chain
   the unified serialization in `primitives/` tolerates the legacy fields
   (transaction `nTime`, trailing PoS block signature) at every height. If
   the modern format ever diverges incompatibly, selection must come from
   *transport context* — which protocol/peer delivered the bytes, or which
   storage era they were read from — never from consensus height.

Skeleton interfaces for the eventual split live in
[`src/legacy/codec.h`](../../src/legacy/codec.h).

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
