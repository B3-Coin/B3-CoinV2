// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_FINALITY_FRESH_KEY_INDEX_H
#define B3COIN_NODE_FINALITY_FRESH_KEY_INDEX_H

#include <consensus/amount.h>
#include <consensus/params.h>
#include <modern/finality_fresh_key.h>
#include <modern/finality_key.h>
#include <node/finality_binding_index.h>
#include <node/finality_fresh_lineage.h>
#include <node/stake_registry.h>
#include <node/validator_set.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class CBlockIndex;
class CChain;

namespace node {

class BlockManager;

/**
 * B-37 fresh key bindings: derived node state (b3-bridge-v2-finality-
 * dependency.md section 6.4, rules (1)-(3); architecture B-37).
 *
 * Consensus judges a fresh binding (FINALITY_KEY policy version 2 + MPA type
 * 12, modern/finality_fresh_key.h) on structure and cryptography only. What a
 * binding is WORTH is decided here, from the connected chain, so that no
 * in-block conflict can ever invalidate a block:
 *
 *   ever bound  every nonzero BLS key ever claimed on the chain, under
 *               either kind: a V1 FINALITY_KEY binding (including a key the
 *               validator later moved off or revoked) or a fresh binding.
 *               The FIRST claim wins, in block / transaction order and,
 *               inside one transaction, V1 pairs (MatchFinalityKeyPairs
 *               order) before fresh pairs (MatchFreshFinalityKeyPairs
 *               order).
 *   bindings    (validator_key, target_epoch) -> every fresh binding that
 *               won its key, in chain order, with its including height.
 *               Seat rules (the windows, the double-binding void, the
 *               carry-over) read this list; they live with the committee
 *               rule, not in the index: FreshSeatFor and
 *               BuildFreshCommittee below, applied by
 *               node::ProjectFreshLineage through the tracker.
 *   inert       a fresh claim of a key that is already bound is recorded
 *               nowhere: it can never seat its validator, so a V1 key (or a
 *               key bound for an earlier epoch) never enters a fresh
 *               committee. A V1 claim of a fresh-bound key is left to the
 *               unchanged V1 rules (the V1 index never reads this one).
 *
 * Whole-block operations only, and a rebuild from the same blocks yields the
 * same index. There is no undo trail: the only keeper, node::FinalityTracker,
 * never disconnects a block from its private trackers (a disconnect marks it
 * dirty and the next Sync() rebuilds them, this index included, from H+1).
 * The index is kept only when Consensus::Params configure the fresh lineage
 * WITH R_key (regtest -b3freshkeyheight); with the knob absent nothing here
 * is ever stepped, so mainnet and testnet pay nothing.
 */
struct FreshKeyBinding {
    modern::BlsPubkeyBytes bls_pubkey{};
    int height{-1}; //!< height of the including block
    friend bool operator==(const FreshKeyBinding&, const FreshKeyBinding&) = default;
};

//! One counted fresh binding with its owner and target (RPC listings).
struct FreshKeyBindingEntry {
    modern::ValidatorKeyBytes validator_key{};
    uint64_t target_epoch{0};
    FreshKeyBinding binding{};
    friend bool operator==(const FreshKeyBindingEntry&, const FreshKeyBindingEntry&) = default;
};

class FreshKeyBindingIndex
{
public:
    //! One key claim of a block, in claim order (BlockKeyClaims).
    struct Claim {
        bool fresh{false};                     //!< fresh binding (else a V1 nonzero binding)
        modern::ValidatorKeyBytes validator_key{};
        uint64_t epoch{0};                     //!< target epoch (fresh claims only)
        modern::BlsPubkeyBytes bls_pubkey{};
        friend bool operator==(const Claim&, const Claim&) = default;
    };

    //! Was `bls_pubkey` ever claimed, under either kind?
    bool EverBound(const modern::BlsPubkeyBytes& bls_pubkey) const;
    //! Height of the first claim of `bls_pubkey`, if any.
    std::optional<int> FirstBoundHeight(const modern::BlsPubkeyBytes& bls_pubkey) const;
    //! Counted fresh bindings of `validator_key` naming `target_epoch` and
    //! included at lo <= height <= hi, in chain (then claim) order.
    std::vector<FreshKeyBinding> Bindings(const modern::ValidatorKeyBytes& validator_key, uint64_t target_epoch,
                                          int lo, int hi) const;
    //! Every counted fresh binding naming an epoch >= `min_epoch`, ordered by
    //! (validator_key, target_epoch) then chain order.
    std::vector<FreshKeyBindingEntry> BindingsFrom(uint64_t min_epoch) const;
    /** The target epoch of `validator_key`'s fresh binding of `bls_pubkey`
     * when that binding is the key's first claim (so the index recorded it);
     * nullopt otherwise. A key that is EverBound() without one was claimed
     * first by someone else (another validator, or a V1 binding): this
     * validator's own binding of it, if any, is inert. */
    std::optional<uint64_t> OwnFreshEpoch(const modern::ValidatorKeyBytes& validator_key,
                                          const modern::BlsPubkeyBytes& bls_pubkey) const;

