// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/core/nonce_rule.h>

#include <algorithm>
#include <utility>

namespace flowmesh2 {

std::string_view ToString(NonceVerdict verdict)
{
    switch (verdict) {
    case NonceVerdict::ACCEPTABLE: return "ACCEPTABLE";
    case NonceVerdict::USED: return "USED";
    case NonceVerdict::TOO_LOW: return "TOO_LOW";
    }
    return "UNKNOWN";
}

bool NonceWindowValid(std::span<const uint64_t> used, uint32_t window)
{
    if (window == 0 || used.size() > window) return false;
    for (size_t i{1}; i < used.size(); ++i) {
        if (used[i - 1] >= used[i]) return false;
    }
    return true;
}

Result<NonceVerdict> ClassifyNonce(std::span<const uint64_t> used, uint64_t nonce, uint32_t window)
{
    if (!NonceWindowValid(used, window)) return Error{ErrorCode::INVALID_INPUT, "malformed nonce window"};
    if (std::binary_search(used.begin(), used.end(), nonce)) return Ok(NonceVerdict::USED);
    if (used.size() < window || nonce > used.front()) return Ok(NonceVerdict::ACCEPTABLE);
    return Ok(NonceVerdict::TOO_LOW);
}

bool NonceAcceptable(std::span<const uint64_t> used, uint64_t nonce, uint32_t window)
{
    const auto verdict{ClassifyNonce(used, nonce, window)};
    return verdict.has_value() && *verdict == NonceVerdict::ACCEPTABLE;
}

Result<std::vector<uint64_t>> NonceInsert(std::span<const uint64_t> used, uint64_t nonce, uint32_t window)
{
    const auto verdict{ClassifyNonce(used, nonce, window)};
    if (!verdict.has_value()) return verdict.error();
    if (*verdict != NonceVerdict::ACCEPTABLE) return Error{ErrorCode::INVALID_INPUT, "nonce is not acceptable"};
    std::vector<uint64_t> next(used.begin(), used.end());
    next.insert(std::upper_bound(next.begin(), next.end(), nonce), nonce);
    if (next.size() > window) next.erase(next.begin());
    return Ok(std::move(next));
}

bool InExpiryWindow(uint64_t ref_slot, uint16_t max_delay, uint64_t slot)
{
    return slot > ref_slot && slot - ref_slot <= max_delay;
}

bool ExpiredAt(uint64_t ref_slot, uint16_t max_delay, uint64_t slot)
{
    return slot > ref_slot && slot - ref_slot > max_delay;
}

} // namespace flowmesh2
