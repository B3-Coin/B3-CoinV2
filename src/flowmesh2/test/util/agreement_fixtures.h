// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_TEST_UTIL_AGREEMENT_FIXTURES_H
#define B3_FLOWMESH2_TEST_UTIL_AGREEMENT_FIXTURES_H

#include <crypto/bls.h>
#include <flowmesh2/core/authority.h>
#include <flowmesh2/core/facts.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/seat_set.h>
#include <flowmesh2/test/util/keys.h>
#include <flowmesh2/test/util/ledger_fixtures.h>
#include <flowmesh2/test/util/slot_fixtures.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/** TEST fixtures of the chain adapter and agreement suites (slice R10).
 * Synthetic values only: the TEST network of test/util/slot_fixtures.h,
 * BLS seats from IKM = SHA256("fm2/test/seat/<label>"), label block hashes.
 */
namespace flowmesh2::test {

inline std::string SeatLabel(std::string_view name) { return "fm2/test/seat/" + std::string{name}; }

//! The PoP-verified seat set of the BLS test keys of `names`, in that order.
inline SeatSet TestSeats(const std::vector<std::string>& names)
{
    std::vector<SeatRegistration> registrations;
    for (const std::string& name : names) {
        const bls::SecretKey key{BlsKeyFromLabel(SeatLabel(name))};
        registrations.push_back(SeatRegistration{key.GetPublicKey(), key.SignPoP()});
    }
    Result<SeatSet> seats{SeatSet::FromPoP(registrations)};
    RequireOk(seats, "SeatSet::FromPoP");
    return std::move(seats).value();
}

//! The authority of `seats` at `epoch` under the regtest profile.
inline AuthorityVersion SeatAuthority(const SeatSet& seats, uint64_t epoch = 0)
{
    return AuthorityVersion{epoch, seats.Root(), ProfileHash(RegtestProfile())};
}

//! The synthetic block hash of `height` on fork `fork`.
inline uint256 BlockHash(uint32_t height, std::string_view fork = "main")
{
    return LabelHash("fm2/test/block/" + std::string{fork} + "/" + std::to_string(height));
}

//! A deposit of `amount` rUSD to alice at `cursor`, from outpoint (label tx, 0).
inline DepositFact DepositAtCursor(const FactCursor& cursor, std::string_view tx, Atoms amount = 1'000)
{
    DepositFact fact{Deposit(TestAccount("alice"), TestAsset("rUSD"), amount, TestOutpoint(tx, 0))};
    fact.cursor = cursor;
    return fact;
}

} // namespace flowmesh2::test

#endif // B3_FLOWMESH2_TEST_UTIL_AGREEMENT_FIXTURES_H
