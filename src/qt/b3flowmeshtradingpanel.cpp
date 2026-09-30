// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/b3flowmeshtradingpanel.h>
#include <qt/b3flowmeshchart.h>
#include <qt/b3theme.h>
#include <flowmesh/observed_time.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <node/flowmesh_client_work.h>
#include <node/flowmesh_timing.h>
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
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QSignalBlocker>
#include <QStyle>
#include <QTableWidget>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <exception>
#include <functional>
#include <limits>
#include <set>
#include <stdexcept>

using B3FlowMeshTrading::Action;
using B3FlowMeshTrading::Market;
using B3FlowMeshTrading::Operation;
namespace {
// Local scheduling only: not transport failure, action cancellation or a
// freshness observation. Caught separately before applying any partial read.
struct PassiveRefreshYield {};
QLabel* Label(const QString& text, QWidget* parent) {
    auto* label{new QLabel{text, parent}}; label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true); label->setTextInteractionFlags(Qt::TextSelectableByMouse); return label;
}
bool PreparedOperation(Operation op) { return op == Operation::Deposit || op == Operation::Checkpoint || op == Operation::Vault; }
bool SignedAction(Operation op) { return op == Operation::Order || op == Operation::Cancel || op == Operation::Withdraw; }
bool NoSpendingRequired(const std::string& method) {
    return method == "listflowmeshmarkets" || method == "getflowmeshbalance" || method == "getblockchaininfo" ||
        method == "getflowmeshclientinfo" || method == "flowmeshclientconnect" ||
        method == "getflowmeshmarketdata" || method == "getflowmeshcandles" || method == "getflowmeshactionstatus" || method == "listflowmeshactions" || method == "listflowmeshvaultoperations" || method == "testmempoolaccept";
}
//! Display sections of a selected-market read: the clearing ticks after the
//! last one held (the newest otherwise), the book levels of every verified
//! standing curve in the displayed grouping (`levels`, see levelsRequest)
//! and, only while the ticket holds an order that parses, the indicative
//! next clearing for it (a node copies the standing curves once per head
//! for both). The book's spread row never uses that draft indicative: its
//! standing clearing comes with the levels. Display only; nothing is signed.
void AddMarketViews(UniValue& options, const std::optional<uint64_t>& after, const UniValue& draft, const UniValue& levels)
{
    UniValue ticks{UniValue::VOBJ};
    if (after) ticks.pushKV("after_sequence", *after);
    ticks.pushKV("limit", static_cast<uint64_t>(B3FlowMeshMarketData::MAX_HELD_TICKS));
    options.pushKV("ticks", std::move(ticks));
    // Operator-observed times of the head, history and ticks; display only.
    options.pushKV("observed_time", true);
    if (levels.isObject()) options.pushKV("levels", levels);
    if (!draft.isObject()) return;
    UniValue indicative{UniValue::VOBJ};
    indicative.pushKV("draft", draft);
    options.pushKV("indicative", std::move(indicative));
}
QString RpcError(const UniValue& error) {
    const auto& message{error.find_value("message")};
    return error.isObject() && message.isStr() ? QString::fromStdString(message.get_str()).left(500) : QStringLiteral("FlowMesh RPC failed.");
}
//! A curve-depth price cell whose exact inverse price, in its tooltip, is
//! computed only when the tooltip is read: the same text, without formatting
//! every row of a (usually hidden) table on each update.
class DepthPriceItem final : public QTableWidgetItem
{
public:
    static constexpr int TYPE{QTableWidgetItem::UserType + 1};
    explicit DepthPriceItem(const QString& text) : QTableWidgetItem{text, TYPE} {}
    //! No tooltip unless `inverse`.
    void setExactInverse(bool inverse, CAmount price, int decimals)
    {
        m_inverse = inverse; m_price = price; m_decimals = decimals;
    }
    QVariant data(int role) const override
    {
        if (role != Qt::ToolTipRole) return QTableWidgetItem::data(role);
        if (!m_inverse) return QString{};
        return B3FlowMeshTradingPanel::tr("Exact inverse price: %1").arg(B3FlowMeshMarketData::ExactInversePrice(m_price, m_decimals));
    }

private:
    bool m_inverse{false};
    CAmount m_price{0};
    int m_decimals{0};
};
// Keep the model and its existing items alive during background updates.
// Identical data must not reset selection, scroll position or widget geometry.
// `make`, when given, creates the new items (of a column).
void SetRows(QTableWidget* table, const std::vector<QStringList>& rows,
             const std::function<QTableWidgetItem*(int column, const QString& text)>& make = {})
{
    const int scroll{table->verticalScrollBar()->value()};
    if (table->rowCount() != static_cast<int>(rows.size())) table->setRowCount(static_cast<int>(rows.size()));
    for (int r{0}; r < static_cast<int>(rows.size()); ++r) {
        for (int c{0}; c < rows[r].size(); ++c) {
            auto* item{table->item(r, c)};
            if (!item) {
                item = make ? make(c, rows[r][c]) : new QTableWidgetItem{rows[r][c]};
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
//! Whether the book of `s` offers `price` (canonical) to a click: an edge of
//! one of the node's verified levels shown in `grouping` (exact: its price;
//! grouped: the edge every order in it accepts), or, where the book shows no
//! such levels, a limit or ladder price of the projected curve page.
bool BookOffersPrice(const B3FlowMeshMarketData::Snapshot& s, const CAmount price, const bool inverse, const flowmesh::LevelGrouping& grouping)
{
    if (price <= 0) return false;
    const auto* levels{s.levels ? &*s.levels : nullptr};
    if (levels && levels->available && levels->head == s.head && levels->grouping == grouping && (grouping.exact || grouping.inverse == inverse)) {
        for (const auto* side : {&levels->bids, &levels->asks}) {
            const bool bid{side == &levels->bids};
            for (const auto& level : side->levels) {
                if ((grouping.exact || bid ? level.low : level.high) == price) return true;
            }
        }
        return false;
    }
    const auto book{B3FlowMeshMarketData::ProjectLimitBook(s, inverse)};
    for (const auto* rows : {&book.asks, &book.bids}) {
        for (const auto& level : *rows) if (level.canonical_price == price) return true;
    }
    return false;
}
QString ClearingStripTip()
{
    return B3FlowMeshTradingPanel::tr("The latest certified auction: its uniform price, matched quantity and the side whose orders were left partly unfilled at that price. Its time is when the serving node observed it, by that node's clock; auctions carry no certified time.");
}
QString IndicativeTip()
{
    return B3FlowMeshTradingPanel::tr("Computed by this node from the verified standing orders, as if the next auction cleared them now with this order added. Not certified: orders not yet certified are excluded, and your order's admission (balance and reservation) is assumed. It never affects what you sign.");
}
} // namespace

B3FlowMeshTradingPanel::B3FlowMeshTradingPanel(QWidget* parent) : QWidget{parent}
{
    setObjectName(QStringLiteral("flowMeshTradingPanel"));
    auto* outer{new QVBoxLayout{this}}; outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll{new QScrollArea{this}}; scroll->setWidgetResizable(true);
    auto* content{new QWidget{scroll}}; auto* layout{new QVBoxLayout{content}};
    layout->setContentsMargins(12, 10, 12, 12); layout->setSpacing(8);
    // Notices float over the top-left of the visible page, away from the
    // ticket: in no layout, so they never move it, and they stay in view
    // however far the page is scrolled.
    m_toasts = new B3FlowMeshToasts{scroll->viewport()};
    auto* connection_row{new QHBoxLayout};
    m_endpoint = new QLineEdit{content}; m_endpoint->setObjectName(QStringLiteral("flowMeshEndpoint"));
    m_endpoint->setMaxLength(2048); m_endpoint->setPlaceholderText(tr("https://trading.example.org"));
    m_endpoint->setAccessibleName(tr("HTTPS trading endpoint"));
    m_endpoint->setToolTip(tr("Connect saves this HTTPS endpoint and checks market discovery. It does not unlock your wallet or submit any trading action."));
    m_connect = new QPushButton{tr("Connect"), content}; m_connect->setObjectName(QStringLiteral("flowMeshConnect"));
    connection_row->addWidget(m_endpoint, 1); connection_row->addWidget(m_connect); layout->addLayout(connection_row);
    m_link_banner = Label(QString{}, content); m_link_banner->setObjectName(QStringLiteral("flowMeshLinkBanner"));
    m_link_banner->setAccessibleName(tr("Connection state")); m_link_banner->hide(); layout->addWidget(m_link_banner);
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
    // Hyperliquid's layout: the chart above the activity tabs on the left,
    // and the order book and the ticket each the full height beside them,
    // so the book shows its depth (the tabs no longer take its height).
    auto* market_column{new QSplitter{Qt::Vertical, center}}; market_column->setChildrenCollapsible(false);
    market_column->setObjectName(QStringLiteral("flowMeshMarketColumn"));
    auto* chart_card{new QWidget{market_column}}; B3Theme::markCard(chart_card); auto* chart_layout{new QVBoxLayout{chart_card}};
    auto* chart_modes{new QHBoxLayout}; auto* modes{new QButtonGroup{chart_card}};
    auto* candle_interval{new QComboBox{chart_card}}; candle_interval->setObjectName(QStringLiteral("flowMeshCandleInterval"));
    m_candle_interval_combo = candle_interval;
    updateIntervalChoices(false);
    for (const auto& mode : {std::pair{tr("Candles"), B3FlowMeshChart::Mode::Prices}, std::pair{tr("Depth"), B3FlowMeshChart::Mode::Liquidity}}) {
        auto* button{new QPushButton{mode.first, chart_card}}; button->setCheckable(true); button->setChecked(mode.second == B3FlowMeshChart::Mode::Prices); button->setProperty("b3variant", QStringLiteral("timeframe")); modes->addButton(button); chart_modes->addWidget(button);
        button->setObjectName(mode.second == B3FlowMeshChart::Mode::Prices ? QStringLiteral("flowMeshChartPrices") : QStringLiteral("flowMeshChartLiquidity"));
        connect(button, &QPushButton::clicked, this, [this, mode, candle_interval] { m_chart->setMode(mode.second); candle_interval->setEnabled(mode.second == B3FlowMeshChart::Mode::Prices); });
    }
    chart_modes->addStretch(); chart_modes->addWidget(candle_interval); chart_layout->addLayout(chart_modes); m_chart = new B3FlowMeshChart{chart_card}; chart_layout->addWidget(m_chart, 1);
    m_clearing_strip = Label(QString{}, chart_card); m_clearing_strip->setObjectName(QStringLiteral("flowMeshClearingStrip"));
    m_clearing_strip->setAccessibleName(tr("Latest auction")); B3Theme::markTextRole(m_clearing_strip, QStringLiteral("secondary"));
    m_clearing_strip->setToolTip(ClearingStripTip());
    m_clearing_strip->hide(); chart_layout->addWidget(m_clearing_strip);
    connect(candle_interval, &QComboBox::currentIndexChanged, this, [this, candle_interval] {
        const auto identity{candle_interval->currentData(Qt::UserRole + 1).toString()};
        if (identity.startsWith(QLatin1Char('t'))) {
            const auto interval{flowmesh::ParseCandleInterval(identity.mid(1).toStdString())};
            if (!interval || *interval == m_candle_interval) return;
            m_candle_interval = *interval;
            // Another interval is another window: read it at once.
            updateDataViews();
            refresh();
        } else {
            m_chart->setCandleInterval(candle_interval->currentData().toULongLong());
        }
    });
    m_chart->setToolTip(tr("Candles show open, high, low and close from actual clearings. Hover to inspect a candle and its volume; scroll to zoom, drag to pan and double-click for the latest. Periods without an observed auction are not filled in. Depth lines show evaluated demand and supply, not historical trades."));
    const auto table = [](QWidget* parent, const QStringList& headers, const char* name) {
        auto* view{new QTableWidget{parent}}; view->setObjectName(QLatin1String(name)); view->setColumnCount(headers.size()); view->setHorizontalHeaderLabels(headers); view->verticalHeader()->hide(); view->setShowGrid(false); view->setAlternatingRowColors(false); view->setEditTriggers(QAbstractItemView::NoEditTriggers); view->setSelectionMode(QAbstractItemView::NoSelection); view->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); view->verticalHeader()->setDefaultSectionSize(26); view->setMinimumWidth(200); return view;
    };
    auto* liquidity_card{new QWidget{center}}; B3Theme::markCard(liquidity_card); auto* liquidity_layout{new QVBoxLayout{liquidity_card}};
    // The title and the view selector share one row: the height is the book's.
    auto* liquidity_header{new QHBoxLayout}; liquidity_header->setContentsMargins(0, 0, 0, 0);
    auto* liquidity_title{Label(tr("Liquidity"), liquidity_card)}; liquidity_title->setObjectName(QStringLiteral("flowMeshLiquidityTitle")); B3Theme::markTextRole(liquidity_title, QStringLiteral("h3")); liquidity_header->addWidget(liquidity_title, 1);
    auto* liquidity_mode{new QComboBox{liquidity_card}}; liquidity_mode->setObjectName(QStringLiteral("flowMeshLiquidityMode"));
    liquidity_mode->setAccessibleName(tr("Liquidity display")); liquidity_mode->addItems({tr("Order book"), tr("Curve depth"), tr("Trades")}); liquidity_header->addWidget(liquidity_mode);
    liquidity_layout->addLayout(liquidity_header);
    m_order_book = new B3FlowMeshOrderBook{liquidity_card}; liquidity_layout->addWidget(m_order_book, 1);
    m_trade_tape = new B3FlowMeshTradeTape{liquidity_card}; liquidity_layout->addWidget(m_trade_tape, 1); m_trade_tape->hide();
    m_depth_view = table(liquidity_card, {tr("Price"), tr("Buy liquidity"), tr("Sell liquidity")}, "flowMeshCurveDepth"); liquidity_layout->addWidget(m_depth_view, 1);
    m_liquidity_note = Label(tr("No certified curves yet."), liquidity_card); B3Theme::markTextRole(m_liquidity_note, QStringLiteral("secondary")); liquidity_layout->addWidget(m_liquidity_note);
    m_depth_view->hide(); m_liquidity_note->hide();
    connect(liquidity_mode, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_order_book->setVisible(index == 0); m_depth_view->setVisible(index == 1); m_liquidity_note->setVisible(index == 1);
        m_trade_tape->setVisible(index == 2);
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
    auto* marketable_row{new QHBoxLayout};
    m_marketable = new QCheckBox{tr("Marketable limit"), ticket_card}; m_marketable->setObjectName(QStringLiteral("flowMeshMarketable"));
    m_marketable->setToolTip(tr("Not a market order: FlowMesh has no market or immediate-or-cancel orders. This prices an ordinary limit order at the best certified opposite price moved by at most the maximum slippage. Any unfilled part stays as a standing order until you cancel it."));
    m_slippage = entry("flowMeshMaxSlippage", 6); m_slippage->setText(B3FlowMeshMarketData::FormatSlippage(B3FlowMeshMarketData::DEFAULT_SLIPPAGE_BPS));
    m_slippage->setAccessibleName(tr("Maximum slippage in percent")); m_slippage->setMaximumWidth(64);
    m_slippage->setToolTip(tr("Maximum slippage from the best certified opposite price, 0.01% to 10%."));
    marketable_row->addWidget(m_marketable); marketable_row->addStretch(); marketable_row->addWidget(m_slippage); marketable_row->addWidget(Label(tr("% max"), ticket_card));
    ticket->addLayout(marketable_row);
    m_price_label = Label(tr("Limit price · B3 / token"), ticket_card); ticket->addWidget(m_price_label); m_price = entry("flowMeshPriceAtoms", 64); m_price->setPlaceholderText(tr("Enter price")); ticket->addWidget(m_price);
    m_marketable_note = Label(QString{}, ticket_card); m_marketable_note->setObjectName(QStringLiteral("flowMeshMarketableNote"));
    B3Theme::markTextRole(m_marketable_note, QStringLiteral("secondary")); m_marketable_note->hide(); ticket->addWidget(m_marketable_note);
    m_quantity_label = Label(tr("Quantity · token"), ticket_card); ticket->addWidget(m_quantity_label); m_quantity = entry("flowMeshRawQuantity", 64); m_quantity->setPlaceholderText(tr("Enter quantity")); ticket->addWidget(m_quantity);
    // Ladder: up to three more prices on this side, one order with the first.
    m_ladder = new QCheckBox{tr("Ladder · up to %1 prices").arg(flowmesh::FLOWMESH_LADDER_MAX_STEPS), ticket_card};
    m_ladder->setObjectName(QStringLiteral("flowMeshLadder"));
    m_ladder->setToolTip(tr("Several prices on this side as ONE order: each price has its own quantity, and the price and quantity above are the first. "
                            "Like any order it replaces your order on this side; a price cannot be cancelled or edited alone. A buy reserves exactly the sum of quantity x price, as separate orders would. Nothing is signed until you review it."));
    ticket->addWidget(m_ladder);
    m_ladder_rows = new QWidget{ticket_card};
    auto* ladder_grid{new QGridLayout{m_ladder_rows}}; ladder_grid->setContentsMargins(0, 0, 0, 0);
    for (size_t i{0}; i < m_ladder_prices.size(); ++i) {
        const int n{static_cast<int>(i) + 2};
        m_ladder_prices[i] = entry(QStringLiteral("flowMeshLadderPrice%1").arg(n).toLatin1().constData(), 64);
        m_ladder_quantities[i] = entry(QStringLiteral("flowMeshLadderQuantity%1").arg(n).toLatin1().constData(), 64);
        m_ladder_prices[i]->setPlaceholderText(tr("Price %1").arg(n)); m_ladder_quantities[i]->setPlaceholderText(tr("Quantity %1").arg(n));
        m_ladder_prices[i]->setAccessibleName(tr("Ladder price %1").arg(n)); m_ladder_quantities[i]->setAccessibleName(tr("Ladder quantity %1").arg(n));
        ladder_grid->addWidget(m_ladder_prices[i], static_cast<int>(i), 0); ladder_grid->addWidget(m_ladder_quantities[i], static_cast<int>(i), 1);
        connect(m_ladder_prices[i], &QLineEdit::textChanged, this, &B3FlowMeshTradingPanel::updateTicket);
        connect(m_ladder_quantities[i], &QLineEdit::textChanged, this, &B3FlowMeshTradingPanel::updateTicket);
    }
    m_ladder_rows->hide(); ticket->addWidget(m_ladder_rows);
    connect(m_ladder, &QCheckBox::toggled, this, [this](bool on) {
        m_ladder_rows->setVisible(on);
        // A ladder names its own prices: never a marketable limit.
        if (on) m_marketable->setChecked(false);
        updateTicket(); updateControls();
    });
    m_grid_note = Label(tr("Token units will be verified from asset metadata."), ticket_card); B3Theme::markTextRole(m_grid_note, QStringLiteral("secondary"));
    m_ticket_available = Label(tr("Available  —"), ticket_card); ticket->addWidget(m_ticket_available); m_ticket_total = Label(tr("Limit notional  —"), ticket_card); ticket->addWidget(m_ticket_total);
    m_ticket_fee = Label(tr("Spot fee 0.01% · seller-paid"), ticket_card); B3Theme::markTextRole(m_ticket_fee, QStringLiteral("secondary")); ticket->addWidget(m_ticket_fee);
    m_indicative = Label(QString{}, ticket_card); m_indicative->setObjectName(QStringLiteral("flowMeshIndicative"));
    m_indicative->setAccessibleName(tr("Indicative next clearing")); B3Theme::markTextRole(m_indicative, QStringLiteral("secondary"));
    m_indicative->setToolTip(IndicativeTip());
    m_indicative->hide(); ticket->addWidget(m_indicative);
    m_order = new QPushButton{tr("Review buy order…"), ticket_card}; m_order->setObjectName(QStringLiteral("flowMeshSubmitOrder")); m_order->setProperty("b3variant", QStringLiteral("primary")); ticket->addWidget(m_order);
    m_order->setToolTip(tr("One order per side. A new order replaces that side. Orders clear together at one auction price; an accepted request is not a fill.")); ticket->addStretch();
    center->addWidget(market_column); center->addWidget(liquidity_card); center->addWidget(ticket_card); center->setStretchFactor(0, 6); center->setStretchFactor(1, 3); center->setStretchFactor(2, 3); center->setSizes({600, 290, 300}); layout->addWidget(center, 1);
    m_activity = new QTabWidget{market_column}; m_activity->setMinimumHeight(185);
    auto* own{new QWidget{m_activity}}; auto* own_layout{new QVBoxLayout{own}};
    m_pending_view = table(own, {tr("Request"), tr("Signed"), tr("Sent"), tr("Admitted"), tr("Certified"), tr("Outcome"), tr("Status")}, "flowMeshPendingActions");
    m_pending_view->setAccessibleName(tr("Your requests in progress"));
    m_pending_view->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    for (const int column : {0, 6}) m_pending_view->horizontalHeader()->setSectionResizeMode(column, QHeaderView::Stretch);
    m_pending_view->horizontalHeaderItem(4)->setToolTip(tr("Inclusion in a quorum-certified batch, verified by this wallet. Inclusion alone does not say whether it was applied."));
    m_pending_view->horizontalHeaderItem(5)->setToolTip(tr("A verified per-action outcome: a fill, a cancellation, or not executed. \"not reported\" means inclusion is verified but this server reported no verified outcome."));
    m_pending_view->setMaximumHeight(28 + 26 * static_cast<int>(MAX_REQUEST_ROWS + 1)); m_pending_view->hide(); own_layout->addWidget(m_pending_view);
    m_own_note = Label(tr("No open orders"), own); own_layout->addWidget(m_own_note); m_own_view = table(own, {tr("Side"), tr("Price"), tr("Remaining"), tr("Reserved")}, "flowMeshOwnCurves"); own_layout->addWidget(m_own_view);
    // An own order can be selected for Edit; the other tables stay unselectable.
    m_own_view->setSelectionMode(QAbstractItemView::SingleSelection); m_own_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_balances = Label(tr("Certified balances will appear here."), own); own_layout->addWidget(m_balances);
    auto* own_actions{new QHBoxLayout};
    m_edit_order = new QPushButton{tr("Edit selected order…"), own}; m_edit_order->setObjectName(QStringLiteral("flowMeshEditOrder"));
    m_edit_order->setToolTip(tr("Fills the ticket with a replacement of the selected order: same side, its exact limit and its remaining quantity. Nothing is signed: review the replacement to send it. A new order on a side replaces that side's order at once; there is no gap and no second request."));
    m_cancel_order = new QPushButton{tr("Cancel selected Buy / Sell side…"), own};
    own_actions->addWidget(m_edit_order); own_actions->addWidget(m_cancel_order); own_layout->addLayout(own_actions); m_activity->addTab(own, tr("Your orders"));
    auto* history_page{new QWidget{m_activity}}; auto* history_layout{new QVBoxLayout{history_page}}; m_history_note = Label(tr("No trades yet"), history_page); history_layout->addWidget(m_history_note); m_history_view = table(history_page, {tr("Time"), tr("Price (B3)"), tr("Amount"), tr("Your buy"), tr("Your sell")}, "flowMeshCertifiedFills"); history_layout->addWidget(m_history_view); m_activity->addTab(history_page, tr("Trade history"));
    m_log = new QPlainTextEdit{m_activity}; m_log->setObjectName(QStringLiteral("flowMeshOperationLog")); m_log->setReadOnly(true); m_log->setMaximumBlockCount(100); m_activity->addTab(m_log, tr("Activity"));
    market_column->addWidget(chart_card); market_column->addWidget(m_activity); market_column->setStretchFactor(0, 3); market_column->setStretchFactor(1, 2);
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
        // Never reinterpret a typed price or a deferred click in another
        // unit: every price of the ticket, the ladder's included (hidden rows
        // too), and with no undo or redo that would bring one back.
        // Quantities are token amounts in both views and are kept.
        clearTicketPrices(); m_deferred_review.reset();
        updateMarketText(); updateDataViews();
    });
    connect(m_market, &QComboBox::currentIndexChanged, this, [this] {
        // A passive read of the market being left yields at its next RPC
        // (an RPC already under way still completes); the new market is
        // then read at once. User-paced, like a review click.
        if (m_active_result && m_active_result->passive) m_yield_refresh->store(true);
        resetRefreshSchedule();
        m_deferred_status.reset(); // A queued click never follows a market switch.
        // The ticket was typed for the market left: its prices and quantities
        // are never read in another token. (Internal list updates block this
        // signal, so this runs only for a market the user picked.)
        clearTicketEntries();
        // A failed read/backoff belongs to its selected market. Another listed
        // market may already have a certified head and can be read immediately.
        m_read_error.clear(); m_read_failed = false; m_read_failures = 0; m_link_attempts = 0; m_attempt_age.invalidate();
        m_gap_markers.clear();
        m_display.reset(); m_display_confirmed.invalidate();
        const auto selected{market()};
        if (!selected || !m_snapshot || selected->id != m_snapshot->market) {
            m_amount->clear(); m_withdrawal_draft_amount.clear(); m_destination->clear(); m_deposit_txid->clear(); m_deposit_vout->setText(QStringLiteral("0"));
            m_snapshot.reset(); m_response_age.invalidate(); m_certificate_age.invalidate(); m_queue_age.invalidate();
            m_loading = selected.has_value(); updateDataViews();
        }
        updateMarketText(); updateWatchState(); if (!m_busy) refresh();
    });
    connect(m_order, &QPushButton::clicked, this, &B3FlowMeshTradingPanel::requestOrderReview);
    connect(m_cancel_order, &QPushButton::clicked, this, [this] { begin(Operation::Cancel); });
    connect(m_edit_order, &QPushButton::clicked, this, &B3FlowMeshTradingPanel::editOrder);
    connect(m_order_book, &B3FlowMeshOrderBook::levelClicked, this, [this](const QString& price, qint64 canonical, qint64 cumulative, const QString& head) {
        fillFromBook(price, static_cast<CAmount>(canonical), static_cast<CAmount>(cumulative), head);
    });
    connect(m_order_book, &B3FlowMeshOrderBook::groupingSelected, this, &B3FlowMeshTradingPanel::selectBookGrouping);
    connect(m_own_view->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
        // Bind to the canonical side and exact points shown when selected:
        // rows keep their index while the certified data behind them changes.
        const auto rows{m_own_view->selectionModel()->selectedRows()};
        const int row{rows.isEmpty() ? -1 : rows.front().row()};
        // The order shown in that row (Edit then requires exactly it in the
        // gates' verified data; see editCurve).
        const bool shown{row >= 0 && static_cast<size_t>(row) < m_shown_own_curves.size()};
        m_edit_side = shown ? m_shown_own_curves[row].side : QString{};
        m_edit_points = shown ? m_shown_own_curves[row].points : std::vector<B3FlowMeshMarketData::Point>{};
        updateControls();
    });
    connect(m_deposit, &QPushButton::clicked, this, [this] { openFunding(false); });
    connect(m_admit, &QPushButton::clicked, this, [this] { begin(Operation::Admit); });
    connect(m_withdraw, &QPushButton::clicked, this, [this] { openFunding(true); });
    connect(m_checkpoint, &QPushButton::clicked, this, [this] { begin(Operation::Checkpoint); });
    connect(m_publish, &QPushButton::clicked, this, [this] { begin(Operation::Vault); });
    connect(m_review_uncertain, &QPushButton::clicked, this, &B3FlowMeshTradingPanel::reviewUncertain);
    connect(m_retry_receipt, &QPushButton::clicked, this, &B3FlowMeshTradingPanel::retryReceipt);
    connect(m_effect, &QComboBox::currentIndexChanged, this, &B3FlowMeshTradingPanel::updateControls);
    connect(m_price, &QLineEdit::textChanged, this, &B3FlowMeshTradingPanel::updateTicket); connect(m_quantity, &QLineEdit::textChanged, this, &B3FlowMeshTradingPanel::updateTicket);
    connect(m_marketable, &QCheckBox::toggled, this, &B3FlowMeshTradingPanel::updateTicket); connect(m_slippage, &QLineEdit::textChanged, this, &B3FlowMeshTradingPanel::updateTicket);
    connect(m_buy, &QPushButton::clicked, this, [this] { m_side->setCurrentIndex(0); updateTicket(); }); connect(m_sell, &QPushButton::clicked, this, [this] { m_side->setCurrentIndex(1); updateTicket(); });
    m_timer = new QTimer{this}; m_timer->setInterval(500);
    m_watch_clock.start();
    connect(m_timer, &QTimer::timeout, this, &B3FlowMeshTradingPanel::tick);
    m_rearm = new QTimer{this}; m_rearm->setSingleShot(true); m_rearm->setTimerType(Qt::PreciseTimer);
    connect(m_rearm, &QTimer::timeout, this, &B3FlowMeshTradingPanel::rearmWatch);
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] { m_closing = true; cancelAndWait(); });
    updateControls();
}

