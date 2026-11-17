// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_CHAIN_B3D_ADAPTER_H
#define B3_FLOWMESH2_CHAIN_B3D_ADAPTER_H

#include <flowmesh2/chain/chain_adapter.h>
#include <flowmesh2/core/anchor_proof.h>
#include <flowmesh2/core/authority.h>
#include <flowmesh2/core/facts.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/seat_set.h>
#include <sync.h>
#include <uint256.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string_view>
#include <vector>

/** The b3d chain adapters (slice R10: interfaces; M7-ADP-01: the finalized
 * adapter is real; plan section 3.3).
 *
 * Both are ChainAdapters (chain/chain_adapter.h) over the facts of a fully
 * validating b3d. Guard rule 1 keeps every node header out of fm2 code, so
 * neither reads the node: b3d feeds them through the narrow link below, from
 * its own ConnectTip, DisconnectTip and finality hooks (the node side,
 * MB1-ADP-07, lives outside src/flowmesh2/ and is owner-gated). Until that
 * link exists in b3d, the regtest daemon feeds the fake adapter from the
 * shared dev chain file (daemon/fm2d_main.cpp).
 *
 * - B3dTrustedFactsAdapter (DEX-1 regtest mode, D8): "trusted facts", depth
 *   only, no finality, same operator. Create refuses every chain type but
 *   REGTEST with UNSUPPORTED_NETWORK (plan 3.3, Security M4: refused at
 *   start-up; the compile-time refusal comes with the node link). On
 *   REGTEST it is the stub MB1-ADP-06.
 *
 * - B3dFinalizedAdapter (M7-ADP-01, real): facts are usable once their block
 *   is finalized AND at least `min_depth` blocks deep (TRD-4: 30; the plan's
 *   bare finalized pin is decision C-9 and not built). Serves every network.
 *
 *   The link. b3d calls, synchronously and in chain order, on whichever
 *   thread validates (the adapter takes only its own mutex and never waits,
 *   so the call costs the node a few vector operations):
 *   - OnConnected(block): the next block of the active chain. Refused
 *     (INVALID_INPUT, nothing changes) unless block.height is tip + 1
 *     (B3D_HEIGHT), block.parent is the tip's hash (B3D_PARENT), the hash is
 *     not zero (B3D_BLOCK_HASH), the facts sit at cursors (height, 0), (height,
 *     1), ... in order (B3D_FACT_CURSOR), pass CheckBatch's fact rules in
 *     slot-sized pieces (a stub kind returns its NotImplemented; no zero
 *     deposit, deposit to a protocol account or repeated outpoint, O-34; a
 *     block may hold more facts than a slot, Poll cuts batches by max_facts)
 *     and repeat no outpoint of the current chain (B3D_OUTPOINT_REPEATED).
 *     `block.seats`, when set, is the PoP-verified seat set in force from
 *     this block on (PROVISIONAL: until M7-SEAT-01 derives it from
 *     SeatChangeFacts, the link supplies it). A refusal leaves the tip where
 *     it was, so every later block fails B3D_HEIGHT: the adapter fails closed
 *     and the node must treat the refusal as fatal for FlowMesh (the link
 *     contract for b3d, MB1-ADP-07; a fact kind the adapter cannot encode yet
 *     must not be handed over).
 *   - OnDisconnected(block): the tip leaves the active chain. Refused unless
 *     `block` is the tip (B3D_NOT_TIP); the base is never disconnected
 *     (B3D_BELOW_BASE: a reorg below the adapter's start is a fault the
 *     operator sees). Its outpoints are forgotten.
 *   - OnFinalized(block): the node's finality now covers `block` and
 *     everything below. Refused unless `block` is on the chain
 *     (B3D_FINALITY_OFF_CHAIN) and at or above the pin (B3D_FINALITY_BACKWARDS;
 *     a disconnect is the only thing that lowers it).
 *
 *   Usable anchor. The highest block that is finalized and at least
 *   min_depth below the tip, never below the base. The adapter remembers the
 *   highest usable anchor it has exposed (`Exposed()`: the anchor of every
 *   FACTS batch and the bound of VerifyAnchor and ProveAnchor); it rises as
 *   the chain grows or the pin moves and falls only when the exposed block
 *   itself is disconnected, one block at a time. So after a shallow reorg
 *   the exposed block stays exposed while it is on the chain although it is
 *   fewer than min_depth below the new tip: the depth rule is applied when a
 *   block is exposed, finality is what keeps it (TRD-4 as built; O-107).
 *
 *   Authority epoch (PLAN-11). The epoch changes only:
 *   (a) when a block carrying `seats` becomes usable (its seat set is the
 *       authority's from then on, and SeatSetAt knows the new epoch), or
 *   (b) when a disconnect removes the exposed block: a reorg under the
 *       consumer's anchor, which on a finalized adapter is also a finality
 *       violation. The pin drops to the new tip when it covered the block,
 *       the exposure steps down to the new tip (the block below, exposed
 *       earlier and still on the chain), the seat set in force becomes the
 *       one of the highest remaining exposed `seats` block (else the genesis
 *       set), the epoch bumps, and every Poll from a cursor after
 *       rewind_to = (block.height, 0) answers AUTHORITY_CHANGED{rewind_to}
 *       until the consumer resumes at or before it. A reorg of several
 *       exposed blocks comes as several disconnects: each bumps once and the
 *       rewind cursor is the lowest, so a consumer that applied facts of any
 *       removed block is sent back below all of them.
 *   A disconnect above the exposed anchor (an ordinary shallow reorg) changes
 *   no fact and no anchor a consumer saw: no bump, no rewind (Performance
 *   P-2: a new B3 block or a shallow reorg never stalls trading); only the
 *   pin follows the chain down when it covered the block. Both bumps happen
 *   inside the link call, before b3d queues any notification (CONS-10); a
 *   seat-set block may become usable through OnConnected (it was final, the
 *   new block makes it deep), so a connect above the anchor can bump the
 *   epoch by rule (a), never by rule (b).
 *
 *   ChainAdapter behaviour:
 *   - Poll(after, max): INVALID_INPUT (POLL_MAX_FACTS) unless 1 <= max <=
 *     MAX_SLOT_FACTS; the pending rewind (above); INVALID_INPUT
 *     (B3D_CURSOR_BEFORE_BASE) for a cursor before the first fact cursor
 *     (base.height + 1, 0); NEED_DATA for a cursor after the usable end
 *     (exposed.height + 1, 0); else FACTS: every usable fact at or after
 *     `after`, at most `max`, anchored at the exposed anchor, ending at the
 *     usable end or, when cut by `max`, right after the last fact returned.
 *     Every batch passes CheckBatch. It never answers BUSY: nothing it does
 *     waits.
 *   - VerifyAnchor(candidate, last_certified): the answers of
 *     chain/chain_adapter.h with the exposed anchor as "the adapter's final
 *     tip": a candidate above it is NEED_DATA (ANCHOR_UNKNOWN) even when the
 *     block is on the chain, because a seat must not sign over an anchor its
 *     own node has not finalized and seen deep enough.
 *   - ProveAnchor(candidate, from): VerifyAnchor(candidate, from) first, then
 *     the AnchorProof (core/anchor_proof.h) of the header path from `from`
 *     to `candidate` under the current finality pin; it passes
 *     CheckAnchorProof.
 *   - SeatSetAt(epoch): the seat set of every epoch since creation; NEED_DATA
 *     (SEAT_SET_UNKNOWN_EPOCH) for any other.
 *
 *   Create refuses, each INVALID_INPUT: a seat set whose root is not
 *   authority.seat_set_root (B3D_SEAT_SET), a zero base hash
 *   (B3D_BLOCK_HASH), a base height above 2^32 - 3 (B3D_BASE_HEIGHT) and, on
 *   every chain type but REGTEST, a min_depth below FINALIZED_MIN_DEPTH
 *   (B3D_MIN_DEPTH; regtest may run shallower, a cluster demo cannot wait
 *   thirty blocks per deposit; PROVISIONAL). The base is the block the fact
 *   stream starts after; it counts as finalized and usable from the start.
 *
 *   Locking: one Mutex guards everything; it is held only inside the
 *   adapter's own methods, never across a call into another object.
 */
