// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_QT_B3FLOWMESHMARKETDATA_H
#define BITCOIN_QT_B3FLOWMESHMARKETDATA_H

#include <consensus/amount.h>
#include <QString>
#include <univalue.h>
#include <cstdint>
#include <optional>
#include <vector>

namespace B3FlowMeshMarketData {
//! Verified token precision is required. No ticker-based decimal guesses.
struct Units {
    QString asset, ticker, name, source;
    int decimals{0};
    bool known{false}, test_only{false};
    CAmount quantity_step{1}, price_step{1};
};
QString FormatAmount(CAmount raw, int decimals);
QString FormatPrice(CAmount raw_atoms_per_raw_unit, int base_decimals);
std::optional<CAmount> ParseQuantity(const QString& text, const Units& units, QString* error = nullptr);
std::optional<CAmount> ParsePrice(const QString& text, const Units& units, QString* error = nullptr);
std::optional<CAmount> Notional(CAmount raw_price, CAmount raw_quantity);
//! The published aggregate 100-ppm fee; seller allocation is not a future quote.
std::optional<CAmount> FeeExample(CAmount raw_notional);

struct Point { CAmount price{0}, quantity{0}; };
struct Curve {
    QString account, side;
    std::vector<Point> points;
    CAmount filled{0}, remaining{0}, reserved{0};
};
struct Depth { CAmount price{0}, demand{0}, supply{0}; };
struct Trade {
    uint64_t sequence{0}, epoch{0};
    QString hash;
    int64_t anchor_height{0};
    bool cleared{false}, own_fills_known{false};
    CAmount price{0}, quantity{0}, notional{0}, fee{0}, own_buy{0}, own_sell{0};
};
struct Snapshot {
    QString market, base, domain, config, head, state_root, account, halt, error;
    Units units;
    bool certified{false}, running{false}, paused{true}, handoff{false}, observer{true}, unchanged{false};
    bool curves_complete{false}, history_truncated{false}, history_available{false}, history_page_partial{false};
    uint64_t next_sequence{0}, epoch{0}, account_sequence{0}, active_seats{0}, quorum_required{0}, pending_actions{0};
    CAmount base_available{0}, base_reserved{0}, b3_available{0}, b3_reserved{0};
    std::vector<Curve> curves, own_curves;
    std::vector<Trade> history;
    std::vector<Depth> depth;
};
//! Fail closed; no JSON display values are financial sources of truth.
Snapshot Parse(const UniValue& value);
std::vector<Depth> Aggregate(const std::vector<Curve>& curves);
//! Poll health is not evidence that validators currently form a quorum.
bool AdmissionReady(const Snapshot& snapshot, int64_t response_age_ms, int64_t certificate_age_ms, int64_t queued_without_progress_ms = -1);
QString StatusText(const Snapshot& snapshot, int64_t response_age_ms, int64_t certificate_age_ms, int64_t queued_without_progress_ms = -1);
} // namespace B3FlowMeshMarketData
#endif
