// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <flowmesh/outcome_evidence.h>

#include <hash.h>
#include <serialize.h>
#include <streams.h>
#include <util/int128.h>

#include <algorithm>
#include <exception>
#include <map>
#include <set>
#include <string>
#include <type_traits>

namespace flowmesh {
namespace {

// Minimum encoded sizes, used to bound every count before allocation.
constexpr size_t APPLIED_ITEM{32};
constexpr size_t REJECTED_ITEM{33};
constexpr size_t FILL_ITEM{40};
constexpr size_t SELLER_ITEM{32 + 24};
constexpr size_t SEAT_ITEM{40};
constexpr size_t EFFECT_ITEM{1 + modern::FLOWMESH_WITHDRAWAL_RECEIPT_V1_SIZE};

bool ValidReject(const ActionReject reason)
{
    // UNAUTHENTICATED is a pre-admission concept; execution never emits it.
    return reason == ActionReject::EQUIVOCATION || reason == ActionReject::BAD_SEQUENCE ||
           reason == ActionReject::REJECTED_BY_STATE || reason == ActionReject::MALFORMED;
}

template <typename T>
bool StrictlyAscendingAccounts(const std::vector<T>& items)
{
    for (size_t i{1}; i < items.size(); ++i) {
        if constexpr (std::is_same_v<T, SellerFeeShare>) {
            if (!(items[i - 1].account < items[i].account)) return false;
        } else {
            if (!(items[i - 1].first < items[i].first)) return false;
        }
    }
    return true;
}

//! Only what a canonical encoding needs; the hash binds every value.
bool PreimageIsCanonical(const ExecutionOutcomePreimageV1& p)
{
    if (p.applied.size() > FLOWMESH_V1_MAX_MICROBLOCK_ACTIONS ||
        p.rejected.size() > FLOWMESH_V1_MAX_MICROBLOCK_ACTIONS - p.applied.size()) {
        return false;
    }
    for (size_t i{0}; i < p.rejected.size(); ++i) {
        if (!ValidReject(p.rejected[i].second)) return false;
        if (i > 0 && !(p.rejected[i - 1] < p.rejected[i])) return false;
    }
    return StrictlyAscendingAccounts(p.bid_fill) && StrictlyAscendingAccounts(p.ask_fill) &&
           StrictlyAscendingAccounts(p.seller_fees);
}

class Reader {
public:
    explicit Reader(std::span<const unsigned char> bytes) : m_reader{bytes} {}
    size_t Remaining() const { return m_reader.size(); }
    bool Empty() const { return m_reader.empty(); }
    template <typename T> Reader& operator>>(T& value)
    {
        m_reader >> value;
        return *this;
    }
    //! A count whose items cannot fit in the remaining bytes is refused
    //! before anything is reserved.
    uint64_t Count(const size_t item_bytes, const size_t maximum)
    {
        const uint64_t count{ReadCompactSize(m_reader)};
        if (count > maximum || count > Remaining() / item_bytes) {
            throw std::ios_base::failure("FlowMesh outcome evidence count exceeds its bound");
        }
        return count;
    }
    std::vector<unsigned char> Bytes(const size_t size)
    {
        if (size > Remaining()) throw std::ios_base::failure("FlowMesh outcome evidence is truncated");
        std::vector<unsigned char> out(size);
        m_reader.read(std::as_writable_bytes(std::span{out}));
        return out;
    }
    uint64_t CompactSize() { return ReadCompactSize(m_reader); }

private:
    SpanReader m_reader;
};

const Action* BodyAction(const std::map<uint256, const Action*>& body, const uint256& id)
{
    const auto it{body.find(id)};
    return it == body.end() ? nullptr : it->second;
}

std::optional<CAmount> CheckedQuote(const CAmount quantity, const CAmount price)
{
    if (quantity < 0 || price < 0) return std::nullopt;
    const util::Unsigned128 product{static_cast<util::Unsigned128>(quantity) *
                                    static_cast<util::Unsigned128>(price)};
    if (product > static_cast<util::Unsigned128>(MAX_MONEY)) return std::nullopt;
    return static_cast<CAmount>(product);
}

std::optional<CAmount> FindFill(const std::vector<std::pair<AccountId, CAmount>>& fills,
                                const AccountId& account)
{
    const auto it{std::lower_bound(fills.begin(), fills.end(), account,
                                   [](const auto& item, const AccountId& key) { return item.first < key; })};
    if (it == fills.end() || it->first != account) return std::nullopt;
    return it->second;
}

bool IsSubmit(const ActionType type)
{
    return type == ActionType::SUBMIT_BID || type == ActionType::SUBMIT_ASK;
}

bool SameSide(const ActionType a, const ActionType b)
{
    const auto bid = [](ActionType t) { return t == ActionType::SUBMIT_BID || t == ActionType::CANCEL_BID; };
    const auto ask = [](ActionType t) { return t == ActionType::SUBMIT_ASK || t == ActionType::CANCEL_ASK; };
    return (bid(a) && bid(b)) || (ask(a) && ask(b));
}

bool IsWithdrawal(const ActionType type)
{
    return type == ActionType::WITHDRAW || type == ActionType::CLAIM_SEAT_REWARD;
}

} // namespace

ExecutionOutcomePreimageV1 MakeExecutionOutcomePreimageV1(const BatchResult& result)
{
    return MakeExecutionOutcomePreimageV1(BatchResult{result});
}

ExecutionOutcomePreimageV1 MakeExecutionOutcomePreimageV1(BatchResult&& result)
{
    ExecutionOutcomePreimageV1 out;
    out.slot = result.slot;
    out.applied = std::move(result.applied);
    out.rejected = std::move(result.rejected);
    out.request_root = result.request_root;
    const auto& clearing{result.clearing};
    out.cleared = clearing.cleared;
    out.price = clearing.price;
    out.volume = clearing.volume;
    out.imbalance = clearing.imbalance;
    out.bid_fill.assign(clearing.bid_fill.begin(), clearing.bid_fill.end());
    out.ask_fill.assign(clearing.ask_fill.begin(), clearing.ask_fill.end());
    out.matched_b3_quote_notional = clearing.fees.matched_b3_quote_notional;
    out.fee_total = clearing.fees.fee_total;
    out.treasury_fee = clearing.fees.treasury_fee;
    out.seat_fee = clearing.fees.seat_fee;
    out.seller_fees = std::move(result.clearing.fees.seller_fees);
    out.seat_rewards = std::move(result.clearing.fees.seat_rewards);
    return out;
}

uint256 ComputeExecResultCommitmentV1(const ExecutionOutcomePreimageV1& p)
{
    // Mirrors BatchExecutor::ExecuteSlot's "b3/flowmesh/execresult/v1" hash
    // byte for byte; the lockstep test compares both over random batches.
    HashWriter h;
    h << std::string{"b3/flowmesh/execresult/v1"} << p.slot;
    h << static_cast<uint64_t>(p.applied.size());
    for (const uint256& id : p.applied) h << id;
    h << static_cast<uint64_t>(p.rejected.size());
    for (const auto& [id, reason] : p.rejected) h << id << static_cast<uint8_t>(reason);
    h << p.request_root;
    h << p.cleared << p.price << p.volume << p.imbalance;
    h << static_cast<uint64_t>(p.bid_fill.size());
    for (const auto& [account, fill] : p.bid_fill) h << account << fill;
    h << static_cast<uint64_t>(p.ask_fill.size());
    for (const auto& [account, fill] : p.ask_fill) h << account << fill;
    h << p.matched_b3_quote_notional << p.fee_total << p.treasury_fee << p.seat_fee;
    h << static_cast<uint64_t>(p.seller_fees.size());
    for (const SellerFeeShare& share : p.seller_fees) {
        h << share.account << share.gross_quote_proceeds << share.fee << share.net_quote_proceeds;
    }
    h << static_cast<uint64_t>(p.seat_rewards.size());
    for (const SeatFeeShare& share : p.seat_rewards) h << share.seat_id << share.reward;
    return h.GetHash();
}

std::optional<std::vector<unsigned char>> EncodeExecutionOutcomeEvidenceV1(
    const uint64_t entry_sequence, const uint256& entry_hash,
    const ExecutionOutcomePreimageV1& p, const std::span<const modern::FlowMeshEffectV1> effects)
{
    if (!PreimageIsCanonical(p) || effects.size() > modern::FLOWMESH_MAX_CHECKPOINT_EFFECTS) return std::nullopt;
    // O(1) upper bound before writing anything.
    const size_t estimate{128 + APPLIED_ITEM * p.applied.size() + REJECTED_ITEM * p.rejected.size() +
                          FILL_ITEM * (p.bid_fill.size() + p.ask_fill.size()) +
                          SELLER_ITEM * p.seller_fees.size() + SEAT_ITEM * p.seat_rewards.size() +
                          (EFFECT_ITEM + 8) * effects.size() + 6 * 9};
    if (estimate > FLOWMESH_OUTCOME_EVIDENCE_MAX_BYTES) return std::nullopt;
    std::vector<unsigned char> out;
    out.reserve(estimate);
    try {
        VectorWriter w{out, 0};
        w << OUTCOME_EVIDENCE_VERSION_V1 << entry_sequence << entry_hash << p.slot;
        WriteCompactSize(w, p.applied.size());
        for (const uint256& id : p.applied) w << id;
        WriteCompactSize(w, p.rejected.size());
        for (const auto& [id, reason] : p.rejected) w << id << static_cast<uint8_t>(reason);
        w << p.request_root << static_cast<uint8_t>(p.cleared ? 1 : 0) << p.price << p.volume << p.imbalance;
        WriteCompactSize(w, p.bid_fill.size());
        for (const auto& [account, fill] : p.bid_fill) w << account << fill;
        WriteCompactSize(w, p.ask_fill.size());
        for (const auto& [account, fill] : p.ask_fill) w << account << fill;
        w << p.matched_b3_quote_notional << p.fee_total << p.treasury_fee << p.seat_fee;
        WriteCompactSize(w, p.seller_fees.size());
        for (const SellerFeeShare& share : p.seller_fees) {
            w << share.account << share.gross_quote_proceeds << share.fee << share.net_quote_proceeds;
        }
        WriteCompactSize(w, p.seat_rewards.size());
        for (const SeatFeeShare& share : p.seat_rewards) w << share.seat_id << share.reward;
        WriteCompactSize(w, effects.size());
        for (const auto& effect : effects) {
            const auto bytes{modern::EncodeFlowMeshEffectV1(effect)};
            if (!bytes) return std::nullopt;
            WriteCompactSize(w, bytes->size());
            w.write(std::as_bytes(std::span{*bytes}));
        }
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (out.size() > FLOWMESH_OUTCOME_EVIDENCE_MAX_BYTES) return std::nullopt;
    return out;
}

std::optional<std::vector<unsigned char>> EncodeExecutionOutcomeEvidenceV1(
    const ExecutionOutcomeEvidenceV1& evidence)
{
    return EncodeExecutionOutcomeEvidenceV1(evidence.entry_sequence, evidence.entry_hash,
                                            evidence.preimage, evidence.effects);
}

std::optional<ExecutionOutcomeEvidenceV1> DecodeExecutionOutcomeEvidenceV1(
    const std::span<const unsigned char> bytes)
{
    if (bytes.empty() || bytes.size() > FLOWMESH_OUTCOME_EVIDENCE_MAX_BYTES) return std::nullopt;
    try {
        Reader r{bytes};
        uint8_t version{0};
        r >> version;
        if (version != OUTCOME_EVIDENCE_VERSION_V1) return std::nullopt;
        ExecutionOutcomeEvidenceV1 out;
        auto& p{out.preimage};
        r >> out.entry_sequence >> out.entry_hash >> p.slot;
        const uint64_t applied{r.Count(APPLIED_ITEM, FLOWMESH_V1_MAX_MICROBLOCK_ACTIONS)};
        p.applied.resize(applied);
        for (auto& id : p.applied) r >> id;
        const uint64_t rejected{r.Count(REJECTED_ITEM, FLOWMESH_V1_MAX_MICROBLOCK_ACTIONS - applied)};
        p.rejected.resize(rejected);
        for (auto& [id, reason] : p.rejected) {
            uint8_t code{0};
            r >> id >> code;
            if (code > static_cast<uint8_t>(ActionReject::MALFORMED)) return std::nullopt;
            reason = static_cast<ActionReject>(code);
        }
        uint8_t cleared{0};
        r >> p.request_root >> cleared >> p.price >> p.volume >> p.imbalance;
        if (cleared > 1) return std::nullopt;
        p.cleared = cleared == 1;
        for (auto* fills : {&p.bid_fill, &p.ask_fill}) {
            const uint64_t count{r.Count(FILL_ITEM, FLOWMESH_OUTCOME_EVIDENCE_MAX_BYTES)};
            fills->resize(count);
            for (auto& [account, fill] : *fills) r >> account >> fill;
        }
        r >> p.matched_b3_quote_notional >> p.fee_total >> p.treasury_fee >> p.seat_fee;
        p.seller_fees.resize(r.Count(SELLER_ITEM, FLOWMESH_OUTCOME_EVIDENCE_MAX_BYTES));
        for (auto& share : p.seller_fees) {
            r >> share.account >> share.gross_quote_proceeds >> share.fee >> share.net_quote_proceeds;
        }
        p.seat_rewards.resize(r.Count(SEAT_ITEM, FLOWMESH_OUTCOME_EVIDENCE_MAX_BYTES));
        for (auto& share : p.seat_rewards) r >> share.seat_id >> share.reward;
        const uint64_t effects{r.Count(EFFECT_ITEM, modern::FLOWMESH_MAX_CHECKPOINT_EFFECTS)};
        out.effects.reserve(effects);
        for (uint64_t i{0}; i < effects; ++i) {
            const uint64_t size{r.CompactSize()};
            if (size != modern::FLOWMESH_DEPOSIT_ACCEPTANCE_V1_SIZE &&
                size != modern::FLOWMESH_WITHDRAWAL_RECEIPT_V1_SIZE) {
                return std::nullopt;
            }
            const auto effect{modern::DecodeFlowMeshEffectV1(r.Bytes(size))};
            if (!effect) return std::nullopt;
            out.effects.push_back(*effect);
        }
        if (!r.Empty() || !PreimageIsCanonical(p)) return std::nullopt;
        return out;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

OutcomeEvidenceCheck VerifyExecutionOutcomeEvidence(const ProductionEntryCore& entry,
                                                    const ExecutionOutcomeEvidenceV1& evidence)
{
    if (entry.kind != static_cast<uint8_t>(ProductionEntryKind::EXECUTION) ||
        evidence.entry_sequence != entry.sequence || evidence.entry_hash != entry.GetHash()) {
        return OutcomeEvidenceCheck::WRONG_ENTRY;
    }
    const auto root{ComputeProductionExecutionResultRoot(
        ComputeExecResultCommitmentV1(evidence.preimage), evidence.effects)};
    if (!root || *root != entry.result_root) return OutcomeEvidenceCheck::RESULT_ROOT_MISMATCH;
    const auto effect_root{modern::ComputeFlowMeshEffectRoot(entry.effect_start, evidence.effects)};
    if (entry.effect_count != evidence.effects.size() || !effect_root || *effect_root != entry.effect_root) {
        return OutcomeEvidenceCheck::EFFECT_ROOT_MISMATCH;
    }
    // Deterministic execution disposes of every canonical body action once.
    std::set<uint256> body;
    for (const Action& action : entry.actions) body.insert(action.Id());
    std::set<uint256> seen;
    for (const uint256& id : evidence.preimage.applied) {
        if (!body.contains(id) || !seen.insert(id).second) return OutcomeEvidenceCheck::BODY_MISMATCH;
    }
    for (const auto& [id, reason] : evidence.preimage.rejected) {
        (void)reason;
        if (!body.contains(id) || !seen.insert(id).second) return OutcomeEvidenceCheck::BODY_MISMATCH;
    }
    return seen.size() == body.size() ? OutcomeEvidenceCheck::OK : OutcomeEvidenceCheck::BODY_MISMATCH;
}

std::optional<DerivedActionOutcome> DeriveActionOutcome(const ProductionEntryCore& entry,
                                                        const ExecutionOutcomeEvidenceV1& evidence,
                                                        const Action& action)
{
    const auto& p{evidence.preimage};
    const uint256 self{action.Id()};
    std::map<uint256, const Action*> body;
    for (const Action& item : entry.actions) body.emplace(item.Id(), &item);
    const Action* in_body{BodyAction(body, self)};
    if (!in_body) return std::nullopt;
    const ActionType type{static_cast<ActionType>(action.type)};
    DerivedActionOutcome out;

    const auto applied_at{std::find(p.applied.begin(), p.applied.end(), self)};
    if (applied_at == p.applied.end()) {
        const auto rejected{std::find_if(p.rejected.begin(), p.rejected.end(),
                                         [&](const auto& item) { return item.first == self; })};
        if (rejected == p.rejected.end()) return std::nullopt;
        switch (rejected->second) {
        case ActionReject::REJECTED_BY_STATE:
            out.status = ActionOutcomeStatus::REJECTED;
            // A deposit's outpoint is not consumed: it may be credited later.
            out.final = !action.IsDeposit();
            out.code = action.IsDeposit() ? ActionReasonCode::DEPOSIT_NOT_CREDITED
                                           : ActionReasonCode::REJECTED_BY_STATE;
            return out;
        case ActionReject::BAD_SEQUENCE:
            if (action.IsDeposit()) return std::nullopt;
            out.status = ActionOutcomeStatus::INCLUDED_NOT_EXECUTED;
            out.code = ActionReasonCode::INCLUDED_BAD_SEQUENCE;
            return out;
        case ActionReject::EQUIVOCATION:
            if (action.IsDeposit()) return std::nullopt;
            out.status = ActionOutcomeStatus::INCLUDED_NOT_EXECUTED;
            out.code = ActionReasonCode::INCLUDED_EQUIVOCATION;
            return out;
        case ActionReject::MALFORMED:
            // Shape is a pure function of the id's content.
            out.status = ActionOutcomeStatus::INCLUDED_NOT_EXECUTED;
            out.final = true;
            out.code = ActionReasonCode::INCLUDED_MALFORMED;
            return out;
        case ActionReject::UNAUTHENTICATED:
            return std::nullopt;
        }
        return std::nullopt;
    }

    out.status = ActionOutcomeStatus::APPLIED;
    out.final = true;
    out.code = ActionReasonCode::APPLIED;
    if (action.IsDeposit()) {
        for (const auto& effect : evidence.effects) {
            const auto* deposit{std::get_if<modern::FlowMeshDepositAcceptanceV1>(&effect)};
            if (deposit && deposit->deposit_outpoint == action.outpoint) {
                out.deposit = *deposit;
                break;
            }
        }
        return out;
    }
    if (IsSubmit(type)) {
        ActionFill fill;
        fill.side = type == ActionType::SUBMIT_BID ? ClearingEngine::Side::BID : ClearingEngine::Side::ASK;
        fill.cleared = p.cleared;
        for (auto later{std::next(applied_at)}; later != p.applied.end(); ++later) {
            const Action* other{BodyAction(body, *later)};
            if (!other) return std::nullopt;
            if (!other->IsDeposit() && other->signer == action.signer &&
                SameSide(type, static_cast<ActionType>(other->type))) {
                fill.superseded_in_entry = true;
            }
        }
        if (!fill.superseded_in_entry) {
            const auto& fills{fill.side == ClearingEngine::Side::BID ? p.bid_fill : p.ask_fill};
            fill.quantity = FindFill(fills, action.signer).value_or(0);
            fill.price = p.cleared ? p.price : 0;
            const auto quote{CheckedQuote(fill.quantity, fill.price)};
            if (!quote) return std::nullopt;
            if (fill.side == ClearingEngine::Side::BID) {
                fill.quote_atoms = *quote;
                fill.net_quote_atoms = *quote;
            } else {
                const auto seller{std::find_if(p.seller_fees.begin(), p.seller_fees.end(),
                                               [&](const auto& share) { return share.account == action.signer; })};
                if (seller != p.seller_fees.end()) {
                    fill.quote_atoms = seller->gross_quote_proceeds;
                    fill.fee_atoms = seller->fee;
                    fill.net_quote_atoms = seller->net_quote_proceeds;
                } else if (fill.quantity > 0 && fill.price > 0) {
                    return std::nullopt; // a paid ask always has a seller share
                } else {
                    fill.quote_atoms = *quote;
                    fill.net_quote_atoms = *quote;
                }
            }
        }
        out.fill = fill;
        return out;
    }
    if (IsWithdrawal(type)) {
        // The k-th applied withdrawal of this signer, in execution order, is
        // the k-th withdrawal receipt effect for this account.
        size_t k{0};
        for (auto earlier{p.applied.begin()}; earlier != applied_at; ++earlier) {
            const Action* other{BodyAction(body, *earlier)};
            if (!other) return std::nullopt;
            if (!other->IsDeposit() && other->signer == action.signer &&
                IsWithdrawal(static_cast<ActionType>(other->type))) {
                ++k;
            }
        }
        for (const auto& effect : evidence.effects) {
            const auto* receipt{std::get_if<modern::FlowMeshWithdrawalReceiptV1>(&effect)};
            if (!receipt || receipt->account != action.signer) continue;
            if (k-- != 0) continue;
            if (receipt->asset == action.asset && receipt->amount == action.amount &&
                receipt->destination_owner_commitment == action.destination) {
                out.withdrawal = *receipt;
            }
            break;
        }
    }
    return out;
}

bool ProvesSequenceConsumedByOther(const ProductionEntryCore& entry,
                                   const ExecutionOutcomeEvidenceV1& evidence,
                                   const Action& action, const uint256& consumer_id)
{
    if (action.IsDeposit() || consumer_id == action.Id()) return false;
    const auto consumer{std::find_if(entry.actions.begin(), entry.actions.end(),
                                     [&](const Action& item) { return item.Id() == consumer_id; })};
    if (consumer == entry.actions.end() || consumer->IsDeposit() ||
        consumer->signer != action.signer || consumer->sequence != action.sequence) {
        return false;
    }
    const auto& p{evidence.preimage};
    if (std::find(p.applied.begin(), p.applied.end(), consumer_id) != p.applied.end()) return true;
    return std::find(p.rejected.begin(), p.rejected.end(),
                     std::make_pair(consumer_id, ActionReject::REJECTED_BY_STATE)) != p.rejected.end();
}

} // namespace flowmesh
