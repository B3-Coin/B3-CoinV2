// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/engine/engine.h>

#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/fee_math.h>
#include <flowmesh2/core/hashes.h>
#include <flowmesh2/core/merkle.h>
#include <flowmesh2/core/nonce_rule.h>
#include <flowmesh2/engine/matcher.h>
#include <flowmesh2/engine/residual.h>
#include <flowmesh2/ledger/nonce_window.h>
#include <flowmesh2/ledger/state_commitment.h>
#include <hash.h>
#include <util/overflow.h>

#include <algorithm>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <tuple>
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

Result<SlotPhase> PhaseOf(const ActionV2& action, const ProfileParams& profile)
{
    if (action.kind == ActionKind::WITHDRAW && profile.withdrawals_enabled) return Ok(SlotPhase::ACCOUNT);
    return PhaseOf(action);
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
    for (const MarketDescriptor& market : markets) {
        if (const Result<void> policy{ValidateMarketForProfile(market, profile)}; !policy.has_value()) return policy.error();
    }
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
std::optional<ActionPayload> DecodePayload(const ActionV2& action, const ProfileParams& profile)
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
    case ActionKind::WITHDRAW: {
        if (!profile.withdrawals_enabled) return std::nullopt;
        Result<WithdrawPayload> withdrawal{action.Withdraw()};
        if (!withdrawal.has_value()) return std::nullopt;
        return ActionPayload{std::move(withdrawal).value()};
    }
    case ActionKind::CANCEL_ALL:
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
    // A caller cannot smuggle an incompatible book into an otherwise valid
    // stable-Q parent. Check every market, including a slot with no fills.
    for (const auto& entry : parent.State().books.Books()) {
        if (const Result<void> policy{ValidateMarketForProfile(entry.second.Market(), profile)}; !policy.has_value()) return policy.error();
    }
    const uint8_t expected_commitment{ExpectedCommitmentVersion(profile)};
    if (head.commitment_version != expected_commitment) return Invalid("parent commitment version differs from profile");
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
    bool source_range_seen{false};
    for (const auto& bytes : body.facts) {
        const Result<Fact> fact{DecodeFact(bytes)};
        if (!fact.has_value()) {
            // A later fact kind propagates its stub error (its R4 task id) unchanged.
            const ErrorCode code{fact.error().code};
            if (code == ErrorCode::NOT_IMPLEMENTED || code == ErrorCode::UNSUPPORTED) return fact.error();
            return Invalid(SLOT_FACT_MALFORMED);
        }
        const FactCursor cursor{CursorOf(*fact)};
        if (cursor < inputs.facts_from || !(cursor < inputs.facts_to) ||
            (previous.has_value() && !(*previous < cursor))) return Invalid(SLOT_FACT_CURSOR);
        previous = cursor;
        if (const auto* deposit{std::get_if<DepositFact>(&*fact)}) {
            if (deposit->amount <= 0 || ledger.IsProtocolAccount(deposit->account) || ledger.Credited(deposit->outpoint) ||
                !outpoints.insert(deposit->outpoint).second) return Invalid(SLOT_DEPOSIT_REFUSED);
            deposits.push_back(*deposit);
        } else if (const auto* payout{std::get_if<PayoutConfirmedFact>(&*fact)}; payout && profile.withdrawals_enabled) {
            const auto claim{ledger.Withdrawals().find(payout->withdrawal)};
            if (claim == ledger.Withdrawals().end() || claim->second.slot >= height || payout->domain != inputs.domain ||
                claim->second.account != payout->account || claim->second.asset != payout->asset ||
                claim->second.amount != payout->amount || claim->second.recipient != payout->recipient ||
                (claim->second.paid && claim->second.payout_event != payout->payout_event) ||
                !outpoints.insert(payout->payout_event).second) return Invalid("payout does not match a committed withdrawal");
            for (const auto& [id, prior] : ledger.Withdrawals()) {
                if (prior.paid && prior.payout_event == payout->payout_event && id != payout->withdrawal) {
                    return Invalid("payout event already settles another withdrawal");
                }
            }
        } else if (const auto* source{std::get_if<SourceRangeFact>(&*fact)}) {
            if (!profile.source_range_commitment || source_range_seen || &bytes != &body.facts.back() ||
                source->domain != inputs.domain || source->chain_id != profile.source_chain_id ||
                source->confirmations != profile.source_confirmations || source->cursor.height != inputs.facts_to.height ||
                uint64_t{source->cursor.index} + 1 != inputs.facts_to.index) {
                return Invalid("source range commitment is duplicated, misplaced or differs from its pinned profile/domain/cursor");
            }
            source_range_seen = true;
        } else {
            return Invalid(SLOT_FACT_KIND);
        }
        fact_ids.push_back(FactLeaf(bytes));
    }
    if (profile.source_range_commitment && !source_range_seen) return Invalid("network slot lacks its terminal source range commitment");

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
        if (!KindSupported(action.kind, profile)) return Invalid(SLOT_ACTION_KIND);
        std::optional<ActionPayload> payload{DecodePayload(action, profile)};
        if (!payload.has_value()) return Invalid(SLOT_ACTION_PAYLOAD);
        const Result<SlotPhase> phase{PhaseOf(action, profile)};
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

// ---- Executing a slot ------------------------------------------------------------------

Result<void> RunFuturesPhase(EngineState&, const ProfileParams& profile)
{
    if (!profile.futures_enabled) return Ok();
    return NotImplemented("M9-FUT-04"); // TODO(rebirth:M9-FUT-04) futures sweep: liquidation index, cap, insurance, deficit halt
}

namespace {

//! a + b for balance deltas, refused outside [-MAX_ATOMS, MAX_ATOMS].
Result<int64_t> AddDelta(int64_t a, int64_t b)
{
    const std::optional<int64_t> sum{CheckedAdd(a, b)};
    if (!sum.has_value() || *sum < -MAX_ATOMS || *sum > MAX_ATOMS) {
        return Error{ErrorCode::ARITHMETIC_OVERFLOW, "balance delta outside [-MAX_ATOMS, MAX_ATOMS]"};
    }
    return Ok(*sum);
}

using DeltaKey = std::pair<AccountId, AssetId>;
using DeltaMap = std::map<DeltaKey, std::pair<int64_t, int64_t>>; // (available, reserved)

Result<void> AddTo(DeltaMap& map, const AccountId& account, const AssetId& asset, int64_t available, int64_t reserved)
{
    auto& entry{map[{account, asset}]};
    const Result<int64_t> a{AddDelta(entry.first, available)};
    if (!a.has_value()) return a.error();
    const Result<int64_t> r{AddDelta(entry.second, reserved)};
    if (!r.has_value()) return r.error();
    entry = {*a, *r};
    return Ok();
}

//! Balance changes of one outcome, aggregated per (account, asset).
class DeltaSet
{
public:
    Result<void> Add(const AccountId& account, const AssetId& asset, int64_t available, int64_t reserved)
    {
        return AddTo(m_deltas, account, asset, available, reserved);
    }
    //! Sorted by (account, asset), without all-zero entries.
    std::vector<BalanceDelta> Sorted() const
    {
        std::vector<BalanceDelta> out;
        for (const auto& [key, value] : m_deltas) {
            if (value.first != 0 || value.second != 0) out.push_back(BalanceDelta{key.first, key.second, value.first, value.second});
        }
        return out;
    }

private:
    DeltaMap m_deltas;
};

//! The outcome of one action while the phases build it.
struct ActionRecord {
    std::optional<OutcomeCode> code;
    uint64_t remaining_lots{0};
    std::vector<Fill> fills;
    DeltaSet deltas;
};

/** True for the two codes that refuse only a taker's remainder and keep the
 * fills made before (O-40); every other rejection moves no money.
 */
// TODO(rebirth:M3-ENG-02) the residual call replaces REJ_RESIDUAL_CROSS_UNSUPPORTED here (M3-ENG-04 rules the cap codes).
bool RefusesRemainderOnly(OutcomeCode code)
{
    return code == OutcomeCode::REJ_FILL_CAP || code == OutcomeCode::REJ_RESIDUAL_CROSS_UNSUPPORTED;
}

//! The asset an order of `side` holds reserved in `market`.
const AssetId& ReservedAsset(const MarketDescriptor& market, Side side) { return side == Side::BUY ? market.quote : market.base; }

//! One execution of one validated slot on a copy of the parent state.
class SlotRun
{
public:
    SlotRun(const ProfileParams& profile, const EngineState& parent, const ValidatedSlot& slot)
        : m_profile{profile}, m_parent{parent}, m_slot{slot}, m_state{parent}, m_records(slot.Actions().size()) {}

    Result<SlotResult> Run()
    {
        for (const auto step : {&SlotRun::ConsumeNonces, &SlotRun::Deposits, &SlotRun::AccountActions, &SlotRun::Cancels,
                                &SlotRun::PostOnly, &SlotRun::Takers, &SlotRun::RestOrRelease, &SlotRun::Futures,
                                &SlotRun::Fees}) {
            const Result<void> done{(this->*step)()};
            if (!done.has_value()) return done.error();
        }
        return Finish();
    }

private:
    size_t Count() const { return m_slot.Order().size(); }
    //! The action at canonical `rank`.
    const SlotAction& At(size_t rank) const { return m_slot.Actions()[m_slot.Order()[rank]]; }
    ledger::LedgerState& Ledger() { return m_state.ledger; }

    //! A deterministic rejection: the nonce is consumed, nothing else changes.
    void Reject(size_t rank, OutcomeCode code) { m_records[rank].code = code; }

    // Nonces: every action consumes its nonce, in canonical order (so each
    // account's nonces ascend), whatever its outcome (DUR-5).
    Result<void> ConsumeNonces()
    {
        for (size_t rank{0}; rank < Count(); ++rank) {
            const VerifiedAction& action{At(rank).action};
            const Result<void> consumed{Ledger().ConsumeNonce(action.Account(), action.Action().nonce)};
            if (!consumed.has_value()) return consumed.error();
        }
        return Ok();
    }

    // Phase 1.
    Result<void> Deposits()
    {
        // Preserve exact fact order and FactId association for mixed deposits/payouts.
        const auto& facts{m_slot.Inputs().body.facts};
        for (size_t i{0}; i < facts.size(); ++i) {
            const Result<Fact> decoded{DecodeFact(facts[i])};
            if (!decoded.has_value()) return decoded.error();
            if (std::holds_alternative<SourceRangeFact>(*decoded)) continue; // authenticated boundary only: no ledger effect/outcome
            OutcomeV2 outcome;
            outcome.source = OutcomeSource::FACT;
            outcome.subject = m_slot.FactIds()[i];
            if (const auto* deposit{std::get_if<DepositFact>(&*decoded)}) {
                const Result<void> credited{Ledger().CreditDeposit(*deposit)};
                if (!credited.has_value()) return credited.error();
                DeltaSet deltas;
                const Result<void> added{deltas.Add(deposit->account, deposit->asset, deposit->amount, 0)};
                if (!added.has_value()) return added.error();
                outcome.account = deposit->account;
                outcome.code = OutcomeCode::DEPOSIT_CREDITED;
                outcome.deltas = deltas.Sorted();
            } else {
                const auto* payout{std::get_if<PayoutConfirmedFact>(&*decoded)};
                if (!payout) return Broken("validated fact of unsupported kind");
                const Result<void> confirmed{Ledger().ConfirmWithdrawal(*payout)};
                if (!confirmed.has_value()) return confirmed.error();
                outcome.account = payout->account;
                outcome.code = OutcomeCode::WITHDRAWAL_PAID;
            }
            m_fact_outcomes.push_back(std::move(outcome));
        }
        return Ok();
    }

    // Phase 2.
    Result<void> AccountActions()
    {
        for (size_t rank{0}; rank < Count(); ++rank) {
            const SlotAction& a{At(rank)};
            if (a.phase != SlotPhase::ACCOUNT) continue;
            if (a.action.Action().kind == ActionKind::WITHDRAW) {
                const WithdrawPayload& withdrawal{std::get<WithdrawPayload>(a.payload)};
                const bool known{Ledger().Withdrawals().contains(a.action.Id())};
                const Result<OutcomeCode> code{Ledger().RequestWithdrawal(a.action.Id(), a.action.Account(), withdrawal, m_slot.Height())};
                if (!code.has_value()) return code.error();
                ActionRecord& record{m_records[rank]};
                record.code = *code;
                if (*code == OutcomeCode::WITHDRAWAL_PENDING && !known) {
                    const Result<Atoms> debit{AddAtoms(withdrawal.amount, m_profile.withdrawal_fee_atoms)};
                    if (!debit.has_value()) return debit.error();
                    const Result<void> delta{record.deltas.Add(a.action.Account(), withdrawal.asset, -*debit, 0)};
                    if (!delta.has_value()) return delta.error();
                    const Result<Atoms> fees{AddAtoms(m_fees[withdrawal.asset], m_profile.withdrawal_fee_atoms)};
                    if (!fees.has_value()) return fees.error();
                    m_fees[withdrawal.asset] = *fees;
                }
                continue;
            }
            const TransferPayload& t{std::get<TransferPayload>(a.payload)};
            const Result<OutcomeCode> code{Ledger().Transfer(a.action.Account(), t.to, t.asset, t.amount)};
            if (!code.has_value()) return code.error();
            ActionRecord& record{m_records[rank]};
            record.code = *code;
            if (*code == OutcomeCode::TRANSFERRED) {
                const Result<void> from{record.deltas.Add(a.action.Account(), t.asset, -t.amount, 0)};
                if (!from.has_value()) return from.error();
                const Result<void> to{record.deltas.Add(t.to, t.asset, t.amount, 0)};
                if (!to.has_value()) return to.error();
            }
        }
        return Ok();
    }

    // Phase 3.
    Result<void> Cancels()
    {
        for (size_t rank{0}; rank < Count(); ++rank) {
            const SlotAction& a{At(rank)};
            if (a.phase != SlotPhase::CANCELS) continue;
            const OrderId& id{std::get<CancelPayload>(a.payload).order};
            const AccountId& owner{a.action.Account()};
            OrderBook* book{m_state.books.FindOrderMutable(id)};
            if (book == nullptr) {
                Reject(rank, OutcomeCode::REJ_UNKNOWN_ORDER);
                continue;
            }
            if (!(book->Find(id)->owner == owner)) {
                Reject(rank, OutcomeCode::REJ_NOT_OWN_ORDER);
                continue;
            }
            const Result<RestingOrder> removed{book->Remove(id)};
            if (!removed.has_value()) return removed.error();
            const AssetId& asset{ReservedAsset(book->Market(), removed->side)};
            const Result<void> released{Ledger().Release(owner, asset, removed->reserved_left)};
            if (!released.has_value()) return released.error();
            const Result<void> closed{Ledger().RemoveOpenOrder(owner)};
            if (!closed.has_value()) return closed.error();
            ActionRecord& record{m_records[rank]};
            record.code = OutcomeCode::CANCELLED;
            const Result<void> added{record.deltas.Add(owner, asset, removed->reserved_left, -removed->reserved_left)};
            if (!added.has_value()) return added.error();
        }
        return Ok();
    }

    /** The checks every PLACE shares (unknown market, bad params) and its
     * exact reservation; nullopt after a rejection was recorded.
     */
    Result<std::optional<Reservation>> PlaceReservation(size_t rank, const PlacePayload& p, OrderBook*& book)
    {
        book = m_state.books.FindMutable(p.market);
        if (book == nullptr) {
            Reject(rank, OutcomeCode::REJ_UNKNOWN_MARKET);
            return Ok(std::optional<Reservation>{});
        }
        if (p.price_ticks == 0 || p.lots == 0) {
            Reject(rank, OutcomeCode::REJ_BAD_PARAMS);
            return Ok(std::optional<Reservation>{});
        }
        Result<Reservation> need{RequiredReservation(book->Market(), p.side, p.price_ticks, p.lots)};
        if (!need.has_value()) {
            if (need.error().code != ErrorCode::ARITHMETIC_OVERFLOW) return need.error();
            Reject(rank, OutcomeCode::REJ_BAD_PARAMS); // the order is worth more than MAX_ATOMS
            return Ok(std::optional<Reservation>{});
        }
        return Ok(std::optional<Reservation>{*need});
    }

    //! Open-order cap, then the reservation; true iff reserved (else the rejection is recorded).
    Result<bool> ReserveForPlace(size_t rank, const AccountId& owner, const Reservation& need, bool takes_order_slot)
    {
        const ledger::AccountState* account{Ledger().Find(owner)};
        if (account == nullptr) return Broken("signer without an account");
        if (takes_order_slot && account->open_orders >= m_profile.max_open_orders_per_account) {
            Reject(rank, OutcomeCode::REJ_OPEN_ORDER_CAP);
            return Ok(false);
        }
        const Result<ledger::ReserveResult> reserved{Ledger().Reserve(owner, need.asset, need.amount)};
        if (!reserved.has_value()) return reserved.error();
        if (*reserved == ledger::ReserveResult::INSUFFICIENT) {
            Reject(rank, OutcomeCode::REJ_INSUFFICIENT_AVAILABLE);
            return Ok(false);
        }
        if (takes_order_slot) {
            const Result<ledger::OpenOrderResult> opened{Ledger().AddOpenOrder(owner)};
            if (!opened.has_value()) return opened.error();
            if (*opened != ledger::OpenOrderResult::OK) return Broken("open-order slot vanished");
        }
        const Result<void> added{m_records[rank].deltas.Add(owner, need.asset, -need.amount, need.amount)};
        if (!added.has_value()) return added.error();
        return Ok(true);
    }

    // Phase 4.
    Result<void> PostOnly()
    {
        for (size_t rank{0}; rank < Count(); ++rank) {
            const SlotAction& a{At(rank)};
            if (a.phase != SlotPhase::POST_ONLY) continue;
            const PlacePayload& p{std::get<PlacePayload>(a.payload)};
            const AccountId& owner{a.action.Account()};
            OrderBook* book{nullptr};
            const Result<std::optional<Reservation>> need{PlaceReservation(rank, p, book)};
            if (!need.has_value()) return need.error();
            if (!need->has_value()) continue;
            if (book->WouldCross(p.side, p.price_ticks)) {
                Reject(rank, OutcomeCode::REJ_WOULD_CROSS);
                continue;
            }
            const Result<bool> reserved{ReserveForPlace(rank, owner, **need, /*takes_order_slot=*/true)};
            if (!reserved.has_value()) return reserved.error();
            if (!*reserved) continue;
            const Result<uint64_t> arrival{ArrivalRank(m_slot.Height(), SlotPhase::POST_ONLY, rank, m_profile.max_actions_per_slot)};
            if (!arrival.has_value()) return arrival.error();
            RestingOrder order;
            order.id = OrderId::FromPlace(a.action.Id());
            order.owner = owner;
            order.side = p.side;
            order.price_ticks = p.price_ticks;
            order.remaining_lots = p.lots;
            order.arrival_rank = *arrival;
            order.reserved_left = (*need)->amount;
            const Result<void> rested{book->Rest(order)};
            if (!rested.has_value()) return rested.error();
            m_records[rank].code = OutcomeCode::RESTING;
            m_records[rank].remaining_lots = p.lots;
        }
        return Ok();
    }

    // Phase 5.
    Result<void> Takers()
    {
        for (size_t rank{0}; rank < Count(); ++rank) {
            const SlotAction& a{At(rank)};
            if (a.phase != SlotPhase::TAKERS) continue;
            const PlacePayload& p{std::get<PlacePayload>(a.payload)};
            const AccountId& owner{a.action.Account()};
            OrderBook* book{nullptr};
            const Result<std::optional<Reservation>> need{PlaceReservation(rank, p, book)};
            if (!need.has_value()) return need.error();
            if (!need->has_value()) continue;
            const Result<bool> reserved{ReserveForPlace(rank, owner, **need, /*takes_order_slot=*/p.tif == TimeInForce::GTC)};
            if (!reserved.has_value()) return reserved.error();
            if (!*reserved) continue;
            Taker taker;
            taker.id = OrderId::FromPlace(a.action.Id());
            taker.owner = owner;
            taker.side = p.side;
            taker.tif = p.tif;
            taker.limit_ticks = p.price_ticks;
            taker.remaining_lots = p.lots;
            taker.rank = rank;
            taker.reserved_left = (*need)->amount;
            m_taker_rank.emplace(taker.id, rank);
            m_takers[p.market].push_back(taker);
        }

        // One sweep per market, ascending MarketId, sharing the slot's fill budget.
        // TODO(rebirth:M3-ENG-04) rule the budget order across markets and sides (O-36) and SLOT_FULL.
        SlotFillBudget budget{SlotFillBudget::ForSlot(m_profile)};
        for (auto& [market, takers] : m_takers) {
            OrderBook* book{m_state.books.FindMutable(market)};
            if (book == nullptr) return Broken("taker of a market without a book");
            const Result<SweepResult> swept{SweepA2(*book, Ledger(), takers, m_profile, budget)};
            if (!swept.has_value()) return swept.error();
            const AssetId& quote{book->Market().quote};
            const Result<Atoms> fees{AddAtoms(m_fees[quote], swept->fees)};
            if (!fees.has_value()) return fees.error();
            m_fees[quote] = *fees;
            for (const ExecutedFill& executed : swept->fills) {
                const Result<void> recorded{RecordFill(book->Market(), executed)};
                if (!recorded.has_value()) return recorded.error();
            }
        }
        return Ok();
    }

    //! Both legs of one fill go into its taker's deltas; the Fill into its fills.
    Result<void> RecordFill(const MarketDescriptor& market, const ExecutedFill& executed)
    {
        const auto it{m_taker_rank.find(executed.fill.taker)};
        if (it == m_taker_rank.end()) return Broken("fill of an unknown taker");
        ActionRecord& record{m_records[it->second]};
        const Fill& fill{executed.fill};
        const bool taker_buys{fill.taker_side == Side::BUY};
        const AccountId& buyer{taker_buys ? executed.taker_owner : executed.maker_owner};
        const AccountId& seller{taker_buys ? executed.maker_owner : executed.taker_owner};
        for (const auto& [account, asset, available, reserved] :
             {std::tuple{seller, market.base, int64_t{0}, -executed.base_atoms},
              std::tuple{buyer, market.base, executed.base_atoms, int64_t{0}},
              std::tuple{buyer, market.quote, int64_t{0}, -fill.quote_atoms},
              std::tuple{seller, market.quote, fill.quote_atoms - fill.seller_fee, int64_t{0}}}) {
            const Result<void> added{record.deltas.Add(account, asset, available, reserved)};
            if (!added.has_value()) return added.error();
        }
        record.fills.push_back(fill);
        return Ok();
    }

    // Phase 6.
    Result<void> RestOrRelease()
    {
        // The GTC remainders that would rest crossed against each other (M3-ENG-02).
        std::set<OrderId> crossing;
        for (const auto& [market, takers] : m_takers) {
            std::vector<Taker> buys;
            std::vector<Taker> sells;
            for (const Taker& t : takers) {
                if (t.tif != TimeInForce::GTC || t.remaining_lots == 0 || t.stop != TakerStop::NO_CROSS) continue;
                (t.side == Side::BUY ? buys : sells).push_back(t);
            }
            const Result<ResidualCross> cross{FindResidualCross(buys, sells)};
            if (!cross.has_value()) return cross.error();
            for (const size_t i : cross->buys) crossing.insert(buys[i].id);
            for (const size_t i : cross->sells) crossing.insert(sells[i].id);
        }

        // Every taker in canonical order (ranks are unique, so this is total).
        std::vector<std::pair<const Taker*, OrderBook*>> ranked;
        for (auto& [market, takers] : m_takers) {
            OrderBook* book{m_state.books.FindMutable(market)};
            if (book == nullptr) return Broken("taker of a market without a book");
            for (const Taker& t : takers) ranked.emplace_back(&t, book);
        }
        std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first->rank < b.first->rank; });

        for (const auto& [taker, book] : ranked) {
            const Taker& t{*taker};
            ActionRecord& record{m_records[t.rank]};
            const AssetId& asset{ReservedAsset(book->Market(), t.side)};
            Atoms release{t.reserved_left};
            bool rests{false};
            if (t.remaining_lots == 0) {
                record.code = OutcomeCode::FILLED;
            } else if (t.stop == TakerStop::ORDER_FILL_CAP || t.stop == TakerStop::SLOT_FILL_CAP) {
                record.code = OutcomeCode::REJ_FILL_CAP; // the remainder only; SLOT_FULL is M3-ENG-04
            } else if (t.stop != TakerStop::NO_CROSS) {
                return Broken("taker left the sweep without a final stop");
            } else if (t.tif == TimeInForce::IOC) {
                record.code = OutcomeCode::IOC_DONE;
            } else if (crossing.contains(t.id)) {
                // TODO(rebirth:M3-ENG-02) the same-slot uniform-price call replaces this certified refusal.
                record.code = OutcomeCode::REJ_RESIDUAL_CROSS_UNSUPPORTED;
            } else {
                const Result<Reservation> keep{RequiredReservation(book->Market(), t.side, t.limit_ticks, t.remaining_lots)};
                if (!keep.has_value()) return keep.error();
                if (keep->amount > t.reserved_left) return Broken("remainder reserved below its limit");
                release = t.reserved_left - keep->amount; // price improvement
                const Result<uint64_t> arrival{ArrivalRank(m_slot.Height(), SlotPhase::REST, t.rank, m_profile.max_actions_per_slot)};
                if (!arrival.has_value()) return arrival.error();
                const Result<void> rested{book->Rest(RestingOrder{t.id, t.owner, t.side, t.limit_ticks, t.remaining_lots, *arrival,
                                                                  keep->amount, t.cum_quote, t.fee_charged, t.fills})};
                if (!rested.has_value()) return rested.error();
                rests = true;
                record.code = t.fills > 0 ? OutcomeCode::PARTIAL_RESTING : OutcomeCode::RESTING;
                record.remaining_lots = t.remaining_lots;
            }
            if (release > 0) {
                const Result<void> released{Ledger().Release(t.owner, asset, release)};
                if (!released.has_value()) return released.error();
                const Result<void> added{record.deltas.Add(t.owner, asset, release, -release)};
                if (!added.has_value()) return added.error();
            }
            if (!rests && t.tif == TimeInForce::GTC) {
                const Result<void> closed{Ledger().RemoveOpenOrder(t.owner)};
                if (!closed.has_value()) return closed.error();
            }
        }
        return Ok();
    }

    // Phase 7.
    Result<void> Futures() { return RunFuturesPhase(m_state, m_profile); }

    // Phase 8.
    Result<void> Fees()
    {
        for (const auto& [asset, total] : m_fees) {
            if (total == 0) continue;
            const Result<FeeSplit> split{SplitSlotFees(total, m_profile.seat_share_percent)};
            if (!split.has_value()) return split.error();
            const AccountId seat_pool{Ledger().SeatPool()};
            const AccountId treasury{Ledger().Treasury()};
            const Result<void> to_seats{Ledger().Credit(seat_pool, asset, split->seat_pool)};
            if (!to_seats.has_value()) return to_seats.error();
            const Result<void> to_treasury{Ledger().Credit(treasury, asset, split->treasury)};
            if (!to_treasury.has_value()) return to_treasury.error();
            DeltaSet deltas;
            const Result<void> seats_delta{deltas.Add(seat_pool, asset, split->seat_pool, 0)};
            if (!seats_delta.has_value()) return seats_delta.error();
            const Result<void> treasury_delta{deltas.Add(treasury, asset, split->treasury, 0)};
            if (!treasury_delta.has_value()) return treasury_delta.error();
            OutcomeV2 outcome;
            outcome.source = OutcomeSource::SLOT;
            outcome.subject = asset.id;
            outcome.account = AccountId{};
            outcome.nonce = 0;
            outcome.code = OutcomeCode::FEES_DISTRIBUTED;
            outcome.deltas = deltas.Sorted();
            m_fee_outcomes.push_back(std::move(outcome));
        }
        return Ok();
    }

    // Phase 9.
    Result<SlotResult> Finish()
    {
        const Result<void> invariants{m_state.CheckInvariants()};
        if (!invariants.has_value()) return invariants.error();

        std::vector<OutcomeV2> outcomes{std::move(m_fact_outcomes)};
        outcomes.reserve(outcomes.size() + Count() + m_fee_outcomes.size());
        for (size_t rank{0}; rank < Count(); ++rank) {
            ActionRecord& record{m_records[rank]};
            if (!record.code.has_value()) return Broken("action without an outcome");
            const VerifiedAction& action{At(rank).action};
            OutcomeV2 outcome;
            outcome.source = OutcomeSource::ACTION;
            outcome.subject = action.Id().id;
            outcome.account = action.Account();
            outcome.nonce = action.Action().nonce;
            outcome.code = *record.code;
            outcome.remaining_lots = record.remaining_lots;
            outcome.fills = std::move(record.fills);
            outcome.deltas = record.deltas.Sorted();
            if (IsRejection(outcome.code) && !RefusesRemainderOnly(outcome.code) &&
                (!outcome.fills.empty() || !outcome.deltas.empty())) {
                return Broken("a rejected action changed balances");
            }
            outcomes.push_back(std::move(outcome));
        }
        for (OutcomeV2& outcome : m_fee_outcomes) outcomes.push_back(std::move(outcome));

        // Every outcome decodes under the profile, and the deltas are the balance transition.
        DeltaMap reported;
        for (const OutcomeV2& outcome : outcomes) {
            if (outcome.fills.size() > OutcomeV2::MaxFills(m_profile) || outcome.deltas.size() > OutcomeV2::MaxDeltas(m_profile)) {
                return Broken("outcome above the profile's fill or delta cap");
            }
            for (const BalanceDelta& d : outcome.deltas) {
                const Result<void> added{AddTo(reported, d.account, d.asset, d.available_delta, d.reserved_delta)};
                if (!added.has_value()) return Broken("outcome deltas overflow");
            }
        }
        DeltaMap actual;
        for (const auto& [account, state] : m_state.ledger.Accounts()) {
            for (const auto& [asset, balance] : state.balances) {
                const Result<void> added{AddTo(actual, account, asset, balance.available, balance.reserved)};
                if (!added.has_value()) return Broken("balance outside [0, MAX_ATOMS]");
            }
        }
        for (const auto& [account, state] : m_parent.ledger.Accounts()) {
            for (const auto& [asset, balance] : state.balances) {
                const Result<void> added{AddTo(actual, account, asset, -balance.available, -balance.reserved)};
                if (!added.has_value()) return Broken("balance outside [0, MAX_ATOMS]");
            }
        }
        std::erase_if(reported, [](const auto& entry) { return entry.second == std::pair<int64_t, int64_t>{0, 0}; });
        std::erase_if(actual, [](const auto& entry) { return entry.second == std::pair<int64_t, int64_t>{0, 0}; });
        if (reported != actual) return Broken("outcome deltas differ from the balance transition");

        const Result<StateRootsV0> roots{m_state.Commit()};
        if (!roots.has_value()) return roots.error();
        std::vector<uint256> leaves;
        leaves.reserve(outcomes.size());
        for (const OutcomeV2& outcome : outcomes) leaves.push_back(outcome.LeafHash());
        const uint256 outcomes_root{MerkleRoot(leaves)};
        return Ok(SlotResult{std::move(m_state), std::move(outcomes), outcomes_root, *roots});
    }

    const ProfileParams& m_profile;
    const EngineState& m_parent;
    const ValidatedSlot& m_slot;
    EngineState m_state;                        //!< the candidate, a copy of the parent
    std::vector<ActionRecord> m_records;        //!< by canonical rank
    std::vector<OutcomeV2> m_fact_outcomes;     //!< by body order
    std::vector<OutcomeV2> m_fee_outcomes;      //!< by quote AssetId
    std::map<MarketId, std::vector<Taker>> m_takers; //!< reserved takers per market, in canonical order
    std::map<OrderId, size_t> m_taker_rank;
    std::map<AssetId, Atoms> m_fees;            //!< withheld fees per quote asset
};

} // namespace

