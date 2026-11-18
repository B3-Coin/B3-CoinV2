// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/ledger/ledger.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace flowmesh2::ledger {

namespace {

constexpr Error UnknownAccount() { return Error{ErrorCode::INVALID_INPUT, "unknown account"}; }
constexpr Error BadAmount() { return Error{ErrorCode::INVALID_INPUT, "amount outside [0, MAX_ATOMS]"}; }

//! Store `next` for `asset`, erasing the entry when it is all zero (canonical state).
void PutBalance(AccountState& account, const AssetId& asset, const Balance& next)
{
    if (next.IsZero()) {
        account.balances.erase(asset);
    } else {
        account.balances[asset] = next;
    }
}

//! OVERSIZE if storing `next` would give the account one asset more than MAX_LEAF_ASSETS.
Result<void> CheckAssetCap(const AccountState* account, const AssetId& asset, const Balance& next)
{
    if (next.IsZero() || account == nullptr || account->balances.contains(asset)) return Ok();
    if (account->balances.size() >= MAX_LEAF_ASSETS) {
        return Error{ErrorCode::OVERSIZE, "account already holds MAX_LEAF_ASSETS assets"};
    }
    return Ok();
}

} // namespace

Balance AccountState::BalanceOf(const AssetId& asset) const
{
    const auto it{balances.find(asset)};
    return it == balances.end() ? Balance{} : it->second;
}

std::string_view ToString(ReserveResult result)
{
    switch (result) {
    case ReserveResult::OK: return "OK";
    case ReserveResult::INSUFFICIENT: return "INSUFFICIENT";
    }
    return "UNKNOWN";
}

std::string_view ToString(OpenOrderResult result)
{
    switch (result) {
    case OpenOrderResult::OK: return "OK";
    case OpenOrderResult::AT_CAP: return "AT_CAP";
    }
    return "UNKNOWN";
}

Result<void> CheckConservation(const AccountMap& accounts, const TotalsMap& totals, uint32_t max_open_orders)
{
    std::map<AssetId, Atoms> liabilities;
    for (const auto& [id, account] : accounts) {
        if (account.open_orders > max_open_orders) return Error{ErrorCode::INVARIANT, "open orders above the cap"};
        if (!NonceWindowValid(account.nonces.Used(), account.nonces.Size())) {
            return Error{ErrorCode::INVARIANT, "malformed nonce window"};
        }
        for (const auto& [asset, balance] : account.balances) {
            if (balance.IsZero()) return Error{ErrorCode::INVARIANT, "all-zero balance entry stored"};
            const Result<Atoms> held{AddAtoms(balance.available, balance.reserved)};
            if (!held.has_value()) return Error{ErrorCode::INVARIANT, "account balance outside [0, MAX_ATOMS]"};
            const Result<Atoms> sum{AddAtoms(liabilities[asset], *held)};
            if (!sum.has_value()) return Error{ErrorCode::INVARIANT, "liabilities outside [0, MAX_ATOMS]"};
            liabilities[asset] = *sum;
        }
    }
    for (const auto& entry : liabilities) {
        if (!totals.contains(entry.first)) return Error{ErrorCode::INVARIANT, "balance of an asset without totals"};
    }
    for (const auto& [asset, t] : totals) {
        const auto it{liabilities.find(asset)};
        const Atoms owed{it == liabilities.end() ? Atoms{0} : it->second};
        const Result<Atoms> with_pending{AddAtoms(owed, t.pending)};
        if (!with_pending.has_value()) return Error{ErrorCode::INVARIANT, "liabilities and pending outside [0, MAX_ATOMS]"};
        const Result<Atoms> lhs{AddAtoms(*with_pending, t.fees_held)};
        if (!lhs.has_value()) return Error{ErrorCode::INVARIANT, "liabilities and fees outside [0, MAX_ATOMS]"};
        const Result<Atoms> rhs{SubAtoms(t.deposits, t.effects)};
        if (!rhs.has_value()) return Error{ErrorCode::INVARIANT, "effects exceed deposits"};
        if (*lhs != *rhs) return Error{ErrorCode::INVARIANT, "conservation broken"};
    }
    return Ok();
}