    /** Apply one block's claims in order (see the class comment). Heights
     * must be strictly increasing. */
    void ConnectBlock(int height, const std::vector<Claim>& claims);
    //! Forget everything (before a rebuild).
    void Clear();

    //! Read-only counters (diagnostics and unit tests).
    size_t EverBoundCount() const { return m_ever_bound.size(); }
    size_t BindingCount() const;

    //! Same ever-bound keys (with first heights) and same counted bindings.
    friend bool operator==(const FreshKeyBindingIndex& a, const FreshKeyBindingIndex& b)
    {
        return a.m_ever_bound == b.m_ever_bound && a.m_bindings == b.m_bindings;
    }

private:
    using BindingKey = std::pair<modern::ValidatorKeyBytes, uint64_t>;

    std::map<modern::BlsPubkeyBytes, int> m_ever_bound;
    std::map<BindingKey, std::vector<FreshKeyBinding>> m_bindings;
    int m_last_height{-1}; //!< last connected height (strictly increasing)
};

/**
 * The key claims of a block, in claim order: per transaction, its V1
 * FINALITY_KEY pairs with a nonzero key (revocations claim nothing), then its
 * fresh pairs. False (and `out` empty) when a matcher refuses a transaction,
 * which a connected block never does.
 */
bool BlockKeyClaims(const CBlock& block, std::vector<FreshKeyBindingIndex::Claim>& out, std::string& error);

//! True when the chain's fresh lineage uses B-37 per-epoch keys (regtest
//! -b3freshkeyheight); false on every shipped network.
inline bool FreshKeyModeConfigured(const Consensus::Params& params)
{
    return params.fresh_lineage && params.fresh_lineage->FreshKeysEnabled();
}

/**
 * Keeps a FreshKeyBindingIndex in step with the active chain (the V1 binding
 * tracker's discipline): BlockConnected applies a block when in step with its
 * parent, otherwise marks dirty; Sync() rebuilds from H+1 when not in step.
 * Every fresh pair is re-verified (structure + cryptography,
 * modern::VerifyFreshKeyBindingsInBlock) before it is counted; the V1 pairs
 * are verified by the V1 binding tracker stepped next to this one.
 */
class FreshKeyBindingTracker
{
public:
    //! Bring the index to `target` (on `chain`); false if unavailable.
    bool Sync(const CChain& chain, const BlockManager& blockman, const Consensus::Params& params,
              const CBlockIndex& target);
    //! Apply a just-connected modern-era block when in step with its parent;
    //! otherwise (or on a failed re-verification) mark dirty.
    void BlockConnected(const CBlock& block, const CBlockIndex& index, const Consensus::Params& params);
    void MarkDirty() { m_dirty = true; }
    bool Synced(const uint256& tip_hash) const { return !m_dirty && m_synced_tip == tip_hash; }
    const FreshKeyBindingIndex& Index() const { return m_index; }

private:
    //! Extract, re-verify and apply one block; false (index cleared, dirty) on failure.
    bool Apply(const CBlock& block, int height, const Consensus::Params& params);

