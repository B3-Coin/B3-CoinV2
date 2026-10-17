// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_FUTURES_FUTURES_H
#define B3_FLOWMESH2_FUTURES_FUTURES_H

#include <flowmesh2/core/action.h>
#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

/** The futures domain (slice R13: interface and stubs; plan section 3.11;
 * D21 BLOCKED-ON-RULING; PLAN-13; [CC] C8b).
 *
 * Futures are off in the outline: Validate refuses a profile with
 * futures_enabled (M9-FUT-01), engine phase 7 is the stub RunFuturesPhase
 * (M9-FUT-04) and admission refuses both spot/futures transfer kinds
 * (M2-LED-06). Target:
 * - PlaceFuture (M9-FUT-01): dated, isolated, linear contracts settled in
 *   the quote stable, matched like spot (D4) under their own caps.
 * - SubmitOracle (M9-FUT-02): the mark is the median of at least 2f + 1
 *   vote-committed seat submissions for one slot (manipulation-resistant;
 *   a missing quorum keeps the old mark and freezes liquidations).
 * - MarginCheck (M9-FUT-03): initial and maintenance margin of an
 *   account's isolated positions against the mark, in integer atoms.
 * - Sweep (M9-FUT-04): engine phase 7: liquidations in a deterministic
 *   index order under a per-slot cap, then the insurance fund, then a
 *   deficit halt; a storm stays within the slot budget.
 * Every value here is integer and deterministic; time enters only as slot
 * heights.
 */
namespace flowmesh2::futures {

//! Target shape: one futures order (M9-FUT-01).
struct FutureOrder {
    AccountId account;
    MarketId contract;
    Side side{Side::BUY};
    uint64_t price_ticks{0};
    uint64_t lots{0};
    uint64_t expiry_height{0};
};

//! Target shape: one seat's price submission for one slot (M9-FUT-02).
struct OracleSubmission {
    uint32_t seat{0};
    MarketId contract;
    uint64_t slot{0};
    uint64_t price_ticks{0};
    std::array<unsigned char, 96> signature{};
};

//! Target shape: an account's margin against the mark (M9-FUT-03).
struct MarginState {
    AccountId account;
    Atoms collateral{0};
    Atoms initial_margin{0};
    Atoms maintenance_margin{0};
};

//! Target shape: what one sweep did (M9-FUT-04).
struct SweepReport {
    size_t liquidated{0};
    Atoms insurance_used{0};
    bool deficit_halt{false};
};

class FuturesDomain
{
public:
    explicit FuturesDomain(const ProfileParams& profile) : m_profile{profile} {}

    //! Stub M9-FUT-01.
    Result<void> PlaceFuture(const FutureOrder& order);
    //! The mark price of the slot's quorum. Stub M9-FUT-02.
    Result<uint64_t> SubmitOracle(std::span<const OracleSubmission> submissions);
    //! Stub M9-FUT-03.
    Result<MarginState> MarginCheck(const AccountId& account) const;
    //! Stub M9-FUT-04.
    Result<SweepReport> Sweep(uint64_t slot);

    const ProfileParams& Profile() const { return m_profile; }

private:
    ProfileParams m_profile;
};

} // namespace flowmesh2::futures

#endif // B3_FLOWMESH2_FUTURES_FUTURES_H
