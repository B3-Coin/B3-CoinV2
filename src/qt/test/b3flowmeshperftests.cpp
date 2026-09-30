// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
//
// FlowMesh Qt UI responsiveness harness: the U1-U5 metrics of
// doc/flowmesh-latency-definition.md (provisional targets).
//
// Everything here is synthetic and in-process: a regtest TestingSetup without
// networking, an unregistered offline wallet that is never unlocked, and a
// fake read-only market (the RPC table's getflowmeshmarketdata and its
// companions) that commits heads on a clock. No socket, signing path,
// submission or real node data is involved.
//
// Environment:
//   FLOWMESH_UI_PERF_STRICT=1        assert the provisional targets (meant
//                                    for a Release build on the owner's Mac);
//                                    by default only 10x the targets fails.
//   FLOWMESH_UI_PERF_REPORT=<path>   write the JSON report there (absolute).
//   FLOWMESH_UI_PERF_COMMIT=<sha>    the source commit, for the report.
// Both strict and report runs use the longer sample windows.

#include <clientversion.h>
#include <flowmesh/market.h>
#include <flowmesh/market_view.h>
#include <flowmesh/microblock.h>
#include <flowmesh/observed_time.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <node/flowmesh_client.h>
#include <qt/b3flowmeshchart.h>
#include <qt/b3flowmeshmarketdata.h>
#include <qt/b3flowmeshorderbook.h>
#include <qt/b3flowmeshtradingpanel.h>
#include <qt/b3theme.h>
#include <qt/clientmodel.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>
#include <rpc/server.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <wallet/context.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSettings>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#ifndef B3_UI_PERF_BUILD_TYPE
#define B3_UI_PERF_BUILD_TYPE "unknown"
#endif

const std::function<void(const std::string&)> G_TEST_LOG_FUN{};
const std::function<std::vector<const char*>()> G_TEST_COMMAND_LINE_ARGUMENTS{};
const std::function<std::string()> G_TEST_GET_FULL_NAME{[] { return "b3_flowmesh_perf"; }};

using namespace B3FlowMeshMarketData;

