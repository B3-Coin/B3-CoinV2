// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef B3_FLOWMESH2_AGREEMENT_REGTEST_PBFT_H
#define B3_FLOWMESH2_AGREEMENT_REGTEST_PBFT_H
#include <flowmesh2/agreement/pbft.h>
namespace flowmesh2::agreement {
//! Called only after PbftAgreement's explicit REGTEST/stable-Q/four-seat gate.
Result<std::unique_ptr<Agreement>> MakeRegtestFullViewAgreement(const ProfileParams& profile, const PbftConfig& config,
                                                               const PbftContext& context, SeatSet seats);
}
#endif
