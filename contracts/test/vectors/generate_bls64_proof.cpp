#include <bridge/bootstrap_proof.h>
#include <crypto/bls.h>
#include <modern/finality_types.h>
#include <util/strencodings.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <span>
#include <vector>

namespace {

uint256 Filled(unsigned char value)
{
    uint256 out;
    std::fill(out.begin(), out.end(), value);
    return out;
}

bls::SecretKey Key(unsigned char seed)
{
    std::array<unsigned char, 32> ikm{};
    for (size_t i{0}; i < ikm.size(); ++i) ikm[i] = seed + i;
    return *bls::SecretKey::FromIKM(ikm);
}

std::vector<std::vector<uint256>> Tree(
    const std::vector<bls::PublicKey>& keys)
{
    std::vector<std::vector<uint256>> levels;
    levels.emplace_back(modern::MAX_FINALITY_SET, uint256{});
    for (size_t i{0}; i < keys.size(); ++i) {
        levels[0][i] = modern::ValidatorSetLeaf(
            static_cast<uint32_t>(i), keys[i].Compressed(), 1);
    }
    for (size_t level{0}; level < modern::FINALITY_SET_TREE_DEPTH; ++level) {
        const auto& current{levels.back()};
        std::vector<uint256> next(current.size() / 2);
        for (size_t i{0}; i < current.size(); i += 2) {
            std::array<unsigned char, 64> pair{};
            std::copy(current[i].begin(), current[i].end(), pair.begin());
            std::copy(
                current[i + 1].begin(),
                current[i + 1].end(),
                pair.begin() + 32);
            next[i / 2] = modern::Keccak(pair);
        }
        levels.push_back(std::move(next));
    }
    return levels;
}

std::vector<unsigned char> EncodeAbsentArray(
    const std::vector<bridge::BootstrapAbsentWitness>& witnesses)
{
    using namespace bridge::bootstrap_detail;
    std::vector<std::vector<unsigned char>> elements;
    for (const auto& witness : witnesses) {
        elements.push_back(EncodeAbsent(witness));
    }

    std::vector<unsigned char> out;
    Append(out, Word(witnesses.size()));
    size_t offset{witnesses.size() * 32};
    for (const auto& element : elements) {
        Append(out, Word(offset));
        offset += element.size();
    }
    for (const auto& element : elements) Append(out, element);
    return out;
}

std::vector<unsigned char> EncodeProof(
    std::span<const unsigned char> bitmap,
    std::span<const unsigned char> signature,
    std::span<const unsigned char> aggregate_pubkey,
    const std::vector<bridge::BootstrapAbsentWitness>& witnesses)
{
    using namespace bridge::bootstrap_detail;
    const auto bitmap_tail{DynamicBytes(bitmap)};
    const auto signature_tail{DynamicBytes(signature)};
    const auto aggregate_tail{DynamicBytes(aggregate_pubkey)};
    const auto absent_tail{EncodeAbsentArray(witnesses)};
    constexpr size_t HEAD{4 * 32};
    std::vector<unsigned char> out;
    Append(out, Word(HEAD));
    Append(out, Word(HEAD + bitmap_tail.size()));
    Append(out, Word(HEAD + bitmap_tail.size() + signature_tail.size()));
    Append(
        out,
        Word(
            HEAD + bitmap_tail.size() + signature_tail.size() +
            aggregate_tail.size()));
    Append(out, bitmap_tail);
    Append(out, signature_tail);
    Append(out, aggregate_tail);
    Append(out, absent_tail);
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::fprintf(
            stderr,
            "usage: %s CERTIFICATE_OUTPUT BOOTSTRAP_OUTPUT\n",
            argv[0]);
        return 2;
    }

    constexpr uint32_t VALIDATORS{64};
    constexpr uint32_t SIGNERS{43};

    std::vector<bls::SecretKey> secrets;
    std::vector<bls::PublicKey> public_keys;
    std::vector<bls::VerifiedPublicKey> verified;
    for (uint32_t i{0}; i < VALIDATORS; ++i) {
        secrets.push_back(Key(static_cast<unsigned char>(i + 1)));
        public_keys.push_back(secrets.back().GetPublicKey());
        verified.push_back(*bls::VerifiedPublicKey::FromPoP(
            public_keys.back(), secrets.back().SignPoP()));
    }
    const auto set_aggregate_key{*bls::AggregatePublicKeys(verified)};
    const auto set_levels{Tree(public_keys)};

    modern::ValidatorSetHeader set0;
    set0.epoch = 0;
    set0.ruleset_version = modern::FINALITY_RULESET_V1;
    set0.validator_count = VALIDATORS;
    set0.total_weight = VALIDATORS;
    set0.quorum_weight = modern::QuorumWeightV1(VALIDATORS);
    set0.aggregate_pubkey = set_aggregate_key.Compressed();
    set0.members_root = set_levels.back().front();

    modern::ValidatorSetHeader set1{set0};
    set1.epoch = 1;

    modern::FinalizedBlock finalized;
    finalized.height = 811001;
    finalized.block_hash = Filled(0x01);
    finalized.withdrawal_root = Filled(0x02);
    finalized.validator_set_hash = modern::ValidatorSetHash(set1);
    finalized.epoch = 0;
    const uint256 domain{Filled(0xd0)};
    const uint256 digest{modern::FinalityDigest(domain, finalized)};

