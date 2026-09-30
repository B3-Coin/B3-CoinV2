// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/b3flowmeshorderbook.h>
#include <qt/b3theme.h>
#include <flowmesh/market_view.h>
#include <util/int128.h>
#include <QApplication>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <set>

using namespace B3FlowMeshMarketData;
using Kind = B3FlowMeshLadderModel::Row::Kind;

namespace {
using Wide = util::Unsigned128;
constexpr const char* ANIMATE_SETTING{"flowmesh/animateOrderBook"};
constexpr const char* SIZE_ON_CLICK_SETTING{"flowmesh/orderBookClickSetsSize"};

QString WideText(Wide n)
{
    QString out;
    do { out.prepend(QChar{static_cast<ushort>('0' + static_cast<unsigned>(n % 10))}); n /= 10; } while (n != 0);
    return out;
}
//! An exact amount of `decimals` places, trailing zeros trimmed.
QString FormatWide(const Wide raw, const int decimals)
{
    QString text{WideText(raw)};
    if (decimals <= 0) return text;
    text = text.rightJustified(decimals + 1, QLatin1Char('0'));
    text.insert(text.size() - decimals, QLatin1Char('.'));
    while (text.endsWith(QLatin1Char('0'))) text.chop(1);
    if (text.endsWith(QLatin1Char('.'))) text.chop(1);
    return text;
}
//! value x 10^exponent as an exact decimal.
QString ScaledDecimal(const Wide value, const int exponent)
{
    if (exponent >= 0) return WideText(value) + QString(exponent, QLatin1Char('0'));
    return FormatWide(value, -exponent);
}
Wide Pow10(int n) { Wide out{1}; while (n-- > 0) out *= 10; return out; }
QLabel* Text(QWidget* parent, const char* name)
{
    auto* label{new QLabel{parent}}; label->setObjectName(QLatin1String(name));
    label->setTextFormat(Qt::PlainText); label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse); return label;
}
bool KnownUnits(const Snapshot& s)
{
    return s.units.known && !s.base.isEmpty() && s.units.asset == s.base && s.units.decimals >= 0 && s.units.decimals <= 18 &&
           s.units.quantity_step > 0 && s.units.price_step > 0 && MoneyRange(s.units.quantity_step) && MoneyRange(s.units.price_step);
}
//! 200 x (ask - bid) / (ask + bid) percent, the same in both orientations,
//! to three places (display only; never below 0.001% when positive).
QString RelativeSpread(const CAmount bid, const CAmount ask)
{
    if (ask <= bid) return {};
    const Wide scaled{Wide{200'000} * static_cast<uint64_t>(ask - bid) / (Wide{static_cast<uint64_t>(ask)} + static_cast<uint64_t>(bid))};
    if (scaled == 0) return QStringLiteral("<0.001%");
    return FormatWide(scaled, 3) + QLatin1Char('%');
}
} // namespace

QVariant B3FlowMeshLadderModel::data(const QModelIndex& index, const int role) const
{
    if (!index.isValid() || index.row() >= rowCount() || index.column() >= 3) return {};
    const Row& row{m_rows[static_cast<size_t>(index.row())]};
    const int column{index.column()};
    switch (role) {
    case Qt::DisplayRole:
        if (row.kind == Kind::Spread) return column == 0 ? row.price : QString{};
        return column == 0 ? row.price : column == 1 ? row.size : row.total;
    case Qt::ToolTipRole: return row.tooltip.isEmpty() ? QVariant{} : QVariant{row.tooltip};
    case Qt::AccessibleTextRole:
        if (row.kind == Kind::Empty) return QString{};
        return row.accessible.isEmpty() ? data(index, Qt::DisplayRole) : QVariant{row.accessible};
    case Qt::ForegroundRole:
        if (row.kind == Kind::Spread) return B3Theme::kTextSecondary;
        if (column == 0 && row.kind == Kind::Ask) return B3Theme::kNegative;
        if (column == 0 && row.kind == Kind::Bid) return B3Theme::kPositive;
        return B3Theme::kTextPrimary;
    case Qt::TextAlignmentRole:
        if (row.kind == Kind::Spread) return static_cast<int>(Qt::AlignCenter);
        return static_cast<int>((column == 0 ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter);
    case DepthRole: return row.depth;
    case KindRole: return static_cast<int>(row.kind);
    case OwnRole: return row.own;
    case ClickPriceRole: return row.click_price;
    }
    return {};
}

QVariant B3FlowMeshLadderModel::headerData(const int section, const Qt::Orientation orientation, const int role) const
{
    if (orientation != Qt::Horizontal || section < 0 || section >= m_headers.size()) return {};
    if (role == Qt::DisplayRole || role == Qt::ToolTipRole) return m_headers[section];
    if (role == Qt::TextAlignmentRole) return static_cast<int>((section == 0 ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter);
    return {};
}

void B3FlowMeshLadderModel::setRows(std::vector<Row> rows)
{
    if (rows.size() != m_rows.size()) {
        beginResetModel();
        m_rows = std::move(rows);
        endResetModel();
        return;
    }
    for (size_t i{0}; i < rows.size(); ++i) {
        if (rows[i] == m_rows[i]) continue;
        m_rows[i] = std::move(rows[i]);
        Q_EMIT dataChanged(index(static_cast<int>(i), 0), index(static_cast<int>(i), 2));
    }
}

void B3FlowMeshLadderModel::setHeaders(const QStringList& headers)
{
    if (headers == m_headers) return;
    m_headers = headers;
    Q_EMIT headerDataChanged(Qt::Horizontal, 0, 2);
}

int B3FlowMeshLadderModel::spreadRow() const
{
    for (size_t i{0}; i < m_rows.size(); ++i) {
        if (m_rows[i].kind == Kind::Spread) return static_cast<int>(i);
    }
    return -1;
}

//! Paints depth bars (left-anchored, one shared scale), the hover band from
//! the spread to the hovered row, flashes and the own-order marker, then the
//! text. Accessible text stays in the model.
class B3FlowMeshLadderDelegate final : public QStyledItemDelegate
{
    const B3FlowMeshOrderBook* m_book;
public:
    B3FlowMeshLadderDelegate(const B3FlowMeshOrderBook* book, QObject* parent) : QStyledItemDelegate{parent}, m_book{book} {}
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        const auto kind{static_cast<Kind>(index.data(B3FlowMeshLadderModel::KindRole).toInt())};
        const QRect row_rect{0, option.rect.y(), m_book->view()->viewport()->width(), option.rect.height()};
        painter->save();
        painter->setClipRect(option.rect);
        if (kind == Kind::Spread) {
            painter->fillRect(option.rect, B3Theme::kSurface);
        } else if (kind == Kind::Ask || kind == Kind::Bid) {
            const QColor side{kind == Kind::Ask ? B3Theme::kNegative : B3Theme::kPositive};
            // Hover: from the spread to the hovered row, on the hovered side.
            const int hover{m_book->hoverRow()}, spread{m_book->model()->spreadRow()}, row{index.row()};
            if (hover >= 0 && spread >= 0 && (kind == Kind::Ask ? hover <= row && row < spread : spread < row && row <= hover)) {
                painter->fillRect(option.rect, B3Theme::kCardHover);
            }
            // Floating point only for pixel widths, never economic values.
            const double fraction{std::clamp(index.data(B3FlowMeshLadderModel::DepthRole).toDouble(), 0.0, 1.0)};
            QColor bar{side}; bar.setAlpha(38);
            painter->fillRect(QRect{row_rect.x(), row_rect.y(), static_cast<int>(row_rect.width() * fraction), row_rect.height()}, bar);
            if (const double flash{m_book->flashIntensity(row)}; flash > 0) {
                QColor glow{side}; glow.setAlphaF(0.7 * flash * 0.45);
                painter->fillRect(option.rect, glow);
            }
            if (index.column() == 0 && index.data(B3FlowMeshLadderModel::OwnRole).toBool()) {
                painter->setRenderHint(QPainter::Antialiasing);
                painter->setPen(Qt::NoPen); painter->setBrush(B3Theme::kAccent);
                painter->drawEllipse(QPointF{option.rect.x() + 4.0, option.rect.center().y() + 0.5}, 2.5, 2.5);
            }
        }
        painter->restore();
        QStyleOptionViewItem text{option};
        text.state &= ~(QStyle::State_MouseOver | QStyle::State_HasFocus | QStyle::State_Selected);
        if (index.column() == 0 && kind != Kind::Spread) text.rect.adjust(9, 0, 0, 0);
        text.rect.adjust(0, 0, -4, 0);
        QStyledItemDelegate::paint(painter, text, index);
    }
};

//! The ladder's view: no scrolling (rows fill the height), hover tracking,
//! and a click that offers the level's exact price.
class B3FlowMeshLadderView final : public QTableView
{
    B3FlowMeshOrderBook* m_book;
public:
    B3FlowMeshLadderView(B3FlowMeshOrderBook* book) : QTableView{book}, m_book{book} {}
protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QTableView::resizeEvent(event);
        m_book->viewResized();
    }
    void mouseMoveEvent(QMouseEvent* event) override
    {
        QTableView::mouseMoveEvent(event);
        m_book->setHoverRow(rowAt(event->position().toPoint().y()));
    }
    void leaveEvent(QEvent* event) override
    {
        QTableView::leaveEvent(event);
        m_book->setHoverRow(-1);
    }
    void mouseReleaseEvent(QMouseEvent* event) override
    {
        QTableView::mouseReleaseEvent(event);
        if (event->button() == Qt::LeftButton) m_book->activateRow(rowAt(event->position().toPoint().y()));
    }
};

B3FlowMeshOrderBook::B3FlowMeshOrderBook(QWidget* parent) : QWidget{parent}
{
    setObjectName(QStringLiteral("flowMeshOrderBook"));
    m_clock.start();
    auto* layout{new QVBoxLayout{this}}; layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(4);
    auto* header{new QHBoxLayout}; header->setContentsMargins(0, 0, 0, 0);
    m_last = Text(this, "flowMeshBookLast"); B3Theme::markTextRole(m_last, QStringLiteral("secondary")); header->addWidget(m_last, 1);
    m_grouping = new QComboBox{this}; m_grouping->setObjectName(QStringLiteral("flowMeshBookGrouping"));
    m_grouping->setAccessibleName(tr("Order book price grouping"));
    m_grouping->setToolTip(tr("Groups the book's prices into steps of this size. A buy level shows its lowest price and a sell level its highest, so every order in it accepts the price shown."));
    m_grouping->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    connect(m_grouping, &QComboBox::activated, this, [this](int index) {
        if (index >= 0 && static_cast<size_t>(index) < m_grouping_choices.size()) Q_EMIT groupingSelected(m_grouping_choices[static_cast<size_t>(index)]);
    });
    header->addWidget(m_grouping);
    layout->addLayout(header);
    // Exact until choices for a price arrive.
    setGroupingChoices({flowmesh::LevelGrouping{}}, flowmesh::LevelGrouping{});
    m_stale = Text(this, "flowMeshBookStale"); m_stale->setText(tr("Stale snapshot · last known orders"));
    B3Theme::markTextRole(m_stale, QStringLiteral("secondary")); m_stale->hide(); layout->addWidget(m_stale);
    m_model = new B3FlowMeshLadderModel{this};
    m_view = new B3FlowMeshLadderView{this};
    m_view->setObjectName(QStringLiteral("flowMeshBookLadder"));
    m_view->setAccessibleName(tr("Order book: sell levels above the spread, buy levels below, best prices beside it"));
    m_view->setModel(m_model);
    m_view->setItemDelegate(new B3FlowMeshLadderDelegate{this, m_view});
    m_view->verticalHeader()->hide();
    m_view->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_view->verticalHeader()->setDefaultSectionSize(ROW_HEIGHT);
    m_view->verticalHeader()->setMinimumSectionSize(ROW_HEIGHT);
    m_view->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_view->horizontalHeader()->setHighlightSections(false);
    m_view->setShowGrid(false); m_view->setAlternatingRowColors(false);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setSelectionMode(QAbstractItemView::NoSelection);
    m_view->setFocusPolicy(Qt::NoFocus);
    m_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_view->setMouseTracking(true);
    m_view->setMinimumWidth(260); m_view->setMinimumHeight(5 * ROW_HEIGHT);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_view, &QWidget::customContextMenuRequested, this, [this](const QPoint& at) {
        QMenu menu{this};
        auto* animate{menu.addAction(tr("Animate order book"))};
        animate->setCheckable(true);
        animate->setChecked(QSettings{}.value(QLatin1String(ANIMATE_SETTING), true).toBool());
        animate->setToolTip(tr("Briefly highlights a price that newly appears. Also off when the system turns interface animations off."));
        connect(animate, &QAction::toggled, this, [](bool on) { SetAnimationSetting(on); });
        auto* size{menu.addAction(tr("Set size on click"))};
        size->setCheckable(true);
        size->setChecked(SizeOnClick());
        size->setToolTip(tr("A click on a level also sets the order quantity to the total up to that level, within your available balance. Off by default; the order is still reviewed before anything is signed."));
        connect(size, &QAction::toggled, this, [](bool on) { SetSizeOnClick(on); });
        menu.exec(m_view->viewport()->mapToGlobal(at));
    });
    layout->addWidget(m_view, 1);
    m_note = Text(this, "flowMeshBookNote"); B3Theme::markTextRole(m_note, QStringLiteral("secondary")); layout->addWidget(m_note);
    m_flash_timer = new QTimer{this}; m_flash_timer->setInterval(FLASH_STEP_MS);
    connect(m_flash_timer, &QTimer::timeout, this, [this] {
        m_view->viewport()->update();
        const qint64 now{m_clock.elapsed()};
        if (std::none_of(m_flash_start.begin(), m_flash_start.end(), [&](qint64 start) { return start >= 0 && now < start + FLASH_MS; })) {
            std::fill(m_flash_start.begin(), m_flash_start.end(), -1);
            m_flash_timer->stop();
        }
    });
    setSnapshot(std::nullopt, false);
}

