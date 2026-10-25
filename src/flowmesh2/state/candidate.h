// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_STATE_CANDIDATE_H
#define B3_FLOWMESH2_STATE_CANDIDATE_H

#include <flowmesh2/core/authority.h>
#include <flowmesh2/core/body.h>
#include <flowmesh2/core/market.h>
#include <flowmesh2/core/outcome.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/slot.h>
#include <flowmesh2/engine/engine.h>
#include <uint256.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

/** The values the state owner works with (slice R9; ARCHITECTURE.md sections
 * 3, 4 and 7.4): the published state, a committed head, the parent a slot
 * builds on, the slot inputs and the candidate a slot produces, plus the
 * pure function that turns a head and inputs into a candidate.
 *
 * - CoreState: everything a slot transforms. The engine state (the shared
 *   spot ledger and one book per market, engine/engine.h) and the seat set
 *   the state recognises. Published only as shared_ptr<const CoreState>, so
 *   a reader never sees it change.
 * - HeadView: one committed head, owning: its header, the hash of the
 *   certificate that decided it (zero at genesis) and the state after it.
 *   Header, certificate and state always belong to one slot.
 * - ParentRef: the parent a slot is built on. The outline only builds on
 *   the COMMITTED head; a CANDIDATE parent (pipelining) is the stub
 *   M4-AGR-10 in StateOwner::Preview.
 * - SlotInputs: what a proposer puts into the slot after a parent: the
 *   authority it is certified under, the anchor, the fact range and the
 *   body. The domain is the parent's; the header's profile_hash is
 *   authority.profile_hash.
 * - Candidate: what executing a slot produced, before any certificate: the
 *   header, the certified outcomes with their leaves, the action leaves, the
 *   CANONICAL body and the next state. Only BuildCandidate makes one
 *   (private constructor), so the state owner can only ever install a state
 *   that the engine computed from its head (StateOwner::Apply).
 *
 * Canonical body (O-50, PROVISIONAL). A candidate's body lists the originals
 * in canonical order (engine CanonicalOrder over the parent header hash and
 * the height), not in the order the proposer supplied them; facts stay in
 * cursor order. Execution never depended on the supplied order (the engine
 * orders canonically), so the header, including actions_root, is a function
 * of the parent and the SET of originals: permuted inputs give the same
 * header, and a leader cannot produce two different headers for one content.
 * The certificate still covers the exact original bytes (actions_root over
 * ActionLeaf of each original), in that canonical order. Re-execution
 * (ReexecuteOn) therefore refuses a body that is not in canonical order.
 *
 * Everything in this file is deterministic and takes no lock.
 */
namespace flowmesh2::state {

// Static details (ErrorCode in parentheses). Engine refusals keep their own
// SLOT_* details (engine/engine.h).
inline constexpr std::string_view SLOT_EPOCH_REGRESSED{"authority epoch below the parent's"};          //!< (INVALID_INPUT)
inline constexpr std::string_view SLOT_SEAT_SET_MISMATCH{"seat set root is not the state's"};           //!< (INVALID_INPUT)
inline constexpr std::string_view PARENT_NOT_HEAD{"parent is not the committed head"};                  //!< (STALE_PARENT)
inline constexpr std::string_view PARENT_KIND_UNKNOWN{"unknown parent kind"};                           //!< (INVALID_INPUT)
inline constexpr std::string_view REEXECUTE_GENESIS{"the genesis header is not re-executable"};         //!< (INVALID_INPUT)
inline constexpr std::string_view REEXECUTE_HEADER_MISMATCH{"re-executed header differs"};              //!< (VERIFY_FAILED)
inline constexpr std::string_view REEXECUTE_BODY_MISMATCH{"body is not the canonical body"};            //!< (VERIFY_FAILED)
inline constexpr std::string_view CANDIDATE_OUTCOMES_ROOT{"outcome leaves do not give outcomes_root"};  //!< (INVARIANT)
inline constexpr std::string_view CANDIDATE_OUTCOME_COUNT{"more outcomes than a header can count"};     //!< (INVARIANT)
inline constexpr std::string_view GENESIS_ZERO_SEAT_SET{"zero seat set root"};                          //!< (INVALID_INPUT)
inline constexpr std::string_view GENESIS_PROFILE_MISMATCH{"authority profile_hash is not the profile's"}; //!< (INVALID_INPUT)
inline constexpr std::string_view HEAD_STATE_MISSING{"head without a state"};                           //!< (INVALID_INPUT)

/** The state a slot transforms and the state owner publishes. A value type;
 * the state owner only ever holds it as shared_ptr<const CoreState>.
 */
struct CoreState {
    engine::EngineState engine; //!< shared spot ledger and the books of every market
    /** The seat set this state recognises; a slot's authority must name it
     * (SLOT_SEAT_SET_MISMATCH). Fixed at genesis in the outline: seat-set
     * facts that change it are M7-SEAT-01, which also decides whether a
     * change takes effect in its own slot or the next one.
     */
    uint256 seat_set_root;

