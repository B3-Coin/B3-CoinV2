// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/netio/bip324_session.h>

#include <crypto/hkdf_sha256_32.h>
#include <random.h>
#include <span.h>
#include <support/cleanse.h>

#include <cassert>
#include <string>

namespace flowmesh2::netio {

SecureChannel::SecureChannel()
{
    m_key.MakeNewKey(true);
    std::array<std::byte, 32> entropy{};
    GetRandBytes(std::span<unsigned char>{reinterpret_cast<unsigned char*>(entropy.data()), entropy.size()});
    m_our_pubkey = m_key.EllSwiftCreate(entropy);
    memory_cleanse(entropy.data(), entropy.size());
}

void SecureChannel::Initialize(const EllSwiftPubKey& their_pubkey, bool initiator, const std::array<uint8_t, 4>& magic)
{
    const std::string salt{std::string{"fm2_v1_shared_secret"} + std::string(magic.begin(), magic.end())};
    ECDHSecret ecdh_secret{m_key.ComputeBIP324ECDHSecret(their_pubkey, m_our_pubkey, initiator)};
    CHKDF_HMAC_SHA256_L32 hkdf(UCharCast(ecdh_secret.data()), ecdh_secret.size(), salt);
    std::array<std::byte, 32> okm{};
    hkdf.Expand32("initiator_L", UCharCast(okm.data()));
    (initiator ? m_send_l : m_recv_l).emplace(okm, REKEY_INTERVAL);
    hkdf.Expand32("initiator_P", UCharCast(okm.data()));
    (initiator ? m_send_p : m_recv_p).emplace(okm, REKEY_INTERVAL);
    hkdf.Expand32("responder_L", UCharCast(okm.data()));
    (initiator ? m_recv_l : m_send_l).emplace(okm, REKEY_INTERVAL);
    hkdf.Expand32("responder_P", UCharCast(okm.data()));
    (initiator ? m_recv_p : m_send_p).emplace(okm, REKEY_INTERVAL);
    hkdf.Expand32("session_id", m_session_id.data());
    memory_cleanse(ecdh_secret.data(), ecdh_secret.size());
    memory_cleanse(okm.data(), okm.size());
    memory_cleanse(&hkdf, sizeof(hkdf));
    m_key = CKey();
}

std::vector<unsigned char> SecureChannel::Encrypt(std::span<const unsigned char> contents)
{
    assert(Ready());
    assert(contents.size() <= MAX_CONTENTS);
    std::vector<unsigned char> out(contents.size() + EXPANSION);
    std::span<std::byte> output{reinterpret_cast<std::byte*>(out.data()), out.size()};
    std::byte len[LENGTH_LEN];
    len[0] = std::byte{static_cast<uint8_t>(contents.size() & 0xff)};
    len[1] = std::byte{static_cast<uint8_t>((contents.size() >> 8) & 0xff)};
    len[2] = std::byte{static_cast<uint8_t>((contents.size() >> 16) & 0xff)};
    m_send_l->Crypt(len, output.first(LENGTH_LEN));
    const std::byte header[HEADER_LEN]{std::byte{0}};
    const std::span<const std::byte> plain{reinterpret_cast<const std::byte*>(contents.data()), contents.size()};
    const std::span<const std::byte> aad;
    m_send_p->Encrypt(header, plain, aad, output.subspan(LENGTH_LEN));
    return out;
}

uint32_t SecureChannel::DecryptLength(std::span<const unsigned char> prefix)
{
    assert(Ready());
    assert(prefix.size() == LENGTH_LEN);
    std::byte buf[LENGTH_LEN];
    m_recv_l->Crypt(std::span<const std::byte>{reinterpret_cast<const std::byte*>(prefix.data()), LENGTH_LEN}, buf);
    return static_cast<uint32_t>(buf[0]) + (static_cast<uint32_t>(buf[1]) << 8) + (static_cast<uint32_t>(buf[2]) << 16);
}

bool SecureChannel::Decrypt(std::span<const unsigned char> body, std::vector<unsigned char>& contents)
{
    assert(Ready());
    if (body.size() < HEADER_LEN + TAG_LEN) return false;
    contents.assign(body.size() - HEADER_LEN - TAG_LEN, 0);
    std::byte header[HEADER_LEN];
    const std::span<const std::byte> aad;
    const std::span<const std::byte> input{reinterpret_cast<const std::byte*>(body.data()), body.size()};
    const std::span<std::byte> plain{reinterpret_cast<std::byte*>(contents.data()), contents.size()};
    if (!m_recv_p->Decrypt(input, aad, header, plain)) return false;
    return header[0] == std::byte{0};
}

} // namespace flowmesh2::netio
