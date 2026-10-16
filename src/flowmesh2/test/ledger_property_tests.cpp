// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/codes.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/ledger/ledger.h>
#include <flowmesh2/test/util/ledger_fixtures.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::ledger;
using namespace flowmesh2::test;

namespace {

//! splitmix64: a tiny deterministic generator, independent of the code under test.
struct Rng {
    uint64_t state;
    uint64_t Next()
    {
        uint64_t z{state += 0x9e3779b97f4a7c15ULL};
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
    uint64_t Below(uint64_t n) { return n == 0 ? 0 : Next() % n; }
    bool Percent(uint64_t p) { return Below(100) < p; }
};

//! The verdict of one operation, as the ledger returned it or the shadow predicts it.
enum class Verdict { OK, INSUFFICIENT, AT_CAP, REJ_BAD_PARAMS, REJ_NO_RECIPIENT, REJ_INSUFFICIENT, ERR_INVALID, OTHER };

const char* Name(Verdict v)
{
    switch (v) {
    case Verdict::OK: return "OK";
    case Verdict::INSUFFICIENT: return "INSUFFICIENT";
    case Verdict::AT_CAP: return "AT_CAP";
    case Verdict::REJ_BAD_PARAMS: return "REJ_BAD_PARAMS";
    case Verdict::REJ_NO_RECIPIENT: return "REJ_NO_RECIPIENT";
    case Verdict::REJ_INSUFFICIENT: return "REJ_INSUFFICIENT";
    case Verdict::ERR_INVALID: return "ERR_INVALID";
    case Verdict::OTHER: return "OTHER";
    }
    return "?";
}

Verdict Of(const Result<void>& r)
{
    if (r.has_value()) return Verdict::OK;
    return r.error().code == ErrorCode::INVALID_INPUT ? Verdict::ERR_INVALID : Verdict::OTHER;
}
Verdict Of(const Result<ReserveResult>& r)
{
    if (!r.has_value()) return r.error().code == ErrorCode::INVALID_INPUT ? Verdict::ERR_INVALID : Verdict::OTHER;
    return *r == ReserveResult::OK ? Verdict::OK : *r == ReserveResult::INSUFFICIENT ? Verdict::INSUFFICIENT : Verdict::OTHER;
}
Verdict Of(const Result<OpenOrderResult>& r)
{
    if (!r.has_value()) return r.error().code == ErrorCode::INVALID_INPUT ? Verdict::ERR_INVALID : Verdict::OTHER;
    return *r == OpenOrderResult::OK ? Verdict::OK : *r == OpenOrderResult::AT_CAP ? Verdict::AT_CAP : Verdict::OTHER;
}
Verdict Of(const Result<OutcomeCode>& r)
{
    if (!r.has_value()) return r.error().code == ErrorCode::INVALID_INPUT ? Verdict::ERR_INVALID : Verdict::OTHER;
    switch (*r) {
    case OutcomeCode::TRANSFERRED: return Verdict::OK;
    case OutcomeCode::REJ_BAD_PARAMS: return Verdict::REJ_BAD_PARAMS;
    case OutcomeCode::REJ_NO_RECIPIENT: return Verdict::REJ_NO_RECIPIENT;
    case OutcomeCode::REJ_INSUFFICIENT_AVAILABLE: return Verdict::REJ_INSUFFICIENT;
    default: return Verdict::OTHER;
    }
}

/** Independent shadow of the ledger, written from the ledger.h contract with
 * plain maps and int64 sums (all values stay far below 2^62 here). It predicts
 * each verdict before the ledger runs and applies only its own update.
 */
struct Shadow {
    struct Account {
        std::map<AssetId, std::pair<int64_t, int64_t>> balances; // available, reserved; no zero entry
        std::set<uint64_t> nonces;
        uint32_t open{0};
    };
    uint32_t window;
    uint32_t cap;
    std::set<AccountId> protocol;
    std::map<AccountId, Account> accounts;
    std::set<Outpoint> credited;
    std::map<AssetId, int64_t> deposits;
    std::map<AssetId, int64_t> fees;

