// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_QT_B3FLOWMESHCHART_H
#define BITCOIN_QT_B3FLOWMESHCHART_H
#include <qt/b3flowmeshmarketdata.h>
#include <util/int128.h>
#include <QWidget>
#include <optional>

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
    explicit B3FlowMeshChart(QWidget* parent = nullptr);
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
    static QString FormatCandleVolume(const Candle& candle, int base_decimals, bool inverse);
    int pricePointCount() const;
    QString emptyMessage() const;
    QSize minimumSizeHint() const override { return {280, 260}; }
protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;
private:
    void rebuildCandles();
    std::optional<B3FlowMeshMarketData::Snapshot> m_snapshot;
    std::vector<Candle> m_candles;
    uint64_t m_candle_interval{5};
    Mode m_mode{Mode::Prices};
    bool m_loading{false}, m_stale{false};
    bool m_inverted{false};
    QPointF m_pointer{-1, -1};
};
#endif