LedgerState::LedgerState(ProfileParams profile, AccountId seat_pool, AccountId treasury, NonceWindow empty_window)
    : m_profile{std::move(profile)}, m_seat_pool{seat_pool}, m_treasury{treasury}, m_empty_window{std::move(empty_window)}
{
}

Result<LedgerState> LedgerState::Create(const ProfileParams& profile)
{
    const Result<void> valid{Validate(profile)};
    if (!valid.has_value()) return valid.error();
    Result<NonceWindow> window{NonceWindow::Create(profile.nonce_window)};
    if (!window.has_value()) return window.error();
    const uint256 profile_hash{ProfileHash(profile)};
    return Ok(LedgerState{profile, SeatPoolAccount(profile_hash), TreasuryAccount(profile_hash), std::move(window).value()});
}

bool LedgerState::IsProtocolAccount(const AccountId& account) const
{
    return account == m_seat_pool || account == m_treasury;
}

const AccountState* LedgerState::Find(const AccountId& account) const
{
    const auto it{m_accounts.find(account)};
    return it == m_accounts.end() ? nullptr : &it->second;
}

AccountState* LedgerState::FindMutable(const AccountId& account)
{
    const auto it{m_accounts.find(account)};
    return it == m_accounts.end() ? nullptr : &it->second;
}

Balance LedgerState::BalanceOf(const AccountId& account, const AssetId& asset) const
{
    const AccountState* state{Find(account)};
    return state ? state->BalanceOf(asset) : Balance{};
}

AssetTotals LedgerState::TotalsOf(const AssetId& asset) const
{
    const auto it{m_totals.find(asset)};
    return it == m_totals.end() ? AssetTotals{} : it->second;
}

void LedgerState::PutTotals(const AssetId& asset, const AssetTotals& totals)
{
    if (totals.IsZero()) {
        m_totals.erase(asset);
    } else {
        m_totals[asset] = totals;
    }
}

Result<AccountLeafV0> LedgerState::Leaf(const AccountId& account) const
{
    const AccountState* state{Find(account)};
    if (!state) return UnknownAccount();
    AccountLeafV0 leaf;
    leaf.account = account;
    leaf.balances.reserve(state->balances.size());
    for (const auto& [asset, balance] : state->balances) {
        leaf.balances.push_back(AssetBalance{asset, balance.available, balance.reserved});
    }
    leaf.used_nonces.assign(state->nonces.Used().begin(), state->nonces.Used().end());
    leaf.open_orders = state->open_orders;
    if (!leaf.IsCanonical(m_profile)) return Error{ErrorCode::INVARIANT, "account state is not canonical"};
    return Ok(std::move(leaf));
}

