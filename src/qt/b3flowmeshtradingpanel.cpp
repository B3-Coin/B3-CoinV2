// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/b3flowmeshtradingpanel.h>
#include <qt/b3flowmeshchart.h>
#include <qt/b3theme.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <rpc/protocol.h>
#include <util/moneystr.h>
#include <QApplication>
#include <QAbstractButton>
#include <QCheckBox>
#include <QButtonGroup>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QHeaderView>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <exception>
#include <limits>
#include <stdexcept>

using B3FlowMeshTrading::Action;
using B3FlowMeshTrading::Market;
using B3FlowMeshTrading::Operation;
namespace {
QLabel* Label(const QString& text, QWidget* parent) {
    auto* label{new QLabel{text, parent}}; label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true); label->setTextInteractionFlags(Qt::TextSelectableByMouse); return label;
}
bool PreparedOperation(Operation op) { return op == Operation::Deposit || op == Operation::Checkpoint || op == Operation::Vault; }
bool SignedAction(Operation op) { return op == Operation::Order || op == Operation::Cancel || op == Operation::Withdraw; }
bool NoSpendingRequired(const std::string& method) {
    return method == "listflowmeshmarkets" || method == "getflowmeshbalance" || method == "getblockchaininfo" ||
        method == "getflowmeshclientinfo" || method == "flowmeshclientconnect" ||
        method == "getflowmeshmarketdata" || method == "getflowmeshactionstatus" || method == "listflowmeshactions" || method == "listflowmeshvaultoperations" || method == "testmempoolaccept";
}
QString RpcError(const UniValue& error) {
    const auto& message{error.find_value("message")};
    return error.isObject() && message.isStr() ? QString::fromStdString(message.get_str()).left(500) : QStringLiteral("FlowMesh RPC failed.");
}
// Keep the model and its existing items alive during background updates.
// Identical data must not reset selection, scroll position or widget geometry.
void SetRows(QTableWidget* table, const std::vector<QStringList>& rows)
{
    const int scroll{table->verticalScrollBar()->value()};
    if (table->rowCount() != static_cast<int>(rows.size())) table->setRowCount(static_cast<int>(rows.size()));
    for (int r{0}; r < static_cast<int>(rows.size()); ++r) {
        for (int c{0}; c < rows[r].size(); ++c) {
            auto* item{table->item(r, c)};
            if (!item) {
                item = new QTableWidgetItem{rows[r][c]};
                item->setTextAlignment(c == 0 ? Qt::AlignLeft | Qt::AlignVCenter : Qt::AlignRight | Qt::AlignVCenter);
                table->setItem(r, c, item);
            } else if (item->text() != rows[r][c]) item->setText(rows[r][c]);
        }
    }
    if (table->verticalScrollBar()->value() != scroll) table->verticalScrollBar()->setValue(scroll);
}
struct Choice { QString text; QVariant data; QString identity; };
void SetChoices(QComboBox* combo, const std::vector<Choice>& choices, const QString& selected, bool fallback)
{
    const QSignalBlocker block{combo};
    while (combo->count() > static_cast<int>(choices.size())) combo->removeItem(combo->count() - 1);
    int selected_index{-1};
    for (int i{0}; i < static_cast<int>(choices.size()); ++i) {
        const auto& choice{choices[i]};
        if (i == combo->count()) combo->addItem(choice.text, choice.data);
        else {
            if (combo->itemText(i) != choice.text) combo->setItemText(i, choice.text);
            if (combo->itemData(i) != choice.data) combo->setItemData(i, choice.data);
        }
        if (combo->itemData(i, Qt::UserRole + 1).toString() != choice.identity) combo->setItemData(i, choice.identity, Qt::UserRole + 1);
        if (choice.identity == selected) selected_index = i;
    }
    if (selected_index < 0 && fallback && !choices.empty()) selected_index = 0;
    if (combo->currentIndex() != selected_index) combo->setCurrentIndex(selected_index);
}
} // namespace

B3FlowMeshTradingPanel::B3FlowMeshTradingPanel(QWidget* parent) : QWidget{parent}
{
    setObjectName(QStringLiteral("flowMeshTradingPanel"));
    auto* outer{new QVBoxLayout{this}}; outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll{new QScrollArea{this}}; scroll->setWidgetResizable(true);
    auto* content{new QWidget{scroll}}; auto* layout{new QVBoxLayout{content}};
    layout->setContentsMargins(12, 10, 12, 12); layout->setSpacing(8);
    auto* connection_row{new QHBoxLayout};
    m_endpoint = new QLineEdit{content}; m_endpoint->setObjectName(QStringLiteral("flowMeshEndpoint"));
    m_endpoint->setMaxLength(2048); m_endpoint->setPlaceholderText(tr("https://trading.example.org"));
    m_endpoint->setAccessibleName(tr("HTTPS trading endpoint"));
    m_endpoint->setToolTip(tr("Connect saves this HTTPS endpoint and checks market discovery. It does not unlock your wallet or submit any trading action."));
    m_connect = new QPushButton{tr("Connect"), content}; m_connect->setObjectName(QStringLiteral("flowMeshConnect"));
    connection_row->addWidget(m_endpoint, 1); connection_row->addWidget(m_connect); layout->addLayout(connection_row);
    m_connection_status = Label(tr("Select a wallet to check its trading connection."), content);
    m_connection_status->setObjectName(QStringLiteral("flowMeshConnectionStatus"));
    B3Theme::markTextRole(m_connection_status, QStringLiteral("secondary")); layout->addWidget(m_connection_status);
    auto* heading{new QHBoxLayout}; auto* heading_copy{new QVBoxLayout};
    auto* eyebrow{Label(tr("FlowMesh"), content)}; B3Theme::markTextRole(eyebrow, QStringLiteral("h3")); heading_copy->addWidget(eyebrow);
    m_pair_title = Label(tr("Trade"), content); m_pair_title->hide(); heading->addLayout(heading_copy);
    m_market = new QComboBox{content}; m_market->setObjectName(QStringLiteral("flowMeshMarket"));
    m_market->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); m_market->setMinimumContentsLength(16); m_market->setAccessibleName(tr("Spot market")); heading->addWidget(m_market);
    m_orientation = new QComboBox{content}; m_orientation->setObjectName(QStringLiteral("flowMeshOrientation"));
    m_orientation->addItems({tr("Token / B3 (canonical)"), tr("B3 / token")}); m_orientation->setCurrentIndex(1); m_orientation->setAccessibleName(tr("Market display orientation"));
    m_orientation->setToolTip(tr("B3 trading view is the default. The optional canonical view does not change the market or old orders. Orders specify a token quantity cap, never a guaranteed B3 fill.")); heading->addWidget(m_orientation);
    m_last_price = Label(tr("Last price  —"), content); B3Theme::markTextRole(m_last_price, QStringLiteral("h3")); heading->addWidget(m_last_price); heading->addStretch();
    m_deposit = new QPushButton{tr("Deposit"), content}; m_deposit->setProperty("b3variant", QStringLiteral("primary")); m_withdraw = new QPushButton{tr("Withdraw"), content}; heading->addWidget(m_deposit); heading->addWidget(m_withdraw); layout->addLayout(heading);
    m_status = Label(tr("Select a wallet to trade."), content); m_status->setObjectName(QStringLiteral("flowMeshReadiness")); B3Theme::markTextRole(m_status, QStringLiteral("secondary")); layout->addWidget(m_status);
    m_refresh = new QPushButton{tr("Refresh now"), content}; m_refresh->setToolTip(tr("Market data updates automatically. Request an immediate read-only update."));
    m_progress = Label(QString{}, content); B3Theme::markTextRole(m_progress, QStringLiteral("secondary"));
    auto* center{new QSplitter{Qt::Horizontal, content}}; center->setChildrenCollapsible(false);
    auto* chart_card{new QWidget{center}}; B3Theme::markCard(chart_card); auto* chart_layout{new QVBoxLayout{chart_card}};
    auto* chart_modes{new QHBoxLayout}; auto* modes{new QButtonGroup{chart_card}};
    auto* candle_interval{new QComboBox{chart_card}}; candle_interval->setObjectName(QStringLiteral("flowMeshCandleInterval"));
    candle_interval->setAccessibleName(tr("Microblocks per candlestick"));
    for (const int count : {1, 5, 20}) candle_interval->addItem(tr("%1 microblock(s)").arg(count), count);
    candle_interval->setCurrentIndex(1);
    candle_interval->setToolTip(tr("Candles group actual clearings by microblock sequence. These intervals are not minutes; no signed trade timestamp is available."));
    for (const auto& mode : {std::pair{tr("Candles"), B3FlowMeshChart::Mode::Prices}, std::pair{tr("Depth"), B3FlowMeshChart::Mode::Liquidity}}) {
        auto* button{new QPushButton{mode.first, chart_card}}; button->setCheckable(true); button->setChecked(mode.second == B3FlowMeshChart::Mode::Prices); button->setProperty("b3variant", QStringLiteral("timeframe")); modes->addButton(button); chart_modes->addWidget(button);
        button->setObjectName(mode.second == B3FlowMeshChart::Mode::Prices ? QStringLiteral("flowMeshChartPrices") : QStringLiteral("flowMeshChartLiquidity"));
        connect(button, &QPushButton::clicked, this, [this, mode, candle_interval] { m_chart->setMode(mode.second); candle_interval->setEnabled(mode.second == B3FlowMeshChart::Mode::Prices); });
    }
    chart_modes->addStretch(); chart_modes->addWidget(candle_interval); chart_layout->addLayout(chart_modes); m_chart = new B3FlowMeshChart{chart_card}; chart_layout->addWidget(m_chart, 1);
    connect(candle_interval, &QComboBox::currentIndexChanged, this, [this, candle_interval] { m_chart->setCandleInterval(candle_interval->currentData().toULongLong()); });
    m_chart->setToolTip(tr("Candles show open, high, low and close from actual clearings, grouped by microblock sequence. Hover to inspect a candle and its volume. Idle intervals are not filled in. Depth lines show evaluated demand and supply, not historical trades."));
    const auto table = [](QWidget* parent, const QStringList& headers, const char* name) {
        auto* view{new QTableWidget{parent}}; view->setObjectName(QLatin1String(name)); view->setColumnCount(headers.size()); view->setHorizontalHeaderLabels(headers); view->verticalHeader()->hide(); view->setShowGrid(false); view->setAlternatingRowColors(false); view->setEditTriggers(QAbstractItemView::NoEditTriggers); view->setSelectionMode(QAbstractItemView::NoSelection); view->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); view->verticalHeader()->setDefaultSectionSize(26); view->setMinimumWidth(200); return view;
    };
    auto* liquidity_card{new QWidget{center}}; B3Theme::markCard(liquidity_card); auto* liquidity_layout{new QVBoxLayout{liquidity_card}};
    auto* liquidity_title{Label(tr("Liquidity"), liquidity_card)}; liquidity_title->setObjectName(QStringLiteral("flowMeshLiquidityTitle")); B3Theme::markTextRole(liquidity_title, QStringLiteral("h3")); liquidity_layout->addWidget(liquidity_title);
    auto* liquidity_mode{new QComboBox{liquidity_card}}; liquidity_mode->setObjectName(QStringLiteral("flowMeshLiquidityMode"));
    liquidity_mode->setAccessibleName(tr("Liquidity display")); liquidity_mode->addItems({tr("Order book"), tr("Curve depth")}); liquidity_layout->addWidget(liquidity_mode);
    m_order_book = new B3FlowMeshOrderBook{liquidity_card}; liquidity_layout->addWidget(m_order_book, 1);
    m_depth_view = table(liquidity_card, {tr("Price"), tr("Buy liquidity"), tr("Sell liquidity")}, "flowMeshCurveDepth"); liquidity_layout->addWidget(m_depth_view, 1);
    m_liquidity_note = Label(tr("No certified curves yet."), liquidity_card); B3Theme::markTextRole(m_liquidity_note, QStringLiteral("secondary")); liquidity_layout->addWidget(m_liquidity_note);
    m_depth_view->hide(); m_liquidity_note->hide();
    connect(liquidity_mode, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_order_book->setVisible(index == 0); m_depth_view->setVisible(index == 1); m_liquidity_note->setVisible(index == 1);
    });
    auto* ticket_card{new QWidget{center}}; ticket_card->setMinimumWidth(250); B3Theme::markCard(ticket_card); auto* ticket{new QVBoxLayout{ticket_card}};
    auto* sides{new QHBoxLayout}; auto* side_group{new QButtonGroup{ticket_card}};
    m_buy = new QPushButton{tr("Buy"), ticket_card}; m_sell = new QPushButton{tr("Sell"), ticket_card};
    m_buy->setObjectName(QStringLiteral("flowMeshBuy")); m_sell->setObjectName(QStringLiteral("flowMeshSell"));
    for (auto* b : {m_buy, m_sell}) { b->setCheckable(true); side_group->addButton(b); sides->addWidget(b); } m_buy->setChecked(true); ticket->addLayout(sides);
    m_side = new QComboBox{this}; m_side->addItems({QStringLiteral("bid"), QStringLiteral("ask")}); m_side->hide();
    const auto entry = [&](const char* name, int length) {
        auto* field{new QLineEdit{this}}; field->setObjectName(QLatin1String(name)); field->setMaxLength(length); return field;
    };
    m_price_label = Label(tr("Limit price · B3 / token"), ticket_card); ticket->addWidget(m_price_label); m_price = entry("flowMeshPriceAtoms", 64); m_price->setPlaceholderText(tr("Enter price")); ticket->addWidget(m_price);
    m_quantity_label = Label(tr("Quantity · token"), ticket_card); ticket->addWidget(m_quantity_label); m_quantity = entry("flowMeshRawQuantity", 64); m_quantity->setPlaceholderText(tr("Enter quantity")); ticket->addWidget(m_quantity);
    m_grid_note = Label(tr("Token units will be verified from asset metadata."), ticket_card); B3Theme::markTextRole(m_grid_note, QStringLiteral("secondary"));
    m_ticket_available = Label(tr("Available  —"), ticket_card); ticket->addWidget(m_ticket_available); m_ticket_total = Label(tr("Limit notional  —"), ticket_card); ticket->addWidget(m_ticket_total);
    m_ticket_fee = Label(tr("Spot fee 0.01% · seller-paid"), ticket_card); B3Theme::markTextRole(m_ticket_fee, QStringLiteral("secondary")); ticket->addWidget(m_ticket_fee);
    m_order = new QPushButton{tr("Review buy order…"), ticket_card}; m_order->setObjectName(QStringLiteral("flowMeshSubmitOrder")); m_order->setProperty("b3variant", QStringLiteral("primary")); ticket->addWidget(m_order);
    m_order->setToolTip(tr("One order per side. A new order replaces that side. Orders clear together at one auction price; an accepted request is not a fill.")); ticket->addStretch();
    center->addWidget(chart_card); center->addWidget(liquidity_card); center->addWidget(ticket_card); center->setStretchFactor(0, 6); center->setStretchFactor(1, 3); center->setStretchFactor(2, 3); center->setSizes({600, 290, 300}); layout->addWidget(center, 1);
    m_activity = new QTabWidget{content}; m_activity->setMinimumHeight(185);
    auto* own{new QWidget{m_activity}}; auto* own_layout{new QVBoxLayout{own}}; m_own_note = Label(tr("No open orders"), own); own_layout->addWidget(m_own_note); m_own_view = table(own, {tr("Side"), tr("Price"), tr("Remaining"), tr("Reserved")}, "flowMeshOwnCurves"); own_layout->addWidget(m_own_view);
    m_balances = Label(tr("Certified balances will appear here."), own); own_layout->addWidget(m_balances); m_cancel_order = new QPushButton{tr("Cancel selected Buy / Sell side…"), own}; own_layout->addWidget(m_cancel_order); m_activity->addTab(own, tr("Your orders"));
    auto* history_page{new QWidget{m_activity}}; auto* history_layout{new QVBoxLayout{history_page}}; m_history_note = Label(tr("No trades yet"), history_page); history_layout->addWidget(m_history_note); m_history_view = table(history_page, {tr("Trade batch"), tr("Price (B3)"), tr("Amount"), tr("Your buy"), tr("Your sell")}, "flowMeshCertifiedFills"); history_layout->addWidget(m_history_view); m_activity->addTab(history_page, tr("Trade history"));
    m_log = new QPlainTextEdit{m_activity}; m_log->setObjectName(QStringLiteral("flowMeshOperationLog")); m_log->setReadOnly(true); m_log->setMaximumBlockCount(100); m_activity->addTab(m_log, tr("Activity")); layout->addWidget(m_activity);
    auto* saved_page{new QWidget{m_activity}}; auto* saved_layout{new QVBoxLayout{saved_page}};
    m_saved_selector = new QComboBox{saved_page}; m_saved_selector->setObjectName(QStringLiteral("flowMeshSavedActions"));
    saved_layout->addWidget(m_saved_selector);
    m_receipt_card = Label(tr("Loading this wallet's locally saved requests. No action is automatically resent."), saved_page);
    m_receipt_card->setObjectName(QStringLiteral("flowMeshReceiptCard")); saved_layout->addWidget(m_receipt_card);
    m_status_read_state = Label(tr("Status checks are read-only; no action is resent."), saved_page);
    m_status_read_state->setObjectName(QStringLiteral("flowMeshStatusReadState")); saved_layout->addWidget(m_status_read_state);
    m_check_receipt = new QPushButton{tr("Check selected request status"), saved_page}; m_check_receipt->setObjectName(QStringLiteral("flowMeshCheckReceipt"));
    saved_layout->addWidget(m_check_receipt); m_activity->addTab(saved_page, tr("Saved requests"));
    connect(m_saved_selector, &QComboBox::currentIndexChanged, this, &B3FlowMeshTradingPanel::selectSavedAction);
    connect(m_check_receipt, &QPushButton::clicked, this, &B3FlowMeshTradingPanel::requestStatusRead);
    auto* advanced_toggle{new QPushButton{tr("Details"), content}}; advanced_toggle->setObjectName(QStringLiteral("flowMeshDetails")); advanced_toggle->setCheckable(true); advanced_toggle->setFlat(true); auto* details_row{new QHBoxLayout}; details_row->addStretch(); details_row->addWidget(advanced_toggle); layout->addLayout(details_row);
    m_advanced = new QWidget{content}; B3Theme::markCard(m_advanced); auto* settlement{new QVBoxLayout{m_advanced}};
    m_identity_detail = Label(tr("Full market identity will appear after a verified snapshot."), m_advanced); settlement->addWidget(m_identity_detail);
    settlement->addWidget(m_progress); settlement->addWidget(m_grid_note); settlement->addWidget(m_refresh);
    settlement->addWidget(Label(tr("Operator tools: publish certified checkpoints and connected sweeps/payouts. Each transaction requires a separate B3-fee review; nothing is automatically broadcast."), m_advanced));
    m_checkpoint = new QPushButton{tr("Prepare pending checkpoint…"), m_advanced}; settlement->addWidget(m_checkpoint);
    m_effect = new QComboBox{m_advanced}; m_effect->setMinimumContentsLength(20); m_effect->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); settlement->addWidget(m_effect);
    m_destination = entry("flowMeshWithdrawalAddress", 256); m_destination->setPlaceholderText(tr("Exact bound payout destination (withdrawal effects only)")); settlement->addWidget(m_destination);
    m_publish = new QPushButton{tr("Prepare selected sweep / payout…"), m_advanced}; settlement->addWidget(m_publish); layout->addWidget(m_advanced); m_advanced->hide(); connect(advanced_toggle, &QPushButton::toggled, m_advanced, &QWidget::setVisible); connect(advanced_toggle, &QPushButton::toggled, this, [this](bool shown) { if (shown) { m_catalog_age.invalidate(); refresh(); } });
    m_review_uncertain = new QPushButton{tr("Review uncertain submission…"), content}; layout->addWidget(m_review_uncertain);
    m_retry_receipt = new QPushButton{tr("Retry exact saved action…"), content}; m_retry_receipt->setObjectName(QStringLiteral("flowMeshExactRetry")); layout->addWidget(m_retry_receipt);
    // Funding/admission data lives in a modal flow, never in the trade ticket.
    m_asset = new QComboBox{this}; m_asset->addItems({tr("Base asset"), QStringLiteral("B3")}); m_asset->hide();
    m_amount = entry("flowMeshFundingAmount", 64); m_amount->hide(); m_deposit_txid = entry("flowMeshDepositTxid", 64); m_deposit_txid->hide(); m_deposit_vout = entry("flowMeshDepositVout", 10); m_deposit_vout->setText(QStringLiteral("0")); m_deposit_vout->hide();
    m_admit = new QPushButton{this}; m_admit->hide();
    scroll->setWidget(content);
    auto* products{new QTabWidget{this}}; products->setObjectName(QStringLiteral("flowMeshProductTabs"));
    products->setAccessibleName(tr("Trading product")); products->addTab(scroll, tr("Spot"));
    auto* futures{new QWidget{products}}; futures->setObjectName(QStringLiteral("flowMeshFuturesPage"));
    auto* futures_layout{new QVBoxLayout{futures}}; futures_layout->setContentsMargins(24, 24, 24, 24);
    auto* futures_title{Label(tr("Futures trading"), futures)};
    B3Theme::markTextRole(futures_title, QStringLiteral("h3")); futures_layout->addWidget(futures_title);
    auto* futures_status{Label(tr("Not available in this build"), futures)};
    futures_status->setObjectName(QStringLiteral("flowMeshFuturesStatus")); futures_layout->addWidget(futures_status);
    futures_layout->addWidget(Label(tr("Futures remains part of the V2 plan. Positions, margin, funding, price-oracle checks and liquidation are not connected to this wallet yet."), futures));
    futures_layout->addWidget(Label(tr("Futures will use a separate account. Only an explicit authorized transfer may move funds from Spot; futures must never automatically spend your Spot balance."), futures));
    futures_layout->addWidget(Label(tr("This tab is informational only. Opening it does not place an order, transfer funds or unlock your wallet."), futures));
    futures_layout->addStretch(); products->addTab(futures, tr("Futures"));
    products->setTabToolTip(1, tr("Planned V2 product; no futures orders or transfers are available in this build."));
    outer->addWidget(products);
    connect(m_refresh, &QPushButton::clicked, this, &B3FlowMeshTradingPanel::refresh);
    connect(m_connect, &QPushButton::clicked, this, &B3FlowMeshTradingPanel::requestConnect);
    connect(m_endpoint, &QLineEdit::returnPressed, this, &B3FlowMeshTradingPanel::requestConnect);
    connect(m_endpoint, &QLineEdit::textChanged, this, &B3FlowMeshTradingPanel::updateConnectionState);
    connect(m_orientation, &QComboBox::currentIndexChanged, this, [this] {
        // Never reinterpret a typed price or a deferred click in another unit.
        m_price->clear(); m_deferred_review.reset();
        updateMarketText(); updateDataViews();
    });
    connect(m_market, &QComboBox::currentIndexChanged, this, [this] {
        m_deferred_status.reset(); // A queued click never follows a market switch.
        // A failed read/backoff belongs to its selected market. Another listed
        // market may already have a certified head and can be read immediately.
        m_read_error.clear(); m_read_failed = false; m_read_failures = 0; m_attempt_age.invalidate();
        const auto selected{market()};
        if (!selected || !m_snapshot || selected->id != m_snapshot->market) {
            m_snapshot.reset(); m_response_age.invalidate(); m_certificate_age.invalidate(); m_queue_age.invalidate();
            m_loading = selected.has_value(); updateDataViews();
        }
        updateMarketText(); if (!m_busy) refresh();
    });
    connect(m_order, &QPushButton::clicked, this, [this] { begin(Operation::Order); });
    connect(m_cancel_order, &QPushButton::clicked, this, [this] { begin(Operation::Cancel); });
    connect(m_deposit, &QPushButton::clicked, this, [this] { openFunding(false); });
    connect(m_admit, &QPushButton::clicked, this, [this] { begin(Operation::Admit); });
    connect(m_withdraw, &QPushButton::clicked, this, [this] { openFunding(true); });
    connect(m_checkpoint, &QPushButton::clicked, this, [this] { begin(Operation::Checkpoint); });
    connect(m_publish, &QPushButton::clicked, this, [this] { begin(Operation::Vault); });
    connect(m_review_uncertain, &QPushButton::clicked, this, &B3FlowMeshTradingPanel::reviewUncertain);
    connect(m_retry_receipt, &QPushButton::clicked, this, &B3FlowMeshTradingPanel::retryReceipt);
    connect(m_effect, &QComboBox::currentIndexChanged, this, &B3FlowMeshTradingPanel::updateControls);
    connect(m_price, &QLineEdit::textChanged, this, &B3FlowMeshTradingPanel::updateTicket); connect(m_quantity, &QLineEdit::textChanged, this, &B3FlowMeshTradingPanel::updateTicket);
    connect(m_buy, &QPushButton::clicked, this, [this] { m_side->setCurrentIndex(0); updateTicket(); }); connect(m_sell, &QPushButton::clicked, this, [this] { m_side->setCurrentIndex(1); updateTicket(); });
    m_timer = new QTimer{this}; m_timer->setInterval(500);
    connect(m_timer, &QTimer::timeout, this, [this] { if (isVisible()) { updateMarketText(); refresh(); } });
    connect(qApp, &QCoreApplication::aboutToQuit, this, &B3FlowMeshTradingPanel::cancelAndWait);
    updateControls();
}

