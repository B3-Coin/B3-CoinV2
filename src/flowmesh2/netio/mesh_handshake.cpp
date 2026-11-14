// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/netio/mesh_handshake.h>

#include <crypto/bls.h>
#include <flowmesh2/core/hashes.h>
#include <pubkey.h>
#include <random.h>

#include <cstring>

namespace flowmesh2::netio {

namespace detail {

void PutU32(std::vector<unsigned char>& out, uint32_t v)
{
    for (int i{0}; i < 4; ++i) out.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xff));
}
void PutU64(std::vector<unsigned char>& out, uint64_t v)
{
    for (int i{0}; i < 8; ++i) out.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xff));
}
uint32_t GetU32(const unsigned char* p)
{
    uint32_t v{0};
    for (int i{0}; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
    return v;
}
uint64_t GetU64(const unsigned char* p)
{
    uint64_t v{0};
    for (int i{0}; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

//! The HELLO fields the signature covers, in wire order, without the signature.
void PutHelloFields(std::vector<unsigned char>& out, const MeshHello& hello)
{
    out.push_back(hello.version);
    PutU32(out, hello.from_seat);
    PutU32(out, hello.to_seat);
    out.push_back(static_cast<unsigned char>(hello.lane));
    PutU64(out, hello.epoch);
    out.insert(out.end(), hello.seat_set_root.begin(), hello.seat_set_root.end());
    out.insert(out.end(), hello.transport_key.begin(), hello.transport_key.end());
}

} // namespace detail

using detail::GetU32;
using detail::GetU64;
using detail::PutHelloFields;
using detail::PutU32;
using detail::PutU64;

std::vector<unsigned char> MeshHello::Encode() const
{
    std::vector<unsigned char> out;
    out.reserve(MESH_HELLO_BYTES);
    PutHelloFields(out, *this);
    out.insert(out.end(), signature.begin(), signature.end());
    return out;
}

Result<MeshHello> MeshHello::Decode(std::span<const unsigned char> bytes)
{
    if (bytes.size() != MESH_HELLO_BYTES) return Error{ErrorCode::DECODE, HELLO_SHAPE};
    MeshHello hello;
    const unsigned char* p{bytes.data()};
    hello.version = *p++;
    if (hello.version != MESH_HELLO_VERSION) return Error{ErrorCode::DECODE, HELLO_VERSION_UNKNOWN};
    hello.from_seat = GetU32(p);
    p += 4;
    hello.to_seat = GetU32(p);
    p += 4;
    hello.lane = static_cast<net::Lane>(*p++);
    if (!net::IsKnown(hello.lane)) return Error{ErrorCode::DECODE, HELLO_LANE_UNKNOWN};
    hello.epoch = GetU64(p);
    p += 8;
    std::memcpy(hello.seat_set_root.data(), p, 32);
    p += 32;
    std::memcpy(hello.transport_key.data(), p, 32);
    p += 32;
    std::memcpy(hello.signature.data(), p, 64);
    return Ok(hello);
}

uint256 SessionDigest(const std::array<unsigned char, 32>& session_id, const MeshHello& hello, const std::array<uint8_t, 4>& magic)
{
    std::vector<unsigned char> bytes;
    bytes.insert(bytes.end(), session_id.begin(), session_id.end());
    PutHelloFields(bytes, hello);
    bytes.insert(bytes.end(), magic.begin(), magic.end());
    return Tagged(HashTag::MESH_SESSION, bytes);
}

bool SignHello(const CKey& transport_key, const std::array<unsigned char, 32>& session_id, const std::array<uint8_t, 4>& magic, MeshHello& hello)
{
    if (!transport_key.IsValid()) return false;
    const uint256 digest{SessionDigest(session_id, hello, magic)};
    uint256 aux;
    GetRandBytes(std::span<unsigned char>{aux.data(), aux.size()});
    return transport_key.SignSchnorr(digest, hello.signature, /*merkle_root=*/nullptr, aux);
}

Result<void> VerifyHello(const MeshHello& hello, const std::array<unsigned char, 32>& session_id, const std::array<uint8_t, 4>& magic)
{
    const XOnlyPubKey key{std::span<const unsigned char>{hello.transport_key.data(), hello.transport_key.size()}};
    if (!key.IsFullyValid()) return Error{ErrorCode::DECODE, HELLO_KEY};
    if (!key.VerifySchnorr(SessionDigest(session_id, hello, magic), hello.signature)) return Error{ErrorCode::VERIFY_FAILED, HELLO_SIGNATURE};
    return Ok();
}

uint256 EndpointDigest(const net::SeatEndpointRecord& record, const uint256& domain)
{
    std::vector<unsigned char> bytes{domain.begin(), domain.end()};
    const std::vector<unsigned char> fields{record.SigningBytes()};
    bytes.insert(bytes.end(), fields.begin(), fields.end());
    return Tagged(HashTag::MESH_ENDPOINT, bytes);
}

Result<void> VerifyEndpointRecord(const net::SeatEndpointRecord& record, const SeatSet& seats, const uint256& domain)
{
    if (record.seat >= seats.Size()) return Error{ErrorCode::INVALID_INPUT, net::TCP_MESH_ENDPOINT_SEAT};
    if (record.addresses.empty() || record.addresses.size() > net::MESH_ENDPOINT_MAX_ADDRESSES || record.port == 0) {
        return Error{ErrorCode::INVALID_INPUT, net::TCP_MESH_ENDPOINT_SHAPE};
    }
    for (const std::string& address : record.addresses) {
        if (address.empty() || address.size() > 64) return Error{ErrorCode::INVALID_INPUT, net::TCP_MESH_ENDPOINT_SHAPE};
    }
    const std::optional<bls::Signature> signature{bls::Signature::Decode(record.signature)};
    if (!signature.has_value()) return Error{ErrorCode::VERIFY_FAILED, net::TCP_MESH_ENDPOINT_SIGNATURE};
    const uint256 digest{EndpointDigest(record, domain)};
    if (!bls::Verify(seats.Keys()[record.seat].Key(), std::span<const unsigned char>{digest.data(), digest.size()}, *signature)) {
        return Error{ErrorCode::VERIFY_FAILED, net::TCP_MESH_ENDPOINT_SIGNATURE};
    }
    return Ok();
}

} // namespace flowmesh2::netio

namespace flowmesh2::net {

std::vector<unsigned char> SeatEndpointRecord::SigningBytes() const
{
    std::vector<unsigned char> out;
    netio::detail::PutU32(out, seat);
    out.push_back(static_cast<unsigned char>(port & 0xff));
    out.push_back(static_cast<unsigned char>((port >> 8) & 0xff));
    netio::detail::PutU64(out, valid_from_epoch);
    netio::detail::PutU64(out, valid_until_epoch);
    netio::detail::PutU64(out, counter);
    out.insert(out.end(), transport_key.begin(), transport_key.end());
    out.push_back(static_cast<unsigned char>(addresses.size()));
    for (const std::string& address : addresses) {
        out.push_back(static_cast<unsigned char>(address.size()));
        out.insert(out.end(), address.begin(), address.end());
    }
    return out;
}

} // namespace flowmesh2::net
