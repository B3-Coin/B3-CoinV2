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

BOOST_AUTO_TEST_SUITE_END()
