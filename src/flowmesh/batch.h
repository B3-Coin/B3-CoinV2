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
 * CANONICAL FORM FIRST: a candidate action set has exactly one
 * canonical representation (CanonicalizeActions) — deposits in outpoint
 * order, then signed entries by (signer, sequence, id, credential),
 * exact duplicates removed. Identity (the microblock actions root) is
 * defined over that form, execution consumes that form, and validation
 * rejects any non-canonical body — so the same logical candidate set
 * yields the same encoded body, the same hash and the same result, no
 * matter how the network delivered it.
 *
 * Execution: authenticate signed entries through an opaque credential
 * interface (an id may carry several credential variants; the outcome is
 * a pure function of the variant SET), enforce per-signer sequencing,
 * apply deposits then signed survivors, clear the slot ONCE through the
 * uniform-price engine, and produce the pure resulting state root plus a
 * separate execution-result commitment.
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
    //! chain data — an action-supplied claim of them is never trusted,
    //! so there is nowhere to even carry one.
    COutPoint outpoint;
    //! Opaque authorization material, judged only by the
    //! ActionAuthenticator. Not part of the action id.
    std::vector<unsigned char> credential;

    //! Bounded strict codec: vector counts are checked BEFORE their
    //! elements are read, so attacker-sized counts cannot drive
    //! allocation past the FlowMesh limits.
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
     * Canonical shape EXCLUDING the credential: bounds, plus every field
     * a type does not use held at its zero value. Credential validity is
     * judged per VARIANT (an id may arrive with several credentials), so
     * it is deliberately outside this check — a malformed variant must
     * never change the disposition of a well-formed one.
     */
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
        }
        return false;
    }

    //! Full canonical shape of one entry (pool admission etc.): the
    //! sans-credential rules plus this entry's credential bounds. A
    //! DEPOSIT is chain-authorized and carries no credential.
    bool ShapeIsCanonical() const
    {
        if (!ShapeIsCanonicalSansCredential()) return false;
        if (credential.size() > MAX_ACTION_CREDENTIAL_SIZE) return false;
        if (static_cast<ActionType>(type) == ActionType::DEPOSIT && !credential.empty()) {
            return false;
        }
        return true;
    }
};

//! Strict-weak canonical entry order: deposits first by outpoint, then
//! signed entries by (signer, sequence, id, credential). Total over
//! distinct (id, credential) pairs.
inline bool ActionCanonicalLess(const Action& a, const Action& b)
{
    const bool a_deposit{static_cast<ActionType>(a.type) == ActionType::DEPOSIT};
    const bool b_deposit{static_cast<ActionType>(b.type) == ActionType::DEPOSIT};
    if (a_deposit != b_deposit) return a_deposit;
    if (a_deposit) {
        if (!(a.outpoint == b.outpoint)) return a.outpoint < b.outpoint;
        return a.credential < b.credential; // non-canonical variants still order totally
    }
    if (a.signer != b.signer) return a.signer < b.signer;
    if (a.sequence != b.sequence) return a.sequence < b.sequence;
    const uint256 ida{a.Id()}, idb{b.Id()};
    if (ida != idb) return ida < idb;
    return a.credential < b.credential;
}

/**
 * THE canonical representation of a candidate action set: sorted by
 * ActionCanonicalLess with exact duplicates (same id AND same
 * credential) removed. Pure and idempotent; identity, execution and
 * validation are all defined over this form, so network arrival order
 * can never influence a microblock hash or an execution result.
 */
inline std::vector<Action> CanonicalizeActions(std::vector<Action> actions)
{
    std::stable_sort(actions.begin(), actions.end(), ActionCanonicalLess);
    actions.erase(std::unique(actions.begin(), actions.end(),
                              [](const Action& a, const Action& b) {
                                  return a.Id() == b.Id() && a.credential == b.credential;
                              }),
                  actions.end());
    return actions;
}

//! Whether `actions` is already in canonical form (strictly increasing
//! in the canonical order). Cheap: one linear scan.
inline bool ActionsAreCanonical(const std::vector<Action>& actions)
{
    for (size_t i{1}; i < actions.size(); ++i) {
        if (!ActionCanonicalLess(actions[i - 1], actions[i])) return false;
    }
    return true;
}

