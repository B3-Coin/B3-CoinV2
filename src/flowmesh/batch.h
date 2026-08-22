// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_BATCH_H
#define B3COIN_FLOWMESH_BATCH_H

#include <consensus/amount.h>
#include <flowmesh/clearing.h>
#include <flowmesh/deposit.h>
#include <flowmesh/ledger.h>
#include <flowmesh/state.h>
#include <hash.h>
#include <modern/policy.h>
#include <modern/vault.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <uint256.h>

#include <algorithm>
#include <cstdint>
#include <ios>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace flowmesh {

/**
 * Canonical batch execution for one FlowMesh slot / microblock.
 *
 * IDENTITY IS SEMANTIC ONLY. A microblock body contains exactly ONE
 * canonical representation per semantic action — actions in a body
 * carry NO credentials at all (canonically empty). Authentication
 * evidence travels OUTSIDE the semantic body (the proposal envelope /
 * certified entry) and can therefore never make two bodies for the same
 * logical action set hash differently: same semantic set => same
 * canonical body => same microblock hash, regardless of which
 * credential encodings any node happened to observe.
 *
 * Consequently EXECUTION performs no authentication: authentication is
 * a pre-admission filter (pool admission and proposal/evidence
 * verification, sync.h). Execution outcomes are limited to the
 * deterministic set {MALFORMED, EQUIVOCATION, BAD_SEQUENCE,
 * REJECTED_BY_STATE} plus application.
 */

//! Consensus-stable action type numbering; append only.
enum class ActionType : uint8_t {
    SUBMIT_BID = 0,
    SUBMIT_ASK = 1,
    CANCEL_BID = 2,
    CANCEL_ASK = 3,
    WITHDRAW = 4,
    DEPOSIT = 5,
    //! RESERVED (owner accounting rule 2026-08-22): the ONLY paths by which
    //! value may cross between the SPOT state domain (this ledger) and the
    //! future FUTURES state domain. FUTURES_TO_SPOT requires a margin-safety
    //! check. Both are REJECTED in v1 — futures are not implemented; the
    //! numbers are fixed now so the registry stays append-only.
    SPOT_TO_FUTURES = 6,
    FUTURES_TO_SPOT = 7,
};

//! Structural bounds, enforced during decode (before allocation) and
//! before any cryptography.
inline constexpr size_t MAX_ACTION_CURVE_POINTS{64};
inline constexpr size_t MAX_ACTION_CREDENTIAL_SIZE{256};

struct Action {
    AccountId signer;
    uint64_t sequence{0};
    uint8_t type{0};
    //! SUBMIT_*: the demand curve.
    std::vector<ClearingEngine::Breakpoint> curve;
    //! WITHDRAW: asset, amount, destination.
    AssetId asset;
    CAmount amount{0};
    uint256 destination;
    //! DEPOSIT: the B3 outpoint being consumed. The asset, amount and
    //! beneficiary account are established by the DepositVerifier from
    //! chain data — an action-supplied claim of them is never trusted.
    COutPoint outpoint;
    //! TRANSPORT-LEVEL authorization material (pool submission), judged
    //! only by the ActionAuthenticator. NEVER part of the action id and
    //! NEVER part of a microblock body: canonical bodies carry this
    //! field EMPTY, with the admitted evidence carried in the proposal
    //! envelope / certified entry instead.
    std::vector<unsigned char> credential;

