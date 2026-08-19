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
 * A slot's actions arrive as an unordered set; nothing about execution
 * depends on network arrival order. The executor canonicalizes the set,
 * authenticates every signed action through an opaque credential
 * interface, enforces per-signer sequencing, applies deposits (in
 * outpoint order) then signed survivors (in (signer, sequence, id)
 * order), clears the slot ONCE through the uniform-price engine, and
 * produces the pure resulting state root plus a separate execution-result
 * commitment. Identical action sets over identical states always produce
 * identical results, byte for byte.
 */

//! Consensus-stable action type numbering; append only.
enum class ActionType : uint8_t {
    SUBMIT_BID = 0,
    SUBMIT_ASK = 1,
    CANCEL_BID = 2,
    CANCEL_ASK = 3,
    WITHDRAW = 4,
    DEPOSIT = 5,
};

//! Structural bounds, enforced before any cryptography.
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
    //! chain data — an action-supplied claim of them is never trusted,
    //! so there is nowhere to even carry one.
    COutPoint outpoint;
    //! Opaque certificate/signature material, judged only by the
    //! ActionAuthenticator. Not part of the action id.
    std::vector<unsigned char> credential;

    SERIALIZE_METHODS(Action, obj)
    {
        READWRITE(obj.signer, obj.sequence, obj.type, obj.curve, obj.asset, obj.amount,
                  obj.destination, obj.outpoint, obj.credential);
    }

    //! Canonical identity of what is being authorized: every field except
    //! the credential. (v2: the DEPOSIT outpoint joined the preimage.)
    uint256 Id() const
    {
        HashWriter h;
        h << std::string{"b3/flowmesh/action/v2"} << signer << sequence << type;
        h << static_cast<uint64_t>(curve.size());
        for (const ClearingEngine::Breakpoint& bp : curve) h << bp.price << bp.qty;
        h << asset << amount << destination << outpoint;
        return h.GetHash();
    }

    /**
     * Canonical shape: bounds, plus every field a type does not use held
     * at its zero value — one byte representation per meaning. A DEPOSIT
     * is chain-authorized: no signer, no sequence, no credential, only
     * the outpoint. Pure function; checked before authentication.
     */
    bool ShapeIsCanonical() const
    {
        if (curve.size() > MAX_ACTION_CURVE_POINTS) return false;
        if (credential.size() > MAX_ACTION_CREDENTIAL_SIZE) return false;
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
                   amount == 0 && destination.IsNull() && !outpoint.IsNull() &&
                   credential.empty();
        }
        return false;
    }
};

/**
 * The consensus/certificate cryptography boundary for SIGNED actions.
 * Implementations decide what a valid credential is; the batch layer only
 * ever asks yes or no. DEPOSIT actions never reach it.
 *
 * CONTRACT: Authenticate must be DETERMINISTIC (a pure function of the
 * action bytes, identical on every node) and SIDE-EFFECT-FREE — the
 * executor calls it in canonical credential order and may call it any
 * number of times; nothing about consensus may depend on how often or
 * in what request order a node happens to invoke it.
 */
class ActionAuthenticator
{
public:
    virtual ~ActionAuthenticator() = default;
    virtual bool Authenticate(const Action& action) const = 0;
};

enum class ActionReject : uint8_t {
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
    //! Withdrawal receipts created this slot, in canonical order.
    std::vector<modern::WithdrawalReceipt> receipts;
    uint256 receipt_root;
    //! PURE resulting state root: FlowMeshState::Root() after the slot —
    //! a function of state alone.
    uint256 state_root;
    //! Commitment to what EXECUTING this slot did (applied/rejected sets,
    //! receipts, clearing outcome). Kept separate from the state root so
    //! state identity never depends on how it was reached.
    uint256 result_commitment;
    ClearingEngine::ClearingResult clearing;
};

class BatchExecutor
{
public:
    //! `deposits` may be null: every DEPOSIT action is then rejected by
    //! state (fail closed) — custody facts must come from a verifier.
    BatchExecutor(FlowMeshState& state, const ActionAuthenticator& auth,
                  const DepositVerifier* deposits = nullptr)
        : m_state{state}, m_auth{auth}, m_deposits{deposits}
    {
    }

    uint64_t NextSequence(const AccountId& signer) const { return m_state.NextSequence(signer); }

