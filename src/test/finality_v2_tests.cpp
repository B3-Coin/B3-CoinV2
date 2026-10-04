// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// Fresh finality lineage, digest V2 (pure layer, modern/finality_v2.h):
// FinalizedBlockV2 bytes, lineage id, digest V2, certId, the checkpoint
// history tree and its paths, the ruleset-2 set header, the V2 certificate
// payload and VerifyFinalityCertificateV2. Byte layouts are owned by
// doc/design/b3-bridge-v2-contracts-spec.md section 2.
//
// Vector provenance. Every expected hash below was computed independently of
// this C++ code, with python3 stdlib only. The pattern vectors of
// contracts_spec_symmetric_vectors are the committed generator's own
// functions (contracts/test/vectors/gen_v2_vectors.py: lineage_id,
// history_leaf, tree_root, encode_finalized_block, keccak256, digest_v2)
// evaluated on the 0x11.. pattern inputs, which that generator does not write
// out. The asymmetric set and shared_vector_history_paths equal the shared
// C++/Foundry vectors file contracts/test/vectors/bridge_v2_vectors.json,
// written by contracts/test/vectors/gen_v2_vectors.sh (lineage[0]
// "stand_in", set_header_v2[0..1], history leaves, roots and paths,
// finalized_block_v2[0..1]). This test hard-codes those values rather than
// reading that file, so a regenerated file must be copied here again. Hex is
// in EVM byte order: the 32 bytes of a hash are its raw uint256 bytes, as
// V1's FinalizedBlock::Encode.
//
// The last case covers the type-11 carrier registry (MPA
// FINALITY_CERTIFICATE_V2): activation gate, size rule, declared cost and
// coinbase-only placement.

#include <chainparams.h>
#include <common/args.h>
#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <consensus/era.h>
#include <consensus/params.h>
#include <crypto/bls.h>
#include <modern/creation_action.h>
#include <modern/finality_certificate.h>
#include <modern/finality_types.h>
#include <modern/finality_v2.h>
#include <modern/mpa.h>
#include <modern/payload_cost.h>
#include <node/finality_binding_index.h>
#include <node/validator_set.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <uint256.h>
#include <util/chaintype.h>
#include <util/strencodings.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using modern::CertificateCheck;
using modern::FinalityCertificate;
using modern::FinalizedBlock;
using modern::FinalizedBlockV2;
using modern::ValidatorSetHeader;
using modern::ValidatorSetView;
using node::FinalityBindingIndex;
using node::ValidatorSetSnapshot;