    std::vector<bls::Signature> certificate_signatures;
    for (uint32_t i{0}; i < SIGNERS; ++i) {
        certificate_signatures.push_back(secrets[i].Sign(
            std::span<const unsigned char>{digest.begin(), 32}));
    }
    const auto certificate_signature{
        *bls::AggregateSignatures(certificate_signatures)};

    std::vector<unsigned char> certificate_bitmap((VALIDATORS + 7) / 8);
    for (uint32_t i{0}; i < SIGNERS; ++i) {
        certificate_bitmap[i >> 3] |= 1U << (i & 7);
    }

    std::vector<bridge::BootstrapAbsentWitness> certificate_absent;
    for (uint32_t index{SIGNERS}; index < VALIDATORS; ++index) {
        bridge::BootstrapAbsentWitness witness;
        witness.index = index;
        witness.compressed_pubkey = public_keys[index].Compressed();
        witness.weight = 1;
        witness.uncompressed_pubkey = public_keys[index].Eip2537Uncompressed();
        size_t cursor{index};
        for (
            size_t level{0}; level < modern::FINALITY_SET_TREE_DEPTH;
            ++level) {
            witness.siblings[level] = set_levels[level][cursor ^ 1];
            cursor >>= 1;
        }
        certificate_absent.push_back(witness);
    }

    const auto certificate_proof{EncodeProof(
        certificate_bitmap,
        certificate_signature.Eip2537Uncompressed(),
        set_aggregate_key.Eip2537Uncompressed(),
        certificate_absent)};

    std::vector<bls::PublicKey> bootstrap_public_keys{
        public_keys.begin(), public_keys.begin() + 4};
    std::vector<bls::VerifiedPublicKey> bootstrap_verified{
        verified.begin(), verified.begin() + 4};
    const auto bootstrap_aggregate_key{
        *bls::AggregatePublicKeys(bootstrap_verified)};
    const auto bootstrap_levels{Tree(bootstrap_public_keys)};

    modern::ValidatorSetHeader bootstrap;
    bootstrap.epoch = 0;
    bootstrap.ruleset_version = modern::FINALITY_RULESET_V1;
    bootstrap.validator_count = 4;
    bootstrap.total_weight = 4;
    bootstrap.quorum_weight = 3;
    bootstrap.aggregate_pubkey = bootstrap_aggregate_key.Compressed();
    bootstrap.members_root = bootstrap_levels.back().front();

    modern::FinalizedBlock snapshot;
    snapshot.height = 811000;
    snapshot.block_hash = Filled(0x11);
    snapshot.withdrawal_root = uint256{};
    snapshot.validator_set_hash = modern::ValidatorSetHash(set0);
    snapshot.epoch = 0;
    const uint256 snapshot_digest{modern::FinalityDigest(domain, snapshot)};

    std::vector<bls::Signature> bootstrap_signatures;
    for (uint32_t i{0}; i < 3; ++i) {
        bootstrap_signatures.push_back(secrets[i].Sign(
            std::span<const unsigned char>{
                snapshot_digest.begin(), snapshot_digest.size()}));
    }
    const auto bootstrap_signature{
        *bls::AggregateSignatures(bootstrap_signatures)};

    bridge::BootstrapAbsentWitness bootstrap_absent;
    bootstrap_absent.index = 3;
    bootstrap_absent.compressed_pubkey = public_keys[3].Compressed();
    bootstrap_absent.weight = 1;
    bootstrap_absent.uncompressed_pubkey =
        public_keys[3].Eip2537Uncompressed();
    size_t bootstrap_cursor{3};
    for (
        size_t level{0}; level < modern::FINALITY_SET_TREE_DEPTH; ++level) {
        bootstrap_absent.siblings[level] =
            bootstrap_levels[level][bootstrap_cursor ^ 1];
        bootstrap_cursor >>= 1;
    }
    const std::array<unsigned char, 1> bootstrap_bitmap{0x07};
    const auto bootstrap_proof{EncodeProof(
        bootstrap_bitmap,
        bootstrap_signature.Eip2537Uncompressed(),
        bootstrap_aggregate_key.Eip2537Uncompressed(),
        {bootstrap_absent})};

    std::ofstream certificate_output(argv[1], std::ios::binary);
    certificate_output.write(
        reinterpret_cast<const char*>(certificate_proof.data()),
        certificate_proof.size());
    if (!certificate_output) return 1;

    std::ofstream bootstrap_output(argv[2], std::ios::binary);
    bootstrap_output.write(
        reinterpret_cast<const char*>(bootstrap_proof.data()),
        bootstrap_proof.size());
    if (!bootstrap_output) return 1;

    std::printf(
        "set0_hash=%s\n",
        HexStr(modern::ValidatorSetHash(set0)).c_str());
    std::printf(
        "set1_hash=%s\n",
        HexStr(modern::ValidatorSetHash(set1)).c_str());
    std::printf(
        "set_aggregate_pubkey=%s\n",
        HexStr(set0.aggregate_pubkey).c_str());
    std::printf(
        "set_members_root=%s\n", HexStr(set0.members_root).c_str());
    std::printf(
        "bootstrap_set_hash=%s\n",
        HexStr(modern::ValidatorSetHash(bootstrap)).c_str());
    std::printf(
        "bootstrap_aggregate_pubkey=%s\n",
        HexStr(bootstrap.aggregate_pubkey).c_str());
    std::printf(
        "bootstrap_members_root=%s\n",
        HexStr(bootstrap.members_root).c_str());
    std::printf("certificate_proof_size=%zu\n", certificate_proof.size());
    std::printf("bootstrap_proof_size=%zu\n", bootstrap_proof.size());
    return 0;
}
