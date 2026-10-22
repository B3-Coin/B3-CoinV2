// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bridge/arbitrum_proof_codec.h>
#include <util/strencodings.h>
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <type_traits>

namespace arb = bridge::arbitrum;
namespace wire = arb::proof_codec;

BOOST_AUTO_TEST_SUITE(bridge_arbitrum_proof_codec_tests)
namespace {
using Bytes = arb::Bytes;

uint256 Repeated(unsigned char value)
{
    std::array<unsigned char, 32> bytes;
    bytes.fill(value);
    return uint256{std::span<const unsigned char>{bytes}};
}

arb::Proof Tiny()
{
    arb::Proof proof;
    proof.assertion = {Repeated(0x11), Repeated(0x22), Repeated(0x33),
                       0x0102030405060708ULL, 0x1112131415161718ULL, 1,
                       Repeated(0x44), Repeated(0x55)};
    proof.blocks.push_back({{0xc0}, {{0, {0x01}}}, {{0, {0x02}}}});
    return proof;
}

// An independent test-only writer deliberately permits malformed structures.
// It is not the production encoder and never verifies or authorizes a proof.
class FixtureWire {
public:
    Bytes bytes;
    void U8(unsigned char value) { bytes.push_back(value); }
    void U32(uint32_t value) { for (size_t i{0}; i < 4; ++i) U8(static_cast<unsigned char>(value >> (24 - 8 * i))); }
    void U64(uint64_t value) { for (size_t i{0}; i < 8; ++i) U8(static_cast<unsigned char>(value >> (56 - 8 * i))); }
    void Raw(std::span<const unsigned char> value) { bytes.insert(bytes.end(), value.begin(), value.end()); }
    void Root(const uint256& value) { Raw({value.begin(), 32}); }
    void Blob(const Bytes& value) { U32(static_cast<uint32_t>(value.size())); Raw(value); }
    void Nodes(const arb::Nodes& value) { U32(static_cast<uint32_t>(value.size())); for (const auto& node : value) Blob(node); }
    void Accounts(const std::vector<arb::AccountWitness>& value)
    {
        U32(static_cast<uint32_t>(value.size()));
        for (const auto& account : value) {
            Raw(account.address); Nodes(account.nodes); U32(static_cast<uint32_t>(account.storage.size()));
            for (const auto& storage : account.storage) { Raw(storage.slot); Nodes(storage.nodes); }
        }
    }
    template <typename T> void Indexed(const std::vector<T>& value)
    {
        U32(static_cast<uint32_t>(value.size()));
        for (const auto& item : value) { U64(item.index); Blob(item.encoded); }
    }
};

void Put32(Bytes& out, size_t offset, uint32_t value)
{
    BOOST_REQUIRE(offset + 4 <= out.size());
    for (size_t i{0}; i < 4; ++i) out[offset + i] = static_cast<unsigned char>(value >> (24 - 8 * i));
}

Bytes Unchecked(const arb::Proof& proof)
{
    FixtureWire out;
    const std::array<unsigned char, 8> magic{'B', '3', 'A', 'R', 'P', 'R', '0', '1'};
    out.Raw(magic); out.U32(0);
    const auto& a{proof.assertion};
    out.Root(a.parent_assertion); out.Root(a.block_hash); out.Root(a.send_root);
    out.U64(a.inbox_position); out.U64(a.position_in_message); out.U8(a.machine_status);
    out.Root(a.end_history_root); out.Root(a.inbox_accumulator);
    out.Accounts(proof.l1_accounts);
    out.U32(static_cast<uint32_t>(proof.blocks.size()));
    for (const auto& block : proof.blocks) { out.Blob(block.header); out.Indexed(block.receipts); out.Indexed(block.transactions); }
    out.Accounts(proof.confirmed_accounts); out.Accounts(proof.historical_accounts);
    Put32(out.bytes, 8, static_cast<uint32_t>(out.bytes.size() - 12));
    return std::move(out.bytes);
}

Bytes Encode(const arb::Proof& proof)
{
    auto encoded{wire::EncodeProofV1(proof)};
    BOOST_REQUIRE(encoded.result == wire::Result::OK);
    BOOST_REQUIRE(encoded.bytes);
    return std::move(*encoded.bytes);
}

void Refuses(const Bytes& bytes, wire::Result result)
{
    const auto decoded{wire::DecodeProofV1(bytes)};
    BOOST_CHECK_MESSAGE(decoded.result == result, wire::Name(decoded.result));
    BOOST_CHECK(!decoded.proof);
    BOOST_CHECK(decoded.digest.IsNull());
}

void RejectsBoth(const arb::Proof& proof, wire::Result result)
{
    const auto encoded{wire::EncodeProofV1(proof)};
    BOOST_CHECK(encoded.result == result);
    BOOST_CHECK(!encoded.bytes);
    BOOST_CHECK(encoded.digest.IsNull());
    Refuses(Unchecked(proof), result);
}

void Roundtrip(const arb::Proof& proof)
{
    const auto expected{Unchecked(proof)};
    const auto encoded{wire::EncodeProofV1(proof)};
    BOOST_REQUIRE(encoded.result == wire::Result::OK);
    BOOST_REQUIRE(encoded.bytes);
    BOOST_CHECK(*encoded.bytes == expected);
    const auto decoded{wire::DecodeProofV1(expected)};
    BOOST_REQUIRE(decoded.result == wire::Result::OK);
    BOOST_REQUIRE(decoded.proof);
    BOOST_CHECK(Encode(*decoded.proof) == expected);
    BOOST_CHECK(decoded.digest == encoded.digest);
}

arb::AccountWitness Account()
{
    arb::AccountWitness account;
    account.address.fill(0x61);
    account.nodes = {{0xaa, 0xbb}};
    arb::StorageWitness storage;
    storage.slot.fill(0x71);
    storage.nodes = {{0xcc, 0xdd}};
    account.storage.push_back(std::move(storage));
    return account;
}

arb::Proof ManyEntries(size_t count)
{
    auto proof{Tiny()};
    proof.blocks.clear();
    for (size_t remaining{count}; remaining;) {
        const size_t n{std::min(remaining, arb::MAX_RECEIPTS)};
        arb::BlockWitness block;
        block.header = {0xc0};
        for (size_t i{0}; i < n; ++i) {
            block.receipts.push_back({i, {0x01}});
            block.transactions.push_back({i, {0x02}});
        }
        proof.blocks.push_back(std::move(block));
        remaining -= n;
    }
    return proof;
}
} // namespace

