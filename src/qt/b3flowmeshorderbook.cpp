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
Wide AsWide(const CAmount amount) { return Wide{static_cast<uint64_t>(std::max<CAmount>(0, amount))}; }
//! A price text without its "≈": the column's header says it once.
QString Bare(const QString& text) { return text.startsWith(QStringLiteral("≈")) ? text.mid(1) : text; }
//! One level before its row is formatted: an exact level of the node's
//! levels or of the fallback projection, or a grouped bucket.
struct Entry {
    //! Its label, without "≈"; whether it is exact, and else its exact price
    //! (or what a label without a price means).
    QString price, exact;
    bool whole{true};
    //! An exact level's label at the view's places: levels whose labels are
    //! equal share one row.
    bool mergeable{false};
    //! The exact canonical price a click offers (0: not clickable).
    CAmount click{0};
    CAmount size{0}, cumulative{0}, own{0};
    //! Exact B3 atoms of the level and cumulative from the best level (B3
    //! view sizes), when known; a sloped level's bounds otherwise.
    std::optional<Wide> notional, cumulative_notional;
    std::optional<std::pair<Wide, Wide>> notional_bounds;
    uint32_t curves{0};
    //! A grouped bucket's canonical range, for its tooltip.
    QString range;
};
//! Levels in a row: one, or consecutive exact levels whose labels are equal
//! at the view's places (best first).
using Run = std::vector<Entry>;
std::vector<Run> Runs(std::vector<Entry> entries)
{
    std::vector<Run> runs;
    for (auto& entry : entries) {
        if (!runs.empty() && entry.mergeable && runs.back().back().mergeable && runs.back().back().price == entry.price) {
            runs.back().push_back(std::move(entry));
        } else {
            runs.push_back(Run{std::move(entry)});
        }
    }
    return runs;
}
//! The run's quantity: tokens, and exact B3 atoms when every level has them.
CAmount RunTokens(const Run& run)
{
    CAmount out{0};
    for (const auto& entry : run) out = entry.size > MAX_MONEY - out ? MAX_MONEY : out + entry.size;
    return out;
}
std::optional<Wide> RunNotional(const Run& run)
{
    Wide out{0};
    for (const auto& entry : run) {
        if (!entry.notional) return std::nullopt;
        out += *entry.notional;
    }
    return out;
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
    if (role == Qt::DisplayRole) return m_headers[section];
    if (role == Qt::ToolTipRole) return section < m_header_tips.size() && !m_header_tips[section].isEmpty() ? m_header_tips[section] : m_headers[section];
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

void B3FlowMeshLadderModel::setHeaders(const QStringList& headers, const QStringList& tips)
{
    if (headers == m_headers && tips == m_header_tips) return;
    m_headers = headers;
    m_header_tips = tips;
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
    void paintEvent(QPaintEvent* event) override
    {
        QTableView::paintEvent(event);
        if (m_book->m_view_observer) m_book->m_view_observer(m_book->m_head, true);
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
    // Price takes what Size and Total (a fixed share each) leave: its title
    // can carry the one "≈" of a rounded column.
    m_view->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_view->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
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
    Q_EMIT levelClicked(item.click_price, item.click_canonical, item.click_cumulative, m_head);
    return true;
}

void B3FlowMeshOrderBook::fitColumns()
{
    // Size and Total: over a quarter of the width each, and room for their
    // titles; Price (stretched) takes the rest.
    auto* header{m_view->horizontalHeader()};
    const int width{m_view->viewport()->width()};
    for (const int section : {1, 2}) {
        const QString title{m_model->headerData(section, Qt::Horizontal, Qt::DisplayRole).toString()};
        const int size{std::max(width * 27 / 100, header->fontMetrics().horizontalAdvance(title) + 20)};
        if (header->sectionSize(section) != size) header->resizeSection(section, size);
    }
}

void B3FlowMeshOrderBook::viewResized()
{
    fitColumns();
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
        row.click_price = level.click_price; row.click_canonical = level.click_canonical; row.click_cumulative = level.cumulative_tokens;
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
    // The spread row spans the three columns. Setting it again repaints the
    // whole ladder, so only a new layout does.
    if (m_view->columnSpan(per_side, 0) != 3) {
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
    // Headers: "≈" once on the price column when some label is rounded.
    const auto headers = [&](const bool rounded, const std::optional<int> places) {
        const QString exact_tip{tr("Hover a level for its exact price and the orders in it.")};
        QString price_tip{tr("Price in %1 per %2. %3").arg(quote, base, exact_tip)};
        if (rounded) {
            const QString rule{!places ? tr("Prices rounded for display, to the nearest value.")
                               : *places == 1 ? tr("Prices rounded to 1 decimal place, to the nearest value.")
                                              : tr("Prices rounded to %1 decimal places, to the nearest value.").arg(*places)};
            price_tip = rule + QLatin1Char(' ') +
                        tr("Every price of a level or an order in this view is rounded the same way (the rows, the spread, your orders), never by side. "
                           "Levels whose prices round to one value share a row. %1").arg(exact_tip);
        }
        m_model->setHeaders({rounded ? tr("Price (%1) ≈").arg(quote) : tr("Price (%1)").arg(quote), tr("Size (%1)").arg(base), tr("Total (%1)").arg(base)},
                            {price_tip, tr("Quantity at this level, in %1").arg(base), tr("Cumulative quantity from the best level, in %1").arg(base)});
        fitColumns();
    };
    m_asks.clear(); m_bids.clear(); m_head.clear(); m_verified = false; m_inverse = inverse; m_price_places.reset();
    const QString last_tip{tr("Historical last trade, not the current book midpoint. Endpoint-reported history is not independently execution-verified when marked reported.")};
    if (!known || !snapshot->certified || (snapshot->remote && (!snapshot->certificate_verified || !snapshot->account_state_verified))) {
        m_last->setText(tr("Last trade  —")); m_last->setToolTip(last_tip);
        m_spread = tr("Spread  —"); m_spread_tip.clear();
        m_note->setText(!snapshot ? tr("Select a market · no snapshot") : !known ? tr("Verified asset precision required") : tr("Verified order state unavailable"));
        m_note->setToolTip(QString{});
        m_flash_key.clear();
        headers(false, std::nullopt);
        layoutRows();
        if (m_view_observer) m_view_observer(m_head, false);
        return;
    }
    const auto& s{*snapshot};
    const auto& u{s.units};
    const int decimals{u.decimals};
    m_head = s.head;
    const Trade* last_trade{nullptr};
    for (auto it{s.history.rbegin()}; it != s.history.rend() && !last_trade; ++it) if (it->cleared && it->quantity > 0) last_trade = &*it;
    const QString price_units{inverse ? tr("%1 / B3").arg(token) : tr("B3 / %1").arg(token)};
    const auto* levels{s.levels ? &*s.levels : nullptr};
    const bool usable{levels && levels->available && levels->head == s.head && levels->grouping == grouping &&
                      (grouping.exact || grouping.inverse == inverse)};
    std::optional<CAmount> best_bid, best_ask;
    QStringList notes;
    QString note_tip{tr("FlowMesh clears one uniform-price auction per certified entry: there is no price-time priority and no queue position. "
                        "A level is the quantity of standing orders whose own limit lies at its price, after fills; Total is cumulative from the best level. "
                        "Pending orders are not included until certified. A crossed book can stand after a rationed auction until the next certified one.")};
    // One rounding rule for the view (ViewPricePlaces, from the market's best
    // prices): every price of a level or an order it shows is rounded to the
    // nearest value at the same places, never by side, so a price reads the
    // same in its row, beside the spread and under Your orders. One decimal
    // count per column (Hyperliquid's fixed-precision rows); exact levels
    // whose prices round to one value share a row. Grouped rows are labelled
    // with the step's places.
    std::optional<int> places;
    const auto view_places = [&](const std::optional<CAmount> bid, const std::optional<CAmount> ask) {
        // The market's price: its best prices, else its last trade.
        std::vector<CAmount> references;
        for (const auto& price : {bid, ask}) if (price) references.push_back(*price);
        if (references.empty() && last_trade) references.push_back(last_trade->price);
        return ViewPricePlaces(u, inverse, references);
    };
    const auto label = [&](const CAmount price, Entry& entry) {
        if (inverse && price == 0) {
            entry.price = QStringLiteral("—");
            return;
        }
        const auto shown{ViewPrice(price, u, inverse, places)};
        entry.price = Bare(shown.text);
        entry.whole = !shown.approximate;
        if (!entry.whole) entry.exact = shown.exact;
        entry.mergeable = true;
    };
    // Displayed sides, best first: in the B3 view a canonical bid (buying
    // the token) sells B3 and is a displayed ask.
    std::vector<Entry> ask_entries, bid_entries;
    if (usable) {
        m_verified = true;
        best_bid = levels->best_bid; best_ask = levels->best_ask;
        places = view_places(best_bid, best_ask);
        const int step_places{std::max(0, -static_cast<int>(grouping.exponent))};
        const auto collect = [&](const BookLevelsSide& side, const bool bid) {
            std::vector<Entry> out;
            std::optional<Wide> cumulative_notional{Wide{0}};
            for (const auto& level : side.levels) {
                Entry entry;
                entry.size = level.size; entry.cumulative = level.cumulative; entry.own = level.own; entry.curves = level.curves;
                entry.notional = level.notional;
                if (cumulative_notional && level.notional) *cumulative_notional += *level.notional;
                else cumulative_notional.reset();
                entry.cumulative_notional = cumulative_notional;
                if (inverse && !level.notional) {
                    entry.notional_bounds = std::pair{AsWide(level.size) * static_cast<uint64_t>(level.low), AsWide(level.size) * static_cast<uint64_t>(level.high)};
                }
                if (grouping.exact) {
                    label(level.low, entry);
                    entry.click = level.low;
                } else if (!grouping.inverse) {
                    // Canonical buckets of g ticks: a buy labelled with its
                    // lower edge, a sell with its upper edge.
                    const Wide ticks{grouping.mantissa * Pow10(grouping.exponent + 9 - grouping.decimals)};
                    const Wide edge{bid ? Wide{static_cast<uint64_t>(level.low)} : level.bucket * ticks};
                    entry.price = PadDecimals(FormatPrice(static_cast<CAmount>(edge), decimals), step_places);
                    entry.click = bid ? level.low : level.high;
                } else if (level.zero_price) {
                    entry.price = bid ? QStringLiteral("∞") : tr("Any");
                    entry.exact = bid ? tr("Buys the token only at price zero: no finite token-per-B3 price") : tr("Sells the token at any price: accepts any token-per-B3 price");
                    entry.whole = false;
                } else if (!bid && level.bucket == 0) {
                    entry.price = QStringLiteral("< ") + PadDecimals(ScaledDecimal(grouping.mantissa, grouping.exponent), step_places);
                    entry.exact = tr("Every price below one display step");
                    entry.whole = false;
                } else {
                    entry.price = PadDecimals(ScaledDecimal(level.bucket * grouping.mantissa, grouping.exponent), step_places);
                    // Every lot of a buy bucket accepts its lower canonical
                    // edge (a displayed sell), of a sell bucket its upper one.
                    entry.click = bid ? level.low : level.high;
                }
                entry.click = std::max<CAmount>(0, entry.click);
                if (!grouping.exact) {
                    const auto low{inverse ? (level.high == MAX_MONEY || level.high == 0 ? QStringLiteral("—") : ExactInversePrice(level.high, decimals)) : FormatPrice(level.low, decimals)};
                    const auto high{inverse ? (level.low == 0 ? QStringLiteral("—") : ExactInversePrice(level.low, decimals)) : FormatPrice(level.high, decimals)};
                    entry.range = tr("Grouped prices %1 to %2 %3; every order here accepts the label price.").arg(low, high, price_units);
                }
                out.push_back(std::move(entry));
            }
            return out;
        };
        // `bid` names the canonical side: the B3 view's asks are canonical bids.
        ask_entries = collect(inverse ? levels->bids : levels->asks, /*bid=*/inverse);
        bid_entries = collect(inverse ? levels->asks : levels->bids, /*bid=*/!inverse);
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
        places = view_places(book.canonical_best_bid, book.canonical_best_ask);
        const auto collect = [&](const auto begin, const auto end) {
            std::vector<Entry> out;
            CAmount cumulative{0};
            Wide cumulative_notional{0};
            for (auto it{begin}; it != end; ++it) {
                const LimitLevel& level{*it};
                Entry entry;
                label(level.canonical_price, entry);
                if (level.canonical_price > 0 && entry.price != QStringLiteral("—")) entry.click = level.canonical_price;
                entry.size = level.remaining; entry.curves = static_cast<uint32_t>(std::min<size_t>(level.curve_count, UINT32_MAX));
                cumulative = level.remaining > MAX_MONEY - cumulative ? MAX_MONEY : cumulative + level.remaining;
                entry.cumulative = cumulative;
                // Gross B3 at the limit: exact for a limit order.
                entry.notional = AsWide(level.gross_notional);
                cumulative_notional += AsWide(level.gross_notional);
                entry.cumulative_notional = cumulative_notional;
                out.push_back(std::move(entry));
            }
            return out;
        };
        // Both sides are in descending display price: the asks' best is last.
        ask_entries = collect(book.asks.rbegin(), book.asks.rend());
        bid_entries = collect(book.bids.begin(), book.bids.end());
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
    m_price_places = places;
    {
        // The last trade at the view's places too: the price its row shows.
        QString last{tr("Last trade  —")}, tip{last_tip};
        if (last_trade) {
            const auto price{ViewPrice(last_trade->price, u, inverse, places)};
            last = (s.remote && !s.execution_result_verified ? tr("Reported last  %1 %2") : tr("Last trade  %1 %2")).arg(price.text, quote);
            if (price.approximate) tip += tr("\nExact price: %1 %2").arg(price.exact, quote);
        }
        m_last->setText(last); m_last->setToolTip(tip);
    }
    const auto ask_runs{Runs(std::move(ask_entries))}, bid_runs{Runs(std::move(bid_entries))};
    // Size and Total share one count: the places the rows need.
    int size_places{0};
    bool rounded{false};
    for (const auto* runs : {&ask_runs, &bid_runs}) {
        for (const auto& run : *runs) {
            for (const auto& entry : run) rounded |= entry.mergeable && !entry.whole;
            if (inverse) {
                if (const auto notional{RunNotional(run)}) size_places = std::max(size_places, NeededDecimals(*notional, 9));
                if (run.back().cumulative_notional) size_places = std::max(size_places, NeededDecimals(*run.back().cumulative_notional, 9));
            } else {
                size_places = std::max({size_places, NeededDecimals(AsWide(RunTokens(run)), decimals), NeededDecimals(AsWide(run.back().cumulative), decimals)});
            }
        }
    }
    const auto format = [&](const std::vector<Run>& runs, const bool displayed_ask, std::vector<Level>& out) {
        for (const auto& run : runs) {
            const Entry& first{run.front()};
            const Entry& last{run.back()};
            const bool merged{run.size() > 1};
            const CAmount tokens{RunTokens(run)};
            const auto notional{RunNotional(run)};
            CAmount own{0};
            uint32_t curves{0};
            for (const auto& entry : run) {
                own = entry.own > MAX_MONEY - own ? MAX_MONEY : own + std::max<CAmount>(0, entry.own);
                curves = entry.curves > UINT32_MAX - curves ? UINT32_MAX : curves + entry.curves;
            }
            Level row;
            row.price = first.price;
            row.own = own > 0;
            row.cumulative_tokens = last.cumulative;
            if (inverse) {
                row.size = notional ? PadDecimals(FormatWide(*notional, 9), size_places) : QStringLiteral("—");
                row.total = last.cumulative_notional ? PadDecimals(FormatWide(*last.cumulative_notional, 9), size_places) : QStringLiteral("—");
                row.cumulative_display = last.cumulative_notional;
            } else {
                row.size = PadDecimals(FormatAmount(tokens, decimals), size_places);
                row.total = PadDecimals(FormatAmount(last.cumulative, decimals), size_places);
                row.cumulative_display = AsWide(last.cumulative);
            }
            // A click offers the price as shown; the ticket reads it back as
            // exactly `click` (see fillFromBook). A shared row's is its
            // deepest level's: the price that takes every level in it.
            if (last.click > 0) { row.click_price = row.price; row.click_canonical = last.click; }
            const auto exact_of = [](const Entry& entry) { return entry.exact.isEmpty() ? entry.price : entry.exact; };
            QStringList tip;
            tip << tr("%1 %2 %3").arg(displayed_ask ? tr("Sell") : tr("Buy"), row.price, price_units);
            if (!merged) {
                if (!first.exact.isEmpty()) tip << tr("Exact: %1").arg(first.exact);
            } else {
                tip << tr("%1 prices round to this value and share this row:").arg(run.size());
                for (const auto& entry : run) {
                    tip << tr("  Exact %1: %2 %3 · %4 order(s)").arg(exact_of(entry), FormatAmount(entry.size, decimals), token).arg(entry.curves);
                }
            }
            if (!first.range.isEmpty()) tip << first.range;
            tip << tr("Size %1 %2 · %3 %4").arg(row.size, inverse ? QStringLiteral("B3") : token, FormatAmount(tokens, decimals), token);
            if (inverse && !notional) {
                Wide low{0}, high{0};
                bool bounded{true};
                for (const auto& entry : run) {
                    if (entry.notional) { low += *entry.notional; high += *entry.notional; }
                    else if (entry.notional_bounds) { low += entry.notional_bounds->first; high += entry.notional_bounds->second; }
                    else bounded = false;
                }
                if (bounded) tip << tr("B3 at each order's own price not computed exactly for this level (a sloped curve): between %1 and %2 B3.").arg(FormatWide(low, 9), FormatWide(high, 9));
            }
            tip << tr("Total from the best level: %1 %2").arg(row.total, inverse ? QStringLiteral("B3") : token);
            if (last.cumulative_notional && last.cumulative > 0) {
                // The average price of everything from the best level.
                const auto average{DisplayPrice(static_cast<CAmount>(*last.cumulative_notional / static_cast<uint64_t>(last.cumulative)), u, inverse)};
                tip << tr("Average price to here ≈%1 %2").arg(Bare(average.text), price_units);
            }
            // One ladder may contribute at several exact prices in a rounded
            // display row. Adding level counts is not a distinct-order count.
            if (merged) tip << tr("%1 price-level contributions; one ladder order may contribute more than once.").arg(curves);
            else if (m_verified) tip << (curves == 1 ? tr("1 standing order (one per account and side)") : tr("%1 standing orders (one per account and side)").arg(curves));
            else tip << tr("%1 limit or ladder order(s) at this level").arg(curves);
            if (own > 0) tip << tr("Your order: %1 %2 at this level").arg(FormatAmount(own, decimals), token);
            if (!row.click_price.isEmpty()) {
                if (merged) {
                    tip << tr("Click to use this row's price in the ticket (nothing is signed): the price that takes every level in it, exactly %1 %2. The ticket and the review show its exact executable price.")
                               .arg(exact_of(last), price_units);
                } else {
                    tip << (inverse ? tr("Click to use this price in the ticket (nothing is signed); the ticket and the review show its exact executable price.")
                                    : tr("Click to use this price in the ticket (nothing is signed)."));
                }
            }
            row.tooltip = tip.join(QLatin1Char('\n'));
            const bool exact{!merged && first.exact.isEmpty()};
            row.accessible = (merged ? tr("%1 %2, size %3, total %4, %5 price-level contributions")
                                     : tr("%1 %2, size %3, total %4, %5 order(s)")).arg(displayed_ask ? tr("Sell") : tr("Buy"),
                                                                           exact ? row.price : tr("about %1").arg(row.price), row.size, row.total).arg(curves) +
                             (row.own ? tr(", includes yours") : QString{});
            out.push_back(std::move(row));
        }
    };
    format(ask_runs, /*displayed_ask=*/true, m_asks);
    format(bid_runs, /*displayed_ask=*/false, m_bids);
    headers(rounded, places);
    // Grouped display rows may show bucket edges instead of the exact best.
    m_spread_tip = tr("The difference between the exact best sell and best buy price, and its share of their midpoint. Grouped or rounded row labels may differ from those exact prices. "
                      "FlowMesh clears at one uniform price per auction; a crossed book (a buy above a sell) can stand after a rationed auction until the next certified one.");
    if (m_asks.empty() && m_bids.empty()) {
        m_spread = tr("No standing orders");
    } else if (!best_bid || !best_ask) {
        m_spread = m_verified || !m_asks.empty() != !m_bids.empty() ? tr("One side only") : tr("Spread unavailable · partial book");
    } else if (*best_bid >= *best_ask) {
        const auto* standing{usable ? &levels->standing : nullptr};
        const QString kind{*best_bid > *best_ask ? tr("Crossed") : tr("Locked")};
        if (standing && standing->available && standing->cleared) {
            const auto price{ViewPrice(standing->price, u, inverse, places)};
            m_spread = tr("%1 · next auction ≈%2 for %3 %4 (indicative)").arg(kind, Bare(price.text), FormatAmount(standing->volume, decimals), token);
            m_spread_tip += tr("\nIndicative, not certified: the standing orders, cleared as they are without any pending order, would trade %1 %2 at %3 %4.")
                                .arg(FormatAmount(standing->volume, decimals), token, price.exact, price_units);
        } else {
            m_spread = tr("%1 · awaiting the next certified auction").arg(kind);
        }
    } else {
        const auto relative{RelativeSpread(*best_bid, *best_ask)};
        const auto spread{ViewSpread(*best_bid, *best_ask, u, inverse, places)};
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
    if (m_view_observer) m_view_observer(m_head, false);
}
