// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_NETIO_BIP324_SESSION_H
#define B3_FLOWMESH2_NETIO_BIP324_SESSION_H

#include <crypto/chacha20.h>
#include <crypto/chacha20poly1305.h>
#include <key.h>
#include <pubkey.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

/** The encrypted channel of a seat-mesh connection (M4-NET-01, stage 2;
 * O-102): BIP324's key exchange and packet layout, keyed for FlowMesh.
 *
 * Both ends send a fresh 64-byte ElligatorSwift public key; the shared secret
 * is `CKey::ComputeBIP324ECDHSecret` (it hashes both encodings and the x
 * coordinate, so the session id binds both ephemeral keys), expanded with
 * HKDF-SHA256 under the salt `"fm2_v1_shared_secret" || magic` into
 * initiator_L, initiator_P, responder_L, responder_P and session_id. Lengths
 * are encrypted with FSChaCha20, payloads with FSChaCha20Poly1305, both
 * rekeyed every REKEY_INTERVAL packets (BIP324's values). Differences from
 * BIP324 on purpose: no garbage and no decoys (a firewalled seat port needs
 * no traffic shaping), the header byte is always 0x00 and any other value
 * closes the connection, the AEAD associated data is empty. This mirrors
 * `BIP324Cipher` (src/bip324.cpp) rather than linking it: that class lives in
 * bitcoin_node and keys off the B3 P2P magic, which the mesh must never share.
 *
 * Packet: `enc_len(3) || AEAD(header(1) || contents)`, EXPANSION = 20 bytes.
 */
namespace flowmesh2::netio {

class SecureChannel
{
public:
    static constexpr size_t LENGTH_LEN{3};
    static constexpr size_t HEADER_LEN{1};
    static constexpr size_t TAG_LEN{16};
    static constexpr size_t EXPANSION{LENGTH_LEN + HEADER_LEN + TAG_LEN};
    static constexpr uint32_t REKEY_INTERVAL{224};
    static constexpr size_t SESSION_ID_LEN{32};
    static constexpr uint32_t MAX_CONTENTS{(1u << 24) - 1};

    //! A fresh ephemeral key and its ElligatorSwift encoding.
    SecureChannel();
    const EllSwiftPubKey& OurPubKey() const { return m_our_pubkey; }

    //! Derive every key from the peer's encoding; `initiator` is the dialer. The ephemeral secret is wiped.
    void Initialize(const EllSwiftPubKey& their_pubkey, bool initiator, const std::array<uint8_t, 4>& magic);
    bool Ready() const { return m_send_l.has_value(); }
    const std::array<unsigned char, SESSION_ID_LEN>& SessionId() const { return m_session_id; }

    //! One packet for `contents` (at most MAX_CONTENTS bytes).
    std::vector<unsigned char> Encrypt(std::span<const unsigned char> contents);
    //! The contents length of the packet whose first LENGTH_LEN bytes are `prefix`.
    uint32_t DecryptLength(std::span<const unsigned char> prefix);
    //! `body` = the packet after its length prefix (HEADER_LEN + contents + TAG_LEN bytes); false on a bad tag or a non-zero header.
    bool Decrypt(std::span<const unsigned char> body, std::vector<unsigned char>& contents);

private:
    CKey m_key;
    EllSwiftPubKey m_our_pubkey;
    std::optional<FSChaCha20> m_send_l, m_recv_l;
    std::optional<FSChaCha20Poly1305> m_send_p, m_recv_p;
    std::array<unsigned char, SESSION_ID_LEN> m_session_id{};
};

} // namespace flowmesh2::netio

#endif // B3_FLOWMESH2_NETIO_BIP324_SESSION_H
