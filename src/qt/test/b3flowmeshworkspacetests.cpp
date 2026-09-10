// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/b3flowmeshmarketdata.h>
#include <qt/b3flowmeshchart.h>
#include <qt/b3flowmeshtradingpanel.h>
#include <qt/b3theme.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <qt/clientmodel.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <flowmesh/market.h>
#include <test/util/setup_common.h>
#include <wallet/context.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPersistentModelIndex>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

const std::function<void(const std::string&)> G_TEST_LOG_FUN{};
const std::function<std::vector<const char*>()> G_TEST_COMMAND_LINE_ARGUMENTS{};
const std::function<std::string()> G_TEST_GET_FULL_NAME{[] { return "b3_flowmesh_workspace"; }};
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
    std::unique_ptr<TestingSetup> m_setup;
    std::unique_ptr<interfaces::Node> m_node;
    std::unique_ptr<interfaces::WalletLoader> m_loader;
    std::unique_ptr<OptionsModel> m_options;
    std::unique_ptr<ClientModel> m_client;
    std::unique_ptr<const PlatformStyle> m_style;
    std::shared_ptr<wallet::CWallet> m_wallet;
    std::unique_ptr<WalletModel> m_model;

    void AttachOfflineWallet(B3FlowMeshTradingPanel& panel)
    {
        // Assign the isolated model directly through the existing friend test
        // seam: public setWalletModel intentionally starts a real RPC refresh.
        panel.m_timer->stop();
        panel.m_wallet = m_model.get();
        panel.m_backend = interfaces::MakeWallet(*m_loader->context(), m_wallet);
        panel.m_wallet_name = QStringLiteral("offline-display-test");
        panel.m_cancel->store(false);
        Observe(panel, Parse(Data()));
        panel.m_price->setText(QStringLiteral("1"));
        panel.m_quantity->setText(QStringLiteral("1"));
    }

    static void ReadInFlight(B3FlowMeshTradingPanel& panel)
    {
        // A never-started, owned thread models the in-flight read marker. No
        // RPC, sockets or clock sleeps are needed to exercise GUI scheduling.
        panel.m_thread = new QThread{&panel};
        panel.m_busy = false;
        panel.updateControls();
    }

    static void Observe(B3FlowMeshTradingPanel& panel, const Snapshot& snapshot)
    {
        panel.m_timer->stop();
        B3FlowMeshTrading::Market market;
        market.id = snapshot.market; market.base = snapshot.base;
        market.domain = snapshot.domain; market.config = snapshot.config;
        market.vault = QString::fromStdString(flowmesh::ComputeFlowMeshVaultId(
            H(1), *uint256::FromHex(snapshot.market.toStdString()))->GetHex());
        market.account = snapshot.account; market.has_account = true;
        market.sequence = snapshot.account_sequence; market.ready = true;
        market.publish_ready = true;
        market.base_available = snapshot.base_available; market.base_reserved = snapshot.base_reserved;
        market.b3_available = snapshot.b3_available; market.b3_reserved = snapshot.b3_reserved;
        panel.m_market_data = {market};
        {
            QSignalBlocker blocked{panel.m_market};
            panel.m_market->clear();
            panel.m_market->addItem(QStringLiteral("tUSD / B3 — synthetic test"), market.id);
        }
        panel.m_snapshot = snapshot;
        panel.m_response_age.start();
        panel.updateDataViews();
        panel.updateMarketText();
    }

