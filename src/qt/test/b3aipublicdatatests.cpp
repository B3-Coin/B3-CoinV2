// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.

#include <qt/b3aipublicdata.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

class B3AiPublicDataTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void marketAccountAndEndpointNeverLeaveTheAdapter()
    {
        const QJsonObject raw{
            {"market_id", QString(64, 'a')}, {"available", true}, {"running", true}, {"epoch", 12}, {"halt", "certificate-conflict"},
            {"account", QJsonObject{{"account_id", "PRIVATE_ACCOUNT"}, {"b3_available", 345}, {"slot", 8}}},
            {"pending_actions", 4}, {"error", "PRIVATE_RPC_ERROR"}, {"future_field", "PRIVATE_FUTURE_FIELD"},
            {"verification", QJsonObject{{"endpoint", "https://PRIVATE_ENDPOINT"}, {"account_state_verified", true},
                                         {"certificate_verified", true}, {"b3_checkpoint_confirmed", false},
                                         {"future_field", "PRIVATE_NESTED_FIELD"}}}};
        const QJsonArray out = B3AiPublicData::SanitizeResult("listflowmeshmarkets", QJsonArray{raw}).toArray();
        QCOMPARE(out.size(), 1);
        const QJsonObject market = out.at(0).toObject();
        QCOMPARE(market.value("market_id").toString(), QString(64, 'a'));
        QCOMPARE(market.value("epoch").toInt(), 12);
        QVERIFY(market.value("running").toBool());
        QCOMPARE(market.value("halt").toString(), QStringLiteral("certificate-conflict"));
        QVERIFY(market.value("verification").toObject().value("certificate_verified").toBool());
        QVERIFY(!market.value("verification").toObject().value("b3_checkpoint_confirmed").toBool());
        QVERIFY(!QJsonDocument(out).toJson().contains("PRIVATE"));
        QVERIFY(!market.contains("account"));
        QVERIFY(!market.contains("pending_actions"));
        QVERIFY(!market.value("verification").toObject().contains("account_state_verified"));
        QJsonObject unrecognizedHalt = raw;
        unrecognizedHalt.insert("halt", "PRIVATE_HALT_TEXT");
        const QJsonValue filtered = B3AiPublicData::SanitizeResult("listflowmeshmarkets", QJsonArray{unrecognizedHalt});
        QVERIFY(!filtered.toArray().at(0).toObject().contains("halt"));
        QVERIFY(!QJsonDocument(filtered.toArray()).toJson().contains("PRIVATE"));
    }

    void connectionDetailsAreReducedToHealthCounts()
    {
        const QJsonObject raw{{"version", 310100}, {"networkactive", true}, {"connections", 7}, {"connections_in", 2},
                              {"subversion", "PRIVATE_AGENT"}, {"networks", QJsonArray{QJsonObject{{"proxy", "PRIVATE_PROXY"}}}},
                              {"localaddresses", QJsonArray{QJsonObject{{"address", "PRIVATE_ADDRESS"}}}}, {"warnings", "PRIVATE_WARNING"}};
        const QJsonObject out = B3AiPublicData::SanitizeResult("getnetworkinfo", raw).toObject();
        QCOMPARE(out.value("connections").toInt(), 7);
        QCOMPARE(out.value("connections_in").toInt(), 2);
        QVERIFY(out.value("networkactive").toBool());
        QVERIFY(!QJsonDocument(out).toJson().contains("PRIVATE"));
    }

    void finalityPreservesCheckpointWithoutLocalSignatures()
    {
        const QJsonObject raw{{"configured", true}, {"active", true}, {"epoch", 13},
                              {"finalized", QJsonObject{{"height", 123}, {"hash", QString(64, 'b')}, {"epoch", 12}, {"extra", "PRIVATE_EXTRA"}}},
                              {"validator_set", QJsonObject{{"size", 9}, {"set_hash", QString(64, 'c')}, {"members", "PRIVATE_MEMBERS"}}},
                              {"pool_checkpoints", 2}, {"verified_checkpoints", QJsonArray{QJsonObject{{"signer_indices", QJsonArray{1, 2}}}}}};
        const QJsonObject out = B3AiPublicData::SanitizeResult("getfinalitystatus", raw).toObject();
        QCOMPARE(out.value("epoch").toInt(), 13);
        QCOMPARE(out.value("finalized").toObject().value("height").toInt(), 123);
        QCOMPARE(out.value("finalized").toObject().value("hash").toString(), QString(64, 'b'));
        QCOMPARE(out.value("validator_set").toObject().value("size").toInt(), 9);
        QVERIFY(!out.contains("pool_checkpoints"));
        QVERIFY(!out.contains("verified_checkpoints"));
        QVERIFY(!QJsonDocument(out).toJson().contains("PRIVATE"));
    }

    void chainStatusExcludesFreeTextAndUnexpectedHashContents()
    {
        const QJsonObject raw{{"chain", "regtest"}, {"blocks", 42}, {"headers", 42}, {"initialblockdownload", false},
                              {"bestblockhash", "PRIVATE_INVALID_HASH"}, {"warnings", "PRIVATE_WARNING"},
                              {"size_on_disk", 5000}, {"future", "PRIVATE_FUTURE"}};
        const QJsonObject out = B3AiPublicData::SanitizeResult("getblockchaininfo", raw).toObject();
        QCOMPARE(out.value("chain").toString(), QStringLiteral("regtest"));
        QCOMPARE(out.value("blocks").toInt(), 42);
        QVERIFY(!out.contains("bestblockhash"));
        QVERIFY(!out.contains("size_on_disk"));
        QVERIFY(!QJsonDocument(out).toJson().contains("PRIVATE"));
    }

    void missingAndMalformedObservationsAreNotReportedAsSuccess()
    {
        const QStringList commands{"getblockchaininfo", "getnetworkinfo", "getfinalitystatus", "listflowmeshmarkets", "sendtoaddress"};
        for (const QString& command : commands) {
            QVERIFY(B3AiPublicData::SanitizeResult(command, QJsonValue{}).toObject().contains("error"));
            const QJsonObject error = B3AiPublicData::SanitizeResult(command, QJsonObject{{"error", "PRIVATE_ERROR"}}).toObject();
            QVERIFY(error.contains("error"));
            QVERIFY(!QJsonDocument(error).toJson().contains("PRIVATE"));
        }
        QVERIFY(B3AiPublicData::SanitizeResult("getblockchaininfo", QJsonObject{{"chain", "PRIVATE_CHAIN"}, {"blocks", 1}}).toObject().contains("error"));
        QVERIFY(B3AiPublicData::SanitizeResult("getnetworkinfo", QJsonObject{{"networkactive", "yes"}, {"connections", 1}}).toObject().contains("error"));
        QVERIFY(B3AiPublicData::SanitizeResult("listflowmeshmarkets", QJsonArray{QJsonObject{{"market_id", "invalid"}}}).toObject().contains("error"));
        // A successful RPC's empty array is a meaningful observation.
        const QJsonValue empty = B3AiPublicData::SanitizeResult("listflowmeshmarkets", QJsonArray{});
        QVERIFY(empty.isArray());
        QVERIFY(empty.toArray().isEmpty());
    }
};

QTEST_GUILESS_MAIN(B3AiPublicDataTests)
#include "b3aipublicdatatests.moc"
