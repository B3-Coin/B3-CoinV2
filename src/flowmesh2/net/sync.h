// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_NET_SYNC_H
#define B3_FLOWMESH2_NET_SYNC_H

#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

/** The BULK-lane sync messages (stage 2, landed 2026-11-14 with the catch-up
 * of a seat that lacks a decided body, O-99(1) resolved; ARCHITECTURE.md
 * O-101). BULK frames are opaque to the mesh (O-79); a seat reads them only
 * as sync messages. Envelope: `u8 SYNC_VERSION (1) | u8 SyncKind | body`,
 * integers little-endian, hashes 32 raw bytes, byte strings as `u32 len |
 * bytes` with the length checked against its cap BEFORE allocation; the
 * decoder is exact (no trailing bytes).
 *
 * - DECISIONS_REQUEST{from_slot, max}: "send me the decided slots from
 *   from_slot on, at most max (1..MAX_SYNC_DECISIONS) of them, one per frame".
 * - DECISIONS_REPLY{slot, record}: one encoded storage::DecisionRecord
 *   (header, certificate, body) of `slot`, or an empty record meaning "I do
 *   not hold that slot" (the requester resets that peer's claim). One record
 *   per frame, so the longest reply fits MaxBulkFrameBytes.
 * - PROPOSAL_REQUEST{slot, header_hash}: a new leader whose NEW_VIEW selected
 *   a header it does not hold asks for its proposal (PbftAgreement::
 *   ProposeSelected re-proposes it).
 * - PROPOSAL_REPLY{slot, header_hash, proposal}: the encoded agreement
 *   PROPOSAL message of that header, or empty when not held.
 *
 * MaxBulkFrameBytes(profile) is the longest sync frame; the BULK lane and the
 * mesh frame cap must admit it (net::ProvisionalLaneLimits, sim/cluster).
 */
namespace flowmesh2::net {

inline constexpr uint8_t SYNC_VERSION{1};
inline constexpr uint8_t MAX_SYNC_DECISIONS{8};

//! APPEND-ONLY registry; zero is never a value.
enum class SyncKind : uint8_t {
    DECISIONS_REQUEST = 1,
    DECISIONS_REPLY = 2,
    PROPOSAL_REQUEST = 3,
    PROPOSAL_REPLY = 4,
};
bool IsKnown(SyncKind kind);
std::string_view ToString(SyncKind kind);

inline constexpr std::string_view SYNC_VERSION_UNKNOWN{"sync message of another version"};   //!< (DECODE)
inline constexpr std::string_view SYNC_KIND_UNKNOWN{"sync message of an unregistered kind"};  //!< (DECODE)
inline constexpr std::string_view SYNC_TRUNCATED{"sync message shorter than its fields"};     //!< (DECODE)
inline constexpr std::string_view SYNC_TRAILING{"sync message with trailing bytes"};          //!< (DECODE)
inline constexpr std::string_view SYNC_OVERSIZE{"sync message above its cap"};                //!< (OVERSIZE)
inline constexpr std::string_view SYNC_MAX_RANGE{"decisions request max outside 1..8"};       //!< (INVALID_INPUT)

struct DecisionsRequest {
    uint64_t from_slot{0};
    uint8_t max{1};
    friend bool operator==(const DecisionsRequest&, const DecisionsRequest&) = default;
};
struct DecisionsReply {
    uint64_t slot{0};
    std::vector<unsigned char> record; //!< a storage::DecisionRecord encoding, or empty: not held
    friend bool operator==(const DecisionsReply&, const DecisionsReply&) = default;
};
struct ProposalRequest {
    uint64_t slot{0};
    uint256 header_hash;
    friend bool operator==(const ProposalRequest&, const ProposalRequest&) = default;
};
struct ProposalReply {
    uint64_t slot{0};
    uint256 header_hash;
    std::vector<unsigned char> proposal; //!< an agreement PROPOSAL message, or empty: not held
    friend bool operator==(const ProposalReply&, const ProposalReply&) = default;
};
using SyncMessage = std::variant<DecisionsRequest, DecisionsReply, ProposalRequest, ProposalReply>;
SyncKind KindOf(const SyncMessage& message);

//! The longest DecisionsReply payload: a maximal DecisionRecord (header, certificate, body).
size_t MaxSyncRecordBytes(const ProfileParams& profile);
//! The longest sync frame of any kind.
size_t MaxBulkFrameBytes(const ProfileParams& profile);

//! INVALID_INPUT for a request max outside 1..MAX_SYNC_DECISIONS, OVERSIZE for a record or proposal above its cap.
Result<std::vector<unsigned char>> EncodeSync(const SyncMessage& message, const ProfileParams& profile);
//! Exact: DECODE for a version, kind, truncation or trailing bytes; OVERSIZE above the caps.
Result<SyncMessage> DecodeSync(std::span<const unsigned char> bytes, const ProfileParams& profile);

} // namespace flowmesh2::net

#endif // B3_FLOWMESH2_NET_SYNC_H
