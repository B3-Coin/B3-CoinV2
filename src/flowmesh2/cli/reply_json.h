// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_CLI_REPLY_JSON_H
#define B3_FLOWMESH2_CLI_REPLY_JSON_H

#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/read/read_service.h>
#include <flowmesh2/read/views.h>
#include <uint256.h>
#include <univalue.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/** The client's side of netio/api_json.h (M5-CLI-01, ARCHITECTURE.md O-106):
 * the replies of the HTTPS API back into the structs b3verify takes. Everything certified is
 * rebuilt from the hex of its canonical encoding (`envelope`, `leaf.encoded`,
 * `roots`, `proof.encoded`, `outcome.encoded`), never from the JSON's
 * convenience fields, so what the client verifies is what the seat signed.
 * A missing field or one of another shape is DECODE (JSON_SHAPE); a hex
 * field of the wrong length is DECODE (JSON_HEX). Hashes and ids are in
 * `uint256::GetHex` order, byte strings in byte order (api_json.h).
 */
namespace flowmesh2::cli {

inline constexpr std::string_view JSON_TEXT{"the reply is not JSON"};                                 //!< (DECODE)
inline constexpr std::string_view JSON_SHAPE{"the reply lacks a field or has one of another shape"}; //!< (DECODE)
inline constexpr std::string_view JSON_HEX{"a reply field is not hex of the expected length"};       //!< (DECODE)
inline constexpr std::string_view JSON_LABEL{"the reply head carries an unknown label"};             //!< (DECODE)

//! GET /v2/state/{account}: the head, and the leaf witness when FOUND.
struct StateReply {
    read::ReplyHead head;
    bool found{false};
    std::string reason;
    std::optional<read::AccountWitness> witness;
};

//! POST /v2/actions/status and GET /v2/actions/{id}.
struct StatusReply {
    std::string status; //!< INCLUDED, PENDING, EXPIRED or UNKNOWN (net::ToString(ApiActionStatus))
    std::string reason;
    std::optional<read::ReplyHead> head;
    std::optional<read::IncludedOutcome> included;
    std::optional<read::AccountWitness> expiry;
    std::string pending_status; //!< PENDING: the pool's status name
    uint64_t pending_height{0};
};

//! POST /v2/actions.
struct AdmissionReplyJson {
    std::string code; //!< ToString(AdmissionCode)
    std::string reason;
    std::optional<ActionId> id;
    std::optional<uint64_t> head_height;
    std::optional<read::IncludedOutcome> outcome; //!< DUPLICATE of an included action, when the seat still holds the slot
};

//! GET /v2/health.
struct HealthReplyJson {
    bool fenced{false};
    std::optional<uint64_t> published_height;
    uint64_t pool_live{0};
};

Result<UniValue> ParseJson(std::string_view text);
Result<uint256> ParseHash(const UniValue& value);
Result<std::vector<unsigned char>> ParseBytes(const UniValue& value);
Result<uint64_t> ParseU64(const UniValue& value);

Result<read::ReplyHead> ParseReplyHead(const UniValue& value);
Result<read::AccountWitness> ParseWitness(const UniValue& value, const ProfileParams& profile);
Result<read::IncludedOutcome> ParseIncluded(const UniValue& value, const ProfileParams& profile);
Result<StateReply> ParseStateReply(const UniValue& value, const ProfileParams& profile);
Result<StatusReply> ParseStatusReply(const UniValue& value, const ProfileParams& profile);
Result<AdmissionReplyJson> ParseAdmissionReply(const UniValue& value, const ProfileParams& profile);
Result<HealthReplyJson> ParseHealthReply(const UniValue& value);

} // namespace flowmesh2::cli

#endif // B3_FLOWMESH2_CLI_REPLY_JSON_H
