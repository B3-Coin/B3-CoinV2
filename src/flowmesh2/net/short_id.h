// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_NET_SHORT_ID_H
#define B3_FLOWMESH2_NET_SHORT_ID_H

#include <flowmesh2/agreement/messages.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/result.h>
#include <uint256.h>

#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

/** Salted short IDs of actions for compact relay (slice R13 interface;
 * codec landed with M4-NET-02; BIP152 pattern; research-network-api
 * section 5; PLAN-10).
 *
 * Encode(id) is the 6-byte SipHash-2-4 of the full 32-byte ActionId under
 * keys derived from (header_hash, nonce) as BIP152 derives them from the
 * block header and a nonce: k0 and k1 are the first two little-endian
 * 64-bit words of SHA256(header_hash || LE64(nonce)); the 64-bit hash is
 * truncated to its low 48 bits, little-endian (Keys, SipHashOf). Decode(short,
 * known) returns every ActionId in `known` with that short ID, in `known`
 * order: none means "fetch the body on the BULK lane", two or more is a
 * collision and the full IDs are fetched. Neither ever refuses. Short IDs
 * only shrink relay encoding: what is signed and certified always carries
 * the full 32-byte ActionIds (they "are expected to occasionally collide"),
 * and the inclusion-list IDs of agreement/messages.h use the same codec
 * (M4-AGR-04).
 *
 * The fetch itself (the request on the BULK lane and the re-request of full
 * IDs on a collision) rides the seat mesh and lands with the TCP mesh
 * (M4-NET-01); Decode's empty and multiple answers are its trigger.
 * Deterministic: no clock, I/O, lock or randomness.
 */
namespace flowmesh2::net {

// Self-pinned golden vectors (short_id_tests), reproduced by an independent
// SipHash-2-4 on 2026-11-14: codec (header ONE, nonce 7) of ActionId ZERO and
// ONE, and codec (ZERO, 0) of ZERO. PROVISIONAL relay encoding.
inline constexpr std::string_view GOLDEN_SHORT_ID_ZERO{"33e15b84a015"};
inline constexpr std::string_view GOLDEN_SHORT_ID_ONE{"921ac5e62546"};
inline constexpr std::string_view GOLDEN_SHORT_ID_ALL_ZERO{"6192efdc71c9"};

class ShortIdCodec
{
public:
    //! A codec keyed by one proposal (see the file comment).
    ShortIdCodec(const uint256& header_hash, uint64_t nonce);

    //! The 6-byte short ID of `id` under this codec's keys.
    Result<agreement::ShortId> Encode(const ActionId& id) const;
    //! Every id in `known` whose short ID is `short_id`, in `known` order (see the file comment).
    Result<std::vector<ActionId>> Decode(const agreement::ShortId& short_id, std::span<const ActionId> known) const;

    //! (k0, k1): the first two little-endian 64-bit words of SHA256(header_hash || LE64(nonce)).
    static std::pair<uint64_t, uint64_t> Keys(const uint256& header_hash, uint64_t nonce);
    //! The full 64-bit SipHash-2-4 of `id` under Keys(header_hash, nonce).
    static uint64_t SipHashOf(const uint256& header_hash, uint64_t nonce, const ActionId& id);

    const uint256& HeaderHash() const { return m_header_hash; }
    uint64_t Nonce() const { return m_nonce; }

private:
    uint256 m_header_hash;
    uint64_t m_nonce;
    uint64_t m_k0;
    uint64_t m_k1;
};

} // namespace flowmesh2::net

#endif // B3_FLOWMESH2_NET_SHORT_ID_H