    FreshKeyBindingIndex m_index;
    uint256 m_synced_tip{};
    int m_synced_height{-1};
    bool m_dirty{true};
};

/**
 * Which target epoch a fresh binding included at `height` counts for
 * (finality dependency section 6.4, rules (2) and (3)): the genesis binding
 * (target 0, serving Set'_0 = Set'_1) from R_key to R_f - 1; from R_f a
 * binding names e + 2, where e is the fresh epoch of `height`, so it is
 * included after Snapshot(start_e - 1) and by Snapshot(start_{e+1} - 1),
 * which fixes Set'_{e+2}. `projected` is the fresh state projected for
 * `height` (FinalityTracker::Projected(height).fresh).
 */
struct FreshBindingWindow {
    enum class Status {
        OPEN,                //!< target_epoch counts at this height
        NOT_ACTIVE,          //!< no key mode, or below R_key
        LINEAGE_UNAVAILABLE, //!< from R_f: lineage not bootstrapped, or broken
    };
    Status status{Status::NOT_ACTIVE};
    uint64_t target_epoch{0};
    //! First height of the window that counts target_epoch (R_key, or start_e).
    int window_start{-1};
};
FreshBindingWindow FreshBindingWindowAt(int height, const Consensus::Params& params,
                                        const std::optional<FreshLineageState>& projected);
//! "open" / "not-active" / "lineage-unavailable" (RPC spelling).
const char* FreshBindingWindowStatusName(FreshBindingWindow::Status status);

/**
 * One member's B-37 seat for Set'_{target_epoch} (finality dependency
 * section 6.4, rules (2)-(4); node/finality_fresh_lineage.h seat rules): the
 * counted fresh bindings of `validator_key` naming `target_epoch` and
 * included at lo <= height <= hi, in chain order. Exactly one seats the
 * member on that key; none sits it out; two or more VOID the seat for that
 * epoch. Shared by the committee rule (BuildFreshCommittee) and the status
 * reporting (RPC, signer alerts) so both read the same seat.
 */
struct FreshSeat {
    std::vector<modern::BlsPubkeyBytes> pubkeys;
    //! Two or more counted bindings: no seat for this epoch.
    bool voided{false};
    //! The seated key: the only counted binding, when there is exactly one.
    std::optional<modern::BlsPubkeyBytes> Key() const
    {
        if (pubkeys.size() != 1) return std::nullopt;
        return pubkeys.front();
    }
};
FreshSeat FreshSeatFor(const FreshKeyBindingIndex& index, const modern::ValidatorKeyBytes& validator_key,
                       uint64_t target_epoch, int lo, int hi);

/**
 * The inclusion window [lo, hi] in which fresh bindings naming
 * `target_epoch` count for Set'_{target_epoch} (finality dependency section
 * 6.4, rules (2) and (3); the bounds of the requests ProjectFreshLineage
 * makes): target 0, the genesis binding serving Set'_0 = Set'_1, in [R_key,
 * R_f - 1]; target t >= 2 in [start_{t-2}, start_{t-1} - 1]. While a window
 * is still open (tip below R_f - 1 for target 0, fresh epoch t - 2 current
 * for t >= 2) hi is `tip_height`. nullopt without key mode, for target 1 (a
 * binding naming 1 never counts), for a window that has not opened on this
 * chain (`state` absent, unbootstrapped or not yet in epoch t - 2), or when
 * hi < lo. Status reporting only (signer alerts, RPC); FreshSeatFor over
 * this window reads the seat the committee rule will apply or applied.
 */
std::optional<std::pair<int, int>> FreshSeatWindow(uint64_t target_epoch, const Consensus::Params& params,
                                                   const FreshLineageState* state, int tip_height);

/**
 * Committee(Snapshot(request.snapshot_height)) under the B-37 seat rules,
 * stamped epoch 0 (ProjectFreshLineage restamps it): the members of the V1
 * snapshot (`active_weights` and `v1_bindings` as of the snapshot height:
 * positive whole weight, non-revoked V1 binding) that hold exactly one
 * counted fresh binding naming request.target_epoch in [request.window_lo,
 * request.snapshot_height] (FreshSeatFor), each seated on that fresh key
 * with its stake weight; voided and unbound members are left out. nullopt
 * when nobody is seated (or more than MAX_FINALITY_SET would be, as for any
 * snapshot). The V1 key never enters: the snapshot is built from a
 * temporary binding index holding only the fresh keys, which passed their
 * proof of possession when the binding connected (re-verified on rebuild by
 * FreshKeyBindingTracker). `fresh_keys` must hold the bindings as of the
 * snapshot height or later (the window's upper end excludes later ones).
 */
std::optional<ValidatorSetSnapshot> BuildFreshCommittee(const std::map<ValidatorKey, CAmount>& active_weights,
                                                        const FinalityBindingIndex& v1_bindings,
                                                        const FreshKeyBindingIndex& fresh_keys,
                                                        const FreshCommitteeRequest& request);

/**
 * Mempool policy for the fresh pairs of one transaction (TX_NOT_STANDARD:
 * none of it is block-invalidating, it only keeps useless bindings out of
 * relay). In pair order: a key already bound under either kind, or claimed
 * twice by this transaction (its V1 pairs first), is "fresh-key-ever-bound";
 * then the target must be the one the window counts:
 * "fresh-key-lineage-unavailable" when the lineage cannot count one,
 * "fresh-key-binding-outside-window" otherwise. Bindings sitting in the
 * mempool are not consulted: a later duplicate is inert once mined.
 */
bool CheckFreshKeyBindingPolicy(const CTransaction& tx, const std::vector<modern::FreshFinalityKeyPair>& pairs,
                                const FreshKeyBindingIndex& index, const FreshBindingWindow& window,
                                std::string& reason);

class FinalityTracker;

/**
 * B-37 key status of the chain at the active tip: status reporting only
 * (getfinalitylineage, and the wallet's bindfreshfinalitykey /
 * getfreshfinalitykeys through interfaces::Chain::freshFinalityKeyStatus);
 * never consensus, never relayed.
 */
struct FreshKeyChainStatus {
    //! One fresh binding of the index: the first claim of its key (a reused
    //! key is inert and recorded nowhere, FreshKeyBindingIndex).
    struct Binding {
        modern::ValidatorKeyBytes validator_key{};
        uint64_t epoch{0}; //!< target epoch
        modern::BlsPubkeyBytes bls_pubkey{};
        int height{-1}; //!< including block
        //! Included inside the window that counts its target
        //! (FreshSeatWindow; a window still open ends at the tip): it seats
        //! its validator in Set'_epoch, or with another one voids that seat.
        //! A binding naming 1, or included outside its window, never counts.
        bool counted{false};
        //! Counted, and two or more counted bindings of this validator name
        //! this epoch: the seat for that epoch is void (FreshSeatFor).
        bool voided{false};
        friend bool operator==(const Binding&, const Binding&) = default;
    };
    //! One member of a retained committee (Set'_{e-1}, Set'_e, Set'_{e+1}).
    struct Seat {
        uint64_t epoch{0};
        modern::ValidatorKeyBytes validator_key{};
        modern::BlsPubkeyBytes bls_pubkey{};
        uint64_t weight{0};
        friend bool operator==(const Seat&, const Seat&) = default;
    };

