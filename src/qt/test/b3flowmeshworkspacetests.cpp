// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/b3flowmeshmarketdata.h>
#include <qt/b3flowmeshchart.h>
#include <qt/b3flowmeshtradingpanel.h>
#include <qt/b3theme.h>
#include <flowmesh/market.h>
#include <util/translation.h>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTest>
#include <QTimer>
#include <array>
#include <stdexcept>

const TranslateFn G_TRANSLATION_FUN{nullptr};
using namespace B3FlowMeshMarketData;
namespace {
uint256 H(unsigned char n) { uint256 h; h.begin()[0] = n; return h; }
UniValue CurveJson(const char* side, unsigned char owner = 5)
{
    const bool bid{std::string{side} == "bid"}; UniValue c{UniValue::VOBJ}; c.pushKV("account_id", H(owner).GetHex()); c.pushKV("side", side);
    c.pushKV("filled_quantity", 250'000); c.pushKV("remaining_quantity", 750'000); c.pushKV("reserved_amount", bid ? 750'000'000 : 750'000);
    UniValue points{UniValue::VARR};
    for (const auto [price, quantity] : {std::pair{bid ? 1000 : 999, bid ? 1'000'000 : 0}, std::pair{bid ? 1001 : 1000, bid ? 0 : 1'000'000}}) { UniValue p{UniValue::VOBJ}; p.pushKV("price", price); p.pushKV("quantity", quantity); points.push_back(p); }
    c.pushKV("points", points); return c;
}
UniValue Data(bool trade = true)
{
    UniValue v{UniValue::VOBJ}; const auto market{*flowmesh::ComputeFlowMeshMarketId(H(1), H(2))};
    v.pushKV("market_id", market.GetHex()); v.pushKV("domain", H(1).GetHex()); v.pushKV("base_asset_id", H(2).GetHex()); v.pushKV("execution_config_id", H(3).GetHex());
    v.pushKV("quote_asset", "B3"); v.pushKV("matching_model", "uniform-price-curve-auction"); v.pushKV("quantity_lot_raw", 1); v.pushKV("price_tick_raw", 1); v.pushKV("unchanged", false);
    UniValue snapshot{UniValue::VOBJ}; snapshot.pushKV("certified", true); snapshot.pushKV("next_microblock_sequence", 3); snapshot.pushKV("last_microblock_hash", H(4).GetHex()); snapshot.pushKV("state_root", H(6).GetHex()); snapshot.pushKV("epoch", 0); snapshot.pushKV("active_seats", 4); snapshot.pushKV("quorum_required", 3); snapshot.pushKV("running", true); snapshot.pushKV("paused", false); snapshot.pushKV("pending_handoff", false); snapshot.pushKV("observer_only", true); snapshot.pushKV("halt", "none"); snapshot.pushKV("error", ""); snapshot.pushKV("pending_actions", 0); v.pushKV("snapshot", snapshot);
    UniValue metadata{UniValue::VOBJ}; metadata.pushKV("known", true); metadata.pushKV("decimals", 6); metadata.pushKV("ticker", "tUSD"); metadata.pushKV("name", "Synthetic test dollar"); metadata.pushKV("source", "isolated-test-fixture"); metadata.pushKV("test_only", true); v.pushKV("base_metadata", metadata);
    UniValue liquidity{UniValue::VOBJ}; UniValue curves{UniValue::VARR}; curves.push_back(CurveJson("bid")); curves.push_back(CurveJson("ask", 7)); liquidity.pushKV("curves", curves); liquidity.pushKV("total_curves", 2); liquidity.pushKV("complete", true); v.pushKV("liquidity", liquidity);
    UniValue account{UniValue::VOBJ}; account.pushKV("account_id", H(5).GetHex()); account.pushKV("next_sequence", 8); account.pushKV("base_available", 2'000'000); account.pushKV("base_reserved", 0); account.pushKV("b3_available_atoms", 2'000'000'000); account.pushKV("b3_reserved_atoms", 750'000'000); UniValue own{UniValue::VARR}; own.push_back(CurveJson("bid")); account.pushKV("curves", own); v.pushKV("account", account);
    UniValue history{UniValue::VOBJ}; history.pushKV("available", true); history.pushKV("truncated", false); history.pushKV("scope", "bounded-certified-log-cache"); UniValue entries{UniValue::VARR};
    if (trade) { UniValue t{UniValue::VOBJ}; t.pushKV("sequence", 2); t.pushKV("microblock_hash", H(4).GetHex()); t.pushKV("epoch", 0); t.pushKV("anchor_height", 50); t.pushKV("cleared", true); t.pushKV("price", 1000); t.pushKV("quantity", 250'000); t.pushKV("notional_atoms", 250'000'000); t.pushKV("fee_atoms", 25'000); t.pushKV("account_fills_known", true); t.pushKV("account_bid_fill", 250'000); t.pushKV("account_ask_fill", 0); entries.push_back(t); }
    history.pushKV("entries", entries); v.pushKV("history", history); return v;
}
bool Rejects(const std::function<void()>& f) { try { f(); } catch (const std::exception&) { return true; } catch (const UniValue&) { return true; } return false; }
}

class B3FlowMeshWorkspaceTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void tokenUnitsAreExactAndReadable()
    {
        const auto units{Parse(Data()).units}; QString error;
        QCOMPARE(ParseQuantity(QStringLiteral("1"), units, &error).value(), CAmount{1'000'000});
        QCOMPARE(ParseQuantity(QStringLiteral("0.000001"), units).value(), CAmount{1});
        QCOMPARE(ParsePrice(QStringLiteral("1"), units).value(), CAmount{1000});
        QCOMPARE(ParsePrice(QStringLiteral("0.001"), units).value(), CAmount{1});
        QCOMPARE(FormatPrice(1000, 6), QStringLiteral("1")); QCOMPARE(FormatAmount(1'250'001, 6), QStringLiteral("1.250001"));
        QCOMPARE(Notional(1000, 1'000'000).value(), CAmount{1'000'000'000}); QCOMPARE(FeeExample(1'000'000'000).value(), CAmount{100'000});
        QCOMPARE(ParseQuantity(QStringLiteral("9007199254.740993"), units).value(), CAmount{9'007'199'254'740'993LL});
        QVERIFY(!Notional(MAX_MONEY, 2));
    }
    void offGridOrAmbiguousInputsNeverRound()
    {
        auto u{Parse(Data()).units};
        for (const auto& text : {QStringLiteral("0.0000001"), QStringLiteral("-1"), QStringLiteral("+1"), QStringLiteral("1e6"), QStringLiteral(" 1"), QStringLiteral("1 "), QStringLiteral("1."), QStringLiteral("NaN"), QStringLiteral("0")}) QVERIFY2(!ParseQuantity(text, u), qPrintable(text));
        QVERIFY(!ParsePrice(QStringLiteral("0.0001"), u));
        u.decimals = 18; QCOMPARE(ParseQuantity(QStringLiteral("0.000000000000000001"), u).value(), CAmount{1});
        QCOMPARE(ParsePrice(QStringLiteral("1000000000"), u).value(), CAmount{1}); QVERIFY(!ParsePrice(QStringLiteral("1000000001"), u));
        QCOMPARE(FormatPrice(MAX_MONEY, 18), QString::number(MAX_MONEY) + QStringLiteral("000000000"));
        u.known = false; QVERIFY(!ParseQuantity(QStringLiteral("1"), u)); QVERIFY(!ParsePrice(QStringLiteral("1"), u));
    }
    void realCurvesAreNotSummedLikeAnOrderBook()
    {
        const auto s{Parse(Data())}; QCOMPARE(s.curves.size(), size_t{2}); QCOMPARE(s.depth.size(), size_t{3});
        QCOMPARE(s.depth[1].price, CAmount{1000}); QCOMPARE(s.depth[1].demand, CAmount{750'000}); QCOMPARE(s.depth[1].supply, CAmount{750'000});
        QCOMPARE(s.own_curves.size(), size_t{1}); QCOMPARE(s.history.size(), size_t{1}); QCOMPARE(s.history[0].own_buy, CAmount{250'000});
        auto malformed{s.curves}; malformed[0].points.clear(); QVERIFY(Rejects([&] { Aggregate(malformed); }));
    }
    void fundingSelectionCannotRelabelAnOrderQuantity()
    {
        B3FlowMeshTrading::Action a; const auto s{Parse(Data())}; a.market.id = s.market; a.market.base = s.base; a.market.account = s.account; a.market.has_account = true;
        a.side = QStringLiteral("bid"); a.price = 1000; a.amount = 1'000'000; a.native = true; a.display_decimals = 6; a.display_ticker = QStringLiteral("tUSD");
        const auto text{B3FlowMeshTrading::Describe(a)}; QVERIFY(text.contains(QStringLiteral("1 tUSD (1000000 atomic units)"))); QVERIFY(text.contains(QStringLiteral("1 B3 / tUSD")));
    }
    void malformedOrUncertifiedRowsFailClosed()
    {
        auto v{Data()}; v.pushKV("matching_model", "price-time-book"); QVERIFY(Rejects([&] { Parse(v); }));
        v = Data(); v.pushKV("market_id", H(90).GetHex()); QVERIFY(Rejects([&] { Parse(v); }));
        v = Data(); auto meta{v["base_metadata"]}; meta.pushKV("decimals", 19); v.pushKV("base_metadata", meta); QVERIFY(Rejects([&] { Parse(v); }));
        v = Data(); auto hist{v["history"]}; auto entries{hist["entries"]}; auto trade{entries[0]}; trade.pushKV("notional_atoms", 1); entries.setArray(); entries.push_back(trade); hist.pushKV("entries", entries); v.pushKV("history", hist); QVERIFY(Rejects([&] { Parse(v); }));
        v = Data(); auto snap{v["snapshot"]}; snap.pushKV("certified", false); v.pushKV("snapshot", snap); QVERIFY(Rejects([&] { Parse(v); }));
    }
    void staleAndPausedGatesDoNotMisdiagnoseIdleMarkets()
    {
        auto s{Parse(Data(false))}; QVERIFY(AdmissionReady(s, 100, 600'000));
        s.pending_actions = 1;
        QVERIFY(AdmissionReady(s, 100, 600'000, 29'999));
        QVERIFY(!AdmissionReady(s, 100, 600'000, 30'000));
        QVERIFY(StatusText(s, 100, 600'000, 30'000).contains(QStringLiteral("not canceled")));
        s.pending_actions = 0;
        QVERIFY(AdmissionReady(s, 100, 600'000, 600'000));
        QVERIFY(!AdmissionReady(s, 3001, 10)); QVERIFY(StatusText(s, 3001, 10).contains(QStringLiteral("stale")));
        s.paused = true; QVERIFY(!AdmissionReady(s, 10, 10)); QVERIFY(StatusText(s, 10, 10).contains(QStringLiteral("paused")));
        s.paused = false; s.handoff = true; QVERIFY(!AdmissionReady(s, 10, 10));
        s.handoff = false; s.quorum_required = 2; QVERIFY(!AdmissionReady(s, 10, 10));
        s.quorum_required = 3; s.certified = false; QVERIFY(!AdmissionReady(s, 10, 10));
    }
    void unchangedReplyContainsNoInventedEmptyHistory()
    {
        const auto full{Data()}; UniValue small{UniValue::VOBJ};
        for (const auto* key : {"market_id", "base_asset_id", "domain", "execution_config_id", "quote_asset", "matching_model", "quantity_lot_raw", "price_tick_raw", "snapshot"}) small.pushKV(key, full[key]);
        small.pushKV("unchanged", true); const auto s{Parse(small)}; QVERIFY(s.unchanged); QVERIFY(!s.units.known); QVERIFY(s.history.empty());
    }
    void chartShowsOnlyCertifiedRecordsAndHonestEmptiness()
    {
        B3FlowMeshChart chart; chart.resize(640, 340); QCOMPARE(chart.pricePointCount(), 0); chart.setLoading(true); QVERIFY(chart.emptyMessage().contains(QStringLiteral("Reading")));
        chart.setSnapshot(Parse(Data(false))); QCOMPARE(chart.pricePointCount(), 0); QVERIFY(chart.emptyMessage().contains(QStringLiteral("No certified trades")));
        chart.setSnapshot(Parse(Data())); QCOMPARE(chart.pricePointCount(), 1); QImage image{chart.size(), QImage::Format_ARGB32}; image.fill(Qt::transparent); chart.render(&image); QVERIFY(!image.isNull());
        chart.setMode(B3FlowMeshChart::Mode::Liquidity); chart.setStale(true); chart.render(&image); chart.setSnapshot(std::nullopt); QCOMPARE(chart.pricePointCount(), 0);
    }
    void walletlessWorkspaceRendersDataButCanNeverSubmit()
    {
        B3FlowMeshTradingPanel panel; panel.resize(1200, 850); panel.m_timer->stop();
        const auto s{Parse(Data())}; B3FlowMeshTrading::Market m; m.id = s.market; m.base = s.base; m.domain = s.domain; m.config = s.config; m.vault = QString::fromStdString(flowmesh::ComputeFlowMeshVaultId(H(1), *uint256::FromHex(s.market.toStdString()))->GetHex()); m.account = s.account; m.has_account = true; m.ready = true; m.publish_ready = true; m.base_available = s.base_available; m.b3_available = s.b3_available;
        m.base_reserved = s.base_reserved; m.b3_reserved = s.b3_reserved;
        panel.m_market_data = {m}; { QSignalBlocker block{panel.m_market}; panel.m_market->addItem(QStringLiteral("tUSD / B3"), m.id); } panel.m_snapshot = s; panel.m_response_age.start(); panel.updateDataViews(); panel.updateMarketText();
        QCOMPARE(panel.m_chart->pricePointCount(), 1); QCOMPARE(panel.m_history_view->rowCount(), 1); QCOMPARE(panel.m_own_view->rowCount(), 1);
        auto* prices{panel.findChild<QPushButton*>(QStringLiteral("flowMeshChartPrices"))};
        auto* liquidity{panel.findChild<QPushButton*>(QStringLiteral("flowMeshChartLiquidity"))};
        QVERIFY(prices); QVERIFY(liquidity); QVERIFY(prices->isChecked());
        liquidity->click(); QVERIFY(liquidity->isChecked()); QVERIFY(!prices->isChecked());
        prices->click(); QVERIFY(prices->isChecked()); QVERIFY(!liquidity->isChecked());
        QCOMPARE(panel.m_own_view->item(0, 3)->text(), QStringLiteral("0.75 B3"));
        QVERIFY(panel.m_balances->text().contains(QStringLiteral("Reserved  0 tUSD · 0.75 B3")));
        panel.m_price->setText(QStringLiteral("1")); panel.m_quantity->setText(QStringLiteral("1")); QVERIFY(panel.m_ticket_total->text().contains(QStringLiteral("1 B3"))); QVERIFY(!panel.m_order->isEnabled()); QVERIFY(!panel.m_deposit->isEnabled()); QVERIFY(!panel.m_advanced->isVisible());
        panel.m_read_failed = true; panel.updateMarketText(); QVERIFY(panel.m_status->text().contains(QStringLiteral("stale"))); QVERIFY(!panel.m_order->isEnabled());
        panel.m_queue_age.start(); QVERIFY(panel.m_queue_age.isValid());
        panel.setWalletModel(nullptr); QVERIFY(!panel.m_snapshot); QVERIFY(!panel.m_queue_age.isValid()); QCOMPARE(panel.m_chart->pricePointCount(), 0); QCOMPARE(panel.m_history_view->rowCount(), 0); QVERIFY(!panel.m_order->isEnabled());
    }
    void exportOptInSyntheticVisualFixtures()
    {
        const QString directory{qEnvironmentVariable("FLOWMESH_UI_SNAPSHOT_DIR")};
        if (directory.isEmpty()) QSKIP("Set FLOWMESH_UI_SNAPSHOT_DIR to export explicitly synthetic, walletless QA PNGs.");
        QVERIFY(QDir::isAbsolutePath(directory)); QDir output{directory}; QVERIFY(output.mkpath(QStringLiteral(".")));
        auto s{Parse(Data())}; s.next_sequence = 9; s.pending_actions = 2; s.history.clear();
        for (int i{0}; i < 7; ++i) {
            Trade t; t.sequence = i + 2; t.hash = QString::fromStdString(H(30 + i).GetHex()); t.epoch = 0; t.anchor_height = 50; t.cleared = true; t.price = 1000 + std::array<int, 7>{0, 4, 2, 6, 3, 7, 9}[i]; t.quantity = 100'000 + i * 25'000; t.notional = t.price * t.quantity; t.fee = *FeeExample(t.notional); t.own_fills_known = true; t.own_buy = i % 2 ? 0 : t.quantity / 2; t.own_sell = 0; s.history.push_back(t);
        }
        B3FlowMeshTradingPanel panel; panel.resize(1320, 940); panel.m_timer->stop();
        B3FlowMeshTrading::Market m; m.id = s.market; m.base = s.base; m.domain = s.domain; m.config = s.config; m.vault = QString::fromStdString(flowmesh::ComputeFlowMeshVaultId(H(1), *uint256::FromHex(s.market.toStdString()))->GetHex()); m.account = s.account; m.has_account = true; m.ready = true; m.publish_ready = true; m.sequence = s.account_sequence; m.base_available = s.base_available; m.b3_available = s.b3_available;
        m.base_reserved = s.base_reserved; m.b3_reserved = s.b3_reserved;
        panel.m_market_data = {m}; { QSignalBlocker block{panel.m_market}; panel.m_market->addItem(QStringLiteral("tUSD / B3 — SYNTHETIC QA"), m.id); }
        panel.m_wallet_name = QStringLiteral("SYNTHETIC QA · no wallet / no signing"); panel.m_snapshot = s; panel.m_response_age.start(); panel.m_certificate_age.start(); panel.m_pending_market = s.market; panel.m_pending_account = s.account; panel.m_pending_sequence = s.account_sequence;
        panel.m_price->setText(QStringLiteral("1.009")); panel.m_quantity->setText(QStringLiteral("0.25")); panel.updateDataViews(); panel.updateMarketText(); panel.show(); QCoreApplication::processEvents();
        QCOMPARE(panel.m_own_view->item(0, 3)->text(), QStringLiteral("0.75 B3"));
        QVERIFY(panel.m_balances->text().contains(QStringLiteral("Reserved  0 tUSD · 0.75 B3")));
        const auto save = [&](const QString& name) { QImage image{panel.size(), QImage::Format_ARGB32}; image.fill(Qt::transparent); panel.render(&image); return image.save(output.filePath(name)); };
        QVERIFY(save(QStringLiteral("synthetic-flowmesh-pending.png")));
        auto* liquidity{panel.findChild<QPushButton*>(QStringLiteral("flowMeshChartLiquidity"))};
        QVERIFY(liquidity); liquidity->click(); QVERIFY(liquidity->isChecked());
        QVERIFY(save(QStringLiteral("synthetic-flowmesh-liquidity.png")));
        panel.m_snapshot->paused = true; panel.m_market_data[0].ready = false; panel.m_market_data[0].checkpoint_pending = true; panel.m_market_data[0].checkpoint = QString::fromStdString(H(80).GetHex()); panel.updateMarketText();
        QVERIFY(save(QStringLiteral("synthetic-flowmesh-paused.png")));
        panel.m_read_failed = true; panel.updateMarketText(); QVERIFY(save(QStringLiteral("synthetic-flowmesh-stale.png")));
        panel.hide(); QVERIFY(!panel.m_order->isEnabled()); QVERIFY(!panel.m_wallet); // Fixture cannot submit.
    }
};
int main(int argc, char** argv) { QApplication app{argc, argv}; B3Theme::apply(app); B3FlowMeshWorkspaceTests tests; return QTest::qExec(&tests, argc, argv); }
#include "b3flowmeshworkspacetests.moc"
