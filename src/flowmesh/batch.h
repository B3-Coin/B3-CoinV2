// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_BATCH_H
#define B3COIN_FLOWMESH_BATCH_H

#include <consensus/amount.h>
#include <flowmesh/clearing.h>
#include <flowmesh/ledger.h>
#include <hash.h>
#include <modern/policy.h>
#include <modern/vault.h>
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
 * Canonical batch execution for one FlowMesh slot.
 *
 * A slot's actions arrive as an unordered set; nothing about execution
 * depends on network arrival order. The executor canonicalizes the set,
 * authenticates every action through an opaque credential interface,
 * enforces per-signer sequencing, applies the survivors in canonical
 * order, clears and settles the slot, and produces a deterministic
 * receipt root and resulting state root. Identical action sets always
 * produce identical results, byte for byte.
 */

enum class ActionType : uint8_t {
    SUBMIT_BID = 0,
    SUBMIT_ASK = 1,
    CANCEL_BID = 2,
    CANCEL_ASK = 3,
    WITHDRAW = 4,
};

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
    //! Opaque certificate/signature material, judged only by the
    //! ActionAuthenticator. Not part of the action id.
    std::vector<unsigned char> credential;

    //! Canonical identity of what is being authorized: every field except
    //! the credential.
    uint256 Id() const
    {
        HashWriter h;
        h << std::string{"b3/flowmesh/action/v1"} << signer << sequence << type;
        h << static_cast<uint64_t>(curve.size());
        for (const ClearingEngine::Breakpoint& bp : curve) h << bp.price << bp.qty;
        h << asset << amount << destination;
        return h.GetHash();
    }
};

