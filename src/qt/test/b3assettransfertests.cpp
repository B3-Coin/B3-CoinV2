// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <chainparams.h>
#include <core_io.h>
#include <key_io.h>
#include <modern/asset_output.h>
#include <node/transaction.h>
#include <qt/b3assetmodel.h>
#include <qt/b3assettransfer.h>
#include <streams.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <util/translation.h>

#include <QTest>

#include <stdexcept>
#include <string>
#include <vector>

const TranslateFn G_TRANSLATION_FUN{nullptr};

namespace {
constexpr CAmount RAW_AMOUNT{1'250'001};
constexpr CAmount NETWORK_FEE{1'000};

uint256 TestHash(unsigned char tag)
{
    uint256 hash;
    hash.begin()[0] = tag;
    return hash;
}

PKHash Destination(unsigned char tag)
{
    uint160 hash;
    hash.begin()[0] = tag;
    return PKHash{hash};
}

B3AssetRecord Asset()
{
    B3AssetRecord asset;
    asset.asset_id = QString::fromStdString(TestHash(0x11).GetHex());
    asset.display_name = QStringLiteral("Transfer test asset");
    asset.ticker = QStringLiteral("TST");
    asset.decimals = 6;
    asset.precision_known = true;
    asset.metadata_known = true;
    asset.metadata_source = QStringLiteral("wallet-issuance");
    asset.status = B3AssetRecord::Status::Active;
    asset.available = RAW_AMOUNT * 2;
    return asset;
}

QString Recipient()
{
    return QString::fromStdString(EncodeDestination(Destination(0x22)));
}

CMutableTransaction Transaction()
{
    CMutableTransaction tx;
    // These are synthetic signature-shaped inputs, not real funds or keys.
    // The pure review helper checks structure; node admission verifies scripts.
    const CScript signed_input{CScript{} << std::vector<unsigned char>{0x30, 0x01, 0x01}
                                        << std::vector<unsigned char>(33, 0x02)};
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(TestHash(0x33)), 0}, signed_input);
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(TestHash(0x44)), 0}, signed_input);
    const auto output{modern::MakeAssetOwnerOutput(
        TestHash(0x11), RAW_AMOUNT, GetScriptForDestination(Destination(0x22)))};
    if (!output) throw std::runtime_error("Unable to create isolated test output");
    tx.vout.push_back(*output);
    tx.vout.emplace_back(KILO_COIN, GetScriptForDestination(Destination(0x55)));
    return tx;
}

UniValue Reply(const CMutableTransaction& tx)
{
    const CTransaction immutable{tx};
    DataStream serialized;
    serialized << TX_MODERN(immutable);
    UniValue reply{UniValue::VOBJ};
    reply.pushKV("txid", immutable.GetHash().GetHex());
    reply.pushKV("hex", HexStr(serialized));
    reply.pushKV("broadcast", false);
    reply.pushKV("asset_id", TestHash(0x11).GetHex());
    reply.pushKV("amount", RAW_AMOUNT);
    reply.pushKV("asset_change", 0);
    reply.pushKV("owner_address", Recipient().toStdString());
    reply.pushKV("network_fee", ValueFromAmount(NETWORK_FEE));
    reply.pushKV("disintegration", ValueFromAmount(0));
    return reply;
}
} // namespace

