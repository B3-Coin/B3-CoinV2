// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/b3flowmeshmarketdata.h>
#include <qt/b3flowmeshchart.h>
#include <qt/b3flowmeshorderbook.h>
#include <qt/b3flowmeshtradingpanel.h>
#include <qt/b3theme.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <rpc/server.h>
#include <qt/clientmodel.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <flowmesh/market.h>
#include <flowmesh/microblock.h>
#include <test/util/setup_common.h>
#include <wallet/context.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>
#include <QApplication>
#include <QComboBox>
#include <QDeadlineTimer>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QHBoxLayout>
#include <QImage>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPersistentModelIndex>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSemaphore>
#include <QSettings>
#include <QSignalBlocker>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
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
UniValue RemoteData(bool trade = true)
{
    auto v{Data(trade)}; UniValue proof{UniValue::VOBJ}; proof.pushKV("source", "remote_endpoint"); proof.pushKV("endpoint", "https://operator.invalid:18443");
    proof.pushKV("certificate_verified", true); proof.pushKV("account_state_verified", true); proof.pushKV("execution_result_verified", false);
    proof.pushKV("b3_checkpoint_confirmed", false); proof.pushKV("event_gap", false); v.pushKV("verification", proof); return v;
}

Curve LimitCurve(const char* side, unsigned char owner, CAmount price, CAmount quantity, CAmount filled = 0)
{
    Curve curve;
    curve.account = QString::fromStdString(H(owner).GetHex()); curve.side = QLatin1String(side);
    const bool bid{curve.side == QStringLiteral("bid")};
    const auto points{bid ? flowmesh::MakeLimitBidCurve(price, quantity) : flowmesh::MakeLimitAskCurve(price, quantity)};
    if (!points) throw std::runtime_error{"Invalid synthetic limit-curve fixture"};
    for (const auto& point : *points) curve.points.push_back({point.price, point.qty});
    curve.filled = filled; curve.remaining = quantity - filled;
    curve.reserved = bid ? Notional(price, curve.remaining).value_or(0) : curve.remaining;
    return curve;
}