Result<void> LedgerState::CheckConservation() const
{
    const Result<void> balanced{ledger::CheckConservation(m_accounts, m_totals, m_profile.max_open_orders_per_account)};
    if (!balanced.has_value()) return balanced.error();
    std::map<AssetId, std::pair<Atoms, Atoms>> obligations;
    std::set<Outpoint> paid_events;
    size_t pending_count{0};
    for (const auto& [id, record] : m_withdrawals) {
        if (!m_profile.withdrawals_enabled || !m_profile.stable_fee_asset || record.asset != *m_profile.stable_fee_asset ||
            id != record.id || !record.IsCanonical() || !HasAccount(record.account) || IsProtocolAccount(record.account) ||
            record.fee != m_profile.withdrawal_fee_atoms) return Error{ErrorCode::INVARIANT, "noncanonical withdrawal obligation"};
        auto& [pending, effects]{obligations[record.asset]};
        Atoms& total{record.paid ? effects : pending};
        const Result<Atoms> sum{AddAtoms(total, record.amount)};
        if (!sum.has_value()) return Error{ErrorCode::INVARIANT, "withdrawal obligations exceed atom range"};
        total = *sum;
        if (record.paid) {
            if (!paid_events.insert(record.payout_event).second) return Error{ErrorCode::INVARIANT, "payout event reused"};
        } else if (++pending_count > MAX_PENDING_WITHDRAWALS) return Error{ErrorCode::INVARIANT, "pending withdrawal cap"};
    }
    for (const auto& [asset, totals] : m_totals) {
        const auto it{obligations.find(asset)};
        const auto sums{it == obligations.end() ? std::pair<Atoms, Atoms>{0, 0} : it->second};
        if (totals.pending != sums.first || totals.effects != sums.second) return Error{ErrorCode::INVARIANT, "withdrawal totals differ from records"};
    }
    for (const auto& [asset, sums] : obligations) {
        if (!m_totals.contains(asset)) return Error{ErrorCode::INVARIANT, "withdrawal asset without totals"};
    }
    return Ok();
}

Result<void> LedgerState::CreditDeposit(const DepositFact& fact)
{
    if (fact.amount <= 0 || !InAtomRange(fact.amount)) {
        return Error{ErrorCode::INVALID_INPUT, "deposit amount outside [1, MAX_ATOMS]"};
    }
    if (IsProtocolAccount(fact.account)) return Error{ErrorCode::INVALID_INPUT, "deposit to a protocol account"};
    // Outline rule: an outpoint is credited once and any second fact for it is refused.
    // TODO(rebirth:M2-LED-07) identical facts collapse and contradictory ones refuse, as in the frozen model.
    if (m_nullifiers.contains(fact.outpoint)) return Error{ErrorCode::INVALID_INPUT, "deposit outpoint already credited"};

    AssetTotals totals{TotalsOf(fact.asset)};
    const Result<Atoms> deposits{AddAtoms(totals.deposits, fact.amount)};
    if (!deposits.has_value()) return deposits.error();
    const AccountState* existing{Find(fact.account)};
    const Balance before{existing ? existing->BalanceOf(fact.asset) : Balance{}};
    const Result<Atoms> available{AddAtoms(before.available, fact.amount)};
    if (!available.has_value()) return available.error();
    const Balance next{*available, before.reserved};
    const Result<void> cap{CheckAssetCap(existing, fact.asset, next)};
    if (!cap.has_value()) return cap.error();

    // Commit: nothing below can fail.
    AccountState& account{m_accounts.try_emplace(fact.account, m_empty_window).first->second};
    PutBalance(account, fact.asset, next);
    m_nullifiers.insert(fact.outpoint);
    totals.deposits = *deposits;
    PutTotals(fact.asset, totals);
    return Ok();
}

Result<ReserveResult> LedgerState::Reserve(const AccountId& account, const AssetId& asset, Atoms amount)
{
    AccountState* state{FindMutable(account)};
    if (!state) return UnknownAccount();
    if (!InAtomRange(amount)) return BadAmount();
    const Balance before{state->BalanceOf(asset)};
    if (before.available < amount) return Ok(ReserveResult::INSUFFICIENT);
    const Result<Atoms> reserved{AddAtoms(before.reserved, amount)};
    if (!reserved.has_value()) return reserved.error();
    PutBalance(*state, asset, Balance{before.available - amount, *reserved});
    return Ok(ReserveResult::OK);
}

Result<void> LedgerState::Release(const AccountId& account, const AssetId& asset, Atoms amount)
{
    AccountState* state{FindMutable(account)};
    if (!state) return UnknownAccount();
    if (!InAtomRange(amount)) return BadAmount();
    const Balance before{state->BalanceOf(asset)};
    if (before.reserved < amount) return Error{ErrorCode::INVALID_INPUT, "release exceeds the reservation"};
    const Result<Atoms> available{AddAtoms(before.available, amount)};
    if (!available.has_value()) return available.error();
    PutBalance(*state, asset, Balance{*available, before.reserved - amount});
    return Ok();
}

