// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_CORE_SEAT_SET_H
#define B3_FLOWMESH2_CORE_SEAT_SET_H

#include <crypto/bls.h>
#include <flowmesh2/core/result.h>
#include <uint256.h>

#include <cstddef>
#include <span>
#include <vector>

/** The FlowMesh seat set: the BLS keys that certify slots (slice R4).
 *
 * Seat i is the i-th registration, in the order the chain facts gave them
 * (seat-change facts are M7-SEAT-01; the outline builds seat sets directly).
 * Index order is the bitmap order of a SlotCertificate (core/certificate.h).
 *
 * - FromPoP is the only way to build a SeatSet. Every key enters through
 *   bls::VerifiedPublicKey::FromPoP, so only PoP-verified keys can ever be
 *   aggregated (crypto/bls.h; never TrustedFromValidatedChain here).
 *   Refusals, cheapest first: no registration (INVALID_INPUT), more than
 *   MAX_SEATS (OVERSIZE), a key that appears twice (INVALID_INPUT), a proof
 *   of possession that does not verify (VERIFY_FAILED).
 * - Quorum() = QuorumFor(Size()) = floor(2k/3) + 1, never lowered. For k = 1
 *   (the REGTEST dev certifier, O-10) the quorum is 1.
 * - Root() = Tagged(SEATSET, CompactSize(k) || the 48 compressed bytes of
 *   each key in index order). The slot header's seat_set_root.
 *
 * MAX_SEATS is PROVISIONAL (O-27): it bounds the certificate bitmap to 128
 * bytes. FN-10 asks for k >= 4 on real networks; the outline only builds the
 * k = 1 regtest set and test sets.
 */
namespace flowmesh2 {

//! Largest seat set. PROVISIONAL (O-27).
inline constexpr size_t MAX_SEATS{1'024};

//! floor(2k/3) + 1 for k >= 1; 0 for k == 0.
constexpr size_t QuorumFor(size_t seat_count) { return seat_count == 0 ? 0 : (2 * seat_count) / 3 + 1; }

struct SeatRegistration {
    bls::PublicKey key;
    bls::Signature pop;
};

class SeatSet
{
public:
    static Result<SeatSet> FromPoP(std::span<const SeatRegistration> registrations);

    size_t Size() const { return m_keys.size(); }
    size_t Quorum() const { return QuorumFor(m_keys.size()); }
    const uint256& Root() const { return m_root; }
    std::span<const bls::VerifiedPublicKey> Keys() const { return m_keys; }

private:
    SeatSet(std::vector<bls::VerifiedPublicKey> keys, const uint256& root);

    std::vector<bls::VerifiedPublicKey> m_keys;
    uint256 m_root;
};

} // namespace flowmesh2

#endif // B3_FLOWMESH2_CORE_SEAT_SET_H