Result<Engine> Engine::Create(const ProfileParams& profile)
{
    const Result<void> valid{Validate(profile)};
    if (!valid.has_value()) return valid.error();
    if (profile.matching != MatchingProfile::A2_PRORATA) return Invalid("matching profile is not A2_PRORATA");
    return Ok(Engine{profile});
}

Result<SlotResult> Engine::ApplySlot(const HeadView& parent, const ValidatedSlot& slot) const
{
    if (slot.ParentHash() != parent.HeaderHash()) return Error{ErrorCode::STALE_PARENT, "slot validated against another parent"};
    if (slot.Inputs().profile_hash != ProfileHash(m_profile) || !(parent.State().ledger.Profile() == m_profile)) {
        return Invalid("slot or parent under another profile");
    }
    // Recheck at application: ValidatedSlot's parent hash binds the header,
    // but a direct caller may have replaced the value-type parent books.
    for (const auto& entry : parent.State().books.Books()) {
        if (const Result<void> policy{ValidateMarketForProfile(entry.second.Market(), m_profile)}; !policy.has_value()) return policy.error();
    }
    for (const auto& [asset, totals] : parent.State().ledger.Totals()) {
        if (totals.fees_held != 0) return Broken("parent holds undistributed fees");
    }
    SlotRun run{m_profile, parent.State(), slot};
    return run.Run();
}

} // namespace flowmesh2::engine
