// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_NETIO_API_JSON_H
#define B3_FLOWMESH2_NETIO_API_JSON_H

#include <flowmesh2/admission/admission.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/net/client_api.h>
#include <flowmesh2/read/read_service.h>
#include <univalue.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/** The JSON renderings of the client API's replies (M5-API-01, O-104).
 * Hashes and ids are hex; encoded structures (envelopes, leaves, roots,
 * proofs, outcomes, certificates) are hex of their canonical encodings, so a
 * client verifies them with b3verify instead of trusting the JSON; status
 * and code names are strings. Every object carries only what the reply
 * holds: a field absent from the reply is absent from the JSON.
 */
namespace flowmesh2::netio {

std::string ToHex(std::span<const unsigned char> bytes);
std::optional<std::vector<unsigned char>> FromHex(std::string_view hex);

UniValue ErrorJson(std::string_view code, std::string_view reason);
UniValue ErrorJson(const Error& error);
UniValue ReplyHeadJson(const read::ReplyHead& head);
UniValue AdmissionReplyJson(const admission::AdmissionReply& reply);
UniValue ActionStatusJson(const net::ActionStatusReply& reply);
UniValue AccountReplyJson(const read::AccountReply& reply);
UniValue HealthJson(const net::HealthReply& reply);
UniValue StreamEventJson(const net::StreamEvent& event);
//! `epoch:seq:head` for the SSE id and the from= query.
std::string CursorText(const net::StreamCursor& cursor);
std::optional<net::StreamCursor> CursorFromText(std::string_view text);

} // namespace flowmesh2::netio

#endif // B3_FLOWMESH2_NETIO_API_JSON_H
