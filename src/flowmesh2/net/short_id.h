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
#include <vector>

/** Salted short IDs of actions for compact relay (slice R13: interface and
 * stubs; BIP152 pattern; research-network-api section 5; PLAN-10).
 *
 * Target (M4-NET-02): Encode(id) is the 6-byte SipHash-2-4 of the full
 * ActionId under keys derived from (header_hash, nonce), as BIP152 derives
 * them from the block header and a nonce (crypto/siphash.h). Decode(short,
 * known) returns every ActionId in `known` with that short ID: none means
 * "fetch the body on the BULK lane", two or more is a collision and the
 * full IDs are fetched. Short IDs only shrink relay encoding: what is
 * signed and certified always carries the full 32-byte ActionIds (they
 * "are expected to occasionally collide"), and the inclusion-list IDs of
 * agreement/messages.h use the same codec (M4-AGR-04).
 *
 * Stubs: Encode and Decode return NotImplemented(M4-NET-02).
 */
namespace flowmesh2::net {

class ShortIdCodec
{
public:
    //! A codec keyed by one proposal (see the file comment).
    ShortIdCodec(const uint256& header_hash, uint64_t nonce) : m_header_hash{header_hash}, m_nonce{nonce} {}

    //! Stub M4-NET-02.
    Result<agreement::ShortId> Encode(const ActionId& id) const;
    //! Stub M4-NET-02.
    Result<std::vector<ActionId>> Decode(const agreement::ShortId& short_id, std::span<const ActionId> known) const;

    const uint256& HeaderHash() const { return m_header_hash; }
    uint64_t Nonce() const { return m_nonce; }

private:
    uint256 m_header_hash;
    uint64_t m_nonce;
};

} // namespace flowmesh2::net

#endif // B3_FLOWMESH2_NET_SHORT_ID_H