bool B3FlowMeshOrderBook::AnimationEnabled()
{
    return QSettings{}.value(QLatin1String(ANIMATE_SETTING), true).toBool() && QApplication::isEffectEnabled(Qt::UI_General);
}

void B3FlowMeshOrderBook::SetAnimationSetting(const bool enabled)
{
    QSettings{}.setValue(QLatin1String(ANIMATE_SETTING), enabled);
}

bool B3FlowMeshOrderBook::SizeOnClick()
{
    return QSettings{}.value(QLatin1String(SIZE_ON_CLICK_SETTING), false).toBool();
}

void B3FlowMeshOrderBook::SetSizeOnClick(const bool enabled)
{
    QSettings{}.setValue(QLatin1String(SIZE_ON_CLICK_SETTING), enabled);
}

bool B3FlowMeshOrderBook::flashing() const { return m_flash_timer->isActive(); }

double B3FlowMeshOrderBook::flashIntensity(const int row) const
{
    if (row < 0 || static_cast<size_t>(row) >= m_flash_start.size() || m_flash_start[row] < 0) return 0;
    const qint64 elapsed{m_clock.elapsed() - m_flash_start[row]};
    if (elapsed < 0 || elapsed >= FLASH_MS) return 0;
    // Up to the peak at mid-flash, then down again.
    const double t{static_cast<double>(elapsed) / FLASH_MS};
    return t < 0.5 ? 2 * t : 2 * (1 - t);
}