private Q_SLOTS:
    void initTestCase()
    {
        Q_INIT_RESOURCE(bitcoin);
        m_setup = std::make_unique<TestingSetup>(ChainType::REGTEST,
            TestOpts{.extra_args = {"-keypool=1"}, .setup_net = false});
        m_node = interfaces::MakeNode(m_setup->m_node);
        m_loader = interfaces::MakeWalletLoader(*m_setup->m_node.chain, *m_setup->m_node.args);
        m_setup->m_node.wallet_loader = m_loader.get();
        m_options = std::make_unique<OptionsModel>(*m_node);
        bilingual_str error;
        QVERIFY2(m_options->Init(error), error.original.c_str());
        m_client = std::make_unique<ClientModel>(*m_node, m_options.get());
        m_style.reset(PlatformStyle::instantiate("other"));
        QVERIFY(m_style);
        m_wallet = std::make_shared<wallet::CWallet>(m_setup->m_node.chain.get(),
            "offline-display-test", wallet::CreateMockableWalletDatabase());
        {
            LOCK(m_wallet->cs_wallet);
            m_wallet->SetLastBlockProcessed(0, m_setup->m_node.chain->getBlockHash(0));
            m_wallet->m_keypool_size = 1;
            m_wallet->SetWalletFlag(wallet::WALLET_FLAG_DESCRIPTORS);
            m_wallet->SetupDescriptorScriptPubKeyMans();
        }
        {
            LOCK(m_loader->context()->wallets_mutex);
            m_loader->context()->wallets.push_back(m_wallet);
        }
        m_model = std::make_unique<WalletModel>(
            interfaces::MakeWallet(*m_loader->context(), m_wallet), *m_client, m_style.get());
        QVERIFY(m_model->setWalletEncrypted(SecureString{"offline display fixture passphrase"}));
        QVERIFY(m_model->setWalletLocked(true));
    }

    void init()
    {
        QVERIFY(m_model->setWalletLocked(true));
        QVERIFY(m_wallet->IsLocked());
    }

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
        chart.setSnapshot(Parse(Data(false))); QCOMPARE(chart.pricePointCount(), 0);
        QVERIFY(chart.emptyMessage().contains(QStringLiteral("No trades yet")));
        QVERIFY(chart.emptyMessage().contains(QStringLiteral("history window")));
        auto unavailable{Parse(Data(false))}; unavailable.history_available = false;
        chart.setSnapshot(unavailable); QCOMPARE(chart.pricePointCount(), 0);
        QVERIFY(chart.emptyMessage().contains(QStringLiteral("unavailable")));
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
        QVERIFY(panel.m_balances->text().contains(QStringLiteral("In orders  0 tUSD · 0.75 B3")));
        panel.m_price->setText(QStringLiteral("1")); panel.m_quantity->setText(QStringLiteral("1")); QVERIFY(panel.m_ticket_total->text().contains(QStringLiteral("1 B3"))); QVERIFY(!panel.m_order->isEnabled()); QVERIFY(!panel.m_deposit->isEnabled()); QVERIFY(!panel.m_advanced->isVisible());
        panel.m_read_failed = true; panel.updateMarketText();
        QVERIFY(panel.m_status->text().contains(QStringLiteral("Updates delayed")));
        QVERIFY(panel.m_status->text().contains(QStringLiteral("paused")));
        QVERIFY(panel.m_status->toolTip().contains(QStringLiteral("stale")));
        QVERIFY(!panel.m_order->isEnabled());
        panel.m_queue_age.start(); QVERIFY(panel.m_queue_age.isValid());
        panel.setWalletModel(nullptr); QVERIFY(!panel.m_snapshot); QVERIFY(!panel.m_queue_age.isValid()); QCOMPARE(panel.m_chart->pricePointCount(), 0); QCOMPARE(panel.m_history_view->rowCount(), 0); QVERIFY(!panel.m_order->isEnabled());
    }
    void identicalSnapshotsPreserveCellsSelectionAndScroll()
    {
        // Synthetic display data only. A long retained history makes a real,
        // nonzero scroll position observable without any wallet or RPC.
        auto snapshot{Parse(Data())};
        const auto trade{snapshot.history.front()};
        snapshot.history.clear();
        for (uint64_t i{0}; i < 40; ++i) {
            auto entry{trade}; entry.sequence = i + 2;
            entry.hash = QString::fromStdString(H(static_cast<unsigned char>(i + 20)).GetHex());
            snapshot.history.push_back(entry);
        }
        snapshot.next_sequence = 42;
        snapshot.head = snapshot.history.back().hash;
        B3FlowMeshTradingPanel panel;
        panel.resize(1200, 850);
        Observe(panel, snapshot);
        panel.m_history_view->setFixedHeight(160);
        panel.m_history_view->setSelectionMode(QAbstractItemView::SingleSelection);
        panel.m_history_view->setSelectionBehavior(QAbstractItemView::SelectRows);
        panel.m_activity->setCurrentWidget(panel.m_history_view->parentWidget());
        panel.show();
        QCoreApplication::processEvents();
        QCOMPARE(panel.m_history_view->rowCount(), 40);
        panel.m_history_view->setCurrentCell(20, 1);
        panel.m_history_view->selectRow(20);
        auto* scroll{panel.m_history_view->verticalScrollBar()};
        QVERIFY(scroll->maximum() > 0);
        scroll->setValue(std::max(1, scroll->maximum() / 2));
        const int position{scroll->value()};
        QVERIFY(position > 0);
        const QPersistentModelIndex selected{panel.m_history_view->model()->index(20, 1)};
        const auto selection{panel.m_history_view->selectionModel()->selectedIndexes()};
        QVERIFY(!selection.isEmpty());
        const std::array<QTableWidgetItem*, 3> cells{
            panel.m_depth_view->item(0, 0), panel.m_history_view->item(20, 1), panel.m_own_view->item(0, 0)};
        for (auto* cell : cells) QVERIFY(cell);
        QSignalSpy removed{panel.m_history_view->model(), &QAbstractItemModel::rowsRemoved};
        QSignalSpy inserted{panel.m_history_view->model(), &QAbstractItemModel::rowsInserted};
        QSignalSpy reset{panel.m_history_view->model(), &QAbstractItemModel::modelReset};
        for (int refresh{0}; refresh < 5; ++refresh) {
            panel.m_snapshot = snapshot; // Equal full response, not pointer reuse.
            panel.m_response_age.restart();
            panel.updateDataViews();
            panel.updateMarketText();
        }
        QCoreApplication::processEvents();
        QCOMPARE(panel.m_depth_view->item(0, 0), cells[0]);
        QCOMPARE(panel.m_history_view->item(20, 1), cells[1]);
        QCOMPARE(panel.m_own_view->item(0, 0), cells[2]);
        QVERIFY(selected.isValid());
        QCOMPARE(panel.m_history_view->currentIndex(), QModelIndex{selected});
        QCOMPARE(panel.m_history_view->selectionModel()->selectedIndexes(), selection);
        QCOMPARE(scroll->value(), position);
        QCOMPARE(removed.count(), 0); QCOMPARE(inserted.count(), 0); QCOMPARE(reset.count(), 0);
    }
    void backgroundReadKeepsReviewControlsStableAndQueuesOnlyReview()
    {
        B3FlowMeshTradingPanel panel;
        AttachOfflineWallet(panel);
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        ReadInFlight(panel);
        QThread* const read{panel.m_thread};
        for (int poll{0}; poll < 5; ++poll) {
            panel.updateControls();
            for (auto* button : {panel.m_order, panel.m_cancel_order, panel.m_deposit, panel.m_withdraw, panel.m_admit}) QVERIFY(button->isEnabled());
            QVERIFY(panel.m_price->isEnabled()); QVERIFY(panel.m_quantity->isEnabled());
            QVERIFY(panel.m_buy->isEnabled()); QVERIFY(panel.m_sell->isEnabled());
        }
        panel.m_order->click();
        QVERIFY(panel.m_deferred_review);
        QCOMPARE(panel.m_deferred_review->operation, B3FlowMeshTrading::Operation::Order);
        QVERIFY(!panel.m_deferred_review->funding);
        QCOMPARE(panel.m_deferred_review->generation, panel.m_generation);
        QCOMPARE(panel.m_deferred_review->market, panel.m_snapshot->market);
        QVERIFY(panel.m_busy); QVERIFY(!panel.m_price->isEnabled()); QVERIFY(!panel.m_quantity->isEnabled());
        QVERIFY(!panel.m_order->isEnabled()); QVERIFY(!panel.m_confirmation); QVERIFY(!panel.m_funding_dialog);
        QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_unlock); QCOMPARE(unlock.count(), 0);
        // A repeated click or direct action dispatch cannot replace the first
        // review intent or start a mutation while the read marker exists.
        panel.begin(B3FlowMeshTrading::Operation::Cancel);
        panel.openFunding(true);
        B3FlowMeshTrading::Action action;
        action.market = panel.m_market_data.front();
        panel.startJob(action);
        QCOMPARE(panel.m_deferred_review->operation, B3FlowMeshTrading::Operation::Order);
        QCOMPARE(panel.m_thread, read); QVERIFY(!panel.m_active_result);
        panel.cancelAndWait();
        QVERIFY(!panel.m_thread); QVERIFY(!panel.m_deferred_review);
        QVERIFY(!panel.m_confirmation); QVERIFY(!panel.m_unlock);
        QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
    }
    void fundingClickDuringReadQueuesOnlyDialogOpening()
    {
        B3FlowMeshTradingPanel panel;
        AttachOfflineWallet(panel);
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        ReadInFlight(panel);
        panel.m_deposit->click();
        QVERIFY(panel.m_deferred_review);
        QCOMPARE(panel.m_deferred_review->operation, B3FlowMeshTrading::Operation::Deposit);
        QVERIFY(panel.m_deferred_review->funding);
        QVERIFY(panel.m_busy); QVERIFY(!panel.m_funding_dialog); QVERIFY(!panel.m_confirmation);
        QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_unlock);
        panel.cancelAndWait();
        QVERIFY(!panel.m_deferred_review); QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
    }
    void deferredVaultReviewCannotFollowEffectFallback()
    {
        for (bool missing_identity : {false, true}) {
            B3FlowMeshTradingPanel panel;
            AttachOfflineWallet(panel);
            QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
            for (int i{0}; i < 2; ++i) {
                const QString id{QString::fromStdString(H(static_cast<unsigned char>(70 + i)).GetHex())};
                UniValue effect{UniValue::VOBJ};
                effect.pushKV("market_id", panel.m_snapshot->market.toStdString());
                effect.pushKV("account_id", panel.m_snapshot->account.toStdString());
                effect.pushKV("asset", panel.m_snapshot->base.toStdString());
                effect.pushKV("effect_id", id.toStdString());
                effect.pushKV("kind", "deposit-sweep"); effect.pushKV("amount", 1);
                panel.m_effect_data.push_back(effect);
                panel.m_effect->addItem(id, i);
                panel.m_effect->setItemData(i, id, Qt::UserRole + 1);
            }
            ReadInFlight(panel);
            QVERIFY(panel.m_publish->isEnabled());
            panel.m_publish->click();
            QVERIFY(panel.m_deferred_review);
            QCOMPARE(panel.m_deferred_review->effect, panel.m_effect->itemData(0, Qt::UserRole + 1).toString());
            panel.stopWorker(); panel.m_busy = false;
            if (missing_identity) panel.m_effect->setItemData(0, QVariant{}, Qt::UserRole + 1);
            else panel.m_effect->setCurrentIndex(1);
            panel.updateControls();
            QVERIFY(panel.m_publish->isEnabled()); // A new review is still possible.
            bool opened{false};
            QTimer::singleShot(0, &panel, [&] {
                if (panel.m_confirmation) { opened = true; panel.m_confirmation->done(QMessageBox::Cancel); }
            });
            panel.resumeReview();
            QVERIFY(!opened); QVERIFY(!panel.m_deferred_review); QVERIFY(!panel.m_confirmation);
            QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_unlock);
            QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
        }
    }
    void marketSwitchClearsPriorDataButSameMarketFailureRetainsIt()
    {
        B3FlowMeshTradingPanel panel;
        Observe(panel, Parse(Data()));
        auto* retained{panel.m_history_view->item(0, 0)};
        const QString balance{panel.m_balances->text()};
        QVERIFY(!balance.isEmpty());
        panel.m_read_failed = true;
        panel.updateMarketText();
        QVERIFY(panel.m_snapshot); QCOMPARE(panel.m_chart->pricePointCount(), 1);
        QCOMPARE(panel.m_history_view->item(0, 0), retained);
        QCOMPARE(panel.m_balances->text(), balance);
        auto other{panel.m_market_data.front()};
        other.base = QString::fromStdString(H(90).GetHex());
        other.id = QString::fromStdString(flowmesh::ComputeFlowMeshMarketId(H(1), H(90))->GetHex());
        other.vault = QString::fromStdString(flowmesh::ComputeFlowMeshVaultId(H(1), *uint256::FromHex(other.id.toStdString()))->GetHex());
        panel.m_market_data.push_back(other);
        panel.m_market->addItem(QStringLiteral("Other synthetic market"), other.id);
        panel.m_queue_age.start();
        panel.m_market->setCurrentIndex(1); // Exercise the real selection callback.
        QVERIFY(!panel.m_snapshot); QVERIFY(!panel.m_response_age.isValid());
        QVERIFY(!panel.m_queue_age.isValid()); QCOMPARE(panel.m_chart->pricePointCount(), 0);
        QCOMPARE(panel.m_depth_view->rowCount(), 0); QCOMPARE(panel.m_history_view->rowCount(), 0);
        QCOMPARE(panel.m_own_view->rowCount(), 0); QVERIFY(panel.m_balances->text().isEmpty());
        QVERIFY(!panel.m_order->isEnabled()); QVERIFY(!panel.m_thread); QVERIFY(!panel.m_wallet);
    }
    void backgroundReadDoesNotWeakenReadinessGuards()
    {
        for (int condition{0}; condition < 7; ++condition) {
            B3FlowMeshTradingPanel panel;
            AttachOfflineWallet(panel);
            ReadInFlight(panel);
            switch (condition) {
            case 0: panel.m_read_failed = true; break;
            case 1: panel.m_response_age.invalidate(); break;
            case 2: panel.m_uncertain = true; break;
            case 3: panel.m_security_warning = QStringLiteral("Unverified relock in isolated test"); break;
            case 4: panel.m_snapshot->paused = true; break;
            case 5: panel.m_snapshot->handoff = true; break;
            case 6: panel.m_snapshot->certified = false; break;
            }
            panel.updateControls();
            QVERIFY(!panel.m_busy); // Not merely disabled by a modal operation.
            for (auto* button : {panel.m_order, panel.m_cancel_order, panel.m_deposit, panel.m_withdraw, panel.m_admit}) QVERIFY(!button->isEnabled());
            QVERIFY(!panel.m_deferred_review); QVERIFY(!panel.m_active_result);
        }
    }
    void deferredReviewRequiresSuccessfulUnchangedContext_data()
    {
        QTest::addColumn<int>("condition");
        QTest::newRow("read failed") << 0;
        QTest::newRow("freshness lost") << 1;
        QTest::newRow("uncertain submission") << 2;
        QTest::newRow("security warning") << 3;
        QTest::newRow("paused snapshot") << 4;
        QTest::newRow("generation changed") << 5;
        QTest::newRow("market changed") << 6;
        QTest::newRow("account request still pending") << 7;
    }
    void deferredReviewRequiresSuccessfulUnchangedContext()
    {
        QFETCH(int, condition);
        B3FlowMeshTradingPanel panel;
        AttachOfflineWallet(panel);
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        ReadInFlight(panel);
        panel.m_order->click();
        QVERIFY(panel.m_deferred_review);
        panel.stopWorker();
        panel.m_busy = false;
        switch (condition) {
        case 0: panel.m_read_failed = true; break;
        case 1: panel.m_response_age.invalidate(); break;
        case 2: panel.m_uncertain = true; break;
        case 3: panel.m_security_warning = QStringLiteral("Unverified relock in isolated test"); break;
        case 4: panel.m_snapshot->paused = true; break;
        case 5: ++panel.m_generation; break;
        case 6: { QSignalBlocker blocked{panel.m_market}; panel.m_market->setCurrentIndex(-1); break; }
        case 7:
            panel.m_pending_market = panel.m_snapshot->market;
            panel.m_pending_account = panel.m_snapshot->account;
            panel.m_pending_sequence = panel.m_snapshot->account_sequence;
            break;
        }
        // If a regression opens a modal review, dismiss it without approving
        // anything; the explicit assertion then fails without hanging tests.
        bool opened{false};
        QTimer::singleShot(0, &panel, [&] {
            if (panel.m_confirmation) { opened = true; panel.m_confirmation->done(QMessageBox::Cancel); }
        });
        panel.resumeReview();
        QVERIFY(!opened); QVERIFY(!panel.m_deferred_review); QVERIFY(!panel.m_confirmation);
        QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_thread); QVERIFY(!panel.m_unlock);
        QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
        if (condition != 5) QVERIFY(!panel.m_order->isEnabled());
    }
    void successfulReadResumesExplicitReviewButNeverApprovesIt()
    {
        B3FlowMeshTradingPanel panel;
        AttachOfflineWallet(panel);
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        ReadInFlight(panel);
        panel.m_order->click();
        QVERIFY(panel.m_deferred_review);
        panel.stopWorker(); panel.m_busy = false; panel.m_response_age.restart();
        bool reviewed{false}, cancel_default{false};
        QTimer::singleShot(0, &panel, [&] {
            if (panel.m_confirmation) {
                reviewed = true;
                cancel_default = panel.m_confirmation->defaultButton() == panel.m_confirmation->button(QMessageBox::Cancel);
                panel.m_confirmation->done(QMessageBox::Cancel);
            }
        });
        panel.resumeReview();
        QVERIFY(reviewed); QVERIFY(cancel_default); QVERIFY(!panel.m_deferred_review); QVERIFY(!panel.m_confirmation);
        QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_thread); QVERIFY(!panel.m_unlock);
        QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
        panel.resumeReview(); // A consumed intent cannot replay.
        QVERIFY(!panel.m_confirmation); QVERIFY(!panel.m_active_result); QCOMPARE(unlock.count(), 0);
    }
    void technicalDetailsAreHiddenUntilExplicitlyRequested()
    {
        B3FlowMeshTradingPanel panel;
        panel.resize(1200, 850);
        Observe(panel, Parse(Data())); // No wallet: opening Details cannot start RPC.
        panel.show();
        QCoreApplication::processEvents();
        auto* details{panel.findChild<QPushButton*>(QStringLiteral("flowMeshDetails"))};
        QVERIFY(details); QVERIFY(details->isVisible()); QVERIFY(!details->isChecked());
        for (QWidget* technical : std::array<QWidget*, 7>{panel.m_advanced, panel.m_identity_detail,
             panel.m_progress, panel.m_grid_note, panel.m_refresh, panel.m_checkpoint, panel.m_publish}) QVERIFY(!technical->isVisible());
        QVERIFY(panel.m_chart->isVisible()); QVERIFY(panel.m_depth_view->isVisible());
        QVERIFY(panel.m_price->isVisible()); QVERIFY(panel.m_quantity->isVisible());
        QVERIFY(panel.m_deposit->isVisible()); QVERIFY(panel.m_order->isVisible());
        QVERIFY(panel.m_balances->isVisible()); QVERIFY(panel.m_own_view->isVisible());
        // Simplifying the default view must not hide the test-asset warning.
        QVERIFY(panel.m_status->isVisible());
        QVERIFY(panel.m_status->text().contains(QStringLiteral("test"), Qt::CaseInsensitive));
        QVERIFY(panel.m_status->text().contains(QStringLiteral("unbacked"), Qt::CaseInsensitive));
        details->click();
        QCoreApplication::processEvents();
        QVERIFY(details->isChecked()); QVERIFY(panel.m_advanced->isVisible());
        QVERIFY(panel.m_identity_detail->isVisible()); QVERIFY(panel.m_grid_note->isVisible());
        QVERIFY(panel.m_refresh->isVisible()); QVERIFY(panel.m_checkpoint->isVisible());
        QVERIFY(panel.m_publish->isVisible()); QVERIFY(!panel.m_thread);
        details->click();
        QVERIFY(!panel.m_advanced->isVisible()); QVERIFY(!panel.m_refresh->isVisible());
        QVERIFY(!panel.m_order->isEnabled()); QVERIFY(!panel.m_wallet);
    }
    void conciseStatusStillDisclosesPausedUnknownAndPartialData()
    {
        auto snapshot{Parse(Data())};
        snapshot.curves_complete = false;
        snapshot.history_page_partial = true;
        B3FlowMeshTradingPanel panel;
        Observe(panel, snapshot);
        QVERIFY(panel.m_liquidity_note->text().contains(QStringLiteral("Partial")));
        QVERIFY(panel.m_history_note->text().contains(QStringLiteral("partial")));
        QVERIFY(panel.m_history_note->toolTip().contains(QStringLiteral("unknown, not zero")));
        QCOMPARE(panel.m_history_view->rowCount(), 1);
        panel.m_snapshot->paused = true;
        panel.updateMarketText();
        QVERIFY(panel.m_status->text().contains(QStringLiteral("paused")));
        QVERIFY(panel.m_status->text().contains(QStringLiteral("unbacked")));
        panel.m_uncertain = true;
        panel.updateMarketText();
        QVERIFY(panel.m_status->text().contains(QStringLiteral("outcome unknown")));
        QVERIFY(panel.m_status->text().contains(QStringLiteral("review before trading")));
        QVERIFY(panel.m_status->text().contains(QStringLiteral("unbacked")));
        QVERIFY(!panel.m_order->isEnabled());
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
        QVERIFY(panel.m_balances->text().contains(QStringLiteral("In orders  0 tUSD · 0.75 B3")));
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
    void cleanupTestCase()
    {
        m_model.reset();
        {
            LOCK(m_loader->context()->wallets_mutex);
            m_loader->context()->wallets.clear();
        }
        m_wallet.reset(); m_client.reset(); m_options.reset();
        m_setup->m_node.wallet_loader = nullptr;
        m_loader.reset(); m_node.reset(); m_setup.reset();
    }
};
int main(int argc, char** argv)
{
    QApplication app{argc, argv};
    QTemporaryDir settings;
    if (!settings.isValid()) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QCoreApplication::setOrganizationName(QStringLiteral("B3OfflineTests"));
    QCoreApplication::setApplicationName(QStringLiteral("FlowMeshWorkspace"));
    B3Theme::apply(app);
    B3FlowMeshWorkspaceTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "b3flowmeshworkspacetests.moc"