BOOST_AUTO_TEST_CASE(independent_literal_golden_wire)
{
    // Independent Python struct.pack('>I'/'>Q') fixture, not a commitment proof.
    const std::string hex{
        "4233415250523031000000e8"
        "1111111111111111111111111111111111111111111111111111111111111111"
        "2222222222222222222222222222222222222222222222222222222222222222"
        "3333333333333333333333333333333333333333333333333333333333333333"
        "0102030405060708111213141516171801"
        "4444444444444444444444444444444444444444444444444444444444444444"
        "5555555555555555555555555555555555555555555555555555555555555555"
        "000000000000000100000001c00000000100000000000000000000000101"
        "00000001000000000000000000000001020000000000000000"};
    const auto expected{TryParseHex<unsigned char>(hex)};
    BOOST_REQUIRE(expected);
    BOOST_REQUIRE_EQUAL(expected->size(), 244U);
    BOOST_CHECK(Encode(Tiny()) == *expected);
    const auto decoded{wire::DecodeProofV1(*expected)};
    BOOST_REQUIRE(decoded.result == wire::Result::OK);
    BOOST_REQUIRE(decoded.proof);
    uint256 plain;
    CSHA256().Write(expected->data(), expected->size()).Finalize(plain.begin());
    BOOST_CHECK_EQUAL(HexStr(arb::detail::Raw(plain)), "b09b19ebcbc5f1ad29166bc563f278f6cfd07742ee9c7216a56629d71ae23d88");
    BOOST_CHECK_EQUAL(HexStr(arb::detail::Raw(decoded.digest)), "aa989c48713b75b092e0863a2766e5870f1a1a807f7b8acfcf682745a68876d3");
    BOOST_CHECK_EQUAL(decoded.proof->assertion.inbox_position, 0x0102030405060708ULL);
    BOOST_CHECK_EQUAL(decoded.proof->assertion.position_in_message, 0x1112131415161718ULL);
    BOOST_CHECK(decoded.proof->assertion.parent_assertion == Repeated(0x11));
    BOOST_CHECK(Encode(*decoded.proof) == *expected);
}