void B3FlowMeshOrderBook::setStale(const bool stale)
{
    if (m_stale->isVisible() != stale) m_stale->setVisible(stale);
}

void B3FlowMeshOrderBook::setHoverRow(const int row)
{
    if (row == m_hover_row) return;
    m_hover_row = row;
    m_view->viewport()->update();
}

void B3FlowMeshOrderBook::setGroupingChoices(const std::vector<flowmesh::LevelGrouping>& choices, const flowmesh::LevelGrouping& selected)
{
    int current{-1};
    for (size_t i{0}; i < choices.size(); ++i) if (choices[i] == selected) current = static_cast<int>(i);
    auto shown{choices};
    // A choice made at another price stays selectable while it is valid.
    if (current < 0) { shown.push_back(selected); current = static_cast<int>(shown.size()) - 1; }
    if (shown == m_grouping_choices && m_grouping->currentIndex() == current) return;
    const QSignalBlocker block{m_grouping};
    m_grouping_choices = shown;
    while (m_grouping->count() > static_cast<int>(shown.size())) m_grouping->removeItem(m_grouping->count() - 1);
    for (size_t i{0}; i < shown.size(); ++i) {
        const auto label{GroupingLabel(shown[i])};
        if (static_cast<int>(i) < m_grouping->count()) {
            if (m_grouping->itemText(static_cast<int>(i)) != label) m_grouping->setItemText(static_cast<int>(i), label);
        } else {
            m_grouping->addItem(label);
        }
    }
    m_grouping->setCurrentIndex(current);
}

