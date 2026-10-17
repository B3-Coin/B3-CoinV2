// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/admission/admission.h>

#include <flowmesh2/core/nonce_rule.h>
#include <flowmesh2/engine/engine.h>
#include <flowmesh2/ledger/ledger.h>
#include <flowmesh2/ledger/nonce_window.h>

#include <algorithm>
#include <utility>

namespace flowmesh2::admission {

Result<void> KindAdmissible(ActionKind kind)
{
    switch (kind) {
    case ActionKind::PLACE:
    case ActionKind::CANCEL:
    case ActionKind::TRANSFER:
        return Ok();
    case ActionKind::CANCEL_ALL:
        return Unsupported("M3-ENG-09"); // TODO(rebirth:M3-ENG-09) admit CANCEL_ALL once the engine executes it
    case ActionKind::WITHDRAW:
        return Unsupported("M2-LED-05"); // TODO(rebirth:M2-LED-05) admit WITHDRAW once the ledger reserves withdrawals
    case ActionKind::SPOT_TO_FUTURES:
    case ActionKind::FUTURES_TO_SPOT:
        return Unsupported("M2-LED-06"); // TODO(rebirth:M2-LED-06) admit the spot/futures transfers with their risk gate
    }
    return Error{ErrorCode::INVALID_INPUT, ADMISSION_KIND_UNREGISTERED};
}

bool PayloadAdmissible(const ActionV2& action)
{
    bool decodes{false};
    switch (action.kind) {
    case ActionKind::PLACE: decodes = action.Place().has_value(); break;
    case ActionKind::CANCEL: decodes = action.Cancel().has_value(); break;
    case ActionKind::TRANSFER: decodes = action.Transfer().has_value(); break;
    case ActionKind::CANCEL_ALL:
    case ActionKind::WITHDRAW:
    case ActionKind::SPOT_TO_FUTURES:
    case ActionKind::FUTURES_TO_SPOT:
        return false;
    }
    return decodes && engine::PhaseOf(action).has_value();
}

std::optional<Refusal> WindowRefusal(uint64_t ref_slot, uint16_t max_delay, uint64_t head_height, const ProfileParams& profile)
{
    // TODO(rebirth:M2-ADM-01) the D7 model and exhaustive tests replace this reference rule.
    if (max_delay < 1 || max_delay > profile.max_delay_slots) return Refusal{AdmissionCode::BAD_EXPIRY, REASON_BAD_EXPIRY};
    if (ref_slot > head_height) return Refusal{AdmissionCode::REF_SLOT_AHEAD, REASON_REF_SLOT_AHEAD};
    if (head_height == UINT64_MAX) return Refusal{AdmissionCode::EXPIRED, REASON_HEIGHT_EXHAUSTED};
    if (ExpiredAt(ref_slot, max_delay, head_height + 1)) return Refusal{AdmissionCode::EXPIRED, REASON_EXPIRED};
    return std::nullopt;
}

uint64_t AdmissionStats::Count(AdmissionCode code) const
{
    const auto it{by_code.find(code)};
    return it == by_code.end() ? 0 : it->second;
}

namespace {

AdmissionReply Reply(AdmissionCode code, std::string_view reason)
{
    AdmissionReply reply;
    reply.code = code;
    reply.reason = reason;
    return reply;
}

AdmissionReply Reply(AdmissionReply reply, AdmissionCode code, std::string_view reason)
{
    reply.code = code;
    reply.reason = reason;
    return reply;
}

} // namespace

Result<std::unique_ptr<Admission>> Admission::Create(const ProfileParams& profile, const state::StateOwner& owner,
                                                     const read::ReadService& read, ActionPool& pool, BudgetPolicy& budget)
{
    const Result<void> valid{Validate(profile)};
    if (!valid.has_value()) return valid.error();
    const uint256 hash{ProfileHash(profile)};
    if (ProfileHash(owner.Profile()) != hash || ProfileHash(read.Profile()) != hash || ProfileHash(pool.Profile()) != hash) {
        return Error{ErrorCode::INVALID_INPUT, ADMISSION_PROFILE_MISMATCH};
    }
    const std::shared_ptr<const state::HeadView> head{owner.Head()};
    if (!head) return Error{ErrorCode::INVARIANT, ADMISSION_NO_HEAD};
    return Ok(std::unique_ptr<Admission>{new Admission{profile, head->Header().domain, owner, read, pool, budget}});
}

Admission::Admission(const ProfileParams& profile, const uint256& domain, const state::StateOwner& owner, const read::ReadService& read,
                     ActionPool& pool, BudgetPolicy& budget)
    : m_profile{profile}, m_domain{domain}, m_owner{owner}, m_read{read}, m_pool{pool}, m_budget{budget}
{
}

Result<AdmissionReply> Admission::Admit(std::span<const unsigned char> frame)
{
    Attempt attempt;
    Result<AdmissionReply> reply{Evaluate(frame, attempt)};
    if (attempt.budget_allowed) {
        m_budget.Settle(*attempt.budget_allowed, reply.has_value() && reply->code == AdmissionCode::ADMITTED);
    }
    if (!reply.has_value()) {
        ++m_errors;
        return reply;
    }
    ++m_replies;
    const auto index{static_cast<size_t>(reply->code)};
    if (index < m_by_code.size()) ++m_by_code[index];
    return reply;
}

Result<std::vector<AdmissionReply>> Admission::AdmitBatch(std::span<const std::vector<unsigned char>>)
{
    // TODO(rebirth:M4-ADM-01) A8 in batches of <= 64 (BatchVerifier, CCheckQueue, CuckooCache); A0..A7 and A9..A10 unchanged.
    return NotImplemented("M4-ADM-01");
}

AdmissionStats Admission::Stats() const
{
    AdmissionStats stats;
    stats.sig_verifications = m_sig_verifications.load();
    stats.replies = m_replies.load();
    stats.errors = m_errors.load();
    for (const CodeInfo<AdmissionCode>& info : AdmissionCodeRegistry()) {
        const auto index{static_cast<size_t>(info.code)};
        stats.by_code[info.code] = index < m_by_code.size() ? m_by_code[index].load() : 0;
    }
    return stats;
}

AdmissionReply Admission::UnknownKind(std::span<const unsigned char> frame) const
{
    // The kind byte has a fixed position (core/action.h), so the frame and the
    // domain are judged with a registered kind in its place: A1 and the A2
    // domain check keep their precedence over UNKNOWN_KIND.
    std::vector<unsigned char> probe{frame.begin(), frame.end()};
    probe[ACTION_KIND_OFFSET] = static_cast<unsigned char>(ActionKind::PLACE);
    const Result<ActionV2> decoded{ActionV2::Decode(probe, m_profile)};
    if (!decoded.has_value()) {
        if (decoded.error().code == ErrorCode::OVERSIZE) return Reply(AdmissionCode::OVERSIZE, REASON_FRAME_OVERSIZE);
        return Reply(AdmissionCode::MALFORMED, REASON_FRAME_MALFORMED);
    }
    if (decoded->Encode() != probe) return Reply(AdmissionCode::MALFORMED, REASON_FRAME_MALFORMED);
    if (decoded->domain != m_domain) return Reply(AdmissionCode::WRONG_DOMAIN, REASON_WRONG_DOMAIN);
    return Reply(AdmissionCode::UNKNOWN_KIND, REASON_UNKNOWN_KIND);
}

Result<std::optional<read::IncludedOutcome>> Admission::IncludedOutcomeOf(const ActionV2& action) const
{
    const Result<read::ActionReply> reply{m_read.Action(action)};
    if (!reply.has_value()) {
        // BUSY or NEED_DATA: the read service cannot answer now, which is not a
        // refusal of the action; anything else is a broken invariant.
        if (reply.error().code == ErrorCode::BUSY || reply.error().code == ErrorCode::NEED_DATA) return Ok(std::optional<read::IncludedOutcome>{});
        return reply.error();
    }
    if (reply->status != read::ActionStatus::INCLUDED || !reply->included) return Ok(std::optional<read::IncludedOutcome>{});
    return Ok(std::optional<read::IncludedOutcome>{*reply->included});
}

Result<AdmissionReply> Admission::Duplicate(AdmissionReply reply, const KnownAction& known, const ActionV2& action) const
{
    if (known.included) {
        reply = Reply(std::move(reply), AdmissionCode::DUPLICATE, REASON_DUPLICATE_INCLUDED);
        reply.included = known.included;
        Result<std::optional<read::IncludedOutcome>> outcome{IncludedOutcomeOf(action)};
        if (!outcome.has_value()) return outcome.error();
        if (*outcome && (*outcome)->ref == *known.included) reply.outcome = std::move(*outcome);
        return Ok(std::move(reply));
    }
    reply = Reply(std::move(reply), AdmissionCode::DUPLICATE, REASON_DUPLICATE_PENDING);
    reply.pending = known.pending;
    return Ok(std::move(reply));
}

Result<AdmissionReply> Admission::Evaluate(std::span<const unsigned char> frame, Attempt& attempt)
{
    // A0: fenced.
    if (m_fenced.load()) return Ok(Reply(AdmissionCode::HALTED, REASON_HALTED));

    // A1: frame caps and exact decode; A2: version, domain, kind and payload.
    Result<ActionV2> decoded{ActionV2::Decode(frame, m_profile)};
    if (!decoded.has_value()) {
        const Error& error{decoded.error()};
        if (error.code == ErrorCode::OVERSIZE) return Ok(Reply(AdmissionCode::OVERSIZE, REASON_FRAME_OVERSIZE));
        if (error.code == ErrorCode::DECODE && error.detail == ACTION_UNKNOWN_VERSION) {
            return Ok(Reply(AdmissionCode::WRONG_DOMAIN, REASON_UNKNOWN_VERSION));
        }
        if (error.code == ErrorCode::DECODE && error.detail == ACTION_UNKNOWN_KIND) return Ok(UnknownKind(frame));
        return Ok(Reply(AdmissionCode::MALFORMED, REASON_FRAME_MALFORMED));
    }
    const ActionV2 action{std::move(decoded).value()};
    // Exactness: the pool stores Encode(), which must be the client's bytes.
    const std::vector<unsigned char> encoded{action.Encode()};
    if (!std::equal(frame.begin(), frame.end(), encoded.begin(), encoded.end())) {
        return Ok(Reply(AdmissionCode::MALFORMED, REASON_FRAME_MALFORMED));
    }
    if (action.domain != m_domain) return Ok(Reply(AdmissionCode::WRONG_DOMAIN, REASON_WRONG_DOMAIN));
    if (const Result<void> kind{KindAdmissible(action.kind)}; !kind.has_value()) {
        // TODO(rebirth:M2-LED-05) TODO(rebirth:M3-ENG-09) TODO(rebirth:M2-LED-06) each task makes its kind admissible.
        return Ok(Reply(AdmissionCode::UNSUPPORTED_KIND, kind.error().detail));
    }
    if (!PayloadAdmissible(action)) return Ok(Reply(AdmissionCode::MALFORMED, REASON_PAYLOAD_MALFORMED));
    const ActionId id{action.Id()};
    const AccountId account_id{action.Account()};
    AdmissionReply reply;
    reply.id = id;

    // A3: an original this node already authenticated (no signature check).
    if (const std::optional<KnownAction> known{m_pool.FindAuthenticated(id, action.sig)}) return Duplicate(std::move(reply), *known, action);

    std::optional<VerifiedAction> verified;
    for (int attempt_index{0}; attempt_index < MAX_SNAPSHOT_ATTEMPTS; ++attempt_index) {
        // One snapshot for A4..A9.
        const std::shared_ptr<const state::HeadView> head{m_owner.Head()};
        if (!head) return Error{ErrorCode::INVARIANT, ADMISSION_NO_HEAD};
        const uint64_t height{head->Header().height};
        reply.head_height = height;

        // A4: the account exists.
        const ledger::AccountState* account{head->State().engine.ledger.Find(account_id)};
        if (account == nullptr) return Ok(Reply(std::move(reply), AdmissionCode::NO_ACCOUNT, REASON_NO_ACCOUNT));
        // A5: the nonce is not below the window (USED waits for the signature, A9).
        const NonceVerdict nonce{account->nonces.Classify(action.nonce)};
        if (nonce == NonceVerdict::TOO_LOW) return Ok(Reply(std::move(reply), AdmissionCode::NONCE_TOO_LOW, REASON_NONCE_TOO_LOW));
        // A6: the window against the next slot.
        if (const std::optional<Refusal> window{WindowRefusal(action.ref_slot, action.max_delay, height, m_profile)}) {
            return Ok(Reply(std::move(reply), window->code, window->reason));
        }
        // A7: the budget (once per Admit).
        if (!attempt.budget_allowed) {
            const BudgetRequest request{account_id, action.kind, frame.size(), height, account->open_orders};
            const Result<BudgetVerdict> verdict{m_budget.Check(request)};
            if (!verdict.has_value()) return verdict.error();
            if (*verdict != BudgetVerdict::ALLOW) return Ok(Reply(std::move(reply), AdmissionCode::BUDGET, REASON_BUDGET));
            attempt.budget_allowed = request;
        }
        // A8: BIP340 (once per Admit).
        if (!verified) {
            ++m_sig_verifications;
            Result<VerifiedAction> checked{VerifiedAction::Verify(action)};
            if (!checked.has_value()) return Ok(Reply(std::move(reply), AdmissionCode::BAD_SIG, REASON_BAD_SIG));
            verified.emplace(std::move(checked).value());
        }
        // A9: the same ActionId with another valid signature, live or included.
        if (const std::optional<KnownAction> known{m_pool.FindById(id)}) return Duplicate(std::move(reply), *known, action);
        Result<std::optional<read::IncludedOutcome>> indexed{IncludedOutcomeOf(action)};
        if (!indexed.has_value()) return indexed.error();
        if (*indexed) {
            reply = Reply(std::move(reply), AdmissionCode::DUPLICATE, REASON_DUPLICATE_INCLUDED);
            reply.included = (*indexed)->ref;
            reply.outcome = std::move(*indexed);
            return Ok(std::move(reply));
        }
        // A9: the nonce is used at the snapshot.
        if (nonce == NonceVerdict::USED) return Ok(Reply(std::move(reply), AdmissionCode::NONCE_USED, REASON_NONCE_USED));
        // A9 (pool side) and A10, atomically.
        const InsertReply inserted{m_pool.Insert(*verified, height)};
        switch (inserted.result) {
        case InsertResult::INSERTED:
            reply = Reply(std::move(reply), AdmissionCode::ADMITTED, REASON_ADMITTED);
            reply.pending = inserted.pending;
            return Ok(std::move(reply));
        case InsertResult::DUPLICATE:
            return Duplicate(std::move(reply), KnownAction{inserted.included ? PoolStatus::INCLUDED : PoolStatus::PENDING, inserted.pending,
                                                           inserted.included},
                             action);
        case InsertResult::NONCE_PENDING:
            return Ok(Reply(std::move(reply), AdmissionCode::NONCE_PENDING, REASON_NONCE_PENDING));
        case InsertResult::POOL_FULL:
            return Ok(Reply(std::move(reply), AdmissionCode::POOL_FULL, REASON_POOL_FULL));
        case InsertResult::STALE_SNAPSHOT:
            continue; // the pool committed a newer head since this snapshot: check again
        }
        return Error{ErrorCode::INVARIANT, ADMISSION_INSERT_RESULT};
    }
    return Ok(Reply(std::move(reply), AdmissionCode::BUSY, REASON_BUSY_SNAPSHOT));
}

} // namespace flowmesh2::admission