B3FlowMeshTradingPanel::~B3FlowMeshTradingPanel() { cancelAndWait(); }

void B3FlowMeshTradingPanel::setWalletModel(WalletModel* wallet)
{
    cancelAndWait();
    m_wallet = wallet; m_backend.reset(); m_market_data.clear(); m_effect_data.clear(); m_snapshot.reset();
    m_saved_actions = {}; m_saved_actions_ready = false;
    m_client_info.reset(); m_connection_error.clear(); m_connect_error.clear();
    { QSignalBlocker blocker{m_saved_selector}; m_saved_selector->clear(); }
    m_receipt_card->setText(wallet ? tr("Loading this wallet's locally saved requests. No action is automatically resent.") : tr("Open a wallet to inspect its saved requests."));
    m_response_age.invalidate(); m_certificate_age.invalidate(); m_catalog_age.invalidate(); m_attempt_age.invalidate(); m_queue_age.invalidate(); m_read_failed = false; m_read_failures = 0; m_read_error.clear();
    m_uncertain_refreshed = false;
    { QSignalBlocker blocker{m_market}; m_market->clear(); } m_effect->clear();
    m_wallet_name = wallet ? wallet->getDisplayName() : tr("No wallet");
    if (wallet) {
        for (auto& candidate : wallet->node().walletLoader().getWallets()) {
            if (candidate->wallet() == wallet->wallet().wallet()) { m_backend = std::move(candidate); break; }
        }
        const auto generation{m_generation};
        // Already queued notifications from a formerly selected model must not
        // detach a new wallet after a switch.
        connect(wallet, &WalletModel::unload, this, [this, generation] { if (generation == m_generation) setWalletModel(nullptr); });
        connect(wallet, &QObject::destroyed, this, [this, generation] { if (generation == m_generation) setWalletModel(nullptr); });
        m_cancel->store(false);
        m_timer->start();
    }
    updateDataViews(); updateMarketText(); refresh();
}

void B3FlowMeshTradingPanel::selectBaseAsset(const QString& asset_id, bool withdrawal)
{
    if (m_busy) { notice(tr("Finish or cancel the current operation before selecting another asset.")); return; }
    m_requested_base = asset_id.toLower(); m_requested_withdrawal = withdrawal; m_route_pending = true;
    m_asset->setCurrentIndex(asset_id.isEmpty() ? 1 : 0);
    refresh();
}

std::optional<Market> B3FlowMeshTradingPanel::market() const
{
    const int i{m_market->currentIndex()};
    if (i < 0 || static_cast<size_t>(i) >= m_market_data.size()) return std::nullopt;
    return m_market_data[i];
}

void B3FlowMeshTradingPanel::notice(const QString& text) { m_log->appendPlainText(text); }
bool B3FlowMeshTradingPanel::inverted() const { return m_orientation->currentIndex() == 1; }

