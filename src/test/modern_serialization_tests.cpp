// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <core_io.h>
#include <modern/asset_output.h>
#include <primitives/transaction.h>
#include <psbt.h>
#include <script/signingprovider.h>
#include <streams.h>
#include <test/util/setup_common.h>

#include <addresstype.h>
#include <key.h>
#include <univalue.h>

#include <boost/test/unit_test.hpp>

#include <vector>

namespace {

CMpaRecord TestRecord(const unsigned char fill)
{
    CMpaRecord record;
    record.payload_type = 3;
    record.payload_version = 1;
    record.payload = {fill, static_cast<unsigned char>(fill + 1),
                      static_cast<unsigned char>(fill + 2)};
    return record;
}

CMutableTransaction UnsignedModernTx(const CMpaRecord& record)
{
    CMutableTransaction tx;
    tx.version = 2;
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), 0});
    tx.vout.emplace_back(1, CScript() << OP_TRUE);
    tx.mpa.push_back(record);
    return tx;
}

modern::AssetId TestAsset()
{
    modern::AssetId asset;
    asset.begin()[0] = 0x42;
    return asset;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(modern_serialization_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(raw_codec_and_json_preserve_mpa)
{
    CMutableTransaction mutable_tx{UnsignedModernTx(TestRecord(0x31))};
    mutable_tx.vin[0].scriptWitness.stack.push_back({0xaa, 0xbb});
    const CTransaction tx{mutable_tx};

    DataStream canonical;
    canonical << TX_MODERN(tx);
    const std::string expected_hex{HexStr(canonical)};
    BOOST_CHECK_EQUAL(EncodeHexTx(tx), expected_hex);

    CMutableTransaction decoded;
    BOOST_REQUIRE(DecodeHexTx(decoded, expected_hex));
    const CTransaction round_trip{decoded};
    BOOST_CHECK(round_trip.mpa == tx.mpa);
    BOOST_CHECK(round_trip.GetPtxid() == tx.GetPtxid());
    BOOST_CHECK(round_trip.GetWitnessHash().ToUint256() == tx.GetPtxid().ToUint256());

    // MPA does not imply witness. An explicit no-witness decode accepts
    // flag 0x02 while continuing to reject actual flag 0x01 data.
    const CTransaction no_witness{UnsignedModernTx(TestRecord(0x41))};
    CMutableTransaction decoded_no_witness;
    BOOST_REQUIRE(DecodeHexTx(decoded_no_witness, EncodeHexTx(no_witness),
                              /*try_no_witness=*/true,
                              /*try_witness=*/false));
    BOOST_CHECK(decoded_no_witness.mpa == no_witness.mpa);
    CMutableTransaction rejected_witness;
    BOOST_CHECK(!DecodeHexTx(rejected_witness, expected_hex,
                             /*try_no_witness=*/true,
                             /*try_witness=*/false));

    UniValue json{UniValue::VOBJ};
    TxToUniv(round_trip, uint256{}, json, /*include_hex=*/true);
    BOOST_CHECK_EQUAL(json["ptxid"].get_str(), tx.GetPtxid().GetHex());
    BOOST_REQUIRE_EQUAL(json["mpa"].size(), 1U);
    BOOST_CHECK_EQUAL(json["mpa"][0]["type"].getInt<int>(), 3);
    BOOST_CHECK_EQUAL(json["mpa"][0]["version"].getInt<int>(), 1);
    BOOST_CHECK_EQUAL(json["mpa"][0]["payload"].get_str(), "313233");
    BOOST_CHECK_EQUAL(json["hex"].get_str(), expected_hex);
}

BOOST_AUTO_TEST_CASE(psbt_round_trip_preserves_and_binds_mpa)
{
    const CMutableTransaction tx{UnsignedModernTx(TestRecord(0x51))};
    PartiallySignedTransaction psbt{tx};

    DataStream encoded;
    encoded << psbt;
    PartiallySignedTransaction decoded;
    encoded >> decoded;
    BOOST_REQUIRE(decoded.tx.has_value());
    BOOST_CHECK(decoded.tx->mpa == tx.mpa);
    BOOST_CHECK(CTransaction{*decoded.tx}.GetPtxid() == CTransaction{tx}.GetPtxid());
    BOOST_CHECK(decoded.m_proprietary.empty()); // recognized, not an opaque duplicate

    CMutableTransaction different{tx};
    different.mpa[0].payload[0] ^= 0x01;
    PartiallySignedTransaction different_psbt{different};
    BOOST_CHECK(!decoded.Merge(different_psbt));
}

BOOST_AUTO_TEST_CASE(psbt_sign_and_finalize_asset_owner_preserves_mpa)
{
    CKey key;
    key.MakeNewKey(true);
    FillableSigningProvider provider;
    BOOST_REQUIRE(provider.AddKey(key));
    const CScript owner{GetScriptForDestination(PKHash{key.GetPubKey()})};
    const auto asset_output{modern::MakeAssetOwnerOutput(
        TestAsset(), 1, modern::PolicyType::FN, owner)};
    BOOST_REQUIRE(asset_output.has_value());

    CMutableTransaction funding;
    funding.version = 2;
    funding.vin.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), 2});
    funding.vout.push_back(*asset_output);
    const CTransactionRef funding_ref{MakeTransactionRef(std::move(funding))};

    CMutableTransaction spend;
    spend.version = 2;
    spend.vin.emplace_back(COutPoint{funding_ref->GetHash(), 0});
    spend.vout.emplace_back(0, CScript() << OP_RETURN
                                        << std::vector<unsigned char>{0x01});
    spend.mpa.push_back(TestRecord(0x61));

    PartiallySignedTransaction psbt{spend};
    psbt.inputs[0].non_witness_utxo = funding_ref;
    PrecomputedTransactionData txdata{PrecomputePSBTData(psbt)};

    // The PSBT bytes prove the prevout contents, not its creation height.
    // Contextless signing must therefore keep the full B3A1-looking script
    // and refuse to reinterpret it as a modern owner carrier.
    BOOST_CHECK(SignPSBTInput(provider, psbt, 0, &txdata, SIGHASH_ALL,
                              nullptr, /*finalize=*/true) ==
                PSBTError::INCOMPLETE);
    BOOST_CHECK(!PSBTInputSignedAndVerified(psbt, 0, &txdata));

    // A wallet may opt in only after deriving post-H provenance from its own
    // chain state. This runtime context is intentionally not serialized.
    psbt.SetInputAssetSigningContext(0,
                                     AssetSigningContext::OWNER_SUFFIX);
    BOOST_REQUIRE(SignPSBTInput(provider, psbt, 0, &txdata, SIGHASH_ALL,
                                nullptr, /*finalize=*/true) == PSBTError::OK);
    BOOST_CHECK(PSBTInputSignedAndVerified(psbt, 0, &txdata));

    // Exercise the actual serialized PSBT boundary before extraction.
    DataStream encoded;
    encoded << psbt;
    PartiallySignedTransaction decoded;
    encoded >> decoded;
    CMutableTransaction extracted;
    const PrecomputedTransactionData decoded_txdata{PrecomputePSBTData(decoded)};
    BOOST_CHECK(decoded.GetInputAssetSigningContext(0) ==
                AssetSigningContext::FULL_SCRIPT);
    BOOST_CHECK(!PSBTInputSignedAndVerified(decoded, 0, &decoded_txdata));
    BOOST_CHECK(!FinalizeAndExtractPSBT(decoded, extracted));

    // A receiving wallet that independently proves the Coin is post-H can
    // restore the local context and finalize without any spoofable PSBT flag.
    decoded.SetInputAssetSigningContext(0,
                                        AssetSigningContext::OWNER_SUFFIX);
    BOOST_REQUIRE(FinalizeAndExtractPSBT(decoded, extracted));
    BOOST_CHECK(extracted.mpa == spend.mpa);

    const std::string raw_hex{EncodeHexTx(CTransaction{extracted})};
    CMutableTransaction raw_round_trip;
    BOOST_REQUIRE(DecodeHexTx(raw_round_trip, raw_hex));
    BOOST_CHECK(raw_round_trip.mpa == spend.mpa);
}

BOOST_AUTO_TEST_SUITE_END()