bool B3FlowMeshOrderBook::activateRow(const int row)
{
    if (row < 0 || row >= m_model->rowCount()) return false;
    const auto& item{m_model->row(row)};
    if (item.click_price.isEmpty() || (item.kind != Kind::Ask && item.kind != Kind::Bid)) return false;
    Q_EMIT levelClicked(item.click_price, item.click_cumulative, m_head);
    return true;
}

void B3FlowMeshOrderBook::viewResized()
{
    const int rows{std::max(1, (m_view->viewport()->height() / ROW_HEIGHT - 1) / 2)};
    if (rows == m_rows_per_side) return;
    m_rows_per_side = rows;
    layoutRows();
}

void B3FlowMeshOrderBook::layoutRows()
{
    const int per_side{std::max(1, m_rows_per_side)};
    std::vector<B3FlowMeshLadderModel::Row> rows(static_cast<size_t>(2 * per_side + 1));
    const size_t asks{std::min<size_t>(m_asks.size(), per_side)}, bids{std::min<size_t>(m_bids.size(), per_side)};
    // One scale for both sides: the deeper visible cumulative, in the
    // displayed unit when every visible level has it, else in tokens.
    bool display_units{true};
    for (size_t i{0}; i < asks; ++i) display_units &= m_asks[i].cumulative_display.has_value();
    for (size_t i{0}; i < bids; ++i) display_units &= m_bids[i].cumulative_display.has_value();
    const auto depth_of = [&](const Level& level) -> Wide {
        return display_units ? *level.cumulative_display : Wide{static_cast<uint64_t>(level.cumulative_tokens)};
    };
    Wide deepest{0};
    if (asks) deepest = std::max(deepest, depth_of(m_asks[asks - 1]));
    if (bids) deepest = std::max(deepest, depth_of(m_bids[bids - 1]));
    const auto fill = [&](B3FlowMeshLadderModel::Row& row, const Level& level, const Kind kind) {
        row.kind = kind; row.price = level.price; row.size = level.size; row.total = level.total;
        row.tooltip = level.tooltip; row.accessible = level.accessible; row.own = level.own;
        row.click_price = level.click_price; row.click_cumulative = level.cumulative_tokens;
        // Floating point only for the bar's pixel width.
        row.depth = deepest == 0 ? 0.0 : static_cast<double>(depth_of(level)) / static_cast<double>(deepest);
    };
    for (size_t i{0}; i < asks; ++i) fill(rows[static_cast<size_t>(per_side) - 1 - i], m_asks[i], Kind::Ask);
    auto& spread{rows[static_cast<size_t>(per_side)]};
    spread.kind = Kind::Spread; spread.price = m_spread; spread.tooltip = m_spread_tip; spread.accessible = m_spread;
    for (size_t i{0}; i < bids; ++i) fill(rows[static_cast<size_t>(per_side) + 1 + i], m_bids[i], Kind::Bid);

    // A new price flashes: one not shown before for the same market,
    // orientation, grouping and source. A size change alone never does.
    const auto previous{m_model->rows()};
    const bool same_layout{previous.size() == rows.size()};
    const bool comparable{m_shown_key == m_flash_key && !previous.empty()};
    std::set<QString> shown;
    for (const auto& row : previous) if (row.kind == Kind::Ask || row.kind == Kind::Bid) shown.insert(QString::number(static_cast<int>(row.kind)) + row.price);
    m_model->setRows(rows);
    if (m_view->rowSpan(per_side, 0) != 3) {
        m_view->clearSpans();
        m_view->setSpan(per_side, 0, 1, 3);
    }
    if (!same_layout) m_flash_start.assign(rows.size(), -1);
    m_shown_key = m_flash_key;
    if (!comparable || !same_layout || !AnimationEnabled()) return;
    const qint64 now{m_clock.elapsed()};
    // A fixed stagger by row, 0 to FLASH_STAGGER_MS.
    constexpr std::array<int, 10> order{0, 5, 2, 7, 4, 9, 1, 6, 3, 8};
    bool started{false};
    for (size_t i{0}; i < rows.size(); ++i) {
        const auto& row{rows[i]};
        if ((row.kind != Kind::Ask && row.kind != Kind::Bid) || shown.contains(QString::number(static_cast<int>(row.kind)) + row.price)) continue;
        m_flash_start[i] = now + order[i % order.size()] * FLASH_STAGGER_MS / 9;
        started = true;
    }
    if (started && !m_flash_timer->isActive()) m_flash_timer->start();
}