    /**
     * Execute one slot over an unordered action set, against `anchor`
     * (the B3 position deposits are judged at).
     *
     * Canonicalization is a pure function of the action SET:
     *
     *  1. Actions dedupe by id; credential variants are canonicalized
     *     (deduplicated, sorted) and an id authenticates iff ANY variant
     *     does.
     *  2. Malformed shapes are rejected before anything else.
     *  3. Authentication runs before equivocation grouping, so a forged
     *     action can never manufacture an equivocation against an honest
     *     signer. DEPOSITs skip both: they carry no signer and are
     *     authorized by the chain via the verifier.
     *  4. Execution order: DEPOSITs first, in outpoint order (chain
     *     events fund same-slot trading), then signed survivors sorted by
     *     (signer, sequence, id).
     *
     * Sequencing: a signed action must carry exactly the signer's next
     * sequence. Applied actions and actions rejected by state both
     * consume their sequence, like nonces; malformed, unauthenticated,
     * equivocating and wrong-sequence actions do not.
     */
    BatchResult ExecuteSlot(const std::vector<Action>& actions, const AnchorRef& anchor = {})
    {
        BatchResult result;
        result.slot = m_state.ledger.Slot();

        // 1. Canonicalize by id (std::map: arrival-order independent).
        std::map<uint256, std::pair<const Action*, std::set<std::vector<unsigned char>>>> by_id;
        for (const Action& action : actions) {
            auto& entry{by_id[action.Id()]};
            entry.first = &action; // equal id => equal action fields
            entry.second.insert(action.credential);
        }

        // 2./3. Shape, then authentication (signed actions only), then
        // equivocation grouping over authenticated signed actions.
        std::map<COutPoint, const Action*> deposits;
        std::map<std::pair<AccountId, uint64_t>, std::vector<const Action*>> groups;
        for (const auto& [id, entry] : by_id) {
            const auto& [representative, credentials]{entry};
            if (!representative->ShapeIsCanonical()) {
                result.rejected.emplace_back(id, ActionReject::MALFORMED);
                continue;
            }
            if (static_cast<ActionType>(representative->type) == ActionType::DEPOSIT) {
                // Distinct ids cannot share an outpoint unless some other
                // field differs — and every other field is pinned to zero
                // by the canonical shape — so this insert cannot collide.
                deposits.emplace(representative->outpoint, representative);
                continue;
            }
            bool authenticated{false};
            for (const std::vector<unsigned char>& credential : credentials) {
                Action candidate{*representative};
                candidate.credential = credential;
                if (m_auth.Authenticate(candidate)) {
                    authenticated = true;
                    break;
                }
            }
            if (!authenticated) {
                result.rejected.emplace_back(id, ActionReject::UNAUTHENTICATED);
                continue;
            }
            groups[{representative->signer, representative->sequence}].push_back(representative);
        }

        std::vector<const Action*> ordered;
        for (const auto& [key, group] : groups) {
            if (group.size() > 1) {
                // Same-sequence equivocation: reject the whole group and
                // do not advance the sequence.
                for (const Action* action : group) {
                    result.rejected.emplace_back(action->Id(), ActionReject::EQUIVOCATION);
                }
                continue;
            }
            ordered.push_back(group.front());
        }
        std::stable_sort(ordered.begin(), ordered.end(),
                         [](const Action* a, const Action* b) {
                             if (a->signer != b->signer) return a->signer < b->signer;
                             if (a->sequence != b->sequence) return a->sequence < b->sequence;
                             return a->Id() < b->Id();
                         });

        // 4a. Deposits, in outpoint order.
        for (const auto& [outpoint, action_ptr] : deposits) {
            const uint256 id{action_ptr->Id()};
            if (ApplyDeposit(*action_ptr, anchor)) {
                result.applied.push_back(id);
            } else {
                result.rejected.emplace_back(id, ActionReject::REJECTED_BY_STATE);
            }
        }

        // 4b. Signed survivors.
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
                ok = m_state.book.SubmitCurve(action.signer, ClearingEngine::Side::BID,
                                              action.curve);
                break;
            case ActionType::SUBMIT_ASK:
                ok = m_state.book.SubmitCurve(action.signer, ClearingEngine::Side::ASK,
                                              action.curve);
                break;
            case ActionType::CANCEL_BID:
                ok = m_state.book.CancelCurve(action.signer, ClearingEngine::Side::BID);
                break;
            case ActionType::CANCEL_ASK:
                ok = m_state.book.CancelCurve(action.signer, ClearingEngine::Side::ASK);
                break;
            case ActionType::WITHDRAW: {
                const std::optional<modern::WithdrawalReceipt> receipt{
                    m_state.ledger.FinalizeWithdrawal(action.signer, action.asset, action.amount,
                                                      action.destination)};
                if (receipt) result.receipts.push_back(*receipt);
                ok = receipt.has_value();
                break;
            }
            case ActionType::DEPOSIT:
                break; // unreachable: deposits were split off above
            }

            // Applied or state-rejected: the sequence is consumed either
            // way, so a rejected action cannot be silently retried.
            m_state.next_seq[action.signer] = action.sequence + 1;
            if (ok) {
                result.applied.push_back(id);
            } else {
                result.rejected.emplace_back(id, ActionReject::REJECTED_BY_STATE);
            }
        }

        std::sort(result.rejected.begin(), result.rejected.end());

        // ONE clearing pass per slot (advances the ledger slot).
        result.clearing = m_state.book.ClearSlot();

        // Receipt root: a one-time commitment to the withdrawal receipts
        // created this slot, each already carrying a unique deterministic id.
        {
            HashWriter h;
            h << std::string{"b3/flowmesh/receipts/v1"} << result.slot
              << static_cast<uint64_t>(result.receipts.size());
            for (const modern::WithdrawalReceipt& receipt : result.receipts) h << receipt;
            result.receipt_root = h.GetHash();
        }

        // Pure state root, and the separate execution-result commitment.
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
            h << result.receipt_root;
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
        if (m_deposits == nullptr) return false; // fail closed: no verifier, no custody facts
        if (m_state.consumed_deposits.count(action.outpoint) > 0) return false;
        const std::optional<DepositInfo> info{m_deposits->GetDeposit(action.outpoint, anchor)};
        if (!info) return false;
        if (!m_state.ledger.Deposit(info->account, info->asset, info->amount)) return false;
        m_state.consumed_deposits.insert(action.outpoint);
        return true;
    }

    FlowMeshState& m_state;
    const ActionAuthenticator& m_auth;
    const DepositVerifier* m_deposits;
};

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_BATCH_H