BOOST_AUTO_TEST_CASE(all_nested_fields_are_owned_and_roundtrip)
{
    auto proof{Tiny()};
    proof.l1_accounts = {Account()};
    proof.confirmed_accounts = {Account(), Account()};
    proof.historical_accounts = {Account()};
    proof.blocks.push_back(proof.blocks.front());
    proof.blocks.back().header = {0x81, 0x01};
    Roundtrip(proof);
    auto bytes{Encode(proof)};
    auto decoded{wire::DecodeProofV1(bytes)};
    BOOST_REQUIRE(decoded.proof);
    BOOST_CHECK(decoded.proof->l1_accounts[0].storage[0].slot == proof.l1_accounts[0].storage[0].slot);
    BOOST_CHECK(decoded.proof->confirmed_accounts[1].nodes == proof.confirmed_accounts[1].nodes);
    BOOST_CHECK(decoded.proof->historical_accounts[0].storage[0].nodes == proof.historical_accounts[0].storage[0].nodes);
    std::fill(bytes.begin(), bytes.end(), 0xff);
    BOOST_CHECK(Encode(*decoded.proof) == Encode(proof));
}

BOOST_AUTO_TEST_CASE(every_truncated_prefix_refuses_without_a_partial_proof)
{
    auto proof{Tiny()};
    proof.l1_accounts = {Account()};
    const auto bytes{Encode(proof)};
    for (size_t i{0}; i < bytes.size(); ++i) {
        const auto decoded{wire::DecodeProofV1(std::span<const unsigned char>{bytes}.first(i))};
        BOOST_CHECK(decoded.result == wire::Result::TRUNCATED);
        BOOST_CHECK(!decoded.proof);
        BOOST_CHECK(decoded.digest.IsNull());
    }
}

BOOST_AUTO_TEST_CASE(magic_version_and_payload_length_are_exact)
{
    const auto bytes{Encode(Tiny())};
    for (size_t i{0}; i < 8; ++i) {
        auto bad{bytes}; bad[i] ^= 1;
        Refuses(bad, wire::Result::BAD_MAGIC);
    }
    auto bad{bytes}; Put32(bad, 8, 231); Refuses(bad, wire::Result::TRAILING);
    bad = bytes; Put32(bad, 8, 233); Refuses(bad, wire::Result::TRUNCATED);
    bad = bytes; Put32(bad, 8, 0xffffffffU); Refuses(bad, wire::Result::LIMIT);
    bad = bytes; std::reverse(bad.begin() + 8, bad.begin() + 12); Refuses(bad, wire::Result::LIMIT);
}

BOOST_AUTO_TEST_CASE(trailing_bytes_never_form_an_extension)
{
    auto bytes{Encode(Tiny())};
    bytes.push_back(0);
    Refuses(bytes, wire::Result::TRAILING);
    Put32(bytes, 8, static_cast<uint32_t>(bytes.size() - 12));
    Refuses(bytes, wire::Result::TRAILING);
}

BOOST_AUTO_TEST_CASE(huge_top_level_counts_refuse_before_vector_allocation)
{
    const auto bytes{Encode(Tiny())};
    auto bad{bytes}; Put32(bad, 189, 0xffffffffU); Refuses(bad, wire::Result::LIMIT);
    bad = bytes; Put32(bad, 189, 16); Refuses(bad, wire::Result::TRUNCATED);
    bad = bytes; Put32(bad, 193, 0); Refuses(bad, wire::Result::BAD_STRUCTURE);
    bad = bytes; Put32(bad, 193, 129); Refuses(bad, wire::Result::LIMIT);
    bad = bytes; Put32(bad, 193, 128); Refuses(bad, wire::Result::TRUNCATED);
}