Result<void> LedgerState::PayFromReserved(const AccountId& from, const AccountId& to, const AssetId& asset,
                                          Atoms amount, Atoms withheld_fee)
{
    AccountState* payer{FindMutable(from)};
    AccountState* payee{FindMutable(to)};
    if (!payer || !payee) return UnknownAccount();
    if (!InAtomRange(amount) || !InAtomRange(withheld_fee)) return BadAmount();
    if (withheld_fee > amount) return Error{ErrorCode::INVALID_INPUT, "withheld fee exceeds the payment"};
    const Balance payer_before{payer->BalanceOf(asset)};
    if (payer_before.reserved < amount) return Error{ErrorCode::INVALID_INPUT, "payment exceeds the reservation"};
    const Atoms net{amount - withheld_fee};

    AssetTotals totals{TotalsOf(asset)};
    const Result<Atoms> fees_held{AddAtoms(totals.fees_held, withheld_fee)};
    if (!fees_held.has_value()) return fees_held.error();

    const Balance payer_after{payer_before.available, payer_before.reserved - amount};
    // For a self-trade the payee's starting point is the payer's new balance.
    const Balance payee_before{payer == payee ? payer_after : payee->BalanceOf(asset)};
    const Result<Atoms> payee_available{AddAtoms(payee_before.available, net)};
    if (!payee_available.has_value()) return payee_available.error();
    const Balance payee_after{*payee_available, payee_before.reserved};
    if (payer != payee) {
        const Result<void> cap{CheckAssetCap(payee, asset, payee_after)};
        if (!cap.has_value()) return cap.error();
    }

    // Commit: nothing below can fail.
    if (payer != payee) PutBalance(*payer, asset, payer_after);
    PutBalance(*payee, asset, payee_after);
    totals.fees_held = *fees_held;
    PutTotals(asset, totals);
    return Ok();
}

Result<void> LedgerState::Credit(const AccountId& account, const AssetId& asset, Atoms amount)
{
    if (!IsProtocolAccount(account)) return Error{ErrorCode::INVALID_INPUT, "credit to a non-protocol account"};
    if (!InAtomRange(amount)) return BadAmount();
    AssetTotals totals{TotalsOf(asset)};
    if (totals.fees_held < amount) return Error{ErrorCode::INVALID_INPUT, "credit exceeds the fees held"};
    if (amount == 0) return Ok();
    const AccountState* existing{Find(account)};
    const Balance before{existing ? existing->BalanceOf(asset) : Balance{}};
    const Result<Atoms> available{AddAtoms(before.available, amount)};
    if (!available.has_value()) return available.error();
    const Balance next{*available, before.reserved};
    const Result<void> cap{CheckAssetCap(existing, asset, next)};
    if (!cap.has_value()) return cap.error();

    // Commit: nothing below can fail.
    AccountState& state{m_accounts.try_emplace(account, m_empty_window).first->second};
    PutBalance(state, asset, next);
    totals.fees_held -= amount;
    PutTotals(asset, totals);
    return Ok();
}

