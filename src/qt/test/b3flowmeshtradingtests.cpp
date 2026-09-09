// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <chainparams.h>
#include <flowmesh/market.h>
#include <key_io.h>
#include <qt/b3flowmeshtrading.h>
#include <core_io.h>
#include <rpc/util.h>
#include <util/translation.h>
#include <QTest>
#include <stdexcept>

const TranslateFn G_TRANSLATION_FUN{nullptr};
using namespace B3FlowMeshTrading;
namespace {
uint256 H(unsigned char n) { uint256 h; h.begin()[0] = n; return h; }
UniValue Snapshot(bool paused = false)
{
    const auto id{*flowmesh::ComputeFlowMeshMarketId(H(1), H(2))};
    UniValue v{UniValue::VOBJ};
    v.pushKV("market_id", id.GetHex()); v.pushKV("base_asset_id", H(2).GetHex());
    v.pushKV("vault_id", flowmesh::ComputeFlowMeshVaultId(H(1), id)->GetHex());
    v.pushKV("domain", H(1).GetHex()); v.pushKV("execution_config_id", H(3).GetHex());
    v.pushKV("quote_asset", "B3"); v.pushKV("available", true); v.pushKV("running", true);
    v.pushKV("paused", paused); v.pushKV("pending_handoff", false); v.pushKV("halt", "none"); v.pushKV("error", "");
    v.pushKV("checkpoint_pending", true); v.pushKV("pending_checkpoint_id", H(4).GetHex());
    UniValue a{UniValue::VOBJ}; a.pushKV("account_id", H(5).GetHex()); a.pushKV("next_sequence", 7);
    a.pushKV("base_available", MAX_MONEY); a.pushKV("base_reserved", 0);
    a.pushKV("b3_available", ValueFromAmount(MAX_MONEY)); a.pushKV("b3_reserved", ValueFromAmount(0)); v.pushKV("account", a);
    return v;
}
bool Rejected(const std::function<void()>& f) {
    try { f(); } catch (const std::exception&) { return true; } catch (const UniValue&) { return true; } return false;
}
Action Order() { Action a; a.market = ParseMarket(Snapshot()); a.side = QStringLiteral("bid"); a.price = 1000; a.amount = 1'000'000; return a; }
QString Address() { uint160 n; n.begin()[0] = 1; return QString::fromStdString(EncodeDestination(PKHash{n})); }
}
class B3FlowMeshTradingTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() { SelectParams(ChainType::REGTEST); }
    void exactTypedOrderAndSequence() {
        auto a{Order()}; const auto r{Parameters(a)};
        QCOMPARE(r.method, std::string{"submitflowmeshorder"}); QCOMPARE(r.params.size(), size_t{5});
        QVERIFY(r.params[2].isNum()); QCOMPARE(r.params[2].getInt<int64_t>(), int64_t{1000});
        QCOMPARE(r.params[3].getInt<int64_t>(), int64_t{1'000'000}); QCOMPARE(r.params[4].getInt<int64_t>(), int64_t{7});
        QVERIFY(Describe(a).contains(QStringLiteral("1.00 B3")));
        a.side = QStringLiteral("ask"); a.price = 1; a.amount = 9'007'199'254'740'993LL;
        QCOMPARE(Parameters(a).params[3].getInt<int64_t>(), a.amount);
        a.price = MAX_MONEY; QVERIFY(Rejected([&] { Parameters(a); }));
        a.price = 1; a.amount = -1; QVERIFY(Rejected([&] { Parameters(a); }));
    }
    void nativeAndAssetUnitsRemainDistinct() {
        auto a{Order()}; a.operation = Operation::Deposit; a.amount = 1'000'001;
        auto r{Parameters(a)}; QCOMPARE(r.method, std::string{"flowmeshdeposit"}); QCOMPARE(r.params[2].getInt<int64_t>(), a.amount);
        QVERIFY(!r.params[3]["broadcast"].get_bool()); QCOMPARE(r.params[3]["minconf"].getInt<int>(), 1);
        QVERIFY(!r.params[3]["include_unsafe"].get_bool()); QVERIFY(r.params[3]["market_bootstrap"].isNull());
        a.native = true; r = Parameters(a); QCOMPARE(r.params[1].get_str(), std::string{"B3"});
        QCOMPARE(r.params[2].write(), std::string{"0.001000001"});
        a.operation = Operation::Withdraw; a.destination = Address(); r = Parameters(a);
        QCOMPARE(r.method, std::string{"requestflowmeshwithdrawal"}); QCOMPARE(r.params[2].write(), std::string{"0.001000001"});
        QCOMPARE(r.params[4].getInt<int>(), 7);
    }
    void pausedGenesisOnlyAllowsPublication() {
        auto a{Order()}; a.market = ParseMarket(Snapshot(true)); QVERIFY(!a.market.ready); QVERIFY(a.market.publish_ready);
        QVERIFY(Rejected([&] { Parameters(a); }));
        a.operation = Operation::Deposit; QVERIFY(Rejected([&] { Parameters(a); }));
        a.operation = Operation::Checkpoint; const auto r{Parameters(a)};
        QCOMPARE(r.method, std::string{"createflowmeshcheckpoint"}); QVERIFY(!r.params[1]["broadcast"].get_bool());
        CheckSameMarket(a.market, a.market, false, true);
        QVERIFY(Rejected([&] { CheckSameMarket(a.market, a.market, false); }));
    }
    void cancellationNeverCallsRpc() {
        auto a{Order()}; int calls{0}; const RpcCall rpc = [&](const auto&, const auto&) { ++calls; return UniValue{}; };
        QVERIFY(Rejected([&] { DispatchApproved(a, false, [] { return false; }, rpc); }));
        QVERIFY(Rejected([&] { DispatchApproved(a, true, [] { return true; }, rpc); })); QCOMPARE(calls, 0);
        bool cancelled{false};
        const RpcCall reads = [&](const std::string& method, const UniValue&) {
            ++calls; if (method == "listflowmeshmarkets") { UniValue rows{UniValue::VARR}; rows.push_back(Snapshot()); return rows; }
            if (method == "getflowmeshbalance") { cancelled = true; return Snapshot(); }
            throw std::runtime_error{"A cancelled action must never reach a write"};
        };
        QVERIFY(Rejected([&] { DispatchApproved(a, true, [&] { return cancelled; }, reads); })); QCOMPARE(calls, 2);
    }
    void changedSequenceAndCheckpointRequireNewReview() {
        auto a{Order()}; auto fresh{a.market}; ++fresh.sequence;
        QVERIFY(Rejected([&] { CheckSameMarket(a.market, fresh, true); }));
        a.operation = Operation::Checkpoint; int writes{0};
        const RpcCall rpc = [&](const std::string& method, const UniValue&) {
            UniValue v{Snapshot()}; v.pushKV("pending_checkpoint_id", H(9).GetHex());
            if (method == "listflowmeshmarkets") { UniValue rows{UniValue::VARR}; rows.push_back(v); return rows; }
            if (method == "getflowmeshbalance") return v;
            ++writes; return UniValue{};
        };
        QVERIFY(Rejected([&] { DispatchApproved(a, true, [] { return false; }, rpc); })); QCOMPARE(writes, 0);
    }
    void malformedIdentityAndSyncFailClosed() {
        auto v{Snapshot()}; v.pushKV("vault_id", H(99).GetHex()); QVERIFY(Rejected([&] { ParseMarket(v); }));
        UniValue chain{UniValue::VOBJ}; chain.pushKV("initialblockdownload", false); chain.pushKV("headers", 2); chain.pushKV("blocks", 1);
        QVERIFY(Rejected([&] { CheckSynced(chain); })); chain.pushKV("blocks", 2); CheckSynced(chain);
        chain.pushKV("initialblockdownload", true); QVERIFY(Rejected([&] { CheckSynced(chain); }));
    }
};
QTEST_GUILESS_MAIN(B3FlowMeshTradingTests)
#include "b3flowmeshtradingtests.moc"