    //! Fresh-key mode (FreshKeyModeConfigured); nothing else is set without it.
    bool configured{false};
    //! The tracker and its fresh key index are synced at the tip.
    bool available{false};
    std::optional<int> key_height; //!< R_key
    int activation_height{-1};     //!< R_f
    std::optional<uint256> chain_domain;
    int tip_height{-1};
    //! The binding window of the NEXT block (FreshBindingWindowAt(tip + 1)).
    FreshBindingWindow::Status window_status{FreshBindingWindow::Status::NOT_ACTIVE};
    //! The target epoch a binding included in the next block counts for, and
    //! the first height of that window; set only while the window is OPEN.
    std::optional<uint64_t> next_binding_epoch;
    std::optional<int> window_start;
    bool bootstrapped{false}; //!< the fresh lineage exists at the tip
    bool broken{false};
    uint64_t epoch{0}; //!< current fresh epoch (bootstrapped only)
    //! Fresh bindings naming an epoch >= e - 2 (every epoch before the
    //! lineage bootstraps), ordered by (validator_key, epoch), then chain
    //! order; only the given validator's when one is given.
    std::vector<Binding> bindings;
    //! Members of Set'_{e-1}, Set'_e and Set'_{e+1} on their fresh keys, by
    //! epoch then member order; only the given validator's when one is given.
    std::vector<Seat> seats;
    //! The first on-chain claim of one probed key (FreshKeyStatusAt's
    //! `probe_keys`, any epoch): its height, and the target epoch when that
    //! claim is the given validator's own fresh binding. A claimed key
    //! without own_epoch was taken first by someone else (another
    //! validator's binding of either kind): the validator's binding of it is
    //! inert and, under identity-bound proofs of possession, only a holder
    //! of the key's secret could have made that claim.
    struct ProbeClaim {
        modern::BlsPubkeyBytes bls_pubkey{};
        std::optional<int> first_height;
        std::optional<uint64_t> own_epoch;
        friend bool operator==(const ProbeClaim&, const ProbeClaim&) = default;
    };
    //! One entry per probed key, in order; empty unless available.
    std::vector<ProbeClaim> probe_claims;
};

/**
 * FreshKeyChainStatus at the tip of `chain`. `tracker` must be synced to
 * that tip (the caller runs FinalityTracker::Sync under cs_main first);
 * otherwise only the configuration fields are filled (available = false).
 * Reads the tracker's current state, its fresh key index and its
 * projection for the next block (the binding window). `probe_keys` are
 * looked up in the ever-bound index (FreshKeyChainStatus::probe_claims; the
 * wallet probes its store's keys to detect a key claimed first elsewhere).
 */
FreshKeyChainStatus FreshKeyStatusAt(const FinalityTracker& tracker, const CChain& chain,
                                     const Consensus::Params& params,
                                     const std::optional<modern::ValidatorKeyBytes>& validator_key,
                                     const std::vector<modern::BlsPubkeyBytes>& probe_keys = {});

} // namespace node

#endif // B3COIN_NODE_FINALITY_FRESH_KEY_INDEX_H
