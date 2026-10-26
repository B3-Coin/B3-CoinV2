// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** Stateful fuzz target of the shared spot ledger (M2-LED-08).
 *
 * fm2_ledger_ops runs a sequence of ledger operations chosen by the input
 * against a LedgerState and against a small model written here from the
 * contract in ledger/ledger.h, which shares no ledger code. After every
 * operation:
 *
 * - the result matches the model's: the same ErrorCode, or the same success
 *   value (ReserveResult, OutcomeCode, OpenOrderResult);
 * - every account (balances, used nonces, open orders), the nullifier set,
 *   the totals and the withdrawal records equal the model's (the model
 *   changes nothing on an error or a refusal outcome, so neither may the
 *   ledger);
 * - CheckConservation() passes.
 *
 * Every 32 operations, on the DEEP operation and at the end:
 *
 * - AccountsRootV0 and NullifiersRootV0, which start from the levels the
 *   last Seal stored, equal MerkleRoot over every leaf;
 * - FromLeaves of the ledger's leaves is equal to it and has the same roots;
 * - an account's proof verifies against the root;
 * - CheckConservationSince(checkpoint) passes, and the checkpoint, a copy that
 *   shares structure with the ledger, still has the roots it had when taken.
 *
 * The universe is small, so operations collide: three users and the two
 * protocol accounts, four common assets (four times in five) and 300 more
 * (to reach MAX_LEAF_ASSETS), 48 outpoints, 8 withdrawal ids, 3 payout
 * events. Operations are weighted (WEIGHTS): balance moves are common, and
 * half of them reuse the previous operation's account and asset.
 * Payments are self-trades half the time, and withdrawals are mostly of the
 * stable asset. In one input in four, one burst deposits up to 300
 * distinct assets to one account, often just reaching the asset cap; more
 * bursts would leave every user at the cap and slow every check. The profile is one of the three regtest profiles, as is
 * or with a 3-nonce window and at most 2 open orders, chosen by the input.
 *
 * Run: FUZZ=fm2_ledger_ops bin/fm2_fuzz [corpus dir]; see ARCHITECTURE.md.
 */

#include <flowmesh2/core/account_leaf.h>
#include <flowmesh2/core/action.h>
#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/codes.h>
#include <flowmesh2/core/facts.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/merkle.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/state_root.h>
#include <flowmesh2/ledger/ledger.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <uint256.h>
#include <util/chaintype.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

using namespace flowmesh2;
using ledger::LedgerState;

namespace {

constexpr size_t USERS{3};
constexpr uint32_t OUTPOINTS{48};
constexpr uint32_t WIDE_ASSETS{300};
constexpr uint32_t WITHDRAWAL_IDS{8};
constexpr size_t MAX_OPS{400};
constexpr size_t DEEP_EVERY{32};

const std::vector<ProfileParams>& Profiles()
{
    static const std::vector<ProfileParams> profiles{[] {
        std::vector<ProfileParams> out;
        for (const ProfileParams& base : {DevRegtestProfile(ChainType::REGTEST).value(), StableQViewRegtestProfile(ChainType::REGTEST).value(),
                                          StableQNetworkRegtestProfile(ChainType::REGTEST).value()}) {
            out.push_back(base);
            ProfileParams tight{base};
            tight.nonce_window = 3;
            tight.max_open_orders_per_account = 2;
            out.push_back(tight);
        }
        for (const ProfileParams& profile : out) assert(LedgerState::Create(profile).has_value());
        return out;
    }()};
    return profiles;
}

void InitLedgerOps() { (void)Profiles(); }

//! A non-null 256-bit value naming entry `index` of kind `kind`.
uint256 Label(uint8_t kind, uint32_t index)
{
    uint256 value;
    value.data()[0] = kind;
    for (int i{0}; i < 4; ++i) value.data()[1 + i] = static_cast<unsigned char>(index >> (8 * i));
    value.data()[31] = 0xb3;
    return value;
}

// ---- The model ---------------------------------------------------------------

struct MBalance {
    Atoms available{0};
    Atoms reserved{0};
};

struct MAccount {
    std::map<AssetId, MBalance> balances; //!< never an all-zero entry
    std::set<uint64_t> used;              //!< at most `window` nonces
    uint32_t open{0};
};

/** The ledger contract of ledger/ledger.h, restated: what each operation
 * refuses, in the documented order, and what it changes.
 */
struct Model {
    const ProfileParams& profile;
    AccountId seat_pool;
    AccountId treasury;
    std::map<AccountId, MAccount> accounts;
    std::set<Outpoint> nullifiers;
    std::map<AssetId, AssetTotals> totals; //!< never an all-zero entry
    std::map<ActionId, WithdrawalRecord> withdrawals;