    //! Bounded strict codec: vector counts are checked BEFORE their
    //! elements are read/allocated.
    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << signer << sequence << type;
        WriteCompactSize(s, curve.size());
        for (const ClearingEngine::Breakpoint& bp : curve) s << bp;
        s << asset << amount << destination << outpoint;
        WriteCompactSize(s, credential.size());
        if (!credential.empty()) {
            s.write(std::as_bytes(std::span{credential.data(), credential.size()}));
        }
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> signer >> sequence >> type;
        const uint64_t points{ReadCompactSize(s)};
        if (points > MAX_ACTION_CURVE_POINTS) {
            throw std::ios_base::failure("flowmesh action curve too large");
        }
        curve.clear();
        curve.reserve(points);
        for (uint64_t i{0}; i < points; ++i) {
            ClearingEngine::Breakpoint bp;
            s >> bp;
            curve.push_back(bp);
        }
        s >> asset >> amount >> destination >> outpoint;
        const uint64_t cred{ReadCompactSize(s)};
        if (cred > MAX_ACTION_CREDENTIAL_SIZE) {
            throw std::ios_base::failure("flowmesh action credential too large");
        }
        credential.resize(cred);
        if (cred > 0) {
            s.read(std::as_writable_bytes(std::span{credential.data(), credential.size()}));
        }
    }

    //! Canonical SEMANTIC identity: every field except the credential —
    //! evidence can never perturb identity.
    uint256 Id() const
    {
        HashWriter h;
        h << std::string{"b3/flowmesh/action/v2"} << signer << sequence << type;
        h << static_cast<uint64_t>(curve.size());
        for (const ClearingEngine::Breakpoint& bp : curve) h << bp.price << bp.qty;
        h << asset << amount << destination << outpoint;
        return h.GetHash();
    }

    //! Canonical SEMANTIC shape (credential excluded): bounds, plus
    //! every field a type does not use held at its zero value.
    bool ShapeIsCanonicalSansCredential() const
    {
        if (curve.size() > MAX_ACTION_CURVE_POINTS) return false;
        switch (static_cast<ActionType>(type)) {
        case ActionType::SUBMIT_BID:
        case ActionType::SUBMIT_ASK:
            return !signer.IsNull() && !curve.empty() && asset.IsNull() && amount == 0 &&
                   destination.IsNull() && outpoint.IsNull();
        case ActionType::CANCEL_BID:
        case ActionType::CANCEL_ASK:
            return !signer.IsNull() && curve.empty() && asset.IsNull() && amount == 0 &&
                   destination.IsNull() && outpoint.IsNull();
        case ActionType::WITHDRAW:
            return !signer.IsNull() && curve.empty() && amount > 0 && amount <= MAX_MONEY &&
                   !destination.IsNull() && outpoint.IsNull();
        case ActionType::DEPOSIT:
            return signer.IsNull() && sequence == 0 && curve.empty() && asset.IsNull() &&
                   amount == 0 && destination.IsNull() && !outpoint.IsNull();
        case ActionType::SPOT_TO_FUTURES:
        case ActionType::FUTURES_TO_SPOT:
            return false; // reserved for the futures domain; rejected in v1
        }
        return false;
    }

    //! Transport-level shape (pool admission): semantic shape plus this
    //! entry's credential bounds. A DEPOSIT is chain-authorized and
    //! carries no credential.
    bool ShapeIsCanonical() const
    {
        if (!ShapeIsCanonicalSansCredential()) return false;
        if (credential.size() > MAX_ACTION_CREDENTIAL_SIZE) return false;
        if (static_cast<ActionType>(type) == ActionType::DEPOSIT && !credential.empty()) {
            return false;
        }
        return true;
    }

    bool IsDeposit() const { return static_cast<ActionType>(type) == ActionType::DEPOSIT; }
};

//! Strict-weak canonical SEMANTIC order: deposits first by outpoint,
//! then signed entries by (signer, sequence, id). Total over distinct
//! semantic ids; credentials play no part.
inline bool ActionCanonicalLess(const Action& a, const Action& b)
{
    const bool a_deposit{a.IsDeposit()};
    const bool b_deposit{b.IsDeposit()};
    if (a_deposit != b_deposit) return a_deposit;
    if (a_deposit) return a.outpoint < b.outpoint;
    if (a.signer != b.signer) return a.signer < b.signer;
    if (a.sequence != b.sequence) return a.sequence < b.sequence;
    return a.Id() < b.Id();
}

/**
 * THE canonical semantic representation of a candidate action set:
 * credentials STRIPPED, deduplicated by semantic id, sorted by
 * ActionCanonicalLess. Pure and idempotent. One logical action set has
 * exactly one canonical body — and therefore exactly one microblock
 * hash — no matter how many credential variants or arrival orders were
 * observed.
 */
inline std::vector<Action> CanonicalizeActions(std::vector<Action> actions)
{
    for (Action& action : actions) action.credential.clear();
    std::stable_sort(actions.begin(), actions.end(), ActionCanonicalLess);
    actions.erase(std::unique(actions.begin(), actions.end(),
                              [](const Action& a, const Action& b) {
                                  return a.Id() == b.Id();
                              }),
                  actions.end());
    return actions;
}

//! Whether `actions` is already the canonical semantic body: strictly
//! increasing in the canonical order AND credential-free.
inline bool ActionsAreCanonical(const std::vector<Action>& actions)
{
    for (size_t i{0}; i < actions.size(); ++i) {
        if (!actions[i].credential.empty()) return false;
        if (i > 0 && !ActionCanonicalLess(actions[i - 1], actions[i])) return false;
    }
    return true;
}

