// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/ledger/nonce_window.h>

#include <util/check.h>

#include <utility>

namespace flowmesh2::ledger {

NonceWindow::NonceWindow(uint32_t window, std::vector<uint64_t> used) : m_window{window}, m_used{std::move(used)} {}

Result<NonceWindow> NonceWindow::Create(uint32_t window)
{
    if (window == 0) return Error{ErrorCode::INVALID_INPUT, "nonce window size must be at least 1"};
    return Ok(NonceWindow{window, {}});
}

Result<NonceWindow> NonceWindow::FromUsed(uint32_t window, std::span<const uint64_t> used)
{
    if (!NonceWindowValid(used, window)) return Error{ErrorCode::INVALID_INPUT, "malformed nonce window"};
    return Ok(NonceWindow{window, std::vector<uint64_t>(used.begin(), used.end())});
}

NonceVerdict NonceWindow::Classify(uint64_t nonce) const
{
    const Result<NonceVerdict> verdict{ClassifyNonce(m_used, nonce, m_window)};
    // The class invariant keeps the window valid, so the spec function cannot refuse it.
    Assert(verdict.has_value());
    return *verdict;
}

Result<void> NonceWindow::Insert(uint64_t nonce)
{
    Result<std::vector<uint64_t>> next{NonceInsert(m_used, nonce, m_window)};
    if (!next.has_value()) return next.error();
    m_used = std::move(next).value();
    return Ok();
}

std::optional<AdmissionCode> AdmissionCodeFor(NonceVerdict verdict)
{
    switch (verdict) {
    case NonceVerdict::USED: return AdmissionCode::NONCE_USED;
    case NonceVerdict::TOO_LOW: return AdmissionCode::NONCE_TOO_LOW;
    case NonceVerdict::ACCEPTABLE: return std::nullopt;
    }
    return std::nullopt;
}

} // namespace flowmesh2::ledger