namespace {
using Clock = std::chrono::steady_clock;
int64_t NowUs() { return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count(); }
double Ms(int64_t us) { return static_cast<double>(us) / 1000.0; }

uint256 H(unsigned char n) { uint256 h; h.begin()[0] = n; return h; }
//! A distinct hash for `n` in series `tag` of market `market`; SequenceOf
//! reads `n` back.
uint256 Tagged(uint64_t n, unsigned char tag, unsigned char market = 0)
{
    uint256 h;
    for (int i{0}; i < 8; ++i) h.begin()[i] = static_cast<unsigned char>(n >> (8 * i));
    h.begin()[29] = market; h.begin()[30] = tag; h.begin()[31] = 0xb3;
    return h;
}
std::optional<std::pair<uint64_t, unsigned char>> SequenceOf(const QString& hex)
{
    const auto hash{uint256::FromHex(hex.toStdString())};
    if (!hash || hash->begin()[31] != 0xb3) return std::nullopt;
    uint64_t n{0};
    for (int i{0}; i < 8; ++i) n |= static_cast<uint64_t>(hash->begin()[i]) << (8 * i);
    return std::pair{n, hash->begin()[29]};
}
constexpr unsigned char HEAD_TAG{0x48};
constexpr int64_t TICK_SPACING_MS{12'000};

bool Strict() { return qEnvironmentVariable("FLOWMESH_UI_PERF_STRICT") == QStringLiteral("1"); }
bool Measuring() { return Strict() || !qEnvironmentVariable("FLOWMESH_UI_PERF_REPORT").isEmpty(); }
//! The provisional target in strict mode, else a smoke ceiling of 10x it.
double Ceiling(double target_ms) { return Strict() ? target_ms : 10 * target_ms; }

// doc/flowmesh-latency-definition.md: nearest-rank statistics; p99 is not
// estimable from fewer than 100 samples.
struct Distribution {
    size_t n{0};
    double p50{0}, p90{0}, p95{0}, p99{0}, max{0};
    bool p99_estimable{false};
};
Distribution Distribute(std::vector<double> values)
{
    Distribution d;
    d.n = values.size();
    if (values.empty()) return d;
    std::sort(values.begin(), values.end());
    // Rank ceil(n x q), in integer permille so no rounding moves a rank.
    const auto rank = [&](size_t permille) { return values[std::max<size_t>(1, (values.size() * permille + 999) / 1000) - 1]; };
    d.p50 = rank(500); d.p90 = rank(900); d.p95 = rank(950); d.p99 = rank(990);
    d.max = values.back();
    d.p99_estimable = values.size() >= 100;
    return d;
}
UniValue Json(const Distribution& d)
{
    UniValue v{UniValue::VOBJ};
    v.pushKV("n", static_cast<uint64_t>(d.n));
    const auto round = [](double ms) { return std::round(ms * 1000.0) / 1000.0; };
    v.pushKV("p50_ms", round(d.p50)); v.pushKV("p90_ms", round(d.p90)); v.pushKV("p95_ms", round(d.p95));
    v.pushKV("p99_ms", round(d.p99)); v.pushKV("max_ms", round(d.max)); v.pushKV("p99_estimable", d.p99_estimable);
    return v;
}
QString Describe(const char* name, const Distribution& d)
{
    return QStringLiteral("%1 n=%2 p50=%3 p90=%4 p95=%5 p99=%6%7 max=%8 ms").arg(QLatin1String(name)).arg(d.n)
        .arg(d.p50, 0, 'f', 3).arg(d.p90, 0, 'f', 3).arg(d.p95, 0, 'f', 3).arg(d.p99, 0, 'f', 3)
        .arg(d.p99_estimable ? QString{} : QStringLiteral(" (not estimable)")).arg(d.max, 0, 'f', 3);
}

//! The report's cases, written once at the end of the run.
UniValue& ReportCases() { static UniValue cases{UniValue::VOBJ}; return cases; }
void Record(const std::string& name, UniValue value)
{
    auto& cases{ReportCases()};
    if (cases.exists(name)) {
        UniValue copy{UniValue::VOBJ};
        for (size_t i{0}; i < cases.size(); ++i) {
            if (cases.getKeys()[i] != name) copy.pushKV(cases.getKeys()[i], cases.getValues()[i]);
        }
        cases = std::move(copy);
    }
    cases.pushKV(name, std::move(value));
}
std::string CpuName()
{
#if defined(__APPLE__)
    char brand[256]{};
    size_t size{sizeof(brand)};
    if (sysctlbyname("machdep.cpu.brand_string", brand, &size, nullptr, 0) == 0) return brand;
#endif
    QFile info{QStringLiteral("/proc/cpuinfo")};
    if (info.open(QIODevice::ReadOnly)) {
        for (const auto& line : QString::fromLatin1(info.readAll()).split(QLatin1Char('\n'))) {
            if (line.startsWith(QStringLiteral("model name"))) return line.section(QLatin1Char(':'), 1).trimmed().toStdString();
        }
    }
    return QSysInfo::currentCpuArchitecture().toStdString();
}

using B3FlowMeshMarketData::Curve;
Curve LimitCurve(bool bid, const uint256& account, CAmount price, CAmount quantity, CAmount filled)
{
    Curve curve;
    curve.account = QString::fromStdString(account.GetHex()); curve.side = bid ? QStringLiteral("bid") : QStringLiteral("ask");
    const auto points{bid ? flowmesh::MakeLimitBidCurve(price, quantity) : flowmesh::MakeLimitAskCurve(price, quantity)};
    if (!points) throw std::runtime_error{"Invalid synthetic limit curve"};
    for (const auto& point : *points) curve.points.push_back({point.price, point.qty});
    curve.filled = filled; curve.remaining = quantity - filled;
    curve.reserved = bid ? Notional(price, curve.remaining).value_or(0) : curve.remaining;
    return curve;
}
Curve LadderCurve(bool bid, const uint256& account, const std::vector<flowmesh::LadderStep>& steps)
{
    Curve curve;
    curve.account = QString::fromStdString(account.GetHex()); curve.side = bid ? QStringLiteral("bid") : QStringLiteral("ask");
    const auto points{bid ? flowmesh::MakeLadderBidCurve(steps) : flowmesh::MakeLadderAskCurve(steps)};
    if (!points) throw std::runtime_error{"Invalid synthetic ladder curve"};
    for (const auto& point : *points) curve.points.push_back({point.price, point.qty});
    curve.remaining = bid ? curve.points.front().quantity : curve.points.back().quantity;
    curve.reserved = *flowmesh::LadderReservation(bid, steps);
    return curve;
}
UniValue CurveJson(const Curve& curve)
{
    UniValue c{UniValue::VOBJ};
    c.pushKV("account_id", curve.account.toStdString()); c.pushKV("side", curve.side.toStdString());
    c.pushKV("filled_quantity", curve.filled); c.pushKV("remaining_quantity", curve.remaining); c.pushKV("reserved_amount", curve.reserved);
    UniValue points{UniValue::VARR};
    for (const auto& point : curve.points) { UniValue p{UniValue::VOBJ}; p.pushKV("price", point.price); p.pushKV("quantity", point.quantity); points.push_back(p); }
    c.pushKV("points", points);
    return c;
}
//! The node's "book_levels" of `curves`: the engine's own aggregation.
UniValue LevelsJson(const std::vector<const Curve*>& curves, const uint256& head, const uint256& own)
{
    std::vector<flowmesh::ClearingEngine::CurveView> views;
    for (const auto* curve : curves) {
        flowmesh::ClearingEngine::CurveView view;
        view.account_id = *uint256::FromHex(curve->account.toStdString());
        view.side = curve->side == QStringLiteral("bid") ? flowmesh::ClearingEngine::Side::BID : flowmesh::ClearingEngine::Side::ASK;
        for (const auto& point : curve->points) view.points.push_back({point.price, point.quantity});
        view.filled_quantity = curve->filled; view.remaining_quantity = curve->remaining; view.reserved_amount = curve->reserved;
        views.push_back(std::move(view));
    }
    flowmesh::BookLevelsQuery query;
    query.depth = flowmesh::MARKET_DATA_MAX_LEVELS; query.account = own;
    auto levels{flowmesh::ComputeBookLevels(views, query)};
    levels.head = head; levels.standing.head = head;
    return node::FlowMeshBookLevelsJson(levels);
}

/** One synthetic certified market, head by head: 57 limit orders a side from
 * 114 accounts, a four-price ladder a side, the viewer's own bid, a clearing
 * tick per head with its observed time, and candles rolled up from those
 * times as a node does. Every head moves some sizes, and every third one a
 * price, so each changes the book. Thread-safe: the RPC fake reads it on
 * worker threads while a test commits heads. */
class SyntheticMarket
{
public:
    struct Options {
        unsigned char index{0};
        unsigned char domain{1}, base{2};
        std::string ticker{"tUSD"};
        bool remote{false};
        std::string endpoint{"https://perf.invalid:18443"};
    };
    explicit SyntheticMarket(Options options) : m_options{std::move(options)}
    {
        m_id = flowmesh::ComputeFlowMeshMarketId(H(m_options.domain), H(m_options.base))->GetHex();
        for (int i{0}; i < 57; ++i) {
            const CAmount quantity{60'000 + static_cast<CAmount>((i * 37) % 9) * 45'000};
            m_bids.push_back(LimitCurve(true, Tagged(static_cast<uint64_t>(i), 0xA0, m_options.index), 999 - i, quantity, 0));
            m_asks.push_back(LimitCurve(false, Tagged(static_cast<uint64_t>(i), 0xA1, m_options.index), 1001 + i, quantity + 30'000, 0));
            m_bid_base.push_back(999 - i); m_ask_base.push_back(1001 + i);
        }
        m_ladders.push_back(LadderCurve(true, Tagged(1, 0xA2, m_options.index), {{990, 100'000}, {985, 100'000}, {980, 100'000}, {975, 100'000}}));
        m_ladders.push_back(LadderCurve(false, Tagged(2, 0xA2, m_options.index), {{1010, 150'000}, {1015, 150'000}, {1020, 150'000}, {1025, 150'000}}));
        m_own = LimitCurve(true, H(5), 997, 250'000, 50'000);
        // Five hours of history before the stream: one auction every 12 s.
        const int64_t now{QDateTime::currentMSecsSinceEpoch()};
        constexpr uint64_t HISTORY{1500};
        m_next = 1;
        for (uint64_t i{0}; i < HISTORY; ++i) AddHead(now - static_cast<int64_t>(HISTORY - i) * TICK_SPACING_MS, /*mutate=*/false);
        RebuildViews();
    }
    const std::string& Id() const { return m_id; }
    const Options& options() const { return m_options; }
    //! A new certified head, observed now.
    uint64_t Commit()
    {
        std::lock_guard lock{m_mutex};
        AddHead(QDateTime::currentMSecsSinceEpoch(), /*mutate=*/true);
        RebuildViews();
        const uint64_t sequence{m_next - 1};
        m_committed_us[sequence] = NowUs();
        return sequence;
    }
    uint64_t HeadSequence() const { std::lock_guard lock{m_mutex}; return m_next - 1; }
    std::string HeadHex() const { std::lock_guard lock{m_mutex}; return Head().GetHex(); }
    std::map<uint64_t, int64_t> Committed() const { std::lock_guard lock{m_mutex}; return m_committed_us; }
    UniValue Skeleton(bool unchanged) const
    {
        UniValue v{UniValue::VOBJ};
        v.pushKV("market_id", m_id); v.pushKV("domain", H(m_options.domain).GetHex()); v.pushKV("base_asset_id", H(m_options.base).GetHex());
        v.pushKV("execution_config_id", H(3).GetHex()); v.pushKV("quote_asset", "B3"); v.pushKV("matching_model", "uniform-price-curve-auction");
        v.pushKV("quantity_lot_raw", 1); v.pushKV("price_tick_raw", 1); v.pushKV("unchanged", unchanged);
        UniValue snapshot{UniValue::VOBJ};
        snapshot.pushKV("certified", true); snapshot.pushKV("next_microblock_sequence", m_next); snapshot.pushKV("last_microblock_hash", Head().GetHex());
        snapshot.pushKV("state_root", Tagged(m_next - 1, 0x52, m_options.index).GetHex()); snapshot.pushKV("epoch", 0); snapshot.pushKV("active_seats", 4);
        snapshot.pushKV("quorum_required", 3); snapshot.pushKV("running", true); snapshot.pushKV("paused", false); snapshot.pushKV("pending_handoff", false);
        snapshot.pushKV("observer_only", false); snapshot.pushKV("halt", "none"); snapshot.pushKV("error", ""); snapshot.pushKV("pending_actions", 0);
        v.pushKV("snapshot", snapshot);
        UniValue metadata{UniValue::VOBJ};
        metadata.pushKV("known", true); metadata.pushKV("decimals", 6); metadata.pushKV("ticker", m_options.ticker); metadata.pushKV("name", "Synthetic perf asset");
        metadata.pushKV("source", "isolated-perf-fixture"); metadata.pushKV("test_only", true);
        v.pushKV("base_metadata", metadata);
        if (m_options.remote) {
            UniValue proof{UniValue::VOBJ};
            proof.pushKV("source", "remote_endpoint"); proof.pushKV("endpoint", m_options.endpoint);
            proof.pushKV("certificate_verified", true); proof.pushKV("account_state_verified", true); proof.pushKV("execution_result_verified", false);
            proof.pushKV("b3_checkpoint_confirmed", false); proof.pushKV("event_gap", false);
            v.pushKV("verification", proof);
        }
        return v;
    }
    //! getflowmeshmarketdata as the local RPC answers it: unchanged at the
    //! known head (with levels only when asked again), else the whole view.
    UniValue Reply(const UniValue& options, bool* unchanged_out = nullptr) const
    {
        std::lock_guard lock{m_mutex};
        const auto& known{options.find_value("known_head")};
        const bool unchanged{known.isStr() && known.get_str() == Head().GetHex()};
        if (unchanged_out) *unchanged_out = unchanged;
        auto v{Skeleton(unchanged)};
        std::vector<const Tick*> returned;
        if (const auto& ticks{options.find_value("ticks")}; ticks.isObject()) {
            const size_t limit{ticks.find_value("limit").isNum() ? static_cast<size_t>(ticks.find_value("limit").getInt<int64_t>()) : size_t{64}};
            const auto& after{ticks.find_value("after_sequence")};
            bool gap{false}, more{false};
            if (after.isNum()) {
                const auto from{after.getInt<uint64_t>()};
                gap = !m_ticks.empty() && from + 1 < m_ticks.front().sequence;
                for (const auto& tick : m_ticks) {
                    if (tick.sequence <= from) continue;
                    if (returned.size() == limit) { more = true; break; }
                    returned.push_back(&tick);
                }
            } else {
                for (size_t i{m_ticks.size() > limit ? m_ticks.size() - limit : 0}; i < m_ticks.size(); ++i) returned.push_back(&m_ticks[i]);
            }
            UniValue page{UniValue::VOBJ};
            page.pushKV("available", true); page.pushKV("source", m_options.remote ? "endpoint_reported" : "local_engine");
            page.pushKV("gap", gap); page.pushKV("more", more);
            UniValue rows{UniValue::VARR};
            for (const auto* tick : returned) rows.push_back(TickJson(*tick));
            page.pushKV("ticks", rows);
            v.pushKV("clearing_ticks", page);
        }
        if (options.find_value("observed_time").isTrue()) {
            // The rows of what this reply shows: its history (when changed)
            // and ticks, ascending.
            std::map<uint64_t, const Tick*> shown;
            for (const auto* tick : returned) shown.emplace(tick->sequence, tick);
            if (!unchanged) for (size_t i{m_ticks.size() > 100 ? m_ticks.size() - 100 : 0}; i < m_ticks.size(); ++i) shown.emplace(m_ticks[i].sequence, &m_ticks[i]);
            UniValue observed{UniValue::VOBJ};
            observed.pushKV("available", true); observed.pushKV("source", m_options.remote ? m_options.endpoint : "this_node");
            observed.pushKV("source_kind", "operator_observed"); observed.pushKV("certified", false); observed.pushKV("clock_suspect", false);
            observed.pushKV("recorded_since_sequence", 0);
            UniValue rows{UniValue::VARR};
            for (const auto& [sequence, tick] : shown) {
                UniValue row{UniValue::VOBJ};
                row.pushKV("sequence", sequence); row.pushKV("microblock_hash", tick->hash.GetHex()); row.pushKV("mode", "live");
                row.pushKV("observed_ms", tick->observed_ms); row.pushKV("flags", UniValue{UniValue::VARR});
                rows.push_back(row);
            }
            observed.pushKV("rows", rows);
            v.pushKV("observed_time", observed);
        }
        const auto& levels{options.find_value("levels")};
        if (!unchanged || (levels.isObject() && levels.find_value("include_unchanged").isTrue())) v.pushKV("book_levels", m_levels);
        if (unchanged) return v;
        UniValue liquidity{UniValue::VOBJ};
        liquidity.pushKV("curves", m_curves); liquidity.pushKV("total_curves", static_cast<uint64_t>(m_curves.size())); liquidity.pushKV("complete", true);
        v.pushKV("liquidity", liquidity);
        UniValue account{UniValue::VOBJ};
        account.pushKV("account_id", H(5).GetHex()); account.pushKV("next_sequence", 8);
        account.pushKV("base_available", 25'000'000); account.pushKV("base_reserved", 0);
        account.pushKV("b3_available_atoms", 90'000'000'000); account.pushKV("b3_reserved_atoms", m_own.reserved);
        UniValue own{UniValue::VARR}; own.push_back(CurveJson(m_own)); account.pushKV("curves", own);
        v.pushKV("account", account);
        UniValue history{UniValue::VOBJ};
        history.pushKV("available", true); history.pushKV("truncated", true); history.pushKV("scope", "bounded-certified-log-cache");
        UniValue entries{UniValue::VARR};
        for (size_t i{m_ticks.size() > 100 ? m_ticks.size() - 100 : 0}; i < m_ticks.size(); ++i) {
            const auto& tick{m_ticks[i]};
            UniValue t{UniValue::VOBJ};
            t.pushKV("sequence", tick.sequence); t.pushKV("microblock_hash", tick.hash.GetHex()); t.pushKV("epoch", 0); t.pushKV("anchor_height", 50);
            t.pushKV("cleared", tick.cleared); t.pushKV("price", tick.cleared ? tick.price : 0); t.pushKV("quantity", tick.cleared ? tick.volume : 0);
            t.pushKV("notional_atoms", tick.cleared ? tick.price * tick.volume : 0); t.pushKV("fee_atoms", tick.cleared ? *FeeExample(tick.price * tick.volume) : 0);
            t.pushKV("account_fills_known", true); t.pushKV("account_bid_fill", tick.cleared && tick.sequence % 4 == 0 ? tick.volume / 4 : 0); t.pushKV("account_ask_fill", 0);
            entries.push_back(t);
        }
        history.pushKV("entries", entries);
        v.pushKV("history", history);
        return v;
    }
    UniValue CandlesReply(const UniValue& options) const
    {
        std::lock_guard lock{m_mutex};
        const auto interval{flowmesh::ParseCandleInterval(options.find_value("interval").get_str())};
        if (!interval) throw std::runtime_error{"Synthetic candles: unknown interval"};
        const size_t limit{options.find_value("limit").isNum() ? static_cast<size_t>(options.find_value("limit").getInt<int64_t>()) : size_t{300}};
        auto page{m_rollup.Page(*interval, std::nullopt, limit)};
        page.available = true; page.source = m_options.remote ? m_options.endpoint : "this_node"; page.remote = m_options.remote;
        return node::FlowMeshObservedCandlePageJson(page);
    }
    //! listflowmeshmarkets / getflowmeshbalance.
    UniValue MarketRow() const
    {
        UniValue row{UniValue::VOBJ};
        row.pushKV("market_id", m_id); row.pushKV("base_asset_id", H(m_options.base).GetHex()); row.pushKV("domain", H(m_options.domain).GetHex());
        row.pushKV("execution_config_id", H(3).GetHex()); row.pushKV("quote_asset", "B3");
        row.pushKV("vault_id", flowmesh::ComputeFlowMeshVaultId(H(m_options.domain), *uint256::FromHex(m_id))->GetHex());
        row.pushKV("available", true); row.pushKV("checkpoint_pending", false);
        for (const auto* key : {"running", "paused", "pending_handoff"}) row.pushKV(key, std::string{key} == "running");
        row.pushKV("halt", "none"); row.pushKV("error", "");
        auto skeleton{Skeleton(false)};
        if (skeleton.exists("verification")) row.pushKV("verification", skeleton["verification"]);
        UniValue account{UniValue::VOBJ};
        account.pushKV("account_id", H(5).GetHex()); account.pushKV("next_sequence", 8);
        account.pushKV("base_available", 25'000'000); account.pushKV("base_reserved", 0);
        account.pushKV("b3_available", "90"); account.pushKV("b3_reserved", FormatAmount(m_own.reserved, 9).toStdString());
        row.pushKV("account", account);
        return row;
    }

private:
    struct Tick {
        uint64_t sequence{0};
        uint256 hash;
        bool cleared{false};
        CAmount price{0}, volume{0};
        int64_t observed_ms{0};
    };
    uint256 Head() const { return Tagged(m_next - 1, HEAD_TAG, m_options.index); }
    static UniValue TickJson(const Tick& tick)
    {
        UniValue t{UniValue::VOBJ};
        t.pushKV("sequence", tick.sequence); t.pushKV("microblock_hash", tick.hash.GetHex()); t.pushKV("kind", "execution");
        t.pushKV("cleared", tick.cleared); t.pushKV("price", tick.cleared ? tick.price : 0); t.pushKV("volume", tick.cleared ? tick.volume : 0);
        t.pushKV("imbalance", 0); t.pushKV("imbalance_side", "none");
        return t;
    }
    void AddHead(int64_t observed_ms, bool mutate)
    {
        const uint64_t sequence{m_next++};
        if (mutate) {
            // Partial fills move sizes; every third head moves a price.
            const auto fill = [](Curve& curve, bool bid, CAmount price) {
                const CAmount quantity{curve.remaining + curve.filled};
                curve = LimitCurve(bid, *uint256::FromHex(curve.account.toStdString()), price,
                                   quantity, curve.remaining > 20'000 ? curve.filled + 5'000 : 0);
            };
            const size_t k{static_cast<size_t>(sequence % m_bids.size())}, j{static_cast<size_t>((sequence * 7) % m_asks.size())};
            fill(m_bids[k], true, m_bids[k].points[0].price);
            fill(m_asks[j], false, m_asks[j].points[1].price);
            if (sequence % 3 == 0) {
                const size_t b{static_cast<size_t>((sequence / 3) % m_bids.size())}, a{static_cast<size_t>((sequence / 3 * 5) % m_asks.size())};
                const CAmount bid_price{m_bids[b].points[0].price == m_bid_base[b] ? m_bid_base[b] - 60 : m_bid_base[b]};
                const CAmount ask_price{m_asks[a].points[1].price == m_ask_base[a] ? m_ask_base[a] + 60 : m_ask_base[a]};
                fill(m_bids[b], true, bid_price);
                fill(m_asks[a], false, ask_price);
            }
        }
        Tick tick;
        tick.sequence = sequence; tick.hash = Tagged(sequence, HEAD_TAG, m_options.index); tick.observed_ms = observed_ms;
        tick.cleared = sequence % 5 != 3;
        if (tick.cleared) {
            m_price = std::clamp<CAmount>(m_price + static_cast<CAmount>((sequence * 7919) % 7) - 3, 992, 1008);
            tick.price = m_price; tick.volume = 20'000 + static_cast<CAmount>((sequence * 104729) % 9) * 22'500;
        }
        flowmesh::ObservedStamp stamp;
        stamp.sequence = sequence; stamp.hash = tick.hash; stamp.mode = flowmesh::ObservedMode::LIVE; stamp.ms = observed_ms;
        flowmesh::ClearingTick clearing;
        clearing.sequence = sequence; clearing.microblock_hash = tick.hash; clearing.cleared = tick.cleared; clearing.price = tick.price; clearing.volume = tick.volume;
        m_rollup.Add(stamp, clearing);
        m_ticks.push_back(tick);
        while (m_ticks.size() > 600) m_ticks.pop_front();
    }
    void RebuildViews()
    {
        std::vector<const Curve*> all;
        for (const auto& curve : m_bids) all.push_back(&curve);
        all.push_back(&m_ladders[0]); all.push_back(&m_own);
        for (const auto& curve : m_asks) all.push_back(&curve);
        all.push_back(&m_ladders[1]);
        m_levels = LevelsJson(all, Head(), H(5));
        m_curves = UniValue{UniValue::VARR};
        for (const auto* curve : all) m_curves.push_back(CurveJson(*curve));
    }

    Options m_options;
    std::string m_id;
    mutable std::mutex m_mutex;
    std::vector<Curve> m_bids, m_asks, m_ladders;
    std::vector<CAmount> m_bid_base, m_ask_base;
    Curve m_own;
    std::deque<Tick> m_ticks;
    CAmount m_price{1000};
    uint64_t m_next{1};
    flowmesh::ObservedRollup m_rollup;
    UniValue m_levels, m_curves;
    std::map<uint64_t, int64_t> m_committed_us;
};

/** The read-only FlowMesh RPCs of a local engine (or, with `remote`, of an
 * engine-off client reading an endpoint), answered from SyntheticMarkets. A
 * watch waits for the next commit like LocalBackend::Watch. Mutations are
 * counted and refused. */
class MarketProbe
{
public:
    explicit MarketProbe(std::vector<SyntheticMarket::Options> markets, int live_ms = 2000) : m_live_ms{live_ms}
    {
        for (auto& options : markets) m_markets.push_back(std::make_unique<SyntheticMarket>(std::move(options)));
        if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
        Add("getflowmeshclientinfo", [this](const JSONRPCRequest&) { return ClientInfo(); });
        Add("listflowmeshactions", [this](const JSONRPCRequest&) {
            { std::lock_guard lock{m_record}; job_starts_us.push_back(NowUs()); }
            UniValue saved{UniValue::VOBJ}; saved.pushKV("source", "local-retained-outbox"); saved.pushKV("actions", UniValue{UniValue::VARR}); return saved;
        });
        Add("listflowmeshmarkets", [this](const JSONRPCRequest&) {
            UniValue rows{UniValue::VARR};
            for (const auto& market : m_markets) rows.push_back(market->MarketRow());
            return rows;
        });
        Add("getflowmeshbalance", [this](const JSONRPCRequest& request) { return Find(request).MarketRow(); });
        Add("listflowmeshvaultoperations", [](const JSONRPCRequest&) { return UniValue{UniValue::VARR}; });
        Add("getflowmeshactionstatus", [](const JSONRPCRequest&) -> UniValue { throw std::runtime_error{"Synthetic status unavailable"}; });
        Add("getflowmeshcandles", [this](const JSONRPCRequest& request) { return Find(request).CandlesReply(request.params[1]); });
        Add("getflowmeshmarketdata", [this](const JSONRPCRequest& request) { return MarketData(request); });
        for (const auto* method : {"submitflowmeshorder", "submitflowmeshladder", "cancelflowmeshorder", "requestflowmeshwithdrawal", "submitflowmeshdeposit", "retryflowmeshaction", "sendrawtransaction"}) {
            Add(method, [this](const JSONRPCRequest&) -> UniValue { ++writes; throw std::runtime_error{"No mutation in the perf harness"}; });
        }
    }
    ~MarketProbe()
    {
        {
            std::lock_guard lock{m_wake_mutex};
            m_closing = true;
        }
        m_wake.notify_all();
        for (const auto& command : m_commands) tableRPC.removeCommand(command->name, command.get());
    }
    SyntheticMarket& market(size_t index = 0) { return *m_markets[index]; }
    //! A new head of market `index`; wakes every watch.
    uint64_t Commit(size_t index = 0)
    {
        const auto sequence{m_markets[index]->Commit()};
        { std::lock_guard lock{m_wake_mutex}; ++m_wakes; }
        m_wake.notify_all();
        return sequence;
    }
    UniValue ClientInfo() const
    {
        UniValue info{UniValue::VOBJ};
        const bool remote{m_markets.front()->options().remote};
        info.pushKV("backend", remote ? "remote" : "local"); info.pushKV("engine_enabled", !remote);
        UniValue endpoints{UniValue::VARR};
        if (remote) {
            const auto& url{m_markets.front()->options().endpoint};
            info.pushKV("selected_endpoint", url); info.pushKV("active_endpoint", url);
            UniValue row{UniValue::VOBJ};
            row.pushKV("url", url); row.pushKV("available", true); row.pushKV("transport_available", true); row.pushKV("last_attempt_ms", 123);
            row.pushKV("retry_after_ms", 0); row.pushKV("consecutive_failures", 0); row.pushKV("last_error", ""); row.pushKV("live_updates_ms", m_live_ms);
            endpoints.push_back(row);
        }
        info.pushKV("endpoints", endpoints);
        return info;
    }

    //! A busy endpoint: every watch finds a new head and answers at once.
    std::atomic_bool busy_watch{false};
    //! Every watch fails (the panel polls and backs off its watch).
    std::atomic_bool fail_watch{false};
    //! Added to every ordinary (non-watch) market read: a slow link.
    std::atomic_int read_delay_ms{0};
    std::atomic_int writes{0};
    mutable std::mutex m_record;
    //! When each passive job, watch and ordinary market read started, and
    //! how long each ordinary read took.
    std::vector<int64_t> job_starts_us, watch_starts_us, read_starts_us, read_us;
    //! When a reply first carried each head of market 0.
    std::map<uint64_t, int64_t> reply_us;

private:
    void Add(const char* method, std::function<UniValue(const JSONRPCRequest&)> run)
    {
        auto command{std::make_unique<CRPCCommand>("hidden", method,
            [run](const JSONRPCRequest& request, UniValue& result, bool) { result = run(request); return true; },
            std::vector<std::pair<std::string, bool>>{}, 997700 + m_commands.size())};
        tableRPC.appendCommand(command->name, command.get());
        m_commands.push_back(std::move(command));
    }
    SyntheticMarket& Find(const JSONRPCRequest& request)
    {
        const auto id{request.params[0].get_str()};
        for (const auto& market : m_markets) if (market->Id() == id) return *market;
        throw std::runtime_error{"Synthetic market not found"};
    }
    UniValue MarketData(const JSONRPCRequest& request)
    {
        auto& market{Find(request)};
        const auto& options{request.params[1]};
        const auto& watch_ms{options.find_value("watch_ms")};
        const bool watch{watch_ms.isNum() && watch_ms.getInt<int64_t>() > 0};
        const bool release{watch_ms.isNum() && !watch};
        const bool remote{market.options().remote};
        UniValue watch_object{UniValue::VOBJ};
        if (watch) {
            { std::lock_guard lock{m_record}; watch_starts_us.push_back(NowUs()); }
            if (fail_watch) throw std::runtime_error{"Synthetic live update unavailable"};
            const auto index{static_cast<size_t>(&market == m_markets[0].get() ? 0 : 1)};
            if (busy_watch) Commit(index);
            // The wake count is read before the head, so a commit after that
            // read always ends the wait (as the local engine's cursor does).
            uint64_t seen;
            { std::lock_guard lock{m_wake_mutex}; seen = m_wakes; }
            const auto& known{options.find_value("known_head")};
            if (known.isStr() && known.get_str() == market.HeadHex()) {
                std::unique_lock lock{m_wake_mutex};
                m_wake.wait_for(lock, std::chrono::milliseconds{watch_ms.getInt<int64_t>()}, [&] { return m_closing || m_wakes != seen; });
            }
        } else if (!release) {
            { std::lock_guard lock{m_record}; read_starts_us.push_back(NowUs()); }
            if (const int delay{read_delay_ms.load()}; delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds{delay});
        }
        const auto started{NowUs()};
        bool unchanged{false};
        auto reply{market.Reply(options, &unchanged)};
        if (!watch && !release) { std::lock_guard lock{m_record}; read_us.push_back(NowUs() - started + int64_t{read_delay_ms.load()} * 1000); }
        if (!unchanged && &market == m_markets[0].get()) {
            std::lock_guard lock{m_record};
            reply_us.emplace(market.HeadSequence(), NowUs());
        }
        if (watch || release) {
            watch_object.pushKV("status", release ? "not_eligible" : unchanged ? "timeout" : "head_changed");
            watch_object.pushKV("supported", !release); watch_object.pushKV("waited", !release);
            watch_object.pushKV("gap", false); watch_object.pushKV("endpoint", remote ? market.options().endpoint : std::string{});
            watch_object.pushKV("endpoint_live_ms", remote ? m_live_ms : 0);
            reply.pushKV("watch", watch_object);
        }
        return reply;
    }

    int m_live_ms;
    std::vector<std::unique_ptr<SyntheticMarket>> m_markets;
    std::vector<std::unique_ptr<CRPCCommand>> m_commands;
    std::mutex m_wake_mutex;
    std::condition_variable m_wake;
    uint64_t m_wakes{0};
    bool m_closing{false};
};

//! Book and chart paints: when, and which head the book showed.
struct PaintLog {
    struct Paint { int64_t us{0}; std::optional<std::pair<uint64_t, unsigned char>> head; };
    std::vector<Paint> book;
    std::vector<int64_t> chart;
    void Attach(B3FlowMeshTradingPanel& panel, B3FlowMeshOrderBook& book_widget, B3FlowMeshChart& chart_widget)
    {
        (void)panel;
        book_widget.setPaintObserver([this](const QString& head) { book.push_back({NowUs(), SequenceOf(head)}); });
        chart_widget.setPaintObserver([this] { chart.push_back(NowUs()); });
    }
};

//! GUI event-loop stalls: a 4 ms precise timer's late fires.
class FrameProbe : public QObject
{
public:
    static constexpr int PERIOD_MS{4};
    FrameProbe()
    {
        m_timer.setTimerType(Qt::PreciseTimer);
        m_timer.setInterval(PERIOD_MS);
        QObject::connect(&m_timer, &QTimer::timeout, this, [this] {
            const int64_t now{NowUs()};
            if (m_last) stalls.push_back(std::max(0.0, Ms(now - *m_last) - PERIOD_MS));
            m_last = now;
        });
    }
    void Start() { m_last.reset(); stalls.clear(); m_timer.start(); }
    void Stop() { m_timer.stop(); }
    std::vector<double> stalls;

private:
    QTimer m_timer;
    std::optional<int64_t> m_last;
};

//! Runs the event loop for `ms`.
void Spin(int ms)
{
    QElapsedTimer spun; spun.start();
    while (spun.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::usleep(200);
    }
}
} // namespace

class B3FlowMeshPerfTests : public QObject
{
    Q_OBJECT
    std::unique_ptr<TestingSetup> m_setup;
    std::unique_ptr<interfaces::Node> m_node;
    std::unique_ptr<interfaces::WalletLoader> m_loader;
    std::unique_ptr<OptionsModel> m_options;
    std::unique_ptr<ClientModel> m_client;
    std::unique_ptr<const PlatformStyle> m_style;
    std::shared_ptr<wallet::CWallet> m_wallet;
    std::unique_ptr<WalletModel> m_model;

    //! The offline wallet, attached through the friend seam: setWalletModel
    //! would start real reads before the fixture is in place.
    void Attach(B3FlowMeshTradingPanel& panel)
    {
        panel.m_timer->stop();
        panel.m_wallet = m_model.get();
        panel.m_backend = interfaces::MakeWallet(*m_loader->context(), m_wallet);
        panel.m_wallet_name = m_model->getDisplayName();
        panel.m_cancel->store(false);
        panel.m_saved_actions_ready = true; // An empty, successfully read local outbox.
    }
    //! The markets of `probe` listed and market 0 displayed from a fresh read,
    //! in the B3 view (the default orientation), as a finished job leaves it.
    static void Show(B3FlowMeshTradingPanel& panel, MarketProbe& probe, size_t markets = 1)
    {
        panel.m_market_data.clear();
        {
            QSignalBlocker blocked{panel.m_market};
            panel.m_market->clear();
            for (size_t i{0}; i < markets; ++i) {
                auto row{B3FlowMeshTrading::ParseMarket(probe.market(i).MarketRow())};
                panel.m_market_data.push_back(row);
                panel.m_market->addItem(QString::fromStdString(probe.market(i).options().ticker) + QStringLiteral(" / B3"), row.id);
            }
            panel.m_market->setCurrentIndex(0);
        }
        UniValue options{UniValue::VOBJ};
        UniValue ticks{UniValue::VOBJ}; ticks.pushKV("limit", static_cast<uint64_t>(MAX_HELD_TICKS)); options.pushKV("ticks", ticks);
        options.pushKV("observed_time", true);
        QElapsedTimer age; age.start();
        panel.m_client_info = probe.ClientInfo();
        panel.m_catalog_age.start();
        panel.m_refresh_phase = B3FlowMeshTradingPanel::RefreshPhase::None;
        panel.applySelectedSnapshot(Parse(probe.market().Reply(options)), age, {});
        UniValue candles{UniValue::VOBJ}; candles.pushKV("interval", flowmesh::CandleIntervalName(panel.m_candle_interval)); candles.pushKV("limit", 300);
        panel.applyCandles(ParseCandles(probe.market().CandlesReply(candles), panel.m_candle_interval), 300);
        panel.updateDataViews();
        panel.updateMarketText();
    }
    //! The next head's reply as a watch against the displayed head brings it.
    static Snapshot NextReply(B3FlowMeshTradingPanel& panel, MarketProbe& probe, bool changed = true)
    {
        if (changed) probe.Commit();
        UniValue options{UniValue::VOBJ};
        options.pushKV("known_head", panel.m_snapshot->head.toStdString());
        UniValue ticks{UniValue::VOBJ};
        if (const auto after{panel.ticksAfter(panel.m_snapshot->market)}) ticks.pushKV("after_sequence", *after);
        ticks.pushKV("limit", static_cast<uint64_t>(MAX_HELD_TICKS));
        options.pushKV("ticks", ticks);
        options.pushKV("observed_time", true);
        options.pushKV("levels", panel.levelsRequest());
        return Parse(probe.market().Reply(options));
    }
    //! A live update's result for the displayed head, as the watch worker
    //! leaves it.
    static std::shared_ptr<B3FlowMeshTradingPanel::WatchResult> WatchReply(B3FlowMeshTradingPanel& panel, Snapshot snapshot)
    {
        auto result{std::make_shared<B3FlowMeshTradingPanel::WatchResult>()};
        result->scope = {panel.m_wallet, panel.m_generation, panel.m_watch_generation, panel.m_snapshot->market, panel.m_snapshot->account, panel.m_snapshot->head};
        const bool unchanged{snapshot.unchanged};
        result->snapshot = std::move(snapshot);
        result->status = unchanged ? QStringLiteral("timeout") : QStringLiteral("head_changed");
        result->supported = result->waited = true;
        return result;
    }
    //! The GUI thread's cost of applying `count` live updates, and of the
    //! paint that follows each.
    struct ApplyCost { std::vector<double> handler, paint; };
    static ApplyCost Apply(B3FlowMeshTradingPanel& panel, MarketProbe& probe, int count, bool changed)
    {
        ApplyCost cost;
        // Only the handler is measured: no further watch is armed.
        panel.m_watch.Disable();
        std::vector<Snapshot> replies;
        for (int i{0}; i < count; ++i) {
            replies.push_back(NextReply(panel, probe, changed));
            // Each reply is against the head it follows, as a watch chain is.
            auto result{WatchReply(panel, replies.back())};
            const int64_t started{NowUs()};
            panel.finishWatch(result);
            cost.handler.push_back(Ms(NowUs() - started));
            const int64_t painting{NowUs()};
            QCoreApplication::processEvents();
            cost.paint.push_back(Ms(NowUs() - painting));
        }
        return cost;
    }

    UniValue Metadata() const
    {
        UniValue m{UniValue::VOBJ};
        m.pushKV("build_type", B3_UI_PERF_BUILD_TYPE);
        m.pushKV("qt_version", qVersion());
        m.pushKV("qpa", QGuiApplication::platformName().toStdString());
        m.pushKV("cpu", CpuName());
        m.pushKV("logical_cores", static_cast<uint64_t>(std::thread::hardware_concurrency()));
        m.pushKV("os", QSysInfo::prettyProductName().toStdString());
        m.pushKV("client_version", FormatFullVersion());
        m.pushKV("source_commit", qEnvironmentVariable("FLOWMESH_UI_PERF_COMMIT", QStringLiteral("unknown")).toStdString());
        m.pushKV("strict", Strict());
        return m;
    }

private Q_SLOTS:
    void initTestCase()
    {
        Q_INIT_RESOURCE(bitcoin);
        m_setup = std::make_unique<TestingSetup>(ChainType::REGTEST, TestOpts{.extra_args = {"-keypool=1"}, .setup_net = false});
        m_node = interfaces::MakeNode(m_setup->m_node);
        m_loader = interfaces::MakeWalletLoader(*m_setup->m_node.chain, *m_setup->m_node.args);
        m_setup->m_node.wallet_loader = m_loader.get();
        m_options = std::make_unique<OptionsModel>(*m_node);
        bilingual_str error;
        QVERIFY2(m_options->Init(error), error.original.c_str());
        m_client = std::make_unique<ClientModel>(*m_node, m_options.get());
        m_style.reset(PlatformStyle::instantiate("other"));
        QVERIFY(m_style);
        m_wallet = std::make_shared<wallet::CWallet>(m_setup->m_node.chain.get(), "offline-perf-test", wallet::CreateMockableWalletDatabase());
        {
            LOCK(m_wallet->cs_wallet);
            m_wallet->SetLastBlockProcessed(0, m_setup->m_node.chain->getBlockHash(0));
            m_wallet->m_keypool_size = 1;
            m_wallet->SetWalletFlag(wallet::WALLET_FLAG_DESCRIPTORS);
            m_wallet->SetupDescriptorScriptPubKeyMans();
        }
        {
            LOCK(m_loader->context()->wallets_mutex);
            m_loader->context()->wallets.push_back(m_wallet);
        }
        m_model = std::make_unique<WalletModel>(interfaces::MakeWallet(*m_loader->context(), m_wallet), *m_client, m_style.get());
        QVERIFY(m_model->setWalletEncrypted(SecureString{"offline perf fixture passphrase"}));
        QVERIFY(m_model->setWalletLocked(true));
        // Flashes are part of the measured paint.
        QApplication::setEffectEnabled(Qt::UI_General, true);
        B3FlowMeshOrderBook::SetAnimationSetting(true);
    }

    //! Worker side, reported only: parsing one changed-head reply.
    void parseCost()
    {
        MarketProbe probe{{SyntheticMarket::Options{}}};
        std::vector<double> costs;
        UniValue options{UniValue::VOBJ};
        UniValue ticks{UniValue::VOBJ}; ticks.pushKV("limit", static_cast<uint64_t>(MAX_HELD_TICKS)); options.pushKV("ticks", ticks);
        options.pushKV("observed_time", true);
        for (int i{0}; i < 100; ++i) {
            probe.Commit();
            ticks.pushKV("after_sequence", probe.market().HeadSequence() - 1); options.pushKV("ticks", ticks);
            const auto reply{probe.market().Reply(options)};
            const int64_t started{NowUs()};
            const auto parsed{Parse(reply)};
            costs.push_back(Ms(NowUs() - started));
            QVERIFY(parsed.levels && parsed.levels->available);
        }
        const auto d{Distribute(costs)};
        qInfo("%s", qPrintable(Describe("parse (worker thread)", d)));
        UniValue v{UniValue::VOBJ}; v.pushKV("parse", Json(d)); v.pushKV("thread", "watch/job worker");
        Record("parse", v);
    }

    //! U2 (handler part) and U1-qt's GUI share: one changed head through the
    //! live-update handler (finishWatch), then its paint.
    void applyChangedHead_data()
    {
        QTest::addColumn<bool>("inverse");
        QTest::newRow("b3-view") << true;
        QTest::newRow("canonical") << false;
    }
    void applyChangedHead()
    {
        QFETCH(bool, inverse);
        MarketProbe probe{{SyntheticMarket::Options{}}};
        B3FlowMeshTradingPanel panel; panel.resize(1320, 940); Attach(panel);
        { QSignalBlocker blocked{panel.m_orientation}; panel.m_orientation->setCurrentIndex(inverse ? 1 : 0); }
        Show(panel, probe);
        panel.show(); Spin(50);
        QVERIFY(panel.m_order_book->showsVerifiedLevels());
        Apply(panel, probe, 10, true); // Warm up.
        const auto cost{Apply(panel, probe, Measuring() ? 200 : 60, true)};
        const auto handler{Distribute(cost.handler)}, paint{Distribute(cost.paint)};
        std::vector<double> total;
        for (size_t i{0}; i < cost.handler.size(); ++i) total.push_back(cost.handler[i] + cost.paint[i]);
        const auto sum{Distribute(total)};
        qInfo("%s", qPrintable(Describe("changed head: handler", handler)));
        qInfo("%s", qPrintable(Describe("changed head: paint", paint)));
        qInfo("%s", qPrintable(Describe("changed head: handler + paint", sum)));
        UniValue v{UniValue::VOBJ};
        v.pushKV("handler", Json(handler)); v.pushKV("paint", Json(paint)); v.pushKV("handler_plus_paint", Json(sum));
        v.pushKV("book_levels_per_side", static_cast<uint64_t>(panel.m_snapshot->levels->bids.levels.size()));
        v.pushKV("curves", static_cast<uint64_t>(panel.m_snapshot->curves.size()));
        v.pushKV("held_ticks", static_cast<uint64_t>(panel.m_ticks.size()));
        v.pushKV("book_rows_per_side", panel.m_order_book->rowsPerSide());
        v.pushKV("target", "U2 handler p95 <= 8 ms; U1-qt handler + paint p95 <= 30 ms");
        Record(std::string{"apply_changed_head_"} + (inverse ? "b3_view" : "canonical"), v);
        QVERIFY2(handler.p95 <= Ceiling(8), qPrintable(Describe("handler", handler)));
        QVERIFY2(sum.p95 <= Ceiling(30), qPrintable(Describe("handler + paint", sum)));
        QCOMPARE(probe.writes.load(), 0);
    }

    //! U4b: an unchanged reply (a quiet watch timeout, a same-head poll).
    void unchangedReply()
    {
        MarketProbe probe{{SyntheticMarket::Options{}}};
        B3FlowMeshTradingPanel panel; panel.resize(1320, 940); Attach(panel);
        Show(panel, probe);
        panel.show(); Spin(50);
        PaintLog paints; paints.Attach(panel, *panel.m_order_book, *panel.m_chart);
        Apply(panel, probe, 5, false);
        paints.book.clear(); paints.chart.clear();
        const auto cost{Apply(panel, probe, Measuring() ? 200 : 60, false)};
        const auto handler{Distribute(cost.handler)};
        qInfo("%s", qPrintable(Describe("unchanged reply: handler", handler)));
        qInfo("unchanged reply: book paints=%zu chart paints=%zu", paints.book.size(), paints.chart.size());
        UniValue v{UniValue::VOBJ};
        v.pushKV("handler", Json(handler)); v.pushKV("paint", Json(Distribute(cost.paint)));
        v.pushKV("book_paints", static_cast<uint64_t>(paints.book.size())); v.pushKV("chart_paints", static_cast<uint64_t>(paints.chart.size()));
        v.pushKV("target", "U4b: <= 2 ms and no book or chart paint");
        Record("unchanged_reply", v);
        QVERIFY2(handler.p95 <= Ceiling(2), qPrintable(Describe("handler", handler)));
        if (Strict()) { QCOMPARE(paints.book.size(), size_t{0}); QCOMPARE(paints.chart.size(), size_t{0}); }
    }

    //! U4: the 500 ms housekeeping tick while live updates supply the data.
    void idleTick_data()
    {
        QTest::addColumn<bool>("marketable");
        QTest::newRow("limit") << false;
        QTest::newRow("marketable") << true;
    }
    void idleTick()
    {
        QFETCH(bool, marketable);
        MarketProbe probe{{SyntheticMarket::Options{}}};
        B3FlowMeshTradingPanel panel; panel.resize(1320, 940); Attach(panel);
        Show(panel, probe);
        panel.m_price->setText(QStringLiteral("1.003")); panel.m_quantity->setText(QStringLiteral("0.25"));
        panel.m_marketable->setChecked(marketable);
        panel.show(); Spin(50);
        PaintLog paints; paints.Attach(panel, *panel.m_order_book, *panel.m_chart);
        // Live, with a watch outstanding: the tick polls nothing.
        panel.updateWatchState();
        panel.m_watch.mode = B3FlowMeshWatch::Mode::Live; panel.m_watch.outstanding = true;
        std::vector<double> ticks;
        for (int i{0}; i < (Measuring() ? 200 : 60); ++i) {
            panel.m_watch.armed_at = panel.m_watch_clock.elapsed(); panel.m_watch.last_ok_at = panel.m_watch.armed_at;
            panel.m_response_age.start(); panel.m_catalog_age.start(); panel.m_candles_age.start();
            const int64_t started{NowUs()};
            panel.tick();
            ticks.push_back(Ms(NowUs() - started));
            QVERIFY(!panel.m_thread); QVERIFY(!panel.m_watch_thread);
            QCoreApplication::processEvents();
            if (i == 4) { paints.book.clear(); paints.chart.clear(); }
        }
        panel.m_watch.outstanding = false; panel.m_watch.Disable();
        const auto d{Distribute(ticks)};
        qInfo("%s", qPrintable(Describe(marketable ? "idle tick (marketable)" : "idle tick", d)));
        qInfo("idle tick: book paints=%zu chart paints=%zu", paints.book.size(), paints.chart.size());
        UniValue v{UniValue::VOBJ};
        v.pushKV("tick", Json(d)); v.pushKV("book_paints", static_cast<uint64_t>(paints.book.size())); v.pushKV("chart_paints", static_cast<uint64_t>(paints.chart.size()));
        v.pushKV("target", "U4: <= 2 ms and no book or chart paint");
        Record(marketable ? "idle_tick_marketable" : "idle_tick", v);
        QVERIFY2(d.p95 <= Ceiling(2), qPrintable(Describe("tick", d)));
        if (Strict()) { QCOMPARE(paints.book.size(), size_t{0}); QCOMPARE(paints.chart.size(), size_t{0}); }
    }

    //! U3: chart hover, wheel zoom and drag pan paints at 1320 x 940.
    void chartInteraction_data()
    {
        QTest::addColumn<bool>("time");
        QTest::newRow("observed-time") << true;
        QTest::newRow("auction-order") << false;
    }
    void chartInteraction()
    {
        QFETCH(bool, time);
        MarketProbe probe{{SyntheticMarket::Options{}}};
        B3FlowMeshTradingPanel panel; panel.resize(1320, 940); Attach(panel);
        panel.m_candle_interval = flowmesh::CandleInterval::M1;
        Show(panel, probe);
        if (!time) {
            // An older server: no times, candles in auction order from the
            // held ticks.
            panel.m_snapshot->observed.reset(); panel.m_candles.reset(); panel.m_tick_times.clear();
            panel.updateDataViews();
        }
        panel.show(); Spin(50);
        auto* chart{panel.m_chart};
        QCOMPARE(chart->timeMode(), time);
        QVERIFY(time ? chart->timeSeries().candles.size() >= 250 : chart->candles().size() >= 40);
        std::vector<double> hover, zoom, pan;
        const QRect area{chart->rect()};
        for (int i{0}; i < (Measuring() ? 400 : 120); ++i) {
            const QPointF at{area.left() + 20.0 + (i * 7) % std::max(1, area.width() - 140), area.top() + 90.0 + (i * 13) % std::max(1, area.height() - 180)};
            QMouseEvent move{QEvent::MouseMove, at, chart->mapToGlobal(at), Qt::NoButton, Qt::NoButton, Qt::NoModifier};
            const int64_t started{NowUs()};
            QCoreApplication::sendEvent(chart, &move);
            chart->repaint();
            hover.push_back(Ms(NowUs() - started));
            if (time && i % 4 == 0) {
                const int64_t zooming{NowUs()};
                chart->zoomTime(i % 8 == 0 ? 1.25 : 0.8, 0.5); chart->repaint();
                zoom.push_back(Ms(NowUs() - zooming));
                const int64_t panning{NowUs()};
                chart->panTime(i % 8 == 0 ? 3 : -3); chart->repaint();
                pan.push_back(Ms(NowUs() - panning));
            }
        }
        const auto h{Distribute(hover)};
        qInfo("%s", qPrintable(Describe(time ? "chart hover (time)" : "chart hover (auction order)", h)));
        UniValue v{UniValue::VOBJ};
        v.pushKV("hover", Json(h));
        if (time) {
            qInfo("%s", qPrintable(Describe("chart zoom", Distribute(zoom))));
            qInfo("%s", qPrintable(Describe("chart pan", Distribute(pan))));
            v.pushKV("zoom", Json(Distribute(zoom))); v.pushKV("pan", Json(Distribute(pan)));
        }
        v.pushKV("chart_size", strprintf("%dx%d", chart->width(), chart->height()));
        v.pushKV("candles", static_cast<uint64_t>(time ? chart->timeSeries().candles.size() : chart->candles().size()));
        v.pushKV("target", "U3: hover, zoom and pan paint p95 <= 8 ms");
        Record(time ? "chart_interaction_time" : "chart_interaction_auction_order", v);
        QVERIFY2(h.p95 <= Ceiling(8), qPrintable(Describe("hover", h)));
        if (time) {
            QVERIFY2(Distribute(zoom).p95 <= Ceiling(8), "zoom");
            QVERIFY2(Distribute(pan).p95 <= Ceiling(8), "pan");
        }
    }

    //! U2: GUI stalls while 20 changed heads a second are applied and the
    //! pointer hovers the chart every 16 ms.
    void sustainedTwentyUpdatesPerSecond()
    {
        MarketProbe probe{{SyntheticMarket::Options{}}};
        B3FlowMeshTradingPanel panel; panel.resize(1320, 940); Attach(panel);
        panel.m_candle_interval = flowmesh::CandleInterval::M1;
        Show(panel, probe);
        panel.show(); Spin(50);
        panel.m_watch.Disable();
        const int seconds{Measuring() ? 10 : 3};
        // Replies are built and parsed ahead, off the measured loop.
        std::deque<Snapshot> pending;
        std::vector<double> handler;
        FrameProbe frames;
        QTimer updates; updates.setTimerType(Qt::PreciseTimer); updates.setInterval(50);
        QTimer hover; hover.setTimerType(Qt::PreciseTimer); hover.setInterval(16);
        int applied{0}, moves{0};
        connect(&updates, &QTimer::timeout, &panel, [&] {
            if (pending.empty()) return;
            auto snapshot{std::move(pending.front())}; pending.pop_front();
            auto result{WatchReply(panel, std::move(snapshot))};
            const int64_t started{NowUs()};
            panel.finishWatch(result);
            handler.push_back(Ms(NowUs() - started));
            ++applied;
        });
        connect(&hover, &QTimer::timeout, &panel, [&] {
            auto* chart{panel.m_chart};
            const QPointF at{30.0 + (moves * 11) % std::max(1, chart->width() - 160), 100.0 + (moves * 5) % std::max(1, chart->height() - 200)};
            QMouseEvent move{QEvent::MouseMove, at, chart->mapToGlobal(at), Qt::NoButton, Qt::NoButton, Qt::NoModifier};
            QCoreApplication::sendEvent(chart, &move);
            ++moves;
        });
        // Build every reply as the chain the displayed heads will follow.
        {
            auto shown{*panel.m_snapshot};
            std::optional<uint64_t> after{panel.ticksAfter(shown.market)};
            for (int i{0}; i < seconds * 20 + 5; ++i) {
                probe.Commit();
                UniValue options{UniValue::VOBJ};
                options.pushKV("known_head", shown.head.toStdString());
                UniValue ticks{UniValue::VOBJ}; if (after) ticks.pushKV("after_sequence", *after); ticks.pushKV("limit", static_cast<uint64_t>(MAX_HELD_TICKS));
                options.pushKV("ticks", ticks); options.pushKV("observed_time", true);
                auto snapshot{Parse(probe.market().Reply(options))};
                after = snapshot.next_sequence - 1; shown = snapshot;
                pending.push_back(std::move(snapshot));
            }
        }
        frames.Start(); updates.start(); hover.start();
        Spin(seconds * 1000);
        updates.stop(); hover.stop(); frames.Stop();
        const auto stall{Distribute(frames.stalls)}, h{Distribute(handler)};
        qInfo("sustained: %d updates, %d hover moves in %d s", applied, moves, seconds);
        qInfo("%s", qPrintable(Describe("sustained: frame stall", stall)));
        qInfo("%s", qPrintable(Describe("sustained: handler", h)));
        UniValue v{UniValue::VOBJ};
        v.pushKV("seconds", seconds); v.pushKV("updates", applied); v.pushKV("hover_moves", moves);
        v.pushKV("frame_stall", Json(stall)); v.pushKV("handler", Json(h));
        v.pushKV("target", "U2: stall p99 <= 16 ms, max <= 50 ms; handler p95 <= 8 ms");
        Record("sustained_20_updates_per_second", v);
        QVERIFY(applied >= seconds * 20 * 8 / 10);
        QVERIFY2(stall.p99 <= Ceiling(16), qPrintable(Describe("stall", stall)));
        QVERIFY2(h.p95 <= Ceiling(8), qPrintable(Describe("handler", h)));
        if (Strict()) QVERIFY2(stall.max <= 50, qPrintable(Describe("stall", stall)));
    }

    //! U1-local and U1-qt: heads committed on a clock reach the painted book
    //! through the panel's own watch loop (timer running, local engine).
    void watchEndToEnd_data()
    {
        QTest::addColumn<int>("period_ms");
        QTest::addColumn<QString>("name");
        QTest::newRow("quiet") << 1200 << QStringLiteral("quiet");
        QTest::newRow("busy-5-per-second") << 200 << QStringLiteral("busy_5");
        QTest::newRow("stress-20-per-second") << 50 << QStringLiteral("stress_20");
    }
    void watchEndToEnd()
    {
        QFETCH(int, period_ms);
        QFETCH(QString, name);
        MarketProbe probe{{SyntheticMarket::Options{}}};
        B3FlowMeshTradingPanel panel; panel.resize(1320, 940); Attach(panel);
        Show(panel, probe);
        PaintLog paints; paints.Attach(panel, *panel.m_order_book, *panel.m_chart);
        panel.show(); Spin(50);
        panel.updateWatchState();
        panel.m_timer->start();
        // Live first.
        QTRY_COMPARE_WITH_TIMEOUT(panel.m_watch.mode, B3FlowMeshWatch::Mode::Live, 5000);
        Spin(300);
        const uint64_t first{probe.market().HeadSequence() + 1};
        paints.book.clear();
        const int seconds{Measuring() ? (period_ms >= 1000 ? 30 : 10) : (period_ms >= 1000 ? 6 : 3)};
        std::atomic_bool stop{false};
        std::thread committer{[&] {
            auto next{Clock::now()};
            while (!stop) {
                next += std::chrono::milliseconds{period_ms};
                std::this_thread::sleep_until(next);
                if (!stop) probe.Commit();
            }
        }};
        Spin(seconds * 1000);
        stop = true; committer.join();
        const uint64_t last{probe.market().HeadSequence()};
        Spin(1500); // The display catches up.
        panel.m_timer->stop();
        panel.m_watch.Disable();
        QTRY_VERIFY_WITH_TIMEOUT(!panel.m_watch_thread && !panel.m_thread, 5000);
        const auto committed{probe.market().Committed()};
        std::map<uint64_t, int64_t> replies;
        { std::lock_guard lock{probe.m_record}; replies = probe.reply_us; }
        std::vector<double> freshness, qt;
        size_t missed{0};
        for (uint64_t h{first}; h <= last; ++h) {
            const auto commit{committed.find(h)};
            if (commit == committed.end()) continue;
            const auto painted{std::find_if(paints.book.begin(), paints.book.end(), [&](const auto& paint) {
                return paint.us >= commit->second && paint.head && paint.head->first >= h;
            })};
            if (painted == paints.book.end()) { ++missed; continue; }
            freshness.push_back(Ms(painted->us - commit->second));
            if (const auto reply{replies.find(h)}; reply != replies.end()) {
                const auto shown{std::find_if(paints.book.begin(), paints.book.end(), [&](const auto& paint) {
                    return paint.us >= reply->second && paint.head && paint.head->first >= h;
                })};
                if (shown != paints.book.end()) qt.push_back(Ms(shown->us - reply->second));
            }
        }
        const auto u1{Distribute(freshness)}, u1_qt{Distribute(qt)};
        qInfo("watch end to end (%s, a head every %d ms): %llu heads, %zu never painted", qPrintable(name), period_ms,
              static_cast<unsigned long long>(last - first + 1), missed);
        qInfo("%s", qPrintable(Describe("U1-local (commit -> painted book)", u1)));
        qInfo("%s", qPrintable(Describe("U1-qt (reply -> painted book)", u1_qt)));
        UniValue v{UniValue::VOBJ};
        v.pushKV("head_period_ms", period_ms); v.pushKV("seconds", seconds); v.pushKV("heads", last - first + 1);
        v.pushKV("never_painted", static_cast<uint64_t>(missed));
        v.pushKV("u1_local", Json(u1)); v.pushKV("u1_qt", Json(u1_qt));
        v.pushKV("book_paints", static_cast<uint64_t>(paints.book.size()));
        v.pushKV("target", period_ms >= 1000 ? "U1-local quiet p95 <= 50 ms, p99 <= 100 ms; U1-qt p95 <= 30 ms" :
                           period_ms >= 200 ? "U1-local busy p95 <= 150 ms (local reads up to 10/s); U1-qt p95 <= 30 ms" :
                                              "stress case: reported; every head superseded by a painted one within 1 s");
        Record("watch_end_to_end_" + name.toStdString(), v);
        QCOMPARE(missed, size_t{0});
        QVERIFY2(u1.max <= Ceiling(1000), qPrintable(Describe("U1-local", u1)));
        QVERIFY2(u1_qt.p95 <= Ceiling(30), qPrintable(Describe("U1-qt", u1_qt)));
        if (period_ms >= 1000) QVERIFY2(u1.p95 <= Ceiling(50), qPrintable(Describe("U1-local quiet", u1)));
        else if (period_ms >= 200) QVERIFY2(u1.p95 <= Ceiling(150), qPrintable(Describe("U1-local busy", u1)));
        QCOMPARE(probe.writes.load(), 0);
    }

    //! A remote endpoint whose every watch finds a new head: live reads
    //! stay within a burst of two and then one per 500 ms (the endpoint's
    //! bound), and start as soon as that allows.
    void remoteWatchRearmSpacing()
    {
        SyntheticMarket::Options options; options.remote = true;
        MarketProbe probe{{options}};
        probe.busy_watch = true;
        B3FlowMeshTradingPanel panel; panel.resize(1320, 940); Attach(panel);
        Show(panel, probe);
        panel.show(); Spin(50);
        panel.updateWatchState();
        panel.m_timer->start();
        QTRY_COMPARE_WITH_TIMEOUT(panel.m_watch.mode, B3FlowMeshWatch::Mode::Live, 5000);
        Spin(Measuring() ? 10'000 : 4'000);
        panel.m_timer->stop(); panel.m_watch.Disable();
        probe.busy_watch = false;
        QTRY_VERIFY_WITH_TIMEOUT(!panel.m_watch_thread && !panel.m_thread, 5000);
        std::vector<int64_t> starts;
        { std::lock_guard lock{probe.m_record}; starts = probe.watch_starts_us; }
        QVERIFY(starts.size() > 6);
        std::vector<double> spacing;
        // Skip the opening burst.
        for (size_t i{3}; i < starts.size(); ++i) spacing.push_back(Ms(starts[i] - starts[i - 1]));
        for (size_t i{2}; i < starts.size(); ++i) QVERIFY2(Ms(starts[i] - starts[i - 2]) >= 500 - 1, "at most a burst of two, then one per 500 ms");
        const auto d{Distribute(spacing)};
        qInfo("%s", qPrintable(Describe("remote busy watch: start spacing", d)));
        UniValue v{UniValue::VOBJ};
        v.pushKV("watch_starts", static_cast<uint64_t>(starts.size())); v.pushKV("spacing", Json(d));
        v.pushKV("target", "never more than a burst of two then one per 500 ms; spacing p95 <= 560 ms");
        Record("remote_watch_rearm_spacing", v);
        if (Strict()) QVERIFY2(d.p95 <= 560, qPrintable(Describe("spacing", d)));
        QCOMPARE(probe.writes.load(), 0);
    }

    //! Critique B1: a remote endpoint without live updates is polled on the
    //! 500 ms tick; a slow link's read never starts back to back, so the
    //! remote client's passive share of its work gate does not rise.
    void remotePollPacing_data()
    {
        QTest::addColumn<int>("delay_ms");
        QTest::newRow("fast-link") << 0;
        QTest::newRow("slow-link-520ms") << 520;
    }
    void remotePollPacing()
    {
        QFETCH(int, delay_ms);
        SyntheticMarket::Options options; options.remote = true;
        MarketProbe probe{{options}, /*live_ms=*/0};
        probe.read_delay_ms = delay_ms;
        B3FlowMeshTradingPanel panel; panel.resize(1320, 940); Attach(panel);
        Show(panel, probe);
        panel.show(); Spin(50);
        panel.updateWatchState();
        QCOMPARE(panel.m_watch.mode, B3FlowMeshWatch::Mode::Polling);
        const int seconds{Measuring() ? 10 : 4};
        const int64_t began{NowUs()};
        panel.m_timer->start();
        Spin(seconds * 1000);
        panel.m_timer->stop();
        QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 5000);
        const int64_t ended{NowUs()};
        std::vector<int64_t> starts, reads;
        { std::lock_guard lock{probe.m_record}; starts = probe.job_starts_us; reads = probe.read_us; }
        QVERIFY(starts.size() >= 3);
        std::vector<double> spacing;
        for (size_t i{1}; i < starts.size(); ++i) spacing.push_back(Ms(starts[i] - starts[i - 1]));
        double busy{0};
        for (const auto us : reads) busy += Ms(us);
        const double occupancy{busy / Ms(ended - began)};
        const auto d{Distribute(spacing)};
        qInfo("%s", qPrintable(Describe(delay_ms ? "remote poll starts (slow link)" : "remote poll starts (fast link)", d)));
        qInfo("remote passive read occupancy: %.3f (%zu reads)", occupancy, reads.size());
        UniValue v{UniValue::VOBJ};
        v.pushKV("read_delay_ms", delay_ms); v.pushKV("jobs", static_cast<uint64_t>(starts.size())); v.pushKV("start_spacing", Json(d));
        v.pushKV("passive_read_occupancy", std::round(occupancy * 1000) / 1000);
        v.pushKV("target", "remote passive polls stay on the 500 ms tick: never two starts within 450 ms (a coarse timer fires up to 5% early)");
        Record(delay_ms ? "remote_poll_pacing_slow_link" : "remote_poll_pacing_fast_link", v);
        for (const double gap : spacing) QVERIFY2(gap >= 450, qPrintable(Describe("spacing", d)));
        // A 520 ms read never starts back to back: the next tick after it.
        if (delay_ms > 500) QVERIFY2(d.p50 >= 900, qPrintable(Describe("spacing", d)));
        QCOMPARE(probe.writes.load(), 0);
    }

    //! U5: a market switch until the new market's book is painted.
    void marketSwitch()
    {
        SyntheticMarket::Options second; second.index = 1; second.base = 9; second.ticker = "tEUR";
        MarketProbe probe{{SyntheticMarket::Options{}, second}};
        B3FlowMeshTradingPanel panel; panel.resize(1320, 940); Attach(panel);
        Show(panel, probe, 2);
        PaintLog paints; paints.Attach(panel, *panel.m_order_book, *panel.m_chart);
        panel.show(); Spin(50);
        panel.updateWatchState();
        panel.m_timer->start();
        QTRY_COMPARE_WITH_TIMEOUT(panel.m_watch.mode, B3FlowMeshWatch::Mode::Live, 5000);
        std::vector<double> switches;
        for (int i{0}; i < (Measuring() ? 20 : 6); ++i) {
            Spin(700);
            const unsigned char target{static_cast<unsigned char>((i + 1) % 2)};
            paints.book.clear();
            const int64_t started{NowUs()};
            panel.m_market->setCurrentIndex(target);
            QTRY_VERIFY_WITH_TIMEOUT(std::any_of(paints.book.begin(), paints.book.end(), [&](const auto& paint) {
                return paint.head && paint.head->second == target;
            }), 5000);
            const auto shown{std::find_if(paints.book.begin(), paints.book.end(), [&](const auto& paint) { return paint.head && paint.head->second == target; })};
            switches.push_back(Ms(shown->us - started));
        }
        panel.m_timer->stop(); panel.m_watch.Disable();
        QTRY_VERIFY_WITH_TIMEOUT(!panel.m_watch_thread && !panel.m_thread, 5000);
        const auto d{Distribute(switches)};
        qInfo("%s", qPrintable(Describe("market switch -> verified book painted (local engine)", d)));
        UniValue v{UniValue::VOBJ};
        v.pushKV("switch_to_verified_paint", Json(d));
        v.pushKV("target", "U5: <= 50 ms to the first paint of the new market (local engine: a fresh verified read)");
        Record("market_switch", v);
        QVERIFY2(d.p95 <= Ceiling(50), qPrintable(Describe("switch", d)));
        QCOMPARE(probe.writes.load(), 0);
    }

    void cleanupTestCase()
    {
        const QString path{qEnvironmentVariable("FLOWMESH_UI_PERF_REPORT")};
        if (!path.isEmpty()) {
            QVERIFY2(QDir::isAbsolutePath(path), "FLOWMESH_UI_PERF_REPORT must be an absolute path");
            UniValue report{UniValue::VOBJ};
            report.pushKV("definition", "flowmesh-ui-responsiveness-v1 (doc/flowmesh-latency-definition.md, U1-U5; provisional targets)");
            report.pushKV("metadata", Metadata());
            report.pushKV("cases", ReportCases());
            QFile file{path};
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            const auto text{report.write(2) + "\n"};
            QVERIFY(file.write(text.data(), static_cast<qint64>(text.size())) == static_cast<qint64>(text.size()));
            qInfo("wrote %s", qPrintable(path));
        }
        m_model.reset();
        {
            LOCK(m_loader->context()->wallets_mutex);
            m_loader->context()->wallets.clear();
        }
        m_wallet.reset(); m_client.reset(); m_options.reset();
        m_setup->m_node.wallet_loader = nullptr;
        m_loader.reset(); m_node.reset(); m_setup.reset();
    }
};

int main(int argc, char** argv)
{
    QApplication app{argc, argv};
    QTemporaryDir settings;
    if (!settings.isValid()) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QCoreApplication::setOrganizationName(QStringLiteral("B3OfflineTests"));
    QCoreApplication::setApplicationName(QStringLiteral("FlowMeshPerf"));
    B3Theme::apply(app);
    B3FlowMeshPerfTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "b3flowmeshperftests.moc"
