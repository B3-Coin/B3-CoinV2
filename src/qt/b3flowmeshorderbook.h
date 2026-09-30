// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_QT_B3FLOWMESHORDERBOOK_H
#define BITCOIN_QT_B3FLOWMESHORDERBOOK_H
#include <qt/b3flowmeshmarketdata.h>
#include <QAbstractTableModel>
#include <QElapsedTimer>
#include <QTableView>
#include <QWidget>
#include <functional>
#include <optional>
#include <vector>

class QComboBox;
class QLabel;
class QTimer;

/** The order book ladder's rows: displayed asks (worst at the top, the best
 * beside the spread), one spread row, then displayed bids (best first). The
 * row count depends only on the view's height; data updates change cells in
 * place and emit dataChanged only for rows whose content changed. */
class B3FlowMeshLadderModel final : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Role {
        //! 0..1: the row's cumulative depth on the book's one shared scale.
        DepthRole = Qt::UserRole + 1,
        //! Row::Kind as an int.
        KindRole,
        //! The viewer's own account has quantity at this level.
        OwnRole,
        //! A click offers this price, as shown (empty: not clickable).
        ClickPriceRole,
    };
    struct Row {
        enum class Kind { Empty, Ask, Spread, Bid };
        Kind kind{Kind::Empty};
        //! Price, Size, Total; the spread row's text is in `price`.
        QString price, size, total, tooltip, accessible;
        double depth{0};
        bool own{false};
        //! A clickable level's price as the row shows it, the exact
        //! canonical price that text stands for (B3 atoms per base atom),
        //! and its cumulative token quantity (base atoms) from the best level.
        QString click_price;
        CAmount click_canonical{0}, click_cumulative{0};
        bool operator==(const Row&) const = default;
    };
    explicit B3FlowMeshLadderModel(QObject* parent = nullptr) : QAbstractTableModel{parent} {}
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : static_cast<int>(m_rows.size()); }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : 3; }
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    //! A different row count resets the model (only when the view's height
    //! changes); otherwise only changed rows emit dataChanged.
    void setRows(std::vector<Row> rows);
    //! Column titles and their tooltips (a title's own text when empty).
    void setHeaders(const QStringList& headers, const QStringList& tips = {});
    const Row& row(int index) const { return m_rows[static_cast<size_t>(index)]; }
    const std::vector<Row>& rows() const { return m_rows; }
    //! The spread row's index (-1 without rows).
    int spreadRow() const;

private:
    std::vector<Row> m_rows;
    QStringList m_headers{tr("Price"), tr("Size"), tr("Total")}, m_header_tips;
};

/** A Hyperliquid-style order book of the certified FlowMesh market.
 *
 * Levels are the node's exact aggregation of every verified standing curve
 * ("book_levels"): displayed asks above one spread row and displayed bids
 * below it, Price | Size | Total with Total cumulative from the best level,
 * cumulative depth bars on one scale shared by both sides, only non-empty
 * levels, the viewer's own quantity marked, and as many rows as the view's
 * height holds. Prices follow the view's one rounding rule
 * (ViewPricePlaces: to the nearest value at one count of places, never by
 * side), which Your orders and the spread row share; exact levels whose
 * prices round to one value share a row, each exact price in its tooltip.
 * Grouped levels are labelled with an edge every lot in them accepts. A
 * crossed book (legal after a pro-rata auction) says so and shows the
 * standing clearing instead of a negative spread. Without verified levels
 * the book falls back to the exact limit-shaped curves of the first curve
 * page, labelled partial. A new price flashes for FLASH_MS unless animation
 * is off. Clicking a level offers its price, as shown, to the ticket, which
 * reads it back as exactly the level's canonical price; nothing is ever
 * signed from the book. There is no price-time priority. */
