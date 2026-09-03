// Copyright (c) 2017-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <key.h>
#include <key_io.h>
#include <node/context.h>
#include <script/script.h>
#include <script/solver.h>
#include <script/signingprovider.h>
#include <test/util/setup_common.h>
#include <wallet/types.h>
#include <wallet/wallet.h>
#include <wallet/receive.h>
#include <wallet/test/util.h>

#include <boost/test/unit_test.hpp>

using namespace util::hex_literals;

namespace wallet {
BOOST_FIXTURE_TEST_SUITE(ismine_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(ismine_standard)
{
    CKey keys[2];
    CPubKey pubkeys[2];
    for (int i = 0; i < 2; i++) {
        keys[i].MakeNewKey(true);
        pubkeys[i] = keys[i].GetPubKey();
    }

    CKey uncompressedKey = GenerateRandomKey(/*compressed=*/false);
    CPubKey uncompressedPubkey = uncompressedKey.GetPubKey();
    std::unique_ptr<interfaces::Chain>& chain = m_node.chain;

    CScript scriptPubKey;
    bool result;

    // P2PK compressed - Descriptor
    {
        CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());
        std::string desc_str = "pk(" + EncodeSecret(keys[0]) + ")";

        auto spk_manager = CreateDescriptor(keystore, desc_str, true);

        scriptPubKey = GetScriptForRawPubKey(pubkeys[0]);
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK(result);
    }

    // P2PK uncompressed - Descriptor
    {
        CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());
        std::string desc_str = "pk(" + EncodeSecret(uncompressedKey) + ")";

        auto spk_manager = CreateDescriptor(keystore, desc_str, true);

        scriptPubKey = GetScriptForRawPubKey(uncompressedPubkey);
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK(result);
    }

    // P2PKH compressed - Descriptor
    {
        CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());
        std::string desc_str = "pkh(" + EncodeSecret(keys[0]) + ")";

        auto spk_manager = CreateDescriptor(keystore, desc_str, true);