    bool Protocol(const AccountId& a) const { return a == seat_pool || a == treasury; }
    MAccount* Find(const AccountId& a)
    {
        const auto it{accounts.find(a)};
        return it == accounts.end() ? nullptr : &it->second;
    }
    static MBalance BalanceOf(const MAccount* a, const AssetId& asset)
    {
        if (a == nullptr) return MBalance{};
        const auto it{a->balances.find(asset)};
        return it == a->balances.end() ? MBalance{} : it->second;
    }
    static void Put(MAccount& a, const AssetId& asset, MBalance b)
    {
        if (b.available == 0 && b.reserved == 0) {
            a.balances.erase(asset);
        } else {
            a.balances[asset] = b;
        }
    }
    AssetTotals TotalsOf(const AssetId& asset) const
    {
        const auto it{totals.find(asset)};
        return it == totals.end() ? AssetTotals{} : it->second;
    }
    void PutTotals(const AssetId& asset, const AssetTotals& t)
    {
        if (t.IsZero()) {
            totals.erase(asset);
        } else {
            totals[asset] = t;
        }
    }
    //! True iff giving `a` a non-zero `next` of `asset` would exceed MAX_LEAF_ASSETS.
    static bool OverCap(const MAccount* a, const AssetId& asset, MBalance next)
    {
        if (a == nullptr || (next.available == 0 && next.reserved == 0) || a->balances.contains(asset)) return false;
        return a->balances.size() >= MAX_LEAF_ASSETS;
    }
    // D7: acceptable iff unused and (fewer than W used or above the lowest used).
    enum class Nonce { ACCEPTABLE, USED, TOO_LOW };
    Nonce Classify(const MAccount& a, uint64_t n) const
    {
        if (a.used.contains(n)) return Nonce::USED;
        if (a.used.size() < profile.nonce_window || n > *a.used.begin()) return Nonce::ACCEPTABLE;
        return Nonce::TOO_LOW;
    }
};

//! a + b <= MAX_ATOMS, for a and b in [0, MAX_ATOMS].
bool Fits(Atoms a, Atoms b) { return a <= MAX_ATOMS - b; }

//! An expected result: an error code, or success with a value (0 for void).
struct Expect {
    std::optional<ErrorCode> error;
    int value{0};
    bool changes{false}; //!< success that changes the ledger
};
Expect Err(ErrorCode code) { return Expect{code, 0, false}; }
template <class E>
Expect Value(E value, bool changes) { return Expect{std::nullopt, static_cast<int>(value), changes}; }
Expect Done() { return Expect{std::nullopt, 0, true}; }
Expect Unchanged() { return Expect{std::nullopt, 0, false}; }

template <class T>
void Require(const Result<T>& got, const Expect& want)
{
    if (want.error.has_value()) {
        assert(!got.has_value());
        assert(got.error().code == *want.error);
        return;
    }
    assert(got.has_value());
    if constexpr (!std::is_void_v<T>) assert(static_cast<int>(*got) == want.value);
}

// ---- Comparison and the deep checks -------------------------------------------

void Compare(const LedgerState& l, const Model& m)
{
    assert(l.Accounts().size() == m.accounts.size());
    for (const auto& [id, state] : l.Accounts()) {
        const auto it{m.accounts.find(id)};
        assert(it != m.accounts.end());
        const MAccount& want{it->second};
        assert(state.balances.size() == want.balances.size());
        for (const auto& [asset, balance] : state.balances) {
            const auto b{want.balances.find(asset)};
            assert(b != want.balances.end());
            assert(balance.available == b->second.available && balance.reserved == b->second.reserved);
        }
        assert(std::ranges::equal(state.nonces.Used(), want.used));
        assert(state.open_orders == want.open);
    }
    assert(l.Nullifiers().size() == m.nullifiers.size());
    for (const Outpoint& outpoint : m.nullifiers) assert(l.Credited(outpoint));
    assert(l.Totals() == m.totals);
    assert(l.Withdrawals() == m.withdrawals);
    assert(l.CheckConservation().has_value());
}

struct Roots {
    uint256 accounts;
    uint256 nullifiers;
};

//! Both roots from every leaf, without the stored levels.
Roots ScratchRoots(const LedgerState& l)
{
    std::vector<uint256> accounts;
    for (const auto& [id, state] : l.Accounts()) accounts.push_back(l.Leaf(id).value().LeafValue());
    std::vector<uint256> nullifiers;
    for (const Outpoint& outpoint : l.Nullifiers()) nullifiers.push_back(NullifierLeafValue(outpoint));
    return Roots{MerkleRoot(accounts), MerkleRoot(nullifiers)};
}

struct Checkpoint {
    LedgerState ledger;
    Roots roots;
};

void Deep(const LedgerState& l, const std::optional<Checkpoint>& checkpoint, size_t probe)
{
    const Roots scratch{ScratchRoots(l)};
    const Result<uint256> accounts_root{l.AccountsRootV0()};
    assert(accounts_root.has_value() && *accounts_root == scratch.accounts);
    assert(l.NullifiersRootV0() == scratch.nullifiers);

    std::vector<AccountLeafV0> leaves;
    for (const auto& [id, state] : l.Accounts()) leaves.push_back(l.Leaf(id).value());
    const std::vector<Outpoint> nullifiers(l.Nullifiers().begin(), l.Nullifiers().end());
    const std::vector<std::pair<AssetId, AssetTotals>> totals(l.Totals().begin(), l.Totals().end());
    std::vector<WithdrawalRecord> withdrawals;
    for (const auto& entry : l.Withdrawals()) withdrawals.push_back(entry.second);
    const Result<LedgerState> rebuilt{LedgerState::FromLeaves(l.Profile(), leaves, nullifiers, totals, withdrawals)};
    assert(rebuilt.has_value());
    assert(*rebuilt == l);
    assert(rebuilt->AccountsRootV0().value() == scratch.accounts);
    assert(rebuilt->NullifiersRootV0() == scratch.nullifiers);

    if (!leaves.empty()) {
        const AccountLeafV0& leaf{leaves[probe % leaves.size()]};
        const Result<MerkleProof> proof{l.ProveAccountLeafV0(leaf.account)};
        assert(proof.has_value() && MerkleVerify(leaf.LeafValue(), *proof, scratch.accounts));
    }
    if (checkpoint.has_value()) {
        assert(l.CheckConservationSince(checkpoint->ledger).has_value());
        const Roots kept{ScratchRoots(checkpoint->ledger)};
        assert(kept.accounts == checkpoint->roots.accounts && kept.nullifiers == checkpoint->roots.nullifiers);
        assert(checkpoint->ledger.AccountsRootV0().value() == checkpoint->roots.accounts);
    }
}

// ---- Inputs ------------------------------------------------------------------------

struct Universe {
    std::array<AccountId, USERS> users;
    std::array<AssetId, 4> common;

