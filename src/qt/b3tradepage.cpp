// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/b3tradepage.h>

#include <qt/b3chartwidget.h>
#include <qt/b3fixed.h>
#include <qt/b3theme.h>

#include <QButtonGroup>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSplitter>
#include <QTableView>
#include <QTabWidget>
#include <QVBoxLayout>

namespace {

constexpr int kPriceDecimals{8};
constexpr int kQuantityDecimals{8};

//! Parse a user-entered decimal string into raw integer units. Digits
//! only — no float round-trip. Returns false on malformed input or
//! overflow.
bool ParseFixed(const QString& text, int decimals, qint64& out)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) return false;
    const QStringList parts = trimmed.split(QLatin1Char('.'));
    if (parts.size() > 2) return false;
    const QString whole_text = parts.at(0);
    QString frac_text = parts.size() == 2 ? parts.at(1) : QString();
    if (frac_text.size() > decimals) return false;
    frac_text = frac_text.leftJustified(decimals, QLatin1Char('0'));

    bool ok{false};
    const qulonglong whole = whole_text.isEmpty() ? 0 : whole_text.toULongLong(&ok);
    if (!whole_text.isEmpty() && !ok) return false;
    const qulonglong frac = frac_text.isEmpty() ? 0 : frac_text.toULongLong(&ok);
    if (!frac_text.isEmpty() && !ok) return false;

    qulonglong scale = 1;
    for (int i = 0; i < decimals; ++i) scale *= 10;
    if (whole > static_cast<qulonglong>(INT64_MAX) / scale) return false;
    const qulonglong raw = whole * scale + frac;
    if (raw > static_cast<qulonglong>(INT64_MAX)) return false;
    out = static_cast<qint64>(raw);
    return true;
}

QTableView* makeCompactTable(QWidget* parent, const char* name)
{
    auto* view = new QTableView(parent);
    view->setObjectName(QLatin1String(name));
    view->setShowGrid(false);
    view->verticalHeader()->setVisible(false);
    view->horizontalHeader()->setStretchLastSection(true);
    view->setFrameShape(QFrame::NoFrame);
    view->setSelectionMode(QAbstractItemView::NoSelection);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    return view;
}

} // namespace

