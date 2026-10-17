// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/net/client_api.h>

#include <algorithm>

namespace flowmesh2::net {

uint256 ToMailboxId(RequestId id)
{
    uint256 out;
    unsigned char* bytes{out.data()};
    for (size_t i{0}; i < 8; ++i) bytes[i] = static_cast<unsigned char>(id.value >> (8 * i));
    return out;
}

std::optional<RequestId> RequestIdFromMailbox(const uint256& mailbox_id)
{
    const unsigned char* bytes{mailbox_id.data()};
    if (std::any_of(bytes + 8, bytes + uint256::size(), [](unsigned char b) { return b != 0; })) return std::nullopt;
    RequestId id;
    for (size_t i{0}; i < 8; ++i) id.value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
    return id;
}

bool IsKnown(RequestKind kind)
{
    switch (kind) {
    case RequestKind::SUBMIT_ACTION:
    case RequestKind::ACTION_STATUS:
    case RequestKind::HEALTH:
        return true;
    }
    return false;
}

std::string_view ToString(RequestKind kind)
{
    switch (kind) {
    case RequestKind::SUBMIT_ACTION: return "SUBMIT_ACTION";
    case RequestKind::ACTION_STATUS: return "ACTION_STATUS";
    case RequestKind::HEALTH: return "HEALTH";
    }
    return "UNKNOWN";
}

bool IsKnown(ApiActionStatus status)
{
    switch (status) {
    case ApiActionStatus::INCLUDED:
    case ApiActionStatus::PENDING:
    case ApiActionStatus::EXPIRED:
    case ApiActionStatus::UNKNOWN:
        return true;
    }
    return false;
}

std::string_view ToString(ApiActionStatus status)
{
    switch (status) {
    case ApiActionStatus::INCLUDED: return "INCLUDED";
    case ApiActionStatus::PENDING: return "PENDING";
    case ApiActionStatus::EXPIRED: return "EXPIRED";
    case ApiActionStatus::UNKNOWN: return "UNKNOWN";
    }
    return "UNKNOWN";
}

} // namespace flowmesh2::net
