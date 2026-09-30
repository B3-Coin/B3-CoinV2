// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/b3flowmeshtradetape.h>
#include <qt/b3theme.h>
#include <QDateTime>
#include <QHeaderView>
#include <QLabel>
#include <QScrollBar>
#include <QTableWidget>
#include <QTimeZone>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>

namespace {
QString Who(const QString& source, bool remote)
{
    if (!remote) return B3FlowMeshTradeTape::tr("this node");
    const QUrl url{source};
    return url.isValid() && !url.host().isEmpty() ? url.host() : source;
}
QString Utc(int64_t ms)
{
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")) + QStringLiteral(" UTC");
}
} // namespace

std::vector<B3FlowMeshTradeTape::Row> B3FlowMeshTradeTape::BuildRows(const std::deque<B3FlowMeshMarketData::Tick>& ticks,
                                                                     const std::map<uint64_t, flowmesh::ObservedStamp>& times,
                                                                     const std::map<uint64_t, int64_t>& received)
{
    std::vector<Row> rows;
    std::optional<CAmount> previous;
    int direction{0};
    for (const auto& tick : ticks) {
        if (!tick.cleared) continue;
        if (previous) {
            if (tick.price > *previous) direction = 1;
            else if (tick.price < *previous) direction = -1;
        }
        previous = tick.price;
        Row row;
        row.sequence = tick.sequence;
        row.hash = tick.hash;
        row.price = tick.price;
        row.volume = tick.volume;
        row.direction = direction;
        if (const auto stamp{times.find(tick.sequence)}; stamp != times.end() &&
            QString::fromStdString(stamp->second.hash.GetHex()) == tick.hash) {
            row.stamp = stamp->second;
        } else if (const auto at{received.find(tick.sequence)}; at != received.end()) {
            row.received_ms = at->second;
        }
        rows.push_back(std::move(row));
    }
    std::reverse(rows.begin(), rows.end());
    if (rows.size() > MAX_ROWS) rows.resize(MAX_ROWS);
    return rows;
}

QString B3FlowMeshTradeTape::TimeText(const Row& row, const QString& source, const bool remote, QString* tooltip)
{
    QString text{QStringLiteral("—")}, tip;
    const QString who{Who(source, remote)};
    if (row.stamp && row.stamp->mode == flowmesh::ObservedMode::LIVE) {
        text = QDateTime::fromMSecsSinceEpoch(row.stamp->ms).toLocalTime().toString(QStringLiteral("HH:mm:ss"));
        tip = tr("Observed by %1 at %2 (its own clock, not certified execution time)").arg(who, Utc(row.stamp->ms));
    } else if (row.stamp && row.stamp->mode == flowmesh::ObservedMode::DELAYED) {
        tip = tr("No trade time: %1 applied this auction late (after a catch-up, a lag or a restart), at %2. That is only an upper bound.").arg(who, Utc(row.stamp->ms));
    } else if (row.stamp) {
        tip = tr("No trade time: %1's clock stood behind an earlier stamp when it applied this auction.").arg(who);
    } else if (row.received_ms) {
        text = QDateTime::fromMSecsSinceEpoch(*row.received_ms).toLocalTime().toString(QStringLiteral("HH:mm:ss"));
        tip = tr("Received by this app at %1 (this computer's clock): this server reports no times. The auction was certified at or before then; not a certified or observed trade time.").arg(Utc(*row.received_ms));
    } else {
        tip = source.isEmpty() ? tr("No trade time: this source does not report observed times.")
                               : tr("No trade time: %1 has no observed time for this auction (earlier history, or not observed live).").arg(who);
    }
    if (row.stamp && (row.stamp->flags & flowmesh::OBSERVED_IMPLAUSIBLE)) tip += tr("\nImplausible against this app's own checks; shown as reported.");
    if (row.stamp && (row.stamp->flags & (flowmesh::OBSERVED_CLOCK_STEP | flowmesh::OBSERVED_CLOCK_CLAMPED))) tip += tr("\nThe observer's clock was adjusted around this time.");
    if (row.stamp && (row.stamp->flags & flowmesh::OBSERVED_CLOCK_SUSPECT)) tip += tr("\nThe observer's clock is suspected wrong.");
    tip += tr("\nAuction #%1").arg(row.sequence);
    if (tooltip) *tooltip = tip;
    return text;
}

