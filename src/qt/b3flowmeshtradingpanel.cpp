// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/b3flowmeshtradingpanel.h>
#include <qt/b3flowmeshchart.h>
#include <qt/b3theme.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <util/moneystr.h>
#include <QApplication>
#include <QAbstractButton>
#include <QCheckBox>
#include <QButtonGroup>
#include <QComboBox>
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
#include <QVBoxLayout>
#include <algorithm>
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
bool ReadOnly(const std::string& method) {
    return method == "listflowmeshmarkets" || method == "getflowmeshbalance" || method == "getblockchaininfo" ||
        method == "getflowmeshmarketdata" || method == "listflowmeshvaultoperations" || method == "testmempoolaccept";
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

struct B3FlowMeshTradingPanel::Result {
    std::optional<Action> action;
    std::optional<B3AssetTransfer::Prepared> prepared;
    std::vector<Market> markets;
    std::vector<UniValue> effects;
    std::optional<B3FlowMeshMarketData::Snapshot> snapshot;
    UniValue response;
    QString wallet, error;
    bool broadcast{false}, write_attempted{false}, catalog{false};
};

B3FlowMeshTradingPanel::B3FlowMeshTradingPanel(QWidget* parent) : QWidget{parent}
{
    setObjectName(QStringLiteral("flowMeshTradingPanel"));
    auto* outer{new QVBoxLayout{this}}; outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll{new QScrollArea{this}}; scroll->setWidgetResizable(true);
    auto* content{new QWidget{scroll}}; auto* layout{new QVBoxLayout{content}};
    layout->setContentsMargins(12, 10, 12, 12); layout->setSpacing(8);
    auto* heading{new QHBoxLayout}; auto* heading_copy{new QVBoxLayout};
    auto* eyebrow{Label(tr("FlowMesh"), content)}; B3Theme::markTextRole(eyebrow, QStringLiteral("h3")); heading_copy->addWidget(eyebrow);
    m_pair_title = Label(tr("Trade"), content); m_pair_title->hide(); heading->addLayout(heading_copy);
    m_market = new QComboBox{content}; m_market->setObjectName(QStringLiteral("flowMeshMarket"));
    m_market->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); m_market->setMinimumContentsLength(16); m_market->setAccessibleName(tr("Spot market")); heading->addWidget(m_market);
    m_last_price = Label(tr("Last price  —"), content); B3Theme::markTextRole(m_last_price, QStringLiteral("h3")); heading->addWidget(m_last_price); heading->addStretch();
    m_deposit = new QPushButton{tr("Deposit"), content}; m_deposit->setProperty("b3variant", QStringLiteral("primary")); m_withdraw = new QPushButton{tr("Withdraw"), content}; heading->addWidget(m_deposit); heading->addWidget(m_withdraw); layout->addLayout(heading);
    m_status = Label(tr("Select a wallet to trade."), content); m_status->setObjectName(QStringLiteral("flowMeshReadiness")); B3Theme::markTextRole(m_status, QStringLiteral("secondary")); layout->addWidget(m_status);
    m_refresh = new QPushButton{tr("Refresh now"), content}; m_refresh->setToolTip(tr("Market data updates automatically. Request an immediate read-only update."));
    m_progress = Label(QString{}, content); B3Theme::markTextRole(m_progress, QStringLiteral("secondary"));
    auto* center{new QSplitter{Qt::Horizontal, content}}; center->setChildrenCollapsible(false);
    auto* chart_card{new QWidget{center}}; B3Theme::markCard(chart_card); auto* chart_layout{new QVBoxLayout{chart_card}};
    auto* chart_modes{new QHBoxLayout}; auto* modes{new QButtonGroup{chart_card}};
    for (const auto& mode : {std::pair{tr("Chart"), B3FlowMeshChart::Mode::Prices}, std::pair{tr("Depth"), B3FlowMeshChart::Mode::Liquidity}}) {
        auto* button{new QPushButton{mode.first, chart_card}}; button->setCheckable(true); button->setChecked(mode.second == B3FlowMeshChart::Mode::Prices); button->setProperty("b3variant", QStringLiteral("timeframe")); modes->addButton(button); chart_modes->addWidget(button);
        button->setObjectName(mode.second == B3FlowMeshChart::Mode::Prices ? QStringLiteral("flowMeshChartPrices") : QStringLiteral("flowMeshChartLiquidity"));
        connect(button, &QPushButton::clicked, this, [this, mode] { m_chart->setMode(mode.second); });
    }
    chart_modes->addStretch(); chart_layout->addLayout(chart_modes); m_chart = new B3FlowMeshChart{chart_card}; chart_layout->addWidget(m_chart, 1);
    m_chart->setToolTip(tr("Each price point is a real certified clearing, indexed by microblock sequence—not an invented timestamp. Idle intervals are not filled in. Liquidity lines are a display guide through exact evaluated samples."));
    const auto table = [](QWidget* parent, const QStringList& headers, const char* name) {
        auto* view{new QTableWidget{parent}}; view->setObjectName(QLatin1String(name)); view->setColumnCount(headers.size()); view->setHorizontalHeaderLabels(headers); view->verticalHeader()->hide(); view->setShowGrid(false); view->setAlternatingRowColors(false); view->setEditTriggers(QAbstractItemView::NoEditTriggers); view->setSelectionMode(QAbstractItemView::NoSelection); view->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); view->verticalHeader()->setDefaultSectionSize(26); view->setMinimumWidth(200); return view;
    };
    auto* liquidity_card{new QWidget{center}}; B3Theme::markCard(liquidity_card); auto* liquidity_layout{new QVBoxLayout{liquidity_card}};
    auto* liquidity_title{Label(tr("Liquidity"), liquidity_card)}; liquidity_title->setObjectName(QStringLiteral("flowMeshLiquidityTitle")); B3Theme::markTextRole(liquidity_title, QStringLiteral("h3")); liquidity_layout->addWidget(liquidity_title);
    m_depth_view = table(liquidity_card, {tr("Price"), tr("Demand"), tr("Supply")}, "flowMeshCurveDepth"); liquidity_layout->addWidget(m_depth_view, 1);
    m_liquidity_note = Label(tr("No certified curves yet."), liquidity_card); B3Theme::markTextRole(m_liquidity_note, QStringLiteral("secondary")); liquidity_layout->addWidget(m_liquidity_note);
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
    // Funding/admission data lives in a modal flow, never in the trade ticket.
    m_asset = new QComboBox{this}; m_asset->addItems({tr("Base asset"), QStringLiteral("B3")}); m_asset->hide();
    m_amount = entry("flowMeshFundingAmount", 64); m_amount->hide(); m_deposit_txid = entry("flowMeshDepositTxid", 64); m_deposit_txid->hide(); m_deposit_vout = entry("flowMeshDepositVout", 10); m_deposit_vout->setText(QStringLiteral("0")); m_deposit_vout->hide();
    m_admit = new QPushButton{this}; m_admit->hide();
    scroll->setWidget(content); outer->addWidget(scroll);
    connect(m_refresh, &QPushButton::clicked, this, &B3FlowMeshTradingPanel::refresh);
    connect(m_market, &QComboBox::currentIndexChanged, this, [this] {
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
    if (m_wallet) disconnect(m_wallet, nullptr, this, nullptr);
    m_wallet = wallet; m_backend.reset(); m_market_data.clear(); m_effect_data.clear(); m_snapshot.reset();
    m_response_age.invalidate(); m_certificate_age.invalidate(); m_catalog_age.invalidate(); m_attempt_age.invalidate(); m_queue_age.invalidate(); m_read_failed = false; m_read_failures = 0;
    m_uncertain_refreshed = false;
    { QSignalBlocker blocker{m_market}; m_market->clear(); } m_effect->clear();
    m_wallet_name = wallet ? wallet->getDisplayName() : tr("No wallet");
    if (wallet) {
        for (auto& candidate : wallet->node().walletLoader().getWallets()) {
            if (candidate->wallet() == wallet->wallet().wallet()) { m_backend = std::move(candidate); break; }
        }
        connect(wallet, &WalletModel::unload, this, [this] { setWalletModel(nullptr); });
        connect(wallet, &QObject::destroyed, this, [this] { setWalletModel(nullptr); });
        m_timer->start();
    }
    m_cancel->store(false); updateDataViews(); updateMarketText(); refresh();
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

void B3FlowMeshTradingPanel::updateMarketText()
{
    const auto selected{market()};
    const bool matched{selected && m_snapshot && m_snapshot->market == selected->id};
    QString status{m_wallet ? tr("Select a market") : tr("Select a wallet to trade")};
    if (!selected) { m_balances->clear(); m_pair_title->setText(tr("Trade")); m_identity_detail->setText(tr("Select a market to see its details.")); }
    else {
        m_pair_title->setText(matched && m_snapshot->units.known ? m_snapshot->units.ticker + QStringLiteral(" / B3") : tr("Spot market · %1…").arg(selected->base.left(12)));
        m_pair_title->setToolTip(tr("Wallet: %1\nBase asset: %2\nMarket: %3\nVault: %4").arg(m_wallet_name, selected->base, selected->id, selected->vault));
        m_identity_detail->setText(m_pair_title->toolTip() + (matched && m_snapshot->units.known ? tr("\nMetadata: %1 · %2 decimals · %3\nNames and tickers do not prove reserves or dollar backing.").arg(m_snapshot->units.source).arg(m_snapshot->units.decimals).arg(m_snapshot->units.test_only ? tr("TEST ASSET") : tr("asset identity shown above")) : tr("\nToken precision has not been verified.")));
        status = m_read_failed ? tr("Updates delayed · trading paused") : tr("Loading market…");
        if (!matched) m_balances->clear();
        if (matched) {
            const auto& s{*m_snapshot};
            if (m_read_failed || !m_response_age.isValid() || m_response_age.elapsed() > 3000) status = tr("Updates delayed · trading paused");
            else if (!s.running) status = tr("Market service offline");
            else if (s.halt != QStringLiteral("none") || !s.error.isEmpty()) status = tr("Market halted · see Details");
            else if (s.handoff) status = tr("Validator handoff · trading paused");
            else if (s.paused) status = tr("Trading paused · waiting for validators");
            else if (!s.certified) status = tr("Waiting for the first confirmed update");
            else if (s.pending_actions && m_queue_age.isValid() && m_queue_age.elapsed() >= 30'000) status = tr("Confirmation delayed · new requests paused");
            else if (!B3FlowMeshMarketData::AdmissionReady(s, m_response_age.elapsed(), -1)) status = tr("Trading unavailable · check Details");
            else if (m_pending_sequence && selected->id == m_pending_market && selected->account == m_pending_account && selected->sequence <= *m_pending_sequence) status = tr("Request submitted · awaiting confirmation");
            else if (s.pending_actions) status = tr("Confirming orders…");
            else status = tr("Latest confirmed market data");
            if (!s.units.known) status += tr(" · asset precision unavailable");
            if (s.units.test_only) status = tr("TEST ASSET · unbacked · ") + status;
            const auto base = [&](CAmount n) -> QString {
                if (s.units.known) return B3FlowMeshMarketData::FormatAmount(n, s.units.decimals) + QLatin1Char(' ') + s.units.ticker;
                return QString::number(n) + tr(" raw units (precision unknown)");
            };
            m_balances->setText(selected->has_account ? tr("Available  %1 · %2 B3    |    In orders  %3 · %4 B3").arg(base(selected->base_available), B3FlowMeshMarketData::FormatAmount(selected->b3_available, 9), base(selected->base_reserved), B3FlowMeshMarketData::FormatAmount(selected->b3_reserved, 9)) : tr("Deposit to start trading"));
            m_identity_detail->setText(m_identity_detail->text() + tr("\nAccount %1 · next sequence %2").arg(selected->account, QString::number(selected->sequence)));
        }
    }
    if (selected && selected->checkpoint_pending) m_progress->setText(tr("Certified checkpoint awaiting B3 publication. This is separate from trade execution."));
    else if (selected && m_pending_sequence && selected->id == m_pending_market && selected->sequence <= *m_pending_sequence) m_progress->setText(tr("Request accepted · awaiting account sequence %1 to be certified. No retry will be sent.").arg(*m_pending_sequence));
    else if (matched) m_progress->setText(tr("Certified microblock #%1 · epoch %2 · configured threshold %3 of %4 FN seats · %5 queued actions\nAn available runtime or configured threshold is not proof that those validators are currently online.").arg(m_snapshot->next_sequence ? m_snapshot->next_sequence - 1 : 0).arg(m_snapshot->epoch).arg(m_snapshot->quorum_required).arg(m_snapshot->active_seats).arg(m_snapshot->pending_actions));
    else m_progress->setText(tr("Uniform-price curve auction · no price-time priority, no fabricated prices or trades."));
    if (matched) m_progress->setText(B3FlowMeshMarketData::StatusText(*m_snapshot, m_read_failed || !m_response_age.isValid() ? -1 : m_response_age.elapsed(), m_certificate_age.isValid() ? m_certificate_age.elapsed() : -1, m_queue_age.isValid() ? m_queue_age.elapsed() : -1) + QLatin1Char('\n') + m_progress->text());
    if (m_uncertain) status = tr("Submission outcome unknown · review before trading") + (matched && m_snapshot->units.test_only ? tr(" · TEST ASSET · unbacked") : QString{});
    if (!m_security_warning.isEmpty()) status = m_security_warning + QLatin1Char('\n') + status;
    m_status->setText(status); m_status->setToolTip(m_progress->text());
    updateTicket(); updateControls();
}

void B3FlowMeshTradingPanel::updateDataViews()
{
    using namespace B3FlowMeshMarketData;
    const auto selected{market()}; const bool matched{selected && m_snapshot && selected->id == m_snapshot->market};
    m_chart->setSnapshot(matched ? m_snapshot : std::nullopt); m_chart->setLoading(m_loading);
    if (!matched || !m_snapshot->units.known) {
        for (auto* table : {m_depth_view, m_history_view, m_own_view}) SetRows(table, {});
        if (auto* title{m_depth_view->parentWidget()->findChild<QLabel*>(QStringLiteral("flowMeshLiquidityTitle"))}) title->setText(tr("Liquidity"));
        m_history_note->setText(tr("Trade history unavailable")); m_own_note->setText(tr("Orders unavailable"));
        m_last_price->setText(tr("Last price  —")); m_liquidity_note->setText(matched ? tr("Asset precision unavailable") : tr("Loading liquidity…")); return;
    }
    const auto& s{*m_snapshot}; const auto& u{s.units};
    m_own_note->setText(s.own_curves.empty() ? tr("No open orders") : tr("Your open orders"));
    m_own_note->setToolTip(tr("Orders are persistent certified curves. Replacing a side changes that entire curve; there is no price-time priority queue."));
    std::vector<QStringList> depth, history, own;
    for (const auto& d : s.depth) depth.push_back({FormatPrice(d.price, u.decimals), FormatAmount(d.demand, u.decimals), FormatAmount(d.supply, u.decimals)});
    m_depth_view->horizontalHeaderItem(0)->setToolTip(tr("Price in B3 per %1").arg(u.ticker));
    for (int column : {1, 2}) m_depth_view->horizontalHeaderItem(column)->setToolTip(tr("Quantity in %1, evaluated at this price").arg(u.ticker));
    if (auto* title{m_depth_view->parentWidget()->findChild<QLabel*>(QStringLiteral("flowMeshLiquidityTitle"))}) title->setText(tr("Liquidity · %1").arg(u.ticker));
    m_liquidity_note->setText(s.curves.empty() ? tr("No orders yet") : s.curves_complete ? tr("Price in B3 / %1").arg(u.ticker) : tr("Partial liquidity only"));
    m_liquidity_note->setToolTip(tr("Price is B3 per %1; demand and supply are quantities of %1. Each quantity is evaluated at that price, not added as a cumulative order-book level. Only certified curves are shown.").arg(u.ticker));
    QString last_price{tr("Last price  —")};
    for (auto it{s.history.rbegin()}; it != s.history.rend(); ++it) {
        if (!it->cleared || it->quantity == 0) continue;
        if (history.empty()) last_price = tr("%1 B3 / %2").arg(FormatPrice(it->price, u.decimals), u.ticker);
        history.push_back({QStringLiteral("#%1").arg(it->sequence), FormatPrice(it->price, u.decimals), FormatAmount(it->quantity, u.decimals), it->own_fills_known ? FormatAmount(it->own_buy, u.decimals) : QStringLiteral("—"), it->own_fills_known ? FormatAmount(it->own_sell, u.decimals) : QStringLiteral("—")});
    }
    for (const auto& c : s.own_curves) {
        QString description;
        if (c.points.size() == 2 && c.points[1].price == c.points[0].price + 1) description = FormatPrice(c.side == QStringLiteral("bid") ? c.points[0].price : c.points[1].price, u.decimals) + QStringLiteral(" B3");
        else description = tr("%1-point curve").arg(c.points.size());
        own.push_back({c.side == QStringLiteral("bid") ? tr("Buy") : tr("Sell"), description, FormatAmount(c.remaining, u.decimals), FormatAmount(c.reserved, c.side == QStringLiteral("bid") ? 9 : u.decimals) + (c.side == QStringLiteral("bid") ? QStringLiteral(" B3") : QLatin1Char(' ') + u.ticker)});
    }
    SetRows(m_depth_view, depth); SetRows(m_history_view, history); SetRows(m_own_view, own);
    for (int i{0}; i < static_cast<int>(s.own_curves.size()); ++i) { const auto& c{s.own_curves[i]}; m_own_view->item(i, 0)->setToolTip(tr("Certified account %1\nFilled: %2 %3\nCancellation targets this account's whole selected side.").arg(c.account, FormatAmount(c.filled, u.decimals), u.ticker)); }
    m_last_price->setText(last_price);
    m_history_note->setText(!s.history_available ? tr("History unavailable on this node") : history.empty() ? tr("No trades in recent history") : s.history_truncated || s.history_page_partial ? tr("Recent trades · partial history") : tr("Confirmed trades"));
    m_history_note->setToolTip(tr("Only retained certified clearings are shown, indexed by microblock sequence. A dash means your fill is unknown, not zero. Missing history does not prove that no trading occurred."));
}

void B3FlowMeshTradingPanel::updateTicket()
{
    using namespace B3FlowMeshMarketData;
    const auto selected{market()}; const bool units_known{selected && m_snapshot && selected->id == m_snapshot->market && m_snapshot->units.known};
    const QString ticker{units_known ? m_snapshot->units.ticker : tr("token")}; const bool buy{m_side->currentIndex() == 0};
    m_price_label->setText(tr("Limit price · B3 / %1").arg(ticker)); m_quantity_label->setText(tr("Quantity · %1").arg(ticker));
    m_order->setText(buy ? tr("Review buy order…") : tr("Review sell order…")); m_cancel_order->setText(buy ? tr("Cancel buy order…") : tr("Cancel sell order…"));
    m_ticket_fee->setText(tr("Spot fee 0.01% · seller-paid"));
    m_ticket_fee->setToolTip(tr("The protocol fee is 0.01% of matched B3 notional, deducted from seller proceeds. Actual fees and allocation depend on certified fills. Submitting an order has no network fee."));
    if (!units_known) { m_grid_note->setText(tr("Verified asset precision required; no raw-unit guessing.")); m_grid_note->setToolTip(QString{}); m_ticket_available->setText(tr("Available  —")); m_ticket_total->setText(tr("Order value  —")); return; }
    const auto& u{m_snapshot->units}; m_grid_note->setText(tr("Steps: %1 %2 · %3 B3").arg(FormatAmount(u.quantity_step, u.decimals), ticker, FormatPrice(u.price_step, u.decimals)));
    m_grid_note->setToolTip(tr("Quantity step %1 %2; price step %3 B3 per %2. Inputs are exact and never rounded.").arg(FormatAmount(u.quantity_step, u.decimals), ticker, FormatPrice(u.price_step, u.decimals)));
    m_ticket_available->setText(tr("Available  %1 %2").arg(FormatAmount(buy ? selected->b3_available : selected->base_available, buy ? 9 : u.decimals), buy ? QStringLiteral("B3") : ticker));
    QString error; const auto price{ParsePrice(m_price->text(), u, &error)}, quantity{ParseQuantity(m_quantity->text(), u, &error)};
    if (!price || !quantity) { m_ticket_total->setText(tr("Order value  —")); return; }
    const auto total{Notional(*price, *quantity)}; if (!total) { m_ticket_total->setText(tr("Order value exceeds supported range")); return; }
    m_ticket_total->setText(tr("Order value  %1 B3").arg(FormatAmount(*total, 9)));
    m_ticket_fee->setToolTip(tr("Protocol fee: 0.01%, deducted from seller proceeds. Whole-auction example at this notional: %1 B3. Your final share depends on certified fills; submitting an order has no network fee.").arg(FormatAmount(*FeeExample(*total), 9)));
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
    auto* layout{new QVBoxLayout{dialog}}; layout->addWidget(Label(tr("Wallet: %1\nMarket: %2 / B3\nFull base asset ID: %3").arg(m_wallet_name, ticker, selected->base), dialog));
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
    const bool signing{idle && !m_uncertain && !m_backend->privateKeysDisabled()};
    const bool data_ready{selected && m_snapshot && m_snapshot->market == selected->id && !m_read_failed && B3FlowMeshMarketData::AdmissionReady(*m_snapshot, m_response_age.isValid() ? m_response_age.elapsed() : -1, m_certificate_age.isValid() ? m_certificate_age.elapsed() : -1, m_queue_age.isValid() ? m_queue_age.elapsed() : -1)};
    const bool ready{signing && selected && selected->ready && data_ready};
    const bool known{m_snapshot && selected && m_snapshot->market == selected->id && m_snapshot->units.known};
    const bool pending{selected && m_pending_sequence && selected->id == m_pending_market && selected->account == m_pending_account && selected->sequence <= *m_pending_sequence};
    m_refresh->setEnabled(idle && !m_thread); m_market->setEnabled(idle);
    for (auto* field : {m_price, m_quantity, m_amount, m_destination, m_deposit_txid, m_deposit_vout}) field->setEnabled(idle);
    m_side->setEnabled(idle); m_asset->setEnabled(idle); m_effect->setEnabled(idle);
    m_buy->setEnabled(idle); m_sell->setEnabled(idle);
    m_order->setEnabled(ready && known && selected->has_account && !pending); m_cancel_order->setEnabled(ready && selected->has_account && !pending);
    m_withdraw->setEnabled(ready && selected->has_account && !pending); m_deposit->setEnabled(ready);
    m_admit->setEnabled(ready && selected->has_account);
    m_checkpoint->setEnabled(signing && selected && selected->publish_ready && selected->checkpoint_pending);
    m_publish->setEnabled(signing && selected && selected->publish_ready && m_effect->currentIndex() >= 0);
    m_review_uncertain->setVisible(m_uncertain);
    m_review_uncertain->setEnabled(idle && !m_thread && !m_read_failed && m_uncertain && m_uncertain_refreshed && m_uncertain_backend && m_backend->wallet() == m_uncertain_backend->wallet());
    m_chart->setStale(m_snapshot && (m_read_failed || !m_response_age.isValid() || m_response_age.elapsed() > 3000));
    m_chart->setLoading(m_loading);
}

void B3FlowMeshTradingPanel::refresh()
{
    if (!m_wallet || !m_backend || m_busy || m_thread) return;
    if (m_read_failures && m_attempt_age.isValid() && m_attempt_age.elapsed() < std::min(10000U, 500U << std::min(4U, m_read_failures))) return;
    startJob();
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
    Action a; a.operation = operation; a.market = *selected; a.side = m_side->currentText(); a.native = m_asset->currentIndex() == 1;
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
            const auto price{B3FlowMeshMarketData::ParsePrice(m_price->text(), m_snapshot->units, &error)}, quantity{B3FlowMeshMarketData::ParseQuantity(m_quantity->text(), m_snapshot->units, &error)};
            if (!price || !quantity) throw std::runtime_error{error.toStdString()}; a.price = *price; a.amount = *quantity;
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

void B3FlowMeshTradingPanel::startJob(std::optional<Action> action, std::optional<B3AssetTransfer::Prepared> prepared)
{
    if (!m_wallet || !m_backend || m_thread) return;
    const bool watch_queue{action && action->operation != Operation::Checkpoint && action->operation != Operation::Vault &&
        m_snapshot && m_snapshot->market == action->market.id && m_snapshot->pending_actions > 0 && m_queue_age.isValid()};
    const QElapsedTimer queue_watch{m_queue_age};
    // Recheck after any modal review/unlock/fee dialog, including the second
    // exact-transaction review. A rejected start must restore acquired unlock.
    if (watch_queue && queue_watch.elapsed() >= 30'000) {
        restoreLock(); m_busy = false; notice(tr("Queued requests have not certified for 30 seconds. No new request was submitted. Refresh and inspect the market; existing requests are not canceled.")); updateControls(); return;
    }
    m_busy = action.has_value(); m_loading = !action && !m_snapshot; m_cancel->store(false); m_attempt_age.restart(); updateControls(); m_chart->setLoading(m_loading);
    auto result{std::make_shared<Result>()}; result->action = action; result->prepared = prepared; result->broadcast = prepared.has_value(); result->wallet = m_wallet_name;
    m_active_result = result;
    auto* node{&m_wallet->node()}; const auto backend{m_backend}; const auto cancel{m_cancel};
    const auto uri{B3AssetTransfer::WalletUri(m_wallet->getWalletName())}; const auto generation{m_generation};
    const auto selected{market()}; const QString selected_id{selected ? selected->id : QString{}};
    result->catalog = !action && (!m_catalog_age.isValid() || m_catalog_age.elapsed() >= 5000 || m_route_pending || m_market_data.empty());
    result->markets = m_market_data; result->effects = m_effect_data;
    const QString known_head{m_snapshot && m_snapshot->market == selected_id ? m_snapshot->head : QString{}};
    const QString route_base{m_route_pending ? m_requested_base : QString{}};
    m_thread = QThread::create([node, backend, cancel, uri, result, selected_id, known_head, route_base, watch_queue, queue_watch] {
        try {
            const auto cancelled = [&] { return cancel->load() || node->shutdownRequested(); };
            const B3FlowMeshTrading::RpcCall rpc = [&](const std::string& method, const UniValue& params) {
                if (cancelled()) throw std::runtime_error{"Operation cancelled."};
                if (!ReadOnly(method)) {
                    // Readiness RPCs may themselves outlast the local deadline.
                    // Do not access UI timers from this worker; this captured
                    // monotonic observation only fails closed until refresh.
                    if (watch_queue && queue_watch.elapsed() >= 30'000) throw std::runtime_error{"Queued requests have not certified for 30 seconds. Refresh before submitting; no new mutation was started."};
                    if (method != "submitflowmeshdeposit" && (backend->isLocked() || backend->privateKeysDisabled())) throw std::runtime_error{"The captured wallet is locked for spending."};
                    if (method == "sendrawtransaction" || method == "submitflowmeshorder" || method == "cancelflowmeshorder" || method == "requestflowmeshwithdrawal" || method == "submitflowmeshdeposit") result->write_attempted = true;
                }
                return node->executeRpc(method, params, method == "getblockchaininfo" || method == "testmempoolaccept" ? "" : uri);
            };
            if (!result->action) {
                if (result->catalog) result->markets = B3FlowMeshTrading::ParseMarkets(rpc("listflowmeshmarkets", UniValue{UniValue::VARR}));
                auto selected{std::find_if(result->markets.begin(), result->markets.end(), [&](const auto& m) { return route_base.isEmpty() ? m.id == selected_id : m.base == route_base; })};
                if (selected == result->markets.end() && selected_id.isEmpty() && route_base.isEmpty()) selected = result->markets.begin();
                if (selected != result->markets.end()) {
                    UniValue params{UniValue::VARR}; params.push_back(selected->id.toStdString()); UniValue options{UniValue::VOBJ}; options.pushKV("limit", 100); options.pushKV("curve_limit", 128);
                    if (!known_head.isEmpty() && selected->id == selected_id && !result->catalog) options.pushKV("known_head", known_head.toStdString());
                    params.push_back(options); result->snapshot = B3FlowMeshMarketData::Parse(rpc("getflowmeshmarketdata", params));
                    const auto& s{*result->snapshot};
                    if (s.market != selected->id || s.base != selected->base || s.domain != selected->domain || s.config != selected->config) throw std::runtime_error{"Certified data does not match the selected market identity."};
                    selected->publish_ready = selected->publish_ready && s.running;
                    selected->ready = selected->publish_ready && !s.paused && !s.handoff && s.halt == QStringLiteral("none") && s.error.isEmpty();
                    if (!s.unchanged) {
                        if (selected->has_account && selected->account != s.account) throw std::runtime_error{"Certified data belongs to another wallet account."};
                        selected->has_account = !s.account.isEmpty(); selected->account = s.account; selected->sequence = s.account_sequence;
                        selected->base_available = s.base_available; selected->base_reserved = s.base_reserved; selected->b3_available = s.b3_available; selected->b3_reserved = s.b3_reserved;
                    }
                    if (result->catalog && selected->has_account) {
                        result->effects.clear(); UniValue filter{UniValue::VARR}; filter.push_back(selected->id.toStdString());
                        const auto effects{rpc("listflowmeshvaultoperations", filter)};
                        if (!effects.isArray() || effects.size() > 1000) throw std::runtime_error{"Invalid or oversized connected-effect list."};
                        for (const auto& e : effects.getValues()) {
                            if (e.isObject() && e.find_value("market_id").isStr() && e.find_value("market_id").get_str() == selected->id.toStdString() && e.find_value("account_id").isStr() && e.find_value("account_id").get_str() == selected->account.toStdString()) result->effects.push_back(e);
                        }
                    }
                }
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
                } else { B3FlowMeshTrading::CheckActionResult(response, a); result->response = response; }
            }
        } catch (const UniValue& error) { result->error = RpcError(error); }
        catch (const std::exception& error) { result->error = QString::fromUtf8(error.what()).left(500); }
        catch (...) { result->error = QStringLiteral("The FlowMesh operation failed."); }
    });
    m_thread->setParent(this); connect(m_thread, &QThread::finished, this, [this, result, generation] { if (generation == m_generation) finishJob(result); }); m_thread->start(); updateControls();
}

void B3FlowMeshTradingPanel::finishJob(const std::shared_ptr<Result>& result)
{
    stopWorker(); m_active_result.reset();
    if (!m_wallet || m_cancel->load()) { m_deferred_review.reset(); restoreLock(); m_busy = false; updateControls(); return; }
    if (!result->error.isEmpty()) {
        m_deferred_review.reset();
        if (!result->action) {
            if (!m_read_failed) notice(tr("Market refresh failed: %1. Last certified data is retained; new actions are disabled until a successful refresh.").arg(result->error));
            m_read_failed = true; m_read_failures = std::min(5U, m_read_failures + 1); m_loading = false; m_uncertain_refreshed = false; m_busy = false;
            updateMarketText(); if (!m_snapshot) m_status->setText((m_security_warning.isEmpty() ? QString{} : m_security_warning + QLatin1Char('\n')) + tr("Market data unavailable · %1").arg(result->error)); updateControls(); return;
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
        m_loading = false; m_read_failed = false; m_read_failures = 0;
        if (result->catalog) m_catalog_age.restart();
        if (m_uncertain && m_uncertain_backend && m_backend->wallet() == m_uncertain_backend->wallet()) m_uncertain_refreshed = true;
        const QString previous{market() ? market()->id : QString{}};
        const QString previous_effect{m_effect->currentData(Qt::UserRole + 1).toString()};
        m_market_data = result->markets; m_effect_data = result->effects;
        { std::vector<Choice> choices; QString selection{previous}; const bool route_asset{m_route_pending && !m_requested_base.isEmpty()};
            if (route_asset) selection.clear();
            for (size_t i{0}; i < m_market_data.size(); ++i) {
                const auto& m{m_market_data[i]}; const auto* units{result->snapshot && result->snapshot->market == m.id && result->snapshot->units.known ? &result->snapshot->units : m_snapshot && m_snapshot->market == m.id && m_snapshot->units.known ? &m_snapshot->units : nullptr};
                QString label = m.base.left(12) + QStringLiteral("… / B3");
                if (units) label = units->ticker + QStringLiteral(" / B3");
                choices.push_back({label, m.id, m.id});
                if (route_asset && m.base == m_requested_base) selection = m.id;
            }
            SetChoices(m_market, choices, selection, !route_asset);
            if (route_asset && selection.isEmpty()) notice(tr("No established market was reported for the selected asset. This panel does not bootstrap one."));
        }
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
                if (!m_snapshot || m_snapshot->market != fresh.market || m_snapshot->head != fresh.head || m_snapshot->state_root != fresh.state_root) { m_read_failed = true; notice(tr("Unchanged snapshot did not match the retained certificate; data was not reused.")); }
                else {
                    auto retained{*m_snapshot}; retained.running = fresh.running; retained.paused = fresh.paused; retained.handoff = fresh.handoff; retained.observer = fresh.observer; retained.halt = fresh.halt; retained.error = fresh.error; retained.pending_actions = fresh.pending_actions; retained.active_seats = fresh.active_seats; retained.quorum_required = fresh.quorum_required; m_snapshot = std::move(retained); m_response_age.restart();
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
        const bool changed_selection{selected_now && selected_now->id != previous};
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
        if (SignedAction(a.operation)) { m_pending_market = a.market.id; m_pending_account = a.market.account; m_pending_sequence = a.market.sequence; }
        notice(tr("Wallet: %1\nRequest accepted, not yet proof of execution. Action ID: %2\nRefresh certified balances and connected effects; no automatic retry.")
            .arg(result->wallet, QString::fromStdString(result->response.find_value("action_id").get_str())));
    }
    updateMarketText(); refresh();
}

void B3FlowMeshTradingPanel::stopWorker()
{
    if (!m_thread) return; disconnect(m_thread, nullptr, this, nullptr); m_thread->wait(); delete m_thread; m_thread = nullptr;
}
void B3FlowMeshTradingPanel::cancelAndWait()
{
    m_timer->stop(); m_cancel->store(true); m_deferred_review.reset(); ++m_generation;
    if (m_confirmation) m_confirmation->reject();
    if (m_funding_dialog) m_funding_dialog->reject();
    stopWorker();
    // Draining a running write does not prove it was cancelled. Preserve its
    // public identity even if a wallet switch disconnects the completion slot.
    if (m_active_result && m_active_result->write_attempted) {
        markUncertain(m_active_result);
        notice(tr("Wallet changed or operation closed after submission began. Inspect this saved request; it was not retried.\n%1").arg(m_uncertain_details));
    }
    m_active_result.reset(); restoreLock(); m_busy = false;
}
void B3FlowMeshTradingPanel::markUncertain(const std::shared_ptr<Result>& result)
{
    m_uncertain = true; m_uncertain_refreshed = false; m_uncertain_backend = m_backend;
    m_uncertain_details = tr("Captured wallet: %1").arg(result->wallet);
    if (result->action) m_uncertain_details += QStringLiteral("\n") + B3FlowMeshTrading::Describe(*result->action);
    if (result->prepared) m_uncertain_details += tr("\nExact transaction ID: %1").arg(result->prepared->txid);
}
void B3FlowMeshTradingPanel::reviewUncertain()
{
    if (!m_wallet || !m_backend || m_busy || m_thread || !m_uncertain || !m_uncertain_refreshed ||
        !m_security_warning.isEmpty() || !m_uncertain_backend || m_backend->wallet() != m_uncertain_backend->wallet()) return;
    const QPointer<B3FlowMeshTradingPanel> self{this}; const auto generation{m_generation};
    m_busy = true; updateControls();
    const bool accepted{confirm(m_uncertain_details + tr("\n\nA fresh read-only refresh succeeded, but this does NOT prove whether the submission was accepted. Check the saved transaction ID or certified account sequence in wallet/node state.\n\nI have inspected the outcome and want to re-enable NEW actions. This acknowledgement never resends the saved request."), false)};
    if (!self || generation != m_generation) return;
    if (accepted) { m_uncertain = false; m_uncertain_refreshed = false; m_uncertain_backend.reset(); notice(tr("Uncertain submission acknowledged after inspection. Nothing was replayed.")); }
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
