// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/b3flowmeshorderbook.h>
#include <qt/b3theme.h>
#include <util/int128.h>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace {
constexpr int DEPTH_ROLE{Qt::UserRole + 1};
constexpr size_t VISIBLE_LEVELS{12};
class BookTable final : public QTableWidget
{
    bool m_asks;
public:
    BookTable(QWidget* parent, bool asks) : QTableWidget{0, 3, parent}, m_asks{asks} {}
    QSize sizeHint() const override
    {
        auto size{QTableWidget::sizeHint()};
        if (m_asks) size.setHeight(horizontalHeader()->sizeHint().height() +
            std::max(rowCount(), 1) * verticalHeader()->defaultSectionSize() + 2 * frameWidth());
        return size;
    }
protected:
    void resizeEvent(QResizeEvent* event) override
    {
        const bool follow{m_asks && verticalScrollBar()->value() == verticalScrollBar()->maximum()};
        QTableWidget::resizeEvent(event);
        if (follow) scrollToBottom();
    }
};
class DepthDelegate final : public QStyledItemDelegate
{
    QTableWidget* m_table;
    QColor m_color;
public:
    DepthDelegate(QTableWidget* table, QColor color) : QStyledItemDelegate{table}, m_table{table}, m_color{color} {}
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        // Floating point is used only for pixel widths, never economic values.
        const double fraction{std::clamp(index.data(DEPTH_ROLE).toDouble(), 0.0, 1.0)};
        const int width{m_table->viewport()->width()};
        const int start{static_cast<int>(width * (1.0 - fraction))};
        painter->save(); painter->setClipRect(option.rect);
        QColor shade{m_color}; shade.setAlpha(27);
        painter->fillRect(QRect{start, option.rect.y(), width - start, option.rect.height()}, shade);
        painter->restore();
        QStyledItemDelegate::paint(painter, option, index);
    }
};
QLabel* Text(QWidget* parent, const char* name)
{
    auto* label{new QLabel{parent}}; label->setObjectName(QLatin1String(name));
    label->setTextFormat(Qt::PlainText); label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse); return label;
}
QTableWidget* Table(QWidget* parent, const char* name, const QColor& color, bool asks)
{
    auto* table{new BookTable{parent, asks}}; table->setObjectName(QLatin1String(name));
    table->verticalHeader()->hide(); table->verticalHeader()->setDefaultSectionSize(24);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->setShowGrid(false); table->setAlternatingRowColors(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setItemDelegate(new DepthDelegate{table, color});
    table->setMinimumHeight(asks ? 0 : 110); table->setMinimumWidth(280);
    if (asks) table->setMaximumHeight(60);
    table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    return table;
}
void Rows(QTableWidget* table, const std::vector<B3FlowMeshMarketData::LimitLevel>& levels, bool asks, bool inverse, int decimals, const QColor& color)
{
    using namespace B3FlowMeshMarketData;
    const size_t count{std::min(VISIBLE_LEVELS, levels.size())};
    const size_t begin{asks ? levels.size() - count : 0};
    const int scroll{table->verticalScrollBar()->value()};
    const bool follow_best{asks && scroll == table->verticalScrollBar()->maximum()};
    if (table->rowCount() != static_cast<int>(count)) {
        table->setRowCount(count);
        if (asks) table->setMaximumHeight(table->horizontalHeader()->sizeHint().height() +
            std::max<size_t>(count, 1) * table->verticalHeader()->defaultSectionSize() + 2 * table->frameWidth());
    }
    util::Unsigned128 total{0}, cumulative{0};
    for (size_t r{begin}; r < begin + count; ++r) total += inverse ? levels[r].gross_notional : levels[r].remaining;
    for (size_t step{0}; step < count; ++step) {
        const size_t row{asks ? count - 1 - step : step};
        const auto& level{levels[begin + row]};
        cumulative += inverse ? level.gross_notional : level.remaining;
        const double fraction{total ? static_cast<double>(cumulative) / static_cast<double>(total) : 0.0};
        const QStringList values{FormatDisplayPrice(level.canonical_price, decimals, inverse), level.amount, level.total};
        const QString tooltip{QObject::tr("Price: %1\nAmount: %2\nTotal: %3\n%4 limit order(s) at this level. Total is this level's notional, not a promised fill. Shading is cumulative amount from the best shown level.").arg(level.price, level.amount, level.total).arg(level.curve_count) +
            (inverse ? QObject::tr("\nExact reciprocal price: %1\nAmount is gross B3 at the limit; original token caps remain unchanged. Actual B3 and fees depend on the auction fill.").arg(ExactInversePrice(level.canonical_price, decimals)) : QString{})};
        for (int c{0}; c < 3; ++c) {
            auto* item{table->item(row, c)};
            if (!item) {
                item = new QTableWidgetItem{values[c]}; item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                table->setItem(row, c, item);
            } else if (item->text() != values[c]) item->setText(values[c]);
            const QColor foreground{c == 0 ? color : B3Theme::kTextPrimary};
            if (item->foreground().color() != foreground) item->setForeground(foreground);
            if (item->data(DEPTH_ROLE) != fraction) item->setData(DEPTH_ROLE, fraction);
            if (item->toolTip() != tooltip) item->setToolTip(tooltip);
        }
    }
    if (follow_best) table->scrollToBottom();
    else if (table->verticalScrollBar()->value() != scroll) table->verticalScrollBar()->setValue(scroll);
}
}