class B3FlowMeshOrderBook : public QWidget
{
    Q_OBJECT
public:
    //! Flash length, its largest stagger and the timer step while one runs:
    //! 30 frames a second, only while a flash runs (the responsiveness
    //! design's frame budget; data renders themselves stay at most ~20/s).
    static constexpr int FLASH_MS{800}, FLASH_STAGGER_MS{450}, FLASH_STEP_MS{33};
    static constexpr int ROW_HEIGHT{22};
    explicit B3FlowMeshOrderBook(QWidget* parent = nullptr);
    //! The node's levels are shown only in `grouping`, their head's and the
    //! displayed orientation's; otherwise (a read still under way, levels
    //! unavailable) the fallback projection is shown, labelled.
    void setSnapshot(const std::optional<B3FlowMeshMarketData::Snapshot>& snapshot, bool inverse,
                     const flowmesh::LevelGrouping& grouping = {});
    void setStale(bool stale);
    //! Price flashes: the "Animate order book" setting (default on) and the
    //! platform's UI effects (QApplication::isEffectEnabled(Qt::UI_General)).
    static bool AnimationEnabled();
    static void SetAnimationSetting(bool enabled);
    //! "Set size on click" (default off): a click also offers the level's
    //! cumulative size. Hyperliquid's default is on; B3's is off because a
    //! cumulative size can be large (it is still reviewed before signing).
    static bool SizeOnClick();
    static void SetSizeOnClick(bool enabled);
    B3FlowMeshLadderModel* model() const { return m_model; }
    QTableView* view() const { return m_view; }
    //! Level rows per side the view's height holds.
    int rowsPerSide() const { return m_rows_per_side; }
    //! A flash is running (its timer is active only then).
    bool flashing() const;
    //! The node's verified levels are shown (not the fallback projection).
    bool showsVerifiedLevels() const { return m_verified; }
    //! The orientation the rows were last shown in (B3 view: true).
    bool shownInverse() const { return m_inverse; }
    //! The view's price places (ViewPricePlaces) the rows were last shown
    //! with: the one rounding rule of every price of a level or an order in
    //! this view (nullopt: none shown, or DisplayPrice's own digits).
    const std::optional<int>& pricePlaces() const { return m_price_places; }
    //! Flash progress of a row, 0 (none) to 1 (peak), at the book's clock.
    double flashIntensity(int row) const;
    //! The row the pointer is over, or -1; the book highlights from the
    //! spread to it.
    int hoverRow() const { return m_hover_row; }
    void setHoverRow(int row);
    //! Offers row `row`'s price to the ticket (as a click does).
    bool activateRow(int row);
    //! The price-grouping selector's choices (BookGroupingChoices) and the
    //! current one; a selection only emits groupingSelected.
    void setGroupingChoices(const std::vector<flowmesh::LevelGrouping>& choices, const flowmesh::LevelGrouping& selected);
    QComboBox* groupingSelector() const { return m_grouping; }
    //! The head of the data the rows were last shown from (empty: none).
    const QString& head() const { return m_head; }
    //! Diagnostics and tests only (never set in production): called with
    //! head() when setSnapshot has installed rows (`painted` false; rows
    //! that did not change repaint nothing) and at the end of every paint of
    //! the ladder (`painted` true).
    void setViewObserver(std::function<void(const QString& head, bool painted)> observer) { m_view_observer = std::move(observer); }

Q_SIGNALS:
    //! The viewer chose another price grouping for the displayed orientation.
    void groupingSelected(const flowmesh::LevelGrouping& grouping);
    //! A level was clicked: its price as shown, the exact canonical price
    //! it stands for, its cumulative token quantity (base atoms) from the
    //! best level, and the head of the data it was shown from. Never a
    //! signing or review request.
    void levelClicked(const QString& price, qint64 canonical, qint64 cumulative, const QString& head);

private:
    friend class B3FlowMeshLadderView;
    struct Level {
        QString price, size, total, tooltip, accessible, click_price;
        CAmount click_canonical{0};
        bool own{false};
        CAmount cumulative_tokens{0};
        //! Cumulative in the displayed base unit for the depth bar (tokens,
        //! or B3 atoms in the B3 view); absent when not exact.
        std::optional<util::Unsigned128> cumulative_display;
    };
    void layoutRows();
    void viewResized();
    void fitColumns();
    QLabel *m_last, *m_note, *m_stale;
    QComboBox* m_grouping;
    std::vector<flowmesh::LevelGrouping> m_grouping_choices;
    QTableView* m_view;
    B3FlowMeshLadderModel* m_model;
    QTimer* m_flash_timer;
    int m_rows_per_side{0}, m_hover_row{-1};
    bool m_verified{false}, m_inverse{false};
    std::optional<int> m_price_places;
    //! Displayed sides, best first, and the spread row.
    std::vector<Level> m_asks, m_bids;
    QString m_spread, m_spread_tip, m_head;
    //! What the prices were last shown for (market, orientation, grouping,
    //! source): a new price flashes only against the same key.
    QString m_flash_key, m_shown_key;
    //! Per row: when its flash started (m_clock ms, staggered), or -1.
    std::vector<qint64> m_flash_start;
    QElapsedTimer m_clock;
    std::function<void(const QString&, bool)> m_view_observer;
};
#endif
