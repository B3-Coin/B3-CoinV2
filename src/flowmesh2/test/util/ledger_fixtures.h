// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_TEST_UTIL_LEDGER_FIXTURES_H
#define B3_FLOWMESH2_TEST_UTIL_LEDGER_FIXTURES_H

#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/authority.h>
#include <flowmesh2/core/facts.h>
#include <flowmesh2/core/hashes.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/market.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/ledger/ledger.h>
#include <flowmesh2/test/util/keys.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

/** Shared TEST fixtures of the ledger and commitment tests (slice R5).
 *
 * Synthetic values only: assets are Tagged(TEST_ASSET, name) as in
 * ARCHITECTURE.md section 14.1, accounts and outpoints are SHA256(label).
 * No real network value, key or datadir is used.
 */
namespace flowmesh2::test {

inline ProfileParams RegtestProfile() { return DevRegtestProfile(ChainType::REGTEST).value(); }

//! Tagged(TEST_ASSET, name); "rUSD" and "TOK" are the walking-skeleton assets.
inline AssetId TestAsset(std::string_view name)
{
    return AssetId{Tagged(HashTag::TEST_ASSET, std::span{reinterpret_cast<const unsigned char*>(name.data()), name.size()})};
}

//! AccountId{SHA256("fm2/test/account/" + name)}: a synthetic, keyless test account.
inline AccountId TestAccount(std::string_view name) { return AccountId{LabelHash("fm2/test/account/" + std::string{name})}; }

//! Outpoint{SHA256("fm2/test/" + tx), vout}.
inline Outpoint TestOutpoint(std::string_view tx, uint32_t vout)
{
    return Outpoint{LabelHash("fm2/test/" + std::string{tx}), vout};
}

inline DepositFact Deposit(const AccountId& account, const AssetId& asset, Atoms amount, const Outpoint& outpoint)
{
    DepositFact fact;
    fact.cursor = FactCursor{130, 0};
    fact.outpoint = outpoint;
    fact.account = account;
    fact.asset = asset;
    fact.amount = amount;
    return fact;
}

//! ARCHITECTURE.md section 14.1: M1 = B3/rUSD, M2 = TOK/rUSD.
inline MarketDescriptor MarketM1() { return MarketDescriptor{AssetId::Native(), TestAsset("rUSD"), 1'000'000, 1}; }
inline MarketDescriptor MarketM2() { return MarketDescriptor{TestAsset("TOK"), TestAsset("rUSD"), 1, 10}; }

inline ledger::LedgerState EmptyLedger(const ProfileParams& profile)
{
    Result<ledger::LedgerState> ledger{ledger::LedgerState::Create(profile)};
    BOOST_REQUIRE(ledger.has_value());
    return std::move(ledger).value();
}
inline ledger::LedgerState EmptyLedger() { return EmptyLedger(RegtestProfile()); }

//! Requires success; reads error() only on the failure path (it Asserts on a value).
template <class T>
void RequireOk(const Result<T>& result, std::string_view what)
{
    if (!result.has_value()) {
        BOOST_FAIL(std::string{what} + " failed: " + std::string{ToString(result.error().code)} + " " +
                   std::string{result.error().detail});
    }
}

//! Requires an error with `code`.
template <class T>
void RequireError(const Result<T>& result, ErrorCode code, std::string_view what)
{
    if (result.has_value()) BOOST_FAIL(std::string{what} + " unexpectedly succeeded");
    BOOST_CHECK_MESSAGE(result.error().code == code, std::string{what} + ": got " + std::string{ToString(result.error().code)} +
                                                         " (" + std::string{result.error().detail} + "), want " +
                                                         std::string{ToString(code)});
}

//! Requires the NOT_IMPLEMENTED stub error of `task_id` (and never reads error() on success).
template <class T>
void RequireStub(const Result<T>& result, std::string_view task_id)
{
    if (result.has_value()) BOOST_FAIL("stub " + std::string{task_id} + " returned success");
    BOOST_CHECK(result.error().code == ErrorCode::NOT_IMPLEMENTED);
    BOOST_CHECK_EQUAL(result.error().detail, task_id);
}

} // namespace flowmesh2::test

#endif // B3_FLOWMESH2_TEST_UTIL_LEDGER_FIXTURES_H
