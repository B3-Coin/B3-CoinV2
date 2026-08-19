// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_MICROBLOCK_H
#define B3COIN_FLOWMESH_MICROBLOCK_H

#include <flowmesh/batch.h>
#include <flowmesh/deposit.h>
#include <flowmesh/state.h>
#include <hash.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <ios>
#include <optional>
#include <string>
#include <vector>

namespace flowmesh {

/**
 * FlowMesh microblock: one entry of the certified deterministic
 * execution log. NOT a blockchain block — there is no fork choice and no
 * chain weight; finality is a certificate (certificate.h) over this
 * structure's hash, and the certified log IS the history.
 *
 * IDENTITY IS EXECUTION CONTENT ONLY. The microblock commits exactly
 * what deterministic execution consumes: domain, position, anchor,
 * previous root, the CANONICAL action body, and the claimed results.
 * The certificate is separate (attestations accumulate over a fixed
 * identity), and the PROPOSER is separate too — proposer identity and
 * round live in the proposal ENVELOPE (sync.h), never in the candidate
 * hash, so a replacement proposer in a later recovery round can
 * re-propose a locked candidate under the SAME hash and locked
 * validators can re-attest it. Authorship is authorization metadata,
 * not content.
 *
 * The action body MUST be in canonical form (batch.h
 * CanonicalizeActions): a non-canonical body is rejected outright, so
 * one logical candidate set has exactly one microblock hash regardless
 * of network arrival order.
 */

//! Hard consensus bounds on validator work per microblock. Enforced
//! during decode (before element allocation) and re-checked
//! structurally before any execution or cryptography.
inline constexpr size_t MAX_MICROBLOCK_ACTIONS{4096};

inline constexpr uint32_t MICROBLOCK_VERSION_V1{1};

struct MicroblockCore {
    uint32_t version{MICROBLOCK_VERSION_V1};
    //! The one shared DEX domain for v1 (multiple domains are research).
    uint256 domain;
    //! Position in the certified log; equals the number of microblocks
    //! executed before this one (== the state's ledger slot).
    uint64_t sequence{0};
    //! Hash of the previous microblock; null at sequence 0.
    uint256 parent_hash;
    //! The B3 position this microblock builds relative to. Whether an
    //! anchor is acceptable (finality depth, reorg behavior) is judged
    //! by the caller's anchor rules — OWNER DECISION (OD-6).
    AnchorRef anchor;
    uint256 prev_state_root;
    //! CANONICAL action body (see above).
    std::vector<Action> actions;
    //! Commitment to `actions` (framed); must match ComputeActionsRoot.
    uint256 actions_root;
    //! ExecutionResultCommitment of applying `actions` to the previous
    //! state (see BatchResult::result_commitment).
    uint256 result_commitment;
    //! PURE FlowMeshState::Root() after execution.
    uint256 resulting_state_root;

    //! Bounded strict codec: the action count is checked BEFORE actions
    //! are read.
    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << version << domain << sequence << parent_hash << anchor << prev_state_root;
        WriteCompactSize(s, actions.size());
        for (const Action& action : actions) s << action;
        s << actions_root << result_commitment << resulting_state_root;
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> version >> domain >> sequence >> parent_hash >> anchor >> prev_state_root;
        const uint64_t n{ReadCompactSize(s)};
        if (n > MAX_MICROBLOCK_ACTIONS) {
            throw std::ios_base::failure("flowmesh microblock has too many actions");
        }
        actions.clear();
        actions.reserve(n);
        for (uint64_t i{0}; i < n; ++i) {
            Action action;
            s >> action;
            actions.push_back(std::move(action));
        }
        s >> actions_root >> result_commitment >> resulting_state_root;
    }

    static uint256 ComputeActionsRoot(const std::vector<Action>& actions)
    {
        HashWriter h;
        h << std::string{"b3/flowmesh/actions/v1"};
        h << static_cast<uint64_t>(actions.size());
        for (const Action& action : actions) h << action;
        return h.GetHash();
    }

    //! Canonical identity: the tagged hash of the full canonical
    //! serialization. Domain-separated from every other B3 hash.
    //! (v2 preimage: the proposer moved out to the proposal envelope.)
    uint256 GetHash() const
    {
        HashWriter h;
        h << std::string{"b3/flowmesh/microblock/v2"} << *this;
        return h.GetHash();
    }

    //! Structural rules — no state, no cryptography. Includes the
    //! canonical-body requirement: identity is only defined over the one
    //! canonical representation of the action set.
    bool ShapeIsValid() const
    {
        if (version != MICROBLOCK_VERSION_V1) return false;
        if (actions.size() > MAX_MICROBLOCK_ACTIONS) return false;
        if (sequence == 0 && !parent_hash.IsNull()) return false;
        if (sequence != 0 && parent_hash.IsNull()) return false;
        if (!ActionsAreCanonical(actions)) return false;
        return true;
    }
};

enum class CandidateError : uint8_t {
    NONE = 0,
    SHAPE = 1, // structural bounds or non-canonical action body
    WRONG_DOMAIN = 2,
    SEQUENCE = 3,
    PARENT = 4,
    PREV_ROOT = 5,
    ACTIONS_ROOT = 6,
    RESULT_COMMITMENT = 7,
    RESULT_ROOT = 8,
    //! Fatal internal execution failure (clearing/settlement
    //! inconsistency): the candidate is unusable and NOTHING was
    //! committed — never a mere per-action rejection.
    EXECUTION_FAILURE = 9,
};

