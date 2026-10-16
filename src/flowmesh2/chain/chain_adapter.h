// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_CHAIN_CHAIN_ADAPTER_H
#define B3_FLOWMESH2_CHAIN_CHAIN_ADAPTER_H

#include <flowmesh2/core/authority.h>
#include <flowmesh2/core/facts.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/seat_set.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

/** The chain adapter: the only seam between FlowMesh and the B3 chain
 * (slice R10; ARCHITECTURE.md sections 3 and 8; plan section 3.3; [CC]
 * ownership boundary 6).
 *
 * FlowMesh reads finalized B3 facts (deposits, later seat changes, listings,
 * checkpoint acceptances, payouts and exit nullifiers) and the current
 * authority through a ChainAdapter, and nothing else of the chain. No
 * adapter method takes cs_main or any node lock on the caller's thread:
 * guard rule 1 keeps validation.h, node/ and chainparams.h out of every fm2
 * library, so a b3d adapter can only ever be a narrow link the node
 * implements (MB1-ADP-06, M7-ADP-01, owner-gated).
 *
 * The contract every caller relies on:
 *
 * - Network(): the chain type, magic and ModernChainDomain the adapter
 *   serves. Fixed for the adapter's lifetime.
 * - CurrentAuthority(): the AuthorityVersion now. Its epoch changes only when
 *   a seat-set fact is finalized or a disconnect removes a block at or below
 *   the anchor (plan PLAN-11); a b3d adapter bumps it synchronously inside
 *   ConnectTip/DisconnectTip (MB1-ADP-07). A read never blocks on the chain.
 * - CheckAuthority(expected): Ok iff CurrentAuthority() == expected, else
 *   AUTHORITY_CHANGED (CHAIN_AUTHORITY_CHANGED). The agreement calls it
 *   before writing ACCEPTED_PROPOSAL (check 1) and after B-INTENT,
 *   immediately before signing (check 2); the state owner's Apply is check
 *   3 (ARCHITECTURE.md section 8). This narrows the sign-versus-reorg window;
 *   it is NOT a proof of the linearization (M10-SPEC-01).
 * - Poll(after, max_facts): the next facts of the stream. `after` is the
 *   caller's facts_to: every fact before it has been consumed, so a batch
 *   always starts exactly at `after`. The reply (PollReply) is one of:
 *     FACTS             a FactBatch that passes CheckBatch(batch, after,
 *                       max_facts): possibly empty (a heartbeat anchor);
 *     BUSY              the adapter cannot answer now; nothing changed and
 *                       nothing was consumed; poll again later;
 *     NEED_DATA         the adapter has no data at `after` yet (the cursor is
 *                       beyond its final tip, or blocks are missing); poll
 *                       again after the chain advanced;
 *     AUTHORITY_CHANGED the facts at or after `rewind_to` changed (a reorg
 *                       at or below the caller's position); the authority
 *                       moved to Authority(). The caller fences, and resumes
 *                       only from a cursor at or before rewind_to.
 *   Errors are reserved for misuse and stubs: INVALID_INPUT (POLL_MAX_FACTS)
 *   unless 1 <= max_facts <= MAX_SLOT_FACTS, INVALID_INPUT for a cursor the
 *   adapter can never serve (before its first block), and NotImplemented of
 *   the b3d adapters.
 * - VerifyAnchor(candidate, last_certified): Ok iff `candidate` is a block of
 *   the adapter's chain that equals or descends from `last_certified`.
 *   INVALID_INPUT (ANCHOR_BELOW_CERTIFIED) for a candidate below
 *   last_certified; NEED_DATA (ANCHOR_UNKNOWN) for a candidate above the
 *   adapter's final tip; VERIFY_FAILED (ANCHOR_NOT_ON_CHAIN) for a candidate
 *   that is not on the chain; VERIFY_FAILED (ANCHOR_NOT_DESCENDANT) when
 *   last_certified is not an ancestor of it. A follower accepts any anchor
 *   at or above its last certified anchor that it can verify (plan 3.3).
 * - SeatSetAt(epoch): the PoP-verified seat set of an authority epoch;
 *   NEED_DATA (SEAT_SET_UNKNOWN_EPOCH) for an epoch the adapter has not seen.
 *
 * FactBatch and CheckBatch (real): a batch covers the half-open cursor range
 * [from, to) completely, so a slot can take facts_from = from and facts_to =
 * to. CheckBatch refuses, each INVALID_INPUT with its detail: from != after
 * (BATCH_FROM); to before from (BATCH_REVERSED); to beyond the anchor, i.e.
 * above (anchor.height + 1, 0) (BATCH_BEYOND_ANCHOR); more facts than
 * max_facts or MAX_SLOT_FACTS (BATCH_TOO_MANY); a fact cursor outside
 * [from, to) or not strictly ascending (BATCH_CURSOR); a fact that does not
 * encode (its EncodeFact error, so a stub kind returns that kind's
 * NotImplemented) or encodes above MAX_FACT_BYTES (OVERSIZE); and, because
 * the ledger would refuse them with an error that leaves the slot without a
 * valid candidate (O-34): a deposit of zero atoms (BATCH_DEPOSIT_ZERO), a
 * deposit to a protocol account of the batch's profile hash
 * (BATCH_DEPOSIT_PROTOCOL) and two deposits of one outpoint
 * (BATCH_DUPLICATE_OUTPOINT). A repeat of an outpoint across batches, a
 * cumulative deposit above 2^62 atoms and a 257th asset in one account are
 * still errors of the ledger, not of the batch (O-34, M2-LED-07).
 *
 * Threading: Network, CurrentAuthority, CheckAuthority, VerifyAnchor and
 * SeatSetAt may be called from any thread; Poll is called by the sealing
 * thread only. Implementations: dev/fake_chain.h (REGTEST only, fm2_dev) and
 * the b3d stubs of chain/b3d_adapter.h.
 */
