// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_DEV_FAKE_CHAIN_H
#define B3_FLOWMESH2_DEV_FAKE_CHAIN_H

#ifndef FM2_DEV_COMPONENTS
#error "dev/fake_chain.h is a REGTEST-only dev component: only the fm2_dev target (FM2_DEV_COMPONENTS) and its users may include it"
#endif

#include <flowmesh2/chain/chain_adapter.h>
#include <flowmesh2/core/authority.h>
#include <flowmesh2/core/facts.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/seat_set.h>
#include <flowmesh2/storage/journal.h>
#include <flowmesh2/storage/record.h>
#include <sync.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string_view>
#include <vector>

/** FakeChainAdapter: a scripted REGTEST chain (slice R10; D8; ARCHITECTURE.md
 * sections 8 and 14.1). DEV ONLY: it lives in fm2_dev, compiles only with
 * FM2_DEV_COMPONENTS, and guard rule 3 keeps its symbols out of fm2_core and
 * fm2_verify. It is the only ChainAdapter of the outline: the simulator and
 * the agreement tests use it in place of a b3d adapter.
 *
 * Model. A chain of blocks from `base` (the block the fact stream starts
 * after; facts start at cursor (base.height + 1, 0)) to the tip. Every block
 * is final and carries its facts at cursors (height, 0), (height, 1), ...
 * The adapter serves ONE consumer (one sealing thread).
 *
 * Script (each refusal changes nothing):
 * - AppendBlock(hash, facts): the next block. INVALID_INPUT for a zero hash
 *   (FAKE_BLOCK_HASH), no height left (FAKE_CHAIN_FULL) or a fact whose
 *   cursor is not (height, i) in order (FAKE_FACT_CURSOR); CheckBatch's
 *   rules apply to the block's facts too (at most MAX_SLOT_FACTS of them; a
 *   stub fact kind returns its NotImplemented; a zero deposit, a deposit to
 *   a protocol account or one outpoint twice are refused, O-34), and so is
 *   an outpoint already deposited on the current chain
 *   (FAKE_OUTPOINT_REPEATED).
 * - QueueBusy(n): the next n polls answer BUSY.
 * - BumpAuthority(seats): epoch + 1; `seats`, when given, becomes the seat
 *   set (its root the authority's seat_set_root) from the new epoch on.
 * - Rewind(fork_height): drops every block above fork_height (at least the
 *   base, below the tip; INVALID_INPUT FAKE_REWIND otherwise), forgets their
 *   outpoints, bumps the authority (a disconnect at or below the consumer's
 *   anchor, PLAN-11; the fake does not know the anchor, so it always bumps)
 *   and remembers rewind_to = (fork_height + 1, 0). Until the consumer polls
 *   from a cursor at or before rewind_to, every poll from a cursor after it
 *   answers AUTHORITY_CHANGED{rewind_to}.
 * - BumpAt(point, occurrence, seats): arms a hook that calls
 *   BumpAuthority(seats) at the `occurrence`-th time (1-based, counted from
 *   arming) that `point` is reached. Points: POLL (start of Poll), and the
 *   barriers of the journals that use JournalHook() as their observer:
 *   SIGNER_BARRIER_START/END and COMMIT_BARRIER_START/END. This is how a
 *   test changes the authority "during the intent barrier".
 *
 * ChainAdapter behaviour:
 * - Poll(after, max): INVALID_INPUT (POLL_MAX_FACTS) unless 1 <= max <=
 *   MAX_SLOT_FACTS; fires POLL; then, in this order: BUSY while queued; the
 *   pending rewind (above); INVALID_INPUT (FAKE_CURSOR_BEFORE_BASE) for a
 *   cursor before the first fact cursor; NEED_DATA for a cursor after the
 *   end (tip + 1, 0); else FACTS: every fact at or after `after`, at most
 *   `max`, anchored at the tip, ending at the end or, when cut by `max`,
 *   right after the last fact returned. Every batch passes CheckBatch.
 * - VerifyAnchor: the chain answers of chain/chain_adapter.h; a block below
 *   the base is never verifiable (ANCHOR_NOT_ON_CHAIN / ANCHOR_NOT_DESCENDANT).
 * - SeatSetAt(epoch): the seat set of every epoch since creation; NEED_DATA
 *   (SEAT_SET_UNKNOWN_EPOCH) for any other.
 *
 * Create refuses every chain type but REGTEST (UNSUPPORTED_NETWORK), a seat
 * set whose root is not authority.seat_set_root (FAKE_SEAT_SET), a zero
 * base hash (FAKE_BLOCK_HASH) and a base height above 2^32 - 3
 * (FAKE_BASE_HEIGHT), each INVALID_INPUT.
 *
 * Locking: one Mutex guards the chain, the authority, the seat sets, the
 * queues and the hooks. It is held only inside the fake's own methods,
 * never across a call into another object. JournalHook's callbacks take it
 * on the thread that runs the journal's barrier, which calls observers
 * without holding the journal's lock (storage/journal.h), so no lock order
 * arises.
 */