void B3FlowMeshOrderBook::setSnapshot(const std::optional<Snapshot>& snapshot, const bool inverse, const flowmesh::LevelGrouping& grouping)
{
    const bool known{snapshot && KnownUnits(*snapshot)};
    const QString token{known ? snapshot->units.ticker : tr("token")};
    const QString base{inverse ? QStringLiteral("B3") : token}, quote{inverse ? token : QStringLiteral("B3")};
    m_model->setHeaders({tr("Price (%1)").arg(quote), tr("Size (%1)").arg(base), tr("Total (%1)").arg(base)});
    m_asks.clear(); m_bids.clear(); m_head.clear(); m_verified = false;
    const QString last_tip{tr("Historical last trade, not the current book midpoint. Endpoint-reported history is not independently execution-verified when marked reported.")};
    if (!known || !snapshot->certified || (snapshot->remote && (!snapshot->certificate_verified || !snapshot->account_state_verified))) {
        m_last->setText(tr("Last trade  —")); m_last->setToolTip(last_tip);
        m_spread = tr("Spread  —"); m_spread_tip.clear();
        m_note->setText(!snapshot ? tr("Select a market · no snapshot") : !known ? tr("Verified asset precision required") : tr("Verified order state unavailable"));
        m_note->setToolTip(QString{});
        m_flash_key.clear();
        layoutRows();
        return;
    }
    const auto& s{*snapshot};
    const auto& u{s.units};
    const int decimals{u.decimals};
    m_head = s.head;
    {
        QString last{tr("Last trade  —")}, tip{last_tip};
        for (auto it{s.history.rbegin()}; it != s.history.rend(); ++it) if (it->cleared && it->quantity > 0) {
            const auto price{DisplayPrice(it->price, u, inverse)};
            last = (s.remote && !s.execution_result_verified ? tr("Reported last  %1 %2") : tr("Last trade  %1 %2")).arg(price.text, quote);
            if (price.approximate) tip += tr("\nExact price: %1 %2").arg(price.exact, quote);
            break;
        }
        m_last->setText(last); m_last->setToolTip(tip);
    }
    const QString price_units{inverse ? tr("%1 / B3").arg(token) : tr("B3 / %1").arg(token)};
    const auto* levels{s.levels ? &*s.levels : nullptr};
    const bool usable{levels && levels->available && levels->head == s.head && levels->grouping == grouping &&
                      (grouping.exact || grouping.inverse == inverse)};
    std::optional<CAmount> best_bid, best_ask;
    QStringList notes;
    QString note_tip{tr("FlowMesh clears one uniform-price auction per certified entry: there is no price-time priority and no queue position. "
                        "A level is the quantity of standing orders whose own limit lies at its price, after fills; Total is cumulative from the best level. "
                        "Pending orders are not included until certified. A crossed book can stand after a rationed auction until the next certified one.")};
    if (usable) {
        m_verified = true;
        best_bid = levels->best_bid; best_ask = levels->best_ask;
        // Displayed sides: in the B3 view a canonical bid (buying the
        // token) sells B3 and is a displayed ask, best first either way.
        const auto build = [&](const BookLevelsSide& side, const bool bid, std::vector<Level>& out) {
            std::optional<Wide> cumulative_notional{Wide{0}};
            for (const auto& level : side.levels) {
                Level row;
                QString exact;
                CAmount click{0};
                bool clickable{false};
                if (grouping.exact) {
                    const auto shown{DisplayPrice(level.low, u, inverse)};
                    row.price = inverse && level.low == 0 ? QStringLiteral("—") : shown.text;
                    if (shown.approximate) exact = shown.exact;
                    click = level.low; clickable = level.low > 0;
                } else if (!grouping.inverse) {
                    // Canonical buckets of g ticks: a buy labelled with its
                    // lower edge, a sell with its upper edge.
                    const Wide ticks{grouping.mantissa * Pow10(grouping.exponent + 9 - grouping.decimals)};
                    const Wide edge{bid ? Wide{static_cast<uint64_t>(level.low)} : level.bucket * ticks};
                    row.price = FormatPrice(static_cast<CAmount>(edge), decimals);
                    click = bid ? level.low : level.high; clickable = click > 0;
                } else if (level.zero_price) {
                    row.price = bid ? QStringLiteral("∞") : tr("Any");
                    exact = bid ? tr("Buys the token only at price zero: no finite token-per-B3 price") : tr("Sells the token at any price: accepts any token-per-B3 price");
                } else if (!bid && level.bucket == 0) {
                    row.price = QStringLiteral("< ") + ScaledDecimal(grouping.mantissa, grouping.exponent);
                    exact = tr("Every price below one display step");
                } else {
                    row.price = ScaledDecimal(level.bucket * grouping.mantissa, grouping.exponent);
                    // Every lot of a buy bucket accepts its lower canonical
                    // edge (a displayed sell), of a sell bucket its upper one.
                    click = bid ? level.low : level.high; clickable = click > 0;
                }
                if (clickable) row.click_price = inverse ? ExactInversePrice(click, decimals) : FormatPrice(click, decimals);
                row.own = level.own > 0;
                row.cumulative_tokens = level.cumulative;
                if (cumulative_notional && level.notional) *cumulative_notional += *level.notional;
                else cumulative_notional.reset();
                if (inverse) {
                    row.size = level.notional ? FormatWide(*level.notional, 9) : QStringLiteral("—");
                    row.total = cumulative_notional ? FormatWide(*cumulative_notional, 9) : QStringLiteral("—");
                    row.cumulative_display = cumulative_notional;
                } else {
                    row.size = FormatAmount(level.size, decimals);
                    row.total = FormatAmount(level.cumulative, decimals);
                    row.cumulative_display = Wide{static_cast<uint64_t>(level.cumulative)};
                }
                const bool displayed_ask{bid == inverse};
                QStringList tip;
                tip << tr("%1 %2 %3").arg(displayed_ask ? tr("Sell") : tr("Buy"), row.price, price_units);
                if (!exact.isEmpty()) tip << tr("Exact: %1").arg(exact);
                if (!grouping.exact) {
                    const auto low{inverse ? (level.high == MAX_MONEY || level.high == 0 ? QStringLiteral("—") : ExactInversePrice(level.high, decimals)) : FormatPrice(level.low, decimals)};
                    const auto high{inverse ? (level.low == 0 ? QStringLiteral("—") : ExactInversePrice(level.low, decimals)) : FormatPrice(level.high, decimals)};
                    tip << tr("Grouped prices %1 to %2 %3; every order here accepts the label price.").arg(low, high, price_units);
                }
                tip << tr("Size %1 %2 · %3 %4").arg(row.size, inverse ? QStringLiteral("B3") : token, FormatAmount(level.size, decimals), token);
                if (inverse && !level.notional) tip << tr("B3 at each order's own price not computed exactly for this level (a sloped curve): between %1 and %2 B3.")
                    .arg(FormatWide(Wide{static_cast<uint64_t>(level.size)} * static_cast<uint64_t>(level.low), 9),
                         FormatWide(Wide{static_cast<uint64_t>(level.size)} * static_cast<uint64_t>(level.high), 9));
                tip << tr("Total from the best level: %1 %2").arg(row.total, inverse ? QStringLiteral("B3") : token);
                if (cumulative_notional && level.cumulative > 0) {
                    // The average price of everything from the best level.
                    const auto average{DisplayPrice(static_cast<CAmount>(*cumulative_notional / static_cast<uint64_t>(level.cumulative)), u, inverse)};
                    tip << tr("Average price to here ≈%1 %2").arg(average.text.startsWith(QStringLiteral("≈")) ? average.text.mid(1) : average.text, price_units);
                }
                tip << (level.curves == 1 ? tr("1 standing order (one per account and side)") : tr("%1 standing orders (one per account and side)").arg(level.curves));
                if (level.own > 0) tip << tr("Your order: %1 %2 at this level").arg(FormatAmount(level.own, decimals), token);
                if (!row.click_price.isEmpty()) tip << tr("Click to use this price in the ticket (nothing is signed).");
                row.tooltip = tip.join(QLatin1Char('\n'));
                row.accessible = tr("%1 %2, size %3, total %4, %5 order(s)").arg(displayed_ask ? tr("Sell") : tr("Buy"), row.price, row.size, row.total).arg(level.curves) +
                                 (row.own ? tr(", includes yours") : QString{});
                out.push_back(std::move(row));
            }
        };
        // `bid` names the canonical side: the B3 view's asks are canonical bids.
        build(inverse ? levels->bids : levels->asks, /*bid=*/inverse, m_asks);
        build(inverse ? levels->asks : levels->bids, /*bid=*/!inverse, m_bids);
        notes << (levels->total_curves == 1 ? tr("All 1 standing order · uniform-price auction") : tr("All %1 standing orders · uniform-price auction").arg(levels->total_curves));
        if (!grouping.exact) notes << tr("Grouped by %1 %2").arg(ScaledDecimal(grouping.mantissa, grouping.exponent), price_units);
        for (const auto* side : {&levels->bids, &levels->asks}) {
            if (!side->available) notes << tr("Some levels unavailable (%1)").arg(side->reason);
            else if (!side->truncated.isEmpty()) notes << tr("Deeper levels not computed (%1)").arg(side->truncated);
        }
        if (inverse) notes << tr("B3 amounts at each order's own price");
    } else {
        // Fallback: the limit and ladder orders of the first curve page.
        const auto book{ProjectLimitBook(s, inverse)};
        const auto add = [&](const LimitLevel& level, std::vector<Level>& out, CAmount& cumulative, Wide& cumulative_display) {
            Level row;
            const auto shown{DisplayPrice(level.canonical_price, u, inverse)};
            row.price = inverse && level.canonical_price == 0 ? QStringLiteral("—") : shown.text;
            cumulative += level.remaining;
            cumulative_display += inverse ? static_cast<uint64_t>(level.gross_notional) : static_cast<uint64_t>(level.remaining);
            row.cumulative_tokens = cumulative; row.cumulative_display = cumulative_display;
            row.size = inverse ? FormatAmount(level.gross_notional, 9) : FormatAmount(level.remaining, decimals);
            row.total = FormatWide(cumulative_display, inverse ? 9 : decimals);
            if (level.canonical_price > 0) row.click_price = inverse ? ExactInversePrice(level.canonical_price, decimals) : FormatPrice(level.canonical_price, decimals);
            row.tooltip = tr("Price: %1 %2%3\nSize: %4\nTotal from the best level: %5\n%6 limit or ladder order(s) at this level")
                .arg(row.price, price_units, shown.approximate ? tr(" (exact %1)").arg(shown.exact) : QString{}, row.size, row.total).arg(level.curve_count);
            out.push_back(std::move(row));
        };
        CAmount ask_tokens{0}, bid_tokens{0};
        Wide ask_display{0}, bid_display{0};
        for (auto it{book.asks.rbegin()}; it != book.asks.rend(); ++it) add(*it, m_asks, ask_tokens, ask_display);
        for (const auto& level : book.bids) add(level, m_bids, bid_tokens, bid_display);
        if (book.complete) { best_bid = book.canonical_best_bid; best_ask = book.canonical_best_ask; }
        notes << (s.levels && !s.levels->available && !s.levels->reason.isEmpty()
                      ? tr("Book levels unavailable (%1) · limit and ladder orders of the first curve page only").arg(s.levels->reason)
                      : s.levels && s.levels->available ? tr("Updating levels…") : tr("Limit and ladder orders of the first curve page only"));
        if (!s.curves_complete) notes << tr("Partial snapshot · other orders may be missing");
        if (book.general_curves) notes << tr("%1 non-limit curve(s): see Curve depth").arg(book.general_curves);
        if (book.invalid_curves) notes << tr("%1 invalid or overflowing curve(s) omitted").arg(book.invalid_curves);
        if (book.zero_inverse_curves) notes << tr("%1 zero-price order(s) have no inverse price").arg(book.zero_inverse_curves);
        if (inverse) notes << tr("B3 amounts are gross equivalents at limit");
        note_tip += tr("\nShown without the node's book levels: only the limit and ladder orders of the first 128-curve page are grouped by price; general curves are not turned into limit orders.");
    }
    // The spread row, from the exact best prices whatever the grouping.
    m_spread_tip = tr("The difference between the best sell and best buy price, and its share of their midpoint. "
                      "FlowMesh clears at one uniform price per auction; a crossed book (a buy above a sell) can stand after a rationed auction until the next certified one.");
    if (m_asks.empty() && m_bids.empty()) {
        m_spread = tr("No standing orders");
    } else if (!best_bid || !best_ask) {
        m_spread = m_verified || !m_asks.empty() != !m_bids.empty() ? tr("One side only") : tr("Spread unavailable · partial book");
    } else if (*best_bid >= *best_ask) {
        const auto* standing{usable ? &levels->standing : nullptr};
        const QString kind{*best_bid > *best_ask ? tr("Crossed") : tr("Locked")};
        if (standing && standing->available && standing->cleared) {
            const auto price{DisplayPrice(standing->price, u, inverse)};
            m_spread = tr("%1 · next auction ≈%2 for %3 %4 (indicative)").arg(kind, price.text.startsWith(QStringLiteral("≈")) ? price.text.mid(1) : price.text,
                                                                               FormatAmount(standing->volume, decimals), token);
            m_spread_tip += tr("\nIndicative, not certified: the standing orders, cleared as they are without any pending order, would trade %1 %2 at %3 %4.")
                                .arg(FormatAmount(standing->volume, decimals), token, price.exact, price_units);
        } else {
            m_spread = tr("%1 · awaiting the next certified auction").arg(kind);
        }
    } else {
        const auto relative{RelativeSpread(*best_bid, *best_ask)};
        // The exact spread in the displayed orientation, rounded as its prices.
        const auto spread{DisplaySpread(*best_bid, *best_ask, u, inverse)};
        const QString absolute{spread.text};
        if (spread.approximate) m_spread_tip += tr("\nExact spread: %1 %2").arg(spread.exact, price_units);
        m_spread = absolute.isEmpty() ? tr("Spread  —") : tr("Spread  %1 %2 · %3").arg(absolute, inverse ? token : QStringLiteral("B3"), relative);
    }
    if (m_asks.empty() && m_bids.empty() && notes.isEmpty()) notes << tr("No displayable orders");
    m_note->setText(notes.join(QStringLiteral(" · ")));
    m_note->setToolTip(note_tip);
    m_flash_key = s.market + QLatin1Char('|') + (inverse ? QLatin1Char('i') : QLatin1Char('c')) + QLatin1Char('|') + (m_verified ? QLatin1Char('v') : QLatin1Char('p')) +
                  QLatin1Char('|') + QString::number(grouping.exact) + QString::number(grouping.inverse) + QString::number(grouping.mantissa) +
                  QLatin1Char(',') + QString::number(grouping.exponent) + QLatin1Char(',') + QString::number(grouping.decimals);
    layoutRows();
}