Result<OutcomeCode> LedgerState::Transfer(const AccountId& from, const AccountId& to, const AssetId& asset, Atoms amount)
{
    AccountState* sender{FindMutable(from)};
    if (!sender) return Error{ErrorCode::INVALID_INPUT, "unknown sender"};
    if (!InAtomRange(amount)) return BadAmount();
    // O-7 (PROVISIONAL): self-transfer and zero amount are certified rejections.
    if (from == to || amount == 0) return Ok(OutcomeCode::REJ_BAD_PARAMS);
    AccountState* recipient{FindMutable(to)};
    if (!recipient) return Ok(OutcomeCode::REJ_NO_RECIPIENT);
    const Balance sender_before{sender->BalanceOf(asset)};
    if (sender_before.available < amount) return Ok(OutcomeCode::REJ_INSUFFICIENT_AVAILABLE);
    const Balance recipient_before{recipient->BalanceOf(asset)};
    const Result<Atoms> recipient_available{AddAtoms(recipient_before.available, amount)};
    if (!recipient_available.has_value()) return recipient_available.error();
    const Balance recipient_after{*recipient_available, recipient_before.reserved};
    const Result<void> cap{CheckAssetCap(recipient, asset, recipient_after)};
    if (!cap.has_value()) return cap.error();

    // Commit: nothing below can fail.
    PutBalance(*sender, asset, Balance{sender_before.available - amount, sender_before.reserved});
    PutBalance(*recipient, asset, recipient_after);
    return Ok(OutcomeCode::TRANSFERRED);
}

Result<void> LedgerState::ConsumeNonce(const AccountId& account, uint64_t nonce)
{
    AccountState* state{FindMutable(account)};
    if (!state) return UnknownAccount();
    return state->nonces.Insert(nonce);
}

Result<OpenOrderResult> LedgerState::AddOpenOrder(const AccountId& account)
{
    AccountState* state{FindMutable(account)};
    if (!state) return UnknownAccount();
    if (state->open_orders >= m_profile.max_open_orders_per_account) return Ok(OpenOrderResult::AT_CAP);
    ++state->open_orders;
    return Ok(OpenOrderResult::OK);
}

Result<void> LedgerState::RemoveOpenOrder(const AccountId& account)
{
    AccountState* state{FindMutable(account)};
    if (!state) return UnknownAccount();
    if (state->open_orders == 0) return Error{ErrorCode::INVALID_INPUT, "account has no open order"};
    --state->open_orders;
    return Ok();
}

Result<OutcomeCode> LedgerState::RequestWithdrawal(const ActionId& id, const AccountId& account,
                                                    const WithdrawPayload& withdrawal, uint64_t slot)
{
    if (!m_profile.withdrawals_enabled) return Unsupported("M2-LED-05"); // TODO(rebirth:M2-LED-05) legacy profile withdrawals stay disabled.
    if (!HasAccount(account) || IsProtocolAccount(account)) return UnknownAccount();
    if (!m_profile.stable_fee_asset || withdrawal.asset != *m_profile.stable_fee_asset ||
        id.id.IsNull() || slot == 0 || withdrawal.amount <= 0 || !InAtomRange(withdrawal.amount) ||
        withdrawal.destination_script.size() != 20 ||
        std::all_of(withdrawal.destination_script.begin(), withdrawal.destination_script.end(), [](unsigned char b) { return b == 0; })) {
        return Ok(OutcomeCode::REJ_BAD_PARAMS);
    }
    WithdrawalRecord record;
    record.id = id;
    record.account = account;
    record.asset = withdrawal.asset;
    record.amount = withdrawal.amount;
    record.fee = m_profile.withdrawal_fee_atoms;
    std::copy(withdrawal.destination_script.begin(), withdrawal.destination_script.end(), record.recipient.begin());
    record.slot = slot;
    const auto existing{m_withdrawals.find(id)};
    if (existing != m_withdrawals.end()) {
        const WithdrawalRecord& before{existing->second};
        if (before.account != record.account || before.asset != record.asset || before.amount != record.amount ||
            before.fee != record.fee || before.recipient != record.recipient || before.slot != record.slot) {
            return Error{ErrorCode::EQUIVOCATION_REFUSED, "withdrawal identity reused"};
        }
        return Ok(OutcomeCode::WITHDRAWAL_PENDING);
    }
    size_t pending_count{0};
    for (const auto& [key, obligation] : m_withdrawals) if (!obligation.paid) ++pending_count;
    if (pending_count >= MAX_PENDING_WITHDRAWALS) return Ok(OutcomeCode::REJ_BAD_PARAMS);
    const Result<Atoms> debit{AddAtoms(withdrawal.amount, record.fee)};
    if (!debit.has_value()) return Ok(OutcomeCode::REJ_BAD_PARAMS);
    AccountState* owner{FindMutable(account)};
    const Balance before{owner->BalanceOf(withdrawal.asset)};
    if (before.available < *debit) return Ok(OutcomeCode::REJ_INSUFFICIENT_AVAILABLE);
    AssetTotals totals{TotalsOf(withdrawal.asset)};
    const Result<Atoms> pending{AddAtoms(totals.pending, withdrawal.amount)};
    const Result<Atoms> fees{AddAtoms(totals.fees_held, record.fee)};
    if (!pending.has_value()) return pending.error();
    if (!fees.has_value()) return fees.error();
    // Stage the only new node before mutating existing balances/totals.
    m_withdrawals.emplace(id, record);
    PutBalance(*owner, withdrawal.asset, Balance{before.available - *debit, before.reserved});
    totals.pending = *pending;
    totals.fees_held = *fees;
    PutTotals(withdrawal.asset, totals);
    return Ok(OutcomeCode::WITHDRAWAL_PENDING);
}

