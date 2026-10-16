// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/core/seat_set.h>

#include <flowmesh2/core/hashes.h>
#include <serialize.h>

#include <array>
#include <optional>
#include <set>
#include <utility>

namespace flowmesh2 {

SeatSet::SeatSet(std::vector<bls::VerifiedPublicKey> keys, const uint256& root) : m_keys{std::move(keys)}, m_root{root}
{
}

Result<SeatSet> SeatSet::FromPoP(std::span<const SeatRegistration> registrations)
{
    if (registrations.empty()) return Error{ErrorCode::INVALID_INPUT, "a seat set needs at least one seat"};
    if (registrations.size() > MAX_SEATS) return Error{ErrorCode::OVERSIZE, "more seats than MAX_SEATS"};

    std::set<std::array<unsigned char, bls::PUBKEY_SIZE>> seen;
    for (const auto& registration : registrations) {
        if (!seen.insert(registration.key.Compressed()).second) {
            return Error{ErrorCode::INVALID_INPUT, "a seat key appears twice"};
        }
    }

    std::vector<bls::VerifiedPublicKey> keys;
    keys.reserve(registrations.size());
    HashWriter writer{TaggedWriter(HashTag::SEATSET)};
    WriteCompactSize(writer, registrations.size());
    for (const auto& registration : registrations) {
        std::optional<bls::VerifiedPublicKey> verified{bls::VerifiedPublicKey::FromPoP(registration.key, registration.pop)};
        if (!verified.has_value()) return Error{ErrorCode::VERIFY_FAILED, "a seat's proof of possession does not verify"};
        writer.write(std::as_bytes(std::span{registration.key.Compressed()}));
        keys.push_back(*verified);
    }
    return Ok(SeatSet{std::move(keys), writer.GetSHA256()});
}

} // namespace flowmesh2