B3FlowMeshOrderBook::B3FlowMeshOrderBook(QWidget* parent) : QWidget{parent}
{
    setObjectName(QStringLiteral("flowMeshOrderBook"));
    auto* layout{new QVBoxLayout{this}}; layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(5);
    m_stale = Text(this, "flowMeshBookStale"); m_stale->setText(tr("Stale snapshot · last known orders"));
    B3Theme::markTextRole(m_stale, QStringLiteral("secondary")); m_stale->hide(); layout->addWidget(m_stale);
    m_asks = Table(this, "flowMeshBookAsks", B3Theme::kNegative, true); m_asks->setAccessibleName(tr("Sell limit levels · best sell at bottom")); layout->addWidget(m_asks, 1, Qt::AlignBottom);
    m_last = Text(this, "flowMeshBookLast"); B3Theme::markTextRole(m_last, QStringLiteral("h3")); layout->addWidget(m_last);
    m_spread = Text(this, "flowMeshBookSpread"); B3Theme::markTextRole(m_spread, QStringLiteral("secondary")); layout->addWidget(m_spread);
    m_bids = Table(this, "flowMeshBookBids", B3Theme::kPositive, false); m_bids->setAccessibleName(tr("Buy limit levels · best buy at top")); layout->addWidget(m_bids, 1);
    m_note = Text(this, "flowMeshBookNote"); B3Theme::markTextRole(m_note, QStringLiteral("secondary")); layout->addWidget(m_note);
    setSnapshot(std::nullopt, false);
}

void B3FlowMeshOrderBook::setStale(bool stale)
{
    m_stale->setVisible(stale);
}