BOOST_AUTO_TEST_CASE(nested_counts_and_lengths_are_bounded_before_allocation)
{
    auto proof{Tiny()}; proof.l1_accounts = {Account()};
    const auto bytes{Encode(proof)};
    // Fixed fixture offsets independently calculated from the V1 layout.
    auto bad{bytes}; Put32(bad, 213, 65); Refuses(bad, wire::Result::LIMIT);
    bad = bytes; Put32(bad, 217, 0); Refuses(bad, wire::Result::BAD_STRUCTURE);
    bad = bytes; Put32(bad, 217, 8193); Refuses(bad, wire::Result::LIMIT);
    bad = bytes; Put32(bad, 217, 8192); Refuses(bad, wire::Result::TRUNCATED);
    bad = bytes; Put32(bad, 223, 66); Refuses(bad, wire::Result::LIMIT);
    bad = bytes; Put32(bad, 223, 65); Refuses(bad, wire::Result::TRUNCATED);
    bad = bytes; Put32(bad, 259, 0xffffffffU); Refuses(bad, wire::Result::LIMIT);
    bad = bytes; Put32(bad, 263, 0xffffffffU); Refuses(bad, wire::Result::LIMIT);
}

BOOST_AUTO_TEST_CASE(empty_blocks_and_empty_raw_blobs_refuse)
{
    auto proof{Tiny()}; proof.blocks.clear(); RejectsBoth(proof, wire::Result::BAD_STRUCTURE);
    proof = Tiny(); proof.blocks[0].header.clear(); RejectsBoth(proof, wire::Result::BAD_STRUCTURE);
    proof = Tiny(); proof.blocks[0].receipts[0].encoded.clear(); RejectsBoth(proof, wire::Result::BAD_STRUCTURE);
    proof = Tiny(); proof.blocks[0].transactions[0].encoded.clear(); RejectsBoth(proof, wire::Result::BAD_STRUCTURE);
    proof = Tiny(); proof.l1_accounts = {Account()}; proof.l1_accounts[0].nodes[0].clear();
    RejectsBoth(proof, wire::Result::BAD_STRUCTURE);
    proof = Tiny(); proof.l1_accounts = {Account()}; proof.l1_accounts[0].storage[0].nodes[0].clear();
    RejectsBoth(proof, wire::Result::BAD_STRUCTURE);
}

BOOST_AUTO_TEST_CASE(indices_and_receipt_transaction_counts_are_canonical)
{
    auto proof{Tiny()}; proof.blocks[0].receipts[0].index = 1; RejectsBoth(proof, wire::Result::BAD_STRUCTURE);
    proof = Tiny(); proof.blocks[0].transactions[0].index = 0xffffffffffffffffULL; RejectsBoth(proof, wire::Result::BAD_STRUCTURE);
    proof = Tiny(); proof.blocks[0].transactions.clear(); RejectsBoth(proof, wire::Result::BAD_STRUCTURE);
    proof = Tiny(); proof.blocks[0].receipts.push_back(proof.blocks[0].receipts.front());
    proof.blocks[0].transactions.push_back(proof.blocks[0].transactions.front());
    RejectsBoth(proof, wire::Result::BAD_STRUCTURE);
}

BOOST_AUTO_TEST_CASE(header_and_node_maximum_lengths_are_inclusive)
{
    auto proof{Tiny()}; proof.blocks[0].header.resize(arb::MAX_HEADER_BYTES, 0x11); Roundtrip(proof);
    proof.blocks[0].header.push_back(0); RejectsBoth(proof, wire::Result::LIMIT);
    proof = Tiny(); proof.l1_accounts = {Account()};
    proof.l1_accounts[0].nodes[0].resize(wire::MAX_NODE_BYTES, 0x22); Roundtrip(proof);
    proof.l1_accounts[0].nodes[0].push_back(0); RejectsBoth(proof, wire::Result::LIMIT);
}