namespace flowmesh2::chain {

// Static details (ErrorCode in parentheses).
inline constexpr std::string_view CHAIN_AUTHORITY_CHANGED{"chain authority is not the expected one"};          //!< (AUTHORITY_CHANGED)
inline constexpr std::string_view POLL_MAX_FACTS{"max_facts outside 1..MAX_SLOT_FACTS"};                       //!< (INVALID_INPUT)
inline constexpr std::string_view BATCH_FROM{"batch does not start at the cursor"};                            //!< (INVALID_INPUT)
inline constexpr std::string_view BATCH_REVERSED{"batch ends before it starts"};                               //!< (INVALID_INPUT)
inline constexpr std::string_view BATCH_BEYOND_ANCHOR{"batch range beyond its anchor"};                        //!< (INVALID_INPUT)
inline constexpr std::string_view BATCH_TOO_MANY{"batch holds more facts than asked for"};                     //!< (INVALID_INPUT)
inline constexpr std::string_view BATCH_CURSOR{"fact cursor outside the batch range or out of order"};         //!< (INVALID_INPUT)
inline constexpr std::string_view BATCH_FACT_BYTES{"fact longer than MAX_FACT_BYTES"};                         //!< (OVERSIZE)
inline constexpr std::string_view BATCH_DEPOSIT_ZERO{"deposit of zero atoms"};                                 //!< (INVALID_INPUT)
inline constexpr std::string_view BATCH_DEPOSIT_PROTOCOL{"deposit to a protocol account"};                     //!< (INVALID_INPUT)
inline constexpr std::string_view BATCH_DUPLICATE_OUTPOINT{"two deposits of one outpoint"};                    //!< (INVALID_INPUT)
inline constexpr std::string_view ANCHOR_BELOW_CERTIFIED{"anchor below the last certified anchor"};            //!< (INVALID_INPUT)
inline constexpr std::string_view ANCHOR_UNKNOWN{"anchor block above the adapter's final tip"};              //!< (NEED_DATA)
inline constexpr std::string_view ANCHOR_NOT_ON_CHAIN{"anchor block not on the adapter's chain"};              //!< (VERIFY_FAILED)
inline constexpr std::string_view ANCHOR_NOT_DESCENDANT{"anchor does not descend from the last certified anchor"}; //!< (VERIFY_FAILED)
inline constexpr std::string_view SEAT_SET_UNKNOWN_EPOCH{"no seat set known for the epoch"};                   //!< (NEED_DATA)

/** Facts of the half-open cursor range [from, to), complete: every fact of
 * the stream in that range is in `facts`, in ascending cursor order. Final
 * under `anchor` (to is at most (anchor.height + 1, 0)) and read under
 * `authority`.
 */
struct FactBatch {
    FactCursor from;
    FactCursor to;
    Anchor anchor;
    AuthorityVersion authority;
    std::vector<Fact> facts;

