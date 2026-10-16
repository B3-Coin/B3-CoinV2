// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_LEDGER_NONCE_WINDOW_H
#define B3_FLOWMESH2_LEDGER_NONCE_WINDOW_H

#include <flowmesh2/core/codes.h>
#include <flowmesh2/core/nonce_rule.h>
#include <flowmesh2/core/result.h>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

/** The per-account nonce window of the shared ledger (slice R5). PROVISIONAL (D7).
 *
 * A NonceWindow is the stateful holder of one account's `used` set. It adds
 * no rule of its own: every verdict and every insert is the core spec
 * function of core/nonce_rule.h (ClassifyNonce, NonceInsert), so the ledger,
 * admission (R12), the engine (R7) and b3verify (R11) cannot disagree.
 *
 * Invariant (established by Create/FromUsed, kept by Insert): window >= 1 and
 * `used` is strictly ascending with at most `window` entries, so
 * NonceWindowValid(Used(), Size()) always holds.
 *
 * - Classify(n): ACCEPTABLE iff n is not used and (|used| < W or n > min(used));
 *   USED if n is in the set; TOO_LOW otherwise.
 * - Insert(n): consumes an ACCEPTABLE nonce; with more than W entries the
 *   minimum is evicted. Any other nonce is INVALID_INPUT and leaves the window
 *   unchanged.
 *
 * Value type: copyable, no locks, no I/O, deterministic. Cost of Insert is
 * O(W) (a sorted vector of at most W = 100 entries).
 */
namespace flowmesh2::ledger {

class NonceWindow
{
public:
    //! An empty window of `window` nonces; INVALID_INPUT for window = 0.
    static Result<NonceWindow> Create(uint32_t window);
    //! A window holding `used`; INVALID_INPUT unless NonceWindowValid(used, window).
    static Result<NonceWindow> FromUsed(uint32_t window, std::span<const uint64_t> used);

    //! W, the most nonces the window holds.
    uint32_t Size() const { return m_window; }
    //! The used nonces, strictly ascending.
    std::span<const uint64_t> Used() const { return m_used; }

    //! The D7 verdict for `nonce` (core ClassifyNonce).
    NonceVerdict Classify(uint64_t nonce) const;
    //! Classify(nonce) == ACCEPTABLE.
    bool Acceptable(uint64_t nonce) const { return Classify(nonce) == NonceVerdict::ACCEPTABLE; }
    //! Consume `nonce` (core NonceInsert); INVALID_INPUT and no change unless ACCEPTABLE.
    Result<void> Insert(uint64_t nonce);

    friend bool operator==(const NonceWindow&, const NonceWindow&) = default;

private:
    NonceWindow(uint32_t window, std::vector<uint64_t> used);

    uint32_t m_window;
    std::vector<uint64_t> m_used;
};

/** The admission code for a refused verdict: USED gives NONCE_USED, TOO_LOW
 * gives NONCE_TOO_LOW, ACCEPTABLE (and any unregistered value) gives nothing.
 * Admission (R12) answers these before or after BIP340 as section 10.1 says.
 */
std::optional<AdmissionCode> AdmissionCodeFor(NonceVerdict verdict);

} // namespace flowmesh2::ledger

#endif // B3_FLOWMESH2_LEDGER_NONCE_WINDOW_H