    /** The state at genesis: EngineState::Genesis(profile, markets) (its
     * errors propagate) and `seat_set_root`; INVALID_INPUT
     * (GENESIS_ZERO_SEAT_SET) for a zero root.
     */
    static Result<CoreState> Genesis(const ProfileParams& profile, std::span<const MarketDescriptor> markets,
                                     const uint256& seat_set_root);

    //! The profile the state was created under (the ledger's).
    const ProfileParams& Profile() const { return engine.ledger.Profile(); }

    friend bool operator==(const CoreState&, const CoreState&) = default;
};

/** A committed head: its header, the hash of the certificate that decided
 * it (zero at genesis: the genesis header is not certified) and the state
 * after it. Immutable and owning; copies share the state. HeaderHash() is
 * computed once.
 */
class HeadView
{
public:
    //! INVALID_INPUT (HEAD_STATE_MISSING) for a null state.
    static Result<HeadView> Make(const SlotHeaderV2& header, const uint256& cert_hash, std::shared_ptr<const CoreState> state);

    const SlotHeaderV2& Header() const { return m_header; }
    const uint256& HeaderHash() const { return m_header_hash; }
    const uint256& CertHash() const { return m_cert_hash; }
    const CoreState& State() const { return *m_state; }
    const std::shared_ptr<const CoreState>& StatePtr() const { return m_state; }

    /** The engine's non-owning view of this head (engine::HeadView). It
     * points into this object and its state, so it is valid only while this
     * object lives; there is none for a temporary.
     */
    engine::HeadView EngineView() const& { return engine::HeadView{m_header, m_state->engine}; }
    engine::HeadView EngineView() const&& = delete;

private:
    HeadView(const SlotHeaderV2& header, const uint256& cert_hash, std::shared_ptr<const CoreState> state);

    SlotHeaderV2 m_header;
    uint256 m_header_hash;
    uint256 m_cert_hash;
    std::shared_ptr<const CoreState> m_state;
};

class Candidate;

//! What a ParentRef names. APPEND-ONLY; zero is never a value.
enum class ParentKind : uint8_t {
    COMMITTED = 1, //!< the state owner's committed head
    CANDIDATE = 2, //!< an uncommitted candidate (pipelining, M4-AGR-10)
};

//! The parent a slot builds on: its kind, height and header hash.
struct ParentRef {
    ParentKind kind{ParentKind::COMMITTED};
    uint64_t height{0};
    uint256 header_hash;

    //! The committed head `head`.
    static ParentRef Committed(const HeadView& head);
    //! The uncommitted `candidate` (only StateOwner::Preview's stub M4-AGR-10 accepts it).
    static ParentRef Uncommitted(const Candidate& candidate);

    friend bool operator==(const ParentRef&, const ParentRef&) = default;
};

/** What a proposer puts into the slot after a parent. The header is built
 * from these, the parent and the execution result.
 */
struct SlotInputs {
    /** The authority the slot is certified under: the header's
     * authority_epoch, seat_set_root and profile_hash. The epoch may grow
     * (a reorg at or below the anchor, a seat-set change; ARCHITECTURE.md 8)
     * but never fall below the parent's; the seat set must be the parent
     * state's; the profile hash must be the engine's.
     */
    AuthorityVersion authority;
    Anchor anchor;         //!< the B3 block the slot is anchored to
    FactCursor facts_from; //!< equal to the parent's facts_to
    FactCursor facts_to;   //!< end of the consumed fact range (exclusive)
    SlotBody body;         //!< canonical fact bytes in cursor order; exact original action bytes in any order