class B3AssetTransferTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        // Select address rules only. No node, wallet, RPC or user files.
        SelectParams(ChainType::REGTEST);
    }

    void amountsRemainExactAcrossPrecisions()
    {
        const auto check = [](const QString& text, int decimals, CAmount expected) {
            QString error;
            const auto amount{B3AssetTransfer::ParseAmount(text, decimals, &error)};
            QVERIFY2(amount.has_value(), qPrintable(error));
            QCOMPARE(*amount, expected);
        };
        check(QStringLiteral("1.250001"), 6, RAW_AMOUNT);
        check(QStringLiteral("9007199254.740993"), 6, 9'007'199'254'740'993LL);
        check(QStringLiteral("0.000000000000000001"), 18, 1);
        check(QString::number(MAX_MONEY), 0, MAX_MONEY);
        check(QStringLiteral("2"), 0, 2);
    }

    void invalidAmountsAreRejected()
    {
        for (const QString& text : {QStringLiteral(""), QStringLiteral("0"), QStringLiteral("-1"),
                 QStringLiteral("+1"), QStringLiteral("1e3"), QStringLiteral("NaN"),
                 QStringLiteral("1,25"), QStringLiteral("1.0000001"), QStringLiteral("١.٢")}) {
            QString error;
            QVERIFY2(!B3AssetTransfer::ParseAmount(text, 6, &error), qPrintable(text));
            QVERIFY(!error.isEmpty());
        }
        QVERIFY(!B3AssetTransfer::ParseAmount(QStringLiteral("1.1"), 0));
        QVERIFY(!B3AssetTransfer::ParseAmount(QStringLiteral("1"), -1));
        QVERIFY(!B3AssetTransfer::ParseAmount(QStringLiteral("1"), 19));
        QVERIFY(!B3AssetTransfer::ParseAmount(QString::number(MAX_MONEY + 1), 0));
    }

    void assetEligibilityAndPreparationAreExplicit()
    {
        B3AssetRecord asset{Asset()};
        QVERIFY(B3AssetTransfer::ValidateAsset(asset).isEmpty());
        const UniValue params{B3AssetTransfer::PrepareParameters(asset, RAW_AMOUNT, Recipient())};
        QCOMPARE(params.size(), size_t{4});
        QCOMPARE(params[0].get_str(), asset.asset_id.toStdString());
        QCOMPARE(params[1].getInt<int64_t>(), RAW_AMOUNT);
        QCOMPARE(params[2].get_str(), Recipient().toStdString());
        QVERIFY(params[3].isObject());
        QVERIFY(!params[3].find_value("broadcast").get_bool());

        // Unknown precision is supported only through explicit raw-unit entry.
        asset.precision_known = false;
        QVERIFY(B3AssetTransfer::ValidateAsset(asset).isEmpty());
        asset = Asset();
        asset.is_bridge = true;
        QVERIFY(B3AssetTransfer::ValidateAsset(asset).isEmpty());
        asset = Asset();
        asset.decimals = 19;
        QVERIFY(!B3AssetTransfer::ValidateAsset(asset).isEmpty());
        asset = Asset();
        asset.status = B3AssetRecord::Status::Unavailable;
        QVERIFY(!B3AssetTransfer::ValidateAsset(asset).isEmpty());
        asset = Asset();
        asset.status = B3AssetRecord::Status::Native;
        QVERIFY(!B3AssetTransfer::ValidateAsset(asset).isEmpty());
        asset = Asset();
        asset.decimals = 0;
        asset.is_fn = true;
        QVERIFY(B3AssetTransfer::ValidateAsset(asset).isEmpty());
        QVERIFY_EXCEPTION_THROWN(B3AssetTransfer::PrepareParameters(Asset(), 0, Recipient()), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(B3AssetTransfer::PrepareParameters(Asset(), RAW_AMOUNT, QStringLiteral("invalid")), std::runtime_error);
    }

    void broadcastUsesTheReviewedTransactionAndFeeLimit()
    {
        const UniValue reply{Reply(Transaction())};
        const auto prepared{B3AssetTransfer::ParsePrepared(reply, Asset(), RAW_AMOUNT, Recipient())};
        QCOMPARE(prepared.txid.toStdString(), reply.find_value("txid").get_str());
        QCOMPARE(prepared.hex, reply.find_value("hex").get_str());
        QCOMPARE(prepared.fee, NETWORK_FEE);
        QCOMPARE(prepared.raw_amount, RAW_AMOUNT);
        QCOMPARE(prepared.recipient, Recipient());
        const UniValue params{B3AssetTransfer::BroadcastParameters(prepared)};
        QCOMPARE(params.size(), size_t{3});
        QCOMPARE(params[0].get_str(), prepared.hex);
        QCOMPARE(params[1].write(), ValueFromAmount(node::DEFAULT_MAX_RAW_TX_FEE_RATE.GetFeePerK()).write());
        QCOMPARE(params[2].write(), ValueFromAmount(0).write());
    }

    void preflightMustConfirmTheSameTransactionAndFee()
    {
        const auto prepared{B3AssetTransfer::ParsePrepared(Reply(Transaction()), Asset(), RAW_AMOUNT, Recipient())};
        const UniValue params{B3AssetTransfer::AcceptanceParameters(prepared)};
        QCOMPARE(params.size(), size_t{2});
        QCOMPARE(params[0].size(), size_t{1});
        QCOMPARE(params[0][0].get_str(), prepared.hex);
        QCOMPARE(params[1].write(), ValueFromAmount(node::DEFAULT_MAX_RAW_TX_FEE_RATE.GetFeePerK()).write());
        const auto acceptance = [&](CAmount fee, bool allowed, const std::string& txid) {
            UniValue fees{UniValue::VOBJ};
            fees.pushKV("base", ValueFromAmount(fee));
            UniValue entry{UniValue::VOBJ};
            entry.pushKV("txid", txid);
            entry.pushKV("allowed", allowed);
            entry.pushKV("fees", fees);
            UniValue result{UniValue::VARR};
            result.push_back(entry);
            return result;
        };
        B3AssetTransfer::CheckAcceptance(acceptance(NETWORK_FEE, true, prepared.txid.toStdString()), prepared);
        QVERIFY_EXCEPTION_THROWN(B3AssetTransfer::CheckAcceptance(acceptance(NETWORK_FEE + 1, true, prepared.txid.toStdString()), prepared), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(B3AssetTransfer::CheckAcceptance(acceptance(NETWORK_FEE, false, prepared.txid.toStdString()), prepared), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(B3AssetTransfer::CheckAcceptance(acceptance(NETWORK_FEE, true, TestHash(0x66).GetHex()), prepared), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(B3AssetTransfer::CheckAcceptance(UniValue{UniValue::VARR}, prepared), std::runtime_error);
    }

    void changeOutputsMustBelongToTheCapturedWallet()
    {
        CMutableTransaction tx{Transaction()};
        constexpr CAmount asset_change{500};
        tx.vout.push_back(*modern::MakeAssetOwnerOutput(
            TestHash(0x11), asset_change, GetScriptForDestination(Destination(0x77))));
        UniValue reply{Reply(tx)};
        reply.pushKV("asset_change", asset_change);
        const auto prepared{B3AssetTransfer::ParsePrepared(reply, Asset(), RAW_AMOUNT, Recipient())};
        const CTxDestination native_change{Destination(0x55)};
        const CTxDestination colored_change{Destination(0x77)};
        size_t checked{0};
        B3AssetTransfer::CheckChangeOwnership(prepared, [&](const CTxDestination& destination) {
            ++checked;
            // The intended recipient is deliberately not wallet-owned.
            return destination == native_change || destination == colored_change;
        });
        QCOMPARE(checked, size_t{2});
        QVERIFY_EXCEPTION_THROWN(B3AssetTransfer::CheckChangeOwnership(prepared,
            [&](const CTxDestination& destination) { return destination == native_change; }), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(B3AssetTransfer::CheckChangeOwnership(prepared,
            [&](const CTxDestination& destination) { return destination == colored_change; }), std::runtime_error);

        tx = Transaction();
        tx.vout[1].scriptPubKey = CScript{} << OP_TRUE;
        const auto nonstandard_change{
            B3AssetTransfer::ParsePrepared(Reply(tx), Asset(), RAW_AMOUNT, Recipient())};
        QVERIFY_EXCEPTION_THROWN(B3AssetTransfer::CheckChangeOwnership(nonstandard_change,
            [](const CTxDestination&) { return true; }), std::runtime_error);
    }

    void currentAvailabilityAndPrecisionMustMatch()
    {
        const auto inventory = [](CAmount spendable, int decimals, bool known) {
            UniValue entry{UniValue::VOBJ};
            entry.pushKV("asset_id", TestHash(0x11).GetHex());
            entry.pushKV("spendable", spendable);
            entry.pushKV("decimals", decimals);
            entry.pushKV("precision_known", known);
            UniValue assets{UniValue::VARR};
            assets.push_back(entry);
            UniValue result{UniValue::VOBJ};
            result.pushKV("assets", assets);
            return result;
        };
        B3AssetTransfer::CheckAvailable(inventory(RAW_AMOUNT, 6, true), Asset(), RAW_AMOUNT);
        QVERIFY_EXCEPTION_THROWN(B3AssetTransfer::CheckAvailable(inventory(RAW_AMOUNT - 1, 6, true), Asset(), RAW_AMOUNT), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(B3AssetTransfer::CheckAvailable(inventory(RAW_AMOUNT, 7, true), Asset(), RAW_AMOUNT), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(B3AssetTransfer::CheckAvailable(inventory(RAW_AMOUNT, 6, false), Asset(), RAW_AMOUNT), std::runtime_error);
        UniValue missing{UniValue::VOBJ};
        missing.pushKV("assets", UniValue{UniValue::VARR});
        QVERIFY_EXCEPTION_THROWN(B3AssetTransfer::CheckAvailable(missing, Asset(), RAW_AMOUNT), std::runtime_error);
    }

    void walletEndpointIsExplicitAndEncoded()
    {
        QCOMPARE(B3AssetTransfer::WalletUri(QString{}), std::string{"/wallet/"});
        QCOMPARE(B3AssetTransfer::WalletUri(QStringLiteral("..")), std::string{"/wallet/%2E%2E"});
        QCOMPARE(B3AssetTransfer::WalletUri(QStringLiteral("test/wallet?x=#%")),
                 std::string{"/wallet/test%2Fwallet%3Fx%3D%23%25"});
    }

    void malformedOrMismatchedRepliesCannotBeConfirmed()
    {
        const auto reject = [](const UniValue& reply) {
            QVERIFY_EXCEPTION_THROWN(B3AssetTransfer::ParsePrepared(reply, Asset(), RAW_AMOUNT, Recipient()), std::runtime_error);
        };
        reject(UniValue{UniValue::VNULL});
        UniValue reply{Reply(Transaction())};
        reply.pushKV("broadcast", true);
        reject(reply);
        reply = Reply(Transaction());
        reply.pushKV("asset_id", TestHash(0x66).GetHex());
        reject(reply);
        reply = Reply(Transaction());
        reply.pushKV("amount", RAW_AMOUNT + 1);
        reject(reply);
        reply = Reply(Transaction());
        reply.pushKV("owner_address", EncodeDestination(Destination(0x66)));
        reject(reply);
        reply = Reply(Transaction());
        reply.pushKV("txid", TestHash(0x66).GetHex());
        reject(reply);
        reply = Reply(Transaction());
        reply.pushKV("network_fee", ValueFromAmount(-1));
        reject(reply);
        reply = Reply(Transaction());
        reply.pushKV("network_fee", ValueFromAmount(KILO_COIN));
        reject(reply);
        reply = Reply(Transaction());
        reply.pushKV("disintegration", ValueFromAmount(1));
        reject(reply);
        reply = Reply(Transaction());
        reply.pushKV("hex", "not-transaction-hex");
        reject(reply);
    }

    void serializedOutputsMustMatchTheReview()
    {
        const auto reject = [](const CMutableTransaction& tx) {
            // The RPC labels still describe the expected payment and txid is
            // recomputed, so rejection must inspect the actual transaction.
            QVERIFY_EXCEPTION_THROWN(B3AssetTransfer::ParsePrepared(Reply(tx), Asset(), RAW_AMOUNT, Recipient()), std::runtime_error);
        };
        CMutableTransaction tx{Transaction()};
        tx.vout[0] = *modern::MakeAssetOwnerOutput(TestHash(0x11), RAW_AMOUNT + 1, GetScriptForDestination(Destination(0x22)));
        reject(tx);
        tx = Transaction();
        tx.vout[0] = *modern::MakeAssetOwnerOutput(TestHash(0x66), RAW_AMOUNT, GetScriptForDestination(Destination(0x22)));
        reject(tx);
        tx = Transaction();
        tx.vout[0] = *modern::MakeAssetOwnerOutput(TestHash(0x11), RAW_AMOUNT, GetScriptForDestination(Destination(0x66)));
        reject(tx);
        tx = Transaction();
        tx.vin[0].scriptSig.clear();
        reject(tx);
        tx = Transaction();
        tx.mpa.push_back(CMpaRecord{3, 1, {1}});
        reject(tx);
    }
};

QTEST_GUILESS_MAIN(B3AssetTransferTests)
#include "b3assettransfertests.moc"