        scriptPubKey = GetScriptForDestination(PKHash(pubkeys[0]));
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK(result);
    }

    // P2PKH uncompressed - Descriptor
    {
        CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());
        std::string desc_str = "pkh(" + EncodeSecret(uncompressedKey) + ")";

        auto spk_manager = CreateDescriptor(keystore, desc_str, true);

        scriptPubKey = GetScriptForDestination(PKHash(uncompressedPubkey));
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK(result);
    }

    // P2SH - Descriptor
    {
        CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());
        std::string desc_str = "sh(pkh(" + EncodeSecret(keys[0]) + "))";

        auto spk_manager = CreateDescriptor(keystore, desc_str, true);

        CScript redeemScript = GetScriptForDestination(PKHash(pubkeys[0]));
        scriptPubKey = GetScriptForDestination(ScriptHash(redeemScript));
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK(result);
    }

    // (P2PKH inside) P2SH inside P2SH (invalid) - Descriptor
    {
        CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());
        std::string desc_str = "sh(sh(" + EncodeSecret(keys[0]) + "))";

        auto spk_manager = CreateDescriptor(keystore, desc_str, false);
        BOOST_CHECK_EQUAL(spk_manager, nullptr);
    }

    // (P2PKH inside) P2SH inside P2WSH (invalid) - Descriptor
    {
        CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());
        std::string desc_str = "wsh(sh(" + EncodeSecret(keys[0]) + "))";

        auto spk_manager = CreateDescriptor(keystore, desc_str, false);
        BOOST_CHECK_EQUAL(spk_manager, nullptr);
    }

    // P2WPKH inside P2WSH (invalid) - Descriptor
    {
        CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());
        std::string desc_str = "wsh(wpkh(" + EncodeSecret(keys[0]) + "))";

        auto spk_manager = CreateDescriptor(keystore, desc_str, false);
        BOOST_CHECK_EQUAL(spk_manager, nullptr);
    }

    // (P2PKH inside) P2WSH inside P2WSH (invalid) - Descriptor
    {
        CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());
        std::string desc_str = "wsh(wsh(" + EncodeSecret(keys[0]) + "))";

        auto spk_manager = CreateDescriptor(keystore, desc_str, false);
        BOOST_CHECK_EQUAL(spk_manager, nullptr);
    }

    // P2WPKH compressed - Descriptor
    {
        CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());
        std::string desc_str = "wpkh(" + EncodeSecret(keys[0]) + ")";

        auto spk_manager = CreateDescriptor(keystore, desc_str, true);

        scriptPubKey = GetScriptForDestination(WitnessV0KeyHash(pubkeys[0]));
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK(result);
    }

    // P2WPKH uncompressed (invalid) - Descriptor
    {
        CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());
        std::string desc_str = "wpkh(" + EncodeSecret(uncompressedKey) + ")";

        auto spk_manager = CreateDescriptor(keystore, desc_str, false);
        BOOST_CHECK_EQUAL(spk_manager, nullptr);
    }

    // scriptPubKey multisig - Descriptor
    {
        CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());
        std::string desc_str = "multi(2," + EncodeSecret(uncompressedKey) + "," + EncodeSecret(keys[1]) + ")";

        auto spk_manager = CreateDescriptor(keystore, desc_str, true);

        scriptPubKey = GetScriptForMultisig(2, {uncompressedPubkey, pubkeys[1]});
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK(result);
    }

    // P2SH multisig - Descriptor
    {
        CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());

        std::string desc_str = "sh(multi(2," + EncodeSecret(uncompressedKey) + "," + EncodeSecret(keys[1]) + "))";

        auto spk_manager = CreateDescriptor(keystore, desc_str, true);

        CScript redeemScript = GetScriptForMultisig(2, {uncompressedPubkey, pubkeys[1]});
        scriptPubKey = GetScriptForDestination(ScriptHash(redeemScript));
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK(result);
    }

    // P2WSH multisig with compressed keys - Descriptor
    {
        CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());

        std::string desc_str = "wsh(multi(2," + EncodeSecret(keys[0]) + "," + EncodeSecret(keys[1]) + "))";

        auto spk_manager = CreateDescriptor(keystore, desc_str, true);

        CScript redeemScript = GetScriptForMultisig(2, {pubkeys[0], pubkeys[1]});
        scriptPubKey = GetScriptForDestination(WitnessV0ScriptHash(redeemScript));
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK(result);
    }

    // P2WSH multisig with uncompressed key (invalid) - Descriptor
    {
        CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());

        std::string desc_str = "wsh(multi(2," + EncodeSecret(uncompressedKey) + "," + EncodeSecret(keys[1]) + "))";

        auto spk_manager = CreateDescriptor(keystore, desc_str, false);
        BOOST_CHECK_EQUAL(spk_manager, nullptr);
    }

    // P2WSH multisig wrapped in P2SH - Descriptor
    {
        CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());

        std::string desc_str = "sh(wsh(multi(2," + EncodeSecret(keys[0]) + "," + EncodeSecret(keys[1]) + ")))";

        auto spk_manager = CreateDescriptor(keystore, desc_str, true);

        CScript witnessScript = GetScriptForMultisig(2, {pubkeys[0], pubkeys[1]});
        CScript redeemScript = GetScriptForDestination(WitnessV0ScriptHash(witnessScript));
        scriptPubKey = GetScriptForDestination(ScriptHash(redeemScript));
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK(result);
    }

    // Combo - Descriptor
    {
        CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());

        std::string desc_str = "combo(" + EncodeSecret(keys[0]) + ")";

        auto spk_manager = CreateDescriptor(keystore, desc_str, true);

        // Test P2PK
        result = spk_manager->IsMine(GetScriptForRawPubKey(pubkeys[0]));
        BOOST_CHECK(result);

        // Test P2PKH
        result = spk_manager->IsMine(GetScriptForDestination(PKHash(pubkeys[0])));
        BOOST_CHECK(result);

        // Test P2SH (combo descriptor does not describe P2SH)
        CScript redeemScript = GetScriptForDestination(PKHash(pubkeys[0]));
        scriptPubKey = GetScriptForDestination(ScriptHash(redeemScript));
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK(!result);

        // Test P2WPKH
        scriptPubKey = GetScriptForDestination(WitnessV0KeyHash(pubkeys[0]));
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK(result);

        // P2SH-P2WPKH output
        redeemScript = GetScriptForDestination(WitnessV0KeyHash(pubkeys[0]));
        scriptPubKey = GetScriptForDestination(ScriptHash(redeemScript));
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK(result);

        // Test P2TR (combo descriptor does not describe P2TR)
        XOnlyPubKey xpk(pubkeys[0]);
        Assert(xpk.IsFullyValid());
        TaprootBuilder builder;
        builder.Finalize(xpk);
        WitnessV1Taproot output = builder.GetOutput();
        scriptPubKey = GetScriptForDestination(output);
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK(!result);
    }

    // Taproot - Descriptor
    {
        CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());

        std::string desc_str = "tr(" + EncodeSecret(keys[0]) + ")";

        auto spk_manager = CreateDescriptor(keystore, desc_str, true);

        XOnlyPubKey xpk(pubkeys[0]);
        Assert(xpk.IsFullyValid());
        TaprootBuilder builder;
        builder.Finalize(xpk);
        WitnessV1Taproot output = builder.GetOutput();
        scriptPubKey = GetScriptForDestination(output);
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK(result);
    }
}

