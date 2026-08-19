// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_MICROBLOCK_H
#define B3COIN_FLOWMESH_MICROBLOCK_H

#include <flowmesh/batch.h>
#include <flowmesh/deposit.h>
#include <flowmesh/state.h>
#include <hash.h>
#include <pubkey.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
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
 * The certificate is deliberately NOT part of the microblock or its
 * hash: attestations accumulate over an already-fixed identity.
 */

//! Hard consensus bounds on validator work per microblock. Checked
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
    std::vector<Action> actions;
    //! Commitment to `actions` (framed); must match ActionsRoot().
    uint256 actions_root;
    //! ExecutionResultCommitment of applying `actions` to the previous
    //! state (see BatchResult::result_commitment).
    uint256 result_commitment;
    //! PURE FlowMeshState::Root() after execution.
    uint256 resulting_state_root;
    //! The proposing FN seat's operator key. Eligibility is judged by
    //! the caller against the proposer schedule (recovery.h).
    XOnlyPubKey proposer;

    SERIALIZE_METHODS(MicroblockCore, obj)
    {
        READWRITE(obj.version, obj.domain, obj.sequence, obj.parent_hash, obj.anchor,
                  obj.prev_state_root, obj.actions, obj.actions_root, obj.result_commitment,
                  obj.resulting_state_root, obj.proposer);
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
    uint256 GetHash() const
    {
        HashWriter h;
        h << std::string{"b3/flowmesh/microblock/v1"} << *this;
        return h.GetHash();
    }

    //! Structural bounds only — no state, no cryptography.
    bool ShapeIsValid() const
    {
        if (version != MICROBLOCK_VERSION_V1) return false;
        if (actions.size() > MAX_MICROBLOCK_ACTIONS) return false;
        if (sequence == 0 && !parent_hash.IsNull()) return false;
        if (sequence != 0 && parent_hash.IsNull()) return false;
        return true;
    }
};

enum class CandidateError : uint8_t {
    NONE = 0,
    SHAPE = 1,
    WRONG_DOMAIN = 2,
    SEQUENCE = 3,
    PARENT = 4,
    PREV_ROOT = 5,
    ACTIONS_ROOT = 6,
    RESULT_COMMITMENT = 7,
    RESULT_ROOT = 8,
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
 * the caller's checks (anchor rules / proposer schedule) — this function
 * verifies exactly the deterministic execution claim.
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
    BatchResult result{exec.ExecuteSlot(mb.actions, mb.anchor)};

    if (result.result_commitment != mb.result_commitment) {
        return CandidateError::RESULT_COMMITMENT;
    }
    if (result.state_root != mb.resulting_state_root) return CandidateError::RESULT_ROOT;

    next_out = std::move(next);
    result_out = std::move(result);
    return CandidateError::NONE;
}

/**
 * Proposer construction: execute the action set on a copy of the
 * previous state and emit the microblock whose claims match that
 * execution exactly. The proposer has no authority beyond ordering —
 * every claim is recomputed by every validator via ExecuteCandidate.
 */
inline MicroblockCore BuildMicroblock(const FlowMeshState& prev, const uint256& domain,
                                      const uint256& parent_hash, const AnchorRef& anchor,
                                      std::vector<Action> actions, const XOnlyPubKey& proposer,
                                      const ActionAuthenticator& auth,
                                      const DepositVerifier* deposits, FlowMeshState& next_out,
                                      BatchResult& result_out)
{
    MicroblockCore mb;
    mb.domain = domain;
    mb.sequence = prev.ledger.Slot();
    mb.parent_hash = parent_hash;
    mb.anchor = anchor;
    mb.prev_state_root = prev.Root();
    mb.actions = std::move(actions);
    mb.actions_root = MicroblockCore::ComputeActionsRoot(mb.actions);
    mb.proposer = proposer;

    FlowMeshState next{prev};
    BatchExecutor exec{next, auth, deposits};
    BatchResult result{exec.ExecuteSlot(mb.actions, mb.anchor)};
    mb.result_commitment = result.result_commitment;
    mb.resulting_state_root = result.state_root;

    next_out = std::move(next);
    result_out = std::move(result);
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
//! zero above it. Requires price + 1 to be representable.
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