/**
 * Independent re-execution of a candidate microblock against the
 * previous FINALIZED state (MB-0 atomic): the previous state is copied,
 * the copy is mutated, and only a fully matching candidate yields a next
 * state — the caller commits by replacement. On any error the outputs
 * are untouched and the previous state was never mutated.
 *
 * `expect_parent` is the caller's last certified microblock hash (null
 * before the first). Anchor acceptability and proposer eligibility are
 * the caller's checks (anchor rules / proposer schedule + envelope) —
 * this function verifies exactly the deterministic execution claim.
 */
inline CandidateError ExecuteCandidate(const FlowMeshState& prev, const uint256& domain,
                                       const uint256& expect_parent, const MicroblockCore& mb,
                                       const ActionAuthenticator& auth,
                                       const DepositVerifier* deposits, FlowMeshState& next_out,
                                       BatchResult& result_out)
{
    if (!mb.ShapeIsValid()) return CandidateError::SHAPE;
    if (mb.domain != domain) return CandidateError::WRONG_DOMAIN;
    if (mb.sequence != prev.ledger.Slot()) return CandidateError::SEQUENCE;
    if (mb.parent_hash != expect_parent) return CandidateError::PARENT;
    if (mb.prev_state_root != prev.Root()) return CandidateError::PREV_ROOT;
    if (mb.actions_root != MicroblockCore::ComputeActionsRoot(mb.actions)) {
        return CandidateError::ACTIONS_ROOT;
    }

    FlowMeshState next{prev};
    BatchExecutor exec{next, auth, deposits};
    std::optional<BatchResult> result{exec.ExecuteSlot(mb.actions, mb.anchor)};
    if (!result) return CandidateError::EXECUTION_FAILURE;

    if (result->result_commitment != mb.result_commitment) {
        return CandidateError::RESULT_COMMITMENT;
    }
    if (result->state_root != mb.resulting_state_root) return CandidateError::RESULT_ROOT;

    next_out = std::move(next);
    result_out = std::move(*result);
    return CandidateError::NONE;
}

/**
 * Proposer construction: canonicalize the candidate action set, execute
 * it on a copy of the previous state, and emit the microblock whose
 * claims match that execution exactly. Returns nullopt on fatal
 * execution failure. The proposer has no authority beyond selecting the
 * candidate set — the body is canonicalized, the identity is
 * proposer-free, and every claim is recomputed by every validator via
 * ExecuteCandidate.
 */
inline std::optional<MicroblockCore> BuildMicroblock(
    const FlowMeshState& prev, const uint256& domain, const uint256& parent_hash,
    const AnchorRef& anchor, std::vector<Action> actions, const ActionAuthenticator& auth,
    const DepositVerifier* deposits, FlowMeshState& next_out, BatchResult& result_out)
{
    MicroblockCore mb;
    mb.domain = domain;
    mb.sequence = prev.ledger.Slot();
    mb.parent_hash = parent_hash;
    mb.anchor = anchor;
    mb.prev_state_root = prev.Root();
    mb.actions = CanonicalizeActions(std::move(actions));
    mb.actions_root = MicroblockCore::ComputeActionsRoot(mb.actions);

    FlowMeshState next{prev};
    BatchExecutor exec{next, auth, deposits};
    std::optional<BatchResult> result{exec.ExecuteSlot(mb.actions, mb.anchor)};
    if (!result) return std::nullopt;
    mb.result_commitment = result->result_commitment;
    mb.resulting_state_root = result->state_root;

    next_out = std::move(next);
    result_out = std::move(*result);
    return mb;
}

// ---- BUY / SELL intents over the existing curve economics --------------
//
// A limit order IS a degenerate demand curve; these mappings are the
// canonical BUY/SELL encoding. The clearing economics stay exactly the
// repository's: one uniform-price pass per microblock, maximum volume,
// largest-remainder allocation. (A price-time order book is explicitly
// NOT built.)

//! BUY `qty` lots at limit `price`: full demand at or below the limit,
//! zero above it. Reserves exactly qty × price (the integer-price
//! worst case; no fill above the limit is possible).
inline std::optional<std::vector<ClearingEngine::Breakpoint>> MakeLimitBidCurve(CAmount price,
                                                                                CAmount qty)
{
    if (price < 0 || price >= MAX_MONEY || qty <= 0 || qty > MAX_MONEY) return std::nullopt;
    return std::vector<ClearingEngine::Breakpoint>{{price, qty}, {price + 1, 0}};
}

//! SELL `qty` lots at limit `price`: full supply at or above the limit,
//! zero below it.
inline std::optional<std::vector<ClearingEngine::Breakpoint>> MakeLimitAskCurve(CAmount price,
                                                                                CAmount qty)
{
    if (price < 0 || price > MAX_MONEY || qty <= 0 || qty > MAX_MONEY) return std::nullopt;
    if (price == 0) return std::vector<ClearingEngine::Breakpoint>{{0, qty}};
    return std::vector<ClearingEngine::Breakpoint>{{price - 1, 0}, {price, qty}};
}

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_MICROBLOCK_H
