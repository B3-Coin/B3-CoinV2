// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/b3marketmodel.h>

#include <qt/b3fixed.h>
#include <qt/guiutil.h>

#include <QDateTime>

#include <algorithm>

B3CandleSeries::B3CandleSeries(QObject* parent)
    : QObject{parent}
{
}

void B3CandleSeries::setPrecision(int price_decimals, int quantity_decimals)
{
    m_price_decimals = price_decimals;
    m_quantity_decimals = quantity_decimals;
}

bool B3CandleSeries::wellFormed(const B3Candle& candle)
{
    return candle.high >= candle.low && candle.volume >= 0 &&
           candle.open >= candle.low && candle.open <= candle.high &&
           candle.close >= candle.low && candle.close <= candle.high;
}

void B3CandleSeries::setCandles(QVector<B3Candle> candles)
{
    // Sanitize: drop malformed entries, sort by time, last duplicate wins.
    candles.erase(std::remove_if(candles.begin(), candles.end(),
                                 [](const B3Candle& c) { return !wellFormed(c); }),
                  candles.end());
    std::stable_sort(candles.begin(), candles.end(),
                     [](const B3Candle& a, const B3Candle& b) { return a.timestamp < b.timestamp; });
    QVector<B3Candle> deduped;
    deduped.reserve(candles.size());
    for (const B3Candle& candle : candles) {
        if (!deduped.isEmpty() && deduped.last().timestamp == candle.timestamp) {
            deduped.last() = candle;
        } else {
            deduped.push_back(candle);
        }
    }
    m_candles = std::move(deduped);
    Q_EMIT resetted();
}

void B3CandleSeries::append(const B3Candle& candle)
{
    if (!wellFormed(candle)) return;
    if (m_candles.isEmpty() || candle.timestamp > m_candles.last().timestamp) {
        m_candles.push_back(candle);
        Q_EMIT updated(m_candles.size() - 1);
        return;
    }
    // Late or replacement data: keep the series ordered.
    auto it = std::lower_bound(m_candles.begin(), m_candles.end(), candle.timestamp,
                               [](const B3Candle& c, qint64 ts) { return c.timestamp < ts; });
    const int index = static_cast<int>(it - m_candles.begin());
    if (it != m_candles.end() && it->timestamp == candle.timestamp) {
        *it = candle;
    } else {
        m_candles.insert(it, candle);
    }
    Q_EMIT updated(index);
}

void B3CandleSeries::clear()
{
    m_candles.clear();
    Q_EMIT resetted();
}

B3OrderBookModel::B3OrderBookModel(Side side, QObject* parent)
    : QAbstractTableModel{parent},
      m_side{side}
{
}

void B3OrderBookModel::setPrecision(int price_decimals, int quantity_decimals)
{
    m_price_decimals = price_decimals;
    m_quantity_decimals = quantity_decimals;
}

void B3OrderBookModel::setLevels(const QVector<Level>& levels)
{
    beginResetModel();
    m_levels = levels;
    // Bids descend, asks ascend, best price first.
    std::stable_sort(m_levels.begin(), m_levels.end(), [this](const Level& a, const Level& b) {
        return m_side == Side::Bid ? a.price > b.price : a.price < b.price;
    });
    m_cumulative.resize(m_levels.size());
    qint64 running = 0;
    for (int i = 0; i < m_levels.size(); ++i) {
        running += m_levels[i].quantity;
        m_cumulative[i] = running;
    }
    m_state = m_levels.isEmpty() ? State::Empty : State::Live;
    endResetModel();
}

void B3OrderBookModel::setState(State state)
{
    beginResetModel();
    m_state = state;
    if (state != State::Live) {
        m_levels.clear();
        m_cumulative.clear();
    }
    endResetModel();
}

int B3OrderBookModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_levels.size();
}

int B3OrderBookModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant B3OrderBookModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_levels.size()) return {};
    const Level& level = m_levels.at(index.row());

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case Price: return B3Fixed::format(level.price, m_price_decimals);
        case Quantity: return B3Fixed::format(level.quantity, m_quantity_decimals);
        case Total: return B3Fixed::format(m_cumulative.at(index.row()), m_quantity_decimals);
        }
    }
    if (role == Qt::TextAlignmentRole) {
        return QVariant{Qt::AlignRight | Qt::AlignVCenter};
    }
    if (role == DepthRole && !m_cumulative.isEmpty() && m_cumulative.last() > 0) {
        return static_cast<double>(m_cumulative.at(index.row())) / static_cast<double>(m_cumulative.last());
    }
    return {};
}

QVariant B3OrderBookModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
    case Price: return tr("Price");
    case Quantity: return tr("Quantity");
    case Total: return tr("Total");
    }
    return {};
}

B3TradesModel::B3TradesModel(QObject* parent)
    : QAbstractTableModel{parent}
{
}

void B3TradesModel::setPrecision(int price_decimals, int quantity_decimals)
{
    m_price_decimals = price_decimals;
    m_quantity_decimals = quantity_decimals;
}

void B3TradesModel::setTrades(const QVector<Trade>& trades)
{
    beginResetModel();
    m_trades = trades;
    endResetModel();
}

int B3TradesModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_trades.size();
}

int B3TradesModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant B3TradesModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_trades.size()) return {};
    const Trade& trade = m_trades.at(index.row());
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case Time: return GUIUtil::dateTimeStr(QDateTime::fromSecsSinceEpoch(trade.timestamp));
        case Price: return B3Fixed::format(trade.price, m_price_decimals);
        case Quantity: return B3Fixed::format(trade.quantity, m_quantity_decimals);
        }
    }
    if (role == Qt::TextAlignmentRole && index.column() != Time) {
        return QVariant{Qt::AlignRight | Qt::AlignVCenter};
    }
    return {};
}

QVariant B3TradesModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
    case Time: return tr("Time");
    case Price: return tr("Price");
    case Quantity: return tr("Quantity");
    }
    return {};
}

B3EmptyTableModel::B3EmptyTableModel(QStringList headers, QObject* parent)
    : QAbstractTableModel{parent},
      m_headers{std::move(headers)}
{
}

int B3EmptyTableModel::rowCount(const QModelIndex&) const
{
    return 0;
}

int B3EmptyTableModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_headers.size();
}

QVariant B3EmptyTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    if (section < 0 || section >= m_headers.size()) return {};
    return m_headers.at(section);
}
