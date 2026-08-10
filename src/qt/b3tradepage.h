// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_B3TRADEPAGE_H
#define BITCOIN_QT_B3TRADEPAGE_H

#include <qt/b3marketmodel.h>

#include <QWidget>

class B3ChartWidget;

QT_BEGIN_NAMESPACE
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableView;
class QTabWidget;
QT_END_NAMESPACE

/**
 * The B3FlowMesh trading workspace shell: market header, native chart,
 * order book, recent trades, trade ticket and orders/positions/fills
 * tabs. Entirely model-driven; with the null backend of this build
 * every surface shows an honest unavailable/empty state, the ticket's
 * primary action stays disabled, and nothing can be submitted.
 */
class B3TradePage : public QWidget
{
    Q_OBJECT

public:
    explicit B3TradePage(QWidget* parent = nullptr);

    //! Attach a trading backend (not owned; null falls back to the
    //! built-in null backend semantics).
    void setBackend(B3TradingBackend* backend);
    B3TradingBackend* backend() const { return m_backend; }

    B3ChartWidget* chart() const { return m_chart; }
    B3CandleSeries* candles() const { return m_candles; }
    B3OrderBookModel* bids() const { return m_bids; }
    B3OrderBookModel* asks() const { return m_asks; }
    B3TradesModel* trades() const { return m_trades; }

private Q_SLOTS:
    void updateAvailability();
    void updateTicketTotal();

private:
    B3TradingBackend* m_backend{nullptr};
    B3NullTradingBackend* m_null_backend{nullptr};

    B3CandleSeries* m_candles{nullptr};
    B3OrderBookModel* m_bids{nullptr};
    B3OrderBookModel* m_asks{nullptr};
    B3TradesModel* m_trades{nullptr};

    QComboBox* m_market_selector{nullptr};
    QLabel* m_market_label{nullptr};
    QLabel* m_availability{nullptr};
    B3ChartWidget* m_chart{nullptr};
    QTableView* m_bid_view{nullptr};
    QTableView* m_ask_view{nullptr};
    QTableView* m_trades_view{nullptr};
    QLabel* m_book_state{nullptr};

    QPushButton* m_buy{nullptr};
    QPushButton* m_sell{nullptr};
    QComboBox* m_order_type{nullptr};
    QLineEdit* m_price{nullptr};
    QLineEdit* m_quantity{nullptr};
    QLabel* m_total{nullptr};
    QLabel* m_balance{nullptr};
    QLabel* m_fee{nullptr};
    QPushButton* m_submit{nullptr};
    QLabel* m_ticket_note{nullptr};
    QTabWidget* m_tabs{nullptr};
};

#endif // BITCOIN_QT_B3TRADEPAGE_H
