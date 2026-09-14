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
#include <QDeadlineTimer>
#include <QDir>
#include <QEvent>
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
UniValue RemoteData(bool trade = true)
{
    auto v{Data(trade)}; UniValue proof{UniValue::VOBJ}; proof.pushKV("source", "remote_endpoint"); proof.pushKV("endpoint", "https://operator.invalid:18443");
    proof.pushKV("certificate_verified", true); proof.pushKV("account_state_verified", true); proof.pushKV("execution_result_verified", false);
    proof.pushKV("b3_checkpoint_confirmed", false); proof.pushKV("event_gap", false); v.pushKV("verification", proof); return v;
}
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