Result<void> LedgerState::ConfirmWithdrawal(const PayoutConfirmedFact& payout)
{
    if (!m_profile.withdrawals_enabled) return Unsupported("M8-BRG-02"); // TODO(rebirth:M8-BRG-02) legacy profile payout route stays disabled.
    if (!payout.IsCanonical()) return Error{ErrorCode::INVALID_INPUT, "noncanonical payout confirmation"};
    const auto found{m_withdrawals.find(payout.withdrawal)};
    if (found == m_withdrawals.end()) return Error{ErrorCode::VERIFY_FAILED, "payout obligation unknown"};
    WithdrawalRecord& record{found->second};
    if (record.account != payout.account || record.asset != payout.asset || record.amount != payout.amount ||
        record.recipient != payout.recipient) return Error{ErrorCode::VERIFY_FAILED, "payout obligation mismatch"};
    if (record.paid) {
        if (record.payout_event == payout.payout_event) return Ok();
        return Error{ErrorCode::VERIFY_FAILED, "withdrawal already paid by another event"};
    }
    for (const auto& [id, other] : m_withdrawals) {
        if (other.paid && other.payout_event == payout.payout_event) return Error{ErrorCode::VERIFY_FAILED, "payout event reused"};
    }
    AssetTotals totals{TotalsOf(record.asset)};
    if (totals.pending < record.amount) return Error{ErrorCode::INVARIANT, "payout exceeds pending total"};
    const Result<Atoms> effects{AddAtoms(totals.effects, record.amount)};
    if (!effects.has_value()) return effects.error();
    totals.pending -= record.amount;
    totals.effects = *effects;
    PutTotals(record.asset, totals);
    record.paid = true;
    record.payout_event = payout.payout_event;
    return Ok();
}

Result<void> LedgerState::RequestWithdrawal(const AccountId&, const WithdrawPayload&)
{
    if (m_profile.withdrawals_enabled) return Error{ErrorCode::INVALID_INPUT, "authenticated withdrawal identity and slot required"};
    return NotImplemented("M2-LED-05"); // TODO(rebirth:M2-LED-05)
}

Result<void> LedgerState::TransferToFutures(const AccountId&, const AssetId&, Atoms)
{
    return NotImplemented("M2-LED-06"); // TODO(rebirth:M2-LED-06)
}

Result<void> LedgerState::TransferFromFutures(const AccountId&, const AssetId&, Atoms)
{
    return NotImplemented("M2-LED-06"); // TODO(rebirth:M2-LED-06)
}

} // namespace flowmesh2::ledger
