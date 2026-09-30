// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_QT_B3FLOWMESHTRADETAPE_H
#define BITCOIN_QT_B3FLOWMESHTRADETAPE_H
#include <qt/b3flowmeshmarketdata.h>
#include <QWidget>
#include <deque>
#include <map>
#include <optional>
#include <vector>

class QLabel;
class QTableWidget;

/** Recent auctions, newest first: price, matched size and time.
 *
 * Display only. FlowMesh clears one uniform-price auction per certified
 * entry: there is no taker side, so a price is coloured by its change from
 * the previous auction's price (an unchanged price keeps the last colour).
 * A time is shown only where the source observed the auction live, as local
 * HH:mm:ss; an auction applied late, without a time, or from history before
 * the source recorded times shows "—" (its bound, if any, is in the
 * tooltip). Nothing is interpolated or backdated. Rows are updated in
 * place; an identical update changes no item. */
class B3FlowMeshTradeTape : public QWidget
{
    Q_OBJECT
public:
    static constexpr size_t MAX_ROWS{50};
    struct Row {
        uint64_t sequence{0};
        QString hash;
        CAmount price{0}, volume{0};
        //! +1 up, -1 down against the previous auction (zero tick keeps the
        //! last direction), 0 for the first one held.
        int direction{0};
        std::optional<flowmesh::ObservedStamp> stamp;
        bool operator==(const Row&) const = default;
    };
    //! The newest cleared held ticks, newest first, at most MAX_ROWS, with
    //! the stamps held for them.
    static std::vector<Row> BuildRows(const std::deque<B3FlowMeshMarketData::Tick>& ticks,
                                      const std::map<uint64_t, flowmesh::ObservedStamp>& times);
    //! "HH:mm:ss" (local) for a live stamp, else "—"; `tooltip` explains it,
    //! with UTC milliseconds and the sequence.
    static QString TimeText(const Row& row, const QString& source, bool remote, QString* tooltip = nullptr);

    explicit B3FlowMeshTradeTape(QWidget* parent = nullptr);
    //! `source` observed the times (empty: none); `reason` says why there
    //! are none when that is known.
    void setRows(const std::vector<Row>& rows, const B3FlowMeshMarketData::Units& units, bool inverse, const QString& source,
                 bool remote, const QString& reason);
    QTableWidget* table() const { return m_table; }

private:
    QTableWidget* m_table;
    QLabel* m_note;
};
#endif
