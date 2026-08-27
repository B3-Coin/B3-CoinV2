// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Legacy-era wallet sends (Codex finding 1): a locally constructed,
// legacy-encoded transaction must sign over the HISTORICAL preimage
// (nTime after the version) and verify under the legacy script rules --
// and the provenance flag must be load-bearing (dropping it or altering
// nTime must invalidate the signature).

#include <key.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <test/util/setup_common.h>
#include <core_io.h>
#include <test/util/transaction_utils.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(legacy_wallet_send_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(legacy_encoded_send_signs_and_verifies)
{
    CKey key;
    key.MakeNewKey(true);
    FillableSigningProvider keystore;
    BOOST_REQUIRE(keystore.AddKey(key));
    const CScript p2pkh{GetScriptForDestination(PKHash{key.GetPubKey()})};

    // Funding transaction (legacy-encoded, as it would arrive from the chain).
    CMutableTransaction prev;
    prev.m_legacy_encoding = true;
    prev.version = 1;
    prev.nTime = 1'700'000'000;
    prev.vout.emplace_back(50'000'000, p2pkh);
    const CTransaction prev_tx{prev};

    // The wallet-constructed spend, exactly as spend.cpp now builds it in
    // the legacy era: flagged, timestamped, version 1.
    CMutableTransaction send;
    send.m_legacy_encoding = true;
    send.version = 1;
    send.nTime = 1'700'000'123;
    send.vin.emplace_back(COutPoint{prev_tx.GetHash(), 0});
    send.vout.emplace_back(49'000'000, p2pkh);

    SignatureData sigdata;
    BOOST_REQUIRE(SignSignature(keystore, prev_tx, send, 0, SIGHASH_ALL, sigdata));

    const CTransaction signed_tx{send};
    BOOST_CHECK(signed_tx.IsLegacyEncoded());
    const PrecomputedTransactionData txdata{signed_tx};

    auto verify = [&](const CTransaction& tx) {
        TransactionSignatureChecker checker{&tx, 0, prev_tx.vout[0].nValue,
                                            MissingDataBehavior::ASSERT_FAIL};
        return VerifyScript(tx.vin[0].scriptSig, p2pkh, nullptr,
                            SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_DERSIG, checker);
    };
    BOOST_CHECK(verify(signed_tx));

    // The provenance flag is LOAD-BEARING: the same bytes reinterpreted as a
    // modern transaction hash a different preimage and must NOT verify.
    {
        CMutableTransaction stripped{send};
        stripped.m_legacy_encoding = false;
        BOOST_CHECK(!verify(CTransaction{stripped}));
    }
    // nTime is inside the signed preimage: altering it invalidates.
    {
        CMutableTransaction shifted{send};
        shifted.nTime += 1;
        BOOST_CHECK(!verify(CTransaction{shifted}));
    }
    // Modern-era construction still signs and verifies unchanged (regression).
    {
        CMutableTransaction modern;
        modern.version = 2;
        modern.vin.emplace_back(COutPoint{prev_tx.GetHash(), 0});
        modern.vout.emplace_back(49'000'000, p2pkh);
        SignatureData modern_sigdata;
        BOOST_REQUIRE(SignSignature(keystore, prev_tx, modern, 0, SIGHASH_ALL, modern_sigdata));
        BOOST_CHECK(verify(CTransaction{modern}));
    }
}

BOOST_AUTO_TEST_CASE(legacy_raw_rpc_round_trip)
{
    // The raw-RPC surface for legacy transactions: EncodeHexTx must emit
    // the historical nTime encoding and DecodeHexTx (with DEFAULT flags,
    // as sendrawtransaction and both signing RPCs call it) must recover
    // the identical transaction -- id, provenance, nTime and all.
    CKey key;
    key.MakeNewKey(true);
    const CScript p2pkh{GetScriptForDestination(PKHash{key.GetPubKey()})};

    CMutableTransaction m;
    m.m_legacy_encoding = true;
    m.version = 1;
    m.nTime = 1'711'111'111;
    m.vin.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), 3});
    m.vin[0].scriptSig = CScript() << std::vector<unsigned char>(71, 0x01)
                                   << std::vector<unsigned char>(33, 0x02);
    m.vout.emplace_back(12'345'678, p2pkh);
    const CTransaction tx{m};

    const std::string hex{EncodeHexTx(tx)};
    CMutableTransaction decoded;
    BOOST_REQUIRE(DecodeHexTx(decoded, hex)); // DEFAULT flags: the RPC path
    const CTransaction round{decoded};
    BOOST_CHECK(round.IsLegacyEncoded());
    BOOST_CHECK_EQUAL(round.nTime, tx.nTime);
    BOOST_CHECK(round.GetHash() == tx.GetHash());
    BOOST_CHECK_EQUAL(EncodeHexTx(round), hex); // stable round trip

    // A modern transaction still round-trips through the same surface.
    CMutableTransaction mod;
    mod.version = 2;
    mod.vin.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), 0});
    mod.vout.emplace_back(1'000, p2pkh);
    const CTransaction mtx{mod};
    CMutableTransaction mdecoded;
    BOOST_REQUIRE(DecodeHexTx(mdecoded, EncodeHexTx(mtx)));
    BOOST_CHECK(!CTransaction{mdecoded}.IsLegacyEncoded());
    BOOST_CHECK(CTransaction{mdecoded}.GetHash() == mtx.GetHash());
}

BOOST_AUTO_TEST_SUITE_END()