namespace flowmesh2::chain {

//! TRD-4: a fact's block must be this deep (and finalized) before a seat uses it.
inline constexpr uint32_t FINALIZED_MIN_DEPTH{30};

// Static details (ErrorCode in parentheses).
inline constexpr std::string_view B3D_SEAT_SET{"seat set root is not the authority's"};               //!< (INVALID_INPUT)
inline constexpr std::string_view B3D_BLOCK_HASH{"zero block hash"};                                  //!< (INVALID_INPUT)
inline constexpr std::string_view B3D_BASE_HEIGHT{"base height leaves no room for blocks"};           //!< (INVALID_INPUT)
inline constexpr std::string_view B3D_MIN_DEPTH{"min_depth below the TRD-4 anchor depth"};            //!< (INVALID_INPUT)
inline constexpr std::string_view B3D_HEIGHT{"block is not the next height"};                         //!< (INVALID_INPUT)
inline constexpr std::string_view B3D_PARENT{"block does not extend the tip"};                        //!< (INVALID_INPUT)
inline constexpr std::string_view B3D_FACT_CURSOR{"fact cursor is not (height, index) in order"};     //!< (INVALID_INPUT)
inline constexpr std::string_view B3D_OUTPOINT_REPEATED{"outpoint already deposited on this chain"};  //!< (INVALID_INPUT)
inline constexpr std::string_view B3D_NOT_TIP{"disconnected block is not the tip"};                   //!< (INVALID_INPUT)
inline constexpr std::string_view B3D_BELOW_BASE{"the base block cannot be disconnected"};            //!< (INVALID_INPUT)
inline constexpr std::string_view B3D_FINALITY_OFF_CHAIN{"finalized block is not on the chain"};      //!< (INVALID_INPUT)
inline constexpr std::string_view B3D_FINALITY_BACKWARDS{"finality pin cannot move backwards"};       //!< (INVALID_INPUT)
inline constexpr std::string_view B3D_CURSOR_BEFORE_BASE{"cursor before the first fact cursor"};      //!< (INVALID_INPUT)

struct B3dAdapterConfig {
    uint32_t min_depth{0};
};

class B3dTrustedFactsAdapter
{
public:
    B3dTrustedFactsAdapter() = delete;
    //! UNSUPPORTED_NETWORK unless REGTEST; then the stub MB1-ADP-06.
    static Result<std::unique_ptr<ChainAdapter>> Create(const NetworkId& network, const B3dAdapterConfig& config);
};

//! What the node hands the finalized adapter for one connected block.
struct ChainBlock {
    Anchor block;
    uint256 parent;
    std::vector<Fact> facts;       //!< at cursors (block.height, 0), (block.height, 1), ...
    std::optional<SeatSet> seats;  //!< the seat set in force from this block on, when it changes here
};

class B3dFinalizedAdapter final : public ChainAdapter
{
public:
    static Result<std::unique_ptr<B3dFinalizedAdapter>> Create(const NetworkId& network, const B3dAdapterConfig& config,
                                                               const AuthorityVersion& authority, const SeatSet& seats,
                                                               const Anchor& base);
    ~B3dFinalizedAdapter() override;

