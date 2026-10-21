// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef B3_FLOWMESH2_AGREEMENT_CONTROL_MESSAGES_H
#define B3_FLOWMESH2_AGREEMENT_CONTROL_MESSAGES_H

#include <flowmesh2/agreement/messages.h>
#include <flowmesh2/storage/control_log.h>

namespace flowmesh2::agreement {

//! Exact canonical transcript: own signature zeroed, nested report signatures
//! intact. ControlLog derives the phase/instance separated signing digest.
Result<storage::ControlIntentRecord> ControlIntent(const ControlReport& report, const ProfileParams& profile);
Result<storage::ControlIntentRecord> ControlIntent(const ControlNewView& view, const ProfileParams& profile);

struct ControlSelection {
    std::optional<ControlPrepared> highest;
    std::optional<CertifiedHeader> decision;
};

//! Full cryptography and exact current decision-instance binding, independent
//! of local tip observations or peer availability. Body/anchor availability is
//! checked separately before any vote or committed application.
Result<void> VerifyControlReport(const ControlReport& report, const SlotHeaderV2& parent,
                                 const SeatSet& seats, const ProfileParams& profile);
Result<ControlSelection> VerifyControlNewView(const ControlNewView& view, const SlotHeaderV2& parent,
                                             const SeatSet& seats, const ProfileParams& profile);

//! Complete proof checks for this node's unsigned durable intent. These do
//! not authenticate its sender and never grant remote voting permission.
Result<void> VerifyControlReportEvidence(const ControlReport& report, const SlotHeaderV2& parent,
                                          const SeatSet& seats, const ProfileParams& profile);
Result<ControlSelection> VerifyControlNewViewEvidence(const ControlNewView& view, const SlotHeaderV2& parent,
                                                       const SeatSet& seats, const ProfileParams& profile);

} // namespace flowmesh2::agreement
#endif