/**
 * The authorization boundary for SIGNED actions. Implementations decide
 * what a valid credential is; this layer only ever asks yes or no.
 * DEPOSIT actions never reach it.
 *
 * CONTRACT: Authenticate must be DETERMINISTIC (a pure function of the
 * action bytes, identical on every node) and SIDE-EFFECT-FREE; it is
 * called in canonical credential order and may be called any number of
 * times.
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
    //! Withdrawal REQUESTS created this slot (REQUESTED stage; see the
    //! lifecycle note in ledger.h — nothing here is B3-redeemable).
    std::vector<modern::WithdrawalReceipt> withdrawal_requests;
    uint256 request_root;
    //! PURE resulting state root: FlowMeshState::Root() after the slot.
    uint256 state_root;
    //! Commitment to what EXECUTING this slot did (applied/rejected sets,
    //! requests, clearing outcome). Kept separate from the state root so
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
     * Execute one slot over a candidate action set, against `anchor`.
     * The set is reduced to canonical form first, so the result is a
     * pure function of the SET.
     *
     * Per-id disposition (deterministic in the variant set):
     *  - sans-credential shape invalid            -> MALFORMED
     *  - DEPOSIT with only non-empty credentials  -> MALFORMED
     *  - signed, no size-valid variant passes auth-> UNAUTHENTICATED
     *  - same-signer same-sequence different ids  -> EQUIVOCATION (all;
     *    sequence does not advance)
     *  - wrong sequence                           -> BAD_SEQUENCE
     *  - applied or refused by state              -> sequence consumed
     *    either way (a state-rejected action cannot be silently
     *    retried); this REJECTED_BY_STATE semantic is unchanged.
     *
     * Returns std::nullopt on a FATAL internal failure (clearing or
     * settlement inconsistency): the caller must discard this state
     * copy — nothing may be committed from it. Ordinary rejections are
     * NOT fatal and never roll anything back.
     */
    [[nodiscard]] std::optional<BatchResult> ExecuteSlot(const std::vector<Action>& actions,
                                                         const AnchorRef& anchor = {})
    {
        BatchResult result;
        result.slot = m_state.ledger.Slot();

        const std::vector<Action> canonical{CanonicalizeActions(actions)};

        // Group canonical entries into per-id variant runs. Within an
        // id, entries differ only in credential (every other field is in
        // the id) and are credential-sorted by canonical order.
        std::map<COutPoint, const Action*> deposit_entries;
        std::map<std::pair<AccountId, uint64_t>, std::vector<const Action*>> groups;
        for (size_t i{0}; i < canonical.size();) {
            const Action& first{canonical[i]};
            const uint256 id{first.Id()};
            size_t j{i};
            while (j < canonical.size() && canonical[j].Id() == id) ++j;

            if (!first.ShapeIsCanonicalSansCredential()) {
                result.rejected.emplace_back(id, ActionReject::MALFORMED);
                i = j;
                continue;
            }
            if (static_cast<ActionType>(first.type) == ActionType::DEPOSIT) {
                // A deposit is authorized by the chain: only the
                // canonical empty-credential entry is meaningful; junk
                // variants beside it change nothing.
                bool canonical_variant{false};
                for (size_t k{i}; k < j; ++k) {
                    canonical_variant = canonical_variant || canonical[k].credential.empty();
                }
                if (!canonical_variant) {
                    result.rejected.emplace_back(id, ActionReject::MALFORMED);
                } else {
                    deposit_entries.emplace(first.outpoint, &first);
                }
                i = j;
                continue;
            }
            // Signed: authenticate against the size-valid variants in
            // canonical credential order; ANY acceptance vouches for the
            // id. Oversized variants are skipped deterministically and
            // can never shadow a valid one.
            bool authenticated{false};
            for (size_t k{i}; k < j && !authenticated; ++k) {
                if (canonical[k].credential.size() > MAX_ACTION_CREDENTIAL_SIZE) continue;
                authenticated = m_auth.Authenticate(canonical[k]);
            }
            if (!authenticated) {
                result.rejected.emplace_back(id, ActionReject::UNAUTHENTICATED);
                i = j;
                continue;
            }
            groups[{first.signer, first.sequence}].push_back(&first);
            i = j;
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
                ok = m_state.book.SubmitCurve(m_state.ledger, action.signer,
                                              ClearingEngine::Side::BID, action.curve);
                break;
            case ActionType::SUBMIT_ASK:
                ok = m_state.book.SubmitCurve(m_state.ledger, action.signer,
                                              ClearingEngine::Side::ASK, action.curve);
                break;
            case ActionType::CANCEL_BID:
                ok = m_state.book.CancelCurve(m_state.ledger, action.signer,
                                              ClearingEngine::Side::BID);
                break;
            case ActionType::CANCEL_ASK:
                ok = m_state.book.CancelCurve(m_state.ledger, action.signer,
                                              ClearingEngine::Side::ASK);
                break;
            case ActionType::WITHDRAW: {
                const std::optional<modern::WithdrawalReceipt> request{
                    m_state.ledger.RequestWithdrawal(action.signer, action.asset, action.amount,
                                                     action.destination)};
                if (request) result.withdrawal_requests.push_back(*request);
                ok = request.has_value();
                break;
            }
            case ActionType::DEPOSIT:
                break; // unreachable: deposits were split off above
            }

            m_state.next_seq[action.signer] = action.sequence + 1;
            if (ok) {
                result.applied.push_back(id);
            } else {
                result.rejected.emplace_back(id, ActionReject::REJECTED_BY_STATE);
            }
        }

        std::sort(result.rejected.begin(), result.rejected.end());

        // ONE clearing pass per slot (advances the ledger slot). A fatal
        // clearing/settlement failure poisons this whole candidate.
        const std::optional<ClearingEngine::ClearingResult> clearing{
            m_state.book.ClearSlot(m_state.ledger)};
        if (!clearing) return std::nullopt;
        result.clearing = *clearing;

        // Request root: a commitment to the withdrawal requests created
        // this slot, each already carrying a unique deterministic id.
        {
            HashWriter h;
            h << std::string{"b3/flowmesh/receipts/v1"} << result.slot
              << static_cast<uint64_t>(result.withdrawal_requests.size());
            for (const modern::WithdrawalReceipt& request : result.withdrawal_requests) {
                h << request;
            }
            result.request_root = h.GetHash();
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