    std::pair<int64_t, int64_t> Get(const AccountId& a, const AssetId& asset) const
    {
        const auto it{accounts.find(a)};
        if (it == accounts.end()) return {0, 0};
        const auto jt{it->second.balances.find(asset)};
        return jt == it->second.balances.end() ? std::pair<int64_t, int64_t>{0, 0} : jt->second;
    }
    void Set(const AccountId& a, const AssetId& asset, std::pair<int64_t, int64_t> b)
    {
        if (b.first == 0 && b.second == 0) {
            accounts.at(a).balances.erase(asset);
        } else {
            accounts.at(a).balances[asset] = b;
        }
    }
    bool Has(const AccountId& a) const { return accounts.contains(a); }
};

struct Stats {
    std::map<std::string, uint64_t> counts;
    void Add(const std::string& op, Verdict v) { ++counts[op + ":" + Name(v)]; }
    uint64_t Get(const std::string& key) const
    {
        const auto it{counts.find(key)};
        return it == counts.end() ? 0 : it->second;
    }
};

Outpoint FreshOutpoint(uint64_t seed, uint64_t counter)
{
    uint256 txid;
    std::memcpy(txid.data(), &seed, sizeof(seed));
    std::memcpy(txid.data() + 8, &counter, sizeof(counter));
    return Outpoint{txid, static_cast<uint32_t>(counter % 4)};
}

//! "" if the ledger equals the shadow and conservation holds, else the first difference.
std::string Compare(const LedgerState& ledger, const Shadow& shadow, const std::vector<AssetId>& assets)
{
    const Result<void> conservation{ledger.CheckConservation()};
    if (!conservation.has_value()) return "CheckConservation: " + std::string{conservation.error().detail};
    if (ledger.Accounts().size() != shadow.accounts.size()) return "account count";
    for (const auto& [id, account] : shadow.accounts) {
        const AccountState* state{ledger.Find(id)};
        if (!state) return "missing account";
        if (state->balances.size() != account.balances.size()) return "balance entry count";
        for (const auto& [asset, b] : account.balances) {
            const Balance got{state->BalanceOf(asset)};
            if (got.available != b.first || got.reserved != b.second) return "balance";
        }
        if (!std::equal(state->nonces.Used().begin(), state->nonces.Used().end(), account.nonces.begin(), account.nonces.end())) {
            return "nonces";
        }
        if (state->open_orders != account.open) return "open orders";
    }
    if (ledger.Nullifiers() != shadow.credited) return "nullifiers";
    for (const AssetId& asset : assets) {
        const AssetTotals t{ledger.TotalsOf(asset)};
        const int64_t deposits{shadow.deposits.contains(asset) ? shadow.deposits.at(asset) : 0};
        const int64_t fees{shadow.fees.contains(asset) ? shadow.fees.at(asset) : 0};
        if (t.deposits != deposits || t.fees_held != fees || t.effects != 0 || t.pending != 0) return "totals";
        // Independent tally: what the accounts hold plus the fees held equals the deposits.
        int64_t ledger_sum{0};
        int64_t shadow_sum{0};
        for (const auto& [id, state] : ledger.Accounts()) {
            const Balance b{state.BalanceOf(asset)};
            ledger_sum += b.available + b.reserved;
        }
        for (const auto& [id, account] : shadow.accounts) {
            const auto it{account.balances.find(asset)};
            if (it != account.balances.end()) shadow_sum += it->second.first + it->second.second;
        }
        if (ledger_sum + t.fees_held != deposits) return "ledger tally";
        if (shadow_sum + fees != deposits) return "shadow tally";
    }
    return "";
}

} // namespace

BOOST_AUTO_TEST_SUITE(ledger_property_tests)

BOOST_AUTO_TEST_CASE(random_ops_keep_conservation_and_match_an_independent_tally)
{
    // A small window and open-order cap so that eviction and AT_CAP occur.
    ProfileParams profile{RegtestProfile()};
    profile.nonce_window = 8;
    profile.max_open_orders_per_account = 4;

    const std::vector<AssetId> assets{AssetId::Native(), TestAsset("rUSD"), TestAsset("TOK")};
    constexpr int SEEDS{64};
    constexpr int OPS{10'000};
    Stats stats;
    uint64_t total_ops{0};

    for (int seed{1}; seed <= SEEDS; ++seed) {
        Rng rng{static_cast<uint64_t>(seed) * 0x2545f4914f6cdd1dULL};
        LedgerState ledger{EmptyLedger(profile)};
        Shadow shadow{profile.nonce_window, profile.max_open_orders_per_account, {ledger.SeatPool(), ledger.Treasury()}, {}, {}, {}, {}};
        // Six users, a ghost that never gets an account, and the two protocol accounts.
        std::vector<AccountId> users;
        for (int i{0}; i < 6; ++i) users.push_back(TestAccount("prop-" + std::to_string(i)));
        const AccountId ghost{TestAccount("prop-ghost")};
        std::vector<AccountId> anyone{users};
        anyone.push_back(ghost);
        anyone.push_back(ledger.SeatPool());
        anyone.push_back(ledger.Treasury());
        std::vector<Outpoint> history;
        uint64_t counter{0};

        for (int op{0}; op < OPS; ++op) {
            ++total_ops;
            const uint64_t kind{rng.Below(100)};
            const AssetId& asset{assets[rng.Below(assets.size())]};
            std::string name;
            Verdict want{Verdict::OTHER};
            Verdict got{Verdict::OTHER};

            if (kind < 15) {
                name = "deposit";
                const AccountId target{rng.Percent(5) ? anyone[7 + rng.Below(2)] : users[rng.Below(users.size())]};
                const Atoms amount{rng.Percent(2) ? 0 : static_cast<Atoms>(1 + rng.Below(1'000'000'000))};
                const Outpoint outpoint{(!history.empty() && rng.Percent(5)) ? history[rng.Below(history.size())]
                                                                              : FreshOutpoint(seed, ++counter)};
                if (shadow.protocol.contains(target) || amount <= 0 || shadow.credited.contains(outpoint)) {
                    want = Verdict::ERR_INVALID;
                } else {
                    want = Verdict::OK;
                }
                got = Of(ledger.CreditDeposit(Deposit(target, asset, amount, outpoint)));
                if (want == Verdict::OK && got == Verdict::OK) {
                    shadow.accounts.try_emplace(target);
                    auto b{shadow.Get(target, asset)};
                    b.first += amount;
                    shadow.Set(target, asset, b);
                    shadow.credited.insert(outpoint);
                    shadow.deposits[asset] += amount;
                    history.push_back(outpoint);
                }
            } else if (kind < 30) {
                name = "reserve";
                const AccountId& a{anyone[rng.Below(anyone.size())]};
                const auto b{shadow.Get(a, asset)};
                const Atoms amount{static_cast<Atoms>(rng.Below(static_cast<uint64_t>(b.first) * 5 / 4 + 2))};
                want = !shadow.Has(a) ? Verdict::ERR_INVALID : b.first < amount ? Verdict::INSUFFICIENT : Verdict::OK;
                got = Of(ledger.Reserve(a, asset, amount));
                if (want == Verdict::OK && got == Verdict::OK) shadow.Set(a, asset, {b.first - amount, b.second + amount});
            } else if (kind < 40) {
                name = "release";
                const AccountId& a{anyone[rng.Below(anyone.size())]};
                const auto b{shadow.Get(a, asset)};
                const Atoms amount{static_cast<Atoms>(rng.Below(static_cast<uint64_t>(b.second) * 5 / 4 + 2))};
                want = (!shadow.Has(a) || b.second < amount) ? Verdict::ERR_INVALID : Verdict::OK;
                got = Of(ledger.Release(a, asset, amount));
                if (want == Verdict::OK && got == Verdict::OK) shadow.Set(a, asset, {b.first + amount, b.second - amount});
            } else if (kind < 52) {
                name = "pay";
                const AccountId& from{anyone[rng.Below(anyone.size())]};
                const AccountId& to{rng.Percent(25) ? from : anyone[rng.Below(anyone.size())]};
                const auto fb{shadow.Get(from, asset)};
                const Atoms amount{static_cast<Atoms>(rng.Below(static_cast<uint64_t>(fb.second) * 11 / 10 + 2))};
                const uint64_t fee_kind{rng.Below(10)};
                const Atoms fee{fee_kind < 3 ? 0 : fee_kind < 9 ? static_cast<Atoms>(rng.Below(static_cast<uint64_t>(amount) / 100 + 1)) : amount + 1};
                want = (!shadow.Has(from) || !shadow.Has(to) || fee > amount || fb.second < amount) ? Verdict::ERR_INVALID : Verdict::OK;
                got = Of(ledger.PayFromReserved(from, to, asset, amount, fee));
                if (want == Verdict::OK && got == Verdict::OK) {
                    shadow.Set(from, asset, {fb.first, fb.second - amount});
                    auto tb{shadow.Get(to, asset)};
                    tb.first += amount - fee;
                    shadow.Set(to, asset, tb);
                    shadow.fees[asset] += fee;
                }
            } else if (kind < 60) {
                name = "credit";
                const AccountId target{rng.Percent(80) ? anyone[7 + rng.Below(2)] : anyone[rng.Below(7)]};
                const int64_t held{shadow.fees.contains(asset) ? shadow.fees.at(asset) : 0};
                const Atoms amount{static_cast<Atoms>(rng.Below(static_cast<uint64_t>(held) * 6 / 5 + 2))};
                want = (!shadow.protocol.contains(target) || held < amount) ? Verdict::ERR_INVALID : Verdict::OK;
                got = Of(ledger.Credit(target, asset, amount));
                if (want == Verdict::OK && got == Verdict::OK && amount > 0) {
                    shadow.accounts.try_emplace(target);
                    auto b{shadow.Get(target, asset)};
                    b.first += amount;
                    shadow.Set(target, asset, b);
                    shadow.fees[asset] -= amount;
                }
            } else if (kind < 75) {
                name = "transfer";
                const AccountId& from{anyone[rng.Below(anyone.size())]};
                const AccountId& to{rng.Percent(20) ? from : anyone[rng.Below(anyone.size())]};
                const auto fb{shadow.Get(from, asset)};
                const Atoms amount{rng.Percent(10) ? 0 : static_cast<Atoms>(rng.Below(static_cast<uint64_t>(fb.first) * 5 / 4 + 2))};
                if (!shadow.Has(from)) {
                    want = Verdict::ERR_INVALID;
                } else if (from == to || amount == 0) {
                    want = Verdict::REJ_BAD_PARAMS;
                } else if (!shadow.Has(to)) {
                    want = Verdict::REJ_NO_RECIPIENT;
                } else if (fb.first < amount) {
                    want = Verdict::REJ_INSUFFICIENT;
                } else {
                    want = Verdict::OK;
                }
                got = Of(ledger.Transfer(from, to, asset, amount));
                if (want == Verdict::OK && got == Verdict::OK) {
                    shadow.Set(from, asset, {fb.first - amount, fb.second});
                    auto tb{shadow.Get(to, asset)};
                    tb.first += amount;
                    shadow.Set(to, asset, tb);
                }
            } else if (kind < 90) {
                name = "nonce";
                const AccountId& a{anyone[rng.Below(anyone.size())]};
                const uint64_t nonce{rng.Below(40)};
                if (!shadow.Has(a)) {
                    want = Verdict::ERR_INVALID;
                } else {
                    const std::set<uint64_t>& used{shadow.accounts.at(a).nonces};
                    const bool ok{!used.contains(nonce) && (used.size() < shadow.window || nonce > *used.begin())};
                    want = ok ? Verdict::OK : Verdict::ERR_INVALID;
                }
                got = Of(ledger.ConsumeNonce(a, nonce));
                if (want == Verdict::OK && got == Verdict::OK) {
                    std::set<uint64_t>& used{shadow.accounts.at(a).nonces};
                    used.insert(nonce);
                    if (used.size() > shadow.window) {
                        used.erase(used.begin());
                        stats.Add("nonce-evict", Verdict::OK);
                    }
                }
            } else if (kind < 95) {
                name = "open";
                const AccountId& a{anyone[rng.Below(anyone.size())]};
                want = !shadow.Has(a) ? Verdict::ERR_INVALID : shadow.accounts.at(a).open >= shadow.cap ? Verdict::AT_CAP : Verdict::OK;
                got = Of(ledger.AddOpenOrder(a));
                if (want == Verdict::OK && got == Verdict::OK) ++shadow.accounts.at(a).open;
            } else {
                name = "close";
                const AccountId& a{anyone[rng.Below(anyone.size())]};
                want = (!shadow.Has(a) || shadow.accounts.at(a).open == 0) ? Verdict::ERR_INVALID : Verdict::OK;
                got = Of(ledger.RemoveOpenOrder(a));
                if (want == Verdict::OK && got == Verdict::OK) --shadow.accounts.at(a).open;
            }

            stats.Add(name, got);
            if (got != want) {
                BOOST_FAIL("seed " << seed << " op " << op << " " << name << ": ledger " << Name(got) << ", shadow " << Name(want));
            }
            const std::string difference{Compare(ledger, shadow, assets)};
            if (!difference.empty()) BOOST_FAIL("seed " << seed << " op " << op << " after " << name << ": " << difference);
        }
    }

    BOOST_CHECK_EQUAL(total_ops, static_cast<uint64_t>(SEEDS) * OPS);
    for (const auto& [key, count] : stats.counts) BOOST_TEST_MESSAGE(key << " " << count);
    // Every branch was exercised.
    for (const char* key : {"deposit:OK", "deposit:ERR_INVALID", "reserve:OK", "reserve:INSUFFICIENT", "reserve:ERR_INVALID",
                            "release:OK", "release:ERR_INVALID", "pay:OK", "pay:ERR_INVALID", "credit:OK", "credit:ERR_INVALID",
                            "transfer:OK", "transfer:REJ_BAD_PARAMS", "transfer:REJ_NO_RECIPIENT", "transfer:REJ_INSUFFICIENT",
                            "transfer:ERR_INVALID", "nonce:OK", "nonce:ERR_INVALID", "nonce-evict:OK", "open:OK", "open:AT_CAP",
                            "open:ERR_INVALID", "close:OK", "close:ERR_INVALID"}) {
        BOOST_CHECK_MESSAGE(stats.Get(key) > 0, std::string{"never exercised: "} + key);
    }
}

BOOST_AUTO_TEST_SUITE_END()
