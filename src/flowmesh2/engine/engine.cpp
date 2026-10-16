// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/engine/engine.h>

#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/hashes.h>
#include <flowmesh2/core/nonce_rule.h>
#include <flowmesh2/ledger/nonce_window.h>
#include <flowmesh2/ledger/state_commitment.h>
#include <hash.h>

#include <algorithm>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <utility>

namespace flowmesh2::engine {

namespace {

constexpr Error Invalid(const char* detail) { return Error{ErrorCode::INVALID_INPUT, detail}; }
constexpr Error Invalid(std::string_view detail) { return Error{ErrorCode::INVALID_INPUT, detail}; }
constexpr Error Broken(const char* detail) { return Error{ErrorCode::INVARIANT, detail}; }

} // namespace

// ---- Canonical order -----------------------------------------------------------

uint256 OrderKey(const uint256& parent_header_hash, uint64_t height, const ActionId& id)
{
    HashWriter writer{TaggedWriter(HashTag::ORDER)};
    writer << parent_header_hash << height << id;
    return writer.GetSHA256();
}

Result<std::vector<size_t>> CanonicalOrder(std::span<const OrderingEntry> originals, const uint256& parent_header_hash,
                                           uint64_t height)
{
    std::set<ActionId> ids;
    std::set<std::pair<AccountId, uint64_t>> nonces;
    for (const OrderingEntry& entry : originals) {
        if (!ids.insert(entry.id).second) return Invalid("repeated ActionId in the canonical order");
        if (!nonces.emplace(entry.account, entry.nonce).second) {
            return Invalid("repeated (account, nonce) in the canonical order");
        }
    }

    // 1. Sort by the seeded key; ties (a SHA-256 collision) by ActionId, which is unique.
    std::vector<uint256> keys;
    keys.reserve(originals.size());
    for (const OrderingEntry& entry : originals) keys.push_back(OrderKey(parent_header_hash, height, entry.id));
    std::vector<size_t> seeded(originals.size());
    std::iota(seeded.begin(), seeded.end(), size_t{0});
    std::sort(seeded.begin(), seeded.end(), [&](size_t a, size_t b) {
        if (keys[a] != keys[b]) return keys[a] < keys[b];
        return originals[a].id < originals[b].id;
    });

    // 2. Each account keeps the positions it drew and fills them in ascending nonce.
    std::map<AccountId, std::vector<size_t>> positions;  // ascending ranks
    std::map<AccountId, std::vector<size_t>> by_nonce;   // indices, sorted below
    for (size_t rank{0}; rank < seeded.size(); ++rank) {
        const OrderingEntry& entry{originals[seeded[rank]]};
        positions[entry.account].push_back(rank);
        by_nonce[entry.account].push_back(seeded[rank]);
    }
    std::vector<size_t> order(originals.size());
    for (auto& [account, indices] : by_nonce) {
        std::sort(indices.begin(), indices.end(),
                  [&](size_t a, size_t b) { return originals[a].nonce < originals[b].nonce; });
        const std::vector<size_t>& ranks{positions.at(account)};
        for (size_t k{0}; k < indices.size(); ++k) order[ranks[k]] = indices[k];
    }
    return Ok(std::move(order));
}

Result<std::vector<size_t>> CanonicalOrder(std::span<const VerifiedAction> originals, const uint256& parent_header_hash,
                                           uint64_t height)
{
    std::vector<OrderingEntry> entries;
    entries.reserve(originals.size());
    for (const VerifiedAction& action : originals) {
        entries.push_back(OrderingEntry{action.Id(), action.Account(), action.Action().nonce});
    }
    return CanonicalOrder(std::span<const OrderingEntry>{entries}, parent_header_hash, height);
}

// ---- Phases ---------------------------------------------------------------------

bool IsKnown(SlotPhase phase)
{
    switch (phase) {
    case SlotPhase::DEPOSITS:
    case SlotPhase::ACCOUNT:
    case SlotPhase::CANCELS:
    case SlotPhase::POST_ONLY:
    case SlotPhase::TAKERS:
    case SlotPhase::REST:
    case SlotPhase::FUTURES:
    case SlotPhase::FEES:
    case SlotPhase::CONSERVATION:
        return true;
    }
    return false;
}

std::string_view ToString(SlotPhase phase)
{
    switch (phase) {
    case SlotPhase::DEPOSITS: return "DEPOSITS";
    case SlotPhase::ACCOUNT: return "ACCOUNT";
    case SlotPhase::CANCELS: return "CANCELS";
    case SlotPhase::POST_ONLY: return "POST_ONLY";
    case SlotPhase::TAKERS: return "TAKERS";
    case SlotPhase::REST: return "REST";
    case SlotPhase::FUTURES: return "FUTURES";
    case SlotPhase::FEES: return "FEES";
    case SlotPhase::CONSERVATION: return "CONSERVATION";
    }
    return "UNKNOWN";
}

Result<SlotPhase> PhaseOf(const ActionV2& action)
{
    switch (action.kind) {
    case ActionKind::PLACE: {
        const Result<PlacePayload> place{action.Place()};
        if (!place.has_value()) return place.error();
        return Ok(place->tif == TimeInForce::POST_ONLY ? SlotPhase::POST_ONLY : SlotPhase::TAKERS);
    }
    case ActionKind::CANCEL:
        return Ok(SlotPhase::CANCELS);
    case ActionKind::TRANSFER:
        return Ok(SlotPhase::ACCOUNT);
    case ActionKind::CANCEL_ALL:
        return NotImplemented("M3-ENG-09"); // TODO(rebirth:M3-ENG-09) CANCEL_ALL in phase 3: release every order of the account
    case ActionKind::WITHDRAW:
        return NotImplemented("M2-LED-05"); // TODO(rebirth:M2-LED-05) WITHDRAW in phase 2 through LedgerState::RequestWithdrawal
    case ActionKind::SPOT_TO_FUTURES:
    case ActionKind::FUTURES_TO_SPOT:
        return NotImplemented("M2-LED-06"); // TODO(rebirth:M2-LED-06) spot/futures cash transfers in phase 2
    }
    return Invalid("unregistered action kind");
}

Result<uint64_t> ArrivalRank(uint64_t height, SlotPhase phase, uint64_t rank, uint32_t max_actions_per_slot)
{
    if (phase != SlotPhase::POST_ONLY && phase != SlotPhase::REST) return Invalid("arrival rank outside phases 4 and 6");
    if (max_actions_per_slot == 0) return Invalid("zero max_actions_per_slot");
    if (rank >= max_actions_per_slot) return Invalid("canonical rank at or above max_actions_per_slot");
    const Result<uint64_t> doubled{MulChecked(height, 2)};
    if (!doubled.has_value()) return doubled.error();
    const uint64_t block_index{*doubled + (phase == SlotPhase::REST ? 1 : 0)}; // doubled is even, so +1 cannot wrap
    const Result<uint64_t> block_start{MulChecked(block_index, max_actions_per_slot)};
    if (!block_start.has_value()) return block_start.error();
    if (rank > std::numeric_limits<uint64_t>::max() - *block_start) {
        return Error{ErrorCode::ARITHMETIC_OVERFLOW, "arrival rank above 2^64-1"};
    }
    return Ok(*block_start + rank);
}

// ---- EngineState -----------------------------------------------------------------

Result<EngineState> EngineState::Genesis(const ProfileParams& profile, std::span<const MarketDescriptor> markets)
{
    Result<ledger::LedgerState> ledger{ledger::LedgerState::Create(profile)};
    if (!ledger.has_value()) return ledger.error();
    Result<BookSet> books{BookSet::Create(markets)};
    if (!books.has_value()) return books.error();
    return Ok(EngineState{std::move(ledger).value(), std::move(books).value()});
}

std::vector<MarketDescriptor> EngineState::Markets() const
{
    std::vector<MarketDescriptor> markets;
    markets.reserve(books.MarketCount());
    for (const auto& [id, book] : books.Books()) markets.push_back(book.Market());
    return markets;
}

std::vector<RestingOrderLeafV0> EngineState::OrderLeaves() const
{
    std::vector<RestingOrderLeafV0> leaves;
    leaves.reserve(books.OrderCount());
    for (const auto& [id, book] : books.Books()) {
        for (const RestingOrder& o : book.Orders()) {
            leaves.push_back(RestingOrderLeafV0{o.id, id, o.owner, o.side, o.price_ticks, o.remaining_lots, o.arrival_rank,
                                                o.reserved_left, o.cum_quote, o.fee_charged, o.fills});
        }
    }
    return leaves;
}

Result<StateRootsV0> EngineState::Commit() const
{
    const std::vector<MarketDescriptor> markets{Markets()};
    const std::vector<RestingOrderLeafV0> orders{OrderLeaves()};
    return ledger::CommitTradingStateV0(ledger, markets, orders);
}

Result<void> EngineState::CheckInvariants() const
{
    const Result<void> conserved{ledger.CheckConservation()};
    if (!conserved.has_value()) return conserved.error();
    const Result<void> booked{books.CheckInvariants()};
    if (!booked.has_value()) return booked.error();
    for (const auto& [asset, totals] : ledger.Totals()) {
        if (totals.fees_held != 0) return Broken("fees held at a slot boundary");
    }

    // Reservations and open orders are exactly those of the resting orders.
    std::map<std::pair<AccountId, AssetId>, Atoms> reserved;
    std::map<AccountId, uint64_t> resting;
    for (const auto& [id, book] : books.Books()) {
        for (const RestingOrder& o : book.Orders()) {
            if (!ledger.HasAccount(o.owner) || ledger.IsProtocolAccount(o.owner)) {
                return Broken("resting order without a trading account");
            }
            const AssetId& asset{o.side == Side::BUY ? book.Market().quote : book.Market().base};
            Atoms& sum{reserved[{o.owner, asset}]};
            const Result<Atoms> next{AddAtoms(sum, o.reserved_left)};
            if (!next.has_value()) return Broken("resting reservations above MAX_ATOMS");
            sum = *next;
            ++resting[o.owner];
        }
    }
    for (const auto& [account, state] : ledger.Accounts()) {
        for (const auto& [asset, balance] : state.balances) {
            const auto it{reserved.find({account, asset})};
            if (balance.reserved != (it == reserved.end() ? Atoms{0} : it->second)) {
                return Broken("reserved balance differs from the resting orders' reservations");
            }
        }
        const auto count{resting.find(account)};
        if (state.open_orders != (count == resting.end() ? uint64_t{0} : count->second)) {
            return Broken("open-order count differs from the resting orders");
        }
    }
    for (const auto& [key, amount] : reserved) {
        if (amount != 0 && ledger.BalanceOf(key.first, key.second).reserved != amount) {
            return Broken("resting reservation the ledger does not hold");
        }
    }
    return Ok();
}

// ---- The slot validity predicate ---------------------------------------------------

ValidatedSlot::ValidatedSlot(uint256 parent_hash, uint64_t height, SlotInputs inputs, std::vector<DepositFact> deposits,
                             std::vector<uint256> fact_ids, std::vector<SlotAction> actions, std::vector<size_t> order)
    : m_parent_hash{parent_hash},
      m_height{height},
      m_inputs{std::move(inputs)},
      m_deposits{std::move(deposits)},
      m_fact_ids{std::move(fact_ids)},
      m_actions{std::move(actions)},
      m_order{std::move(order)}
{
}

namespace {

//! The payload of a supported kind, decoded; nullopt if it does not decode.
std::optional<ActionPayload> DecodePayload(const ActionV2& action)
{
    switch (action.kind) {
    case ActionKind::PLACE: {
        Result<PlacePayload> place{action.Place()};
        if (!place.has_value()) return std::nullopt;
        return ActionPayload{std::move(place).value()};
    }
    case ActionKind::CANCEL: {
        Result<CancelPayload> cancel{action.Cancel()};
        if (!cancel.has_value()) return std::nullopt;
        return ActionPayload{std::move(cancel).value()};
    }
    case ActionKind::TRANSFER: {
        Result<TransferPayload> transfer{action.Transfer()};
        if (!transfer.has_value()) return std::nullopt;
        return ActionPayload{std::move(transfer).value()};
    }
    case ActionKind::CANCEL_ALL:
    case ActionKind::WITHDRAW:
    case ActionKind::SPOT_TO_FUTURES:
    case ActionKind::FUTURES_TO_SPOT:
        break;
    }
    return std::nullopt;
}

//! An action that passed the checks before its signature.
struct CheckedOriginal {
    ActionV2 action;
    ActionPayload payload;
    SlotPhase phase;
};

} // namespace

Result<ValidatedSlot> ValidateSlotInputs(const HeadView& parent, const SlotInputs& inputs, const ProfileParams& profile)
{
    // 0-1. The profile.
    const Result<void> valid{Validate(profile)};
    if (!valid.has_value()) return valid.error();
    const SlotHeaderV2& head{parent.Header()};
    const ledger::LedgerState& ledger{parent.State().ledger};
    const uint256 profile_hash{ProfileHash(profile)};
    if (inputs.profile_hash != profile_hash || head.profile_hash != profile_hash || !(ledger.Profile() == profile)) {
        return Invalid(SLOT_PROFILE_MISMATCH);
    }
    // 2-3. Domain and height.
    if (inputs.domain != head.domain) return Invalid(SLOT_DOMAIN_MISMATCH);
    if (head.height == std::numeric_limits<uint64_t>::max()) return Invalid(SLOT_HEIGHT_EXHAUSTED);
    const uint64_t height{head.height + 1};
    // 4. Anchor.
    if (inputs.anchor.height < head.anchor.height) return Invalid(SLOT_ANCHOR_BELOW_PARENT);
    if (inputs.anchor.height == head.anchor.height && inputs.anchor.hash != head.anchor.hash) return Invalid(SLOT_ANCHOR_FORK);
    // 5. The fact range.
    if (!(inputs.facts_from == head.facts_to)) return Invalid(SLOT_FACTS_NOT_CONTIGUOUS);
    if (inputs.facts_to < inputs.facts_from) return Invalid(SLOT_FACTS_REVERSED);
    const uint64_t after_anchor{uint64_t{inputs.anchor.height} + 1};
    if (inputs.facts_to.height > after_anchor || (inputs.facts_to.height == after_anchor && inputs.facts_to.index != 0)) {
        return Invalid(SLOT_FACTS_BEYOND_ANCHOR);
    }
    // 6. Caps, before anything is decoded.
    // TODO(rebirth:M3-ENG-04) the 8,192 changed-leaves cap and the SLOT_FULL rule join these caps.
    const SlotBody& body{inputs.body};
    if (body.facts.size() > MAX_SLOT_FACTS) return Invalid(SLOT_FACT_COUNT_CAP);
    for (const auto& fact : body.facts) {
        if (fact.size() > MAX_FACT_BYTES) return Invalid(SLOT_FACT_BYTES_CAP);
    }
    if (body.originals.size() > profile.max_actions_per_slot) return Invalid(SLOT_ACTION_COUNT_CAP);
    const size_t max_action_bytes{ActionV2::MaxEncodedSize(profile)};
    for (const auto& original : body.originals) {
        if (original.size() > max_action_bytes) return Invalid(SLOT_ACTION_BYTES_CAP);
    }

    // 7. Facts.
    std::vector<DepositFact> deposits;
    std::vector<uint256> fact_ids;
    deposits.reserve(body.facts.size());
    fact_ids.reserve(body.facts.size());
    std::set<Outpoint> outpoints;
    std::optional<FactCursor> previous;
    for (const auto& bytes : body.facts) {
        const Result<Fact> fact{DecodeFact(bytes)};
        if (!fact.has_value()) {
            // A later fact kind propagates its stub error (its R4 task id) unchanged.
            const ErrorCode code{fact.error().code};
            if (code == ErrorCode::NOT_IMPLEMENTED || code == ErrorCode::UNSUPPORTED) return fact.error();
            return Invalid(SLOT_FACT_MALFORMED);
        }
        const DepositFact* deposit{std::get_if<DepositFact>(&*fact)};
        if (deposit == nullptr) return Invalid(SLOT_FACT_KIND); // each fact task adds its engine branch
        if (deposit->cursor < inputs.facts_from || !(deposit->cursor < inputs.facts_to) ||
            (previous.has_value() && !(*previous < deposit->cursor))) {
            return Invalid(SLOT_FACT_CURSOR);
        }
        previous = deposit->cursor;
        if (deposit->amount <= 0 || ledger.IsProtocolAccount(deposit->account) || ledger.Credited(deposit->outpoint) ||
            !outpoints.insert(deposit->outpoint).second) {
            return Invalid(SLOT_DEPOSIT_REFUSED);
        }
        deposits.push_back(*deposit);
        fact_ids.push_back(FactLeaf(bytes));
    }

    // 8. Originals, every check before the signature.
    std::vector<CheckedOriginal> checked;
    checked.reserve(body.originals.size());
    std::set<ActionId> ids;
    std::set<std::pair<AccountId, uint64_t>> account_nonces;
    std::map<AccountId, std::vector<uint64_t>> nonces_by_account;
    for (const auto& bytes : body.originals) {
        Result<ActionV2> decoded{ActionV2::Decode(bytes, profile)};
        if (!decoded.has_value()) return Invalid(SLOT_ACTION_MALFORMED);
        ActionV2 action{std::move(decoded).value()};
        if (action.domain != inputs.domain) return Invalid(SLOT_ACTION_DOMAIN);
        if (!KindSupported(action.kind)) return Invalid(SLOT_ACTION_KIND);
        std::optional<ActionPayload> payload{DecodePayload(action)};
        if (!payload.has_value()) return Invalid(SLOT_ACTION_PAYLOAD);
        const Result<SlotPhase> phase{PhaseOf(action)};
        if (!phase.has_value()) return Invalid(SLOT_ACTION_PAYLOAD);
        if (!action.MaxDelayValid(profile) || !InExpiryWindow(action.ref_slot, action.max_delay, height)) {
            return Invalid(SLOT_ACTION_WINDOW);
        }
        if (!ids.insert(action.Id()).second) return Invalid(SLOT_DUPLICATE_ACTION);
        const AccountId account{action.Account()};
        if (!account_nonces.emplace(account, action.nonce).second) return Invalid(SLOT_DUPLICATE_NONCE);
        const ledger::AccountState* state{ledger.Find(account)};
        if (state == nullptr) return Invalid(SLOT_ACCOUNT_ABSENT);
        if (!state->nonces.Acceptable(action.nonce)) return Invalid(SLOT_NONCE_EXCLUDED);
        nonces_by_account[account].push_back(action.nonce);
        checked.push_back(CheckedOriginal{std::move(action), std::move(payload).value(), *phase});
    }
    // 9. Consumed in ascending order, every account's nonces fit its window.
    for (auto& [account, nonces] : nonces_by_account) {
        std::sort(nonces.begin(), nonces.end());
        ledger::NonceWindow window{ledger.Find(account)->nonces};
        for (const uint64_t nonce : nonces) {
            if (!window.Insert(nonce).has_value()) return Invalid(SLOT_NONCE_EXCLUDED);
        }
    }
    // 10. Signatures.
    std::vector<SlotAction> actions;
    std::vector<VerifiedAction> verified;
    actions.reserve(checked.size());
    verified.reserve(checked.size());
    for (CheckedOriginal& original : checked) {
        Result<VerifiedAction> action{VerifiedAction::Verify(std::move(original.action))};
        if (!action.has_value()) return Invalid(SLOT_BAD_SIGNATURE);
        verified.push_back(*action);
        actions.push_back(SlotAction{std::move(action).value(), std::move(original.payload), original.phase});
    }

    Result<std::vector<size_t>> order{CanonicalOrder(std::span<const VerifiedAction>{verified}, parent.HeaderHash(), height)};
    if (!order.has_value()) return order.error();
    return Ok(ValidatedSlot{parent.HeaderHash(), height, inputs, std::move(deposits), std::move(fact_ids), std::move(actions),
                            std::move(order).value()});
}

} // namespace flowmesh2::engine
