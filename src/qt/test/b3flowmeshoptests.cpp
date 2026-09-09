// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <chainparams.h>
#include <common/args.h>
#include <core_io.h>
#include <crypto/bls.h>
#include <key_io.h>
#include <modern/flowmesh_seat.h>
#include <qt/b3assetmodel.h>
#include <qt/b3flowmeshoperator.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <util/translation.h>

#include <QTest>

#include <array>
#include <functional>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

const TranslateFn G_TRANSLATION_FUN{nullptr};

namespace {
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

bls::SecretKey FixtureKey(unsigned char tag = 1)
{
    // Publicly known test scalars only. No wallet, random key or live funds.
    std::array<unsigned char, bls::SECRET_SIZE> bytes{};
    bytes.back() = tag;
    const auto key{bls::SecretKey::FromBytes(bytes)};
    if (!key) throw std::runtime_error{"Invalid isolated BLS fixture"};
    return *key;
}

std::string PublicKeyHex(unsigned char tag = 1)
{
    return HexStr(FixtureKey(tag).GetPublicKey().Compressed());
}

UniValue Array(std::initializer_list<std::string> strings)
{
    UniValue result{UniValue::VARR};
    for (const auto& string : strings) result.push_back(string);
    return result;
}

UniValue Without(const UniValue& value, const std::string& field)
{
    UniValue result{UniValue::VOBJ};
    for (size_t i{0}; i < value.size(); ++i) {
        if (value.getKeys()[i] != field) result.pushKV(value.getKeys()[i], value.getValues()[i]);
    }
    return result;
}

bool Rejected(const std::function<void()>& operation)
{
    try {
        operation();
    } catch (const std::exception&) {
        return true;
    } catch (const UniValue&) {
        // AmountFromValue uses the same structured error type as wallet RPC.
        return true;
    }
    return false;
}

UniValue Market()
{
    UniValue market{UniValue::VOBJ};
    market.pushKV("market_id", TestHash(0x88).GetHex());
    market.pushKV("paused", false);
    market.pushKV("observer_only", false);
    market.pushKV("running", true);
    market.pushKV("error", "");
    market.pushKV("halt", "none");
    return market;
}

UniValue StatusReply(const UniValue& market = Market())
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("scope", "node-global");
    result.pushKV("service_available", true);
    result.pushKV("armed", true);
    result.pushKV("armed_keys_fingerprint", TestHash(0x77).GetHex());
    result.pushKV("wallet_bls_pubkeys", Array({PublicKeyHex()}));
    result.pushKV("armed_bls_pubkeys", Array({PublicKeyHex(), PublicKeyHex(2)}));
    result.pushKV("armed_key_count", 2);
    result.pushKV("wallet_armed_key_count", 1);
    UniValue markets{UniValue::VARR};
    markets.push_back(market);
    result.pushKV("markets", markets);
    return result;
}

B3AssetRecord FnAsset()
{
    B3AssetRecord result;
    result.asset_id = QString::fromStdString(TestHash(0x11).GetHex());
    result.ticker = QStringLiteral("FN");
    result.display_name = QStringLiteral("Isolated FN fixture");
    result.is_fn = true;
    result.status = B3AssetRecord::Status::Active;
    result.decimals = 0;
    result.precision_known = true;
    result.metadata_known = true;
    result.confirmed = result.available = 1;
    return result;
}

bool Owned(const CTxDestination& destination)
{
    return destination == CTxDestination{Destination(0x22)} ||
           destination == CTxDestination{Destination(0x55)};
}

CMutableTransaction Binding()
{
    CMutableTransaction tx;
    // Signature-shaped inputs suffice for this pure review fixture. The
    // actual node's testmempoolaccept still checks real scripts before send.
    const CScript signature{CScript{} << std::vector<unsigned char>{0x30, 0x01, 0x01}
                                      << std::vector<unsigned char>(33, 0x02)};
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(TestHash(0x33)), 2}, signature);
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(TestHash(0x44)), 0}, signature);
    const auto key{FixtureKey()};
    const auto seat{modern::MakeFlowMeshSeatOutput(
        TestHash(0x11), GetScriptForDestination(Destination(0x22)), key.GetPublicKey())};
    if (!seat) throw std::runtime_error{"Cannot construct isolated FN seat"};
    tx.vout.push_back(*seat);
    tx.vout.emplace_back(KILO_COIN, GetScriptForDestination(Destination(0x55)));
    tx.mpa.push_back(modern::MakeFlowMeshSeatBindingRecord(0, key.SignPoP().Compressed()));
    return tx;
}