B3TradePage::B3TradePage(QWidget* parent)
    : QWidget{parent}
{
    m_null_backend = new B3NullTradingBackend(this);
    m_backend = m_null_backend;

    m_candles = new B3CandleSeries(this);
    m_candles->setPrecision(kPriceDecimals, kQuantityDecimals);
    m_bids = new B3OrderBookModel(B3OrderBookModel::Side::Bid, this);
    m_asks = new B3OrderBookModel(B3OrderBookModel::Side::Ask, this);
    m_trades = new B3TradesModel(this);
    for (auto* book : {m_bids, m_asks}) {
        book->setPrecision(kPriceDecimals, kQuantityDecimals);
    }
    m_trades->setPrecision(kPriceDecimals, kQuantityDecimals);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(B3Theme::kSpaceLg, B3Theme::kSpaceLg, B3Theme::kSpaceLg, B3Theme::kSpaceLg);
    layout->setSpacing(B3Theme::kSpaceMd);

    // ── Header: honest inactive preview + market identity ────────────
    // Keep identity, market state and timeframe controls on separate rows.
    // Besides being calmer visually, this prevents the inactive Trade page
    // from imposing a desktop-only minimum width on every page in B3Shell's
    // stacked layout.
    auto* topBar = new QVBoxLayout();
    topBar->setSpacing(B3Theme::kSpaceSm);
    auto* identityRow = new QHBoxLayout();
    auto* identityCopy = new QVBoxLayout();
    identityCopy->setContentsMargins(0, 0, 0, 0);
    identityCopy->setSpacing(B3Theme::kSpaceXs);
    auto* eyebrow = new QLabel(tr("B3 FLOWMESH"), this);
    B3Theme::markTextRole(eyebrow, QStringLiteral("eyebrow"));
    identityCopy->addWidget(eyebrow);
    auto* heading = new QLabel(tr("Trading workspace"), this);
    B3Theme::markTextRole(heading, QStringLiteral("h1"));
    identityCopy->addWidget(heading);
    identityRow->addLayout(identityCopy);

    auto* preview = new QLabel(tr("INACTIVE PREVIEW"), this);
    preview->setObjectName(QStringLiteral("tradePreviewBadge"));
    B3Theme::markTextRole(preview, QStringLiteral("accent"));
    identityRow->addWidget(preview);

    m_market_selector = new QComboBox(this);
    m_market_selector->setObjectName(QStringLiteral("marketSelector"));
    m_market_selector->setPlaceholderText(tr("No markets"));
    m_market_selector->setProperty("requiresTradingBackend", true);

    m_market_label = new QLabel(tr("No market selected"), this);
    B3Theme::markTextRole(m_market_label, QStringLiteral("secondary"));
    identityRow->addStretch();
    topBar->addLayout(identityRow);

    auto* controlsRow = new QHBoxLayout();
    auto* timeframes = new QButtonGroup(this);
    for (const QString& tf : {QStringLiteral("1m"), QStringLiteral("15m"), QStringLiteral("1h"), QStringLiteral("1d")}) {
        auto* button = new QPushButton(tf, this);
        button->setCheckable(true);
        button->setObjectName(QStringLiteral("timeframe_") + tf);
        button->setProperty("requiresTradingBackend", true);
        button->setProperty("b3variant", QStringLiteral("timeframe"));
        timeframes->addButton(button);
        controlsRow->addWidget(button);
        if (tf == QLatin1String("1h")) button->setChecked(true);
    }
    controlsRow->addStretch();

    m_availability = new QLabel(this);
    m_availability->setObjectName(QStringLiteral("tradeAvailability"));
    B3Theme::markTextRole(m_availability, QStringLiteral("secondary"));

    auto* marketRow = new QHBoxLayout();
    marketRow->addWidget(m_market_selector);
    marketRow->addWidget(m_market_label);
    marketRow->addStretch();
    marketRow->addWidget(m_availability);
    topBar->addLayout(marketRow);
    topBar->addLayout(controlsRow);
    layout->addLayout(topBar);

    auto* inactiveMessage = new QLabel(
        tr("◇  FlowMesh is packaged as inactive infrastructure. No markets, orders, prices, or balances are presented as live."),
        this);
    inactiveMessage->setObjectName(QStringLiteral("tradeInactiveMessage"));
    inactiveMessage->setWordWrap(true);
    layout->addWidget(inactiveMessage);

    // ── Center: chart | order book + trades ──────────────────────────
    auto* center = new QSplitter(Qt::Horizontal, this);

    auto* chartCard = new QFrame(center);
    B3Theme::markCard(chartCard);
    {
        auto* chartLayout = new QVBoxLayout(chartCard);
        chartLayout->setContentsMargins(B3Theme::kSpaceSm, B3Theme::kSpaceSm, B3Theme::kSpaceSm, B3Theme::kSpaceSm);
        auto* chartTools = new QHBoxLayout();
        auto* candlesButton = new QPushButton(tr("Candles"), chartCard);
        auto* lineButton = new QPushButton(tr("Line"), chartCard);
        auto* resetButton = new QPushButton(tr("Reset"), chartCard);
        candlesButton->setObjectName(QStringLiteral("chartCandles"));
        lineButton->setObjectName(QStringLiteral("chartLine"));
        resetButton->setObjectName(QStringLiteral("chartReset"));
        for (auto* b : {candlesButton, lineButton, resetButton}) {
            b->setProperty("requiresTradingBackend", true);
            chartTools->addWidget(b);
        }
        chartTools->addStretch();
        chartLayout->addLayout(chartTools);

        m_chart = new B3ChartWidget(chartCard);
        m_chart->setSeries(m_candles);
        chartLayout->addWidget(m_chart, 1);

        connect(candlesButton, &QPushButton::clicked, this, [this] { m_chart->setMode(B3ChartWidget::Mode::Candles); });
        connect(lineButton, &QPushButton::clicked, this, [this] { m_chart->setMode(B3ChartWidget::Mode::Line); });
        connect(resetButton, &QPushButton::clicked, this, [this] { m_chart->resetView(); });
    }
    center->addWidget(chartCard);

    auto* bookCard = new QFrame(center);
    B3Theme::markCard(bookCard);
    {
        auto* bookLayout = new QVBoxLayout(bookCard);
        bookLayout->setContentsMargins(B3Theme::kSpaceSm, B3Theme::kSpaceSm, B3Theme::kSpaceSm, B3Theme::kSpaceSm);
        auto* bookTitle = new QLabel(tr("Liquidity preview"), bookCard);
        B3Theme::markTextRole(bookTitle, QStringLiteral("h3"));
        bookLayout->addWidget(bookTitle);

        m_ask_view = makeCompactTable(bookCard, "askView");
        m_ask_view->setModel(m_asks);
        bookLayout->addWidget(m_ask_view, 1);

        m_bid_view = makeCompactTable(bookCard, "bidView");
        m_bid_view->setModel(m_bids);
        bookLayout->addWidget(m_bid_view, 1);

        m_book_state = new QLabel(bookCard);
        m_book_state->setObjectName(QStringLiteral("bookState"));
        B3Theme::markTextRole(m_book_state, QStringLiteral("secondary"));
        m_book_state->setWordWrap(true);
        bookLayout->addWidget(m_book_state);

        auto* tradesTitle = new QLabel(tr("Recent trades"), bookCard);
        B3Theme::markTextRole(tradesTitle, QStringLiteral("h3"));
        bookLayout->addWidget(tradesTitle);
        m_trades_view = makeCompactTable(bookCard, "tradesView");
        m_trades_view->setModel(m_trades);
        bookLayout->addWidget(m_trades_view, 1);
    }
    center->addWidget(bookCard);
    center->setStretchFactor(0, 4);
    center->setStretchFactor(1, 1);
    center->setSizes({900, 250});
    layout->addWidget(center, 3);

    // ── Lower area: inactive limit ticket + spot activity tabs ───────
    auto* lower = new QHBoxLayout();
    lower->setSpacing(B3Theme::kSpaceMd);

    auto* ticketCard = new QFrame(this);
    B3Theme::markCard(ticketCard);
    {
        auto* ticket = new QVBoxLayout(ticketCard);
        ticket->setContentsMargins(B3Theme::kSpaceMd, B3Theme::kSpaceMd, B3Theme::kSpaceMd, B3Theme::kSpaceMd);
        ticket->setSpacing(B3Theme::kSpaceSm);

        auto* sideRow = new QHBoxLayout();
        m_buy = new QPushButton(tr("Buy"), ticketCard);
        m_sell = new QPushButton(tr("Sell"), ticketCard);
        m_buy->setObjectName(QStringLiteral("ticketBuy"));
        m_sell->setObjectName(QStringLiteral("ticketSell"));
        m_buy->setCheckable(true);
        m_sell->setCheckable(true);
        m_buy->setChecked(true);
        m_buy->setProperty("requiresTradingBackend", true);
        m_sell->setProperty("requiresTradingBackend", true);
        auto* sideGroup = new QButtonGroup(ticketCard);
        sideGroup->addButton(m_buy);
        sideGroup->addButton(m_sell);
        sideRow->addWidget(m_buy);
        sideRow->addWidget(m_sell);
        m_order_type = new QComboBox(ticketCard);
        m_order_type->setObjectName(QStringLiteral("orderType"));
        // FlowMesh v1 is represented honestly: the inactive shell exposes
        // no market-order or futures surface.
        m_order_type->addItem(tr("Limit"));
        m_order_type->setProperty("requiresTradingBackend", true);
        sideRow->addWidget(m_order_type);
        ticket->addLayout(sideRow);

        // Digits-and-dot input only; parsing is pure integer math.
        auto* validator = new QRegularExpressionValidator(
            QRegularExpression(QStringLiteral("^[0-9]{0,12}(\\.[0-9]{0,8})?$")), ticketCard);
        auto addField = [&](const QString& title, QLineEdit** field_out, const char* name) {
            auto* row = new QHBoxLayout();
            auto* label = new QLabel(title, ticketCard);
            B3Theme::markTextRole(label, QStringLiteral("secondary"));
            row->addWidget(label);
            row->addStretch();
            *field_out = new QLineEdit(ticketCard);
            (*field_out)->setObjectName(QLatin1String(name));
            (*field_out)->setValidator(validator);
            (*field_out)->setAlignment(Qt::AlignRight);
            (*field_out)->setMaximumWidth(180);
            (*field_out)->setProperty("requiresTradingBackend", true);
            row->addWidget(*field_out);
            ticket->addLayout(row);
        };
        addField(tr("Price"), &m_price, "ticketPrice");
        addField(tr("Quantity"), &m_quantity, "ticketQuantity");

        auto addReadout = [&](const QString& title, QLabel** value_out, const char* name) {
            auto* row = new QHBoxLayout();
            auto* label = new QLabel(title, ticketCard);
            B3Theme::markTextRole(label, QStringLiteral("secondary"));
            row->addWidget(label);
            row->addStretch();
            *value_out = new QLabel(QStringLiteral("—"), ticketCard);
            (*value_out)->setObjectName(QLatin1String(name));
            row->addWidget(*value_out);
            ticket->addLayout(row);
        };
        addReadout(tr("Total"), &m_total, "ticketTotal");
        addReadout(tr("Available balance"), &m_balance, "ticketBalance");
        addReadout(tr("Estimated fee"), &m_fee, "ticketFee");

        m_submit = new QPushButton(ticketCard);
        m_submit->setObjectName(QStringLiteral("ticketSubmit"));
        m_submit->setProperty("requiresTradingBackend", true);
        ticket->addWidget(m_submit);

        m_ticket_note = new QLabel(ticketCard);
        m_ticket_note->setObjectName(QStringLiteral("ticketNote"));
        B3Theme::markTextRole(m_ticket_note, QStringLiteral("secondary"));
        m_ticket_note->setWordWrap(true);
        ticket->addWidget(m_ticket_note);
        ticket->addStretch();

        connect(m_price, &QLineEdit::textChanged, this, &B3TradePage::updateTicketTotal);
        connect(m_quantity, &QLineEdit::textChanged, this, &B3TradePage::updateTicketTotal);
        connect(m_buy, &QPushButton::toggled, this, &B3TradePage::updateAvailability);
        // The submit button is connected to nothing that submits: there
        // is no backend, and this shell must not fake one.
    }
    lower->addWidget(ticketCard, 2);

    m_tabs = new QTabWidget(this);
    {
        auto addEmptyTab = [&](const QString& title, const QStringList& headers, const char* name) {
            auto* view = makeCompactTable(m_tabs, name);
            view->setModel(new B3EmptyTableModel(headers, view));
            m_tabs->addTab(view, title);
        };
        addEmptyTab(tr("Open orders"), {tr("Market"), tr("Side"), tr("Price"), tr("Quantity"), tr("Status")}, "openOrdersView");
        addEmptyTab(tr("Fills"), {tr("Time"), tr("Market"), tr("Side"), tr("Price"), tr("Quantity")}, "fillsView");
        addEmptyTab(tr("Balances"), {tr("Asset"), tr("Available"), tr("Reserved")}, "tradeBalancesView");
    }
    lower->addWidget(m_tabs, 3);
    layout->addLayout(lower, 2);

    updateAvailability();
    updateTicketTotal();
}