    friend bool operator==(const FactBatch&, const FactBatch&) = default;
};

//! What Poll answered. APPEND-ONLY registry; zero is never a value.
enum class PollStatus : uint8_t {
    FACTS = 1,             //!< a FactBatch (possibly empty)
    BUSY = 2,              //!< cannot answer now; nothing changed
    NEED_DATA = 3,         //!< no data at the cursor yet
    AUTHORITY_CHANGED = 4, //!< the facts at or after rewind_to changed
};

bool IsKnown(PollStatus status);
//! Registry name; "UNKNOWN" for an unregistered value.
std::string_view ToString(PollStatus status);

/** One answer of Poll. The factories are the only constructors, so the
 * status and the fields always agree: Batch() is set iff FACTS, RewindTo()
 * iff AUTHORITY_CHANGED; Authority() is the adapter's authority when it
 * answered (the batch's for FACTS).
 */
class PollReply
{
public:
    static PollReply Facts(FactBatch batch);
    static PollReply Busy(const AuthorityVersion& authority);
    static PollReply NeedData(const AuthorityVersion& authority);
    static PollReply AuthorityChanged(const AuthorityVersion& authority, const FactCursor& rewind_to);

    PollStatus Status() const { return m_status; }
    const AuthorityVersion& Authority() const { return m_authority; }
    //! The batch of a FACTS reply; nullptr otherwise.
    const FactBatch* Batch() const { return m_batch ? &*m_batch : nullptr; }
    //! The rewind cursor of an AUTHORITY_CHANGED reply; nullopt otherwise.
    const std::optional<FactCursor>& RewindTo() const { return m_rewind_to; }

    friend bool operator==(const PollReply&, const PollReply&) = default;

private:
    PollReply(PollStatus status, const AuthorityVersion& authority, std::optional<FactBatch> batch,
              std::optional<FactCursor> rewind_to);

    PollStatus m_status;
    AuthorityVersion m_authority;
    std::optional<FactBatch> m_batch;
    std::optional<FactCursor> m_rewind_to;
};

//! Checks a batch answered for Poll(after, max_facts); see the file comment.
Result<void> CheckBatch(const FactBatch& batch, const FactCursor& after, size_t max_facts);

class ChainAdapter
{
public:
    virtual ~ChainAdapter() = default;
    ChainAdapter(const ChainAdapter&) = delete;
    ChainAdapter& operator=(const ChainAdapter&) = delete;

    virtual NetworkId Network() const = 0;
    virtual AuthorityVersion CurrentAuthority() const = 0;
    //! Ok iff CurrentAuthority() == expected; AUTHORITY_CHANGED (CHAIN_AUTHORITY_CHANGED) otherwise.
    Result<void> CheckAuthority(const AuthorityVersion& expected) const;
    virtual Result<PollReply> Poll(const FactCursor& after, size_t max_facts) = 0;
    virtual Result<void> VerifyAnchor(const Anchor& candidate, const Anchor& last_certified) const = 0;
    virtual Result<SeatSet> SeatSetAt(uint64_t epoch) const = 0;

protected:
    ChainAdapter() = default;
};

} // namespace flowmesh2::chain

#endif // B3_FLOWMESH2_CHAIN_CHAIN_ADAPTER_H
