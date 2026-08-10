// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! Immutable regression vectors freezing B3's legacy chain identity and the
//! era/codec selection rules (doc/design/b3-architecture-contract.md).
//! Every literal in this file is frozen: a change to any of these values
//! means consensus identity has been altered. Never regenerate them.

#include <consensus/block_codec.h>
#include <consensus/era.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <legacy/codec.h>
#include <legacy/primitives.h>
#include <primitives/block.h>
#include <script/script.h>
#include <streams.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>

BOOST_AUTO_TEST_SUITE(legacy_identity_tests)

namespace {

//! Historical mainnet identity, recorded from the original client.
const std::string GENESIS_HASH{"4b0d7f133c5267d715d4d8992635a5490d1edd6b7072cce3f8fe116aba983b6a"};
const std::string GENESIS_MERKLE{"4243fd570d4cb2e2930767f5bf18b2f65f1b7c4e16a392552d1efadeec00753d"};

//! Exact serialized bytes of the genesis coinbase in the legacy encoding.
const std::string GENESIS_TX_HEX{
    "010000001b735058010000000000000000000000000000000000000000000000000000"
    "000000000000ffffffff5300012a4c4e4368696e61206c61756e636865732047616f66"
    "656e2d3320537461656c6c69746520746f2067657420616363757261746520696d6167"
    "6573206f66206561727468206f6e2031312d617567757374ffffffff01"
    "00000000000000000000000000"};

//! Exact serialized bytes of the entire genesis block in the legacy
//! encoding: 80-byte header, one coinbase, empty trailing block signature.
const std::string GENESIS_BLOCK_HEX{"0100000000000000000000000000000000000000000000000000000000000000000000"
    "003d7500ecdefa1e2d5592a3164e7c1b5ff6b218bff5670793e2b24c0d57fd43421b73"
    "5058ffff0f1e3b9f070001010000001b73505801000000000000000000000000000000"
    "0000000000000000000000000000000000ffffffff5300012a4c4e4368696e61206c61"
    "756e636865732047616f66656e2d3320537461656c6c69746520746f20676574206163"
    "63757261746520696d61676573206f66206561727468206f6e2031312d617567757374"
    "ffffffff010000000000000000000000000000"};

//! Synthetic legacy transaction with a non-zero nTime, frozen forever.
CMutableTransaction LegacyTxVector()
{
    CMutableTransaction mtx;
    mtx.version = 1;
    mtx.nTime = 0x5A5A5A5A;
    mtx.m_legacy_encoding = true;
    mtx.vin.resize(1);
    mtx.vin[0].prevout = COutPoint{
        Txid::FromUint256(uint256{"00000000000000000000000000000000000000000000000000000000000000b3"}), 1};
    mtx.vin[0].scriptSig = CScript() << OP_1;
    mtx.vin[0].nSequence = 0xFFFFFFFE;
    mtx.vout.emplace_back(1'234'567, CScript() << OP_TRUE);
    mtx.nLockTime = 7;
    return mtx;
}

const std::string LEGACY_TX_HEX{"010000005a5a5a5a01b300000000000000000000000000000000000000000000000000"
    "000000000000010000000151feffffff0187d6120000000000015107000000"};
const std::string LEGACY_TXID{"01ce3ea25423eb88dbb4525da9f53ef12add15ca744c80e26f98837ab3c77627"};

//! Synthetic legacy block with a trailing block signature, frozen forever.
CBlock LegacyBlockVector()
{
    CBlock block;
    block.nVersion = 4;
    block.hashPrevBlock = uint256{"00000000000000000000000000000000000000000000000000000000000000aa"};
    block.nTime = 1'500'000'000;
    block.nBits = 0x1e0fffff;
    block.nNonce = 42;
    block.vtx.push_back(MakeTransactionRef(LegacyTxVector()));
    block.hashMerkleRoot = BlockMerkleRoot(block);
    block.vchBlockSig = {0xde, 0xad, 0xbe, 0xef};
    return block;
}

const std::string LEGACY_BLOCK_HEX{"04000000aa000000000000000000000000000000000000000000000000000000000000"
    "002776c7b37a83986fe2804c74ca15dd2af13ef5a95d52b4db88eb2354a23ece01002f"
    "6859ffff0f1e2a00000001010000005a5a5a5a01b30000000000000000000000000000"
    "0000000000000000000000000000000000010000000151feffffff0187d61200000000"
    "0001510700000004deadbeef"};
const std::string LEGACY_BLOCK_HASH{"09adcb797942622a52580be2369d9dcf53f3e9e98db034e1551a4bb2173021c8"};

//! Synthetic marker-modern block: stock Core body, no nTime, no signature.
CBlock ModernBlockVector()
{
    CMutableTransaction mtx;
    mtx.version = 2;
    mtx.vin.resize(1);
    mtx.vin[0].prevout = COutPoint{
        Txid::FromUint256(uint256{"00000000000000000000000000000000000000000000000000000000000000cc"}), 0};
    mtx.vin[0].scriptSig = CScript() << OP_1;
    mtx.vout.emplace_back(9'999, CScript() << OP_TRUE);

    CBlock block;
    block.nVersion = static_cast<int32_t>(Consensus::B3_BLOCK_CODEC_V2_VERSION);
    block.hashPrevBlock = uint256{"00000000000000000000000000000000000000000000000000000000000000bb"};
    block.nTime = 1'900'000'000;
    block.nBits = 0x1d00ffff;
    block.nNonce = 7;
    block.vtx.push_back(MakeTransactionRef(std::move(mtx)));
    block.hashMerkleRoot = BlockMerkleRoot(block);
    return block;
}

const std::string MODERN_BLOCK_HEX{"00000028bb000000000000000000000000000000000000000000000000000000000000"
    "00ef7ee218844e99119f450a69dfd2ca7168eafaeece8fa2582ac233088399da6600b3"
    "3f71ffff001d07000000010200000001cc000000000000000000000000000000000000"
    "00000000000000000000000000000000000151ffffffff010f27000000000000015100"
    "000000"};
const std::string MODERN_BLOCK_HASH{"067ba8182ca5c7911e29cc491c25e9594f6732d7deb2026b2975ac3f56f5d7bb"};

//! Synthetic era boundary: H is the final legacy height.
constexpr int SYNTHETIC_H{1000};

Consensus::Params B3Params()
{
    Consensus::Params params{};
    params.legacy_b3coin = true;
    params.hard_fork_height = SYNTHETIC_H + 1;
    return params;
}

std::string LegacyHex(const CBlock& block)
{
    DataStream s;
    s << legacy::TX_LEGACY(block);
    return HexStr(s);
}

} // namespace

BOOST_AUTO_TEST_CASE(genesis_identity_is_frozen)
{
    const CBlock genesis{legacy::CreateCoreGenesisBlock()};

    // Exact genesis block bytes, transaction bytes, txid, merkle and hash.
    BOOST_CHECK_EQUAL(LegacyHex(genesis), GENESIS_BLOCK_HEX);
    DataStream tx_bytes;
    tx_bytes << legacy::TX_LEGACY(*genesis.vtx[0]);
    BOOST_CHECK_EQUAL(HexStr(tx_bytes), GENESIS_TX_HEX);
    BOOST_CHECK_EQUAL(genesis.vtx[0]->GetHash().GetHex(), GENESIS_MERKLE);
    BOOST_CHECK_EQUAL(genesis.hashMerkleRoot.GetHex(), GENESIS_MERKLE);
    BOOST_CHECK_EQUAL(BlockMerkleRoot(genesis).GetHex(), GENESIS_MERKLE);
    BOOST_CHECK_EQUAL(genesis.GetLegacyB3Hash().GetHex(), GENESIS_HASH);

    // The frozen byte vector is self-validating: the historical scrypt hash
    // commits to its first 80 bytes.
    CBlockHeader roundtrip;
    DataStream header_bytes{ParseHex<std::byte>(GENESIS_BLOCK_HEX.substr(0, 160))};
    header_bytes >> roundtrip;
    BOOST_CHECK_EQUAL(roundtrip.GetLegacyB3Hash().GetHex(), GENESIS_HASH);
}

BOOST_AUTO_TEST_CASE(modern_codec_support_cannot_touch_genesis)
{
    const CBlock genesis{legacy::CreateCoreGenesisBlock()};

    // Genesis carries no modern marker, so the marker-aware codec always
    // takes the legacy branch and its hash domain is always scrypt.
    BOOST_CHECK(!Consensus::HasB3BlockCodecV2(genesis.nVersion));
    BOOST_CHECK_EQUAL(genesis.GetMarkerHash(B3Params()).GetHex(), GENESIS_HASH);
    BOOST_CHECK_EQUAL(genesis.GetHash(B3Params(), /*height=*/0).GetHex(), GENESIS_HASH);

    // The legality table pins genesis to the legacy codec forever: legacy
    // allowed at height 0, modern rejected.
    BOOST_CHECK(Consensus::HasExpectedB3BlockCodec(genesis.nVersion, 0, B3Params()));
    BOOST_CHECK(!Consensus::HasExpectedB3BlockCodec(
        static_cast<int32_t>(Consensus::B3_BLOCK_CODEC_V2_VERSION), 0, B3Params()));

    // Reinterpreting the frozen genesis bytes with the modern codec is
    // impossible without changing them: the modern encoding differs.
    DataStream modern_bytes;
    modern_bytes << TX_WITH_WITNESS(genesis);
    BOOST_CHECK(HexStr(modern_bytes) != GENESIS_BLOCK_HEX);
}

BOOST_AUTO_TEST_CASE(nonzero_ntime_legacy_transaction_is_frozen)
{
    const CTransaction tx{LegacyTxVector()};
    BOOST_CHECK(tx.nTime != 0);
    BOOST_CHECK(tx.IsLegacyEncoded());

    DataStream bytes;
    bytes << legacy::TX_LEGACY(tx);
    BOOST_CHECK_EQUAL(HexStr(bytes), LEGACY_TX_HEX);
    BOOST_CHECK_EQUAL(tx.GetHash().GetHex(), LEGACY_TXID);
    BOOST_CHECK_EQUAL(legacy::TxId(tx).GetHex(), LEGACY_TXID);

    // Round-trip through the explicit codec preserves bytes and identity.
    CTransactionRef decoded;
    DataStream copy{bytes};
    copy >> legacy::TX_LEGACY(decoded);
    BOOST_CHECK(decoded->IsLegacyEncoded());
    BOOST_CHECK_EQUAL(decoded->nTime, tx.nTime);
    BOOST_CHECK_EQUAL(decoded->GetHash().GetHex(), LEGACY_TXID);
}

BOOST_AUTO_TEST_CASE(legacy_block_with_trailing_signature_is_frozen)
{
    const CBlock block{LegacyBlockVector()};

    BOOST_CHECK_EQUAL(LegacyHex(block), LEGACY_BLOCK_HEX);
    BOOST_CHECK_EQUAL(block.GetLegacyB3Hash().GetHex(), LEGACY_BLOCK_HASH);

    // Legacy hash behavior: scrypt at every legacy height and under the
    // marker rule (no marker present).
    BOOST_CHECK_EQUAL(block.GetHash(B3Params(), SYNTHETIC_H).GetHex(), LEGACY_BLOCK_HASH);
    BOOST_CHECK_EQUAL(block.GetMarkerHash(B3Params()).GetHex(), LEGACY_BLOCK_HASH);
    BOOST_CHECK(block.GetHash().GetHex() != LEGACY_BLOCK_HASH);

    // The trailing signature survives a codec round-trip.
    CBlock decoded;
    DataStream bytes{ParseHex<std::byte>(LEGACY_BLOCK_HEX)};
    bytes >> legacy::TX_LEGACY(decoded);
    BOOST_REQUIRE_EQUAL(decoded.vchBlockSig.size(), 4U);
    BOOST_CHECK(decoded.vtx[0]->IsLegacyEncoded());
    BOOST_CHECK_EQUAL(decoded.vtx[0]->GetHash().GetHex(), LEGACY_TXID);
}

BOOST_AUTO_TEST_CASE(marker_modern_block_body_is_frozen)
{
    const CBlock block{ModernBlockVector()};

    // The marker selects the stock Core body even through the legacy-chain
    // codec: identical bytes both ways, no nTime, no trailing signature.
    BOOST_CHECK_EQUAL(LegacyHex(block), MODERN_BLOCK_HEX);
    DataStream modern_bytes;
    modern_bytes << TX_WITH_WITNESS(block);
    BOOST_CHECK_EQUAL(HexStr(modern_bytes), MODERN_BLOCK_HEX);
    BOOST_CHECK(!block.vtx[0]->IsLegacyEncoded());

    // Modern hash domain: SHA256d, selected by the marker.
    BOOST_CHECK_EQUAL(block.GetMarkerHash(B3Params()).GetHex(), MODERN_BLOCK_HASH);
    BOOST_CHECK_EQUAL(block.GetHash().GetHex(), MODERN_BLOCK_HASH);
    BOOST_CHECK_EQUAL(block.GetHash(B3Params(), SYNTHETIC_H + 1).GetHex(), MODERN_BLOCK_HASH);
    BOOST_CHECK(block.GetLegacyB3Hash().GetHex() != MODERN_BLOCK_HASH);
}

BOOST_AUTO_TEST_CASE(era_and_codec_combinations_at_the_boundary)
{
    const Consensus::Params params{B3Params()};
    constexpr int32_t legacy_version{4};
    constexpr int32_t modern_version{static_cast<int32_t>(Consensus::B3_BLOCK_CODEC_V2_VERSION)};

    struct Row {
        int height;
        Consensus::B3Era era;
        bool legacy_codec_allowed;
        bool modern_codec_allowed;
    };
    const Row rows[]{
        {SYNTHETIC_H - 1, Consensus::B3Era::LEGACY, true, false},
        {SYNTHETIC_H, Consensus::B3Era::LEGACY, true, false},
        {SYNTHETIC_H + 1, Consensus::B3Era::MODERN, false, true},
    };
    for (const Row& row : rows) {
        BOOST_CHECK(Consensus::GetB3Era(row.height, params) == row.era);
        BOOST_CHECK_EQUAL(Consensus::HasExpectedB3BlockCodec(legacy_version, row.height, params),
                          row.legacy_codec_allowed);
        BOOST_CHECK_EQUAL(Consensus::HasExpectedB3BlockCodec(modern_version, row.height, params),
                          row.modern_codec_allowed);
    }
}

BOOST_AUTO_TEST_CASE(unknown_parent_modern_identity_ignores_guessed_height)
{
    const CBlock block{ModernBlockVector()};
    const Consensus::Params params{B3Params()};

    // An unknown-parent modern header's identity comes from its marker.
    // A guessed height of zero would select the legacy scrypt domain and
    // yield a different, wrong identity — the marker path must not.
    BOOST_CHECK_EQUAL(block.GetMarkerHash(params).GetHex(), MODERN_BLOCK_HASH);
    BOOST_CHECK(Consensus::GetB3Era(0, params) == Consensus::B3Era::LEGACY);
    BOOST_CHECK(block.GetHash(params, /*height=*/0).GetHex() != MODERN_BLOCK_HASH);

    // And the reverse for a legacy header: the marker path agrees with the
    // legacy domain without needing any height at all.
    const CBlock legacy_block{LegacyBlockVector()};
    BOOST_CHECK_EQUAL(legacy_block.GetMarkerHash(params).GetHex(), LEGACY_BLOCK_HASH);
}

BOOST_AUTO_TEST_SUITE_END()