void B3FlowMeshOrderBook::setSnapshot(const std::optional<B3FlowMeshMarketData::Snapshot>& snapshot, bool inverse)
{
    using namespace B3FlowMeshMarketData;
    const bool known{snapshot && snapshot->units.known && !snapshot->base.isEmpty() && snapshot->units.asset == snapshot->base &&
        snapshot->units.decimals >= 0 && snapshot->units.decimals <= 18 && snapshot->units.quantity_step > 0 && snapshot->units.price_step > 0 &&
        MoneyRange(snapshot->units.quantity_step) && MoneyRange(snapshot->units.price_step)};
    const QString token{known ? snapshot->units.ticker : tr("token")};
    const QString base{inverse ? QStringLiteral("B3") : token}, quote{inverse ? token : QStringLiteral("B3")};
    const QStringList labels{tr("Price\n(%1)").arg(quote), tr("Amount\n(%1)").arg(base), tr("Total\n(%1)").arg(quote)};
    for (auto* table : {m_asks, m_bids}) for (int c{0}; c < 3; ++c) {
        if (!table->horizontalHeaderItem(c)) table->setHorizontalHeaderItem(c, new QTableWidgetItem{labels[c]});
        else if (table->horizontalHeaderItem(c)->text() != labels[c]) table->horizontalHeaderItem(c)->setText(labels[c]);
        if (table->horizontalHeaderItem(c)->toolTip() != labels[c]) table->horizontalHeaderItem(c)->setToolTip(labels[c]);
    }
    if (!known || !snapshot->certified || (snapshot->remote && (!snapshot->certificate_verified || !snapshot->account_state_verified))) {
        Rows(m_asks, {}, true, inverse, 0, B3Theme::kNegative); Rows(m_bids, {}, false, inverse, 0, B3Theme::kPositive);
        m_last->setText(tr("Last trade  —")); m_spread->setText(tr("Spread  —"));
        m_note->setText(!snapshot ? tr("Select a market · no snapshot") : !known ? tr("Verified asset precision required") : tr("Verified order state unavailable")); return;
    }
    const auto& s{*snapshot}; const auto book{ProjectLimitBook(s, inverse)};
    Rows(m_asks, book.asks, true, inverse, s.units.decimals, B3Theme::kNegative);
    Rows(m_bids, book.bids, false, inverse, s.units.decimals, B3Theme::kPositive);
    QString last{tr("Last trade  —")};
    for (auto it{s.history.rbegin()}; it != s.history.rend(); ++it) if (it->cleared && it->quantity > 0) {
        last = (s.remote && !s.execution_result_verified ? tr("Reported last  %1 %2") : tr("Last trade  %1 %2"))
            .arg(FormatDisplayPrice(it->price, s.units.decimals, inverse), quote); break;
    }
    m_last->setText(last);
    m_last->setToolTip(tr("Historical last trade, not the current book midpoint. Endpoint-reported history is not independently execution-verified when marked reported."));
    const bool crossed{book.canonical_best_bid && book.canonical_best_ask && *book.canonical_best_bid > *book.canonical_best_ask};
    m_spread->setText(!book.spread.isEmpty() ? tr("Spread  %1 %2").arg(book.spread, quote) : crossed ? tr("Crossed auction levels · no positive spread") : book.complete ? tr("Spread  — · two sides required") : tr("Spread unavailable · partial limit book"));
    QString note{tr("Limit orders · uniform-price auction")};
    if (book.asks.empty() && book.bids.empty()) note += tr("\nNo displayable limit orders");
    if (!s.curves_complete) note += tr("\nPartial snapshot · other orders may be missing");
    if (book.general_curves) note += tr("\n%1 non-limit curve(s): see Curve depth").arg(book.general_curves);
    if (book.invalid_curves) note += tr("\n%1 invalid or overflowing curve(s) omitted").arg(book.invalid_curves);
    if (book.zero_inverse_curves) note += tr("\n%1 zero-price order(s) have no inverse price").arg(book.zero_inverse_curves);
    if (book.asks.size() > VISIBLE_LEVELS || book.bids.size() > VISIBLE_LEVELS) note += tr("\nNearest %1 levels per side shown").arg(VISIBLE_LEVELS);
    if (inverse) note += tr("\nB3 amounts are gross equivalents at limit");
    m_note->setText(note);
    m_note->setToolTip(tr("Only exact limit-shaped curves are grouped by price. General curves are not fabricated into limit orders. There is no price-time priority; a crossed book may wait for the next certified auction. Shading shows cumulative quantity within the visible levels, independently scaled for each side. Prices marked ≈ are display approximations; hover for exact values."));
}
