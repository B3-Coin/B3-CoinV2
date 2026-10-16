// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_CORE_NONCE_RULE_H
#define B3_FLOWMESH2_CORE_NONCE_RULE_H

#include <flowmesh2/core/result.h>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

/** D7 nonce window and expiry: the shared spec functions (slice R4; plan
 * section 3.6, PLAN-1). PROVISIONAL (D7).
 *
 * One definition used by the ledger's NonceWindow (R5), admission (R12),
 * the engine's validity predicate (R7) and b3verify's VerifyExpired (R11).
 *
 * Nonce window. An account's `used` set holds the up to W highest nonces it
 * has consumed, strictly ascending (W = profile nonce_window, 100).
 * - Nonce n is ACCEPTABLE iff n is not in `used` and (|used| < W or
 *   n > min(used)). Otherwise it is USED (n in `used`) or TOO_LOW.
 * - Inserting an acceptable n adds it and, if the set then holds more than W
 *   nonces, removes the minimum.
 * - Lemma (VerifyExpired relies on it): along any sequence of inserts, if n
 *   is ACCEPTABLE for the current set then n was never inserted. Evictions
 *   only remove the minimum, so an evicted nonce is always below min(used),
 *   and with |used| < W nothing was ever evicted. nonce_rule_tests checks
 *   the lemma over every insert sequence of length <= 5 from 0..9 with W = 3.
 *
 * Expiry. An action with ref_slot r and max_delay d may be included in slot
 * s iff r < s <= r + d (InExpiryWindow). It can never be included at or
 * after slot s once s > r + d (ExpiredAt). Both are computed without
 * overflow for every r and s. Whether d itself is allowed (1 <= d <=
 * max_delay_slots) is ActionV2::MaxDelayValid (core/action.h).
 *
 * Every function is total and deterministic. A malformed window (not
 * strictly ascending, more than W entries, or W = 0) is INVALID_INPUT, and
 * NonceAcceptable is false for it.
 */
namespace flowmesh2 {

// TODO(rebirth:M2-ADM-01) Exhaustive nonce/expiry tests plus a Quint model of D7 replace this reference spec.

//! Classification of one nonce against a window. APPEND-ONLY; zero is never a value.
enum class NonceVerdict : uint8_t {
    ACCEPTABLE = 1,
    USED = 2,    //!< admission NONCE_USED
    TOO_LOW = 3, //!< admission NONCE_TOO_LOW
};

std::string_view ToString(NonceVerdict verdict);

//! True iff `used` is strictly ascending, holds at most `window` nonces and window >= 1.
bool NonceWindowValid(std::span<const uint64_t> used, uint32_t window);

//! The D7 verdict for `nonce`; INVALID_INPUT for a malformed window.
Result<NonceVerdict> ClassifyNonce(std::span<const uint64_t> used, uint64_t nonce, uint32_t window);

//! ClassifyNonce(...) == ACCEPTABLE; false for a malformed window.
bool NonceAcceptable(std::span<const uint64_t> used, uint64_t nonce, uint32_t window);

//! The window after consuming `nonce`; INVALID_INPUT unless it is acceptable.
Result<std::vector<uint64_t>> NonceInsert(std::span<const uint64_t> used, uint64_t nonce, uint32_t window);

//! ref_slot < slot <= ref_slot + max_delay, without overflow.
bool InExpiryWindow(uint64_t ref_slot, uint16_t max_delay, uint64_t slot);

//! slot > ref_slot + max_delay, without overflow: the window has passed for good.
bool ExpiredAt(uint64_t ref_slot, uint16_t max_delay, uint64_t slot);

} // namespace flowmesh2

#endif // B3_FLOWMESH2_CORE_NONCE_RULE_H
