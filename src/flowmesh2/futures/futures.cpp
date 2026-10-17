// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/futures/futures.h>

namespace flowmesh2::futures {

Result<void> FuturesDomain::PlaceFuture(const FutureOrder& /*order*/)
{
    return NotImplemented("M9-FUT-01"); // TODO(rebirth:M9-FUT-01)
}

Result<uint64_t> FuturesDomain::SubmitOracle(std::span<const OracleSubmission> /*submissions*/)
{
    return NotImplemented("M9-FUT-02"); // TODO(rebirth:M9-FUT-02)
}

Result<MarginState> FuturesDomain::MarginCheck(const AccountId& /*account*/) const
{
    return NotImplemented("M9-FUT-03"); // TODO(rebirth:M9-FUT-03)
}

Result<SweepReport> FuturesDomain::Sweep(uint64_t /*slot*/)
{
    return NotImplemented("M9-FUT-04"); // TODO(rebirth:M9-FUT-04)
}

} // namespace flowmesh2::futures
