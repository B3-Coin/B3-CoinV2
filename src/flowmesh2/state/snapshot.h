// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_STATE_SNAPSHOT_H
#define B3_FLOWMESH2_STATE_SNAPSHOT_H

#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/slot.h>
#include <flowmesh2/state/candidate.h>
#include <uint256.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

/** State snapshot bytes (M4-STO-03; plan section 3.7).
 *
 * A snapshot holds exactly the leaves that commitment v0 commits to
 * (ledger/state_commitment.h), so a snapshot that reproduces a certified
 * header's state root is that header's state, and nothing outside the
 * certified chain needs to be trusted. The bytes are, in Bitcoin
 * serialization:
 *
 *   magic "B3FM2SNP" (8) | u32 SNAPSHOT_FORMAT_VERSION | profile hash (32)
 *   | seat set root (32)
 *   | CompactSize #markets, each MarketDescriptor, ascending MarketId
 *   | CompactSize #accounts, each a byte vector AccountLeafV0::Encode(),
 *     ascending AccountId
 *   | CompactSize #nullifiers, each Outpoint, ascending
 *   | CompactSize #totals, each (AssetId, AssetTotals), ascending AssetId
 *   | CompactSize #withdrawals, each WithdrawalRecord::Encode(), ascending
 *   | CompactSize #orders, each RestingOrderLeafV0::Encode(), in
 *     EngineState::OrderLeaves() order (ascending MarketId, then book order).
 *
 * Every leaf goes through its own exact decoder, so a snapshot holds only
 * canonical leaves. Every count is checked against the bytes that remain
 * before anything is allocated (OVERSIZE). The format does not depend on
 * how the order book is built (M3-ENG-01): orders are rested again in book
 * order.
 *
 * EncodeSnapshot(state) gives the bytes; INVARIANT if the state holds a
 * leaf that is not canonical. DecodeSnapshot(bytes, profile) rebuilds the
 * state: DECODE or OVERSIZE for malformed bytes, VERIFY_FAILED
 * (SNAPSHOT_PROFILE) for another profile, INVALID_INPUT for leaves out of
 * order or a ledger the leaves cannot form, and the engine's invariants
 * (O-54) on the result. SnapshotHead(bytes, profile, header, cert_hash) is
 * the head a restart installs: DecodeSnapshot, then the state's
 * commitment, seat set and profile equal to the header's (VERIFY_FAILED,
 * SNAPSHOT_HEADER).
 *
 * Deterministic, no lock, no I/O: SnapshotStore (storage/) keeps the bytes.
 */
namespace flowmesh2::state {

inline constexpr std::array<unsigned char, 8> SNAPSHOT_MAGIC{'B', '3', 'F', 'M', '2', 'S', 'N', 'P'};
inline constexpr uint32_t SNAPSHOT_FORMAT_VERSION{1};
//! The largest snapshot DecodeSnapshot reads. PROVISIONAL.
inline constexpr size_t MAX_SNAPSHOT_BYTES{size_t{1} << 31};

// Static details (ErrorCode in parentheses).
inline constexpr std::string_view SNAPSHOT_FORMAT{"not a state snapshot of this format"};          //!< (DECODE)
inline constexpr std::string_view SNAPSHOT_PROFILE{"snapshot of another profile"};                 //!< (VERIFY_FAILED)
inline constexpr std::string_view SNAPSHOT_ORDER{"snapshot leaves out of order"};                 //!< (INVALID_INPUT)
inline constexpr std::string_view SNAPSHOT_HEADER{"snapshot state is not the header's state"};     //!< (VERIFY_FAILED)

//! The snapshot bytes of `state`. See the file comment.
Result<std::vector<unsigned char>> EncodeSnapshot(const CoreState& state);

//! The state `bytes` hold, under `profile`. See the file comment.
Result<std::shared_ptr<const CoreState>> DecodeSnapshot(std::span<const unsigned char> bytes, const ProfileParams& profile);

//! The head of `header` (decided with certificate hash `cert_hash`) whose state `bytes` hold. See the file comment.
Result<HeadView> SnapshotHead(std::span<const unsigned char> bytes, const ProfileParams& profile, const SlotHeaderV2& header,
                              const uint256& cert_hash);

} // namespace flowmesh2::state

#endif // B3_FLOWMESH2_STATE_SNAPSHOT_H