B3FlowMeshTradingPanel::~B3FlowMeshTradingPanel()
{
    m_closing = true;
    cancelAndWait();
}

void B3FlowMeshTradingPanel::tick()
{
    const bool visible{isVisible()};
    const bool became_hidden{m_tick_was_visible && !visible};
    m_tick_was_visible = visible;
    if (!visible) {
        // Hidden: no polls or watches. A live-update connection is closed
        // once instead of lingering until the endpoint's idle timeout.
        if (became_hidden && (m_watch.mode == B3FlowMeshWatch::Mode::Live || m_watch.mode == B3FlowMeshWatch::Mode::Probing)) m_watch_release_pending = true;
        if (m_watch_release_pending) armWatch(/*release=*/true);
        m_polling_since.invalidate();
        return;
    }
    if (!m_polling_since.isValid()) m_polling_since.start();
    updateMarketText();
    updateWatchState();
    // A watch is armed before the poll, so a probe it starts holds this
    // tick's market read. While live updates arrive (or a probe is
    // outstanding), the 500 ms tick keeps display ageing and the
    // auxiliary cycle; the watch supplies the market read.
    armWatch();
    if (!liveSkipsPoll()) refresh();
    if (m_watch_release_pending && m_watch.mode != B3FlowMeshWatch::Mode::Live && m_watch.mode != B3FlowMeshWatch::Mode::Probing) armWatch(/*release=*/true);
}