    friend bool operator==(const SlotInputs&, const SlotInputs&) = default;
};

/** What executing one slot produced, before any certificate (see the file
 * comment). Copyable; every copy keeps the guarantee that the engine
 * computed it. Only BuildCandidate constructs one.
 */
class Candidate
{
public:
    //! The header: parent link, authority, inputs, roots and counts.
    const SlotHeaderV2& Header() const { return m_header; }
    //! The certified outcomes in emission order (engine/engine.h).
    const std::vector<OutcomeV2>& Outcomes() const { return m_outcomes; }
    //! OutcomeV2::LeafHash of each outcome; MerkleRoot of them is header.outcomes_root.
    const std::vector<uint256>& OutcomeLeaves() const { return m_outcome_leaves; }
    //! ActionLeaf of each original of Body(); MerkleRoot of them is header.actions_root.
    const std::vector<uint256>& ActionLeaves() const { return m_action_leaves; }
    //! The canonical body (originals in canonical order; O-50).
    const SlotBody& Body() const { return m_body; }
    //! The state after the slot; its commitment root is header.state_root.
    const std::shared_ptr<const CoreState>& Next() const { return m_next; }

private:
    friend Result<Candidate> BuildCandidate(const engine::Engine& engine, const HeadView& parent, const SlotInputs& inputs);
    Candidate(SlotHeaderV2 header, std::vector<OutcomeV2> outcomes, std::vector<uint256> outcome_leaves,
              std::vector<uint256> action_leaves, SlotBody body, std::shared_ptr<const CoreState> next);

    SlotHeaderV2 m_header;
    std::vector<OutcomeV2> m_outcomes;
    std::vector<uint256> m_outcome_leaves;
    std::vector<uint256> m_action_leaves;
    SlotBody m_body;
    std::shared_ptr<const CoreState> m_next;
};

/** Execute `inputs` on the committed head `parent`: the work of
 * StateOwner::Preview without the lock. Pure: no lock, clock, I/O or
 * randomness, and `parent` is not changed.
 *
 * Steps, the first failure returned (no candidate):
 * 1. authority: epoch not below the parent header's (INVALID_INPUT,
 *    SLOT_EPOCH_REGRESSED); seat set root equal to the parent state's
 *    (INVALID_INPUT, SLOT_SEAT_SET_MISMATCH);
 * 2. engine::ValidateSlotInputs over {parent domain, authority.profile_hash,
 *    anchor, facts, body} (its INVALID_INPUT details, a stub fact kind's
 *    NotImplemented, the profile's own errors) and the canonical order;
 * 3. engine::Engine::ApplySlot (STALE_PARENT, INVARIANT for a broken parent
 *    or a failed phase 9, the ledger's errors);
 * 4. the canonical body, ActionLeaf and LeafHash of every original and
 *    outcome (INVARIANT, CANDIDATE_OUTCOMES_ROOT, if the outcome leaves do
 *    not give the engine's outcomes_root; CANDIDATE_OUTCOME_COUNT above
 *    2^32 - 1 outcomes);
 * 5. the header: version 1, the parent's domain, authority.profile_hash,
 *    authority.epoch, parent height + 1, the parent's header hash and
 *    certificate hash, the anchor and fact range, authority.seat_set_root,
 *    facts_root, actions_root, outcomes_root, state_root (the next state's
 *    commitment v0 root), action_count, outcome_count, commitment_version 0.
 * The next state keeps the parent's seat set.
 *
 * REAL-REPLACE M2-LED-10: the candidate shares the parent's ledger trees and
 * commitment levels, so the ledger's part of the work is O(changed); the
 * books are still copied, hashed and checked whole, O(orders).
 */
Result<Candidate> BuildCandidate(const engine::Engine& engine, const HeadView& parent, const SlotInputs& inputs);

/** Re-execute a proposed or decided slot (`header`, `body`) on `parent`:
 * INVALID_INPUT (REEXECUTE_GENESIS) for height 0; STALE_PARENT
 * (PARENT_NOT_HEAD) unless header names `parent` (height and parent hash);
 * then BuildCandidate with the inputs the header names ({header.Authority(),
 * anchor, facts_from, facts_to, body}) and its errors; then VERIFY_FAILED
 * (REEXECUTE_HEADER_MISMATCH) unless the candidate's header equals `header`
 * exactly, and VERIFY_FAILED (REEXECUTE_BODY_MISMATCH) unless `body` is the
 * candidate's canonical body. Pure.
 */
Result<Candidate> ReexecuteOn(const engine::Engine& engine, const HeadView& parent, const SlotHeaderV2& header,
                              const SlotBody& body);

/** The genesis head of a FlowMesh instance: CoreState::Genesis(profile,
 * markets, authority.seat_set_root) and GenesisHeader(domain, authority,
 * anchor, facts_start, its commitment root), with a zero certificate hash.
 * INVALID_INPUT (GENESIS_PROFILE_MISMATCH) unless authority.profile_hash is
 * ProfileHash(profile); the errors of CoreState::Genesis, the commitment and
 * GenesisHeader propagate.
 */
Result<HeadView> GenesisHead(const ProfileParams& profile, std::span<const MarketDescriptor> markets, const uint256& domain,
                             const AuthorityVersion& authority, const Anchor& anchor, const FactCursor& facts_start);

} // namespace flowmesh2::state

#endif // B3_FLOWMESH2_STATE_CANDIDATE_H