void B3TradePage::setBackend(B3TradingBackend* backend)
{
    if (m_backend && m_backend != m_null_backend) {
        disconnect(m_backend, nullptr, this, nullptr);
    }
    m_backend = backend ? backend : m_null_backend;
    if (m_backend != m_null_backend) {
        connect(m_backend, &B3TradingBackend::availabilityChanged, this, &B3TradePage::updateAvailability);
        connect(m_backend, &QObject::destroyed, this, [this] {
            m_backend = m_null_backend;
            updateAvailability();
        });
    }
    updateAvailability();
}

void B3TradePage::updateAvailability()
{
    const bool data_available = m_backend->available();

    m_availability->setText(data_available
                                ? tr("Preview data connected · trading inactive")
                                : tr("Preview only"));

    // The current backend interface can report availability and fee previews,
    // but it has no approved order-submission path. Keep every action control
    // inert even if a test or future data source reports itself available.
    // Tables remain visible as honest read-only previews.
    for (QWidget* control : findChildren<QWidget*>()) {
        if (control->property("requiresTradingBackend").toBool()) {
            control->setEnabled(false);
        }
    }
    m_chart->setEnabled(false);

    m_submit->setEnabled(false);
    m_submit->setText(tr("Trading unavailable"));
    m_ticket_note->setVisible(true);
    m_ticket_note->setText(
        tr("FlowMesh trading is not active in this wallet build. This preview cannot place "
           "orders, reserve balances, or create positions."));

    if (!data_available) {
        m_bids->setState(B3OrderBookModel::State::Unavailable);
        m_asks->setState(B3OrderBookModel::State::Unavailable);
        m_book_state->setVisible(true);
        m_book_state->setText(tr("No live FlowMesh market data is connected. Values shown here are never fabricated."));
        m_balance->setText(QStringLiteral("—"));
    } else {
        m_book_state->setVisible(m_bids->rowCount() == 0 && m_asks->rowCount() == 0);
        m_book_state->setText(tr("Preview order book is empty; trading remains inactive."));
    }
    updateTicketTotal();
}

void B3TradePage::updateTicketTotal()
{
    qint64 price{0};
    qint64 quantity{0};
    const bool have_price = ParseFixed(m_price->text(), kPriceDecimals, price);
    const bool have_quantity = ParseFixed(m_quantity->text(), kQuantityDecimals, quantity);

    if (have_price && have_quantity) {
        const auto total = B3Fixed::mulScaled(price, quantity, kQuantityDecimals);
        m_total->setText(total ? B3Fixed::format(*total, kPriceDecimals) : tr("Too large"));
    } else {
        m_total->setText(QStringLiteral("—"));
    }

    const auto fee = m_backend->estimatedFee(price, quantity);
    m_fee->setText(fee ? B3Fixed::format(*fee, kPriceDecimals) : QStringLiteral("—"));
}