UniValue BindingReply(const CMutableTransaction& mutable_tx)
{
    const CTransaction tx{mutable_tx};
    UniValue reply{UniValue::VOBJ};
    reply.pushKV("txid", tx.GetHash().GetHex());
    reply.pushKV("seat_txid", tx.GetHash().GetHex());
    reply.pushKV("seat_vout", 0);
    reply.pushKV("fn_input_txid", TestHash(0x33).GetHex());
    reply.pushKV("fn_input_vout", 2);
    reply.pushKV("hex", EncodeHexTx(tx));
    reply.pushKV("broadcast", false);
    reply.pushKV("rotation", false);
    reply.pushKV("bls_pubkey", PublicKeyHex());
    reply.pushKV("owner_address", EncodeDestination(Destination(0x22)));
    reply.pushKV("network_fee", ValueFromAmount(NETWORK_FEE));
    reply.pushKV("disintegration", ValueFromAmount(0));
    return reply;
}
} // namespace

class B3FlowMeshOperatorTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        // B3's MPA codec is deliberately unavailable on ordinary Bitcoin
        // regtest. Select the B3-modern REGTEST parameters used by the real
        // encoder/decoder, without a node, wallet, network or mining.
        gArgs.ForceSetArg("-b3modernregtest", "1");
        SelectParams(ChainType::REGTEST);
        QVERIFY(Params().GetConsensus().legacy_b3coin);
    }

    void statusKeepsWalletAndNodeGlobalKeysDistinct()
    {
        const auto status{B3FlowMeshOperator::ParseStatus(StatusReply())};
        QVERIFY(status.available);
        QVERIFY(status.armed);
        QCOMPARE(status.wallet_keys.size(), qsizetype{1});
        QCOMPARE(status.armed_keys.size(), qsizetype{2});
        QCOMPARE(status.wallet_armed, 1);
        QCOMPARE(status.wallet_keys.front().toStdString(), PublicKeyHex());
        QCOMPARE(status.markets.size(), qsizetype{1});
        QVERIFY(status.markets.front().contains(QStringLiteral("signing not independently confirmed")));

        auto stopped{StatusReply()};
        stopped.pushKV("armed", false);
        stopped.pushKV("armed_bls_pubkeys", UniValue{UniValue::VARR});
        stopped.pushKV("armed_key_count", 0);
        stopped.pushKV("wallet_armed_key_count", 0);
        const auto result{B3FlowMeshOperator::ParseStatus(stopped)};
        QVERIFY(!result.armed);
        QCOMPARE(result.wallet_armed, 0);
        QCOMPARE(result.wallet_keys.size(), qsizetype{1});
    }

    void malformedStatusScopesCountsAndKeysAreRejected()
    {
        const std::vector<std::pair<std::string, UniValue>> changes{
            {"scope", UniValue{"wallet-local"}},
            {"service_available", UniValue{1}},
            {"armed", UniValue{1}},
            {"armed", UniValue{false}},
            {"armed_keys_fingerprint", UniValue{"not a fingerprint"}},
            {"wallet_bls_pubkeys", UniValue{"not an array"}},
            {"armed_bls_pubkeys", UniValue{}},
            {"armed_key_count", UniValue{-1}},
            {"armed_key_count", UniValue{1}},
            {"armed_key_count", UniValue{true}},
            {"armed_key_count", UniValue{UniValue::VNUM, "2.5"}},
            {"wallet_armed_key_count", UniValue{2}},
            {"wallet_armed_key_count", UniValue{-1}},
            {"wallet_bls_pubkeys", Array({PublicKeyHex(), PublicKeyHex()})},
            {"armed_bls_pubkeys", Array({PublicKeyHex(), PublicKeyHex()})},
            {"wallet_bls_pubkeys", Array({std::string(96, '0')})},
            {"wallet_bls_pubkeys", Array({std::string(96, 'g')})},
            {"wallet_bls_pubkeys", Array({PublicKeyHex().substr(2)})},
            {"markets", UniValue{false}},
        };
        for (const auto& [field, invalid] : changes) {
            auto reply{StatusReply()};
            reply.pushKV(field, invalid);
            QVERIFY2(Rejected([&] { B3FlowMeshOperator::ParseStatus(reply); }), field.c_str());
        }
        const auto valid{StatusReply()};
        for (const auto& field : valid.getKeys()) {
            QVERIFY2(Rejected([&] { B3FlowMeshOperator::ParseStatus(Without(valid, field)); }), field.c_str());
        }
        QVERIFY(Rejected([] { B3FlowMeshOperator::ParseStatus(UniValue{}); }));
    }

    void everyMarketFlagIsValidatedEvenWhenPaused()
    {
        for (bool paused : {false, true}) {
            for (bool observer : {false, true}) {
                for (const char* field : {"paused", "observer_only", "running", "error", "halt", "market_id"}) {
                    auto market{Market()};
                    market.pushKV("paused", paused);
                    market.pushKV("observer_only", observer);
                    market.pushKV(field, UniValue{1});
                    QVERIFY2(Rejected([&] { B3FlowMeshOperator::ParseStatus(StatusReply(market)); }), field);
                }
            }
        }
    }

    void controlRequestsCarryTheExactReviewedFingerprint()
    {
        auto status{B3FlowMeshOperator::ParseStatus(StatusReply())};
        const auto params{B3FlowMeshOperator::ControlParameters(status)};
        QCOMPARE(params.size(), size_t{1});
        QVERIFY(params[0].isStr());
        QCOMPARE(params[0].get_str(), status.fingerprint.toStdString());
        for (const QString& fingerprint : {QString{}, QString(63, QLatin1Char('1')), QString(64, QLatin1Char('x'))}) {
            status.fingerprint = fingerprint;
            QVERIFY(Rejected([&] { B3FlowMeshOperator::ControlParameters(status); }));
        }
    }

    void bindingPreparationCannotBroadcastOrUseUnsafeInputs()
    {
        const auto params{B3FlowMeshOperator::BindParameters()};
        QCOMPARE(params.size(), size_t{2});
        QVERIFY(params[0].isNull());
        const auto& options{params[1]};
        QVERIFY(options.isObject());
        QCOMPARE(options.size(), size_t{4});
        QVERIFY(!options.find_value("broadcast").get_bool());
        QVERIFY(!options.find_value("replaceable").get_bool());
        QVERIFY(!options.find_value("include_unsafe").get_bool());
        QCOMPARE(options.find_value("minconf").getInt<int>(), 1);
    }

    void validBindingReviewsOneFnAndWalletOwnedNativeChange()
    {
        const auto reply{BindingReply(Binding())};
        CMutableTransaction decoded;
        QVERIFY(DecodeHexTx(decoded, reply.find_value("hex").get_str()));
        QCOMPARE(decoded.mpa.size(), size_t{1});
        QString public_key;
        std::vector<CTxDestination> checked;
        const auto prepared{B3FlowMeshOperator::ParseBinding(reply, FnAsset(), [&](const CTxDestination& destination) {
            checked.push_back(destination);
            return Owned(destination);
        }, public_key)};
        QCOMPARE(checked.size(), size_t{2});
        QCOMPARE(public_key.toStdString(), PublicKeyHex());
        QCOMPARE(prepared.raw_amount, CAmount{1});
        QCOMPARE(prepared.fee, NETWORK_FEE);
        QCOMPARE(prepared.txid.toStdString(), reply.find_value("txid").get_str());
        QCOMPARE(prepared.hex, reply.find_value("hex").get_str());
        QCOMPARE(prepared.recipient.toStdString(), EncodeDestination(Destination(0x22)));
        const auto broadcast{B3AssetTransfer::BroadcastParameters(prepared)};
        QCOMPARE(broadcast[0].get_str(), prepared.hex);
        QCOMPARE(broadcast[2].write(), ValueFromAmount(0).write());
    }

    void bindingReviewRejectsMetadataChangesAndRotation()
    {
        const std::vector<std::pair<std::string, UniValue>> changes{
            {"broadcast", UniValue{true}}, {"rotation", UniValue{true}},
            {"seat_txid", UniValue{TestHash(0x99).GetHex()}},
            {"txid", UniValue{TestHash(0x99).GetHex()}},
            {"seat_vout", UniValue{1}},
            {"fn_input_txid", UniValue{TestHash(0x99).GetHex()}},
            {"fn_input_vout", UniValue{0}},
            {"fn_input_vout", UniValue{int64_t{0x1'0000'0000}}},
            {"owner_address", UniValue{EncodeDestination(Destination(0x99))}},
            {"owner_address", UniValue{"not an address"}},
            {"bls_pubkey", UniValue{PublicKeyHex(2)}},
            {"bls_pubkey", UniValue{std::string(96, '0')}},
            {"hex", UniValue{"zz"}},
            {"network_fee", ValueFromAmount(-1)},
            {"network_fee", ValueFromAmount(KILO_COIN)},
            {"disintegration", ValueFromAmount(1)},
        };
        for (const auto& [field, invalid] : changes) {
            auto reply{BindingReply(Binding())};
            reply.pushKV(field, invalid);
            QString public_key{QStringLiteral("unchanged")};
            QVERIFY2(Rejected([&] { B3FlowMeshOperator::ParseBinding(reply, FnAsset(), Owned, public_key); }), field.c_str());
            QCOMPARE(public_key, QStringLiteral("unchanged"));
        }
        auto asset{FnAsset()};
        asset.is_fn = false;
        QString public_key;
        QVERIFY(Rejected([&] { B3FlowMeshOperator::ParseBinding(BindingReply(Binding()), asset, Owned, public_key); }));
        asset = FnAsset();
        asset.asset_id = QString::fromStdString(TestHash(0x99).GetHex());
        QVERIFY(Rejected([&] { B3FlowMeshOperator::ParseBinding(BindingReply(Binding()), asset, Owned, public_key); }));
    }

    void bindingProofAndSerializedCarrierMustMatchTheReview()
    {
        const auto reject = [](const CMutableTransaction& tx) {
            QString public_key{QStringLiteral("unchanged")};
            // Recompute txid: rejection must inspect the actual carrier/PoP,
            // not merely notice stale RPC transaction identifiers.
            QVERIFY(Rejected([&] { B3FlowMeshOperator::ParseBinding(BindingReply(tx), FnAsset(), Owned, public_key); }));
            QCOMPARE(public_key, QStringLiteral("unchanged"));
        };
        auto tx{Binding()};
        tx.mpa[0] = modern::MakeFlowMeshSeatBindingRecord(0, FixtureKey(2).SignPoP().Compressed());
        reject(tx);
        tx = Binding();
        tx.mpa[0].payload.back() ^= 1;
        reject(tx);
        tx = Binding();
        tx.mpa[0] = modern::MakeFlowMeshSeatBindingRecord(1, FixtureKey().SignPoP().Compressed());
        reject(tx);
        tx = Binding();
        tx.mpa.push_back(tx.mpa.front());
        reject(tx);
        tx = Binding();
        tx.mpa.clear();
        reject(tx);
        tx = Binding();
        tx.vout[0] = *modern::MakeFlowMeshSeatOutput(TestHash(0x99), GetScriptForDestination(Destination(0x22)), FixtureKey().GetPublicKey());
        reject(tx);
        tx = Binding();
        auto seat{modern::ParseAssetOutput(tx.vout.front())};
        QVERIFY(seat);
        seat->amount = 2;
        tx.vout[0] = *modern::MakeAssetOwnerOutput(*seat, GetScriptForDestination(Destination(0x22)));
        reject(tx);
        tx = Binding();
        tx.vout[0] = *modern::MakeAssetOwnerOutput(TestHash(0x11), 1, modern::PolicyType::FN, GetScriptForDestination(Destination(0x22)));
        reject(tx);
        tx = Binding();
        tx.vout[0] = *modern::MakeFlowMeshSeatOutput(TestHash(0x11), GetScriptForDestination(Destination(0x22)), FixtureKey(2).GetPublicKey());
        reject(tx);
        tx = Binding();
        tx.vin[0].scriptSig.clear();
        reject(tx);
        tx = Binding();
        tx.vout.emplace_back(*modern::MakeAssetOwnerOutput(TestHash(0x99), 1, GetScriptForDestination(Destination(0x55))));
        reject(tx);
        tx = Binding();
        tx.vout.emplace_back(COIN, CScript{} << OP_RETURN);
        reject(tx);
    }

    void bindingCannotSendSeatOrChangeToAnotherWallet()
    {
        QString public_key;
        const auto reply{BindingReply(Binding())};
        QVERIFY(Rejected([&] { B3FlowMeshOperator::ParseBinding(reply, FnAsset(), [](const CTxDestination&) { return false; }, public_key); }));
        QVERIFY(Rejected([&] { B3FlowMeshOperator::ParseBinding(reply, FnAsset(), [](const CTxDestination& destination) {
            return destination == CTxDestination{Destination(0x22)}; // Seat owned, native change foreign.
        }, public_key); }));
        QVERIFY(Rejected([&] { B3FlowMeshOperator::ParseBinding(reply, FnAsset(), [](const CTxDestination& destination) {
            return destination == CTxDestination{Destination(0x55)}; // Native change owned, seat foreign.
        }, public_key); }));
    }

    void cleanupTestCase()
    {
        gArgs.ForceSetArg("-b3modernregtest", "0");
        SelectParams(ChainType::REGTEST);
    }
};

QTEST_GUILESS_MAIN(B3FlowMeshOperatorTests)
#include "b3flowmeshoptests.moc"
