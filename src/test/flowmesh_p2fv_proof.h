// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_TEST_FLOWMESH_P2FV_PROOF_H
#define B3COIN_TEST_FLOWMESH_P2FV_PROOF_H

#include <crypto/bls.h>
#include <uint256.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// TEST ONLY: the proof rules of pf_model.py's fixed N=4, one-instance,
// views 0..2 profile. Not production consensus or a signing-authority guard.
// Authentication proves key possession, NOT durable signing/publication,
// body availability, execution validity, application or anti-rollback state.
namespace p2fv {

using Bytes = std::vector<unsigned char>;
inline constexpr size_t N{4};
inline constexpr size_t Q{3};
inline constexpr size_t FAST_QUORUM{3};
inline constexpr int32_t MAX_VIEW{2};
inline constexpr size_t MAX_TREE_NODES{64};
inline constexpr size_t MAX_TREE_DEPTH{8};
inline constexpr size_t MAX_CHILDREN{4};
inline constexpr size_t MAX_PROOF_BYTES{32 * 1024};

enum class Kind : uint8_t {
    EMPTY = 0, PREPARE, COMMIT, PC, FAST, SLOW, V0, EQ, REPORT, NEW_VIEW, PROPOSE,
};

struct Packet {
    Kind kind{Kind::EMPTY};
    int32_t view{-1};
    int32_t sender{-1};
    std::optional<uint256> value;
    std::vector<Packet> items;
    uint256 instance;
    std::array<unsigned char, bls::SIGNATURE_SIZE> signature{};
    friend bool operator==(const Packet&, const Packet&) = default;
};

struct Member {
    std::array<unsigned char, bls::PUBKEY_SIZE> public_key{};
    std::array<unsigned char, bls::SIGNATURE_SIZE> proof_of_possession{};
};

class Invalid : public std::runtime_error {
public:
    explicit Invalid(const std::string& reason) : std::runtime_error(reason) {}
};

class Context {
public:
    // Exactly four distinct keys, with real PoP verification. Input order IS
    // seat-index order; the constructor never silently sorts membership.
    Context(const uint256& instance, std::span<const Member> members);
    const uint256& Instance() const { return m_instance; }
    const uint256& MembershipHash() const { return m_membership_hash; }
    const bls::VerifiedPublicKey& Key(size_t seat) const;
private:
    uint256 m_instance;
    uint256 m_membership_hash;
    std::vector<bls::VerifiedPublicKey> m_keys;
};

bool IsSigned(Kind kind);
const char* KindName(Kind kind);
Packet Make(const Context& context, Kind kind, int32_t view = -1,
            int32_t sender = -1, std::optional<uint256> value = std::nullopt,
            std::vector<Packet> items = {});
Packet Empty(const Context& context);

// Fixed-version exact tree encoding. Bounds apply before child allocation or
// recursion. Encoding preserves item order (as Python immutable tuples do),
// does not sort/deduplicate evidence, and requires complete consumption.
// Codec validation alone does NOT establish semantic validity or signatures.
Bytes Encode(const Packet& packet);
Packet Decode(std::span<const unsigned char> bytes);

// Digest commits membership, all own metadata and the exact recursively
// encoded proof, INCLUDING nested signatures. Only this packet's own signature
// is replaced with zeros. Distinct TEST-only domain; not a production vote.
uint256 SigningDigest(const Context& context, const Packet& packet);
// Computes only. The caller must durably authorize/store before publishing.
// Does not authorize a conflicting or old-view signature, or validate a report
// as truthful local state. Semantic verification is deliberately separate.
Packet Sign(const Context& context, const bls::SecretKey& key, Packet packet);

void VerifyVote(const Context& context, const Packet& packet, Kind expected);
void VerifyCertificate(const Context& context, const Packet& packet);
void VerifyReport(const Context& context, const Packet& packet);

struct Choice {
    // Arbitrary uint256 values replace the model's finite {x,y} universe.
    // unrestricted means any externally execution-valid value, NOT a claim
    // that its body exists or has executed. Zero is a valid value hash.
    bool unrestricted{false};
    std::set<uint256> values;
    bool Allows(const uint256& value) const { return unrestricted || values.contains(value); }
};

Choice Choices(const Context& context, int32_t view,
               std::span<const Packet> reports, const Packet& equivocation);
void VerifyNewView(const Context& context, const Packet& packet);
void VerifyProposal(const Context& context, const Packet& packet);
// Dispatches proof/vote verification, including standalone EMPTY/V0/EQ.
void Verify(const Context& context, const Packet& packet);

// Optional bounded runtime-free cryptographic regression entry point. Throws
// Invalid on failure; returns the number of assertions exercised. No I/O,
// threads, storage, node state or durable-publication simulation.
size_t SelfCheck(const Context& context, std::span<const bls::SecretKey> keys);

} // namespace p2fv

#endif // B3COIN_TEST_FLOWMESH_P2FV_PROOF_H