/**
 * The consensus/certificate cryptography boundary. Implementations decide
 * what a valid credential is — a single signature, a quorum certificate,
 * a threshold scheme — and none of those semantics are defined here. The
 * batch layer only ever asks yes or no about an action's credential.
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
    uint256 state_root;
    ClearingEngine::ClearingResult clearing;
};

class BatchExecutor
{
public:
    BatchExecutor(Ledger& ledger, ClearingEngine& engine, const ActionAuthenticator& auth)
        : m_ledger{ledger}, m_engine{engine}, m_auth{auth} {}

    uint64_t NextSequence(const AccountId& signer) const
    {
        const auto it{m_next_seq.find(signer)};
        return it == m_next_seq.end() ? 0 : it->second;
    }

    /**
     * Execute one slot over an unordered action set.
     *
     * Canonicalization is a pure function of the action SET — nothing
     * depends on arrival order:
     *
     *  1. Actions dedupe by id. The credential is not part of the id, so
     *     one id may arrive with several credentials; the action
     *     authenticates iff ANY supplied credential authenticates. (A
     *     first-arrival rule here would make the outcome depend on
     *     ordering, and a canonical-credential rule would let a griefer
     *     shadow a valid submission with a junk credential.)
     *  2. Authentication runs FIRST. Unauthenticated ids are rejected
     *     and take no further part — so a forged action can never
     *     manufacture an equivocation against an honest signer.
     *  3. Two DIFFERENT authenticated actions from one signer at the
     *     same sequence are equivocation: every such id is rejected and
     *     the sequence does not advance. Survivors execute sorted by
     *     (signer, sequence, id).
     *
     * Sequencing: an action must carry exactly the signer's next sequence.
     * Applied actions and actions rejected by state (insufficient funds,
     * invalid curve) both consume their sequence, like nonces; failed
     * authentication, equivocation and wrong sequences do not.
     */
    BatchResult ExecuteSlot(const std::vector<Action>& actions)
    {
        BatchResult result;
        result.slot = m_ledger.Slot();

        // 1. Canonicalize by id (std::map: arrival-order independent).
        // The credential is outside the id, so one id may arrive with
        // several credential variants: they are CANONICALIZED too —
        // deduplicated exactly and sorted lexicographically — so every
        // node performs the same authentication calls in the same order
        // no matter how the network delivered the set.
        std::map<uint256, std::pair<const Action*, std::set<std::vector<unsigned char>>>> by_id;
        for (const Action& action : actions) {
            auto& entry{by_id[action.Id()]};
            entry.first = &action; // equal id => equal action fields
            entry.second.insert(action.credential);
        }

        // 2. Authenticate each id in canonical credential order: any
        // valid credential vouches for the id. Only authenticated
        // actions proceed to equivocation grouping.
        std::map<std::pair<AccountId, uint64_t>, std::vector<const Action*>> groups;
        for (const auto& [id, entry] : by_id) {
            const auto& [representative, credentials]{entry};
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

        // 3. Equivocation over AUTHENTICATED actions only.
        std::vector<const Action*> ordered;
        for (const auto& [key, group] : groups) {
            if (group.size() > 1) {
                // Same-sequence equivocation: reject the whole group and do
                // not advance the sequence.
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

        for (const Action* action_ptr : ordered) {
            const Action& action{*action_ptr};
            const uint256 id{action.Id()};
            if (action.sequence != NextSequence(action.signer)) {
                result.rejected.emplace_back(id, ActionReject::BAD_SEQUENCE);
                continue;
            }

            bool ok{false};
            switch (static_cast<ActionType>(action.type)) {
            case ActionType::SUBMIT_BID:
                ok = m_engine.SubmitCurve(action.signer, ClearingEngine::Side::BID, action.curve);
                break;
            case ActionType::SUBMIT_ASK:
                ok = m_engine.SubmitCurve(action.signer, ClearingEngine::Side::ASK, action.curve);
                break;
            case ActionType::CANCEL_BID:
                ok = m_engine.CancelCurve(action.signer, ClearingEngine::Side::BID);
                break;
            case ActionType::CANCEL_ASK:
                ok = m_engine.CancelCurve(action.signer, ClearingEngine::Side::ASK);
                break;
            case ActionType::WITHDRAW: {
                const std::optional<modern::WithdrawalReceipt> receipt{
                    m_ledger.FinalizeWithdrawal(action.signer, action.asset, action.amount,
                                                action.destination)};
                if (receipt) result.receipts.push_back(*receipt);
                ok = receipt.has_value();
                break;
            }
            }

            // Applied or state-rejected: the sequence is consumed either
            // way, so a rejected action cannot be silently retried.
            m_next_seq[action.signer] = action.sequence + 1;
            if (ok) {
                result.applied.push_back(id);
            } else {
                result.rejected.emplace_back(id, ActionReject::REJECTED_BY_STATE);
            }
        }

        std::sort(result.rejected.begin(), result.rejected.end());

        // Settle the slot (clears the persistent book and advances the
        // ledger slot).
        result.clearing = m_engine.ClearSlot();

        // Receipt root: a one-time commitment to the withdrawal receipts
        // created this slot, each already carrying a unique deterministic id.
        {
            HashWriter h;
            h << std::string{"b3/flowmesh/receipts/v1"} << result.slot
              << static_cast<uint64_t>(result.receipts.size());
            for (const modern::WithdrawalReceipt& receipt : result.receipts) h << receipt;
            result.receipt_root = h.GetHash();
        }

        // Resulting state root: binds the applied set, the canonical
        // rejections, the receipt root, every signer's next sequence, and
        // the post-settlement engine/ledger state.
        {
            HashWriter h;
            h << std::string{"b3/flowmesh/batch/v1"} << result.slot;
            h << static_cast<uint64_t>(result.applied.size());
            for (const uint256& id : result.applied) h << id;
            h << static_cast<uint64_t>(result.rejected.size());
            for (const auto& [id, reason] : result.rejected) {
                h << id << static_cast<uint8_t>(reason);
            }
            h << result.receipt_root;
            h << static_cast<uint64_t>(m_next_seq.size());
            for (const auto& [signer, seq] : m_next_seq) h << signer << seq;
            h << m_engine.StateRoot();
            result.state_root = h.GetHash();
        }

        return result;
    }

private:
    Ledger& m_ledger;
    ClearingEngine& m_engine;
    const ActionAuthenticator& m_auth;
    std::map<AccountId, uint64_t> m_next_seq;
};

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_BATCH_H
