// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> defines DOMAIN (ARCHITECTURE.md O-21)

#include <boost/test/unit_test.hpp>

#include <crypto/hex_base.h>
#include <crypto/sha256.h>
#include <flowmesh2/core/hashes.h>
#include <uint256.h>
#include <univalue.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace flowmesh2;

namespace {

std::string Hex(const uint256& value) { return HexStr(value); }

//! BIP340 tagged hash written out with CSHA256, independent of hash.cpp.
uint256 ReferenceTagged(std::string_view tag, std::span<const unsigned char> message)
{
    unsigned char tag_hash[CSHA256::OUTPUT_SIZE];
    CSHA256{}.Write(reinterpret_cast<const unsigned char*>(tag.data()), tag.size()).Finalize(tag_hash);
    uint256 out;
    CSHA256{}
        .Write(tag_hash, sizeof(tag_hash))
        .Write(tag_hash, sizeof(tag_hash))
        .Write(message.data(), message.size())
        .Finalize(out.data());
    return out;
}

struct PinnedTag {
    HashTag tag;
    int value;
    const char* text;
    const char* empty_digest; //!< Tagged(tag, ""), computed with Python hashlib
};

// APPEND-ONLY and owner-protected: these pins only ever gain rows.
const PinnedTag PINNED_TAGS[]{
    {HashTag::NET_DOMAIN, 1, "B3/FM2/DOMAIN", "db26f07e728acf0ce73d45af9000886d5c113611969c081cf98d6cb89b05be77"},
    {HashTag::ACCOUNT, 2, "B3/FM2/ACCOUNT", "e53b8a8729ac20e094d61ea527bde33a36a9d525f67d76c851406ef09d8ff02f"},
    {HashTag::ACTION, 3, "B3/FM2/ACTION", "bff59e1c3e5b2e47e95f044824e6231f89153c7a40b64188630198386ce0edc2"},
    {HashTag::ACTION_LEAF, 4, "B3/FM2/ACTION_LEAF", "a041691fae85ce114560285c6dac9464ee88a31576c4be2bc5c4f8e950c3d15a"},
    {HashTag::BODY, 5, "B3/FM2/BODY", "30d2766df2f48d3d51b3ebd295c85e824b87a94d9545e43ba7ee53b7e35391f3"},
    {HashTag::FACT, 6, "B3/FM2/FACT", "b2740cd7bc0cf73610fad6fc018fbc52411842bb56787613ea2d53a09bc659cf"},
    {HashTag::FACTS, 7, "B3/FM2/FACTS", "153d32e70bce34d889a4eb8cfeae0272f9aed2c00a5fd0941ae9649e382b2f4d"},
    {HashTag::MARKET, 8, "B3/FM2/MARKET", "c76b5bd78db620992c9e047b6e969b054d8836235ea8e0352830f024e1c0f7cd"},
    {HashTag::ORDER, 9, "B3/FM2/ORDER", "7c582ce6e4d66c2a0249b89c94de63ba8337e8b2c26e7938ddb73320e4152de7"},
    {HashTag::SLOT, 10, "B3/FM2/SLOT", "ca79d0b35d77a1758ce055bb89ec0aa114a77a3f74c82919aa70d7f7d666e9e2"},
    {HashTag::COMMIT, 11, "B3/FM2/COMMIT", "db631a81238c9837a5ea769a64e306b96ac27dfe141303411306cd2117f82602"},
    {HashTag::PREPARE, 12, "B3/FM2/PREPARE", "d8535d6620feb8a4e8e61accde8971432a9568801f3e7edd705ce2b39ea940f9"},
    {HashTag::CERT, 13, "B3/FM2/CERT", "6ab39689cdad347aed065b81e5d6105f2220c20c4febfd49d1666e8bf00705c7"},
    {HashTag::OUTCOME, 14, "B3/FM2/OUTCOME", "cba80bd195cae3792c33bb7b2a1ea0d9ef79513b36c7d265bd6b75330299d42b"},
    {HashTag::SEATSET, 15, "B3/FM2/SEATSET", "36491d579dabaa9307b008eb73d8ea8e15d3dec7bf9bbf13a0a9d7c38bf32769"},
    {HashTag::PROFILE, 16, "B3/FM2/PROFILE", "c34ec5ec344faf5df466ac8513140e69a3e43c63301be55c551954b8728eb087"},
    {HashTag::STATE, 17, "B3/FM2/STATE", "10a5b590f253444d841d40cd63b7c23103d852176af4f435cbaf03fd98e9d498"},
    {HashTag::STATE_ACCOUNTS, 18, "B3/FM2/STATE_ACCOUNTS", "6dea49f4ab6e59c4998964ae4bde16d229ab5ad6ab8cd8161f6a8ba682e9b0f5"},
    {HashTag::STATE_ORDERS, 19, "B3/FM2/STATE_ORDERS", "5d9e5438c93c75cdd12aaefdb0457dc56d6ed271e847ebf3820ab980516fcc6a"},
    {HashTag::STATE_MARKETS, 20, "B3/FM2/STATE_MARKETS", "4b42e2f07ccc56562c6e1fa66fc72b181abe4bdc4cd99b84d9dc2d4dcfb56bb0"},
    {HashTag::STATE_NULLIFIERS, 21, "B3/FM2/STATE_NULLIFIERS", "cb3452addc8768099f0de47f568c659bc18a24d63a16cdf51bc02d88d07ff91c"},
    {HashTag::STATE_TOTALS, 22, "B3/FM2/STATE_TOTALS", "5ec8870d931620129e37f7203335c55be7a74de84cee1d59822dac65f7be6d4c"},
    {HashTag::MERKLE_LEAF, 23, "B3/FM2/MERKLE_LEAF", "ad1bd62c831e19d4d348fb322c3e5ec89fc8c9446412c56adfca7c21f4d584cd"},
    {HashTag::MERKLE_NODE, 24, "B3/FM2/MERKLE_NODE", "b75497479db7bbee352e606891161124f72712f9e53f033b1113adba64c051fe"},
    {HashTag::MERKLE_EMPTY, 25, "B3/FM2/MERKLE_EMPTY", "c798d3999b048a1a920b5b7c8ffb3d0fb892e04c6c006d18e873a00b5214016d"},
    {HashTag::MERKLE_ROOT, 26, "B3/FM2/MERKLE_ROOT", "752515f01f992c0305e50e8032cc8b1cd6cde2efa3ace85db81c6be3ab5964c6"},
    {HashTag::PROTOCOL_ACCOUNT, 27, "B3/FM2/PROTOCOL_ACCOUNT", "dd170734e9886cefee4be813658ec7f08523ea1fe1df78aec279c0d43f84f3a9"},
    {HashTag::JOURNAL, 28, "B3/FM2/JOURNAL", "61a775b2eac97901da6842c51654a3fce27ca42f621dedb21b0641a766ced621"},
    {HashTag::TEST_ASSET, 29, "B3/FM2/TEST_ASSET", "bcbd107c316a7ee9193d8f593a2a1b8345f42cbfe8cc637032b76c12fef26c05"},
};

bool ScannedSuffix(const std::filesystem::path& path)
{
    static const std::set<std::string> suffixes{".h", ".hpp", ".cpp", ".c", ".py", ".sol"};
    return suffixes.contains(path.extension().string());
}

//! Every "B3/..." string literal in tracked-language files of the repository,
//! outside src/flowmesh2/ and outside hidden and build directories.
std::set<std::string> RepositoryB3Tags(const std::filesystem::path& root)
{
    namespace sfs = std::filesystem;
    // Double- and single-quoted literals (C++, Solidity, Python b"..." too).
    static const std::regex literals[]{std::regex{"\"(B3/[^\"\\\\\\n]*)\""}, std::regex{"'(B3/[^'\\\\\\n]*)'"}};
    std::set<std::string> tags;
    for (const char* top : {"src", "contrib", "test", "contracts"}) {
        const sfs::path base{root / top};
        if (!sfs::is_directory(base)) continue;
        sfs::recursive_directory_iterator it{base, sfs::directory_options::skip_permission_denied};
        for (const sfs::recursive_directory_iterator end; it != end; ++it) {
            const sfs::path& path{it->path()};
            const std::string name{path.filename().string()};
            if (it->is_directory()) {
                if (name.starts_with(".") || name.starts_with("build") || path == root / "src" / "flowmesh2") {
                    it.disable_recursion_pending();
                }
                continue;
            }
            if (!it->is_regular_file() || !ScannedSuffix(path)) continue;
            std::ifstream file{path, std::ios::binary};
            const std::string text{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
            if (text.find("B3/") == std::string::npos) continue;
            for (const auto& literal : literals) {
                for (std::sregex_iterator m{text.begin(), text.end(), literal}, m_end; m != m_end; ++m) {
                    tags.insert((*m)[1].str());
                }
            }
        }
    }
    return tags;
}

} // namespace

BOOST_AUTO_TEST_SUITE(hashes_tests)

BOOST_AUTO_TEST_CASE(registry_pinned)
{
    const auto registry{HashTagRegistry()};
    BOOST_REQUIRE_EQUAL(registry.size(), std::size(PINNED_TAGS));
    for (size_t i{0}; i < registry.size(); ++i) {
        const auto& pin{PINNED_TAGS[i]};
        BOOST_CHECK(registry[i].tag == pin.tag);
        BOOST_CHECK_EQUAL(static_cast<int>(pin.tag), pin.value);
        BOOST_CHECK_EQUAL(registry[i].text, pin.text);
        BOOST_CHECK_EQUAL(TagText(pin.tag), pin.text);
        BOOST_CHECK(IsKnown(pin.tag));
        BOOST_CHECK_EQUAL(Hex(Tagged(pin.tag, {})), pin.empty_digest);
    }
}

BOOST_AUTO_TEST_CASE(tags_unique_prefixed_and_distinct)
{
    std::set<std::string_view> texts;
    std::set<std::string_view> names;
    std::set<uint256> digests;
    for (const auto& info : HashTagRegistry()) {
        BOOST_CHECK(info.text.starts_with(HASH_TAG_PREFIX));
        BOOST_CHECK_EQUAL(std::string{HASH_TAG_PREFIX} + std::string{info.name}, std::string{info.text});
        BOOST_CHECK(texts.insert(info.text).second);
        BOOST_CHECK(names.insert(info.name).second);
        BOOST_CHECK(digests.insert(Tagged(info.tag, {})).second);
    }
    BOOST_CHECK_EQUAL(HASH_TAG_PREFIX, "B3/FM2/");
}

BOOST_AUTO_TEST_CASE(unknown_tags_are_refused)
{
    BOOST_CHECK(!IsKnown(static_cast<HashTag>(0)));
    BOOST_CHECK(!IsKnown(static_cast<HashTag>(30)));
    BOOST_CHECK(!IsKnown(static_cast<HashTag>(255)));
    BOOST_CHECK(TagText(static_cast<HashTag>(0)).empty());
    BOOST_CHECK(TagText(static_cast<HashTag>(30)).empty());
}

BOOST_AUTO_TEST_CASE(tagged_matches_the_bip340_definition)
{
    const std::vector<unsigned char> messages[]{{}, {0x00}, {0x01, 0x02, 0x03}, std::vector<unsigned char>(200, 0xab)};
    for (const auto& info : HashTagRegistry()) {
        for (const auto& message : messages) {
            BOOST_CHECK(Tagged(info.tag, message) == ReferenceTagged(info.text, message));
            HashWriter writer{TaggedWriter(info.tag)};
            writer.write(std::as_bytes(std::span{message}));
            BOOST_CHECK(writer.GetSHA256() == Tagged(info.tag, message));
        }
    }
    // A serializable object hashes as its serialization.
    uint256 value;
    value.data()[0] = 0x42;
    BOOST_CHECK(TaggedObject(HashTag::ACCOUNT, value) == Tagged(HashTag::ACCOUNT, std::span{value.data(), value.size()}));
}

BOOST_AUTO_TEST_CASE(public_adapter_catalog_matches_the_canonical_registry)
{
    std::ifstream file{std::filesystem::path{FM2_SOURCE_ROOT} / "src/flowmesh2/core/HASH_DOMAINS.json"};
    BOOST_REQUIRE(file.is_open());
    const std::string encoded{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    UniValue catalog;
    BOOST_REQUIRE(catalog.read(encoded));
    BOOST_REQUIRE_EQUAL(catalog["format"].get_str(), "fm2-hash-domains/1");
    BOOST_CHECK_EQUAL(catalog["prefix"].get_str(), HASH_TAG_PREFIX);
    const auto registry{HashTagRegistry()};
    const auto& rows{catalog["tags"].get_array()};
    BOOST_REQUIRE_EQUAL(rows.size(), registry.size());
    for (size_t i{0}; i < registry.size(); ++i) {
        BOOST_CHECK_EQUAL(rows[i]["id"].getInt<int>(), static_cast<int>(registry[i].tag));
        BOOST_CHECK_EQUAL(rows[i]["name"].get_str(), registry[i].name);
        BOOST_CHECK_EQUAL(rows[i]["text"].get_str(), registry[i].text);
    }
}

BOOST_AUTO_TEST_CASE(no_collision_with_repository_tags)
{
    const std::filesystem::path root{FM2_SOURCE_ROOT};
    const auto existing{RepositoryB3Tags(root)};
    // The scan is not vacuous: it finds tags that are known to exist.
    BOOST_REQUIRE_MESSAGE(existing.size() >= 20, "repository scan found only " << existing.size() << " B3/ tags");
    BOOST_CHECK(existing.contains("B3/MODERN/CHAIN"));
    BOOST_CHECK(existing.contains("B3/FLOWMESH/SEATSET/V1"));
    for (const auto& info : HashTagRegistry()) {
        BOOST_CHECK_MESSAGE(!existing.contains(std::string{info.text}), info.text << " collides with an existing tag");
    }
    // The B3/FM2/ namespace is reserved for this registry.
    for (const auto& tag : existing) {
        BOOST_CHECK_MESSAGE(!tag.starts_with(std::string{HASH_TAG_PREFIX}), tag << " uses the reserved B3/FM2/ prefix outside src/flowmesh2");
    }
    BOOST_TEST_MESSAGE("repository B3/ tags scanned: " << existing.size());
}

BOOST_AUTO_TEST_SUITE_END()
