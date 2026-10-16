// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/core/result.h>

#include <array>

namespace flowmesh2 {
namespace {

constexpr std::array<ErrorCodeInfo, 17> ERROR_CODES{{
    {ErrorCode::NOT_IMPLEMENTED, "NOT_IMPLEMENTED"},
    {ErrorCode::UNSUPPORTED, "UNSUPPORTED"},
    {ErrorCode::UNSUPPORTED_NETWORK, "UNSUPPORTED_NETWORK"},
    {ErrorCode::DECODE, "DECODE"},
    {ErrorCode::OVERSIZE, "OVERSIZE"},
    {ErrorCode::ARITHMETIC_OVERFLOW, "ARITHMETIC_OVERFLOW"},
    {ErrorCode::INVARIANT, "INVARIANT"},
    {ErrorCode::ORDER_VIOLATION, "ORDER_VIOLATION"},
    {ErrorCode::EQUIVOCATION_REFUSED, "EQUIVOCATION_REFUSED"},
    {ErrorCode::STORAGE_FATAL, "STORAGE_FATAL"},
    {ErrorCode::FENCED, "FENCED"},
    {ErrorCode::AUTHORITY_CHANGED, "AUTHORITY_CHANGED"},
    {ErrorCode::VERIFY_FAILED, "VERIFY_FAILED"},
    {ErrorCode::STALE_PARENT, "STALE_PARENT"},
    {ErrorCode::BUSY, "BUSY"},
    {ErrorCode::NEED_DATA, "NEED_DATA"},
    {ErrorCode::INVALID_INPUT, "INVALID_INPUT"},
}};

// Append-only shape: contiguous from 1 in declaration order, unique names.
constexpr bool RegistryIsAppendOnlyShape()
{
    for (size_t i{0}; i < ERROR_CODES.size(); ++i) {
        if (static_cast<size_t>(ERROR_CODES[i].code) != i + 1) return false;
        if (ERROR_CODES[i].name.empty() || ERROR_CODES[i].name == "UNKNOWN") return false;
        for (size_t j{0}; j < i; ++j) {
            if (ERROR_CODES[j].name == ERROR_CODES[i].name) return false;
        }
    }
    return true;
}
static_assert(RegistryIsAppendOnlyShape(), "ErrorCode registry must be contiguous from 1 with unique names");

} // namespace

std::span<const ErrorCodeInfo> ErrorCodeRegistry() { return ERROR_CODES; }

bool IsKnown(ErrorCode code)
{
    for (const auto& info : ERROR_CODES) {
        if (info.code == code) return true;
    }
    return false;
}

std::string_view ToString(ErrorCode code)
{
    for (const auto& info : ERROR_CODES) {
        if (info.code == code) return info.name;
    }
    return "UNKNOWN";
}

} // namespace flowmesh2
