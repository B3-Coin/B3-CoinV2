// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_CORE_CODES_H
#define B3_FLOWMESH2_CORE_CODES_H

#include <cstdint>
#include <span>
#include <string_view>

/** Outcome and admission code registries (rebirth outline, slice R1).
 *
 * Both registries are APPEND-ONLY and owner-protected: never renumber, reuse
 * or remove a value. A new code takes the next free integer of its band. The
 * integers and names are pinned by codes_tests. Zero is never a code, so a
 * zeroed field cannot decode as a valid outcome.
 *
 * Unknown values (for example from a corrupted or newer peer) are neither
 * success nor rejection: IsKnown() is false, IsRejection() is false and
 * ToString() returns "UNKNOWN". Decoders refuse unknown values with
 * ErrorCode::DECODE; callers must never infer success from !IsRejection().
 */
namespace flowmesh2 {

/** Certified execution outcome of one action or fact. It is committed under
 * the slot header's outcomes_root, so it is part of the certified data.
 *
 * Bands: 1-63 executed without rejection; 64-95 deterministic rejections by a
 * rule; 96-127 certified typed refusals of a feature the outline does not
 * implement yet (each names a TODO.md task at its use site).
 */
enum class OutcomeCode : uint16_t {
    DEPOSIT_CREDITED = 1,
    RESTING = 2,
    FILLED = 3,
    PARTIAL_RESTING = 4,
    IOC_DONE = 5,
    CANCELLED = 6,
    TRANSFERRED = 7,
    FEES_DISTRIBUTED = 8,

    REJ_INSUFFICIENT_AVAILABLE = 64,
    REJ_UNKNOWN_MARKET = 65,
    REJ_UNKNOWN_ORDER = 66,
    REJ_NOT_OWN_ORDER = 67,
    REJ_ORDER_CLOSED = 68,
    REJ_WOULD_CROSS = 69,
    REJ_BOOK_FULL = 70,
    REJ_FILL_CAP = 71,
    REJ_PRICE_BAND = 72,
    REJ_OPEN_ORDER_CAP = 73,
    REJ_BAD_PARAMS = 74,
    REJ_UNKNOWN_ASSET = 75,
    REJ_NO_RECIPIENT = 76,

    //! Same-slot opposite GTC residuals that still cross (D4 tie-break not
    //! ruled; task M3-ENG-02). The reservation is released.
    REJ_RESIDUAL_CROSS_UNSUPPORTED = 96,
};

/** Admission reply for one submitted action. NOT certified: it says whether
 * this node queued the action now, never how it executed. Execution is only
 * an OutcomeCode under a certificate.
 *
 * Bands: 1-15 the action is queued or already known to this node (ADMITTED,
 * DUPLICATE; neither is a rejection); 16-63 not admitted now.
 */
enum class AdmissionCode : uint16_t {
    ADMITTED = 1,
    //! The same ActionId and signature are already pending or included; the
    //! reply carries a reference to the original. Not a rejection.
    DUPLICATE = 2,

    MALFORMED = 16,
    OVERSIZE = 17,
    WRONG_DOMAIN = 18,
    UNKNOWN_KIND = 19,
    UNSUPPORTED_KIND = 20,
    NO_ACCOUNT = 21,
    EXPIRED = 22,
    REF_SLOT_AHEAD = 23,
    BAD_EXPIRY = 24,
    NONCE_TOO_LOW = 25,
    NONCE_USED = 26,
    NONCE_PENDING = 27,
    BUDGET = 28,
    BAD_SIG = 29,
    BUSY = 30,
    POOL_FULL = 31,
    HALTED = 32,
};

template <class Code>
struct CodeInfo {
    Code code;
    std::string_view name;
    bool rejection;
};

//! Registries in ascending integer order, one entry per enumerator above.
std::span<const CodeInfo<OutcomeCode>> OutcomeCodeRegistry();
std::span<const CodeInfo<AdmissionCode>> AdmissionCodeRegistry();

bool IsKnown(OutcomeCode code);
bool IsKnown(AdmissionCode code);

//! Total: the registry name, or "UNKNOWN" for an unregistered value.
std::string_view ToString(OutcomeCode code);
std::string_view ToString(AdmissionCode code);

//! True only for a registered rejection. Unknown values return false.
bool IsRejection(OutcomeCode code);
bool IsRejection(AdmissionCode code);

} // namespace flowmesh2

#endif // B3_FLOWMESH2_CORE_CODES_H