BOOST_AUTO_TEST_CASE(receipt_and_transaction_maximum_lengths_are_inclusive)
{
    auto proof{Tiny()};
    proof.blocks[0].receipts[0].encoded.resize(arb::MAX_RECEIPT_BYTES, 0x33);
    proof.blocks[0].transactions[0].encoded.resize(arb::MAX_TRANSACTION_BYTES, 0x44);
    Roundtrip(proof);
    proof.blocks[0].receipts[0].encoded.push_back(0); RejectsBoth(proof, wire::Result::LIMIT);
    proof.blocks[0].receipts[0].encoded.pop_back();
    proof.blocks[0].transactions[0].encoded.push_back(0); RejectsBoth(proof, wire::Result::LIMIT);
}

BOOST_AUTO_TEST_CASE(account_storage_and_node_count_limits_are_inclusive)
{
    auto proof{Tiny()};
    proof.l1_accounts.resize(arb::MAX_ACCOUNTS);
    proof.confirmed_accounts.resize(arb::MAX_ACCOUNTS);
    proof.historical_accounts.resize(arb::MAX_ACCOUNTS);
    Roundtrip(proof);
    proof.historical_accounts.emplace_back(); RejectsBoth(proof, wire::Result::LIMIT);
    proof = Tiny(); proof.l1_accounts = {Account()};
    proof.l1_accounts[0].storage.resize(wire::MAX_STORAGE_PER_ACCOUNT);
    proof.l1_accounts[0].nodes.resize(wire::MAX_NODES_PER_WITNESS, {0xc0}); Roundtrip(proof);
    proof.l1_accounts[0].nodes.push_back({0xc0}); RejectsBoth(proof, wire::Result::LIMIT);
    proof.l1_accounts[0].nodes.pop_back();
    proof.l1_accounts[0].storage.emplace_back(); RejectsBoth(proof, wire::Result::LIMIT);
}

BOOST_AUTO_TEST_CASE(header_count_and_per_block_item_count_limits_are_inclusive)
{
    auto proof{Tiny()}; proof.blocks.resize(arb::MAX_HEADERS, proof.blocks.front()); Roundtrip(proof);
    proof.blocks.push_back(proof.blocks.front()); RejectsBoth(proof, wire::Result::LIMIT);
    proof = ManyEntries(arb::MAX_RECEIPTS); Roundtrip(proof);
    proof.blocks[0].receipts.push_back({arb::MAX_RECEIPTS, {1}});
    proof.blocks[0].transactions.push_back({arb::MAX_RECEIPTS, {2}});
    RejectsBoth(proof, wire::Result::LIMIT);
}

BOOST_AUTO_TEST_CASE(aggregate_receipt_and_transaction_budgets_span_blocks)
{
    auto proof{ManyEntries(wire::MAX_TOTAL_RECEIPTS)};
    Roundtrip(proof);
    proof.blocks.push_back(Tiny().blocks.front());
    RejectsBoth(proof, wire::Result::LIMIT);
}

BOOST_AUTO_TEST_CASE(aggregate_mpt_node_budget_spans_accounts_and_storage)
{
    auto proof{Tiny()}; proof.l1_accounts.resize(2);
    for (auto& account : proof.l1_accounts) {
        account.storage.resize(64);
        for (auto& storage : account.storage) storage.nodes.resize(64, {0xc0});
    }
    Roundtrip(proof); // 2 * 64 * 64 == 8192 nodes
    proof.confirmed_accounts.resize(1);
    proof.confirmed_accounts[0].nodes.push_back({0xc0});
    RejectsBoth(proof, wire::Result::LIMIT);
}