namespace {

//! A hash whose 32 raw bytes are the given EVM-order hex (no byte reversal).
uint256 Evm(const std::string_view hex)
{
    const auto bytes{ParseHex(hex)};
    BOOST_REQUIRE_EQUAL(bytes.size(), 32U);
    uint256 out;
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return out;
}

//! EVM-order hex of a hash's raw bytes.
std::string EvmHex(const uint256& h) { return HexStr(std::span<const unsigned char>(h.begin(), 32)); }

uint256 Fill(const unsigned char b)
{
    uint256 out;
    std::fill(out.begin(), out.end(), b);
    return out;
}

// ----------------------------------------------------------- BLS set helpers
// (same construction as finality_certificate_tests: the real snapshot
// machinery, so the view is the one a node would use)

const uint256 CHAIN_DOMAIN{Fill(0xd0)};
const uint256 OTHER_CHAIN_DOMAIN{Fill(0xe1)};
constexpr CAmount UNIT{modern::FINALITY_WEIGHT_UNIT};

bls::SecretKey BlsK(const unsigned i)
{
    std::array<unsigned char, 32> ikm{};
    ikm[0] = static_cast<unsigned char>(i);
    ikm[1] = static_cast<unsigned char>(i >> 8);
    ikm[31] = 0x5E;
    return *bls::SecretKey::FromIKM(ikm);
}

modern::ValidatorKeyBytes VK(const unsigned i)
{
    modern::ValidatorKeyBytes k{};
    k[0] = static_cast<unsigned char>(i);
    k[1] = static_cast<unsigned char>(i >> 8);
    k[31] = 0x44;
    return k;
}

struct SetUnderTest {
    std::vector<bls::SecretKey> keys;
    ValidatorSetSnapshot snapshot;
    ValidatorSetView view;
    explicit SetUnderTest(const std::vector<uint64_t>& weights)
        : snapshot{Make(weights, keys)}, view{snapshot.View()} {}
    static ValidatorSetSnapshot Make(const std::vector<uint64_t>& weights, std::vector<bls::SecretKey>& keys)
    {
        FinalityBindingIndex bindings;
        std::map<node::ValidatorKey, CAmount> w;
        std::vector<FinalityBindingIndex::Transition> ts;
        for (size_t i = 0; i < weights.size(); ++i) {
            keys.push_back(BlsK(static_cast<unsigned>(i + 1)));
            ts.push_back({VK(static_cast<unsigned>(i + 1)), {keys.back().GetPublicKey().Compressed(), 0, 1}});
            w[VK(static_cast<unsigned>(i + 1))] = static_cast<CAmount>(weights[i]) * UNIT;
        }
        bindings.ConnectBlock(1, ts);
        // The V1 snapshot epoch is irrelevant: fresh headers re-stamp it.
        return *ValidatorSetSnapshot::Build(/*epoch=*/9, w, bindings);
    }
    //! Fresh-lineage successor hash: ruleset-2 header of the same members at `epoch`.
    uint256 V2Hash(const uint64_t epoch) const
    {
        return modern::ValidatorSetHash(modern::RulesetV2Header(snapshot.Header(), epoch));
    }
};

FinalizedBlockV2 FbV2(const uint256& successor, const uint64_t epoch = 0)
{
    FinalizedBlockV2 fb;
    fb.lineage_id = Fill(0x4c);
    fb.height = 266;
    fb.block_hash = Fill(0x01);
    fb.withdrawal_root = uint256{};
    fb.validator_set_hash = successor;
    fb.epoch = epoch;
    fb.checkpoint_index = 1;
    fb.checkpoint_history_root = Fill(0x7a);
    fb.bridge_flags = 0;
    return fb;
}

//! Sign `digest` by the given member indices (snapshot order).
FinalityCertificate SignDigest(const SetUnderTest& set, const uint256& digest, const std::vector<uint32_t>& signers)
{
    FinalityCertificate cert;
    cert.signer_bitmap.assign(modern::SignerBitmapBytes(set.view.validator_count), 0);
    std::vector<bls::Signature> sigs;
    for (const uint32_t idx : signers) {
        cert.signer_bitmap[idx / 8] |= static_cast<unsigned char>(1U << (idx % 8));
        const auto& pk{set.snapshot.Members()[idx].bls_pubkey};
        for (const auto& k : set.keys) {
            if (k.GetPublicKey().Compressed() == pk) {
                sigs.push_back(k.Sign(std::span<const unsigned char>(digest.begin(), 32)));
                break;
            }
        }
    }
    if (!sigs.empty()) cert.aggregate_sig = bls::AggregateSignatures(sigs)->Compressed();
    return cert;
}

FinalityCertificate SignV2(const SetUnderTest& set, const FinalizedBlockV2& fb, const std::vector<uint32_t>& signers,
                           const uint256& domain = CHAIN_DOMAIN)
{
    return SignDigest(set, modern::FinalityDigestV2(domain, fb), signers);
}

CertificateCheck VerifyV2(const SetUnderTest& set, const FinalizedBlockV2& fb, const FinalityCertificate& cert,
                          const uint256& domain = CHAIN_DOMAIN)
{
    return modern::VerifyFinalityCertificateV2(domain, fb, cert, set.view, set.V2Hash(fb.epoch + 1));
}

// ---------------------------------------------------- stand-in set header
// set_header_v2 of the shared vectors: one member, W = 333, quorum 223.
ValidatorSetHeader StandInHeaderV1(const uint64_t epoch)
{
    ValidatorSetHeader h;
    h.epoch = epoch;
    h.ruleset_version = modern::FINALITY_RULESET_V1;
    h.validator_count = 1;
    h.total_weight = 333;
    h.quorum_weight = 223;
    const auto pk{ParseHex("aa3b6ee8aadb63c8e064dafdc3d0ae794ded037af26e60829da1592db0a237dc"
                           "42684307339f176c31d97714fd78aed2")};
    std::copy(pk.begin(), pk.end(), h.aggregate_pubkey.begin());
    h.members_root = Evm("7764a7ea0f9404e15d467ec3c6f67ed968418c12aa65c7f0fe1b3ef76e493590");
    return h;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(finality_v2_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(contracts_spec_symmetric_vectors)
{
    // (a) + (b): gen_v2_vectors.py's functions on pattern inputs (see the
    // provenance note above). domain 0x11*32, G = 261, hash(G) = 0x22*32,
    // setHash(Set'_0) = 0x33*32.
    const uint256 domain{Fill(0x11)};
    const uint256 lineage{modern::FinalityLineageIdV2(domain, 261, Fill(0x22), Fill(0x33))};
    BOOST_CHECK_EQUAL(EvmHex(lineage), "b3fa7e335b816e86407ac25c85741a943f5c5eb88ed35ba25a23bb6cdb370fcc");

    const uint256 leaf0{modern::FinalityHistoryLeaf(261, Fill(0x22))};
    BOOST_CHECK_EQUAL(EvmHex(leaf0), "b3792319acd545d953dbc0f307d2bf9ffbcab79dcd7d8cd223d903dee88420bb");
    const std::vector<uint256> leaves{leaf0, modern::FinalityHistoryLeaf(266, Fill(0x44)),
                                      modern::FinalityHistoryLeaf(271, Fill(0x55))};
    const auto root3{modern::FinalityHistoryRoot(leaves)};
    BOOST_REQUIRE(root3);
    BOOST_CHECK_EQUAL(EvmHex(*root3), "17d6c7fa2af3ad793e20b996f5e3d7a05ede9f9f3886ff97af79f11ebb870deb");
    const auto root2{modern::FinalityHistoryRoot(std::span{leaves}.first(2))};
    BOOST_REQUIRE(root2);
    BOOST_CHECK_EQUAL(EvmHex(*root2), "c969927a3b02863f85a53eb1685439e65629e942b7128b92a28415d96c9f2ae4");

    FinalizedBlockV2 fb;
    fb.lineage_id = lineage;
    fb.height = 271;
    fb.block_hash = Fill(0x55);
    fb.withdrawal_root = uint256{};
    fb.validator_set_hash = Fill(0x66);
    fb.epoch = 0;
    fb.checkpoint_index = 2;
    fb.checkpoint_history_root = *root2;
    fb.bridge_flags = 0;
    const auto enc{fb.Encode()};
    BOOST_CHECK_EQUAL(enc.size(), 185U);
    BOOST_CHECK_EQUAL(HexStr(enc),
                      "b3fa7e335b816e86407ac25c85741a943f5c5eb88ed35ba25a23bb6cdb370fcc"
                      "000000000000010f"
                      "5555555555555555555555555555555555555555555555555555555555555555"
                      "0000000000000000000000000000000000000000000000000000000000000000"
                      "6666666666666666666666666666666666666666666666666666666666666666"
                      "0000000000000000"
                      "0000000000000002"
                      "c969927a3b02863f85a53eb1685439e65629e942b7128b92a28415d96c9f2ae4"
                      "00");
    BOOST_CHECK_EQUAL(EvmHex(modern::FinalityCertIdV2(fb)),
                      "5375617099b6bc8f52f8514d71192e52c5335125030ab5eda9670380f866746f");
    BOOST_CHECK_EQUAL(EvmHex(modern::FinalityDigestV2(domain, fb)),
                      "f477de7f06a31c8372f87b01bfe06f14711fd27b7f2ab07efb278cf8ec9bc7f7");
}

BOOST_AUTO_TEST_CASE(asymmetric_vectors_pin_byte_order)
{
    // Asymmetric hashes pin the raw-byte placement of every bytes32 field,
    // which the 0x11.. pattern vectors cannot. Shared-vector values (see the
    // provenance note at the top of this file).
    const uint256 domain{Evm("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f")};
    const uint256 genesis_hash{Evm("fa563b7b2b4bcc75cadc13fd8ea8f1a5b631700480730e20ffdb72941a6ee3f4")};
    const uint256 hash266{Evm("3eb15e38c1fe49793d90a4ed8a01f0cb39c2fd362fc0afc4d32041913bb6442e")};
    const uint256 hash271{Evm("5bc77028bff2710500506b584d72eb0b7ace2a6b7f259e21fa2faee06011c2f4")};

    // (e) with external vectors: ruleset-2 headers of one member set.
    const ValidatorSetHeader v1{StandInHeaderV1(0)};
    const uint256 set0{modern::ValidatorSetHash(modern::RulesetV2Header(v1, 0))};
    const uint256 set1{modern::ValidatorSetHash(modern::RulesetV2Header(v1, 1))};
    BOOST_CHECK_EQUAL(HexStr(modern::RulesetV2Header(v1, 0).Encode()),
                      "0000000000000000" "0002" "00000001" "000000000000014d" "00000000000000df"
                      "aa3b6ee8aadb63c8e064dafdc3d0ae794ded037af26e60829da1592db0a237dc"
                      "42684307339f176c31d97714fd78aed2"
                      "7764a7ea0f9404e15d467ec3c6f67ed968418c12aa65c7f0fe1b3ef76e493590");
    BOOST_CHECK_EQUAL(EvmHex(set0), "c631d4251ee10b5f2f48fb4b9ad9c91fa9a3ff2bd645876b81771d80760df0d6");
    BOOST_CHECK_EQUAL(EvmHex(set1), "d577dae80a4733e62d77115a58a6f4c26339989bf8cd60c6ab0b8a42a104d425");
    BOOST_CHECK_EQUAL(EvmHex(modern::ValidatorSetHash(v1)),
                      "e0526e5223d441e6d46c6efdbad619704d4b422446d34526a65bb43b8aa44739");

    const uint256 lineage{modern::FinalityLineageIdV2(domain, 261, genesis_hash, set0)};
    BOOST_CHECK_EQUAL(EvmHex(lineage), "1a963944632a75ae707940a80da3ba4625bc32d5a65f3c342ce75e379dd78446");

    const uint256 leaf0{modern::FinalityHistoryLeaf(261, genesis_hash)};
    const uint256 leaf1{modern::FinalityHistoryLeaf(266, hash266)};
    const uint256 leaf2{modern::FinalityHistoryLeaf(271, hash271)};
    BOOST_CHECK_EQUAL(EvmHex(leaf0), "d40278962c11402f8468c48d9a5362c98aacd090e6efa9dac9bfe25789e8ba8f");
    BOOST_CHECK_EQUAL(EvmHex(leaf1), "52aa35b9cdbf38c09c553c511b723c10e6f69fc843ca22d06de147a6db3e09a2");
    BOOST_CHECK_EQUAL(EvmHex(leaf2), "85f41fe53f595b19c1d1dd26b567a73cc99fc27ef4df8ba3695310a130bcca82");
    const std::vector<uint256> leaves{leaf0, leaf1, leaf2};
    const std::array<std::string_view, 4> roots{
        "27ae5ba08d7291c96c8cbddcc148bf48a6d68c7974b94356f53754ef6171d757", // empty (zero_32)
        "1243ccb653de3036ff1b508195e5b82a90ff7592a269e42cb347f901f11c4769",
        "d32e3390e4ba5238e590e92bd7e6d10899fb4bb7afcf472d1f5874d341299bd3",
        "b403ac366596de480d729bc8d28e588f197b5fd4b87d8f924132613e31d55407",
    };
    for (size_t k{0}; k < roots.size(); ++k) {
        const auto root{modern::FinalityHistoryRoot(std::span{leaves}.first(k))};
        BOOST_REQUIRE(root);
        BOOST_CHECK_EQUAL(EvmHex(*root), roots[k]);
    }

    FinalizedBlockV2 fb;
    fb.lineage_id = lineage;
    fb.height = 266;
    fb.block_hash = hash266;
    fb.withdrawal_root = uint256{};
    fb.validator_set_hash = set1;
    fb.epoch = 0;
    fb.checkpoint_index = 1;
    fb.checkpoint_history_root = Evm(roots[1]);
    fb.bridge_flags = 0;
    const auto enc{fb.Encode()};
    BOOST_CHECK_EQUAL(HexStr(enc),
                      "1a963944632a75ae707940a80da3ba4625bc32d5a65f3c342ce75e379dd78446"
                      "000000000000010a"
                      "3eb15e38c1fe49793d90a4ed8a01f0cb39c2fd362fc0afc4d32041913bb6442e"
                      "0000000000000000000000000000000000000000000000000000000000000000"
                      "d577dae80a4733e62d77115a58a6f4c26339989bf8cd60c6ab0b8a42a104d425"
                      "0000000000000000"
                      "0000000000000001"
                      "1243ccb653de3036ff1b508195e5b82a90ff7592a269e42cb347f901f11c4769"
                      "00");
    // The block hash occupies [40,72) as its raw uint256 bytes (V1 convention).
    BOOST_CHECK(std::equal(enc.begin() + 40, enc.begin() + 72, hash266.begin()));
    BOOST_CHECK_EQUAL(EvmHex(modern::FinalityCertIdV2(fb)),
                      "431ae456c0e04803941a1c1940aabc6b4035e6d02c34800ba0c7eaf0d03d2b44");
    BOOST_CHECK_EQUAL(EvmHex(modern::FinalityDigestV2(domain, fb)),
                      "b99f7c908501c7fb7184bda390cb7561defd787e59d51bfe60799d8652fa8a4f");

    // The V2 digest is not the V1 digest of the analogous V1 object.
    FinalizedBlock analogous;
    analogous.height = fb.height;
    analogous.block_hash = fb.block_hash;
    analogous.withdrawal_root = fb.withdrawal_root;
    analogous.validator_set_hash = fb.validator_set_hash;
    analogous.epoch = fb.epoch;
    BOOST_CHECK(modern::FinalityDigest(domain, analogous) != modern::FinalityDigestV2(domain, fb));
    // Every lineage-id input matters.
    BOOST_CHECK(modern::FinalityLineageIdV2(Fill(0x01), 261, genesis_hash, set0) != lineage);
    BOOST_CHECK(modern::FinalityLineageIdV2(domain, 266, genesis_hash, set0) != lineage);
    BOOST_CHECK(modern::FinalityLineageIdV2(domain, 261, hash266, set0) != lineage);
    BOOST_CHECK(modern::FinalityLineageIdV2(domain, 261, genesis_hash, modern::ValidatorSetHash(v1)) != lineage);

    // Second statement (finalized_block_v2[1]): regtest chain domain, a
    // non-zero withdrawal root, epoch 1, index 10 over leaves 0..9. Pins the
    // [72,104) and [136,152) placement and the decoder's offsets.
    const uint256 regtest_domain{Evm("1a3405424a805d6b95600c5b7598af38b1f8f3ba25bdb2636500c76ad3f43879")};
    const uint256 regtest_lineage{modern::FinalityLineageIdV2(regtest_domain, 261, genesis_hash, set0)};
    BOOST_CHECK_EQUAL(EvmHex(regtest_lineage), "ee1d6fed4cf82fed57e0f53a0980614b20c9761190ef53da05bc9d65b8f9d667");
    const std::array<std::string_view, 11> grid_hashes{
        "fa563b7b2b4bcc75cadc13fd8ea8f1a5b631700480730e20ffdb72941a6ee3f4",
        "3eb15e38c1fe49793d90a4ed8a01f0cb39c2fd362fc0afc4d32041913bb6442e",
        "5bc77028bff2710500506b584d72eb0b7ace2a6b7f259e21fa2faee06011c2f4",
        "9acce4f0475155d779394c25c31eb5a7446e91a4d6bca9e630b9b80e22a1684f",
        "73736192039f036c5dd8e98370ffe24f2effc3ccfa3b8f09026fc064d87a7b6d",
        "2b296f0b926fa2db63d332bbd28fc85445f0ea7bf64e73ac174b86ab31785e79",
        "7279a345a305d6e9b6c3ce1b77692c398d191c476dcd25bc73f8b30264dea964",
        "87e91fe7737b012fab0967ba4574b3dbddaf2ee8a53b104a1f26c34d7f63dc60",
        "c3cf63c20df794b427750c4b7675443cc29dda3e1feef23bf9800bc7f1d5cc99",
        "e0b40c08d7041b3ceb68255187ef02effc75ee224200ba721aac95489b5fc0b4",
        "501b43b36bcf95465ad96973918744260549166c3d9264647c1e78360d197479",
    };
    std::vector<uint256> leaves10;
    for (uint64_t i{0}; i < 10; ++i) leaves10.push_back(modern::FinalityHistoryLeaf(261 + 5 * i, Evm(grid_hashes[i])));
    const auto root10{modern::FinalityHistoryRoot(leaves10)};
    BOOST_REQUIRE(root10);
    BOOST_CHECK_EQUAL(EvmHex(*root10), "dfbf06841a6eca6e4a5f7e51cd92a0188ebe8ce075e2cb97a443a6cadb35557a");
    const std::string fb2_hex{
        "ee1d6fed4cf82fed57e0f53a0980614b20c9761190ef53da05bc9d65b8f9d667"
        "0000000000000137"
        "501b43b36bcf95465ad96973918744260549166c3d9264647c1e78360d197479"
        "e2b107af8daa890eec0fef52f0f558733348e1cadf982fb8eec04124eb44d79a"
        "0a4058e802574e1406a3305205d4ccac56aba330908794d6b8509b77443fd9b9"
        "0000000000000001"
        "000000000000000a"
        "dfbf06841a6eca6e4a5f7e51cd92a0188ebe8ce075e2cb97a443a6cadb35557a"
        "00"};
    const auto fb2{FinalizedBlockV2::Decode(ParseHex(fb2_hex))};
    BOOST_REQUIRE(fb2);
    BOOST_CHECK(fb2->lineage_id == regtest_lineage);
    BOOST_CHECK_EQUAL(fb2->height, 311U);
    BOOST_CHECK(fb2->block_hash == Evm(grid_hashes[10]));
    BOOST_CHECK(fb2->withdrawal_root == Evm("e2b107af8daa890eec0fef52f0f558733348e1cadf982fb8eec04124eb44d79a"));
    BOOST_CHECK(fb2->validator_set_hash == modern::ValidatorSetHash(modern::RulesetV2Header(v1, 2)));
    BOOST_CHECK_EQUAL(fb2->epoch, 1U);
    BOOST_CHECK_EQUAL(fb2->checkpoint_index, 10U);
    BOOST_CHECK(fb2->checkpoint_history_root == *root10);
    BOOST_CHECK_EQUAL(fb2->bridge_flags, 0);
    BOOST_CHECK_EQUAL(HexStr(fb2->Encode()), fb2_hex);
    BOOST_CHECK_EQUAL(EvmHex(modern::FinalityCertIdV2(*fb2)),
                      "7e2cc2cb866493b379842ad61a16ffa190e5b0b8210571471ffc50c700ab7f87");
    BOOST_CHECK_EQUAL(EvmHex(modern::FinalityDigestV2(regtest_domain, *fb2)),
                      "1e250f522a8066b0a29859b588804d5ddfb574fb7f7436937d43fe36577f0c16");
}

BOOST_AUTO_TEST_CASE(history_paths_fold_to_root)
{
    // (c) Asymmetric leaves at the regtest grid 261 + 5i.
    std::vector<uint256> leaves;
    for (uint64_t i{0}; i < 5; ++i) {
        uint256 h;
        for (size_t b{0}; b < 32; ++b) h.begin()[b] = static_cast<unsigned char>(i * 37 + b * 11 + 1);
        leaves.push_back(modern::FinalityHistoryLeaf(261 + 5 * i, h));
    }
    for (const size_t k : {size_t{1}, size_t{2}, size_t{3}, size_t{5}}) {
        const std::span<const uint256> prefix{std::span{leaves}.first(k)};
        // Incremental accumulator == root of the level loop.
        modern::FinalityHistoryState state;
        for (const auto& l : prefix) BOOST_REQUIRE(modern::AppendFinalityHistoryLeaf(state, l));
        BOOST_CHECK_EQUAL(state.count, k);
        const auto root{modern::FinalityHistoryRoot(prefix)};
        BOOST_REQUIRE(root);
        BOOST_CHECK(*root == state.root);
        for (uint64_t index{0}; index < k; ++index) {
            uint256 path_root;
            const auto path{modern::FinalityHistoryPath(prefix, index, path_root)};
            BOOST_REQUIRE(path);
            BOOST_CHECK(path_root == *root);
            BOOST_CHECK(modern::FoldFinalityHistoryPath(prefix[index], index, *path) == *root);
            // A path is bound to its position and to its leaf.
            if (k > 1) {
                BOOST_CHECK(modern::FoldFinalityHistoryPath(prefix[index], index ^ 1U, *path) != *root);
                BOOST_CHECK(modern::FoldFinalityHistoryPath(prefix[(index + 1) % k], index, *path) != *root);
            }
        }
        // leaf0Path: leaf 0 (block R_f - 1) folds at index 0.
        uint256 leaf0_root;
        const auto leaf0_path{modern::FinalityHistoryPath(prefix, 0, leaf0_root)};
        BOOST_REQUIRE(leaf0_path);
        BOOST_CHECK(modern::FoldFinalityHistoryPath(leaves[0], 0, *leaf0_path) == *root);
        // Out of range.
        uint256 unused;
        BOOST_CHECK(!modern::FinalityHistoryPath(prefix, k, unused));
    }
    // One leaf: every sibling is the zero subtree of its level.
    uint256 r1;
    const auto p1{modern::FinalityHistoryPath(std::span{leaves}.first(1), 0, r1)};
    BOOST_REQUIRE(p1);
    for (unsigned d{0}; d < modern::FINALITY_HISTORY_DEPTH; ++d) {
        BOOST_CHECK((*p1)[d] == modern::WithdrawalZeroHashes()[d]);
    }
    // Empty tree root, empty path set, null leaves.
    BOOST_CHECK(*modern::FinalityHistoryRoot({}) == modern::WithdrawalZeroHashes()[modern::FINALITY_HISTORY_DEPTH]);
    uint256 unused;
    BOOST_CHECK(!modern::FinalityHistoryPath({}, 0, unused));
    const std::vector<uint256> with_null{leaves[0], uint256{}};
    BOOST_CHECK(!modern::FinalityHistoryRoot(with_null));
    BOOST_CHECK(!modern::FinalityHistoryPath(with_null, 0, unused));

    // Checkpoint index on the fresh grid (G = 261, interval 5).
    BOOST_CHECK_EQUAL(*modern::FinalityCheckpointIndexV2(261, 261, 5), 0U);
    BOOST_CHECK_EQUAL(*modern::FinalityCheckpointIndexV2(266, 261, 5), 1U);
    BOOST_CHECK_EQUAL(*modern::FinalityCheckpointIndexV2(311, 261, 5), 10U);
    BOOST_CHECK(!modern::FinalityCheckpointIndexV2(262, 261, 5));
    BOOST_CHECK(!modern::FinalityCheckpointIndexV2(256, 261, 5));
    BOOST_CHECK(!modern::FinalityCheckpointIndexV2(266, 261, 0));
    BOOST_CHECK_EQUAL(*modern::FinalityCheckpointIndexV2(int64_t{0xFFFFFFFF}, 0, 1), 0xFFFFFFFFU);
    BOOST_CHECK(!modern::FinalityCheckpointIndexV2(int64_t{1} << 32, 0, 1));
}

BOOST_AUTO_TEST_CASE(shared_vector_history_paths)
{
    // contracts/test/vectors/bridge_v2_vectors.json "history" (written by
    // contracts/test/vectors/gen_v2_vectors.sh), copied verbatim: the leaves
    // at 261 + 5i over stand-in block hashes, the root over leaves 0..k-1 for
    // every k, and all seven paths. A sibling above level 3 of these trees
    // (at most 12 leaves) is the zero subtree of its level; the one-leaf path
    // (index 0, count 1) lists exactly those, so each other path is pinned
    // as its four low siblings plus that shared tail. The paths are checked
    // against FinalityHistoryPath and folded from the JSON values themselves,
    // so a regenerated vectors file must be copied here again.
    const std::array<std::string_view, 12> block_hashes{
        "fa563b7b2b4bcc75cadc13fd8ea8f1a5b631700480730e20ffdb72941a6ee3f4", // 261
        "3eb15e38c1fe49793d90a4ed8a01f0cb39c2fd362fc0afc4d32041913bb6442e", // 266
        "5bc77028bff2710500506b584d72eb0b7ace2a6b7f259e21fa2faee06011c2f4", // 271
        "9acce4f0475155d779394c25c31eb5a7446e91a4d6bca9e630b9b80e22a1684f", // 276
        "73736192039f036c5dd8e98370ffe24f2effc3ccfa3b8f09026fc064d87a7b6d", // 281
        "2b296f0b926fa2db63d332bbd28fc85445f0ea7bf64e73ac174b86ab31785e79", // 286
        "7279a345a305d6e9b6c3ce1b77692c398d191c476dcd25bc73f8b30264dea964", // 291
        "87e91fe7737b012fab0967ba4574b3dbddaf2ee8a53b104a1f26c34d7f63dc60", // 296
        "c3cf63c20df794b427750c4b7675443cc29dda3e1feef23bf9800bc7f1d5cc99", // 301
        "e0b40c08d7041b3ceb68255187ef02effc75ee224200ba721aac95489b5fc0b4", // 306
        "501b43b36bcf95465ad96973918744260549166c3d9264647c1e78360d197479", // 311
        "83a5be6d241644877a3d9ef8563cc15c190fc5fff279e42098f642c2c74101a9", // 316
    };
    const std::array<std::string_view, 12> leaf_hex{
        "d40278962c11402f8468c48d9a5362c98aacd090e6efa9dac9bfe25789e8ba8f",
        "52aa35b9cdbf38c09c553c511b723c10e6f69fc843ca22d06de147a6db3e09a2",
        "85f41fe53f595b19c1d1dd26b567a73cc99fc27ef4df8ba3695310a130bcca82",
        "25ef36debab87d72522249a2a26ee82dfd5501caa5d151a4f5246961a123c67d",
        "6e7137da9566d0a3646614b86f8f73b55b942586d9312d1514895bf6a6c2d6e8",
        "6a802dfba61426bc12d24fdc6d0a20ac1b7732f2ed628d53b470d3300d7802cf",
        "2826fbc231c60fe8f304a855a2bbf873eee6ae84dccd8c9a904b0ab5d944b81a",
        "3c3dc4f4878cac4ddef4cee187703bcce4cb6109463cc7bc5519861183a943c1",
        "3a464516151b40271ced7f99754ef62fd37961820ac295cbb9e53b7288de4543",
        "03252ac02e3259762f9099f0f6675fcfed57a6448db49aaba69061267a32d7b0",
        "0f24ec23a15299bd62190ddebfdd5b5e33f29c2127a20d5f8c6763f5454d9d08",
        "0307f469d54ac5dbf67eb62cbbc71b4461e26d84d4d797ab7eff78a6c2d9c8ef",
    };
    const std::array<std::string_view, 13> roots{ // over leaves 0..k-1, k = 0..12
        "27ae5ba08d7291c96c8cbddcc148bf48a6d68c7974b94356f53754ef6171d757",
        "1243ccb653de3036ff1b508195e5b82a90ff7592a269e42cb347f901f11c4769",
        "d32e3390e4ba5238e590e92bd7e6d10899fb4bb7afcf472d1f5874d341299bd3",
        "b403ac366596de480d729bc8d28e588f197b5fd4b87d8f924132613e31d55407",
        "268d9e673b9ad74197e8e665127f2abe5178ce23a217f0a51f53b97bfc7db042",
        "7451bae10b1a72aca7a6809139c4d3ed1efce17f249f805986d1a7bf2d3bcebf",
        "2ce6b085cb3ac20d951f36291da78fb06b043d4853ef0ac84139436cc42f5cab",
        "1dd378b4d6b611846a7115a782636093b99288246b94507a488bea580f018cb4",
        "ae6d17b35f8ed842ae127b78533610bbf00f61ae282ebea5c55174bb8d334096",
        "6de9c4115fb28e10191ddb56f3bb3343fcc0bf5404e48c49de430887ef1246fd",
        "dfbf06841a6eca6e4a5f7e51cd92a0188ebe8ce075e2cb97a443a6cadb35557a",
        "6b93a456c4b9eeac87c4d4cc597a0536d2f658ea45e4f4d0db3b0a567afac2e5",
        "ced6d5522897163c841e016b6580c5d1cfeb2a01b126e898ab765f7fe6829046",
    };
    const std::array<std::string_view, 32> zero_tail{ // paths[0].siblings
        "0000000000000000000000000000000000000000000000000000000000000000",
        "ad3228b676f7d3cd4284a5443f17f1962b36e491b30a40b2405849e597ba5fb5",
        "b4c11951957c6f8f642c4af61cd6b24640fec6dc7fc607ee8206a99e92410d30",
        "21ddb9a356815c3fac1026b6dec5df3124afbadb485c9ba5a3e3398a04b7ba85",
        "e58769b32a1beaf1ea27375a44095a0d1fb664ce2dd358e7fcbfb78c26a19344",
        "0eb01ebfc9ed27500cd4dfc979272d1f0913cc9f66540d7e8005811109e1cf2d",
        "887c22bd8750d34016ac3c66b5ff102dacdd73f6b014e710b51e8022af9a1968",
        "ffd70157e48063fc33c97a050f7f640233bf646cc98d9524c6b92bcf3ab56f83",
        "9867cc5f7f196b93bae1e27e6320742445d290f2263827498b54fec539f756af",
        "cefad4e508c098b9a7e1d8feb19955fb02ba9675585078710969d3440f5054e0",
        "f9dc3e7fe016e050eff260334f18a5d4fe391d82092319f5964f2e2eb7c1c3a5",
        "f8b13a49e282f609c317a833fb8d976d11517c571d1221a265d25af778ecf892",
        "3490c6ceeb450aecdc82e28293031d10c7d73bf85e57bf041a97360aa2c5d99c",
        "c1df82d9c4b87413eae2ef048f94b4d3554cea73d92b0f7af96e0271c691e2bb",
        "5c67add7c6caf302256adedf7ab114da0acfe870d449a3a489f781d659e8becc",
        "da7bce9f4e8618b6bd2f4132ce798cdc7a60e7e1460a7299e3c6342a579626d2",
        "2733e50f526ec2fa19a22b31e8ed50f23cd1fdf94c9154ed3a7609a2f1ff981f",
        "e1d3b5c807b281e4683cc6d6315cf95b9ade8641defcb32372f1c126e398ef7a",
        "5a2dce0a8a7f68bb74560f8f71837c2c2ebbcbf7fffb42ae1896f13f7c7479a0",
        "b46a28b6f55540f89444f63de0378e3d121be09e06cc9ded1c20e65876d36aa0",
        "c65e9645644786b620e2dd2ad648ddfcbf4a7e5b1a3a4ecfe7f64667a3f0b7e2",
        "f4418588ed35a2458cffeb39b93d26f18d2ab13bdce6aee58e7b99359ec2dfd9",
        "5a9c16dc00d6ef18b7933a6f8dc65ccb55667138776f7dea101070dc8796e377",
        "4df84f40ae0c8229d0d6069e5c8f39a7c299677a09d367fc7b05e3bc380ee652",
        "cdc72595f74c7b1043d0e1ffbab734648c838dfb0527d971b602bc216c9619ef",
        "0abf5ac974a1ed57f4050aa510dd9c74f508277b39d7973bb2dfccc5eeb0618d",
        "b8cd74046ff337f0a7bf2c8e03e10f642c1886798d71806ab1e888d9e5ee87d0",
        "838c5655cb21c6cb83313b5a631175dff4963772cce9108188b34ac87c81c41e",
        "662ee4dd2dd7b2bc707961b1e646c4047669dcb6584f0d8d770daf5d7e7deb2e",
        "388ab20e2573d171a88108e79d820e98f26c0b84aa8b2f4aa4968dbb818ea322",
        "93237c50ba75ee485f4c22adf2f741400bdf8d6a9cc7df7ecae576221665d735",
        "8448818bb4ae4562849e949e17ac16e0be16688e156b5cf15e098c627c0056a9",
    };
    struct PathVector {
        uint64_t index;
        size_t count;
        std::string_view leaf;
        std::array<std::string_view, 4> low; // siblings at levels 0..3
        std::string_view root;
    };
    const std::array<PathVector, 7> paths{{
        {0, 1, "d40278962c11402f8468c48d9a5362c98aacd090e6efa9dac9bfe25789e8ba8f",
         {"0000000000000000000000000000000000000000000000000000000000000000",
          "ad3228b676f7d3cd4284a5443f17f1962b36e491b30a40b2405849e597ba5fb5",
          "b4c11951957c6f8f642c4af61cd6b24640fec6dc7fc607ee8206a99e92410d30",
          "21ddb9a356815c3fac1026b6dec5df3124afbadb485c9ba5a3e3398a04b7ba85"},
         "1243ccb653de3036ff1b508195e5b82a90ff7592a269e42cb347f901f11c4769"},
        {0, 8, "d40278962c11402f8468c48d9a5362c98aacd090e6efa9dac9bfe25789e8ba8f",
         {"52aa35b9cdbf38c09c553c511b723c10e6f69fc843ca22d06de147a6db3e09a2",
          "74c11514156c7681ccd9e8cd51ce586bba0be72d607c4b735b367ee2861dfe48",
          "837d8a257a8ca29a671f317644801839b3ae80066d718d01cdd78b6cddddfb8c",
          "21ddb9a356815c3fac1026b6dec5df3124afbadb485c9ba5a3e3398a04b7ba85"},
         "ae6d17b35f8ed842ae127b78533610bbf00f61ae282ebea5c55174bb8d334096"},
        {7, 8, "3c3dc4f4878cac4ddef4cee187703bcce4cb6109463cc7bc5519861183a943c1",
         {"2826fbc231c60fe8f304a855a2bbf873eee6ae84dccd8c9a904b0ab5d944b81a",
          "f0fc8192d6245f59089ae929287df721caebd641f9f345d8e218bf9a736807ff",
          "ae084d46275cc461f74402272b5a8991f8208926bffc6b6cbcf4d97d6406dc5a",
          "21ddb9a356815c3fac1026b6dec5df3124afbadb485c9ba5a3e3398a04b7ba85"},
         "ae6d17b35f8ed842ae127b78533610bbf00f61ae282ebea5c55174bb8d334096"},
        {0, 11, "d40278962c11402f8468c48d9a5362c98aacd090e6efa9dac9bfe25789e8ba8f",
         {"52aa35b9cdbf38c09c553c511b723c10e6f69fc843ca22d06de147a6db3e09a2",
          "74c11514156c7681ccd9e8cd51ce586bba0be72d607c4b735b367ee2861dfe48",
          "837d8a257a8ca29a671f317644801839b3ae80066d718d01cdd78b6cddddfb8c",
          "32567f1285ec9e4422f5175714b2fb922db68f40c444fc5380cb700fb2309fd4"},
         "6b93a456c4b9eeac87c4d4cc597a0536d2f658ea45e4f4d0db3b0a567afac2e5"},
        {5, 11, "6a802dfba61426bc12d24fdc6d0a20ac1b7732f2ed628d53b470d3300d7802cf",
         {"6e7137da9566d0a3646614b86f8f73b55b942586d9312d1514895bf6a6c2d6e8",
          "54f60c294b7435f6d563cef5a570662f21efc7853d8e2f5902b7f22d8ed44adc",
          "ae084d46275cc461f74402272b5a8991f8208926bffc6b6cbcf4d97d6406dc5a",
          "32567f1285ec9e4422f5175714b2fb922db68f40c444fc5380cb700fb2309fd4"},
         "6b93a456c4b9eeac87c4d4cc597a0536d2f658ea45e4f4d0db3b0a567afac2e5"},
        {10, 11, "0f24ec23a15299bd62190ddebfdd5b5e33f29c2127a20d5f8c6763f5454d9d08",
         {"0000000000000000000000000000000000000000000000000000000000000000",
          "bd53f02c0b202225196c0b7791b06536f732937f8539f743360e46376434582f",
          "b4c11951957c6f8f642c4af61cd6b24640fec6dc7fc607ee8206a99e92410d30",
          "1270198f5a57df92ed7b4fb486af5cd7dabe530ef58660f87c04e98c50f95b8b"},
         "6b93a456c4b9eeac87c4d4cc597a0536d2f658ea45e4f4d0db3b0a567afac2e5"},
        {11, 12, "0307f469d54ac5dbf67eb62cbbc71b4461e26d84d4d797ab7eff78a6c2d9c8ef",
         {"0f24ec23a15299bd62190ddebfdd5b5e33f29c2127a20d5f8c6763f5454d9d08",
          "bd53f02c0b202225196c0b7791b06536f732937f8539f743360e46376434582f",
          "b4c11951957c6f8f642c4af61cd6b24640fec6dc7fc607ee8206a99e92410d30",
          "1270198f5a57df92ed7b4fb486af5cd7dabe530ef58660f87c04e98c50f95b8b"},
         "ced6d5522897163c841e016b6580c5d1cfeb2a01b126e898ab765f7fe6829046"},
    }};

    std::vector<uint256> leaves;
    for (size_t i{0}; i < block_hashes.size(); ++i) {
        leaves.push_back(modern::FinalityHistoryLeaf(261 + 5 * i, Evm(block_hashes[i])));
        BOOST_CHECK_EQUAL(EvmHex(leaves.back()), leaf_hex[i]);
    }
    modern::FinalityHistoryState accumulator;
    for (size_t k{0}; k < roots.size(); ++k) {
        const auto root{modern::FinalityHistoryRoot(std::span{leaves}.first(k))};
        BOOST_REQUIRE(root);
        BOOST_CHECK_EQUAL(EvmHex(*root), roots[k]);
        if (k > 0) {
            BOOST_REQUIRE(modern::AppendFinalityHistoryLeaf(accumulator, leaves[k - 1]));
            BOOST_CHECK_EQUAL(EvmHex(accumulator.root), roots[k]);
        }
    }
    for (const PathVector& v : paths) {
        BOOST_TEST_CONTEXT("history path index " << v.index << " count " << v.count)
        {
            BOOST_CHECK_EQUAL(EvmHex(leaves[v.index]), v.leaf);
            BOOST_CHECK_EQUAL(roots[v.count], v.root);
            modern::FinalityHistoryPathV2 expected;
            for (size_t d{0}; d < expected.size(); ++d) expected[d] = Evm(d < v.low.size() ? v.low[d] : zero_tail[d]);
            uint256 path_root;
            const auto path{modern::FinalityHistoryPath(std::span{leaves}.first(v.count), v.index, path_root)};
            BOOST_REQUIRE(path);
            BOOST_CHECK_EQUAL(EvmHex(path_root), v.root);
            for (size_t d{0}; d < expected.size(); ++d) BOOST_CHECK_EQUAL(EvmHex((*path)[d]), EvmHex(expected[d]));
            BOOST_CHECK_EQUAL(EvmHex(modern::FoldFinalityHistoryPath(Evm(v.leaf), v.index, expected)), v.root);
        }
    }
}

BOOST_AUTO_TEST_CASE(decode_and_payload_rules)
{
    // (d) FinalizedBlockV2 round trip and strict decoding.
    FinalizedBlockV2 fb{FbV2(Fill(0x3c), 4)};
    fb.withdrawal_root = Fill(0x9d);
    fb.checkpoint_index = 0xFFFFFFFF;
    fb.height = 0x0102030405060708ULL;
    const auto enc{fb.Encode()};
    const auto back{FinalizedBlockV2::Decode(enc)};
    BOOST_REQUIRE(back);
    BOOST_CHECK(*back == fb);
    BOOST_CHECK_EQUAL(back->height, 0x0102030405060708ULL);
    BOOST_CHECK_EQUAL(back->checkpoint_index, 0xFFFFFFFFU);
    BOOST_CHECK(back->withdrawal_root == Fill(0x9d));
    std::vector<unsigned char> buf(enc.begin(), enc.end());
    BOOST_CHECK(!FinalizedBlockV2::Decode(std::span{buf}.first(184)));
    buf.push_back(0);
    BOOST_CHECK(!FinalizedBlockV2::Decode(buf));
    buf.pop_back();
    for (const unsigned char flags : {0x02, 0x80, 0x03, 0xFF}) {
        buf.back() = flags;
        BOOST_CHECK(!FinalizedBlockV2::Decode(buf));
    }
    buf.back() = modern::FINALITY_BRIDGE_FLAG_DEPOSITS_CLOSED;
    const auto closed{FinalizedBlockV2::Decode(buf)};
    BOOST_REQUIRE(closed);
    BOOST_CHECK_EQUAL(closed->bridge_flags, 0x01);
    BOOST_CHECK(!(*closed == fb));

    // Payload V2: fb(185) || bitmap(ceil(n/8)) || sig(96).
    FinalityCertificate cert;
    cert.signer_bitmap = {0x0b}; // members 0, 1, 3 of n = 4
    cert.aggregate_sig.fill(0xa5);
    const auto payload{modern::EncodeCertificatePayloadV2(fb, cert)};
    BOOST_CHECK_EQUAL(payload.size(), 185U + 1 + 96);
    const auto decoded{modern::DecodeCertificatePayloadV2(payload, 4)};
    BOOST_REQUIRE(decoded);
    BOOST_CHECK(decoded->first == fb);
    BOOST_CHECK(decoded->second.signer_bitmap == cert.signer_bitmap);
    BOOST_CHECK(decoded->second.aggregate_sig == cert.aggregate_sig);
    BOOST_CHECK(!modern::DecodeCertificatePayloadV2(payload, 9));  // wrong width
    BOOST_CHECK(!modern::DecodeCertificatePayloadV2(payload, 3));  // bit 3 >= n
    BOOST_CHECK(!modern::DecodeCertificatePayloadV2(payload, 0));
    BOOST_CHECK(!modern::DecodeCertificatePayloadV2(std::span{payload}.first(payload.size() - 1), 4));
    auto bad_flags{payload};
    bad_flags[184] = 0x02;
    BOOST_CHECK(!modern::DecodeCertificatePayloadV2(bad_flags, 4));
    // A V1 payload of the same set is not a V2 payload.
    FinalizedBlock v1;
    v1.height = fb.height;
    BOOST_CHECK(!modern::DecodeCertificatePayloadV2(modern::EncodeCertificatePayload(v1, cert), 4));
    // Maximum set: 8,192 members -> 1,305 bytes, the record maximum.
    FinalityCertificate big;
    big.signer_bitmap.assign(modern::MAX_FINALITY_SET / 8, 0xFF);
    const auto max_payload{modern::EncodeCertificatePayloadV2(fb, big)};
    BOOST_CHECK_EQUAL(max_payload.size(), modern::FINALITY_CERTIFICATE_V2_RECORD_MAX);
    BOOST_CHECK_EQUAL(modern::FINALITY_CERTIFICATE_V2_RECORD_MAX, 1305U);
    BOOST_CHECK(modern::DecodeCertificatePayloadV2(max_payload, modern::MAX_FINALITY_SET));
    BOOST_CHECK(!modern::DecodeCertificatePayloadV2(max_payload, modern::MAX_FINALITY_SET + 1));
}

BOOST_AUTO_TEST_CASE(ruleset_v2_header_is_distinct)
{
    // (e) Same members, ruleset 2: a different set hash from ruleset 1, and
    // only epoch/ruleset change.
    const SetUnderTest set{{3, 3, 3, 1}};
    const ValidatorSetHeader& v1{set.snapshot.Header()};
    BOOST_CHECK_EQUAL(v1.ruleset_version, modern::FINALITY_RULESET_V1);
    const ValidatorSetHeader v2{modern::RulesetV2Header(v1, v1.epoch)};
    BOOST_CHECK_EQUAL(v2.ruleset_version, modern::FINALITY_RULESET_V2);
    BOOST_CHECK(modern::ValidatorSetHash(v2) != set.snapshot.SetHash());
    BOOST_CHECK(modern::ValidatorSetHash(v2) != modern::ValidatorSetHash(v1));
    BOOST_CHECK_EQUAL(v2.epoch, v1.epoch);
    BOOST_CHECK_EQUAL(v2.validator_count, v1.validator_count);
    BOOST_CHECK_EQUAL(v2.total_weight, v1.total_weight);
    BOOST_CHECK_EQUAL(v2.quorum_weight, v1.quorum_weight);
    BOOST_CHECK(v2.aggregate_pubkey == v1.aggregate_pubkey);
    BOOST_CHECK(v2.members_root == v1.members_root);
    const auto v1_enc{v1.Encode()};
    const auto v2_enc{v2.Encode()};
    BOOST_CHECK(std::equal(v1_enc.begin(), v1_enc.begin() + 8, v2_enc.begin()));
    BOOST_CHECK(std::equal(v1_enc.begin() + 10, v1_enc.end(), v2_enc.begin() + 10));
    // Epochs re-stamp the hash; a V1 header never equals any V2 header.
    BOOST_CHECK(set.V2Hash(0) != set.V2Hash(1));
    for (uint64_t e{0}; e < 4; ++e) {
        BOOST_CHECK(set.V2Hash(e) != modern::ValidatorSetHash(
                                         [&] { ValidatorSetHeader h{v1}; h.epoch = e; return h; }()));
    }
}

BOOST_AUTO_TEST_CASE(verify_certificate_v2)
{
    // (f) weights 3,3,3,1 -> W = 10, quorum = 7, headcount floor(8/3)+1 = 3.
    const SetUnderTest set{{3, 3, 3, 1}};
    BOOST_CHECK_EQUAL(set.view.quorum_weight, 7U);
    const FinalizedBlockV2 fb{FbV2(set.V2Hash(1))};
    const auto all{SignV2(set, fb, {0, 1, 2, 3})};
    BOOST_CHECK(VerifyV2(set, fb, all) == CertificateCheck::OK);

    std::vector<uint32_t> heavy, light;
    for (uint32_t i = 0; i < 4; ++i) (set.view.weights[i] == 3 ? heavy : light).push_back(i);
    BOOST_REQUIRE_EQUAL(heavy.size(), 3U);
    BOOST_CHECK(VerifyV2(set, fb, SignV2(set, fb, heavy)) == CertificateCheck::OK);
    BOOST_CHECK(VerifyV2(set, fb, SignV2(set, fb, {heavy[0], heavy[1], light[0]})) == CertificateCheck::OK);
    BOOST_CHECK(VerifyV2(set, fb, SignV2(set, fb, {heavy[0], heavy[1]})) == CertificateCheck::INSUFFICIENT_WEIGHT);
    BOOST_CHECK(VerifyV2(set, fb, SignV2(set, fb, light)) == CertificateCheck::INSUFFICIENT_WEIGHT);
    BOOST_CHECK(VerifyV2(set, fb, SignV2(set, fb, {})) == CertificateCheck::NO_SIGNERS);
    // Weight quorum without the headcount quorum.
    {
        const SetUnderTest concentrated{{4, 4, 1, 1}};
        std::vector<uint32_t> weight_four;
        for (uint32_t i = 0; i < 4; ++i) {
            if (concentrated.view.weights[i] == 4) weight_four.push_back(i);
        }
        BOOST_REQUIRE_EQUAL(weight_four.size(), 2U);
        const FinalizedBlockV2 cfb{FbV2(concentrated.V2Hash(1))};
        BOOST_CHECK(VerifyV2(concentrated, cfb, SignV2(concentrated, cfb, weight_four)) ==
                    CertificateCheck::INSUFFICIENT_HEADCOUNT);
    }

    // A V1-tagged signature over the analogous V1 object is valid under V1
    // and BAD_SIGNATURE under V2.
    {
        FinalizedBlock analogous;
        analogous.height = fb.height;
        analogous.block_hash = fb.block_hash;
        analogous.withdrawal_root = fb.withdrawal_root;
        analogous.validator_set_hash = fb.validator_set_hash;
        analogous.epoch = fb.epoch;
        const auto v1_cert{SignDigest(set, modern::FinalityDigest(CHAIN_DOMAIN, analogous), {0, 1, 2, 3})};
        BOOST_CHECK(modern::VerifyFinalityCertificate(CHAIN_DOMAIN, analogous, v1_cert, set.view, fb.validator_set_hash) ==
                    CertificateCheck::OK);
        BOOST_CHECK(VerifyV2(set, fb, v1_cert) == CertificateCheck::BAD_SIGNATURE);
    }
    // A ruleset-1 successor hash (correctly V2-signed) is the wrong successor.
    {
        ValidatorSetHeader rs1{set.snapshot.Header()};
        rs1.epoch = 1;
        FinalizedBlockV2 other{fb};
        other.validator_set_hash = modern::ValidatorSetHash(rs1);
        const auto c{SignV2(set, other, {0, 1, 2, 3})};
        BOOST_CHECK(VerifyV2(set, other, c) == CertificateCheck::WRONG_SUCCESSOR_SET);
        // The successor must be the ruleset-2 header of epoch + 1, not of epoch.
        FinalizedBlockV2 same_epoch{fb};
        same_epoch.validator_set_hash = set.V2Hash(0);
        BOOST_CHECK(VerifyV2(set, same_epoch, SignV2(set, same_epoch, {0, 1, 2, 3})) ==
                    CertificateCheck::WRONG_SUCCESSOR_SET);
    }
    // Wrong chain domain.
    {
        const auto c{SignV2(set, fb, {0, 1, 2, 3}, OTHER_CHAIN_DOMAIN)};
        BOOST_CHECK(VerifyV2(set, fb, c) == CertificateCheck::BAD_SIGNATURE);
        BOOST_CHECK(VerifyV2(set, fb, c, OTHER_CHAIN_DOMAIN) == CertificateCheck::OK);
    }
    // Every V2-only field is signed: changing it after signing breaks the signature.
    {
        auto check_mutation{[&](auto mutate) {
            FinalizedBlockV2 m{fb};
            mutate(m);
            BOOST_CHECK(VerifyV2(set, m, all) == CertificateCheck::BAD_SIGNATURE);
        }};
        check_mutation([](FinalizedBlockV2& m) { m.lineage_id = Fill(0x4d); });
        check_mutation([](FinalizedBlockV2& m) { m.checkpoint_index = 2; });
        check_mutation([](FinalizedBlockV2& m) { m.checkpoint_history_root = Fill(0x7b); });
        check_mutation([](FinalizedBlockV2& m) { m.bridge_flags = modern::FINALITY_BRIDGE_FLAG_DEPOSITS_CLOSED; });
        check_mutation([](FinalizedBlockV2& m) { m.withdrawal_root = Fill(0x01); });
        check_mutation([](FinalizedBlockV2& m) { m.height = 271; });
        check_mutation([](FinalizedBlockV2& m) { m.block_hash = Fill(0x02); });
    }
    // Malformed bitmaps, mismatched claims, tampered aggregate, inconsistent view.
    {
        auto c{all};
        c.signer_bitmap.push_back(0x00);
        BOOST_CHECK(VerifyV2(set, fb, c) == CertificateCheck::MALFORMED_BITMAP);
        auto d{all};
        d.signer_bitmap[0] |= 0x10;
        BOOST_CHECK(VerifyV2(set, fb, d) == CertificateCheck::MALFORMED_BITMAP);
        auto e{SignV2(set, fb, {0, 1, 2})};
        e.signer_bitmap[0] |= 0x08; // claims member 3 without its signature
        BOOST_CHECK(VerifyV2(set, fb, e) == CertificateCheck::BAD_SIGNATURE);
        auto f{all};
        f.aggregate_sig[20] ^= 0x01;
        BOOST_CHECK(VerifyV2(set, fb, f) == CertificateCheck::BAD_SIGNATURE);
        auto g{all};
        g.aggregate_sig.fill(0);
        g.aggregate_sig[0] = 0xc0; // infinity
        BOOST_CHECK(VerifyV2(set, fb, g) == CertificateCheck::BAD_SIGNATURE);
        ValidatorSetView broken{set.view};
        broken.weights.pop_back();
        BOOST_CHECK(modern::VerifyFinalityCertificateV2(CHAIN_DOMAIN, fb, all, broken, set.V2Hash(1)) ==
                    CertificateCheck::SET_VIEW_INCONSISTENT);
    }
    // The decoded carrier payload verifies like the in-memory certificate.
    {
        const auto payload{modern::EncodeCertificatePayloadV2(fb, all)};
        const auto decoded{modern::DecodeCertificatePayloadV2(payload, set.view.validator_count)};
        BOOST_REQUIRE(decoded);
        BOOST_CHECK(VerifyV2(set, decoded->first, decoded->second) == CertificateCheck::OK);
        BOOST_CHECK(modern::FinalityCertIdV2(decoded->first) == modern::FinalityCertIdV2(fb));
        // certId is the same for another aggregation (three heavy signers) of one statement.
        const auto other_payload{modern::EncodeCertificatePayloadV2(fb, SignV2(set, fb, heavy))};
        BOOST_CHECK(other_payload != payload);
        BOOST_CHECK(modern::FinalityCertIdV2(modern::DecodeCertificatePayloadV2(other_payload, 4)->first) ==
                    modern::FinalityCertIdV2(fb));
    }
}

// ------------------------------------------------------- type-11 carrier
// MPA type 11 FINALITY_CERTIFICATE_V2: the fresh lineage's coinbase-only
// carrier with no metadata cell. Recognition never implies activation: the
// type is INACTIVE unless Params::fresh_lineage is configured (regtest knob
// only) and the height has reached R_f.

BOOST_AUTO_TEST_CASE(carrier_registry_type_11)
{
    using modern::PayloadTypeStatus;
    constexpr uint16_t T11{modern::MPA_TYPE_FINALITY_CERTIFICATE_V2};
    BOOST_CHECK_EQUAL(T11, 11);
    BOOST_CHECK_EQUAL(modern::CREATION_ACTION_FINALITY_CERTIFICATE_V2, 11);
    // An MPA record, never a standalone creation action.
    BOOST_CHECK(!modern::IsKnownCreationAction(T11, modern::MPA_VERSION_V1));
    BOOST_CHECK(!modern::EncodeCreationActionSection({modern::CreationAction{T11, 1, {0x00}}}));

    // ---- activation gate (the mpa_tests X-pin context)
    Consensus::Params production{};
    production.legacy_b3coin = true;
    Consensus::Params xpin{production};
    xpin.hard_fork_height = 100;
    xpin.legacy_final_hash = uint256::ONE;
    xpin.modern_pos = Consensus::ModernPosParams{};
    BOOST_REQUIRE(Consensus::ModernObjectRulesActive(xpin));
    constexpr int R_F{172};
    Consensus::Params fresh{xpin};
    fresh.fresh_lineage = Consensus::FreshLineageParams{
        .activation_height = R_F, .max_epoch_extension = 1280, .staging_signer = true};
    Consensus::Params fresh_without_xpin{production};
    fresh_without_xpin.fresh_lineage = fresh.fresh_lineage;
    const auto status{[](const uint16_t version, const Consensus::Params& p, const std::optional<int> h) {
        return modern::GetPayloadTypeStatus(T11, version, p, h);
    }};
    for (const int h : {0, R_F - 1, R_F, R_F + 1, 1'000'000, std::numeric_limits<int>::max()}) {
        BOOST_CHECK(status(1, production, h) == PayloadTypeStatus::INACTIVE);
        BOOST_CHECK(status(1, xpin, h) == PayloadTypeStatus::INACTIVE);
        BOOST_CHECK(status(1, fresh_without_xpin, h) == PayloadTypeStatus::INACTIVE);
        BOOST_CHECK(status(2, fresh, h) == PayloadTypeStatus::UNKNOWN);
        BOOST_CHECK(status(0, fresh, h) == PayloadTypeStatus::UNKNOWN);
    }
    BOOST_CHECK(status(1, fresh, 0) == PayloadTypeStatus::INACTIVE);
    BOOST_CHECK(status(1, fresh, R_F - 1) == PayloadTypeStatus::INACTIVE);
    BOOST_CHECK(status(1, fresh, R_F) == PayloadTypeStatus::ACTIVE);
    BOOST_CHECK(status(1, fresh, R_F + 1) == PayloadTypeStatus::ACTIVE);
    BOOST_CHECK(status(1, fresh, std::numeric_limits<int>::max()) == PayloadTypeStatus::ACTIVE);
    BOOST_CHECK(status(1, fresh, std::nullopt) == PayloadTypeStatus::INACTIVE); // no height: fail closed
    // Neighbours are unaffected: type 4 stays X-pin gated, 12 stays unknown.
    BOOST_CHECK(modern::GetPayloadTypeStatus(4, 1, fresh, R_F - 1) == PayloadTypeStatus::ACTIVE);
    BOOST_CHECK(modern::GetPayloadTypeStatus(4, 1, xpin) == PayloadTypeStatus::ACTIVE);
    BOOST_CHECK(modern::GetPayloadTypeStatus(12, 1, fresh, R_F) == PayloadTypeStatus::UNKNOWN);

    // Shipped networks configure no fresh lineage: type 11 is INACTIVE at
    // every height, including on mainnet where the X-pin types 4/5 are live.
    for (const ChainType chain : {ChainType::MAIN, ChainType::TESTNET, ChainType::TESTNET4,
                                  ChainType::SIGNET, ChainType::REGTEST}) {
        const auto params{CreateChainParams(ArgsManager{}, chain)};
        const Consensus::Params& c{params->GetConsensus()};
        BOOST_CHECK(!c.fresh_lineage);
        for (const int h : {0, 810'001, 811'001, 820'001, 10'000'000, std::numeric_limits<int>::max()}) {
            BOOST_CHECK(modern::GetPayloadTypeStatus(T11, 1, c, h) == PayloadTypeStatus::INACTIVE);
        }
    }
    BOOST_CHECK(Consensus::ModernObjectRulesActive(CreateChainParams(ArgsManager{}, ChainType::MAIN)->GetConsensus()));
    // The real FlowMesh regtest: inactive without the knob, gated at R_f with it.
    {
        ArgsManager args;
        args.ForceSetArg("-b3modernregtest", "1");
        args.ForceSetArg("-b3flowmeshtest", "1");
        const auto plain{CreateChainParams(args, ChainType::REGTEST)};
        BOOST_REQUIRE(Consensus::ModernObjectRulesActive(plain->GetConsensus()));
        for (const int h : {R_F - 1, R_F, 10'000}) {
            BOOST_CHECK(modern::GetPayloadTypeStatus(T11, 1, plain->GetConsensus(), h) == PayloadTypeStatus::INACTIVE);
        }
        args.ForceSetArg("-b3freshlineageheight", "172");
        const auto knob{CreateChainParams(args, ChainType::REGTEST)};
        BOOST_REQUIRE(knob->GetConsensus().fresh_lineage);
        BOOST_CHECK_EQUAL(knob->GetConsensus().fresh_lineage->activation_height, R_F);
        BOOST_CHECK(modern::GetPayloadTypeStatus(T11, 1, knob->GetConsensus(), R_F - 1) == PayloadTypeStatus::INACTIVE);
        BOOST_CHECK(modern::GetPayloadTypeStatus(T11, 1, knob->GetConsensus(), R_F) == PayloadTypeStatus::ACTIVE);
        BOOST_CHECK(modern::GetPayloadTypeStatus(T11, 2, knob->GetConsensus(), R_F) == PayloadTypeStatus::UNKNOWN);
    }

    // ---- size rule: 185 + ceil(n/8) + 96 for some n in 1..8192
    BOOST_CHECK_EQUAL(modern::FINALITY_CERTIFICATE_V2_RECORD_MIN, 282U);
    BOOST_CHECK_EQUAL(modern::FINALITY_CERTIFICATE_V2_RECORD_MAX, 1305U);
    for (const size_t n : {size_t{0}, size_t{1}, size_t{96}, size_t{185}, size_t{281}, size_t{1306}, size_t{1307},
                           size_t{4000}, MAX_PAYLOAD_RECORD_SIZE}) {
        BOOST_CHECK_MESSAGE(!modern::PayloadSizeAllowed(T11, n), "size " << n);
    }
    for (const size_t n : {size_t{282}, size_t{283}, size_t{297}, size_t{1232}, size_t{1304}, size_t{1305}}) {
        BOOST_CHECK_MESSAGE(modern::PayloadSizeAllowed(T11, n), "size " << n);
    }
    for (const size_t members : {size_t{1}, size_t{4}, size_t{8}, size_t{9}, size_t{64}, size_t{4097},
                                 size_t{modern::MAX_FINALITY_SET}}) {
        BOOST_CHECK(modern::PayloadSizeAllowed(
            T11, FinalizedBlockV2::SIZE + modern::SignerBitmapBytes(members) + modern::BLS_SIGNATURE_SIZE));
    }
    // The V1 type-4 rule is unchanged.
    BOOST_CHECK(modern::PayloadSizeAllowed(modern::MPA_TYPE_FINALITY_CERTIFICATE, 1232));
    BOOST_CHECK(!modern::PayloadSizeAllowed(modern::MPA_TYPE_FINALITY_CERTIFICATE, 1233));

    // ---- declared cost: one FastAggregateVerify, like type 4
    BOOST_CHECK_EQUAL(modern::PayloadRecordVerifyCost(T11, 1), 2000);
    BOOST_CHECK_EQUAL(modern::PayloadRecordVerifyCost(T11, 1), modern::FINALITY_CERTIFICATE_VERIFY_COST);
    BOOST_CHECK_EQUAL(modern::PayloadRecordVerifyCost(T11, 2), 0);
    BOOST_CHECK_EQUAL(modern::PayloadRecordVerifyCost(modern::MPA_TYPE_FINALITY_CERTIFICATE, 1), 2000);

    // ---- a coinbase carrying one V1 and one V2 certificate record
    FinalityCertificate cert;
    cert.signer_bitmap = {0x0b}; // members 0, 1, 3 of n = 4
    cert.aggregate_sig.fill(0xa5);
    CMpaRecord v2rec;
    v2rec.payload_type = T11;
    v2rec.payload_version = modern::MPA_VERSION_V1;
    v2rec.payload = modern::EncodeCertificatePayloadV2(FbV2(Fill(0x3c)), cert);
    BOOST_REQUIRE_EQUAL(v2rec.payload.size(), modern::FINALITY_CERTIFICATE_V2_RECORD_MIN);
    CMpaRecord v1rec;
    v1rec.payload_type = modern::MPA_TYPE_FINALITY_CERTIFICATE;
    v1rec.payload_version = modern::MPA_VERSION_V1;
    v1rec.payload = modern::EncodeCertificatePayload(FinalizedBlock{}, cert);
    CMutableTransaction coinbase;
    coinbase.version = 2;
    coinbase.vin.resize(1);
    coinbase.vout.emplace_back(0, CScript() << OP_TRUE);
    coinbase.mpa = {v1rec, v2rec}; // canonical order: type 4 before type 11
    std::string err;
    BOOST_CHECK(modern::CheckTransactionMpa(CTransaction{coinbase}, fresh, R_F, err));
    BOOST_CHECK_EQUAL(modern::PayloadVerifyCost(CTransaction{coinbase}), 4000);
    BOOST_CHECK_LE(modern::PayloadVerifyCost(CTransaction{coinbase}), MAX_TX_PAYLOAD_COST);
    BOOST_CHECK(!modern::CheckTransactionMpa(CTransaction{coinbase}, fresh, R_F - 1, err));
    BOOST_CHECK_EQUAL(err, "mpa-inactive-type");
    BOOST_CHECK(!modern::CheckTransactionMpa(CTransaction{coinbase}, xpin, R_F, err));
    BOOST_CHECK_EQUAL(err, "mpa-inactive-type");
    BOOST_CHECK(!modern::CheckTransactionMpa(CTransaction{coinbase}, fresh, err)); // context-free caller
    BOOST_CHECK_EQUAL(err, "mpa-inactive-type");
    {
        CMutableTransaction swapped{coinbase};
        std::swap(swapped.mpa[0], swapped.mpa[1]);
        BOOST_CHECK(!modern::CheckTransactionMpa(CTransaction{swapped}, fresh, R_F, err));
        BOOST_CHECK_EQUAL(err, "mpa-record-order");
    }
    for (const size_t bad_size : {size_t{281}, size_t{1306}}) {
        CMutableTransaction resized{coinbase};
        resized.mpa[1].payload.resize(bad_size, 0x00);
        BOOST_CHECK(!modern::CheckTransactionMpa(CTransaction{resized}, fresh, R_F, err));
        BOOST_CHECK_EQUAL(err, "mpa-bad-record-size");
    }
    {
        CMutableTransaction versioned{coinbase};
        versioned.mpa[1].payload_version = 2;
        BOOST_CHECK(!modern::CheckTransactionMpa(CTransaction{versioned}, fresh, R_F, err));
        BOOST_CHECK_EQUAL(err, "mpa-unknown-type");
    }

    // ---- placement: coinbase-only, no metadata cell to place
    CBlock block;
    block.vtx.push_back(MakeTransactionRef(coinbase));
    BOOST_CHECK(modern::CheckFinalityCertificatePlacement(block, err));
    BOOST_CHECK(modern::CheckBlockPayloadCost(block, err));
    CMutableTransaction spend;
    spend.version = 2;
    spend.vin.resize(1);
    spend.vout.emplace_back(1, CScript() << OP_TRUE);
    spend.mpa = {v2rec};
    block.vtx.push_back(MakeTransactionRef(spend));
    BOOST_CHECK(!modern::CheckFinalityCertificatePlacement(block, err));
    BOOST_CHECK_EQUAL(err, "fresh-cert-record-not-in-coinbase");
    // Placement ignores the version, as V1's does: any type-11 frame is refused.
    spend.mpa[0].payload_version = 2;
    block.vtx[1] = MakeTransactionRef(spend);
    BOOST_CHECK(!modern::CheckFinalityCertificatePlacement(block, err));
    BOOST_CHECK_EQUAL(err, "fresh-cert-record-not-in-coinbase");
    // With a V1 record in the same transaction, the V1 rule reports first.
    spend.mpa = {v1rec, v2rec};
    block.vtx[1] = MakeTransactionRef(spend);
    BOOST_CHECK(!modern::CheckFinalityCertificatePlacement(block, err));
    BOOST_CHECK_EQUAL(err, "finality-cert-record-not-in-coinbase");
    // A later non-coinbase transaction is checked too.
    spend.mpa.clear();
    block.vtx[1] = MakeTransactionRef(spend);
    spend.mpa = {v2rec};
    spend.vout[0].nValue = 2;
    block.vtx.push_back(MakeTransactionRef(spend));
    BOOST_CHECK(!modern::CheckFinalityCertificatePlacement(block, err));
    BOOST_CHECK_EQUAL(err, "fresh-cert-record-not-in-coinbase");
    block.vtx.pop_back();
    BOOST_CHECK(modern::CheckFinalityCertificatePlacement(block, err));
}

BOOST_AUTO_TEST_SUITE_END()
