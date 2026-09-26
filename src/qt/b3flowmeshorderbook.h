// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_QT_B3FLOWMESHORDERBOOK_H
#define BITCOIN_QT_B3FLOWMESHORDERBOOK_H
#include <qt/b3flowmeshmarketdata.h>
#include <QWidget>
#include <optional>

class QLabel;
class QTableWidget;

//! A limit-shaped subset of the existing curve auction, never a new matching engine.
//! Uses normal accessible table items; background refreshes do not rebuild widgets.
class B3FlowMeshOrderBook : public QWidget
{
    Q_OBJECT
public:
    explicit B3FlowMeshOrderBook(QWidget* parent = nullptr);
    void setSnapshot(const std::optional<B3FlowMeshMarketData::Snapshot>& snapshot, bool inverse);
    void setStale(bool stale);
private:
    QTableWidget *m_asks, *m_bids;
    QLabel *m_last, *m_spread, *m_note, *m_stale;
};
#endif
