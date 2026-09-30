// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_QT_B3FLOWMESHCHART_H
#define BITCOIN_QT_B3FLOWMESHCHART_H
#include <qt/b3flowmeshmarketdata.h>
#include <util/int128.h>
#include <QWidget>
#include <optional>
#include <utility>
#include <vector>

//! Display-only projection of certified integers; never used to price an order.
class B3FlowMeshChart : public QWidget
{
    Q_OBJECT
public:
    enum class Mode { Prices, Liquidity };
    //! Prices remain canonical integers for exact formatting. In inverse view,
    //! high/low select the opposite canonical extrema, never rounded reciprocals.
    struct Candle {
        uint64_t bucket_sequence{0}, first_sequence{0}, last_sequence{0};
        CAmount open{0}, high{0}, low{0}, close{0};
        util::Unsigned128 base_volume{0}, quote_volume{0};
        size_t trades{0};
        bool operator==(const Candle&) const = default;
    };
    /** Candles in operator-observed time, as the source served them (and
     * extended from held ticks by the same bucketing): never interpolated,
     * never certified time. Without them the chart keeps auction order. */
    struct TimeSeries {
        //! The displayed data carries observed times from `source`.
        bool available{false};
        //! Why there are no times (e.g. an older server), or no candles yet.
        QString reason;
        //! "this_node" or the endpoint URL that reported them.
        QString source;
        //! Endpoint-reported: prices, volumes and times are its claims.
        bool remote{false};
        //! Candles have not been read yet.
        bool loading{false};
        flowmesh::CandleInterval interval{flowmesh::CandleInterval::M1};
        //! Oldest first; a window of the source's retained candles.
        std::vector<flowmesh::ObservedCandle> candles;
        bool operator==(const TimeSeries&) const = default;
    };
    explicit B3FlowMeshChart(QWidget* parent = nullptr);
    void setTimeSeries(TimeSeries series);
    const TimeSeries& timeSeries() const { return m_time; }
    //! Display only: the wall clock, so the time axis may reach now.
    void setNowMs(int64_t now_ms);
    //! Drawing candles in observed time (else auction order).
    bool timeMode() const { return m_time.available; }
    //! Visible buckets: the oldest and newest open, in time mode.
    std::pair<int64_t, int64_t> timeWindow() const;
    double timeSlots() const { return m_time_slots; }
    bool followsTail() const { return !m_time_right; }
    //! Wheel zoom and drag pan funnel into these; resetTimeView follows the
    //! newest candle again (double-click).
    void zoomTime(double factor, double anchor_fraction = 1.0);
    void panTime(double slots);
    void resetTimeView();
    //! A UTC axis label for a bucket start at this interval.
    static QString TimeAxisLabel(int64_t open_ms, flowmesh::CandleInterval interval);
    //! What the prices and times are and who reported them.
    static QString ProvenanceText(bool remote, const QString& source);
    //! "Every N auctions" for the auction-order fallback.
    static QString AuctionOrderLabel(uint64_t auctions);
    //! What lies between two consecutive candles: a proven quiet stretch
    //! (consecutive sequences and no seam), a gap of auctions without an
    //! observed time (their count), or a clock seam without missing auctions.
    enum class Between { QUIET, GAP, SEAM };
    static std::pair<Between, uint64_t> SpanBetween(const flowmesh::ObservedCandle& earlier, const flowmesh::ObservedCandle& later);
    void setSnapshot(const std::optional<B3FlowMeshMarketData::Snapshot>& snapshot);
    void setMode(Mode mode);
    void setInverted(bool inverted);
    void setLoading(bool loading);
    void setStale(bool stale);
    void setCandleInterval(uint64_t microblocks);
    uint64_t candleInterval() const { return m_candle_interval; }
    const std::vector<Candle>& candles() const { return m_candles; }
    //! Fixed sequence buckets, no invented timestamps or empty-period fills.
    //! Invalid or duplicate records fail closed. An inverse candle containing
    //! any zero-price trade is omitted because its extrema are not finite.
    static std::vector<Candle> AggregateCandles(const std::vector<B3FlowMeshMarketData::Trade>& history, uint64_t microblocks, bool inverse = false);
    //! Volume is canonical asset turnover in either price orientation (e.g.
    //! rUSD in B3/rUSD view), never a conversion at the candle closing price.
    static QString FormatCandleVolume(const Candle& candle, int base_decimals, bool inverse);
    int pricePointCount() const;
    QString emptyMessage() const;
    //! The exact form of every price the last paint showed rounded (empty
    //! when none was). The hover tooltip adds it to toolTip().
    QString exactPrices() const { return m_exact_prices; }
    QSize minimumSizeHint() const override { return {280, 260}; }
protected:
    bool event(QEvent*) override;
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void leaveEvent(QEvent*) override;
private:
    void rebuildCandles();
    //! The newest bucket shown: the chosen one, or (following the tail) the
    //! newest candle, or now when now is still within the window.
    int64_t timeRight() const;
    void paintTime(QPainter& p, const QRectF& plot, int axis_width, QStringList& exact);
    TimeSeries m_time;
    int64_t m_now_ms{0};
    double m_time_slots{60};
    std::optional<int64_t> m_time_right;
    bool m_dragging{false};
    QPointF m_drag_origin;
    int64_t m_drag_right{0};
    std::optional<B3FlowMeshMarketData::Snapshot> m_snapshot;
    std::vector<Candle> m_candles;
    uint64_t m_candle_interval{5};
    Mode m_mode{Mode::Prices};
    bool m_loading{false}, m_stale{false};
    bool m_inverted{false};
    QPointF m_pointer{-1, -1};
    QString m_exact_prices;
};
#endif