void B3FlowMeshTradingPanel::updateMarketText()
{
    const auto selected{market()};
    const bool matched{selected && m_snapshot && m_snapshot->market == selected->id};
    QString status{m_wallet ? tr("Select a market") : tr("Select a wallet to trade")};
    if (m_wallet && m_read_failed) status = tr("Market list unavailable");
    QString details{tr("Select a market to see its details.")};
    if (!selected) { m_balances->clear(); m_pair_title->setText(tr("Trade")); }
    else {
        const QString token{matched && m_snapshot->units.known ? m_snapshot->units.ticker : selected->base.left(12) + QStringLiteral("…")};
        m_pair_title->setText((inverted() ? QStringLiteral("B3 / %1") : QStringLiteral("%1 / B3")).arg(token));
        m_orientation->setItemText(0, token + tr(" / B3 (canonical)")); m_orientation->setItemText(1, QStringLiteral("B3 / ") + token);
        for (int i{0}; i < m_market->count() && static_cast<size_t>(i) < m_market_data.size(); ++i) {
            const QString asset{m_market_data[i].id == selected->id ? token : m_market_data[i].base.left(12) + QStringLiteral("…")};
            m_market->setItemText(i, (inverted() ? QStringLiteral("B3 / %1") : QStringLiteral("%1 / B3")).arg(asset));
        }
        m_pair_title->setToolTip(tr("Wallet: %1\nCanonical base asset: %2\nMarket: %3\nVault: %4").arg(m_wallet_name, selected->base, selected->id, selected->vault));
        details = m_pair_title->toolTip() + (matched && m_snapshot->units.known ? tr("\nMetadata: %1 · %2 decimals · %3\nNames and tickers do not prove reserves or dollar backing.").arg(m_snapshot->units.source).arg(m_snapshot->units.decimals).arg(m_snapshot->units.test_only ? tr("TEST ASSET") : tr("asset identity shown above")) : tr("\nToken precision has not been verified."));
        status = m_read_failed ? tr("Market data unavailable · trading paused") : tr("Loading market…");
        if (!matched) m_balances->clear();
        if (matched) {
            const auto& s{*m_snapshot};
            if (m_read_failed || !m_response_age.isValid() || m_response_age.elapsed() > 3000) status = tr("Updates delayed · trading paused");
            else if (!s.running) status = tr("Market service offline");
            else if (s.chain_reconciling && s.halt == QStringLiteral("none")) status = tr("Reconciling B3 tip · retrying");
            else if (s.halt != QStringLiteral("none") || !s.error.isEmpty()) status = tr("Market halted · see Details");
            else if (s.handoff) status = tr("Validator handoff · trading paused");
            else if (s.paused) status = tr("Trading paused · waiting for validators");
            else if (!s.certified) status = tr("Waiting for the first confirmed update");
            else if (s.pending_actions && m_queue_age.isValid() && m_queue_age.elapsed() >= 30'000) status = tr("Confirmation delayed · new requests paused");
            else if (!B3FlowMeshMarketData::AdmissionReady(s, m_response_age.elapsed(), -1)) status = tr("Trading unavailable · check Details");
            else if (m_pending_sequence && selected->id == m_pending_market && selected->account == m_pending_account && (m_receipt || selected->sequence <= *m_pending_sequence)) status = tr("Request submitted · awaiting verified inclusion");
            else if (s.pending_actions) status = tr("Confirming orders…");
            else status = s.remote ? tr("Remote client · verified state") : tr("Latest confirmed market data");
            if (!s.units.known) status += tr(" · asset precision unavailable");
            if (s.units.test_only) status = tr("TEST ASSET · unbacked · ") + status;
            const auto base = [&](CAmount n) -> QString {
                if (s.units.known) return B3FlowMeshMarketData::FormatAmount(n, s.units.decimals) + QLatin1Char(' ') + s.units.ticker;
                return QString::number(n) + tr(" raw units (precision unknown)");
            };
            m_balances->setText(selected->has_account ? tr("Available  %1 · %2 B3    |    In orders  %3 · %4 B3").arg(base(selected->base_available), B3FlowMeshMarketData::FormatAmount(selected->b3_available, 9), base(selected->base_reserved), B3FlowMeshMarketData::FormatAmount(selected->b3_reserved, 9)) : tr("Deposit to start trading"));
            details += tr("\nAccount %1 · next sequence %2").arg(selected->account, QString::number(selected->sequence));
            if (s.remote) details += tr("\nEndpoint: %1\nCertificate and account state verified locally. Endpoint availability does not prove current validator quorum or the newest network head.").arg(s.endpoint);
        }
    }
    // Build the final copy before touching labels: intermediate variants of
    // unchanged data still invalidate layouts and accessibility observations.
    m_identity_detail->setText(details);
    QString progress;
    if (selected && selected->checkpoint_pending) progress = tr("Certified checkpoint awaiting B3 publication. This is separate from trade execution.");
    else if (selected && m_pending_sequence && selected->id == m_pending_market && !m_receipt) progress = tr("Request submitted at account sequence %1. A sequence change alone does not prove its outcome. No automatic retry.").arg(*m_pending_sequence);
    else if (matched) progress = tr("Certified microblock #%1 · epoch %2 · configured threshold %3 of %4 FN seats · %5 queued actions\nAn available runtime or configured threshold is not proof that those validators are currently online.").arg(m_snapshot->next_sequence ? m_snapshot->next_sequence - 1 : 0).arg(m_snapshot->epoch).arg(m_snapshot->quorum_required).arg(m_snapshot->active_seats).arg(m_snapshot->pending_actions);
    else progress = tr("Uniform-price curve auction · no price-time priority, no fabricated prices or trades.");
    if (matched) progress = B3FlowMeshMarketData::StatusText(*m_snapshot, m_read_failed || !m_response_age.isValid() ? -1 : m_response_age.elapsed(), m_certificate_age.isValid() ? m_certificate_age.elapsed() : -1, m_queue_age.isValid() ? m_queue_age.elapsed() : -1) + QLatin1Char('\n') + progress;
    if (m_receipt && selected && selected->id == m_pending_market && receiptWalletSelected()) {
        progress += QLatin1Char('\n') + B3FlowMeshTrading::DescribeReceipt(*m_receipt);
        if (!m_receipt_error.isEmpty()) progress += tr("\nAction status unavailable: %1. The saved request remains unresolved.").arg(m_receipt_error);
    }
    m_progress->setText(progress);
    if (m_uncertain) status = tr("Submission outcome unknown · review before trading") + (matched && m_snapshot->units.test_only ? tr(" · TEST ASSET · unbacked") : QString{});
    if (m_read_failed && !m_read_error.isEmpty()) status += QLatin1Char('\n') + m_read_error;
    if (!m_security_warning.isEmpty()) status = m_security_warning + QLatin1Char('\n') + status;
    m_status->setText(status); m_status->setToolTip(m_progress->text());
    updateTicket(); updateControls();
}

void B3FlowMeshTradingPanel::updateDataViews()
{
    using namespace B3FlowMeshMarketData;
    const auto selected{market()}; const bool matched{selected && m_snapshot && selected->id == m_snapshot->market};
    const bool inverse{inverted()};
    m_chart->setInverted(inverse); m_chart->setSnapshot(matched ? m_snapshot : std::nullopt); m_chart->setLoading(m_loading);
    m_order_book->setSnapshot(matched ? m_snapshot : std::nullopt, inverse);
    if (!matched || !m_snapshot->units.known) {
        for (auto* table : {m_depth_view, m_history_view, m_own_view}) SetRows(table, {});
        if (auto* title{m_depth_view->parentWidget()->findChild<QLabel*>(QStringLiteral("flowMeshLiquidityTitle"))}) title->setText(tr("Liquidity"));
        m_history_note->setText(tr("Trade history unavailable")); m_own_note->setText(tr("Orders unavailable"));
        m_last_price->setText(tr("Last price  —")); m_liquidity_note->setText(matched ? tr("Asset precision unavailable") : m_loading ? tr("Loading liquidity…") : selected ? tr("Market data unavailable") : tr("Select a market")); return;
    }
    const auto& s{*m_snapshot}; const auto& u{s.units};
    m_own_note->setText(s.own_curves.empty() ? tr("No open orders") : tr("Your open orders"));
    m_own_note->setToolTip(tr("Orders are persistent certified curves. Replacing a side changes that entire curve; there is no price-time priority queue."));
    std::vector<QStringList> depth, history, own;
    const auto add_depth = [&](const Depth& d) {
        depth.push_back({FormatDisplayPrice(d.price, u.decimals, inverse),
            FormatDepthQuantity(d.price, inverse ? d.supply : d.demand, u.decimals, inverse),
            FormatDepthQuantity(d.price, inverse ? d.demand : d.supply, u.decimals, inverse)});
    };
    if (inverse) { for (auto it{s.depth.rbegin()}; it != s.depth.rend(); ++it) add_depth(*it); }
    else { for (const auto& d : s.depth) add_depth(d); }
    const QString price_units{(inverse ? QStringLiteral("%1 / B3") : QStringLiteral("B3 / %1")).arg(u.ticker)};
    const QString quantity_units{inverse ? QStringLiteral("B3") : u.ticker};
    m_depth_view->setHorizontalHeaderLabels({tr("Price"), tr("Buy liquidity"), tr("Sell liquidity")});
    m_depth_view->horizontalHeaderItem(0)->setToolTip(tr("Price in %1; ≈ marks display-only approximation").arg(price_units));
    for (int column : {1, 2}) m_depth_view->horizontalHeaderItem(column)->setToolTip(tr("Gross quantity in %1, evaluated at this price").arg(quantity_units));
    if (auto* title{m_depth_view->parentWidget()->findChild<QLabel*>(QStringLiteral("flowMeshLiquidityTitle"))}) title->setText(tr("Liquidity · %1").arg(quantity_units));
    m_liquidity_note->setText(s.curves.empty() ? tr("No orders yet") : s.curves_complete ? tr("Aggregate curves · Price in %1").arg(price_units) : tr("Partial liquidity only"));
    m_liquidity_note->setToolTip(tr("Each row evaluates all remaining curves at that price; do not sum the rows. Only certified curves are shown. A zero canonical price has no finite inverse and is shown as a dash.") +
        (inverse ? tr("\nBuy/sell liquidity is gross B3 equivalents at each sampled price, not fixed B3-sized orders. Original token quantities remain the exact order amounts.") : QString{}));
    if (m_history_view->columnCount() != 6) m_history_view->setColumnCount(6);
    m_history_view->setHorizontalHeaderLabels({tr("Trade batch"), tr("Price (%1)").arg(price_units), tr("Amount (%1)").arg(quantity_units), tr("Your buy"), tr("Your sell"), tr("Auction fee (B3)")});
    m_history_view->horizontalHeaderItem(5)->setToolTip(tr("Whole-auction B3 fee, not your individual allocation. Endpoint-reported history remains unverified when indicated below."));
    m_own_view->setHorizontalHeaderLabels({tr("Side"), tr("Price"), tr("Remaining (%1)").arg(u.ticker), tr("Reserved")});
    QString last_price{tr("Last price  —")};
    for (auto it{s.history.rbegin()}; it != s.history.rend(); ++it) {
        if (!it->cleared || it->quantity == 0) continue;
        if (history.empty()) last_price = FormatDisplayPrice(it->price, u.decimals, inverse) + QLatin1Char(' ') + price_units;
        history.push_back({QStringLiteral("#%1").arg(it->sequence), FormatDisplayPrice(it->price, u.decimals, inverse), FormatDepthQuantity(it->price, it->quantity, u.decimals, inverse),
            it->own_fills_known ? FormatDepthQuantity(it->price, inverse ? it->own_sell : it->own_buy, u.decimals, inverse) : QStringLiteral("—"),
            it->own_fills_known ? FormatDepthQuantity(it->price, inverse ? it->own_buy : it->own_sell, u.decimals, inverse) : QStringLiteral("—"), FormatAmount(it->fee, 9)});
    }
    for (const auto& c : s.own_curves) {
        QString description;
        if (c.points.size() == 2 && c.points[1].price == c.points[0].price + 1) description = FormatDisplayPrice(c.side == QStringLiteral("bid") ? c.points[0].price : c.points[1].price, u.decimals, inverse) + QLatin1Char(' ') + (inverse ? price_units : QStringLiteral("B3"));
        else description = tr("%1-point curve").arg(c.points.size());
        const bool buy{DisplayBuy(c.side, inverse)};
        own.push_back({inverse ? (buy ? tr("Buy B3") : tr("Sell B3")) : (buy ? tr("Buy") : tr("Sell")), description,
            (inverse ? (buy ? tr("Spend up to ") : tr("Receive up to ")) : QString{}) + FormatAmount(c.remaining, u.decimals),
            FormatAmount(c.reserved, c.side == QStringLiteral("bid") ? 9 : u.decimals) + (c.side == QStringLiteral("bid") ? QStringLiteral(" B3") : QLatin1Char(' ') + u.ticker)});
    }
    SetRows(m_depth_view, depth); SetRows(m_history_view, history); SetRows(m_own_view, own);
    for (int i{0}; i < m_depth_view->rowCount(); ++i) {
        const QString tooltip{inverse ? tr("Exact inverse price: %1").arg(ExactInversePrice(s.depth[s.depth.size() - 1 - i].price, u.decimals)) : QString{}};
        m_depth_view->item(i, 0)->setToolTip(tooltip);
        if (m_depth_view->item(i, 1)->foreground().color() != B3Theme::kPositive) m_depth_view->item(i, 1)->setForeground(B3Theme::kPositive);
        if (m_depth_view->item(i, 2)->foreground().color() != B3Theme::kNegative) m_depth_view->item(i, 2)->setForeground(B3Theme::kNegative);
    }
    for (int i{0}; i < static_cast<int>(s.own_curves.size()); ++i) {
        const auto& c{s.own_curves[i]};
        const bool limit_curve{c.points.size() == 2 && c.points[1].price == c.points[0].price + 1};
        const QString tooltip{inverse && limit_curve ? tr("Exact inverse limit: %1").arg(ExactInversePrice(c.side == QStringLiteral("bid") ? c.points[0].price : c.points[1].price, u.decimals)) : QString{}};
        m_own_view->item(i, 1)->setToolTip(tooltip);
    }
    for (int i{0}; i < static_cast<int>(s.own_curves.size()); ++i) { const auto& c{s.own_curves[i]}; m_own_view->item(i, 0)->setToolTip(tr("Certified account %1\nFilled: %2 %3\nCancellation targets this account's whole selected side.").arg(c.account, FormatAmount(c.filled, u.decimals), u.ticker)); }
    QString history_note{!s.history_available ? tr("History unavailable on this node") : history.empty() ? tr("No trades in recent history") : s.history_truncated || s.history_page_partial ? tr("Recent trades · partial history") : tr("Confirmed trades")};
    if (inverse && !history.empty()) history_note += tr(" · gross B3 before fees");
    QString history_tooltip{tr("Only retained certified clearings are shown, indexed by microblock sequence. A dash means your fill is unknown, not zero. Missing history does not prove that no trading occurred.") +
        (inverse ? tr("\nB3 buys are gross proceeds before your allocated B3 fee. This snapshot does not report each account's fee, so net B3 received is not invented.") : QString{})};
    const bool reported{s.remote && !s.execution_result_verified};
    if (reported) {
        history_note = tr("Endpoint-reported trades · execution results not independently verified");
        history_tooltip = tr("The whole-state proof verifies balances and remaining orders, not these historical prices or fills. Missing history does not prove that no trades occurred.");
        if (inverse) {
            history_note += tr(" · gross B3 before fees");
            history_tooltip += tr("\nReported B3 buys are gross proceeds before the account's allocated B3 fee, not net B3 received. Individual fee allocation is not supplied and must not be inferred from reported fills or the whole-auction fee.");
        }
        if (!history.empty()) last_price = tr("Reported: %1").arg(last_price);
    }
    m_last_price->setText(last_price);
    m_history_note->setText(history_note); m_history_note->setToolTip(history_tooltip);
    m_chart->setToolTip((reported ? tr("Endpoint-reported price and fill history; execution results are not independently verified.") : tr("Locally verified execution history.")) +
        tr("\nCandles show open, high, low and close by microblock sequence, not elapsed time. Hover to inspect prices and summed trade volume. Unfilled sequence intervals remain gaps; the first and latest candle may contain only part of their interval. Depth shows current orders separately."));
    m_history_view->setToolTip(m_history_note->toolTip());
}

void B3FlowMeshTradingPanel::updateTicket()
{
    using namespace B3FlowMeshMarketData;
    const auto selected{market()}; const bool units_known{selected && m_snapshot && selected->id == m_snapshot->market && m_snapshot->units.known && m_snapshot->units.asset == selected->base};
    const QString ticker{units_known ? m_snapshot->units.ticker : tr("token")}; const bool buy{m_side->currentIndex() == 0}, inverse{inverted()};
    const bool bid{CanonicalSide(buy, inverse) == QStringLiteral("bid")};
    m_buy->setText(inverse ? tr("Buy B3") : tr("Buy")); m_sell->setText(inverse ? tr("Sell B3") : tr("Sell"));
    m_price_label->setText(inverse ? (buy ? tr("Maximum price · %1 / B3") : tr("Minimum price · %1 / B3")).arg(ticker) : tr("Limit price · B3 / %1").arg(ticker));
    m_quantity_label->setText(inverse ? (buy ? tr("Spend up to · %1") : tr("Receive up to · %1")).arg(ticker) : tr("Quantity · %1").arg(ticker));
    m_order->setText(buy ? tr("Review buy order…") : tr("Review sell order…")); m_cancel_order->setText(buy ? tr("Cancel buy order…") : tr("Cancel sell order…"));
    m_ticket_fee->setText(inverse ? (buy ? tr("Actual fee asset: B3 · amount after execution") : tr("Actual fee asset: B3 · paid by B3 receiver")) : tr("Actual fee asset: B3 · seller-paid"));
    QString fee_tooltip{tr("The protocol fee is 0.01% of matched B3 notional, deducted from the B3 receiver's proceeds. Actual fees and allocation depend on certified fills. Submitting an order has no network fee.")};
    QString available_tooltip, total_tooltip;
    const auto apply_tooltips = [&] {
        m_ticket_fee->setToolTip(fee_tooltip);
        m_ticket_available->setToolTip(available_tooltip);
        m_ticket_total->setToolTip(total_tooltip);
    };
    if (!units_known) { m_grid_note->setText(tr("Verified asset precision required; no raw-unit guessing.")); m_grid_note->setToolTip(QString{}); m_ticket_available->setText(tr("Available  —")); m_ticket_total->setText(tr("Order value  —")); apply_tooltips(); return; }
    const auto& u{m_snapshot->units}; m_grid_note->setText(inverse ? tr("Exact %1 quantity cap · no guaranteed B3 fill").arg(ticker) : tr("Steps: %1 %2 · %3 B3").arg(FormatAmount(u.quantity_step, u.decimals), ticker, FormatPrice(u.price_step, u.decimals)));
    m_grid_note->setToolTip(tr("Quantity step %1 %2; canonical price step %3 B3 per %2. Quantity is exact and never rounded.").arg(FormatAmount(u.quantity_step, u.decimals), ticker, FormatPrice(u.price_step, u.decimals)) +
        (inverse ? tr("\nInverse ticks are nonuniform. Prices adjust only in your favor; the exact executable limit is shown before signing. Partial fills and better prices change the actual B3 amount.") : QString{}));
    const CAmount free{bid ? selected->b3_available : selected->base_available}, reserved{bid ? selected->b3_reserved : selected->base_reserved};
    const auto budget{ReplacementBudget(free, reserved)};
    m_ticket_available->setText(tr("Available  %1 %2").arg(FormatAmount(free, bid ? 9 : u.decimals), bid ? QStringLiteral("B3") : ticker));
    available_tooltip = tr("This side already reserves %1 %2; a replacement may use %3 %2 including that reservation.").arg(FormatAmount(reserved, bid ? 9 : u.decimals), bid ? QStringLiteral("B3") : ticker, budget ? FormatAmount(*budget, bid ? 9 : u.decimals) : QStringLiteral("—"));
    QString error; const auto limit{ParseDisplayLimit(m_price->text(), u, inverse, buy, &error)};
    const auto quantity{ParseQuantity(m_quantity->text(), u, &error)};
    if (!limit || !quantity) { m_ticket_total->setText(tr("Order value  —")); total_tooltip = error; apply_tooltips(); return; }
    const auto total{Notional(limit->price, *quantity)}; if (!total) { m_ticket_total->setText(tr("Order value exceeds supported range")); apply_tooltips(); return; }
    QString total_text{(inverse ? (buy ? tr("Gross B3 at limit, if fully filled: %1 B3") : tr("Maximum B3 reservation/spend: %1 B3")) : tr("Order value  %1 B3")).arg(FormatAmount(*total, 9))};
    if (inverse) total_text += tr("\n%1: %2 %3 / B3").arg(limit->adjusted ? tr("Safely adjusted executable limit") : tr("Exact executable limit"), limit->executable_price, ticker);
    m_ticket_total->setText(total_text);
    total_tooltip = inverse ? tr("The exact cap is %1 %2. No fill is guaranteed. Buy B3: the full-fill gross proceeds may improve above the limit illustration, before allocated B3 fees. Sell B3: the stated B3 amount is the maximum reservation/spend, not a promise to sell that exact amount.").arg(FormatAmount(*quantity, u.decimals), ticker) : QString{};
    fee_tooltip = tr("Actual protocol fee asset: B3. The whole-auction fee is 0.01% of matched B3 and is allocated among B3 receivers. Your exact allocated amount is unknown before execution; this is not a stable-asset charge. Submitting an order has no network fee.");
    apply_tooltips();
}

void B3FlowMeshTradingPanel::openFunding(bool withdrawal)
{
    if (!m_wallet || !m_backend || m_busy || !m_security_warning.isEmpty() || m_uncertain) return;
    if (m_thread) { deferReview(withdrawal ? Operation::Withdraw : Operation::Deposit, true); return; }
    const auto selected{market()}; if (!selected) return;
    const bool known{m_snapshot && m_snapshot->market == selected->id && m_snapshot->units.known};
    const QString ticker{known ? m_snapshot->units.ticker : tr("Base asset (precision unavailable)")};
    const auto generation{m_generation}; const QPointer<B3FlowMeshTradingPanel> self{this};
    m_busy = true; updateControls();
    m_funding_dialog = new QDialog{this}; const QPointer<QDialog> dialog{m_funding_dialog}; dialog->setWindowTitle(withdrawal ? tr("Withdraw from FlowMesh") : tr("Deposit to FlowMesh")); dialog->setMinimumWidth(430);
    auto* layout{new QVBoxLayout{dialog}}; layout->addWidget(Label(tr("Wallet: %1\nMarket view: %2\nCanonical base asset ID: %3").arg(m_wallet_name, (inverted() ? QStringLiteral("B3 / %1") : QStringLiteral("%1 / B3")).arg(ticker), selected->base), dialog));
    auto* tabs{new QTabWidget{dialog}}; auto* transfer{new QWidget{tabs}}; auto* form{new QFormLayout{transfer}};
    auto* asset{new QComboBox{transfer}}; asset->addItems({ticker, QStringLiteral("B3")}); asset->setCurrentIndex(m_asset->currentIndex()); form->addRow(tr("Asset"), asset);
    auto* amount{new QLineEdit{transfer}}; amount->setMaxLength(64); amount->setPlaceholderText(tr("Amount in whole tokens, e.g. 1.25")); form->addRow(tr("Amount"), amount);
    auto* destination{new QLineEdit{transfer}}; destination->setMaxLength(256); destination->setText(m_destination->text());
    if (withdrawal) form->addRow(tr("B3 destination address"), destination); else destination->hide();
    form->addRow(Label(withdrawal ? tr("This requests a certified withdrawal; it is not an immediate payout. A checkpoint and a separately reviewed on-chain payout follow. No automatic fee spending.") : tr("A new deposit locks funds in a keyless vault. It needs 31 confirmations before admission and may stay locked without validator quorum. You review the actual B3 network fee before any broadcast."), transfer));
    tabs->addTab(transfer, withdrawal ? tr("Request withdrawal") : tr("New deposit"));
    auto* existing{new QWidget{tabs}}; auto* existing_form{new QFormLayout{existing}};
    auto* txid{new QLineEdit{existing}}; txid->setMaxLength(64); txid->setText(m_deposit_txid->text()); existing_form->addRow(tr("Existing deposit transaction ID"), txid);
    auto* vout{new QLineEdit{existing}}; vout->setMaxLength(10); vout->setText(m_deposit_vout->text()); existing_form->addRow(tr("Deposit output index"), vout);
    existing_form->addRow(Label(tr("Use your already funded deposit—no new tokens are sent. It must belong to this wallet and have at least 31 confirmations. Admission alone is not completed settlement."), existing));
    if (!withdrawal) tabs->addTab(existing, tr("Use existing deposit")); else existing->hide();
    layout->addWidget(tabs); auto* buttons{new QDialogButtonBox{QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog}}; buttons->button(QDialogButtonBox::Ok)->setText(tr("Continue to review…")); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    const bool accepted{dialog->exec() == QDialog::Accepted};
    if (!self || generation != m_generation) { if (dialog) delete dialog.data(); return; }
    const bool admission{!withdrawal && tabs->currentIndex() == 1};
    if (accepted) { m_asset->setCurrentIndex(asset->currentIndex()); m_amount->setText(amount->text()); if (withdrawal) m_destination->setText(destination->text()); m_deposit_txid->setText(txid->text()); m_deposit_vout->setText(vout->text()); }
    if (dialog) delete dialog.data(); m_busy = false; updateControls();
    if (accepted) begin(admission ? Operation::Admit : withdrawal ? Operation::Withdraw : Operation::Deposit);
}

void B3FlowMeshTradingPanel::updateControls()
{
    const auto selected{market()};
    const bool idle{m_wallet && m_backend && !m_busy && m_security_warning.isEmpty()};
    // A read does not disable the ticket. A click waits for that read before
    // opening its review; writes remain serialized by m_busy and m_thread.
    const bool connecting{m_deferred_connect || (m_active_result && !m_active_result->connect_url.isEmpty())};
    const bool signing{idle && !connecting && m_saved_actions_ready && !m_uncertain && !m_backend->privateKeysDisabled()};
    const bool data_ready{selected && m_snapshot && m_snapshot->market == selected->id && !m_read_failed && B3FlowMeshMarketData::AdmissionReady(*m_snapshot, m_response_age.isValid() ? m_response_age.elapsed() : -1, m_certificate_age.isValid() ? m_certificate_age.elapsed() : -1, m_queue_age.isValid() ? m_queue_age.elapsed() : -1)};
    const bool ready{signing && selected && selected->ready && data_ready};
    const bool settlement_ready{selected && m_snapshot && m_snapshot->market == selected->id &&
        !m_read_failed && m_response_age.isValid() && m_response_age.elapsed() <= 3000 &&
        (!m_snapshot->remote || (m_snapshot->certificate_verified && m_snapshot->account_state_verified)) && m_snapshot->running && !m_snapshot->chain_reconciling};
    const bool known{m_snapshot && selected && m_snapshot->market == selected->id && m_snapshot->units.known && m_snapshot->units.asset == selected->base};
    const bool pending{selected && m_pending_sequence && selected->id == m_pending_market && selected->account == m_pending_account && (m_receipt || selected->sequence <= *m_pending_sequence)};
    m_refresh->setEnabled(idle && !m_thread); m_market->setEnabled(idle); m_orientation->setEnabled(idle);
    for (auto* field : {m_price, m_quantity, m_amount, m_destination, m_deposit_txid, m_deposit_vout}) field->setEnabled(idle);
    m_side->setEnabled(idle); m_asset->setEnabled(idle); m_effect->setEnabled(idle);
    m_buy->setEnabled(idle); m_sell->setEnabled(idle);
    m_order->setEnabled(ready && known && selected->has_account && !pending); m_cancel_order->setEnabled(ready && selected->has_account && !pending);
    m_withdraw->setEnabled(ready && selected->has_account && !pending); m_deposit->setEnabled(ready);
    m_admit->setEnabled(ready && selected->has_account);
    m_checkpoint->setEnabled(signing && settlement_ready && selected->publish_ready && selected->checkpoint_pending);
    m_publish->setEnabled(signing && settlement_ready && selected->publish_ready && m_effect->currentIndex() >= 0);
    m_review_uncertain->setVisible(m_uncertain);
    m_review_uncertain->setEnabled(idle && !m_thread && !m_read_failed && m_uncertain && m_uncertain_refreshed && (uncertainWalletSelected() || !m_uncertain_wallet));
    const bool saved{m_receipt && !m_receipt->Included() && !m_receipt->no_resubmit && receiptWalletSelected()};
    m_retry_receipt->setVisible(saved);
    m_retry_receipt->setEnabled(saved && idle && !m_thread && !m_read_failed && m_response_age.isValid() && m_response_age.elapsed() <= 3000);
    m_saved_selector->setEnabled(idle);
    m_check_receipt->setEnabled(idle && selectedStatusRead().has_value());
    updateStatusReadState();
    m_chart->setStale(m_snapshot && (m_read_failed || !m_response_age.isValid() || m_response_age.elapsed() > 3000));
    m_order_book->setStale(m_snapshot && (m_read_failed || !m_response_age.isValid() || m_response_age.elapsed() > 3000));
    m_chart->setLoading(m_loading);
    updateConnectionState();
}

void B3FlowMeshTradingPanel::refresh()
{
    if (!m_wallet || !m_backend || m_busy || m_thread) return;
    if (m_deferred_connect) { resumeConnect(); return; }
    if (m_deferred_status) { resumeStatusRead(); return; }
    if (m_read_failures && m_attempt_age.isValid() && m_attempt_age.elapsed() < std::min(10000U, 500U << std::min(4U, m_read_failures))) return;
    startJob();
}

void B3FlowMeshTradingPanel::requestConnect()
{
    if (!m_connect->isEnabled() || !m_wallet || !m_backend || m_busy || m_cancel->load()) return;
    if (m_active_result && !m_active_result->connect_url.isEmpty()) return;
    m_deferred_review.reset(); // A reviewed market choice never follows an endpoint change.
    m_connection_error.clear(); m_connect_error.clear();
    m_deferred_connect = DeferredConnect{m_wallet, m_generation, &m_wallet->node(), m_endpoint->text().trimmed()};
    resumeConnect(); updateControls();
}

void B3FlowMeshTradingPanel::resumeConnect()
{
    if (!m_deferred_connect || m_thread || m_busy) return;
    const auto queued{*m_deferred_connect}; m_deferred_connect.reset();
    if (m_wallet && m_backend && !m_cancel->load() && m_security_warning.isEmpty() &&
        queued.wallet == m_wallet && queued.generation == m_generation && queued.node == &m_wallet->node())
        startJob(std::nullopt, std::nullopt, false, false, std::nullopt, queued.url);
    updateConnectionState();
}

void B3FlowMeshTradingPanel::updateConnectionState()
{
    const auto text = [](const UniValue& value, const char* field) {
        const auto& item{value.find_value(field)}; return item.isStr() ? QString::fromStdString(item.get_str()).left(2048) : QString{};
    };
    const auto flag = [](const UniValue& value, const char* field) {
        const auto& item{value.find_value(field)}; return item.isBool() && item.get_bool();
    };
    const bool local{m_client_info && (text(*m_client_info, "backend") == QStringLiteral("local") || flag(*m_client_info, "engine_enabled"))};
    const bool running{m_active_result && !m_active_result->connect_url.isEmpty()};
    const QUrl endpoint{m_endpoint->text().trimmed(), QUrl::StrictMode};
    const bool valid_url{endpoint.isValid() && endpoint.scheme() == QStringLiteral("https") && !endpoint.host().isEmpty() && endpoint.userInfo().isEmpty() && !endpoint.hasQuery() && !endpoint.hasFragment()};
    const bool editable{m_wallet && m_backend && !m_busy && m_security_warning.isEmpty() && !local};
    const bool remote{m_client_info && text(*m_client_info, "backend") == QStringLiteral("remote")};
    m_endpoint->setEnabled(editable); m_connect->setEnabled(editable && remote && valid_url && !running);
    m_connect->setText(running ? tr("Connecting…") : m_deferred_connect ? tr("Connect queued") : tr("Connect"));
    QString status{m_wallet ? tr("Checking trading connection…") : tr("Select a wallet to check its trading connection.")};
    QString details{tr("Endpoint reachability does not establish market certification, validator quorum, or trading readiness.")};
    if (m_wallet && m_client_info) {
        if (local) status = tr("Local market engine · remote Connect is unavailable");
        else {
            const auto& endpoints{m_client_info->find_value("endpoints")};
            const QString selected{text(*m_client_info, "selected_endpoint")}, active{text(*m_client_info, "active_endpoint")};
            const UniValue* observation{nullptr};
            if (endpoints.isArray()) for (const auto& row : endpoints.getValues()) {
                if (!observation || text(row, "url") == selected) observation = &row;
                if (text(row, "url") == selected) break;
            }
            if (!observation) status = tr("No HTTPS endpoint configured · enter an endpoint and Connect");
            else {
                const auto& attempted{observation->find_value("last_attempt_ms")};
                if (!attempted.isNum() || attempted.getInt<int64_t>() == 0) status = tr("HTTPS endpoint configured · connection not yet checked");
                else if (!flag(*observation, "transport_available")) status = tr("Selected endpoint: HTTPS connection failed");
                else if (!flag(*observation, "available")) status = tr("Selected endpoint: HTTPS responded · market data unavailable");
                else status = tr("HTTPS responded · selected-market readiness is shown below");
                const QString error{text(*observation, "last_error")};
                if (!error.isEmpty()) status += tr(" · %1").arg(error.left(500));
                const auto& retry{observation->find_value("retry_after_ms")};
                if (retry.isNum() && retry.getInt<int64_t>() > QDateTime::currentMSecsSinceEpoch())
                    status += tr(" · retry in %1 s").arg((retry.getInt<int64_t>() - QDateTime::currentMSecsSinceEpoch() + 999) / 1000);
                details += tr("\nSelected endpoint: %1").arg(selected);
                if (!active.isEmpty()) details += tr("\nLast usable endpoint: %1").arg(active);
                if (!active.isEmpty() && active != selected) status += tr("\nLast usable endpoint: %1").arg(active);
                const auto& failures{observation->find_value("consecutive_failures")};
                if (failures.isNum()) details += tr("\nConsecutive transport failures: %1").arg(failures.getInt<int64_t>());
            }
        }
    }
    if (!m_connection_error.isEmpty()) status += tr("\nConnection check: %1").arg(m_connection_error);
    if (!m_connect_error.isEmpty()) status += tr("\nConnect failed: %1").arg(m_connect_error);
    if (m_deferred_connect) status += tr("\nConnect queued after the current read: %1").arg(m_deferred_connect->url);
    else if (running) status += tr("\nConnecting to %1…").arg(m_active_result->connect_url);
    m_connection_status->setText(status); m_connection_status->setToolTip(details);
}

bool B3FlowMeshTradingPanel::confirm(const QString& text, bool final_transaction)
{
    m_confirmation = new QMessageBox{QMessageBox::Warning, tr("Review FlowMesh action"), text, QMessageBox::Yes | QMessageBox::Cancel, this};
    const QPointer<QMessageBox> dialog{m_confirmation}; dialog->setTextFormat(Qt::PlainText); dialog->setDefaultButton(QMessageBox::Cancel);
    if (final_transaction) {
        auto* checked{new QCheckBox{tr("I checked the wallet, exact outputs, transaction ID and B3 fee."), dialog}};
        dialog->setCheckBox(checked); dialog->button(QMessageBox::Yes)->setEnabled(false);
        connect(checked, &QCheckBox::toggled, dialog->button(QMessageBox::Yes), &QAbstractButton::setEnabled);
    }
    const bool accepted{dialog->exec() == QMessageBox::Yes};
    if (dialog) delete dialog.data();
    return accepted;
}

void B3FlowMeshTradingPanel::begin(Operation operation)
{
    if (!m_wallet || !m_backend || m_busy || m_uncertain || !m_security_warning.isEmpty()) return;
    if (m_thread) { deferReview(operation); return; }
    const auto selected{market()}; if (!selected) return;
    Action a; a.operation = operation; a.market = *selected; a.inverse_display = inverted();
    a.side = B3FlowMeshMarketData::CanonicalSide(m_side->currentIndex() == 0, a.inverse_display); a.native = m_asset->currentIndex() == 1;
    if (operation == Operation::Order) a.native = false; // Funding selection never changes the traded base quantity.
    if (m_snapshot && m_snapshot->market == selected->id && m_snapshot->units.known) { a.display_decimals = m_snapshot->units.decimals; a.display_ticker = m_snapshot->units.ticker; }
    a.destination = m_destination->text();
    try {
        QString error;
        // A funding/review dialog can outlast the display freshness interval.
        // The worker re-reads authoritative readiness immediately before any
        // mutation; never bypass that check or silently refresh reviewed units.
        if (operation != Operation::Checkpoint && operation != Operation::Vault &&
            (!m_snapshot || m_snapshot->market != selected->id || m_read_failed || !m_snapshot->certified)) throw std::runtime_error{"Certified market data is required. Refresh the market before submitting."};
        if (operation != Operation::Checkpoint && operation != Operation::Vault &&
            m_snapshot && m_snapshot->pending_actions > 0 && m_queue_age.isValid() && m_queue_age.elapsed() >= 30'000) throw std::runtime_error{"Queued requests have not certified for 30 seconds. Refresh and inspect the market before submitting. Existing requests were not canceled."};
        if (operation == Operation::Order) {
            if (m_snapshot->units.asset != selected->base) throw std::runtime_error{"Asset precision belongs to a different configured AssetId. Nothing was signed."};
            const auto limit{B3FlowMeshMarketData::ParseDisplayLimit(m_price->text(), m_snapshot->units, a.inverse_display, m_side->currentIndex() == 0, &error)};
            const auto quantity{B3FlowMeshMarketData::ParseQuantity(m_quantity->text(), m_snapshot->units, &error)};
            if (!limit || !quantity) throw std::runtime_error{error.toStdString()}; a.price = limit->price; a.amount = *quantity;
            a.display_limit_adjusted = limit->adjusted; a.entered_display_limit = m_price->text();
        } else if (operation == Operation::Deposit || operation == Operation::Withdraw) {
            const auto amount{a.native ? B3AssetTransfer::ParseAmount(m_amount->text(), 9, &error) : B3FlowMeshMarketData::ParseQuantity(m_amount->text(), m_snapshot->units, &error)};
            if (!amount) throw std::runtime_error{error.toStdString()}; a.amount = *amount;
        } else if (operation == Operation::Admit) {
            a.txid = m_deposit_txid->text().toLower(); const QString text{m_deposit_vout->text()};
            bool ok{false}; const auto n{text.toULongLong(&ok)};
            if (!ok || text.isEmpty() || !std::all_of(text.begin(), text.end(), [](QChar c) { return c >= QLatin1Char('0') && c <= QLatin1Char('9'); }) || n > UINT32_MAX) throw std::runtime_error{"Enter an output index from 0 to 4294967295."};
            a.vout = static_cast<uint32_t>(n);
        } else if (operation == Operation::Vault) {
            const int index{m_effect->currentData().toInt()};
            if (m_effect->currentIndex() < 0 || index < 0 || static_cast<size_t>(index) >= m_effect_data.size()) throw std::runtime_error{"Select a connected wallet effect."};
            a.effect = m_effect_data[index];
        }
        B3FlowMeshTrading::Parameters(a);
    } catch (const std::exception& e) { notice(QString::fromUtf8(e.what())); return; }
    catch (const UniValue& e) { notice(RpcError(e)); return; }
    const QPointer<B3FlowMeshTradingPanel> self{this}; const QPointer<WalletModel> wallet{m_wallet}; const auto generation{m_generation};
    m_busy = true; updateControls();
    const bool accepted{confirm(tr("Selected wallet: %1\n\n").arg(m_wallet_name) + B3FlowMeshTrading::Describe(a) +
        (PreparedOperation(operation) ? tr("\n\nThis first step prepares only. You will review the actual B3 fee and exact transaction before broadcasting.") : tr("\n\nSubmit this request ONCE? There is no automatic retry.")), false)};
    if (!self || !wallet || generation != m_generation) return;
    if (!accepted) { m_busy = false; updateControls(); return; }
    if (operation != Operation::Admit) {
        const bool was_locked{m_backend->isLocked()}; m_restore_locked = was_locked; m_relock_backend = was_locked ? m_backend : nullptr; m_relock_wallet_name = m_wallet_name;
        std::unique_ptr<WalletModel::UnlockContext> unlock;
        try { unlock.reset(new WalletModel::UnlockContext{wallet->requestUnlock(WalletModel::UnlockPurpose::General)}); }
        catch (...) {
            if (wallet && was_locked) wallet->setWalletLocked(true);
            if (self && generation == m_generation) { restoreLock(); m_busy = false; notice(tr("Unlock failed. No FlowMesh request was submitted.")); updateControls(); }
            return;
        }
        if (!self || !wallet || generation != m_generation) return;
        if (!unlock->isValid() || m_backend->isLocked()) { unlock.reset(); restoreLock(); m_busy = false; notice(tr("Wallet remains locked. Nothing was submitted.")); updateControls(); return; }
        m_unlock = std::move(unlock);
    }
    startJob(a);
}

bool B3FlowMeshTradingPanel::deferReview(Operation operation, bool funding)
{
    const auto selected{market()};
    if (!m_thread || m_busy || !m_wallet || !m_backend || !selected || m_uncertain || !m_security_warning.isEmpty()) return false;
    // Save only an intent to OPEN a review. No action is approved, signed or
    // sent here. Freeze inputs so the pending click cannot change underneath it.
    m_deferred_review = DeferredReview{operation, funding, m_generation, selected->id, m_effect->currentData(Qt::UserRole + 1).toString()};
    m_busy = true; updateControls(); return true;
}

void B3FlowMeshTradingPanel::resumeReview()
{
    if (!m_deferred_review || m_thread) return;
    const auto intent{*m_deferred_review}; m_deferred_review.reset();
    m_busy = false; updateControls();
    const auto selected{market()};
    if (intent.generation != m_generation || !selected || intent.market != selected->id ||
        m_read_failed || m_uncertain || !m_security_warning.isEmpty()) return;
    if (intent.operation == Operation::Vault && (intent.effect.isEmpty() || intent.effect != m_effect->currentData(Qt::UserRole + 1).toString())) {
        notice(tr("The selected settlement changed. Nothing was submitted; select it again.")); return;
    }
    QPushButton* button{nullptr};
    switch (intent.operation) {
    case Operation::Order: button = m_order; break;
    case Operation::Cancel: button = m_cancel_order; break;
    case Operation::Deposit: button = m_deposit; break;
    case Operation::Withdraw: button = m_withdraw; break;
    case Operation::Admit: button = m_admit; break;
    case Operation::Checkpoint: button = m_checkpoint; break;
    case Operation::Vault: button = m_publish; break;
    }
    // Re-evaluate freshness/readiness after the read, never approve on the
    // authority of the stale frame in which the click occurred.
    if (!button || !button->isEnabled()) { notice(tr("Market status changed. Nothing was submitted; review again when ready.")); return; }
    if (intent.funding) openFunding(intent.operation == Operation::Withdraw);
    else begin(intent.operation);
}

std::vector<UniValue> B3FlowMeshTradingPanel::ReadEffectsForRefresh(
    const B3FlowMeshTrading::RpcCall& rpc, const QString& market_id,
    B3FlowMeshMarketData::Snapshot& snapshot)
{
    if (snapshot.market != market_id) throw std::runtime_error{"Settlement data does not match the selected market."};
    // A service overlay is not lost certified data. Do not ask an explicitly
    // unreconciled service to discover live vault inputs, or retain old effects.
    if (snapshot.chain_reconciling) return {};
    UniValue filter{UniValue::VARR}; filter.push_back(market_id.toStdString());
    UniValue effects;
    try {
        effects = rpc("listflowmeshvaultoperations", filter);
    } catch (const UniValue& error) {
        const auto& code{error.find_value("code")};
        if (!code.isNum() || code.getInt<int>() != RPC_MISC_ERROR) throw;
        const auto original{std::current_exception()};
        try {
            // A tip may change between the two read-only queries. Confirm the
            // typed state once, from this same captured wallet RPC endpoint.
            // A later healthy read must NOT turn missing effects into success.
            UniValue params{UniValue::VARR}; params.push_back(market_id.toStdString());
            UniValue options{UniValue::VOBJ}; options.pushKV("limit", 100); options.pushKV("curve_limit", 128); params.push_back(options);
            auto fresh{B3FlowMeshMarketData::Parse(rpc("getflowmeshmarketdata", params))};
            if (fresh.unchanged || !fresh.chain_reconciling || fresh.market != snapshot.market ||
                fresh.base != snapshot.base || fresh.domain != snapshot.domain || fresh.config != snapshot.config ||
                fresh.account != snapshot.account) std::rethrow_exception(original);
            snapshot = std::move(fresh);
            return {};
        } catch (...) { std::rethrow_exception(original); }
    }
    if (!effects.isArray() || effects.size() > 1000) throw std::runtime_error{"Invalid or oversized connected-effect list."};
    std::vector<UniValue> out;
    for (const auto& e : effects.getValues()) {
        if (e.isObject() && e.find_value("market_id").isStr() && e.find_value("market_id").get_str() == market_id.toStdString() &&
            e.find_value("account_id").isStr() && e.find_value("account_id").get_str() == snapshot.account.toStdString()) out.push_back(e);
    }
    return out;
}

std::optional<B3FlowMeshTradingPanel::StatusRead> B3FlowMeshTradingPanel::selectedStatusRead() const
{
    if (!m_wallet || !m_backend || !m_saved_actions_ready || m_cancel->load()) return std::nullopt;
    const QString selected{m_saved_selector->currentData(Qt::UserRole + 1).toString()};
    const auto row{std::find_if(m_saved_actions.actions.begin(), m_saved_actions.actions.end(), [&](const auto& a) {
        return a.market + QLatin1Char(':') + a.receipt.action_id == selected;
    })};
    if (row == m_saved_actions.actions.end()) return std::nullopt;
    const auto visible{market()};
    StatusRead scope{m_wallet, m_generation, &m_wallet->node(), row->domain, row->config,
                     row->market, row->account, row->receipt.action_id, visible ? visible->id : QString{}};
    return statusReadValid(scope, false) ? std::optional{scope} : std::nullopt;
}

bool B3FlowMeshTradingPanel::statusReadValid(const StatusRead& scope, bool selected) const
{
    if (!m_wallet || !m_backend || m_cancel->load() || !m_saved_actions_ready ||
        scope.wallet != m_wallet || scope.generation != m_generation || scope.node != &m_wallet->node()) return false;
    const auto row{std::find_if(m_saved_actions.actions.begin(), m_saved_actions.actions.end(), [&](const auto& a) {
        return a.domain == scope.domain && a.config == scope.config && a.market == scope.market &&
            a.account == scope.account && a.receipt.action_id == scope.action_id;
    })};
    if (row == m_saved_actions.actions.end() || scope.account != m_saved_actions.account) return false;
    const auto known{std::find_if(m_market_data.begin(), m_market_data.end(), [&](const auto& m) { return m.id == scope.market; })};
    if (known != m_market_data.end() && (known->domain != scope.domain || known->config != scope.config ||
        (known->has_account && known->account != scope.account))) return false;
    if (!selected) return true; // A late result may update only its original saved row.
    const auto visible{market()};
    return scope.selected_market == (visible ? visible->id : QString{}) &&
        m_saved_selector->currentData(Qt::UserRole + 1).toString() == scope.market + QLatin1Char(':') + scope.action_id;
}

void B3FlowMeshTradingPanel::requestStatusRead()
{
    if (m_busy || !m_security_warning.isEmpty()) return;
    const auto scope{selectedStatusRead()};
    if (!scope) return;
    // Repeated clicks for an already-running explicit read coalesce into that
    // read. Otherwise one non-owning slot waits behind the existing worker.
    if (m_active_result && m_active_result->receipt_only && m_active_result->receipt_scope == scope) {
        updateStatusReadState(); return;
    }
    m_deferred_status = scope;
    updateStatusReadState(); resumeStatusRead();
}

void B3FlowMeshTradingPanel::resumeStatusRead()
{
    if (!m_deferred_status || m_thread || m_busy) return;
    const auto scope{*m_deferred_status}; m_deferred_status.reset();
    if (m_security_warning.isEmpty() && statusReadValid(scope, true))
        startJob(std::nullopt, std::nullopt, false, true, scope);
    updateStatusReadState();
}

void B3FlowMeshTradingPanel::updateStatusReadState()
{
    QString text{tr("Status checks are read-only; no action is resent.")};
    if (m_deferred_status && statusReadValid(*m_deferred_status, true))
        text = tr("Status check queued after the current read; original request retained.");
    else if (m_active_result && m_active_result->receipt_only && m_active_result->receipt_scope &&
             statusReadValid(*m_active_result->receipt_scope, true))
        text = tr("Checking this saved request; original instruction and protections retained.");
    if (m_status_read_state->text() != text) m_status_read_state->setText(text);
}

void B3FlowMeshTradingPanel::startJob(std::optional<Action> action, std::optional<B3AssetTransfer::Prepared> prepared, bool exact_retry, bool receipt_only,
                                    std::optional<StatusRead> status_read, const QString& connect_url)
{
    if (!m_wallet || !m_backend || m_thread) return;
    if (receipt_only) {
        if (!status_read) status_read = selectedStatusRead();
        if (!status_read || !statusReadValid(*status_read, true)) return;
    }
    if (action && !m_saved_actions_ready) return;
    const bool tracked{m_receipt && receiptWalletSelected()};
    if ((exact_retry || receipt_only) && !tracked) return;
    if (exact_retry && (m_receipt->Included() || m_receipt->no_resubmit)) return;
    const bool watch_queue{action && action->operation != Operation::Checkpoint && action->operation != Operation::Vault &&
        m_snapshot && m_snapshot->market == action->market.id && m_snapshot->pending_actions > 0 && m_queue_age.isValid()};
    const QElapsedTimer queue_watch{m_queue_age};
    // Recheck after any modal review/unlock/fee dialog, including the second
    // exact-transaction review. A rejected start must restore acquired unlock.
    if (watch_queue && queue_watch.elapsed() >= 30'000) {
        restoreLock(); m_busy = false; notice(tr("Queued requests have not certified for 30 seconds. No new request was submitted. Refresh and inspect the market; existing requests are not canceled.")); updateControls(); return;
    }
    m_busy = action.has_value() || exact_retry; m_loading = !action && !m_snapshot; m_cancel->store(false); m_attempt_age.restart(); updateControls(); m_chart->setLoading(m_loading);
    auto result{std::make_shared<Result>()}; result->action = action; result->prepared = prepared; result->broadcast = prepared.has_value(); result->wallet = m_wallet_name;
    result->connect_url = connect_url;
    result->exact_retry = exact_retry; result->receipt_only = receipt_only;
    result->receipt_scope = receipt_only ? status_read : !action && !exact_retry ? selectedStatusRead() : std::nullopt;
    m_active_result = result;
    auto* node{&m_wallet->node()}; const auto backend{m_backend}; const auto cancel{m_cancel};
    const auto uri{B3AssetTransfer::WalletUri(m_wallet->getWalletName())}; const auto generation{m_generation};
    const auto selected{market()}; const QString selected_id{selected ? selected->id : QString{}};
    const bool refresh_catalog{!action && (!m_catalog_age.isValid() || m_catalog_age.elapsed() >= 5000 || m_route_pending || m_market_data.empty() || (m_snapshot && m_snapshot->chain_reconciling))};
    result->markets = m_market_data; result->effects = m_effect_data;
    const QString known_head{m_snapshot && m_snapshot->market == selected_id ? m_snapshot->head : QString{}};
    const QString route_base{m_route_pending ? m_requested_base : QString{}};
    const QString receipt_market{status_read ? status_read->market : tracked ? m_pending_market : QString{}},
        receipt_id{status_read ? status_read->action_id : tracked ? m_receipt->action_id : QString{}};
    result->receipt_market = receipt_market; result->receipt_action_id = receipt_id;
    result->receipt_account = status_read ? status_read->account : tracked ? m_pending_account : QString{};
    m_thread = QThread::create([this, generation, node, backend, cancel, uri, result, selected_id, known_head, route_base, refresh_catalog, watch_queue, queue_watch, receipt_market, receipt_id] {
        const auto read_client_info = [&] {
            if (cancel->load() || node->shutdownRequested()) return;
            try {
                auto info{node->executeRpc("getflowmeshclientinfo", UniValue{UniValue::VARR}, uri)};
                if (!info.isObject()) throw std::runtime_error{"Trading connection status is unavailable."};
                result->client_info = std::move(info); result->client_error.clear();
            } catch (const UniValue& error) { result->client_error = RpcError(error); }
            catch (const std::exception& error) { result->client_error = QString::fromUtf8(error.what()).left(500); }
            catch (...) { result->client_error = QStringLiteral("Trading connection status is unavailable."); }
        };
        try {
            const auto cancelled = [&] { return cancel->load() || node->shutdownRequested(); };
            const B3FlowMeshTrading::RpcCall rpc = [&](const std::string& method, const UniValue& params) {
                if (cancelled()) throw std::runtime_error{"Operation cancelled."};
                if (!NoSpendingRequired(method)) {
                    // Readiness RPCs may themselves outlast the local deadline.
                    // Do not access UI timers from this worker; this captured
                    // monotonic observation only fails closed until refresh.
                    if (watch_queue && queue_watch.elapsed() >= 30'000) throw std::runtime_error{"Queued requests have not certified for 30 seconds. Refresh before submitting; no new mutation was started."};
                    if (method != "submitflowmeshdeposit" && method != "retryflowmeshaction" && (backend->isLocked() || backend->privateKeysDisabled())) throw std::runtime_error{"The captured wallet is locked for spending."};
                    if (method == "sendrawtransaction" || method == "submitflowmeshorder" || method == "cancelflowmeshorder" || method == "requestflowmeshwithdrawal" || method == "submitflowmeshdeposit" || method == "retryflowmeshaction") result->write_attempted = true;
                }
                return node->executeRpc(method, params, method == "getblockchaininfo" || method == "testmempoolaccept" ? "" : uri);
            };
            const auto read_receipt = [&](const UniValue& response) {
                auto receipt{B3FlowMeshTrading::ParseReceipt(response, receipt_market, receipt_id)};
                // Older status responses omit account_id. Bind them to the
                // captured request, never a newly selected row on completion.
                if (receipt.account.isEmpty()) receipt.account = result->receipt_account;
                else if (!result->receipt_account.isEmpty() && receipt.account != result->receipt_account)
                    throw std::runtime_error{"Status response belongs to another captured wallet account."};
                return receipt;
            };
            if (!result->connect_url.isEmpty()) {
                // This RPC persists endpoint selection and probes discovery;
                // it neither needs spending unlock nor touches signed actions.
                UniValue params{UniValue::VARR}; params.push_back(result->connect_url.toStdString());
                auto info{rpc("flowmeshclientconnect", params)};
                if (!info.isObject()) throw std::runtime_error{"Trading connection status is unavailable."};
                result->client_info = std::move(info); return;
            }
            if (result->exact_retry || result->receipt_only) {
                const auto request{B3FlowMeshTrading::ReceiptParameters(receipt_market, receipt_id, result->exact_retry)};
                result->receipt = read_receipt(rpc(request.method, request.params));
                return;
            }
            if (!result->action) {
                read_client_info();
                // Restore public cards before attempting remote reads. The
                // panel owns and drains this worker before destruction; the
                // queued callback is context-bound and generation guarded.
                const auto saved{B3FlowMeshTrading::ParseSavedActions(rpc("listflowmeshactions", UniValue{UniValue::VARR}))};
                QMetaObject::invokeMethod(this, [this, generation, saved] {
                    if (generation == m_generation && m_wallet && !m_cancel->load()) restoreSavedActions(saved);
                }, Qt::QueuedConnection);
                if (refresh_catalog) {
                    result->markets = B3FlowMeshTrading::ParseMarkets(rpc("listflowmeshmarkets", UniValue{UniValue::VARR}));
                    result->catalog = true; // Successful discovery survives a later selected-market failure.
                }
                auto selected{std::find_if(result->markets.begin(), result->markets.end(), [&](const auto& m) { return route_base.isEmpty() ? m.id == selected_id : m.base == route_base; })};
                if (selected == result->markets.end() && selected_id.isEmpty() && route_base.isEmpty()) selected = result->markets.begin();
                if (selected != result->markets.end()) {
                    result->read_market = selected->id;
                    if (result->catalog && selected->remote) {
                        UniValue params{UniValue::VARR}; params.push_back(selected->id.toStdString());
                        const auto status{B3FlowMeshTrading::ParseMarket(rpc("getflowmeshbalance", params))};
                        if (status.id != selected->id || status.base != selected->base || status.vault != selected->vault || status.domain != selected->domain || status.config != selected->config || !status.remote) throw std::runtime_error{"Selected remote status changed its pinned market identity."};
                        *selected = status;
                    }
                    UniValue params{UniValue::VARR}; params.push_back(selected->id.toStdString()); UniValue options{UniValue::VOBJ}; options.pushKV("limit", 100); options.pushKV("curve_limit", 128);
                    if (!known_head.isEmpty() && selected->id == selected_id && !result->catalog) options.pushKV("known_head", known_head.toStdString());
                    params.push_back(options); result->snapshot = B3FlowMeshMarketData::Parse(rpc("getflowmeshmarketdata", params));
                    const auto& s{*result->snapshot};
                    if (s.market != selected->id || s.base != selected->base || s.domain != selected->domain || s.config != selected->config) throw std::runtime_error{"Certified data does not match the selected market identity."};
                    if (!s.unchanged && selected->has_account && selected->account != s.account) throw std::runtime_error{"Certified data belongs to another wallet account."};
                    // The local backend independently verifies remote connected
                    // settlement proofs. They are separate from whole-state data.
                    if (result->catalog && !s.account.isEmpty()) result->effects = ReadEffectsForRefresh(rpc, selected->id, *result->snapshot);
                    if (s.chain_reconciling) result->effects.clear();
                    selected->publish_ready = (s.remote ? s.certificate_verified && s.account_state_verified : selected->publish_ready) && s.running && !s.chain_reconciling;
                    selected->remote = s.remote;
                    selected->ready = s.remote ? B3FlowMeshMarketData::AdmissionReady(s, 0, -1) : selected->publish_ready && !s.paused && !s.handoff && s.halt == QStringLiteral("none") && s.error.isEmpty();
                    if (!s.unchanged) {
                        if (selected->has_account && selected->account != s.account) throw std::runtime_error{"Certified data belongs to another wallet account."};
                        selected->has_account = !s.account.isEmpty(); selected->account = s.account; selected->sequence = s.account_sequence;
                        selected->base_available = s.base_available; selected->base_reserved = s.base_reserved; selected->b3_available = s.b3_available; selected->b3_reserved = s.b3_reserved;
                    }
                }
                if (!receipt_id.isEmpty()) {
                    try {
                        const auto request{B3FlowMeshTrading::ReceiptParameters(receipt_market, receipt_id)};
                        result->receipt = read_receipt(rpc(request.method, request.params));
                    } catch (const UniValue& error) { result->receipt_error = RpcError(error); }
                    catch (const std::exception& error) { result->receipt_error = QString::fromUtf8(error.what()).left(500); }
                }
                read_client_info(); // Include observations from this market read.
                return;
            }
            auto& a{*result->action};
            B3FlowMeshTrading::CheckSynced(rpc("getblockchaininfo", UniValue{UniValue::VARR}));
            if (result->broadcast) {
                const auto fresh{B3FlowMeshTrading::ReadMarket(a.market.id, rpc)};
                B3FlowMeshTrading::CheckSameMarket(a.market, fresh, false, a.operation == Operation::Checkpoint || a.operation == Operation::Vault);
                B3AssetTransfer::CheckAcceptance(rpc("testmempoolaccept", B3AssetTransfer::AcceptanceParameters(*result->prepared)), *result->prepared);
                result->response = rpc("sendrawtransaction", B3AssetTransfer::BroadcastParameters(*result->prepared));
                if (!result->response.isStr() || result->response.get_str() != result->prepared->txid.toStdString()) throw std::runtime_error{"The node did not confirm the reviewed transaction ID."};
            } else {
                const auto response{B3FlowMeshTrading::DispatchApproved(a, true, cancelled, rpc)};
                if (PreparedOperation(a.operation)) {
                    // A first deposit may create the account. Verify it through
                    // the captured wallet endpoint, never through response labels alone.
                    const auto fresh{B3FlowMeshTrading::ReadMarket(a.market.id, rpc)};
                    B3FlowMeshTrading::CheckSameMarket(a.market, fresh, false, a.operation == Operation::Checkpoint || a.operation == Operation::Vault);
                    if (a.operation == Operation::Deposit) a.market = fresh;
                    result->prepared = B3FlowMeshTrading::ParsePrepared(response, a, [backend](const auto& destination) { return backend->isSpendable(destination); });
                    B3AssetTransfer::CheckAcceptance(rpc("testmempoolaccept", B3AssetTransfer::AcceptanceParameters(*result->prepared)), *result->prepared);
                } else { B3FlowMeshTrading::CheckActionResult(response, a); result->response = response; result->receipt = B3FlowMeshTrading::ParseReceipt(response, a.market.id); }
            }
        } catch (const UniValue& error) { result->error = RpcError(error); }
        catch (const std::exception& error) { result->error = QString::fromUtf8(error.what()).left(500); }
        catch (...) { result->error = QStringLiteral("The FlowMesh operation failed."); }
        if (!result->action && !result->exact_retry && !result->receipt_only) read_client_info();
    });
    m_thread->setParent(this); connect(m_thread, &QThread::finished, this, [this, result, generation] { if (generation == m_generation) finishJob(result); }); m_thread->start(); updateControls();
}

void B3FlowMeshTradingPanel::finishJob(const std::shared_ptr<Result>& result)
{
    stopWorker(); m_active_result.reset();
    if (result->receipt_scope && !statusReadValid(*result->receipt_scope, false)) {
        result->receipt.reset(); result->receipt_error.clear();
        result->receipt_market.clear(); result->receipt_account.clear(); result->receipt_action_id.clear();
        if (result->receipt_only) result->error = tr("Captured status scope changed; the late result was not applied.");
    }
    // Applying an economic result can enter an existing modal review, whose
    // event loop may destroy the panel. Never drain a read through that owner.
    const QPointer<B3FlowMeshTradingPanel> self{this};
    applyJobResult(result);
    if (!self) return;
    // Drain on both successful and failed reads, including while hidden. The
    // existing worker must be destroyed before another read can be started.
    resumeConnect(); resumeStatusRead();
    updateStatusReadState();
}

void B3FlowMeshTradingPanel::applyJobResult(const std::shared_ptr<Result>& result)
{
    if (!m_wallet || m_cancel->load()) { m_deferred_review.reset(); restoreLock(); m_busy = false; updateControls(); return; }
    if (result->client_info) {
        m_client_info = result->client_info;
        const auto& selected{m_client_info->find_value("selected_endpoint")};
        if (selected.isStr() && m_endpoint->text().isEmpty() && !m_endpoint->isModified() && !m_endpoint->hasFocus())
            m_endpoint->setText(QString::fromStdString(selected.get_str()));
        m_connection_error.clear();
    }
    if (!result->client_error.isEmpty()) m_connection_error = result->client_error;
    if (!result->connect_url.isEmpty()) {
        m_connect_error = result->error;
        // Keep the old display, but require a fresh certificate/account read
        // after endpoint selection before enabling any economic action.
        m_catalog_age.invalidate(); m_response_age.invalidate(); m_attempt_age.invalidate(); m_read_failures = 0;
        m_read_failed = true; m_read_error = tr("Refreshing selected market after the connection check.");
        m_loading = false; m_busy = false; updateMarketText(); refresh(); return;
    }
    if (result->exact_retry || result->receipt_only) {
        m_busy = false;
        if (result->receipt) { applyReceipt(*result->receipt); notice(B3FlowMeshTrading::DescribeReceipt(*result->receipt)); }
        else {
            applyReceiptError(*result, result->error);
            if (result->write_attempted) markUncertain(result);
            notice((result->receipt_only ? tr("Status unavailable: %1. The original request and its protections are retained; no action was resent.")
                : tr("Exact-action retry did not return a verified status: %1. No new request was created or signed; the saved action remains unresolved.")).arg(result->error));
        }
        updateReceiptCard(); updateMarketText();
        if (m_deferred_review) resumeReview();
        else if (!result->receipt_only) refresh();
        return;
    }
    if (!result->error.isEmpty()) {
        m_deferred_review.reset();
        if (!result->action) {
            if (result->catalog) applyMarketCatalog(*result);
            const auto selected{market()};
            if (!result->read_market.isEmpty() && (!selected || result->read_market != selected->id)) {
                // Selection can change during a passive read. Keep discovery,
                // but never attach that old market's error to the new choice.
                m_loading = false; m_busy = false; updateDataViews(); updateMarketText(); refresh(); return;
            }
            if (!m_read_failed) notice(tr("Market refresh failed: %1. Last certified data is retained; new actions are disabled until a successful refresh.").arg(result->error));
            m_read_failed = true; m_read_error = result->error; m_read_failures = std::min(5U, m_read_failures + 1); m_loading = false; m_uncertain_refreshed = false; m_busy = false;
            updateDataViews(); updateMarketText(); return;
        }
        restoreLock(); m_busy = false;
        if (result->write_attempted) markUncertain(result);
        notice(tr("Wallet: %1\n%2\n%3").arg(result->wallet, result->error, result->write_attempted
            ? tr("Submission outcome may be unknown. Refresh this captured wallet, inspect its state, then use Review uncertain submission to acknowledge it. No retry occurred.")
            : tr("No order, deposit admission, withdrawal request or transaction was submitted. Preparation may have created a wallet account/address; back up the wallet.")));
        if (result->prepared) notice(tr("Reviewed transaction ID: %1").arg(result->prepared->txid));
        if (!result->action) { m_uncertain_refreshed = false; m_market_data.clear(); QSignalBlocker block{m_market}; m_market->clear(); m_effect->clear(); }
        updateMarketText(); return;
    }
    if (!result->action) {
        m_loading = false; m_read_failed = false; m_read_failures = 0; m_read_error.clear();
        applyReceiptError(*result, result->receipt_error);
        if (result->receipt) applyReceipt(*result->receipt);
        if (m_uncertain && (uncertainWalletSelected() || !m_uncertain_wallet)) m_uncertain_refreshed = true;
        const QString previous{market() ? market()->id : QString{}};
        const QString previous_effect{m_effect->currentData(Qt::UserRole + 1).toString()};
        applyMarketCatalog(*result); m_effect_data = result->effects;
        { std::vector<Choice> choices; const auto selected{market()};
            for (size_t i{0}; selected && i < m_effect_data.size(); ++i) {
                const auto& e{m_effect_data[i]}; if (e.find_value("market_id").get_str() != selected->id.toStdString()) continue;
                const auto& kind{e.find_value("kind")}; const auto& id{e.find_value("effect_id")};
                if (kind.isStr() && id.isStr()) choices.push_back({QString::fromStdString(kind.get_str()) + QStringLiteral(" — ") + QString::fromStdString(id.get_str()).left(20), static_cast<int>(i), QString::fromStdString(id.get_str())});
            }
            SetChoices(m_effect, choices, previous_effect, true);
        }
        const auto selected_now{market()};
        if (result->snapshot && selected_now && result->snapshot->market == selected_now->id) {
            auto fresh{*result->snapshot};
            const bool new_head{!m_snapshot || m_snapshot->market != fresh.market || m_snapshot->head != fresh.head};
            if (fresh.unchanged) {
                if (!m_snapshot || m_snapshot->market != fresh.market || m_snapshot->head != fresh.head || m_snapshot->state_root != fresh.state_root || m_snapshot->remote != fresh.remote || m_snapshot->execution_result_verified != fresh.execution_result_verified) { m_read_failed = true; notice(tr("Unchanged snapshot did not match the retained certificate and provenance; data was not reused.")); m_catalog_age.invalidate(); }
                else {
                    auto retained{*m_snapshot}; retained.running = fresh.running; retained.paused = fresh.paused; retained.chain_reconciling = fresh.chain_reconciling; retained.handoff = fresh.handoff; retained.observer = fresh.observer; retained.halt = fresh.halt; retained.error = fresh.error; retained.pending_actions = fresh.pending_actions; retained.active_seats = fresh.active_seats; retained.quorum_required = fresh.quorum_required;
                    retained.remote = fresh.remote; retained.endpoint = fresh.endpoint; retained.certificate_verified = fresh.certificate_verified; retained.account_state_verified = fresh.account_state_verified; retained.execution_result_verified = fresh.execution_result_verified; retained.b3_checkpoint_confirmed = fresh.b3_checkpoint_confirmed; retained.event_gap = fresh.event_gap;
                    // Cosmetic discovery can advance without a new certified
                    // microblock. Keep the exact account/book and user inputs;
                    // only accept units for this same canonical asset.
                    if (fresh.units.known && fresh.units.asset == retained.base) retained.units = fresh.units;
                    m_snapshot = std::move(retained); m_response_age.restart(); updateDataViews();
                }
            } else {
                if (!m_snapshot || m_snapshot->head != fresh.head) m_certificate_age.restart();
                m_snapshot = std::move(fresh); m_response_age.restart(); updateDataViews();
            }
            if (!m_read_failed && m_snapshot) {
                if (m_snapshot->pending_actions == 0) m_queue_age.invalidate();
                else if (new_head || !m_queue_age.isValid()) m_queue_age.restart();
            }
        } else if (!selected_now || !m_snapshot || selected_now->id != m_snapshot->market) { m_snapshot.reset(); m_response_age.invalidate(); m_queue_age.invalidate(); updateDataViews(); }
        const bool changed_selection{selected_now && (selected_now->id != previous || (!result->read_market.isEmpty() && selected_now->id != result->read_market))};
        const bool open_routed_funding{m_route_pending && selected_now && m_snapshot && m_snapshot->market == selected_now->id};
        if (open_routed_funding) { m_asset->setCurrentIndex(m_requested_base.isEmpty() ? 1 : 0); m_route_pending = false; }
        m_busy = false; updateMarketText();
        if (m_deferred_review) resumeReview();
        else if (open_routed_funding) openFunding(m_requested_withdrawal);
        else if (changed_selection) refresh(); // Fill the newly selected account/effects immediately.
        return;
    }
    const auto& a{*result->action};
    if (result->prepared && !result->broadcast) {
        const QPointer<B3FlowMeshTradingPanel> self{this}; const auto generation{m_generation};
        const bool accepted{confirm(tr("Wallet: %1\n\n%2\n\nActual B3 network fee: %3 B3\nExact transaction ID: %4\n\nSubmit these exact signed bytes ONCE?")
            .arg(result->wallet, B3FlowMeshTrading::Describe(a), QString::fromStdString(FormatMoney(result->prepared->fee)), result->prepared->txid), true)};
        if (!self || generation != m_generation) return;
        if (!accepted) { restoreLock(); m_busy = false; notice(tr("Prepared transaction cancelled; nothing was broadcast. Back up any account/address created during preparation.")); updateControls(); return; }
        startJob(a, result->prepared); return;
    }
    restoreLock(); m_busy = false;
    if (result->broadcast) {
        notice(tr("Wallet: %1\nTransaction submitted: %2\nWait for on-chain confirmation; this is not a guarantee of market settlement or payout finality.").arg(result->wallet, result->prepared->txid));
        if (a.operation == Operation::Deposit) { m_deposit_txid->setText(result->prepared->txid); m_deposit_vout->setText(QStringLiteral("0")); }
    } else {
        m_pending_market = a.market.id; m_pending_account = a.market.account; m_receipt_wallet = m_wallet;
        if (SignedAction(a.operation)) m_pending_sequence = a.market.sequence;
        if (result->receipt) {
            m_receipt.reset(); // The explicitly reviewed new instruction, not a late status read.
            applyReceipt(*result->receipt);
            notice(tr("Wallet: %1\n%2\nNo automatic retry. Check this exact action with getflowmeshactionstatus %3 %4; retryflowmeshaction resends only retained bytes, never signs a replacement.")
                .arg(result->wallet, B3FlowMeshTrading::DescribeReceipt(*result->receipt), a.market.id, result->receipt->action_id));
        }
    }
    updateMarketText(); refresh();
}

void B3FlowMeshTradingPanel::applyMarketCatalog(const Result& result)
{
    const QString previous{market() ? market()->id : QString{}};
    auto markets{result.markets};
    if (!result.error.isEmpty()) {
        // Discovery rows are not certified balances. Preserve cached account
        // displays only for exactly the same pins; the failed-read gate still
        // disables every economic action until a successful selected read.
        for (auto& market : markets) {
            const auto retained{std::find_if(m_market_data.begin(), m_market_data.end(), [&](const auto& old) {
                return old.id == market.id && old.base == market.base && old.vault == market.vault &&
                    old.domain == market.domain && old.config == market.config && old.remote == market.remote;
            })};
            if (retained != m_market_data.end()) market = *retained;
        }
    }
    m_market_data = std::move(markets);
    std::vector<Choice> choices; QString selection{previous}; const bool route_asset{m_route_pending && !m_requested_base.isEmpty()};
    if (route_asset) selection.clear();
    for (const auto& market : m_market_data) {
        const auto* units{result.error.isEmpty() && result.snapshot && result.snapshot->market == market.id && result.snapshot->units.known ? &result.snapshot->units : m_snapshot && m_snapshot->market == market.id && m_snapshot->units.known ? &m_snapshot->units : nullptr};
        const QString token{units ? units->ticker : market.base.left(12) + QStringLiteral("…")};
        choices.push_back({(inverted() ? QStringLiteral("B3 / %1") : QStringLiteral("%1 / B3")).arg(token), market.id, market.id});
        if (route_asset && market.base == m_requested_base) selection = market.id;
    }
    SetChoices(m_market, choices, selection, !route_asset);
    if (route_asset && selection.isEmpty()) notice(tr("No established market was reported for the selected asset. This panel does not bootstrap one."));
    if (result.catalog) m_catalog_age.restart();
    const auto selected{market()};
    if (m_snapshot && (!selected || selected->id != m_snapshot->market || selected->base != m_snapshot->base ||
        selected->domain != m_snapshot->domain || selected->config != m_snapshot->config || selected->remote != m_snapshot->remote)) {
        m_snapshot.reset(); m_response_age.invalidate(); m_certificate_age.invalidate(); m_queue_age.invalidate();
    }
}

void B3FlowMeshTradingPanel::stopWorker()
{
    if (!m_thread) return; disconnect(m_thread, nullptr, this, nullptr); m_thread->wait(); delete m_thread; m_thread = nullptr;
}
void B3FlowMeshTradingPanel::cancelAndWait()
{
    m_timer->stop(); m_cancel->store(true); m_deferred_review.reset(); m_deferred_status.reset(); m_deferred_connect.reset(); ++m_generation;
    m_busy = true; updateControls();
    if (m_wallet) disconnect(m_wallet, nullptr, this, nullptr);
    if (m_confirmation) m_confirmation->reject();
    if (m_funding_dialog) m_funding_dialog->reject();
    stopWorker();
    // Draining a running write does not prove it was cancelled. Preserve its
    // public identity even if a wallet switch disconnects the completion slot.
    if (m_active_result && m_active_result->write_attempted) {
        markUncertain(m_active_result);
        // A completed, verified receipt remains certified even when its GUI
        // completion callback was still queued when teardown began.
        if (m_active_result->receipt && m_active_result->receipt->Included()) applyReceipt(*m_active_result->receipt);
        notice(tr("Wallet changed or operation closed after submission began. Inspect this saved request; it was not cancelled or retried.\n%1").arg(m_uncertain_details));
    }
    m_active_result.reset(); restoreLock();
    // Worker captures are gone and relocking has been attempted. Retain any
    // security warning, not a strong wallet owner that prevents shutdown.
    // The durable client outbox and public action/uncertainty records are not
    // changed here. Stopping local work is never a FlowMesh order cancellation.
    m_relock_backend.reset(); m_backend.reset(); m_wallet.clear();
    m_loading = false; m_busy = false; updateControls();
}
void B3FlowMeshTradingPanel::markUncertain(const std::shared_ptr<Result>& result)
{
    m_uncertain = true; m_uncertain_refreshed = false; m_uncertain_wallet = m_wallet;
    m_uncertain_action_id = result->receipt ? result->receipt->action_id : result->exact_retry && m_receipt ? m_receipt->action_id : QString{};
    m_uncertain_market = result->action ? result->action->market.id : result->exact_retry ? m_pending_market : QString{};
    m_uncertain_account = result->action ? result->action->market.account : result->exact_retry ? m_pending_account : QString{};
    m_uncertain_details = tr("Captured wallet: %1").arg(result->wallet);
    if (result->action) m_uncertain_details += QStringLiteral("\n") + B3FlowMeshTrading::Describe(*result->action);
    if (result->prepared) m_uncertain_details += tr("\nExact transaction ID: %1").arg(result->prepared->txid);
    if (!m_uncertain_action_id.isEmpty()) m_uncertain_details += tr("\nExact action ID: %1").arg(m_uncertain_action_id);
    if (result->action && result->receipt) {
        m_receipt = result->receipt; m_receipt_wallet = m_wallet;
        m_pending_market = result->action->market.id; m_pending_account = result->action->market.account;
        if (SignedAction(result->action->operation)) m_pending_sequence = result->action->market.sequence;
    }
}

void B3FlowMeshTradingPanel::applyReceipt(const B3FlowMeshTrading::Receipt& receipt)
{
    auto received{receipt};
    if (received.market.isEmpty()) received.market = m_pending_market;
    if (received.account.isEmpty()) received.account = m_pending_account;
    // An earlier read can finish after the local outbox refresh selects a
    // different saved request. Update only its originating public row; never
    // reinterpret that result as the currently selected instruction.
    if (received.market != m_pending_market || received.account != m_pending_account ||
        (m_receipt && receiptWalletSelected() && received.action_id != m_receipt->action_id)) {
        for (auto& saved : m_saved_actions.actions) {
            if (saved.market == received.market && saved.account == received.account && saved.receipt.action_id == received.action_id) {
                const bool protected_before{saved.receipt.no_resubmit};
                saved.receipt = received; saved.receipt.no_resubmit |= protected_before || received.Included();
            }
        }
        updateReceiptCard(); return;
    }
    // ActionId is semantic and does not include market/domain/config. Keep
    // replay protection and uncertainty attached to the full local scope.
    const bool protected_before{m_receipt && m_receipt->action_id == received.action_id && m_receipt->market == received.market &&
        m_receipt->account == received.account && receiptWalletSelected() && m_receipt->no_resubmit};
    m_receipt = received; m_receipt->no_resubmit |= protected_before || receipt.Included(); m_receipt_error.clear();
    const bool same_uncertain{m_uncertain_action_id == received.action_id && m_uncertain_market == received.market && m_uncertain_account == received.account && uncertainWalletSelected()};
    if (m_receipt->Included() || m_receipt->no_resubmit) {
        m_pending_sequence.reset();
        // An old action's proof cannot resolve an unrelated transaction whose
        // submission outcome became unknown later in the same wallet.
        if (same_uncertain) {
            m_uncertain = false; m_uncertain_refreshed = false; m_uncertain_wallet.clear(); m_uncertain_action_id.clear(); m_uncertain_market.clear(); m_uncertain_account.clear();
        }
    } else if (!m_receipt->no_resubmit && (receipt.state == QStringLiteral("unknown") || receipt.state == QStringLiteral("rejected"))) {
        if (!m_uncertain || same_uncertain) {
            m_uncertain = true; m_uncertain_refreshed = false; m_uncertain_wallet = m_wallet; m_uncertain_action_id = receipt.action_id;
            m_uncertain_market = received.market; m_uncertain_account = received.account;
            m_uncertain_details = tr("Captured wallet: %1\nMarket: %2\nAccount: %3\n%4").arg(m_wallet_name, received.market, received.account, B3FlowMeshTrading::DescribeReceipt(*m_receipt));
        }
    }
    for (auto& saved : m_saved_actions.actions) {
        if (saved.market == received.market && saved.receipt.action_id == received.action_id && saved.account == received.account && receiptWalletSelected()) {
            const bool no_resubmit{saved.receipt.no_resubmit}; saved.receipt = *m_receipt; saved.receipt.no_resubmit |= no_resubmit;
        }
    }
    updateReceiptCard();
}

void B3FlowMeshTradingPanel::applyReceiptError(const Result& result, const QString& error)
{
    if (receiptWalletSelected() && m_receipt && result.receipt_market == m_receipt->market &&
        result.receipt_account == m_receipt->account && result.receipt_action_id == m_receipt->action_id) {
        m_receipt_error = error; updateReceiptCard();
    }
}

void B3FlowMeshTradingPanel::restoreSavedActions(const B3FlowMeshTrading::SavedActions& saved)
{
    if (!m_wallet || m_cancel->load()) return;
    const auto fail = [this](const QString& reason) {
        m_saved_actions_ready = false; m_receipt_card->setText(reason + tr(" Original requests remain retained; new actions are disabled.")); updateControls();
    };
    if ((m_snapshot && !m_snapshot->account.isEmpty() && !saved.account.isEmpty() && m_snapshot->account != saved.account) ||
        (m_saved_actions_ready && !m_saved_actions.account.isEmpty() && m_saved_actions.account != saved.account)) {
        fail(tr("Saved requests do not match the selected wallet account.")); return;
    }
    for (const auto& action : saved.actions) {
        const auto market{std::find_if(m_market_data.begin(), m_market_data.end(), [&](const auto& value) { return value.id == action.market; })};
        if (market != m_market_data.end() && (market->domain != action.domain || market->config != action.config)) {
            fail(tr("Saved request configuration does not match this market.")); return;
        }
        const auto old{std::find_if(m_saved_actions.actions.begin(), m_saved_actions.actions.end(), [&](const auto& value) { return value.market == action.market && value.receipt.action_id == action.receipt.action_id; })};
        if (old != m_saved_actions.actions.end() && (old->domain != action.domain || old->config != action.config || old->account != action.account ||
            old->sequence != action.sequence || old->type != action.type || old->signed_bytes_sha256 != action.signed_bytes_sha256 ||
            old->signed_bytes_size != action.signed_bytes_size || old->initial_submission_ms != action.initial_submission_ms ||
            (old->receipt.no_resubmit && !action.receipt.no_resubmit) || (old->may_have_been_sent && !action.may_have_been_sent))) {
            fail(tr("Retained instruction identity or its durable protections changed unexpectedly.")); return;
        }
    }
    const QString previous{m_saved_selector->currentData(Qt::UserRole + 1).toString()};
    m_saved_actions = saved; m_saved_actions_ready = true;
    std::vector<Choice> choices;
    QString selection{previous};
    for (const auto& action : saved.actions) {
        const QString identity{action.market + QLatin1Char(':') + action.receipt.action_id};
        const QString status{action.receipt.no_resubmit ? tr("no resubmit") : action.receipt.state};
        choices.push_back({tr("%1 · %2 · %3").arg(action.sequence ? tr("Sequence %1").arg(*action.sequence) : tr("Deposit"), action.receipt.action_id.left(16), status), identity, identity});
    }
    if (std::none_of(choices.begin(), choices.end(), [&](const auto& choice) { return choice.identity == previous; })) {
        const auto pending{std::find_if(saved.actions.begin(), saved.actions.end(), [](const auto& a) { return !a.receipt.no_resubmit && !a.receipt.Included(); })};
        selection = pending != saved.actions.end() ? pending->market + QLatin1Char(':') + pending->receipt.action_id : QString{};
    }
    SetChoices(m_saved_selector, choices, selection, true);
    if (previous != m_saved_selector->currentData(Qt::UserRole + 1).toString()) selectSavedAction();
    else {
        // Another local status caller may have renewed the durable replay
        // prohibition. Reflect that guard immediately, even if this panel's
        // selected receipt has not yet obtained a fresh certificate.
        const auto selected{std::find_if(saved.actions.begin(), saved.actions.end(), [&](const auto& a) {
            return receiptWalletSelected() && m_receipt && a.market == m_pending_market && a.receipt.action_id == m_receipt->action_id;
        })};
        if (selected != saved.actions.end() && selected->receipt.no_resubmit && !m_receipt->no_resubmit) {
            auto protected_receipt{*m_receipt}; protected_receipt.no_resubmit = true; applyReceipt(protected_receipt);
        }
        updateReceiptCard();
    }
    updateControls();
}

void B3FlowMeshTradingPanel::selectSavedAction()
{
    if (!m_wallet || !m_saved_actions_ready || m_busy) return;
    if (m_deferred_status && !statusReadValid(*m_deferred_status, true)) m_deferred_status.reset();
    const QString selected{m_saved_selector->currentData(Qt::UserRole + 1).toString()};
    const auto action{std::find_if(m_saved_actions.actions.begin(), m_saved_actions.actions.end(), [&](const auto& a) { return a.market + QLatin1Char(':') + a.receipt.action_id == selected; })};
    if (action == m_saved_actions.actions.end()) { updateReceiptCard(); updateControls(); return; }
    if (m_receipt && (m_receipt->market != action->market || m_receipt->account != action->account || m_receipt->action_id != action->receipt.action_id))
        m_receipt.reset(); // Explicit selection; retained rows and uncertainty are untouched.
    m_pending_market = action->market; m_pending_account = action->account; m_pending_sequence = action->sequence;
    m_receipt_wallet = m_wallet; applyReceipt(action->receipt);
    updateReceiptCard(); updateMarketText();
}

void B3FlowMeshTradingPanel::updateReceiptCard()
{
    const QString selected{m_saved_selector->currentData(Qt::UserRole + 1).toString()};
    const auto action{std::find_if(m_saved_actions.actions.begin(), m_saved_actions.actions.end(), [&](const auto& a) { return a.market + QLatin1Char(':') + a.receipt.action_id == selected; })};
    if (action == m_saved_actions.actions.end()) {
        if (m_saved_actions_ready) m_receipt_card->setText(tr("No locally retained requests for this wallet. This is not a complete market history. Nothing was resent."));
        return;
    }
    auto shown{*action};
    if (receiptWalletSelected() && m_receipt && m_receipt->market == action->market && m_receipt->account == action->account && m_receipt->action_id == action->receipt.action_id) {
        shown.receipt = *m_receipt; shown.receipt.no_resubmit |= action->receipt.no_resubmit;
    }
    // Reuse precision only from this exact authenticated market, never from a
    // same-ticker asset or another selected receipt. Raw canonical economics
    // remain visible when that market is not selected or metadata is absent.
    const bool metadata_matches{m_snapshot && m_snapshot->market == shown.market && m_snapshot->domain == shown.domain &&
        m_snapshot->config == shown.config && m_snapshot->units.known && m_snapshot->units.asset == m_snapshot->base};
    QString text{tr("Wallet: %1\n%2").arg(m_wallet_name, B3FlowMeshTrading::DescribeSavedAction(shown,
        metadata_matches ? std::optional<int>{m_snapshot->units.decimals} : std::nullopt, metadata_matches ? m_snapshot->units.ticker : QString{}))};
    if (!m_receipt_error.isEmpty() && m_receipt && m_receipt->market == shown.market && m_receipt->account == shown.account && m_receipt->action_id == shown.receipt.action_id) text += tr("\nStatus unavailable: %1. Original instruction and protections retained.").arg(m_receipt_error);
    m_receipt_card->setText(text);
}

void B3FlowMeshTradingPanel::retryReceipt()
{
    if (!m_wallet || !m_backend || m_busy || m_thread || !m_receipt || m_receipt->Included() || m_receipt->no_resubmit ||
        !receiptWalletSelected() || !m_security_warning.isEmpty() ||
        m_read_failed || !m_response_age.isValid() || m_response_age.elapsed() > 3000) return;
    const auto action_id{m_receipt->action_id}; const auto saved_market{m_pending_market};
    const QPointer<B3FlowMeshTradingPanel> self{this}; const auto generation{m_generation};
    m_busy = true; updateControls();
    const bool approved{confirm(tr("Wallet: %1\nMarket: %2\nExact action ID: %3\n\nResend the already retained signed bytes? This does not unlock the wallet, sign again, change the amount or use a new account sequence. If the bytes are no longer retained, the retry fails; it never constructs a replacement. Inclusion and execution may still be unknown.")
        .arg(m_wallet_name, saved_market, action_id), false)};
    if (!self || generation != m_generation) return;
    m_busy = false;
    if (approved && m_receipt && m_receipt->action_id == action_id && m_pending_market == saved_market) startJob(std::nullopt, std::nullopt, true);
    else updateControls();
}
void B3FlowMeshTradingPanel::reviewUncertain()
{
    if (!m_wallet || !m_backend || m_busy || m_thread || !m_uncertain || !m_uncertain_refreshed ||
        !m_security_warning.isEmpty() || (!uncertainWalletSelected() && m_uncertain_wallet)) return;
    const QPointer<B3FlowMeshTradingPanel> self{this}; const auto generation{m_generation};
    m_busy = true; updateControls();
    const QString owner_warning{!m_uncertain_wallet ? tr("\n\nThe original wallet instance was unloaded. This wallet's refresh does not resolve the original request. Open the original wallet and inspect the saved ID through its existing status command; automatic retry is disabled here.") : QString{}};
    const bool accepted{confirm(m_uncertain_details + owner_warning + tr("\n\nA fresh read-only refresh succeeded, but this does NOT prove execution. Check the saved transaction ID or the exact action ID with getflowmeshactionstatus. An account sequence change alone does not establish this request's outcome. To resend retained bytes, use Retry exact saved action; never re-sign an unknown request as a retry.\n\nI have inspected the outcome and want to re-enable NEW actions. This acknowledgement never resends the saved request."), false)};
    if (!self || generation != m_generation) return;
    if (accepted) { m_uncertain = false; m_uncertain_refreshed = false; m_uncertain_wallet.clear(); if (m_receipt && m_receipt->action_id == m_uncertain_action_id && m_receipt->market == m_uncertain_market && m_receipt->account == m_uncertain_account) { m_pending_sequence.reset(); m_receipt.reset(); m_receipt_wallet.clear(); } m_uncertain_action_id.clear(); m_uncertain_market.clear(); m_uncertain_account.clear(); notice(tr("Uncertain submission acknowledged after inspection. Nothing was replayed; acknowledging does not cancel the saved action.")); }
    m_busy = false; updateControls();
}
bool B3FlowMeshTradingPanel::restoreLock()
{
    m_unlock.reset(); if (!m_restore_locked) return true;
    try {
        if (m_relock_backend && !m_relock_backend->isLocked()) m_relock_backend->lock();
        if (m_relock_backend && m_relock_backend->isLocked()) { m_restore_locked = false; m_relock_backend.reset(); return true; }
    } catch (...) { }
    m_security_warning = tr("SECURITY: spending relock could not be verified for wallet %1. Lock it or close the app before leaving it unattended. FlowMesh controls are blocked.").arg(m_relock_wallet_name);
    notice(m_security_warning); m_status->setText(m_security_warning); updateControls(); Q_EMIT securityWarning(m_security_warning); return false;
}