/**
 * The authorization boundary for SIGNED actions — a PRE-ADMISSION
 * filter (pool admission, proposal-evidence verification), never part
 * of execution.
 *
 * CONTRACT: Authenticate must be DETERMINISTIC (a pure function of the
 * action bytes, identical on every node) and SIDE-EFFECT-FREE.
 */
class ActionAuthenticator
{
public:
    virtual ~ActionAuthenticator() = default;
    virtual bool Authenticate(const Action& action) const = 0;
    //! The IMMUTABLE binding this authenticator judges under. Consumers
    //! (MeshNode construction, store replay) verify these against their
    //! own validated domain and execution configuration, so a
    //! wrong-market or wrong-domain authenticator fails EXPLICITLY at
    //! wiring time — never by accident downstream.
    virtual const uint256& DomainId() const = 0;
    virtual const uint256& ExecConfigId() const = 0;
};

enum class ActionReject : uint8_t {
    //! Pre-admission concept only (pool / proposal evidence); an
    //! executed microblock can never contain this outcome. Retained for
    //! numbering stability.
    UNAUTHENTICATED = 0,
    EQUIVOCATION = 1,
    BAD_SEQUENCE = 2,
    REJECTED_BY_STATE = 3,
    MALFORMED = 4,
};

struct BatchResult {
    uint64_t slot{0};
    //! Action ids applied, in canonical execution order.
    std::vector<uint256> applied;
    //! Rejections, sorted by (action id, reason) — canonical.
    std::vector<std::pair<uint256, ActionReject>> rejected;
    //! Withdrawal REQUESTS created this slot (REQUESTED stage; see the
    //! lifecycle note in ledger.h — nothing here is B3-redeemable).
    std::vector<modern::WithdrawalReceipt> withdrawal_requests;
    uint256 request_root;
    //! PURE resulting state root: FlowMeshState::Root() after the slot.
    uint256 state_root;
    //! Commitment to what EXECUTING this slot did (applied/rejected
    //! sets, requests, clearing outcome). Separate from the state root.
    uint256 result_commitment;
    ClearingEngine::ClearingResult clearing;
};

class BatchExecutor
{
public:
    //! `deposits` may be null: every DEPOSIT action is then rejected by
    //! state (fail closed) — custody facts must come from a verifier.
    explicit BatchExecutor(FlowMeshState& state, const DepositVerifier* deposits = nullptr)
        : m_state{state}, m_deposits{deposits}
    {
    }

    uint64_t NextSequence(const AccountId& signer) const { return m_state.NextSequence(signer); }

