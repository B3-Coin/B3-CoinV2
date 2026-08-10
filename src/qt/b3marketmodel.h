// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_B3MARKETMODEL_H
#define BITCOIN_QT_B3MARKETMODEL_H

#include <QAbstractTableModel>
#include <QString>
#include <QVector>

#include <cstdint>
#include <optional>

/**
 * Model layer for the B3FlowMesh trading workspace. Everything here is
 * a passive container the views render from; nothing fetches data, and
 * the only backend implementation in this build reports itself
 * unavailable, so no market data, orders, fills, balances or fees are
 * ever fabricated in production. Prices and quantities are int64 raw
 * units with explicit decimal counts (see B3Fixed) — floating point is
 * never a financial source of truth.
 */

//! One OHLCV entry. All values are raw integer units.
struct B3Candle {
    qint64 timestamp{0}; // seconds since epoch, bucket open time
    qint64 open{0};
    qint64 high{0};
    qint64 low{0};
    qint64 close{0};
    qint64 volume{0};
};

/**
 * Ordered candle container feeding the chart. Input is sanitized:
 * candles are kept sorted by timestamp, duplicates replace the earlier
 * entry, and malformed candles (high < low, negative volume) are
 * rejected rather than drawn.
 */
class B3CandleSeries : public QObject
{
    Q_OBJECT

public:
    explicit B3CandleSeries(QObject* parent = nullptr);

    void setPrecision(int price_decimals, int quantity_decimals);
    int priceDecimals() const { return m_price_decimals; }
    int quantityDecimals() const { return m_quantity_decimals; }

    //! Replace all data (sorts, dedups, drops malformed entries).
    void setCandles(QVector<B3Candle> candles);
    //! Incremental append. A timestamp equal to the last candle updates
    //! it in place; an older timestamp is inserted in order.
    void append(const B3Candle& candle);
    void clear();

    int count() const { return m_candles.size(); }
    const B3Candle& at(int index) const { return m_candles[index]; }
    const QVector<B3Candle>& candles() const { return m_candles; }

Q_SIGNALS:
    //! Full replacement/clear.
    void resetted();
    //! One candle appended or updated at `index`.
    void updated(int index);

private:
    static bool wellFormed(const B3Candle& candle);

    QVector<B3Candle> m_candles;
    int m_price_decimals{8};
    int m_quantity_decimals{8};
};

/** One side of the order book: price, quantity, cumulative total. */
class B3OrderBookModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column { Price = 0, Quantity = 1, Total = 2, ColumnCount = 3 };
    enum class Side { Bid, Ask };
    enum class State { Unavailable, Loading, Empty, Live };
    //! Fraction (0..1) of the deepest cumulative total, for depth bars.
    static constexpr int DepthRole = Qt::UserRole + 1;

    struct Level {
        qint64 price{0};
        qint64 quantity{0};
    };

    explicit B3OrderBookModel(Side side, QObject* parent = nullptr);

    void setPrecision(int price_decimals, int quantity_decimals);
    void setLevels(const QVector<Level>& levels);
    void setState(State state);
    State state() const { return m_state; }
    Side side() const { return m_side; }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    Side m_side;
    State m_state{State::Unavailable};
    QVector<Level> m_levels;
    QVector<qint64> m_cumulative;
    int m_price_decimals{8};
    int m_quantity_decimals{8};
};

/** Recent trades: time, price, quantity. Empty in this build. */
class B3TradesModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column { Time = 0, Price = 1, Quantity = 2, ColumnCount = 3 };

    struct Trade {
        qint64 timestamp{0};
        qint64 price{0};
        qint64 quantity{0};
        bool buyer_initiated{false};
    };

    explicit B3TradesModel(QObject* parent = nullptr);

    void setPrecision(int price_decimals, int quantity_decimals);
    void setTrades(const QVector<Trade>& trades);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    QVector<Trade> m_trades;
    int m_price_decimals{8};
    int m_quantity_decimals{8};
};

/**
 * Header-only empty table for open orders / positions / fills. A real
 * backend replaces these models wholesale; the views need no changes.
 */
class B3EmptyTableModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    explicit B3EmptyTableModel(QStringList headers, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex&, int) const override { return {}; }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    QStringList m_headers;
};

/**
 * Abstract trading backend boundary. The trade ticket submits nothing
 * unless an implementation reports available() — and the only
 * implementation in this build, B3NullTradingBackend, never does.
 */
class B3TradingBackend : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    virtual bool available() const = 0;
    virtual QString unavailableReason() const = 0;
    //! Estimated fee in raw quote units for a prospective order, if the
    //! backend can produce one. No backend → no estimate → the UI shows
    //! a placeholder, never a made-up number.
    virtual std::optional<qint64> estimatedFee(qint64 price, qint64 quantity) const = 0;

Q_SIGNALS:
    void availabilityChanged();
};

/** The only backend in this build: honestly unavailable. */
class B3NullTradingBackend : public B3TradingBackend
{
    Q_OBJECT

public:
    using B3TradingBackend::B3TradingBackend;

    bool available() const override { return false; }
    QString unavailableReason() const override
    {
        return tr("No trading backend is available in this build.");
    }
    std::optional<qint64> estimatedFee(qint64, qint64) const override { return std::nullopt; }
};

#endif // BITCOIN_QT_B3MARKETMODEL_H