    // ChainAdapter.
    NetworkId Network() const override { return m_network; }
    AuthorityVersion CurrentAuthority() const override EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    Result<PollReply> Poll(const FactCursor& after, size_t max_facts) override EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    Result<void> VerifyAnchor(const Anchor& candidate, const Anchor& last_certified) const override EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    Result<SeatSet> SeatSetAt(uint64_t epoch) const override EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    // The node link (see the file comment).
    Result<void> OnConnected(ChainBlock block) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    Result<void> OnDisconnected(const Anchor& block) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    Result<void> OnFinalized(const Anchor& block) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    //! The header path from `from` to `candidate` under the current pin; the answers of VerifyAnchor(candidate, from) first.
    Result<AnchorProof> ProveAnchor(const Anchor& candidate, const Anchor& from) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    // Inspection.
    Anchor Base() const { return m_base; }
    uint32_t MinDepth() const { return m_min_depth; }
    Anchor Tip() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    Anchor Finalized() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! The highest usable anchor exposed so far (the anchor of the next FACTS batch).
    Anchor Exposed() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! The first fact cursor, (base.height + 1, 0).
    FactCursor Start() const;
    //! The usable end, (exposed.height + 1, 0).
    FactCursor End() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

private:
    struct Block {
        uint256 hash;
        uint256 parent;
        std::vector<Fact> facts;
        std::optional<SeatSet> seats;
    };

    B3dFinalizedAdapter(const NetworkId& network, uint32_t min_depth, const AuthorityVersion& authority, const SeatSet& seats,
                        const Anchor& base);

    uint32_t TipHeightLocked() const EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    Anchor AnchorAtLocked(uint32_t height) const EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    bool OnChainLocked(const Anchor& anchor) const EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    //! min(finalized, tip - min_depth), never below the base.
    uint32_t UsableHeightLocked() const EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    //! Exposes every newly usable block in order, bumping the epoch at each one that carries a seat set.
    void AdvanceLocked() EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    //! The seat set of the highest usable block that carries one, else the genesis set.
    const SeatSet& SeatsInForceLocked() const EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    void BumpLocked(const SeatSet& seats) EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    Result<void> VerifyAnchorLocked(const Anchor& candidate, const Anchor& last_certified) const EXCLUSIVE_LOCKS_REQUIRED(m_mutex);

    const NetworkId m_network;
    const uint32_t m_min_depth;
    const Anchor m_base;
    const SeatSet m_genesis_seats;

    mutable Mutex m_mutex;
    AuthorityVersion m_authority GUARDED_BY(m_mutex);
    std::map<uint64_t, SeatSet> m_seat_sets GUARDED_BY(m_mutex);
    std::vector<Block> m_blocks GUARDED_BY(m_mutex); //!< m_blocks[i] is height base.height + 1 + i
    std::set<Outpoint> m_outpoints GUARDED_BY(m_mutex);
    Anchor m_finalized GUARDED_BY(m_mutex);
    Anchor m_exposed GUARDED_BY(m_mutex);
    std::optional<FactCursor> m_rewind_to GUARDED_BY(m_mutex);
};

} // namespace flowmesh2::chain

#endif // B3_FLOWMESH2_CHAIN_B3D_ADAPTER_H
