// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_GOVERNANCE_GOVERNANCE_H
#define B3_FLOWMESH2_GOVERNANCE_GOVERNANCE_H

#include <flowmesh2/core/facts.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/market.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>

#include <cstdint>

/** Listings, removals and versioned parameters (slice R13: interface and
 * stubs; AST-6, AST-8, GOV-2; [CC] boundary 2).
 *
 * In the outline the markets are fixed at genesis and one profile holds
 * for every height. Target:
 * - ApplyListing (M7-GOV-01, OWNER-GATED): a ListingFact (core/facts.h,
 *   paid with the listing fee, 100,000 B3 in the plan) adds a market at a
 *   versioned height; the result is the market it lists (per-market tick
 *   and lot, M3-ENG-11), and the number of listed assets stays within
 *   MAX_LEAF_ASSETS (O-34).
 * - ApplyRemoval (M7-GOV-02): a removal makes a market cancel-only, then
 *   claim-only; nothing is seized.
 * - ParamsAt (M7-GOV-03): the profile that holds at a height; a change
 *   takes effect only at a versioned height and is bound by profile_hash
 *   in every header, and it must keep ArrivalRank increasing (O-39).
 */
namespace flowmesh2::governance {

//! Target shape: a removal schedule (M7-GOV-02).
struct RemovalRequest {
    MarketId market;
    uint64_t cancel_only_height{0};
    uint64_t claim_only_height{0};
};

class Governance
{
public:
    explicit Governance(const ProfileParams& genesis_profile) : m_genesis_profile{genesis_profile} {}

    //! Stub M7-GOV-01.
    Result<MarketDescriptor> ApplyListing(const ListingFact& fact);
    //! Stub M7-GOV-02.
    Result<void> ApplyRemoval(const RemovalRequest& removal);
    //! Stub M7-GOV-03.
    Result<ProfileParams> ParamsAt(uint64_t height) const;

    const ProfileParams& GenesisProfile() const { return m_genesis_profile; }

private:
    ProfileParams m_genesis_profile;
};

} // namespace flowmesh2::governance

#endif // B3_FLOWMESH2_GOVERNANCE_GOVERNANCE_H
