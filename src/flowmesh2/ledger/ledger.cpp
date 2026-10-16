// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/ledger/ledger.h>

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
    return ledger::CheckConservation(m_accounts, m_totals, m_profile.max_open_orders_per_account);
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

Result<void> LedgerState::RequestWithdrawal(const AccountId&, const WithdrawPayload&)
{
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
