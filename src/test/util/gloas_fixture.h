// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license.
#ifndef B3_TEST_UTIL_GLOAS_FIXTURE_H
#define B3_TEST_UTIL_GLOAS_FIXTURE_H

#include <bridge/gloas_peer_light_client.h>
#include <crypto/bls.h>

#include <map>
#include <stdexcept>

/** SYNTHETIC unit-test trust roots only. This helper exercises the actual
 * private capability mint through encoded bootstrap records, SSZ branches and
 * a checked RLP hash preimage. It never constructs a capability or supplies a
 * production checkpoint, peer, node or source fact. */
namespace gloas_fixture {
using Bytes = std::vector<unsigned char>;
namespace gl = bridge::peer::gloas;

inline uint256 Tag(unsigned char byte) { return uint256{Bytes(32, byte)}; }
inline Bytes Raw(const uint256& value) { return {value.begin(), value.end()}; }
inline uint256 Keccak(std::span<const unsigned char> bytes)
{
    uint256 out;
    Keccak256().Write(bytes).Finalize(out);
    return out;
}
inline uint256 Fold(uint256 value, const std::vector<uint256>& branch, uint64_t index)
{
    for (const auto& sibling : branch) {
        value = index & 1 ? bridge::ssz::HashPair(sibling, value) : bridge::ssz::HashPair(value, sibling);
        index >>= 1;
    }
    if (index != 1) throw std::runtime_error("invalid synthetic branch depth");
    return value;
}

inline bls::SecretKey Key()
{
    std::array<unsigned char, 32> scalar{};
    scalar.back() = 1;
    auto key{bls::SecretKey::FromBytes(scalar)};
    if (!key) throw std::runtime_error("test scalar unavailable");
    return *key;
}
inline bridge::ssz::SyncCommittee Committee()
{
    const auto key{Key()};
    const auto pub{key.GetPublicKey()};
    const auto verified{bls::VerifiedPublicKey::FromPoP(pub, key.SignPoP())};
    if (!verified) throw std::runtime_error("test key binding unavailable");
    std::vector<bls::VerifiedPublicKey> keys(512, *verified);
    const auto aggregate{bls::AggregatePublicKeys(keys)};
    if (!aggregate) throw std::runtime_error("test aggregate unavailable");
    bridge::ssz::SyncCommittee out;
    out.pubkeys.assign(512, pub.Compressed());
    out.aggregate_pubkey = aggregate->Compressed();
    return out;
}
inline std::array<unsigned char, 96> Sign(const uint256& signing_root, size_t participants = 512)
{
    if (!participants || participants > 512) throw std::runtime_error("test participant bound");
    const auto key{Key()};
    const auto signature{key.Sign({signing_root.begin(), 32})};
    const std::vector<bls::Signature> signatures(participants, signature);
    const auto aggregate{bls::AggregateSignatures(signatures)};
    if (!aggregate) throw std::runtime_error("test signature unavailable");
    return aggregate->Compressed();
}

// A sparse synthetic generalized-index tree, used to put finalized-header and
// next-committee leaves into one attested state root in actual BLS tests.
struct Tree {
    std::map<uint64_t, uint256> leaves;
    uint256 Node(uint64_t index) const
    {
        if (const auto found{leaves.find(index)}; found != leaves.end()) return found->second;
        bool descendant{false};
        for (const auto& [leaf_index, unused] : leaves) {
            auto position{leaf_index};
            while (position > index) position >>= 1;
            if (position == index) { descendant = true; break; }
        }
        if (!descendant) return {};
        if (index > (uint64_t{1} << 62)) throw std::runtime_error("test tree depth");
        return bridge::ssz::HashPair(Node(index * 2), Node(index * 2 + 1));
    }
    std::vector<uint256> Branch(uint64_t index) const
    {
        std::vector<uint256> out;
        while (index > 1) { out.push_back(Node(index ^ 1)); index >>= 1; }
        return out;
    }
};

inline Bytes ExecutionHeader(const uint256& state_root, uint64_t slot, uint64_t number = 1'000'000)
{
    using namespace bridge;
    const auto empty{Keccak(RlpEncodeBytes({}))};
    return RlpEncodeList({
        RlpEncodeBytes(Raw(Tag(1))), RlpEncodeBytes(Raw(Tag(2))), RlpEncodeBytes(Bytes(20, 0)),
        RlpEncodeBytes(Raw(state_root)), RlpEncodeBytes(Raw(empty)), RlpEncodeBytes(Raw(empty)),
        RlpEncodeBytes(Bytes(256, 0)), RlpEncodeUint64(0), RlpEncodeUint64(number),
        RlpEncodeUint64(30'000'000), RlpEncodeUint64(0), RlpEncodeUint64(gl::GENESIS_TIME + slot * 12),
        RlpEncodeBytes({}), RlpEncodeBytes(Raw(Tag(3))), RlpEncodeBytes(Bytes(8, 0)), RlpEncodeUint64(1),
        RlpEncodeBytes(Raw(Tag(4))), RlpEncodeUint64(0), RlpEncodeUint64(0),
        RlpEncodeBytes(Raw(Tag(5))), RlpEncodeBytes(Raw(Tag(6))), RlpEncodeBytes(Raw(Tag(7))),
        RlpEncodeUint64(slot)});
}

inline gl::Header Header(uint64_t slot, const uint256& execution_hash, const uint256& state_root)
{
    gl::Header out;
    out.beacon.slot = slot;
    out.beacon.proposer_index = 1;
    out.beacon.parent_root = Tag(0x71);
    out.beacon.state_root = state_root;
    out.execution_block_hash = execution_hash;
    out.execution_branch.resize(11);
    out.beacon.body_root = Fold(execution_hash, out.execution_branch, gl::EXECUTION_GINDEX);
    return out;
}

struct Fixture {
    uint64_t slot{gl::FORK_EPOCH * 32 + 100};
    Bytes execution_header;
    gl::Bootstrap bootstrap;
    gl::Policy policy;
    std::vector<Bytes> journal;
    explicit Fixture(const uint256& l1_state_root = Tag(0x33), uint64_t number = 1'000'000)
    {
        execution_header = ExecutionHeader(l1_state_root, slot, number);
        bootstrap.current = Committee();
        bootstrap.branch.resize(11);
        bootstrap.header = Header(slot, Keccak(execution_header),
            Fold(bootstrap.current.HashTreeRoot(), bootstrap.branch, gl::CURRENT_GINDEX));
        policy.trusted_slot = slot;
        policy.trusted_root = bootstrap.header.beacon.HashTreeRoot();
        policy.fork_schedule_valid_through_epoch = gl::REVIEWED_THROUGH_EPOCH;
        policy.max_sync_lag_slots = bridge::SLOTS_PER_PERIOD;
        policy.max_recovery_lag_slots = bridge::SLOTS_PER_PERIOD;
        if (!policy.Valid()) throw std::runtime_error("invalid synthetic policy");
    }
    gl::LightClient::DurableAppend Sink()
    {
        return [this](auto bytes) { journal.emplace_back(bytes.begin(), bytes.end()); return true; };
    }
    Bytes Record() const
    {
        const auto encoded{gl::EncodeRecord(gl::Record{bootstrap})};
        if (!encoded) throw std::runtime_error("synthetic bootstrap encoding");
        return *encoded;
    }
    void Bootstrap(gl::LightClient& owner)
    {
        if (!owner.EndReplay() || owner.Submit(Record(), slot, Sink()).result != bridge::peer::Result::OK)
            throw std::runtime_error("synthetic bootstrap failed actual verification");
    }
};
} // namespace gloas_fixture
#endif
