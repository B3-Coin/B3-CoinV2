// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/net/short_id.h>

#include <crypto/common.h>
#include <crypto/sha256.h>
#include <crypto/siphash.h>

#include <array>

namespace flowmesh2::net {

std::pair<uint64_t, uint64_t> ShortIdCodec::Keys(const uint256& header_hash, uint64_t nonce)
{
    std::array<unsigned char, 8> nonce_bytes{};
    WriteLE64(nonce_bytes.data(), nonce);
    std::array<unsigned char, CSHA256::OUTPUT_SIZE> digest{};
    CSHA256().Write(header_hash.begin(), header_hash.size()).Write(nonce_bytes.data(), nonce_bytes.size()).Finalize(digest.data());
    return {ReadLE64(digest.data()), ReadLE64(digest.data() + 8)};
}

uint64_t ShortIdCodec::SipHashOf(const uint256& header_hash, uint64_t nonce, const ActionId& id)
{
    const auto [k0, k1]{Keys(header_hash, nonce)};
    return CSipHasher(k0, k1).Write(std::span<const unsigned char>{id.id.begin(), id.id.size()}).Finalize();
}

ShortIdCodec::ShortIdCodec(const uint256& header_hash, uint64_t nonce) : m_header_hash{header_hash}, m_nonce{nonce}
{
    const auto [k0, k1]{Keys(header_hash, nonce)};
    m_k0 = k0;
    m_k1 = k1;
}

Result<agreement::ShortId> ShortIdCodec::Encode(const ActionId& id) const
{
    const uint64_t full{CSipHasher(m_k0, m_k1).Write(std::span<const unsigned char>{id.id.begin(), id.id.size()}).Finalize()};
    agreement::ShortId out{};
    for (size_t i{0}; i < out.size(); ++i) out[i] = static_cast<unsigned char>(full >> (8 * i));
    return Ok(out);
}

Result<std::vector<ActionId>> ShortIdCodec::Decode(const agreement::ShortId& short_id, std::span<const ActionId> known) const
{
    std::vector<ActionId> matches;
    for (const ActionId& id : known) {
        const Result<agreement::ShortId> encoded{Encode(id)};
        if (!encoded.has_value()) return encoded.error();
        if (*encoded == short_id) matches.push_back(id);
    }
    return Ok(std::move(matches));
}

} // namespace flowmesh2::net
