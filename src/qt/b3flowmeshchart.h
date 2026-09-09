// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_QT_B3FLOWMESHCHART_H
#define BITCOIN_QT_B3FLOWMESHCHART_H
#include <qt/b3flowmeshmarketdata.h>
#include <QWidget>
#include <optional>

//! Display-only projection of certified integers; never used to price an order.
class B3FlowMeshChart : public QWidget
{
    Q_OBJECT
public:
    enum class Mode { Prices, Liquidity };
    explicit B3FlowMeshChart(QWidget* parent = nullptr);
    void setSnapshot(const std::optional<B3FlowMeshMarketData::Snapshot>& snapshot);
    void setMode(Mode mode);
    void setLoading(bool loading);
    void setStale(bool stale);
    int pricePointCount() const;
    QString emptyMessage() const;
    QSize minimumSizeHint() const override { return {280, 260}; }
protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;
private:
    std::optional<B3FlowMeshMarketData::Snapshot> m_snapshot;
    Mode m_mode{Mode::Prices};
    bool m_loading{false}, m_stale{false};
    QPointF m_pointer{-1, -1};
};
#endif