B3FlowMeshTradeTape::B3FlowMeshTradeTape(QWidget* parent) : QWidget{parent}
{
    setObjectName(QStringLiteral("flowMeshTradeTape"));
    auto* layout{new QVBoxLayout{this}}; layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(5);
    m_table = new QTableWidget{0, 3, this}; m_table->setObjectName(QStringLiteral("flowMeshTradeTapeRows"));
    m_table->setAccessibleName(tr("Recent auctions, newest first"));
    m_table->verticalHeader()->hide(); m_table->verticalHeader()->setDefaultSectionSize(22);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->setShowGrid(false); m_table->setAlternatingRowColors(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_table->setMinimumWidth(240);
    layout->addWidget(m_table, 1);
    m_note = new QLabel{this}; m_note->setObjectName(QStringLiteral("flowMeshTradeTapeNote"));
    m_note->setTextFormat(Qt::PlainText); m_note->setWordWrap(true);
    B3Theme::markTextRole(m_note, QStringLiteral("secondary"));
    layout->addWidget(m_note);
    setRows({}, {}, false, {}, false, {});
}

void B3FlowMeshTradeTape::setRows(const std::vector<Row>& rows, const B3FlowMeshMarketData::Units& units, const bool inverse,
                                  const QString& source, const bool remote, const QString& reason)
{
    using namespace B3FlowMeshMarketData;
    const bool known{units.known && units.decimals >= 0 && units.decimals <= 18};
    const QString token{known ? units.ticker : tr("token")};
    const QString quote{inverse ? token : QStringLiteral("B3")}, base{inverse ? QStringLiteral("B3") : token};
    const QStringList labels{tr("Price (%1)").arg(quote), tr("Size (%1)").arg(base), tr("Time")};
    const QStringList tips{tr("Uniform clearing price of the auction; coloured by its change from the previous auction (there is no taker side)"),
                           inverse ? tr("Matched quantity as gross B3 at the clearing price") : tr("Matched quantity"),
                           tr("Local time the source observed the auction (operator-observed, not certified)")};
    for (int c{0}; c < 3; ++c) {
        if (!m_table->horizontalHeaderItem(c)) m_table->setHorizontalHeaderItem(c, new QTableWidgetItem{labels[c]});
        else if (m_table->horizontalHeaderItem(c)->text() != labels[c]) m_table->horizontalHeaderItem(c)->setText(labels[c]);
        if (m_table->horizontalHeaderItem(c)->toolTip() != tips[c]) m_table->horizontalHeaderItem(c)->setToolTip(tips[c]);
    }
    const int scroll{m_table->verticalScrollBar()->value()};
    const int count{known ? static_cast<int>(rows.size()) : 0};
    if (m_table->rowCount() != count) m_table->setRowCount(count);
    for (int r{0}; r < count; ++r) {
        const auto& row{rows[static_cast<size_t>(r)]};
        const auto price{DisplayPrice(row.price, units, inverse)};
        QString time_tip;
        const QString time{TimeText(row, source, remote, &time_tip)};
        const QStringList values{inverse && row.price == 0 ? QStringLiteral("—") : price.text,
                                 FormatDepthQuantity(row.price, row.volume, units.decimals, inverse), time};
        const QString price_tip{(price.approximate ? tr("Exact price: %1 %2\n").arg(price.exact, quote) : QString{}) +
                                tr("Auction #%1 · uniform price, no taker side").arg(row.sequence)};
        // In the B3 view a higher canonical price is a lower displayed one.
        const int shown{inverse ? -row.direction : row.direction};
        const QColor price_color{shown > 0 ? B3Theme::kPositive : shown < 0 ? B3Theme::kNegative : B3Theme::kTextPrimary};
        for (int c{0}; c < 3; ++c) {
            auto* item{m_table->item(r, c)};
            if (!item) {
                item = new QTableWidgetItem{values[c]};
                item->setTextAlignment((c == 2 ? Qt::AlignRight : Qt::AlignRight) | Qt::AlignVCenter);
                m_table->setItem(r, c, item);
            } else if (item->text() != values[c]) {
                item->setText(values[c]);
            }
            const bool live{row.stamp && row.stamp->mode == flowmesh::ObservedMode::LIVE};
            // Observed live: primary; received by this app: secondary; no time: muted.
            const QColor time_color{live ? ((row.stamp->flags & flowmesh::OBSERVED_IMPLAUSIBLE) ? B3Theme::kWarning : B3Theme::kTextPrimary)
                                         : !row.stamp && row.received_ms ? B3Theme::kTextSecondary : B3Theme::kTextMuted};
            const QColor foreground{c == 0 ? price_color : c == 2 ? time_color : B3Theme::kTextPrimary};
            if (item->foreground().color() != foreground) item->setForeground(foreground);
            const QString tip{c == 0 ? price_tip : c == 2 ? time_tip : QString{}};
            if (item->toolTip() != tip) item->setToolTip(tip);
        }
    }
    if (m_table->verticalScrollBar()->value() != scroll) m_table->verticalScrollBar()->setValue(scroll);
    QString note;
    if (!known) note = tr("Verified asset precision required");
    else if (rows.empty()) note = tr("No matched auctions held yet");
    else if (!source.isEmpty()) note = tr("Times observed by %1 · not certified · local time").arg(Who(source, remote));
    else if (std::any_of(rows.begin(), rows.end(), [](const auto& row) { return row.received_ms.has_value(); })) note = tr("This server reports no times · times received by this app, live only");
    else note = reason == QStringLiteral("endpoint_does_not_advertise_observed_time") ? tr("This server does not report times (older version)")
                                                                                   : tr("No observed times");
    if (remote && known && !rows.empty()) note += tr(" · endpoint-reported");
    if (m_note->text() != note) m_note->setText(note);
}