BOOST_AUTO_TEST_CASE(exact_script_signability)
{
    CKey keys[3];
    CPubKey pubkeys[3];
    for (int i = 0; i < 3; ++i) {
        keys[i].MakeNewKey(/*fCompressedIn=*/true);
        pubkeys[i] = keys[i].GetPubKey();
    }
    std::unique_ptr<interfaces::Chain>& chain = m_node.chain;
    const auto can_sign = [](CWallet& wallet, const CScript& script) {
        LOCK(wallet.cs_wallet);
        return WalletCanSignScript(wallet, script);
    };

    // A private descriptor can satisfy its exact script, but possession of a
    // wallet key never makes an unrelated script signable.
    {
        CWallet wallet(chain.get(), "private", CreateMockableWalletDatabase());
        CreateDescriptor(wallet, "pkh(" + EncodeSecret(keys[0]) + ")", true);
        BOOST_CHECK(can_sign(
            wallet, GetScriptForDestination(PKHash{pubkeys[0]})));
        BOOST_CHECK(!can_sign(
            wallet, GetScriptForDestination(PKHash{pubkeys[1]})));
    }

    // A descriptor which knows the exact script but no secret remains
    // watch-only.
    {
        CWallet wallet(chain.get(), "watch", CreateMockableWalletDatabase());
        CreateDescriptor(wallet, "pkh(" + HexStr(pubkeys[0]) + ")", true);
        BOOST_CHECK(!can_sign(
            wallet, GetScriptForDestination(PKHash{pubkeys[0]})));
    }

    // Manager-wide HavePrivateKeys() is insufficient: one key in a 2-of-2
    // descriptor must not make the output spendable.
    {
        CWallet wallet(chain.get(), "partial", CreateMockableWalletDatabase());
        CreateDescriptor(
            wallet,
            "sh(multi(2," + EncodeSecret(keys[0]) + "," +
                HexStr(pubkeys[1]) + "))",
            true);
        const CScript redeem{GetScriptForMultisig(
            2, {pubkeys[0], pubkeys[1]})};
        BOOST_CHECK(!can_sign(
            wallet, GetScriptForDestination(ScriptHash{redeem})));
    }

    // Threshold semantics are exact as well: two available keys satisfy a
    // 2-of-3 descriptor even though the third key is watch-only.
    {
        CWallet wallet(chain.get(), "threshold", CreateMockableWalletDatabase());
        CreateDescriptor(
            wallet,
            "sh(multi(2," + EncodeSecret(keys[0]) + "," +
                EncodeSecret(keys[1]) + "," + HexStr(pubkeys[2]) + "))",
            true);
        const CScript redeem{GetScriptForMultisig(
            2, {pubkeys[0], pubkeys[1], pubkeys[2]})};
        BOOST_CHECK(can_sign(
            wallet, GetScriptForDestination(ScriptHash{redeem})));
    }

    // Locked encryption removes present signing capability and unlocking
    // restores it without changing ownership.
    {
        CWallet wallet(chain.get(), "locked", CreateMockableWalletDatabase());
        CreateDescriptor(wallet, "pkh(" + EncodeSecret(keys[0]) + ")", true);
        const CScript script{GetScriptForDestination(PKHash{pubkeys[0]})};
        BOOST_REQUIRE(wallet.EncryptWallet("signability-pass"));
        BOOST_CHECK(wallet.IsLocked());
        BOOST_CHECK(!can_sign(wallet, script));
        BOOST_REQUIRE(wallet.Unlock("signability-pass"));
        BOOST_CHECK(can_sign(wallet, script));
    }

    // An external signer has no local secrets. Exact manager ownership is the
    // capability boundary; foreign scripts still fail.
    {
        CWallet wallet(chain.get(), "external", CreateMockableWalletDatabase());
        CreateDescriptor(wallet, "pkh(" + HexStr(pubkeys[0]) + ")", true);
        wallet.SetWalletFlag(WALLET_FLAG_DISABLE_PRIVATE_KEYS);
        wallet.SetWalletFlag(WALLET_FLAG_EXTERNAL_SIGNER);
        BOOST_CHECK(can_sign(
            wallet, GetScriptForDestination(PKHash{pubkeys[0]})));
        BOOST_CHECK(!can_sign(
            wallet, GetScriptForDestination(PKHash{pubkeys[1]})));
    }
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