    /**
     * Execute one slot over a candidate action set, against `anchor`.
     * The set is reduced to canonical semantic form first, so the
     * result is a pure function of the SET. No authentication happens
     * here (pre-admission concern).
     *
     * Per-id disposition (deterministic):
     *  - semantic shape invalid                    -> MALFORMED
     *  - same-signer same-sequence different ids   -> EQUIVOCATION
     *    (all; sequence does not advance)
     *  - wrong sequence                            -> BAD_SEQUENCE
     *  - applied or refused by state               -> sequence consumed
     *    either way (REJECTED_BY_STATE semantics unchanged).
     *
     * Returns std::nullopt on a FATAL internal failure (clearing or
     * settlement inconsistency): the caller must discard this state
     * copy. Ordinary rejections are never fatal.
     */
    [[nodiscard]] std::optional<BatchResult> ExecuteSlot(const std::vector<Action>& actions,
                                                         const AnchorRef& anchor = {})
    {
        BatchResult result;
        result.slot = m_state.Slot();

        const std::vector<Action> canonical{CanonicalizeActions(actions)};

        std::map<COutPoint, const Action*> deposit_entries;
        std::map<std::pair<AccountId, uint64_t>, std::vector<const Action*>> groups;
        for (const Action& action : canonical) {
            if (!action.ShapeIsCanonicalSansCredential()) {
                result.rejected.emplace_back(action.Id(), ActionReject::MALFORMED);
                continue;
            }
            if (action.IsDeposit()) {
                deposit_entries.emplace(action.outpoint, &action);
                continue;
            }
            groups[{action.signer, action.sequence}].push_back(&action);
        }

        std::vector<const Action*> ordered;
        for (const auto& [key, group] : groups) {
            if (group.size() > 1) {
                for (const Action* action : group) {
                    result.rejected.emplace_back(action->Id(), ActionReject::EQUIVOCATION);
                }
                continue;
            }
            ordered.push_back(group.front());
        }
        // groups iterate by (signer, sequence): already canonical order.

        // Deposits, in outpoint order.
        for (const auto& [outpoint, action_ptr] : deposit_entries) {
            const uint256 id{action_ptr->Id()};
            if (ApplyDeposit(*action_ptr, anchor)) {
                result.applied.push_back(id);
            } else {
                result.rejected.emplace_back(id, ActionReject::REJECTED_BY_STATE);
            }
        }

        // Signed survivors.
        for (const Action* action_ptr : ordered) {
            const Action& action{*action_ptr};
            const uint256 id{action.Id()};
            if (action.sequence != m_state.NextSequence(action.signer)) {
                result.rejected.emplace_back(id, ActionReject::BAD_SEQUENCE);
                continue;
            }

            bool ok{false};
            switch (static_cast<ActionType>(action.type)) {
            case ActionType::SUBMIT_BID:
                ok = m_state.SubmitCurve(action.signer, ClearingEngine::Side::BID, action.curve);
                break;
            case ActionType::SUBMIT_ASK:
                ok = m_state.SubmitCurve(action.signer, ClearingEngine::Side::ASK, action.curve);
                break;
            case ActionType::CANCEL_BID:
                ok = m_state.CancelCurve(action.signer, ClearingEngine::Side::BID);
                break;
            case ActionType::CANCEL_ASK:
                ok = m_state.CancelCurve(action.signer, ClearingEngine::Side::ASK);
                break;
            case ActionType::WITHDRAW: {
                const std::optional<modern::WithdrawalReceipt> request{
                    m_state.RequestWithdrawal(action.signer, action.asset, action.amount,
                                              action.destination)};
                if (request) result.withdrawal_requests.push_back(*request);
                ok = request.has_value();
                break;
            }
            case ActionType::DEPOSIT:
                break; // unreachable: deposits were split off above
            case ActionType::SPOT_TO_FUTURES:
            case ActionType::FUTURES_TO_SPOT:
                ok = false; // unreachable: shape validation rejects reserved types
                break;
            }

            m_state.AdvanceSequence(action.signer, action.sequence + 1);
            if (ok) {
                result.applied.push_back(id);
            } else {
                result.rejected.emplace_back(id, ActionReject::REJECTED_BY_STATE);
            }
        }

        std::sort(result.rejected.begin(), result.rejected.end());

        // ONE clearing pass per slot (advances the ledger slot). A fatal
        // clearing/settlement failure poisons this whole candidate.
        const std::optional<ClearingEngine::ClearingResult> clearing{m_state.ClearSlot()};
        if (!clearing) return std::nullopt;
        result.clearing = *clearing;

        {
            HashWriter h;
            h << std::string{"b3/flowmesh/receipts/v1"} << result.slot
              << static_cast<uint64_t>(result.withdrawal_requests.size());
            for (const modern::WithdrawalReceipt& request : result.withdrawal_requests) {
                h << request;
            }
            result.request_root = h.GetHash();
        }

        result.state_root = m_state.Root();
        {
            HashWriter h;
            h << std::string{"b3/flowmesh/execresult/v1"} << result.slot;
            h << static_cast<uint64_t>(result.applied.size());
            for (const uint256& id : result.applied) h << id;
            h << static_cast<uint64_t>(result.rejected.size());
            for (const auto& [id, reason] : result.rejected) {
                h << id << static_cast<uint8_t>(reason);
            }
            h << result.request_root;
            h << result.clearing.cleared << result.clearing.price << result.clearing.volume
              << result.clearing.imbalance;
            h << static_cast<uint64_t>(result.clearing.bid_fill.size());
            for (const auto& [account, fill] : result.clearing.bid_fill) h << account << fill;
            h << static_cast<uint64_t>(result.clearing.ask_fill.size());
            for (const auto& [account, fill] : result.clearing.ask_fill) h << account << fill;
            result.result_commitment = h.GetHash();
        }

        return result;
    }

private:
    bool ApplyDeposit(const Action& action, const AnchorRef& anchor)
    {
        // consumed-deposit state is provisional model state only.
        // OWNER DECISION REQUIRED: retain or defer consumed-deposit
        // state, and define same-slot deposit/trading semantics.
        // Production deposits stay fail-closed regardless (null
        // verifier), and no additional semantics are added here.
        if (m_deposits == nullptr) return false;
        if (m_state.DepositConsumed(action.outpoint)) return false;
        const std::optional<DepositInfo> info{m_deposits->GetDeposit(action.outpoint, anchor)};
        if (!info) return false;
        return m_state.CreditDeposit(action.outpoint, info->account, info->asset, info->amount);
    }

    FlowMeshState& m_state;
    const DepositVerifier* m_deposits;
};

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_BATCH_H