namespace flowmesh2::dev {

// Static details (ErrorCode in parentheses).
inline constexpr std::string_view FAKE_NETWORK{"the fake chain is REGTEST only"};                       //!< (UNSUPPORTED_NETWORK)
inline constexpr std::string_view FAKE_SEAT_SET{"seat set root is not the authority's"};               //!< (INVALID_INPUT)
inline constexpr std::string_view FAKE_BLOCK_HASH{"zero block hash"};                                  //!< (INVALID_INPUT)
inline constexpr std::string_view FAKE_BASE_HEIGHT{"base height leaves no room for blocks"};           //!< (INVALID_INPUT)
inline constexpr std::string_view FAKE_CHAIN_FULL{"no height left for another block"};                 //!< (INVALID_INPUT)
inline constexpr std::string_view FAKE_FACT_CURSOR{"fact cursor is not (height, index) in order"};     //!< (INVALID_INPUT)
inline constexpr std::string_view FAKE_OUTPOINT_REPEATED{"outpoint already deposited on this chain"};  //!< (INVALID_INPUT)
inline constexpr std::string_view FAKE_REWIND{"rewind height outside [base, tip)"};                    //!< (INVALID_INPUT)
inline constexpr std::string_view FAKE_CURSOR_BEFORE_BASE{"cursor before the first fact cursor"};      //!< (INVALID_INPUT)
inline constexpr std::string_view FAKE_HOOK_OCCURRENCE{"hook occurrence must be at least 1"};          //!< (INVALID_INPUT)

//! Where an armed BumpAt hook can fire. APPEND-ONLY; zero is never a value.
enum class HookPoint : uint8_t {
    POLL = 1,
    SIGNER_BARRIER_START = 2,
    SIGNER_BARRIER_END = 3,
    COMMIT_BARRIER_START = 4,
    COMMIT_BARRIER_END = 5,
};

std::string_view ToString(HookPoint point);

class FakeChainAdapter final : public chain::ChainAdapter
{
public:
    static Result<std::unique_ptr<FakeChainAdapter>> Create(const NetworkId& network, const AuthorityVersion& authority,
                                                            const SeatSet& seats, const Anchor& base);
    ~FakeChainAdapter() override;

    // ChainAdapter.
    NetworkId Network() const override { return m_network; }
    AuthorityVersion CurrentAuthority() const override EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    Result<chain::PollReply> Poll(const FactCursor& after, size_t max_facts) override EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    Result<void> VerifyAnchor(const Anchor& candidate, const Anchor& last_certified) const override EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    Result<SeatSet> SeatSetAt(uint64_t epoch) const override EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    // Script.
    Result<Anchor> AppendBlock(const uint256& hash, std::vector<Fact> facts) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void QueueBusy(uint32_t polls) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    AuthorityVersion BumpAuthority(const std::optional<SeatSet>& seats = std::nullopt) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    Result<AuthorityVersion> Rewind(uint32_t fork_height) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    Result<void> BumpAt(HookPoint point, uint64_t occurrence, const std::optional<SeatSet>& seats = std::nullopt)
        EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Observer for the signer and commit journals whose barriers are hook points.
    storage::BarrierObserver& JournalHook() { return m_journal_hook; }

    // Inspection.
    Anchor Base() const { return m_base; }
    Anchor Tip() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! The first fact cursor, (base.height + 1, 0).
    FactCursor Start() const;
    //! The cursor after the last block, (tip + 1, 0).
    FactCursor End() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! The number of times `point` was reached since creation.
    uint64_t Reached(HookPoint point) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

private:
    class JournalHookObserver final : public storage::BarrierObserver
    {
    public:
        explicit JournalHookObserver(FakeChainAdapter& fake) : m_fake{fake} {}
        void OnAppend(storage::JournalRole, uint64_t, storage::RecordType, uint64_t) override {}
        void OnBarrierStart(storage::JournalRole role, uint64_t instance_id, uint64_t target_seq) override;
        void OnBarrierEnd(storage::JournalRole role, uint64_t instance_id, uint64_t target_seq, bool ok) override;

    private:
        FakeChainAdapter& m_fake;
    };

    struct Block {
        uint256 hash;
        std::vector<Fact> facts;
    };
    struct Hook {
        HookPoint point;
        uint64_t remaining; //!< fires when this reaches 0
        std::optional<SeatSet> seats;
    };

    FakeChainAdapter(const NetworkId& network, const AuthorityVersion& authority, const SeatSet& seats, const Anchor& base);

    //! Counts `point` and fires the hooks armed for it.
    void Reach(HookPoint point) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    AuthorityVersion BumpLocked(const std::optional<SeatSet>& seats) EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    uint32_t TipHeightLocked() const EXCLUSIVE_LOCKS_REQUIRED(m_mutex);

    const NetworkId m_network;
    const Anchor m_base;
    JournalHookObserver m_journal_hook;

    mutable Mutex m_mutex;
    AuthorityVersion m_authority GUARDED_BY(m_mutex);
    std::map<uint64_t, SeatSet> m_seat_sets GUARDED_BY(m_mutex);
    std::vector<Block> m_blocks GUARDED_BY(m_mutex); //!< m_blocks[i] is height base.height + 1 + i
    std::set<Outpoint> m_outpoints GUARDED_BY(m_mutex);
    uint32_t m_busy GUARDED_BY(m_mutex){0};
    std::optional<FactCursor> m_rewind_to GUARDED_BY(m_mutex);
    std::vector<Hook> m_hooks GUARDED_BY(m_mutex);
    std::map<HookPoint, uint64_t> m_reached GUARDED_BY(m_mutex);
};

} // namespace flowmesh2::dev

#endif // B3_FLOWMESH2_DEV_FAKE_CHAIN_H