BOOST_AUTO_TEST_CASE(frame_budget_includes_every_prefix_and_is_exact)
{
    auto proof{Tiny()}; proof.blocks.resize(16, proof.blocks.front());
    for (auto& block : proof.blocks) {
        block.receipts[0].encoded.resize(arb::MAX_RECEIPT_BYTES, 0x11);
        block.transactions[0].encoded.resize(arb::MAX_TRANSACTION_BYTES, 0x22);
    }
    auto refused{wire::EncodeProofV1(proof)};
    BOOST_CHECK(refused.result == wire::Result::LIMIT);
    BOOST_CHECK(!refused.bytes);
    // 205 fixed bytes + 37 framing bytes per one-item block, independently
    // calculated; raw data alone equals 32MiB before this adjustment.
    constexpr size_t overhead{205 + 16 * 37};
    proof.blocks.back().transactions[0].encoded.resize(arb::MAX_TRANSACTION_BYTES - overhead);
    auto encoded{wire::EncodeProofV1(proof)};
    BOOST_REQUIRE(encoded.result == wire::Result::OK);
    BOOST_REQUIRE(encoded.bytes);
    BOOST_CHECK_EQUAL(encoded.bytes->size(), wire::MAX_FRAME_BYTES);
    {
        auto decoded{wire::DecodeProofV1(*encoded.bytes)};
        BOOST_REQUIRE(decoded.result == wire::Result::OK);
        BOOST_REQUIRE(decoded.proof);
    }
    encoded.bytes->push_back(0);
    Refuses(*encoded.bytes, wire::Result::LIMIT);
}

BOOST_AUTO_TEST_CASE(blob_lengths_do_not_borrow_following_or_absent_bytes)
{
    const auto bytes{Encode(Tiny())};
    auto bad{bytes}; Put32(bad, 197, 0xffffffffU); Refuses(bad, wire::Result::LIMIT);
    bad = bytes; Put32(bad, 197, 1000); Refuses(bad, wire::Result::TRUNCATED);
    bad = bytes; Put32(bad, 202, 8193); Refuses(bad, wire::Result::LIMIT);
    bad = bytes; Put32(bad, 202, 8192); Refuses(bad, wire::Result::TRUNCATED);
    bad = bytes; Put32(bad, 214, 0xffffffffU); Refuses(bad, wire::Result::LIMIT);
}

BOOST_AUTO_TEST_CASE(raw_hash_bytes_and_all_integer_bits_are_preserved)
{
    auto proof{Tiny()};
    for (size_t i{0}; i < 32; ++i) proof.assertion.parent_assertion.begin()[i] = static_cast<unsigned char>(i);
    proof.assertion.inbox_position = 0xffffffffffffffffULL;
    proof.assertion.position_in_message = 0;
    proof.assertion.machine_status = 255; // Codec is not the FINISHED verifier.
    Roundtrip(proof);
    const auto bytes{Encode(proof)};
    for (size_t i{0}; i < 32; ++i) BOOST_CHECK_EQUAL(bytes[12 + i], i);
}

BOOST_AUTO_TEST_CASE(diagnostic_digest_is_domain_separated_and_changes_with_proof)
{
    auto first{wire::EncodeProofV1(Tiny())};
    BOOST_REQUIRE(first.bytes);
    const auto plain{bridge::peer::TaggedDigest("", *first.bytes)};
    BOOST_CHECK(first.digest != plain);
    auto proof{Tiny()}; proof.assertion.inbox_position ^= 1;
    const auto second{wire::EncodeProofV1(proof)};
    BOOST_REQUIRE(second.bytes);
    BOOST_CHECK(first.digest != second.digest);
    auto decoded{wire::DecodeProofV1(*first.bytes)};
    BOOST_CHECK(decoded.digest == first.digest);
}

BOOST_AUTO_TEST_CASE(codec_success_never_constructs_production_authority)
{
    static_assert(!std::is_default_constructible_v<arb::ConfirmedRange>);
    static_assert(!std::is_convertible_v<wire::DecodedProof, arb::ConfirmedRange>);
    static_assert(!std::is_convertible_v<arb::fixture::Evidence, arb::ConfirmedRange>);
    const auto decoded{wire::DecodeProofV1(Encode(Tiny()))};
    BOOST_REQUIRE(decoded.proof);
    // Tiny's RLP and all fixture trust inputs are intentionally invalid even
    // though transport is well formed; parser success is never proof success.
    const auto checked{arb::fixture::Verify({}, {}, {}, *decoded.proof)};
    BOOST_CHECK(checked.error == arb::Error::BAD_POLICY);
    BOOST_CHECK(!checked.evidence);
}

BOOST_AUTO_TEST_SUITE_END()