Snapshot SyntheticLimitBook(size_t levels = 8)
{
    // Display-only generated values. No node, wallet or public market feed.
    auto snapshot{Parse(Data())};
    snapshot.curves.clear(); snapshot.own_curves.clear();
    for (size_t i{0}; i < levels; ++i) {
        const CAmount quantity{100'000 + static_cast<CAmount>(i) * 25'000};
        const CAmount filled{static_cast<CAmount>(i % 3) * 5'000};
        snapshot.curves.push_back(LimitCurve("bid", static_cast<unsigned char>(20 + i), 1000 - 2 * static_cast<CAmount>(i), quantity, filled));
        snapshot.curves.push_back(LimitCurve("ask", static_cast<unsigned char>(80 + i), 1010 + 2 * static_cast<CAmount>(i), quantity + 50'000, filled));
    }
    snapshot.depth = Aggregate(snapshot.curves);
    return snapshot;
}

bool SaveOrderBookGallery(const Snapshot& snapshot, bool inverse, const QString& directory, const QString& filename, const QString& heading)
{
    QWidget view;
    auto* layout{new QVBoxLayout{&view}};
    auto* title{new QLabel{heading, &view}}; title->setTextFormat(Qt::PlainText); title->setWordWrap(true);
    B3Theme::markTextRole(title, QStringLiteral("h3")); layout->addWidget(title);
    auto* row{new QHBoxLayout}; layout->addLayout(row, 1);
    auto* chart{new B3FlowMeshChart{&view}}; chart->setSnapshot(snapshot); chart->setInverted(inverse); row->addWidget(chart, 1);
    auto* book{new B3FlowMeshOrderBook{&view}}; book->setFixedWidth(420); book->setSnapshot(snapshot, inverse); row->addWidget(book);
    view.resize(1400, 850); view.show(); QCoreApplication::processEvents();
    QImage image{view.size(), QImage::Format_ARGB32}; image.fill(Qt::transparent); view.render(&image);
    return image.save(QDir{directory}.filePath(filename));
}

// Intercept only the existing read-only RPC in an isolated Qt unit process.
// No remote endpoint, wallet outbox or signing path is substituted or opened.
struct StatusReadProbe {
    QSemaphore entered, release;
    std::atomic_int count{0};
    std::atomic_bool fail{false}, wrong_account{false};
    std::mutex mutex;
    std::vector<std::pair<std::string, UniValue>> requests;
    CRPCCommand command;
    StatusReadProbe() : command{"hidden", "getflowmeshactionstatus",
        [this](const JSONRPCRequest& request, UniValue& result, bool) {
            { std::lock_guard lock{mutex}; requests.emplace_back(request.URI, request.params); }
            ++count; entered.release();
            if (!release.tryAcquire(1, 2000)) throw std::runtime_error{"Synthetic status request exceeded its test bound"};
            if (fail) throw std::runtime_error{"Synthetic status endpoint unavailable"};
            result = UniValue{UniValue::VOBJ};
            result.pushKV("market_id", request.params[0].get_str()); result.pushKV("action_id", request.params[1].get_str());
            result.pushKV("accepted", false); // Existing legacy status format; account_id is optional.
            if (wrong_account) result.pushKV("account_id", H(111).GetHex());
            return true;
        }, {}, 998877} {
        if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
        tableRPC.appendCommand(command.name, &command);
    }
    ~StatusReadProbe() { tableRPC.removeCommand(command.name, &command); }
};

UniValue ConnectionInfo(const std::string& url = "https://trading.invalid", bool attempted = true, bool transport = true, bool available = true)
{
    UniValue info{UniValue::VOBJ}; info.pushKV("backend", "remote"); info.pushKV("engine_enabled", false);
    info.pushKV("selected_endpoint", url); info.pushKV("active_endpoint", available ? url : "");
    UniValue endpoints{UniValue::VARR};
    if (!url.empty()) {
        UniValue row{UniValue::VOBJ}; row.pushKV("url", url); row.pushKV("available", available);
        row.pushKV("transport_available", transport); row.pushKV("last_attempt_ms", attempted ? 123 : 0);
        row.pushKV("retry_after_ms", 0); row.pushKV("consecutive_failures", transport ? 0 : 1);
        row.pushKV("last_error", available ? "" : transport ? "FlowMesh client snapshot has no certified head" : "Synthetic TLS connection failed");
        endpoints.push_back(row);
    }
    info.pushKV("endpoints", endpoints); return info;
}

// Exercise the actual worker and wallet-scoped dispatch, with only the existing
// non-economic RPCs intercepted. No sockets, signing, or live wallet are used.
struct ConnectionProbe {
    QSemaphore entered, release;
    std::atomic_int count{0};
    std::atomic_bool fail{false};
    std::mutex mutex;
    std::vector<std::pair<std::string, std::string>> requests;
    std::vector<std::unique_ptr<CRPCCommand>> commands;
    ConnectionProbe()
    {
        if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
        const auto add = [&](const char* method, const std::function<UniValue(const JSONRPCRequest&)>& run) {
            auto command{std::make_unique<CRPCCommand>("hidden", method,
                [run](const JSONRPCRequest& request, UniValue& result, bool) { result = run(request); return true; },
                std::vector<std::pair<std::string, bool>>{}, 998880 + commands.size())};
            tableRPC.appendCommand(command->name, command.get()); commands.push_back(std::move(command));
        };
        add("flowmeshclientconnect", [this](const JSONRPCRequest& request) {
            const auto url{request.params[0].get_str()};
            { std::lock_guard lock{mutex}; requests.emplace_back(request.URI, url); }
            ++count; entered.release();
            if (!release.tryAcquire(1, 2000)) throw std::runtime_error{"Synthetic connection exceeded its test bound"};
            if (fail) throw std::runtime_error{"Synthetic endpoint configuration rejected"};
            return ConnectionInfo(url);
        });
        add("getflowmeshclientinfo", [](const JSONRPCRequest&) { return ConnectionInfo(); });
        add("listflowmeshactions", [](const JSONRPCRequest&) {
            UniValue saved{UniValue::VOBJ}; saved.pushKV("source", "local-retained-outbox"); saved.pushKV("actions", UniValue{UniValue::VARR}); return saved;
        });
        add("listflowmeshmarkets", [](const JSONRPCRequest&) -> UniValue { throw std::runtime_error{"Synthetic selected-market read unavailable"}; });
    }
    ~ConnectionProbe() { for (const auto& command : commands) tableRPC.removeCommand(command->name, command.get()); }
};
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

    void AttachOfflineWallet(B3FlowMeshTradingPanel& panel, WalletModel& model,
                             const std::shared_ptr<wallet::CWallet>& wallet)
    {
        // Assign the isolated model directly through the existing friend test
        // seam: public setWalletModel intentionally starts a real RPC refresh.
        panel.m_timer->stop();
        panel.m_wallet = &model;
        panel.m_backend = interfaces::MakeWallet(*m_loader->context(), wallet);
        panel.m_wallet_name = model.getDisplayName();
        panel.m_cancel->store(false);
        panel.m_saved_actions_ready = true; // This offline seam models an empty, successfully read local outbox.
        Observe(panel, Parse(Data()));
        panel.m_price->setText(QStringLiteral("1"));
        panel.m_quantity->setText(QStringLiteral("1"));
    }

    void AttachOfflineWallet(B3FlowMeshTradingPanel& panel) { AttachOfflineWallet(panel, *m_model, m_wallet); }

    struct OfflineWallet {
        std::shared_ptr<wallet::CWallet> wallet;
        std::unique_ptr<WalletModel> model;
    };

    OfflineWallet MakeOfflineWallet(const std::string& name)
    {
        OfflineWallet result;
        result.wallet = std::make_shared<wallet::CWallet>(m_setup->m_node.chain.get(), name, wallet::CreateMockableWalletDatabase());
        {
            LOCK(result.wallet->cs_wallet);
            result.wallet->SetLastBlockProcessed(0, m_setup->m_node.chain->getBlockHash(0));
        }
        result.model = std::make_unique<WalletModel>(
            interfaces::MakeWallet(*m_loader->context(), result.wallet), *m_client, m_style.get());
        // Deliberately not registered with the loader: these models exercise
        // identity/lifetime, never wallet discovery or an automatic RPC refresh.
        return result;
    }

    static void ReadInFlight(B3FlowMeshTradingPanel& panel)
    {
        // A never-started, owned thread models the in-flight read marker. No
        // RPC, sockets or clock sleeps are needed to exercise GUI scheduling.
        panel.m_thread = new QThread{&panel};
        panel.m_busy = false;
        panel.updateControls();
    }

    static B3FlowMeshTrading::SavedActions SavedReadFixture(B3FlowMeshTradingPanel& panel)
    {
        B3FlowMeshTrading::SavedActions saved;
        saved.account = panel.m_snapshot->account;
        B3FlowMeshTrading::SavedAction action;
        action.market = panel.m_snapshot->market; action.domain = panel.m_snapshot->domain;
        action.config = panel.m_snapshot->config; action.account = saved.account;
        action.sequence = 7; action.type = 0; action.signed_bytes_size = 279;
        action.initial_submission_ms = 123; action.may_have_been_sent = true;
        action.signed_bytes_sha256 = QString::fromStdString(H(94).GetHex());
        action.receipt.action_id = QString::fromStdString(H(95).GetHex());
        action.receipt.market = action.market; action.receipt.account = action.account;
        action.receipt.state = QStringLiteral("unknown"); action.receipt.no_resubmit = true;
        saved.actions = {action}; panel.restoreSavedActions(saved);
        return saved;
    }

    static void Observe(B3FlowMeshTradingPanel& panel, const Snapshot& snapshot, bool inverse = false)
    {
        panel.m_timer->stop();
        // Older retained lifecycle fixtures explicitly use canonical entry.
        // Default production orientation is independently exercised below.
        { QSignalBlocker blocked{panel.m_orientation}; panel.m_orientation->setCurrentIndex(inverse ? 1 : 0); }
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
    void limitBookGroupsExactPricesAndUsesRemainingQuantities()
    {
        auto snapshot{Parse(Data(false))};
        snapshot.curves = {LimitCurve("bid", 1, 90, 4), LimitCurve("ask", 4, 110, 8, 2),
            LimitCurve("bid", 2, 100, 10, 3), LimitCurve("ask", 6, 120, 4, 1),
            LimitCurve("bid", 3, 100, 7, 2), LimitCurve("ask", 5, 110, 2)};
        const auto original{snapshot};
        const auto book{ProjectLimitBook(snapshot, false)};
        QVERIFY(book.complete); QVERIFY(book.units_known); QCOMPARE(book.projected_curves, size_t{6});
        QCOMPARE(book.bids.size(), size_t{2}); QCOMPARE(book.asks.size(), size_t{2});
        QCOMPARE(book.bids[0].canonical_price, CAmount{100}); QCOMPARE(book.bids[1].canonical_price, CAmount{90});
        QCOMPARE(book.asks[0].canonical_price, CAmount{120}); QCOMPARE(book.asks[1].canonical_price, CAmount{110});
        QCOMPARE(book.bids[0].remaining, CAmount{12}); QCOMPARE(book.bids[0].gross_notional, CAmount{1200});
        QCOMPARE(book.bids[0].curve_count, size_t{2}); QCOMPARE(book.asks[1].curve_count, size_t{2});
        QCOMPARE(book.asks[1].remaining, CAmount{8}); QCOMPARE(book.asks[1].gross_notional, CAmount{880});
        QCOMPARE(book.bids[0].price, QStringLiteral("0.1"));
        QCOMPARE(book.bids[0].amount, QStringLiteral("0.000012"));
        QCOMPARE(book.bids[0].total, QStringLiteral("0.0000012"));
        QVERIFY(book.canonical_best_bid); QCOMPARE(*book.canonical_best_bid, CAmount{100});
        QVERIFY(book.canonical_best_ask); QCOMPARE(*book.canonical_best_ask, CAmount{110});
        QCOMPARE(book.spread, QStringLiteral("0.01"));

        const auto inverse{ProjectLimitBook(snapshot, true)};
        QVERIFY(inverse.complete); QCOMPARE(inverse.projected_curves, size_t{6});
        // Both visible sides descend. Reverse asks come from canonical bids;
        // reverse bids come from canonical asks. Best asks remain at bottom.
        QCOMPARE(inverse.asks[0].canonical_price, CAmount{90}); QCOMPARE(inverse.asks[1].canonical_price, CAmount{100});
        QCOMPARE(inverse.bids[0].canonical_price, CAmount{110}); QCOMPARE(inverse.bids[1].canonical_price, CAmount{120});
        QCOMPARE(inverse.bids[0].price, ExactInversePrice(110, 6));
        QCOMPARE(inverse.bids[0].amount, QStringLiteral("0.00000088"));
        QCOMPARE(inverse.bids[0].total, QStringLiteral("0.000008"));
        QCOMPARE(inverse.spread, QStringLiteral("10/11"));
        QVERIFY(snapshot == original);
        std::reverse(snapshot.curves.begin(), snapshot.curves.end());
        QVERIFY(ProjectLimitBook(snapshot, false) == book);
        QVERIFY(ProjectLimitBook(snapshot, true) == inverse);
    }
    void limitBookNeverGroupsRoundedInversePrices()
    {
        auto snapshot{Parse(Data(false))};
        const CAmount price{9'007'199'254'740'992LL};
        snapshot.curves = {LimitCurve("bid", 1, price + 1, 1), LimitCurve("bid", 2, price, 1)};
        // The chart's display-only approximations coincide; exact book levels
        // must remain distinct, including beyond double's consecutive range.
        QCOMPARE(FormatDisplayPrice(price, 6, true), FormatDisplayPrice(price + 1, 6, true));
        const auto book{ProjectLimitBook(snapshot, true)};
        QCOMPARE(book.asks.size(), size_t{2}); QVERIFY(book.bids.empty());
        QCOMPARE(book.asks[0].canonical_price, price); QCOMPARE(book.asks[1].canonical_price, price + 1);
        QVERIFY(book.asks[0].price != book.asks[1].price);
        QCOMPARE(ParseDisplayPrice(book.asks[0].price, snapshot.units, true).value(), price);
        QCOMPARE(ParseDisplayPrice(book.asks[1].price, snapshot.units, true).value(), price + 1);
        QVERIFY(book.spread.isEmpty());
    }
    void limitBookDisclosesGeneralPartialAndUnknownUnitData()
    {
        auto snapshot{Parse(Data(false))};
        snapshot.curves = {LimitCurve("bid", 1, 100, 10), LimitCurve("ask", 2, 110, 10)};
        auto slope{LimitCurve("bid", 3, 100, 10)};
        slope.points = {{99, 10}, {101, 0}}; // Valid curve, not one discrete limit.
        snapshot.curves.push_back(slope);
        const auto general{ProjectLimitBook(snapshot, false)};
        QCOMPARE(general.general_curves, size_t{1}); QCOMPARE(general.invalid_curves, size_t{0});
        QCOMPARE(general.projected_curves, size_t{2}); QCOMPARE(general.bids.size(), size_t{1});
        QCOMPARE(general.bids.front().remaining, CAmount{10}); QVERIFY(!general.complete); QVERIFY(general.spread.isEmpty());
        snapshot.curves.pop_back(); snapshot.curves_complete = false;
        const auto partial{ProjectLimitBook(snapshot, false)};
        QVERIFY(!partial.complete); QCOMPARE(partial.projected_curves, size_t{2}); QVERIFY(partial.spread.isEmpty());
        snapshot.curves_complete = true;
        for (const int unknown : {0, 1}) {
            auto unverified{snapshot};
            if (unknown == 0) unverified.units.known = false;
            else unverified.units.asset = QString::fromStdString(H(99).GetHex());
            const auto book{ProjectLimitBook(unverified, true)};
            QVERIFY(!book.units_known); QVERIFY(book.spread.isEmpty());
            QCOMPARE(book.bids.size(), size_t{1}); QCOMPARE(book.asks.size(), size_t{1});
            for (const auto* rows : {&book.asks, &book.bids}) for (const auto& row : *rows) {
                QVERIFY(row.price.isEmpty()); QVERIFY(row.amount.isEmpty()); QVERIFY(row.total.isEmpty());
            }
        }
        snapshot.curves = {LimitCurve("bid", 1, 100, 10, 10), LimitCurve("ask", 2, 110, 10)};
        const auto exhausted{ProjectLimitBook(snapshot, false)};
        QCOMPARE(exhausted.exhausted_curves, size_t{1}); QCOMPARE(exhausted.projected_curves, size_t{1});
        QVERIFY(exhausted.bids.empty()); QCOMPARE(exhausted.asks.size(), size_t{1}); QVERIFY(exhausted.complete);
        QVERIFY(exhausted.spread.isEmpty());
    }
    void limitBookZeroLimitsAndCrossedBooksHaveNoInventedSpread()
    {
        auto snapshot{Parse(Data(false))};
        snapshot.curves = {LimitCurve("bid", 1, 0, 3), LimitCurve("ask", 2, 0, 5)};
        const auto canonical{ProjectLimitBook(snapshot, false)};
        QCOMPARE(canonical.bids.size(), size_t{1}); QCOMPARE(canonical.asks.size(), size_t{1});
        QCOMPARE(canonical.bids[0].price, QStringLiteral("0")); QCOMPARE(canonical.bids[0].gross_notional, CAmount{0});
        const auto inverse{ProjectLimitBook(snapshot, true)};
        QCOMPARE(inverse.zero_inverse_curves, size_t{2}); QVERIFY(inverse.bids.empty()); QVERIFY(inverse.asks.empty());
        QVERIFY(!inverse.complete); QVERIFY(inverse.spread.isEmpty());
        snapshot.curves = {LimitCurve("bid", 1, 111, 3), LimitCurve("ask", 2, 110, 5)};
        QVERIFY(ProjectLimitBook(snapshot, false).spread.isEmpty());
        QVERIFY(ProjectLimitBook(snapshot, true).spread.isEmpty());
    }
    void limitBookRejectsWholeOverflowingGroupsAndInvalidDuplicates()
    {
        auto snapshot{Parse(Data(false))};
        const auto valid_ask{LimitCurve("ask", 3, 7, 1)};
        // Zero price isolates remaining-quantity overflow from notional.
        snapshot.curves = {LimitCurve("bid", 1, 0, MAX_MONEY / 2 + 1),
            LimitCurve("bid", 2, 0, MAX_MONEY / 2 + 1), valid_ask};
        auto book{ProjectLimitBook(snapshot, false)};
        QCOMPARE(book.invalid_curves, size_t{2}); QVERIFY(book.bids.empty()); QCOMPARE(book.asks.size(), size_t{1});
        QCOMPARE(book.projected_curves, size_t{1}); QVERIFY(!book.complete); QVERIFY(book.spread.isEmpty());
        // Here remaining fits, but summed gross quote notional does not.
        snapshot.curves = {LimitCurve("bid", 1, 2, MAX_MONEY / 4 + 1),
            LimitCurve("bid", 2, 2, MAX_MONEY / 4 + 1), valid_ask};
        book = ProjectLimitBook(snapshot, false);
        QCOMPARE(book.invalid_curves, size_t{2}); QVERIFY(book.bids.empty()); QCOMPARE(book.asks.size(), size_t{1});
        snapshot.curves = {LimitCurve("ask", 1, MAX_MONEY, 2), valid_ask};
        book = ProjectLimitBook(snapshot, false);
        QCOMPARE(book.invalid_curves, size_t{1}); QCOMPARE(book.asks.size(), size_t{1});
        auto duplicate{LimitCurve("bid", 0xab, 100, 1)};
        auto upper{duplicate}; upper.account = upper.account.toUpper();
        snapshot.curves = {duplicate, upper, valid_ask};
        book = ProjectLimitBook(snapshot, false);
        QCOMPARE(book.invalid_curves, size_t{2}); QVERIFY(book.bids.empty()); QCOMPARE(book.asks.size(), size_t{1});
        for (const int fault : {0, 1, 2, 3}) {
            auto malformed{LimitCurve("bid", 1, 100, 10)};
            if (fault == 0) ++malformed.remaining;
            if (fault == 1) malformed.filled = -1;
            if (fault == 2) malformed.side = QStringLiteral("unknown");
            if (fault == 3) malformed.points.clear();
            snapshot.curves = {malformed, valid_ask};
            book = ProjectLimitBook(snapshot, false);
            QCOMPARE(book.invalid_curves, size_t{1}); QVERIFY(book.bids.empty()); QCOMPARE(book.asks.size(), size_t{1});
        }
        snapshot.curves.assign(129, valid_ask);
        book = ProjectLimitBook(snapshot, false);
        QVERIFY(!book.complete); QVERIFY(book.bids.empty()); QVERIFY(book.asks.empty());
        QCOMPARE(book.invalid_curves, size_t{129});
    }
    void limitBookInverseSpreadRemainsExactBeyond128BitDenominator()
    {
        auto snapshot{Parse(Data(false))}; snapshot.units.decimals = 18;
        snapshot.curves = {LimitCurve("bid", 1, MAX_MONEY - 2, 1), LimitCurve("ask", 2, MAX_MONEY - 1, 1)};
        const auto book{ProjectLimitBook(snapshot, true)};
        QVERIFY(book.complete); QCOMPARE(book.asks.size(), size_t{1}); QCOMPARE(book.bids.size(), size_t{1});
        QCOMPARE(book.spread, QStringLiteral("1/438508839999999998013400000000000002000000000"));
        QCOMPARE(ParseDisplayPrice(book.asks[0].price, snapshot.units, true).value(), MAX_MONEY - 2);
        QCOMPARE(ParseDisplayPrice(book.bids[0].price, snapshot.units, true).value(), MAX_MONEY - 1);
    }
    void limitBookWidgetShowsNearestLevelsWithoutRefreshChurn_data()
    {
        QTest::addColumn<bool>("inverse");
        QTest::newRow("canonical") << false;
        QTest::newRow("inverse") << true;
    }
    void limitBookWidgetShowsNearestLevelsWithoutRefreshChurn()
    {
        QFETCH(bool, inverse);
        const auto snapshot{SyntheticLimitBook(16)};
        const auto projection{ProjectLimitBook(snapshot, inverse)};
        B3FlowMeshOrderBook book; book.resize(420, 440); book.setSnapshot(snapshot, inverse);
        auto* asks{book.findChild<QTableWidget*>(QStringLiteral("flowMeshBookAsks"))};
        auto* bids{book.findChild<QTableWidget*>(QStringLiteral("flowMeshBookBids"))};
        auto* note{book.findChild<QLabel*>(QStringLiteral("flowMeshBookNote"))};
        QVERIFY(asks); QVERIFY(bids); QVERIFY(note);
        asks->setFixedHeight(140); bids->setFixedHeight(140);
        book.show(); QCoreApplication::processEvents(); QCoreApplication::processEvents();
        QCOMPARE(asks->rowCount(), 12); QCOMPARE(bids->rowCount(), 12);
        QCOMPARE(asks->item(0, 0)->text(), FormatDisplayPrice(projection.asks[4].canonical_price, snapshot.units.decimals, inverse));
        QCOMPARE(asks->item(11, 0)->text(), FormatDisplayPrice(projection.asks.back().canonical_price, snapshot.units.decimals, inverse));
        QCOMPARE(bids->item(0, 0)->text(), FormatDisplayPrice(projection.bids.front().canonical_price, snapshot.units.decimals, inverse));
        QCOMPARE(bids->item(11, 0)->text(), FormatDisplayPrice(projection.bids[11].canonical_price, snapshot.units.decimals, inverse));
        QCOMPARE(asks->item(11, 1)->text(), projection.asks.back().amount);
        QCOMPARE(bids->item(0, 2)->text(), projection.bids.front().total);
        QCOMPARE(asks->item(0, 0)->foreground().color(), B3Theme::kNegative);
        QCOMPARE(bids->item(0, 0)->foreground().color(), B3Theme::kPositive);
        QVERIFY(note->text().contains(QStringLiteral("Nearest 12")));
        QVERIFY(note->toolTip().contains(QStringLiteral("no price-time priority")));
        if (inverse) {
            QVERIFY(note->text().contains(QStringLiteral("gross")));
            QVERIFY(bids->item(0, 0)->toolTip().contains(ExactInversePrice(projection.bids.front().canonical_price, snapshot.units.decimals)));
        }
        // A short view must initially show the nearest ask beside the spread.
        auto* scroll{asks->verticalScrollBar()};
        QVERIFY(scroll->maximum() > 0); QCOMPARE(scroll->value(), scroll->maximum());
        QVERIFY(asks->viewport()->rect().contains(asks->visualItemRect(asks->item(11, 0)).center()));
        asks->setFixedHeight(110); book.resize(430, 400);
        QCoreApplication::processEvents(); QCoreApplication::processEvents();
        QCOMPARE(scroll->value(), scroll->maximum());
        QVERIFY(asks->viewport()->rect().contains(asks->visualItemRect(asks->item(11, 0)).center()));
        // Cumulative shading grows outward from the best displayed levels.
        QCOMPARE(asks->item(0, 0)->data(Qt::UserRole + 1).toDouble(), 1.0);
        QCOMPARE(bids->item(11, 0)->data(Qt::UserRole + 1).toDouble(), 1.0);
        QVERIFY(asks->item(11, 0)->data(Qt::UserRole + 1).toDouble() > 0);
        QVERIFY(asks->item(11, 0)->data(Qt::UserRole + 1).toDouble() < 1);
        QVERIFY(bids->item(0, 0)->data(Qt::UserRole + 1).toDouble() < 1);
        // The production book deliberately has NoSelection. To exercise
        // retained row selection, opt this test into BOTH SingleSelection and
        // SelectRows; selectRow() does not establish a row selection with the
        // table's default SelectItems/SingleSelection combination.
        QCOMPARE(asks->selectionMode(), QAbstractItemView::NoSelection);
        asks->setSelectionMode(QAbstractItemView::SingleSelection);
        asks->setSelectionBehavior(QAbstractItemView::SelectRows);
        asks->setFocus(); QCoreApplication::processEvents();
        asks->selectRow(5);
        asks->selectionModel()->setCurrentIndex(asks->model()->index(5, 0), QItemSelectionModel::NoUpdate);
        scroll->setValue(scroll->maximum() / 2);
        QCoreApplication::processEvents(); QVERIFY(asks->hasFocus());
        const int position{scroll->value()}; QVERIFY(position < scroll->maximum());
        const auto selected{asks->selectionModel()->selectedIndexes()}; QCOMPARE(selected.size(), qsizetype{3});
        for (const auto& index : selected) QCOMPARE(index.row(), 5);
        const QPersistentModelIndex current{asks->currentIndex()}; QCOMPARE(QModelIndex{current}, asks->model()->index(5, 0));
        const auto* ask_cell{asks->item(5, 0)}; const auto* bid_cell{bids->item(0, 0)};
        QSignalSpy asks_changed{asks->model(), &QAbstractItemModel::dataChanged};
        QSignalSpy bids_changed{bids->model(), &QAbstractItemModel::dataChanged};
        QSignalSpy asks_reset{asks->model(), &QAbstractItemModel::modelReset};
        QSignalSpy bids_reset{bids->model(), &QAbstractItemModel::modelReset};
        QSignalSpy removed{asks->model(), &QAbstractItemModel::rowsRemoved};
        QSignalSpy inserted{asks->model(), &QAbstractItemModel::rowsInserted};
        for (int i{0}; i < 5; ++i) { book.setSnapshot(Snapshot{snapshot}, inverse); book.setStale(false); }
        QCoreApplication::processEvents();
        QCOMPARE(asks->item(5, 0), ask_cell); QCOMPARE(bids->item(0, 0), bid_cell);
        QCOMPARE(asks->selectionModel()->selectedIndexes(), selected); QCOMPARE(scroll->value(), position);
        QVERIFY(current.isValid()); QCOMPARE(asks->currentIndex(), QModelIndex{current});
        QVERIFY(asks->hasFocus());
        QCOMPARE(asks_changed.count(), 0); QCOMPARE(bids_changed.count(), 0);
        QCOMPARE(asks_reset.count(), 0); QCOMPARE(bids_reset.count(), 0);
        QCOMPARE(removed.count(), 0); QCOMPARE(inserted.count(), 0);
        // Genuine changed liquidity still appears without rebuilding the view.
        auto changed{snapshot}; ++changed.curves.front().filled; --changed.curves.front().remaining;
        changed.curves.front().reserved = *Notional(changed.curves.front().points.front().price, changed.curves.front().remaining);
        const auto changed_book{ProjectLimitBook(changed, inverse)};
        book.setSnapshot(changed, inverse); QCoreApplication::processEvents();
        QVERIFY(asks_changed.count() + bids_changed.count() > 0);
        QCOMPARE(asks->item(11, 1)->text(), changed_book.asks.back().amount);
        QCOMPARE(bids->item(0, 1)->text(), changed_book.bids.front().amount);
        QCOMPARE(asks->item(5, 0), ask_cell); QCOMPARE(bids->item(0, 0), bid_cell);
        QCOMPARE(asks->selectionModel()->selectedIndexes(), selected); QCOMPARE(scroll->value(), position);
        QVERIFY(asks->hasFocus()); QVERIFY(current.isValid());
        QCOMPARE(asks_reset.count(), 0); QCOMPARE(bids_reset.count(), 0);
        QCOMPARE(removed.count(), 0); QCOMPARE(inserted.count(), 0);
    }
    void limitBookWidgetRequiresVerifiedOrderStateAndDisclosesSubsets()
    {
        const auto original{Parse(RemoteData())};
        B3FlowMeshOrderBook book; book.setSnapshot(original, false);
        auto* asks{book.findChild<QTableWidget*>(QStringLiteral("flowMeshBookAsks"))};
        auto* bids{book.findChild<QTableWidget*>(QStringLiteral("flowMeshBookBids"))};
        auto* last{book.findChild<QLabel*>(QStringLiteral("flowMeshBookLast"))};
        auto* note{book.findChild<QLabel*>(QStringLiteral("flowMeshBookNote"))};
        auto* spread{book.findChild<QLabel*>(QStringLiteral("flowMeshBookSpread"))};
        QVERIFY(asks); QVERIFY(bids); QVERIFY(last); QVERIFY(note); QVERIFY(spread);
        QCOMPARE(asks->rowCount(), 1); QCOMPARE(bids->rowCount(), 1);
        QVERIFY(last->text().startsWith(QStringLiteral("Reported last")));
        for (const int fault : {0, 1, 2, 3, 4}) {
            auto snapshot{original};
            if (fault == 0) snapshot.units.known = false;
            if (fault == 1) snapshot.units.asset = QString::fromStdString(H(99).GetHex());
            if (fault == 2) snapshot.certificate_verified = false;
            if (fault == 3) snapshot.account_state_verified = false;
            if (fault == 4) snapshot.certified = false;
            book.setSnapshot(snapshot, false);
            QCOMPARE(asks->rowCount(), 0); QCOMPARE(bids->rowCount(), 0);
            QVERIFY(last->text().contains(QStringLiteral("—"))); QVERIFY(spread->text().contains(QStringLiteral("—")));
        }
        book.setSnapshot(std::nullopt, false);
        QCOMPARE(asks->rowCount(), 0); QCOMPARE(bids->rowCount(), 0);
        auto partial{original}; partial.curves_complete = false;
        book.setSnapshot(partial, true);
        QCOMPARE(asks->rowCount(), 1); QCOMPARE(bids->rowCount(), 1);
        QVERIFY(note->text().contains(QStringLiteral("Partial"))); QVERIFY(note->text().contains(QStringLiteral("gross")));
        QVERIFY(spread->text().contains(QStringLiteral("partial")));
        auto general{original}; auto slope{LimitCurve("bid", 12, 100, 10)};
        slope.points = {{99, 10}, {101, 0}}; general.curves.push_back(slope);
        book.setSnapshot(general, false);
        QVERIFY(note->text().contains(QStringLiteral("non-limit"))); QVERIFY(spread->text().contains(QStringLiteral("partial")));
        auto crossed{original}; crossed.curves = {LimitCurve("bid", 1, 111, 1), LimitCurve("ask", 2, 110, 1)};
        book.setSnapshot(crossed, false);
        QVERIFY(spread->text().contains(QStringLiteral("Crossed"))); QVERIFY(!spread->text().contains(QStringLiteral("two sides required")));
        book.setSnapshot(original, false); book.setStale(true);
        auto* stale{book.findChild<QLabel*>(QStringLiteral("flowMeshBookStale"))}; QVERIFY(stale); QVERIFY(!stale->isHidden());
        QCOMPARE(asks->rowCount(), 1); QCOMPARE(bids->rowCount(), 1); // Retained, not silently emptied.
    }
    void switchingBookAndCurveViewsPreservesUnsubmittedInput()
    {
        B3FlowMeshTradingPanel panel; panel.resize(1400, 950); AttachOfflineWallet(panel);
        Observe(panel, SyntheticLimitBook());
        auto* mode{panel.findChild<QComboBox*>(QStringLiteral("flowMeshLiquidityMode"))};
        auto* book{panel.findChild<B3FlowMeshOrderBook*>(QStringLiteral("flowMeshOrderBook"))};
        QVERIFY(mode); QVERIFY(book); QCOMPARE(mode->currentIndex(), 0);
        panel.m_price->setText(QStringLiteral("1.009")); panel.m_quantity->setText(QStringLiteral("0.25"));
        panel.show(); QCoreApplication::processEvents();
        QVERIFY(book->isVisible()); QVERIFY(!panel.m_depth_view->isVisible());
        panel.m_price->setFocus(); panel.m_price->setSelection(2, 2); QCoreApplication::processEvents();
        QVERIFY(panel.m_price->hasFocus());
        const int cursor{panel.m_price->cursorPosition()}; const QString selection{panel.m_price->selectedText()};
        const auto snapshot{panel.m_snapshot}; QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        mode->setCurrentIndex(1); QCoreApplication::processEvents();
        QVERIFY(!book->isVisible()); QVERIFY(panel.m_depth_view->isVisible());
        mode->setCurrentIndex(0); QCoreApplication::processEvents();
        QVERIFY(book->isVisible()); QVERIFY(!panel.m_depth_view->isVisible());
        QCOMPARE(panel.m_price->text(), QStringLiteral("1.009")); QCOMPARE(panel.m_quantity->text(), QStringLiteral("0.25"));
        QCOMPARE(panel.m_price->cursorPosition(), cursor); QCOMPARE(panel.m_price->selectedText(), selection); QVERIFY(panel.m_price->hasFocus());
        QVERIFY(panel.m_snapshot == snapshot); QCOMPARE(unlock.count(), 0);
        QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_confirmation);
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
    void remoteStateRequiresLocalProofButNotLocalValidator()
    {
        auto v{RemoteData()}; const auto s{Parse(v)};
        QVERIFY(s.remote); QVERIFY(s.observer); QVERIFY(s.certificate_verified); QVERIFY(s.account_state_verified); QVERIFY(!s.execution_result_verified);
        QVERIFY(AdmissionReady(s, 50, -1)); QVERIFY(StatusText(s, 50, -1).contains(QStringLiteral("not B3-checkpoint confirmed")));
        QVERIFY(!Parse(Data()).remote); // Older local replies retain compatibility.
        for (const auto* flag : {"certificate_verified", "account_state_verified"}) {
            auto bad{v}; auto proof{bad["verification"]}; proof.pushKV(flag, false); bad.pushKV("verification", proof);
            QVERIFY(Rejects([&] { Parse(bad); }));
        }
        auto proof{v["verification"]}; proof.pushKV("event_gap", true); v.pushKV("verification", proof);
        QVERIFY(StatusText(Parse(v), 50, -1).contains(QStringLiteral("gap")));
        v.pushKV("unchanged", true); const auto small{Parse(v)}; QVERIFY(small.remote); QVERIFY(small.certificate_verified); QVERIFY(small.history.empty());
        auto unsafe{s}; unsafe.account_state_verified = false; QVERIFY(!AdmissionReady(unsafe, 50, -1));
    }
    void remoteDiscoveryFetchesOnlySelectedStateEvenWithoutAccount()
    {
        const auto data{RemoteData()}; const auto s{Parse(data)};
        UniValue row{UniValue::VOBJ};
        for (const auto* key : {"market_id", "base_asset_id", "domain", "execution_config_id", "quote_asset"}) row.pushKV(key, data[key]);
        row.pushKV("vault_id", flowmesh::ComputeFlowMeshVaultId(H(1), *uint256::FromHex(s.market.toStdString()))->GetHex());
        row.pushKV("available", true); row.pushKV("running", false); row.pushKV("paused", true); row.pushKV("pending_handoff", false);
        row.pushKV("halt", "none"); row.pushKV("error", "Select market to verify its certified state"); row.pushKV("checkpoint_pending", false);
        auto proof{data["verification"]}; proof.pushKV("certificate_verified", false); proof.pushKV("account_state_verified", false); row.pushKV("verification", proof);
        QVERIFY(!B3FlowMeshTrading::ParseMarket(row).ready);
        for (bool account : {false, true}) {
            int reads{0};
            const B3FlowMeshTrading::RpcCall rpc = [&](const std::string& method, const UniValue& params) {
                ++reads;
                if (method == "listflowmeshmarkets") { UniValue rows{UniValue::VARR}; rows.push_back(row); return rows; }
                if (method != "getflowmeshbalance" || params[0].get_str() != s.market.toStdString()) throw std::runtime_error{"Unexpected RPC; discovery must not sign or query every market"};
                auto response{row}; response.pushKV("running", true); response.pushKV("paused", false); response.pushKV("error", ""); response.pushKV("verification", data["verification"]);
                if (account) { auto a{data["account"]}; a.pushKV("b3_available", FormatAmount(s.b3_available, 9).toStdString()); a.pushKV("b3_reserved", FormatAmount(s.b3_reserved, 9).toStdString()); response.pushKV("account", a); }
                return response;
            };
            const auto market{B3FlowMeshTrading::ReadMarket(s.market, rpc)};
            QCOMPARE(reads, 2); QVERIFY(market.ready); QVERIFY(market.remote); QVERIFY(market.publish_ready); QCOMPARE(market.has_account, account);
            if (account) { QCOMPARE(market.sequence, s.account_sequence); QCOMPARE(market.b3_available, s.b3_available); }
        }
    }
    void remoteHistoryIsReportedAndUnknownActionCannotBecomeANewOrder()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        QVERIFY(panel.m_order->isEnabled()); QVERIFY(m_wallet->IsLocked());
        QVERIFY(panel.m_history_note->text().contains(QStringLiteral("Endpoint-reported")));
        QVERIFY(panel.m_last_price->text().startsWith(QStringLiteral("Reported:")));
        QVERIFY(panel.m_identity_detail->text().contains(QStringLiteral("https://operator.invalid:18443")));
        QVERIFY(!panel.m_checkpoint->isEnabled()); QVERIFY(!panel.m_publish->isEnabled());
        B3FlowMeshTrading::Receipt receipt; receipt.action_id = QString::fromStdString(H(20).GetHex()); receipt.state = QStringLiteral("unknown");
        panel.m_pending_market = panel.m_snapshot->market; panel.m_pending_account = panel.m_snapshot->account;
        panel.m_pending_sequence = panel.m_snapshot->account_sequence - 1; panel.m_receipt_wallet = panel.m_wallet;
        panel.applyReceipt(receipt); panel.updateMarketText();
        QVERIFY(panel.m_uncertain); QVERIFY(!panel.m_order->isEnabled()); QVERIFY(!panel.m_withdraw->isEnabled()); QVERIFY(panel.m_retry_receipt->isEnabled());
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock}; bool reviewed{false};
        QTimer::singleShot(0, &panel, [&] {
            if (panel.m_confirmation) { reviewed = panel.m_confirmation->text().contains(receipt.action_id) && panel.m_confirmation->text().contains(QStringLiteral("does not unlock")); panel.m_confirmation->done(QMessageBox::Cancel); }
        });
        panel.retryReceipt(); QVERIFY(reviewed); QCOMPARE(unlock.count(), 0); QVERIFY(!panel.m_thread); QVERIFY(m_wallet->IsLocked());
        // Advancing the nonce is not inclusion evidence for this exact action.
        panel.m_market_data[0].sequence += 100; panel.updateControls(); QVERIFY(!panel.m_order->isEnabled());
        receipt.state = QStringLiteral("admitted"); panel.applyReceipt(receipt); panel.updateControls(); QVERIFY(!panel.m_order->isEnabled());
        receipt.state = QStringLiteral("certified_inclusion"); receipt.certificate_verified = true; receipt.microblock_hash = QString::fromStdString(H(21).GetHex()); receipt.microblock_sequence = 4;
        panel.applyReceipt(receipt); panel.updateMarketText(); QVERIFY(!panel.m_uncertain); QVERIFY(!panel.m_pending_sequence); QVERIFY(panel.m_order->isEnabled());
        QVERIFY(panel.m_progress->text().contains(QStringLiteral("Execution outcome is unknown"))); QVERIFY(!panel.m_retry_receipt->isEnabled());
        QVERIFY(m_wallet->IsLocked()); QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result);
        // Returning to local data clears remote provenance labels.
        Observe(panel, Parse(Data())); QVERIFY(!panel.m_history_note->text().contains(QStringLiteral("Endpoint-reported")));
        B3FlowMeshChart chart; chart.setSnapshot(Parse(RemoteData(false))); QVERIFY(chart.emptyMessage().contains(QStringLiteral("not independently verified")));
    }
    void reverseRemoteHistoryPreservesProvenanceAndGrossFeeWarning()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        Observe(panel, Parse(RemoteData()), true);
        QVERIFY(panel.m_history_note->text().contains(QStringLiteral("Endpoint-reported")));
        QVERIFY(panel.m_history_note->text().contains(QStringLiteral("not independently verified")));
        QVERIFY(panel.m_history_note->text().contains(QStringLiteral("gross B3 before fees")));
        QVERIFY(panel.m_history_note->toolTip().contains(QStringLiteral("not these historical prices or fills")));
        QVERIFY(panel.m_history_note->toolTip().contains(QStringLiteral("not net B3 received")));
        QVERIFY(panel.m_history_note->toolTip().contains(QStringLiteral("must not be inferred")));
        QCOMPARE(panel.m_history_view->toolTip(), panel.m_history_note->toolTip());
        QVERIFY(panel.m_last_price->text().startsWith(QStringLiteral("Reported:")));
        QVERIFY(panel.m_chart->toolTip().contains(QStringLiteral("not independently verified")));
        QVERIFY(!panel.m_thread); QVERIFY(m_wallet->IsLocked());
    }
    void retainedReceiptDetachReleasesWalletWithoutLosingPublicState_data()
    {
        QTest::addColumn<bool>("visible");
        QTest::addColumn<bool>("included");
        QTest::newRow("visible-certified") << true << true;
        QTest::newRow("hidden-certified") << false << true;
        QTest::newRow("visible-unknown") << true << false;
        QTest::newRow("hidden-unknown") << false << false;
    }
    void retainedReceiptDetachReleasesWalletWithoutLosingPublicState()
    {
        QFETCH(bool, visible);
        QFETCH(bool, included);
        const auto wallet_owners{m_wallet.use_count()};
        B3FlowMeshTradingPanel panel;
        AttachOfflineWallet(panel);
        Observe(panel, Parse(RemoteData()));
        const std::weak_ptr<interfaces::Wallet> backend{panel.m_backend};
        QVERIFY(!backend.expired());
        QCOMPARE(m_wallet.use_count(), wallet_owners + 1);
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};

        // The completion path remembers the wallet identity for exact-action
        // status/retry. Neither a certified nor an unknown receipt may pin
        // the underlying CWallet after its model is detached.
        const QString market{panel.m_snapshot->market};
        const QString account{panel.m_snapshot->account};
        const auto sequence{panel.m_snapshot->account_sequence};
        B3FlowMeshTrading::Receipt receipt;
        receipt.action_id = QString::fromStdString(H(91).GetHex());
        receipt.state = included ? QStringLiteral("certified_inclusion") : QStringLiteral("unknown");
        receipt.certificate_verified = included;
        if (included) { receipt.microblock_hash = QString::fromStdString(H(92).GetHex()); receipt.microblock_sequence = 4; }
        panel.m_pending_market = market; panel.m_pending_account = account;
        panel.m_pending_sequence = sequence; panel.m_receipt_wallet = panel.m_wallet;
        panel.applyReceipt(receipt);
        panel.updateMarketText();
        const QString uncertain_details{panel.m_uncertain_details};
        QCOMPARE(panel.m_uncertain, !included);
        if (!included) QVERIFY(uncertain_details.contains(receipt.action_id));
        panel.setVisible(visible);
        QCoreApplication::processEvents();
        QCOMPARE(panel.isVisible(), visible);

        // Shutdown hides this shell child but does not destroy it. Exercise
        // the public detach operation with the panel deliberately still alive.
        panel.setWalletModel(nullptr);
        QCoreApplication::processEvents();
        QVERIFY(!panel.m_wallet); QVERIFY(!panel.m_backend);
        QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_unlock);
        QVERIFY(!panel.m_timer->isActive()); QVERIFY(!panel.m_busy);
        QVERIFY(!panel.m_order->isEnabled()); QVERIFY(!panel.m_retry_receipt->isEnabled());
        QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
        QVERIFY(panel.m_receipt);
        QCOMPARE(panel.m_receipt->action_id, receipt.action_id);
        QCOMPARE(panel.m_receipt->state, receipt.state);
        QCOMPARE(panel.m_receipt->certificate_verified, receipt.certificate_verified);
        QCOMPARE(panel.m_pending_market, market); QCOMPARE(panel.m_pending_account, account);
        QCOMPARE(panel.m_uncertain, !included);
        QCOMPARE(panel.m_uncertain_details, uncertain_details);
        if (!included) {
            QCOMPARE(panel.m_uncertain_action_id, receipt.action_id);
            QVERIFY(panel.m_pending_sequence); QCOMPARE(*panel.m_pending_sequence, sequence);
        } else {
            QVERIFY(!panel.m_pending_sequence);
        }
        QVERIFY2(backend.expired(), "A detached, still-live panel must not retain the wallet through a saved receipt or uncertain outcome");
        QCOMPARE(m_wallet.use_count(), wallet_owners);
    }
    void attemptedWriteDetachDrainsAndFencesLateCompletion_data()
    {
        QTest::addColumn<bool>("completion_queued");
        QTest::addColumn<bool>("included");
        QTest::newRow("worker-awaiting-cancellation") << false << false;
        QTest::newRow("finished-callback-already-queued") << true << false;
        QTest::newRow("certified-callback-already-queued") << true << true;
    }
    void attemptedWriteDetachDrainsAndFencesLateCompletion()
    {
        QFETCH(bool, completion_queued);
        QFETCH(bool, included);
        const auto wallet_owners{m_wallet.use_count()};
        B3FlowMeshTradingPanel panel;
        AttachOfflineWallet(panel);
        const std::weak_ptr<interfaces::Wallet> backend{panel.m_backend};
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        QVERIFY(m_model->setWalletLocked(false, SecureString{"offline display fixture passphrase"}));
        QVERIFY(!m_wallet->IsLocked());
        panel.m_unlock = std::make_unique<WalletModel::UnlockContext>(m_model.get(), true, true);
        panel.m_restore_locked = true;
        panel.m_relock_backend = panel.m_backend;
        panel.m_relock_wallet_name = panel.m_wallet_name;

        // Use the exact private production result and a real WalletImpl. The
        // synthetic worker represents a write which already crossed admission;
        // it performs no RPC/signature/network operation in this offline test.
        auto result{std::make_shared<B3FlowMeshTradingPanel::Result>()};
        B3FlowMeshTrading::Action action;
        action.operation = B3FlowMeshTrading::Operation::Order;
        action.market = panel.m_market_data.front();
        action.side = QStringLiteral("bid"); action.price = 1000; action.amount = 1;
        result->action = action; result->wallet = panel.m_wallet_name;
        result->write_attempted = true;
        B3FlowMeshTrading::Receipt receipt;
        receipt.action_id = QString::fromStdString(H(93).GetHex());
        receipt.state = included ? QStringLiteral("certified_inclusion") : QStringLiteral("unknown");
        receipt.certificate_verified = included;
        if (included) { receipt.microblock_hash = QString::fromStdString(H(94).GetHex()); receipt.microblock_sequence = 4; }
        result->receipt = receipt;
        if (!included) result->error = QStringLiteral("Synthetic response unavailable after an attempted write");
        panel.m_active_result = result;
        panel.m_busy = true;
        const auto generation{panel.m_generation};
        QSemaphore entered;
        std::atomic_bool finished{false}, saw_cancel{false}, unlocked_until_finished{false};
        int delivered_callbacks{0}, accepted_callbacks{0};
        struct DrainOnExit {
            B3FlowMeshTradingPanel& panel;
            ~DrainOnExit() { panel.cancelAndWait(); }
        } drain{panel}; // Also drain before captured locals die after a failed assertion.
        panel.m_thread = QThread::create([owner = panel.m_backend, cancel = panel.m_cancel,
                                         &entered, &finished, &saw_cancel, &unlocked_until_finished, completion_queued] {
            entered.release();
            if (!completion_queued) {
                // Bound even a broken cancellation path, so a failing test
                // cannot hang while the panel destructor drains its worker.
                QDeadlineTimer deadline{1000};
                while (!cancel->load() && !deadline.hasExpired()) QThread::msleep(1);
            }
            saw_cancel.store(cancel->load());
            unlocked_until_finished.store(!owner->isLocked());
            finished.store(true);
        });
        panel.m_thread->setParent(&panel);
        QObject::connect(panel.m_thread, &QThread::finished, &panel,
            [&panel, result, generation, &delivered_callbacks, &accepted_callbacks] {
                ++delivered_callbacks;
                // The same queued completion/generation boundary as startJob.
                if (generation == panel.m_generation) {
                    ++accepted_callbacks;
                    panel.finishJob(result);
                }
            }, Qt::QueuedConnection);
        panel.m_thread->start();
        QVERIFY2(entered.tryAcquire(1, 1000), "Synthetic wallet worker did not start within the test bound");
        if (completion_queued) {
            // Wait without dispatching GUI events: the finished signal is now
            // queued to this live panel but must not run before detachment.
            QVERIFY(panel.m_thread->wait(1000));
            QVERIFY(finished.load());
            QCOMPARE(delivered_callbacks, 0);
        } else {
            QVERIFY(!finished.load());
        }
        QVERIFY(!backend.expired()); QVERIFY(panel.m_active_result);

        panel.setWalletModel(nullptr);
        QVERIFY(finished.load());
        QCOMPARE(saw_cancel.load(), !completion_queued);
        QVERIFY(unlocked_until_finished.load()); // Relock must follow the drain.
        QVERIFY(m_wallet->IsLocked());
        QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_unlock);
        QVERIFY(!panel.m_backend); QVERIFY(!panel.m_relock_backend); QVERIFY(!panel.m_wallet);
        QVERIFY(!panel.m_restore_locked); QVERIFY(panel.m_security_warning.isEmpty());
        QVERIFY(!panel.m_busy); QVERIFY(panel.m_generation > generation);
        QVERIFY(backend.expired()); QCOMPARE(m_wallet.use_count(), wallet_owners);
        QCOMPARE(panel.m_uncertain, !included); QVERIFY(!panel.m_uncertain_refreshed);
        QVERIFY(panel.m_receipt); QCOMPARE(panel.m_receipt->action_id, receipt.action_id);
        QCOMPARE(panel.m_receipt->state, receipt.state);
        QCOMPARE(panel.m_receipt->certificate_verified, included);
        QCOMPARE(panel.m_pending_market, action.market.id);
        QCOMPARE(panel.m_pending_account, action.market.account);
        if (included) {
            QVERIFY(!panel.m_pending_sequence);
            QCOMPARE(panel.m_receipt->microblock_hash, receipt.microblock_hash);
        } else {
            QCOMPARE(panel.m_uncertain_action_id, receipt.action_id);
            QVERIFY(panel.m_pending_sequence); QCOMPARE(*panel.m_pending_sequence, action.market.sequence);
        }
        const QString details{panel.m_uncertain_details};
        QVERIFY(details.contains(receipt.action_id)); QVERIFY(details.contains(result->wallet));

        // Qt may deliver a queued signal even after disconnect. The stale
        // completion must not attach a wallet, accept a receipt, or retry.
        QCoreApplication::sendPostedEvents(&panel, QEvent::MetaCall);
        QCoreApplication::processEvents();
        QCOMPARE(delivered_callbacks, completion_queued ? 1 : 0);
        QCOMPARE(accepted_callbacks, 0);
        QVERIFY(backend.expired()); QCOMPARE(m_wallet.use_count(), wallet_owners);
        QCOMPARE(panel.m_uncertain, !included); QCOMPARE(panel.m_uncertain_details, details);
        QVERIFY(panel.m_receipt); QCOMPARE(panel.m_receipt->action_id, receipt.action_id);
        QCOMPARE(panel.m_receipt->state, receipt.state);
        QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_unlock);
        QVERIFY(!panel.m_order->isEnabled()); QVERIFY(!panel.m_retry_receipt->isEnabled());
        QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
    }
    void savedRequestsHaveDedicatedReadOnlyStatusControls()
    {
        B3FlowMeshTradingPanel panel;
        QVERIFY(panel.findChild<QComboBox*>(QStringLiteral("flowMeshSavedActions")));
        QVERIFY(panel.findChild<QLabel*>(QStringLiteral("flowMeshReceiptCard")));
        const auto check{panel.findChild<QPushButton*>(QStringLiteral("flowMeshCheckReceipt"))};
        QVERIFY(check); QVERIFY(!check->isEnabled());
    }

    void restoredCardsPreserveUnknownAndPriorCertifiedInstructions()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        B3FlowMeshTrading::SavedActions saved; saved.account = panel.m_snapshot->account;
        B3FlowMeshTrading::SavedAction action;
        action.market = panel.m_snapshot->market; action.domain = panel.m_snapshot->domain; action.config = panel.m_snapshot->config;
        action.account = saved.account; action.sequence = 7; action.type = 0; action.initial_submission_ms = 1000;
        action.signed_bytes_sha256 = QString::fromStdString(H(96).GetHex()); action.signed_bytes_size = 279; action.may_have_been_sent = true;
        action.receipt.action_id = QString::fromStdString(H(95).GetHex()); action.receipt.state = QStringLiteral("unknown");
        saved.actions.push_back(action); panel.restoreSavedActions(saved);
        QVERIFY(panel.m_saved_actions_ready); QVERIFY(panel.receiptWalletSelected()); QVERIFY(panel.m_uncertain);
        QCOMPARE(panel.m_receipt->action_id, action.receipt.action_id); QCOMPARE(panel.m_pending_sequence, action.sequence);
        QVERIFY(panel.m_receipt_card->text().contains(action.receipt.action_id));
        QVERIFY(panel.m_receipt_card->text().contains(action.signed_bytes_sha256));
        QVERIFY(panel.m_receipt_card->text().contains(QStringLiteral("Original account sequence: 7")));
        QVERIFY(panel.m_check_receipt->isEnabled()); QVERIFY(panel.m_retry_receipt->isEnabled());
        QVERIFY(!panel.m_order->isEnabled()); QVERIFY(!panel.m_thread);

        auto unavailable{std::make_shared<B3FlowMeshTradingPanel::Result>()}; unavailable->receipt_only = true;
        unavailable->receipt_market = action.market; unavailable->receipt_account = action.account; unavailable->receipt_action_id = action.receipt.action_id;
        unavailable->error = QStringLiteral("HTTPS endpoint unavailable"); panel.finishJob(unavailable);
        QVERIFY(!panel.m_thread); QVERIFY(panel.m_uncertain); QVERIFY(panel.m_check_receipt->isEnabled());
        QVERIFY(panel.m_receipt_card->text().contains(QStringLiteral("Original instruction and protections retained")));
        QCOMPARE(panel.m_receipt->action_id, action.receipt.action_id); QCOMPARE(panel.m_pending_sequence, action.sequence);

        // A durable prior certification is not a fresh verification claim and
        // cannot offer exact retry while current authority is unavailable.
        saved.actions[0].receipt.no_resubmit = true;
        panel.restoreSavedActions(saved); // No selection change or fresh status required to renew the guard.
        QVERIFY(panel.m_receipt->no_resubmit); QVERIFY(!panel.m_receipt->Included());
        QVERIFY(panel.m_retry_receipt->isHidden()); QVERIFY(!panel.m_retry_receipt->isEnabled());
        QVERIFY(!panel.m_uncertain); QVERIFY(!panel.m_pending_sequence);
        QVERIFY(panel.m_receipt_card->text().contains(QStringLiteral("fresh verification label requires current evidence")));
        panel.retryReceipt(); QVERIFY(!panel.m_thread);
        panel.finishJob(unavailable); QVERIFY(panel.m_receipt->no_resubmit); QVERIFY(!panel.m_receipt->Included());

        // A later status response cannot forget the durable replay guard.
        auto unknown{action.receipt}; panel.applyReceipt(unknown);
        QVERIFY(panel.m_receipt->no_resubmit); QVERIFY(!panel.m_retry_receipt->isEnabled());
        auto changed{saved}; changed.actions[0].sequence = 8; panel.restoreSavedActions(changed);
        QVERIFY(!panel.m_saved_actions_ready); QVERIFY(!panel.m_order->isEnabled());
        QCOMPARE(panel.m_saved_actions.actions[0].sequence, action.sequence);
        QVERIFY(panel.m_receipt_card->text().contains(QStringLiteral("identity or its durable protections changed")));
        panel.cancelAndWait(); QVERIFY(!panel.m_backend); QVERIFY(!panel.m_relock_backend);
        QVERIFY(!panel.m_wallet); QVERIFY(!panel.m_thread); QVERIFY(panel.m_receipt->no_resubmit);
    }

    void savedCardsRejectOtherWalletOrMarketAttribution()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        panel.m_saved_actions_ready = false; panel.updateControls();
        QVERIFY(!panel.m_order->isEnabled());
        B3FlowMeshTrading::SavedActions saved; saved.account = QString::fromStdString(H(97).GetHex());
        panel.restoreSavedActions(saved); QVERIFY(!panel.m_saved_actions_ready);
        QVERIFY(panel.m_receipt_card->text().contains(QStringLiteral("selected wallet account")));
        saved.account = panel.m_snapshot->account;
        B3FlowMeshTrading::SavedAction action; action.account = saved.account;
        action.market = panel.m_snapshot->market; action.domain = panel.m_snapshot->domain;
        action.config = QString::fromStdString(H(98).GetHex()); action.receipt.action_id = QString::fromStdString(H(95).GetHex());
        saved.actions.push_back(action); panel.restoreSavedActions(saved);
        QVERIFY(!panel.m_saved_actions_ready); QVERIFY(!panel.m_check_receipt->isEnabled());
        QVERIFY(panel.m_receipt_card->text().contains(QStringLiteral("configuration does not match")));
        QVERIFY(!panel.m_receipt); QVERIFY(!panel.m_thread);
    }

    void sameActionIdInDifferentMarketsDoesNotShareReplayOrUncertaintyState()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        B3FlowMeshTrading::SavedActions saved; saved.account = panel.m_snapshot->account;
        B3FlowMeshTrading::SavedAction a;
        a.market = panel.m_snapshot->market; a.domain = panel.m_snapshot->domain; a.config = panel.m_snapshot->config;
        a.account = saved.account; a.sequence = 7; a.initial_submission_ms = 1000; a.signed_bytes_size = 279;
        a.signed_bytes_sha256 = QString::fromStdString(H(96).GetHex()); a.may_have_been_sent = true;
        a.receipt.action_id = QString::fromStdString(H(95).GetHex()); a.receipt.state = QStringLiteral("unknown"); a.receipt.no_resubmit = true;
        auto b{a}; b.market = QString::fromStdString(flowmesh::ComputeFlowMeshMarketId(H(1), H(98))->GetHex());
        b.receipt.no_resubmit = false; b.initial_submission_ms = 1001; b.signed_bytes_sha256 = QString::fromStdString(H(97).GetHex());
        saved.actions = {a, b}; panel.restoreSavedActions(saved);
        // Unknown B is selected first. Its semantic ActionId deliberately
        // equals A's: the signed market domain, not ActionId, separates them.
        QCOMPARE(panel.m_pending_market, b.market); QVERIFY(panel.m_uncertain);
        QCOMPARE(panel.m_uncertain_market, b.market); QVERIFY(!panel.m_receipt->no_resubmit);
        const auto select = [&](const B3FlowMeshTrading::SavedAction& action) {
            const auto index{panel.m_saved_selector->findData(QString{action.market + QLatin1Char(':') + action.receipt.action_id}, Qt::UserRole + 1)};
            QVERIFY(index >= 0); panel.m_saved_selector->setCurrentIndex(index);
        };
        select(a); QCOMPARE(panel.m_receipt->market, a.market); QVERIFY(panel.m_receipt->no_resubmit);
        QVERIFY(panel.m_uncertain); QCOMPARE(panel.m_uncertain_market, b.market); // A cannot resolve B.
        select(b); QCOMPARE(panel.m_receipt->market, b.market); QVERIFY(!panel.m_receipt->no_resubmit);
        QVERIFY(panel.m_uncertain); QCOMPARE(panel.m_uncertain_market, b.market);
        QVERIFY(panel.m_retry_receipt->isEnabled()); QCOMPARE(panel.m_pending_sequence, b.sequence);
        QVERIFY(!panel.m_saved_actions.actions[1].receipt.no_resubmit); QVERIFY(!panel.m_thread);
        // A read which captured A before local restoration selected B may
        // finish late. Its certificate must not become B's certificate/guard.
        auto late_a{a.receipt}; late_a.market = a.market; late_a.account = a.account;
        late_a.state = QStringLiteral("certified_inclusion"); late_a.certificate_verified = true;
        late_a.microblock_hash = QString::fromStdString(H(99).GetHex()); late_a.microblock_sequence = 9;
        panel.applyReceipt(late_a); panel.updateControls();
        QCOMPARE(panel.m_receipt->market, b.market); QVERIFY(!panel.m_receipt->Included()); QVERIFY(!panel.m_receipt->no_resubmit);
        QCOMPARE(panel.m_pending_market, b.market); QCOMPARE(panel.m_pending_sequence, b.sequence);
        QVERIFY(panel.m_uncertain); QCOMPARE(panel.m_uncertain_market, b.market); QVERIFY(panel.m_retry_receipt->isEnabled());
        QVERIFY(panel.m_saved_actions.actions[0].receipt.Included()); QVERIFY(panel.m_saved_actions.actions[0].receipt.no_resubmit);
        QVERIFY(!panel.m_saved_actions.actions[1].receipt.Included()); QVERIFY(!panel.m_saved_actions.actions[1].receipt.no_resubmit);
        QVERIFY(panel.m_receipt_card->text().contains(b.market));
        QVERIFY(!panel.m_receipt_card->text().contains(QStringLiteral("Exact action inclusion verified")));
        B3FlowMeshTradingPanel::Result error_a;
        error_a.receipt_market = a.market; error_a.receipt_account = a.account; error_a.receipt_action_id = a.receipt.action_id;
        panel.applyReceiptError(error_a, QStringLiteral("Only market A's status request failed"));
        QVERIFY(!panel.m_receipt_card->text().contains(QStringLiteral("Only market A"))); QVERIFY(panel.m_receipt_error.isEmpty());
        auto error_b{error_a}; error_b.receipt_market = b.market;
        panel.applyReceiptError(error_b, QStringLiteral("Market B status unavailable"));
        QVERIFY(panel.m_receipt_card->text().contains(QStringLiteral("Market B status unavailable")));
        panel.applyReceiptError(error_a, QString{}); // A's success cannot clear B's error either.
        QVERIFY(panel.m_receipt_card->text().contains(QStringLiteral("Market B status unavailable")));
        QVERIFY(panel.m_uncertain); QVERIFY(!panel.m_receipt->no_resubmit);
    }

    void receiptIdentitySurvivesSwitchWithoutPinningTheOldWallet()
    {
        B3FlowMeshTradingPanel panel;
        auto other{MakeOfflineWallet("different-offline-wallet")};
        const auto wallet_owners{m_wallet.use_count()};
        const auto other_owners{other.wallet.use_count()};
        AttachOfflineWallet(panel);
        const std::weak_ptr<interfaces::Wallet> original_backend{panel.m_backend};
        B3FlowMeshTrading::Receipt receipt;
        receipt.action_id = QString::fromStdString(H(95).GetHex()); receipt.state = QStringLiteral("unknown");
        panel.m_pending_market = panel.m_snapshot->market; panel.m_pending_account = panel.m_snapshot->account;
        panel.m_pending_sequence = panel.m_snapshot->account_sequence;
        panel.m_receipt_wallet = panel.m_wallet;
        panel.applyReceipt(receipt);
        panel.setWalletModel(nullptr);
        QVERIFY(original_backend.expired()); QCOMPARE(m_wallet.use_count(), wallet_owners);

        AttachOfflineWallet(panel, *other.model, other.wallet);
        const std::weak_ptr<interfaces::Wallet> other_backend{panel.m_backend};
        QVERIFY(!panel.receiptWalletSelected()); QVERIFY(!panel.uncertainWalletSelected());
        QVERIFY(!panel.m_retry_receipt->isEnabled()); QVERIFY(panel.m_uncertain);
        panel.retryReceipt(); panel.startJob(std::nullopt, std::nullopt, true);
        QVERIFY(!panel.m_confirmation); QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result);
        QCOMPARE(panel.m_receipt->action_id, receipt.action_id);
        panel.setWalletModel(nullptr);
        QVERIFY(other_backend.expired()); QCOMPARE(other.wallet.use_count(), other_owners);

        // The same still-live model, not its display name, restores identity.
        // A new WalletImpl may be acquired without resurrecting the old owner.
        AttachOfflineWallet(panel);
        QVERIFY(panel.receiptWalletSelected()); QVERIFY(panel.uncertainWalletSelected());
        QVERIFY(original_backend.expired()); QVERIFY(panel.m_retry_receipt->isEnabled());
        QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_unlock);
        QCOMPARE(panel.m_receipt->action_id, receipt.action_id); QVERIFY(panel.m_uncertain);
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        bool exact_review{false};
        QTimer::singleShot(0, &panel, [&] {
            if (panel.m_confirmation) {
                exact_review = panel.m_confirmation->text().contains(receipt.action_id) && panel.m_confirmation->text().contains(QStringLiteral("already retained signed bytes"));
                panel.m_confirmation->done(QMessageBox::Cancel);
            }
        });
        panel.retryReceipt();
        QVERIFY(exact_review); QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
        QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result);
        panel.setWalletModel(nullptr);
        QCOMPARE(m_wallet.use_count(), wallet_owners);
    }
    void unloadedReceiptOwnerCannotRebindByDisplayName()
    {
        B3FlowMeshTradingPanel panel;
        auto original{MakeOfflineWallet(m_wallet->GetName())};
        QCOMPARE(original.model->getDisplayName(), m_model->getDisplayName());
        const std::weak_ptr<wallet::CWallet> original_wallet{original.wallet};
        AttachOfflineWallet(panel, *original.model, original.wallet);
        const std::weak_ptr<interfaces::Wallet> original_backend{panel.m_backend};
        B3FlowMeshTrading::Receipt receipt;
        receipt.action_id = QString::fromStdString(H(96).GetHex()); receipt.state = QStringLiteral("unknown");
        panel.m_pending_market = panel.m_snapshot->market; panel.m_pending_account = panel.m_snapshot->account;
        panel.m_pending_sequence = panel.m_snapshot->account_sequence;
        panel.m_receipt_wallet = panel.m_wallet; panel.applyReceipt(receipt);
        const QString details{panel.m_uncertain_details};
        panel.setWalletModel(nullptr);
        QVERIFY(original_backend.expired());
        original.model.reset(); original.wallet.reset();
        QVERIFY(original_wallet.expired());
        QVERIFY(!panel.m_receipt_wallet); QVERIFY(!panel.m_uncertain_wallet);

        // Same displayed name and synthetic account fields deliberately do not
        // authorize a different WalletModel to query/retry the saved request.
        AttachOfflineWallet(panel);
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        QVERIFY(!panel.receiptWalletSelected()); QVERIFY(!panel.uncertainWalletSelected());
        panel.retryReceipt(); panel.startJob(std::nullopt, std::nullopt, true);
        QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_confirmation);
        QVERIFY(!panel.m_retry_receipt->isEnabled()); QVERIFY(!panel.m_order->isEnabled());
        QVERIFY(panel.m_uncertain); QCOMPARE(panel.m_uncertain_details, details);
        // A fresh read permits explicit acknowledgement, not implicit outcome
        // resolution or an identity rebind. Feed the real completion handler.
        auto refresh{std::make_shared<B3FlowMeshTradingPanel::Result>()};
        refresh->markets = panel.m_market_data; refresh->snapshot = panel.m_snapshot;
        panel.finishJob(refresh);
        QVERIFY(panel.m_uncertain); QVERIFY(panel.m_uncertain_refreshed);
        QVERIFY(!panel.receiptWalletSelected()); QVERIFY(!panel.m_retry_receipt->isEnabled());
        QVERIFY(panel.m_review_uncertain->isEnabled()); QVERIFY(!panel.m_thread);
        QString warning;
        bool cancel_default{false};
        QTimer::singleShot(0, &panel, [&] {
            if (panel.m_confirmation) {
                warning = panel.m_confirmation->text();
                cancel_default = panel.m_confirmation->defaultButton() == panel.m_confirmation->button(QMessageBox::Cancel);
                panel.m_confirmation->done(QMessageBox::Cancel);
            }
        });
        panel.reviewUncertain();
        QVERIFY(warning.contains(QStringLiteral("original wallet instance was unloaded")));
        QVERIFY(warning.contains(QStringLiteral("does not resolve the original request")));
        QVERIFY(warning.contains(QStringLiteral("automatic retry is disabled")));
        QVERIFY(warning.contains(receipt.action_id)); QVERIFY(cancel_default);
        QVERIFY(panel.m_uncertain); QCOMPARE(panel.m_uncertain_details, details);
        QVERIFY(panel.m_receipt); QCOMPARE(panel.m_receipt->action_id, receipt.action_id);
        QCOMPARE(panel.m_receipt->state, QStringLiteral("unknown"));
        QVERIFY(!panel.m_receipt_wallet); QVERIFY(!panel.m_uncertain_wallet);
        QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_unlock);
        QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
    }
    void queuedOldModelUnloadCannotDetachReplacement()
    {
        B3FlowMeshTradingPanel panel;
        auto original{MakeOfflineWallet("queued-old-model")};
        auto replacement{MakeOfflineWallet("replacement-model")};
        // These unregistered wallets intentionally yield no backend, hence no
        // RPC, but public setWalletModel installs the real lifecycle callbacks.
        panel.setWalletModel(original.model.get());
        QCOMPARE(panel.m_wallet.data(), original.model.get());
        QVERIFY(!panel.m_backend); QVERIFY(!panel.m_thread);
        const auto original_generation{panel.m_generation};
        // A cross-thread signal queues the production AutoConnection. Keep the
        // QObject alive on its owning thread; never destroy a model off-thread.
        auto emitter{std::unique_ptr<QThread>{QThread::create([model = original.model.get()] { Q_EMIT model->unload(); })}};
        emitter->start();
        const bool emitted{emitter->wait(1000)};
        if (!emitted) emitter->wait();
        QVERIFY(emitted);
        panel.setWalletModel(replacement.model.get());
        QCOMPARE(panel.m_wallet.data(), replacement.model.get());
        const auto replacement_generation{panel.m_generation};
        QVERIFY(replacement_generation > original_generation);
        QCoreApplication::sendPostedEvents(&panel, QEvent::MetaCall);
        QCoreApplication::processEvents();
        QCOMPARE(panel.m_wallet.data(), replacement.model.get());
        QCOMPARE(panel.m_generation, replacement_generation);
        QVERIFY(!panel.m_backend); QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result);
        panel.setWalletModel(nullptr);
    }
    void unchangedReplyContainsNoInventedEmptyHistory()
    {
        const auto full{Data()}; UniValue small{UniValue::VOBJ};
        for (const auto* key : {"market_id", "base_asset_id", "domain", "execution_config_id", "quote_asset", "matching_model", "quantity_lot_raw", "price_tick_raw", "snapshot"}) small.pushKV(key, full[key]);
        small.pushKV("unchanged", true); const auto s{Parse(small)}; QVERIFY(s.unchanged); QVERIFY(!s.units.known); QVERIFY(s.history.empty());
    }
    void catalogSurvivesUncertifiedFirstMarket()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        const auto certified{panel.m_market_data.front()};
        const auto snapshot{*panel.m_snapshot};
        B3FlowMeshTrading::Market uncertified;
        uncertified.domain = certified.domain; uncertified.config = certified.config;
        uncertified.base = QString::fromStdString(H(90).GetHex());
        const auto id{*flowmesh::ComputeFlowMeshMarketId(H(1), H(90))};
        uncertified.id = QString::fromStdString(id.GetHex());
        uncertified.vault = QString::fromStdString(flowmesh::ComputeFlowMeshVaultId(H(1), id)->GetHex());
        uncertified.remote = true;
        panel.m_market_data.clear(); panel.m_snapshot.reset();
        { QSignalBlocker blocked{panel.m_market}; panel.m_market->clear(); }
        panel.m_response_age.invalidate(); panel.m_loading = true;

        // Production completed-result boundary: discovery succeeded, then the
        // first remote market's balance RPC rejected its uncertified head.
        auto failed{std::make_shared<B3FlowMeshTradingPanel::Result>()};
        failed->catalog = true; failed->markets = {uncertified, certified};
        failed->error = QStringLiteral("FlowMesh client snapshot has no certified head");
        panel.finishJob(failed);
        QCOMPARE(panel.m_market->count(), 2);
        QVERIFY(panel.market()); QCOMPARE(panel.market()->id, uncertified.id);
        QVERIFY(panel.m_market->isEnabled()); QVERIFY(panel.m_catalog_age.isValid());
        QVERIFY(panel.m_read_failed); QVERIFY(!panel.m_loading); QVERIFY(!panel.m_snapshot);
        panel.updateMarketText(); // Periodic updates must retain the exact error.
        QVERIFY(panel.m_status->text().contains(failed->error));
        QVERIFY(!panel.m_status->text().contains(QStringLiteral("Loading")));
        QVERIFY(!panel.m_liquidity_note->text().contains(QStringLiteral("Loading")));
        QVERIFY(!panel.m_order->isEnabled()); QVERIFY(!panel.m_deposit->isEnabled());
        QVERIFY(!panel.m_withdraw->isEnabled()); QVERIFY(!panel.m_checkpoint->isEnabled());
        QVERIFY(!panel.m_publish->isEnabled()); QVERIFY(!panel.m_admit->isEnabled());

        // Another listed market stays selectable, and its independently
        // successful read can make only that market ready.
        ReadInFlight(panel);
        panel.m_market->setCurrentIndex(1);
        QCOMPARE(panel.market()->id, certified.id);
        QVERIFY(!panel.m_status->text().contains(failed->error));
        auto ready{std::make_shared<B3FlowMeshTradingPanel::Result>()};
        ready->markets = failed->markets; ready->snapshot = snapshot;
        panel.finishJob(ready);
        QCOMPARE(panel.m_market->currentIndex(), 1);
        QVERIFY(panel.m_snapshot && *panel.m_snapshot == snapshot);
        QVERIFY(!panel.m_read_failed); QVERIFY(panel.m_order->isEnabled());
        QCOMPARE(panel.m_chart->pricePointCount(), 1);
        QVERIFY(!panel.m_thread); QVERIFY(!failed->write_attempted); QVERIFY(!ready->write_attempted);
        QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
    }
    void catalogFailureRetainsCertifiedDisplayAndPausesActions()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        const auto snapshot{*panel.m_snapshot};
        const auto original{panel.m_market_data.front()};
        const auto balances{panel.m_balances->text()};
        const auto history{panel.m_history_view->item(0, 1)->text()};
        auto discovery{original}; discovery.has_account = false; discovery.account.clear();
        discovery.base_available = discovery.base_reserved = discovery.b3_available = discovery.b3_reserved = 0;
        discovery.ready = discovery.publish_ready = false;
        auto additional{discovery}; additional.id = QString::fromStdString(H(91).GetHex());
        auto failed{std::make_shared<B3FlowMeshTradingPanel::Result>()};
        failed->catalog = true; failed->markets = {discovery, additional};
        failed->error = QStringLiteral("Synthetic selected-market read unavailable");
        panel.finishJob(failed);
        QCOMPARE(panel.m_market->count(), 2);
        QCOMPARE(panel.market()->id, original.id);
        QCOMPARE(panel.market()->account, original.account);
        QVERIFY(panel.m_snapshot && *panel.m_snapshot == snapshot);
        QCOMPARE(panel.m_balances->text(), balances);
        QCOMPARE(panel.m_history_view->item(0, 1)->text(), history);
        QCOMPARE(panel.m_chart->pricePointCount(), 1);
        QVERIFY(panel.m_read_failed); QVERIFY(panel.m_market->isEnabled());
        panel.updateMarketText(); QVERIFY(panel.m_status->text().contains(failed->error));
        QVERIFY(!panel.m_order->isEnabled()); QVERIFY(!panel.m_cancel_order->isEnabled());
        QVERIFY(!panel.m_deposit->isEnabled()); QVERIFY(!panel.m_admit->isEnabled());
        QVERIFY(!panel.m_withdraw->isEnabled()); QVERIFY(!panel.m_checkpoint->isEnabled());
        QVERIFY(!panel.m_publish->isEnabled()); QVERIFY(m_wallet->IsLocked());
    }
    void queuedCatalogFromPreviousWalletIsDiscarded()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        auto result{std::make_shared<B3FlowMeshTradingPanel::Result>()};
        result->catalog = true; result->markets = panel.m_market_data;
        result->error = QStringLiteral("Old wallet selected-market read failed");
        const auto generation{panel.m_generation};
        int delivered{0}, accepted{0};
        QMetaObject::invokeMethod(&panel, [&] {
            ++delivered;
            if (generation == panel.m_generation) { ++accepted; panel.finishJob(result); }
        }, Qt::QueuedConnection);
        auto replacement{MakeOfflineWallet("catalog-replacement-wallet")};
        panel.setWalletModel(replacement.model.get());
        QCoreApplication::sendPostedEvents(&panel, QEvent::MetaCall);
        QCOMPARE(delivered, 1); QCOMPARE(accepted, 0);
        QCOMPARE(panel.m_wallet.data(), replacement.model.get());
        QCOMPARE(panel.m_market->count(), 0); QVERIFY(panel.m_market_data.empty());
        QVERIFY(!panel.m_snapshot); QVERIFY(!panel.m_read_failed);
        QVERIFY(!panel.m_status->text().contains(result->error));
        QVERIFY(!panel.m_order->isEnabled()); QVERIFY(!panel.m_thread);
        panel.setWalletModel(nullptr);
    }
    void failedCatalogReadDoesNotReplaceExistingMarkets()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        const auto original{panel.m_market_data.front()};
        const auto snapshot{*panel.m_snapshot};
        auto failed{std::make_shared<B3FlowMeshTradingPanel::Result>()};
        // No successfully parsed catalog: an empty or partially filled result
        // cannot remove the cached choices or replace certified account data.
        failed->error = QStringLiteral("Synthetic market discovery unavailable");
        panel.finishJob(failed);
        QCOMPARE(panel.m_market->count(), 1); QCOMPARE(panel.market()->id, original.id);
        QCOMPARE(panel.market()->account, original.account);
        QVERIFY(panel.m_snapshot && *panel.m_snapshot == snapshot);
        QVERIFY(panel.m_read_failed); QVERIFY(!panel.m_order->isEnabled());
        QVERIFY(!panel.m_catalog_age.isValid()); QVERIFY(m_wallet->IsLocked());
    }
    void connectionControlsDistinguishTransportFromMarketReadiness()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        QVERIFY(panel.findChild<QLineEdit*>(QStringLiteral("flowMeshEndpoint")));
        QVERIFY(panel.findChild<QPushButton*>(QStringLiteral("flowMeshConnect")));
        QVERIFY(panel.findChild<QLabel*>(QStringLiteral("flowMeshConnectionStatus")));
        QVERIFY(!panel.m_connect->isEnabled());
        panel.m_client_info = ConnectionInfo(""); panel.updateControls();
        QVERIFY(panel.m_connection_status->text().contains(QStringLiteral("No HTTPS endpoint")));
        panel.m_endpoint->setText(QStringLiteral("http://insecure.invalid")); QVERIFY(!panel.m_connect->isEnabled());
        panel.m_endpoint->setText(QStringLiteral("https://trading.invalid")); QVERIFY(panel.m_connect->isEnabled());
        panel.m_client_info = ConnectionInfo("https://trading.invalid", false); panel.updateControls();
        QVERIFY(panel.m_connection_status->text().contains(QStringLiteral("not yet checked")));
        panel.m_client_info = ConnectionInfo("https://trading.invalid", true, false, false); panel.updateControls();
        QVERIFY(panel.m_connection_status->text().contains(QStringLiteral("HTTPS connection failed")));
        panel.m_client_info->pushKV("active_endpoint", "https://fallback.invalid"); panel.updateControls();
        QVERIFY(panel.m_connection_status->text().contains(QStringLiteral("Last usable endpoint: https://fallback.invalid")));
        panel.m_client_info = ConnectionInfo("https://trading.invalid", true, true, false);
        panel.m_read_failed = true; panel.m_read_error = QStringLiteral("FlowMesh client snapshot has no certified head"); panel.updateMarketText();
        QVERIFY(panel.m_connection_status->text().contains(QStringLiteral("HTTPS responded")));
        QVERIFY(panel.m_connection_status->text().contains(QStringLiteral("no certified head")));
        QVERIFY(panel.m_status->text().contains(panel.m_read_error)); QVERIFY(!panel.m_order->isEnabled());
        panel.m_client_info = ConnectionInfo(); panel.updateControls();
        QVERIFY(panel.m_connection_status->text().contains(QStringLiteral("readiness is shown below")));
        QVERIFY(!panel.m_order->isEnabled()); // Transport success supplies no certificate.
        panel.m_client_info->pushKV("backend", "local"); panel.m_client_info->pushKV("engine_enabled", true); panel.updateControls();
        QVERIFY(panel.m_connection_status->text().contains(QStringLiteral("Local market engine")));
        QVERIFY(!panel.m_connect->isEnabled()); QVERIFY(!panel.m_endpoint->isEnabled());
        panel.requestConnect(); QVERIFY(!panel.m_thread); QVERIFY(!panel.m_deferred_connect);
    }
    void connectUsesExistingWorkerWithoutUnlockOrSubmission()
    {
        ConnectionProbe probe; B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        struct Cleanup { ConnectionProbe& probe; B3FlowMeshTradingPanel& panel; ~Cleanup() { probe.release.release(8); panel.cancelAndWait(); } } cleanup{probe, panel};
        panel.m_client_info = ConnectionInfo(""); panel.m_endpoint->setText(QStringLiteral("https://chosen.invalid")); panel.updateControls();
        const auto snapshot{*panel.m_snapshot}; const auto balances{panel.m_balances->text()};
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        panel.m_connect->click(); QVERIFY(probe.entered.tryAcquire(1, 1000));
        const auto active{panel.m_active_result}; QVERIFY(active); QVERIFY(!active->action); QVERIFY(!active->exact_retry);
        QCOMPARE(active->connect_url, QStringLiteral("https://chosen.invalid")); QVERIFY(!active->write_attempted);
        QVERIFY(panel.m_snapshot && *panel.m_snapshot == snapshot); QCOMPARE(panel.m_balances->text(), balances);
        QVERIFY(panel.m_connection_status->text().contains(QStringLiteral("Connecting to")));
        QVERIFY(!panel.m_order->isEnabled()); QVERIFY(!panel.m_connect->isEnabled());
        panel.requestConnect(); QCOMPARE(probe.count.load(), 1); // Coalesced while running.
        panel.m_endpoint->setText(QStringLiteral("https://still-typing.invalid"));
        panel.m_endpoint->setCursorPosition(8); const int cursor{panel.m_endpoint->cursorPosition()};
        probe.release.release(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QCOMPARE(probe.count.load(), 1); QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
        QVERIFY(!active->write_attempted); QVERIFY(!panel.m_uncertain); QVERIFY(!panel.m_confirmation);
        QCOMPARE(panel.m_endpoint->text(), QStringLiteral("https://still-typing.invalid"));
        QCOMPARE(panel.m_endpoint->cursorPosition(), cursor);
        QVERIFY(panel.m_snapshot && *panel.m_snapshot == snapshot); QCOMPARE(panel.m_balances->text(), balances);
        QVERIFY(panel.m_read_failed); QVERIFY(!panel.m_order->isEnabled());
        { std::lock_guard lock{probe.mutex}; QCOMPARE(probe.requests.size(), size_t{1});
          QCOMPARE(probe.requests[0].first, B3AssetTransfer::WalletUri(m_model->getWalletName()));
          QCOMPARE(probe.requests[0].second, std::string{"https://chosen.invalid"}); }
    }
    void connectQueuesOnceBehindPassiveRead()
    {
        ConnectionProbe probe; B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        struct Cleanup { ConnectionProbe& probe; B3FlowMeshTradingPanel& panel; ~Cleanup() { probe.release.release(8); panel.cancelAndWait(); } } cleanup{probe, panel};
        panel.m_client_info = ConnectionInfo(); panel.m_endpoint->setText(QStringLiteral("https://queued.invalid")); panel.updateControls();
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        ReadInFlight(panel); const auto original{panel.m_thread};
        for (int i{0}; i < 5; ++i) panel.m_connect->click();
        QCOMPARE(panel.m_thread, original); QCOMPARE(probe.count.load(), 0); QVERIFY(panel.m_deferred_connect);
        QCOMPARE(panel.m_deferred_connect->url, QStringLiteral("https://queued.invalid"));
        QVERIFY(panel.m_connection_status->text().contains(QStringLiteral("Connect queued")));
        QVERIFY(!panel.m_order->isEnabled());
        panel.m_endpoint->setText(QStringLiteral("https://edited-after-click.invalid"));
        auto failed{std::make_shared<B3FlowMeshTradingPanel::Result>()}; failed->error = QStringLiteral("Synthetic passive read failed");
        panel.finishJob(failed); QVERIFY(probe.entered.tryAcquire(1, 1000));
        QVERIFY(!panel.m_deferred_connect); QCOMPARE(panel.m_active_result->connect_url, QStringLiteral("https://queued.invalid"));
        probe.release.release(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QCOMPARE(probe.count.load(), 1); QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
        QCOMPARE(panel.m_endpoint->text(), QStringLiteral("https://edited-after-click.invalid"));
    }
    void explicitConnectionErrorSurvivesAutomaticStatusReads()
    {
        ConnectionProbe probe; probe.fail = true;
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        struct Cleanup { ConnectionProbe& probe; B3FlowMeshTradingPanel& panel; ~Cleanup() { probe.release.release(8); panel.cancelAndWait(); } } cleanup{probe, panel};
        panel.m_client_info = ConnectionInfo(); panel.m_endpoint->setText(QStringLiteral("https://rejected.invalid")); panel.updateControls();
        panel.m_connect->click(); QVERIFY(probe.entered.tryAcquire(1, 1000));
        probe.release.release(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QVERIFY(panel.m_client_info); QVERIFY(panel.m_connection_error.isEmpty());
        QCOMPARE(panel.m_connect_error, QStringLiteral("Synthetic endpoint configuration rejected"));
        panel.updateMarketText(); QVERIFY(panel.m_connection_status->text().contains(panel.m_connect_error));
        QVERIFY(!panel.m_order->isEnabled()); QVERIFY(m_wallet->IsLocked());
        panel.setWalletModel(nullptr); QVERIFY(panel.m_connect_error.isEmpty());
    }
    void connectionWorkCannotFollowWalletSwitch_data()
    {
        QTest::addColumn<bool>("completed");
        QTest::newRow("queued-connect") << false;
        QTest::newRow("completion-queued") << true;
    }
    void connectionWorkCannotFollowWalletSwitch()
    {
        QFETCH(bool, completed);
        ConnectionProbe probe; B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        struct Cleanup { ConnectionProbe& probe; B3FlowMeshTradingPanel& panel; ~Cleanup() { probe.release.release(8); panel.cancelAndWait(); } } cleanup{probe, panel};
        panel.m_client_info = ConnectionInfo(); panel.m_endpoint->setText(QStringLiteral("https://old-wallet.invalid")); panel.updateControls();
        const auto generation{panel.m_generation};
        if (!completed) ReadInFlight(panel);
        panel.m_connect->click();
        if (completed) {
            QVERIFY(probe.entered.tryAcquire(1, 1000)); probe.release.release();
            QVERIFY(panel.m_thread->wait(1000)); // Finished callback queued, not applied.
        } else { QVERIFY(panel.m_deferred_connect); QCOMPARE(probe.count.load(), 0); }
        auto replacement{MakeOfflineWallet("connection-replacement-wallet")};
        panel.setWalletModel(replacement.model.get());
        QCoreApplication::sendPostedEvents(&panel, QEvent::MetaCall); QCoreApplication::processEvents();
        QVERIFY(panel.m_generation > generation); QCOMPARE(panel.m_wallet.data(), replacement.model.get());
        QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_deferred_connect);
        QVERIFY(!panel.m_client_info); QVERIFY(panel.m_connection_error.isEmpty()); QVERIFY(panel.m_connect_error.isEmpty());
        QVERIFY(!panel.m_snapshot); QVERIFY(!panel.m_order->isEnabled()); QVERIFY(!panel.m_connect->isEnabled());
        QCOMPARE(probe.count.load(), completed ? 1 : 0); QVERIFY(m_wallet->IsLocked());
        panel.setWalletModel(nullptr);
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
    void candleAggregationUsesActualClearingsAndSequenceBuckets()
    {
        const auto trade = [](uint64_t sequence, CAmount price, CAmount quantity) {
            Trade t; t.sequence = sequence; t.cleared = true; t.price = price; t.quantity = quantity; t.notional = price * quantity; return t;
        };
        QVERIFY(B3FlowMeshChart::AggregateCandles({}, 5).empty());
        // Endpoint order cannot choose open/close; empty sequence buckets have
        // no invented prices or volume. Unmatched auctions are not trades.
        Trade idle; idle.sequence = 6;
        std::vector<Trade> history{trade(21, 800, 50), trade(4, 1100, 400), idle, trade(2, 1300, 200), trade(8, 1200, 1), trade(1, 1000, 100), trade(3, 900, 300)};
        const auto unchanged{history};
        const auto candles{B3FlowMeshChart::AggregateCandles(history, 5)};
        QCOMPARE(candles.size(), size_t{3});
        const auto& first{candles.front()};
        QCOMPARE(first.bucket_sequence, uint64_t{0}); QCOMPARE(first.first_sequence, uint64_t{1}); QCOMPARE(first.last_sequence, uint64_t{4});
        QCOMPARE(first.open, CAmount{1000}); QCOMPARE(first.high, CAmount{1300}); QCOMPARE(first.low, CAmount{900}); QCOMPARE(first.close, CAmount{1100});
        QCOMPARE(first.trades, size_t{4}); QVERIFY(first.base_volume == 1000); QVERIFY(first.quote_volume == 1'070'000);
        QCOMPARE(B3FlowMeshChart::FormatCandleVolume(first, 6, false), QStringLiteral("0.001"));
        QCOMPARE(B3FlowMeshChart::FormatCandleVolume(first, 6, true), QStringLiteral("0.00107"));
        QCOMPARE(candles[1].bucket_sequence, uint64_t{5}); QCOMPARE(candles[2].bucket_sequence, uint64_t{20});
        QCOMPARE(candles[1].open, candles[1].high); QCOMPARE(candles[1].high, candles[1].low); QCOMPARE(candles[1].low, candles[1].close);
        QCOMPARE(B3FlowMeshChart::AggregateCandles(history, 1).size(), size_t{6});
        QCOMPARE(B3FlowMeshChart::AggregateCandles(history, 20).size(), size_t{2});
        QVERIFY(history == unchanged);

        const auto reversed{B3FlowMeshChart::AggregateCandles(history, 5, true)};
        QCOMPARE(reversed.size(), candles.size());
        QCOMPARE(reversed[0].open, first.open); QCOMPARE(reversed[0].close, first.close);
        QCOMPARE(reversed[0].high, first.low); QCOMPARE(reversed[0].low, first.high);
        QCOMPARE(FormatDisplayPrice(reversed[0].high, 6, true), QStringLiteral("≈1.111111111111111111"));
        QVERIFY(reversed[0].base_volume == first.base_volume); QVERIFY(reversed[0].quote_volume == first.quote_volume);
        const auto boundary{B3FlowMeshChart::AggregateCandles({trade(std::numeric_limits<uint64_t>::max(), 1, 1)}, 20)};
        QCOMPARE(boundary.size(), size_t{1}); QCOMPARE(boundary.front().last_sequence, std::numeric_limits<uint64_t>::max());
    }
    void candleAggregationRejectsInvalidAmountsAndUndefinedInverseBuckets()
    {
        const auto valid{Parse(Data()).history.front()};
        QVERIFY(B3FlowMeshChart::AggregateCandles({valid}, 0).empty());
        QVERIFY(B3FlowMeshChart::AggregateCandles({valid, valid}, 5).empty());
        for (const int kind : {0, 1, 2, 3, 4, 5}) {
            auto invalid{valid};
            switch (kind) {
            case 0: invalid.price = -1; break;
            case 1: invalid.quantity = 0; break;
            case 2: invalid.price = MAX_MONEY; invalid.quantity = 2; break;
            case 3: ++invalid.notional; break;
            case 4: invalid.fee = -1; break;
            case 5: invalid.cleared = false; break;
            }
            QVERIFY(B3FlowMeshChart::AggregateCandles({invalid}, 5).empty());
        }
        auto zero{valid}; zero.sequence = 3; zero.price = zero.notional = zero.fee = 0;
        auto later{valid}; later.sequence = 9;
        const std::vector<Trade> history{valid, zero, later};
        QCOMPARE(B3FlowMeshChart::AggregateCandles(history, 5).size(), size_t{2});
        const auto inverse{B3FlowMeshChart::AggregateCandles(history, 5, true)};
        QCOMPARE(inverse.size(), size_t{1}); QCOMPARE(inverse.front().first_sequence, uint64_t{9});
        // A zero-price low means this entire candle's inverse high is
        // undefined; dropping only that trade would invent a finite high.
        QVERIFY(B3FlowMeshChart::AggregateCandles({valid, zero}, 5, true).empty());

        std::vector<Trade> large;
        for (uint64_t i{0}; i < 20; ++i) { auto t{valid}; t.sequence = i; t.price = 1; t.quantity = t.notional = MAX_MONEY; t.fee = 0; large.push_back(t); }
        const auto volumes{B3FlowMeshChart::AggregateCandles(large, 20)};
        QCOMPARE(volumes.size(), size_t{1});
        QVERIFY(volumes.front().base_volume > static_cast<uint64_t>(std::numeric_limits<CAmount>::max()));
        QCOMPARE(B3FlowMeshChart::FormatCandleVolume(volumes.front(), 0, false), QStringLiteral("13244000000000000000"));
        QCOMPARE(B3FlowMeshChart::FormatCandleVolume(volumes.front(), 6, true), QStringLiteral("13244000000"));
    }
    void candleIntervalsRetainSnapshotAndRenderGallery()
    {
        B3FlowMeshChart chart; chart.resize(900, 420);
        auto snapshot{Parse(Data())}; snapshot.history.clear(); snapshot.next_sequence = 100;
        // Deterministic isolated display fixture, never an RPC or wallet feed.
        const CAmount prices[]{1000, 1080, 960, 1060, 1100, 1100, 1140, 1020, 1040, 1030};
        for (uint64_t i{0}; i < 60; ++i) {
            Trade t; t.sequence = i; t.cleared = true; t.price = prices[i % 10] + static_cast<CAmount>(i / 10) * 30;
            t.quantity = 10'000 + static_cast<CAmount>(i % 7) * 2'000; t.notional = t.price * t.quantity;
            snapshot.history.push_back(t);
        }
        chart.setSnapshot(snapshot); QCOMPARE(chart.candleInterval(), uint64_t{5}); QCOMPARE(chart.candles().size(), size_t{12});
        chart.setCandleInterval(20); QCOMPARE(chart.candles().size(), size_t{3});
        const auto retained{chart.candles()};
        chart.setLoading(true); chart.setStale(true); chart.setSnapshot(snapshot);
        QVERIFY(chart.candles() == retained); QCOMPARE(chart.candleInterval(), uint64_t{20});
        chart.setCandleInterval(0); QCOMPARE(chart.candleInterval(), uint64_t{20});
        chart.setCandleInterval(5); chart.setLoading(false); chart.setStale(false);
        QImage image{chart.size(), QImage::Format_ARGB32}; image.fill(Qt::transparent);
        chart.render(&image);
        bool positive{false}, negative{false};
        for (int y{0}; y < image.height(); ++y) for (int x{0}; x < image.width(); ++x) {
            positive |= image.pixelColor(x, y) == B3Theme::kPositive;
            negative |= image.pixelColor(x, y) == B3Theme::kNegative;
        }
        QVERIFY(positive); QVERIFY(negative);
        // Optional local visual QA export. Test data stays in this test only.
        const QString gallery{qEnvironmentVariable("B3_FLOWMESH_CHART_GALLERY")};
        if (!gallery.isEmpty()) {
            QVERIFY(QDir{}.mkpath(gallery));
            QVERIFY(image.save(QDir{gallery}.filePath(QStringLiteral("candles-canonical.png"))));
        }
        chart.setInverted(true); chart.render(&image);
        QCOMPARE(chart.candles().front().high, CAmount{960}); QCOMPARE(chart.candles().front().low, CAmount{1100});
        if (!gallery.isEmpty()) QVERIFY(image.save(QDir{gallery}.filePath(QStringLiteral("candles-inverse.png"))));
        chart.setMode(B3FlowMeshChart::Mode::Liquidity); chart.render(&image);
        if (!gallery.isEmpty()) QVERIFY(image.save(QDir{gallery}.filePath(QStringLiteral("candles-depth.png"))));
        snapshot.remote = true; snapshot.execution_result_verified = false;
        chart.setSnapshot(snapshot); chart.setMode(B3FlowMeshChart::Mode::Prices); chart.render(&image);
        if (!gallery.isEmpty()) QVERIFY(image.save(QDir{gallery}.filePath(QStringLiteral("candles-reported.png"))));
        snapshot.certified = false; chart.setSnapshot(snapshot); QVERIFY(chart.candles().empty());
    }
    void renderCapturedEngineOffLiquidity()
    {
        const QString capture{qEnvironmentVariable("B3_FLOWMESH_CHART_CAPTURE")};
        if (capture.isEmpty()) QSKIP("Set B3_FLOWMESH_CHART_CAPTURE to an isolated engine-off public market-data JSON capture.");
        const QString gallery{qEnvironmentVariable("B3_FLOWMESH_CHART_GALLERY")};
        QVERIFY2(!gallery.isEmpty(), "Set B3_FLOWMESH_CHART_GALLERY to the output directory.");
        QFile file{capture};
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.errorString()));
        QVERIFY(file.size() > 0 && file.size() <= 16 * 1024 * 1024);
        const QByteArray bytes{file.readAll()};
        QCOMPARE(file.error(), QFileDevice::NoError);
        UniValue value;
        QVERIFY(value.read(bytes.toStdString()));
        // Use precisely the normal parser and immutable captured provenance.
        // This does not replay execution or promote remote history to verified.
        const auto snapshot{Parse(value)};
        QVERIFY(snapshot.remote); QVERIFY(snapshot.certified);
        QVERIFY(snapshot.certificate_verified); QVERIFY(snapshot.account_state_verified);
        QVERIFY(!snapshot.execution_result_verified);
        QVERIFY(snapshot.units.known); QVERIFY(snapshot.history_available); QVERIFY(snapshot.curves_complete);
        const auto clearings{std::count_if(snapshot.history.begin(), snapshot.history.end(), [](const auto& trade) { return trade.cleared && trade.quantity > 0; })};
        QVERIFY(clearings > 0);
        QVERIFY(std::any_of(snapshot.depth.begin(), snapshot.depth.end(), [](const auto& row) { return row.demand > 0; }));
        QVERIFY(std::any_of(snapshot.depth.begin(), snapshot.depth.end(), [](const auto& row) { return row.supply > 0; }));
        B3FlowMeshChart chart; chart.resize(1000, 460); chart.setSnapshot(snapshot);
        QCOMPARE(chart.pricePointCount(), static_cast<int>(clearings));
        QVERIFY(!chart.candles().empty());
        QVERIFY(QDir{}.mkpath(gallery));
        QImage image{chart.size(), QImage::Format_ARGB32}; image.fill(Qt::transparent);
        chart.render(&image);
        QVERIFY(image.save(QDir{gallery}.filePath(QStringLiteral("capture-candles-canonical.png"))));
        chart.setInverted(true); QVERIFY(!chart.candles().empty()); chart.render(&image);
        QVERIFY(image.save(QDir{gallery}.filePath(QStringLiteral("capture-candles-inverse.png"))));
        chart.setMode(B3FlowMeshChart::Mode::Liquidity); chart.render(&image);
        QVERIFY(image.save(QDir{gallery}.filePath(QStringLiteral("capture-depth.png"))));
        const auto book{ProjectLimitBook(snapshot, false)};
        QVERIFY(book.complete); QCOMPARE(book.asks.size(), size_t{1}); QCOMPARE(book.bids.size(), size_t{1});
        QCOMPARE(book.bids.front().canonical_price, CAmount{30'000'000}); QCOMPARE(book.bids.front().remaining, CAmount{3});
        QCOMPARE(book.asks.front().canonical_price, CAmount{80'000'000}); QCOMPARE(book.asks.front().remaining, CAmount{5});
        const QString heading{QStringLiteral("Captured generated-regtest orders · %1 / B3 · history remains endpoint-reported").arg(snapshot.units.ticker)};
        QVERIFY(SaveOrderBookGallery(snapshot, false, gallery, QStringLiteral("capture-orderbook-canonical.png"), heading));
        QVERIFY(SaveOrderBookGallery(snapshot, true, gallery, QStringLiteral("capture-orderbook-inverse.png"), heading));
        QVERIFY(snapshot == Parse(value));
    }
    void renderSyntheticMultilevelOrderBookGallery()
    {
        const QString gallery{qEnvironmentVariable("B3_FLOWMESH_CHART_GALLERY")};
        if (gallery.isEmpty()) QSKIP("Set B3_FLOWMESH_CHART_GALLERY to export explicitly synthetic multi-level book images.");
        QVERIFY(QDir::isAbsolutePath(gallery)); QVERIFY(QDir{}.mkpath(gallery));
        auto snapshot{SyntheticLimitBook(10)};
        snapshot.history.clear();
        for (uint64_t sequence{0}; sequence < 40; ++sequence) {
            Trade trade; trade.sequence = sequence;
            trade.hash = QString::fromStdString(H(static_cast<unsigned char>(120 + sequence)).GetHex());
            trade.cleared = true; trade.price = 992 + std::array<CAmount, 10>{0, 4, 2, 6, 9, 8, 6, 11, 7, 3}[sequence % 10];
            trade.quantity = 100'000 + static_cast<CAmount>(sequence % 4) * 25'000;
            trade.notional = trade.price * trade.quantity; trade.fee = *FeeExample(trade.notional);
            snapshot.history.push_back(trade);
        }
        snapshot.next_sequence = 40; snapshot.head = snapshot.history.back().hash;
        const auto original{snapshot}; const auto projection{ProjectLimitBook(snapshot, false)};
        QCOMPARE(projection.asks.size(), size_t{10}); QCOMPARE(projection.bids.size(), size_t{10});
        QVERIFY(SaveOrderBookGallery(snapshot, false, gallery, QStringLiteral("synthetic-orderbook-canonical.png"),
            QStringLiteral("Synthetic UI fixture · invented display values only · no wallet, node, or market feed")));
        QVERIFY(SaveOrderBookGallery(snapshot, true, gallery, QStringLiteral("synthetic-orderbook-inverse.png"),
            QStringLiteral("Synthetic UI fixture · reverse B3/token view · not captured market liquidity")));
        QVERIFY(snapshot == original);
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
        auto* interval{panel.findChild<QComboBox*>(QStringLiteral("flowMeshCandleInterval"))}; QVERIFY(interval);
        interval->setCurrentIndex(2); QCOMPARE(panel.m_chart->candleInterval(), uint64_t{20});
        liquidity->click(); QVERIFY(liquidity->isChecked()); QVERIFY(!prices->isChecked());
        QVERIFY(!interval->isEnabled());
        prices->click(); QVERIFY(prices->isChecked()); QVERIFY(!liquidity->isChecked());
        QVERIFY(interval->isEnabled()); QCOMPARE(panel.m_chart->candleInterval(), uint64_t{20});
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
    void inverseOrientationChangesOnlyPresentation()
    {
        B3FlowMeshTradingPanel panel;
        QCOMPARE(panel.m_orientation->currentIndex(), 1); // Production default.
        const auto snapshot{Parse(Data())};
        Observe(panel, snapshot);
        auto* orientation{panel.findChild<QComboBox*>(QStringLiteral("flowMeshOrientation"))};
        QVERIFY(orientation);
        QCOMPARE(orientation->currentIndex(), 0);
        const auto original{panel.m_market_data.front()};
        panel.m_price->setText(QStringLiteral("0.5"));
        panel.m_quantity->setText(QStringLiteral("1"));
        orientation->setCurrentIndex(1);
        QVERIFY(panel.m_price->text().isEmpty()); // Never reinterpret an old limit.
        QCOMPARE(panel.m_quantity->text(), QStringLiteral("1"));
        QVERIFY(panel.m_snapshot && *panel.m_snapshot == snapshot);
        QCOMPARE(panel.m_market_data.front().id, original.id);
        QCOMPARE(panel.m_market_data.front().base, original.base);
        QCOMPARE(panel.m_market_data.front().vault, original.vault);
        QCOMPARE(panel.m_market_data.front().account, original.account);
        QCOMPARE(panel.m_market_data.front().sequence, original.sequence);
        QCOMPARE(panel.m_chart->pricePointCount(), 1);
        QVERIFY(panel.m_pair_title->text().contains(QStringLiteral("B3")));
        QVERIFY(panel.m_price_label->text().contains(QStringLiteral("tUSD / B3")));
        QVERIFY(panel.m_quantity_label->text().contains(QStringLiteral("Spend")));
        QVERIFY(panel.m_quantity_label->text().contains(QStringLiteral("tUSD")));
        QVERIFY(panel.m_buy->text().contains(QStringLiteral("B3")));
        QVERIFY(panel.m_own_view->item(0, 0)->text().contains(QStringLiteral("Sell")));
        QCOMPARE(panel.m_own_view->item(0, 3)->text(), QStringLiteral("0.75 B3"));
        QCOMPARE(panel.m_depth_view->item(0, 0)->text(),
                 FormatDisplayPrice(snapshot.depth.back().price, snapshot.units.decimals, true));
        QCOMPARE(panel.m_depth_view->item(0, 1)->text(), FormatDepthQuantity(
            snapshot.depth.back().price, snapshot.depth.back().supply, snapshot.units.decimals, true));
        QCOMPARE(panel.m_depth_view->item(0, 2)->text(), FormatDepthQuantity(
            snapshot.depth.back().price, snapshot.depth.back().demand, snapshot.units.decimals, true));
        QCOMPARE(panel.m_depth_view->horizontalHeaderItem(1)->text(), QStringLiteral("Buy liquidity"));
        QCOMPARE(panel.m_depth_view->horizontalHeaderItem(2)->text(), QStringLiteral("Sell liquidity"));
        QCOMPARE(panel.m_depth_view->item(0, 1)->foreground().color(), B3Theme::kPositive);
        QCOMPARE(panel.m_depth_view->item(0, 2)->foreground().color(), B3Theme::kNegative);
        QVERIFY(panel.m_liquidity_note->text().contains(QStringLiteral("Aggregate curves")));
        QVERIFY(panel.m_liquidity_note->toolTip().contains(QStringLiteral("do not sum")));
        QCOMPARE(panel.m_history_view->item(0, 3)->text(), QStringLiteral("0"));
        QCOMPARE(panel.m_history_view->item(0, 4)->text(), QStringLiteral("0.25"));
        QVERIFY(panel.m_history_note->text().contains(QStringLiteral("gross B3 before fees")));
        panel.m_price->setText(QStringLiteral("2"));
        QVERIFY(panel.m_ticket_total->text().contains(QStringLiteral("0.5 B3")));
        QVERIFY(!panel.m_order->isEnabled()); // This display fixture has no wallet.
        QVERIFY(!panel.m_thread);
        orientation->setCurrentIndex(0);
        QVERIFY(panel.m_price->text().isEmpty());
        QVERIFY(panel.m_snapshot && *panel.m_snapshot == snapshot);
        QCOMPARE(panel.m_own_view->item(0, 0)->text(), QStringLiteral("Buy"));
        QCOMPARE(panel.m_depth_view->item(0, 0)->text(),
                 FormatPrice(snapshot.depth.front().price, snapshot.units.decimals));
        QVERIFY(!panel.m_depth_view->item(0, 0)->toolTip().contains(QStringLiteral("inverse")));
        QVERIFY(!panel.m_own_view->item(0, 1)->toolTip().contains(QStringLiteral("inverse")));
        panel.m_busy = true;
        panel.updateControls();
        QVERIFY(!orientation->isEnabled()); // Review context cannot be relabeled mid-flight.
    }
    void inverseReviewUsesSelectedReplacementBudgetAndNeverAutoApproves()
    {
        B3FlowMeshTradingPanel panel;
        AttachOfflineWallet(panel);
        auto snapshot{Parse(Data())};
        snapshot.base_available = 250'000;
        snapshot.base_reserved = 750'000;
        snapshot.b3_available = 250'000'000;
        snapshot.b3_reserved = 750'000'000;
        auto own_ask{snapshot.curves.back()}; own_ask.account = snapshot.account;
        snapshot.curves.push_back(own_ask); snapshot.own_curves.push_back(own_ask);
        snapshot.depth = Aggregate(snapshot.curves);
        Observe(panel, snapshot);
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        auto* orientation{panel.findChild<QComboBox*>(QStringLiteral("flowMeshOrientation"))};
        QVERIFY(orientation);
        QVERIFY(orientation->isEnabled());
        orientation->setCurrentIndex(1);
        panel.m_price->setText(QStringLiteral("2"));
        panel.m_quantity->setText(QStringLiteral("1"));
        QVERIFY(panel.m_ticket_available->text().contains(QStringLiteral("0.25 tUSD")));
        QVERIFY(panel.m_ticket_available->toolTip().contains(QStringLiteral("0.75 tUSD")));
        QVERIFY(panel.m_ticket_available->toolTip().contains(QStringLiteral("1 tUSD")));
        QVERIFY(panel.m_ticket_total->text().contains(QStringLiteral("0.5 B3")));
        QVERIFY(panel.m_ticket_fee->toolTip().contains(QStringLiteral("B3")));
        QVERIFY(panel.m_ticket_fee->text().contains(QStringLiteral("Actual fee asset: B3")));
        QVERIFY(panel.m_ticket_fee->toolTip().contains(QStringLiteral("unknown before execution")));
        QString review;
        bool cancel_default{false}, orientation_locked{false};
        QTimer::singleShot(0, &panel, [&] {
            if (panel.m_confirmation) {
                review = panel.m_confirmation->text();
                cancel_default = panel.m_confirmation->defaultButton() == panel.m_confirmation->button(QMessageBox::Cancel);
                orientation_locked = !orientation->isEnabled();
                panel.m_confirmation->done(QMessageBox::Cancel);
            }
        });
        panel.begin(B3FlowMeshTrading::Operation::Order);
        QVERIFY(review.contains(QStringLiteral("Buy B3: Spend up to 1 tUSD")));
        QVERIFY(review.contains(QStringLiteral("Canonical side: ask")));
        QVERIFY(review.contains(QStringLiteral("deducted from the B3 you receive")));
        QVERIFY(cancel_default);
        QVERIFY(orientation_locked);
        QCOMPARE(unlock.count(), 0);
        QVERIFY(m_wallet->IsLocked());
        QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_unlock);
        panel.m_sell->click();
        QVERIFY(panel.m_quantity_label->text().contains(QStringLiteral("Receive")));
        QVERIFY(panel.m_ticket_available->text().contains(QStringLiteral("0.25 B3")));
        QVERIFY(panel.m_ticket_available->toolTip().contains(QStringLiteral("0.75 B3")));
        QVERIFY(panel.m_ticket_available->toolTip().contains(QStringLiteral("1 B3")));
        review.clear();
        QTimer::singleShot(0, &panel, [&] {
            if (panel.m_confirmation) {
                review = panel.m_confirmation->text();
                panel.m_confirmation->done(QMessageBox::Cancel);
            }
        });
        panel.begin(B3FlowMeshTrading::Operation::Order);
        QVERIFY(review.contains(QStringLiteral("Sell B3: Receive up to 1 tUSD")));
        QVERIFY(review.contains(QStringLiteral("Canonical side: bid")));
        QVERIFY(review.contains(QStringLiteral("not deducted from your token receipt")));
        QCOMPARE(unlock.count(), 0);
        QVERIFY(m_wallet->IsLocked());
        QVERIFY(!panel.m_confirmation); QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result);
        QCOMPARE(panel.m_market_data.front().base, snapshot.base);
        QCOMPARE(panel.m_market_data.front().id, snapshot.market);
        // Off-grid inverse input adjusts only conservatively, and the exact
        // executable price must be disclosed before any unlock/signing.
        panel.m_price->setText(QStringLiteral("1.1"));
        review.clear();
        QTimer::singleShot(0, &panel, [&] {
            if (panel.m_confirmation) {
                review = panel.m_confirmation->text();
                panel.m_confirmation->done(QMessageBox::Cancel);
            }
        });
        panel.begin(B3FlowMeshTrading::Operation::Order);
        QVERIFY(review.contains(QStringLiteral("adjusted only in your favor")));
        QVERIFY(review.contains(QStringLiteral("1000/909 tUSD / B3")));
        QVERIFY(review.contains(QStringLiteral("Maximum B3 reservation/spend")));
        QVERIFY(!panel.m_confirmation); QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result);
        QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
    }
    void reverseDefaultQuantityFeeAndAssetIdentityAreFaithful()
    {
        B3FlowMeshTradingPanel panel;
        QCOMPARE(panel.m_orientation->currentIndex(), 1);
        AttachOfflineWallet(panel);
        const auto snapshot{Parse(Data())}; Observe(panel, snapshot, true);
        QCOMPARE(panel.m_pair_title->text(), QStringLiteral("B3 / tUSD"));
        QCOMPARE(panel.m_buy->text(), QStringLiteral("Buy B3"));
        QCOMPARE(panel.m_sell->text(), QStringLiteral("Sell B3"));
        panel.m_price->setText(QStringLiteral("1.1")); panel.m_quantity->setText(QStringLiteral("0.000001"));
        QVERIFY(panel.m_quantity_label->text().contains(QStringLiteral("Spend up to")));
        QVERIFY(panel.m_ticket_total->text().contains(QStringLiteral("100/91 tUSD / B3")));
        QVERIFY(panel.m_ticket_total->text().contains(QStringLiteral("if fully filled")));
        QVERIFY(panel.m_ticket_total->toolTip().contains(QStringLiteral("No fill is guaranteed")));
        QCOMPARE(panel.m_history_view->item(0, 5)->text(), QStringLiteral("0.000025"));
        QVERIFY(panel.m_history_view->horizontalHeaderItem(5)->text().contains(QStringLiteral("B3")));
        QVERIFY(panel.m_history_view->horizontalHeaderItem(5)->toolTip().contains(QStringLiteral("not your individual")));
        panel.m_sell->click();
        QVERIFY(panel.m_quantity_label->text().contains(QStringLiteral("Receive up to")));
        QVERIFY(panel.m_ticket_total->text().contains(QStringLiteral("Maximum B3 reservation/spend: 0.000000909 B3")));
        QVERIFY(panel.m_ticket_total->text().contains(QStringLiteral("1000/909 tUSD / B3")));
        QString cancellation_review;
        QTimer::singleShot(0, &panel, [&] {
            if (panel.m_confirmation) {
                cancellation_review = panel.m_confirmation->text();
                panel.m_confirmation->done(QMessageBox::Cancel);
            }
        });
        panel.begin(B3FlowMeshTrading::Operation::Cancel);
        QVERIFY(cancellation_review.contains(QStringLiteral("standing Sell B3 (bid)")));
        QVERIFY(cancellation_review.contains(QStringLiteral("not certified until processed")));
        QVERIFY(!panel.m_thread); QVERIFY(m_wallet->IsLocked());
        // Same ticker does not authorize another AssetId's precision.
        panel.m_snapshot->units.asset = QString::fromStdString(H(98).GetHex());
        panel.updateTicket(); panel.updateControls(); QVERIFY(!panel.m_order->isEnabled());
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        panel.begin(B3FlowMeshTrading::Operation::Order);
        QVERIFY(!panel.m_confirmation); QVERIFY(!panel.m_thread); QCOMPARE(unlock.count(), 0);
        QVERIFY(panel.m_log->toPlainText().contains(QStringLiteral("different configured AssetId")));
        QCOMPARE(panel.m_market_data.front().id, snapshot.market);
    }
    void restoredReverseCardsRetainCanonicalInstructionAndMeaning()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        Observe(panel, Parse(Data()), true);
        B3FlowMeshTrading::SavedActions saved; saved.account = panel.m_snapshot->account;
        B3FlowMeshTrading::SavedAction action;
        action.market = panel.m_snapshot->market; action.domain = panel.m_snapshot->domain; action.config = panel.m_snapshot->config;
        action.account = saved.account; action.sequence = 7; action.type = 1; action.initial_submission_ms = 1000;
        action.signed_bytes_sha256 = QString::fromStdString(H(96).GetHex()); action.signed_bytes_size = 279; action.may_have_been_sent = true;
        action.canonical_side = QStringLiteral("ask"); action.canonical_points = {{909, 0}, {910, 1'000'000}};
        action.receipt.action_id = QString::fromStdString(H(95).GetHex()); action.receipt.state = QStringLiteral("unknown");
        saved.actions.push_back(action); panel.restoreSavedActions(saved);
        QVERIFY(panel.m_receipt_card->text().contains(QStringLiteral("Buy B3 = sell tUSD; canonical ask")));
        QVERIFY(panel.m_receipt_card->text().contains(QStringLiteral("Spend up to 1 tUSD")));
        QVERIFY(panel.m_receipt_card->text().contains(QStringLiteral("100/91 tUSD / B3")));
        QVERIFY(panel.m_receipt_card->text().contains(QStringLiteral("no fee amount is inferred from inclusion")));
        const auto prior{panel.m_receipt_card->text()};
        panel.m_orientation->setCurrentIndex(0); panel.updateReceiptCard();
        QCOMPARE(panel.m_receipt_card->text(), prior); // Legacy canonical history has not changed meaning.
        QCOMPARE(panel.m_saved_actions.actions[0].canonical_points, action.canonical_points);
        QCOMPARE(panel.m_saved_actions.actions[0].signed_bytes_sha256, action.signed_bytes_sha256);
        QCOMPARE(panel.m_pending_sequence, action.sequence); QVERIFY(panel.m_uncertain);
        auto wrong_metadata{*panel.m_snapshot}; wrong_metadata.units.asset = QString::fromStdString(H(98).GetHex());
        panel.m_snapshot = wrong_metadata; panel.updateReceiptCard();
        QVERIFY(panel.m_receipt_card->text().contains(QStringLiteral("Buy B3 = sell configured token; canonical ask")));
        QVERIFY(!panel.m_receipt_card->text().contains(QStringLiteral("Spend up to 1 tUSD")));
        saved.actions[0].receipt.no_resubmit = true; panel.restoreSavedActions(saved);
        QVERIFY(panel.m_receipt->no_resubmit); QVERIFY(!panel.m_retry_receipt->isEnabled());
        QVERIFY(!panel.m_uncertain); QVERIFY(!panel.m_thread);
    }
    void inverseChartRetainsCertifiedDataAndHandlesZeroBoundary()
    {
        B3FlowMeshChart chart;
        chart.resize(640, 340);
        auto snapshot{Parse(Data())};
        // A canonical ask curve may have a zero-price/zero-volume boundary;
        // its reciprocal is undefined, never a fabricated finite price.
        snapshot.depth.insert(snapshot.depth.begin(), Depth{0, 0, 0});
        chart.setSnapshot(snapshot);
        chart.setInverted(true);
        QCOMPARE(chart.pricePointCount(), 1);
        QImage image{chart.size(), QImage::Format_ARGB32};
        image.fill(Qt::transparent);
        chart.render(&image);
        QVERIFY(!image.isNull());
        chart.setMode(B3FlowMeshChart::Mode::Liquidity);
        chart.render(&image);
        chart.setInverted(false);
        chart.render(&image);
        QCOMPARE(chart.pricePointCount(), 1);
        snapshot.units.known = false;
        chart.setSnapshot(snapshot);
        chart.setInverted(true);
        QVERIFY(chart.emptyMessage().contains(QStringLiteral("precision unavailable")));
        chart.render(&image);
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
        panel.m_history_view->selectRow(20);
        // selectRow may make the leading column current. Establish the exact
        // current cell AFTER row selection without changing that selection,
        // and verify the precondition before testing refresh preservation.
        panel.m_history_view->selectionModel()->setCurrentIndex(
            panel.m_history_view->model()->index(20, 1), QItemSelectionModel::NoUpdate);
        QCOMPARE(panel.m_history_view->currentIndex(), panel.m_history_view->model()->index(20, 1));
        auto* scroll{panel.m_history_view->verticalScrollBar()};
        QVERIFY(scroll->maximum() > 0);
        scroll->setValue(std::max(1, scroll->maximum() / 2));
        const int position{scroll->value()};
        QVERIFY(position > 0);
        const QPersistentModelIndex selected{panel.m_history_view->currentIndex()};
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
    void identicalRefreshHasNoModelChurn_data()
    {
        QTest::addColumn<bool>("inverse");
        QTest::addColumn<bool>("remote");
        QTest::newRow("canonical-local") << false << false;
        QTest::newRow("inverse-local") << true << false;
        QTest::newRow("canonical-remote") << false << true;
        QTest::newRow("inverse-remote") << true << true;
    }
    void identicalRefreshHasNoModelChurn()
    {
        QFETCH(bool, inverse);
        QFETCH(bool, remote);
        auto snapshot{Parse(remote ? RemoteData() : Data())};
        const auto trade{snapshot.history.front()};
        snapshot.history.clear();
        for (uint64_t i{0}; i < 40; ++i) {
            auto entry{trade}; entry.sequence = i + 2;
            entry.hash = QString::fromStdString(H(static_cast<unsigned char>(i + 20)).GetHex());
            snapshot.history.push_back(entry);
        }
        snapshot.next_sequence = 42; snapshot.head = snapshot.history.back().hash;
        B3FlowMeshTradingPanel panel;
        panel.resize(1200, 850);
        AttachOfflineWallet(panel); // Mock wallet only; no RPC, sockets or timer.
        Observe(panel, snapshot, inverse);
        panel.m_history_view->setFixedHeight(160);
        panel.m_history_view->setSelectionMode(QAbstractItemView::SingleSelection);
        panel.m_activity->setCurrentWidget(panel.m_history_view->parentWidget());
        panel.m_price->setText(QStringLiteral("1.009"));
        panel.m_quantity->setText(QStringLiteral("0.25"));
        panel.m_saved_selector->addItem(QStringLiteral("retained first"), QStringLiteral("first"));
        panel.m_saved_selector->addItem(QStringLiteral("retained selected"), QStringLiteral("selected"));
        { QSignalBlocker blocked{panel.m_saved_selector}; panel.m_saved_selector->setCurrentIndex(1); }
        panel.show();
        for (int warmup{0}; warmup < 3; ++warmup) QCoreApplication::processEvents();
        panel.m_history_view->selectRow(20);
        auto* scroll{panel.m_history_view->verticalScrollBar()};
        QVERIFY(scroll->maximum() > 0);
        scroll->setValue(std::max(1, scroll->maximum() / 2));
        panel.m_price->setFocus(); panel.m_price->setSelection(2, 2);
        QCoreApplication::processEvents();
        QVERIFY(panel.m_price->hasFocus());
        const int position{scroll->value()}, cursor{panel.m_price->cursorPosition()};
        const auto selection{panel.m_history_view->selectionModel()->selectedIndexes()};
        const QPersistentModelIndex selected{panel.m_history_view->currentIndex()};
        const auto* depth_cell{panel.m_depth_view->item(0, 0)};
        const auto* history_cell{panel.m_history_view->item(20, 1)};
        const auto* own_cell{panel.m_own_view->item(0, 0)};
        QVERIFY(depth_cell); QVERIFY(history_cell); QVERIFY(own_cell);
        const auto final_copy = [&] {
            return QStringList{panel.m_history_note->text(), panel.m_history_note->toolTip(),
                panel.m_last_price->text(), panel.m_identity_detail->text(), panel.m_progress->text(),
                panel.m_status->text(), panel.m_status->toolTip(), panel.m_ticket_fee->toolTip(),
                panel.m_ticket_available->toolTip(), panel.m_ticket_total->toolTip(),
                panel.m_depth_view->item(0, 0)->toolTip(), panel.m_own_view->item(0, 1)->toolTip()};
        };
        const QStringList before_copy{final_copy()};
        QCOMPARE(panel.m_history_note->text(),
                 (remote ? QStringLiteral("Endpoint-reported trades · execution results not independently verified") : QStringLiteral("Confirmed trades")) +
                 (inverse ? QStringLiteral(" · gross B3 before fees") : QString{}));
        QCOMPARE(panel.m_ticket_fee->toolTip(), QStringLiteral("Actual protocol fee asset: B3. The whole-auction fee is 0.01% of matched B3 and is allocated among B3 receivers. Your exact allocated amount is unknown before execution; this is not a stable-asset charge. Submitting an order has no network fee."));
        QCOMPARE(panel.m_progress->text(), StatusText(snapshot, 0, -1) + QStringLiteral("\nCertified microblock #41 · epoch 0 · configured threshold 3 of 4 FN seats · 0 queued actions\nAn available runtime or configured threshold is not proof that those validators are currently online."));
        QSignalSpy depth_changed{panel.m_depth_view->model(), &QAbstractItemModel::dataChanged};
        QSignalSpy history_changed{panel.m_history_view->model(), &QAbstractItemModel::dataChanged};
        QSignalSpy own_changed{panel.m_own_view->model(), &QAbstractItemModel::dataChanged};
        QSignalSpy market_changed{panel.m_market->model(), &QAbstractItemModel::dataChanged};
        QSignalSpy orientation_changed{panel.m_orientation->model(), &QAbstractItemModel::dataChanged};
        QSignalSpy reset{panel.m_history_view->model(), &QAbstractItemModel::modelReset};
        struct Events final : QObject {
            int layouts{0}, paints{0}, tooltips{0};
            bool eventFilter(QObject*, QEvent* event) override {
                if (event->type() == QEvent::LayoutRequest) ++layouts;
                if (event->type() == QEvent::Paint) ++paints;
                if (event->type() == QEvent::ToolTipChange) ++tooltips;
                return false;
            }
        } events;
        panel.installEventFilter(&events);
        for (auto* child : panel.findChildren<QWidget*>()) child->installEventFilter(&events);
        for (int refresh{0}; refresh < 5; ++refresh) {
            panel.m_snapshot = snapshot; panel.m_response_age.restart();
            panel.updateDataViews(); panel.updateMarketText();
            QCoreApplication::processEvents();
        }
        qInfo("identical refreshes=5 depth_data=%lld history_data=%lld own_data=%lld market_data=%lld orientation_data=%lld layouts=%d paints=%d tooltips=%d",
              static_cast<long long>(depth_changed.count()), static_cast<long long>(history_changed.count()),
              static_cast<long long>(own_changed.count()), static_cast<long long>(market_changed.count()),
              static_cast<long long>(orientation_changed.count()), events.layouts, events.paints, events.tooltips);
        QCOMPARE(panel.m_price->text(), QStringLiteral("1.009"));
        QCOMPARE(panel.m_quantity->text(), QStringLiteral("0.25"));
        QCOMPARE(panel.m_price->cursorPosition(), cursor);
        QCOMPARE(panel.m_price->selectedText(), QStringLiteral("00"));
        QVERIFY(panel.m_price->hasFocus());
        QCOMPARE(panel.m_saved_selector->currentData().toString(), QStringLiteral("selected"));
        QCOMPARE(final_copy(), before_copy);
        QCOMPARE(panel.m_depth_view->item(0, 0), depth_cell);
        QCOMPARE(panel.m_history_view->item(20, 1), history_cell);
        QCOMPARE(panel.m_own_view->item(0, 0), own_cell);
        QCOMPARE(panel.m_history_view->currentIndex(), QModelIndex{selected});
        QCOMPARE(panel.m_history_view->selectionModel()->selectedIndexes(), selection);
        QCOMPARE(scroll->value(), position); QCOMPARE(reset.count(), 0);
        // A same-value refresh must not temporarily clear and restore item roles.
        // Event counts are diagnostics, not a claim of visible Cocoa flicker.
        QCOMPARE(depth_changed.count(), 0); QCOMPARE(history_changed.count(), 0);
        QCOMPARE(own_changed.count(), 0); QCOMPARE(market_changed.count(), 0);
        QCOMPARE(orientation_changed.count(), 0);
        const int changed_row{inverse ? panel.m_depth_view->rowCount() - 1 : 0};
        const auto before{panel.m_depth_view->item(changed_row, 1)->text()};
        auto& changed{*panel.m_snapshot};
        if (inverse) changed.depth.front().supply += 1'000'000;
        else changed.depth.front().demand += 1'000'000;
        panel.updateDataViews(); QCoreApplication::processEvents();
        QVERIFY(depth_changed.count() > 0); // Real updates are not suppressed.
        QVERIFY(panel.m_depth_view->item(changed_row, 1)->text() != before);
    }
    void passiveSavedControlsStayEnabled()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        SavedReadFixture(panel);
        const QString card{panel.m_receipt_card->text()};
        ReadInFlight(panel);
        for (int i{0}; i < 5; ++i) {
            panel.updateControls();
            QVERIFY(panel.m_saved_selector->isEnabled());
            QVERIFY(panel.m_check_receipt->isEnabled());
            QCOMPARE(panel.m_receipt_card->text(), card);
        }
    }

    void explicitStatusClickIsNotLostDuringRead()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        const auto saved{SavedReadFixture(panel)};
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        ReadInFlight(panel); const auto original_thread{panel.m_thread};
        for (int i{0}; i < 5; ++i) panel.m_check_receipt->click();
        QCOMPARE(panel.m_thread, original_thread);
        QVERIFY(!panel.m_active_result);
        const auto failed{std::make_shared<B3FlowMeshTradingPanel::Result>()};
        failed->error = QStringLiteral("Synthetic passive endpoint failure");
        panel.finishJob(failed);
        // A failed passive read still drains the one explicit read. The
        // isolated RPC may fail, but it must not silently lose the click.
        QVERIFY(panel.m_active_result);
        QVERIFY(panel.m_active_result->receipt_only);
        QCOMPARE(panel.m_active_result->receipt_action_id, saved.actions[0].receipt.action_id);
        QVERIFY(!panel.m_active_result->action); QVERIFY(!panel.m_active_result->exact_retry);
        panel.cancelAndWait();
        QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
        QCOMPARE(panel.m_saved_actions.actions[0].signed_bytes_sha256, saved.actions[0].signed_bytes_sha256);
        QCOMPARE(panel.m_saved_actions.actions[0].sequence, saved.actions[0].sequence);
        QVERIFY(panel.m_saved_actions.actions[0].receipt.no_resubmit);
    }

    void deferredStatusReadCoalescesWhileVisibleOrHidden_data()
    {
        QTest::addColumn<bool>("hidden");
        QTest::newRow("visible") << false;
        QTest::newRow("hidden") << true;
    }
    void deferredStatusReadCoalescesWhileVisibleOrHidden()
    {
        QFETCH(bool, hidden);
        StatusReadProbe probe;
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        struct Cleanup { StatusReadProbe& probe; B3FlowMeshTradingPanel& panel; ~Cleanup() { probe.release.release(8); panel.cancelAndWait(); } } cleanup{probe, panel};
        const auto saved{SavedReadFixture(panel)};
        const auto card{panel.m_receipt_card->text()};
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        if (!hidden) panel.show();
        ReadInFlight(panel); const auto thread{panel.m_thread};
        for (int i{0}; i < 20; ++i) panel.m_check_receipt->click();
        QCOMPARE(probe.count.load(), 0); QCOMPARE(panel.m_thread, thread);
        QVERIFY(panel.m_deferred_status); QVERIFY(panel.m_status_read_state->text().contains(QStringLiteral("queued")));
        QCOMPARE(panel.m_receipt_card->text(), card);
        auto failed{std::make_shared<B3FlowMeshTradingPanel::Result>()}; failed->error = QStringLiteral("Passive endpoint unavailable");
        panel.finishJob(failed);
        QVERIFY(probe.entered.tryAcquire(1, 1000)); QVERIFY(panel.m_active_result);
        const auto active{panel.m_active_result};
        QVERIFY(active->receipt_only); QVERIFY(!active->write_attempted); QVERIFY(!active->action); QVERIFY(!active->exact_retry);
        QVERIFY(!panel.m_deferred_status); QVERIFY(panel.m_status_read_state->text().contains(QStringLiteral("Checking")));
        const auto active_thread{panel.m_thread};
        for (int i{0}; i < 20; ++i) panel.m_check_receipt->click();
        QCOMPARE(panel.m_thread, active_thread); QVERIFY(!panel.m_deferred_status);
        QCOMPARE(probe.count.load(), 1);
        {
            std::lock_guard lock{probe.mutex}; QCOMPARE(probe.requests.size(), size_t{1});
            QCOMPARE(probe.requests[0].first, B3AssetTransfer::WalletUri(m_model->getWalletName()));
            QCOMPARE(probe.requests[0].second[0].get_str(), saved.actions[0].market.toStdString());
            QCOMPARE(probe.requests[0].second[1].get_str(), saved.actions[0].receipt.action_id.toStdString());
        }
        probe.release.release(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QCOMPARE(probe.count.load(), 1); QVERIFY(!active->write_attempted);
        QCOMPARE(panel.m_receipt->account, saved.account); QVERIFY(panel.m_receipt->no_resubmit);
        QCOMPARE(panel.m_saved_actions.actions[0].sequence, saved.actions[0].sequence);
        QCOMPARE(panel.m_saved_actions.actions[0].signed_bytes_sha256, saved.actions[0].signed_bytes_sha256);
        QCOMPARE(panel.m_saved_actions.actions[0].signed_bytes_size, saved.actions[0].signed_bytes_size);
        QCOMPARE(panel.m_saved_actions.actions[0].initial_submission_ms, saved.actions[0].initial_submission_ms);
        QVERIFY(panel.m_saved_actions.actions[0].may_have_been_sent);
        QVERIFY(panel.m_check_receipt->isEnabled()); QVERIFY(panel.m_saved_selector->isEnabled());
        QVERIFY(!panel.m_deferred_review); QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
    }

    void deferredStatusReadInvalidatesChangedScope_data()
    {
        QTest::addColumn<QString>("change");
        for (const auto* change : {"selection-away-and-back", "market", "wallet", "generation", "account", "domain", "config"})
            QTest::newRow(change) << QString::fromLatin1(change);
    }
    void deferredStatusReadInvalidatesChangedScope()
    {
        QFETCH(QString, change);
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        auto saved{SavedReadFixture(panel)};
        auto second{saved.actions[0]}; second.sequence = 8; second.receipt.action_id = QString::fromStdString(H(96).GetHex());
        saved.actions.push_back(second); panel.restoreSavedActions(saved);
        ReadInFlight(panel); panel.m_check_receipt->click(); QVERIFY(panel.m_deferred_status);
        std::optional<OfflineWallet> replacement;
        if (change == QStringLiteral("selection-away-and-back")) {
            panel.m_saved_selector->setCurrentIndex(1); panel.m_saved_selector->setCurrentIndex(0);
        } else if (change == QStringLiteral("market")) {
            auto market{panel.m_market_data.front()}; market.id = QString::fromStdString(H(101).GetHex());
            panel.m_market_data.push_back(market); panel.m_market->addItem(QStringLiteral("other market"), market.id); panel.m_market->setCurrentIndex(1);
        } else if (change == QStringLiteral("wallet")) {
            panel.cancelAndWait(); replacement = MakeOfflineWallet(m_wallet->GetName());
            AttachOfflineWallet(panel, *replacement->model, replacement->wallet); SavedReadFixture(panel);
        } else if (change == QStringLiteral("generation")) ++panel.m_generation;
        else if (change == QStringLiteral("account")) panel.m_saved_actions.account = QString::fromStdString(H(101).GetHex());
        else if (change == QStringLiteral("domain")) panel.m_saved_actions.actions[0].domain = QString::fromStdString(H(101).GetHex());
        else if (change == QStringLiteral("config")) panel.m_market_data[0].config = QString::fromStdString(H(101).GetHex());
        auto failed{std::make_shared<B3FlowMeshTradingPanel::Result>()}; failed->error = QStringLiteral("Passive read complete");
        panel.finishJob(failed);
        QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_deferred_status);
        panel.cancelAndWait();
    }

    void statusReadLateResultStaysWithOriginalRequest_data()
    {
        QTest::addColumn<bool>("failure"); QTest::addColumn<bool>("wrong_account");
        QTest::newRow("success-with-optional-account-omitted") << false << false;
        QTest::newRow("endpoint-failure") << true << false;
        QTest::newRow("wrong-account-rejected") << false << true;
    }
    void statusReadLateResultStaysWithOriginalRequest()
    {
        QFETCH(bool, failure); QFETCH(bool, wrong_account);
        StatusReadProbe probe; probe.fail = failure; probe.wrong_account = wrong_account;
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        struct Cleanup { StatusReadProbe& probe; B3FlowMeshTradingPanel& panel; ~Cleanup() { probe.release.release(8); panel.cancelAndWait(); } } cleanup{probe, panel};
        auto saved{SavedReadFixture(panel)};
        // Same semantic ActionId in another market must not alias the first.
        auto second{saved.actions[0]}; second.market = QString::fromStdString(H(101).GetHex()); second.receipt.market = second.market;
        saved.actions.push_back(second); panel.restoreSavedActions(saved);
        panel.m_check_receipt->click(); QVERIFY(probe.entered.tryAcquire(1, 1000));
        auto first_result{panel.m_active_result}; QVERIFY(first_result);
        panel.m_saved_selector->setCurrentIndex(1); const auto other_card{panel.m_receipt_card->text()};
        panel.m_check_receipt->click(); QVERIFY(panel.m_deferred_status);
        probe.release.release();
        QTRY_COMPARE_WITH_TIMEOUT(probe.count.load(), 2, 2000);
        QVERIFY(probe.entered.tryAcquire(1, 1000));
        QCOMPARE(panel.m_receipt_card->text(), other_card); // A's late error/success cannot relabel B.
        QCOMPARE(panel.m_receipt->market, second.market); QVERIFY(panel.m_receipt_error.isEmpty());
        const auto second_result{panel.m_active_result}; QVERIFY(second_result); QVERIFY(second_result != first_result);
        QCOMPARE(second_result->receipt_market, second.market);
        if (!failure && !wrong_account) {
            QVERIFY(first_result->receipt); QCOMPARE(first_result->receipt->account, saved.account);
        } else QVERIFY(!first_result->receipt);
        probe.release.release(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QCOMPARE(probe.count.load(), 2); QVERIFY(!first_result->write_attempted); QVERIFY(!second_result->write_attempted);
        QCOMPARE(panel.m_receipt->market, second.market); QVERIFY(panel.m_receipt->no_resubmit);
        QCOMPARE(panel.m_saved_actions.actions[0].signed_bytes_sha256, saved.actions[0].signed_bytes_sha256);
        if (failure || wrong_account) QVERIFY(!panel.m_receipt_error.isEmpty());
    }

    void shutdownDropsPendingStatusWithoutOwningWalletOrChangingInstruction()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        const auto saved{SavedReadFixture(panel)};
        const std::weak_ptr<interfaces::Wallet> backend{panel.m_backend};
        ReadInFlight(panel); panel.m_check_receipt->click(); QVERIFY(panel.m_deferred_status);
        const auto scope{*panel.m_deferred_status};
        panel.hide(); panel.cancelAndWait();
        QVERIFY(backend.expired()); QVERIFY(!panel.m_deferred_status); QVERIFY(!panel.m_thread);
        QVERIFY(!panel.m_wallet); QVERIFY(!panel.m_backend); QVERIFY(!panel.m_active_result);
        QVERIFY(!panel.statusReadValid(scope, true));
        QCOMPARE(panel.m_saved_actions.actions[0].receipt.action_id, saved.actions[0].receipt.action_id);
        QCOMPARE(panel.m_saved_actions.actions[0].signed_bytes_sha256, saved.actions[0].signed_bytes_sha256);
        QCOMPARE(panel.m_saved_actions.actions[0].sequence, saved.actions[0].sequence);
        QVERIFY(panel.m_saved_actions.actions[0].receipt.no_resubmit);
        QVERIFY(!panel.m_check_receipt->isEnabled()); QVERIFY(m_wallet->IsLocked());
    }

    void pendingReadCannotDrainThroughPanelDestroyedInExistingReview()
    {
        auto* panel{new B3FlowMeshTradingPanel}; AttachOfflineWallet(*panel);
        SavedReadFixture(*panel); ReadInFlight(*panel); panel->m_check_receipt->click();
        QVERIFY(panel->m_deferred_status);
        const QPointer<B3FlowMeshTradingPanel> alive{panel};
        const std::weak_ptr<interfaces::Wallet> backend{panel->m_backend};
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        // Synthetic completion enters the existing fee-review dialog. No RPC,
        // transaction construction or broadcast is performed by this fixture.
        auto result{std::make_shared<B3FlowMeshTradingPanel::Result>()};
        B3FlowMeshTrading::Action action; action.operation = B3FlowMeshTrading::Operation::Checkpoint;
        action.market = panel->m_market_data.front(); result->action = action;
        result->prepared.emplace(); result->prepared->txid = QString::fromStdString(H(112).GetHex());
        bool dialog_seen{false};
        QTimer::singleShot(0, [&] {
            dialog_seen = alive && alive->m_confirmation;
            delete alive.data();
        });
        panel->finishJob(result);
        QVERIFY(dialog_seen); QVERIFY(!alive); QVERIFY(backend.expired());
        QVERIFY(!result->write_attempted); QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
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
        auto* book{panel.findChild<B3FlowMeshOrderBook*>(QStringLiteral("flowMeshOrderBook"))};
        auto* mode{panel.findChild<QComboBox*>(QStringLiteral("flowMeshLiquidityMode"))};
        QVERIFY(book); QVERIFY(mode); QCOMPARE(mode->currentIndex(), 0);
        QVERIFY(panel.m_chart->isVisible()); QVERIFY(book->isVisible()); QVERIFY(!panel.m_depth_view->isVisible());
        mode->setCurrentIndex(1); QCoreApplication::processEvents();
        QVERIFY(panel.m_depth_view->isVisible()); QVERIFY(!book->isVisible());
        mode->setCurrentIndex(0); QCoreApplication::processEvents();
        QVERIFY(book->isVisible()); QVERIFY(!panel.m_depth_view->isVisible());
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