void B3FlowMeshTradingPanel::setWalletModel(WalletModel* wallet)
{
    cancelAndWait();
    m_wallet = wallet; m_backend.reset(); m_market_data.clear(); m_effect_data.clear(); m_snapshot.reset(); m_display.reset(); m_display_confirmed.invalidate();
    m_saved_actions = {}; m_saved_actions_ready = false;
    // Request rows and notices are per wallet; the next attach seeds them again.
    m_request_seen.clear(); m_request_order.clear(); m_requests_seeded = false; m_last_settled.invalidate();
    m_toasts->clear(); m_link_toast_down = false; m_link_toast_age.invalidate();
    m_amount->clear(); m_withdrawal_draft_amount.clear(); m_destination->clear(); m_deposit_txid->clear(); m_deposit_vout->setText(QStringLiteral("0"));
    // The market list is cleared below with its signal blocked and read
    // again for this wallet, its first market selected: the previous
    // wallet's ticket is never read in that market.
    clearTicketEntries();
    m_client_info.reset(); m_connection_error.clear(); m_connect_error.clear();
    { QSignalBlocker blocker{m_saved_selector}; m_saved_selector->clear(); }
    m_receipt_card->setText(wallet ? tr("Loading this wallet's locally saved requests. No action is automatically resent.") : tr("Open a wallet to inspect its saved requests."));
    m_response_age.invalidate(); m_certificate_age.invalidate(); m_catalog_age.invalidate(); m_attempt_age.invalidate(); m_queue_age.invalidate(); m_read_failed = false; m_read_failures = 0; m_read_error.clear();
    m_link_attempts = 0; m_last_live_wall_ms = -1; m_last_live_market.clear();
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
    if (withdrawal) m_withdrawal_draft_amount.clear(); // Explicit asset routing starts a new draft in that asset.
    if (m_asset->currentIndex() != (asset_id.isEmpty() ? 1 : 0)) m_amount->clear();
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
        if (m_receipt && receiptWalletSelected() && m_pending_market == selected->id) {
            details += tr("\nTracked request %1. Command line: getflowmeshactionstatus %2 %1 reads its status; retryflowmeshaction %2 %1 resends only its retained signed bytes, never a new signature.").arg(m_receipt->action_id, selected->id);
        }
    }
    // Build the final copy before touching labels: intermediate variants of
    // unchanged data still invalidate layouts and accessibility observations.
    m_identity_detail->setText(details);
    QString progress;
    if (selected && selected->checkpoint_pending) progress = tr("Certified checkpoint awaiting B3 publication. This is separate from trade execution.");
    else if (selected && m_pending_sequence && selected->id == m_pending_market && !m_receipt) progress = tr("Request submitted at account sequence %1. A sequence change alone does not prove its outcome. No automatic retry.").arg(*m_pending_sequence);
    else if (matched) {
        const uint64_t head{m_snapshot->next_sequence ? m_snapshot->next_sequence - 1 : 0};
        const QString when{observedClock(head, m_snapshot->head)};
        progress = tr("Latest certified auction %1 · epoch %2 · configured threshold %3 of %4 FN seats · %5 queued actions\nAn available runtime or configured threshold is not proof that those validators are currently online.")
                       .arg(when.isEmpty() ? tr("#%1").arg(head) : tr("%1 (#%2)").arg(when).arg(head)).arg(m_snapshot->epoch).arg(m_snapshot->quorum_required).arg(m_snapshot->active_seats).arg(m_snapshot->pending_actions);
    }
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
    ++m_view_builds;
    // What the views show: m_display while it is newer (display only), else
    // the gates' own m_snapshot.
    const auto& view{displayedSnapshot()};
    const auto selected{market()}; const bool matched{selected && view && selected->id == view->market};
    const bool inverse{inverted()};
    // Held ticks that reach the displayed head give the chart its longer
    // window of the same actual clearings (endpoint-reported for a remote
    // client, as its history is).
    const bool ticks_current{matched && !m_ticks.empty() && m_ticks_market == view->market && m_ticks.back().hash == view->head};
    std::optional<Snapshot> chart_snapshot{matched ? view : std::nullopt};
    if (ticks_current) {
        chart_snapshot->history = TicksAsTrades(m_ticks); chart_snapshot->history_available = true;
        chart_snapshot->history_page_partial = false; chart_snapshot->history_truncated = m_ticks.front().sequence > 0;
    }
    m_chart->setInverted(inverse); m_chart->setSnapshot(chart_snapshot); m_chart->setLoading(m_loading);
    {
        // Candles in operator-observed time: the source's page, continued
        // with the held ticks' own times by the node's bucketing.
        B3FlowMeshChart::TimeSeries series;
        series.interval = m_candle_interval;
        if (matched && view->observed) {
            const auto& observed{*view->observed};
            series.available = observed.available;
            series.source = observed.source;
            series.remote = view->remote;
            if (!observed.available) series.reason = observed.reason;
            const bool held{m_candles && m_candles_market == view->market && m_candles->interval == m_candle_interval &&
                            m_candles->source == observed.source};
            if (observed.available && !held) {
                // A failed read says why rather than reading forever.
                if (const auto* failure{candlesFailure(view->market, observed.source)}) series.reason = failure->error;
                else series.loading = true;
            } else if (observed.available && !m_candles->available) {
                series.reason = m_candles->reason;
            } else if (observed.available) {
                std::vector<std::pair<flowmesh::ObservedStamp, flowmesh::ClearingTick>> newer;
                const uint64_t served{m_candles->candles.empty() ? 0 : m_candles->candles.back().last_sequence};
                if (!m_candles->candles.empty() && m_ticks_market == view->market) {
                    for (const auto& tick : m_ticks) {
                        if (tick.sequence <= served) continue;
                        const auto stamp{m_tick_times.find(tick.sequence)};
                        const auto clearing{ToClearingTick(tick)};
                        // A tick without its time yet: the next page brings it.
                        if (stamp == m_tick_times.end() || !clearing) break;
                        newer.emplace_back(stamp->second, *clearing);
                    }
                }
                bool incomplete{false};
                series.candles = flowmesh::ExtendObservedCandles(m_candles->candles, m_candle_interval, newer, &incomplete);
                if (incomplete) m_candles_refetch = true;
            }
        }
        updateIntervalChoices(series.available);
        m_chart->setTimeSeries(std::move(series));
        m_chart->setNowMs(QDateTime::currentMSecsSinceEpoch());
    }
    {
        // The grouping of this market and orientation (another one's never
        // carries over), and the choices around the displayed price.
        const auto grouping{bookGrouping()};
        std::optional<CAmount> best_bid, best_ask;
        if (matched && view->levels && view->levels->available) { best_bid = view->levels->best_bid; best_ask = view->levels->best_ask; }
        m_order_book->setGroupingChoices(BookGroupingChoices(best_bid, best_ask, matched ? view->units : Units{}, inverse), grouping);
        m_order_book->setSnapshot(matched ? view : std::nullopt, inverse, grouping);
        m_book_market = matched ? view->market : QString{};
    }
    // Every price in this view at the order book's places (the view's one
    // rounding rule): trades, the chart, the clearing strip and the request
    // rows' titles read the same as the book's rows and Your orders.
    const auto view_places{m_order_book->pricePlaces()};
    if (titleView() != m_titles_view) updatePendingView();
    m_chart->setPricePlaces(view_places);
    QString strip, strip_exact;
    if (ticks_current) {
        const auto last_match{std::find_if(m_ticks.rbegin(), m_ticks.rend(), [](const auto& tick) { return tick.cleared; })};
        const auto& shown{last_match != m_ticks.rend() ? *last_match : m_ticks.back()};
        strip = ClearingTickText(shown, view->units, inverse, view->remote, &strip_exact, observedClock(shown.sequence, shown.hash), view_places);
        if (last_match != m_ticks.rend() && last_match != m_ticks.rbegin()) strip += tr(" · later auctions had no match");
        strip_exact = tr("Auction #%1").arg(shown.sequence) + (strip_exact.isEmpty() ? QString{} : QLatin1Char('\n') + strip_exact);
    } else if (matched && view->ticks && !view->ticks->available) {
        strip = tr("Auction ticks unavailable (%1)").arg(view->ticks->reason);
    }
    if (m_clearing_strip->text() != strip) m_clearing_strip->setText(strip);
    if (const QString tip{ClearingStripTip() + (strip_exact.isEmpty() ? QString{} : QStringLiteral("\n") + strip_exact)}; m_clearing_strip->toolTip() != tip) m_clearing_strip->setToolTip(tip);
    m_clearing_strip->setVisible(!strip.isEmpty());
    {
        const bool timed{matched && view->observed && view->observed->available};
        const auto rows{matched && m_ticks_market == view->market ? B3FlowMeshTradeTape::BuildRows(m_ticks, m_tick_times, m_received_times)
                                                                         : std::vector<B3FlowMeshTradeTape::Row>{}};
        m_trade_tape->setRows(rows, matched ? view->units : Units{}, inverse, timed ? m_tick_times_source : QString{},
                              matched && view->remote,
                              matched && view->observed ? view->observed->reason : QString{},
                              // As the trade history: a remote client's ticks
                              // are the endpoint's claims (critique B5).
                              matched && view->remote && !view->execution_result_verified, view_places);
    }
    if (!matched || !view->units.known) {
        for (auto* table : {m_depth_view, m_history_view, m_own_view}) SetRows(table, {});
        m_own_view->clearSelection(); m_shown_own_curves.clear();
        if (auto* title{m_depth_view->parentWidget()->findChild<QLabel*>(QStringLiteral("flowMeshLiquidityTitle"))}) title->setText(tr("Liquidity"));
        m_history_note->setText(tr("Trade history unavailable")); m_own_note->setText(tr("Orders unavailable"));
        m_last_price->setText(tr("Last price  —")); m_last_price->setToolTip(QString{}); m_liquidity_note->setText(matched ? tr("Asset precision unavailable") : m_loading ? tr("Loading liquidity…") : selected ? tr("Market data unavailable") : tr("Select a market")); return;
    }
    const auto& s{*view}; const auto& u{s.units};
    m_own_note->setText(s.own_curves.empty() ? tr("No open orders") : tr("Your open orders"));
    m_own_note->setToolTip(tr("Orders are persistent certified curves. Replacing a side changes that entire curve; there is no price-time priority queue."));
    std::vector<QStringList> depth, history, own;
    const auto add_depth = [&](const Depth& d) {
        depth.push_back({ViewPrice(d.price, u, inverse, view_places).text,
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
    m_history_view->setHorizontalHeaderLabels({tr("Time"), tr("Price (%1)").arg(price_units), tr("Amount (%1)").arg(quantity_units), tr("Your buy"), tr("Your sell"), tr("Auction fee (B3)")});
    m_history_view->horizontalHeaderItem(0)->setToolTip(tr("Local time the serving node observed the auction (operator-observed, not certified); a dash means no observed time. Hover a row for its auction number."));
    m_history_view->horizontalHeaderItem(5)->setToolTip(tr("Whole-auction B3 fee, not your individual allocation. Endpoint-reported history remains unverified when indicated below."));
    m_own_view->setHorizontalHeaderLabels({tr("Side"), tr("Price"), tr("Remaining (%1)").arg(u.ticker), tr("Reserved")});
    QString last_price{tr("Last price  —")}, last_price_tip;
    std::vector<uint64_t> history_sequences;
    // The exact form of each history row's rounded price (empty: exact).
    std::vector<QString> history_exact;
    // The time cell's tooltip of each history row: its number and time.
    std::vector<QString> history_times;
    for (auto it{s.history.rbegin()}; it != s.history.rend(); ++it) {
        if (!it->cleared || it->quantity == 0) continue;
        const auto price{ViewPrice(it->price, u, inverse, view_places)};
        if (history.empty()) {
            last_price = price.text + QLatin1Char(' ') + price_units;
            if (price.approximate) last_price_tip = tr("Exact last trade price: %1 %2").arg(price.exact, price_units);
        }
        history_exact.push_back(price.approximate ? tr("Exact price: %1 %2").arg(price.exact, price_units) : QString{});
        B3FlowMeshTradeTape::Row as_row; as_row.sequence = it->sequence; as_row.stamp = observedStamp(it->sequence, it->hash);
        QString time_tip;
        const QString when{B3FlowMeshTradeTape::TimeText(as_row, s.observed && s.observed->available ? s.observed->source : QString{}, s.remote, &time_tip)};
        history_times.push_back(time_tip);
        history.push_back({when, price.text, FormatDepthQuantity(it->price, it->quantity, u.decimals, inverse),
            it->own_fills_known ? FormatDepthQuantity(it->price, inverse ? it->own_sell : it->own_buy, u.decimals, inverse) : QStringLiteral("—"),
            it->own_fills_known ? FormatDepthQuantity(it->price, inverse ? it->own_buy : it->own_sell, u.decimals, inverse) : QStringLiteral("—"), FormatAmount(it->fee, 9)});
        history_sequences.push_back(it->sequence);
    }
    // A live update that missed events: a separator where they would be,
    // between the trades before and after it (newest first). Display only.
    std::set<int> separators;
    for (const auto& marker : m_gap_markers) {
        if (marker.market != s.market) continue;
        const auto at{std::find_if(history_sequences.begin(), history_sequences.end(), [&](uint64_t sequence) { return sequence <= marker.from_sequence; }) - history_sequences.begin()};
        const QString text{tr("— events missed here —")};
        history.insert(history.begin() + at, QStringList{text, QString{}, QString{}, QString{}, QString{}, QString{}});
        history_exact.insert(history_exact.begin() + at, QString{});
        history_times.insert(history_times.begin() + at, marker.to_sequence > marker.from_sequence
            ? tr("Live updates missed events between auctions #%1 and #%2.").arg(marker.from_sequence).arg(marker.to_sequence)
            : tr("Live updates missed events at auction #%1.").arg(marker.to_sequence));
        history_sequences.insert(history_sequences.begin() + at, marker.to_sequence);
        std::set<int> shifted;
        for (const int row : separators) shifted.insert(row >= at ? row + 1 : row);
        separators = std::move(shifted);
        separators.insert(static_cast<int>(at));
    }
    // Your orders' prices as the order book shows them (view_places), so
    // an order reads the same in its row and here; each price cell's
    // tooltip keeps the exact prices the row rounds.
    std::vector<QString> own_tips;
    for (const auto& c : s.own_curves) {
        QString description, tip;
        const auto ladder{LadderStepsOf(c)};
        if (c.points.size() == 2 && c.points[1].price == c.points[0].price + 1) {
            const CAmount limit{c.side == QStringLiteral("bid") ? c.points[0].price : c.points[1].price};
            description = ViewPrice(limit, u, inverse, view_places).text + QLatin1Char(' ') + (inverse ? price_units : QStringLiteral("B3"));
            if (inverse) tip = tr("Exact inverse limit: %1").arg(ExactInversePrice(limit, u.decimals));
        } else if (ladder && ladder->size() > 1) {
            // Best first, each price with quantity left.
            auto steps{*ladder};
            const bool bid{c.side == QStringLiteral("bid")};
            std::sort(steps.begin(), steps.end(), [&](const auto& a, const auto& b) { return bid ? a.price > b.price : a.price < b.price; });
            QStringList prices, exact;
            bool rounded{false};
            for (const auto& step : steps) {
                if (step.remaining <= 0) continue;
                const auto shown{ViewPrice(step.price, u, inverse, view_places)};
                prices << shown.text; exact << shown.exact; rounded |= shown.approximate;
            }
            description = tr("Ladder · %1").arg(prices.join(QStringLiteral(" / ")));
            // Far from the market two steps can round to one label: every
            // exact price, in the row's order, on hover.
            if (rounded) {
                tip = tr("Exact prices, best first: %1 %2").arg(exact.join(QStringLiteral(" / ")), inverse ? price_units : QStringLiteral("B3"));
                if (std::set<QString>(prices.begin(), prices.end()).size() < static_cast<size_t>(prices.size())) {
                    tip += tr("\nSome of them round to one value at this view's places; each is still its own price of this one order.");
                }
            }
        } else description = tr("%1-point curve").arg(c.points.size());
        own_tips.push_back(tip);
        const bool buy{DisplayBuy(c.side, inverse)};
        own.push_back({inverse ? (buy ? tr("Buy B3") : tr("Sell B3")) : (buy ? tr("Buy") : tr("Sell")), description,
            (inverse ? (buy ? tr("Spend up to ") : tr("Receive up to ")) : QString{}) + FormatAmount(c.remaining, u.decimals),
            FormatAmount(c.reserved, c.side == QStringLiteral("bid") ? 9 : u.decimals) + (c.side == QStringLiteral("bid") ? QStringLiteral(" B3") : QLatin1Char(' ') + u.ticker)});
    }
    SetRows(m_depth_view, depth, [](int column, const QString& text) { return column == 0 ? new DepthPriceItem{text} : new QTableWidgetItem{text}; });
    SetRows(m_history_view, history); SetRows(m_own_view, own);
    m_shown_own_curves = s.own_curves;
    // A selection whose order changed (or whose row now shows the other
    // side) is dropped: selecting it again binds the order now shown.
    if (const auto rows{m_own_view->selectionModel()->selectedRows()}; !rows.isEmpty()) {
        const auto row{static_cast<size_t>(rows.front().row())};
        if (row >= s.own_curves.size() || s.own_curves[row].side != m_edit_side || s.own_curves[row].points != m_edit_points) m_own_view->clearSelection();
    }
    for (int row{0}; row < m_history_view->rowCount(); ++row) {
        auto* item{m_history_view->item(row, 0)};
        if (!item) continue;
        const bool separator{separators.contains(row)};
        const QString row_time{static_cast<size_t>(row) < history_times.size() ? history_times[row] : QString{}};
        const QString tip{separator ? row_time + tr("\nBalances and orders were re-verified from a certified snapshot; missing trades may not be listed.") : row_time};
        if (item->toolTip() != tip) item->setToolTip(tip);
        const QBrush brush{separator ? palette().brush(QPalette::PlaceholderText) : QBrush{}};
        if (item->foreground() != brush) item->setForeground(brush);
        if (auto* price{m_history_view->item(row, 1)}) {
            const QString exact{static_cast<size_t>(row) < history_exact.size() ? history_exact[row] : QString{}};
            if (price->toolTip() != exact) price->setToolTip(exact);
        }
    }
    for (int i{0}; i < m_depth_view->rowCount(); ++i) {
        // The exact inverse price is formatted only when its tooltip is read.
        if (m_depth_view->item(i, 0)->type() == DepthPriceItem::TYPE) {
            static_cast<DepthPriceItem*>(m_depth_view->item(i, 0))->setExactInverse(inverse, s.depth[s.depth.size() - 1 - i].price, u.decimals);
        }
        if (m_depth_view->item(i, 1)->foreground().color() != B3Theme::kPositive) m_depth_view->item(i, 1)->setForeground(B3Theme::kPositive);
        if (m_depth_view->item(i, 2)->foreground().color() != B3Theme::kNegative) m_depth_view->item(i, 2)->setForeground(B3Theme::kNegative);
    }
    for (int i{0}; i < static_cast<int>(own_tips.size()); ++i) m_own_view->item(i, 1)->setToolTip(own_tips[i]);
    for (int i{0}; i < static_cast<int>(s.own_curves.size()); ++i) { const auto& c{s.own_curves[i]}; m_own_view->item(i, 0)->setToolTip(tr("Certified account %1\nFilled: %2 %3\nCancellation targets this account's whole selected side.").arg(c.account, FormatAmount(c.filled, u.decimals), u.ticker)); }
    QString history_note{!s.history_available ? tr("History unavailable on this node") : history.empty() ? tr("No trades in recent history") : s.history_truncated || s.history_page_partial ? tr("Recent trades · partial history") : tr("Confirmed trades")};
    if (inverse && !history.empty()) history_note += tr(" · gross B3 before fees");
    QString history_tooltip{tr("Only retained certified clearings are shown. Times are when the serving node observed each auction, never certified; a dash in Time means no observed time, and a dash in a fill column means your fill is unknown, not zero. Missing history does not prove that no trading occurred.") +
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
    if (m_last_price->toolTip() != last_price_tip) m_last_price->setToolTip(last_price_tip);
    m_history_note->setText(history_note); m_history_note->setToolTip(history_tooltip);
    m_chart->setToolTip((reported ? tr("Endpoint-reported price and fill history; execution results are not independently verified.") : tr("Locally verified execution history.")) +
        (m_chart->timeMode()
             ? tr("\nCandles are UTC intervals of operator-observed time: when the serving node applied each certified auction, by its own clock, never certified execution time. Dotted stretches are proven to hold no auction; hatched ones hold auctions without an observed time. Scroll to zoom, drag to pan, double-click for the latest. Depth shows current orders separately.")
             : tr("\nThis server does not report times: candles show open, high, low and close in auction order, not elapsed time. Unfilled intervals remain gaps; the first and latest candle may contain only part of their interval. Depth shows current orders separately.")));
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
    // Marketable limit: the price field shows the helper's exact limit from
    // fresh certified data. Frozen while a review is pending or open; the
    // review itself re-prices from its own fresh read.
    const bool marketable{m_marketable->isChecked()};
    m_price->setReadOnly(marketable);
    if (marketable && !m_busy && !m_deferred_review) {
        const bool fresh{selected && m_snapshot && m_snapshot->market == selected->id && !m_read_failed &&
            AdmissionReady(*m_snapshot, m_response_age.isValid() ? m_response_age.elapsed() : -1, -1, m_queue_age.isValid() ? m_queue_age.elapsed() : -1)};
        QString error;
        const auto limit{fresh ? marketableLimit(&error) : std::nullopt};
        if (!fresh) error = tr("Waiting for fresh certified data from a tradable market; no price is set.");
        if (m_price->text() != (limit ? limit->display : QString{})) { const QSignalBlocker block{m_price}; m_price->setText(limit ? limit->display : QString{}); }
        QString note{error}, note_tip;
        if (limit) {
            // The price field above holds the exact limit; this note only reads it.
            const QString price_units{inverse ? tr("%1 / B3").arg(ticker) : tr("B3 / %1").arg(ticker)};
            // Read as the order book shows them (the best opposite is its row beside the spread).
            const auto& places{m_order_book->pricePlaces()};
            const auto shown{ViewPrice(limit->price, m_snapshot->units, inverse, places)}, reference{ViewPrice(limit->reference, m_snapshot->units, inverse, places)};
            note = tr("Limit %1 %2 · best opposite %3 · at most %4% slippage\nVisible certified depth at or better: %5 %6 (not a fill estimate). Any unfilled part rests until you cancel it.")
                       .arg(shown.text, price_units, reference.text, FormatSlippage(limit->slippage_bps), FormatAmount(limit->visible, m_snapshot->units.decimals), ticker);
            if (shown.approximate || reference.approximate) note_tip = tr("Exact limit: %1 %3\nExact best opposite: %2 %3").arg(limit->display, limit->reference_display, price_units);
        }
        if (m_marketable_note->text() != note) m_marketable_note->setText(note);
        if (m_marketable_note->toolTip() != note_tip) m_marketable_note->setToolTip(note_tip);
    }
    m_marketable_note->setVisible(marketable);
    {
        // Only a view computed for exactly this ticket's order is shown.
        QString line, exact;
        const auto draft{units_known ? indicativeDraft() : UniValue{}};
        if (draft.isObject()) {
            const auto& view{m_snapshot->indicative};
            const bool same{view && view->has_draft && view->draft_side == QString::fromStdString(draft["side"].get_str()) &&
                            view->draft_price == draft["price"].getInt<CAmount>() && view->draft_quantity == draft["quantity"].getInt<CAmount>()};
            line = same ? IndicativeText(*view, m_snapshot->units, inverse, &exact, m_order_book->pricePlaces()) : tr("Indicative price: updating for this order…");
        }
        if (m_indicative->text() != line) m_indicative->setText(line);
        if (const QString tip{IndicativeTip() + (exact.isEmpty() ? QString{} : QStringLiteral("\n") + exact)}; m_indicative->toolTip() != tip) m_indicative->setToolTip(tip);
        m_indicative->setVisible(!line.isEmpty());
    }
    if (!units_known) { m_grid_note->setText(tr("Verified asset precision required; no raw-unit guessing.")); m_grid_note->setToolTip(QString{}); m_ticket_available->setText(tr("Available  —")); m_ticket_total->setText(tr("Order value  —")); apply_tooltips(); return; }
    const auto& u{m_snapshot->units}; m_grid_note->setText(inverse ? tr("Exact %1 quantity cap · no guaranteed B3 fill").arg(ticker) : tr("Steps: %1 %2 · %3 B3").arg(FormatAmount(u.quantity_step, u.decimals), ticker, FormatPrice(u.price_step, u.decimals)));
    m_grid_note->setToolTip(tr("Quantity step %1 %2; canonical price step %3 B3 per %2. Quantity is exact and never rounded.").arg(FormatAmount(u.quantity_step, u.decimals), ticker, FormatPrice(u.price_step, u.decimals)) +
        (inverse ? tr("\nInverse ticks are nonuniform. Prices adjust only in your favor; the exact executable limit is shown before signing. Partial fills and better prices change the actual B3 amount.") : QString{}));
    const CAmount free{bid ? selected->b3_available : selected->base_available}, reserved{bid ? selected->b3_reserved : selected->base_reserved};
    const auto budget{ReplacementBudget(free, reserved)};
    m_ticket_available->setText(tr("Available  %1 %2").arg(FormatAmount(free, bid ? 9 : u.decimals), bid ? QStringLiteral("B3") : ticker));
    available_tooltip = tr("This side already reserves %1 %2; a replacement may use %3 %2 including that reservation.").arg(FormatAmount(reserved, bid ? 9 : u.decimals), bid ? QStringLiteral("B3") : ticker, budget ? FormatAmount(*budget, bid ? 9 : u.decimals) : QStringLiteral("—"));
    QString error;
    if (m_ladder->isChecked()) {
        // One order of several prices: its exact total and reservation.
        const auto steps{ladderSteps(&error)};
        if (!steps) { m_ticket_total->setText(tr("Ladder  —")); total_tooltip = error; apply_tooltips(); return; }
        CAmount total{0};
        for (const auto& step : *steps) total += step.qty;
        const auto reservation{*flowmesh::LadderReservation(bid, *steps)};
        const QString amount{FormatAmount(total, u.decimals)};
        const QString reserves{bid ? tr("Reserves exactly %1 B3 (the sum of quantity x price)").arg(FormatAmount(reservation, 9))
                                   : tr("Reserves %1 %2").arg(FormatAmount(reservation, u.decimals), ticker)};
        // One arg chain per text: the one-price text has no %4 to fill.
        m_ticket_total->setText(steps->size() == 1 ? tr("Ladder of 1 price · %1 %2 in total\n%3").arg(amount, ticker, reserves)
                                                   : tr("Ladder of %4 prices · %1 %2 in total\n%3").arg(amount, ticker, reserves).arg(steps->size()));
        total_tooltip = tr("A ladder is one standing order on this side: each price fills in the same uniform-price auction, a price cannot be cancelled alone, and it replaces your order on this side. With one price it is an ordinary limit order.");
        apply_tooltips();
        return;
    }
    const auto limit{ParseDisplayLimit(m_price->text(), u, inverse, buy, &error)};
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

bool B3FlowMeshTradingPanel::depositDraftAvailable() const
{
    const auto selected{market()};
    if (!m_wallet || !m_backend || m_busy || m_uncertain || !m_security_warning.isEmpty() ||
        !m_saved_actions_ready || m_backend->privateKeysDisabled() || m_deferred_connect ||
        (m_active_result && !m_active_result->connect_url.isEmpty()) || !selected || !m_snapshot) return false;
    const auto& s{*m_snapshot};
    // Opening a draft is not permission to prepare or send funds. Retain the
    // established verified binding across transient pause/read failures only.
    return s.certified && s.running && s.halt == QStringLiteral("none") &&
        s.market == selected->id && s.base == selected->base && s.domain == selected->domain &&
        s.config == selected->config && s.remote == selected->remote &&
        (!selected->has_account || s.account == selected->account) &&
        (!s.remote || (s.certificate_verified && s.account_state_verified));
}

UniValue B3FlowMeshTradingPanel::indicativeDraft() const
{
    using namespace B3FlowMeshMarketData;
    // A what-if is computed for a limit order only, never a ladder.
    if (m_ladder && m_ladder->isChecked()) return {};
    const auto selected{market()};
    if (!selected || !m_snapshot || m_snapshot->market != selected->id || !m_snapshot->units.known || m_snapshot->units.asset != selected->base) return {};
    const bool buy{m_side->currentIndex() == 0}, inverse{inverted()};
    const auto limit{ParseDisplayLimit(m_price->text(), m_snapshot->units, inverse, buy)};
    const auto quantity{ParseQuantity(m_quantity->text(), m_snapshot->units)};
    if (!limit || !quantity || limit->price <= 0 || *quantity <= 0) return {};
    UniValue draft{UniValue::VOBJ};
    draft.pushKV("side", CanonicalSide(buy, inverse).toStdString());
    draft.pushKV("price", limit->price);
    draft.pushKV("quantity", *quantity);
    return draft;
}

std::optional<std::vector<flowmesh::LadderStep>> B3FlowMeshTradingPanel::ladderSteps(QString* error, bool* adjusted) const
{
    using namespace B3FlowMeshMarketData;
    const auto fail = [&](const QString& text) -> std::optional<std::vector<flowmesh::LadderStep>> {
        if (error) *error = text;
        return std::nullopt;
    };
    if (adjusted) *adjusted = false;
    const auto selected{market()};
    if (!selected || !m_snapshot || m_snapshot->market != selected->id || !m_snapshot->units.known) return fail(tr("Verified asset precision is required."));
    const auto& units{m_snapshot->units};
    const bool buy{m_side->currentIndex() == 0}, inverse{inverted()};
    // The prices a view, market or wallet change clears are exactly the ones read here.
    const auto prices{ticketPriceFields()};
    std::vector<std::pair<QLineEdit*, QLineEdit*>> rows{{prices[0], m_quantity}};
    for (size_t i{0}; i < m_ladder_quantities.size(); ++i) rows.emplace_back(prices[i + 1], m_ladder_quantities[i]);
    std::vector<flowmesh::LadderStep> steps;
    for (size_t i{0}; i < rows.size(); ++i) {
        const QString price{rows[i].first->text().trimmed()}, quantity{rows[i].second->text().trimmed()};
        if (i > 0 && price.isEmpty() && quantity.isEmpty()) continue;
        if (price.isEmpty() || quantity.isEmpty()) return fail(tr("Enter both a price and a quantity for price %1.").arg(i + 1));
        QString why;
        const auto limit{ParseDisplayLimit(price, units, inverse, buy, &why)};
        if (!limit) return fail(tr("Price %1: %2").arg(i + 1).arg(why));
        const auto amount{ParseQuantity(quantity, units, &why)};
        if (!amount) return fail(tr("Quantity %1: %2").arg(i + 1).arg(why));
        if (std::any_of(steps.begin(), steps.end(), [&](const auto& step) { return step.price == limit->price; })) {
            return fail(tr("Each price of a ladder must differ (price %1 repeats one above).").arg(i + 1));
        }
        if (adjusted && limit->adjusted) *adjusted = true;
        steps.push_back({limit->price, *amount});
    }
    if (!flowmesh::LadderReservation(CanonicalSide(buy, inverse) == QStringLiteral("bid"), steps)) {
        return fail(tr("The ladder's total exceeds the supported market range."));
    }
    return steps;
}

QStringList B3FlowMeshTradingPanel::ladderText() const
{
    QStringList out;
    for (size_t i{0}; i < m_ladder_prices.size(); ++i) out << m_ladder_prices[i]->text() << m_ladder_quantities[i]->text();
    return out;
}

std::array<QLineEdit*, flowmesh::FLOWMESH_LADDER_MAX_STEPS> B3FlowMeshTradingPanel::ticketPriceFields() const
{
    static_assert(std::tuple_size_v<decltype(m_ladder_prices)> + 1 == flowmesh::FLOWMESH_LADDER_MAX_STEPS);
    static_assert(std::tuple_size_v<decltype(m_ladder_quantities)> == std::tuple_size_v<decltype(m_ladder_prices)>);
    std::array<QLineEdit*, flowmesh::FLOWMESH_LADDER_MAX_STEPS> fields{};
    fields[0] = m_price;
    std::copy(m_ladder_prices.begin(), m_ladder_prices.end(), fields.begin() + 1);
    return fields;
}

void B3FlowMeshTradingPanel::clearTicketPrices()
{
    // setText, never clear(): clear() is an undoable edit, and on a field
    // that is already empty it keeps the redo, so Undo or Redo would bring
    // the old text back to be read in the other unit. setText drops the
    // whole history, even when the field is already empty.
    for (auto* field : ticketPriceFields()) field->setText(QString{});
}

void B3FlowMeshTradingPanel::clearTicketEntries()
{
    // A market's prices are B3 per its token and its quantities are that
    // token: none is read in another market. setText, like clearTicketPrices.
    clearTicketPrices();
    m_quantity->setText(QString{});
    for (auto* field : m_ladder_quantities) field->setText(QString{});
}

flowmesh::LevelGrouping B3FlowMeshTradingPanel::bookGrouping() const
{
    // Only a grouping for the displayed market and orientation applies.
    const auto selected{market()};
    if (!selected) return {};
    const auto it{m_book_groupings.find({selected->id, inverted()})};
    return it == m_book_groupings.end() ? flowmesh::LevelGrouping{} : it->second;
}

void B3FlowMeshTradingPanel::selectBookGrouping(const flowmesh::LevelGrouping& grouping)
{
    const auto selected{market()};
    if (!selected || (!grouping.exact && grouping.inverse != inverted())) return;
    m_book_groupings[{selected->id, inverted()}] = grouping;
    updateDataViews();
    // Read now: the node answers in this grouping even at the same head.
    if (!m_busy) refresh();
}

UniValue B3FlowMeshTradingPanel::levelsRequest() const
{
    const auto selected{market()};
    if (!selected) return {};
    const auto grouping{bookGrouping()};
    UniValue levels{UniValue::VOBJ};
    levels.pushKV("depth", static_cast<uint64_t>(flowmesh::MARKET_DATA_MAX_LEVELS));
    if (!grouping.exact) {
        UniValue group{UniValue::VOBJ};
        group.pushKV("orientation", grouping.inverse ? "inverse" : "canonical");
        group.pushKV("mantissa", grouping.mantissa);
        group.pushKV("exponent", grouping.exponent);
        group.pushKV("decimals", grouping.decimals);
        levels.pushKV("group", std::move(group));
    }
    // An unchanged reply carries levels only when asked: after a grouping
    // change, or when the head's levels are missing or were not copied.
    const auto& shown{displayedSnapshot()};
    const bool displayed{shown && shown->market == selected->id};
    const auto* held{displayed && shown->levels ? &*shown->levels : nullptr};
    if (displayed && (!held || held->grouping != grouping || (!held->available && held->reason == QStringLiteral("head_changed")))) {
        levels.pushKV("include_unchanged", true);
    }
    return levels;
}

std::optional<uint64_t> B3FlowMeshTradingPanel::ticksAfter(const QString& market) const
{
    if (m_ticks.empty() || m_ticks_market != market) return std::nullopt;
    return m_ticks.back().sequence;
}

std::optional<flowmesh::ObservedStamp> B3FlowMeshTradingPanel::observedStamp(const uint64_t sequence, const QString& hash) const
{
    const auto& shown{displayedSnapshot()};
    if (!shown) return std::nullopt;
    const auto matches = [&](const flowmesh::ObservedStamp& stamp) {
        return hash.isEmpty() || QString::fromStdString(stamp.hash.GetHex()) == hash.toLower();
    };
    if (m_ticks_market == shown->market) {
        if (const auto held{m_tick_times.find(sequence)}; held != m_tick_times.end() && matches(held->second)) return held->second;
    }
    if (shown->observed && shown->observed->available) {
        for (const auto& row : shown->observed->rows) if (row.sequence == sequence && matches(row)) return row;
    }
    return std::nullopt;
}

QString B3FlowMeshTradingPanel::observedClock(const uint64_t sequence, const QString& hash) const
{
    const auto stamp{observedStamp(sequence, hash)};
    if (!stamp || stamp->mode != flowmesh::ObservedMode::LIVE) return {};
    return QDateTime::fromMSecsSinceEpoch(stamp->ms).toLocalTime().toString(QStringLiteral("HH:mm:ss"));
}

void B3FlowMeshTradingPanel::updateIntervalChoices(const bool time)
{
    auto* combo{m_candle_interval_combo};
    if (!combo) return;
    const bool shows_time{combo->count() > 0 && combo->itemData(0, Qt::UserRole + 1).toString().startsWith(QLatin1Char('t'))};
    if (combo->count() > 0 && shows_time == time) return;
    std::vector<Choice> choices;
    QString selected;
    if (time) {
        for (const auto interval : flowmesh::CANDLE_INTERVALS) {
            const QString name{QString::fromLatin1(flowmesh::CandleIntervalName(interval))};
            choices.push_back({name, static_cast<int>(interval), QLatin1Char('t') + name});
        }
        selected = QLatin1Char('t') + QString::fromLatin1(flowmesh::CandleIntervalName(m_candle_interval));
        combo->setAccessibleName(tr("Candle interval (UTC)"));
        combo->setToolTip(tr("UTC candles in operator-observed time: when the serving node applied each certified auction, by its own clock. Not certified execution time; auctions it applied late are shown only within their bounds."));
    } else {
        for (const int count : {1, 5, 20}) choices.push_back({B3FlowMeshChart::AuctionOrderLabel(count), count, QStringLiteral("s%1").arg(count)});
        selected = QStringLiteral("s%1").arg(m_chart ? m_chart->candleInterval() : 5);
        combo->setAccessibleName(tr("Auctions per candlestick"));
        combo->setToolTip(tr("This server does not report times: candles group actual clearings in auction order. These are not minutes."));
    }
    SetChoices(combo, choices, selected, true);
}

void B3FlowMeshTradingPanel::mergeHeldTicks()
{
    // The held ticks follow what the views show (m_display while busy).
    const auto& shown{displayedSnapshot()};
    if (!shown) return;
    if (m_ticks_market != shown->market) {
        m_ticks.clear();
        m_tick_times.clear();
        m_received_times.clear();
        m_ticks_market = shown->market;
    }
    if (shown->ticks && shown->ticks->available && !B3FlowMeshMarketData::MergeHeldTicks(m_ticks, *shown->ticks)) {
        // Replaced ticks start their times again.
        m_tick_times.clear();
        m_received_times.clear();
    }
    const auto* observed{shown->observed && shown->observed->available ? &*shown->observed : nullptr};
    if (observed && observed->source != m_tick_times_source) {
        // One source's series only: another endpoint's clock is never mixed in.
        m_tick_times.clear();
        m_tick_times_source = observed->source;
    }
    B3FlowMeshMarketData::MergeHeldTimes(m_tick_times, m_ticks, observed);
    // Receipt times only while no observed times exist, and only for held ticks.
    if (observed || m_ticks.empty()) m_received_times.clear();
    else std::erase_if(m_received_times, [&](const auto& item) { return item.first < m_ticks.front().sequence || item.first > m_ticks.back().sequence; });
}

QString B3FlowMeshTradingPanel::candlesKey(const QString& market, const QString& source) const
{
    return market + QLatin1Char('|') + QString::fromUtf8(flowmesh::CandleIntervalName(m_candle_interval)) + QLatin1Char('|') + source;
}

const B3FlowMeshTradingPanel::CandlesFailure* B3FlowMeshTradingPanel::candlesFailure(const QString& market, const QString& source) const
{
    return m_candles_failure && m_candles_failure->key == candlesKey(market, source) ? &*m_candles_failure : nullptr;
}

void B3FlowMeshTradingPanel::noteCandlesFailure(const QString& error)
{
    if (!m_snapshot || !m_snapshot->observed) return;
    const QString key{candlesKey(m_snapshot->market, m_snapshot->observed->source)};
    if (!m_candles_failure || m_candles_failure->key != key) m_candles_failure = CandlesFailure{.key = key};
    auto& failure{*m_candles_failure};
    failure.error = error.left(200);
    failure.failures = std::min(failure.failures + 1, 16);
    const qint64 backoff{std::min(CANDLES_RETRY_MAX_MS, CANDLES_RETRY_MIN_MS << std::min(failure.failures - 1, 5))};
    failure.retry_at = m_watch_clock.elapsed() + backoff;
}

std::optional<std::pair<flowmesh::CandleInterval, size_t>> B3FlowMeshTradingPanel::candlesDue() const
{
    const auto selected{market()};
    if (!selected || !m_snapshot || m_snapshot->market != selected->id || !m_snapshot->observed || !m_snapshot->observed->available) return std::nullopt;
    // After a failed read, not before its back-off.
    if (const auto* failure{candlesFailure(m_snapshot->market, m_snapshot->observed->source)}; failure && m_watch_clock.elapsed() < failure->retry_at) return std::nullopt;
    constexpr size_t WINDOW{CANDLES_WINDOW}, NEWEST{CANDLES_NEWEST};
    if (!m_candles || m_candles_market != m_snapshot->market || m_candles->interval != m_candle_interval ||
        m_candles->source != m_snapshot->observed->source || !m_candles_age.isValid()) {
        return std::pair{m_candle_interval, WINDOW};
    }
    // The held ticks did not continue the window: read it again, at most
    // every 2 s.
    if (m_candles_refetch && m_candles_age.elapsed() >= 2'000) return std::pair{m_candle_interval, WINDOW};
    // Empty newest pages left auctions unaccounted: the whole window once
    // CANDLES_FULL_REFRESH_MS have passed since the last one.
    if (m_candles_unproven && (!m_candles_window_at || m_watch_clock.elapsed() - *m_candles_window_at >= CANDLES_FULL_REFRESH_MS)) {
        return std::pair{m_candle_interval, WINDOW};
    }
    if (m_candles_age.elapsed() >= 10'000) return std::pair{m_candle_interval, NEWEST};
    return std::nullopt;
}

void B3FlowMeshTradingPanel::applyCandles(const B3FlowMeshMarketData::CandlePage& page, const size_t limit, const std::optional<uint64_t> read_next)
{
    if (!m_snapshot) return;
    const bool same{m_candles && m_candles_market == m_snapshot->market && m_candles->interval == page.interval &&
                    m_candles->source == page.source && m_candles->available && page.available};
    if (limit < CANDLES_WINDOW && page.available) {
        // The newest intervals only, merged only where they continue the
        // held window of this very source: the page reaches back over the
        // held newest candle (which may have grown since it was read), or
        // its first candle directly follows that one's last auction, or
        // nothing was held before the page starts. Otherwise (the panel was
        // hidden, the machine slept, reads failed for longer than the page
        // covers, or the source changed while it was read) the intervals in
        // between would read as auctions without an observed time, which
        // the source does have: the whole window is read now instead.
        const auto reaches = [&] {
            if (!same) return false;
            const auto& held{m_candles->candles};
            if (held.empty()) return page.from_ms <= m_candles->before_ms;
            if (page.from_ms <= held.back().open_ms) return true;
            return !page.candles.empty() && held.back().last_sequence < std::numeric_limits<uint64_t>::max() &&
                   page.candles.front().first_sequence == held.back().last_sequence + 1;
        };
        if (!reaches()) {
            // An empty page of the same source: no auction with an observed
            // time in its intervals. A quiet market (an endpoint that ends
            // its newest window at its own clock serves such pages once no
            // auction happened for three intervals) has nothing to read
            // again: when nothing was certified after what the held window
            // accounts for, up to the verified head this read followed (a
            // newer head only makes this less likely), the window is
            // current and continues. Otherwise auctions are unaccounted
            // (the held newest interval may have grown after it was read,
            // or they have no observed time): the whole window is read, at
            // most once per CANDLES_FULL_REFRESH_MS, and continues until
            // then.
            if (same && page.candles.empty()) {
                const auto& held{m_candles->candles};
                const uint64_t accounted{std::max(m_candles_next, held.empty() || held.back().last_sequence == std::numeric_limits<uint64_t>::max()
                                                                      ? uint64_t{0} : held.back().last_sequence + 1)};
                const bool quiet{m_snapshot->next_sequence <= accounted};
                const bool recent{m_candles_window_at && m_watch_clock.elapsed() - *m_candles_window_at < CANDLES_FULL_REFRESH_MS};
                if (quiet || recent) {
                    // Complete up to the page's end only when proven so.
                    if (quiet) m_candles->before_ms = std::max(m_candles->before_ms, page.before_ms);
                    m_candles_unproven |= !quiet;
                    m_candles_age.start();
                    m_candles_failure.reset();
                    return;
                }
            }
            m_candles_age.invalidate();
            m_candles_failure.reset();
            return;
        }
        auto merged{*m_candles};
        std::erase_if(merged.candles, [&](const auto& candle) { return candle.open_ms >= page.from_ms; });
        merged.candles.insert(merged.candles.end(), page.candles.begin(), page.candles.end());
        merged.before_ms = std::max(merged.before_ms, page.before_ms);
        m_candles = std::move(merged);
        if (read_next) m_candles_next = std::max(m_candles_next, *read_next);
    } else {
        m_candles = page;
        m_candles_next = page.available && read_next ? *read_next : 0;
        m_candles_unproven = false;
        if (limit >= CANDLES_WINDOW) m_candles_window_at = m_watch_clock.elapsed();
    }
    m_candles_market = m_snapshot->market;
    m_candles_age.start();
    m_candles_refetch = false;
    m_candles_failure.reset();
}

bool B3FlowMeshTradingPanel::depositReady() const
{
    return depositDraftAvailable() && market()->ready && !m_read_failed &&
        B3FlowMeshMarketData::AdmissionReady(*m_snapshot, m_response_age.isValid() ? m_response_age.elapsed() : -1,
            m_certificate_age.isValid() ? m_certificate_age.elapsed() : -1, m_queue_age.isValid() ? m_queue_age.elapsed() : -1);
}

bool B3FlowMeshTradingPanel::withdrawalDraftAvailable() const
{
    if (!depositDraftAvailable()) return false;
    const auto selected{market()};
    if (!selected->has_account || (m_pending_sequence && selected->id == m_pending_market &&
        selected->account == m_pending_account && (m_receipt || selected->sequence <= *m_pending_sequence))) return false;
    return true; // Historical outbox rows do not undo explicit uncertain-outcome acknowledgement.
}

bool B3FlowMeshTradingPanel::withdrawalReady() const
{
    return withdrawalDraftAvailable() && depositReady();
}

bool B3FlowMeshTradingPanel::orderReviewAvailable() const
{
    return withdrawalDraftAvailable() && m_snapshot->units.known && m_snapshot->units.asset == market()->base;
}

bool B3FlowMeshTradingPanel::orderReady() const
{
    return orderReviewAvailable() && depositReady();
}

bool B3FlowMeshTradingPanel::dataReady() const
{
    const auto selected{market()};
    return selected && m_snapshot && m_snapshot->market == selected->id && !m_read_failed &&
           B3FlowMeshMarketData::AdmissionReady(*m_snapshot, m_response_age.isValid() ? m_response_age.elapsed() : -1,
                                                m_certificate_age.isValid() ? m_certificate_age.elapsed() : -1,
                                                m_queue_age.isValid() ? m_queue_age.elapsed() : -1);
}

std::optional<B3FlowMeshMarketData::Curve> B3FlowMeshTradingPanel::editCurve() const
{
    const auto selected{market()};
    if (m_edit_side.isEmpty() || !selected || !m_snapshot || m_snapshot->market != selected->id || !m_snapshot->certified) return std::nullopt;
    for (const auto& own : m_snapshot->own_curves) {
        if (own.side == m_edit_side) return own.points == m_edit_points ? std::optional{own} : std::nullopt;
    }
    return std::nullopt;
}

bool B3FlowMeshTradingPanel::editAvailable() const
{
    // Nothing pending or uncertain: an edit never re-signs a request whose
    // outcome is unknown, and never races one in flight (m_busy, the pending
    // gate and m_deferred_review). Like the ticket, it stays available while
    // a read runs: it only prefills the ticket and signs nothing.
    if (!orderReviewAvailable() || m_deferred_review) return false;
    const auto curve{editCurve()};
    if (!curve || curve->remaining <= 0) return false;
    // A limit order, or a ladder order: the prices it still has quantity at.
    const auto steps{B3FlowMeshMarketData::LadderStepsOf(*curve)};
    return steps && std::any_of(steps->begin(), steps->end(), [](const auto& step) { return step.price > 0 && step.remaining > 0; }) &&
           std::all_of(steps->begin(), steps->end(), [](const auto& step) { return step.price > 0 || step.remaining == 0; });
}

void B3FlowMeshTradingPanel::editOrder()
{
    if (!editAvailable()) {
        notice(tr("The selected order changed or cannot be edited now. Nothing was prefilled, signed or sent; select it again."));
        return;
    }
    const auto curve{*editCurve()};
    const auto& units{m_snapshot->units};
    const bool inverse{inverted()}, buy{B3FlowMeshMarketData::DisplayBuy(curve.side, inverse)};
    // The prices it still has quantity at, best first, with what remains.
    auto steps{*B3FlowMeshMarketData::LadderStepsOf(curve)};
    std::erase_if(steps, [](const auto& step) { return step.remaining == 0; });
    const bool bid{curve.side == QStringLiteral("bid")};
    std::sort(steps.begin(), steps.end(), [&](const auto& a, const auto& b) { return bid ? a.price > b.price : a.price < b.price; });
    const auto text = [&](CAmount price) {
        return inverse ? B3FlowMeshMarketData::ExactInversePrice(price, units.decimals) : B3FlowMeshMarketData::FormatPrice(price, units.decimals);
    };
    m_marketable->setChecked(false);
    m_side->setCurrentIndex(buy ? 0 : 1); (buy ? m_buy : m_sell)->setChecked(true);
    m_ladder->setChecked(steps.size() > 1);
    m_price->setText(text(steps.front().price));
    m_quantity->setText(B3FlowMeshMarketData::FormatAmount(steps.front().remaining, units.decimals));
    for (size_t i{0}; i < m_ladder_prices.size(); ++i) {
        const bool used{i + 1 < steps.size()};
        m_ladder_prices[i]->setText(used ? text(steps[i + 1].price) : QString{});
        m_ladder_quantities[i]->setText(used ? B3FlowMeshMarketData::FormatAmount(steps[i + 1].remaining, units.decimals) : QString{});
    }
    updateTicket();
    m_price->setFocus();
    const QString side{inverse ? (buy ? tr("Buy B3") : tr("Sell B3")) : (buy ? tr("Buy") : tr("Sell"))};
    if (steps.size() > 1) {
        notice(tr("Edit prefilled a replacement for your %1 ladder: its %2 prices with what remains at each. Nothing was signed or sent. Change any price or quantity, then review it: the new ladder replaces that side's whole order and its quantities count from zero.")
                   .arg(side).arg(steps.size()));
    } else {
        notice(tr("Edit prefilled a replacement for your %1 order: %2 %3 at its current limit. Nothing was signed or sent. Change the price or quantity, then review it: the new order replaces that side's order and its quantity counts from zero.")
                   .arg(side, m_quantity->text(), units.ticker));
    }
}

void B3FlowMeshTradingPanel::fillFromBook(const QString& shown, const CAmount canonical, const CAmount cumulative, const QString& head)
{
    using namespace B3FlowMeshMarketData;
    const auto selected{market()};
    // The ticket's fields are frozen while an operation or a review waits.
    if (m_busy || m_deferred_review) return;
    // Critique B2: the price goes into a field that is signed after review,
    // so it comes only from the data the gates read, shown as it is now.
    const auto refuse = [this] {
        notice(tr("The order book did not show current, ready verified data when clicked; its price was not used. Nothing was changed, signed or sent."));
    };
    if (!selected || !m_snapshot || m_snapshot->market != selected->id || m_snapshot->head != head ||
        !m_snapshot->units.known || m_snapshot->units.asset != selected->base ||
        m_order_book->shownInverse() != inverted() || !dataReady()) {
        refuse();
        return;
    }
    const auto& units{m_snapshot->units};
    const bool buy{m_side->currentIndex() == 0}, inverse{inverted()};
    // Re-derived from the verified data, never taken from the book's text.
    if (canonical <= 0 || !BookOffersPrice(*m_snapshot, canonical, inverse, bookGrouping())) {
        refuse();
        return;
    }
    // Hyperliquid's click-to-price: the price as shown, when the ticket
    // reads it back for its side as exactly the level's canonical price (a
    // rounded B3-view label is then adjusted only in the customer's favour,
    // and the ticket and review say so); else the same or more places
    // rounded toward this side; else the exact price. Never a different
    // canonical price.
    const auto reads_back = [&](const QString& text) {
        const auto limit{ParseDisplayLimit(text, units, inverse, buy)};
        return limit && limit->price == canonical;
    };
    const auto dot{shown.indexOf(QLatin1Char('.'))};
    const int shown_places{dot < 0 ? 0 : static_cast<int>(shown.size() - dot - 1)};
    // A level's label is its price rounded to the nearest value (the view's
    // one rule; grouped rows show an edge instead): more places must still
    // round to it, so the ticket shows the price the row, Your orders and
    // the spread show, never a neighbouring row's (1.0031 for a row shown
    // 1.003, not 1.004).
    const bool nearest_label{(!m_order_book->showsVerifiedLevels() || bookGrouping().exact) && RoundedDecimal(shown, shown_places) == shown};
    const auto shows_as_label = [&](const QString& text) { return !nearest_label || RoundedDecimal(text, shown_places) == shown; };
    QString price;
    if (!shown.isEmpty() && reads_back(shown)) price = shown;
    for (int places{shown_places}; price.isEmpty() && places <= 18; ++places) {
        const auto rounded{FixedDisplayPrice(canonical, units.decimals, inverse, places, /*up=*/buy)};
        if (!rounded.isEmpty() && reads_back(rounded) && shows_as_label(rounded)) price = rounded;
    }
    if (price.isEmpty()) {
        const auto exact{inverse ? ExactInversePrice(canonical, units.decimals) : FormatPrice(canonical, units.decimals)};
        if (reads_back(exact)) price = exact;
    }
    if (price.isEmpty()) {
        notice(tr("That book level's price cannot be entered exactly for this side. Nothing was changed, signed or sent."));
        return;
    }
    m_marketable->setChecked(false);
    m_price->setText(price);
    if (B3FlowMeshOrderBook::SizeOnClick() && cumulative > 0) {
        // Within the verified available balance, including this side's
        // reservation (a new order replaces it).
        const bool bid{CanonicalSide(buy, inverse) == QStringLiteral("bid")};
        const auto budget{ReplacementBudget(bid ? selected->b3_available : selected->base_available, bid ? selected->b3_reserved : selected->base_reserved)};
        CAmount quantity{cumulative};
        if (budget) quantity = std::min(quantity, bid ? *budget / canonical : *budget);
        quantity -= quantity % std::max<CAmount>(1, units.quantity_step);
        if (quantity > 0) m_quantity->setText(FormatAmount(quantity, units.decimals));
    }
    updateTicket();
}

std::optional<B3FlowMeshMarketData::MarketableLimit> B3FlowMeshTradingPanel::marketableLimit(QString* error) const
{
    const auto selected{market()};
    if (!selected || !m_snapshot || m_snapshot->market != selected->id || m_snapshot->units.asset != selected->base) {
        if (error) *error = tr("Certified market data is required.");
        return std::nullopt;
    }
    const auto bps{B3FlowMeshMarketData::ParseSlippageBps(m_slippage->text(), error)};
    if (!bps) return std::nullopt;
    const auto quantity{B3FlowMeshMarketData::ParseQuantity(m_quantity->text(), m_snapshot->units)};
    return B3FlowMeshMarketData::ComputeMarketableLimit(*m_snapshot, m_side->currentIndex() == 0, inverted(), *bps, quantity, error);
}

void B3FlowMeshTradingPanel::requestOrderReview()
{
    if (!orderReviewAvailable()) return;
    // The stable entry requests only a read. Approval still requires current
    // readiness; a paused or failed refresh consumes this intent, not retries it.
    if (!m_thread) startJob(std::nullopt, std::nullopt, false, false, std::nullopt, {}, true);
    if (deferReview(Operation::Order))
        notice(tr("Checking current market readiness for this exact order. Nothing is approved, signed or sent; if the check fails, review again. There is no automatic retry."));
}

void B3FlowMeshTradingPanel::openFunding(bool withdrawal)
{
    if (!m_wallet || !m_backend || m_busy || !m_security_warning.isEmpty() || m_uncertain) return;
    if (withdrawal ? !withdrawalDraftAvailable() : !depositDraftAvailable()) return;
    if (m_thread) { deferReview(withdrawal ? Operation::Withdraw : Operation::Deposit, true); return; }
    const auto selected{market()}; if (!selected) return;
    const bool known{m_snapshot && m_snapshot->market == selected->id && m_snapshot->units.known};
    const QString ticker{known ? m_snapshot->units.ticker : tr("Base asset (precision unavailable)")};
    const auto units{m_snapshot ? m_snapshot->units : B3FlowMeshMarketData::Units{}};
    const auto generation{m_generation}; const QPointer<B3FlowMeshTradingPanel> self{this};
    m_busy = true; updateControls();
    m_funding_dialog = new QDialog{this}; const QPointer<QDialog> dialog{m_funding_dialog}; dialog->setWindowTitle(withdrawal ? tr("Withdraw from FlowMesh") : tr("Deposit to FlowMesh")); dialog->setMinimumWidth(430);
    auto* layout{new QVBoxLayout{dialog}}; layout->addWidget(Label(tr("Wallet: %1\nMarket view: %2\nCanonical base asset ID: %3").arg(m_wallet_name, (inverted() ? QStringLiteral("B3 / %1") : QStringLiteral("%1 / B3")).arg(ticker), selected->base), dialog));
    auto* tabs{new QTabWidget{dialog}}; auto* transfer{new QWidget{tabs}}; auto* form{new QFormLayout{transfer}};
    auto* asset{new QComboBox{transfer}}; asset->setObjectName(QStringLiteral("flowMeshFundingDraftAsset")); asset->addItems({ticker, QStringLiteral("B3")}); asset->setCurrentIndex(withdrawal && !m_withdrawal_draft_amount.isEmpty() ? m_withdrawal_draft_asset : m_asset->currentIndex()); form->addRow(tr("Asset"), asset);
    auto* amount{new QLineEdit{transfer}}; amount->setObjectName(QStringLiteral("flowMeshFundingDraftAmount")); amount->setMaxLength(64); amount->setPlaceholderText(tr("Amount in whole tokens, e.g. 1.25")); amount->setText(withdrawal ? m_withdrawal_draft_amount : m_amount->text()); form->addRow(tr("Amount"), amount);
    auto* destination{new QLineEdit{transfer}}; destination->setObjectName(QStringLiteral("flowMeshFundingDraftDestination")); destination->setMaxLength(256); destination->setText(m_destination->text());
    if (withdrawal) form->addRow(tr("B3 destination address"), destination); else destination->hide();
    form->addRow(Label(withdrawal ? tr("This requests a certified withdrawal; it is not an immediate payout. A checkpoint and a separately reviewed on-chain payout follow. No automatic fee spending.") : tr("A new deposit locks funds in a keyless vault. It needs 31 confirmations before admission and may stay locked without validator quorum. You review the actual B3 network fee before any broadcast."), transfer));
    layout->addWidget(Label(withdrawal ? tr("This is a withdrawal draft, not permission to withdraw. Continue checks current market readiness and your available balance once before opening a review. If the market is still paused or unavailable, nothing is signed or sent; your amount and destination are retained for reopening. There is no automatic retry.") : tr("This is a draft, not permission to deposit. Continue checks current market readiness once before opening a review. If the market is still paused or unavailable, nothing is prepared or sent; your amount is retained for reopening. There is no automatic retry."), dialog));
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
    if (accepted) {
        if (withdrawal) { m_destination->setText(destination->text()); m_withdrawal_draft_amount = amount->text(); m_withdrawal_draft_asset = asset->currentIndex(); }
        else { m_asset->setCurrentIndex(asset->currentIndex()); m_amount->setText(amount->text()); m_deposit_txid->setText(txid->text()); m_deposit_vout->setText(vout->text()); }
    }
    if (dialog) delete dialog.data(); m_busy = false; updateControls();
    if (accepted) {
        // The modal draft pauses background reads. Refresh once after Continue,
        // then reuse the existing review-only continuation, never an action queue.
        m_catalog_age.invalidate(); resetRefreshSchedule();
        startJob(std::nullopt, std::nullopt, false, false, std::nullopt, {}, true);
        if (deferReview(admission ? Operation::Admit : withdrawal ? Operation::Withdraw : Operation::Deposit))
            m_deferred_review->funding_context = DeferredReview::FundingContext{*selected, units,
                withdrawal ? m_withdrawal_draft_amount : m_amount->text(), withdrawal ? m_destination->text() : QString{},
                withdrawal ? m_withdrawal_draft_asset : m_asset->currentIndex()};
    }
}

void B3FlowMeshTradingPanel::updateControls()
{
    const auto selected{market()};
    const bool idle{m_wallet && m_backend && !m_busy && m_security_warning.isEmpty()};
    // A read does not disable the ticket. A click waits for that read before
    // opening its review; writes remain serialized by m_busy and m_thread.
    const bool connecting{m_deferred_connect || (m_active_result && !m_active_result->connect_url.isEmpty())};
    const bool signing{idle && !connecting && m_saved_actions_ready && !m_uncertain && !m_backend->privateKeysDisabled()};
    const bool data_ready{dataReady()};
    const bool ready{signing && selected && selected->ready && data_ready};
    const bool settlement_ready{selected && m_snapshot && m_snapshot->market == selected->id &&
        !m_read_failed && m_response_age.isValid() && m_response_age.elapsed() <= 3000 &&
        (!m_snapshot->remote || (m_snapshot->certificate_verified && m_snapshot->account_state_verified)) && m_snapshot->running && !m_snapshot->chain_reconciling};
    const bool pending{selected && m_pending_sequence && selected->id == m_pending_market && selected->account == m_pending_account && (m_receipt || selected->sequence <= *m_pending_sequence)};
    m_refresh->setEnabled(idle && !m_thread); m_market->setEnabled(idle); m_orientation->setEnabled(idle);
    for (auto* field : {m_price, m_quantity, m_amount, m_destination, m_deposit_txid, m_deposit_vout}) field->setEnabled(idle);
    m_side->setEnabled(idle); m_asset->setEnabled(idle); m_effect->setEnabled(idle);
    m_buy->setEnabled(idle); m_sell->setEnabled(idle);
    m_marketable->setEnabled(idle && !m_ladder->isChecked()); m_slippage->setEnabled(idle);
    m_ladder->setEnabled(idle && !m_marketable->isChecked());
    for (auto* field : m_ladder_prices) field->setEnabled(idle);
    for (auto* field : m_ladder_quantities) field->setEnabled(idle);
    m_order->setEnabled(orderReviewAvailable()); m_cancel_order->setEnabled(ready && selected->has_account && !pending);
    m_edit_order->setEnabled(editAvailable());
    m_order->setToolTip(tr("Review first checks fresh market readiness and your certified balance. This button does not mean trading is ready. A failed or paused check signs and sends nothing; there is no automatic retry."));
    m_withdraw->setEnabled(withdrawalDraftAvailable()); m_deposit->setEnabled(depositDraftAvailable());
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
    // The views' stale label follows what they show: while busy, live
    // updates for display confirm it; the gates keep their own freshness.
    const bool display_live{m_busy && !m_read_failed && m_display_confirmed.isValid() && m_display_confirmed.elapsed() <= 3000};
    const bool views_stale{m_snapshot && (m_read_failed || !m_response_age.isValid() || m_response_age.elapsed() > 3000) && !display_live};
    m_chart->setStale(views_stale);
    // The time axis reaches now between replies (it repaints only when a
    // new bucket starts); display only.
    m_chart->setNowMs(QDateTime::currentMSecsSinceEpoch());
    m_order_book->setStale(views_stale);
    m_chart->setLoading(m_loading);
    updateConnectionState();
    updateLinkBanner();
    updatePendingView();
}

void B3FlowMeshTradingPanel::refresh()
{
    if (!m_wallet || !m_backend || m_busy || m_thread) return;
    if (m_deferred_connect) { resumeConnect(); return; }
    if (m_deferred_status) { resumeStatusRead(); return; }
    if (m_read_failures && m_attempt_age.isValid() && m_attempt_age.elapsed() < readBackoffMs()) return;
    startJob();
}

void B3FlowMeshTradingPanel::resetRefreshSchedule()
{
    m_refresh_phase = RefreshPhase::Catalog; m_refresh_yields = 0; ++m_refresh_epoch;
}

void B3FlowMeshTradingPanel::requestConnect()
{
    if (!m_connect->isEnabled() || !m_wallet || !m_backend || m_busy || m_cancel->load()) return;
    if (m_active_result && !m_active_result->connect_url.isEmpty()) return;
    m_deferred_review.reset(); // A reviewed market choice never follows an endpoint change.
    resetRefreshSchedule();
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
    if (m_wallet && m_client_info) {
        // How the selected market's data arrives; never a readiness claim.
        if (const auto live{liveUpdateText()}; !live.isEmpty()) status += tr(" · %1").arg(live);
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
    if (operation == Operation::Order && !orderReady()) {
        notice(tr("Market status changed or an account request is pending. Nothing was signed or submitted; review the order again when ready.")); return;
    }
    if (operation == Operation::Deposit && !depositReady()) {
        notice(tr("Market status changed. Nothing was prepared or submitted; reopen the deposit draft when ready.")); return;
    }
    if (operation == Operation::Withdraw && !withdrawalReady()) {
        notice(tr("Market status changed or an account request is pending. Nothing was signed or submitted; reopen the withdrawal draft when ready.")); return;
    }
    Action a; a.operation = operation; a.market = *selected; a.inverse_display = inverted();
    a.side = B3FlowMeshMarketData::CanonicalSide(m_side->currentIndex() == 0, a.inverse_display); a.native = (operation == Operation::Withdraw ? m_withdrawal_draft_asset : m_asset->currentIndex()) == 1;
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
            // A marketable limit is priced here, from this fresh snapshot; the
            // review shows and signs exactly that limit.
            std::optional<B3FlowMeshMarketData::MarketableLimit> marketable;
            if (m_marketable->isChecked()) {
                marketable = marketableLimit(&error);
                if (!marketable) throw std::runtime_error{tr("Marketable limit unavailable: %1 Nothing was signed or submitted.").arg(error).toStdString()};
                const QSignalBlocker block{m_price}; m_price->setText(marketable->display);
            }
            std::optional<std::vector<flowmesh::LadderStep>> ladder;
            if (m_ladder->isChecked()) {
                bool adjusted{false};
                ladder = ladderSteps(&error, &adjusted);
                if (!ladder) throw std::runtime_error{error.toStdString()};
                if (ladder->size() > 1) {
                    // Signed as one curve; price and amount only title it.
                    const bool bid{a.side == QStringLiteral("bid")};
                    for (const auto& step : *ladder) {
                        a.ladder.emplace_back(step.price, step.qty);
                        a.amount += step.qty;
                        if (a.price == 0 || (bid ? step.price > a.price : step.price < a.price)) a.price = step.price;
                    }
                    a.display_limit_adjusted = adjusted;
                }
            }
            if (a.ladder.empty()) {
                const auto limit{B3FlowMeshMarketData::ParseDisplayLimit(m_price->text(), m_snapshot->units, a.inverse_display, m_side->currentIndex() == 0, &error)};
                const auto quantity{B3FlowMeshMarketData::ParseQuantity(m_quantity->text(), m_snapshot->units, &error)};
                if (!limit || !quantity) throw std::runtime_error{error.toStdString()}; a.price = limit->price; a.amount = *quantity;
                a.display_limit_adjusted = limit->adjusted; a.entered_display_limit = m_price->text();
            }
            if (marketable) {
                // A marketable limit is never a ladder (the mode unchecks it).
                if (!a.ladder.empty() || a.price != marketable->price || a.display_limit_adjusted) throw std::runtime_error{"The marketable limit did not round-trip exactly. Nothing was signed."};
                a.marketable = Action::Marketable{marketable->reference, marketable->visible, marketable->slippage_bps, marketable->reference_display};
            }
            // A SUBMIT atomically replaces this side's certified order: say so
            // in both orientations, with what it replaces.
            for (const auto& own : m_snapshot->own_curves) {
                if (own.side != a.side) continue;
                const auto steps{B3FlowMeshMarketData::LadderStepsOf(own)};
                a.replaces = Action::Replaces{own.remaining, own.filled, B3FlowMeshMarketData::LimitPriceOf(own), own.points.size(),
                                              steps && steps->size() > 1 ? steps->size() : size_t{0}};
            }
        } else if (operation == Operation::Deposit || operation == Operation::Withdraw) {
            const auto text{operation == Operation::Withdraw ? m_withdrawal_draft_amount : m_amount->text()};
            const auto amount{a.native ? B3AssetTransfer::ParseAmount(text, 9, &error) : B3FlowMeshMarketData::ParseQuantity(text, m_snapshot->units, &error)};
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
        // A wallet the panel had to unlock is relocked when this job ends,
        // so only an already unlocked wallet waits inside the submission.
        // Anything not verified then still gets one waited status read.
        a.submit_wait_ms = B3FlowMeshTrading::SubmissionWaitMs(operation, a.market.remote, !was_locked, POST_SUBMIT_RECEIPT_WAIT_MS);
    }
    startJob(a);
}

bool B3FlowMeshTradingPanel::deferReview(Operation operation, bool funding)
{
    const auto selected{market()};
    if (!m_thread || m_busy || !m_wallet || !m_backend || !selected || m_uncertain || !m_security_warning.isEmpty()) return false;
    if (operation == Operation::Order && !orderReviewAvailable()) return false;
    // Save only an intent to OPEN a review. No action is approved, signed or
    // sent here. Freeze inputs so the pending click cannot change underneath it.
    m_deferred_review = DeferredReview{operation, funding, m_generation, selected->id, m_effect->currentData(Qt::UserRole + 1).toString(), std::nullopt, std::nullopt};
    if (operation == Operation::Order)
        m_deferred_review->order_context = DeferredReview::OrderContext{m_wallet, *selected, m_snapshot->units,
            m_price->text(), m_quantity->text(), m_side->currentIndex(), inverted(), m_active_result && m_active_result->receipt_only,
            m_marketable->isChecked(), m_slippage->text(), m_ladder->isChecked(), ladderText()};
    if (m_active_result && m_active_result->passive && m_refresh_yields < MAX_REFRESH_YIELDS) m_yield_refresh->store(true);
    m_busy = true; updateControls(); return true;
}

void B3FlowMeshTradingPanel::resumeReview()
{
    if (!m_deferred_review || m_thread) return;
    const auto intent{*m_deferred_review}; m_deferred_review.reset();
    m_busy = false; updateControls();
    const auto selected{market()};
    const bool funding_draft{intent.funding && (intent.operation == Operation::Deposit || intent.operation == Operation::Withdraw)};
    const bool needs_market_read{intent.order_context && intent.order_context->needs_market_read};
    if (intent.generation != m_generation || !selected || intent.market != selected->id ||
        (m_read_failed && !funding_draft && !needs_market_read) || m_uncertain || !m_security_warning.isEmpty()) return;
    if (intent.order_context) {
        const auto& context{*intent.order_context};
        // Fresh balances and sequence may advance before approval. Identity,
        // units and every typed/display input must remain exactly as clicked.
        if (!orderReviewAvailable() || context.wallet != m_wallet || context.market.id != selected->id ||
            context.market.base != selected->base || context.market.vault != selected->vault ||
            context.market.domain != selected->domain || context.market.config != selected->config ||
            context.market.account != selected->account || context.market.remote != selected->remote ||
            context.units != m_snapshot->units || (!context.marketable && context.price != m_price->text()) || context.quantity != m_quantity->text() ||
            context.side != m_side->currentIndex() || context.inverse != inverted() ||
            context.marketable != m_marketable->isChecked() || context.slippage != m_slippage->text() ||
            context.ladder != m_ladder->isChecked() || context.ladder_text != ladderText()) {
            notice(tr("The order inputs, asset units or wallet/market binding changed. Review the order again; nothing was signed or submitted.")); return;
        }
        if (needs_market_read) {
            // An explicit saved-receipt check carries no market snapshot.
            // Follow it once with a foreground read, never an action.
            startJob(std::nullopt, std::nullopt, false, false, std::nullopt, {}, true);
            if (!m_thread) return;
            m_deferred_review = intent; m_deferred_review->order_context->needs_market_read = false;
            m_busy = true; updateControls(); return;
        }
    }
    if (intent.funding_context) {
        try {
            B3FlowMeshTrading::CheckSameMarket(intent.funding_context->market, *selected, false);
            if (!m_snapshot || m_snapshot->units != intent.funding_context->units ||
                selected->remote != intent.funding_context->market.remote ||
                (intent.operation == Operation::Withdraw ? m_withdrawal_draft_amount : m_amount->text()) != intent.funding_context->amount ||
                (intent.operation == Operation::Withdraw ? m_withdrawal_draft_asset : m_asset->currentIndex()) != intent.funding_context->asset ||
                (intent.operation == Operation::Withdraw && m_destination->text() != intent.funding_context->destination))
                throw std::runtime_error{"The funding draft's inputs, asset units or market binding changed. Reopen it and check the amount and destination; nothing was prepared or submitted."};
        } catch (const std::exception& e) { notice(QString::fromUtf8(e.what())); return; }
    }
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
    const bool allowed{intent.operation == Operation::Order ? orderReady() :
        !intent.funding && intent.operation == Operation::Deposit ? depositReady() :
        !intent.funding && intent.operation == Operation::Withdraw ? withdrawalReady() : button && button->isEnabled()};
    if (!allowed) { notice(tr("Market status changed. Nothing was submitted; review again when ready.")); return; }
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
    if (m_active_result && m_active_result->passive && m_refresh_yields < MAX_REFRESH_YIELDS) m_yield_refresh->store(true);
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
                                    std::optional<StatusRead> status_read, const QString& connect_url, bool foreground_read, bool receipt_wait)
{
    if (!m_wallet || !m_backend || m_thread) return;
    if (receipt_only) {
        if (!status_read) status_read = selectedStatusRead();
        if (!status_read || !statusReadValid(*status_read, true)) return;
    }
    if (action && !m_saved_actions_ready) return;
    const bool tracked{m_receipt && receiptWalletSelected()};
    if ((exact_retry || receipt_only || receipt_wait) && !tracked) return;
    if ((exact_retry || receipt_wait) && (m_receipt->Included() || m_receipt->no_resubmit)) return;
    const bool watch_queue{action && action->operation != Operation::Checkpoint && action->operation != Operation::Vault &&
        m_snapshot && m_snapshot->market == action->market.id && m_snapshot->pending_actions > 0 && m_queue_age.isValid()};
    const QElapsedTimer queue_watch{m_queue_age};
    // Recheck after any modal review/unlock/fee dialog, including the second
    // exact-transaction review. A rejected start must restore acquired unlock.
    if (watch_queue && queue_watch.elapsed() >= 30'000) {
        restoreLock(); m_busy = false; notice(tr("Queued requests have not certified for 30 seconds. No new request was submitted. Refresh and inspect the market; existing requests are not canceled.")); updateControls(); return;
    }
    // The in-flight request row is captured here, on the GUI thread.
    m_inflight_action = action && SignedAction(action->operation) ? action : std::nullopt;
    m_inflight_market = action ? action->market.id : QString{};
    m_busy = action.has_value() || exact_retry; m_loading = !action && !m_snapshot; m_cancel->store(false); m_attempt_age.restart(); updateControls(); m_chart->setLoading(m_loading);
    auto result{std::make_shared<Result>()}; result->action = action; result->prepared = prepared; result->broadcast = prepared.has_value(); result->wallet = m_wallet_name;
    result->connect_url = connect_url;
    result->exact_retry = exact_retry; result->receipt_only = receipt_only; result->receipt_wait = receipt_wait;
    result->passive = !action && !exact_retry && !receipt_only && !receipt_wait && connect_url.isEmpty() && !foreground_read;
    result->foreground_read = foreground_read;
    m_yield_refresh->store(false);
    result->receipt_scope = receipt_only ? status_read : !action && !exact_retry && !receipt_wait ? selectedStatusRead() : std::nullopt;
    m_active_result = result;
    auto* node{&m_wallet->node()}; const auto backend{m_backend}; const auto cancel{m_cancel}; const auto yield_refresh{m_yield_refresh};
    const auto uri{B3AssetTransfer::WalletUri(m_wallet->getWalletName())}; const auto generation{m_generation};
    const auto selected{market()}; const QString selected_id{selected ? selected->id : QString{}};
    result->views_market = selected_id; result->ticks_after = ticksAfter(selected_id); result->draft = indicativeDraft(); result->levels = levelsRequest();
    // Candles are display only: never in an order review's, funding,
    // receipt, connect or action job, which a user waits for. They stay
    // due, so the next passive job reads them (liveSkipsPoll() is false
    // while they are due).
    if (result->passive) {
        if (const auto due{candlesDue()}) { result->candles_interval = due->first; result->candles_limit = due->second; }
    }
    const auto phase{foreground_read ? (m_route_pending || m_market_data.empty() || !m_catalog_age.isValid() ? RefreshPhase::Catalog : RefreshPhase::None)
                                     : nextPassivePhase()};
    if (result->passive || foreground_read) {
        result->refresh_phase = phase; result->refresh_epoch = m_refresh_epoch;
    }
    // Same read set and count: the override only replaces this job's phase.
    result->reconcile_first = result->passive && phase == RefreshPhase::Receipt && scheduledPassivePhase() != RefreshPhase::Receipt;
    m_last_job_was_reconcile = result->reconcile_first;
    // Live updates supply the selected market's read: a passive job then runs
    // only its auxiliary phase. Foreground, review and action reads always read.
    result->live = result->passive && watchSuppliesRead();
    const auto refresh_basis{m_snapshot};
    result->markets = m_market_data; result->effects = m_effect_data;
    if (m_snapshot && m_snapshot->market == selected_id) result->basis_sequence = m_snapshot->next_sequence;
    const QString known_head{m_snapshot && m_snapshot->market == selected_id ? m_snapshot->head : QString{}};
    const QString route_base{m_route_pending ? m_requested_base : QString{}};
    const QString receipt_market{status_read ? status_read->market : tracked ? m_pending_market : QString{}},
        receipt_id{status_read ? status_read->action_id : tracked ? m_receipt->action_id : QString{}};
    result->receipt_market = receipt_market; result->receipt_action_id = receipt_id;
    result->receipt_account = status_read ? status_read->account : tracked ? m_pending_account : QString{};
    m_thread = QThread::create([this, generation, node, backend, cancel, yield_refresh, uri, result, selected_id, known_head, route_base, phase, refresh_basis, watch_queue, queue_watch, receipt_market, receipt_id] {
        // This single worker is the backend's only passive caller and runs one
        // job at a time, so a linger after its own foreground job could only
        // delay its own follow-up refresh. RPC-thread callers keep the window.
        const node::FlowMeshClientWorkScope priority{result->passive ? node::FlowMeshClientWorkPriority::PASSIVE : node::FlowMeshClientWorkPriority::FOREGROUND,
                                                     node::FlowMeshClientWorkLinger::NONE};
        std::exception_ptr auxiliary_error;
        const auto record_failure = [&](std::exception_ptr error) {
            try { std::rethrow_exception(error); }
            catch (const UniValue& value) { result->error = RpcError(value); }
            catch (const std::exception& value) { result->error = QString::fromUtf8(value.what()).left(500); }
            catch (...) { result->error = QStringLiteral("The FlowMesh operation failed."); }
        };
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
                if (result->passive && yield_refresh->load()) {
                    // Priority must not turn a real earlier read failure into
                    // a successful yield/review. Preserve conservative gating.
                    if (auxiliary_error) std::rethrow_exception(auxiliary_error);
                    throw PassiveRefreshYield{};
                }
                if (!NoSpendingRequired(method)) {
                    // Readiness RPCs may themselves outlast the local deadline.
                    // Do not access UI timers from this worker; this captured
                    // monotonic observation only fails closed until refresh.
                    if (watch_queue && queue_watch.elapsed() >= 30'000) throw std::runtime_error{"Queued requests have not certified for 30 seconds. Refresh before submitting; no new mutation was started."};
                    if (method != "submitflowmeshdeposit" && method != "retryflowmeshaction" && (backend->isLocked() || backend->privateKeysDisabled())) throw std::runtime_error{"The captured wallet is locked for spending."};
                    if (method == "sendrawtransaction" || method == "submitflowmeshorder" || method == "submitflowmeshladder" || method == "cancelflowmeshorder" || method == "requestflowmeshwithdrawal" || method == "submitflowmeshdeposit" || method == "retryflowmeshaction") result->write_attempted = true;
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
            if (result->exact_retry || result->receipt_only || result->receipt_wait) {
                const auto request{B3FlowMeshTrading::ReceiptParameters(receipt_market, receipt_id, result->exact_retry,
                                                                        result->receipt_wait ? POST_SUBMIT_RECEIPT_WAIT_MS : 0)};
                result->receipt = read_receipt(rpc(request.method, request.params));
                return;
            }
            if (!result->action) {
                node::FlowMeshTimingSpan timing{result->passive ? "qt_passive_refresh" : "qt_review_read"};
                if (timing.Enabled()) {
                    const char* label = phase == RefreshPhase::Catalog ? "catalog" : phase == RefreshPhase::Status ? "status" :
                        phase == RefreshPhase::Effects ? "effects" : phase == RefreshPhase::Receipt ? "receipt" : "none";
                    timing.Field("auxiliary_phase", std::string{label});
                }
                read_client_info();
                // Restore public cards before attempting remote reads. The
                // panel owns and drains this worker before destruction; the
                // queued callback is context-bound and generation guarded.
                const auto saved{B3FlowMeshTrading::ParseSavedActions(rpc("listflowmeshactions", UniValue{UniValue::VARR}))};
                QMetaObject::invokeMethod(this, [this, generation, saved] {
                    if (generation == m_generation && m_wallet && !m_cancel->load()) restoreSavedActions(saved);
                }, Qt::QueuedConnection);
                const auto auxiliary = [&](const auto& read) {
                    try { read(); } catch (const PassiveRefreshYield&) { throw; }
                    catch (...) { auxiliary_error = std::current_exception(); }
                };
                // The effects reconciliation fallback and the final selected
                // read share one response, not a second snapshot RPC. Record
                // its real receipt time, never GUI delivery or optional work.
                std::optional<UniValue> received_snapshot;
                bool snapshot_attempted{false}; std::exception_ptr snapshot_error;
                const B3FlowMeshTrading::RpcCall snapshot_rpc = [&](const std::string& method, const UniValue& params) {
                    if (method != "getflowmeshmarketdata") return rpc(method, params);
                    if (!snapshot_attempted) {
                        snapshot_attempted = true;
                        try {
                            received_snapshot = rpc(method, params); result->snapshot_age.restart(); timing.Mark("snapshot_received_us");
                        } catch (...) { snapshot_error = std::current_exception(); }
                    }
                    if (snapshot_error) std::rethrow_exception(snapshot_error);
                    return *received_snapshot;
                };
                if (phase == RefreshPhase::Catalog) auxiliary([&] {
                    auto discovered{B3FlowMeshTrading::ParseMarkets(rpc("listflowmeshmarkets", UniValue{UniValue::VARR}))};
                    for (auto& row : discovered) if (row.remote) {
                        const auto previous{std::find_if(result->markets.begin(), result->markets.end(), [&](const auto& old) {
                            return row.id == old.id && row.base == old.base && row.vault == old.vault &&
                                row.domain == old.domain && row.config == old.config && row.remote == old.remote;
                        })};
                        // Remote discovery has no account/state proof. Preserve
                        // only the exact prior binding and verified checkpoint
                        // metadata; a new row cannot create publication eligibility.
                        row.has_account = previous != result->markets.end() && previous->has_account;
                        row.account = row.has_account ? previous->account : QString{};
                        row.sequence = row.has_account ? previous->sequence : 0;
                        row.base_available = row.has_account ? previous->base_available : 0;
                        row.base_reserved = row.has_account ? previous->base_reserved : 0;
                        row.b3_available = row.has_account ? previous->b3_available : 0;
                        row.b3_reserved = row.has_account ? previous->b3_reserved : 0;
                        row.checkpoint_pending = previous != result->markets.end() && previous->checkpoint_pending;
                        row.checkpoint = row.checkpoint_pending ? previous->checkpoint : QString{};
                        row.publish_ready = previous != result->markets.end() && previous->publish_ready;
                    }
                    result->markets = std::move(discovered);
                    result->catalog = true; // Discovery survives a later selected-market failure.
                });
                if (phase == RefreshPhase::Receipt && !receipt_id.isEmpty()) {
                    try {
                        const auto request{B3FlowMeshTrading::ReceiptParameters(receipt_market, receipt_id)};
                        result->receipt = read_receipt(rpc(request.method, request.params));
                    } catch (const UniValue& error) { result->receipt_error = RpcError(error); }
                    catch (const std::exception& error) { result->receipt_error = QString::fromUtf8(error.what()).left(500); }
                }
                auto selected{std::find_if(result->markets.begin(), result->markets.end(), [&](const auto& m) { return route_base.isEmpty() ? m.id == selected_id : m.base == route_base; })};
                if (selected == result->markets.end() && selected_id.isEmpty() && route_base.isEmpty()) selected = result->markets.begin();
                if (selected != result->markets.end()) {
                    result->read_market = selected->id;
                    if (phase == RefreshPhase::Status && (selected->remote || selected->has_account)) auxiliary([&] {
                        UniValue params{UniValue::VARR}; params.push_back(selected->id.toStdString());
                        const auto status{B3FlowMeshTrading::ParseMarket(rpc("getflowmeshbalance", params))};
                        if (status.id != selected->id || status.base != selected->base || status.vault != selected->vault || status.domain != selected->domain || status.config != selected->config || status.remote != selected->remote ||
                            (selected->has_account && (!status.has_account || selected->account != status.account))) throw std::runtime_error{"Selected status changed its pinned market or account identity."};
                        *selected = status;
                    });
                    if (phase == RefreshPhase::Effects) auxiliary([&] {
                        result->effects.clear();
                        if (refresh_basis && refresh_basis->market == selected->id && refresh_basis->base == selected->base &&
                            refresh_basis->domain == selected->domain && refresh_basis->config == selected->config &&
                            refresh_basis->account == selected->account && !refresh_basis->account.isEmpty()) {
                            auto basis{*refresh_basis};
                            result->effects = ReadEffectsForRefresh(snapshot_rpc, selected->id, basis);
                        }
                    });
                    if (!result->live || selected->id != selected_id) {
                        UniValue params{UniValue::VARR}; params.push_back(selected->id.toStdString()); UniValue options{UniValue::VOBJ}; options.pushKV("limit", 100); options.pushKV("curve_limit", 128);
                        // The held ticks and the ticket belong to the displayed market only.
                        AddMarketViews(options, selected->id == result->views_market ? result->ticks_after : std::nullopt,
                                       selected->id == result->views_market ? result->draft : UniValue{},
                                       selected->id == result->views_market ? result->levels : UniValue{});
                        if (!known_head.isEmpty() && selected->id == selected_id && !result->catalog) {
                            options.pushKV("known_head", known_head.toStdString());
                            result->basis_head = known_head;
                        }
                        params.push_back(options); result->snapshot = B3FlowMeshMarketData::Parse(snapshot_rpc("getflowmeshmarketdata", params));
                        const auto& s{*result->snapshot};
                        if (s.market != selected->id || s.base != selected->base || s.domain != selected->domain || s.config != selected->config) throw std::runtime_error{"Certified data does not match the selected market identity."};
                        if (!s.unchanged && selected->has_account && selected->account != s.account) throw std::runtime_error{"Certified data belongs to another wallet account."};
                        if (s.chain_reconciling) result->effects.clear();
                        DeriveRowFromSnapshot(*selected, s);
                    }
                    // Observed-time candles, when due: display only, and a
                    // failure here never fails this job.
                    if (result->candles_interval && selected->id == result->views_market) {
                        try {
                            UniValue params{UniValue::VARR}; params.push_back(selected->id.toStdString());
                            UniValue options{UniValue::VOBJ};
                            options.pushKV("interval", flowmesh::CandleIntervalName(*result->candles_interval));
                            options.pushKV("limit", static_cast<uint64_t>(result->candles_limit));
                            params.push_back(options);
                            result->candles = B3FlowMeshMarketData::ParseCandles(rpc("getflowmeshcandles", params), *result->candles_interval);
                        } catch (const PassiveRefreshYield&) { throw; }
                        catch (const UniValue& error) { result->candles_error = RpcError(error); }
                        catch (const std::exception& error) { result->candles_error = QString::fromUtf8(error.what()).left(500); }
                        catch (...) { result->candles_error = QStringLiteral("Candles unavailable."); }
                    }
                }
                if (auxiliary_error) std::rethrow_exception(auxiliary_error); // Conservative failure; no new display freshness.
                read_client_info(); // Include observations from this market read.
                timing.Mark("completed_us");
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
        } catch (const PassiveRefreshYield&) {
            // A shared snapshot attempt can cache the yield while an effects
            // fallback restores its earlier error. That real error still wins.
            if (auxiliary_error) record_failure(auxiliary_error);
            else result->refresh_yielded = true;
        } catch (...) { record_failure(std::current_exception()); }
        if (!result->refresh_yielded && !result->action && !result->exact_retry && !result->receipt_only && !result->receipt_wait) read_client_info();
    });
    m_thread->setParent(this); connect(m_thread, &QThread::finished, this, [this, result, generation] { if (generation == m_generation) finishJob(result); }); m_thread->start(); updateControls();
}

void B3FlowMeshTradingPanel::finishJob(const std::shared_ptr<Result>& result)
{
    stopWorker(); m_active_result.reset(); m_inflight_action.reset(); m_inflight_market.clear();
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
    // A completed status read of the tracked request (the override's or the
    // regular Receipt phase's) completes a pending reconcile. A failed status
    // read in a job whose market read also failed does not; it is tried again,
    // never in consecutive jobs, until one completes.
    if (result->refresh_phase == RefreshPhase::Receipt && !result->receipt_action_id.isEmpty() &&
        (result->receipt || (!result->receipt_error.isEmpty() && result->error.isEmpty()))) m_reconcile_pending = false;
    if (result->refresh_yielded) {
        if (result->refresh_epoch == m_refresh_epoch && m_refresh_yields < MAX_REFRESH_YIELDS) ++m_refresh_yields;
        // No partial catalog/snapshot is published or stamped fresh. Preserve
        // any completed, correctly attributed receipt observation/error; it
        // neither renews the market nor authorizes an action.
        // Keep the auxiliary phase pending; a later passive cycle retries it.
        if (result->receipt) { applyReceipt(*result->receipt); applyReceiptError(*result, {}); }
        if (!result->receipt_error.isEmpty()) applyReceiptError(*result, result->receipt_error);
        m_loading = false; m_busy = false; m_chart->setLoading(false); updateControls();
        if (m_deferred_review) {
            // The click still only asks to OPEN a review. Read the exact
            // captured market first, then run all existing identity/readiness
            // guards and the normal confirmation. No automatic approval.
            startJob(std::nullopt, std::nullopt, false, false, std::nullopt, {}, true);
            if (m_thread) { m_busy = true; updateControls(); }
        } else if (const auto selected{market()}; selected && selected->id != result->views_market) {
            // A market switch yielded this read: read the new market now,
            // not a tick later (refresh drains a queued connect first).
            refresh();
        }
        return; // finishJob drains a captured explicit status next, if any.
    }
    // A reconcile override replaced a phase: the scheduled one still runs next.
    if (!result->foreground_read && result->refresh_phase && result->refresh_epoch == m_refresh_epoch && !result->reconcile_first) {
        m_refresh_yields = 0;
        switch (*result->refresh_phase) {
        case RefreshPhase::Catalog: m_refresh_phase = RefreshPhase::Status; break;
        case RefreshPhase::Status: m_refresh_phase = RefreshPhase::Effects; break;
        case RefreshPhase::Effects: m_refresh_phase = RefreshPhase::Receipt; break;
        case RefreshPhase::Receipt: case RefreshPhase::None: m_refresh_phase = RefreshPhase::None; break;
        }
    }
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
    if (result->receipt_wait) {
        // Read only. A failure records the status error and never marks the
        // submission uncertain; the passive cycle keeps reading its receipt.
        m_busy = false;
        if (result->receipt) applyReceipt(*result->receipt);
        else applyReceiptError(*result, result->error);
        updateReceiptCard(); updateMarketText();
        if (m_deferred_review) resumeReview();
        else refresh();
        return;
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
        const bool open_funding_draft{!result->action && m_deferred_review && m_deferred_review->funding &&
            (m_deferred_review->operation == Operation::Deposit || m_deferred_review->operation == Operation::Withdraw)};
        if (!open_funding_draft) m_deferred_review.reset();
        if (!result->action) {
            // Like a yielded read, a completed and correctly attributed status
            // observation is kept although the market read failed: it neither
            // renews the market nor authorizes an action.
            if (result->receipt) { applyReceipt(*result->receipt); applyReceiptError(*result, {}); }
            if (!result->receipt_error.isEmpty()) applyReceiptError(*result, result->receipt_error);
            // The first lost market read of an episode: reconcile the tracked
            // request before the next market read.
            if (!m_read_failed && !result->snapshot && !result->live && !result->receipt) m_reconcile_pending = true;
            if (result->catalog) applyMarketCatalog(*result);
            const auto selected{market()};
            if (!result->read_market.isEmpty() && (!selected || result->read_market != selected->id)) {
                // Selection can change during a passive read. Keep discovery,
                // but never attach that old market's error to the new choice.
                m_loading = false; m_busy = false; updateDataViews(); updateMarketText(); refresh(); return;
            }
            if (!m_read_failed) {
                notice(tr("Market refresh failed: %1. Last certified data is retained; new actions are disabled until a successful refresh.").arg(result->error));
                linkToast(false);
            }
            m_read_failed = true; m_read_error = result->error; m_read_failures = std::min(5U, m_read_failures + 1); m_loading = false; m_uncertain_refreshed = false; m_busy = false;
            m_link_attempts = std::min(9999U, m_link_attempts + 1);
            updateDataViews(); updateMarketText();
            if (open_funding_draft) resumeReview(); // Opening retained draft only; failed Continue is never resumed.
            return;
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
        m_loading = false; m_read_failed = false; m_read_failures = 0; m_link_attempts = 0; m_read_error.clear();
        linkToast(true);
        if (!result->refresh_phase || *result->refresh_phase == RefreshPhase::Receipt) applyReceiptError(*result, result->receipt_error);
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
        // The verified head this job read before its candles page.
        const std::optional<uint64_t> read_next{result->snapshot && result->snapshot->market == result->views_market
                                                    ? std::optional{result->snapshot->next_sequence} : std::nullopt};
        if (result->snapshot && selected_now && result->snapshot->market == selected_now->id) {
            // Moved, not copied: nothing reads the job's snapshot afterwards
            // (but read_next, taken above).
            applySelectedSnapshot(std::move(*result->snapshot), result->snapshot_age, result->basis_head);
        } else if (!selected_now || !m_snapshot || selected_now->id != m_snapshot->market) { m_snapshot.reset(); m_response_age.invalidate(); m_queue_age.invalidate(); updateDataViews(); }
        if (result->candles && selected_now && m_snapshot && m_snapshot->market == selected_now->id && result->views_market == selected_now->id &&
            result->candles->interval == m_candle_interval) {
            applyCandles(*result->candles, result->candles_limit, read_next);
            updateDataViews();
        } else if (!result->candles_error.isEmpty() && result->candles_interval && selected_now && m_snapshot && m_snapshot->market == selected_now->id &&
                   result->views_market == selected_now->id && *result->candles_interval == m_candle_interval) {
            // Display only: backs off, and live updates go on.
            noteCandlesFailure(result->candles_error);
            updateDataViews();
        }
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
            m_tracked_action = a; m_tracked_kind = B3FlowMeshTrading::KindOf(a.operation);
            m_tracked_title_key = RequestKey(a.market.id, a.market.account, result->receipt->action_id);
            applyReceipt(*result->receipt);
            // Command names live in Details; the log says where to look.
            notice(tr("Wallet: %1\n%2\nNo automatic retry. Its progress is shown under Your orders and Saved requests; a resend only ever uses the retained signed bytes, never a new signature.")
                .arg(result->wallet, B3FlowMeshTrading::DescribeReceipt(*result->receipt)));
        }
    }
    updateMarketText();
    // A remote signed action that is not yet terminal: one foreground status
    // read waits briefly for its certified inclusion, instead of the next
    // passive receipt cycle noticing it. It never resends or signs.
    if (!result->broadcast && SignedAction(a.operation) && a.market.remote && result->receipt &&
        !result->receipt->Included() && result->receipt->state != QStringLiteral("rejected")) {
        startJob(std::nullopt, std::nullopt, false, false, std::nullopt, {}, false, /*receipt_wait=*/true);
        if (m_thread) return;
    }
    refresh();
}

bool B3FlowMeshTradingPanel::endpointNotReady(const QString& endpoint) const
{
    if (endpoint.isEmpty() || !m_client_info || !m_client_info->isObject()) return false;
    const auto& rows{m_client_info->find_value("endpoints")};
    if (!rows.isArray()) return false;
    for (const auto& row : rows.getValues()) {
        const auto& url{row.find_value("url")};
        const auto& readiness{row.find_value("readiness")};
        if (url.isStr() && QString::fromStdString(url.get_str()) == endpoint && readiness.isStr() && readiness.get_str() == "not_ready") return true;
    }
    return false;
}

auto B3FlowMeshTradingPanel::applySelectedSnapshot(B3FlowMeshMarketData::Snapshot fresh, const QElapsedTimer& age, const QString& basis_head,
                                                   const bool live_head_change) -> SnapshotApply
{
    // Reads can complete out of order. Each is compared with the head it was
    // requested against, never only with whatever is displayed now.
    if (m_snapshot && m_snapshot->market == fresh.market) {
        if (fresh.unchanged && !basis_head.isEmpty() && basis_head != m_snapshot->head) return SnapshotApply::SUPERSEDED;
        if (!fresh.unchanged && fresh.next_sequence < m_snapshot->next_sequence) return SnapshotApply::SUPERSEDED;
    }
    if (m_snapshot && fresh.units.known && fresh.units != m_snapshot->units) { m_amount->clear(); m_withdrawal_draft_amount.clear(); }
    const bool new_head{!m_snapshot || m_snapshot->market != fresh.market || m_snapshot->head != fresh.head};
    if (fresh.unchanged) {
        if (!m_snapshot || m_snapshot->market != fresh.market || m_snapshot->head != fresh.head || m_snapshot->state_root != fresh.state_root || m_snapshot->remote != fresh.remote || m_snapshot->execution_result_verified != fresh.execution_result_verified) {
            m_read_failed = true; notice(tr("Unchanged snapshot did not match the retained certificate and provenance; data was not reused.")); m_catalog_age.invalidate();
            return SnapshotApply::MISMATCH;
        }
        // Updated in place: the retained head's curves, history and levels
        // are never copied for a status refresh.
        auto& retained{*m_snapshot};
        // Only what the data views draw asks for them again; status alone
        // is the caller's updateMarketText (ticket, gates and banners).
        bool display{retained.certificate_verified != fresh.certificate_verified || retained.account_state_verified != fresh.account_state_verified ||
                     retained.event_gap != fresh.event_gap || retained.endpoint != fresh.endpoint ||
                     (fresh.levels && retained.levels != fresh.levels)};
        retained.running = fresh.running; retained.paused = fresh.paused; retained.chain_reconciling = fresh.chain_reconciling; retained.handoff = fresh.handoff; retained.observer = fresh.observer; retained.halt = std::move(fresh.halt); retained.error = std::move(fresh.error); retained.pending_actions = fresh.pending_actions; retained.active_seats = fresh.active_seats; retained.quorum_required = fresh.quorum_required;
        retained.remote = fresh.remote; retained.endpoint = std::move(fresh.endpoint); retained.certificate_verified = fresh.certificate_verified; retained.account_state_verified = fresh.account_state_verified; retained.execution_result_verified = fresh.execution_result_verified; retained.b3_checkpoint_confirmed = fresh.b3_checkpoint_confirmed; retained.event_gap = fresh.event_gap;
        // Cosmetic discovery can advance without a new certified
        // microblock. Keep the exact account/book and user inputs;
        // only accept units for this same canonical asset.
        if (fresh.units.known && fresh.units.asset == retained.base && fresh.units != retained.units) { retained.units = std::move(fresh.units); display = true; }
        // Read at this same head: they replace the retained ones.
        retained.ticks = std::move(fresh.ticks); retained.indicative = std::move(fresh.indicative);
        // Only the head's row (and any new tick's): the rows of the head's
        // history and ticks stay, so the first same-head reply after a new
        // head neither rebuilds the views nor drops those times.
        if (B3FlowMeshMarketData::MergeSameHeadObserved(retained.observed, std::move(fresh.observed))) display = true;
        // Levels come with a changed head, or when this read asked for them
        // again (another grouping): take them, else keep the head's own.
        if (fresh.levels) retained.levels = std::move(fresh.levels);
        m_response_age = age;
        dropCaughtUpDisplay();
        // Held ticks and their times are shared text: copies are cheap.
        const auto ticks_before{m_ticks};
        const auto times_before{m_tick_times};
        const auto received_before{m_received_times};
        mergeHeldTicks();
        display = display || m_ticks != ticks_before || m_tick_times != times_before || m_received_times != received_before;
        if (display) updateDataViews();
    } else {
        if (!m_snapshot || m_snapshot->head != fresh.head) m_certificate_age = age;
        // Tape only: a server without observed times, one new auction since
        // the displayed head, received while the link was Live and without
        // an event gap: when this app received it, which bounds its
        // certification (at or before then) and nothing more. Only from a
        // live update whose source held the displayed head when its wait
        // began and saw it move during the wait (HeadMovedDuringWait): a
        // reply answered at once for a stale known head, a poll, a replay of
        // old entries in consecutive replies or a catch-up never stamps one,
        // nor an endpoint advertising that it is not ready. A source that
        // catches up while it waits, without saying so, can still stamp one
        // late. Nothing older, never a chart input.
        const bool one_new{m_snapshot && m_snapshot->market == fresh.market && fresh.next_sequence == m_snapshot->next_sequence + 1};
        const bool was_live{!m_read_failed && m_response_age.isValid() && m_response_age.elapsed() <= B3FlowMeshMarketData::LINK_FRESH_MS};
        const bool untimed{!fresh.observed || !fresh.observed->available};
        const bool receipt{live_head_change && one_new && was_live && untimed && !fresh.event_gap && fresh.certified &&
                           !endpointNotReady(fresh.endpoint)};
        const uint64_t head_sequence{fresh.next_sequence ? fresh.next_sequence - 1 : 0};
        const QString head_hash{fresh.head};
        m_snapshot = std::move(fresh); m_response_age = age; dropCaughtUpDisplay(); mergeHeldTicks();
        if (receipt && !m_ticks.empty() && m_ticks_market == m_snapshot->market && m_ticks.back().sequence == head_sequence &&
            m_ticks.back().hash == head_hash) {
            const int64_t received{QDateTime::currentMSecsSinceEpoch() - age.elapsed()};
            // Monotonic against the times already held (all of earlier
            // auctions): after this computer's clock stepped back (an NTP
            // correction) a newer auction is left without a time ("—")
            // rather than shown received before an older one.
            const bool monotonic{std::none_of(m_received_times.begin(), m_received_times.end(),
                                              [&](const auto& held) { return held.second > received; })};
            if (monotonic) m_received_times[head_sequence] = received;
        }
        updateDataViews();
    }
    // Display only (the banner's "since" time); every gate uses m_response_age.
    m_last_live_wall_ms = QDateTime::currentMSecsSinceEpoch() - age.elapsed(); m_last_live_market = m_snapshot->market;
    if (!m_read_failed && m_snapshot) {
        if (m_snapshot->pending_actions == 0) m_queue_age.invalidate();
        else if (new_head || !m_queue_age.isValid()) m_queue_age = age;
    }
    return SnapshotApply::APPLIED;
}

void B3FlowMeshTradingPanel::DeriveRowFromSnapshot(Market& row, const B3FlowMeshMarketData::Snapshot& s)
{
    row.publish_ready = (s.remote ? s.certificate_verified && s.account_state_verified : row.publish_ready) && s.running && !s.chain_reconciling;
    row.remote = s.remote;
    row.ready = s.remote ? B3FlowMeshMarketData::AdmissionReady(s, 0, -1) : row.publish_ready && !s.paused && !s.handoff && s.halt == QStringLiteral("none") && s.error.isEmpty();
    if (!s.unchanged) {
        row.has_account = !s.account.isEmpty(); row.account = s.account; row.sequence = s.account_sequence;
        row.base_available = s.base_available; row.base_reserved = s.base_reserved; row.b3_available = s.b3_available; row.b3_reserved = s.b3_reserved;
    }
}

bool B3FlowMeshTradingPanel::reconcileDue() const
{
    // Never before discovery ran since a reset (a Connect always reaches its
    // Catalog phase first), never in consecutive jobs, and only for a tracked
    // request whose outcome is not yet settled.
    return m_reconcile_pending && !m_last_job_was_reconcile && m_catalog_age.isValid() && !m_route_pending && !m_market_data.empty() &&
        m_receipt && receiptWalletSelected() && !m_receipt->Included() && !m_receipt->no_resubmit && !m_receipt->NotExecuted();
}

auto B3FlowMeshTradingPanel::nextPassivePhase() const -> RefreshPhase
{
    const auto scheduled{scheduledPassivePhase()};
    return scheduled != RefreshPhase::Receipt && reconcileDue() ? RefreshPhase::Receipt : scheduled;
}

auto B3FlowMeshTradingPanel::scheduledPassivePhase() const -> RefreshPhase
{
    const bool tracked{m_receipt && receiptWalletSelected()};
    const bool catalog_due{!m_catalog_age.isValid() || m_catalog_age.elapsed() >= 5000 || (m_snapshot && m_snapshot->chain_reconciling)};
    // Complete the bounded cycle before considering an overdue catalog again.
    // Receipt gets its own slot so a slow cycle cannot starve saved requests.
    return m_route_pending || m_market_data.empty() ? RefreshPhase::Catalog :
        m_refresh_phase != RefreshPhase::None ? m_refresh_phase : catalog_due ? RefreshPhase::Catalog :
        tracked ? RefreshPhase::Receipt : RefreshPhase::None;
}

bool B3FlowMeshTradingPanel::watchSuppliesRead() const
{
    const auto selected{market()};
    return m_watch.SuppliesMarketRead(m_watch_clock.elapsed()) && !m_read_failed && !m_watch_watchdog && selected && m_snapshot &&
        m_snapshot->market == selected->id;
}

bool B3FlowMeshTradingPanel::liveSkipsPoll() const
{
    return watchSuppliesRead() && !m_deferred_connect && !m_deferred_status && nextPassivePhase() == RefreshPhase::None && !candlesDue();
}

void B3FlowMeshTradingPanel::updateWatchState()
{
    const auto now{m_watch_clock.elapsed()};
    const auto text = [](const UniValue& value, const char* field) {
        const auto& item{value.find_value(field)}; return item.isStr() ? QString::fromStdString(item.get_str()).left(2048) : QString{};
    };
    const auto selected{market()};
    const bool local{m_client_info && (text(*m_client_info, "backend") == QStringLiteral("local") ||
                                       (m_client_info->find_value("engine_enabled").isBool() && m_client_info->find_value("engine_enabled").get_bool()))};
    // Keyed on the endpoint the displayed data came from, which is the one a
    // remote watch is sent to, never merely the selected endpoint.
    const QString endpoint{selected && m_snapshot && m_snapshot->market == selected->id ? m_snapshot->endpoint : QString{}};
    uint32_t live{0};
    if (!local && m_client_info && !endpoint.isEmpty()) {
        const auto& rows{m_client_info->find_value("endpoints")};
        if (rows.isArray()) for (const auto& row : rows.getValues()) {
            const auto& advertised{row.find_value("live_updates_ms")};
            if (text(row, "url") == endpoint && advertised.isNum()) live = static_cast<uint32_t>(std::clamp<int64_t>(advertised.getInt<int64_t>(), 0, 60'000));
        }
    }
    const bool eligible{m_wallet && m_backend && selected && m_security_warning.isEmpty() && m_client_info.has_value()};
    const QString key{eligible ? QStringLiteral("%1/%2/%3").arg(m_generation).arg(selected->id, local ? QStringLiteral("local") : endpoint) : QString{}};
    const bool advertised{local || live > 0};
    // The local engine's own pacing (LOCAL_ARM_REFILL_MS, today the same two
    // reads a second); a reply naming an endpoint drops to the remote pacing.
    const bool local_pacing{local && !(selected && m_snapshot && m_snapshot->market == selected->id && m_snapshot->remote)};
    if (key != m_watch_key) {
        m_watch_key = key; ++m_watch_generation; m_watch_watchdog = false;
        if (key.isEmpty()) m_watch.Disable();
        else m_watch.Enable(now, advertised, local_pacing);
    } else if (!key.isEmpty() && advertised && !m_watch_advertised && m_watch.mode == B3FlowMeshWatch::Mode::Polling) {
        // The endpoint (again) advertises live updates: try at once.
        m_watch.Enable(now, true, local_pacing);
    }
    m_watch_advertised = advertised;
    if (!m_watch_watchdog && m_watch.WatchdogTripped(now)) {
        // No verified live update for 15 s: poll as before and reconcile.
        m_watch_watchdog = true;
        m_watch.Watchdog(now);
        m_reconcile_pending = true;
    }
}

void B3FlowMeshTradingPanel::armWatch(const bool release)
{
    if (!m_wallet || !m_backend || m_watch_thread || m_cancel->load() || m_closing) return;
    const auto selected{market()};
    const auto& basis{watchBasis()};
    if (!selected || !basis || basis->market != selected->id) {
        if (release) m_watch_release_pending = false;
        return;
    }
    if (!release) {
        // A probe starts only with no job in flight: an ordinary market read
        // already on its way would move the client's cursor under it.
        if (m_thread && m_watch.mode != B3FlowMeshWatch::Mode::Live) return;
        // While busy only for display, during a review with nothing in
        // flight (the local engine; see liveWhileBusy).
        if ((m_busy && !liveWhileBusy()) || !isVisible() || !m_security_warning.isEmpty() || m_read_failed || !m_watch.MayArm(m_watch_clock.elapsed())) return;
        m_watch.Armed(m_watch_clock.elapsed());
    }
    auto result{std::make_shared<WatchResult>()};
    result->scope = {m_wallet, m_generation, m_watch_generation, selected->id, selected->account, basis->head};
    result->release = release;
    result->ticks_after = ticksAfter(selected->id); result->draft = indicativeDraft(); result->levels = levelsRequest();
    auto* node{&m_wallet->node()};
    const auto uri{B3AssetTransfer::WalletUri(m_wallet->getWalletName())};
    const auto cancel{m_cancel};
    // Captures values only: no panel, wallet object or backend. The RPC holds
    // the wallet for at most the watch plus 5 s.
    m_watch_thread = QThread::create([node, uri, cancel, result] {
        const node::FlowMeshClientWorkScope priority{node::FlowMeshClientWorkPriority::PASSIVE, node::FlowMeshClientWorkLinger::NONE};
        if (cancel->load() || node->shutdownRequested()) { result->error = QStringLiteral("Live update cancelled."); return; }
        try {
            UniValue params{UniValue::VARR}; params.push_back(result->scope.market.toStdString());
            UniValue options{UniValue::VOBJ}; options.pushKV("limit", 100); options.pushKV("curve_limit", 128);
            options.pushKV("known_head", result->scope.known_head.toStdString());
            options.pushKV("watch_ms", result->release ? int64_t{0} : B3FlowMeshWatch::Policy::WATCH_MS);
            AddMarketViews(options, result->ticks_after, result->draft, result->levels);
            params.push_back(options);
            const auto response{node->executeRpc("getflowmeshmarketdata", params, uri)};
            result->snapshot_age.restart();
            result->snapshot = B3FlowMeshMarketData::Parse(response);
            const auto& watch{response.find_value("watch")};
            if (!watch.isObject()) throw std::runtime_error{"Live update reply has no watch status."};
            const auto& status{watch.find_value("status")}; const auto& endpoint{watch.find_value("endpoint")};
            const auto& supported{watch.find_value("supported")}; const auto& waited{watch.find_value("waited")};
            const auto& gap{watch.find_value("gap")}; const auto& live{watch.find_value("endpoint_live_ms")};
            if (!status.isStr() || !endpoint.isStr() || !supported.isBool() || !waited.isBool() || !gap.isBool() || !live.isNum()) {
                throw std::runtime_error{"Live update reply has a malformed watch status."};
            }
            result->status = QString::fromStdString(status.get_str()).left(32);
            result->endpoint = QString::fromStdString(endpoint.get_str()).left(2048);
            result->supported = supported.get_bool(); result->waited = waited.get_bool(); result->gap = gap.get_bool();
            result->endpoint_live_ms = static_cast<uint32_t>(std::clamp<int64_t>(live.getInt<int64_t>(), 0, 60'000));
        } catch (const UniValue& error) { result->error = RpcError(error); result->snapshot.reset(); }
        catch (const std::exception& error) { result->error = QString::fromUtf8(error.what()).left(500); result->snapshot.reset(); }
        catch (...) { result->error = QStringLiteral("Live update failed."); result->snapshot.reset(); }
    });
    m_watch_thread->setParent(this);
    connect(m_watch_thread, &QThread::finished, this, [this, result] { finishWatch(result); });
    m_watch_thread->start();
}

void B3FlowMeshTradingPanel::finishWatch(const std::shared_ptr<WatchResult>& result)
{
    if (m_watch_thread) {
        m_watch_thread->wait(); // Already finished: this is its finished signal.
        m_watch_thread->deleteLater();
        m_watch_thread = nullptr;
    }
    if (result->release) {
        m_watch_release_pending = false;
        return;
    }
    using Outcome = B3FlowMeshWatch::Outcome;
    const auto now{m_watch_clock.elapsed()};
    const auto& scope{result->scope};
    const auto selected{market()};
    // Applied only on top of exactly the head it was armed against, for the
    // same wallet, market and account. While a review is open it is shown
    // only (the local engine; see liveWhileBusy), never applied; while an
    // operation is in flight it is dropped. Anything else is dropped
    // silently: never a failure, and never renewed freshness.
    const auto& basis{watchBasis()};
    if (!m_wallet || scope.wallet != m_wallet || scope.generation != m_generation || scope.watch_generation != m_watch_generation ||
        m_cancel->load() || (m_busy && !liveWhileBusy()) || !selected || selected->id != scope.market || selected->account != scope.account ||
        !basis || basis->market != scope.market || basis->head != scope.known_head) {
        m_watch.Dropped();
        scheduleRearm();
        return;
    }
    const double jitter{QRandomGenerator::global()->generateDouble() * 0.4 - 0.2};
    const auto before{m_watch.mode};
    // Classified per reply as well: the local engine names no endpoint, so
    // a reply that does is paced as a remote endpoint's, at once.
    if (!result->endpoint.isEmpty()) m_watch.Remote();
    const bool wrong_identity{result->snapshot && (result->snapshot->market != selected->id || result->snapshot->base != selected->base ||
        result->snapshot->domain != selected->domain || result->snapshot->config != selected->config ||
        (!result->snapshot->unchanged && selected->has_account && selected->account != result->snapshot->account))};
    if (!result->snapshot || wrong_identity) {
        // Reads poll again (with failover) while the watch backs off.
        m_watch.Finished(now, Outcome::Failed, jitter);
        m_reconcile_pending = true;
    } else {
        auto& s{*result->snapshot};
        const uint64_t displayed{basis->next_sequence ? basis->next_sequence - 1 : 0};
        const bool display_only{m_busy};
        const bool head_moved{!s.unchanged && s.head != scope.known_head};
        const auto outcome{B3FlowMeshWatch::Classify(false, result->status.toStdString(), result->supported, result->waited,
                                                     head_moved, result->endpoint_live_ms)};
        const bool live_head_change{B3FlowMeshWatch::HeadMovedDuringWait(result->status.toStdString(), result->supported, result->gap, head_moved)};
        const uint64_t reply_head{!s.unchanged && s.next_sequence ? s.next_sequence - 1 : displayed};
        // The selected row as this reply determines it, derived before the
        // reply moves into the display (it is never copied).
        const auto index{m_market->currentIndex()};
        std::optional<B3FlowMeshTrading::Market> row;
        if (!display_only && index >= 0 && static_cast<size_t>(index) < m_market_data.size()) { row = m_market_data[index]; DeriveRowFromSnapshot(*row, s); }
        if (display_only) {
            applyDisplayOnly(std::move(s), result->snapshot_age);
        } else if (applySelectedSnapshot(std::move(s), result->snapshot_age, scope.known_head, live_head_change) == SnapshotApply::APPLIED) {
            if (row) m_market_data[index] = std::move(*row);
            m_read_failed = false; m_read_failures = 0; m_link_attempts = 0; m_read_error.clear(); m_loading = false;
            linkToast(true);
        }
        m_watch.Finished(now, outcome, jitter);
        if (outcome == Outcome::Changed || outcome == Outcome::Quiet) m_watch_watchdog = false;
        // Left live updates, or gave up probing, after replies the client
        // does not close the connection for (superseded, revalidate, ...):
        // it would otherwise stay open against the endpoint's connection
        // limit, reused by each later probe and never idling out.
        if ((before == B3FlowMeshWatch::Mode::Live || before == B3FlowMeshWatch::Mode::Probing) &&
            m_watch.mode == B3FlowMeshWatch::Mode::Polling && outcome == Outcome::NotWaited) {
            m_watch_release_pending = true;
        }
        if (result->gap) {
            m_reconcile_pending = true;
            addGapMarker(scope.market, displayed, reply_head);
            updateDataViews();
        }
    }
    // A failure or gap leaves m_reconcile_pending set: the next passive job
    // reads the tracked request's status first, so a missed event cannot
    // hide its outcome, without discarding the scheduled phase.
    updateMarketText();
    armWatch();
    if (!m_watch_thread) scheduleRearm();
}

void B3FlowMeshTradingPanel::scheduleRearm()
{
    const auto wait{m_watch.NextArmIn(m_watch_clock.elapsed())};
    // Now (0): armWatch refused for another reason (busy, hidden, a read in
    // flight); the tick tries again. Never: Off or already outstanding.
    if (wait <= 0 || m_closing || m_cancel->load()) return;
    m_rearm->start(static_cast<int>(std::min<int64_t>(wait, B3FlowMeshWatch::Policy::UNSUPPORTED_RETRY_MS)));
}

void B3FlowMeshTradingPanel::rearmWatch()
{
    if (m_watch_thread) return;
    armWatch();
    if (!m_watch_thread) scheduleRearm();
}

const std::optional<B3FlowMeshMarketData::Snapshot>& B3FlowMeshTradingPanel::displayedSnapshot() const
{
    const bool newer{m_display && m_snapshot && m_display->market == m_snapshot->market && m_display->next_sequence > m_snapshot->next_sequence};
    return newer ? m_display : m_snapshot;
}

bool B3FlowMeshTradingPanel::liveWhileBusy() const
{
    const auto selected{market()};
    // Only while a review (or another dialog) is open with no job in flight:
    // never while the user's own order, retry or preparation is being sent,
    // nor during the read that opens a review. A local read takes the node's
    // market mutex, which the submission also needs and nothing can preempt.
    return m_busy && !m_thread && m_watch.local && m_watch.mode == B3FlowMeshWatch::Mode::Live && !m_read_failed && m_security_warning.isEmpty() &&
           isVisible() && selected && m_snapshot && m_snapshot->market == selected->id && !m_snapshot->remote;
}

auto B3FlowMeshTradingPanel::applyDisplayOnly(B3FlowMeshMarketData::Snapshot fresh, const QElapsedTimer& age) -> SnapshotApply
{
    const auto& shown{displayedSnapshot()};
    if (!shown || fresh.market != shown->market) return SnapshotApply::SUPERSEDED;
    // Any verified reply at or after the shown head confirms what is shown.
    if (fresh.unchanged ? fresh.head != shown->head : fresh.next_sequence < shown->next_sequence) return SnapshotApply::SUPERSEDED;
    m_display_confirmed = age;
    if (fresh.unchanged || fresh.head == shown->head) return SnapshotApply::SUPERSEDED;
    m_display = std::move(fresh);
    mergeHeldTicks();
    updateDataViews();
    return SnapshotApply::APPLIED;
}

void B3FlowMeshTradingPanel::dropCaughtUpDisplay()
{
    if (m_display && (!m_snapshot || m_display->market != m_snapshot->market || m_display->next_sequence <= m_snapshot->next_sequence)) m_display.reset();
}

void B3FlowMeshTradingPanel::stopWatchWorker(const bool join)
{
    ++m_watch_generation;
    m_watch.Dropped();
    if (!m_watch_thread || !join) return;
    disconnect(m_watch_thread, nullptr, this, nullptr);
    m_watch_thread->wait();
    delete m_watch_thread;
    m_watch_thread = nullptr;
}

QString B3FlowMeshTradingPanel::liveUpdateText() const
{
    using Mode = B3FlowMeshWatch::Mode;
    const auto seconds{(m_watch.RetryInMs(m_watch_clock.elapsed()) + 999) / 1000};
    QString text;
    if (m_watch_watchdog) text = tr("No live update for 15 s · reconnecting");
    else switch (m_watch.mode) {
    case Mode::Off: break;
    case Mode::Live: text = tr("live updates"); break;
    case Mode::Probing: text = tr("starting live updates"); break;
    case Mode::Backoff: text = tr("live updates reconnecting in %1 s").arg(seconds); break;
    case Mode::Polling:
        if (!m_watch_advertised) text = tr("polling every 0.5 s (endpoint offers no live updates)");
        else if (m_watch.last == B3FlowMeshWatch::Outcome::Busy) text = tr("polling (live-update capacity full, retry in %1 s)").arg(seconds);
        else text = tr("polling (live updates unavailable, retry in %1 s)").arg(seconds);
        break;
    }
    const auto selected{market()};
    const bool recent_gap{selected && std::any_of(m_gap_markers.begin(), m_gap_markers.end(), [&](const auto& marker) {
        return marker.market == selected->id && marker.age.isValid() && marker.age.elapsed() < GAP_SUFFIX_MS;
    })};
    if (recent_gap) text += text.isEmpty() ? tr("event gap") : tr(" · event gap");
    return text;
}

void B3FlowMeshTradingPanel::updateLinkBanner()
{
    using B3FlowMeshMarketData::LinkState;
    const auto selected{market()};
    const bool matched{selected && m_snapshot && m_snapshot->market == selected->id};
    B3FlowMeshMarketData::LinkInputs inputs;
    inputs.wallet = m_wallet && m_backend;
    inputs.read_failed = m_read_failed;
    inputs.response_age_ms = matched && m_response_age.isValid() ? m_response_age.elapsed() : -1;
    inputs.polling_ms = m_polling_since.isValid() ? m_polling_since.elapsed() : -1;
    const auto state{B3FlowMeshMarketData::ClassifyLink(inputs)};
    // Wall-clock time appears only in this text, never in a gate.
    const bool since_known{matched && m_last_live_market == selected->id && m_last_live_wall_ms > 0};
    const QString since{since_known ? QDateTime::fromMSecsSinceEpoch(m_last_live_wall_ms).toString(QStringLiteral("HH:mm:ss")) : QString{}};
    const auto duration = [this](int64_t ms) {
        const int64_t seconds{std::max<int64_t>(0, ms) / 1000};
        return seconds < 60 ? tr("%1 s").arg(seconds) : seconds < 3600 ? tr("%1 min").arg(seconds / 60) : tr("%1 h").arg(seconds / 3600);
    };
    QString text, role;
    switch (state) {
    case LinkState::None: break;
    case LinkState::Checking: text = tr("Checking connection…"); role = QStringLiteral("secondary"); break;
    case LinkState::Live: {
        // Live follows the data the gate uses, whichever endpoint served it.
        const QString host{QUrl{m_snapshot->endpoint}.host()};
        text = tr("Connected · %1 · %2").arg(!m_snapshot->remote ? tr("your node") : host.isEmpty() ? tr("remote endpoint") : host,
            m_watch.mode == B3FlowMeshWatch::Mode::Live ? tr("live updates") : tr("polling"));
        role = QStringLiteral("positive"); break;
    }
    case LinkState::Slow:
        text = inputs.response_age_ms >= 0 ? tr("Waiting for an update · last verified %1 ago · new orders paused").arg(duration(inputs.response_age_ms))
                                           : tr("Waiting for verified market data · new orders paused");
        role = QStringLiteral("accent"); break;
    case LinkState::Reconnecting: {
        const int64_t wait{m_attempt_age.isValid() ? static_cast<int64_t>(readBackoffMs()) - m_attempt_age.elapsed() : 0};
        text = tr("Reconnecting · attempt %1 · %2 · %3").arg(std::max(1U, m_link_attempts))
                   .arg(m_thread || wait <= 0 ? tr("retrying now") : tr("next try in %1 s").arg((wait + 999) / 1000),
                        since_known ? tr("showing verified data from %1").arg(since) : tr("no verified data yet"));
        role = QStringLiteral("accent"); break;
    }
    case LinkState::Stale:
        text = since_known ? tr("Stale since %1 (%2) · last verified data shown · trading disabled").arg(since, duration(QDateTime::currentMSecsSinceEpoch() - m_last_live_wall_ms))
                           : tr("No verified market data · trading disabled");
        role = QStringLiteral("negative"); break;
    }
    const bool gap{selected && ((matched && m_snapshot->event_gap) || std::any_of(m_gap_markers.begin(), m_gap_markers.end(), [&](const auto& marker) {
        return marker.market == selected->id && marker.age.isValid() && marker.age.elapsed() < GAP_SUFFIX_MS;
    }))};
    if (!text.isEmpty() && gap) text += tr(" · event gap: request outcomes are being re-checked");
    if (m_link_banner->text() != text) m_link_banner->setText(text);
    if (m_link_banner->property("b3role").toString() != role) {
        B3Theme::markTextRole(m_link_banner, role);
        m_link_banner->style()->unpolish(m_link_banner); m_link_banner->style()->polish(m_link_banner);
    }
    m_link_banner->setVisible(!text.isEmpty());
}

void B3FlowMeshTradingPanel::addGapMarker(const QString& market, const uint64_t from_sequence, const uint64_t to_sequence)
{
    GapMarker marker{market, from_sequence, std::max(from_sequence, to_sequence), {}};
    marker.age.start();
    m_gap_markers.push_back(std::move(marker));
    while (m_gap_markers.size() > MAX_GAP_MARKERS) m_gap_markers.pop_front();
    notice(to_sequence > from_sequence
        ? tr("Live updates missed events between auctions #%1 and #%2; balances and orders were re-verified from a certified snapshot; your pending requests are being re-checked. No request was resent.").arg(from_sequence).arg(to_sequence)
        : tr("Live updates missed events at auction #%1; balances and orders were re-verified from a certified snapshot; your pending requests are being re-checked. No request was resent.").arg(to_sequence));
}

void B3FlowMeshTradingPanel::applyMarketCatalog(const Result& result)
{
    const auto previous_market{market()};
    const QString previous{previous_market ? previous_market->id : QString{}};
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
    // A selection this catalog moved (the market left the catalog, an asset
    // was routed) never reads the ticket typed for the market it left. Only
    // the market's identity counts: a new account row of the same market
    // (the first admission) keeps a typed order.
    if (previous_market && (!selected || previous_market->id != selected->id || previous_market->base != selected->base)) clearTicketEntries();
    if (previous_market && (!selected || previous_market->id != selected->id || previous_market->base != selected->base ||
        previous_market->vault != selected->vault || previous_market->domain != selected->domain ||
        previous_market->config != selected->config || previous_market->account != selected->account || previous_market->remote != selected->remote)) {
        m_amount->clear(); m_withdrawal_draft_amount.clear(); m_destination->clear(); m_deposit_txid->clear(); m_deposit_vout->setText(QStringLiteral("0"));
    }
    if (m_snapshot && (!selected || selected->id != m_snapshot->market || selected->base != m_snapshot->base ||
        selected->domain != m_snapshot->domain || selected->config != m_snapshot->config || selected->remote != m_snapshot->remote)) {
        m_snapshot.reset(); m_response_age.invalidate(); m_certificate_age.invalidate(); m_queue_age.invalidate();
    }
    dropCaughtUpDisplay();
    // The job's rows were captured when it started. When a newer head was
    // displayed meanwhile, its selected row must not roll back to them.
    const auto index{m_market->currentIndex()};
    const uint64_t basis{result.snapshot && selected && result.snapshot->market == selected->id ? result.snapshot->next_sequence : result.basis_sequence};
    if (m_snapshot && selected && m_snapshot->market == selected->id && m_snapshot->next_sequence > basis &&
        index >= 0 && static_cast<size_t>(index) < m_market_data.size() &&
        (!m_market_data[index].has_account || m_market_data[index].account == m_snapshot->account)) {
        DeriveRowFromSnapshot(m_market_data[index], *m_snapshot);
    }
}

void B3FlowMeshTradingPanel::stopWorker()
{
    if (!m_thread) return; disconnect(m_thread, nullptr, this, nullptr); m_thread->wait(); delete m_thread; m_thread = nullptr;
}
void B3FlowMeshTradingPanel::cancelAndWait()
{
    resetRefreshSchedule();
    m_timer->stop(); m_rearm->stop(); m_cancel->store(true); m_deferred_review.reset(); m_deferred_status.reset(); m_deferred_connect.reset(); ++m_generation;
    m_busy = true; updateControls();
    if (m_wallet) disconnect(m_wallet, nullptr, this, nullptr);
    if (m_confirmation) m_confirmation->reject();
    if (m_funding_dialog) m_funding_dialog->reject();
    stopWorker();
    // A wallet switch never waits for an outstanding live-update read: its
    // result is dropped by generation. Only closing the panel joins it.
    stopWatchWorker(m_closing);
    m_watch.Disable(); m_watch_key.clear(); m_watch_watchdog = false; m_watch_release_pending = false;
    m_reconcile_pending = false; m_last_job_was_reconcile = false;
    m_polling_since.invalidate();
    m_gap_markers.clear();
    m_display.reset(); m_display_confirmed.invalidate();
    // Draining a running write does not prove it was cancelled. Preserve its
    // public identity even if a wallet switch disconnects the completion slot.
    if (m_active_result && m_active_result->write_attempted) {
        markUncertain(m_active_result);
        // A completed, verified receipt remains certified even when its GUI
        // completion callback was still queued when teardown began.
        if (m_active_result->receipt && m_active_result->receipt->Included()) applyReceipt(*m_active_result->receipt);
        notice(tr("Wallet changed or operation closed after submission began. Inspect this saved request; it was not cancelled or retried.\n%1").arg(m_uncertain_details));
    }
    m_active_result.reset(); m_inflight_action.reset(); m_inflight_market.clear(); restoreLock();
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
    m_uncertain_action = result->action;
    m_uncertain_title = result->action ? QString{} : tr("Resent saved request");
    if (result->action) m_uncertain_details += QStringLiteral("\n") + B3FlowMeshTrading::Describe(*result->action);
    if (result->prepared) m_uncertain_details += tr("\nExact transaction ID: %1").arg(result->prepared->txid);
    if (!m_uncertain_action_id.isEmpty()) m_uncertain_details += tr("\nExact action ID: %1").arg(m_uncertain_action_id);
    if (result->action && result->receipt) {
        m_receipt = result->receipt; m_receipt_wallet = m_wallet;
        m_pending_market = result->action->market.id; m_pending_account = result->action->market.account;
        if (SignedAction(result->action->operation)) m_pending_sequence = result->action->market.sequence;
        m_tracked_action = *result->action; m_tracked_kind = B3FlowMeshTrading::KindOf(result->action->operation);
        m_tracked_title_key = RequestKey(m_pending_market, m_pending_account, result->receipt->action_id);
    }
    observeRequests();
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
                saved.receipt = received; saved.receipt.no_resubmit |= protected_before || received.Included() || received.NotExecuted();
            }
        }
        updateReceiptCard(); observeRequests(); return;
    }
    // ActionId is semantic and does not include market/domain/config. Keep
    // replay protection and uncertainty attached to the full local scope.
    const bool protected_before{m_receipt && m_receipt->action_id == received.action_id && m_receipt->market == received.market &&
        m_receipt->account == received.account && receiptWalletSelected() && m_receipt->no_resubmit};
    // A proven non-execution never resubmits either: the exact bytes can
    // never execute, so "Retry exact saved action" is hidden too.
    m_receipt = received; m_receipt->no_resubmit |= protected_before || receipt.Included() || receipt.NotExecuted(); m_receipt_error.clear();
    const bool same_uncertain{m_uncertain_action_id == received.action_id && m_uncertain_market == received.market && m_uncertain_account == received.account && uncertainWalletSelected()};
    // Only the local engine's own answer about a credited deposit resolves
    // it; a remote endpoint's claim never resolves anything.
    const bool local_deposit_credited{receipt.endpoint.isEmpty() && receipt.reason_code == QStringLiteral("DEPOSIT_ALREADY_CREDITED")};
    if (m_receipt->Included() || m_receipt->no_resubmit || m_receipt->Terminal() || m_receipt->NotExecuted() || local_deposit_credited) {
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
    updateReceiptCard(); observeRequests();
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
    observeRequests();
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

void B3FlowMeshTradingPanel::observeRequests()
{
    using namespace B3FlowMeshTrading;
    if (!m_wallet) return;
    const bool seed{!m_requests_seeded};
    // Teardown and wallet switches record, never announce.
    const bool announce{!seed && !m_cancel->load()};
    enum : unsigned { CERTIFIED = 1, EXECUTED = 2, NOT_EXECUTED = 4, UNCERTAIN = 8, REFUSED = 16 };
    const auto observe = [&](const QString& key, const Receipt& receipt, const SavedAction* saved) {
        auto seen{m_request_seen.find(key)};
        if (seen == m_request_seen.end()) {
            seen = m_request_seen.emplace(key, RequestSeen{}).first;
            m_request_order.push_back(key);
            while (m_request_order.size() > MAX_REQUEST_KEYS) { m_request_seen.erase(m_request_order.front()); m_request_order.pop_front(); }
        }
        // Settled history (most saved rows) needs no stage text: its events
        // follow from the receipt's verified facts, as in DescribeStage. Only a
        // final outcome is "not executed"; one that may still execute is not settled.
        const bool outcome{receipt.Included() && receipt.outcome_verified && receipt.outcome};
        const bool settled{receipt.Settled()};
        std::optional<StageView> view;
        const auto describe = [&]() -> const StageView& {
            if (!view) {
                const auto kind{key == m_tracked_title_key ? m_tracked_kind : saved ? KindOf(*saved) : RequestKind::Other};
                const auto [decimals, ticker]{requestUnits(receipt.market)};
                const auto market_row{std::find_if(m_market_data.begin(), m_market_data.end(), [&](const auto& m) { return m.id == receipt.market; })};
                view = DescribeStage(receipt, market_row != m_market_data.end() && !market_row->remote, kind, decimals, ticker);
            }
            return *view;
        };
        unsigned now{(receipt.Included() ? CERTIFIED : 0U) |
                     (outcome && receipt.outcome->status == QStringLiteral("applied") ? EXECUTED : 0U) |
                     (receipt.Terminal() && receipt.outcome->status != QStringLiteral("applied") ? NOT_EXECUTED : 0U)};
        if (!settled) {
            // Only the historical facts are kept: never Certified or an outcome.
            seen->second.high = std::max(seen->second.high, std::min(describe().reached, Stage::Admitted));
            now |= (describe().flag == StageFlag::Uncertain ? UNCERTAIN : 0U) | (describe().flag == StageFlag::Refused ? REFUSED : 0U);
            // Unsettled again (included, but it may still execute): it lingers
            // afresh once a final outcome settles it.
            seen->second.settled = false;
        } else if (!seen->second.settled) {
            seen->second.settled = true;
            if (!seed) { seen->second.settled_age.start(); m_last_settled.start(); }
        }
        const unsigned fresh{now & ~seen->second.events};
        seen->second.events |= now;
        if (!announce || !fresh) return;
        const QString title{requestTitle(key, saved, receipt.action_id)};
        const auto& shown{describe()};
        using Kind = B3FlowMeshToasts::Kind;
        using std::chrono::seconds;
        if (fresh & EXECUTED) toast(key + QStringLiteral(":executed"), Kind::Success, tr("Executed: %1 · %2").arg(title, shown.outcome.mid(2)), seconds{8});
        else if (fresh & NOT_EXECUTED) toast(key + QStringLiteral(":not-executed"), Kind::Warning, tr("%1 · %2").arg(title, shown.status), seconds{20});
        else if (fresh & CERTIFIED) {
            const QString when{observedClock(receipt.microblock_sequence, receipt.microblock_hash)};
            toast(key + QStringLiteral(":certified"), Kind::Info, when.isEmpty() ? tr("Certified: %1 · included in auction #%2").arg(title).arg(receipt.microblock_sequence)
                                                                                 : tr("Certified: %1 · included in the auction at %2").arg(title, when), seconds{8});
        }
        if (fresh & UNCERTAIN) toast(key + QStringLiteral(":uncertain"), Kind::Warning, tr("Outcome unknown: %1 · review before trading; it is never re-signed.").arg(title), seconds{20});
        if (fresh & REFUSED) toast(key + QStringLiteral(":refused"), Kind::Warning, tr("Refusal reported: %1 · treated as unknown until reviewed.").arg(title), seconds{20});
    };
    if (m_receipt && receiptWalletSelected()) {
        const auto saved{std::find_if(m_saved_actions.actions.begin(), m_saved_actions.actions.end(), [&](const auto& a) {
            return a.market == m_receipt->market && a.account == m_receipt->account && a.receipt.action_id == m_receipt->action_id;
        })};
        observe(RequestKey(m_receipt->market, m_receipt->account, m_receipt->action_id), *m_receipt, saved == m_saved_actions.actions.end() ? nullptr : &*saved);
    }
    for (const auto& action : m_saved_actions.actions) {
        // The tracked receipt is fresher than its outbox row.
        if (m_receipt && receiptWalletSelected() && action.market == m_receipt->market && action.account == m_receipt->account && action.receipt.action_id == m_receipt->action_id) continue;
        observe(RequestKey(action.market, action.account, action.receipt.action_id), action.receipt, &action);
    }
    if (m_saved_actions_ready) m_requests_seeded = true;
    updatePendingView();
}

QString B3FlowMeshTradingPanel::requestTitle(const QString& key, const B3FlowMeshTrading::SavedAction* saved, const QString& action_id) const
{
    if (key == m_tracked_title_key && m_tracked_action) return B3FlowMeshTrading::ShortTitle(*m_tracked_action, titleView());
    if (saved) return B3FlowMeshTrading::ShortTitle(*saved, titleView());
    return tr("Request %1…").arg(action_id.left(12));
}

B3FlowMeshTrading::TitleView B3FlowMeshTradingPanel::titleView() const
{
    B3FlowMeshTrading::TitleView view;
    view.inverse = inverted();
    const auto selected{market()};
    if (!selected || !m_snapshot || m_snapshot->market != selected->id || !m_snapshot->units.known || m_snapshot->units.asset != selected->base) return view;
    view.market = selected->id; view.units = m_snapshot->units;
    // The view's one price rule, only as the book last showed this market
    // in this orientation (never another market's or orientation's places).
    if (m_book_market == selected->id && m_order_book->shownInverse() == view.inverse) view.places = m_order_book->pricePlaces();
    return view;
}

std::pair<std::optional<int>, QString> B3FlowMeshTradingPanel::requestUnits(const QString& market) const
{
    const auto selected{this->market()};
    if (!selected || selected->id != market || !m_snapshot || m_snapshot->market != market || !m_snapshot->units.known || m_snapshot->units.asset != selected->base) return {std::nullopt, {}};
    return {m_snapshot->units.decimals, m_snapshot->units.ticker};
}

void B3FlowMeshTradingPanel::toast(const QString& key, const B3FlowMeshToasts::Kind kind, const QString& text, const std::chrono::milliseconds ttl)
{
    if (!m_wallet || m_cancel->load()) return;
    if (m_toasts->push(key, kind, text, ttl)) notice(text);
}

void B3FlowMeshTradingPanel::linkToast(const bool restored)
{
    using std::chrono::seconds;
    if (restored) {
        if (!m_link_toast_down) return;
        m_link_toast_down = false;
        toast(QStringLiteral("link:restored:%1").arg(m_link_toast_episode), B3FlowMeshToasts::Kind::Success, tr("Connection restored · verified market data is current again."), seconds{8});
        return;
    }
    if (m_link_toast_down || (m_link_toast_age.isValid() && m_link_toast_age.elapsed() < 30'000)) return;
    m_link_toast_down = true; m_link_toast_age.start();
    toast(QStringLiteral("link:lost:%1").arg(++m_link_toast_episode), B3FlowMeshToasts::Kind::Warning,
          tr("Connection lost · last verified data is shown and new orders are paused. Your requests are re-checked first when it returns."), seconds{20});
}

void B3FlowMeshTradingPanel::updatePendingView()
{
    using namespace B3FlowMeshTrading;
    // Every title below reads in the view as it is now.
    m_titles_view = titleView();
    const auto selected{market()};
    const auto [decimals, ticker]{requestUnits(selected ? selected->id : QString{})};
    const bool local{selected && !selected->remote};
    struct Row { QStringList cells, access; QString request_tip, status_tip; QColor color; };
    std::vector<Row> rows;
    std::set<QString> shown;
    size_t more{0};
    const QStringList columns{tr("Signed"), tr("Sent"), tr("Admitted"), tr("Certified")};
    const auto push = [&](Row row) { if (rows.size() < MAX_REQUEST_ROWS) rows.push_back(std::move(row)); else ++more; };
    // Fast path: nothing settled within the linger interval.
    const bool lingering{m_last_settled.isValid() && m_last_settled.elapsed() < REQUEST_LINGER_MS};
    const auto settled_recently = [&](const QString& key) {
        if (!lingering) return false;
        const auto seen{m_request_seen.find(key)};
        return seen != m_request_seen.end() && seen->second.settled_age.isValid() && seen->second.settled_age.elapsed() < REQUEST_LINGER_MS;
    };
    const auto settled = [](const Receipt& r) { return r.Settled(); };
    const auto receipt_row = [&](const QString& key, const QString& title, const Receipt& r, RequestKind kind) {
        const auto view{DescribeStage(r, local, kind, decimals, ticker)};
        Stage reached{view.reached};
        if (const auto seen{m_request_seen.find(key)}; seen != m_request_seen.end() && reached < Stage::Certified) reached = std::max(reached, seen->second.high);
        Row row;
        row.cells << title;
        // Unknown, refused, or verified but not final (it may still execute).
        const bool stuck{view.flag == StageFlag::Uncertain || view.flag == StageFlag::Refused || view.flag == StageFlag::NotFinal};
        for (int i{0}; i < columns.size(); ++i) {
            const auto stage{static_cast<Stage>(static_cast<int>(Stage::Signed) + i)};
            QString cell, access;
            if (reached >= stage) { cell = stage == Stage::Certified && view.flag == StageFlag::CertifiedEarlier ? tr("✓ earlier") : QStringLiteral("✓"); access = tr("done"); }
            else if (static_cast<int>(stage) == static_cast<int>(reached) + 1 && !view.settled) { cell = stuck ? QStringLiteral("!") : QStringLiteral("…"); access = stuck ? tr("unknown") : tr("waiting"); }
            else if (view.settled) { cell = QStringLiteral("—"); access = tr("not reached"); }
            row.cells << cell; row.access << (access.isEmpty() ? QString{} : columns[i] + QStringLiteral(": ") + access);
        }
        row.cells << view.outcome << view.status;
        row.access << (view.outcome.isEmpty() ? QString{} : tr("Outcome: %1").arg(view.outcome)) << QString{};
        row.request_tip = tr("Action ID: %1\nMarket: %2").arg(r.action_id, r.market);
        row.status_tip = DescribeReceipt(r);
        row.color = view.flag == StageFlag::NotExecuted ? B3Theme::kNegative : stuck ? B3Theme::kAccent : view.settled ? B3Theme::kPositive : QColor{};
        push(std::move(row));
        shown.insert(key);
    };
    if (m_wallet && m_inflight_action && selected && m_inflight_market == selected->id) {
        push({{ShortTitle(*m_inflight_action, m_titles_view), QStringLiteral("…"), {}, {}, {}, {}, tr("Checking, signing and sending…")},
              {tr("Signed: in progress"), {}, {}, {}, {}, {}}, {}, tr("Nothing is certified yet. There is no automatic retry."), {}});
    }
    if (m_wallet && m_uncertain && m_uncertain_action_id.isEmpty() && uncertainWalletSelected()) {
        push({{m_uncertain_action ? ShortTitle(*m_uncertain_action, m_titles_view) : m_uncertain_title.isEmpty() ? tr("Request") : m_uncertain_title,
               QStringLiteral("!"), {}, {}, {}, {}, tr("Outcome unknown · no action ID was returned · review before trading")},
              {tr("Signed: unknown"), {}, {}, {}, {}, {}}, {}, m_uncertain_details, B3Theme::kAccent});
    }
    if (m_wallet && selected) {
        if (m_receipt && receiptWalletSelected() && m_pending_market == selected->id) {
            const QString key{RequestKey(m_receipt->market, m_receipt->account, m_receipt->action_id)};
            if (!settled(*m_receipt) || settled_recently(key)) {
                const auto found{std::find_if(m_saved_actions.actions.begin(), m_saved_actions.actions.end(), [&](const auto& a) {
                    return a.market == m_receipt->market && a.account == m_receipt->account && a.receipt.action_id == m_receipt->action_id;
                })};
                const SavedAction* saved{found == m_saved_actions.actions.end() ? nullptr : &*found};
                receipt_row(key, requestTitle(key, saved, m_receipt->action_id), *m_receipt,
                            key == m_tracked_title_key ? m_tracked_kind : saved ? KindOf(*saved) : RequestKind::Other);
            }
        }
        for (const auto& action : m_saved_actions.actions) {
            if (action.market != selected->id || (settled(action.receipt) && !lingering)) continue;
            const QString key{RequestKey(action.market, action.account, action.receipt.action_id)};
            if (shown.contains(key) || (settled(action.receipt) && !settled_recently(key))) continue;
            receipt_row(key, requestTitle(key, &action, action.receipt.action_id), action.receipt, KindOf(action));
        }
    }
    std::vector<QStringList> cells;
    for (const auto& row : rows) cells.push_back(row.cells);
    if (more) cells.push_back({tr("%1 more in Saved requests").arg(more), {}, {}, {}, {}, {}, {}});
    SetRows(m_pending_view, cells);
    for (int r{0}; r < static_cast<int>(rows.size()); ++r) {
        const auto& row{rows[r]};
        for (int c{0}; c < m_pending_view->columnCount(); ++c) {
            auto* item{m_pending_view->item(r, c)};
            if (!item) continue;
            if (c >= 1 && c <= 4 && item->textAlignment() != Qt::AlignCenter) item->setTextAlignment(Qt::AlignCenter);
            const QString access{c >= 1 && c <= 5 ? row.access.value(c - 1) : QString{}};
            if (item->data(Qt::AccessibleTextRole).toString() != access) item->setData(Qt::AccessibleTextRole, access);
            const QString tip{c == 0 ? row.request_tip : c == 6 ? row.status_tip : QString{}};
            if (item->toolTip() != tip) item->setToolTip(tip);
        }
        if (auto* status{m_pending_view->item(r, 6)}) {
            const QBrush brush{row.color.isValid() ? QBrush{row.color} : QBrush{}};
            if (status->foreground() != brush) status->setForeground(brush);
        }
    }
    m_pending_view->setVisible(!cells.empty());
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