    explicit Universe(const ProfileParams& profile)
    {
        for (size_t i{0}; i < USERS; ++i) users[i] = AccountId{Label(1, static_cast<uint32_t>(i))};
        common = {AssetId::Native(), profile.stable_fee_asset.value_or(AssetId{Label(5, 0)}), AssetId{Label(5, 1)}, AssetId{Label(5, 2)}};
    }
};

class Inputs
{
public:
    Inputs(FuzzedDataProvider& p, const Universe& u, const Model& m) : m_p{p}, m_u{u}, m_m{m} {}

    AccountId Account()
    {
        // A user four times in five, else a protocol account.
        const size_t i{m_p.ConsumeIntegralInRange<size_t>(0, 9)};
        if (i < 8) return m_u.users[i % USERS];
        return i == 8 ? m_m.seat_pool : m_m.treasury;
    }
    AssetId Asset()
    {
        const int i{m_p.ConsumeIntegralInRange<int>(0, 9)};
        if (i < 8) return m_u.common[i % 4];
        return Wide(m_p.ConsumeIntegralInRange<uint32_t>(0, WIDE_ASSETS - 1));
    }
    static AssetId Wide(uint32_t i) { return AssetId{Label(6, i)}; }
    /** An amount: half the time one of `hints` (+-1), else mostly small;
     * rarely zero, near or past MAX_ATOMS, negative or any int64 (each 1 in
     * 64: one near-MAX_ATOMS deposit fills its asset for the rest of the run).
     */
    Atoms Amount(std::initializer_list<Atoms> hints = {})
    {
        const int pick{m_p.ConsumeIntegralInRange<int>(0, 63)};
        switch (pick) {
        case 0: return 0;
        case 1: return MAX_ATOMS - m_p.ConsumeIntegralInRange<Atoms>(0, 3);
        case 2: return MAX_ATOMS + m_p.ConsumeIntegralInRange<Atoms>(1, 3);
        case 3: return -m_p.ConsumeIntegralInRange<Atoms>(1, 3);
        case 4: return m_p.ConsumeIntegral<Atoms>();
        default:
            if (pick <= 36 && hints.size() != 0) {
                const Atoms hint{*(hints.begin() + m_p.ConsumeIntegralInRange<size_t>(0, hints.size() - 1))};
                return std::clamp(hint, -MAX_ATOMS, MAX_ATOMS) + m_p.ConsumeIntegralInRange<Atoms>(-1, 1);
            }
            return m_p.ConsumeIntegralInRange<Atoms>(1, 2'000);
        }
    }
    Outpoint Out() { return Outpoint{Label(2, m_p.ConsumeIntegralInRange<uint32_t>(0, OUTPOINTS - 1)), m_p.ConsumeIntegralInRange<uint32_t>(0, 1)}; }
    uint64_t NonceValue() { return m_p.ConsumeBool() ? m_p.ConsumeIntegralInRange<uint64_t>(0, 12) : m_p.ConsumeIntegral<uint64_t>(); }
    ActionId Withdrawal()
    {
        const uint32_t i{m_p.ConsumeIntegralInRange<uint32_t>(0, WITHDRAWAL_IDS - 1)};
        return i == 0 ? ActionId{} : ActionId{Label(7, i)};
    }
    std::vector<unsigned char> Script()
    {
        switch (m_p.ConsumeIntegralInRange<int>(0, 7)) {
        case 0: return std::vector<unsigned char>(20, 0);
        case 1: return std::vector<unsigned char>(19, 0x51);
        case 2: return std::vector<unsigned char>(21, 0x51);
        case 3: return std::vector<unsigned char>{};
        default: return std::vector<unsigned char>(20, static_cast<unsigned char>(m_p.ConsumeIntegralInRange<int>(1, 3)));
        }
    }
    FuzzedDataProvider& P() { return m_p; }

private:
    FuzzedDataProvider& m_p;
    const Universe& m_u;
    const Model& m_m;
};

// ---- Operations: the model's verdict, then the ledger's -----------------------------

void Deposit(LedgerState& l, Model& m, const DepositFact& f)
{
    MAccount* a{m.Find(f.account)};
    const MBalance before{Model::BalanceOf(a, f.asset)};
    AssetTotals t{m.TotalsOf(f.asset)};
    Expect want{Done()};
    if (f.amount <= 0 || !InAtomRange(f.amount) || m.Protocol(f.account) || m.nullifiers.contains(f.outpoint)) {
        want = Err(ErrorCode::INVALID_INPUT);
    } else if (!Fits(t.deposits, f.amount) || !Fits(before.available, f.amount)) {
        want = Err(ErrorCode::ARITHMETIC_OVERFLOW);
    } else if (Model::OverCap(a, f.asset, MBalance{before.available + f.amount, before.reserved})) {
        want = Err(ErrorCode::OVERSIZE);
    }
    Require(l.CreditDeposit(f), want);
    if (!want.changes) return;
    MAccount& account{m.accounts[f.account]};
    Model::Put(account, f.asset, MBalance{before.available + f.amount, before.reserved});
    m.nullifiers.insert(f.outpoint);
    t.deposits += f.amount;
    m.PutTotals(f.asset, t);
}

void Reserve(LedgerState& l, Model& m, const AccountId& id, const AssetId& asset, Atoms amount)
{
    MAccount* a{m.Find(id)};
    const MBalance before{Model::BalanceOf(a, asset)};
    Expect want{Value(ledger::ReserveResult::OK, true)};
    if (a == nullptr || !InAtomRange(amount)) {
        want = Err(ErrorCode::INVALID_INPUT);
    } else if (before.available < amount) {
        want = Value(ledger::ReserveResult::INSUFFICIENT, false);
    }
    Require(l.Reserve(id, asset, amount), want);
    if (want.changes) Model::Put(*a, asset, MBalance{before.available - amount, before.reserved + amount});
}

void Release(LedgerState& l, Model& m, const AccountId& id, const AssetId& asset, Atoms amount)
{
    MAccount* a{m.Find(id)};
    const MBalance before{Model::BalanceOf(a, asset)};
    const Expect want{a == nullptr || !InAtomRange(amount) || before.reserved < amount ? Err(ErrorCode::INVALID_INPUT) : Done()};
    Require(l.Release(id, asset, amount), want);
    if (want.changes) Model::Put(*a, asset, MBalance{before.available + amount, before.reserved - amount});
}

void Pay(LedgerState& l, Model& m, const AccountId& from, const AccountId& to, const AssetId& asset, Atoms amount, Atoms fee)
{
    MAccount* payer{m.Find(from)};
    MAccount* payee{m.Find(to)};
    AssetTotals t{m.TotalsOf(asset)};
    Expect want{Done()};
    MBalance payer_after{};
    MBalance payee_after{};
    if (payer == nullptr || payee == nullptr || !InAtomRange(amount) || !InAtomRange(fee) || fee > amount ||
        Model::BalanceOf(payer, asset).reserved < amount) {
        want = Err(ErrorCode::INVALID_INPUT);
    } else {
        const MBalance payer_before{Model::BalanceOf(payer, asset)};
        payer_after = MBalance{payer_before.available, payer_before.reserved - amount};
        const MBalance payee_before{payer == payee ? payer_after : Model::BalanceOf(payee, asset)};
        if (!Fits(t.fees_held, fee) || !Fits(payee_before.available, amount - fee)) {
            want = Err(ErrorCode::ARITHMETIC_OVERFLOW);
        } else {
            payee_after = MBalance{payee_before.available + (amount - fee), payee_before.reserved};
            if (payer != payee && Model::OverCap(payee, asset, payee_after)) want = Err(ErrorCode::OVERSIZE);
        }
    }
    Require(l.PayFromReserved(from, to, asset, amount, fee), want);
    if (!want.changes) return;
    Model::Put(*payer, asset, payer_after);
    Model::Put(*payee, asset, payee_after);
    t.fees_held += fee;
    m.PutTotals(asset, t);
}

void Credit(LedgerState& l, Model& m, const AccountId& id, const AssetId& asset, Atoms amount)
{
    MAccount* a{m.Find(id)};
    const MBalance before{Model::BalanceOf(a, asset)};
    AssetTotals t{m.TotalsOf(asset)};
    Expect want{Done()};
    if (!m.Protocol(id) || !InAtomRange(amount) || t.fees_held < amount) {
        want = Err(ErrorCode::INVALID_INPUT);
    } else if (amount == 0) {
        want = Unchanged();
    } else if (!Fits(before.available, amount)) {
        want = Err(ErrorCode::ARITHMETIC_OVERFLOW);
    } else if (Model::OverCap(a, asset, MBalance{before.available + amount, before.reserved})) {
        want = Err(ErrorCode::OVERSIZE);
    }
    Require(l.Credit(id, asset, amount), want);
    if (!want.changes) return;
    Model::Put(m.accounts[id], asset, MBalance{before.available + amount, before.reserved});
    t.fees_held -= amount;
    m.PutTotals(asset, t);
}

void Transfer(LedgerState& l, Model& m, const AccountId& from, const AccountId& to, const AssetId& asset, Atoms amount)
{
    MAccount* sender{m.Find(from)};
    MAccount* recipient{m.Find(to)};
    const MBalance sender_before{Model::BalanceOf(sender, asset)};
    const MBalance recipient_before{Model::BalanceOf(recipient, asset)};
    Expect want{Value(OutcomeCode::TRANSFERRED, true)};
    if (sender == nullptr || !InAtomRange(amount)) {
        want = Err(ErrorCode::INVALID_INPUT);
    } else if (from == to || amount == 0) {
        want = Value(OutcomeCode::REJ_BAD_PARAMS, false);
    } else if (recipient == nullptr) {
        want = Value(OutcomeCode::REJ_NO_RECIPIENT, false);
    } else if (sender_before.available < amount) {
        want = Value(OutcomeCode::REJ_INSUFFICIENT_AVAILABLE, false);
    } else if (!Fits(recipient_before.available, amount)) {
        want = Err(ErrorCode::ARITHMETIC_OVERFLOW);
    } else if (Model::OverCap(recipient, asset, MBalance{recipient_before.available + amount, recipient_before.reserved})) {
        want = Err(ErrorCode::OVERSIZE);
    }
    Require(l.Transfer(from, to, asset, amount), want);
    if (!want.changes) return;
    Model::Put(*sender, asset, MBalance{sender_before.available - amount, sender_before.reserved});
    Model::Put(*recipient, asset, MBalance{recipient_before.available + amount, recipient_before.reserved});
}

void ConsumeNonce(LedgerState& l, Model& m, const AccountId& id, uint64_t nonce)
{
    MAccount* a{m.Find(id)};
    const bool ok{a != nullptr && m.Classify(*a, nonce) == Model::Nonce::ACCEPTABLE};
    Require(l.ConsumeNonce(id, nonce), ok ? Done() : Err(ErrorCode::INVALID_INPUT));
    if (!ok) return;
    a->used.insert(nonce);
    if (a->used.size() > m.profile.nonce_window) a->used.erase(a->used.begin());
}

void AddOpenOrder(LedgerState& l, Model& m, const AccountId& id)
{
    MAccount* a{m.Find(id)};
    Expect want{Value(ledger::OpenOrderResult::OK, true)};
    if (a == nullptr) {
        want = Err(ErrorCode::INVALID_INPUT);
    } else if (a->open >= m.profile.max_open_orders_per_account) {
        want = Value(ledger::OpenOrderResult::AT_CAP, false);
    }
    Require(l.AddOpenOrder(id), want);
    if (want.changes) ++a->open;
}

void RemoveOpenOrder(LedgerState& l, Model& m, const AccountId& id)
{
    MAccount* a{m.Find(id)};
    const bool ok{a != nullptr && a->open > 0};
    Require(l.RemoveOpenOrder(id), ok ? Done() : Err(ErrorCode::INVALID_INPUT));
    if (ok) --a->open;
}

void RequestWithdrawal(LedgerState& l, Model& m, const ActionId& id, const AccountId& account, const WithdrawPayload& w, uint64_t slot)
{
    MAccount* a{m.Find(account)};
    const Atoms fee{m.profile.withdrawal_fee_atoms};
    WithdrawalRecord record;
    record.id = id;
    record.account = account;
    record.asset = w.asset;
    record.amount = w.amount;
    record.fee = fee;
    if (w.destination_script.size() == record.recipient.size()) std::ranges::copy(w.destination_script, record.recipient.begin());
    record.slot = slot;
    const auto existing{m.withdrawals.find(id)};
    size_t pending{0};
    for (const auto& entry : m.withdrawals) pending += entry.second.paid ? 0 : 1;
    const MBalance before{Model::BalanceOf(a, w.asset)};
    AssetTotals t{m.TotalsOf(w.asset)};

    Expect want{Value(OutcomeCode::WITHDRAWAL_PENDING, true)};
    if (!m.profile.withdrawals_enabled) {
        want = Err(ErrorCode::UNSUPPORTED);
    } else if (a == nullptr || m.Protocol(account)) {
        want = Err(ErrorCode::INVALID_INPUT);
    } else if (!m.profile.stable_fee_asset || w.asset != *m.profile.stable_fee_asset || id.id.IsNull() || slot == 0 || w.amount <= 0 ||
               !InAtomRange(w.amount) || w.destination_script.size() != 20 ||
               std::ranges::all_of(w.destination_script, [](unsigned char b) { return b == 0; })) {
        want = Value(OutcomeCode::REJ_BAD_PARAMS, false);
    } else if (existing != m.withdrawals.end()) {
        const WithdrawalRecord& was{existing->second};
        const bool same{was.account == record.account && was.asset == record.asset && was.amount == record.amount && was.fee == record.fee &&
                        was.recipient == record.recipient && was.slot == record.slot};
        want = same ? Value(OutcomeCode::WITHDRAWAL_PENDING, false) : Err(ErrorCode::EQUIVOCATION_REFUSED);
    } else if (pending >= ledger::MAX_PENDING_WITHDRAWALS || !Fits(w.amount, fee)) {
        want = Value(OutcomeCode::REJ_BAD_PARAMS, false);
    } else if (before.available < w.amount + fee) {
        want = Value(OutcomeCode::REJ_INSUFFICIENT_AVAILABLE, false);
    } else if (!Fits(t.pending, w.amount) || !Fits(t.fees_held, fee)) {
        want = Err(ErrorCode::ARITHMETIC_OVERFLOW);
    }
    Require(l.RequestWithdrawal(id, account, w, slot), want);
    if (!want.changes) return;
    m.withdrawals.emplace(id, record);
    Model::Put(*a, w.asset, MBalance{before.available - (w.amount + fee), before.reserved});
    t.pending += w.amount;
    t.fees_held += fee;
    m.PutTotals(w.asset, t);
}

void ConfirmWithdrawal(LedgerState& l, Model& m, const PayoutConfirmedFact& p)
{
    const auto found{m.withdrawals.find(p.withdrawal)};
    Expect want{Done()};
    if (!m.profile.withdrawals_enabled) {
        want = Err(ErrorCode::UNSUPPORTED);
    } else if (!p.IsCanonical()) {
        want = Err(ErrorCode::INVALID_INPUT);
    } else if (found == m.withdrawals.end()) {
        want = Err(ErrorCode::VERIFY_FAILED);
    } else {
        const WithdrawalRecord& r{found->second};
        const bool reused{std::ranges::any_of(m.withdrawals, [&](const auto& e) { return e.second.paid && e.second.payout_event == p.payout_event; })};
        if (r.account != p.account || r.asset != p.asset || r.amount != p.amount || r.recipient != p.recipient) {
            want = Err(ErrorCode::VERIFY_FAILED);
        } else if (r.paid) {
            want = r.payout_event == p.payout_event ? Unchanged() : Err(ErrorCode::VERIFY_FAILED);
        } else if (reused) {
            want = Err(ErrorCode::VERIFY_FAILED);
        } else if (!Fits(m.TotalsOf(r.asset).effects, r.amount)) {
            want = Err(ErrorCode::ARITHMETIC_OVERFLOW);
        }
    }
    Require(l.ConfirmWithdrawal(p), want);
    if (!want.changes) return;
    WithdrawalRecord& r{found->second};
    AssetTotals t{m.TotalsOf(r.asset)};
    t.pending -= r.amount;
    t.effects += r.amount;
    m.PutTotals(r.asset, t);
    r.paid = true;
    r.payout_event = p.payout_event;
}

PayoutConfirmedFact Payout(Inputs& in, const Model& m)
{
    PayoutConfirmedFact p;
    p.withdrawal = in.Withdrawal();
    const auto found{m.withdrawals.find(p.withdrawal)};
    if (found != m.withdrawals.end() && in.P().ConsumeBool()) {
        // The record's own fields, with at most one of them altered.
        p.account = found->second.account;
        p.asset = found->second.asset;
        p.amount = found->second.amount;
        p.recipient = found->second.recipient;
        switch (in.P().ConsumeIntegralInRange<int>(0, 7)) {
        case 0: p.account = in.Account(); break;
        case 1: p.amount += 1; break;
        case 2: p.recipient[0] ^= 1; break;
        case 3: p.asset = in.Asset(); break;
        default: break;
        }
    } else {
        p.account = in.Account();
        p.asset = in.Asset();
        p.amount = in.Amount();
        p.recipient.fill(static_cast<unsigned char>(in.P().ConsumeIntegralInRange<int>(0, 3)));
    }
    // Half the time the event of a paid withdrawal, which no other withdrawal may reuse.
    std::vector<Outpoint> paid_events;
    for (const auto& entry : m.withdrawals) {
        if (entry.second.paid) paid_events.push_back(entry.second.payout_event);
    }
    if (!paid_events.empty() && in.P().ConsumeBool()) {
        p.payout_event = paid_events[in.P().ConsumeIntegralInRange<size_t>(0, paid_events.size() - 1)];
    } else {
        const uint32_t event{in.P().ConsumeIntegralInRange<uint32_t>(0, 3)};
        p.payout_event = event == 0 ? Outpoint{} : Outpoint{Label(8, event), 0};
    }
    p.domain = in.P().ConsumeIntegralInRange<int>(0, 15) == 0 ? uint256{} : Label(9, 0);
    return p;
}

//! The operations, with their weights out of 64: balance moves dominate, a burst is rare.
enum class Op { DEPOSIT, RESERVE, RELEASE, PAY, CREDIT, TRANSFER, NONCE, ADD_ORDER, REMOVE_ORDER, WITHDRAW, CONFIRM, BURST, SEAL, CHECKPOINT, DEEP };
constexpr std::array<std::pair<Op, int>, 15> WEIGHTS{{
    {Op::DEPOSIT, 10}, {Op::RESERVE, 6}, {Op::RELEASE, 4}, {Op::PAY, 6}, {Op::CREDIT, 4}, {Op::TRANSFER, 6}, {Op::NONCE, 4}, {Op::ADD_ORDER, 3},
    {Op::REMOVE_ORDER, 2}, {Op::WITHDRAW, 6}, {Op::CONFIRM, 5}, {Op::BURST, 1}, {Op::SEAL, 3}, {Op::CHECKPOINT, 2}, {Op::DEEP, 2},
}};
static_assert([] {
    int total{0};
    for (const auto& entry : WEIGHTS) total += entry.second;
    return total == 64;
}());

Op PickOp(FuzzedDataProvider& p)
{
    int pick{p.ConsumeIntegralInRange<int>(0, 63)};
    for (const auto& [op, weight] : WEIGHTS) {
        if (pick < weight) return op;
        pick -= weight;
    }
    return Op::DEEP;
}

} // namespace

FUZZ_TARGET(fm2_ledger_ops, .init = InitLedgerOps)
{
    FuzzedDataProvider p{buffer.data(), buffer.size()};
    const ProfileParams& profile{Profiles()[p.ConsumeIntegralInRange<size_t>(0, Profiles().size() - 1)]};
    LedgerState l{LedgerState::Create(profile).value()};
    Model m{profile, l.SeatPool(), l.Treasury(), {}, {}, {}, {}};
    const Universe u{profile};
    Inputs in{p, u, m};
    std::optional<Checkpoint> checkpoint;
    // A burst leaves an account with ~256 balances, which makes every later
    // check of the input ~50 times slower, so one input in four may burst.
    bool burst_done{p.ConsumeIntegralInRange<int>(0, 3) != 0};
    AccountId a;
    AssetId asset;

    for (size_t step{0}; step < MAX_OPS && p.remaining_bytes() > 0; ++step) {
        // Half the time the previous operation's account and asset, so chains on one balance are common.
        if (step == 0 || !p.ConsumeBool()) {
            a = in.Account();
            asset = in.Asset();
        }
        const MBalance held{Model::BalanceOf(m.Find(a), asset)};
        switch (PickOp(p)) {
        case Op::DEPOSIT: Deposit(l, m, DepositFact{FactCursor{1, 0}, in.Out(), a, asset, in.Amount()}); break;
        case Op::RESERVE: Reserve(l, m, a, asset, in.Amount({held.available, held.reserved})); break;
        case Op::RELEASE: Release(l, m, a, asset, in.Amount({held.reserved, held.available})); break;
        case Op::PAY: {
            const AccountId to{p.ConsumeBool() ? a : in.Account()}; // O-17 self-trades
            const Atoms amount{in.Amount({held.reserved})};
            Pay(l, m, a, to, asset, amount, in.Amount({amount, amount / 1000, 0}));
            break;
        }
        case Op::CREDIT: Credit(l, m, a, asset, in.Amount({m.TotalsOf(asset).fees_held})); break;
        case Op::TRANSFER: Transfer(l, m, a, in.Account(), asset, in.Amount({held.available})); break;
        case Op::NONCE: ConsumeNonce(l, m, a, in.NonceValue()); break;
        case Op::ADD_ORDER: AddOpenOrder(l, m, a); break;
        case Op::REMOVE_ORDER: RemoveOpenOrder(l, m, a); break;
        case Op::WITHDRAW: {
            const ActionId id{in.Withdrawal()};
            const AssetId paid{p.ConsumeBool() ? u.common[1] : asset};
            const Atoms free{Model::BalanceOf(m.Find(a), paid).available};
            WithdrawPayload w{paid, in.Amount({free, free - profile.withdrawal_fee_atoms}), in.Script()};
            RequestWithdrawal(l, m, id, a, w, p.ConsumeIntegralInRange<uint64_t>(0, 3));
            break;
        }
        case Op::CONFIRM: ConfirmWithdrawal(l, m, Payout(in, m)); break;
        case Op::BURST: {
            // At most once per input: 1-atom deposits of distinct assets to one account, to reach MAX_LEAF_ASSETS.
            if (burst_done) break;
            burst_done = true;
            const uint32_t first{p.ConsumeIntegralInRange<uint32_t>(0, WIDE_ASSETS - 1)};
            const uint32_t count{p.ConsumeBool() ? static_cast<uint32_t>(MAX_LEAF_ASSETS) + p.ConsumeIntegralInRange<uint32_t>(0, 2)
                                                 : p.ConsumeIntegralInRange<uint32_t>(1, WIDE_ASSETS)};
            for (uint32_t i{0}; i < count; ++i) {
                const uint32_t index{(first + i) % WIDE_ASSETS};
                Deposit(l, m, DepositFact{FactCursor{1, 0}, Outpoint{Label(10, index), 0}, a, Inputs::Wide(index), 1});
            }
            break;
        }
        case Op::SEAL: {
            const LedgerState before{l};
            const Result<void> sealed{l.Seal()};
            assert(sealed.has_value() && l == before);
            break;
        }
        case Op::CHECKPOINT: {
            Checkpoint taken{l, ScratchRoots(l)};
            checkpoint.emplace(std::move(taken));
            break;
        }
        case Op::DEEP: Deep(l, checkpoint, step); break;
        }
        Compare(l, m);
        if (step % DEEP_EVERY == DEEP_EVERY - 1) Deep(l, checkpoint, step);
    }
    Deep(l, checkpoint, 0);
}
