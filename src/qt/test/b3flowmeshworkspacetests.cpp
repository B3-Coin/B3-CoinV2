// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/b3flowmeshmarketdata.h>
#include <qt/b3flowmeshchart.h>
#include <qt/b3flowmeshnotices.h>
#include <qt/b3flowmeshorderbook.h>
#include <qt/b3flowmeshtradingpanel.h>
#include <qt/b3flowmeshtradetape.h>
#include <qt/b3theme.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <node/flowmesh_client.h>
#include <node/flowmesh_client_work.h>
#include <rpc/server.h>
#include <qt/clientmodel.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <flowmesh/market.h>
#include <flowmesh/market_view.h>
#include <flowmesh/microblock.h>
#include <test/util/setup_common.h>
#include <util/int128.h>
#include <wallet/context.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDeadlineTimer>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QImage>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPersistentModelIndex>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSemaphore>
#include <QSettings>
#include <QSignalBlocker>
#include <QSignalSpy>
#include <QSplitter>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <vector>

const std::function<void(const std::string&)> G_TEST_LOG_FUN{};
const std::function<std::vector<const char*>()> G_TEST_COMMAND_LINE_ARGUMENTS{};
const std::function<std::string()> G_TEST_GET_FULL_NAME{[] { return "b3_flowmesh_workspace"; }};
using namespace B3FlowMeshMarketData;
namespace {
uint256 H(unsigned char n) { uint256 h; h.begin()[0] = n; return h; }
UniValue CurveJson(const char* side, unsigned char owner = 5)
{
    const bool bid{std::string{side} == "bid"}; UniValue c{UniValue::VOBJ}; c.pushKV("account_id", H(owner).GetHex()); c.pushKV("side", side);
    c.pushKV("filled_quantity", 250'000); c.pushKV("remaining_quantity", 750'000); c.pushKV("reserved_amount", bid ? 750'000'000 : 750'000);
    UniValue points{UniValue::VARR};
    for (const auto [price, quantity] : {std::pair{bid ? 1000 : 999, bid ? 1'000'000 : 0}, std::pair{bid ? 1001 : 1000, bid ? 0 : 1'000'000}}) { UniValue p{UniValue::VOBJ}; p.pushKV("price", price); p.pushKV("quantity", quantity); points.push_back(p); }
    c.pushKV("points", points); return c;
}
UniValue Data(bool trade = true)
{
    UniValue v{UniValue::VOBJ}; const auto market{*flowmesh::ComputeFlowMeshMarketId(H(1), H(2))};
    v.pushKV("market_id", market.GetHex()); v.pushKV("domain", H(1).GetHex()); v.pushKV("base_asset_id", H(2).GetHex()); v.pushKV("execution_config_id", H(3).GetHex());
    v.pushKV("quote_asset", "B3"); v.pushKV("matching_model", "uniform-price-curve-auction"); v.pushKV("quantity_lot_raw", 1); v.pushKV("price_tick_raw", 1); v.pushKV("unchanged", false);
    UniValue snapshot{UniValue::VOBJ}; snapshot.pushKV("certified", true); snapshot.pushKV("next_microblock_sequence", 3); snapshot.pushKV("last_microblock_hash", H(4).GetHex()); snapshot.pushKV("state_root", H(6).GetHex()); snapshot.pushKV("epoch", 0); snapshot.pushKV("active_seats", 4); snapshot.pushKV("quorum_required", 3); snapshot.pushKV("running", true); snapshot.pushKV("paused", false); snapshot.pushKV("pending_handoff", false); snapshot.pushKV("observer_only", true); snapshot.pushKV("halt", "none"); snapshot.pushKV("error", ""); snapshot.pushKV("pending_actions", 0); v.pushKV("snapshot", snapshot);
    UniValue metadata{UniValue::VOBJ}; metadata.pushKV("known", true); metadata.pushKV("decimals", 6); metadata.pushKV("ticker", "tUSD"); metadata.pushKV("name", "Synthetic test dollar"); metadata.pushKV("source", "isolated-test-fixture"); metadata.pushKV("test_only", true); v.pushKV("base_metadata", metadata);
    UniValue liquidity{UniValue::VOBJ}; UniValue curves{UniValue::VARR}; curves.push_back(CurveJson("bid")); curves.push_back(CurveJson("ask", 7)); liquidity.pushKV("curves", curves); liquidity.pushKV("total_curves", 2); liquidity.pushKV("complete", true); v.pushKV("liquidity", liquidity);
    UniValue account{UniValue::VOBJ}; account.pushKV("account_id", H(5).GetHex()); account.pushKV("next_sequence", 8); account.pushKV("base_available", 2'000'000); account.pushKV("base_reserved", 0); account.pushKV("b3_available_atoms", 2'000'000'000); account.pushKV("b3_reserved_atoms", 750'000'000); UniValue own{UniValue::VARR}; own.push_back(CurveJson("bid")); account.pushKV("curves", own); v.pushKV("account", account);
    UniValue history{UniValue::VOBJ}; history.pushKV("available", true); history.pushKV("truncated", false); history.pushKV("scope", "bounded-certified-log-cache"); UniValue entries{UniValue::VARR};
    if (trade) { UniValue t{UniValue::VOBJ}; t.pushKV("sequence", 2); t.pushKV("microblock_hash", H(4).GetHex()); t.pushKV("epoch", 0); t.pushKV("anchor_height", 50); t.pushKV("cleared", true); t.pushKV("price", 1000); t.pushKV("quantity", 250'000); t.pushKV("notional_atoms", 250'000'000); t.pushKV("fee_atoms", 25'000); t.pushKV("account_fills_known", true); t.pushKV("account_bid_fill", 250'000); t.pushKV("account_ask_fill", 0); entries.push_back(t); }
    history.pushKV("entries", entries); v.pushKV("history", history); return v;
}
bool Rejects(const std::function<void()>& f) { try { f(); } catch (const std::exception&) { return true; } catch (const UniValue&) { return true; } return false; }
//! FLOWMESH_UI_PERF_STRICT=1 (as for test_b3_flowmeshperf-qt) also asserts
//! timing-dependent bounds: measured read spacing to the millisecond, how
//! soon a re-armed read starts, animation frame rates. By default only
//! deterministic checks and generous floors run, so a loaded machine running
//! CTest cannot fail them.
bool UiTimingStrict() { return qEnvironmentVariable("FLOWMESH_UI_PERF_STRICT") == QStringLiteral("1"); }
UniValue RemoteData(bool trade = true)
{
    auto v{Data(trade)}; UniValue proof{UniValue::VOBJ}; proof.pushKV("source", "remote_endpoint"); proof.pushKV("endpoint", "https://operator.invalid:18443");
    proof.pushKV("certificate_verified", true); proof.pushKV("account_state_verified", true); proof.pushKV("execution_result_verified", false);
    proof.pushKV("b3_checkpoint_confirmed", false); proof.pushKV("event_gap", false); v.pushKV("verification", proof); return v;
}

UniValue TickJson(uint64_t sequence, const uint256& hash, bool cleared, const char* side = "none", const char* kind = "execution")
{
    UniValue t{UniValue::VOBJ}; t.pushKV("sequence", sequence); t.pushKV("microblock_hash", hash.GetHex()); t.pushKV("kind", kind);
    t.pushKV("cleared", cleared); t.pushKV("price", cleared ? 1000 : 0); t.pushKV("volume", cleared ? 250'000 : 0);
    t.pushKV("imbalance", cleared && std::string{side} != "none" ? 50'000 : 0); t.pushKV("imbalance_side", side);
    return t;
}
UniValue TickPageJson(const std::vector<UniValue>& ticks, bool remote = true)
{
    UniValue page{UniValue::VOBJ}; page.pushKV("available", true); page.pushKV("source", remote ? "endpoint_reported" : "local_engine");
    page.pushKV("gap", false); page.pushKV("more", false); UniValue rows{UniValue::VARR}; for (const auto& t : ticks) rows.push_back(t);
    page.pushKV("ticks", rows); return page;
}
//! A what-if for the offline ticket (buy 1 tUSD at 1 B3), on Data()'s head.
UniValue IndicativeJson(bool draft = true, const uint256& head = H(4))
{
    UniValue v{UniValue::VOBJ}; v.pushKV("available", true); v.pushKV("certified", false); v.pushKV("excludes_pending_actions", true);
    v.pushKV("basis", draft ? "verified_standing_curves_with_draft" : "verified_standing_curves"); v.pushKV("head", head.GetHex());
    if (draft) {
        UniValue d{UniValue::VOBJ}; d.pushKV("side", "bid"); d.pushKV("price", 1000); d.pushKV("quantity", 1'000'000);
        d.pushKV("account_known", true); d.pushKV("replaces_standing_curve", true); d.pushKV("assumes_admission", true); v.pushKV("draft", d);
    }
    v.pushKV("cleared", draft); v.pushKV("price", draft ? 1000 : 0); v.pushKV("volume", draft ? 250'000 : 0); v.pushKV("imbalance", draft ? 750'000 : 0);
    v.pushKV("imbalance_side", draft ? "bid" : "none"); v.pushKV("best_bid", 1000); v.pushKV("best_ask", 1001);
    if (draft) { v.pushKV("draft_fill_min", 187'500); v.pushKV("draft_fill_max", 187'500); }
    return v;
}
UniValue RemoteDataWithViews()
{
    auto v{RemoteData()};
    v.pushKV("clearing_ticks", TickPageJson({TickJson(1, H(12), false), TickJson(2, H(4), true, "bid")}));
    v.pushKV("indicative", IndicativeJson());
    return v;
}
// 2026-09-30T12:00:00Z
constexpr int64_t OBSERVED_T0{1790769600000};
UniValue StampJson(uint64_t sequence, const uint256& hash, const char* mode, int64_t ms, const std::vector<std::string>& flags = {})
{
    UniValue row{UniValue::VOBJ}; row.pushKV("sequence", sequence); row.pushKV("microblock_hash", hash.GetHex()); row.pushKV("mode", mode);
    if (std::string{mode} == "live") row.pushKV("observed_ms", ms);
    if (std::string{mode} == "delayed") row.pushKV("applied_here_ms", ms);
    UniValue names{UniValue::VARR}; for (const auto& flag : flags) names.push_back(flag); row.pushKV("flags", names);
    return row;
}
UniValue ObservedJson(const std::vector<UniValue>& rows, const std::string& source = "https://operator.invalid:18443")
{
    UniValue v{UniValue::VOBJ}; v.pushKV("available", true); v.pushKV("source", source); v.pushKV("source_kind", "operator_observed");
    v.pushKV("certified", false); v.pushKV("clock_suspect", false); v.pushKV("recorded_since_sequence", 0);
    UniValue array{UniValue::VARR}; for (const auto& row : rows) array.push_back(row); v.pushKV("rows", array);
    return v;
}
//! RemoteDataWithViews with the endpoint's times of both ticks.
UniValue RemoteDataWithTimes()
{
    auto v{RemoteDataWithViews()};
    v.pushKV("observed_time", ObservedJson({StampJson(1, H(12), "live", OBSERVED_T0 + 1500), StampJson(2, H(4), "live", OBSERVED_T0 + 61'000)}));
    return v;
}
UniValue CandleJson(int64_t open_ms, uint64_t first, uint64_t last, CAmount open, CAmount high, CAmount low, CAmount close, uint64_t volume,
                    const std::vector<std::string>& flags = {})
{
    UniValue c{UniValue::VOBJ}; c.pushKV("open_ms", open_ms); c.pushKV("first_sequence", first); c.pushKV("last_sequence", last);
    c.pushKV("entries", last - first + 1); c.pushKV("clearings", volume ? last - first + 1 : 0);
    if (volume) { c.pushKV("open", open); c.pushKV("high", high); c.pushKV("low", low); c.pushKV("close", close); }
    c.pushKV("base_volume", std::to_string(volume)); c.pushKV("quote_volume", std::to_string(volume * static_cast<uint64_t>(close)));
    UniValue names{UniValue::VARR}; for (const auto& flag : flags) names.push_back(flag); c.pushKV("flags", names);
    return c;
}
UniValue CandlesJson(const char* interval, int64_t interval_ms, int64_t before_ms, int64_t span, const std::vector<UniValue>& candles,
                     const std::string& source = "https://operator.invalid:18443", bool remote = true)
{
    UniValue v{UniValue::VOBJ}; v.pushKV("available", true); v.pushKV("source", source); v.pushKV("source_kind", "operator_observed");
    v.pushKV("certified", false); v.pushKV("interval", interval); v.pushKV("interval_ms", interval_ms);
    v.pushKV("from_ms", before_ms - span * interval_ms); v.pushKV("before_ms", before_ms); v.pushKV("more", false);
    UniValue array{UniValue::VARR}; for (const auto& c : candles) array.push_back(c); v.pushKV("candles", array);
    v.pushKV("endpoint_reported", remote);
    return v;
}

Curve LimitCurve(const char* side, unsigned char owner, CAmount price, CAmount quantity, CAmount filled = 0)
{
    Curve curve;
    curve.account = QString::fromStdString(H(owner).GetHex()); curve.side = QLatin1String(side);
    const bool bid{curve.side == QStringLiteral("bid")};
    const auto points{bid ? flowmesh::MakeLimitBidCurve(price, quantity) : flowmesh::MakeLimitAskCurve(price, quantity)};
    if (!points) throw std::runtime_error{"Invalid synthetic limit-curve fixture"};
    for (const auto& point : *points) curve.points.push_back({point.price, point.qty});
    curve.filled = filled; curve.remaining = quantity - filled;
    curve.reserved = bid ? Notional(price, curve.remaining).value_or(0) : curve.remaining;
    return curve;
}

Snapshot SyntheticLimitBook(size_t levels = 8)
{
    // Display-only generated values. No node, wallet or public market feed.
    auto snapshot{Parse(Data())};
    snapshot.curves.clear(); snapshot.own_curves.clear();
    for (size_t i{0}; i < levels; ++i) {
        const CAmount quantity{100'000 + static_cast<CAmount>(i) * 25'000};
        const CAmount filled{static_cast<CAmount>(i % 3) * 5'000};
        snapshot.curves.push_back(LimitCurve("bid", static_cast<unsigned char>(20 + i), 1000 - 2 * static_cast<CAmount>(i), quantity, filled));
        snapshot.curves.push_back(LimitCurve("ask", static_cast<unsigned char>(80 + i), 1010 + 2 * static_cast<CAmount>(i), quantity + 50'000, filled));
    }
    snapshot.depth = Aggregate(snapshot.curves);
    return snapshot;
}

//! The node's "book_levels" of `curves` at `head`, as the local RPC projects
//! them: the engine's own aggregation, never a Qt reimplementation.
UniValue LevelsJson(const std::vector<Curve>& curves, const uint256& head, const flowmesh::LevelGrouping& grouping = {},
                    const std::optional<uint256>& own = std::nullopt, size_t depth = flowmesh::MARKET_DATA_MAX_LEVELS)
{
    std::vector<flowmesh::ClearingEngine::CurveView> views;
    for (const auto& curve : curves) {
        flowmesh::ClearingEngine::CurveView view;
        view.account_id = *uint256::FromHex(curve.account.toStdString());
        view.side = curve.side == QStringLiteral("bid") ? flowmesh::ClearingEngine::Side::BID : flowmesh::ClearingEngine::Side::ASK;
        for (const auto& point : curve.points) view.points.push_back({point.price, point.quantity});
        view.filled_quantity = curve.filled; view.remaining_quantity = curve.remaining; view.reserved_amount = curve.reserved;
        views.push_back(std::move(view));
    }
    flowmesh::BookLevelsQuery query;
    query.depth = depth; query.grouping = grouping; query.account = own;
    auto levels{flowmesh::ComputeBookLevels(views, query)};
    levels.head = head; levels.standing.head = head;
    return node::FlowMeshBookLevelsJson(levels);
}
flowmesh::LevelGrouping Grouping(bool inverse, uint8_t mantissa, int32_t exponent, uint8_t decimals)
{
    flowmesh::LevelGrouping grouping; grouping.exact = false; grouping.inverse = inverse;
    grouping.mantissa = mantissa; grouping.exponent = exponent; grouping.decimals = decimals;
    return grouping;
}

bool SaveOrderBookGallery(const Snapshot& snapshot, bool inverse, const QString& directory, const QString& filename, const QString& heading)
{
    QWidget view;
    auto* layout{new QVBoxLayout{&view}};
    auto* title{new QLabel{heading, &view}}; title->setTextFormat(Qt::PlainText); title->setWordWrap(true);
    B3Theme::markTextRole(title, QStringLiteral("h3")); layout->addWidget(title);
    auto* row{new QHBoxLayout}; layout->addLayout(row, 1);
    auto* chart{new B3FlowMeshChart{&view}}; chart->setSnapshot(snapshot); chart->setInverted(inverse); row->addWidget(chart, 1);
    auto* book{new B3FlowMeshOrderBook{&view}}; book->setFixedWidth(420); book->setSnapshot(snapshot, inverse); row->addWidget(book);
    view.resize(1400, 850); view.show(); QCoreApplication::processEvents();
    QImage image{view.size(), QImage::Format_ARGB32}; image.fill(Qt::transparent); view.render(&image);
    return image.save(QDir{directory}.filePath(filename));
}

// Intercept only the existing read-only RPC in an isolated Qt unit process.
// No remote endpoint, wallet outbox or signing path is substituted or opened.
struct StatusReadProbe {
    QSemaphore entered, release;
    std::atomic_int count{0};
    std::atomic_bool fail{false}, wrong_account{false};
    std::mutex mutex;
    std::vector<std::pair<std::string, UniValue>> requests;
    CRPCCommand command;
    StatusReadProbe() : command{"hidden", "getflowmeshactionstatus",
        [this](const JSONRPCRequest& request, UniValue& result, bool) {
            { std::lock_guard lock{mutex}; requests.emplace_back(request.URI, request.params); }
            ++count; entered.release();
            if (!release.tryAcquire(1, 2000)) throw std::runtime_error{"Synthetic status request exceeded its test bound"};
            if (fail) throw std::runtime_error{"Synthetic status endpoint unavailable"};
            result = UniValue{UniValue::VOBJ};
            result.pushKV("market_id", request.params[0].get_str()); result.pushKV("action_id", request.params[1].get_str());
            result.pushKV("accepted", false); // Existing legacy status format; account_id is optional.
            if (wrong_account) result.pushKV("account_id", H(111).GetHex());
            return true;
        }, {}, 998877} {
        if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
        tableRPC.appendCommand(command.name, &command);
    }
    ~StatusReadProbe() { tableRPC.removeCommand(command.name, &command); }
};

// The post-submit waited status read, intercepted like StatusReadProbe. A
// success is a certified-inclusion receipt; nothing is signed or resent.
struct WaitedReceiptProbe {
    QSemaphore entered, release;
    std::atomic_int count{0};
    std::atomic_bool fail{false};
    std::mutex mutex;
    std::vector<UniValue> params;
    CRPCCommand command;
    WaitedReceiptProbe() : command{"hidden", "getflowmeshactionstatus",
        [this](const JSONRPCRequest& request, UniValue& result, bool) {
            { std::lock_guard lock{mutex}; params.push_back(request.params); }
            ++count; entered.release();
            if (!release.tryAcquire(1, 2000)) throw std::runtime_error{"Synthetic waited status exceeded its test bound"};
            if (fail) throw std::runtime_error{"Synthetic waited status endpoint unavailable"};
            result = UniValue{UniValue::VOBJ};
            result.pushKV("market_id", request.params[0].get_str()); result.pushKV("action_id", request.params[1].get_str());
            result.pushKV("accepted", true); result.pushKV("receipt_state", "certified_inclusion");
            result.pushKV("reason", "Canonical action inclusion verified"); result.pushKV("endpoint", "https://operator.invalid:18443");
            result.pushKV("certificate_verified", true); result.pushKV("outcome_verified", false);
            result.pushKV("microblock_hash", H(97).GetHex()); result.pushKV("microblock_sequence", 5);
            return true;
        }, {}, 998866} {
        if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
        tableRPC.appendCommand(command.name, &command);
    }
    ~WaitedReceiptProbe() { tableRPC.removeCommand(command.name, &command); }
};

UniValue ConnectionInfo(const std::string& url = "https://trading.invalid", bool attempted = true, bool transport = true, bool available = true)
{
    UniValue info{UniValue::VOBJ}; info.pushKV("backend", "remote"); info.pushKV("engine_enabled", false);
    info.pushKV("selected_endpoint", url); info.pushKV("active_endpoint", available ? url : "");
    UniValue endpoints{UniValue::VARR};
    if (!url.empty()) {
        UniValue row{UniValue::VOBJ}; row.pushKV("url", url); row.pushKV("available", available);
        row.pushKV("transport_available", transport); row.pushKV("last_attempt_ms", attempted ? 123 : 0);
        row.pushKV("retry_after_ms", 0); row.pushKV("consecutive_failures", transport ? 0 : 1);
        row.pushKV("last_error", available ? "" : transport ? "FlowMesh client snapshot has no certified head" : "Synthetic TLS connection failed");
        endpoints.push_back(row);
    }
    info.pushKV("endpoints", endpoints); return info;
}

// Exercise the actual worker and wallet-scoped dispatch, with only the existing
// non-economic RPCs intercepted. No sockets, signing, or live wallet are used.
struct ConnectionProbe {
    QSemaphore entered, release;
    std::atomic_int count{0};
    std::atomic_bool fail{false};
    std::mutex mutex;
    std::vector<std::pair<std::string, std::string>> requests;
    std::vector<std::unique_ptr<CRPCCommand>> commands;
    ConnectionProbe()
    {
        if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
        const auto add = [&](const char* method, const std::function<UniValue(const JSONRPCRequest&)>& run) {
            auto command{std::make_unique<CRPCCommand>("hidden", method,
                [run](const JSONRPCRequest& request, UniValue& result, bool) { result = run(request); return true; },
                std::vector<std::pair<std::string, bool>>{}, 998880 + commands.size())};
            tableRPC.appendCommand(command->name, command.get()); commands.push_back(std::move(command));
        };
        add("flowmeshclientconnect", [this](const JSONRPCRequest& request) {
            const auto url{request.params[0].get_str()};
            { std::lock_guard lock{mutex}; requests.emplace_back(request.URI, url); }
            ++count; entered.release();
            if (!release.tryAcquire(1, 2000)) throw std::runtime_error{"Synthetic connection exceeded its test bound"};
            if (fail) throw std::runtime_error{"Synthetic endpoint configuration rejected"};
            return ConnectionInfo(url);
        });
        add("getflowmeshclientinfo", [](const JSONRPCRequest&) { return ConnectionInfo(); });
        add("listflowmeshactions", [](const JSONRPCRequest&) {
            UniValue saved{UniValue::VOBJ}; saved.pushKV("source", "local-retained-outbox"); saved.pushKV("actions", UniValue{UniValue::VARR}); return saved;
        });
        add("listflowmeshmarkets", [](const JSONRPCRequest&) -> UniValue { throw std::runtime_error{"Synthetic selected-market read unavailable"}; });
    }
    ~ConnectionProbe() { for (const auto& command : commands) tableRPC.removeCommand(command->name, command.get()); }
};

// A bounded passive funding refresh. Mutation RPCs are counted and rejected;
// successful cases stop at the first ordinary review, without wallet unlock.
struct FundingReadProbe {
    UniValue data{RemoteData()};
    UniValue saved_response;
    // A status reply for getflowmeshactionstatus; none: the read fails.
    UniValue receipt_response;
    bool fail{false}, hold{false}, effects_misc_error{false}, discovery_unproven{false}, checkpoint{false};
    std::string failed_method, held_method;
    std::vector<std::string> methods;
    // Written only by the single worker, inspected only after that worker is
    // drained. Priority belongs to the request context, not the RPC method.
    std::vector<std::pair<std::string, node::FlowMeshClientWorkPriority>> priorities;
    // Calls whose release would leave the backend gate lingering. The panel's
    // worker is the only passive caller, so its own jobs must never linger.
    std::atomic_int lingering_calls{0};
    QSemaphore auxiliary_entered, auxiliary_release;
    QSemaphore entered, release;
    std::atomic_int snapshots{0}, catalogs{0}, balances{0}, effects{0}, receipts{0}, writes{0};
    // Live-update reads (getflowmeshmarketdata with watch_ms): their reply
    // data and watch object, how many arrived, an optional hold, and what
    // getflowmeshclientinfo advertises for the data's endpoint.
    UniValue watch_data{RemoteData()};
    UniValue watch_object{UniValue::VOBJ};
    std::atomic_int watches{0}, releases{0};
    //! When each live-update read arrived (steady milliseconds).
    std::mutex watch_mutex;
    std::vector<int64_t> watch_times;
    std::atomic_bool hold_watch{false}, fail_watch{false};
    QSemaphore watch_entered, watch_release;
    //! A busy validator, as the remote client sees it: a live-update read
    //! waits 1.2 s (no event of its own market and account) and is answered
    //! with superseded_object (after an ordinary read) when an ordinary market
    //! read ran meanwhile, since any read then moves the client's event cursor
    //! under the outstanding watch.
    std::atomic_bool supersede_on_concurrent_read{false};
    UniValue superseded_object{UniValue::VOBJ};
    std::atomic_int superseded{0};
    int live_ms{0};
    //! Every getflowmeshmarketdata options object, in order.
    std::vector<UniValue> market_options;
    //! getflowmeshcandles: its reply and every options object, in order.
    UniValue candles_response;
    std::vector<UniValue> candle_options;
    std::vector<std::unique_ptr<CRPCCommand>> commands;
    FundingReadProbe()
    {
        if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
        const auto add = [&](const char* method, const std::function<UniValue()>& run) {
            auto command{std::make_unique<CRPCCommand>("hidden", method,
                [this, method, run](const JSONRPCRequest&, UniValue& result, bool) {
                    methods.emplace_back(method);
                    priorities.emplace_back(method, node::FlowMeshClientWorkScope::Current());
                    if (node::FlowMeshClientWorkScope::LingersAfterRelease()) ++lingering_calls;
                    if (held_method == method) { auxiliary_entered.release(); if (!auxiliary_release.tryAcquire(1, 2000)) throw std::runtime_error{"Synthetic auxiliary exceeded its test bound"}; }
                    if (failed_method == method) throw std::runtime_error{"Synthetic auxiliary failed"};
                    result = run(); return true;
                },
                std::vector<std::pair<std::string, bool>>{}, 998900 + commands.size())};
            tableRPC.appendCommand(command->name, command.get()); commands.push_back(std::move(command));
        };
        const auto market = [this] {
            const auto s{Parse(data)}; UniValue row{UniValue::VOBJ};
            for (const auto* key : {"market_id", "base_asset_id", "domain", "execution_config_id", "quote_asset", "verification"}) row.pushKV(key, data[key]);
            row.pushKV("vault_id", flowmesh::ComputeFlowMeshVaultId(H(1), *uint256::FromHex(s.market.toStdString()))->GetHex());
            row.pushKV("available", true); row.pushKV("checkpoint_pending", checkpoint);
            if (checkpoint) row.pushKV("pending_checkpoint_id", H(80).GetHex());
            for (const auto* key : {"running", "paused", "pending_handoff", "halt", "error"}) row.pushKV(key, data["snapshot"][key]);
            auto account{data["account"]}; account.pushKV("b3_available", FormatAmount(s.b3_available, 9).toStdString()); account.pushKV("b3_reserved", FormatAmount(s.b3_reserved, 9).toStdString()); row.pushKV("account", account);
            return row;
        };
        add("getflowmeshclientinfo", [this] {
            auto info{ConnectionInfo("https://operator.invalid:18443")};
            UniValue rows{UniValue::VARR};
            for (auto row : info["endpoints"].getValues()) { row.pushKV("live_updates_ms", live_ms); rows.push_back(row); }
            info.pushKV("endpoints", rows);
            return info;
        });
        add("listflowmeshactions", [this] {
            if (saved_response.isObject()) return saved_response;
            UniValue saved{UniValue::VOBJ}; saved.pushKV("source", "local-retained-outbox"); saved.pushKV("actions", UniValue{UniValue::VARR}); return saved;
        });
        add("listflowmeshmarkets", [this, market] {
            ++catalogs; auto row{market()};
            if (discovery_unproven) {
                row.pushKV("account", UniValue{});
                auto proof{row["verification"]}; proof.pushKV("certificate_verified", false); proof.pushKV("account_state_verified", false); row.pushKV("verification", proof);
            }
            UniValue rows{UniValue::VARR}; rows.push_back(row); return rows;
        });
        add("getflowmeshbalance", [this, market] { ++balances; return market(); });
        {
            auto command{std::make_unique<CRPCCommand>("hidden", "getflowmeshmarketdata",
                [this](const JSONRPCRequest& request, UniValue& result, bool) {
                    const auto& options{request.params[1]};
                    market_options.push_back(options);
                    if (options.isObject() && options.exists("watch_ms")) {
                        if (options["watch_ms"].getInt<int>() == 0) { ++releases; result = data; result.pushKV("watch", watch_object); return true; }
                        { std::lock_guard lock{watch_mutex}; watch_times.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
                        ++watches; watch_entered.release();
                        if (supersede_on_concurrent_read) {
                            const int before{snapshots.load()};
                            std::this_thread::sleep_for(std::chrono::milliseconds{1200});
                            if (snapshots.load() != before) { ++superseded; result = data; result.pushKV("watch", superseded_object); return true; }
                        }
                        if (hold_watch && !watch_release.tryAcquire(1, 3000)) throw std::runtime_error{"Synthetic watch exceeded its test bound"};
                        if (fail_watch) throw std::runtime_error{"Synthetic live update unavailable"};
                        result = watch_data; result.pushKV("watch", watch_object); return true;
                    }
                    methods.emplace_back("getflowmeshmarketdata");
                    priorities.emplace_back("getflowmeshmarketdata", node::FlowMeshClientWorkScope::Current());
                    if (node::FlowMeshClientWorkScope::LingersAfterRelease()) ++lingering_calls;
                    if (held_method == "getflowmeshmarketdata") { auxiliary_entered.release(); if (!auxiliary_release.tryAcquire(1, 2000)) throw std::runtime_error{"Synthetic auxiliary exceeded its test bound"}; }
                    if (failed_method == "getflowmeshmarketdata") throw std::runtime_error{"Synthetic auxiliary failed"};
                    ++snapshots; entered.release();
                    if (hold && !release.tryAcquire(1, 2000)) throw std::runtime_error{"Synthetic funding read exceeded its test bound"};
                    if (fail) throw std::runtime_error{"Synthetic funding refresh unavailable"};
                    result = data; return true;
                },
                std::vector<std::pair<std::string, bool>>{}, 998900 + commands.size())};
            tableRPC.appendCommand(command->name, command.get()); commands.push_back(std::move(command));
        }
        {
            auto command{std::make_unique<CRPCCommand>("hidden", "getflowmeshcandles",
                [this](const JSONRPCRequest& request, UniValue& result, bool) {
                    methods.emplace_back("getflowmeshcandles");
                    candle_options.push_back(request.params[1]);
                    if (!candles_response.isObject()) throw std::runtime_error{"Synthetic candles unavailable"};
                    result = candles_response; return true;
                },
                std::vector<std::pair<std::string, bool>>{}, 998900 + commands.size())};
            tableRPC.appendCommand(command->name, command.get()); commands.push_back(std::move(command));
        }
        add("listflowmeshvaultoperations", [this] {
            ++effects;
            if (effects_misc_error) { UniValue error{UniValue::VOBJ}; error.pushKV("code", -1); error.pushKV("message", "Synthetic reconciliation race"); throw error; }
            return UniValue{UniValue::VARR};
        });
        add("getflowmeshactionstatus", [this]() -> UniValue {
            ++receipts;
            if (receipt_response.isObject()) return receipt_response;
            throw std::runtime_error{"Synthetic saved status unavailable"};
        });
        for (const auto* method : {"flowmeshdeposit", "submitflowmeshdeposit", "submitflowmeshorder", "cancelflowmeshorder", "requestflowmeshwithdrawal", "sendrawtransaction", "createflowmeshaccount", "retryflowmeshaction"})
            add(method, [this]() -> UniValue { ++writes; throw std::runtime_error{"Unexpected mutation in draft test"}; });
    }
    ~FundingReadProbe() { for (const auto& command : commands) tableRPC.removeCommand(command->name, command.get()); }
};
}

class B3FlowMeshWorkspaceTests : public QObject
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

    void AttachOfflineWallet(B3FlowMeshTradingPanel& panel, WalletModel& model,
                             const std::shared_ptr<wallet::CWallet>& wallet)
    {
        // Assign the isolated model directly through the existing friend test
        // seam: public setWalletModel intentionally starts a real RPC refresh.
        panel.m_timer->stop();
        panel.m_wallet = &model;
        panel.m_backend = interfaces::MakeWallet(*m_loader->context(), wallet);
        panel.m_wallet_name = model.getDisplayName();
        panel.m_cancel->store(false);
        panel.m_saved_actions_ready = true; // This offline seam models an empty, successfully read local outbox.
        Observe(panel, Parse(Data()));
        panel.m_price->setText(QStringLiteral("1"));
        panel.m_quantity->setText(QStringLiteral("1"));
    }

    void AttachOfflineWallet(B3FlowMeshTradingPanel& panel) { AttachOfflineWallet(panel, *m_model, m_wallet); }

    struct OfflineWallet {
        std::shared_ptr<wallet::CWallet> wallet;
        std::unique_ptr<WalletModel> model;
    };

    OfflineWallet MakeOfflineWallet(const std::string& name)
    {
        OfflineWallet result;
        result.wallet = std::make_shared<wallet::CWallet>(m_setup->m_node.chain.get(), name, wallet::CreateMockableWalletDatabase());
        {
            LOCK(result.wallet->cs_wallet);
            result.wallet->SetLastBlockProcessed(0, m_setup->m_node.chain->getBlockHash(0));
        }
        result.model = std::make_unique<WalletModel>(
            interfaces::MakeWallet(*m_loader->context(), result.wallet), *m_client, m_style.get());
        // Deliberately not registered with the loader: these models exercise
        // identity/lifetime, never wallet discovery or an automatic RPC refresh.
        return result;
    }

    static void ReadInFlight(B3FlowMeshTradingPanel& panel)
    {
        // A never-started, owned thread models the in-flight read marker. No
        // RPC, sockets or clock sleeps are needed to exercise GUI scheduling.
        panel.m_thread = new QThread{&panel};
        panel.m_busy = false;
        panel.updateControls();
    }

    static B3FlowMeshTrading::SavedActions SavedReadFixture(B3FlowMeshTradingPanel& panel)
    {
        B3FlowMeshTrading::SavedActions saved;
        saved.account = panel.m_snapshot->account;
        B3FlowMeshTrading::SavedAction action;
        action.market = panel.m_snapshot->market; action.domain = panel.m_snapshot->domain;
        action.config = panel.m_snapshot->config; action.account = saved.account;
        action.sequence = 7; action.type = 0; action.signed_bytes_size = 279;
        action.initial_submission_ms = 123; action.may_have_been_sent = true;
        action.signed_bytes_sha256 = QString::fromStdString(H(94).GetHex());
        action.receipt.action_id = QString::fromStdString(H(95).GetHex());
        action.receipt.market = action.market; action.receipt.account = action.account;
        action.receipt.state = QStringLiteral("unknown"); action.receipt.no_resubmit = true;
        saved.actions = {action}; panel.restoreSavedActions(saved);
        return saved;
    }

    //! A newer certified head than Data(): the reply a live update brings.
    static UniValue NewerRemoteData()
    {
        auto data{RemoteData()}; auto snapshot{data["snapshot"]};
        snapshot.pushKV("next_microblock_sequence", 4); snapshot.pushKV("last_microblock_hash", H(10).GetHex()); data.pushKV("snapshot", snapshot);
        auto account{data["account"]}; account.pushKV("next_sequence", 9); data.pushKV("account", account);
        return data;
    }
    static UniValue WatchObject(const char* status, bool supported, bool waited, int live_ms = 2000, bool gap = false)
    {
        UniValue watch{UniValue::VOBJ}; watch.pushKV("status", status); watch.pushKV("supported", supported); watch.pushKV("waited", waited);
        watch.pushKV("gap", gap); watch.pushKV("endpoint", "https://operator.invalid:18443"); watch.pushKV("endpoint_live_ms", live_ms);
        return watch;
    }
    //! The remote panel as the timer leaves it once the client info is read.
    static void Live(B3FlowMeshTradingPanel& panel, FundingReadProbe& probe)
    {
        panel.show(); QCoreApplication::processEvents();
        auto info{ConnectionInfo("https://operator.invalid:18443")};
        UniValue rows{UniValue::VARR};
        for (auto row : info["endpoints"].getValues()) { row.pushKV("live_updates_ms", probe.live_ms); rows.push_back(row); }
        info.pushKV("endpoints", rows);
        panel.m_client_info = info;
        panel.updateWatchState();
    }
    static void AwaitWatchWorker(B3FlowMeshTradingPanel& panel)
    {
        QTRY_VERIFY_WITH_TIMEOUT(!panel.m_watch_thread, 5000);
    }

    //! Tracks an unresolved request of the observed account, as after a submission.
    static void TrackUnknown(B3FlowMeshTradingPanel& panel, unsigned char id = 42)
    {
        B3FlowMeshTrading::Receipt receipt; receipt.action_id = QString::fromStdString(H(id).GetHex()); receipt.state = QStringLiteral("unknown");
        receipt.market = panel.m_snapshot->market; receipt.account = panel.m_snapshot->account;
        panel.m_receipt = receipt; panel.m_receipt_wallet = panel.m_wallet;
        panel.m_pending_market = panel.m_snapshot->market; panel.m_pending_account = panel.m_snapshot->account;
    }
    //! A verified certified-inclusion status of that request.
    static UniValue CertifiedStatus(const QString& market, unsigned char id = 42)
    {
        UniValue r{UniValue::VOBJ};
        r.pushKV("market_id", market.toStdString()); r.pushKV("action_id", H(id).GetHex());
        r.pushKV("accepted", true); r.pushKV("receipt_state", "certified_inclusion"); r.pushKV("reason", "Canonical action inclusion verified");
        r.pushKV("endpoint", "https://operator.invalid:18443"); r.pushKV("certificate_verified", true); r.pushKV("outcome_verified", false);
        r.pushKV("microblock_hash", H(97).GetHex()); r.pushKV("microblock_sequence", 5); r.pushKV("stage", "certified");
        return r;
    }
    //! A retained limit bid of the observed account at 1 B3/tUSD for 1 tUSD.
    static B3FlowMeshTrading::SavedAction SavedBid(const B3FlowMeshTradingPanel& panel, unsigned char id, const char* state, const char* stage, uint64_t submitted_ms = 200)
    {
        B3FlowMeshTrading::SavedAction action;
        action.market = panel.m_snapshot->market; action.domain = panel.m_snapshot->domain;
        action.config = panel.m_snapshot->config; action.account = panel.m_snapshot->account;
        action.sequence = 8; action.type = 0; action.signed_bytes_size = 279; action.initial_submission_ms = submitted_ms;
        action.may_have_been_sent = true; action.signed_bytes_sha256 = QString::fromStdString(H(static_cast<unsigned char>(id + 1)).GetHex());
        action.canonical_side = QStringLiteral("bid"); action.canonical_points = {{1000, 1'000'000}, {1001, 0}};
        action.receipt.action_id = QString::fromStdString(H(id).GetHex());
        action.receipt.market = action.market; action.receipt.account = action.account;
        action.receipt.state = QString::fromLatin1(state); action.receipt.stage = QString::fromLatin1(stage);
        return action;
    }
    static void Certify(B3FlowMeshTrading::Receipt& receipt, uint64_t sequence = 5)
    {
        receipt.state = QStringLiteral("certified_inclusion"); receipt.stage = QStringLiteral("certified"); receipt.certificate_verified = true;
        receipt.microblock_hash = QString::fromStdString(H(97).GetHex()); receipt.microblock_sequence = sequence;
    }
    static QStringList RowText(const QTableWidget* view, int row)
    {
        QStringList cells;
        for (int c{0}; c < view->columnCount(); ++c) cells << (view->item(row, c) ? view->item(row, c)->text() : QString{});
        return cells;
    }
    //! The probe's reads that reach the market endpoint, in order.
    static std::vector<std::string> Remote(const FundingReadProbe& probe)
    {
        std::vector<std::string> remote;
        for (const auto& method : probe.methods) if (method != "getflowmeshclientinfo" && method != "listflowmeshactions") remote.push_back(method);
        return remote;
    }

    static void Observe(B3FlowMeshTradingPanel& panel, const Snapshot& snapshot, bool inverse = false)
    {
        panel.m_timer->stop();
        // Older retained lifecycle fixtures explicitly use canonical entry.
        // Default production orientation is independently exercised below.
        { QSignalBlocker blocked{panel.m_orientation}; panel.m_orientation->setCurrentIndex(inverse ? 1 : 0); }
        B3FlowMeshTrading::Market market;
        market.id = snapshot.market; market.base = snapshot.base;
        market.domain = snapshot.domain; market.config = snapshot.config;
        market.vault = QString::fromStdString(flowmesh::ComputeFlowMeshVaultId(
            H(1), *uint256::FromHex(snapshot.market.toStdString()))->GetHex());
        market.account = snapshot.account; market.has_account = true;
        market.sequence = snapshot.account_sequence; market.ready = true;
        market.remote = snapshot.remote;
        market.publish_ready = true;
        market.base_available = snapshot.base_available; market.base_reserved = snapshot.base_reserved;
        market.b3_available = snapshot.b3_available; market.b3_reserved = snapshot.b3_reserved;
        panel.m_market_data = {market};
        {
            QSignalBlocker blocked{panel.m_market};
            panel.m_market->clear();
            panel.m_market->addItem(QStringLiteral("tUSD / B3 — synthetic test"), market.id);
        }
        panel.m_snapshot = snapshot;
        panel.m_response_age.start();
        panel.updateDataViews();
        panel.updateMarketText();
    }

    //! The walletless synthetic market of the visual QA export: seven
    //! certified clearings, the latest at 1009 B3 atoms per tUSD atom.
    void ShowSyntheticClearings(B3FlowMeshTradingPanel& panel)
    {
        auto s{Parse(Data())}; s.next_sequence = 9; s.pending_actions = 2; s.history.clear();
        for (int i{0}; i < 7; ++i) {
            Trade t; t.sequence = i + 2; t.hash = QString::fromStdString(H(30 + i).GetHex()); t.epoch = 0; t.anchor_height = 50; t.cleared = true; t.price = 1000 + std::array<int, 7>{0, 4, 2, 6, 3, 7, 9}[i]; t.quantity = 100'000 + i * 25'000; t.notional = t.price * t.quantity; t.fee = *FeeExample(t.notional); t.own_fills_known = true; t.own_buy = i % 2 ? 0 : t.quantity / 2; t.own_sell = 0; s.history.push_back(t);
        }
        B3FlowMeshTrading::Market m; m.id = s.market; m.base = s.base; m.domain = s.domain; m.config = s.config; m.vault = QString::fromStdString(flowmesh::ComputeFlowMeshVaultId(H(1), *uint256::FromHex(s.market.toStdString()))->GetHex()); m.account = s.account; m.has_account = true; m.ready = true; m.publish_ready = true; m.sequence = s.account_sequence; m.base_available = s.base_available; m.b3_available = s.b3_available;
        m.base_reserved = s.base_reserved; m.b3_reserved = s.b3_reserved;
        panel.m_market_data = {m}; { QSignalBlocker block{panel.m_market}; panel.m_market->addItem(QStringLiteral("tUSD / B3 — SYNTHETIC QA"), m.id); }
        panel.m_wallet_name = QStringLiteral("SYNTHETIC QA · no wallet / no signing"); panel.m_snapshot = s; panel.m_response_age.start(); panel.m_certificate_age.start(); panel.m_pending_market = s.market; panel.m_pending_account = s.account; panel.m_pending_sequence = s.account_sequence;
        panel.m_price->setText(QStringLiteral("1.009")); panel.m_quantity->setText(QStringLiteral("0.25")); panel.updateDataViews(); panel.updateMarketText(); panel.show(); QCoreApplication::processEvents();
    }

    //! How the synthetic observed-time market's source reports times.
    enum class SyntheticSource { LOCAL, REMOTE, OLDER_SERVER };
    //! A realistic, explicitly synthetic observed-time market for the visual
    //! QA export (no node, wallet or public feed): a multi-account book of 12
    //! limit levels per side from 24 accounts plus a four-price ladder order
    //! per side, the viewer's own bid, 60 held auctions with their observed
    //! times (two applied late, shown without a time), and 90 minutes of 1m
    //! candles as a node serves them (a proven quiet stretch, a gap of late
    //! auctions and a restart seam). An older server has neither times nor
    //! candles: its last live auctions carry receipt times instead.
    void ShowSyntheticTimeMarket(B3FlowMeshTradingPanel& panel, SyntheticSource source)
    {
        auto s{Parse(Data())};
        s.curves.clear(); s.own_curves.clear(); s.history.clear();
        for (int i{0}; i < 12; ++i) {
            const CAmount quantity{60'000 + static_cast<CAmount>((i * 37) % 9) * 45'000};
            const CAmount filled{i % 4 == 1 ? 15'000 : 0};
            s.curves.push_back(LimitCurve("bid", static_cast<unsigned char>(20 + i), 999 - i, quantity, filled));
            s.curves.push_back(LimitCurve("ask", static_cast<unsigned char>(80 + i), 1001 + i, quantity + 30'000));
        }
        // Ladder orders: one curve per side, four prices (eight breakpoints).
        Curve ladder_bid; ladder_bid.account = QString::fromStdString(H(40).GetHex()); ladder_bid.side = QStringLiteral("bid");
        ladder_bid.points = {{985, 400'000}, {986, 300'000}, {988, 300'000}, {989, 200'000}, {991, 200'000}, {992, 100'000}, {994, 100'000}, {995, 0}};
        ladder_bid.remaining = 400'000; ladder_bid.reserved = 395'800'000; // 100,000 x (985 + 988 + 991 + 994)
        Curve ladder_ask; ladder_ask.account = QString::fromStdString(H(41).GetHex()); ladder_ask.side = QStringLiteral("ask");
        ladder_ask.points = {{1005, 0}, {1006, 150'000}, {1008, 150'000}, {1009, 300'000}, {1011, 300'000}, {1012, 450'000}, {1014, 450'000}, {1015, 600'000}};
        ladder_ask.remaining = 600'000; ladder_ask.reserved = 600'000;
        s.curves.push_back(ladder_bid); s.curves.push_back(ladder_ask);
        auto own{LimitCurve("bid", 5, 997, 250'000, 50'000)};
        s.curves.push_back(own); s.own_curves = {own};
        s.curves_complete = true;
        s.depth = Aggregate(s.curves);
        // Sixty held auctions after 1590 served ones, ending at the head.
        constexpr uint64_t FIRST_TICK{1591}, HEAD{1650};
        s.next_sequence = HEAD + 1; s.head = QString::fromStdString(H(250).GetHex());
        TickPage page; page.available = true; page.remote = source != SyntheticSource::LOCAL;
        ObservedTimes times; times.available = source != SyntheticSource::OLDER_SERVER;
        const QString endpoint{QStringLiteral("https://trade.example.org:18443")};
        times.source = source == SyntheticSource::LOCAL ? QStringLiteral("this_node") : endpoint;
        CAmount price{1000};
        int64_t at{OBSERVED_T0 - 7 * 60'000};
        for (uint64_t sequence{FIRST_TICK}; sequence <= HEAD; ++sequence) {
            Tick t; t.sequence = sequence; t.kind = QStringLiteral("execution");
            uint256 hash; hash.begin()[0] = static_cast<unsigned char>(sequence & 0xff); hash.begin()[1] = static_cast<unsigned char>(sequence >> 8); hash.begin()[2] = 0xa5;
            if (sequence == HEAD) hash = H(250);
            t.hash = QString::fromStdString(hash.GetHex());
            const bool cleared{sequence % 5 != 3};
            t.cleared = cleared; t.side = QStringLiteral("none");
            if (cleared) {
                price = std::clamp<CAmount>(price + static_cast<CAmount>((sequence * 7919) % 7) - 3, 992, 1008);
                t.price = price; t.volume = 20'000 + static_cast<CAmount>((sequence * 104729) % 9) * 22'500;
                if (sequence % 3 == 0) { t.side = QStringLiteral("bid"); t.imbalance = 10'000; }
            }
            page.ticks.push_back(t);
            at += 5'000 + static_cast<int64_t>((sequence * 31) % 5) * 1'000;
            flowmesh::ObservedStamp stamp; stamp.sequence = sequence; stamp.hash = hash; stamp.ms = at;
            stamp.mode = sequence == 1620 || sequence == 1621 ? flowmesh::ObservedMode::DELAYED : flowmesh::ObservedMode::LIVE;
            times.rows.push_back(stamp);
            if (cleared && sequence + 20 > HEAD) {
                Trade trade; trade.sequence = sequence; trade.hash = t.hash; trade.anchor_height = 50; trade.cleared = true;
                trade.price = t.price; trade.quantity = t.volume; trade.notional = t.price * t.volume; trade.fee = *FeeExample(trade.notional);
                trade.own_fills_known = true; trade.own_buy = sequence % 4 == 0 ? t.volume / 4 : 0; s.history.push_back(trade);
            }
        }
        s.ticks = page;
        // The node's exact levels of every curve (the viewer's own marked).
        s = WithLevels(s, {}, H(5));
        s.remote = source != SyntheticSource::LOCAL;
        if (s.remote) { s.endpoint = endpoint; s.certificate_verified = s.account_state_verified = true; }
        if (source == SyntheticSource::OLDER_SERVER) times.reason = QStringLiteral("endpoint_does_not_advertise_observed_time");
        s.observed = times;
        // Ninety minutes of served 1m candles, their last sequence right before the held ticks.
        B3FlowMeshMarketData::CandlePage candles;
        candles.available = true; candles.remote = s.remote; candles.source = times.source;
        candles.interval = flowmesh::CandleInterval::M1;
        candles.before_ms = OBSERVED_T0 - 7 * 60'000; candles.from_ms = candles.before_ms - 300 * 60'000;
        std::vector<flowmesh::ObservedCandle> reversed;
        uint64_t last{FIRST_TICK - 1};
        CAmount close{1000};
        for (int minute{89}; minute >= 0; --minute) {
            const int64_t open_ms{candles.before_ms - 90 * 60'000 + minute * 60'000};
            if (minute >= 20 && minute < 27) continue;          // quiet: no auction existed
            if (minute == 46) { last -= 5; continue; }           // five auctions applied late: a gap
            flowmesh::ObservedCandle c; c.open_ms = open_ms;
            c.entries = static_cast<uint32_t>(4 + (minute * 7) % 8);
            c.last_sequence = last; c.first_sequence = last - c.entries + 1; last = c.first_sequence - 1;
            c.clearings = c.entries - (minute % 3 == 0 ? 1 : 0);
            const CAmount swing{static_cast<CAmount>((minute * 13) % 9) - 4};
            c.close = close; c.open = std::clamp<CAmount>(close - swing, 985, 1015);
            c.high = std::max(c.open, c.close) + static_cast<CAmount>(minute % 4); c.low = std::min(c.open, c.close) - static_cast<CAmount>((minute + 1) % 3);
            close = c.open;
            c.base_volume = static_cast<uint64_t>(c.clearings) * (40'000 + static_cast<uint64_t>(minute % 6) * 17'500);
            c.quote_volume = c.base_volume * static_cast<uint64_t>(c.close);
            if (minute == 60) c.flags |= flowmesh::CANDLE_SEAM_BEFORE;
            if (minute == 45 || minute == 47) c.flags |= flowmesh::CANDLE_PARTIAL;
            reversed.push_back(c);
        }
        candles.candles.assign(reversed.rbegin(), reversed.rend());
        panel.m_candle_interval = flowmesh::CandleInterval::M1;
        Observe(panel, s, true);
        B3FlowMeshTrading::Market& row{panel.m_market_data[0]};
        row.remote = s.remote;
        panel.m_wallet_name = QStringLiteral("SYNTHETIC QA · no wallet / no signing");
        panel.m_certificate_age.start();
        panel.mergeHeldTicks();
        if (source == SyntheticSource::OLDER_SERVER) {
            // Receipt times, live single auctions only: the newest three.
            for (uint64_t sequence{HEAD - 2}; sequence <= HEAD; ++sequence) panel.m_received_times[sequence] = OBSERVED_T0 + static_cast<int64_t>(sequence - HEAD) * 6'000;
        } else {
            panel.applyCandles(candles, 300);
        }
        panel.m_price->setText(QStringLiteral("1.003")); panel.m_quantity->setText(QStringLiteral("0.25"));
        panel.updateDataViews(); panel.updateMarketText();
        panel.m_chart->setNowMs(OBSERVED_T0 + 20'000);
        panel.show(); QCoreApplication::processEvents();
    }

    //! Data() one head later (sequence 3 -> 4, head H(10)): a new standing
    //! bid of another account at 998 and the viewer's own bid partly filled.
    static UniValue NewerLocalData()
    {
        auto data{Data()}; auto snapshot{data["snapshot"]};
        snapshot.pushKV("next_microblock_sequence", 4); snapshot.pushKV("last_microblock_hash", H(10).GetHex());
        snapshot.pushKV("state_root", H(11).GetHex()); data.pushKV("snapshot", snapshot);
        auto liquidity{data["liquidity"]}; UniValue curves{liquidity["curves"]};
        auto extra{CurveJson("bid", 9)}; UniValue points{UniValue::VARR};
        for (const auto [price, quantity] : {std::pair{998, 400'000}, std::pair{999, 0}}) { UniValue p{UniValue::VOBJ}; p.pushKV("price", price); p.pushKV("quantity", quantity); points.push_back(p); }
        extra.pushKV("points", points); extra.pushKV("filled_quantity", 0); extra.pushKV("remaining_quantity", 400'000); extra.pushKV("reserved_amount", 399'200'000);
        curves.push_back(extra); liquidity.pushKV("curves", curves); liquidity.pushKV("total_curves", 3); data.pushKV("liquidity", liquidity);
        auto account{data["account"]}; account.pushKV("next_sequence", 9);
        // The viewer replaced its bid: now at 998.
        auto own{CurveJson("bid")}; UniValue own_points{UniValue::VARR};
        for (const auto [price, quantity] : {std::pair{998, 1'000'000}, std::pair{999, 0}}) { UniValue p{UniValue::VOBJ}; p.pushKV("price", price); p.pushKV("quantity", quantity); own_points.push_back(p); }
        own.pushKV("points", own_points); own.pushKV("filled_quantity", 0); own.pushKV("remaining_quantity", 1'000'000); own.pushKV("reserved_amount", 998'000'000);
        UniValue owns{UniValue::VARR}; owns.push_back(own); account.pushKV("curves", owns); data.pushKV("account", account);
        return data;
    }
    static UniValue LocalWatchObject(const char* status = "head_changed")
    {
        UniValue watch{UniValue::VOBJ}; watch.pushKV("status", status); watch.pushKV("supported", true); watch.pushKV("waited", true);
        watch.pushKV("gap", false); watch.pushKV("endpoint", ""); watch.pushKV("endpoint_live_ms", 0);
        return watch;
    }
    //! A local-engine panel with live updates, as the tick leaves it.
    static void LocalLive(B3FlowMeshTradingPanel& panel)
    {
        panel.show(); QCoreApplication::processEvents();
        UniValue info{UniValue::VOBJ}; info.pushKV("backend", "local"); info.pushKV("engine_enabled", true); info.pushKV("endpoints", UniValue{UniValue::VARR});
        panel.m_client_info = info;
        panel.updateWatchState();
        panel.m_watch.mode = B3FlowMeshWatch::Mode::Live; panel.m_watch.last_ok_at = panel.m_watch_clock.elapsed();
    }
    //! The next certified head of the synthetic observed-time market, as a
    //! local live update brings it: one more auction (its tick and observed
    //! time), a partial fill at one level and, every third head, an order at
    //! a new price. The node's exact levels of every curve, the viewer's own
    //! marked. Display only; nothing is signed.
    static Snapshot NextSyntheticHead(const Snapshot& shown, const std::deque<Tick>& held, uint64_t n, std::optional<CAmount> new_bid = std::nullopt)
    {
        auto s{shown};
        const uint64_t sequence{s.next_sequence};
        uint256 hash; hash.begin()[0] = static_cast<unsigned char>(sequence & 0xff); hash.begin()[1] = static_cast<unsigned char>(sequence >> 8); hash.begin()[2] = 0x5c;
        s.next_sequence = sequence + 1; s.head = QString::fromStdString(hash.GetHex());
        uint256 root{hash}; root.begin()[3] = 0x77; s.state_root = QString::fromStdString(root.GetHex());
        Tick t; t.sequence = sequence; t.hash = s.head; t.kind = QStringLiteral("execution"); t.side = QStringLiteral("none");
        t.cleared = sequence % 4 != 1;
        const CAmount last{held.empty() ? 1000 : held.back().price ? held.back().price : 1000};
        if (t.cleared) { t.price = std::clamp<CAmount>(last + static_cast<CAmount>((sequence * 7919) % 5) - 2, 994, 1006); t.volume = 30'000 + static_cast<CAmount>((sequence * 104729) % 7) * 17'500; }
        TickPage page; page.available = true; page.ticks = {t};
        s.ticks = page;
        ObservedTimes times{*s.observed};
        flowmesh::ObservedStamp stamp; stamp.sequence = sequence; stamp.hash = hash; stamp.mode = flowmesh::ObservedMode::LIVE;
        stamp.ms = OBSERVED_T0 + static_cast<int64_t>(n) * 1'700;
        times.rows = {stamp};
        s.observed = times;
        if (t.cleared) {
            Trade trade; trade.sequence = sequence; trade.hash = t.hash; trade.anchor_height = 50; trade.cleared = true;
            trade.price = t.price; trade.quantity = t.volume; trade.notional = t.price * t.volume; trade.fee = *FeeExample(trade.notional);
            trade.own_fills_known = true; s.history.push_back(trade);
            while (s.history.size() > 100) s.history.erase(s.history.begin());
        }
        // A partial fill at one level, and every third head a new price.
        auto& filled{s.curves[static_cast<size_t>(n * 5) % 24]};
        if (filled.remaining > 20'000) { filled.filled += 10'000; filled.remaining -= 10'000; }
        if (new_bid) {
            s.curves.push_back(LimitCurve("bid", static_cast<unsigned char>(140 + n % 110), *new_bid, 65'000));
        } else if (n % 3 == 0) {
            const bool bid{n % 2 == 0};
            s.curves.push_back(LimitCurve(bid ? "bid" : "ask", static_cast<unsigned char>(140 + n % 110), bid ? 987 - static_cast<CAmount>(n % 4) : 1013 + static_cast<CAmount>(n % 4), 45'000 + static_cast<CAmount>(n % 5) * 20'000));
        }
        s.depth = Aggregate(s.curves);
        return WithLevels(s, {}, H(5));
    }
private Q_SLOTS:
    void initTestCase()
    {
        Q_INIT_RESOURCE(bitcoin);
        m_setup = std::make_unique<TestingSetup>(ChainType::REGTEST,
            TestOpts{.extra_args = {"-keypool=1"}, .setup_net = false});
        m_node = interfaces::MakeNode(m_setup->m_node);
        m_loader = interfaces::MakeWalletLoader(*m_setup->m_node.chain, *m_setup->m_node.args);
        m_setup->m_node.wallet_loader = m_loader.get();
        m_options = std::make_unique<OptionsModel>(*m_node);
        bilingual_str error;
        QVERIFY2(m_options->Init(error), error.original.c_str());
        m_client = std::make_unique<ClientModel>(*m_node, m_options.get());
        m_style.reset(PlatformStyle::instantiate("other"));
        QVERIFY(m_style);
        m_wallet = std::make_shared<wallet::CWallet>(m_setup->m_node.chain.get(),
            "offline-display-test", wallet::CreateMockableWalletDatabase());
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
        m_model = std::make_unique<WalletModel>(
            interfaces::MakeWallet(*m_loader->context(), m_wallet), *m_client, m_style.get());
        QVERIFY(m_model->setWalletEncrypted(SecureString{"offline display fixture passphrase"}));
        QVERIFY(m_model->setWalletLocked(true));
    }

    void init()
    {
        QVERIFY(m_model->setWalletLocked(true));
        QVERIFY(m_wallet->IsLocked());
    }

    void tokenUnitsAreExactAndReadable()
    {
        const auto units{Parse(Data()).units}; QString error;
        QCOMPARE(ParseQuantity(QStringLiteral("1"), units, &error).value(), CAmount{1'000'000});
        QCOMPARE(ParseQuantity(QStringLiteral("0.000001"), units).value(), CAmount{1});
        QCOMPARE(ParsePrice(QStringLiteral("1"), units).value(), CAmount{1000});
        QCOMPARE(ParsePrice(QStringLiteral("0.001"), units).value(), CAmount{1});
        QCOMPARE(FormatPrice(1000, 6), QStringLiteral("1")); QCOMPARE(FormatAmount(1'250'001, 6), QStringLiteral("1.250001"));
        QCOMPARE(Notional(1000, 1'000'000).value(), CAmount{1'000'000'000}); QCOMPARE(FeeExample(1'000'000'000).value(), CAmount{100'000});
        QCOMPARE(ParseQuantity(QStringLiteral("9007199254.740993"), units).value(), CAmount{9'007'199'254'740'993LL});
        QVERIFY(!Notional(MAX_MONEY, 2));
    }
    void offGridOrAmbiguousInputsNeverRound()
    {
        auto u{Parse(Data()).units};
        for (const auto& text : {QStringLiteral("0.0000001"), QStringLiteral("-1"), QStringLiteral("+1"), QStringLiteral("1e6"), QStringLiteral(" 1"), QStringLiteral("1 "), QStringLiteral("1."), QStringLiteral("NaN"), QStringLiteral("0")}) QVERIFY2(!ParseQuantity(text, u), qPrintable(text));
        QVERIFY(!ParsePrice(QStringLiteral("0.0001"), u));
        u.decimals = 18; QCOMPARE(ParseQuantity(QStringLiteral("0.000000000000000001"), u).value(), CAmount{1});
        QCOMPARE(ParsePrice(QStringLiteral("1000000000"), u).value(), CAmount{1}); QVERIFY(!ParsePrice(QStringLiteral("1000000001"), u));
        QCOMPARE(FormatPrice(MAX_MONEY, 18), QString::number(MAX_MONEY) + QStringLiteral("000000000"));
        u.known = false; QVERIFY(!ParseQuantity(QStringLiteral("1"), u)); QVERIFY(!ParsePrice(QStringLiteral("1"), u));
    }
    void realCurvesAreNotSummedLikeAnOrderBook()
    {
        const auto s{Parse(Data())}; QCOMPARE(s.curves.size(), size_t{2}); QCOMPARE(s.depth.size(), size_t{3});
        QCOMPARE(s.depth[1].price, CAmount{1000}); QCOMPARE(s.depth[1].demand, CAmount{750'000}); QCOMPARE(s.depth[1].supply, CAmount{750'000});
        QCOMPARE(s.own_curves.size(), size_t{1}); QCOMPARE(s.history.size(), size_t{1}); QCOMPARE(s.history[0].own_buy, CAmount{250'000});
        auto malformed{s.curves}; malformed[0].points.clear(); QVERIFY(Rejects([&] { Aggregate(malformed); }));
    }
    void limitBookGroupsExactPricesAndUsesRemainingQuantities()
    {
        auto snapshot{Parse(Data(false))};
        snapshot.curves = {LimitCurve("bid", 1, 90, 4), LimitCurve("ask", 4, 110, 8, 2),
            LimitCurve("bid", 2, 100, 10, 3), LimitCurve("ask", 6, 120, 4, 1),
            LimitCurve("bid", 3, 100, 7, 2), LimitCurve("ask", 5, 110, 2)};
        const auto original{snapshot};
        const auto book{ProjectLimitBook(snapshot, false)};
        QVERIFY(book.complete); QVERIFY(book.units_known); QCOMPARE(book.projected_curves, size_t{6});
        QCOMPARE(book.bids.size(), size_t{2}); QCOMPARE(book.asks.size(), size_t{2});
        QCOMPARE(book.bids[0].canonical_price, CAmount{100}); QCOMPARE(book.bids[1].canonical_price, CAmount{90});
        QCOMPARE(book.asks[0].canonical_price, CAmount{120}); QCOMPARE(book.asks[1].canonical_price, CAmount{110});
        QCOMPARE(book.bids[0].remaining, CAmount{12}); QCOMPARE(book.bids[0].gross_notional, CAmount{1200});
        QCOMPARE(book.bids[0].curve_count, size_t{2}); QCOMPARE(book.asks[1].curve_count, size_t{2});
        QCOMPARE(book.asks[1].remaining, CAmount{8}); QCOMPARE(book.asks[1].gross_notional, CAmount{880});
        QCOMPARE(book.bids[0].price, QStringLiteral("0.1"));
        QCOMPARE(book.bids[0].amount, QStringLiteral("0.000012"));
        QCOMPARE(book.bids[0].total, QStringLiteral("0.0000012"));
        QVERIFY(book.canonical_best_bid); QCOMPARE(*book.canonical_best_bid, CAmount{100});
        QVERIFY(book.canonical_best_ask); QCOMPARE(*book.canonical_best_ask, CAmount{110});
        QCOMPARE(book.spread, QStringLiteral("0.01"));

        const auto inverse{ProjectLimitBook(snapshot, true)};
        QVERIFY(inverse.complete); QCOMPARE(inverse.projected_curves, size_t{6});
        // Both visible sides descend. Reverse asks come from canonical bids;
        // reverse bids come from canonical asks. Best asks remain at bottom.
        QCOMPARE(inverse.asks[0].canonical_price, CAmount{90}); QCOMPARE(inverse.asks[1].canonical_price, CAmount{100});
        QCOMPARE(inverse.bids[0].canonical_price, CAmount{110}); QCOMPARE(inverse.bids[1].canonical_price, CAmount{120});
        QCOMPARE(inverse.bids[0].price, ExactInversePrice(110, 6));
        QCOMPARE(inverse.bids[0].amount, QStringLiteral("0.00000088"));
        QCOMPARE(inverse.bids[0].total, QStringLiteral("0.000008"));
        QCOMPARE(inverse.spread, QStringLiteral("10/11"));
        // Shown to the 3 places of the best price 1/0.11 = ≈9.091.
        QCOMPARE(inverse.spread_display, QStringLiteral("≈0.909")); QCOMPARE(book.spread_display, book.spread);
        QVERIFY(snapshot == original);
        std::reverse(snapshot.curves.begin(), snapshot.curves.end());
        QVERIFY(ProjectLimitBook(snapshot, false) == book);
        QVERIFY(ProjectLimitBook(snapshot, true) == inverse);
    }
    void limitBookNeverGroupsRoundedInversePrices()
    {
        auto snapshot{Parse(Data(false))};
        const CAmount price{9'007'199'254'740'992LL};
        snapshot.curves = {LimitCurve("bid", 1, price + 1, 1), LimitCurve("bid", 2, price, 1)};
        // The chart's display-only approximations coincide; exact book levels
        // must remain distinct, including beyond double's consecutive range.
        QCOMPARE(FormatDisplayPrice(price, 6, true), FormatDisplayPrice(price + 1, 6, true));
        const auto book{ProjectLimitBook(snapshot, true)};
        QCOMPARE(book.asks.size(), size_t{2}); QVERIFY(book.bids.empty());
        QCOMPARE(book.asks[0].canonical_price, price); QCOMPARE(book.asks[1].canonical_price, price + 1);
        QVERIFY(book.asks[0].price != book.asks[1].price);
        QCOMPARE(ParseDisplayPrice(book.asks[0].price, snapshot.units, true).value(), price);
        QCOMPARE(ParseDisplayPrice(book.asks[1].price, snapshot.units, true).value(), price + 1);
        QVERIFY(book.spread.isEmpty());
    }
    void limitBookDisclosesGeneralPartialAndUnknownUnitData()
    {
        auto snapshot{Parse(Data(false))};
        snapshot.curves = {LimitCurve("bid", 1, 100, 10), LimitCurve("ask", 2, 110, 10)};
        auto slope{LimitCurve("bid", 3, 100, 10)};
        slope.points = {{99, 10}, {101, 0}}; // Valid curve, not one discrete limit.
        snapshot.curves.push_back(slope);
        const auto general{ProjectLimitBook(snapshot, false)};
        QCOMPARE(general.general_curves, size_t{1}); QCOMPARE(general.invalid_curves, size_t{0});
        QCOMPARE(general.projected_curves, size_t{2}); QCOMPARE(general.bids.size(), size_t{1});
        QCOMPARE(general.bids.front().remaining, CAmount{10}); QVERIFY(!general.complete); QVERIFY(general.spread.isEmpty());
        snapshot.curves.pop_back(); snapshot.curves_complete = false;
        const auto partial{ProjectLimitBook(snapshot, false)};
        QVERIFY(!partial.complete); QCOMPARE(partial.projected_curves, size_t{2}); QVERIFY(partial.spread.isEmpty());
        snapshot.curves_complete = true;
        for (const int unknown : {0, 1}) {
            auto unverified{snapshot};
            if (unknown == 0) unverified.units.known = false;
            else unverified.units.asset = QString::fromStdString(H(99).GetHex());
            const auto book{ProjectLimitBook(unverified, true)};
            QVERIFY(!book.units_known); QVERIFY(book.spread.isEmpty());
            QCOMPARE(book.bids.size(), size_t{1}); QCOMPARE(book.asks.size(), size_t{1});
            for (const auto* rows : {&book.asks, &book.bids}) for (const auto& row : *rows) {
                QVERIFY(row.price.isEmpty()); QVERIFY(row.amount.isEmpty()); QVERIFY(row.total.isEmpty());
            }
        }
        snapshot.curves = {LimitCurve("bid", 1, 100, 10, 10), LimitCurve("ask", 2, 110, 10)};
        const auto exhausted{ProjectLimitBook(snapshot, false)};
        QCOMPARE(exhausted.exhausted_curves, size_t{1}); QCOMPARE(exhausted.projected_curves, size_t{1});
        QVERIFY(exhausted.bids.empty()); QCOMPARE(exhausted.asks.size(), size_t{1}); QVERIFY(exhausted.complete);
        QVERIFY(exhausted.spread.isEmpty());
    }
    void limitBookZeroLimitsAndCrossedBooksHaveNoInventedSpread()
    {
        auto snapshot{Parse(Data(false))};
        snapshot.curves = {LimitCurve("bid", 1, 0, 3), LimitCurve("ask", 2, 0, 5)};
        const auto canonical{ProjectLimitBook(snapshot, false)};
        QCOMPARE(canonical.bids.size(), size_t{1}); QCOMPARE(canonical.asks.size(), size_t{1});
        QCOMPARE(canonical.bids[0].price, QStringLiteral("0")); QCOMPARE(canonical.bids[0].gross_notional, CAmount{0});
        const auto inverse{ProjectLimitBook(snapshot, true)};
        QCOMPARE(inverse.zero_inverse_curves, size_t{2}); QVERIFY(inverse.bids.empty()); QVERIFY(inverse.asks.empty());
        QVERIFY(!inverse.complete); QVERIFY(inverse.spread.isEmpty());
        snapshot.curves = {LimitCurve("bid", 1, 111, 3), LimitCurve("ask", 2, 110, 5)};
        QVERIFY(ProjectLimitBook(snapshot, false).spread.isEmpty());
        QVERIFY(ProjectLimitBook(snapshot, true).spread.isEmpty());
    }
    void limitBookRejectsWholeOverflowingGroupsAndInvalidDuplicates()
    {
        auto snapshot{Parse(Data(false))};
        const auto valid_ask{LimitCurve("ask", 3, 7, 1)};
        // Zero price isolates remaining-quantity overflow from notional.
        snapshot.curves = {LimitCurve("bid", 1, 0, MAX_MONEY / 2 + 1),
            LimitCurve("bid", 2, 0, MAX_MONEY / 2 + 1), valid_ask};
        auto book{ProjectLimitBook(snapshot, false)};
        QCOMPARE(book.invalid_curves, size_t{2}); QVERIFY(book.bids.empty()); QCOMPARE(book.asks.size(), size_t{1});
        QCOMPARE(book.projected_curves, size_t{1}); QVERIFY(!book.complete); QVERIFY(book.spread.isEmpty());
        // Here remaining fits, but summed gross quote notional does not.
        snapshot.curves = {LimitCurve("bid", 1, 2, MAX_MONEY / 4 + 1),
            LimitCurve("bid", 2, 2, MAX_MONEY / 4 + 1), valid_ask};
        book = ProjectLimitBook(snapshot, false);
        QCOMPARE(book.invalid_curves, size_t{2}); QVERIFY(book.bids.empty()); QCOMPARE(book.asks.size(), size_t{1});
        snapshot.curves = {LimitCurve("ask", 1, MAX_MONEY, 2), valid_ask};
        book = ProjectLimitBook(snapshot, false);
        QCOMPARE(book.invalid_curves, size_t{1}); QCOMPARE(book.asks.size(), size_t{1});
        auto duplicate{LimitCurve("bid", 0xab, 100, 1)};
        auto upper{duplicate}; upper.account = upper.account.toUpper();
        snapshot.curves = {duplicate, upper, valid_ask};
        book = ProjectLimitBook(snapshot, false);
        QCOMPARE(book.invalid_curves, size_t{2}); QVERIFY(book.bids.empty()); QCOMPARE(book.asks.size(), size_t{1});
        for (const int fault : {0, 1, 2, 3}) {
            auto malformed{LimitCurve("bid", 1, 100, 10)};
            if (fault == 0) ++malformed.remaining;
            if (fault == 1) malformed.filled = -1;
            if (fault == 2) malformed.side = QStringLiteral("unknown");
            if (fault == 3) malformed.points.clear();
            snapshot.curves = {malformed, valid_ask};
            book = ProjectLimitBook(snapshot, false);
            QCOMPARE(book.invalid_curves, size_t{1}); QVERIFY(book.bids.empty()); QCOMPARE(book.asks.size(), size_t{1});
        }
        snapshot.curves.assign(129, valid_ask);
        book = ProjectLimitBook(snapshot, false);
        QVERIFY(!book.complete); QVERIFY(book.bids.empty()); QVERIFY(book.asks.empty());
        QCOMPARE(book.invalid_curves, size_t{129});
    }
    void limitBookInverseSpreadRemainsExactBeyond128BitDenominator()
    {
        auto snapshot{Parse(Data(false))}; snapshot.units.decimals = 18;
        snapshot.curves = {LimitCurve("bid", 1, MAX_MONEY - 2, 1), LimitCurve("ask", 2, MAX_MONEY - 1, 1)};
        const auto book{ProjectLimitBook(snapshot, true)};
        QVERIFY(book.complete); QCOMPARE(book.asks.size(), size_t{1}); QCOMPARE(book.bids.size(), size_t{1});
        QCOMPARE(book.spread, QStringLiteral("1/438508839999999998013400000000000002000000000"));
        QCOMPARE(ParseDisplayPrice(book.asks[0].price, snapshot.units, true).value(), MAX_MONEY - 2);
        QCOMPARE(ParseDisplayPrice(book.bids[0].price, snapshot.units, true).value(), MAX_MONEY - 1);
    }
    void limitPriceOfMatchesBookProjectionShapes()
    {
        QCOMPARE(LimitPriceOf(LimitCurve("bid", 5, 1000, 10)), std::optional<CAmount>{1000});
        QCOMPARE(LimitPriceOf(LimitCurve("ask", 5, 1000, 10)), std::optional<CAmount>{1000});
        QCOMPARE(LimitPriceOf(LimitCurve("ask", 5, 0, 10)), std::optional<CAmount>{0});
        auto sloped{LimitCurve("bid", 5, 1000, 10)}; sloped.points.back().price += 1;
        QVERIFY(!LimitPriceOf(sloped));
        auto relabelled{LimitCurve("bid", 5, 1000, 10)}; relabelled.side = QStringLiteral("ask");
        QVERIFY(!LimitPriceOf(relabelled));
        // Exactly the curves the book projects as limit levels.
        auto snapshot{SyntheticLimitBook(2)}; snapshot.curves.push_back(sloped);
        const auto book{ProjectLimitBook(snapshot, false)};
        QCOMPARE(book.general_curves, size_t{1}); QCOMPARE(book.projected_curves, size_t{4});
    }
    void marketableLimitUsesVerifiedBestAndIntegerSlippage_data()
    {
        QTest::addColumn<bool>("buy");
        QTest::addColumn<bool>("inverse");
        QTest::addColumn<int>("bps");
        QTest::addColumn<qint64>("reference");
        QTest::addColumn<qint64>("price");
        QTest::addColumn<qint64>("visible");
        QTest::addColumn<QString>("display");
        // Best others' bid 1000 and ask 1010 (SyntheticLimitBook).
        QTest::newRow("buy 1%") << true << false << 100 << qint64{1010} << qint64{1020} << qint64{1'245'000} << QStringLiteral("1.02");
        QTest::newRow("sell 1%") << false << false << 100 << qint64{1000} << qint64{990} << qint64{945'000} << QStringLiteral("0.99");
        QTest::newRow("buy 0.01% floors to the best ask") << true << false << 1 << qint64{1010} << qint64{1010} << qint64{150'000} << QStringLiteral("1.01");
        QTest::newRow("sell 0.01% ceils to the best bid") << false << false << 1 << qint64{1000} << qint64{1000} << qint64{100'000} << QStringLiteral("1");
        QTest::newRow("buy 10%") << true << false << 1000 << qint64{1010} << qint64{1111} << qint64{1'865'000} << QStringLiteral("1.111");
        // Inverse: Buy B3 is a canonical ask (sell tokens) against the best bid.
        QTest::newRow("buy B3 1%") << true << true << 100 << qint64{1000} << qint64{990} << qint64{945'000} << QStringLiteral("100/99");
        QTest::newRow("sell B3 1%") << false << true << 100 << qint64{1010} << qint64{1020} << qint64{1'245'000} << QStringLiteral("50/51");
    }
    void marketableLimitUsesVerifiedBestAndIntegerSlippage()
    {
        QFETCH(bool, buy); QFETCH(bool, inverse); QFETCH(int, bps); QFETCH(qint64, reference); QFETCH(qint64, price); QFETCH(qint64, visible); QFETCH(QString, display);
        auto snapshot{SyntheticLimitBook()};
        QString error;
        const auto limit{ComputeMarketableLimit(snapshot, buy, inverse, bps, CAmount{1'000'000}, &error)};
        QVERIFY2(limit, qPrintable(error));
        QCOMPARE(limit->reference, reference); QCOMPARE(limit->price, price); QCOMPARE(limit->visible, visible);
        QCOMPARE(limit->display, display); QCOMPARE(limit->slippage_bps, bps);
        QCOMPARE(limit->covers_quantity, visible >= 1'000'000);
        // Exact integer slippage: never beyond the bound, never past the reference.
        const bool bid{CanonicalSide(buy, inverse) == QStringLiteral("bid")};
        using Wide = util::Unsigned128;
        if (bid) { QVERIFY(Wide{static_cast<uint64_t>(price)} * 10'000 <= Wide{static_cast<uint64_t>(reference)} * (10'000 + bps)); QVERIFY(price >= reference); }
        else { QVERIFY(Wide{static_cast<uint64_t>(price)} * 10'000 >= Wide{static_cast<uint64_t>(reference)} * (10'000 - bps)); QVERIFY(price <= reference); }
        // What the ticket parser reads back is exactly the signed limit.
        const auto parsed{ParseDisplayLimit(limit->display, snapshot.units, inverse, buy)};
        QVERIFY(parsed); QCOMPARE(parsed->price, limit->price); QVERIFY(!parsed->adjusted);
    }
    void marketableLimitRefusesIncompleteStaleOneSidedOrUnknownBooks()
    {
        QString error;
        const auto refuses = [&](const Snapshot& s, bool buy, const QString& reason) {
            error.clear();
            const bool refused{!ComputeMarketableLimit(s, buy, false, 100, std::nullopt, &error)};
            return refused && error.contains(reason);
        };
        auto partial{SyntheticLimitBook()}; partial.curves_complete = false;
        QVERIFY(refuses(partial, true, QStringLiteral("Only part of the certified book")));
        auto general{SyntheticLimitBook()}; general.curves.back().points.back().price += 5;
        QVERIFY(refuses(general, true, QStringLiteral("not simple limits")));
        auto uncertified{SyntheticLimitBook()}; uncertified.certified = false;
        QVERIFY(refuses(uncertified, true, QStringLiteral("certified book")));
        auto unknown{SyntheticLimitBook()}; unknown.units.known = false;
        QVERIFY(refuses(unknown, true, QStringLiteral("precision")));
        auto bids_only{SyntheticLimitBook()};
        std::erase_if(bids_only.curves, [](const Curve& c) { return c.side == QStringLiteral("ask"); });
        QVERIFY(refuses(bids_only, true, QStringLiteral("No certified sell orders")));
        QVERIFY(ComputeMarketableLimit(bids_only, false, false, 100, std::nullopt, &error));
        // A zero-price best ask is named as such, not as a missing book.
        auto zero{SyntheticLimitBook()}; zero.curves.push_back(LimitCurve("ask", 99, 0, 1000));
        QVERIFY(refuses(zero, true, QStringLiteral("zero-price")));
        QVERIFY(!ComputeMarketableLimit(SyntheticLimitBook(), true, false, 0, std::nullopt, &error));
        QVERIFY(!ComputeMarketableLimit(SyntheticLimitBook(), true, false, MAX_SLIPPAGE_BPS + 1, std::nullopt, &error));
    }
    void marketableLimitNeverCrossesOwnOppositeOrder()
    {
        QString error;
        auto snapshot{SyntheticLimitBook()}; snapshot.account = QString::fromStdString(H(5).GetHex());
        // The account's own cheaper ask is neither the reference nor depth…
        const auto own_ask{LimitCurve("ask", 5, 1005, 50'000)};
        snapshot.curves.push_back(own_ask); snapshot.own_curves = {own_ask};
        QVERIFY(!ComputeMarketableLimit(snapshot, true, false, 100, std::nullopt, &error));
        // …and a buy that would reach it would cross it.
        QVERIFY(error.contains(QStringLiteral("cross your own")));
        // An own ask above the limit does not cross.
        snapshot.curves.back() = LimitCurve("ask", 5, 1030, 50'000); snapshot.own_curves = {snapshot.curves.back()};
        const auto limit{ComputeMarketableLimit(snapshot, true, false, 100, std::nullopt, &error)};
        QVERIFY2(limit, qPrintable(error)); QCOMPARE(limit->reference, CAmount{1010}); QCOMPARE(limit->visible, CAmount{1'245'000});
        // An exhausted own opposite curve is not an order; a general one refuses.
        snapshot.own_curves.back().filled = snapshot.own_curves.back().remaining + snapshot.own_curves.back().filled; snapshot.own_curves.back().remaining = 0;
        QVERIFY(ComputeMarketableLimit(snapshot, true, false, 100, std::nullopt, &error));
        auto general{LimitCurve("ask", 5, 1030, 50'000)}; general.points.back().price += 3; general.points.front().price += 1;
        snapshot.own_curves = {general};
        QVERIFY(!ComputeMarketableLimit(snapshot, true, false, 100, std::nullopt, &error)); QVERIFY(error.contains(QStringLiteral("not a limit or ladder order")));
        // The own bid side never matters to a buy.
        snapshot.own_curves = {LimitCurve("bid", 5, 1005, 10)};
        QVERIFY(ComputeMarketableLimit(snapshot, true, false, 100, std::nullopt, &error));
    }
    void marketableLimitClampsSlippageAndPriceRange()
    {
        QCOMPARE(ParseSlippageBps(QStringLiteral("1.00")), std::optional<int>{100});
        QCOMPARE(ParseSlippageBps(QStringLiteral("0.01")), std::optional<int>{1});
        QCOMPARE(ParseSlippageBps(QStringLiteral("10")), std::optional<int>{1000});
        for (const auto* text : {"0", "0.005", "10.01", "-1", "1e2", "", "abc"}) QVERIFY2(!ParseSlippageBps(QString::fromLatin1(text)), text);
        // A reference at the top of the range cannot be exceeded.
        auto snapshot{SyntheticLimitBook(1)};
        snapshot.curves = {LimitCurve("bid", 20, 1000, 10), LimitCurve("ask", 80, MAX_MONEY - 1, 1)};
        QString error;
        const auto top{ComputeMarketableLimit(snapshot, true, false, 1000, std::nullopt, &error)};
        QVERIFY2(top, qPrintable(error)); QCOMPARE(top->price, MAX_MONEY - 1);
        // A best ask at MAX_MONEY cannot be reached by any limit bid.
        snapshot.curves.back() = LimitCurve("ask", 80, MAX_MONEY, 1);
        QVERIFY(!ComputeMarketableLimit(snapshot, true, false, 1000, std::nullopt, &error)); QVERIFY(error.contains(QStringLiteral("outside the supported price range")));
    }
    static QString SpreadText(const B3FlowMeshOrderBook& book)
    {
        const int row{book.model()->spreadRow()};
        return row < 0 ? QString{} : book.model()->row(row).price;
    }
    //! The snapshot with the node's levels of its own curves at its head.
    static Snapshot WithLevels(Snapshot snapshot, const flowmesh::LevelGrouping& grouping = {}, const std::optional<uint256>& own = std::nullopt)
    {
        auto data{Data()};
        auto head{data["snapshot"]};
        head.pushKV("last_microblock_hash", snapshot.head.toStdString()); head.pushKV("next_microblock_sequence", snapshot.next_sequence);
        data.pushKV("snapshot", head);
        data.pushKV("history", [] { UniValue h{UniValue::VOBJ}; h.pushKV("available", true); h.pushKV("truncated", false); h.pushKV("scope", "bounded-certified-log-cache"); h.pushKV("entries", UniValue{UniValue::VARR}); return h; }());
        data.pushKV("book_levels", LevelsJson(snapshot.curves, *uint256::FromHex(snapshot.head.toStdString()), grouping, own));
        snapshot.levels = Parse(data).levels;
        return snapshot;
    }
    void orderBookLadderShowsVerifiedLevelsBesideTheSpreadWithoutChurn_data()
    {
        QTest::addColumn<bool>("inverse");
        QTest::newRow("canonical") << false;
        QTest::newRow("inverse") << true;
    }
    void orderBookLadderShowsVerifiedLevelsBesideTheSpreadWithoutChurn()
    {
        QFETCH(bool, inverse);
        QApplication::setEffectEnabled(Qt::UI_General, true);
        B3FlowMeshOrderBook::SetAnimationSetting(true);
        // Sixteen levels per side from 32 accounts; the viewer holds the best bid.
        const auto snapshot{WithLevels(SyntheticLimitBook(16), {}, H(20))};
        QVERIFY(snapshot.levels && snapshot.levels->available);
        B3FlowMeshOrderBook book; book.resize(420, 440); book.setSnapshot(snapshot, inverse);
        book.show(); QCoreApplication::processEvents(); QCoreApplication::processEvents();
        QVERIFY(book.showsVerifiedLevels());
        auto* model{book.model()};
        const int per_side{book.rowsPerSide()};
        QVERIFY(per_side >= 5 && per_side < 16);
        QCOMPARE(model->rowCount(), 2 * per_side + 1);
        QCOMPARE(model->spreadRow(), per_side);
        // The best displayed ask sits right above the spread, the best bid
        // right below it; the book's best canonical bid is the B3 view's ask.
        const auto& levels{*snapshot.levels};
        const auto& shown_asks{inverse ? levels.bids.levels : levels.asks.levels};
        const auto& shown_bids{inverse ? levels.asks.levels : levels.bids.levels};
        // One decimal count for the column (the tick's, or the B3 view's
        // rounding), every price to the nearest value, never by side.
        const int places{Places(model->row(per_side - 1).price)};
        QCOMPARE(book.pricePlaces(), std::optional<int>{places});
        const auto label = [&](const BookLevel& level, bool) { return NearestDisplayPrice(level.low, 6, inverse, places); };
        QCOMPARE(model->row(per_side - 1).price, label(shown_asks[0], true));
        QCOMPARE(model->row(per_side + 1).price, label(shown_bids[0], false));
        QCOMPARE(model->row(0).price, label(shown_asks[static_cast<size_t>(per_side) - 1], true));
        QVERIFY(model->row(per_side - 1).kind == B3FlowMeshLadderModel::Row::Kind::Ask);
        QVERIFY(model->row(per_side + 1).kind == B3FlowMeshLadderModel::Row::Kind::Bid);
        QCOMPARE(model->data(model->index(per_side - 1, 0), Qt::ForegroundRole).value<QColor>(), B3Theme::kNegative);
        QCOMPARE(model->data(model->index(per_side + 1, 0), Qt::ForegroundRole).value<QColor>(), B3Theme::kPositive);
        // Total is cumulative from the best level.
        const int size_places{Places(model->row(per_side + 1).size)};
        if (!inverse) {
            QCOMPARE(model->row(per_side + 1).total, PadDecimals(FormatAmount(shown_bids[0].size, 6), size_places));
            QCOMPARE(model->row(per_side + 2).total, PadDecimals(FormatAmount(shown_bids[0].size + shown_bids[1].size, 6), size_places));
            QCOMPARE(model->row(per_side + 2).size, PadDecimals(FormatAmount(shown_bids[1].size, 6), size_places));
        } else {
            QCOMPARE(model->row(per_side + 1).size, PadDecimals(FormatAmount(static_cast<CAmount>(*shown_bids[0].notional), 9), size_places));
        }
        // One depth scale for both sides: the deeper side's last visible row is 1.
        const double ask_depth{model->row(0).depth}, bid_depth{model->row(2 * per_side).depth};
        QCOMPARE(std::max(ask_depth, bid_depth), 1.0);
        QVERIFY(std::min(ask_depth, bid_depth) > 0 && std::min(ask_depth, bid_depth) < 1);
        QVERIFY(model->row(per_side + 1).depth < model->row(per_side + 2).depth);
        // The viewer's own order is marked, with the count of orders.
        const int own_row{inverse ? per_side - 1 : per_side + 1};
        QVERIFY(model->data(model->index(own_row, 0), B3FlowMeshLadderModel::OwnRole).toBool());
        QVERIFY(model->row(own_row).tooltip.contains(QStringLiteral("Your order")));
        // Hover highlights from the spread to the hovered level.
        book.setHoverRow(own_row); QCOMPARE(book.hoverRow(), own_row); book.setHoverRow(-1);
        QVERIFY(model->row(own_row).tooltip.contains(QStringLiteral("1 standing order")));
        QVERIFY(model->data(model->index(own_row, 0), Qt::AccessibleTextRole).toString().contains(QStringLiteral("includes yours")));
        QVERIFY(!model->data(model->index(per_side + 2, 0), B3FlowMeshLadderModel::OwnRole).toBool() || inverse);
        // Rows fill the height: a taller view holds more, a new layout once.
        {
            QSignalSpy reset{model, &QAbstractItemModel::modelReset};
            book.resize(420, 640); QCoreApplication::processEvents(); QCoreApplication::processEvents();
            QVERIFY(book.rowsPerSide() > per_side);
            QCOMPARE(reset.count(), 1);
            book.resize(420, 440); QCoreApplication::processEvents(); QCoreApplication::processEvents();
            QCOMPARE(book.rowsPerSide(), per_side);
        }
        // Identical data: no reset, no row churn, no changed cell, no flash.
        QSignalSpy changed{model, &QAbstractItemModel::dataChanged};
        QSignalSpy reset{model, &QAbstractItemModel::modelReset};
        QSignalSpy removed{model, &QAbstractItemModel::rowsRemoved};
        QSignalSpy inserted{model, &QAbstractItemModel::rowsInserted};
        for (int i{0}; i < 5; ++i) { book.setSnapshot(Snapshot{snapshot}, inverse); book.setStale(false); }
        QCOMPARE(changed.count(), 0); QCOMPARE(reset.count(), 0); QCOMPARE(removed.count(), 0); QCOMPARE(inserted.count(), 0);
        QVERIFY(!book.flashing());
        // A size change updates cells in place and does not flash.
        auto resized{snapshot}; resized.curves.front().filled += 1'000; resized.curves.front().remaining -= 1'000;
        book.setSnapshot(WithLevels(resized, {}, H(20)), inverse);
        QVERIFY(changed.count() > 0); QCOMPARE(reset.count(), 0);
        QVERIFY(!book.flashing());
        // A new price flashes once; the timer stops after the flash.
        auto moved{resized}; moved.curves.push_back(LimitCurve("bid", 60, 999, 70'000));
        book.setSnapshot(WithLevels(moved, {}, H(20)), inverse);
        QVERIFY(book.flashing());
        const int new_row{inverse ? per_side - 2 : per_side + 2};
        QCOMPARE(model->row(new_row).price, NearestDisplayPrice(999, 6, inverse, Places(model->row(new_row).price)));
        QTRY_VERIFY_WITH_TIMEOUT(book.flashIntensity(new_row) > 0, 1000);
        QTRY_VERIFY_WITH_TIMEOUT(!book.flashing(), 3000);
        QCOMPARE(book.flashIntensity(new_row), 0.0);
        // With animation off, nothing flashes.
        B3FlowMeshOrderBook::SetAnimationSetting(false);
        auto again{moved}; again.curves.push_back(LimitCurve("ask", 61, 1013, 70'000));
        book.setSnapshot(WithLevels(again, {}, H(20)), inverse);
        QVERIFY(!book.flashing());
        B3FlowMeshOrderBook::SetAnimationSetting(true);
        QCOMPARE(reset.count(), 0); QCOMPARE(removed.count(), 0); QCOMPARE(inserted.count(), 0);
    }
    //! The decimal places of a displayed number ("0.120" has 3).
    static int Places(const QString& text)
    {
        const auto dot{text.indexOf(QLatin1Char('.'))};
        return dot < 0 ? 0 : static_cast<int>(text.size() - dot - 1);
    }
    //! A price text without its "≈" (the book's header carries it).
    static QString Bare(const QString& text) { return text.startsWith(QStringLiteral("≈")) ? text.mid(1) : text; }
    //! Every level row's price, size and total have one decimal count per
    //! column, and no cell carries its own "≈"; returns the price places.
    static int CheckOneDecimalCountPerColumn(const B3FlowMeshOrderBook& book)
    {
        std::array<std::optional<int>, 3> places;
        int rows{0};
        for (const auto& row : book.model()->rows()) {
            if (row.kind != B3FlowMeshLadderModel::Row::Kind::Ask && row.kind != B3FlowMeshLadderModel::Row::Kind::Bid) continue;
            ++rows;
            const std::array<QString, 3> cells{row.price, row.size, row.total};
            for (size_t c{0}; c < 3; ++c) {
                if (cells[c] == QStringLiteral("—")) continue;
                if (cells[c].contains(QStringLiteral("≈"))) { qWarning() << "per-row approximation" << cells[c]; return -1; }
                if (!places[c]) places[c] = Places(cells[c]);
                if (*places[c] != Places(cells[c])) { qWarning() << "column" << c << "mixes" << *places[c] << "and" << cells[c]; return -1; }
            }
        }
        if (rows < 2 || !places[0]) return -1;
        // Size and Total share one count, as Hyperliquid's do.
        if (places[1] && places[2] && *places[1] != *places[2]) { qWarning() << "size" << *places[1] << "total" << *places[2]; return -1; }
        return *places[0];
    }
    void orderBookColumnsShowOneDecimalCountAndApproximateOnlyInTheHeader()
    {
        const auto snapshot{WithLevels(SyntheticLimitBook(16), {}, H(20))};
        const auto& units{snapshot.units}; // 6 decimals, price step 1: 0.001 B3 per token
        B3FlowMeshOrderBook book; book.resize(420, 900); book.show(); QCoreApplication::processEvents();
        const auto* model{book.model()};
        const auto header = [&](int section, int role = Qt::DisplayRole) { return model->headerData(section, Qt::Horizontal, role).toString(); };
        // Canonical, one level per price: the market tick's three places,
        // every price exact, no "≈" anywhere.
        book.setSnapshot(snapshot, false);
        QVERIFY(book.showsVerifiedLevels());
        QCOMPARE(CheckOneDecimalCountPerColumn(book), 3);
        const int spread{model->spreadRow()};
        QCOMPARE(model->row(spread + 1).price, QStringLiteral("1.000")); // 1000 atoms
        QCOMPARE(model->row(spread - 1).price, QStringLiteral("1.010"));
        QCOMPARE(model->row(spread + 1).size, QStringLiteral("0.100")); // 100'000 atoms, and 0.125 below it
        QCOMPARE(header(0), QStringLiteral("Price (B3)"));
        // The B3 view: token-per-B3 prices do not end, so they are rounded
        // to one count for the column, to the nearest value on both sides
        // (the view's one rule), and "≈" is said once, in the header; each
        // exact price stays in its tooltip.
        book.setSnapshot(snapshot, true);
        const int places{CheckOneDecimalCountPerColumn(book)};
        QVERIFY(places > 0 && places <= 6);
        QCOMPARE(book.pricePlaces(), std::optional<int>{places});
        QCOMPARE(header(0), QStringLiteral("Price (tUSD) ≈"));
        QVERIFY(header(0, Qt::ToolTipRole).contains(QStringLiteral("rounded to %1 decimal places").arg(places)));
        QVERIFY(header(0, Qt::ToolTipRole).contains(QStringLiteral("never by side")));
        const auto& levels{*snapshot.levels};
        int rounded_rows{0};
        std::set<QString> labels;
        for (int row{0}; row < model->rowCount(); ++row) {
            const auto& shown{model->row(row)};
            if (shown.kind != B3FlowMeshLadderModel::Row::Kind::Ask && shown.kind != B3FlowMeshLadderModel::Row::Kind::Bid) continue;
            const bool ask_row{shown.kind == B3FlowMeshLadderModel::Row::Kind::Ask};
            // The B3 view's asks are canonical bids, best (highest) first.
            const auto& level{ask_row ? levels.bids.levels[static_cast<size_t>(spread - 1 - row)] : levels.asks.levels[static_cast<size_t>(row - spread - 1)]};
            // The nearest value, whichever side: the same text Your orders
            // and the spread use for this price.
            bool whole{false};
            QCOMPARE(shown.price, NearestDisplayPrice(level.low, 6, true, places, &whole));
            QCOMPARE(shown.price, Bare(ViewPrice(level.low, units, true, places).text));
            // One tick is resolved at these prices: every label is its own,
            // and a click offers exactly its own price.
            QVERIFY2(labels.insert(shown.price).second, qPrintable(shown.price));
            QCOMPARE(shown.click_canonical, level.low);
            // A rounded label keeps its exact price in the tooltip and says
            // "about" to a screen reader; an exact one (1000: 1) does not.
            QCOMPARE(shown.tooltip.contains(QStringLiteral("Exact: ") + ExactInversePrice(level.low, 6)), !whole);
            QCOMPARE(model->data(model->index(row, 0), Qt::AccessibleTextRole).toString().contains(QStringLiteral("about ") + shown.price), !whole);
            rounded_rows += !whole;
        }
        QVERIFY(rounded_rows > 10);
        // Grouped: the step's places, exact edges, no "≈".
        const auto canonical_grouping{Grouping(false, 1, -2, 6)};
        book.setSnapshot(WithLevels(SyntheticLimitBook(16), canonical_grouping, H(20)), false, canonical_grouping);
        QVERIFY(book.showsVerifiedLevels());
        QCOMPARE(CheckOneDecimalCountPerColumn(book), 2);
        QCOMPARE(model->row(model->spreadRow() + 1).price, QStringLiteral("1.00"));
        QCOMPARE(header(0), QStringLiteral("Price (B3)"));
        const auto inverse_grouping{Grouping(true, 1, -4, 6)};
        book.setSnapshot(WithLevels(SyntheticLimitBook(16), inverse_grouping, H(20)), true, inverse_grouping);
        QVERIFY(book.showsVerifiedLevels());
        QCOMPARE(CheckOneDecimalCountPerColumn(book), 4);
        QCOMPARE(model->row(model->spreadRow() - 1).price, QStringLiteral("1.0000"));
        QCOMPARE(header(0), QStringLiteral("Price (tUSD)"));
        // Prices that happen to end early (800 atoms: 1.25; 1000: 1) still
        // show the tick precision around them: telling 799, 800 and 801
        // apart needs three places, not the two those prices alone would.
        {
            auto short_book{SyntheticLimitBook(1)};
            short_book.curves = {LimitCurve("bid", 20, 800, 100'000), LimitCurve("ask", 80, 1000, 100'000)};
            book.setSnapshot(WithLevels(short_book, {}, H(20)), true);
            QVERIFY(book.showsVerifiedLevels());
            QCOMPARE(CheckOneDecimalCountPerColumn(book), 3);
            QCOMPARE(model->row(model->spreadRow() - 1).price, QStringLiteral("1.250"));
            QCOMPARE(model->row(model->spreadRow() + 1).price, QStringLiteral("1.000"));
            QCOMPARE(header(0), QStringLiteral("Price (tUSD)"));
        }
        // Without the node's levels the fallback rows follow the same rule.
        book.setSnapshot(SyntheticLimitBook(16), true);
        QVERIFY(!book.showsVerifiedLevels());
        QVERIFY(CheckOneDecimalCountPerColumn(book) > 0);
        QCOMPARE(header(0), QStringLiteral("Price (tUSD) ≈"));
        book.setSnapshot(SyntheticLimitBook(16), false);
        QCOMPARE(CheckOneDecimalCountPerColumn(book), 3);
        QCOMPARE(header(0), QStringLiteral("Price (B3)"));
    }
    void fixedDisplayPricesRoundOnlyInTheirDirection()
    {
        // 1000/1003 tUSD per B3 = 0.997008973...
        bool exact{true};
        QCOMPARE(FixedDisplayPrice(1003, 6, true, 4, true, &exact), QStringLiteral("0.9971")); QVERIFY(!exact);
        QCOMPARE(FixedDisplayPrice(1003, 6, true, 4, false, &exact), QStringLiteral("0.9970")); QVERIFY(!exact);
        QCOMPARE(FixedDisplayPrice(1000, 6, true, 4, true, &exact), QStringLiteral("1.0000")); QVERIFY(exact);
        QCOMPARE(FixedDisplayPrice(1000, 6, false, 3, false, &exact), QStringLiteral("1.000")); QVERIFY(exact);
        QCOMPARE(FixedDisplayPrice(1234, 6, false, 2, true, &exact), QStringLiteral("1.24")); QVERIFY(!exact);
        QCOMPARE(FixedDisplayPrice(1234, 6, false, 0, false, &exact), QStringLiteral("1"));
        QCOMPARE(FixedDisplayPrice(7, 12, false, 2, false, &exact), QStringLiteral("7000.00")); QVERIFY(exact);
        // No finite inverse of price zero; invalid places.
        QVERIFY(FixedDisplayPrice(0, 6, true, 4, true).isEmpty());
        QVERIFY(FixedDisplayPrice(1000, 6, true, 19, true).isEmpty());
        QCOMPARE(FixedDisplayPrice(1, 0, true, 18, true), QStringLiteral("1000000000.000000000000000000"));
        QCOMPARE(PadDecimals(QStringLiteral("1"), 3), QStringLiteral("1.000"));
        QCOMPARE(PadDecimals(QStringLiteral("0.5"), 3), QStringLiteral("0.500"));
        QCOMPARE(PadDecimals(QStringLiteral("12.3456"), 2), QStringLiteral("12.3456")); // never truncates
        QCOMPARE(NeededDecimals(util::Unsigned128{120'000}, 6), 2);
        QCOMPARE(NeededDecimals(util::Unsigned128{0}, 6), 0);
        QCOMPARE(NeededDecimals(util::Unsigned128{1}, 9), 9);
    }
    void viewPricesRoundToTheNearestValueAtOneCountOfPlaces()
    {
        const auto units{SyntheticLimitBook().units}; // 6 decimals, price step 1
        // To the nearest value, half up, never by side: 1000/997 is 1.003
        // (rounded up it was 1.004, the next tick's value).
        bool exact{true};
        QCOMPARE(NearestDisplayPrice(997, 6, true, 3, &exact), QStringLiteral("1.003")); QVERIFY(!exact);
        QCOMPARE(FixedDisplayPrice(997, 6, true, 3, /*up=*/true), QStringLiteral("1.004"));
        QCOMPARE(NearestDisplayPrice(1003, 6, true, 3), QStringLiteral("0.997"));
        QCOMPARE(NearestDisplayPrice(800, 6, true, 1, &exact), QStringLiteral("1.3")); QVERIFY(!exact); // 1.25, half up
        QCOMPARE(NearestDisplayPrice(1000, 6, false, 3, &exact), QStringLiteral("1.000")); QVERIFY(exact);
        // A view's places. B3 per token: the tick's. Token per B3: the fewest
        // at which the best prices and their neighbouring ticks differ.
        QCOMPARE(ViewPricePlaces(units, false, {}), std::optional<int>{3});
        QCOMPARE(ViewPricePlaces(units, true, {999, 1001}), std::optional<int>{3});
        // 799, 800 and 801 need three places although 800 is exactly 1.25.
        QCOMPARE(ViewPricePlaces(units, true, {800, 1000}), std::optional<int>{3});
        // 1999, 2000, 2001: 0.5003, 0.5000, 0.4998.
        QCOMPARE(ViewPricePlaces(units, true, {2000}), std::optional<int>{4});
        QVERIFY(!ViewPricePlaces(units, true, {}));
        // A price at a view's places: "≈" and the exact form when rounded.
        const auto rounded{ViewPrice(997, units, true, 3)};
        QCOMPARE(rounded.text, QStringLiteral("≈1.003")); QCOMPARE(rounded.exact, QStringLiteral("1000/997")); QVERIFY(rounded.approximate);
        QCOMPARE(ViewPrice(1000, units, true, 3).text, QStringLiteral("1.000")); QVERIFY(!ViewPrice(1000, units, true, 3).approximate);
        QCOMPARE(ViewPrice(1010, units, false, 3).text, QStringLiteral("1.010"));
        // Without places, or too small for them (never "≈0.000"): DisplayPrice.
        QCOMPARE(ViewPrice(997, units, true, std::nullopt), DisplayPrice(997, units, true));
        QCOMPARE(ViewPrice(1'000'000'000'000, units, true, 3), DisplayPrice(1'000'000'000'000, units, true));
        // The spread: the difference of the two prices as shown.
        const auto spread{ViewSpread(999, 1001, units, true, 3)};
        QCOMPARE(spread.text, QStringLiteral("≈0.002")); QVERIFY(spread.approximate); // 1.001 - 0.999
        QCOMPARE(spread.exact, DisplaySpread(999, 1001, units, true).exact);
        QCOMPARE(ViewSpread(1000, 1010, units, false, 3).text, QStringLiteral("0.010"));
        QCOMPARE(ViewSpread(999, 1001, units, true, std::nullopt), DisplaySpread(999, 1001, units, true));
        // Rounding a decimal the ticket shows to the places of a label.
        QCOMPARE(RoundedDecimal(QStringLiteral("1.0031"), 3), QStringLiteral("1.003"));
        QCOMPARE(RoundedDecimal(QStringLiteral("1.0035"), 3), QStringLiteral("1.004"));
        QCOMPARE(RoundedDecimal(QStringLiteral("1"), 2), QStringLiteral("1.00"));
        for (const auto* text : {"1/3", "≈1.003", "0", "", "1.2.3"}) QVERIFY2(RoundedDecimal(QString::fromUtf8(text), 3).isEmpty(), text);
    }
    void aPriceReadsTheSameInItsRowYourOrdersTheSpreadAndTheTicket()
    {
        // Review of 2026-09-30, the default B3 view at 1320 x 940: Your
        // orders showed the viewer's sell of B3 at ≈1.003, the book marked
        // it on the row labelled 1.004 and showed another price as 1.003,
        // and the labels beside the spread (1.002 and 0.999) did not differ
        // by the spread it showed (≈0.002). One rule for the view: to the
        // nearest value at one count of places, never by side.
        B3FlowMeshTradingPanel panel; panel.resize(1320, 940); panel.m_timer->stop();
        ShowSyntheticTimeMarket(panel, SyntheticSource::LOCAL);
        QVERIFY(panel.inverted()); QVERIFY(panel.m_order_book->showsVerifiedLevels());
        const auto* model{panel.m_order_book->model()};
        const auto& units{panel.m_snapshot->units};
        const auto places{panel.m_order_book->pricePlaces()};
        QCOMPARE(places, std::optional<int>{3});
        const int spread{model->spreadRow()};
        std::set<QString> sells, buys;
        int own_row{-1};
        for (int row{0}; row < model->rowCount(); ++row) {
            const auto& shown{model->row(row)};
            if (shown.kind != B3FlowMeshLadderModel::Row::Kind::Ask && shown.kind != B3FlowMeshLadderModel::Row::Kind::Bid) continue;
            // Each row: its level's price to the nearest value, its own label.
            QCOMPARE(shown.price, NearestDisplayPrice(shown.click_canonical, units.decimals, true, *places));
            QVERIFY2((shown.kind == B3FlowMeshLadderModel::Row::Kind::Ask ? sells : buys).insert(shown.price).second, qPrintable(shown.price));
            if (shown.own) own_row = row;
        }
        // The viewer's sell of B3 (a canonical bid at 997 atoms): marked on
        // the row shown 1.003, and Your orders shows the same price.
        QVERIFY(own_row >= 0 && own_row < spread);
        QCOMPARE(model->row(own_row).click_canonical, CAmount{997});
        QCOMPARE(model->row(own_row).price, QStringLiteral("1.003"));
        QCOMPARE(panel.m_own_view->item(0, 0)->text(), QStringLiteral("Sell B3"));
        QCOMPARE(panel.m_own_view->item(0, 1)->text(), QStringLiteral("≈%1 tUSD / B3").arg(model->row(own_row).price));
        // The spread row: the difference of the two labels beside it.
        const auto scaled = [](QString text) { text.remove(QLatin1Char('.')); return text.toLongLong(); };
        const auto above{model->row(spread - 1).price}, below{model->row(spread + 1).price};
        QCOMPARE(above, QStringLiteral("1.001")); QCOMPARE(below, QStringLiteral("0.999"));
        QCOMPARE(SpreadText(*panel.m_order_book), QStringLiteral("Spread  ≈%1 tUSD · 0.2%").arg(FormatAmount(scaled(above) - scaled(below), *places)));
        // A click on the viewer's row fills that price: as shown when the
        // ticket reads it back exactly (sell B3), else with more places that
        // still show as it (buy B3: 1.0031, never the next row's 1.004).
        for (const int side : {1, 0}) {
            panel.m_side->setCurrentIndex(side);
            QVERIFY(panel.m_order_book->activateRow(own_row));
            const auto text{panel.m_price->text()};
            const auto parsed{ParseDisplayLimit(text, units, true, /*display_buy=*/side == 0)};
            QVERIFY2(parsed && parsed->price == 997, qPrintable(text));
            QCOMPARE(RoundedDecimal(text, *places), model->row(own_row).price);
            QCOMPARE(text, side == 1 ? QStringLiteral("1.003") : QStringLiteral("1.0031"));
        }
        QVERIFY(!panel.m_thread); QVERIFY(!panel.m_confirmation); QVERIFY(!panel.m_wallet);
    }
    void levelsWhosePricesRoundToOneValueShareARow()
    {
        // Far from the market two canonical ticks can round to one value at
        // the view's places (set by the best prices): 1000/1428 and 1000/1429
        // tUSD per B3 are both 0.700. They share one row, never two rows
        // with one label, each exact price and size in its tooltip.
        auto snapshot{SyntheticLimitBook(4)};
        snapshot.curves.push_back(LimitCurve("ask", 90, 1428, 100'000));
        snapshot.curves.push_back(LimitCurve("ask", 91, 1429, 200'000));
        const auto levelled{WithLevels(snapshot, {}, H(20))};
        B3FlowMeshOrderBook book; book.resize(420, 900); book.show(); QCoreApplication::processEvents();
        book.setSnapshot(levelled, true);
        QVERIFY(book.showsVerifiedLevels());
        QCOMPARE(book.pricePlaces(), std::optional<int>{3});
        const auto* model{book.model()};
        std::set<QString> buys;
        int shared{-1};
        for (int row{0}; row < model->rowCount(); ++row) {
            if (model->row(row).kind != B3FlowMeshLadderModel::Row::Kind::Bid) continue;
            QVERIFY2(buys.insert(model->row(row).price).second, qPrintable(model->row(row).price));
            if (model->row(row).price == QStringLiteral("0.700")) shared = row;
        }
        QCOMPARE(buys.size(), size_t{5}); // 0.990, 0.988, 0.986, 0.984 and 0.700
        QVERIFY(shared > model->spreadRow());
        const auto& row{model->row(shared)};
        // Its size: both levels' B3 at their own prices, 0.1428 + 0.2858.
        QCOMPARE(row.size, PadDecimals(QStringLiteral("0.4286"), Places(row.size)));
        QVERIFY(row.tooltip.contains(QStringLiteral("2 prices round to this value")));
        QVERIFY(row.tooltip.contains(QStringLiteral("Exact %1: 0.1 tUSD").arg(ExactInversePrice(1428, 6))));
        QVERIFY(row.tooltip.contains(QStringLiteral("Exact %1: 0.2 tUSD").arg(ExactInversePrice(1429, 6))));
        QVERIFY(model->data(model->index(shared, 0), Qt::AccessibleTextRole).toString().contains(QStringLiteral("about 0.700")));
        // A click offers the price that takes every level of the row: its
        // deepest, with the total up to it.
        QCOMPARE(row.click_canonical, CAmount{1429});
        QCOMPARE(row.click_cumulative, levelled.levels->asks.levels.back().cumulative);
        QVERIFY(row.tooltip.contains(QStringLiteral("exactly %1 tUSD / B3").arg(ExactInversePrice(1429, 6))));
        // In the canonical view both prices are exact: two rows.
        book.setSnapshot(levelled, false);
        int canonical_rows{0};
        for (const auto& shown : model->rows()) canonical_rows += shown.price == QStringLiteral("1.428") || shown.price == QStringLiteral("1.429");
        QCOMPARE(canonical_rows, 2);
    }
    void identicalBookUpdatesDoNotRepaintTheLadder()
    {
        // In-place updates: data that did not change never repaints the
        // ladder (a repaint of its whole area on every read otherwise).
        const auto snapshot{WithLevels(SyntheticLimitBook(16), {}, H(20))};
        B3FlowMeshOrderBook book; book.resize(420, 440); book.setSnapshot(snapshot, true);
        book.show(); for (int i{0}; i < 3; ++i) QCoreApplication::processEvents();
        QCOMPARE(book.view()->columnSpan(book.rowsPerSide(), 0), 3);
        int paints{0};
        book.setViewObserver([&](const QString& head, bool painted) { QVERIFY(!head.isEmpty()); if (painted) ++paints; });
        for (int i{0}; i < 5; ++i) { book.setSnapshot(Snapshot{snapshot}, true); QCoreApplication::processEvents(); }
        QCOMPARE(paints, 0);
        // A real change still repaints.
        auto moved{snapshot}; moved.curves.front().filled += 1'000; moved.curves.front().remaining -= 1'000;
        book.setSnapshot(WithLevels(moved, {}, H(20)), true); QCoreApplication::processEvents();
        QVERIFY(paints > 0);
        QCOMPARE(book.view()->columnSpan(book.rowsPerSide(), 0), 3);
    }
    void bookFlashAnimatesAtThirtyFramesASecond()
    {
        // A new price flashes for FLASH_MS: its frames repaint the ladder at
        // most 30 times a second (one timer, only while a flash runs).
        QApplication::setEffectEnabled(Qt::UI_General, true);
        B3FlowMeshOrderBook::SetAnimationSetting(true);
        auto snapshot{WithLevels(SyntheticLimitBook(16), {}, H(20))};
        B3FlowMeshOrderBook book; book.resize(420, 440); book.setSnapshot(snapshot, false);
        book.show(); for (int i{0}; i < 3; ++i) QCoreApplication::processEvents();
        int paints{0};
        book.setViewObserver([&](const QString&, bool painted) { if (painted) ++paints; });
        auto moved{snapshot}; moved.curves.push_back(LimitCurve("bid", 60, 999, 70'000));
        QElapsedTimer flash; flash.start();
        book.setSnapshot(WithLevels(moved, {}, H(20)), false);
        QVERIFY(book.flashing());
        // One frame timer, at 30 frames a second, running only during a flash.
        const auto timers{book.findChildren<QTimer*>()};
        const auto frame{std::find_if(timers.begin(), timers.end(), [](const QTimer* t) { return t->isActive(); })};
        QVERIFY(frame != timers.end());
        QCOMPARE((*frame)->interval(), B3FlowMeshOrderBook::FLASH_STEP_MS);
        QCOMPARE(std::count_if(timers.begin(), timers.end(), [](const QTimer* t) { return t->isActive(); }), qsizetype{1});
        QTRY_VERIFY_WITH_TIMEOUT(!book.flashing(), 10000);
        QVERIFY(!(*frame)->isActive());
        const auto seconds{static_cast<double>(flash.elapsed()) / 1000.0};
        QVERIFY(paints >= 1);
        // The measured frame rate depends on the machine keeping up.
        if (UiTimingStrict()) {
            QVERIFY2(paints / seconds <= 34.0, qPrintable(QStringLiteral("%1 paints in %2 s").arg(paints).arg(seconds)));
            QVERIFY(paints >= 5); // It does animate.
        }
    }
    void orderBookLadderFallsBackLabelledAndRequiresVerifiedState()
    {
        const auto original{Parse(RemoteData())};
        B3FlowMeshOrderBook book; book.resize(420, 400); book.show(); QCoreApplication::processEvents();
        // Exact until the panel offers choices for a price.
        QCOMPARE(book.groupingSelector()->count(), 1); QCOMPARE(book.groupingSelector()->currentText(), QStringLiteral("Exact"));
        book.setSnapshot(original, false);
        auto* last{book.findChild<QLabel*>(QStringLiteral("flowMeshBookLast"))};
        auto* note{book.findChild<QLabel*>(QStringLiteral("flowMeshBookNote"))};
        QVERIFY(last); QVERIFY(note);
        const auto levels = [&] {
            size_t count{0};
            for (const auto& row : book.model()->rows()) count += row.kind == B3FlowMeshLadderModel::Row::Kind::Ask || row.kind == B3FlowMeshLadderModel::Row::Kind::Bid;
            return count;
        };
        // Without the node's levels: the first page's limit orders, labelled.
        QVERIFY(!book.showsVerifiedLevels());
        QCOMPARE(levels(), size_t{2});
        QVERIFY(note->text().contains(QStringLiteral("first curve page")));
        QVERIFY(last->text().startsWith(QStringLiteral("Reported last")));
        for (const int fault : {0, 1, 2, 3, 4}) {
            auto snapshot{WithLevels(original)};
            if (fault == 0) snapshot.units.known = false;
            if (fault == 1) snapshot.units.asset = QString::fromStdString(H(99).GetHex());
            if (fault == 2) snapshot.certificate_verified = false;
            if (fault == 3) snapshot.account_state_verified = false;
            if (fault == 4) snapshot.certified = false;
            book.setSnapshot(snapshot, false);
            QCOMPARE(levels(), size_t{0});
            QVERIFY(last->text().contains(QStringLiteral("—"))); QVERIFY(SpreadText(book).contains(QStringLiteral("—")));
        }
        book.setSnapshot(std::nullopt, false);
        QCOMPARE(levels(), size_t{0});
        // Levels of another grouping (a read still under way) are not shown.
        book.setSnapshot(WithLevels(original, Grouping(false, 1, -2, 6)), false);
        QVERIFY(!book.showsVerifiedLevels()); QVERIFY(note->text().contains(QStringLiteral("Updating levels")));
        book.setSnapshot(WithLevels(original, Grouping(false, 1, -2, 6)), false, Grouping(false, 1, -2, 6));
        QVERIFY(book.showsVerifiedLevels());
        // Levels the node could not compute: the reason, and the fallback.
        auto unavailable{WithLevels(original)}; unavailable.levels->available = false; unavailable.levels->reason = QStringLiteral("work_budget");
        book.setSnapshot(unavailable, false);
        QVERIFY(!book.showsVerifiedLevels()); QVERIFY(note->text().contains(QStringLiteral("unavailable (work_budget)")));
        auto partial{original}; partial.curves_complete = false;
        book.setSnapshot(partial, true);
        QCOMPARE(levels(), size_t{2});
        QVERIFY(note->text().contains(QStringLiteral("Partial"))); QVERIFY(note->text().contains(QStringLiteral("gross")));
        QVERIFY(SpreadText(book).contains(QStringLiteral("partial")));
        auto general{original}; auto slope{LimitCurve("bid", 12, 100, 10)};
        slope.points = {{99, 10}, {101, 0}}; general.curves.push_back(slope);
        book.setSnapshot(general, false);
        QVERIFY(note->text().contains(QStringLiteral("non-limit"))); QVERIFY(SpreadText(book).contains(QStringLiteral("partial")));
        // With the node's levels the same general curve is exact.
        book.setSnapshot(WithLevels(general), false);
        QVERIFY(book.showsVerifiedLevels()); QVERIFY(!note->text().contains(QStringLiteral("non-limit")));
        QCOMPARE(levels(), size_t{4}); // 99, 100 (the slope) and the limit bid and ask
        book.setSnapshot(original, false); book.setStale(true);
        auto* stale{book.findChild<QLabel*>(QStringLiteral("flowMeshBookStale"))}; QVERIFY(stale); QVERIFY(!stale->isHidden());
        QCOMPARE(levels(), size_t{2}); // Retained, not silently emptied.
    }
    void clickingABookLevelFillsTheTicketPriceAndSignsNothing_data()
    {
        QTest::addColumn<bool>("inverse");
        QTest::addColumn<bool>("grouped");
        QTest::newRow("canonical exact") << false << false;
        QTest::newRow("canonical grouped") << false << true;
        QTest::newRow("inverse exact") << true << false;
        QTest::newRow("inverse grouped") << true << true;
    }
    void clickingABookLevelFillsTheTicketPriceAndSignsNothing()
    {
        QFETCH(bool, inverse);
        QFETCH(bool, grouped);
        FundingReadProbe probe;
        B3FlowMeshTradingPanel panel; panel.resize(1320, 940); AttachOfflineWallet(panel);
        auto snapshot{SyntheticLimitBook(10)};
        // 0.01 B3 per token (10 ticks), or 0.0001 token per B3 (s = 7).
        const auto grouping{grouped ? (inverse ? Grouping(true, 1, -4, 6) : Grouping(false, 1, -2, 6)) : flowmesh::LevelGrouping{}};
        snapshot = WithLevels(snapshot, grouping);
        Observe(panel, snapshot, inverse);
        if (grouped) panel.m_book_groupings[{panel.m_snapshot->market, inverse}] = grouping;
        panel.updateDataViews();
        panel.show(); QCoreApplication::processEvents();
        QVERIFY(panel.m_order_book->showsVerifiedLevels());
        auto* model{panel.m_order_book->model()};
        const int spread{model->spreadRow()};
        panel.m_quantity->setText(QStringLiteral("0.1"));
        for (const bool buy : {true, false}) {
            (buy ? panel.m_buy : panel.m_sell)->click();
            for (const int row : {spread - 1, spread + 1, spread + 3}) {
                panel.m_marketable->setChecked(true);
                const auto shown{model->row(row).price};
                QCOMPARE(model->row(row).click_price, shown);
                QVERIFY(panel.m_order_book->activateRow(row));
                const auto parsed{ParseDisplayLimit(panel.m_price->text(), panel.m_snapshot->units, inverse, buy)};
                QVERIFY(parsed);
                QCOMPARE(panel.m_side->currentIndex(), buy ? 0 : 1);
                QVERIFY(!panel.m_marketable->isChecked());
                // Grouped labels are an edge every order of the level accepts;
                // the executable limit is exactly that canonical edge.
                const bool ask_row{row < spread};
                const auto& level{ask_row ? (inverse ? panel.m_snapshot->levels->bids.levels : panel.m_snapshot->levels->asks.levels)[static_cast<size_t>(spread - 1 - row)]
                                          : (inverse ? panel.m_snapshot->levels->asks.levels : panel.m_snapshot->levels->bids.levels)[static_cast<size_t>(row - spread - 1)]};
                const bool canonical_bid{ask_row == inverse};
                QCOMPARE(parsed->price, canonical_bid ? level.low : level.high);
                QCOMPARE(parsed->price, model->row(row).click_canonical);
                // Hyperliquid's click-to-price: the price as shown whenever
                // the ticket reads it back, for its side, as exactly the
                // level's canonical price, and always in the canonical view.
                // Otherwise the same places (a grouped edge) or more, rounded
                // toward the ticket's side: never an exact fraction here, and
                // for an exact level (its label the nearest value) still the
                // price the row shows (1.0021 for a row shown 1.002, never
                // another row's 1.003).
                const auto shown_reads{ParseDisplayLimit(shown, panel.m_snapshot->units, inverse, buy)};
                if ((shown_reads && shown_reads->price == model->row(row).click_canonical) || !inverse) {
                    QCOMPARE(panel.m_price->text(), shown);
                } else {
                    // A grouped edge may take the same places, rounded the other way.
                    QVERIFY2(Places(panel.m_price->text()) >= Places(shown) + (grouped ? 0 : 1), qPrintable(panel.m_price->text() + QStringLiteral(" / ") + shown));
                    QVERIFY(!panel.m_price->text().contains(QLatin1Char('/')));
                    if (!grouped) QCOMPARE(RoundedDecimal(panel.m_price->text(), Places(shown)), shown);
                }
                // A rounded B3-view text is adjusted only in the customer's
                // favour, and the ticket shows the exact executable limit.
                QVERIFY(!parsed->adjusted || inverse);
                if (inverse) {
                    QVERIFY2(panel.m_ticket_total->text().contains((parsed->adjusted ? QStringLiteral("Safely adjusted executable limit: ") : QStringLiteral("Exact executable limit: ")) +
                                                                  ExactInversePrice(parsed->price, 6)), qPrintable(panel.m_ticket_total->text()));
                }
            }
        }
        // Nothing opens a review, signs or reads for it.
        QVERIFY(!panel.m_thread); QVERIFY(!panel.m_confirmation); QVERIFY(!panel.m_deferred_review);
        QCOMPARE(probe.writes.load(), 0); QVERIFY(m_wallet->IsLocked());
        // The review of a clicked price shows exactly the level's canonical
        // limit, the only price that would be signed, and any adjustment of
        // the shown text (only in the customer's favour).
        bool adjusted_reviewed{false};
        for (const bool buy : {true, false}) {
            (buy ? panel.m_buy : panel.m_sell)->click();
            // The second sell level (998 atoms: 1000/998 tUSD per B3 does not end).
            QVERIFY(panel.m_order_book->activateRow(spread - 2));
            const auto shown{panel.m_price->text()};
            const auto parsed{ParseDisplayLimit(shown, panel.m_snapshot->units, inverse, buy)};
            QVERIFY(parsed); QCOMPARE(parsed->price, model->row(spread - 2).click_canonical);
            adjusted_reviewed |= parsed->adjusted;
            QString review;
            QTimer::singleShot(0, &panel, [&] {
                if (panel.m_confirmation) { review = panel.m_confirmation->text(); panel.m_confirmation->done(QMessageBox::Cancel); }
            });
            panel.begin(B3FlowMeshTrading::Operation::Order);
            QVERIFY2(!review.isEmpty(), qPrintable(panel.m_log->toPlainText().right(400)));
            if (inverse) {
                QVERIFY2(review.contains(QStringLiteral("Exact canonical limit: %1 B3 atoms per token atom").arg(parsed->price)), qPrintable(review));
                QCOMPARE(review.contains(QStringLiteral("Your entered limit was %1 tUSD / B3").arg(shown)), parsed->adjusted);
            } else {
                QVERIFY2(review.contains(QStringLiteral(" at %1 B3 / tUSD").arg(FormatPrice(parsed->price, 6))), qPrintable(review));
            }
            QVERIFY(!panel.m_confirmation); QVERIFY(!panel.m_thread);
        }
        QCOMPARE(adjusted_reviewed, inverse);
        QCOMPARE(probe.writes.load(), 0); QVERIFY(m_wallet->IsLocked());
        // Ignored while busy, while a review waits, or for another head.
        const auto before{panel.m_price->text()};
        const int other{spread + 2};
        panel.m_busy = true; QVERIFY(panel.m_order_book->activateRow(other)); QCOMPARE(panel.m_price->text(), before); panel.m_busy = false;
        panel.fillFromBook(model->row(other).click_price, model->row(other).click_canonical, 1, QString::fromStdString(H(77).GetHex()));
        QCOMPARE(panel.m_price->text(), before);
        // The spread row and padding are not clickable.
        QVERIFY(!panel.m_order_book->activateRow(spread));
        // Size on click (off by default): the cumulative size, within the balance.
        QVERIFY(!B3FlowMeshOrderBook::SizeOnClick());
        panel.m_quantity->setText(QStringLiteral("1"));
        panel.m_order_book->activateRow(spread + 1);
        QCOMPARE(panel.m_quantity->text(), QStringLiteral("1"));
        B3FlowMeshOrderBook::SetSizeOnClick(true);
        panel.m_sell->click(); // An ask: the base balance (2 tUSD) caps it.
        panel.m_order_book->activateRow(spread + 3);
        const auto cumulative{model->row(spread + 3).click_cumulative};
        QCOMPARE(panel.m_quantity->text(), FormatAmount(std::min<CAmount>(cumulative, inverse ? cumulative : 2'000'000), 6));
        B3FlowMeshOrderBook::SetSizeOnClick(false);
        QVERIFY(!panel.m_thread); QCOMPARE(probe.writes.load(), 0);
    }
    void clickToPriceUsesOnlyCurrentReadyVerifiedData()
    {
        // Critique B2: a clicked level's price goes into a field that is
        // signed after review, so a click is refused, with a notice, unless
        // the book shows the gates' own data (its head, this orientation),
        // that data is ready, and the price is one it offers.
        B3FlowMeshTradingPanel panel; panel.resize(1320, 940); AttachOfflineWallet(panel);
        Observe(panel, WithLevels(SyntheticLimitBook(10)), false);
        panel.show(); QCoreApplication::processEvents();
        QVERIFY(panel.m_order_book->showsVerifiedLevels());
        auto* model{panel.m_order_book->model()};
        const int row{model->spreadRow() + 1};
        const auto offered{model->row(row).click_price};
        const auto refused = [&](const std::function<void()>& change, const std::function<void()>& restore) {
            const auto notices = [&] { return panel.m_log->toPlainText().count(QStringLiteral("its price was not used")); };
            panel.m_price->setText(QStringLiteral("7")); const auto before{notices()};
            change();
            panel.m_order_book->activateRow(row);
            const bool unchanged{panel.m_price->text() == QStringLiteral("7")};
            const bool noticed{notices() == before + 1};
            restore();
            return unchanged && noticed;
        };
        // Ready and current: the price is used.
        QVERIFY(panel.m_order_book->activateRow(row)); QCOMPARE(panel.m_price->text(), offered);
        // Not ready: a failed read, stale data, a paused market.
        QVERIFY(refused([&] { panel.m_read_failed = true; }, [&] { panel.m_read_failed = false; }));
        QVERIFY(refused([&] { panel.m_response_age.invalidate(); }, [&] { panel.m_response_age.start(); }));
        QVERIFY(refused([&] { panel.m_snapshot->paused = true; }, [&] { panel.m_snapshot->paused = false; }));
        // Another head behind the book, or another orientation.
        const auto head{panel.m_snapshot->head};
        QVERIFY(refused([&] { panel.m_snapshot->head = QString::fromStdString(H(77).GetHex()); }, [&] { panel.m_snapshot->head = head; }));
        QVERIFY(refused([&] { QSignalBlocker blocked{panel.m_orientation}; panel.m_orientation->setCurrentIndex(1); },
                        [&] { QSignalBlocker blocked{panel.m_orientation}; panel.m_orientation->setCurrentIndex(0); }));
        // The price is re-derived from the data: one it no longer offers is refused.
        const auto levels{panel.m_snapshot->levels};
        QVERIFY(refused([&] { panel.m_snapshot->levels->bids.levels.clear(); panel.m_snapshot->levels->asks.levels.clear(); },
                        [&] { panel.m_snapshot->levels = levels; }));
        // Back to current and ready: used again.
        panel.m_price->clear();
        QVERIFY(panel.m_order_book->activateRow(row)); QCOMPARE(panel.m_price->text(), offered);
        QVERIFY(!panel.m_thread); QVERIFY(!panel.m_confirmation); QVERIFY(m_wallet->IsLocked());
    }
    void groupedBookLevelsAreLabelledWithAnEdgeEveryOrderAccepts()
    {
        B3FlowMeshOrderBook book; book.resize(420, 400); book.show(); QCoreApplication::processEvents();
        // Bids 1000, 998, ... and asks 1010, 1012, ... in 0.01 B3 (10-tick)
        // buckets: a buy labelled with its lower edge, a sell with its upper.
        const auto grouping{Grouping(false, 1, -2, 6)};
        book.setSnapshot(WithLevels(SyntheticLimitBook(8), grouping), false, grouping);
        QVERIFY(book.showsVerifiedLevels());
        const auto* model{book.model()};
        const int spread{model->spreadRow()};
        QCOMPARE(model->row(spread - 1).price, QStringLiteral("1.01"));  // asks 1001-1010: 1010
        QCOMPARE(model->row(spread - 2).price, QStringLiteral("1.02"));  // 1011-1020
        QCOMPARE(model->row(spread + 1).price, QStringLiteral("1.00"));  // bids 1000-1009: 1000
        QCOMPARE(model->row(spread + 2).price, QStringLiteral("0.99"));  // 990-999
        QVERIFY(model->row(spread - 1).tooltip.startsWith(QStringLiteral("Sell 1.01 B3 / tUSD")));
        QVERIFY(model->row(spread + 1).tooltip.startsWith(QStringLiteral("Buy 1.00 B3 / tUSD")));
        QCOMPARE(model->row(spread - 1).click_price, QStringLiteral("1.01"));
        QCOMPARE(model->row(spread - 1).click_canonical, CAmount{1010});
        QCOMPARE(model->row(spread + 2).click_price, QStringLiteral("0.99"));
        QCOMPARE(model->row(spread + 2).click_canonical, CAmount{990});
        // The B3 view: a canonical bid is a displayed sell, labelled with the
        // upper token-per-B3 edge; its exact price is its lowest canonical one.
        const auto inverse{Grouping(true, 1, -4, 6)}; // 0.0001 token per B3
        book.setSnapshot(WithLevels(SyntheticLimitBook(8), inverse), true, inverse);
        QVERIFY(book.showsVerifiedLevels());
        QVERIFY(model->row(spread - 1).tooltip.startsWith(QStringLiteral("Sell ")));
        QCOMPARE(model->row(spread - 1).price, QStringLiteral("1.0000")); // 1000 atoms: exactly 1 token per B3
        QCOMPARE(model->row(spread - 1).click_price, QStringLiteral("1.0000"));
        QCOMPARE(model->row(spread - 1).click_canonical, CAmount{1000});
        QVERIFY(model->row(spread + 1).tooltip.startsWith(QStringLiteral("Buy ")));
        QCOMPARE(model->row(spread + 1).price, QStringLiteral("0.9900")); // 1010 atoms: 1000/1010 floored to the step
        QCOMPARE(model->row(spread + 1).click_price, QStringLiteral("0.9900"));
        QCOMPARE(model->row(spread + 1).click_canonical, CAmount{1010});
    }
    void bookGroupingChoicesFollowThePriceMagnitude()
    {
        const auto labels = [](const std::vector<flowmesh::LevelGrouping>& choices) {
            QStringList out; for (const auto& grouping : choices) out << GroupingLabel(grouping); return out;
        };
        const auto units{Parse(Data()).units}; // 6 decimals
        // 1000 atoms per atom: 1 B3 per token; a canonical step below one
        // tick (0.001 B3) is never offered.
        QCOMPARE(labels(BookGroupingChoices(1000, 1010, units, false)), (QStringList{"Exact", "0.001", "0.01", "0.1"}));
        // ... and 1 token per B3 in the B3 view, where finer steps exist.
        QCOMPARE(labels(BookGroupingChoices(1000, 1010, units, true)), (QStringList{"Exact", "0.0001", "0.0002", "0.0005", "0.001", "0.01", "0.1"}));
        for (const auto& grouping : BookGroupingChoices(1000, 1010, units, true)) QVERIFY(flowmesh::LevelGroupingError(grouping).empty());
        // 40,000,000 atoms per atom of a 9-decimal token: 4 x 10^7 B3 per
        // token (k = 7), or 2.5 x 10^-8 token per B3 (k = -8).
        auto nine{units}; nine.decimals = 9;
        QCOMPARE(labels(BookGroupingChoices(40'000'000, std::nullopt, nine, false)), (QStringList{"Exact", "1000", "2000", "5000", "10000", "100000", "1000000"}));
        QCOMPARE(labels(BookGroupingChoices(std::nullopt, 40'000'000, nine, true)).mid(0, 2), (QStringList{"Exact", "0.000000000001"}));
        // A power of ten: 10^6 atoms per atom of a 6-decimal token is 1000
        // B3 per token and exactly 0.001 token per B3 (k = -3).
        QCOMPARE(labels(BookGroupingChoices(1'000'000, std::nullopt, units, true)).at(1), QStringLiteral("0.0000001"));
        // Nothing to group by without units or a price.
        auto unknown{units}; unknown.known = false;
        QCOMPARE(labels(BookGroupingChoices(1000, 1010, unknown, false)), QStringList{"Exact"});
        QCOMPARE(labels(BookGroupingChoices(std::nullopt, std::nullopt, units, false)), QStringList{"Exact"});
    }
    void choosingABookGroupingReadsItAtOnceAndShowsItOnlyWhenEchoed()
    {
        FundingReadProbe probe; probe.live_ms = 0;
        B3FlowMeshTradingPanel panel; panel.resize(1320, 940); AttachOfflineWallet(panel);
        auto exact{RemoteData()}; exact.pushKV("book_levels", LevelsJson(Parse(RemoteData()).curves, H(4)));
        probe.data = exact;
        Observe(panel, Parse(exact));
        panel.show(); QCoreApplication::processEvents();
        auto* combo{panel.m_order_book->groupingSelector()};
        QVERIFY(combo); QCOMPARE(combo->currentText(), QStringLiteral("Exact"));
        QVERIFY(panel.m_order_book->showsVerifiedLevels());
        const int step{combo->findText(QStringLiteral("0.01"))};
        QVERIFY(step > 0);
        const auto grouping{Grouping(false, 1, -2, 6)};
        // The node answers the next read in the chosen grouping.
        auto grouped{RemoteData()}; grouped.pushKV("unchanged", true); grouped.pushKV("book_levels", LevelsJson(Parse(RemoteData()).curves, H(4), grouping));
        probe.data = grouped;
        Q_EMIT combo->activated(step);
        // Until then the other grouping's levels are not shown as these.
        QVERIFY(!panel.m_order_book->showsVerifiedLevels());
        QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QVERIFY(!probe.market_options.empty());
        const auto asked{probe.market_options.back()["levels"]};
        QCOMPARE(asked["group"]["orientation"].get_str(), std::string{"canonical"});
        QCOMPARE(asked["group"]["mantissa"].getInt<int>(), 1);
        QCOMPARE(asked["group"]["exponent"].getInt<int>(), -2);
        QCOMPARE(asked["group"]["decimals"].getInt<int>(), 6);
        QVERIFY(asked["include_unchanged"].isTrue());
        QVERIFY(panel.m_snapshot->levels && panel.m_snapshot->levels->grouping == grouping);
        QVERIFY(panel.m_order_book->showsVerifiedLevels());
        QCOMPARE(combo->currentText(), QStringLiteral("0.01"));
        // The B3 view keeps its own grouping (exact until chosen).
        panel.m_orientation->setCurrentIndex(1); QCoreApplication::processEvents();
        QCOMPARE(combo->currentText(), QStringLiteral("Exact"));
        QVERIFY(!panel.levelsRequest().exists("group") || panel.levelsRequest()["group"]["orientation"].get_str() == "inverse");
        panel.m_orientation->setCurrentIndex(0); QCoreApplication::processEvents();
        QCOMPARE(combo->currentText(), QStringLiteral("0.01"));
        QCOMPARE(probe.writes.load(), 0); QVERIFY(m_wallet->IsLocked());
    }
    static Curve LadderCurve(const char* side, unsigned char owner, const std::vector<flowmesh::LadderStep>& steps, CAmount filled = 0)
    {
        Curve curve; curve.account = QString::fromStdString(H(owner).GetHex()); curve.side = QLatin1String(side);
        const bool bid{curve.side == QStringLiteral("bid")};
        const auto points{bid ? flowmesh::MakeLadderBidCurve(steps) : flowmesh::MakeLadderAskCurve(steps)};
        if (!points) throw std::runtime_error{"Invalid synthetic ladder fixture"};
        for (const auto& point : *points) curve.points.push_back({point.price, point.qty});
        const CAmount maximum{bid ? curve.points.front().quantity : curve.points.back().quantity};
        curve.filled = filled; curve.remaining = maximum - filled;
        curve.reserved = *flowmesh::LadderReservation(bid, steps);
        return curve;
    }
    void ladderTicketReadsEveryPriceAndReviewsOneOrder()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        QVERIFY(panel.m_ladder_rows->isHidden());
        panel.m_ladder->setChecked(true);
        QVERIFY(!panel.m_ladder_rows->isHidden()); QVERIFY(!panel.m_marketable->isEnabled()); QVERIFY(!panel.m_marketable->isChecked());
        panel.m_price->setText(QStringLiteral("1")); panel.m_quantity->setText(QStringLiteral("0.3"));
        panel.m_ladder_prices[0]->setText(QStringLiteral("0.99")); panel.m_ladder_quantities[0]->setText(QStringLiteral("0.2"));
        panel.m_ladder_prices[1]->setText(QStringLiteral("0.98")); panel.m_ladder_quantities[1]->setText(QStringLiteral("0.4"));
        const auto steps{panel.ladderSteps()};
        QVERIFY(steps);
        QCOMPARE(steps->size(), size_t{3});
        QCOMPARE((*steps)[1], (flowmesh::LadderStep{990, 200'000}));
        // 300,000 x 1000 + 200,000 x 990 + 400,000 x 980 atoms: 0.89 B3.
        QVERIFY2(panel.m_ticket_total->text().contains(QStringLiteral("Ladder of 3 prices · 0.9 tUSD in total")), qPrintable(panel.m_ticket_total->text()));
        QVERIFY(panel.m_ticket_total->text().contains(QStringLiteral("Reserves exactly 0.89 B3")));
        // No what-if for a ladder: the indicative is a limit order's.
        QVERIFY(!panel.indicativeDraft().isObject()); QVERIFY(panel.m_indicative->isHidden());
        // The review shows one order of every price; cancelling signs nothing.
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        QString reviewed;
        QTimer::singleShot(0, &panel, [&] { if (panel.m_confirmation) { reviewed = panel.m_confirmation->text(); panel.m_confirmation->done(QMessageBox::Cancel); } });
        panel.begin(B3FlowMeshTrading::Operation::Order);
        QVERIFY2(reviewed.contains(QStringLiteral("Ladder Buy of 3 prices, ONE standing order on this side")), qPrintable(reviewed));
        QVERIFY(reviewed.contains(QStringLiteral("0.4 tUSD at 0.98 B3 / tUSD")));
        QVERIFY(reviewed.contains(QStringLiteral("Replaces your current Buy order")));
        QCOMPARE(unlock.count(), 0); QVERIFY(!panel.m_thread); QVERIFY(m_wallet->IsLocked());
        // Rows that do not read exactly are refused, with the row named.
        QString error;
        panel.m_ladder_prices[1]->setText(QStringLiteral("1"));
        QVERIFY(!panel.ladderSteps(&error)); QVERIFY(error.contains(QStringLiteral("must differ")));
        panel.m_ladder_prices[1]->setText(QStringLiteral("0.98")); panel.m_ladder_quantities[1]->clear();
        QVERIFY(!panel.ladderSteps(&error)); QVERIFY(error.contains(QStringLiteral("price 3")));
        QVERIFY(panel.m_ticket_total->text().startsWith(QStringLiteral("Ladder  —")));
        // One price in ladder mode is an ordinary limit order.
        panel.m_ladder_prices[0]->clear(); panel.m_ladder_quantities[0]->clear(); panel.m_ladder_prices[1]->clear();
        QTimer::singleShot(0, &panel, [&] { if (panel.m_confirmation) { reviewed = panel.m_confirmation->text(); panel.m_confirmation->done(QMessageBox::Cancel); } });
        panel.begin(B3FlowMeshTrading::Operation::Order);
        QVERIFY(reviewed.startsWith(QStringLiteral("Selected wallet")) && reviewed.contains(QStringLiteral("Limit Buy: 0.3 tUSD")));
        // Leaving ladder mode enables the marketable limit again.
        panel.m_ladder->setChecked(false);
        QVERIFY(panel.m_ladder_rows->isHidden()); QVERIFY(panel.m_marketable->isEnabled());
        QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
    }
    void editingALadderPrefillsWhatRemainsAtEachPrice()
    {
        auto s{Parse(RemoteData())};
        // The viewer's bid ladder, 0.1 filled from its best price.
        const auto ladder{LadderCurve("bid", 5, {{1000, 300'000}, {990, 200'000}, {980, 100'000}}, 100'000)};
        s.curves.front() = ladder; s.own_curves = {ladder};
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, s);
        // Each price as the order book shows it: the tick's three places.
        QCOMPARE(panel.m_own_view->item(0, 1)->text(), QStringLiteral("Ladder · 1.000 / 0.990 / 0.980"));
        panel.m_own_view->selectRow(0);
        QVERIFY(panel.editAvailable());
        panel.editOrder();
        QVERIFY(panel.m_ladder->isChecked());
        QCOMPARE(panel.m_price->text(), QStringLiteral("1")); QCOMPARE(panel.m_quantity->text(), QStringLiteral("0.2"));
        QCOMPARE(panel.m_ladder_prices[0]->text(), QStringLiteral("0.99")); QCOMPARE(panel.m_ladder_quantities[0]->text(), QStringLiteral("0.2"));
        QCOMPARE(panel.m_ladder_prices[1]->text(), QStringLiteral("0.98")); QCOMPARE(panel.m_ladder_quantities[1]->text(), QStringLiteral("0.1"));
        QVERIFY(panel.m_ladder_prices[2]->text().isEmpty());
        QVERIFY(panel.m_log->toPlainText().contains(QStringLiteral("replaces that side's whole order")));
        QVERIFY(!panel.m_thread); QVERIFY(!panel.m_confirmation); QVERIFY(m_wallet->IsLocked());
    }
    void ladderOrdersAreBookLevelsForTheMarketableLimit()
    {
        // Another account's ask ladder is exact limit liquidity, so the book
        // stays complete and the marketable limit prices against it.
        auto s{SyntheticLimitBook(4)};
        s.curves.push_back(LadderCurve("ask", 95, {{1004, 50'000}, {1006, 70'000}}));
        const auto book{ProjectLimitBook(s, false)};
        QVERIFY(book.complete); QCOMPARE(book.general_curves, size_t{0});
        QCOMPARE(*book.canonical_best_ask, CAmount{1004});
        QString error;
        const auto limit{ComputeMarketableLimit(s, true, false, 100, std::nullopt, &error)};
        QVERIFY2(limit, qPrintable(error));
        QCOMPARE(limit->reference, CAmount{1004});
        // The viewer's own ask ladder is never crossed, at any of its prices.
        auto own{s}; const auto mine{LadderCurve("ask", 5, {{1020, 10'000}, {1012, 10'000}})};
        own.curves.push_back(mine); own.own_curves = {mine};
        QVERIFY(!ComputeMarketableLimit(own, true, false, 1000, std::nullopt, &error));
        QVERIFY(error.contains(QStringLiteral("cross your own opposite order")));
    }
    void orderBookSpreadRowIsExactCrossedOrOneSided()
    {
        B3FlowMeshOrderBook book; book.resize(420, 400); book.show(); QCoreApplication::processEvents();
        // 1000 and 1010 atoms per atom of a 6-decimal token: 0.010 B3 at
        // the tick's places, and 200 x 10 / 2010 = 0.995% of the midpoint
        // (the same in both views).
        book.setSnapshot(WithLevels(SyntheticLimitBook()), false);
        QCOMPARE(SpreadText(book), QStringLiteral("Spread  0.010 B3 · 0.995%"));
        // The B3 view: the difference of the two prices beside it as they
        // are shown (1.000 - 0.990), exactly 1/101 in its tooltip.
        book.setSnapshot(WithLevels(SyntheticLimitBook()), true);
        QCOMPARE(book.model()->row(book.model()->spreadRow() - 1).price, QStringLiteral("1.000"));
        QCOMPARE(book.model()->row(book.model()->spreadRow() + 1).price, QStringLiteral("0.990"));
        QCOMPARE(SpreadText(book), QStringLiteral("Spread  ≈0.010 tUSD · 0.995%"));
        QVERIFY(book.model()->row(book.model()->spreadRow()).tooltip.contains(QStringLiteral("Exact spread: 1/101 tUSD / B3")));
        // Crossed after a rationed auction: never a negative spread; the
        // standing clearing, labelled indicative.
        auto crossed{SyntheticLimitBook(1)};
        crossed.curves = {LimitCurve("bid", 30, 1012, 5'000), LimitCurve("ask", 31, 1011, 3'000)};
        book.setSnapshot(WithLevels(crossed), false);
        QVERIFY(SpreadText(book).startsWith(QStringLiteral("Crossed · next auction ≈1.011 for 0.003 tUSD")));
        QVERIFY(SpreadText(book).endsWith(QStringLiteral("(indicative)")));
        QVERIFY(book.model()->row(book.model()->spreadRow()).tooltip.contains(QStringLiteral("not certified")));
        // One side, or none.
        auto bids{SyntheticLimitBook(3)}; std::erase_if(bids.curves, [](const Curve& c) { return c.side == QStringLiteral("ask"); });
        book.setSnapshot(WithLevels(bids), false);
        QCOMPARE(SpreadText(book), QStringLiteral("One side only"));
        auto empty{SyntheticLimitBook(1)}; empty.curves.clear();
        book.setSnapshot(WithLevels(empty), false);
        QCOMPARE(SpreadText(book), QStringLiteral("No standing orders"));
    }
    void orderBookShowsDepthAtTheStandardPanelSize()
    {
        // At the standard 1320 x 940 the ladder holds at least ten levels a
        // side, as Hyperliquid's does: the activity tabs sit under the chart
        // and the book (and the ticket) take the full height beside them.
        B3FlowMeshTradingPanel panel; panel.resize(1320, 940); panel.m_timer->stop();
        Observe(panel, WithLevels(SyntheticLimitBook(16), {}, H(20)), true);
        panel.show(); for (int i{0}; i < 3; ++i) QCoreApplication::processEvents();
        QVERIFY(panel.m_order_book->showsVerifiedLevels());
        const int per_side{panel.m_order_book->rowsPerSide()};
        QVERIFY2(per_side >= 10, qPrintable(QStringLiteral("%1 levels a side").arg(per_side)));
        int asks{0}, bids{0};
        for (const auto& row : panel.m_order_book->model()->rows()) {
            asks += row.kind == B3FlowMeshLadderModel::Row::Kind::Ask;
            bids += row.kind == B3FlowMeshLadderModel::Row::Kind::Bid;
        }
        QCOMPARE(asks, per_side); QCOMPARE(bids, per_side);
        auto* column{panel.findChild<QSplitter*>(QStringLiteral("flowMeshMarketColumn"))};
        QVERIFY(column);
        QCOMPARE(column->indexOf(panel.m_activity), 1);
        QVERIFY(column->isAncestorOf(panel.m_chart));
        QVERIFY(!column->isAncestorOf(panel.m_order_book));
        QVERIFY(!column->isAncestorOf(panel.m_order));
        // The book's card is as tall as the chart and the tabs together.
        auto* book_card{panel.m_order_book->parentWidget()};
        QVERIFY(book_card->height() >= column->height() - 8);
    }
    void switchingBookAndCurveViewsPreservesUnsubmittedInput()
    {
        B3FlowMeshTradingPanel panel; panel.resize(1400, 950); AttachOfflineWallet(panel);
        Observe(panel, SyntheticLimitBook());
        auto* mode{panel.findChild<QComboBox*>(QStringLiteral("flowMeshLiquidityMode"))};
        auto* book{panel.findChild<B3FlowMeshOrderBook*>(QStringLiteral("flowMeshOrderBook"))};
        QVERIFY(mode); QVERIFY(book); QCOMPARE(mode->currentIndex(), 0);
        panel.m_price->setText(QStringLiteral("1.009")); panel.m_quantity->setText(QStringLiteral("0.25"));
        panel.show(); QCoreApplication::processEvents();
        QVERIFY(book->isVisible()); QVERIFY(!panel.m_depth_view->isVisible());
        panel.m_price->setFocus(); panel.m_price->setSelection(2, 2); QCoreApplication::processEvents();
        QVERIFY(panel.m_price->hasFocus());
        const int cursor{panel.m_price->cursorPosition()}; const QString selection{panel.m_price->selectedText()};
        const auto snapshot{panel.m_snapshot}; QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        mode->setCurrentIndex(1); QCoreApplication::processEvents();
        QVERIFY(!book->isVisible()); QVERIFY(panel.m_depth_view->isVisible());
        mode->setCurrentIndex(0); QCoreApplication::processEvents();
        QVERIFY(book->isVisible()); QVERIFY(!panel.m_depth_view->isVisible());
        QCOMPARE(panel.m_price->text(), QStringLiteral("1.009")); QCOMPARE(panel.m_quantity->text(), QStringLiteral("0.25"));
        QCOMPARE(panel.m_price->cursorPosition(), cursor); QCOMPARE(panel.m_price->selectedText(), selection); QVERIFY(panel.m_price->hasFocus());
        QVERIFY(panel.m_snapshot == snapshot); QCOMPARE(unlock.count(), 0);
        QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_confirmation);
    }
    void futuresTabIsExplicitlyUnavailableAndDoesNotAlterSpot()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, SyntheticLimitBook());
        auto* products{panel.findChild<QTabWidget*>(QStringLiteral("flowMeshProductTabs"))};
        auto* futures{panel.findChild<QWidget*>(QStringLiteral("flowMeshFuturesPage"))};
        auto* status{panel.findChild<QLabel*>(QStringLiteral("flowMeshFuturesStatus"))};
        QVERIFY(products); QVERIFY(futures); QVERIFY(status);
        QCOMPARE(products->count(), 2); QCOMPARE(products->tabText(0), QStringLiteral("Spot"));
        QCOMPARE(products->tabText(1), QStringLiteral("Futures")); QCOMPARE(products->currentIndex(), 0);
        QCOMPARE(status->text(), QStringLiteral("Not available in this build"));
        QVERIFY(futures->findChildren<QPushButton*>().empty()); QVERIFY(futures->findChildren<QLineEdit*>().empty());
        const QString gallery{qEnvironmentVariable("B3_FLOWMESH_CHART_GALLERY")};
        if (!gallery.isEmpty()) {
            QVERIFY(QDir::isAbsolutePath(gallery)); QVERIFY(QDir{}.mkpath(gallery));
            panel.resize(1550, 1100); panel.show(); QCoreApplication::processEvents();
            QImage image{panel.size(), QImage::Format_ARGB32}; image.fill(Qt::transparent); panel.render(&image);
            QVERIFY(image.save(QDir{gallery}.filePath(QStringLiteral("synthetic-spot-tab.png"))));
            products->setCurrentIndex(1); QCoreApplication::processEvents(); panel.render(&image);
            QVERIFY(image.save(QDir{gallery}.filePath(QStringLiteral("synthetic-futures-tab.png"))));
            products->setCurrentIndex(0);
        }
        panel.m_price->setText(QStringLiteral("1.009")); panel.m_quantity->setText(QStringLiteral("0.25"));
        const auto snapshot{panel.m_snapshot}; QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        products->setCurrentIndex(1); QCOMPARE(products->currentWidget(), futures);
        products->setCurrentIndex(0);
        QCOMPARE(panel.m_price->text(), QStringLiteral("1.009")); QCOMPARE(panel.m_quantity->text(), QStringLiteral("0.25"));
        QVERIFY(panel.m_snapshot == snapshot); QCOMPARE(unlock.count(), 0);
        QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_confirmation);
    }
    void fundingSelectionCannotRelabelAnOrderQuantity()
    {
        B3FlowMeshTrading::Action a; const auto s{Parse(Data())}; a.market.id = s.market; a.market.base = s.base; a.market.account = s.account; a.market.has_account = true;
        a.side = QStringLiteral("bid"); a.price = 1000; a.amount = 1'000'000; a.native = true; a.display_decimals = 6; a.display_ticker = QStringLiteral("tUSD");
        const auto text{B3FlowMeshTrading::Describe(a)}; QVERIFY(text.contains(QStringLiteral("1 tUSD (1000000 atomic units)"))); QVERIFY(text.contains(QStringLiteral("1 B3 / tUSD")));
    }
    void malformedOrUncertifiedRowsFailClosed()
    {
        auto v{Data()}; v.pushKV("matching_model", "price-time-book"); QVERIFY(Rejects([&] { Parse(v); }));
        v = Data(); v.pushKV("market_id", H(90).GetHex()); QVERIFY(Rejects([&] { Parse(v); }));
        v = Data(); auto meta{v["base_metadata"]}; meta.pushKV("decimals", 19); v.pushKV("base_metadata", meta); QVERIFY(Rejects([&] { Parse(v); }));
        v = Data(); auto hist{v["history"]}; auto entries{hist["entries"]}; auto trade{entries[0]}; trade.pushKV("notional_atoms", 1); entries.setArray(); entries.push_back(trade); hist.pushKV("entries", entries); v.pushKV("history", hist); QVERIFY(Rejects([&] { Parse(v); }));
        v = Data(); auto snap{v["snapshot"]}; snap.pushKV("certified", false); v.pushKV("snapshot", snap); QVERIFY(Rejects([&] { Parse(v); }));
    }
    void staleAndPausedGatesDoNotMisdiagnoseIdleMarkets()
    {
        auto s{Parse(Data(false))}; QVERIFY(AdmissionReady(s, 100, 600'000));
        s.pending_actions = 1;
        QVERIFY(AdmissionReady(s, 100, 600'000, 29'999));
        QVERIFY(!AdmissionReady(s, 100, 600'000, 30'000));
        QVERIFY(StatusText(s, 100, 600'000, 30'000).contains(QStringLiteral("not canceled")));
        s.pending_actions = 0;
        QVERIFY(AdmissionReady(s, 100, 600'000, 600'000));
        QVERIFY(!AdmissionReady(s, 3001, 10)); QVERIFY(StatusText(s, 3001, 10).contains(QStringLiteral("stale")));
        s.paused = true; QVERIFY(!AdmissionReady(s, 10, 10)); QVERIFY(StatusText(s, 10, 10).contains(QStringLiteral("paused")));
        s.paused = false; s.handoff = true; QVERIFY(!AdmissionReady(s, 10, 10));
        s.handoff = false; s.quorum_required = 2; QVERIFY(!AdmissionReady(s, 10, 10));
        s.quorum_required = 3; s.certified = false; QVERIFY(!AdmissionReady(s, 10, 10));
    }
    void remoteStateRequiresLocalProofButNotLocalValidator()
    {
        auto v{RemoteData()}; const auto s{Parse(v)};
        QVERIFY(s.remote); QVERIFY(s.observer); QVERIFY(s.certificate_verified); QVERIFY(s.account_state_verified); QVERIFY(!s.execution_result_verified);
        QVERIFY(AdmissionReady(s, 50, -1)); QVERIFY(StatusText(s, 50, -1).contains(QStringLiteral("not B3-checkpoint confirmed")));
        QVERIFY(!Parse(Data()).remote); // Older local replies retain compatibility.
        for (const auto* flag : {"certificate_verified", "account_state_verified"}) {
            auto bad{v}; auto proof{bad["verification"]}; proof.pushKV(flag, false); bad.pushKV("verification", proof);
            QVERIFY(Rejects([&] { Parse(bad); }));
        }
        auto proof{v["verification"]}; proof.pushKV("event_gap", true); v.pushKV("verification", proof);
        QVERIFY(StatusText(Parse(v), 50, -1).contains(QStringLiteral("gap")));
        v.pushKV("unchanged", true); const auto small{Parse(v)}; QVERIFY(small.remote); QVERIFY(small.certificate_verified); QVERIFY(small.history.empty());
        auto unsafe{s}; unsafe.account_state_verified = false; QVERIFY(!AdmissionReady(unsafe, 50, -1));
    }
    void remoteDiscoveryFetchesOnlySelectedStateEvenWithoutAccount()
    {
        const auto data{RemoteData()}; const auto s{Parse(data)};
        UniValue row{UniValue::VOBJ};
        for (const auto* key : {"market_id", "base_asset_id", "domain", "execution_config_id", "quote_asset"}) row.pushKV(key, data[key]);
        row.pushKV("vault_id", flowmesh::ComputeFlowMeshVaultId(H(1), *uint256::FromHex(s.market.toStdString()))->GetHex());
        row.pushKV("available", true); row.pushKV("running", false); row.pushKV("paused", true); row.pushKV("pending_handoff", false);
        row.pushKV("halt", "none"); row.pushKV("error", "Select market to verify its certified state"); row.pushKV("checkpoint_pending", false);
        auto proof{data["verification"]}; proof.pushKV("certificate_verified", false); proof.pushKV("account_state_verified", false); row.pushKV("verification", proof);
        QVERIFY(!B3FlowMeshTrading::ParseMarket(row).ready);
        for (bool account : {false, true}) {
            int reads{0};
            const B3FlowMeshTrading::RpcCall rpc = [&](const std::string& method, const UniValue& params) {
                ++reads;
                if (method == "listflowmeshmarkets") { UniValue rows{UniValue::VARR}; rows.push_back(row); return rows; }
                if (method != "getflowmeshbalance" || params[0].get_str() != s.market.toStdString()) throw std::runtime_error{"Unexpected RPC; discovery must not sign or query every market"};
                auto response{row}; response.pushKV("running", true); response.pushKV("paused", false); response.pushKV("error", ""); response.pushKV("verification", data["verification"]);
                if (account) { auto a{data["account"]}; a.pushKV("b3_available", FormatAmount(s.b3_available, 9).toStdString()); a.pushKV("b3_reserved", FormatAmount(s.b3_reserved, 9).toStdString()); response.pushKV("account", a); }
                return response;
            };
            const auto market{B3FlowMeshTrading::ReadMarket(s.market, rpc)};
            QCOMPARE(reads, 2); QVERIFY(market.ready); QVERIFY(market.remote); QVERIFY(market.publish_ready); QCOMPARE(market.has_account, account);
            if (account) { QCOMPARE(market.sequence, s.account_sequence); QCOMPARE(market.b3_available, s.b3_available); }
        }
    }
    void remoteHistoryIsReportedAndUnknownActionCannotBecomeANewOrder()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        QVERIFY(panel.m_order->isEnabled()); QVERIFY(m_wallet->IsLocked());
        QVERIFY(panel.m_history_note->text().contains(QStringLiteral("Endpoint-reported")));
        QVERIFY(panel.m_last_price->text().startsWith(QStringLiteral("Reported:")));
        QVERIFY(panel.m_identity_detail->text().contains(QStringLiteral("https://operator.invalid:18443")));
        QVERIFY(!panel.m_checkpoint->isEnabled()); QVERIFY(!panel.m_publish->isEnabled());
        B3FlowMeshTrading::Receipt receipt; receipt.action_id = QString::fromStdString(H(20).GetHex()); receipt.state = QStringLiteral("unknown");
        panel.m_pending_market = panel.m_snapshot->market; panel.m_pending_account = panel.m_snapshot->account;
        panel.m_pending_sequence = panel.m_snapshot->account_sequence - 1; panel.m_receipt_wallet = panel.m_wallet;
        panel.applyReceipt(receipt); panel.updateMarketText();
        QVERIFY(panel.m_uncertain); QVERIFY(!panel.m_order->isEnabled()); QVERIFY(!panel.m_withdraw->isEnabled()); QVERIFY(panel.m_retry_receipt->isEnabled());
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock}; bool reviewed{false};
        QTimer::singleShot(0, &panel, [&] {
            if (panel.m_confirmation) { reviewed = panel.m_confirmation->text().contains(receipt.action_id) && panel.m_confirmation->text().contains(QStringLiteral("does not unlock")); panel.m_confirmation->done(QMessageBox::Cancel); }
        });
        panel.retryReceipt(); QVERIFY(reviewed); QCOMPARE(unlock.count(), 0); QVERIFY(!panel.m_thread); QVERIFY(m_wallet->IsLocked());
        // Advancing the nonce is not inclusion evidence for this exact action.
        panel.m_market_data[0].sequence += 100; panel.updateControls(); QVERIFY(!panel.m_order->isEnabled());
        receipt.state = QStringLiteral("admitted"); panel.applyReceipt(receipt); panel.updateControls(); QVERIFY(!panel.m_order->isEnabled());
        receipt.state = QStringLiteral("certified_inclusion"); receipt.certificate_verified = true; receipt.microblock_hash = QString::fromStdString(H(21).GetHex()); receipt.microblock_sequence = 4;
        panel.applyReceipt(receipt); panel.updateMarketText(); QVERIFY(!panel.m_uncertain); QVERIFY(!panel.m_pending_sequence); QVERIFY(panel.m_order->isEnabled());
        QVERIFY(panel.m_progress->text().contains(QStringLiteral("Execution outcome is unknown"))); QVERIFY(!panel.m_retry_receipt->isEnabled());
        QVERIFY(m_wallet->IsLocked()); QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result);
        // Returning to local data clears remote provenance labels.
        Observe(panel, Parse(Data())); QVERIFY(!panel.m_history_note->text().contains(QStringLiteral("Endpoint-reported")));
        B3FlowMeshChart chart; chart.setSnapshot(Parse(RemoteData(false))); QVERIFY(chart.emptyMessage().contains(QStringLiteral("not independently verified")));
    }
    void verifiedOutcomesResolveOnlyWhenProvenAndHideRetry()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        const auto unknown = [&](const QString& action) {
            B3FlowMeshTrading::Receipt receipt; receipt.action_id = action; receipt.state = QStringLiteral("unknown");
            panel.m_receipt.reset(); // a new request, as after a fresh submission
            panel.m_pending_market = panel.m_snapshot->market; panel.m_pending_account = panel.m_snapshot->account;
            panel.m_pending_sequence = panel.m_snapshot->account_sequence - 1; panel.m_receipt_wallet = panel.m_wallet;
            panel.applyReceipt(receipt); panel.updateMarketText();
            return receipt;
        };
        // notExecutedHidesRetry / finalOutcomeClearsUncertaintyAndPending: a
        // proven non-execution resolves the uncertainty, re-enables new
        // actions and never offers a resend of the exact bytes.
        auto receipt{unknown(QString::fromStdString(H(30).GetHex()))};
        QVERIFY(panel.m_uncertain); QVERIFY(panel.m_retry_receipt->isEnabled());
        receipt.state = QStringLiteral("rejected"); receipt.outcome_verified = true;
        B3FlowMeshTrading::Outcome proof; proof.status = QStringLiteral("not_executed"); proof.final = true;
        proof.reason_code = QStringLiteral("NOT_EXECUTED_SEQUENCE_CONSUMED"); proof.consumed_by = QString::fromStdString(H(31).GetHex());
        proof.consumed_in_sequence = 5; proof.microblock_sequence = 5; proof.microblock_hash = QString::fromStdString(H(32).GetHex());
        receipt.outcome = proof;
        panel.applyReceipt(receipt); panel.updateMarketText(); panel.updateControls();
        QVERIFY(!panel.m_uncertain); QVERIFY(!panel.m_pending_sequence); QVERIFY(panel.m_order->isEnabled());
        QVERIFY(panel.m_receipt->no_resubmit); QVERIFY(!panel.m_retry_receipt->isEnabled());
        QVERIFY(panel.m_progress->text().contains(QStringLiteral("Never executes")));
        // remoteDepositCreditedClaimNeverResolves: an endpoint's claim that a
        // deposit was credited is only an observation.
        auto claim{unknown(QString::fromStdString(H(33).GetHex()))};
        QVERIFY(panel.m_uncertain);
        claim.endpoint = QStringLiteral("https://operator.invalid:18443"); claim.reason_code = QStringLiteral("DEPOSIT_ALREADY_CREDITED");
        panel.applyReceipt(claim); panel.updateMarketText();
        QVERIFY(panel.m_uncertain); QVERIFY(panel.m_retry_receipt->isEnabled());
        // The local engine's own answer is certified-state truth and resolves it.
        claim.endpoint.clear();
        panel.applyReceipt(claim); panel.updateMarketText();
        QVERIFY(!panel.m_uncertain);
        // A verified but non-final outcome keeps the inclusion's resolution and
        // says so; it is not terminal.
        auto included{unknown(QString::fromStdString(H(34).GetHex()))};
        QVERIFY(panel.m_uncertain);
        included.state = QStringLiteral("certified_inclusion"); included.certificate_verified = true;
        included.microblock_hash = QString::fromStdString(H(35).GetHex()); included.microblock_sequence = 6;
        B3FlowMeshTrading::Outcome open; open.status = QStringLiteral("included_not_executed"); open.final = false;
        open.reason_code = QStringLiteral("INCLUDED_BAD_SEQUENCE"); open.reason_text = QStringLiteral("Included but not executed.");
        open.microblock_sequence = 6; open.microblock_hash = included.microblock_hash;
        included.outcome = open; included.outcome_verified = true;
        panel.applyReceipt(included); panel.updateMarketText();
        QVERIFY(!panel.m_uncertain); QVERIFY(!panel.m_receipt->Terminal());
        QVERIFY(panel.m_progress->text().contains(QStringLiteral("Not final.")));
    }
    void reverseRemoteHistoryPreservesProvenanceAndGrossFeeWarning()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        Observe(panel, Parse(RemoteData()), true);
        QVERIFY(panel.m_history_note->text().contains(QStringLiteral("Endpoint-reported")));
        QVERIFY(panel.m_history_note->text().contains(QStringLiteral("not independently verified")));
        QVERIFY(panel.m_history_note->text().contains(QStringLiteral("gross B3 before fees")));
        QVERIFY(panel.m_history_note->toolTip().contains(QStringLiteral("not these historical prices or fills")));
        QVERIFY(panel.m_history_note->toolTip().contains(QStringLiteral("not net B3 received")));
        QVERIFY(panel.m_history_note->toolTip().contains(QStringLiteral("must not be inferred")));
        QCOMPARE(panel.m_history_view->toolTip(), panel.m_history_note->toolTip());
        QVERIFY(panel.m_last_price->text().startsWith(QStringLiteral("Reported:")));
        QVERIFY(panel.m_chart->toolTip().contains(QStringLiteral("not independently verified")));
        QVERIFY(!panel.m_thread); QVERIFY(m_wallet->IsLocked());
    }
    void retainedReceiptDetachReleasesWalletWithoutLosingPublicState_data()
    {
        QTest::addColumn<bool>("visible");
        QTest::addColumn<bool>("included");
        QTest::newRow("visible-certified") << true << true;
        QTest::newRow("hidden-certified") << false << true;
        QTest::newRow("visible-unknown") << true << false;
        QTest::newRow("hidden-unknown") << false << false;
    }
    void retainedReceiptDetachReleasesWalletWithoutLosingPublicState()
    {
        QFETCH(bool, visible);
        QFETCH(bool, included);
        const auto wallet_owners{m_wallet.use_count()};
        B3FlowMeshTradingPanel panel;
        AttachOfflineWallet(panel);
        Observe(panel, Parse(RemoteData()));
        const std::weak_ptr<interfaces::Wallet> backend{panel.m_backend};
        QVERIFY(!backend.expired());
        QCOMPARE(m_wallet.use_count(), wallet_owners + 1);
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};

        // The completion path remembers the wallet identity for exact-action
        // status/retry. Neither a certified nor an unknown receipt may pin
        // the underlying CWallet after its model is detached.
        const QString market{panel.m_snapshot->market};
        const QString account{panel.m_snapshot->account};
        const auto sequence{panel.m_snapshot->account_sequence};
        B3FlowMeshTrading::Receipt receipt;
        receipt.action_id = QString::fromStdString(H(91).GetHex());
        receipt.state = included ? QStringLiteral("certified_inclusion") : QStringLiteral("unknown");
        receipt.certificate_verified = included;
        if (included) { receipt.microblock_hash = QString::fromStdString(H(92).GetHex()); receipt.microblock_sequence = 4; }
        panel.m_pending_market = market; panel.m_pending_account = account;
        panel.m_pending_sequence = sequence; panel.m_receipt_wallet = panel.m_wallet;
        panel.applyReceipt(receipt);
        panel.updateMarketText();
        const QString uncertain_details{panel.m_uncertain_details};
        QCOMPARE(panel.m_uncertain, !included);
        if (!included) QVERIFY(uncertain_details.contains(receipt.action_id));
        panel.setVisible(visible);
        QCoreApplication::processEvents();
        QCOMPARE(panel.isVisible(), visible);

        // Shutdown hides this shell child but does not destroy it. Exercise
        // the public detach operation with the panel deliberately still alive.
        panel.setWalletModel(nullptr);
        QCoreApplication::processEvents();
        QVERIFY(!panel.m_wallet); QVERIFY(!panel.m_backend);
        QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_unlock);
        QVERIFY(!panel.m_timer->isActive()); QVERIFY(!panel.m_busy);
        QVERIFY(!panel.m_order->isEnabled()); QVERIFY(!panel.m_retry_receipt->isEnabled());
        QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
        QVERIFY(panel.m_receipt);
        QCOMPARE(panel.m_receipt->action_id, receipt.action_id);
        QCOMPARE(panel.m_receipt->state, receipt.state);
        QCOMPARE(panel.m_receipt->certificate_verified, receipt.certificate_verified);
        QCOMPARE(panel.m_pending_market, market); QCOMPARE(panel.m_pending_account, account);
        QCOMPARE(panel.m_uncertain, !included);
        QCOMPARE(panel.m_uncertain_details, uncertain_details);
        if (!included) {
            QCOMPARE(panel.m_uncertain_action_id, receipt.action_id);
            QVERIFY(panel.m_pending_sequence); QCOMPARE(*panel.m_pending_sequence, sequence);
        } else {
            QVERIFY(!panel.m_pending_sequence);
        }
        QVERIFY2(backend.expired(), "A detached, still-live panel must not retain the wallet through a saved receipt or uncertain outcome");
        QCOMPARE(m_wallet.use_count(), wallet_owners);
    }
    void attemptedWriteDetachDrainsAndFencesLateCompletion_data()
    {
        QTest::addColumn<bool>("completion_queued");
        QTest::addColumn<bool>("included");
        QTest::newRow("worker-awaiting-cancellation") << false << false;
        QTest::newRow("finished-callback-already-queued") << true << false;
        QTest::newRow("certified-callback-already-queued") << true << true;
    }
    void attemptedWriteDetachDrainsAndFencesLateCompletion()
    {
        QFETCH(bool, completion_queued);
        QFETCH(bool, included);
        const auto wallet_owners{m_wallet.use_count()};
        B3FlowMeshTradingPanel panel;
        AttachOfflineWallet(panel);
        const std::weak_ptr<interfaces::Wallet> backend{panel.m_backend};
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        QVERIFY(m_model->setWalletLocked(false, SecureString{"offline display fixture passphrase"}));
        QVERIFY(!m_wallet->IsLocked());
        panel.m_unlock = std::make_unique<WalletModel::UnlockContext>(m_model.get(), true, true);
        panel.m_restore_locked = true;
        panel.m_relock_backend = panel.m_backend;
        panel.m_relock_wallet_name = panel.m_wallet_name;

        // Use the exact private production result and a real WalletImpl. The
        // synthetic worker represents a write which already crossed admission;
        // it performs no RPC/signature/network operation in this offline test.
        auto result{std::make_shared<B3FlowMeshTradingPanel::Result>()};
        B3FlowMeshTrading::Action action;
        action.operation = B3FlowMeshTrading::Operation::Order;
        action.market = panel.m_market_data.front();
        action.side = QStringLiteral("bid"); action.price = 1000; action.amount = 1;
        result->action = action; result->wallet = panel.m_wallet_name;
        result->write_attempted = true;
        B3FlowMeshTrading::Receipt receipt;
        receipt.action_id = QString::fromStdString(H(93).GetHex());
        receipt.state = included ? QStringLiteral("certified_inclusion") : QStringLiteral("unknown");
        receipt.certificate_verified = included;
        if (included) { receipt.microblock_hash = QString::fromStdString(H(94).GetHex()); receipt.microblock_sequence = 4; }
        result->receipt = receipt;
        if (!included) result->error = QStringLiteral("Synthetic response unavailable after an attempted write");
        panel.m_active_result = result;
        panel.m_busy = true;
        const auto generation{panel.m_generation};
        QSemaphore entered;
        std::atomic_bool finished{false}, saw_cancel{false}, unlocked_until_finished{false};
        int delivered_callbacks{0}, accepted_callbacks{0};
        struct DrainOnExit {
            B3FlowMeshTradingPanel& panel;
            ~DrainOnExit() { panel.cancelAndWait(); }
        } drain{panel}; // Also drain before captured locals die after a failed assertion.
        panel.m_thread = QThread::create([owner = panel.m_backend, cancel = panel.m_cancel,
                                         &entered, &finished, &saw_cancel, &unlocked_until_finished, completion_queued] {
            entered.release();
            if (!completion_queued) {
                // Bound even a broken cancellation path, so a failing test
                // cannot hang while the panel destructor drains its worker.
                QDeadlineTimer deadline{1000};
                while (!cancel->load() && !deadline.hasExpired()) QThread::msleep(1);
            }
            saw_cancel.store(cancel->load());
            unlocked_until_finished.store(!owner->isLocked());
            finished.store(true);
        });
        panel.m_thread->setParent(&panel);
        QObject::connect(panel.m_thread, &QThread::finished, &panel,
            [&panel, result, generation, &delivered_callbacks, &accepted_callbacks] {
                ++delivered_callbacks;
                // The same queued completion/generation boundary as startJob.
                if (generation == panel.m_generation) {
                    ++accepted_callbacks;
                    panel.finishJob(result);
                }
            }, Qt::QueuedConnection);
        panel.m_thread->start();
        QVERIFY2(entered.tryAcquire(1, 1000), "Synthetic wallet worker did not start within the test bound");
        if (completion_queued) {
            // Wait without dispatching GUI events: the finished signal is now
            // queued to this live panel but must not run before detachment.
            QVERIFY(panel.m_thread->wait(1000));
            QVERIFY(finished.load());
            QCOMPARE(delivered_callbacks, 0);
        } else {
            QVERIFY(!finished.load());
        }
        QVERIFY(!backend.expired()); QVERIFY(panel.m_active_result);

        panel.setWalletModel(nullptr);
        QVERIFY(finished.load());
        QCOMPARE(saw_cancel.load(), !completion_queued);
        QVERIFY(unlocked_until_finished.load()); // Relock must follow the drain.
        QVERIFY(m_wallet->IsLocked());
        QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_unlock);
        QVERIFY(!panel.m_backend); QVERIFY(!panel.m_relock_backend); QVERIFY(!panel.m_wallet);
        QVERIFY(!panel.m_restore_locked); QVERIFY(panel.m_security_warning.isEmpty());
        QVERIFY(!panel.m_busy); QVERIFY(panel.m_generation > generation);
        QVERIFY(backend.expired()); QCOMPARE(m_wallet.use_count(), wallet_owners);
        QCOMPARE(panel.m_uncertain, !included); QVERIFY(!panel.m_uncertain_refreshed);
        QVERIFY(panel.m_receipt); QCOMPARE(panel.m_receipt->action_id, receipt.action_id);
        QCOMPARE(panel.m_receipt->state, receipt.state);
        QCOMPARE(panel.m_receipt->certificate_verified, included);
        QCOMPARE(panel.m_pending_market, action.market.id);
        QCOMPARE(panel.m_pending_account, action.market.account);
        if (included) {
            QVERIFY(!panel.m_pending_sequence);
            QCOMPARE(panel.m_receipt->microblock_hash, receipt.microblock_hash);
        } else {
            QCOMPARE(panel.m_uncertain_action_id, receipt.action_id);
            QVERIFY(panel.m_pending_sequence); QCOMPARE(*panel.m_pending_sequence, action.market.sequence);
        }
        const QString details{panel.m_uncertain_details};
        QVERIFY(details.contains(receipt.action_id)); QVERIFY(details.contains(result->wallet));

        // Qt may deliver a queued signal even after disconnect. The stale
        // completion must not attach a wallet, accept a receipt, or retry.
        QCoreApplication::sendPostedEvents(&panel, QEvent::MetaCall);
        QCoreApplication::processEvents();
        QCOMPARE(delivered_callbacks, completion_queued ? 1 : 0);
        QCOMPARE(accepted_callbacks, 0);
        QVERIFY(backend.expired()); QCOMPARE(m_wallet.use_count(), wallet_owners);
        QCOMPARE(panel.m_uncertain, !included); QCOMPARE(panel.m_uncertain_details, details);
        QVERIFY(panel.m_receipt); QCOMPARE(panel.m_receipt->action_id, receipt.action_id);
        QCOMPARE(panel.m_receipt->state, receipt.state);
        QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_unlock);
        QVERIFY(!panel.m_order->isEnabled()); QVERIFY(!panel.m_retry_receipt->isEnabled());
        QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
    }
    void savedRequestsHaveDedicatedReadOnlyStatusControls()
    {
        B3FlowMeshTradingPanel panel;
        QVERIFY(panel.findChild<QComboBox*>(QStringLiteral("flowMeshSavedActions")));
        QVERIFY(panel.findChild<QLabel*>(QStringLiteral("flowMeshReceiptCard")));
        const auto check{panel.findChild<QPushButton*>(QStringLiteral("flowMeshCheckReceipt"))};
        QVERIFY(check); QVERIFY(!check->isEnabled());
    }

    void restoredCardsPreserveUnknownAndPriorCertifiedInstructions()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        B3FlowMeshTrading::SavedActions saved; saved.account = panel.m_snapshot->account;
        B3FlowMeshTrading::SavedAction action;
        action.market = panel.m_snapshot->market; action.domain = panel.m_snapshot->domain; action.config = panel.m_snapshot->config;
        action.account = saved.account; action.sequence = 7; action.type = 0; action.initial_submission_ms = 1000;
        action.signed_bytes_sha256 = QString::fromStdString(H(96).GetHex()); action.signed_bytes_size = 279; action.may_have_been_sent = true;
        action.receipt.action_id = QString::fromStdString(H(95).GetHex()); action.receipt.state = QStringLiteral("unknown");
        saved.actions.push_back(action); panel.restoreSavedActions(saved);
        QVERIFY(panel.m_saved_actions_ready); QVERIFY(panel.receiptWalletSelected()); QVERIFY(panel.m_uncertain);
        QCOMPARE(panel.m_receipt->action_id, action.receipt.action_id); QCOMPARE(panel.m_pending_sequence, action.sequence);
        QVERIFY(panel.m_receipt_card->text().contains(action.receipt.action_id));
        QVERIFY(panel.m_receipt_card->text().contains(action.signed_bytes_sha256));
        QVERIFY(panel.m_receipt_card->text().contains(QStringLiteral("Original account sequence: 7")));
        QVERIFY(panel.m_check_receipt->isEnabled()); QVERIFY(panel.m_retry_receipt->isEnabled());
        QVERIFY(!panel.m_order->isEnabled()); QVERIFY(!panel.m_thread);

        auto unavailable{std::make_shared<B3FlowMeshTradingPanel::Result>()}; unavailable->receipt_only = true;
        unavailable->receipt_market = action.market; unavailable->receipt_account = action.account; unavailable->receipt_action_id = action.receipt.action_id;
        unavailable->error = QStringLiteral("HTTPS endpoint unavailable"); panel.finishJob(unavailable);
        QVERIFY(!panel.m_thread); QVERIFY(panel.m_uncertain); QVERIFY(panel.m_check_receipt->isEnabled());
        QVERIFY(panel.m_receipt_card->text().contains(QStringLiteral("Original instruction and protections retained")));
        QCOMPARE(panel.m_receipt->action_id, action.receipt.action_id); QCOMPARE(panel.m_pending_sequence, action.sequence);

        // A durable prior certification is not a fresh verification claim and
        // cannot offer exact retry while current authority is unavailable.
        saved.actions[0].receipt.no_resubmit = true;
        panel.restoreSavedActions(saved); // No selection change or fresh status required to renew the guard.
        QVERIFY(panel.m_receipt->no_resubmit); QVERIFY(!panel.m_receipt->Included());
        QVERIFY(panel.m_retry_receipt->isHidden()); QVERIFY(!panel.m_retry_receipt->isEnabled());
        QVERIFY(!panel.m_uncertain); QVERIFY(!panel.m_pending_sequence);
        QVERIFY(panel.m_receipt_card->text().contains(QStringLiteral("fresh verification label requires current evidence")));
        panel.retryReceipt(); QVERIFY(!panel.m_thread);
        panel.finishJob(unavailable); QVERIFY(panel.m_receipt->no_resubmit); QVERIFY(!panel.m_receipt->Included());

        // A later status response cannot forget the durable replay guard.
        auto unknown{action.receipt}; panel.applyReceipt(unknown);
        QVERIFY(panel.m_receipt->no_resubmit); QVERIFY(!panel.m_retry_receipt->isEnabled());
        auto changed{saved}; changed.actions[0].sequence = 8; panel.restoreSavedActions(changed);
        QVERIFY(!panel.m_saved_actions_ready); QVERIFY(!panel.m_order->isEnabled());
        QCOMPARE(panel.m_saved_actions.actions[0].sequence, action.sequence);
        QVERIFY(panel.m_receipt_card->text().contains(QStringLiteral("identity or its durable protections changed")));
        panel.cancelAndWait(); QVERIFY(!panel.m_backend); QVERIFY(!panel.m_relock_backend);
        QVERIFY(!panel.m_wallet); QVERIFY(!panel.m_thread); QVERIFY(panel.m_receipt->no_resubmit);
    }

    void savedCardsRejectOtherWalletOrMarketAttribution()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        panel.m_saved_actions_ready = false; panel.updateControls();
        QVERIFY(!panel.m_order->isEnabled());
        B3FlowMeshTrading::SavedActions saved; saved.account = QString::fromStdString(H(97).GetHex());
        panel.restoreSavedActions(saved); QVERIFY(!panel.m_saved_actions_ready);
        QVERIFY(panel.m_receipt_card->text().contains(QStringLiteral("selected wallet account")));
        saved.account = panel.m_snapshot->account;
        B3FlowMeshTrading::SavedAction action; action.account = saved.account;
        action.market = panel.m_snapshot->market; action.domain = panel.m_snapshot->domain;
        action.config = QString::fromStdString(H(98).GetHex()); action.receipt.action_id = QString::fromStdString(H(95).GetHex());
        saved.actions.push_back(action); panel.restoreSavedActions(saved);
        QVERIFY(!panel.m_saved_actions_ready); QVERIFY(!panel.m_check_receipt->isEnabled());
        QVERIFY(panel.m_receipt_card->text().contains(QStringLiteral("configuration does not match")));
        QVERIFY(!panel.m_receipt); QVERIFY(!panel.m_thread);
    }

    void sameActionIdInDifferentMarketsDoesNotShareReplayOrUncertaintyState()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        B3FlowMeshTrading::SavedActions saved; saved.account = panel.m_snapshot->account;
        B3FlowMeshTrading::SavedAction a;
        a.market = panel.m_snapshot->market; a.domain = panel.m_snapshot->domain; a.config = panel.m_snapshot->config;
        a.account = saved.account; a.sequence = 7; a.initial_submission_ms = 1000; a.signed_bytes_size = 279;
        a.signed_bytes_sha256 = QString::fromStdString(H(96).GetHex()); a.may_have_been_sent = true;
        a.receipt.action_id = QString::fromStdString(H(95).GetHex()); a.receipt.state = QStringLiteral("unknown"); a.receipt.no_resubmit = true;
        auto b{a}; b.market = QString::fromStdString(flowmesh::ComputeFlowMeshMarketId(H(1), H(98))->GetHex());
        b.receipt.no_resubmit = false; b.initial_submission_ms = 1001; b.signed_bytes_sha256 = QString::fromStdString(H(97).GetHex());
        saved.actions = {a, b}; panel.restoreSavedActions(saved);
        // Unknown B is selected first. Its semantic ActionId deliberately
        // equals A's: the signed market domain, not ActionId, separates them.
        QCOMPARE(panel.m_pending_market, b.market); QVERIFY(panel.m_uncertain);
        QCOMPARE(panel.m_uncertain_market, b.market); QVERIFY(!panel.m_receipt->no_resubmit);
        const auto select = [&](const B3FlowMeshTrading::SavedAction& action) {
            const auto index{panel.m_saved_selector->findData(QString{action.market + QLatin1Char(':') + action.receipt.action_id}, Qt::UserRole + 1)};
            QVERIFY(index >= 0); panel.m_saved_selector->setCurrentIndex(index);
        };
        select(a); QCOMPARE(panel.m_receipt->market, a.market); QVERIFY(panel.m_receipt->no_resubmit);
        QVERIFY(panel.m_uncertain); QCOMPARE(panel.m_uncertain_market, b.market); // A cannot resolve B.
        select(b); QCOMPARE(panel.m_receipt->market, b.market); QVERIFY(!panel.m_receipt->no_resubmit);
        QVERIFY(panel.m_uncertain); QCOMPARE(panel.m_uncertain_market, b.market);
        QVERIFY(panel.m_retry_receipt->isEnabled()); QCOMPARE(panel.m_pending_sequence, b.sequence);
        QVERIFY(!panel.m_saved_actions.actions[1].receipt.no_resubmit); QVERIFY(!panel.m_thread);
        // A read which captured A before local restoration selected B may
        // finish late. Its certificate must not become B's certificate/guard.
        auto late_a{a.receipt}; late_a.market = a.market; late_a.account = a.account;
        late_a.state = QStringLiteral("certified_inclusion"); late_a.certificate_verified = true;
        late_a.microblock_hash = QString::fromStdString(H(99).GetHex()); late_a.microblock_sequence = 9;
        panel.applyReceipt(late_a); panel.updateControls();
        QCOMPARE(panel.m_receipt->market, b.market); QVERIFY(!panel.m_receipt->Included()); QVERIFY(!panel.m_receipt->no_resubmit);
        QCOMPARE(panel.m_pending_market, b.market); QCOMPARE(panel.m_pending_sequence, b.sequence);
        QVERIFY(panel.m_uncertain); QCOMPARE(panel.m_uncertain_market, b.market); QVERIFY(panel.m_retry_receipt->isEnabled());
        QVERIFY(panel.m_saved_actions.actions[0].receipt.Included()); QVERIFY(panel.m_saved_actions.actions[0].receipt.no_resubmit);
        QVERIFY(!panel.m_saved_actions.actions[1].receipt.Included()); QVERIFY(!panel.m_saved_actions.actions[1].receipt.no_resubmit);
        QVERIFY(panel.m_receipt_card->text().contains(b.market));
        QVERIFY(!panel.m_receipt_card->text().contains(QStringLiteral("Exact action inclusion verified")));
        B3FlowMeshTradingPanel::Result error_a;
        error_a.receipt_market = a.market; error_a.receipt_account = a.account; error_a.receipt_action_id = a.receipt.action_id;
        panel.applyReceiptError(error_a, QStringLiteral("Only market A's status request failed"));
        QVERIFY(!panel.m_receipt_card->text().contains(QStringLiteral("Only market A"))); QVERIFY(panel.m_receipt_error.isEmpty());
        auto error_b{error_a}; error_b.receipt_market = b.market;
        panel.applyReceiptError(error_b, QStringLiteral("Market B status unavailable"));
        QVERIFY(panel.m_receipt_card->text().contains(QStringLiteral("Market B status unavailable")));
        panel.applyReceiptError(error_a, QString{}); // A's success cannot clear B's error either.
        QVERIFY(panel.m_receipt_card->text().contains(QStringLiteral("Market B status unavailable")));
        QVERIFY(panel.m_uncertain); QVERIFY(!panel.m_receipt->no_resubmit);
    }

    void receiptIdentitySurvivesSwitchWithoutPinningTheOldWallet()
    {
        B3FlowMeshTradingPanel panel;
        auto other{MakeOfflineWallet("different-offline-wallet")};
        const auto wallet_owners{m_wallet.use_count()};
        const auto other_owners{other.wallet.use_count()};
        AttachOfflineWallet(panel);
        const std::weak_ptr<interfaces::Wallet> original_backend{panel.m_backend};
        B3FlowMeshTrading::Receipt receipt;
        receipt.action_id = QString::fromStdString(H(95).GetHex()); receipt.state = QStringLiteral("unknown");
        panel.m_pending_market = panel.m_snapshot->market; panel.m_pending_account = panel.m_snapshot->account;
        panel.m_pending_sequence = panel.m_snapshot->account_sequence;
        panel.m_receipt_wallet = panel.m_wallet;
        panel.applyReceipt(receipt);
        panel.setWalletModel(nullptr);
        QVERIFY(original_backend.expired()); QCOMPARE(m_wallet.use_count(), wallet_owners);

        AttachOfflineWallet(panel, *other.model, other.wallet);
        const std::weak_ptr<interfaces::Wallet> other_backend{panel.m_backend};
        QVERIFY(!panel.receiptWalletSelected()); QVERIFY(!panel.uncertainWalletSelected());
        QVERIFY(!panel.m_retry_receipt->isEnabled()); QVERIFY(panel.m_uncertain);
        panel.retryReceipt(); panel.startJob(std::nullopt, std::nullopt, true);
        QVERIFY(!panel.m_confirmation); QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result);
        QCOMPARE(panel.m_receipt->action_id, receipt.action_id);
        panel.setWalletModel(nullptr);
        QVERIFY(other_backend.expired()); QCOMPARE(other.wallet.use_count(), other_owners);

        // The same still-live model, not its display name, restores identity.
        // A new WalletImpl may be acquired without resurrecting the old owner.
        AttachOfflineWallet(panel);
        QVERIFY(panel.receiptWalletSelected()); QVERIFY(panel.uncertainWalletSelected());
        QVERIFY(original_backend.expired()); QVERIFY(panel.m_retry_receipt->isEnabled());
        QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_unlock);
        QCOMPARE(panel.m_receipt->action_id, receipt.action_id); QVERIFY(panel.m_uncertain);
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        bool exact_review{false};
        QTimer::singleShot(0, &panel, [&] {
            if (panel.m_confirmation) {
                exact_review = panel.m_confirmation->text().contains(receipt.action_id) && panel.m_confirmation->text().contains(QStringLiteral("already retained signed bytes"));
                panel.m_confirmation->done(QMessageBox::Cancel);
            }
        });
        panel.retryReceipt();
        QVERIFY(exact_review); QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
        QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result);
        panel.setWalletModel(nullptr);
        QCOMPARE(m_wallet.use_count(), wallet_owners);
    }
    void unloadedReceiptOwnerCannotRebindByDisplayName()
    {
        B3FlowMeshTradingPanel panel;
        auto original{MakeOfflineWallet(m_wallet->GetName())};
        QCOMPARE(original.model->getDisplayName(), m_model->getDisplayName());
        const std::weak_ptr<wallet::CWallet> original_wallet{original.wallet};
        AttachOfflineWallet(panel, *original.model, original.wallet);
        const std::weak_ptr<interfaces::Wallet> original_backend{panel.m_backend};
        B3FlowMeshTrading::Receipt receipt;
        receipt.action_id = QString::fromStdString(H(96).GetHex()); receipt.state = QStringLiteral("unknown");
        panel.m_pending_market = panel.m_snapshot->market; panel.m_pending_account = panel.m_snapshot->account;
        panel.m_pending_sequence = panel.m_snapshot->account_sequence;
        panel.m_receipt_wallet = panel.m_wallet; panel.applyReceipt(receipt);
        const QString details{panel.m_uncertain_details};
        panel.setWalletModel(nullptr);
        QVERIFY(original_backend.expired());
        original.model.reset(); original.wallet.reset();
        QVERIFY(original_wallet.expired());
        QVERIFY(!panel.m_receipt_wallet); QVERIFY(!panel.m_uncertain_wallet);

        // Same displayed name and synthetic account fields deliberately do not
        // authorize a different WalletModel to query/retry the saved request.
        AttachOfflineWallet(panel);
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        QVERIFY(!panel.receiptWalletSelected()); QVERIFY(!panel.uncertainWalletSelected());
        panel.retryReceipt(); panel.startJob(std::nullopt, std::nullopt, true);
        QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_confirmation);
        QVERIFY(!panel.m_retry_receipt->isEnabled()); QVERIFY(!panel.m_order->isEnabled());
        QVERIFY(panel.m_uncertain); QCOMPARE(panel.m_uncertain_details, details);
        // A fresh read permits explicit acknowledgement, not implicit outcome
        // resolution or an identity rebind. Feed the real completion handler.
        auto refresh{std::make_shared<B3FlowMeshTradingPanel::Result>()};
        refresh->markets = panel.m_market_data; refresh->snapshot = panel.m_snapshot;
        panel.finishJob(refresh);
        QVERIFY(panel.m_uncertain); QVERIFY(panel.m_uncertain_refreshed);
        QVERIFY(!panel.receiptWalletSelected()); QVERIFY(!panel.m_retry_receipt->isEnabled());
        QVERIFY(panel.m_review_uncertain->isEnabled()); QVERIFY(!panel.m_thread);
        QString warning;
        bool cancel_default{false};
        QTimer::singleShot(0, &panel, [&] {
            if (panel.m_confirmation) {
                warning = panel.m_confirmation->text();
                cancel_default = panel.m_confirmation->defaultButton() == panel.m_confirmation->button(QMessageBox::Cancel);
                panel.m_confirmation->done(QMessageBox::Cancel);
            }
        });
        panel.reviewUncertain();
        QVERIFY(warning.contains(QStringLiteral("original wallet instance was unloaded")));
        QVERIFY(warning.contains(QStringLiteral("does not resolve the original request")));
        QVERIFY(warning.contains(QStringLiteral("automatic retry is disabled")));
        QVERIFY(warning.contains(receipt.action_id)); QVERIFY(cancel_default);
        QVERIFY(panel.m_uncertain); QCOMPARE(panel.m_uncertain_details, details);
        QVERIFY(panel.m_receipt); QCOMPARE(panel.m_receipt->action_id, receipt.action_id);
        QCOMPARE(panel.m_receipt->state, QStringLiteral("unknown"));
        QVERIFY(!panel.m_receipt_wallet); QVERIFY(!panel.m_uncertain_wallet);
        QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_unlock);
        QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
    }
    void queuedOldModelUnloadCannotDetachReplacement()
    {
        B3FlowMeshTradingPanel panel;
        auto original{MakeOfflineWallet("queued-old-model")};
        auto replacement{MakeOfflineWallet("replacement-model")};
        // These unregistered wallets intentionally yield no backend, hence no
        // RPC, but public setWalletModel installs the real lifecycle callbacks.
        panel.setWalletModel(original.model.get());
        QCOMPARE(panel.m_wallet.data(), original.model.get());
        QVERIFY(!panel.m_backend); QVERIFY(!panel.m_thread);
        const auto original_generation{panel.m_generation};
        // A cross-thread signal queues the production AutoConnection. Keep the
        // QObject alive on its owning thread; never destroy a model off-thread.
        auto emitter{std::unique_ptr<QThread>{QThread::create([model = original.model.get()] { Q_EMIT model->unload(); })}};
        emitter->start();
        const bool emitted{emitter->wait(1000)};
        if (!emitted) emitter->wait();
        QVERIFY(emitted);
        panel.setWalletModel(replacement.model.get());
        QCOMPARE(panel.m_wallet.data(), replacement.model.get());
        const auto replacement_generation{panel.m_generation};
        QVERIFY(replacement_generation > original_generation);
        QCoreApplication::sendPostedEvents(&panel, QEvent::MetaCall);
        QCoreApplication::processEvents();
        QCOMPARE(panel.m_wallet.data(), replacement.model.get());
        QCOMPARE(panel.m_generation, replacement_generation);
        QVERIFY(!panel.m_backend); QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result);
        panel.setWalletModel(nullptr);
    }
    void unchangedReplyContainsNoInventedEmptyHistory()
    {
        const auto full{Data()}; UniValue small{UniValue::VOBJ};
        for (const auto* key : {"market_id", "base_asset_id", "domain", "execution_config_id", "quote_asset", "matching_model", "quantity_lot_raw", "price_tick_raw", "snapshot"}) small.pushKV(key, full[key]);
        small.pushKV("unchanged", true); const auto s{Parse(small)}; QVERIFY(s.unchanged); QVERIFY(!s.units.known); QVERIFY(s.history.empty());
    }
    void marketViewSectionsParseAndFailClosedPerSection()
    {
        auto v{RemoteDataWithViews()};
        const auto s{Parse(v)};
        QVERIFY(s.ticks && s.ticks->available && s.ticks->remote);
        QCOMPARE(s.ticks->ticks.size(), size_t{2});
        const auto& tick{s.ticks->ticks[1]};
        QCOMPARE(tick.sequence, uint64_t{2}); QCOMPARE(tick.hash, s.head); QVERIFY(tick.cleared);
        QCOMPARE(tick.price, CAmount{1000}); QCOMPARE(tick.volume, CAmount{250'000}); QCOMPARE(tick.imbalance, CAmount{50'000}); QCOMPARE(tick.side, QStringLiteral("bid"));
        QVERIFY(s.indicative && s.indicative->available && s.indicative->cleared && s.indicative->has_draft && s.indicative->replaces);
        QCOMPARE(s.indicative->draft_side, QStringLiteral("bid")); QCOMPARE(s.indicative->draft_quantity, CAmount{1'000'000});
        QCOMPARE(*s.indicative->fill_max, CAmount{187'500}); QCOMPARE(*s.indicative->best_ask, CAmount{1001});
        QCOMPARE(s.history.size(), size_t{1});
        // A malformed section is unavailable; the rest of the reply is not.
        const auto broken = [&](const char* key, const UniValue& section) {
            auto bad{RemoteDataWithViews()}; bad.pushKV(key, section); return Parse(bad);
        };
        for (const auto& ticks : {TickPageJson({TickJson(3, H(4), true)}), TickPageJson({TickJson(1, H(12), false), TickJson(3, H(4), false)}),
                                  TickPageJson({TickJson(2, H(4), false, "bid")}), TickPageJson({TickJson(2, H(4), true, "none", "handoff")})}) {
            const auto parsed{broken("clearing_ticks", ticks)};
            QVERIFY(parsed.ticks && !parsed.ticks->available && parsed.ticks->ticks.empty());
            QCOMPARE(parsed.ticks->reason, QStringLiteral("malformed"));
            QCOMPARE(parsed.history.size(), size_t{1}); QVERIFY(parsed.indicative->available);
        }
        auto certified{IndicativeJson()}; certified.pushKV("certified", true);
        for (const auto& indicative : {IndicativeJson(true, H(9)), certified}) {
            const auto parsed{broken("indicative", indicative)};
            QVERIFY(parsed.indicative && !parsed.indicative->available);
            QCOMPARE(parsed.indicative->reason, QStringLiteral("malformed"));
            QVERIFY(parsed.ticks->available);
        }
        // An unchanged reply carries both sections.
        UniValue small{UniValue::VOBJ};
        for (const auto* key : {"market_id", "base_asset_id", "domain", "execution_config_id", "quote_asset", "matching_model", "quantity_lot_raw", "price_tick_raw", "snapshot", "clearing_ticks", "indicative"}) small.pushKV(key, v[key]);
        small.pushKV("unchanged", true);
        const auto light{Parse(small)};
        QVERIFY(light.unchanged && light.ticks && light.ticks->available && light.indicative && light.indicative->available);
        // A node's own unavailability is kept as its reason.
        UniValue off{UniValue::VOBJ}; off.pushKV("available", false); off.pushKV("unavailable_reason", "endpoint_does_not_advertise_clearing_ticks");
        off.pushKV("source", "endpoint_reported"); off.pushKV("gap", false); off.pushKV("more", false); off.pushKV("ticks", UniValue{UniValue::VARR});
        QCOMPARE(broken("clearing_ticks", off).ticks->reason, QStringLiteral("endpoint_does_not_advertise_clearing_ticks"));
    }
    void observedTimesAndCandlePagesParseFailClosed()
    {
        const auto s{Parse(RemoteDataWithTimes())};
        QVERIFY(s.observed && s.observed->available);
        QCOMPARE(s.observed->source, QStringLiteral("https://operator.invalid:18443"));
        QCOMPARE(s.observed->rows.size(), size_t{2});
        QCOMPARE(s.observed->rows[1].ms, OBSERVED_T0 + 61'000);
        QVERIFY(s.observed->rows[1].hash == H(4));
        QVERIFY(s.observed->rows[0].mode == flowmesh::ObservedMode::LIVE);
        // A malformed section is only unavailable; the rest of the reply is not.
        const auto broken = [](const UniValue& section) { auto bad{RemoteDataWithViews()}; bad.pushKV("observed_time", section); return Parse(bad); };
        auto certified{ObservedJson({})}; certified.pushKV("certified", true);
        for (const auto& section : {certified, ObservedJson({StampJson(2, H(4), "live", 5), StampJson(1, H(12), "live", 6)}),
                                    ObservedJson({StampJson(3, H(4), "live", 5)}), ObservedJson({StampJson(2, H(4), "sometimes", 5)}),
                                    ObservedJson({StampJson(2, H(4), "delayed", -1)})}) {
            const auto parsed{broken(section)};
            QVERIFY(parsed.observed && !parsed.observed->available);
            QCOMPARE(parsed.observed->reason, QStringLiteral("malformed"));
            QVERIFY(parsed.ticks->available); QCOMPARE(parsed.history.size(), size_t{1});
        }
        UniValue off{UniValue::VOBJ}; off.pushKV("available", false); off.pushKV("unavailable_reason", "endpoint_does_not_advertise_observed_time");
        off.pushKV("source", "https://old.invalid"); off.pushKV("source_kind", "operator_observed"); off.pushKV("certified", false);
        QCOMPARE(broken(off).observed->reason, QStringLiteral("endpoint_does_not_advertise_observed_time"));
        // Candle pages: exact, and refused when misaligned, overlapping or of another interval.
        const auto page{ParseCandles(CandlesJson("1m", 60'000, OBSERVED_T0 + 120'000, 5,
            {CandleJson(OBSERVED_T0, 1, 1, 10, 10, 10, 10, 3), CandleJson(OBSERVED_T0 + 60'000, 2, 3, 12, 14, 9, 13, 5, {"partial"})}),
            flowmesh::CandleInterval::M1)};
        QVERIFY(page.available && page.remote); QCOMPARE(page.candles.size(), size_t{2});
        QCOMPARE(page.candles[1].high, CAmount{14}); QVERIFY(page.candles[1].quote_volume == 65); QCOMPARE(page.candles[1].flags, uint8_t{flowmesh::CANDLE_PARTIAL});
        for (const auto& bad : {CandlesJson("1m", 60'000, OBSERVED_T0 + 120'000, 5, {CandleJson(OBSERVED_T0 + 1, 1, 1, 10, 10, 10, 10, 3)}),
                                CandlesJson("1m", 60'000, OBSERVED_T0 + 120'000, 5, {CandleJson(OBSERVED_T0, 1, 2, 10, 10, 10, 10, 3), CandleJson(OBSERVED_T0 + 60'000, 2, 3, 10, 10, 10, 10, 3)}),
                                CandlesJson("5m", 300'000, OBSERVED_T0 + 300'000, 5, {}),
                                CandlesJson("1m", 60'000, OBSERVED_T0 + 120'000, 5, {CandleJson(OBSERVED_T0, 1, 1, 10, 11, 12, 10, 3)})}) {
            const auto parsed{ParseCandles(bad, flowmesh::CandleInterval::M1)};
            QVERIFY(!parsed.available); QCOMPARE(parsed.reason, QStringLiteral("malformed"));
        }
    }
    void heldTimesFollowHeldTicksAndOneSource()
    {
        B3FlowMeshTradingPanel panel;
        auto s{Parse(RemoteDataWithTimes())};
        panel.m_snapshot = s; panel.mergeHeldTicks();
        QCOMPARE(panel.m_ticks.size(), size_t{2}); QCOMPARE(panel.m_tick_times.size(), size_t{2});
        QCOMPARE(panel.m_tick_times_source, QStringLiteral("https://operator.invalid:18443"));
        // A row for a tick not held, or with another hash, is never kept.
        std::map<uint64_t, flowmesh::ObservedStamp> held;
        ObservedTimes other; other.available = true; other.rows = s.observed->rows;
        other.rows[1].hash = H(99); other.rows.push_back(other.rows[0]); other.rows.back().sequence = 7;
        MergeHeldTimes(held, panel.m_ticks, &other);
        QCOMPARE(held.size(), size_t{1}); QVERIFY(held.contains(1));
        // Another source starts its own series.
        s.observed->source = QStringLiteral("https://other.invalid"); s.observed->rows.pop_back();
        panel.m_snapshot = s; panel.mergeHeldTicks();
        QCOMPARE(panel.m_tick_times.size(), size_t{1}); QCOMPARE(panel.m_tick_times_source, QStringLiteral("https://other.invalid"));
        // Replaced ticks drop every time.
        TickPage replaced; replaced.available = true; replaced.gap = true;
        Tick nine; nine.sequence = 9; nine.hash = QString::fromStdString(H(40).GetHex()); nine.kind = QStringLiteral("execution"); nine.side = QStringLiteral("none");
        replaced.ticks = {nine}; s.ticks = replaced; s.observed->rows.clear(); s.next_sequence = 10;
        panel.m_snapshot = s; panel.mergeHeldTicks();
        QCOMPARE(panel.m_ticks.size(), size_t{1}); QVERIFY(panel.m_tick_times.empty());
    }
    void marketReadsAskForObservedTimeAndCandlesWhenDue()
    {
        FundingReadProbe probe; probe.live_ms = 0; probe.data = RemoteDataWithTimes();
        probe.candles_response = CandlesJson("1m", 60'000, OBSERVED_T0 + 120'000, 300,
            {CandleJson(OBSERVED_T0, 1, 1, 1000, 1000, 1000, 1000, 250'000), CandleJson(OBSERVED_T0 + 60'000, 2, 2, 1000, 1000, 1000, 1000, 250'000)});
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        panel.m_candle_interval = flowmesh::CandleInterval::M1;
        panel.startJob(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QCOMPARE(probe.market_options.size(), size_t{1});
        QVERIFY(probe.market_options[0]["observed_time"].isTrue());
        // Nothing observed was displayed before that read: no candles yet.
        QVERIFY(probe.candle_options.empty());
        QVERIFY(panel.m_snapshot->observed && panel.m_snapshot->observed->available);
        QCOMPARE(panel.m_tick_times.size(), size_t{2});
        // The next job reads a whole window.
        panel.startJob(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QCOMPARE(probe.candle_options.size(), size_t{1});
        QCOMPARE(probe.candle_options[0]["interval"].get_str(), std::string{"1m"});
        QCOMPARE(probe.candle_options[0]["limit"].getInt<int>(), 300);
        QVERIFY(panel.m_candles && panel.m_candles->available); QCOMPARE(panel.m_candles->candles.size(), size_t{2});
        // Within ten seconds nothing more; afterwards only the newest few.
        panel.startJob(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QCOMPARE(probe.candle_options.size(), size_t{1});
        QVERIFY(!panel.candlesDue());
        // A page of the newest intervals replaces only what the window held there.
        panel.applyCandles(ParseCandles(CandlesJson("1m", 60'000, OBSERVED_T0 + 180'000, 2,
            {CandleJson(OBSERVED_T0 + 60'000, 2, 3, 1000, 1004, 998, 1002, 500'000), CandleJson(OBSERVED_T0 + 120'000, 4, 4, 1003, 1003, 1003, 1003, 1)}),
            flowmesh::CandleInterval::M1), 3);
        QCOMPARE(panel.m_candles->candles.size(), size_t{3});
        QCOMPARE(panel.m_candles->candles[0].first_sequence, uint64_t{1});
        QCOMPARE(panel.m_candles->candles[1].high, CAmount{1004});
        QCOMPARE(panel.m_candles->before_ms, OBSERVED_T0 + 180'000);
        // A window the held ticks could not continue is read again, at
        // most every 2 s.
        panel.m_candles_refetch = true;
        QVERIFY(!panel.candlesDue());
        QTest::qWait(2050);
        QVERIFY(panel.candlesDue()); QCOMPARE(panel.candlesDue()->second, size_t{300});
        panel.m_candles_refetch = false;
        // Another interval (or source) is a whole window again.
        panel.m_candle_interval = flowmesh::CandleInterval::M5;
        const auto due{panel.candlesDue()};
        QVERIFY(due); QCOMPARE(due->second, size_t{300});
        // A failed candles read never fails the job or the market read.
        probe.candles_response = UniValue{};
        panel.startJob(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QVERIFY(!panel.m_read_failed);
        QCOMPARE(probe.writes.load(), 0); QVERIFY(m_wallet->IsLocked());
    }
    void aFailedCandlesReadBacksOffAndKeepsLiveUpdates()
    {
        // Every candles read fails (as a remote client's did on any active
        // market): each failure backs off, from 2 s and doubling, so live
        // updates keep supplying the market read instead of the tick
        // polling the market (and candles) twice a second.
        FundingReadProbe probe; probe.live_ms = 2000; probe.data = RemoteDataWithTimes(); probe.candles_response = UniValue{};
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteDataWithTimes()));
        panel.m_candle_interval = flowmesh::CandleInterval::M1;
        Live(panel, probe);
        panel.m_watch.mode = B3FlowMeshWatch::Mode::Live; panel.m_watch.last_ok_at = panel.m_watch_clock.elapsed();
        panel.m_refresh_phase = B3FlowMeshTradingPanel::RefreshPhase::None; panel.m_catalog_age.start();
        // No page held: the window is due, so this tick reads it.
        QVERIFY(panel.candlesDue()); QVERIFY(!panel.liveSkipsPoll());
        panel.startJob(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QCOMPARE(probe.candle_options.size(), size_t{1});
        QVERIFY(!panel.m_read_failed);
        // Failed: not due again for 2 s; meanwhile live updates supply the read.
        panel.m_watch.last_ok_at = panel.m_watch_clock.elapsed();
        QVERIFY(!panel.candlesDue());
        QVERIFY(panel.liveSkipsPoll());
        QVERIFY(panel.m_candles_failure);
        QVERIFY(panel.m_candles_failure->retry_at - panel.m_watch_clock.elapsed() > 1500);
        // The chart says why instead of reading forever.
        panel.updateDataViews();
        QVERIFY(!panel.m_chart->timeSeries().loading);
        QVERIFY(panel.m_chart->timeSeries().reason.contains(QStringLiteral("Synthetic candles unavailable")));
        // Once the back-off has passed it is read again; a second failure
        // waits twice as long.
        panel.m_candles_failure->retry_at = panel.m_watch_clock.elapsed();
        QVERIFY(panel.candlesDue()); QCOMPARE(panel.candlesDue()->second, size_t{300});
        panel.startJob(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QCOMPARE(probe.candle_options.size(), size_t{2});
        QCOMPARE(panel.m_candles_failure->failures, 2);
        QVERIFY(panel.m_candles_failure->retry_at - panel.m_watch_clock.elapsed() > 3500);
        panel.m_watch.last_ok_at = panel.m_watch_clock.elapsed();
        QVERIFY(panel.liveSkipsPoll());
        // Another interval starts afresh: its window is due at once.
        panel.m_candle_interval = flowmesh::CandleInterval::M5;
        QVERIFY(panel.candlesDue());
        panel.m_candle_interval = flowmesh::CandleInterval::M1;
        QVERIFY(!panel.candlesDue());
        // A page ends the back-off.
        probe.candles_response = CandlesJson("1m", 60'000, OBSERVED_T0 + 120'000, 300,
            {CandleJson(OBSERVED_T0, 1, 1, 1000, 1000, 1000, 1000, 250'000), CandleJson(OBSERVED_T0 + 60'000, 2, 2, 1000, 1000, 1000, 1000, 250'000)});
        panel.m_candles_failure->retry_at = panel.m_watch_clock.elapsed();
        panel.startJob(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QCOMPARE(probe.candle_options.size(), size_t{3});
        QVERIFY(panel.m_candles && panel.m_candles->available);
        QVERIFY(!panel.m_candles_failure);
        QVERIFY(!panel.candlesDue());
        panel.m_watch.Disable(); AwaitWatchWorker(panel);
        QCOMPARE(probe.writes.load(), 0); QVERIFY(m_wallet->IsLocked());
    }
    void aNewestCandlesPageThatSkipsHeldIntervalsReadsTheWholeWindow()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteDataWithTimes()));
        panel.m_candle_interval = flowmesh::CandleInterval::M1;
        const auto minute = [](int m, uint64_t first, uint64_t last) {
            return CandleJson(OBSERVED_T0 + m * 60'000, first, last, 1000, 1000, 1000, 1000, 250'000);
        };
        const auto page = [](int64_t before_minute, int64_t span, const std::vector<UniValue>& candles, const std::string& source = "https://operator.invalid:18443") {
            return ParseCandles(CandlesJson("1m", 60'000, OBSERVED_T0 + before_minute * 60'000, span, candles, source), flowmesh::CandleInterval::M1);
        };
        // A held window: minutes 0 and 1 (auctions 1 and 2).
        panel.applyCandles(page(2, 300, {minute(0, 1, 1), minute(1, 2, 2)}), 300);
        QCOMPARE(panel.m_candles->candles.size(), size_t{2});
        QVERIFY(!panel.candlesDue());
        // Hidden (or asleep, or failing) through minutes 2 to 6 of trading:
        // the newest three intervals no longer reach the held window. Merged,
        // minutes 2 and 3 would read as auctions without an observed time,
        // which the source has. Not merged; the whole window is read at once.
        panel.applyCandles(page(7, 3, {minute(4, 9, 10), minute(5, 11, 12), minute(6, 13, 13)}), 3);
        QCOMPARE(panel.m_candles->candles.size(), size_t{2});
        QCOMPARE(panel.m_candles->candles.back().last_sequence, uint64_t{2});
        QVERIFY(panel.candlesDue()); QCOMPARE(panel.candlesDue()->second, size_t{300});
        // The whole window leaves no gap the source does not have.
        panel.applyCandles(page(7, 300, {minute(0, 1, 1), minute(1, 2, 2), minute(2, 3, 5), minute(3, 6, 8), minute(4, 9, 10),
                                         minute(5, 11, 12), minute(6, 13, 13)}), 300);
        QCOMPARE(panel.m_candles->candles.size(), size_t{7});
        for (size_t i{1}; i < panel.m_candles->candles.size(); ++i) {
            QCOMPARE(panel.m_candles->candles[i].first_sequence, panel.m_candles->candles[i - 1].last_sequence + 1);
        }
        QVERIFY(!panel.candlesDue());
        // A newest page over the held newest candle (grown since) is merged.
        panel.applyCandles(page(8, 3, {minute(5, 11, 12), minute(6, 13, 15), minute(7, 16, 16)}), 3);
        QCOMPARE(panel.m_candles->candles.size(), size_t{8});
        QCOMPARE(panel.m_candles->candles[6].last_sequence, uint64_t{15});
        QVERIFY(!panel.candlesDue());
        // So is one whose first candle directly follows the held newest one
        // (the market was quiet in between).
        panel.applyCandles(page(12, 3, {minute(9, 17, 17), minute(10, 18, 18), minute(11, 19, 19)}), 3);
        QCOMPARE(panel.m_candles->candles.size(), size_t{11});
        QVERIFY(!panel.candlesDue());
        // A newest page from another source (the market's endpoint changed
        // while it was read) never replaces the window.
        panel.applyCandles(page(13, 3, {minute(12, 20, 20)}, "https://other.invalid:18443"), 3);
        QCOMPARE(panel.m_candles->candles.size(), size_t{11});
        QCOMPARE(panel.m_candles->source, QStringLiteral("https://operator.invalid:18443"));
        QVERIFY(panel.candlesDue()); QCOMPARE(panel.candlesDue()->second, size_t{300});
    }
    void aQuietMarketsEmptyNewestPageContinuesTheHeldWindow()
    {
        // Review of 2026-09-30 (UI round, should-fix 2): once a market had no
        // auction for three intervals, an endpoint that ends its newest
        // window at its own clock serves an empty page. It did not continue
        // the held window, so the whole 300-interval window was read again
        // every 10 s, indefinitely. Nothing to read on a quiet market; the
        // whole window at most every five minutes while auctions are
        // unaccounted.
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteDataWithTimes()));
        panel.m_candle_interval = flowmesh::CandleInterval::M1;
        const auto minute = [](int m, uint64_t first, uint64_t last) {
            return CandleJson(OBSERVED_T0 + m * 60'000, first, last, 1000, 1000, 1000, 1000, 250'000);
        };
        const auto page = [](int64_t before_minute, int64_t span, const std::vector<UniValue>& candles) {
            return ParseCandles(CandlesJson("1m", 60'000, OBSERVED_T0 + before_minute * 60'000, span, candles), flowmesh::CandleInterval::M1);
        };
        const auto window_due = [&] { return panel.candlesDue() && panel.candlesDue()->second == B3FlowMeshTradingPanel::CANDLES_WINDOW; };
        // A held window: minutes 0 and 1 (auctions 1 and 2), read after the
        // verified head whose next sequence is 3.
        panel.m_snapshot->next_sequence = 3;
        panel.applyCandles(page(2, 300, {minute(0, 1, 1), minute(1, 2, 2)}), 300, 3);
        QVERIFY(!panel.candlesDue());
        // Minutes 7-9 hold no auction: continued, nothing merged or re-read.
        panel.applyCandles(page(10, 3, {}), 3, 3);
        QVERIFY(!panel.candlesDue());
        QCOMPARE(panel.m_candles->candles.size(), size_t{2});
        QCOMPARE(panel.m_candles->before_ms, OBSERVED_T0 + 10 * 60'000);
        QVERIFY(!panel.m_candles_unproven);
        // However long it stays quiet: never the whole window again.
        panel.m_candles_window_at = panel.m_watch_clock.elapsed() - B3FlowMeshTradingPanel::CANDLES_FULL_REFRESH_MS - 1;
        panel.applyCandles(page(16, 3, {}), 3, 3);
        QVERIFY(!panel.candlesDue());
        // A page whose job head is unknown changes nothing.
        panel.applyCandles(page(17, 3, {}), 3);
        QVERIFY(!panel.candlesDue());
        // Auctions 3 and 4 were certified, but an empty page does not show
        // them (the held newest minute may have grown after it was read, or
        // they have no observed time). A whole window was read under five
        // minutes ago: continued until five minutes have passed.
        panel.m_snapshot->next_sequence = 5;
        panel.m_candles_window_at = panel.m_watch_clock.elapsed();
        panel.applyCandles(page(18, 3, {}), 3, 5);
        QCOMPARE(panel.m_candles->candles.size(), size_t{2});
        QVERIFY(panel.m_candles_unproven);
        QVERIFY(!panel.candlesDue());
        // Unproven, the page's end is not taken as complete.
        QCOMPARE(panel.m_candles->before_ms, OBSERVED_T0 + 17 * 60'000);
        panel.m_candles_window_at = panel.m_watch_clock.elapsed() - B3FlowMeshTradingPanel::CANDLES_FULL_REFRESH_MS;
        QVERIFY(window_due());
        // That whole window accounts for them: quiet again.
        panel.applyCandles(page(19, 300, {minute(0, 1, 1), minute(1, 2, 2), minute(2, 3, 4)}), 300, 5);
        QVERIFY(!panel.m_candles_unproven); QCOMPARE(panel.m_candles_next, uint64_t{5});
        QVERIFY(!panel.candlesDue());
        panel.applyCandles(page(20, 3, {}), 3, 5);
        QVERIFY(!panel.candlesDue());
        // Unaccounted auctions after five minutes without a whole window:
        // it is read at once.
        panel.m_snapshot->next_sequence = 7;
        panel.m_candles_window_at = panel.m_watch_clock.elapsed() - B3FlowMeshTradingPanel::CANDLES_FULL_REFRESH_MS;
        panel.applyCandles(page(21, 3, {}), 3, 7);
        QVERIFY(window_due());
        // An empty page from another source never continues the window.
        panel.m_snapshot->next_sequence = 3;
        panel.applyCandles(page(2, 300, {minute(0, 1, 1), minute(1, 2, 2)}), 300, 3);
        QVERIFY(!panel.candlesDue());
        panel.applyCandles(ParseCandles(CandlesJson("1m", 60'000, OBSERVED_T0 + 10 * 60'000, 3, {}, "https://other.invalid:18443"),
                                        flowmesh::CandleInterval::M1), 3, 3);
        QVERIFY(window_due());
    }
    void timeCandlesContinueTheServedPageWithHeldTicksOnce()
    {
        B3FlowMeshTradingPanel panel;
        auto s{Parse(RemoteDataWithTimes())};
        panel.m_candle_interval = flowmesh::CandleInterval::M1;
        Observe(panel, s);
        panel.mergeHeldTicks();
        // Before candles arrive the chart says so; it is already in time.
        panel.updateDataViews();
        QVERIFY(panel.m_chart->timeMode()); QVERIFY(panel.m_chart->timeSeries().loading);
        auto* combo{panel.findChild<QComboBox*>(QStringLiteral("flowMeshCandleInterval"))};
        QVERIFY(combo); QCOMPARE(combo->count(), 6); QCOMPARE(combo->currentText(), QStringLiteral("1m"));
        // The node served tick 1 in the first minute; tick 2 (61 s) is held
        // with its time and continues the page in the next minute.
        panel.m_snapshot = s; panel.m_candles_market = s.market;
        panel.applyCandles(ParseCandles(CandlesJson("1m", 60'000, OBSERVED_T0 + 60'000, 300,
            {CandleJson(OBSERVED_T0, 1, 1, 990, 990, 990, 990, 100'000)}), flowmesh::CandleInterval::M1), 300);
        panel.updateDataViews();
        const auto& series{panel.m_chart->timeSeries()};
        QVERIFY(!series.loading); QVERIFY(series.remote);
        QCOMPARE(series.candles.size(), size_t{2});
        QCOMPARE(series.candles[1].open_ms, OBSERVED_T0 + 60'000);
        QCOMPARE(series.candles[1].first_sequence, uint64_t{2}); QCOMPARE(series.candles[1].close, CAmount{1000});
        // Nothing counted twice when the page already holds the tick.
        panel.applyCandles(ParseCandles(CandlesJson("1m", 60'000, OBSERVED_T0 + 120'000, 300,
            {CandleJson(OBSERVED_T0, 1, 1, 990, 990, 990, 990, 100'000), CandleJson(OBSERVED_T0 + 60'000, 2, 2, 1000, 1000, 1000, 1000, 250'000)}),
            flowmesh::CandleInterval::M1), 300);
        panel.updateDataViews();
        QCOMPARE(panel.m_chart->timeSeries().candles.size(), size_t{2});
        QVERIFY(panel.m_chart->timeSeries().candles[1].base_volume == 250'000);
        QVERIFY(!panel.m_candles_refetch);
        // Choosing another interval asks for its window.
        combo->setCurrentIndex(3);
        QCOMPARE(panel.m_candle_interval, flowmesh::CandleInterval::H1);
        QVERIFY(panel.candlesDue()); QCOMPARE(panel.candlesDue()->first, flowmesh::CandleInterval::H1);
        QVERIFY(panel.m_chart->timeSeries().loading);
    }
    void olderServersKeepAuctionOrderWithANote()
    {
        B3FlowMeshTradingPanel panel;
        auto v{RemoteDataWithViews()};
        UniValue off{UniValue::VOBJ}; off.pushKV("available", false); off.pushKV("unavailable_reason", "endpoint_does_not_advertise_observed_time");
        off.pushKV("source", "https://operator.invalid:18443"); off.pushKV("source_kind", "operator_observed"); off.pushKV("certified", false);
        v.pushKV("observed_time", off);
        Observe(panel, Parse(v)); panel.mergeHeldTicks(); panel.updateDataViews();
        QVERIFY(!panel.m_chart->timeMode());
        QCOMPARE(panel.m_chart->timeSeries().reason, QStringLiteral("endpoint_does_not_advertise_observed_time"));
        auto* combo{panel.findChild<QComboBox*>(QStringLiteral("flowMeshCandleInterval"))};
        QVERIFY(combo); QCOMPARE(combo->count(), 3); QCOMPARE(combo->currentText(), QStringLiteral("Every 5 auctions"));
        QVERIFY(combo->toolTip().contains(QStringLiteral("does not report times")));
        QVERIFY(!panel.candlesDue());
        QVERIFY(panel.m_chart->toolTip().contains(QStringLiteral("does not report times")));
        // Once the server reports times, the chart and its choices switch.
        panel.m_snapshot = Parse(RemoteDataWithTimes()); panel.mergeHeldTicks(); panel.updateDataViews();
        QVERIFY(panel.m_chart->timeMode()); QCOMPARE(combo->count(), 6);
    }
    void timeChartSpansAxisAndViewport()
    {
        using flowmesh::ObservedCandle;
        ObservedCandle a; a.open_ms = OBSERVED_T0; a.first_sequence = 3; a.last_sequence = 5;
        ObservedCandle b; b.open_ms = OBSERVED_T0 + 300'000; b.first_sequence = 6; b.last_sequence = 6;
        QCOMPARE(B3FlowMeshChart::SpanBetween(a, b).first, B3FlowMeshChart::Between::QUIET);
        b.flags = flowmesh::CANDLE_SEAM_BEFORE;
        QCOMPARE(B3FlowMeshChart::SpanBetween(a, b).first, B3FlowMeshChart::Between::SEAM);
        b.first_sequence = 9;
        QCOMPARE(B3FlowMeshChart::SpanBetween(a, b), (std::pair{B3FlowMeshChart::Between::GAP, uint64_t{3}}));
        // UTC labels per interval.
        QCOMPARE(B3FlowMeshChart::TimeAxisLabel(OBSERVED_T0 + 5 * 60'000, flowmesh::CandleInterval::M5), QStringLiteral("12:05"));
        QCOMPARE(B3FlowMeshChart::TimeAxisLabel(OBSERVED_T0 - 12 * 3'600'000, flowmesh::CandleInterval::H1), QStringLiteral("Sep 30"));
        QCOMPARE(B3FlowMeshChart::TimeAxisLabel(OBSERVED_T0, flowmesh::CandleInterval::D1), QStringLiteral("Sep 30"));
        QCOMPARE(B3FlowMeshChart::ProvenanceText(true, QStringLiteral("https://trade.example.org:18443")),
                 QStringLiteral("Prices, volumes and times reported by trade.example.org · not verified"));
        QVERIFY(B3FlowMeshChart::ProvenanceText(false, QStringLiteral("this_node")).contains(QStringLiteral("times observed by this node (not certified)")));
        // Viewport: follows the newest candle, zooms, pans and resets.
        B3FlowMeshChart chart; chart.resize(700, 400);
        B3FlowMeshChart::TimeSeries series; series.available = true; series.source = QStringLiteral("this_node");
        series.interval = flowmesh::CandleInterval::M1;
        for (int i{0}; i < 200; ++i) {
            ObservedCandle c; c.open_ms = OBSERVED_T0 + i * 60'000; c.first_sequence = c.last_sequence = static_cast<uint64_t>(i);
            c.entries = c.clearings = 1; c.open = c.high = c.low = c.close = 1000 + i; c.base_volume = 1; c.quote_volume = 1000 + i;
            series.candles.push_back(c);
        }
        chart.setTimeSeries(series); chart.setNowMs(OBSERVED_T0 + 199 * 60'000 + 30'000);
        QVERIFY(chart.followsTail());
        QCOMPARE(chart.timeWindow().second, OBSERVED_T0 + 199 * 60'000);
        QCOMPARE(chart.timeWindow().second - chart.timeWindow().first, int64_t{59} * 60'000);
        chart.zoomTime(2.0);
        QCOMPARE(chart.timeSlots(), 30.0); QVERIFY(chart.followsTail());
        chart.panTime(20);
        QVERIFY(!chart.followsTail()); QCOMPARE(chart.timeWindow().second, OBSERVED_T0 + 179 * 60'000);
        chart.panTime(-100);
        QVERIFY(chart.followsTail());
        chart.zoomTime(0.01); QCOMPARE(chart.timeSlots(), 500.0);
        chart.resetTimeView(); QCOMPARE(chart.timeSlots(), 60.0); QVERIFY(chart.followsTail());
        // Painting is bounded to the window and never throws on edges.
        QImage image{chart.size(), QImage::Format_ARGB32}; chart.render(&image);
        series.candles.clear(); chart.setTimeSeries(series); chart.render(&image);
    }
    void unchangedRepliesKeepTheirObservedTimes()
    {
        B3FlowMeshTradingPanel panel;
        const auto full{Parse(RemoteDataWithViews())};
        Observe(panel, full); panel.mergeHeldTicks();
        QVERIFY(panel.m_tick_times.empty());
        // A light poll at the same head: its ticks' times replace the old section.
        auto light{RemoteDataWithTimes()}; light.pushKV("unchanged", true);
        QElapsedTimer age; age.start();
        QCOMPARE(panel.applySelectedSnapshot(Parse(light), age, full.head), B3FlowMeshTradingPanel::SnapshotApply::APPLIED);
        QVERIFY(panel.m_snapshot->observed && panel.m_snapshot->observed->available);
        QCOMPARE(panel.m_tick_times.size(), size_t{2});
    }
    void tradeTapeShowsLiveTimesOnlyAndColoursByPriceChange()
    {
        const auto tick = [](uint64_t sequence, CAmount price, bool cleared = true) {
            Tick t; t.sequence = sequence; t.hash = QString::fromStdString(H(static_cast<unsigned char>(sequence)).GetHex());
            t.kind = QStringLiteral("execution"); t.side = QStringLiteral("none"); t.cleared = cleared;
            if (cleared) { t.price = price; t.volume = 10; }
            return t;
        };
        std::deque<Tick> ticks{tick(1, 100), tick(2, 0, false), tick(3, 105), tick(4, 105), tick(5, 90), tick(6, 95)};
        std::map<uint64_t, flowmesh::ObservedStamp> times;
        const auto stamp = [](uint64_t sequence, flowmesh::ObservedMode mode, int64_t ms) {
            flowmesh::ObservedStamp out; out.sequence = sequence; out.hash = H(static_cast<unsigned char>(sequence)); out.mode = mode; out.ms = ms; return out;
        };
        times[6] = stamp(6, flowmesh::ObservedMode::LIVE, OBSERVED_T0 + 5'000);
        times[5] = stamp(5, flowmesh::ObservedMode::DELAYED, OBSERVED_T0 + 4'000);
        times[4] = stamp(4, flowmesh::ObservedMode::LIVE, OBSERVED_T0 + 3'000); times[4].hash = H(99); // another entry's: ignored
        const auto rows{B3FlowMeshTradeTape::BuildRows(ticks, times)};
        // Cleared auctions only, newest first.
        QCOMPARE(rows.size(), size_t{5});
        QCOMPARE(rows[0].sequence, uint64_t{6}); QCOMPARE(rows[4].sequence, uint64_t{1});
        // Up, down, and an unchanged price keeping the last direction.
        QCOMPARE(rows[0].direction, 1); QCOMPARE(rows[1].direction, -1); QCOMPARE(rows[2].direction, 1); QCOMPARE(rows[3].direction, 1); QCOMPARE(rows[4].direction, 0);
        QString tip;
        const auto live{B3FlowMeshTradeTape::TimeText(rows[0], QStringLiteral("https://trade.example.org"), true, &tip)};
        QCOMPARE(live, QDateTime::fromMSecsSinceEpoch(OBSERVED_T0 + 5'000).toLocalTime().toString(QStringLiteral("HH:mm:ss")));
        QVERIFY(tip.contains(QStringLiteral("Observed by trade.example.org at 2026-09-30 12:00:05.000 UTC")));
        QVERIFY(tip.contains(QStringLiteral("not certified"))); QVERIFY(tip.contains(QStringLiteral("Auction #6")));
        // Applied late: never a trade time, only its bound in the tooltip.
        QCOMPARE(B3FlowMeshTradeTape::TimeText(rows[1], QStringLiteral("this_node"), false, &tip), QStringLiteral("—"));
        QVERIFY(tip.contains(QStringLiteral("upper bound")));
        QCOMPARE(B3FlowMeshTradeTape::TimeText(rows[2], QStringLiteral("this_node"), false, &tip), QStringLiteral("—"));
        QVERIFY(!rows[2].stamp);
        // At most fifty rows.
        std::deque<Tick> many; for (uint64_t i{0}; i < 200; ++i) many.push_back(tick(i + 1, 100 + static_cast<CAmount>(i % 7)));
        QCOMPARE(B3FlowMeshTradeTape::BuildRows(many, {}).size(), B3FlowMeshTradeTape::MAX_ROWS);
        // The widget: one row per auction, the price coloured, updated in place.
        B3FlowMeshTradeTape tape;
        const auto units{Parse(Data()).units};
        tape.setRows(rows, units, false, QStringLiteral("this_node"), false, {});
        QCOMPARE(tape.table()->rowCount(), 5);
        QCOMPARE(tape.table()->item(0, 0)->foreground().color(), B3Theme::kPositive);
        QCOMPARE(tape.table()->item(1, 0)->foreground().color(), B3Theme::kNegative);
        QCOMPARE(tape.table()->item(1, 2)->foreground().color(), B3Theme::kTextMuted);
        auto* first{tape.table()->item(0, 0)};
        tape.setRows(rows, units, false, QStringLiteral("this_node"), false, {});
        QCOMPARE(tape.table()->item(0, 0), first);
        QVERIFY(tape.findChild<QLabel*>(QStringLiteral("flowMeshTradeTapeNote"))->text().contains(QStringLiteral("observed by this node")));
        tape.setRows({}, units, false, {}, true, QStringLiteral("endpoint_does_not_advertise_observed_time"));
        QVERIFY(tape.findChild<QLabel*>(QStringLiteral("flowMeshTradeTapeNote"))->text().contains(QStringLiteral("No matched auctions")));
    }
    void remoteTapeIsLabelledReported()
    {
        // A remote client's ticks are the endpoint's claims, as its trade
        // history: the tape's note, header and every row say so.
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        Observe(panel, Parse(RemoteDataWithTimes()), true);
        panel.mergeHeldTicks(); panel.updateDataViews();
        auto* table{panel.m_trade_tape->table()};
        QVERIFY(table->rowCount() > 0);
        const QString label{QStringLiteral("Endpoint-reported · execution not independently verified")};
        QCOMPARE(B3FlowMeshTradeTape::ReportedLabel(), label);
        QVERIFY(panel.m_trade_tape->findChild<QLabel*>(QStringLiteral("flowMeshTradeTapeNote"))->text().contains(label));
        QVERIFY(table->horizontalHeaderItem(0)->toolTip().contains(label));
        QVERIFY(table->horizontalHeaderItem(1)->toolTip().contains(label));
        QVERIFY(table->accessibleName().contains(label));
        for (int row{0}; row < table->rowCount(); ++row) QVERIFY(table->item(row, 0)->toolTip().contains(label));
        QVERIFY(panel.m_history_note->text().contains(QStringLiteral("execution results not independently verified")));
        // The local engine's own execution: no such label.
        Observe(panel, Parse(Data()), true);
        auto page{TickPage{}}; page.available = true;
        Tick tick; tick.sequence = 2; tick.hash = QString::fromStdString(H(4).GetHex()); tick.kind = QStringLiteral("execution"); tick.side = QStringLiteral("none");
        tick.cleared = true; tick.price = 1000; tick.volume = 250'000; page.ticks = {tick};
        panel.m_snapshot->ticks = page; panel.mergeHeldTicks(); panel.updateDataViews();
        QCOMPARE(table->rowCount(), 1);
        QVERIFY(!table->item(0, 0)->toolTip().contains(label));
        QVERIFY(!panel.m_trade_tape->findChild<QLabel*>(QStringLiteral("flowMeshTradeTapeNote"))->text().contains(label));
        QVERIFY(!table->horizontalHeaderItem(0)->toolTip().contains(label));
    }
    void olderServersGetReceiptTimesForLiveSingleAuctionsOnly()
    {
        B3FlowMeshTradingPanel panel;
        // An older server: ticks, no observed times.
        const auto at = [](uint64_t next, std::vector<UniValue> ticks, bool observed = false) {
            auto v{RemoteData(false)};
            UniValue snapshot{v["snapshot"]}; snapshot.pushKV("next_microblock_sequence", next);
            snapshot.pushKV("last_microblock_hash", H(static_cast<unsigned char>(next + 100)).GetHex()); v.pushKV("snapshot", snapshot);
            v.pushKV("clearing_ticks", TickPageJson(ticks));
            if (observed) v.pushKV("observed_time", ObservedJson({}));
            return Parse(v);
        };
        const auto tick = [](uint64_t sequence) { return TickJson(sequence, H(static_cast<unsigned char>(sequence + 101)), true); };
        Observe(panel, at(3, {tick(1), tick(2)})); panel.mergeHeldTicks();
        QElapsedTimer age; age.start();
        // One new auction from a poll (or a caught-up replay in consecutive
        // replies): never a receipt time.
        QCOMPARE(panel.applySelectedSnapshot(at(4, {tick(3)}), age, {}), B3FlowMeshTradingPanel::SnapshotApply::APPLIED);
        QVERIFY(panel.m_received_times.empty());
        Observe(panel, at(3, {tick(1), tick(2)})); panel.mergeHeldTicks();
        // An endpoint advertising that it is not ready: none either.
        auto info{ConnectionInfo("https://operator.invalid:18443")};
        UniValue rows_info{UniValue::VARR};
        for (auto row : info["endpoints"].getValues()) { row.pushKV("readiness", "not_ready"); rows_info.push_back(row); }
        info.pushKV("endpoints", rows_info); panel.m_client_info = info;
        QCOMPARE(panel.applySelectedSnapshot(at(4, {tick(3)}), age, {}, /*live_head_change=*/true), B3FlowMeshTradingPanel::SnapshotApply::APPLIED);
        QVERIFY(panel.m_received_times.empty());
        panel.m_client_info.reset();
        Observe(panel, at(3, {tick(1), tick(2)})); panel.mergeHeldTicks();
        // One new auction in a live update that saw the head move while Live:
        // its receipt time, shown in the tape only.
        QCOMPARE(panel.applySelectedSnapshot(at(4, {tick(3)}), age, {}, /*live_head_change=*/true), B3FlowMeshTradingPanel::SnapshotApply::APPLIED);
        QCOMPARE(panel.m_received_times.size(), size_t{1}); QVERIFY(panel.m_received_times.contains(3));
        const auto rows{B3FlowMeshTradeTape::BuildRows(panel.m_ticks, panel.m_tick_times, panel.m_received_times)};
        QVERIFY(rows[0].received_ms); QVERIFY(!rows[1].received_ms);
        QString tip;
        QVERIFY(B3FlowMeshTradeTape::TimeText(rows[0], {}, true, &tip) != QStringLiteral("—"));
        QVERIFY(tip.contains(QStringLiteral("Received by this app")));
        QVERIFY(!panel.m_chart->timeMode());
        // Two new auctions at once (a catch-up or a slow poll): none.
        QCOMPARE(panel.applySelectedSnapshot(at(6, {tick(4), tick(5)}), age, {}, true), B3FlowMeshTradingPanel::SnapshotApply::APPLIED);
        QCOMPARE(panel.m_received_times.size(), size_t{1});
        // After a failed read, not Live: none.
        panel.m_read_failed = true;
        QCOMPARE(panel.applySelectedSnapshot(at(7, {tick(6)}), age, {}, true), B3FlowMeshTradingPanel::SnapshotApply::APPLIED);
        QCOMPARE(panel.m_received_times.size(), size_t{1});
        panel.m_read_failed = false;
        // A server that reports times: receipt times are dropped, never mixed.
        QCOMPARE(panel.applySelectedSnapshot(at(8, {tick(7)}, true), age, {}, true), B3FlowMeshTradingPanel::SnapshotApply::APPLIED);
        QVERIFY(panel.m_received_times.empty());
    }
    void receiptTimesNeverRunBackwards()
    {
        // The design's "backward clock step -> —": a receipt time is never
        // earlier than one already held for an older auction.
        B3FlowMeshTradingPanel panel;
        const auto at = [](uint64_t next, std::vector<UniValue> ticks) {
            auto v{RemoteData(false)};
            UniValue snapshot{v["snapshot"]}; snapshot.pushKV("next_microblock_sequence", next);
            snapshot.pushKV("last_microblock_hash", H(static_cast<unsigned char>(next + 100)).GetHex()); v.pushKV("snapshot", snapshot);
            v.pushKV("clearing_ticks", TickPageJson(ticks));
            return Parse(v);
        };
        const auto tick = [](uint64_t sequence) { return TickJson(sequence, H(static_cast<unsigned char>(sequence + 101)), true); };
        Observe(panel, at(3, {tick(1), tick(2)})); panel.mergeHeldTicks();
        QElapsedTimer age; age.start();
        // Auction 2 was stamped an hour ahead of this computer's clock as it
        // stands now: the clock has since stepped back (an NTP correction).
        const int64_t ahead{QDateTime::currentMSecsSinceEpoch() + 3'600'000};
        panel.m_received_times[2] = ahead;
        QCOMPARE(panel.applySelectedSnapshot(at(4, {tick(3)}), age, {}, /*live_head_change=*/true), B3FlowMeshTradingPanel::SnapshotApply::APPLIED);
        QCOMPARE(panel.m_ticks.back().sequence, uint64_t{3});
        QVERIFY(!panel.m_received_times.contains(3));
        QCOMPARE(panel.m_received_times.at(2), ahead);
        const auto rows{B3FlowMeshTradeTape::BuildRows(panel.m_ticks, panel.m_tick_times, panel.m_received_times)};
        QCOMPARE(rows[0].sequence, uint64_t{3});
        QVERIFY(!rows[0].received_ms);
        QCOMPARE(B3FlowMeshTradeTape::TimeText(rows[0], {}, true), QStringLiteral("—"));
        // Once no held time is ahead of the clock, stamps resume, in order.
        panel.m_received_times[2] = QDateTime::currentMSecsSinceEpoch() - 1000;
        QCOMPARE(panel.applySelectedSnapshot(at(5, {tick(4)}), age, {}, true), B3FlowMeshTradingPanel::SnapshotApply::APPLIED);
        QVERIFY(panel.m_received_times.contains(4));
        QVERIFY(panel.m_received_times.at(4) >= panel.m_received_times.at(2));
    }
    void receiptTimesNeedTheHeadToMoveDuringTheWait()
    {
        // "head_changed" is answered at once, without any wait, whenever the
        // known head is already stale (the local engine and the public
        // endpoint alike): a lagging display or a source catching up, so it
        // never stamps a receipt time. A watch that waited at the displayed
        // head and woke as that head moved ("ready") does.
        const auto base = [] {
            auto v{RemoteData(false)};
            v.pushKV("clearing_ticks", TickPageJson({TickJson(1, H(12), false), TickJson(2, H(4), true)}));
            return v;
        };
        const auto newer = [&] {
            auto v{base()}; auto snapshot{v["snapshot"]};
            snapshot.pushKV("next_microblock_sequence", 4); snapshot.pushKV("last_microblock_hash", H(10).GetHex()); v.pushKV("snapshot", snapshot);
            auto account{v["account"]}; account.pushKV("next_sequence", 9); v.pushKV("account", account);
            v.pushKV("clearing_ticks", TickPageJson({TickJson(3, H(10), true)}));
            return v;
        };
        for (const bool moved_during_wait : {false, true}) {
            FundingReadProbe probe; probe.live_ms = 2000; probe.data = base(); probe.watch_data = newer();
            probe.watch_object = WatchObject(moved_during_wait ? "ready" : "head_changed", true, true);
            B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(base())); panel.mergeHeldTicks();
            QCOMPARE(panel.m_ticks.back().sequence, uint64_t{2});
            Live(panel, probe);
            probe.hold_watch = true;
            panel.armWatch(); QVERIFY(panel.m_watch_thread);
            QVERIFY(probe.watch_entered.tryAcquire(1, 2000));
            probe.watch_release.release(); // Only this read returns; the next one is held.
            QTRY_COMPARE_WITH_TIMEOUT(panel.m_snapshot->head, QString::fromStdString(H(10).GetHex()), 3000);
            // The one new auction is held either way; only a change seen
            // during the wait gives it a receipt time.
            QCOMPARE(panel.m_ticks.back().sequence, uint64_t{3});
            QCOMPARE(panel.m_received_times.contains(3), moved_during_wait);
            QCOMPARE(panel.m_received_times.size(), size_t{moved_during_wait ? 1U : 0U});
            panel.m_watch.Disable(); probe.hold_watch = false; probe.watch_release.release(); AwaitWatchWorker(panel);
            QCOMPARE(probe.writes.load(), 0);
        }
    }
    void historyAndStripShowObservedTimesNotMicroblocks()
    {
        B3FlowMeshTradingPanel panel;
        auto v{RemoteDataWithTimes()};
        Observe(panel, Parse(v)); panel.mergeHeldTicks(); panel.updateDataViews(); panel.updateMarketText();
        const QString clock{QDateTime::fromMSecsSinceEpoch(OBSERVED_T0 + 61'000).toLocalTime().toString(QStringLiteral("HH:mm:ss"))};
        // The trade history's time column and the clearing strip.
        QCOMPARE(panel.m_history_view->horizontalHeaderItem(0)->text(), QStringLiteral("Time"));
        QCOMPARE(panel.m_history_view->item(0, 0)->text(), clock);
        QVERIFY(panel.m_history_view->item(0, 0)->toolTip().contains(QStringLiteral("Auction #2")));
        QVERIFY(panel.m_clearing_strip->text().startsWith(QStringLiteral("Last auction ") + clock));
        QVERIFY(panel.m_progress->text().contains(QStringLiteral("Latest certified auction ") + clock));
        // The Trades view of the liquidity card.
        auto* mode{panel.findChild<QComboBox*>(QStringLiteral("flowMeshLiquidityMode"))};
        QVERIFY(mode); QCOMPARE(mode->itemText(2), QStringLiteral("Trades"));
        mode->setCurrentIndex(2);
        QVERIFY(!panel.m_trade_tape->isHidden()); QVERIFY(panel.m_order_book->isHidden());
        QCOMPARE(panel.m_trade_tape->table()->rowCount(), 1);
        QCOMPARE(panel.m_trade_tape->table()->item(0, 2)->text(), clock);
        // No primary label speaks of microblocks.
        for (const QString& text : {panel.m_clearing_strip->text(), panel.m_progress->text(), panel.m_chart->toolTip(),
                                    panel.m_history_note->toolTip(), panel.m_clearing_strip->toolTip()}) {
            QVERIFY2(!text.contains(QStringLiteral("microblock"), Qt::CaseInsensitive), qPrintable(text));
        }
        auto* interval{panel.findChild<QComboBox*>(QStringLiteral("flowMeshCandleInterval"))};
        for (int i{0}; i < interval->count(); ++i) QVERIFY(!interval->itemText(i).contains(QStringLiteral("microblock"), Qt::CaseInsensitive));
    }
    void heldTicksExtendOnlyConsecutiveAgreeingPages()
    {
        const auto tick = [](uint64_t sequence, bool cleared = false) {
            Tick t; t.sequence = sequence; t.hash = QString::number(sequence); t.kind = QStringLiteral("execution"); t.side = QStringLiteral("none");
            t.cleared = cleared; if (cleared) { t.price = 10; t.volume = 1; } return t;
        };
        const auto page = [](std::vector<Tick> ticks, bool gap = false) { TickPage p; p.available = true; p.gap = gap; p.ticks = std::move(ticks); return p; };
        const auto sequences = [](const std::deque<Tick>& held) { std::vector<uint64_t> out; for (const auto& t : held) out.push_back(t.sequence); return out; };
        std::deque<Tick> held;
        QVERIFY(MergeHeldTicks(held, page({tick(1), tick(2)})));
        QVERIFY(MergeHeldTicks(held, page({tick(3, true)})));
        QVERIFY(MergeHeldTicks(held, page({tick(2), tick(3, true), tick(4)})));
        QCOMPARE(sequences(held), (std::vector<uint64_t>{1, 2, 3, 4}));
        QVERIFY(!MergeHeldTicks(held, TickPage{}));
        QCOMPARE(held.size(), size_t{4});
        // A tick that disagrees with one held, or a skip, starts again.
        QVERIFY(!MergeHeldTicks(held, page({tick(4, true), tick(5)})));
        QCOMPARE(sequences(held), (std::vector<uint64_t>{4, 5}));
        QVERIFY(!MergeHeldTicks(held, page({tick(9)}, true)));
        QCOMPARE(sequences(held), (std::vector<uint64_t>{9}));
        for (uint64_t next{10}; next < 10 + 2 * MAX_HELD_TICKS; ++next) MergeHeldTicks(held, page({tick(next)}));
        QCOMPARE(held.size(), MAX_HELD_TICKS);
        QCOMPARE(held.back().sequence, uint64_t{9 + 2 * MAX_HELD_TICKS});
        // The chart reads cleared ticks as clearings with their exact notional.
        std::deque<Tick> two{tick(1), tick(2, true)};
        const auto trades{TicksAsTrades(two)};
        QCOMPARE(trades.size(), size_t{2}); QVERIFY(!trades[0].cleared); QCOMPARE(trades[1].notional, CAmount{10});
        QCOMPARE(B3FlowMeshChart::AggregateCandles(trades, 1).size(), size_t{1});
    }
    void clearingStripAndIndicativeTextAreLabelledAndOriented()
    {
        const auto s{Parse(RemoteDataWithViews())};
        const auto& tick{s.ticks->ticks[1]};
        QCOMPARE(ClearingTickText(tick, s.units, false, true), QStringLiteral("Last auction · 0.25 tUSD at 1 B3 / tUSD · buyers left 0.05 tUSD unfilled · endpoint-reported"));
        // With its observed time, never its number.
        QCOMPARE(ClearingTickText(tick, s.units, false, false, nullptr, QStringLiteral("12:34:56")), QStringLiteral("Last auction 12:34:56 · 0.25 tUSD at 1 B3 / tUSD · buyers left 0.05 tUSD unfilled"));
        const auto inverse{ClearingTickText(tick, s.units, true, false)};
        QVERIFY(inverse.contains(QStringLiteral("tUSD / B3"))); QVERIFY(inverse.contains(QStringLiteral("B3 sellers left 0.05 tUSD unfilled")));
        QVERIFY(!inverse.contains(QStringLiteral("endpoint-reported")));
        QCOMPARE(ClearingTickText(s.ticks->ticks[0], s.units, false, false), QStringLiteral("Last auction · no match"));
        auto other{tick}; other.kind = QStringLiteral("handoff"); other.cleared = false;
        QCOMPARE(ClearingTickText(other, s.units, false, false), QStringLiteral("Validator handoff · no auction"));
        other = tick; other.side = QStringLiteral("unknown");
        QVERIFY(ClearingTickText(other, s.units, false, false).contains(QStringLiteral("unfilled side not observed")));
        const auto what_if{IndicativeText(*s.indicative, s.units, false)};
        QVERIFY(what_if.startsWith(QStringLiteral("Indicative, not certified")));
        QVERIFY(what_if.contains(QStringLiteral("would clear at 1 B3 / tUSD, filling 0.1875 of 1 tUSD")));
        QVERIFY(what_if.contains(QStringLiteral("other pending orders excluded"))); QVERIFY(what_if.contains(QStringLiteral("replaces your standing order")));
        auto standing{Parse([] { auto v{RemoteData()}; v.pushKV("indicative", IndicativeJson(false)); return v; }())};
        QCOMPARE(IndicativeText(*standing.indicative, standing.units, false), QStringLiteral("Indicative, not certified: no cross · best bid 1 · best ask 1.001"));
        auto bounds{*s.indicative}; bounds.fill_max = 187'501;
        QVERIFY(IndicativeText(bounds, s.units, false).contains(QStringLiteral("filling 0.1875 to 0.187501 of 1 tUSD")));
        Indicative unavailable; unavailable.reason = QStringLiteral("work_budget");
        QCOMPARE(IndicativeText(unavailable, s.units, false), QStringLiteral("Indicative price unavailable (work_budget)"));
    }
    void bookLevelsParseExactlyOrFailClosedPerSection()
    {
        const auto book{SyntheticLimitBook()};
        auto data{Data()};
        data.pushKV("book_levels", LevelsJson(book.curves, H(4), {}, H(5)));
        const auto s{Parse(data)};
        QVERIFY(s.levels && s.levels->available);
        QCOMPARE(s.levels->head, s.head);
        QCOMPARE(s.levels->bids.levels.size(), size_t{8});
        QCOMPARE(s.levels->asks.levels.size(), size_t{8});
        // Exact levels: best bid 1000 with 100,000 of which 0 filled.
        QCOMPARE(s.levels->bids.levels[0].low, CAmount{1000});
        QCOMPARE(s.levels->bids.levels[0].size, CAmount{100'000});
        QCOMPARE(*s.levels->best_bid, CAmount{1000});
        QCOMPARE(*s.levels->best_ask, CAmount{1010});
        CAmount asks{0}; for (const auto& level : s.levels->asks.levels) asks += level.size;
        QCOMPARE(s.levels->asks.levels.back().cumulative, asks);
        // The viewer's own order (account 5 holds none in this book).
        QCOMPARE(s.levels->bids.levels[0].own, CAmount{0});
        QVERIFY(s.levels->bids.levels[0].notional);
        QVERIFY(s.levels->standing.available && !s.levels->standing.has_draft);
        // A canonical grouping of 0.01 B3 per 6-decimal token: 10 ticks.
        auto grouped_data{Data()};
        grouped_data.pushKV("book_levels", LevelsJson(book.curves, H(4), Grouping(false, 1, -2, 6)));
        const auto grouped{Parse(grouped_data)};
        QVERIFY(grouped.levels && grouped.levels->available);
        QVERIFY(grouped.levels->grouping == Grouping(false, 1, -2, 6));
        QCOMPARE(grouped.levels->bids.levels[0].low, CAmount{1000});
        QCOMPARE(grouped.levels->bids.levels[0].high, CAmount{1009});
        // Every tampered section is only unavailable; the read still parses.
        const auto tampered = [&](const std::function<void(UniValue&)>& change) {
            auto value{Data()};
            auto levels{LevelsJson(book.curves, H(4))};
            change(levels);
            value.pushKV("book_levels", levels);
            const auto parsed{Parse(value)};
            return parsed.levels && !parsed.levels->available && parsed.levels->reason == QStringLiteral("malformed") &&
                   parsed.levels->bids.levels.empty() && parsed.curves.size() == 2;
        };
        const auto row = [](UniValue& levels, const char* side, size_t index, const char* key, const UniValue& value) {
            auto rows{levels[side]["levels"]}; auto item{rows[index]}; item.pushKV(key, value);
            UniValue replaced{UniValue::VARR}; for (size_t i{0}; i < rows.size(); ++i) replaced.push_back(i == index ? item : rows[i]);
            auto section{levels[side]}; section.pushKV("levels", replaced); levels.pushKV(side, section);
        };
        QVERIFY(tampered([](UniValue& v) { v.pushKV("head", H(9).GetHex()); }));               // another head
        QVERIFY(tampered([](UniValue& v) { v.pushKV("certified", true); }));
        QVERIFY(tampered([&](UniValue& v) { row(v, "bids", 1, "cumulative", 1); }));           // not the running sum
        QVERIFY(tampered([&](UniValue& v) { row(v, "bids", 1, "low", 1000); row(v, "bids", 1, "high", 1000); row(v, "bids", 1, "bucket", "1000"); })); // overlaps
        QVERIFY(tampered([&](UniValue& v) { row(v, "asks", 0, "high", 1011); }));             // not its bucket
        QVERIFY(tampered([&](UniValue& v) { row(v, "asks", 0, "own", 10'000'000); }));         // own over size
        QVERIFY(tampered([&](UniValue& v) { row(v, "asks", 0, "notional", "1"); }));          // priced outside
        QVERIFY(tampered([&](UniValue& v) { row(v, "asks", 0, "bucket", "-5"); }));
        QVERIFY(tampered([](UniValue& v) { v.pushKV("depth", 4); }));                         // more rows than asked
        QVERIFY(tampered([](UniValue& v) { v.pushKV("best_bid", 990); }));                    // outside the best level
        QVERIFY(tampered([](UniValue& v) { auto g{v["grouping"]}; g.pushKV("exact", false); v.pushKV("grouping", g); }));
        // Unavailable levels keep their reason and show nothing.
        auto missing{Data()};
        UniValue unavailable{UniValue::VOBJ}; unavailable.pushKV("available", false); unavailable.pushKV("unavailable_reason", "work_budget");
        unavailable.pushKV("certified", false); unavailable.pushKV("excludes_pending_actions", true); unavailable.pushKV("basis", "verified_standing_curves");
        unavailable.pushKV("head", H(4).GetHex()); unavailable.pushKV("depth", 50); UniValue exact{UniValue::VOBJ}; exact.pushKV("exact", true); unavailable.pushKV("grouping", exact);
        missing.pushKV("book_levels", unavailable);
        const auto none{Parse(missing)};
        QVERIFY(none.levels && !none.levels->available && none.levels->reason == QStringLiteral("work_budget"));
    }
    void unchangedReadsKeepTheHeadsLevelsOrTakeFreshOnes()
    {
        B3FlowMeshTradingPanel panel; panel.m_timer->stop();
        auto data{Data()};
        data.pushKV("book_levels", LevelsJson(Parse(Data()).curves, H(4)));
        Observe(panel, Parse(data));
        QVERIFY(panel.m_snapshot->levels && panel.m_snapshot->levels->available);
        const auto held{*panel.m_snapshot->levels};
        // A light poll without levels keeps the head's own.
        auto light{Data()}; light.pushKV("unchanged", true);
        QElapsedTimer age; age.start();
        QVERIFY(panel.applySelectedSnapshot(Parse(light), age, panel.m_snapshot->head) == B3FlowMeshTradingPanel::SnapshotApply::APPLIED);
        QVERIFY(panel.m_snapshot->levels && *panel.m_snapshot->levels == held);
        // One that brings them again (another grouping) replaces them.
        light.pushKV("book_levels", LevelsJson(Parse(Data()).curves, H(4), Grouping(false, 1, -2, 6)));
        QVERIFY(panel.applySelectedSnapshot(Parse(light), age, panel.m_snapshot->head) == B3FlowMeshTradingPanel::SnapshotApply::APPLIED);
        QVERIFY(panel.m_snapshot->levels && panel.m_snapshot->levels->grouping == Grouping(false, 1, -2, 6));
    }
    void marketReadsAskForBookLevelsAgainOnlyWhenNeeded()
    {
        FundingReadProbe probe; probe.live_ms = 0;
        probe.data = RemoteData(); probe.data.pushKV("book_levels", LevelsJson(Parse(RemoteData()).curves, H(4)));
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        panel.startJob(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QCOMPARE(probe.market_options.size(), size_t{1});
        const auto first{probe.market_options[0]["levels"]};
        QCOMPARE(first["depth"].getInt<int>(), 50);
        QVERIFY(!first.exists("group"));
        // The displayed snapshot had no levels: asked for even at the same head.
        QVERIFY(first["include_unchanged"].isTrue());
        QVERIFY(panel.m_snapshot->levels && panel.m_snapshot->levels->available);
        // Held for this head in this grouping: an unchanged read computes nothing.
        panel.startJob(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QCOMPARE(probe.market_options.size(), size_t{2});
        QVERIFY(!probe.market_options[1]["levels"].exists("include_unchanged"));
        // The indicative for the ticket's draft is its own section: the book
        // never asks for a draft-free indicative.
        QVERIFY(probe.market_options[1].exists("indicative") && probe.market_options[1]["indicative"].exists("draft"));
        QCOMPARE(probe.writes.load(), 0); QVERIFY(m_wallet->IsLocked());
    }
    void marketReadsAskForHeldTicksAndTheTicketDraft()
    {
        FundingReadProbe probe; probe.live_ms = 0; probe.data = RemoteDataWithViews();
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        // Nothing held yet; the ticket's order awaits its first read.
        QVERIFY(panel.m_clearing_strip->isHidden());
        QVERIFY(panel.m_indicative->text().contains(QStringLiteral("updating")));
        panel.startJob(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QCOMPARE(probe.market_options.size(), size_t{1});
        const auto first{probe.market_options[0]};
        QVERIFY(first["ticks"].isObject() && !first["ticks"].exists("after_sequence"));
        QCOMPARE(first["ticks"]["limit"].getInt<int>(), 256);
        // The offline ticket buys 1 tUSD at 1 B3: canonical raw units.
        QCOMPARE(first["indicative"]["draft"]["side"].get_str(), std::string{"bid"});
        QCOMPARE(first["indicative"]["draft"]["price"].getInt<int64_t>(), int64_t{1000});
        QCOMPARE(first["indicative"]["draft"]["quantity"].getInt<int64_t>(), int64_t{1'000'000});
        QCOMPARE(panel.m_ticks.size(), size_t{2});
        QVERIFY(!panel.m_clearing_strip->isHidden());
        QVERIFY(panel.m_clearing_strip->text().startsWith(QStringLiteral("Last auction · 0.25 tUSD at 1 B3 / tUSD")));
        QVERIFY(panel.m_clearing_strip->toolTip().contains(QStringLiteral("Auction #2")));
        QVERIFY(!panel.m_indicative->isHidden());
        QVERIFY(panel.m_indicative->text().contains(QStringLiteral("would clear at 1 B3 / tUSD")));
        // The chart draws the held ticks.
        QCOMPARE(panel.m_chart->candles().size(), size_t{1});
        // The next read asks only for the ticks after the last one held.
        panel.startJob(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QCOMPARE(probe.market_options.size(), size_t{2});
        QCOMPARE(probe.market_options[1]["ticks"]["after_sequence"].getInt<int>(), 2);
        // Another order in the ticket: its what-if is not claimed until read.
        panel.m_quantity->setText(QStringLiteral("2")); panel.updateTicket();
        QVERIFY(panel.m_indicative->text().contains(QStringLiteral("updating")));
        // Without an order nothing is shown, and reads ask for no indicative.
        panel.m_price->clear(); panel.updateTicket();
        QVERIFY(panel.m_indicative->isHidden());
        panel.startJob(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QCOMPARE(probe.market_options.size(), size_t{3});
        QVERIFY(!probe.market_options[2].exists("indicative")); QVERIFY(probe.market_options[2].exists("ticks"));
        QCOMPARE(probe.writes.load(), 0); QVERIFY(m_wallet->IsLocked());
    }
    void catalogSurvivesUncertifiedFirstMarket()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        const auto certified{panel.m_market_data.front()};
        const auto snapshot{*panel.m_snapshot};
        B3FlowMeshTrading::Market uncertified;
        uncertified.domain = certified.domain; uncertified.config = certified.config;
        uncertified.base = QString::fromStdString(H(90).GetHex());
        const auto id{*flowmesh::ComputeFlowMeshMarketId(H(1), H(90))};
        uncertified.id = QString::fromStdString(id.GetHex());
        uncertified.vault = QString::fromStdString(flowmesh::ComputeFlowMeshVaultId(H(1), id)->GetHex());
        uncertified.remote = true;
        panel.m_market_data.clear(); panel.m_snapshot.reset();
        { QSignalBlocker blocked{panel.m_market}; panel.m_market->clear(); }
        panel.m_response_age.invalidate(); panel.m_loading = true;

        // Production completed-result boundary: discovery succeeded, then the
        // first remote market's balance RPC rejected its uncertified head.
        auto failed{std::make_shared<B3FlowMeshTradingPanel::Result>()};
        failed->catalog = true; failed->markets = {uncertified, certified};
        failed->error = QStringLiteral("FlowMesh client snapshot has no certified head");
        panel.finishJob(failed);
        QCOMPARE(panel.m_market->count(), 2);
        QVERIFY(panel.market()); QCOMPARE(panel.market()->id, uncertified.id);
        QVERIFY(panel.m_market->isEnabled()); QVERIFY(panel.m_catalog_age.isValid());
        QVERIFY(panel.m_read_failed); QVERIFY(!panel.m_loading); QVERIFY(!panel.m_snapshot);
        panel.updateMarketText(); // Periodic updates must retain the exact error.
        QVERIFY(panel.m_status->text().contains(failed->error));
        QVERIFY(!panel.m_status->text().contains(QStringLiteral("Loading")));
        QVERIFY(!panel.m_liquidity_note->text().contains(QStringLiteral("Loading")));
        QVERIFY(!panel.m_order->isEnabled()); QVERIFY(!panel.m_deposit->isEnabled());
        QVERIFY(!panel.m_withdraw->isEnabled()); QVERIFY(!panel.m_checkpoint->isEnabled());
        QVERIFY(!panel.m_publish->isEnabled()); QVERIFY(!panel.m_admit->isEnabled());

        // Another listed market stays selectable, and its independently
        // successful read can make only that market ready.
        ReadInFlight(panel);
        panel.m_market->setCurrentIndex(1);
        QCOMPARE(panel.market()->id, certified.id);
        QVERIFY(!panel.m_status->text().contains(failed->error));
        auto ready{std::make_shared<B3FlowMeshTradingPanel::Result>()};
        ready->markets = failed->markets; ready->snapshot = snapshot;
        panel.finishJob(ready);
        QCOMPARE(panel.m_market->currentIndex(), 1);
        QVERIFY(panel.m_snapshot && *panel.m_snapshot == snapshot);
        QVERIFY(!panel.m_read_failed); QVERIFY(panel.m_order->isEnabled());
        QCOMPARE(panel.m_chart->pricePointCount(), 1);
        QVERIFY(!panel.m_thread); QVERIFY(!failed->write_attempted); QVERIFY(!ready->write_attempted);
        QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
    }
    void catalogFailureRetainsCertifiedDisplayAndPausesActions()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        const auto snapshot{*panel.m_snapshot};
        const auto original{panel.m_market_data.front()};
        const auto balances{panel.m_balances->text()};
        const auto history{panel.m_history_view->item(0, 1)->text()};
        auto discovery{original}; discovery.has_account = false; discovery.account.clear();
        discovery.base_available = discovery.base_reserved = discovery.b3_available = discovery.b3_reserved = 0;
        discovery.ready = discovery.publish_ready = false;
        auto additional{discovery}; additional.id = QString::fromStdString(H(91).GetHex());
        auto failed{std::make_shared<B3FlowMeshTradingPanel::Result>()};
        failed->catalog = true; failed->markets = {discovery, additional};
        failed->error = QStringLiteral("Synthetic selected-market read unavailable");
        panel.finishJob(failed);
        QCOMPARE(panel.m_market->count(), 2);
        QCOMPARE(panel.market()->id, original.id);
        QCOMPARE(panel.market()->account, original.account);
        QVERIFY(panel.m_snapshot && *panel.m_snapshot == snapshot);
        QCOMPARE(panel.m_balances->text(), balances);
        QCOMPARE(panel.m_history_view->item(0, 1)->text(), history);
        QCOMPARE(panel.m_chart->pricePointCount(), 1);
        QVERIFY(panel.m_read_failed); QVERIFY(panel.m_market->isEnabled());
        panel.updateMarketText(); QVERIFY(panel.m_status->text().contains(failed->error));
        QVERIFY(panel.m_order->isEnabled()); QVERIFY(!panel.orderReady()); QVERIFY(!panel.m_cancel_order->isEnabled());
        QVERIFY(panel.m_deposit->isEnabled()); QVERIFY(!panel.depositReady()); QVERIFY(!panel.m_admit->isEnabled());
        QVERIFY(panel.m_withdraw->isEnabled()); QVERIFY(!panel.withdrawalReady()); QVERIFY(!panel.m_checkpoint->isEnabled());
        QVERIFY(!panel.m_publish->isEnabled()); QVERIFY(m_wallet->IsLocked());
    }
    void queuedCatalogFromPreviousWalletIsDiscarded()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        auto result{std::make_shared<B3FlowMeshTradingPanel::Result>()};
        result->catalog = true; result->markets = panel.m_market_data;
        result->error = QStringLiteral("Old wallet selected-market read failed");
        const auto generation{panel.m_generation};
        int delivered{0}, accepted{0};
        QMetaObject::invokeMethod(&panel, [&] {
            ++delivered;
            if (generation == panel.m_generation) { ++accepted; panel.finishJob(result); }
        }, Qt::QueuedConnection);
        auto replacement{MakeOfflineWallet("catalog-replacement-wallet")};
        panel.setWalletModel(replacement.model.get());
        QCoreApplication::sendPostedEvents(&panel, QEvent::MetaCall);
        QCOMPARE(delivered, 1); QCOMPARE(accepted, 0);
        QCOMPARE(panel.m_wallet.data(), replacement.model.get());
        QCOMPARE(panel.m_market->count(), 0); QVERIFY(panel.m_market_data.empty());
        QVERIFY(!panel.m_snapshot); QVERIFY(!panel.m_read_failed);
        QVERIFY(!panel.m_status->text().contains(result->error));
        QVERIFY(!panel.m_order->isEnabled()); QVERIFY(!panel.m_thread);
        panel.setWalletModel(nullptr);
    }
    void failedCatalogReadDoesNotReplaceExistingMarkets()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        const auto original{panel.m_market_data.front()};
        const auto snapshot{*panel.m_snapshot};
        auto failed{std::make_shared<B3FlowMeshTradingPanel::Result>()};
        // No successfully parsed catalog: an empty or partially filled result
        // cannot remove the cached choices or replace certified account data.
        failed->error = QStringLiteral("Synthetic market discovery unavailable");
        panel.finishJob(failed);
        QCOMPARE(panel.m_market->count(), 1); QCOMPARE(panel.market()->id, original.id);
        QCOMPARE(panel.market()->account, original.account);
        QVERIFY(panel.m_snapshot && *panel.m_snapshot == snapshot);
        QVERIFY(panel.m_read_failed); QVERIFY(panel.m_order->isEnabled()); QVERIFY(!panel.orderReady());
        QVERIFY(!panel.m_catalog_age.isValid()); QVERIFY(m_wallet->IsLocked());
    }
    void connectionControlsDistinguishTransportFromMarketReadiness()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        QVERIFY(panel.findChild<QLineEdit*>(QStringLiteral("flowMeshEndpoint")));
        QVERIFY(panel.findChild<QPushButton*>(QStringLiteral("flowMeshConnect")));
        QVERIFY(panel.findChild<QLabel*>(QStringLiteral("flowMeshConnectionStatus")));
        QVERIFY(!panel.m_connect->isEnabled());
        panel.m_client_info = ConnectionInfo(""); panel.updateControls();
        QVERIFY(panel.m_connection_status->text().contains(QStringLiteral("No HTTPS endpoint")));
        panel.m_endpoint->setText(QStringLiteral("http://insecure.invalid")); QVERIFY(!panel.m_connect->isEnabled());
        panel.m_endpoint->setText(QStringLiteral("https://trading.invalid")); QVERIFY(panel.m_connect->isEnabled());
        panel.m_client_info = ConnectionInfo("https://trading.invalid", false); panel.updateControls();
        QVERIFY(panel.m_connection_status->text().contains(QStringLiteral("not yet checked")));
        panel.m_client_info = ConnectionInfo("https://trading.invalid", true, false, false); panel.updateControls();
        QVERIFY(panel.m_connection_status->text().contains(QStringLiteral("HTTPS connection failed")));
        panel.m_client_info->pushKV("active_endpoint", "https://fallback.invalid"); panel.updateControls();
        QVERIFY(panel.m_connection_status->text().contains(QStringLiteral("Last usable endpoint: https://fallback.invalid")));
        panel.m_client_info = ConnectionInfo("https://trading.invalid", true, true, false);
        panel.m_read_failed = true; panel.m_read_error = QStringLiteral("FlowMesh client snapshot has no certified head"); panel.updateMarketText();
        QVERIFY(panel.m_connection_status->text().contains(QStringLiteral("HTTPS responded")));
        QVERIFY(panel.m_connection_status->text().contains(QStringLiteral("no certified head")));
        QVERIFY(panel.m_status->text().contains(panel.m_read_error)); QVERIFY(panel.m_order->isEnabled()); QVERIFY(!panel.orderReady());
        panel.m_client_info = ConnectionInfo(); panel.updateControls();
        QVERIFY(panel.m_connection_status->text().contains(QStringLiteral("readiness is shown below")));
        QVERIFY(panel.m_order->isEnabled()); QVERIFY(!panel.orderReady()); // Transport success supplies no fresh certificate.
        panel.m_client_info->pushKV("backend", "local"); panel.m_client_info->pushKV("engine_enabled", true); panel.updateControls();
        QVERIFY(panel.m_connection_status->text().contains(QStringLiteral("Local market engine")));
        QVERIFY(!panel.m_connect->isEnabled()); QVERIFY(!panel.m_endpoint->isEnabled());
        panel.requestConnect(); QVERIFY(!panel.m_thread); QVERIFY(!panel.m_deferred_connect);
    }
    void connectUsesExistingWorkerWithoutUnlockOrSubmission()
    {
        ConnectionProbe probe; B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        struct Cleanup { ConnectionProbe& probe; B3FlowMeshTradingPanel& panel; ~Cleanup() { probe.release.release(8); panel.cancelAndWait(); } } cleanup{probe, panel};
        panel.m_client_info = ConnectionInfo(""); panel.m_endpoint->setText(QStringLiteral("https://chosen.invalid")); panel.updateControls();
        const auto snapshot{*panel.m_snapshot}; const auto balances{panel.m_balances->text()};
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        panel.m_connect->click(); QVERIFY(probe.entered.tryAcquire(1, 1000));
        const auto active{panel.m_active_result}; QVERIFY(active); QVERIFY(!active->action); QVERIFY(!active->exact_retry);
        QCOMPARE(active->connect_url, QStringLiteral("https://chosen.invalid")); QVERIFY(!active->write_attempted);
        QVERIFY(panel.m_snapshot && *panel.m_snapshot == snapshot); QCOMPARE(panel.m_balances->text(), balances);
        QVERIFY(panel.m_connection_status->text().contains(QStringLiteral("Connecting to")));
        QVERIFY(!panel.m_order->isEnabled()); QVERIFY(!panel.m_connect->isEnabled());
        panel.requestConnect(); QCOMPARE(probe.count.load(), 1); // Coalesced while running.
        panel.m_endpoint->setText(QStringLiteral("https://still-typing.invalid"));
        panel.m_endpoint->setCursorPosition(8); const int cursor{panel.m_endpoint->cursorPosition()};
        probe.release.release(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QCOMPARE(probe.count.load(), 1); QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
        QVERIFY(!active->write_attempted); QVERIFY(!panel.m_uncertain); QVERIFY(!panel.m_confirmation);
        QCOMPARE(panel.m_endpoint->text(), QStringLiteral("https://still-typing.invalid"));
        QCOMPARE(panel.m_endpoint->cursorPosition(), cursor);
        QVERIFY(panel.m_snapshot && *panel.m_snapshot == snapshot); QCOMPARE(panel.m_balances->text(), balances);
        QVERIFY(panel.m_read_failed); QVERIFY(panel.m_order->isEnabled()); QVERIFY(!panel.orderReady());
        { std::lock_guard lock{probe.mutex}; QCOMPARE(probe.requests.size(), size_t{1});
          QCOMPARE(probe.requests[0].first, B3AssetTransfer::WalletUri(m_model->getWalletName()));
          QCOMPARE(probe.requests[0].second, std::string{"https://chosen.invalid"}); }
    }
    void connectQueuesOnceBehindPassiveRead()
    {
        ConnectionProbe probe; B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        struct Cleanup { ConnectionProbe& probe; B3FlowMeshTradingPanel& panel; ~Cleanup() { probe.release.release(8); panel.cancelAndWait(); } } cleanup{probe, panel};
        panel.m_client_info = ConnectionInfo(); panel.m_endpoint->setText(QStringLiteral("https://queued.invalid")); panel.updateControls();
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        ReadInFlight(panel); const auto original{panel.m_thread};
        for (int i{0}; i < 5; ++i) panel.m_connect->click();
        QCOMPARE(panel.m_thread, original); QCOMPARE(probe.count.load(), 0); QVERIFY(panel.m_deferred_connect);
        QCOMPARE(panel.m_deferred_connect->url, QStringLiteral("https://queued.invalid"));
        QVERIFY(panel.m_connection_status->text().contains(QStringLiteral("Connect queued")));
        QVERIFY(!panel.m_order->isEnabled());
        panel.m_endpoint->setText(QStringLiteral("https://edited-after-click.invalid"));
        auto failed{std::make_shared<B3FlowMeshTradingPanel::Result>()}; failed->error = QStringLiteral("Synthetic passive read failed");
        panel.finishJob(failed); QVERIFY(probe.entered.tryAcquire(1, 1000));
        QVERIFY(!panel.m_deferred_connect); QCOMPARE(panel.m_active_result->connect_url, QStringLiteral("https://queued.invalid"));
        probe.release.release(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QCOMPARE(probe.count.load(), 1); QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
        QCOMPARE(panel.m_endpoint->text(), QStringLiteral("https://edited-after-click.invalid"));
    }
    void explicitConnectionErrorSurvivesAutomaticStatusReads()
    {
        ConnectionProbe probe; probe.fail = true;
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        struct Cleanup { ConnectionProbe& probe; B3FlowMeshTradingPanel& panel; ~Cleanup() { probe.release.release(8); panel.cancelAndWait(); } } cleanup{probe, panel};
        panel.m_client_info = ConnectionInfo(); panel.m_endpoint->setText(QStringLiteral("https://rejected.invalid")); panel.updateControls();
        panel.m_connect->click(); QVERIFY(probe.entered.tryAcquire(1, 1000));
        probe.release.release(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QVERIFY(panel.m_client_info); QVERIFY(panel.m_connection_error.isEmpty());
        QCOMPARE(panel.m_connect_error, QStringLiteral("Synthetic endpoint configuration rejected"));
        panel.updateMarketText(); QVERIFY(panel.m_connection_status->text().contains(panel.m_connect_error));
        QVERIFY(panel.m_order->isEnabled()); QVERIFY(!panel.orderReady()); QVERIFY(m_wallet->IsLocked());
        panel.setWalletModel(nullptr); QVERIFY(panel.m_connect_error.isEmpty());
    }
    void connectionWorkCannotFollowWalletSwitch_data()
    {
        QTest::addColumn<bool>("completed");
        QTest::newRow("queued-connect") << false;
        QTest::newRow("completion-queued") << true;
    }
    void connectionWorkCannotFollowWalletSwitch()
    {
        QFETCH(bool, completed);
        ConnectionProbe probe; B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        struct Cleanup { ConnectionProbe& probe; B3FlowMeshTradingPanel& panel; ~Cleanup() { probe.release.release(8); panel.cancelAndWait(); } } cleanup{probe, panel};
        panel.m_client_info = ConnectionInfo(); panel.m_endpoint->setText(QStringLiteral("https://old-wallet.invalid")); panel.updateControls();
        const auto generation{panel.m_generation};
        if (!completed) ReadInFlight(panel);
        panel.m_connect->click();
        if (completed) {
            QVERIFY(probe.entered.tryAcquire(1, 1000)); probe.release.release();
            QVERIFY(panel.m_thread->wait(1000)); // Finished callback queued, not applied.
        } else { QVERIFY(panel.m_deferred_connect); QCOMPARE(probe.count.load(), 0); }
        auto replacement{MakeOfflineWallet("connection-replacement-wallet")};
        panel.setWalletModel(replacement.model.get());
        QCoreApplication::sendPostedEvents(&panel, QEvent::MetaCall); QCoreApplication::processEvents();
        QVERIFY(panel.m_generation > generation); QCOMPARE(panel.m_wallet.data(), replacement.model.get());
        QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_deferred_connect);
        QVERIFY(!panel.m_client_info); QVERIFY(panel.m_connection_error.isEmpty()); QVERIFY(panel.m_connect_error.isEmpty());
        QVERIFY(!panel.m_snapshot); QVERIFY(!panel.m_order->isEnabled()); QVERIFY(!panel.m_connect->isEnabled());
        QCOMPARE(probe.count.load(), completed ? 1 : 0); QVERIFY(m_wallet->IsLocked());
        panel.setWalletModel(nullptr);
    }
    void chartShowsOnlyCertifiedRecordsAndHonestEmptiness()
    {
        B3FlowMeshChart chart; chart.resize(640, 340); QCOMPARE(chart.pricePointCount(), 0); chart.setLoading(true); QVERIFY(chart.emptyMessage().contains(QStringLiteral("Reading")));
        chart.setSnapshot(Parse(Data(false))); QCOMPARE(chart.pricePointCount(), 0);
        QVERIFY(chart.emptyMessage().contains(QStringLiteral("No trades yet")));
        QVERIFY(chart.emptyMessage().contains(QStringLiteral("history window")));
        auto unavailable{Parse(Data(false))}; unavailable.history_available = false;
        chart.setSnapshot(unavailable); QCOMPARE(chart.pricePointCount(), 0);
        QVERIFY(chart.emptyMessage().contains(QStringLiteral("unavailable")));
        chart.setSnapshot(Parse(Data())); QCOMPARE(chart.pricePointCount(), 1); QImage image{chart.size(), QImage::Format_ARGB32}; image.fill(Qt::transparent); chart.render(&image); QVERIFY(!image.isNull());
        chart.setMode(B3FlowMeshChart::Mode::Liquidity); chart.setStale(true); chart.render(&image); chart.setSnapshot(std::nullopt); QCOMPARE(chart.pricePointCount(), 0);
    }
    void candleAggregationUsesActualClearingsAndSequenceBuckets()
    {
        const auto trade = [](uint64_t sequence, CAmount price, CAmount quantity) {
            Trade t; t.sequence = sequence; t.cleared = true; t.price = price; t.quantity = quantity; t.notional = price * quantity; return t;
        };
        QVERIFY(B3FlowMeshChart::AggregateCandles({}, 5).empty());
        // Endpoint order cannot choose open/close; empty sequence buckets have
        // no invented prices or volume. Unmatched auctions are not trades.
        Trade idle; idle.sequence = 6;
        std::vector<Trade> history{trade(21, 800, 50), trade(4, 1100, 400), idle, trade(2, 1300, 200), trade(8, 1200, 1), trade(1, 1000, 100), trade(3, 900, 300)};
        const auto unchanged{history};
        const auto candles{B3FlowMeshChart::AggregateCandles(history, 5)};
        QCOMPARE(candles.size(), size_t{3});
        const auto& first{candles.front()};
        QCOMPARE(first.bucket_sequence, uint64_t{0}); QCOMPARE(first.first_sequence, uint64_t{1}); QCOMPARE(first.last_sequence, uint64_t{4});
        QCOMPARE(first.open, CAmount{1000}); QCOMPARE(first.high, CAmount{1300}); QCOMPARE(first.low, CAmount{900}); QCOMPARE(first.close, CAmount{1100});
        QCOMPARE(first.trades, size_t{4}); QVERIFY(first.base_volume == 1000); QVERIFY(first.quote_volume == 1'070'000);
        QCOMPARE(B3FlowMeshChart::FormatCandleVolume(first, 6, false), QStringLiteral("0.001"));
        QCOMPARE(B3FlowMeshChart::FormatCandleVolume(first, 6, true), QStringLiteral("0.001"));
        QCOMPARE(candles[1].bucket_sequence, uint64_t{5}); QCOMPARE(candles[2].bucket_sequence, uint64_t{20});
        QCOMPARE(candles[1].open, candles[1].high); QCOMPARE(candles[1].high, candles[1].low); QCOMPARE(candles[1].low, candles[1].close);
        QCOMPARE(B3FlowMeshChart::AggregateCandles(history, 1).size(), size_t{6});
        QCOMPARE(B3FlowMeshChart::AggregateCandles(history, 20).size(), size_t{2});
        QVERIFY(history == unchanged);

        const auto reversed{B3FlowMeshChart::AggregateCandles(history, 5, true)};
        QCOMPARE(reversed.size(), candles.size());
        QCOMPARE(reversed[0].open, first.open); QCOMPARE(reversed[0].close, first.close);
        QCOMPARE(reversed[0].high, first.low); QCOMPARE(reversed[0].low, first.high);
        // Display only: 900 B3 atoms per token atom is 10/9 tokens per B3.
        QCOMPARE(FormatDisplayPrice(reversed[0].high, 6, true), QStringLiteral("≈1.111"));
        QCOMPARE(DisplayPrice(reversed[0].high, 6, true).exact, QStringLiteral("10/9"));
        QVERIFY(reversed[0].base_volume == first.base_volume); QVERIFY(reversed[0].quote_volume == first.quote_volume);
        const auto boundary{B3FlowMeshChart::AggregateCandles({trade(std::numeric_limits<uint64_t>::max(), 1, 1)}, 20)};
        QCOMPARE(boundary.size(), size_t{1}); QCOMPARE(boundary.front().last_sequence, std::numeric_limits<uint64_t>::max());
    }
    void candleVolumeKeepsAssetUnitsWhenPriceIsInverted()
    {
        Trade up; up.sequence = 1; up.cleared = true; up.price = 100;
        up.quantity = 1'000'000; up.notional = up.price * up.quantity;
        Trade down{up}; down.sequence = 2; down.price = 200; down.notional = down.price * down.quantity;
        const auto canonical{B3FlowMeshChart::AggregateCandles({up, down}, 1, false)};
        const auto inverse{B3FlowMeshChart::AggregateCandles({up, down}, 1, true)};
        // Each clearing exchanges exactly 1 rUSD. Price orientation and B3
        // notional must not turn those equal-volume bars into 0.1 / 0.2 B3.
        QCOMPARE(canonical.size(), size_t{2}); QCOMPARE(inverse.size(), size_t{2});
        for (size_t i{0}; i < inverse.size(); ++i) {
            QCOMPARE(B3FlowMeshChart::FormatCandleVolume(inverse[i], 6, true), QStringLiteral("1"));
            QCOMPARE(B3FlowMeshChart::FormatCandleVolume(canonical[i], 6, false), QStringLiteral("1"));
        }
        QCOMPARE(B3FlowMeshChart::FormatCandleVolume(inverse.front(), 2, true), QStringLiteral("10000"));
        QCOMPARE(B3FlowMeshChart::FormatCandleVolume(inverse.front(), 18, true), QStringLiteral("0.000000000001"));
        QCOMPARE(B3FlowMeshChart::FormatCandleVolume(inverse.front(), -1, true), QStringLiteral("—"));
        QCOMPARE(B3FlowMeshChart::FormatCandleVolume(inverse.front(), 19, true), QStringLiteral("—"));
    }
    void candleAggregationRejectsInvalidAmountsAndUndefinedInverseBuckets()
    {
        const auto valid{Parse(Data()).history.front()};
        QVERIFY(B3FlowMeshChart::AggregateCandles({valid}, 0).empty());
        QVERIFY(B3FlowMeshChart::AggregateCandles({valid, valid}, 5).empty());
        for (const int kind : {0, 1, 2, 3, 4, 5}) {
            auto invalid{valid};
            switch (kind) {
            case 0: invalid.price = -1; break;
            case 1: invalid.quantity = 0; break;
            case 2: invalid.price = MAX_MONEY; invalid.quantity = 2; break;
            case 3: ++invalid.notional; break;
            case 4: invalid.fee = -1; break;
            case 5: invalid.cleared = false; break;
            }
            QVERIFY(B3FlowMeshChart::AggregateCandles({invalid}, 5).empty());
        }
        auto zero{valid}; zero.sequence = 3; zero.price = zero.notional = zero.fee = 0;
        auto later{valid}; later.sequence = 9;
        const std::vector<Trade> history{valid, zero, later};
        QCOMPARE(B3FlowMeshChart::AggregateCandles(history, 5).size(), size_t{2});
        const auto inverse{B3FlowMeshChart::AggregateCandles(history, 5, true)};
        QCOMPARE(inverse.size(), size_t{1}); QCOMPARE(inverse.front().first_sequence, uint64_t{9});
        // A zero-price low means this entire candle's inverse high is
        // undefined; dropping only that trade would invent a finite high.
        QVERIFY(B3FlowMeshChart::AggregateCandles({valid, zero}, 5, true).empty());

        std::vector<Trade> large;
        for (uint64_t i{0}; i < 20; ++i) { auto t{valid}; t.sequence = i; t.price = 1; t.quantity = t.notional = MAX_MONEY; t.fee = 0; large.push_back(t); }
        const auto volumes{B3FlowMeshChart::AggregateCandles(large, 20)};
        QCOMPARE(volumes.size(), size_t{1});
        QVERIFY(volumes.front().base_volume > static_cast<uint64_t>(std::numeric_limits<CAmount>::max()));
        QCOMPARE(B3FlowMeshChart::FormatCandleVolume(volumes.front(), 0, false), QStringLiteral("13244000000000000000"));
        QCOMPARE(B3FlowMeshChart::FormatCandleVolume(volumes.front(), 6, true), QStringLiteral("13244000000000"));
    }
    void candleIntervalsRetainSnapshotAndRenderGallery()
    {
        B3FlowMeshChart chart; chart.resize(900, 420);
        auto snapshot{Parse(Data())}; snapshot.history.clear(); snapshot.next_sequence = 100;
        // Deterministic isolated display fixture, never an RPC or wallet feed.
        const CAmount prices[]{1000, 1080, 960, 1060, 1100, 1100, 1140, 1020, 1040, 1030};
        for (uint64_t i{0}; i < 60; ++i) {
            Trade t; t.sequence = i; t.cleared = true; t.price = prices[i % 10] + static_cast<CAmount>(i / 10) * 30;
            t.quantity = 10'000 + static_cast<CAmount>(i % 7) * 2'000; t.notional = t.price * t.quantity;
            snapshot.history.push_back(t);
        }
        chart.setSnapshot(snapshot); QCOMPARE(chart.candleInterval(), uint64_t{5}); QCOMPARE(chart.candles().size(), size_t{12});
        chart.setCandleInterval(20); QCOMPARE(chart.candles().size(), size_t{3});
        const auto retained{chart.candles()};
        chart.setLoading(true); chart.setStale(true); chart.setSnapshot(snapshot);
        QVERIFY(chart.candles() == retained); QCOMPARE(chart.candleInterval(), uint64_t{20});
        chart.setCandleInterval(0); QCOMPARE(chart.candleInterval(), uint64_t{20});
        chart.setCandleInterval(5); chart.setLoading(false); chart.setStale(false);
        QImage image{chart.size(), QImage::Format_ARGB32}; image.fill(Qt::transparent);
        chart.render(&image);
        bool positive{false}, negative{false};
        for (int y{0}; y < image.height(); ++y) for (int x{0}; x < image.width(); ++x) {
            positive |= image.pixelColor(x, y) == B3Theme::kPositive;
            negative |= image.pixelColor(x, y) == B3Theme::kNegative;
        }
        QVERIFY(positive); QVERIFY(negative);
        // Optional local visual QA export. Test data stays in this test only.
        const QString gallery{qEnvironmentVariable("B3_FLOWMESH_CHART_GALLERY")};
        if (!gallery.isEmpty()) {
            QVERIFY(QDir{}.mkpath(gallery));
            QVERIFY(image.save(QDir{gallery}.filePath(QStringLiteral("candles-canonical.png"))));
        }
        chart.setInverted(true); chart.render(&image);
        QCOMPARE(chart.candles().front().high, CAmount{960}); QCOMPARE(chart.candles().front().low, CAmount{1100});
        if (!gallery.isEmpty()) QVERIFY(image.save(QDir{gallery}.filePath(QStringLiteral("candles-inverse.png"))));
        chart.setMode(B3FlowMeshChart::Mode::Liquidity); chart.render(&image);
        if (!gallery.isEmpty()) QVERIFY(image.save(QDir{gallery}.filePath(QStringLiteral("candles-depth.png"))));
        snapshot.remote = true; snapshot.execution_result_verified = false;
        chart.setSnapshot(snapshot); chart.setMode(B3FlowMeshChart::Mode::Prices); chart.render(&image);
        if (!gallery.isEmpty()) QVERIFY(image.save(QDir{gallery}.filePath(QStringLiteral("candles-reported.png"))));
        snapshot.certified = false; chart.setSnapshot(snapshot); QVERIFY(chart.candles().empty());
    }
    void runtimeStatusDoesNotRepaintUnchangedChart()
    {
        B3FlowMeshChart chart;
        chart.resize(640, 340);
        auto snapshot{Parse(RemoteData())};
        chart.setSnapshot(snapshot);
        chart.show();
        for (int warmup{0}; warmup < 3; ++warmup) QCoreApplication::processEvents();
        struct Paints final : QObject {
            int count{0};
            bool eventFilter(QObject*, QEvent* event) override {
                if (event->type() == QEvent::Paint) ++count;
                return false;
            }
        } paints;
        chart.installEventFilter(&paints);
        const auto candles{chart.candles()};
        for (int poll{0}; poll < 6; ++poll) {
            snapshot.chain_reconciling = snapshot.paused = poll % 2 == 0;
            snapshot.pending_actions = poll;
            chart.setSnapshot(snapshot);
            QCoreApplication::processEvents();
            QCOMPARE(chart.candles(), candles);
        }
        // Runtime status is rendered by the panel's status labels. It must not
        // invalidate the unchanged plot on every B3 tip reconciliation.
        QCOMPARE(paints.count, 0);
        const auto changed = [&] {
            paints.count = 0;
            chart.setSnapshot(snapshot);
            QCoreApplication::processEvents();
            return paints.count > 0;
        };
        snapshot.history.front().price += 1;
        snapshot.history.front().notional = snapshot.history.front().price * snapshot.history.front().quantity;
        QVERIFY(changed());
        QVERIFY(chart.candles() != candles);
        snapshot.execution_result_verified = true; QVERIFY(changed());
        snapshot.event_gap = true; QVERIFY(changed());
        snapshot.units.ticker = QStringLiteral("changed"); QVERIFY(changed());
        snapshot.depth.front().demand += 1; QVERIFY(changed());
        snapshot.certified = false; QVERIFY(changed()); QVERIFY(chart.candles().empty());
        paints.count = 0; chart.setStale(true); QCoreApplication::processEvents();
        QVERIFY(paints.count > 0);
    }
    void renderCapturedEngineOffLiquidity()
    {
        const QString capture{qEnvironmentVariable("B3_FLOWMESH_CHART_CAPTURE")};
        if (capture.isEmpty()) QSKIP("Set B3_FLOWMESH_CHART_CAPTURE to an isolated engine-off public market-data JSON capture.");
        const QString gallery{qEnvironmentVariable("B3_FLOWMESH_CHART_GALLERY")};
        QVERIFY2(!gallery.isEmpty(), "Set B3_FLOWMESH_CHART_GALLERY to the output directory.");
        QFile file{capture};
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.errorString()));
        QVERIFY(file.size() > 0 && file.size() <= 16 * 1024 * 1024);
        const QByteArray bytes{file.readAll()};
        QCOMPARE(file.error(), QFileDevice::NoError);
        UniValue value;
        QVERIFY(value.read(bytes.toStdString()));
        // Use precisely the normal parser and immutable captured provenance.
        // This does not replay execution or promote remote history to verified.
        const auto snapshot{Parse(value)};
        QVERIFY(snapshot.remote); QVERIFY(snapshot.certified);
        QVERIFY(snapshot.certificate_verified); QVERIFY(snapshot.account_state_verified);
        QVERIFY(!snapshot.execution_result_verified);
        QVERIFY(snapshot.units.known); QVERIFY(snapshot.history_available); QVERIFY(snapshot.curves_complete);
        const auto clearings{std::count_if(snapshot.history.begin(), snapshot.history.end(), [](const auto& trade) { return trade.cleared && trade.quantity > 0; })};
        QVERIFY(clearings > 0);
        QVERIFY(std::any_of(snapshot.depth.begin(), snapshot.depth.end(), [](const auto& row) { return row.demand > 0; }));
        QVERIFY(std::any_of(snapshot.depth.begin(), snapshot.depth.end(), [](const auto& row) { return row.supply > 0; }));
        B3FlowMeshChart chart; chart.resize(1000, 460); chart.setSnapshot(snapshot);
        QCOMPARE(chart.pricePointCount(), static_cast<int>(clearings));
        QVERIFY(!chart.candles().empty());
        QVERIFY(QDir{}.mkpath(gallery));
        QImage image{chart.size(), QImage::Format_ARGB32}; image.fill(Qt::transparent);
        chart.render(&image);
        QVERIFY(image.save(QDir{gallery}.filePath(QStringLiteral("capture-candles-canonical.png"))));
        chart.setInverted(true); QVERIFY(!chart.candles().empty()); chart.render(&image);
        QVERIFY(image.save(QDir{gallery}.filePath(QStringLiteral("capture-candles-inverse.png"))));
        chart.setMode(B3FlowMeshChart::Mode::Liquidity); chart.render(&image);
        QVERIFY(image.save(QDir{gallery}.filePath(QStringLiteral("capture-depth.png"))));
        const auto book{ProjectLimitBook(snapshot, false)};
        QVERIFY(book.complete); QCOMPARE(book.asks.size(), size_t{1}); QCOMPARE(book.bids.size(), size_t{1});
        QCOMPARE(book.bids.front().canonical_price, CAmount{30'000'000}); QCOMPARE(book.bids.front().remaining, CAmount{3});
        QCOMPARE(book.asks.front().canonical_price, CAmount{80'000'000}); QCOMPARE(book.asks.front().remaining, CAmount{5});
        const QString heading{QStringLiteral("Captured generated-regtest orders · %1 / B3 · history remains endpoint-reported").arg(snapshot.units.ticker)};
        QVERIFY(SaveOrderBookGallery(snapshot, false, gallery, QStringLiteral("capture-orderbook-canonical.png"), heading));
        QVERIFY(SaveOrderBookGallery(snapshot, true, gallery, QStringLiteral("capture-orderbook-inverse.png"), heading));
        QVERIFY(snapshot == Parse(value));
    }
    void renderSyntheticMultilevelOrderBookGallery()
    {
        const QString gallery{qEnvironmentVariable("B3_FLOWMESH_CHART_GALLERY")};
        if (gallery.isEmpty()) QSKIP("Set B3_FLOWMESH_CHART_GALLERY to export explicitly synthetic multi-level book images.");
        QVERIFY(QDir::isAbsolutePath(gallery)); QVERIFY(QDir{}.mkpath(gallery));
        auto snapshot{SyntheticLimitBook(10)};
        snapshot.history.clear();
        for (uint64_t sequence{0}; sequence < 40; ++sequence) {
            Trade trade; trade.sequence = sequence;
            trade.hash = QString::fromStdString(H(static_cast<unsigned char>(120 + sequence)).GetHex());
            trade.cleared = true; trade.price = 992 + std::array<CAmount, 10>{0, 4, 2, 6, 9, 8, 6, 11, 7, 3}[sequence % 10];
            trade.quantity = 100'000 + static_cast<CAmount>(sequence % 4) * 25'000;
            trade.notional = trade.price * trade.quantity; trade.fee = *FeeExample(trade.notional);
            snapshot.history.push_back(trade);
        }
        snapshot.next_sequence = 40; snapshot.head = snapshot.history.back().hash;
        const auto original{snapshot}; const auto projection{ProjectLimitBook(snapshot, false)};
        QCOMPARE(projection.asks.size(), size_t{10}); QCOMPARE(projection.bids.size(), size_t{10});
        QVERIFY(SaveOrderBookGallery(snapshot, false, gallery, QStringLiteral("synthetic-orderbook-canonical.png"),
            QStringLiteral("Synthetic UI fixture · invented display values only · no wallet, node, or market feed")));
        QVERIFY(SaveOrderBookGallery(snapshot, true, gallery, QStringLiteral("synthetic-orderbook-inverse.png"),
            QStringLiteral("Synthetic UI fixture · reverse B3/token view · not captured market liquidity")));
        QVERIFY(snapshot == original);
    }
    void walletlessWorkspaceRendersDataButCanNeverSubmit()
    {
        B3FlowMeshTradingPanel panel; panel.resize(1200, 850); panel.m_timer->stop();
        const auto s{Parse(Data())}; B3FlowMeshTrading::Market m; m.id = s.market; m.base = s.base; m.domain = s.domain; m.config = s.config; m.vault = QString::fromStdString(flowmesh::ComputeFlowMeshVaultId(H(1), *uint256::FromHex(s.market.toStdString()))->GetHex()); m.account = s.account; m.has_account = true; m.ready = true; m.publish_ready = true; m.base_available = s.base_available; m.b3_available = s.b3_available;
        m.base_reserved = s.base_reserved; m.b3_reserved = s.b3_reserved;
        panel.m_market_data = {m}; { QSignalBlocker block{panel.m_market}; panel.m_market->addItem(QStringLiteral("tUSD / B3"), m.id); } panel.m_snapshot = s; panel.m_response_age.start(); panel.updateDataViews(); panel.updateMarketText();
        QCOMPARE(panel.m_chart->pricePointCount(), 1); QCOMPARE(panel.m_history_view->rowCount(), 1); QCOMPARE(panel.m_own_view->rowCount(), 1);
        auto* prices{panel.findChild<QPushButton*>(QStringLiteral("flowMeshChartPrices"))};
        auto* liquidity{panel.findChild<QPushButton*>(QStringLiteral("flowMeshChartLiquidity"))};
        QVERIFY(prices); QVERIFY(liquidity); QVERIFY(prices->isChecked());
        auto* interval{panel.findChild<QComboBox*>(QStringLiteral("flowMeshCandleInterval"))}; QVERIFY(interval);
        interval->setCurrentIndex(2); QCOMPARE(panel.m_chart->candleInterval(), uint64_t{20});
        liquidity->click(); QVERIFY(liquidity->isChecked()); QVERIFY(!prices->isChecked());
        QVERIFY(!interval->isEnabled());
        prices->click(); QVERIFY(prices->isChecked()); QVERIFY(!liquidity->isChecked());
        QVERIFY(interval->isEnabled()); QCOMPARE(panel.m_chart->candleInterval(), uint64_t{20});
        QCOMPARE(panel.m_own_view->item(0, 3)->text(), QStringLiteral("0.75 B3"));
        QVERIFY(panel.m_balances->text().contains(QStringLiteral("In orders  0 tUSD · 0.75 B3")));
        panel.m_price->setText(QStringLiteral("1")); panel.m_quantity->setText(QStringLiteral("1")); QVERIFY(panel.m_ticket_total->text().contains(QStringLiteral("1 B3"))); QVERIFY(!panel.m_order->isEnabled()); QVERIFY(!panel.m_deposit->isEnabled()); QVERIFY(!panel.m_advanced->isVisible());
        panel.m_read_failed = true; panel.updateMarketText();
        QVERIFY(panel.m_status->text().contains(QStringLiteral("Updates delayed")));
        QVERIFY(panel.m_status->text().contains(QStringLiteral("paused")));
        QVERIFY(panel.m_status->toolTip().contains(QStringLiteral("stale")));
        QVERIFY(!panel.m_order->isEnabled());
        panel.m_queue_age.start(); QVERIFY(panel.m_queue_age.isValid());
        panel.setWalletModel(nullptr); QVERIFY(!panel.m_snapshot); QVERIFY(!panel.m_queue_age.isValid()); QCOMPARE(panel.m_chart->pricePointCount(), 0); QCOMPARE(panel.m_history_view->rowCount(), 0); QVERIFY(!panel.m_order->isEnabled());
    }
    void inverseOrientationChangesOnlyPresentation()
    {
        B3FlowMeshTradingPanel panel;
        QCOMPARE(panel.m_orientation->currentIndex(), 1); // Production default.
        const auto snapshot{Parse(Data())};
        Observe(panel, snapshot);
        auto* orientation{panel.findChild<QComboBox*>(QStringLiteral("flowMeshOrientation"))};
        QVERIFY(orientation);
        QCOMPARE(orientation->currentIndex(), 0);
        const auto original{panel.m_market_data.front()};
        panel.m_price->setText(QStringLiteral("0.5"));
        panel.m_quantity->setText(QStringLiteral("1"));
        orientation->setCurrentIndex(1);
        QVERIFY(panel.m_price->text().isEmpty()); // Never reinterpret an old limit.
        QCOMPARE(panel.m_quantity->text(), QStringLiteral("1"));
        QVERIFY(panel.m_snapshot && *panel.m_snapshot == snapshot);
        QCOMPARE(panel.m_market_data.front().id, original.id);
        QCOMPARE(panel.m_market_data.front().base, original.base);
        QCOMPARE(panel.m_market_data.front().vault, original.vault);
        QCOMPARE(panel.m_market_data.front().account, original.account);
        QCOMPARE(panel.m_market_data.front().sequence, original.sequence);
        QCOMPARE(panel.m_chart->pricePointCount(), 1);
        QVERIFY(panel.m_pair_title->text().contains(QStringLiteral("B3")));
        QVERIFY(panel.m_price_label->text().contains(QStringLiteral("tUSD / B3")));
        QVERIFY(panel.m_quantity_label->text().contains(QStringLiteral("Spend")));
        QVERIFY(panel.m_quantity_label->text().contains(QStringLiteral("tUSD")));
        QVERIFY(panel.m_buy->text().contains(QStringLiteral("B3")));
        QVERIFY(panel.m_own_view->item(0, 0)->text().contains(QStringLiteral("Sell")));
        QCOMPARE(panel.m_own_view->item(0, 3)->text(), QStringLiteral("0.75 B3"));
        QCOMPARE(panel.m_depth_view->item(0, 0)->text(),
                 FormatDisplayPrice(snapshot.depth.back().price, snapshot.units.decimals, true));
        QCOMPARE(panel.m_depth_view->item(0, 1)->text(), FormatDepthQuantity(
            snapshot.depth.back().price, snapshot.depth.back().supply, snapshot.units.decimals, true));
        QCOMPARE(panel.m_depth_view->item(0, 2)->text(), FormatDepthQuantity(
            snapshot.depth.back().price, snapshot.depth.back().demand, snapshot.units.decimals, true));
        QCOMPARE(panel.m_depth_view->horizontalHeaderItem(1)->text(), QStringLiteral("Buy liquidity"));
        QCOMPARE(panel.m_depth_view->horizontalHeaderItem(2)->text(), QStringLiteral("Sell liquidity"));
        QCOMPARE(panel.m_depth_view->item(0, 1)->foreground().color(), B3Theme::kPositive);
        QCOMPARE(panel.m_depth_view->item(0, 2)->foreground().color(), B3Theme::kNegative);
        QVERIFY(panel.m_liquidity_note->text().contains(QStringLiteral("Aggregate curves")));
        QVERIFY(panel.m_liquidity_note->toolTip().contains(QStringLiteral("do not sum")));
        QCOMPARE(panel.m_history_view->item(0, 3)->text(), QStringLiteral("0"));
        QCOMPARE(panel.m_history_view->item(0, 4)->text(), QStringLiteral("0.25"));
        QVERIFY(panel.m_history_note->text().contains(QStringLiteral("gross B3 before fees")));
        panel.m_price->setText(QStringLiteral("2"));
        QVERIFY(panel.m_ticket_total->text().contains(QStringLiteral("0.5 B3")));
        QVERIFY(!panel.m_order->isEnabled()); // This display fixture has no wallet.
        QVERIFY(!panel.m_thread);
        orientation->setCurrentIndex(0);
        QVERIFY(panel.m_price->text().isEmpty());
        QVERIFY(panel.m_snapshot && *panel.m_snapshot == snapshot);
        QCOMPARE(panel.m_own_view->item(0, 0)->text(), QStringLiteral("Buy"));
        QCOMPARE(panel.m_depth_view->item(0, 0)->text(),
                 FormatPrice(snapshot.depth.front().price, snapshot.units.decimals));
        QVERIFY(!panel.m_depth_view->item(0, 0)->toolTip().contains(QStringLiteral("inverse")));
        QVERIFY(!panel.m_own_view->item(0, 1)->toolTip().contains(QStringLiteral("inverse")));
        panel.m_busy = true;
        panel.updateControls();
        QVERIFY(!orientation->isEnabled()); // Review context cannot be relabeled mid-flight.
    }
    void inverseReviewUsesSelectedReplacementBudgetAndNeverAutoApproves()
    {
        B3FlowMeshTradingPanel panel;
        AttachOfflineWallet(panel);
        auto snapshot{Parse(Data())};
        snapshot.base_available = 250'000;
        snapshot.base_reserved = 750'000;
        snapshot.b3_available = 250'000'000;
        snapshot.b3_reserved = 750'000'000;
        auto own_ask{snapshot.curves.back()}; own_ask.account = snapshot.account;
        snapshot.curves.push_back(own_ask); snapshot.own_curves.push_back(own_ask);
        snapshot.depth = Aggregate(snapshot.curves);
        Observe(panel, snapshot);
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        auto* orientation{panel.findChild<QComboBox*>(QStringLiteral("flowMeshOrientation"))};
        QVERIFY(orientation);
        QVERIFY(orientation->isEnabled());
        orientation->setCurrentIndex(1);
        panel.m_price->setText(QStringLiteral("2"));
        panel.m_quantity->setText(QStringLiteral("1"));
        QVERIFY(panel.m_ticket_available->text().contains(QStringLiteral("0.25 tUSD")));
        QVERIFY(panel.m_ticket_available->toolTip().contains(QStringLiteral("0.75 tUSD")));
        QVERIFY(panel.m_ticket_available->toolTip().contains(QStringLiteral("1 tUSD")));
        QVERIFY(panel.m_ticket_total->text().contains(QStringLiteral("0.5 B3")));
        QVERIFY(panel.m_ticket_fee->toolTip().contains(QStringLiteral("B3")));
        QVERIFY(panel.m_ticket_fee->text().contains(QStringLiteral("Actual fee asset: B3")));
        QVERIFY(panel.m_ticket_fee->toolTip().contains(QStringLiteral("unknown before execution")));
        QString review;
        bool cancel_default{false}, orientation_locked{false};
        QTimer::singleShot(0, &panel, [&] {
            if (panel.m_confirmation) {
                review = panel.m_confirmation->text();
                cancel_default = panel.m_confirmation->defaultButton() == panel.m_confirmation->button(QMessageBox::Cancel);
                orientation_locked = !orientation->isEnabled();
                panel.m_confirmation->done(QMessageBox::Cancel);
            }
        });
        panel.begin(B3FlowMeshTrading::Operation::Order);
        QVERIFY(review.contains(QStringLiteral("Buy B3: Spend up to 1 tUSD")));
        QVERIFY(review.contains(QStringLiteral("Canonical side: ask")));
        QVERIFY(review.contains(QStringLiteral("deducted from the B3 you receive")));
        QVERIFY(cancel_default);
        QVERIFY(orientation_locked);
        QCOMPARE(unlock.count(), 0);
        QVERIFY(m_wallet->IsLocked());
        QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_unlock);
        panel.m_sell->click();
        QVERIFY(panel.m_quantity_label->text().contains(QStringLiteral("Receive")));
        QVERIFY(panel.m_ticket_available->text().contains(QStringLiteral("0.25 B3")));
        QVERIFY(panel.m_ticket_available->toolTip().contains(QStringLiteral("0.75 B3")));
        QVERIFY(panel.m_ticket_available->toolTip().contains(QStringLiteral("1 B3")));
        review.clear();
        QTimer::singleShot(0, &panel, [&] {
            if (panel.m_confirmation) {
                review = panel.m_confirmation->text();
                panel.m_confirmation->done(QMessageBox::Cancel);
            }
        });
        panel.begin(B3FlowMeshTrading::Operation::Order);
        QVERIFY(review.contains(QStringLiteral("Sell B3: Receive up to 1 tUSD")));
        QVERIFY(review.contains(QStringLiteral("Canonical side: bid")));
        QVERIFY(review.contains(QStringLiteral("not deducted from your token receipt")));
        QCOMPARE(unlock.count(), 0);
        QVERIFY(m_wallet->IsLocked());
        QVERIFY(!panel.m_confirmation); QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result);
        QCOMPARE(panel.m_market_data.front().base, snapshot.base);
        QCOMPARE(panel.m_market_data.front().id, snapshot.market);
        // Off-grid inverse input adjusts only conservatively, and the exact
        // executable price must be disclosed before any unlock/signing.
        panel.m_price->setText(QStringLiteral("1.1"));
        review.clear();
        QTimer::singleShot(0, &panel, [&] {
            if (panel.m_confirmation) {
                review = panel.m_confirmation->text();
                panel.m_confirmation->done(QMessageBox::Cancel);
            }
        });
        panel.begin(B3FlowMeshTrading::Operation::Order);
        QVERIFY(review.contains(QStringLiteral("adjusted only in your favor")));
        QVERIFY(review.contains(QStringLiteral("1000/909 tUSD / B3")));
        QVERIFY(review.contains(QStringLiteral("Maximum B3 reservation/spend")));
        QVERIFY(!panel.m_confirmation); QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result);
        QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
    }
    void reverseDefaultQuantityFeeAndAssetIdentityAreFaithful()
    {
        B3FlowMeshTradingPanel panel;
        QCOMPARE(panel.m_orientation->currentIndex(), 1);
        AttachOfflineWallet(panel);
        const auto snapshot{Parse(Data())}; Observe(panel, snapshot, true);
        QCOMPARE(panel.m_pair_title->text(), QStringLiteral("B3 / tUSD"));
        QCOMPARE(panel.m_buy->text(), QStringLiteral("Buy B3"));
        QCOMPARE(panel.m_sell->text(), QStringLiteral("Sell B3"));
        panel.m_price->setText(QStringLiteral("1.1")); panel.m_quantity->setText(QStringLiteral("0.000001"));
        QVERIFY(panel.m_quantity_label->text().contains(QStringLiteral("Spend up to")));
        QVERIFY(panel.m_ticket_total->text().contains(QStringLiteral("100/91 tUSD / B3")));
        QVERIFY(panel.m_ticket_total->text().contains(QStringLiteral("if fully filled")));
        QVERIFY(panel.m_ticket_total->toolTip().contains(QStringLiteral("No fill is guaranteed")));
        QCOMPARE(panel.m_history_view->item(0, 5)->text(), QStringLiteral("0.000025"));
        QVERIFY(panel.m_history_view->horizontalHeaderItem(5)->text().contains(QStringLiteral("B3")));
        QVERIFY(panel.m_history_view->horizontalHeaderItem(5)->toolTip().contains(QStringLiteral("not your individual")));
        panel.m_sell->click();
        QVERIFY(panel.m_quantity_label->text().contains(QStringLiteral("Receive up to")));
        QVERIFY(panel.m_ticket_total->text().contains(QStringLiteral("Maximum B3 reservation/spend: 0.000000909 B3")));
        QVERIFY(panel.m_ticket_total->text().contains(QStringLiteral("1000/909 tUSD / B3")));
        QString cancellation_review;
        QTimer::singleShot(0, &panel, [&] {
            if (panel.m_confirmation) {
                cancellation_review = panel.m_confirmation->text();
                panel.m_confirmation->done(QMessageBox::Cancel);
            }
        });
        panel.begin(B3FlowMeshTrading::Operation::Cancel);
        QVERIFY(cancellation_review.contains(QStringLiteral("standing Sell B3 (bid)")));
        QVERIFY(cancellation_review.contains(QStringLiteral("not certified until processed")));
        QVERIFY(!panel.m_thread); QVERIFY(m_wallet->IsLocked());
        // Same ticker does not authorize another AssetId's precision.
        panel.m_snapshot->units.asset = QString::fromStdString(H(98).GetHex());
        panel.updateTicket(); panel.updateControls(); QVERIFY(!panel.m_order->isEnabled());
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        panel.begin(B3FlowMeshTrading::Operation::Order);
        QVERIFY(!panel.m_confirmation); QVERIFY(!panel.m_thread); QCOMPARE(unlock.count(), 0);
        QVERIFY(!panel.orderReady()); QVERIFY(panel.m_log->toPlainText().contains(QStringLiteral("Nothing was signed or submitted")));
        QCOMPARE(panel.m_market_data.front().id, snapshot.market);
    }
    void restoredReverseCardsRetainCanonicalInstructionAndMeaning()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        Observe(panel, Parse(Data()), true);
        B3FlowMeshTrading::SavedActions saved; saved.account = panel.m_snapshot->account;
        B3FlowMeshTrading::SavedAction action;
        action.market = panel.m_snapshot->market; action.domain = panel.m_snapshot->domain; action.config = panel.m_snapshot->config;
        action.account = saved.account; action.sequence = 7; action.type = 1; action.initial_submission_ms = 1000;
        action.signed_bytes_sha256 = QString::fromStdString(H(96).GetHex()); action.signed_bytes_size = 279; action.may_have_been_sent = true;
        action.canonical_side = QStringLiteral("ask"); action.canonical_points = {{909, 0}, {910, 1'000'000}};
        action.receipt.action_id = QString::fromStdString(H(95).GetHex()); action.receipt.state = QStringLiteral("unknown");
        saved.actions.push_back(action); panel.restoreSavedActions(saved);
        QVERIFY(panel.m_receipt_card->text().contains(QStringLiteral("Buy B3 = sell tUSD; canonical ask")));
        QVERIFY(panel.m_receipt_card->text().contains(QStringLiteral("Spend up to 1 tUSD")));
        QVERIFY(panel.m_receipt_card->text().contains(QStringLiteral("100/91 tUSD / B3")));
        QVERIFY(panel.m_receipt_card->text().contains(QStringLiteral("no fee amount is inferred from inclusion")));
        const auto prior{panel.m_receipt_card->text()};
        panel.m_orientation->setCurrentIndex(0); panel.updateReceiptCard();
        QCOMPARE(panel.m_receipt_card->text(), prior); // Legacy canonical history has not changed meaning.
        QCOMPARE(panel.m_saved_actions.actions[0].canonical_points, action.canonical_points);
        QCOMPARE(panel.m_saved_actions.actions[0].signed_bytes_sha256, action.signed_bytes_sha256);
        QCOMPARE(panel.m_pending_sequence, action.sequence); QVERIFY(panel.m_uncertain);
        auto wrong_metadata{*panel.m_snapshot}; wrong_metadata.units.asset = QString::fromStdString(H(98).GetHex());
        panel.m_snapshot = wrong_metadata; panel.updateReceiptCard();
        QVERIFY(panel.m_receipt_card->text().contains(QStringLiteral("Buy B3 = sell configured token; canonical ask")));
        QVERIFY(!panel.m_receipt_card->text().contains(QStringLiteral("Spend up to 1 tUSD")));
        saved.actions[0].receipt.no_resubmit = true; panel.restoreSavedActions(saved);
        QVERIFY(panel.m_receipt->no_resubmit); QVERIFY(!panel.m_retry_receipt->isEnabled());
        QVERIFY(!panel.m_uncertain); QVERIFY(!panel.m_thread);
    }
    void inverseChartRetainsCertifiedDataAndHandlesZeroBoundary()
    {
        B3FlowMeshChart chart;
        chart.resize(640, 340);
        auto snapshot{Parse(Data())};
        // A canonical ask curve may have a zero-price/zero-volume boundary;
        // its reciprocal is undefined, never a fabricated finite price.
        snapshot.depth.insert(snapshot.depth.begin(), Depth{0, 0, 0});
        chart.setSnapshot(snapshot);
        chart.setInverted(true);
        QCOMPARE(chart.pricePointCount(), 1);
        QImage image{chart.size(), QImage::Format_ARGB32};
        image.fill(Qt::transparent);
        chart.render(&image);
        QVERIFY(!image.isNull());
        chart.setMode(B3FlowMeshChart::Mode::Liquidity);
        chart.render(&image);
        chart.setInverted(false);
        chart.render(&image);
        QCOMPARE(chart.pricePointCount(), 1);
        snapshot.units.known = false;
        chart.setSnapshot(snapshot);
        chart.setInverted(true);
        QVERIFY(chart.emptyMessage().contains(QStringLiteral("precision unavailable")));
        chart.render(&image);
    }
    void identicalSnapshotsPreserveCellsSelectionAndScroll()
    {
        // Synthetic display data only. A long retained history makes a real,
        // nonzero scroll position observable without any wallet or RPC.
        auto snapshot{Parse(Data())};
        const auto trade{snapshot.history.front()};
        snapshot.history.clear();
        for (uint64_t i{0}; i < 40; ++i) {
            auto entry{trade}; entry.sequence = i + 2;
            entry.hash = QString::fromStdString(H(static_cast<unsigned char>(i + 20)).GetHex());
            snapshot.history.push_back(entry);
        }
        snapshot.next_sequence = 42;
        snapshot.head = snapshot.history.back().hash;
        B3FlowMeshTradingPanel panel;
        panel.resize(1200, 850);
        Observe(panel, snapshot);
        panel.m_history_view->setFixedHeight(160);
        panel.m_history_view->setSelectionMode(QAbstractItemView::SingleSelection);
        panel.m_history_view->setSelectionBehavior(QAbstractItemView::SelectRows);
        panel.m_activity->setCurrentWidget(panel.m_history_view->parentWidget());
        panel.show();
        QCoreApplication::processEvents();
        QCOMPARE(panel.m_history_view->rowCount(), 40);
        panel.m_history_view->selectRow(20);
        // selectRow may make the leading column current. Establish the exact
        // current cell AFTER row selection without changing that selection,
        // and verify the precondition before testing refresh preservation.
        panel.m_history_view->selectionModel()->setCurrentIndex(
            panel.m_history_view->model()->index(20, 1), QItemSelectionModel::NoUpdate);
        QCOMPARE(panel.m_history_view->currentIndex(), panel.m_history_view->model()->index(20, 1));
        auto* scroll{panel.m_history_view->verticalScrollBar()};
        QVERIFY(scroll->maximum() > 0);
        scroll->setValue(std::max(1, scroll->maximum() / 2));
        const int position{scroll->value()};
        QVERIFY(position > 0);
        const QPersistentModelIndex selected{panel.m_history_view->currentIndex()};
        const auto selection{panel.m_history_view->selectionModel()->selectedIndexes()};
        QVERIFY(!selection.isEmpty());
        const std::array<QTableWidgetItem*, 3> cells{
            panel.m_depth_view->item(0, 0), panel.m_history_view->item(20, 1), panel.m_own_view->item(0, 0)};
        for (auto* cell : cells) QVERIFY(cell);
        QSignalSpy removed{panel.m_history_view->model(), &QAbstractItemModel::rowsRemoved};
        QSignalSpy inserted{panel.m_history_view->model(), &QAbstractItemModel::rowsInserted};
        QSignalSpy reset{panel.m_history_view->model(), &QAbstractItemModel::modelReset};
        for (int refresh{0}; refresh < 5; ++refresh) {
            panel.m_snapshot = snapshot; // Equal full response, not pointer reuse.
            panel.m_response_age.restart();
            panel.updateDataViews();
            panel.updateMarketText();
        }
        QCoreApplication::processEvents();
        QCOMPARE(panel.m_depth_view->item(0, 0), cells[0]);
        QCOMPARE(panel.m_history_view->item(20, 1), cells[1]);
        QCOMPARE(panel.m_own_view->item(0, 0), cells[2]);
        QVERIFY(selected.isValid());
        QCOMPARE(panel.m_history_view->currentIndex(), QModelIndex{selected});
        QCOMPARE(panel.m_history_view->selectionModel()->selectedIndexes(), selection);
        QCOMPARE(scroll->value(), position);
        QCOMPARE(removed.count(), 0); QCOMPARE(inserted.count(), 0); QCOMPARE(reset.count(), 0);
    }
    void identicalRefreshHasNoModelChurn_data()
    {
        QTest::addColumn<bool>("inverse");
        QTest::addColumn<bool>("remote");
        QTest::newRow("canonical-local") << false << false;
        QTest::newRow("inverse-local") << true << false;
        QTest::newRow("canonical-remote") << false << true;
        QTest::newRow("inverse-remote") << true << true;
    }
    void identicalRefreshHasNoModelChurn()
    {
        QFETCH(bool, inverse);
        QFETCH(bool, remote);
        auto snapshot{Parse(remote ? RemoteData() : Data())};
        const auto trade{snapshot.history.front()};
        snapshot.history.clear();
        for (uint64_t i{0}; i < 40; ++i) {
            auto entry{trade}; entry.sequence = i + 2;
            entry.hash = QString::fromStdString(H(static_cast<unsigned char>(i + 20)).GetHex());
            snapshot.history.push_back(entry);
        }
        snapshot.next_sequence = 42; snapshot.head = snapshot.history.back().hash;
        B3FlowMeshTradingPanel panel;
        panel.resize(1200, 850);
        AttachOfflineWallet(panel); // Mock wallet only; no RPC, sockets or timer.
        Observe(panel, snapshot, inverse);
        panel.m_history_view->setFixedHeight(160);
        panel.m_history_view->setSelectionMode(QAbstractItemView::SingleSelection);
        panel.m_activity->setCurrentWidget(panel.m_history_view->parentWidget());
        panel.m_price->setText(QStringLiteral("1.009"));
        panel.m_quantity->setText(QStringLiteral("0.25"));
        panel.m_saved_selector->addItem(QStringLiteral("retained first"), QStringLiteral("first"));
        panel.m_saved_selector->addItem(QStringLiteral("retained selected"), QStringLiteral("selected"));
        { QSignalBlocker blocked{panel.m_saved_selector}; panel.m_saved_selector->setCurrentIndex(1); }
        panel.show();
        for (int warmup{0}; warmup < 3; ++warmup) QCoreApplication::processEvents();
        panel.m_history_view->selectRow(20);
        auto* scroll{panel.m_history_view->verticalScrollBar()};
        QVERIFY(scroll->maximum() > 0);
        scroll->setValue(std::max(1, scroll->maximum() / 2));
        panel.m_price->setFocus(); panel.m_price->setSelection(2, 2);
        QCoreApplication::processEvents();
        QVERIFY(panel.m_price->hasFocus());
        const int position{scroll->value()}, cursor{panel.m_price->cursorPosition()};
        const auto selection{panel.m_history_view->selectionModel()->selectedIndexes()};
        const QPersistentModelIndex selected{panel.m_history_view->currentIndex()};
        const auto* depth_cell{panel.m_depth_view->item(0, 0)};
        const auto* history_cell{panel.m_history_view->item(20, 1)};
        const auto* own_cell{panel.m_own_view->item(0, 0)};
        QVERIFY(depth_cell); QVERIFY(history_cell); QVERIFY(own_cell);
        const auto final_copy = [&] {
            return QStringList{panel.m_history_note->text(), panel.m_history_note->toolTip(),
                panel.m_last_price->text(), panel.m_identity_detail->text(), panel.m_progress->text(),
                panel.m_status->text(), panel.m_status->toolTip(), panel.m_ticket_fee->toolTip(),
                panel.m_ticket_available->toolTip(), panel.m_ticket_total->toolTip(),
                panel.m_depth_view->item(0, 0)->toolTip(), panel.m_own_view->item(0, 1)->toolTip()};
        };
        const QStringList before_copy{final_copy()};
        QCOMPARE(panel.m_history_note->text(),
                 (remote ? QStringLiteral("Endpoint-reported trades · execution results not independently verified") : QStringLiteral("Confirmed trades")) +
                 (inverse ? QStringLiteral(" · gross B3 before fees") : QString{}));
        QCOMPARE(panel.m_ticket_fee->toolTip(), QStringLiteral("Actual protocol fee asset: B3. The whole-auction fee is 0.01% of matched B3 and is allocated among B3 receivers. Your exact allocated amount is unknown before execution; this is not a stable-asset charge. Submitting an order has no network fee."));
        QCOMPARE(panel.m_progress->text(), StatusText(snapshot, 0, -1) + QStringLiteral("\nLatest certified auction #41 · epoch 0 · configured threshold 3 of 4 FN seats · 0 queued actions\nAn available runtime or configured threshold is not proof that those validators are currently online."));
        QSignalSpy depth_changed{panel.m_depth_view->model(), &QAbstractItemModel::dataChanged};
        QSignalSpy history_changed{panel.m_history_view->model(), &QAbstractItemModel::dataChanged};
        QSignalSpy own_changed{panel.m_own_view->model(), &QAbstractItemModel::dataChanged};
        QSignalSpy market_changed{panel.m_market->model(), &QAbstractItemModel::dataChanged};
        QSignalSpy orientation_changed{panel.m_orientation->model(), &QAbstractItemModel::dataChanged};
        QSignalSpy reset{panel.m_history_view->model(), &QAbstractItemModel::modelReset};
        struct Events final : QObject {
            int layouts{0}, paints{0}, tooltips{0};
            bool eventFilter(QObject*, QEvent* event) override {
                if (event->type() == QEvent::LayoutRequest) ++layouts;
                if (event->type() == QEvent::Paint) ++paints;
                if (event->type() == QEvent::ToolTipChange) ++tooltips;
                return false;
            }
        } events;
        panel.installEventFilter(&events);
        for (auto* child : panel.findChildren<QWidget*>()) child->installEventFilter(&events);
        for (int refresh{0}; refresh < 5; ++refresh) {
            panel.m_snapshot = snapshot; panel.m_response_age.restart();
            panel.updateDataViews(); panel.updateMarketText();
            QCoreApplication::processEvents();
        }
        qInfo("identical refreshes=5 depth_data=%lld history_data=%lld own_data=%lld market_data=%lld orientation_data=%lld layouts=%d paints=%d tooltips=%d",
              static_cast<long long>(depth_changed.count()), static_cast<long long>(history_changed.count()),
              static_cast<long long>(own_changed.count()), static_cast<long long>(market_changed.count()),
              static_cast<long long>(orientation_changed.count()), events.layouts, events.paints, events.tooltips);
        QCOMPARE(panel.m_price->text(), QStringLiteral("1.009"));
        QCOMPARE(panel.m_quantity->text(), QStringLiteral("0.25"));
        QCOMPARE(panel.m_price->cursorPosition(), cursor);
        QCOMPARE(panel.m_price->selectedText(), QStringLiteral("00"));
        QVERIFY(panel.m_price->hasFocus());
        QCOMPARE(panel.m_saved_selector->currentData().toString(), QStringLiteral("selected"));
        QCOMPARE(final_copy(), before_copy);
        QCOMPARE(panel.m_depth_view->item(0, 0), depth_cell);
        QCOMPARE(panel.m_history_view->item(20, 1), history_cell);
        QCOMPARE(panel.m_own_view->item(0, 0), own_cell);
        QCOMPARE(panel.m_history_view->currentIndex(), QModelIndex{selected});
        QCOMPARE(panel.m_history_view->selectionModel()->selectedIndexes(), selection);
        QCOMPARE(scroll->value(), position); QCOMPARE(reset.count(), 0);
        // A same-value refresh must not temporarily clear and restore item roles.
        // Event counts are diagnostics, not a claim of visible Cocoa flicker.
        QCOMPARE(depth_changed.count(), 0); QCOMPARE(history_changed.count(), 0);
        QCOMPARE(own_changed.count(), 0); QCOMPARE(market_changed.count(), 0);
        QCOMPARE(orientation_changed.count(), 0);
        const int changed_row{inverse ? panel.m_depth_view->rowCount() - 1 : 0};
        const auto before{panel.m_depth_view->item(changed_row, 1)->text()};
        auto& changed{*panel.m_snapshot};
        if (inverse) changed.depth.front().supply += 1'000'000;
        else changed.depth.front().demand += 1'000'000;
        panel.updateDataViews(); QCoreApplication::processEvents();
        QVERIFY(depth_changed.count() > 0); // Real updates are not suppressed.
        QVERIFY(panel.m_depth_view->item(changed_row, 1)->text() != before);
    }
    void identicalRefreshHasNoHeaderChurnAndLazyDepthTips()
    {
        // Identical header text changes nothing (Qt skips it), and a depth
        // row's exact inverse price is formatted only when its tooltip is
        // read (the table is usually hidden).
        B3FlowMeshTradingPanel panel; panel.resize(1200, 850);
        AttachOfflineWallet(panel);
        const auto snapshot{Parse(RemoteData())};
        Observe(panel, snapshot, true);
        QSignalSpy depth{panel.m_depth_view->model(), &QAbstractItemModel::headerDataChanged};
        QSignalSpy history{panel.m_history_view->model(), &QAbstractItemModel::headerDataChanged};
        QSignalSpy own{panel.m_own_view->model(), &QAbstractItemModel::headerDataChanged};
        for (int i{0}; i < 3; ++i) { panel.m_snapshot = snapshot; panel.updateDataViews(); }
        QCOMPARE(depth.count(), 0); QCOMPARE(history.count(), 0); QCOMPARE(own.count(), 0);
        QVERIFY(panel.m_history_view->horizontalHeaderItem(1)->text().contains(QStringLiteral("tUSD / B3")));
        QVERIFY(panel.m_depth_view->horizontalHeaderItem(2)->toolTip().contains(QStringLiteral("Gross quantity in B3")));
        // Another orientation relabels once.
        { QSignalBlocker blocked{panel.m_orientation}; panel.m_orientation->setCurrentIndex(0); }
        panel.updateDataViews(); panel.updateDataViews();
        QVERIFY(depth.count() > 0 && history.count() > 0);
        QVERIFY(panel.m_history_view->horizontalHeaderItem(1)->text().contains(QStringLiteral("B3 / tUSD")));
        QCOMPARE(panel.m_depth_view->item(0, 0)->toolTip(), QString{});
        const int relabels{static_cast<int>(depth.count())};
        panel.updateDataViews(); QCOMPARE(static_cast<int>(depth.count()), relabels);
        // Inverse: the exact price, from the cell's own canonical price.
        { QSignalBlocker blocked{panel.m_orientation}; panel.m_orientation->setCurrentIndex(1); }
        panel.updateDataViews();
        for (int row{0}; row < panel.m_depth_view->rowCount(); ++row) {
            auto* item{panel.m_depth_view->item(row, 0)};
            QCOMPARE(item->type(), QTableWidgetItem::UserType + 1);
            const auto price{snapshot.depth[snapshot.depth.size() - 1 - static_cast<size_t>(row)].price};
            QCOMPARE(item->toolTip(), QStringLiteral("Exact inverse price: %1").arg(ExactInversePrice(price, 6)));
        }
    }
    void reconciliationRefreshRetainsInputAndReceipts()
    {
        B3FlowMeshTradingPanel panel;
        AttachOfflineWallet(panel);
        Observe(panel, Parse(RemoteData()));
        panel.m_market_data.front().remote = true;
        SavedReadFixture(panel);
        panel.resize(1200, 850); panel.show();
        panel.m_price->setText(QStringLiteral("1.009"));
        panel.m_quantity->setText(QStringLiteral("0.25"));
        panel.m_price->setFocus(); panel.m_price->setSelection(2, 2);
        QCoreApplication::processEvents();
        QVERIFY(panel.m_price->hasFocus());
        const QString receipt{panel.m_receipt_card->text()}, balance{panel.m_balances->text()};
        const auto* history_cell{panel.m_history_view->item(0, 1)};
        const auto* own_cell{panel.m_own_view->item(0, 0)};
        const int cursor{panel.m_price->cursorPosition()};
        QSignalSpy reset{panel.m_history_view->model(), &QAbstractItemModel::modelReset};
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        struct EnabledChanges final : QObject {
            int count{0};
            bool eventFilter(QObject*, QEvent* event) override {
                if (event->type() == QEvent::EnabledChange) ++count;
                return false;
            }
        } enabled_changes;
        for (QWidget* widget : std::initializer_list<QWidget*>{&panel, panel.m_price, panel.m_quantity,
                 panel.m_market, panel.m_saved_selector, panel.m_chart}) widget->installEventFilter(&enabled_changes);
        for (int poll{0}; poll < 6; ++poll) {
            const bool reconciling{poll % 2 == 0};
            auto result{std::make_shared<B3FlowMeshTradingPanel::Result>()};
            result->markets = panel.m_market_data;
            result->snapshot = panel.m_snapshot;
            result->snapshot->unchanged = true;
            result->snapshot->chain_reconciling = result->snapshot->paused = reconciling;
            result->snapshot->error = reconciling ? QStringLiteral("FlowMesh service is not active at the current B3 tip") : QString{};
            result->markets.front().ready = result->markets.front().publish_ready = !reconciling;
            ReadInFlight(panel);
            panel.finishJob(result);
            QCoreApplication::processEvents();
            QVERIFY2(panel.m_snapshot.has_value(), qPrintable(panel.m_status->text()));
            QVERIFY2(panel.market().has_value(), qPrintable(panel.m_status->text()));
            QVERIFY(!panel.m_busy); QVERIFY(!panel.m_thread);
            QVERIFY(panel.m_price->isEnabled()); QVERIFY(panel.m_quantity->isEnabled());
            QVERIFY(panel.m_market->isEnabled()); QVERIFY(panel.m_saved_selector->isEnabled());
            QVERIFY(panel.m_check_receipt->isEnabled()); QVERIFY(panel.m_price->hasFocus());
            QCOMPARE(panel.m_price->text(), QStringLiteral("1.009"));
            QCOMPARE(panel.m_quantity->text(), QStringLiteral("0.25"));
            QCOMPARE(panel.m_price->cursorPosition(), cursor);
            QCOMPARE(panel.m_price->selectedText(), QStringLiteral("00"));
            QCOMPARE(panel.m_receipt_card->text(), receipt); QCOMPARE(panel.m_balances->text(), balance);
            QCOMPARE(panel.m_history_view->item(0, 1), history_cell);
            QCOMPARE(panel.m_own_view->item(0, 0), own_cell);
            QVERIFY(panel.m_order->isEnabled()); QCOMPARE(panel.orderReady(), !reconciling);
            QVERIFY(panel.m_deposit->isEnabled()); QCOMPARE(panel.depositReady(), !reconciling);
            QCOMPARE(panel.m_status->text().contains(QStringLiteral("Reconciling")), reconciling);
        }
        panel.m_snapshot->chain_reconciling = true;
        panel.m_snapshot->halt = QStringLiteral("invalid-checkpoint"); panel.updateMarketText();
        QVERIFY(!panel.m_order->isEnabled()); QVERIFY(panel.m_status->text().contains(QStringLiteral("halted")));
        panel.m_response_age.invalidate(); panel.updateMarketText();
        QVERIFY(!panel.m_deposit->isEnabled()); QVERIFY(panel.m_progress->text().contains(QStringLiteral("stale")));
        QVERIFY(panel.m_price->isEnabled()); QVERIFY(panel.m_price->hasFocus());
        QCOMPARE(enabled_changes.count, 0); QCOMPARE(reset.count(), 0); QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
        QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_deferred_review);
    }
    void passiveSavedControlsStayEnabled()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        SavedReadFixture(panel);
        const QString card{panel.m_receipt_card->text()};
        ReadInFlight(panel);
        for (int i{0}; i < 5; ++i) {
            panel.updateControls();
            QVERIFY(panel.m_saved_selector->isEnabled());
            QVERIFY(panel.m_check_receipt->isEnabled());
            QCOMPARE(panel.m_receipt_card->text(), card);
        }
    }

    void explicitStatusSkipsRemainingPassiveReads_data()
    {
        QTest::addColumn<bool>("hidden");
        QTest::newRow("visible") << false;
        QTest::newRow("hidden") << true;
    }
    void explicitStatusSkipsRemainingPassiveReads()
    {
        QFETCH(bool, hidden);
        FundingReadProbe probe; probe.held_method = "listflowmeshmarkets";
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        struct Cleanup { FundingReadProbe& probe; B3FlowMeshTradingPanel& panel; ~Cleanup() { probe.auxiliary_release.release(8); panel.cancelAndWait(); } } cleanup{probe, panel};
        if (!hidden) panel.show();
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        const auto age{panel.m_response_age.msecsSinceReference()};
        const auto phase{panel.m_refresh_phase};
        panel.startJob();
        QVERIFY(probe.auxiliary_entered.tryAcquire(1, 2000));
        // Drain the earlier empty-outbox callback before installing the saved
        // fixture; this test does not mutate or submit any signed instruction.
        QCoreApplication::processEvents();
        const auto saved{SavedReadFixture(panel)};
        const auto active{panel.m_active_result}; const auto worker{panel.m_thread};
        for (int i{0}; i < 10; ++i) panel.m_check_receipt->click();
        QVERIFY(panel.m_deferred_status); QCOMPARE(panel.m_thread, worker);
        probe.auxiliary_release.release();
        QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QCOMPARE(probe.receipts.load(), 1);
        QCOMPARE(probe.snapshots.load(), 0); // The queued explicit read goes next.
        QCOMPARE(panel.m_response_age.msecsSinceReference(), age);
        QCOMPARE(panel.m_refresh_phase, phase); // A yielded phase remains due.
        QVERIFY(!panel.m_read_failed); QVERIFY(!active->write_attempted);
        QVERIFY(active->refresh_yielded);
        bool foreground_status{false};
        for (const auto& [method, priority] : probe.priorities) {
            if (method == "getflowmeshactionstatus") {
                QVERIFY(priority == node::FlowMeshClientWorkPriority::FOREGROUND); foreground_status = true;
            } else QVERIFY(priority == node::FlowMeshClientWorkPriority::PASSIVE);
        }
        QVERIFY(foreground_status);
        QCOMPARE(probe.lingering_calls.load(), 0);
        QCOMPARE(panel.m_saved_actions.actions[0].signed_bytes_sha256, saved.actions[0].signed_bytes_sha256);
        QCOMPARE(panel.m_saved_actions.actions[0].sequence, saved.actions[0].sequence);
        QVERIFY(panel.m_saved_actions.actions[0].receipt.no_resubmit);
        QCOMPARE(probe.writes.load(), 0); QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
    }

    void repeatedStatusPriorityStillPermitsPassiveProgress()
    {
        using Phase = B3FlowMeshTradingPanel::RefreshPhase;
        FundingReadProbe probe; probe.held_method = "listflowmeshmarkets";
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        struct Cleanup { FundingReadProbe& probe; B3FlowMeshTradingPanel& panel; ~Cleanup() { probe.auxiliary_release.release(16); panel.cancelAndWait(); } } cleanup{probe, panel};
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        const auto age{panel.m_response_age.msecsSinceReference()};
        for (unsigned i{0}; i < B3FlowMeshTradingPanel::MAX_REFRESH_YIELDS; ++i) {
            panel.startJob(); QVERIFY(probe.auxiliary_entered.tryAcquire(1, 2000)); QCoreApplication::processEvents();
            SavedReadFixture(panel); panel.m_check_receipt->click(); QVERIFY(panel.m_yield_refresh->load());
            probe.auxiliary_release.release(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
            QCOMPARE(panel.m_refresh_yields, i + 1); QCOMPARE(panel.m_refresh_phase, Phase::Catalog);
            QCOMPARE(probe.snapshots.load(), 0); QCOMPARE(probe.receipts.load(), static_cast<int>(i + 1));
            QCOMPARE(panel.m_response_age.msecsSinceReference(), age);
        }
        // With its yield allowance exhausted, the current passive phase must
        // complete even if more explicit status clicks are queued/coalesced.
        panel.startJob(); QVERIFY(probe.auxiliary_entered.tryAcquire(1, 2000)); QCoreApplication::processEvents();
        SavedReadFixture(panel);
        for (int i{0}; i < 10; ++i) panel.m_check_receipt->click();
        QVERIFY(panel.m_deferred_status); QVERIFY(!panel.m_yield_refresh->load());
        probe.auxiliary_release.release(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QCOMPARE(probe.snapshots.load(), 1); QCOMPARE(panel.m_refresh_phase, Phase::Status);
        QCOMPARE(panel.m_refresh_yields, 0U);
        QCOMPARE(probe.receipts.load(), static_cast<int>(B3FlowMeshTradingPanel::MAX_REFRESH_YIELDS + 1));
        // One successful phase restores the ordinary foreground preference.
        probe.held_method = "getflowmeshbalance";
        panel.startJob(); QVERIFY(probe.auxiliary_entered.tryAcquire(1, 2000)); QCoreApplication::processEvents();
        SavedReadFixture(panel); panel.m_check_receipt->click(); QVERIFY(panel.m_yield_refresh->load());
        probe.auxiliary_release.release(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QCOMPARE(probe.snapshots.load(), 1); QCOMPARE(panel.m_refresh_phase, Phase::Status); QCOMPARE(panel.m_refresh_yields, 1U);
        QCOMPARE(probe.writes.load(), 0); QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
    }

    void effectsFallbackYieldPreservesOriginalRpcFailure()
    {
        FundingReadProbe probe; probe.held_method = "listflowmeshvaultoperations"; probe.effects_misc_error = true;
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        struct Cleanup { FundingReadProbe& probe; B3FlowMeshTradingPanel& panel; ~Cleanup() { probe.auxiliary_release.release(8); panel.cancelAndWait(); } } cleanup{probe, panel};
        panel.m_refresh_phase = B3FlowMeshTradingPanel::RefreshPhase::Effects; panel.m_catalog_age.start();
        panel.m_price->setText(QStringLiteral("0.5")); panel.m_quantity->setText(QStringLiteral("1.25"));
        const auto age{panel.m_response_age.msecsSinceReference()};
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        bool reviewed{false}; QTimer review_guard;
        connect(&review_guard, &QTimer::timeout, &panel, [&] {
            if (panel.m_confirmation) { reviewed = true; panel.m_confirmation->done(QMessageBox::Cancel); }
        });
        review_guard.start(1);
        panel.startJob(); QVERIFY(probe.auxiliary_entered.tryAcquire(1, 2000)); QCoreApplication::processEvents();
        const auto passive{panel.m_active_result}; panel.m_order->click(); QVERIFY(panel.m_yield_refresh->load());
        probe.auxiliary_release.release(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000); review_guard.stop();
        // RPC_MISC_ERROR triggers ReadEffectsForRefresh's snapshot fallback.
        // Yield is cached by snapshot_rpc, but cannot mask that original error.
        QVERIFY(!passive->refresh_yielded); QVERIFY(!passive->write_attempted); QVERIFY(panel.m_read_failed);
        QVERIFY(panel.m_read_error.contains(QStringLiteral("Synthetic reconciliation race")));
        QCOMPARE(probe.effects.load(), 1); QCOMPARE(probe.snapshots.load(), 0);
        QCOMPARE(panel.m_response_age.msecsSinceReference(), age);
        QVERIFY(!reviewed); QVERIFY(!panel.m_deferred_review); QVERIFY(!panel.m_confirmation); QVERIFY(!panel.m_unlock);
        QCOMPARE(probe.writes.load(), 0); QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
    }

    void automaticReceiptFailureSurvivesYieldWithExactAttribution_data()
    {
        QTest::addColumn<bool>("change_selection");
        QTest::newRow("same-request-preserves-error") << false;
        QTest::newRow("other-request-does-not-inherit-error") << true;
    }
    void automaticReceiptFailureSurvivesYieldWithExactAttribution()
    {
        QFETCH(bool, change_selection);
        FundingReadProbe probe; probe.held_method = "getflowmeshactionstatus";
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        struct Cleanup { FundingReadProbe& probe; B3FlowMeshTradingPanel& panel; ~Cleanup() { probe.auxiliary_release.release(8); panel.cancelAndWait(); } } cleanup{probe, panel};
        auto saved{SavedReadFixture(panel)};
        panel.m_refresh_phase = B3FlowMeshTradingPanel::RefreshPhase::Receipt;
        const auto age{panel.m_response_age.msecsSinceReference()};
        panel.startJob(); QVERIFY(probe.auxiliary_entered.tryAcquire(1, 2000)); QCoreApplication::processEvents();
        const auto passive{panel.m_active_result};
        auto second{saved.actions[0]}; second.sequence = 8; second.receipt.action_id = QString::fromStdString(H(96).GetHex());
        saved.actions.push_back(second); panel.restoreSavedActions(saved);
        if (change_selection) panel.m_saved_selector->setCurrentIndex(1);
        const auto selected{panel.m_receipt->action_id}; panel.m_check_receipt->click(); QVERIFY(panel.m_yield_refresh->load());
        probe.auxiliary_release.release();
        // The next held RPC is the explicit request. At this barrier the old
        // worker is drained, so its attributed failure can be inspected safely.
        QTRY_VERIFY_WITH_TIMEOUT(probe.auxiliary_entered.available() > 0, 2000);
        QVERIFY(probe.auxiliary_entered.tryAcquire());
        QVERIFY(passive->refresh_yielded); QVERIFY(passive->receipt_error.contains(QStringLiteral("Synthetic saved status unavailable")));
        QCOMPARE(passive->receipt_action_id, saved.actions[0].receipt.action_id);
        QCOMPARE(panel.m_receipt->action_id, selected);
        if (change_selection) QVERIFY(panel.m_receipt_error.isEmpty());
        else QVERIFY(panel.m_receipt_error.contains(QStringLiteral("Synthetic saved status unavailable")));
        QCOMPARE(panel.m_response_age.msecsSinceReference(), age); QCOMPARE(probe.snapshots.load(), 0);
        probe.auxiliary_release.release(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QCOMPARE(probe.receipts.load(), 2); QCOMPARE(probe.writes.load(), 0); QVERIFY(!passive->write_attempted);
        QCOMPARE(panel.m_saved_actions.actions[0].signed_bytes_sha256, saved.actions[0].signed_bytes_sha256);
        QVERIFY(panel.m_saved_actions.actions[0].receipt.no_resubmit); QVERIFY(m_wallet->IsLocked());
    }

    void orderReviewYieldsAuxiliaryThenReadsForegroundSnapshot_data()
    {
        QTest::addColumn<int>("phase_index"); QTest::addColumn<bool>("auxiliary_failed");
        QTest::newRow("catalog") << 0 << false;
        QTest::newRow("account-status") << 1 << false;
        QTest::newRow("vault-effects") << 2 << false;
        QTest::newRow("catalog-error-is-not-yield") << 0 << true;
        QTest::newRow("account-error-is-not-yield") << 1 << true;
        QTest::newRow("effects-error-is-not-yield") << 2 << true;
    }
    void orderReviewYieldsAuxiliaryThenReadsForegroundSnapshot()
    {
        QFETCH(int, phase_index); QFETCH(bool, auxiliary_failed);
        using Phase = B3FlowMeshTradingPanel::RefreshPhase;
        const std::array phases{Phase::Catalog, Phase::Status, Phase::Effects};
        const std::array<const char*, 3> methods{"listflowmeshmarkets", "getflowmeshbalance", "listflowmeshvaultoperations"};
        FundingReadProbe probe; probe.held_method = methods[phase_index]; probe.hold = true;
        if (auxiliary_failed) probe.failed_method = probe.held_method;
        auto account{probe.data["account"]}; account.pushKV("next_sequence", 9); probe.data.pushKV("account", account);
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        struct Cleanup { FundingReadProbe& probe; B3FlowMeshTradingPanel& panel; ~Cleanup() { probe.auxiliary_release.release(8); probe.release.release(8); panel.cancelAndWait(); } } cleanup{probe, panel};
        panel.m_refresh_phase = phases[phase_index]; panel.m_catalog_age.start();
        panel.m_price->setText(QStringLiteral("0.5")); panel.m_quantity->setText(QStringLiteral("1.25"));
        const auto observed{panel.m_response_age.msecsSinceReference()};
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        QString review; QTimer review_guard;
        connect(&review_guard, &QTimer::timeout, &panel, [&] {
            if (panel.m_confirmation) { review = panel.m_confirmation->text(); panel.m_confirmation->done(QMessageBox::Cancel); }
        });
        review_guard.start(1);
        panel.startJob(); QVERIFY(probe.auxiliary_entered.tryAcquire(1, 2000));
        QCoreApplication::processEvents(); // Apply only the completed local saved-card callback.
        const auto passive{panel.m_active_result}; const auto worker{panel.m_thread};
        QVERIFY(panel.m_order->isEnabled()); panel.m_order->click();
        QVERIFY(panel.m_deferred_review); QCOMPARE(panel.m_thread, worker);
        QVERIFY(panel.m_yield_refresh->load()); QVERIFY(review.isEmpty()); QCOMPARE(probe.snapshots.load(), 0);
        probe.auxiliary_release.release();
        if (auxiliary_failed) {
            QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000); review_guard.stop();
            QVERIFY(!passive->refresh_yielded); QVERIFY(!passive->write_attempted);
            QVERIFY(panel.m_read_failed); QVERIFY(panel.m_read_error.contains(QStringLiteral("Synthetic auxiliary failed")));
            QVERIFY(review.isEmpty()); QVERIFY(!panel.m_deferred_review); QVERIFY(!panel.m_confirmation);
            QCOMPARE(panel.m_response_age.msecsSinceReference(), observed);
            QCOMPARE(probe.snapshots.load(), 0); QCOMPARE(probe.writes.load(), 0); QCOMPARE(unlock.count(), 0);
            for (const auto& [method, priority] : probe.priorities) QVERIFY(priority == node::FlowMeshClientWorkPriority::PASSIVE);
            QVERIFY(!panel.m_unlock); QVERIFY(m_wallet->IsLocked());
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(probe.entered.available() > 0, 2000);
        // The original worker was drained before the foreground snapshot began.
        QVERIFY(passive->refresh_yielded); QVERIFY(!passive->write_attempted);
        QVERIFY(panel.m_active_result != passive); QVERIFY(review.isEmpty());
        QCOMPARE(probe.snapshots.load(), 1); QCOMPARE(panel.m_response_age.msecsSinceReference(), observed);
        QCOMPARE(unlock.count(), 0); QCOMPARE(probe.writes.load(), 0);
        probe.release.release(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000); review_guard.stop();
        QVERIFY2(review.contains(QStringLiteral("Account sequence: 9")), qPrintable(panel.m_log->toPlainText()));
        QVERIFY(review.contains(QStringLiteral("1.25 tUSD"))); QVERIFY(review.contains(QStringLiteral("at 0.5 B3 / tUSD")));
        QCOMPARE(panel.m_refresh_phase, phases[phase_index]); // Preflight does not consume auxiliary maintenance.
        QCOMPARE(panel.market()->sequence, uint64_t{9}); QVERIFY(!panel.m_read_failed);
        QCOMPARE(probe.snapshots.load(), 1); QCOMPARE(probe.writes.load(), 0); QCOMPARE(unlock.count(), 0);
        bool saw_passive{false}, saw_foreground{false};
        for (const auto& [method, priority] : probe.priorities) {
            if (method == methods[phase_index]) { QVERIFY(priority == node::FlowMeshClientWorkPriority::PASSIVE); saw_passive = true; }
            if (method == "getflowmeshmarketdata") { QVERIFY(priority == node::FlowMeshClientWorkPriority::FOREGROUND); saw_foreground = true; }
        }
        QVERIFY(saw_passive); QVERIFY(saw_foreground); QCOMPARE(probe.lingering_calls.load(), 0);
        QVERIFY(!panel.m_deferred_review); QVERIFY(!panel.m_confirmation); QVERIFY(!panel.m_unlock); QVERIFY(m_wallet->IsLocked());
    }

    void yieldedStatusInvalidatesChangedSelectedRequest()
    {
        FundingReadProbe probe; probe.held_method = "listflowmeshmarkets";
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        struct Cleanup { FundingReadProbe& probe; B3FlowMeshTradingPanel& panel; ~Cleanup() { probe.auxiliary_release.release(8); panel.cancelAndWait(); } } cleanup{probe, panel};
        const auto observed{panel.m_response_age.msecsSinceReference()};
        panel.startJob(); QVERIFY(probe.auxiliary_entered.tryAcquire(1, 2000)); QCoreApplication::processEvents();
        auto saved{SavedReadFixture(panel)};
        auto second{saved.actions[0]}; second.sequence = 8; second.receipt.action_id = QString::fromStdString(H(96).GetHex());
        saved.actions.push_back(second); panel.restoreSavedActions(saved);
        panel.m_check_receipt->click(); QVERIFY(panel.m_deferred_status); QVERIFY(panel.m_yield_refresh->load());
        panel.m_saved_selector->setCurrentIndex(1); const auto second_card{panel.m_receipt_card->text()};
        QCOMPARE(panel.m_receipt->action_id, second.receipt.action_id); QVERIFY(!panel.m_deferred_status);
        probe.auxiliary_release.release(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QCOMPARE(panel.m_receipt_card->text(), second_card); QCOMPARE(panel.m_receipt->action_id, second.receipt.action_id);
        QCOMPARE(panel.m_response_age.msecsSinceReference(), observed);
        QCOMPARE(probe.receipts.load(), 0); QCOMPARE(probe.snapshots.load(), 0); QCOMPARE(probe.writes.load(), 0);
        QCOMPARE(panel.m_saved_actions.actions.size(), size_t{2});
        for (size_t i{0}; i < saved.actions.size(); ++i) {
            QCOMPARE(panel.m_saved_actions.actions[i].signed_bytes_sha256, saved.actions[i].signed_bytes_sha256);
            QCOMPARE(panel.m_saved_actions.actions[i].sequence, saved.actions[i].sequence);
            QVERIFY(panel.m_saved_actions.actions[i].receipt.no_resubmit);
        }
    }

    void pendingPassiveYieldDrainsOnShutdownOrWalletSwitch_data()
    {
        QTest::addColumn<bool>("switch_wallet");
        QTest::newRow("hidden-shutdown") << false;
        QTest::newRow("wallet-switch") << true;
    }
    void pendingPassiveYieldDrainsOnShutdownOrWalletSwitch()
    {
        QFETCH(bool, switch_wallet);
        FundingReadProbe probe; probe.held_method = "listflowmeshmarkets";
        std::optional<OfflineWallet> replacement;
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        struct Cleanup { FundingReadProbe& probe; B3FlowMeshTradingPanel& panel; ~Cleanup() { probe.auxiliary_release.release(8); panel.cancelAndWait(); } } cleanup{probe, panel};
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        const std::weak_ptr<interfaces::Wallet> backend{panel.m_backend};
        panel.startJob(); QVERIFY(probe.auxiliary_entered.tryAcquire(1, 2000)); QCoreApplication::processEvents();
        const auto saved{SavedReadFixture(panel)}; const auto passive{panel.m_active_result};
        panel.m_check_receipt->click(); QVERIFY(panel.m_deferred_status); QVERIFY(panel.m_yield_refresh->load());
        probe.auxiliary_release.release(); panel.hide(); panel.cancelAndWait();
        QVERIFY(backend.expired()); QVERIFY(!panel.m_backend); QVERIFY(!panel.m_wallet);
        QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_deferred_status); QVERIFY(!passive->write_attempted);
        QCOMPARE(probe.receipts.load(), 0); QCOMPARE(probe.snapshots.load(), 0); QCOMPARE(probe.writes.load(), 0);
        QCOMPARE(panel.m_saved_actions.actions[0].signed_bytes_sha256, saved.actions[0].signed_bytes_sha256);
        QCOMPARE(panel.m_saved_actions.actions[0].sequence, saved.actions[0].sequence);
        QVERIFY(panel.m_saved_actions.actions[0].receipt.no_resubmit);
        if (switch_wallet) {
            // Test-only attachment to another generated wallet: no real RPC,
            // signing or user wallet. Use the real detach path to clear only
            // the old wallet's displayed cards before the offline attachment;
            // cancelAndWait alone intentionally retains those public records.
            // Late completion cannot cross generation.
            panel.setWalletModel(nullptr);
            replacement = MakeOfflineWallet("priority-replacement-test");
            AttachOfflineWallet(panel, *replacement->model, replacement->wallet);
            auto next{Parse(RemoteData())}; next.account = QString::fromStdString(H(110).GetHex()); Observe(panel, next);
            const auto replacement_saved{SavedReadFixture(panel)}; const auto card{panel.m_receipt_card->text()};
            QCoreApplication::processEvents();
            QCOMPARE(panel.m_saved_actions.account, replacement_saved.account); QCOMPARE(panel.m_receipt_card->text(), card);
            QVERIFY(!panel.m_thread); QVERIFY(!panel.m_deferred_status); QCOMPARE(probe.receipts.load(), 0);
            panel.cancelAndWait(); // Release the generated model before it leaves scope.
        } else QCoreApplication::processEvents();
        QCOMPARE(probe.writes.load(), 0); QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
    }

    void explicitStatusClickIsNotLostDuringRead()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        const auto saved{SavedReadFixture(panel)};
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        ReadInFlight(panel); const auto original_thread{panel.m_thread};
        for (int i{0}; i < 5; ++i) panel.m_check_receipt->click();
        QCOMPARE(panel.m_thread, original_thread);
        QVERIFY(!panel.m_active_result);
        const auto failed{std::make_shared<B3FlowMeshTradingPanel::Result>()};
        failed->error = QStringLiteral("Synthetic passive endpoint failure");
        panel.finishJob(failed);
        // A failed passive read still drains the one explicit read. The
        // isolated RPC may fail, but it must not silently lose the click.
        QVERIFY(panel.m_active_result);
        QVERIFY(panel.m_active_result->receipt_only);
        QCOMPARE(panel.m_active_result->receipt_action_id, saved.actions[0].receipt.action_id);
        QVERIFY(!panel.m_active_result->action); QVERIFY(!panel.m_active_result->exact_retry);
        panel.cancelAndWait();
        QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
        QCOMPARE(panel.m_saved_actions.actions[0].signed_bytes_sha256, saved.actions[0].signed_bytes_sha256);
        QCOMPARE(panel.m_saved_actions.actions[0].sequence, saved.actions[0].sequence);
        QVERIFY(panel.m_saved_actions.actions[0].receipt.no_resubmit);
    }

    void deferredStatusReadCoalescesWhileVisibleOrHidden_data()
    {
        QTest::addColumn<bool>("hidden");
        QTest::newRow("visible") << false;
        QTest::newRow("hidden") << true;
    }
    void deferredStatusReadCoalescesWhileVisibleOrHidden()
    {
        QFETCH(bool, hidden);
        StatusReadProbe probe;
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        struct Cleanup { StatusReadProbe& probe; B3FlowMeshTradingPanel& panel; ~Cleanup() { probe.release.release(8); panel.cancelAndWait(); } } cleanup{probe, panel};
        const auto saved{SavedReadFixture(panel)};
        const auto card{panel.m_receipt_card->text()};
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        if (!hidden) panel.show();
        ReadInFlight(panel); const auto thread{panel.m_thread};
        for (int i{0}; i < 20; ++i) panel.m_check_receipt->click();
        QCOMPARE(probe.count.load(), 0); QCOMPARE(panel.m_thread, thread);
        QVERIFY(panel.m_deferred_status); QVERIFY(panel.m_status_read_state->text().contains(QStringLiteral("queued")));
        QCOMPARE(panel.m_receipt_card->text(), card);
        auto failed{std::make_shared<B3FlowMeshTradingPanel::Result>()}; failed->error = QStringLiteral("Passive endpoint unavailable");
        panel.finishJob(failed);
        QVERIFY(probe.entered.tryAcquire(1, 1000)); QVERIFY(panel.m_active_result);
        const auto active{panel.m_active_result};
        QVERIFY(active->receipt_only); QVERIFY(!active->write_attempted); QVERIFY(!active->action); QVERIFY(!active->exact_retry);
        QVERIFY(!panel.m_deferred_status); QVERIFY(panel.m_status_read_state->text().contains(QStringLiteral("Checking")));
        const auto active_thread{panel.m_thread};
        for (int i{0}; i < 20; ++i) panel.m_check_receipt->click();
        QCOMPARE(panel.m_thread, active_thread); QVERIFY(!panel.m_deferred_status);
        QCOMPARE(probe.count.load(), 1);
        {
            std::lock_guard lock{probe.mutex}; QCOMPARE(probe.requests.size(), size_t{1});
            QCOMPARE(probe.requests[0].first, B3AssetTransfer::WalletUri(m_model->getWalletName()));
            QCOMPARE(probe.requests[0].second[0].get_str(), saved.actions[0].market.toStdString());
            QCOMPARE(probe.requests[0].second[1].get_str(), saved.actions[0].receipt.action_id.toStdString());
        }
        probe.release.release(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QCOMPARE(probe.count.load(), 1); QVERIFY(!active->write_attempted);
        QCOMPARE(panel.m_receipt->account, saved.account); QVERIFY(panel.m_receipt->no_resubmit);
        QCOMPARE(panel.m_saved_actions.actions[0].sequence, saved.actions[0].sequence);
        QCOMPARE(panel.m_saved_actions.actions[0].signed_bytes_sha256, saved.actions[0].signed_bytes_sha256);
        QCOMPARE(panel.m_saved_actions.actions[0].signed_bytes_size, saved.actions[0].signed_bytes_size);
        QCOMPARE(panel.m_saved_actions.actions[0].initial_submission_ms, saved.actions[0].initial_submission_ms);
        QVERIFY(panel.m_saved_actions.actions[0].may_have_been_sent);
        QVERIFY(panel.m_check_receipt->isEnabled()); QVERIFY(panel.m_saved_selector->isEnabled());
        QVERIFY(!panel.m_deferred_review); QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
    }

    void deferredStatusReadInvalidatesChangedScope_data()
    {
        QTest::addColumn<QString>("change");
        for (const auto* change : {"selection-away-and-back", "market", "wallet", "generation", "account", "domain", "config"})
            QTest::newRow(change) << QString::fromLatin1(change);
    }
    void deferredStatusReadInvalidatesChangedScope()
    {
        QFETCH(QString, change);
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        auto saved{SavedReadFixture(panel)};
        auto second{saved.actions[0]}; second.sequence = 8; second.receipt.action_id = QString::fromStdString(H(96).GetHex());
        saved.actions.push_back(second); panel.restoreSavedActions(saved);
        ReadInFlight(panel); panel.m_check_receipt->click(); QVERIFY(panel.m_deferred_status);
        std::optional<OfflineWallet> replacement;
        if (change == QStringLiteral("selection-away-and-back")) {
            panel.m_saved_selector->setCurrentIndex(1); panel.m_saved_selector->setCurrentIndex(0);
        } else if (change == QStringLiteral("market")) {
            auto market{panel.m_market_data.front()}; market.id = QString::fromStdString(H(101).GetHex());
            panel.m_market_data.push_back(market); panel.m_market->addItem(QStringLiteral("other market"), market.id); panel.m_market->setCurrentIndex(1);
        } else if (change == QStringLiteral("wallet")) {
            panel.cancelAndWait(); replacement = MakeOfflineWallet(m_wallet->GetName());
            AttachOfflineWallet(panel, *replacement->model, replacement->wallet); SavedReadFixture(panel);
        } else if (change == QStringLiteral("generation")) ++panel.m_generation;
        else if (change == QStringLiteral("account")) panel.m_saved_actions.account = QString::fromStdString(H(101).GetHex());
        else if (change == QStringLiteral("domain")) panel.m_saved_actions.actions[0].domain = QString::fromStdString(H(101).GetHex());
        else if (change == QStringLiteral("config")) panel.m_market_data[0].config = QString::fromStdString(H(101).GetHex());
        auto failed{std::make_shared<B3FlowMeshTradingPanel::Result>()}; failed->error = QStringLiteral("Passive read complete");
        panel.finishJob(failed);
        QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_deferred_status);
        panel.cancelAndWait();
    }

    void statusReadLateResultStaysWithOriginalRequest_data()
    {
        QTest::addColumn<bool>("failure"); QTest::addColumn<bool>("wrong_account");
        QTest::newRow("success-with-optional-account-omitted") << false << false;
        QTest::newRow("endpoint-failure") << true << false;
        QTest::newRow("wrong-account-rejected") << false << true;
    }
    void statusReadLateResultStaysWithOriginalRequest()
    {
        QFETCH(bool, failure); QFETCH(bool, wrong_account);
        StatusReadProbe probe; probe.fail = failure; probe.wrong_account = wrong_account;
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        struct Cleanup { StatusReadProbe& probe; B3FlowMeshTradingPanel& panel; ~Cleanup() { probe.release.release(8); panel.cancelAndWait(); } } cleanup{probe, panel};
        auto saved{SavedReadFixture(panel)};
        // Same semantic ActionId in another market must not alias the first.
        auto second{saved.actions[0]}; second.market = QString::fromStdString(H(101).GetHex()); second.receipt.market = second.market;
        saved.actions.push_back(second); panel.restoreSavedActions(saved);
        panel.m_check_receipt->click(); QVERIFY(probe.entered.tryAcquire(1, 1000));
        auto first_result{panel.m_active_result}; QVERIFY(first_result);
        panel.m_saved_selector->setCurrentIndex(1); const auto other_card{panel.m_receipt_card->text()};
        panel.m_check_receipt->click(); QVERIFY(panel.m_deferred_status);
        probe.release.release();
        QTRY_COMPARE_WITH_TIMEOUT(probe.count.load(), 2, 2000);
        QVERIFY(probe.entered.tryAcquire(1, 1000));
        QCOMPARE(panel.m_receipt_card->text(), other_card); // A's late error/success cannot relabel B.
        QCOMPARE(panel.m_receipt->market, second.market); QVERIFY(panel.m_receipt_error.isEmpty());
        const auto second_result{panel.m_active_result}; QVERIFY(second_result); QVERIFY(second_result != first_result);
        QCOMPARE(second_result->receipt_market, second.market);
        if (!failure && !wrong_account) {
            QVERIFY(first_result->receipt); QCOMPARE(first_result->receipt->account, saved.account);
        } else QVERIFY(!first_result->receipt);
        probe.release.release(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QCOMPARE(probe.count.load(), 2); QVERIFY(!first_result->write_attempted); QVERIFY(!second_result->write_attempted);
        QCOMPARE(panel.m_receipt->market, second.market); QVERIFY(panel.m_receipt->no_resubmit);
        QCOMPARE(panel.m_saved_actions.actions[0].signed_bytes_sha256, saved.actions[0].signed_bytes_sha256);
        if (failure || wrong_account) QVERIFY(!panel.m_receipt_error.isEmpty());
    }

    void signedActionWaitsOnceForItsReceiptAndFailureStaysCertain_data()
    {
        QTest::addColumn<QString>("mode");
        for (const auto* mode : {"certified", "failure", "rejected", "local", "verified_in_submission"}) QTest::newRow(mode) << QString::fromLatin1(mode);
    }
    void signedActionWaitsOnceForItsReceiptAndFailureStaysCertain()
    {
        QFETCH(QString, mode);
        WaitedReceiptProbe probe; probe.fail = mode == QStringLiteral("failure");
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        struct Cleanup { WaitedReceiptProbe& probe; B3FlowMeshTradingPanel& panel; ~Cleanup() { probe.release.release(8); panel.cancelAndWait(); } } cleanup{probe, panel};
        const bool remote{mode != QStringLiteral("local")};
        if (remote) Observe(panel, Parse(RemoteData()));
        QCOMPARE(panel.m_market_data.front().remote, remote);
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        // The exact private result of a signed order whose submission returned.
        auto result{std::make_shared<B3FlowMeshTradingPanel::Result>()};
        B3FlowMeshTrading::Action action;
        action.operation = B3FlowMeshTrading::Operation::Order; action.market = panel.m_market_data.front();
        action.side = QStringLiteral("bid"); action.price = 1000; action.amount = 1;
        result->action = action; result->wallet = panel.m_wallet_name; result->write_attempted = true;
        B3FlowMeshTrading::Receipt receipt; receipt.action_id = QString::fromStdString(H(98).GetHex()); receipt.market = action.market.id;
        receipt.state = mode == QStringLiteral("rejected") ? QStringLiteral("rejected") : QStringLiteral("queued");
        if (mode == QStringLiteral("verified_in_submission")) {
            // A waited submission (an already unlocked wallet) returned the
            // inclusion, verified by the client in the same call.
            action.submit_wait_ms = B3FlowMeshTradingPanel::POST_SUBMIT_RECEIPT_WAIT_MS; result->action = action;
            receipt.state = QStringLiteral("certified_inclusion"); receipt.certificate_verified = true;
            receipt.microblock_hash = QString::fromStdString(H(97).GetHex()); receipt.microblock_sequence = 5;
        }
        result->receipt = receipt;
        panel.m_busy = true;
        panel.finishJob(result);
        if (mode == QStringLiteral("verified_in_submission")) QVERIFY(panel.m_receipt && panel.m_receipt->Included());
        if (mode == QStringLiteral("rejected") || mode == QStringLiteral("local") || mode == QStringLiteral("verified_in_submission")) {
            // Terminal, verified already, or no transport to wait on: only
            // the ordinary refresh, and no second request for the receipt.
            QVERIFY(!panel.m_active_result || !panel.m_active_result->receipt_wait);
            QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
            QCOMPARE(probe.count.load(), 0);
            return;
        }
        QVERIFY(probe.entered.tryAcquire(1, 1000));
        const auto waited{panel.m_active_result}; QVERIFY(waited);
        QVERIFY(waited->receipt_wait); QVERIFY(!waited->passive); QVERIFY(!waited->write_attempted);
        QVERIFY(!waited->action); QVERIFY(!waited->exact_retry); QVERIFY(!waited->receipt_only);
        QVERIFY(!panel.m_busy); QVERIFY(!panel.m_uncertain);
        {
            std::lock_guard lock{probe.mutex}; QCOMPARE(probe.params.size(), size_t{1});
            QCOMPARE(probe.params[0].size(), size_t{3});
            QCOMPARE(probe.params[0][0].get_str(), action.market.id.toStdString());
            QCOMPARE(probe.params[0][1].get_str(), receipt.action_id.toStdString());
            QCOMPARE(probe.params[0][2].getInt<int>(), 2000);
        }
        probe.release.release();
        // The ordinary refresh follows; it has no saved-action source here.
        QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 3000);
        QCOMPARE(probe.count.load(), 1); QVERIFY(!waited->write_attempted);
        QVERIFY(panel.m_receipt); QCOMPARE(panel.m_receipt->action_id, receipt.action_id);
        QCOMPARE(panel.m_receipt->account, action.market.account);
        if (mode == QStringLiteral("certified")) {
            QVERIFY(panel.m_receipt->Included()); QCOMPARE(panel.m_receipt->microblock_sequence, uint64_t{5});
            QVERIFY(panel.m_receipt_error.isEmpty()); QVERIFY(!panel.m_pending_sequence);
        } else {
            // A failed read is not a failed or uncertain submission.
            QCOMPARE(panel.m_receipt->state, QStringLiteral("queued")); QVERIFY(!panel.m_receipt->Included());
            QVERIFY(panel.m_receipt_error.contains(QStringLiteral("unavailable")));
            QVERIFY(panel.m_pending_sequence); QCOMPARE(*panel.m_pending_sequence, action.market.sequence);
        }
        QVERIFY(!panel.m_uncertain); QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
    }

    void shutdownDropsPendingStatusWithoutOwningWalletOrChangingInstruction()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        const auto saved{SavedReadFixture(panel)};
        const std::weak_ptr<interfaces::Wallet> backend{panel.m_backend};
        ReadInFlight(panel); panel.m_check_receipt->click(); QVERIFY(panel.m_deferred_status);
        const auto scope{*panel.m_deferred_status};
        panel.hide(); panel.cancelAndWait();
        QVERIFY(backend.expired()); QVERIFY(!panel.m_deferred_status); QVERIFY(!panel.m_thread);
        QVERIFY(!panel.m_wallet); QVERIFY(!panel.m_backend); QVERIFY(!panel.m_active_result);
        QVERIFY(!panel.statusReadValid(scope, true));
        QCOMPARE(panel.m_saved_actions.actions[0].receipt.action_id, saved.actions[0].receipt.action_id);
        QCOMPARE(panel.m_saved_actions.actions[0].signed_bytes_sha256, saved.actions[0].signed_bytes_sha256);
        QCOMPARE(panel.m_saved_actions.actions[0].sequence, saved.actions[0].sequence);
        QVERIFY(panel.m_saved_actions.actions[0].receipt.no_resubmit);
        QVERIFY(!panel.m_check_receipt->isEnabled()); QVERIFY(m_wallet->IsLocked());
    }

    void pendingReadCannotDrainThroughPanelDestroyedInExistingReview()
    {
        auto* panel{new B3FlowMeshTradingPanel}; AttachOfflineWallet(*panel);
        SavedReadFixture(*panel); ReadInFlight(*panel); panel->m_check_receipt->click();
        QVERIFY(panel->m_deferred_status);
        const QPointer<B3FlowMeshTradingPanel> alive{panel};
        const std::weak_ptr<interfaces::Wallet> backend{panel->m_backend};
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        // Synthetic completion enters the existing fee-review dialog. No RPC,
        // transaction construction or broadcast is performed by this fixture.
        auto result{std::make_shared<B3FlowMeshTradingPanel::Result>()};
        B3FlowMeshTrading::Action action; action.operation = B3FlowMeshTrading::Operation::Checkpoint;
        action.market = panel->m_market_data.front(); result->action = action;
        result->prepared.emplace(); result->prepared->txid = QString::fromStdString(H(112).GetHex());
        bool dialog_seen{false};
        QTimer::singleShot(0, [&] {
            dialog_seen = alive && alive->m_confirmation;
            delete alive.data();
        });
        panel->finishJob(result);
        QVERIFY(dialog_seen); QVERIFY(!alive); QVERIFY(backend.expired());
        QVERIFY(!result->write_attempted); QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
    }

    void orderReviewEntryStaysAvailableDuringTemporaryReadinessChanges()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        for (int condition{0}; condition < 5; ++condition) {
            Observe(panel, Parse(RemoteData()));
            panel.m_read_failed = false;
            if (condition < 2) {
                panel.m_market_data.front().ready = false; panel.m_snapshot->paused = true;
                panel.m_snapshot->chain_reconciling = condition == 1;
                panel.m_snapshot->error = QStringLiteral("FlowMesh service is not active at the current B3 tip");
            } else if (condition == 2) panel.m_response_age.invalidate();
            else if (condition == 3) panel.m_read_failed = true;
            else panel.m_snapshot->handoff = true;
            panel.updateControls();
            QVERIFY(panel.m_order->isEnabled()); // Entry is read-only; readiness is checked after its fresh read.
            QVERIFY(!panel.orderReady());
            QVERIFY(!panel.m_cancel_order->isEnabled()); // Cancel's submission policy is unchanged.
            QVERIFY(panel.m_price->isEnabled()); QVERIFY(panel.m_quantity->isEnabled());
            QVERIFY(panel.m_order->toolTip().contains(QStringLiteral("does not mean trading is ready")));
            bool reviewed{false};
            QTimer::singleShot(0, &panel, [&] {
                if (panel.m_confirmation) { reviewed = true; panel.m_confirmation->done(QMessageBox::Cancel); }
            });
            panel.begin(B3FlowMeshTrading::Operation::Order); // Internal calls cannot bypass the strict gate.
            QCoreApplication::processEvents(); QVERIFY(!reviewed);
        }
        QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_unlock);
        QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
    }
    void orderReviewReadsOnceBeforeApproval_data()
    {
        QTest::addColumn<bool>("inverse"); QTest::addColumn<bool>("buy"); QTest::addColumn<int>("outcome");
        QTest::newRow("canonical buy reuses passive read") << false << true << 0;
        QTest::newRow("canonical sell") << false << false << 0;
        QTest::newRow("inverse buy") << true << true << 0;
        QTest::newRow("inverse sell") << true << false << 0;
        QTest::newRow("still paused") << false << true << 1;
        QTest::newRow("read failed") << false << true << 2;
        QTest::newRow("fresh replacement balance too small") << false << true << 3;
        QTest::newRow("hard halt") << false << true << 4;
        QTest::newRow("snapshot older than 3000ms") << false << true << 5;
        QTest::newRow("unverified account proof") << false << true << 6;
        QTest::newRow("quorum unavailable") << false << true << 7;
    }
    void orderReviewReadsOnceBeforeApproval()
    {
        QFETCH(bool, inverse); QFETCH(bool, buy); QFETCH(int, outcome);
        FundingReadProbe probe; probe.hold = true; probe.fail = outcome == 2;
        auto account{probe.data["account"]}; account.pushKV("next_sequence", 9); // Refresh before review may advance sequence.
        if (outcome == 3) {
            account.pushKV("b3_available_atoms", 0); account.pushKV("b3_reserved_atoms", 0);
            account.pushKV("curves", UniValue{UniValue::VARR});
        }
        probe.data.pushKV("account", account);
        auto status{probe.data["snapshot"]};
        if (outcome == 1) { status.pushKV("paused", true); status.pushKV("chain_reconciling", true); }
        if (outcome == 4) status.pushKV("halt", "invalid-checkpoint");
        if (outcome == 7) { status.pushKV("active_seats", 3); status.pushKV("quorum_required", 3); }
        probe.data.pushKV("snapshot", status);
        if (outcome == 6) { auto proof{probe.data["verification"]}; proof.pushKV("account_state_verified", false); probe.data.pushKV("verification", proof); }
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()), inverse);
        panel.m_side->setCurrentIndex(buy ? 0 : 1); panel.m_price->setText(inverse ? QStringLiteral("2") : QStringLiteral("0.5"));
        panel.m_quantity->setText(QStringLiteral("1.25")); panel.m_snapshot->paused = true; panel.m_market_data.front().ready = false;
        panel.m_catalog_age.start(); panel.m_refresh_phase = B3FlowMeshTradingPanel::RefreshPhase::None; panel.updateControls();
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        QString review; bool cancel_default{false}; QTimer review_guard;
        connect(&review_guard, &QTimer::timeout, &panel, [&] {
            if (!panel.m_confirmation) return;
            review = panel.m_confirmation->text(); cancel_default = panel.m_confirmation->defaultButton() == panel.m_confirmation->button(QMessageBox::Cancel);
            panel.m_confirmation->done(QMessageBox::Cancel);
        });
        review_guard.start(1);
        const bool reuse{outcome == 0 && !inverse && buy};
        if (reuse) { panel.startJob(); QVERIFY(probe.entered.tryAcquire(1, 2000)); }
        QVERIFY(panel.m_order->isEnabled()); panel.m_order->click();
        QVERIFY(panel.m_thread); QVERIFY(panel.m_deferred_review && panel.m_deferred_review->order_context);
        QVERIFY(!panel.m_deferred_review->order_context->needs_market_read);
        QVERIFY(panel.m_active_result && !panel.m_active_result->action && !panel.m_active_result->write_attempted);
        QVERIFY(!panel.m_price->isEnabled()); QVERIFY(!panel.m_quantity->isEnabled()); QVERIFY(!panel.m_orientation->isEnabled());
        const auto result{panel.m_active_result}; panel.m_order->click(); panel.requestOrderReview(); // Coalesce, never replace the intent.
        QCOMPARE(panel.m_active_result, result); QCOMPARE(unlock.count(), 0); QCOMPARE(probe.writes.load(), 0);
        QVERIFY(review.isEmpty()); QVERIFY(!panel.m_unlock);
        if (!reuse) QVERIFY(probe.entered.tryAcquire(1, 2000));
        probe.release.release();
        if (outcome == 5) { QVERIFY(panel.m_thread->wait(2000)); QTest::qSleep(3050); }
        QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000); review_guard.stop();
        QCOMPARE(probe.snapshots.load(), 1); QCOMPARE(probe.catalogs.load(), 0);
        QCOMPARE(!review.isEmpty(), outcome == 0); QCOMPARE(cancel_default, outcome == 0);
        if (outcome == 0) {
            QVERIFY(review.contains(QStringLiteral("1.25 tUSD")));
            QVERIFY(review.contains(inverse ? CanonicalSide(buy, inverse) : buy ? QStringLiteral("Limit Buy") : QStringLiteral("Limit Sell")));
            QVERIFY(review.contains(inverse ? QStringLiteral("Executable limit: no %1 than 2").arg(buy ? QStringLiteral("more") : QStringLiteral("less")) : QStringLiteral("at 0.5 B3 / tUSD")));
            QVERIFY(review.contains(inverse ? QStringLiteral("account sequence: 9") : QStringLiteral("Account sequence: 9")));
            QVERIFY(review.contains(panel.market()->account)); QCOMPARE(panel.market()->sequence, uint64_t{9});
            if (inverse) QVERIFY(review.contains(buy ? QStringLiteral("Buy B3") : QStringLiteral("Sell B3")));
            QVERIFY(panel.orderReady());
        }
        if (outcome == 5) { QVERIFY(panel.m_response_age.elapsed() > 3000); QVERIFY(panel.m_order->isEnabled()); QVERIFY(!panel.orderReady()); }
        if (outcome == 3) QVERIFY(panel.m_log->toPlainText().contains(QStringLiteral("replacement exceeds")));
        QVERIFY(!panel.m_deferred_review); QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_confirmation); QVERIFY(!panel.m_unlock);
        QVERIFY(!result->write_attempted); QCOMPARE(probe.writes.load(), 0); QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
        panel.resumeReview(); QCoreApplication::processEvents(); QCOMPARE(probe.snapshots.load(), 1); // A consumed intent never retries.
        for (const auto& [method, priority] : probe.priorities)
            QVERIFY(priority == (reuse ? node::FlowMeshClientWorkPriority::PASSIVE : node::FlowMeshClientWorkPriority::FOREGROUND));
        QCOMPARE(probe.lingering_calls.load(), 0);
    }
    void orderReviewAfterReceiptOnlyNeedsOneMarketRead_data()
    {
        QTest::addColumn<int>("outcome");
        QTest::newRow("ready after failed receipt check") << 0;
        QTest::newRow("market read fails") << 1;
        QTest::newRow("wallet switches during chained read") << 2;
    }
    void orderReviewAfterReceiptOnlyNeedsOneMarketRead()
    {
        QFETCH(int, outcome);
        FundingReadProbe probe; probe.held_method = "getflowmeshactionstatus"; probe.hold = true; probe.fail = outcome == 1;
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        const auto saved{SavedReadFixture(panel)}; // A protected historical row, not a new pending instruction.
        // A passive refresh must return the same retained local record. An
        // empty/mismatched outbox correctly disables new reviews.
        const auto& action{saved.actions.front()};
        UniValue receipt{UniValue::VOBJ}; receipt.pushKV("market_id", action.market.toStdString());
        receipt.pushKV("account_id", action.account.toStdString()); receipt.pushKV("action_id", action.receipt.action_id.toStdString()); receipt.pushKV("accepted", false);
        UniValue row{UniValue::VOBJ}; row.pushKV("market_id", action.market.toStdString()); row.pushKV("domain", action.domain.toStdString());
        row.pushKV("execution_config_id", action.config.toStdString()); row.pushKV("account_id", action.account.toStdString());
        row.pushKV("action_id", action.receipt.action_id.toStdString()); row.pushKV("receipt", receipt); row.pushKV("previously_certified", true);
        row.pushKV("action_type", action.type); row.pushKV("sequence", *action.sequence); row.pushKV("signed_bytes_sha256", action.signed_bytes_sha256.toStdString());
        row.pushKV("signed_bytes_size", action.signed_bytes_size); row.pushKV("initial_submission_ms", action.initial_submission_ms); row.pushKV("may_have_been_sent", action.may_have_been_sent);
        UniValue rows{UniValue::VARR}; rows.push_back(row); probe.saved_response = UniValue{UniValue::VOBJ};
        probe.saved_response.pushKV("source", "local-retained-outbox"); probe.saved_response.pushKV("account_id", saved.account.toStdString()); probe.saved_response.pushKV("actions", rows);
        panel.m_refresh_phase = B3FlowMeshTradingPanel::RefreshPhase::None;
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        bool reviewed{false}; QTimer review_guard;
        connect(&review_guard, &QTimer::timeout, &panel, [&] {
            if (panel.m_confirmation) { reviewed = true; panel.m_confirmation->done(QMessageBox::Cancel); }
        });
        review_guard.start(1); panel.requestStatusRead(); QVERIFY(probe.auxiliary_entered.tryAcquire(1, 2000));
        QVERIFY(panel.m_active_result && panel.m_active_result->receipt_only);
        panel.m_order->click(); QVERIFY(panel.m_deferred_review && panel.m_deferred_review->order_context);
        QVERIFY(panel.m_deferred_review->order_context->needs_market_read); QVERIFY(!reviewed); QCOMPARE(probe.snapshots.load(), 0);
        probe.auxiliary_release.release();
        QTRY_VERIFY_WITH_TIMEOUT(probe.entered.available() > 0, 2000);
        QVERIFY(panel.m_active_result && !panel.m_active_result->receipt_only && !panel.m_active_result->action);
        QVERIFY(panel.m_deferred_review && !panel.m_deferred_review->order_context->needs_market_read);
        QVERIFY(!reviewed); QCOMPARE(probe.snapshots.load(), 1); QCOMPARE(unlock.count(), 0); QCOMPARE(probe.writes.load(), 0);
        probe.release.release();
        if (outcome == 2) panel.setWalletModel(nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000); review_guard.stop();
        QVERIFY2(reviewed == (outcome == 0), qPrintable(panel.m_log->toPlainText())); QCOMPARE(probe.receipts.load(), 1); QCOMPARE(probe.snapshots.load(), 1);
        if (outcome != 2) {
            QCOMPARE(panel.m_saved_actions.actions.size(), saved.actions.size());
            QCOMPARE(panel.m_saved_actions.actions.front().signed_bytes_sha256, saved.actions.front().signed_bytes_sha256);
            QVERIFY(panel.m_saved_actions.actions.front().receipt.no_resubmit);
        }
        QVERIFY(!panel.m_deferred_review); QVERIFY(!panel.m_confirmation); QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_unlock);
        QCOMPARE(probe.writes.load(), 0); QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
    }
    void backgroundReadKeepsReviewControlsStableAndQueuesOnlyReview()
    {
        B3FlowMeshTradingPanel panel;
        AttachOfflineWallet(panel);
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        ReadInFlight(panel);
        QThread* const read{panel.m_thread};
        for (int poll{0}; poll < 5; ++poll) {
            panel.updateControls();
            for (auto* button : {panel.m_order, panel.m_cancel_order, panel.m_deposit, panel.m_withdraw, panel.m_admit}) QVERIFY(button->isEnabled());
            QVERIFY(panel.m_price->isEnabled()); QVERIFY(panel.m_quantity->isEnabled());
            QVERIFY(panel.m_buy->isEnabled()); QVERIFY(panel.m_sell->isEnabled());
        }
        panel.m_order->click();
        QVERIFY(panel.m_deferred_review);
        QCOMPARE(panel.m_deferred_review->operation, B3FlowMeshTrading::Operation::Order);
        QVERIFY(!panel.m_deferred_review->funding);
        QCOMPARE(panel.m_deferred_review->generation, panel.m_generation);
        QCOMPARE(panel.m_deferred_review->market, panel.m_snapshot->market);
        QVERIFY(panel.m_busy); QVERIFY(!panel.m_price->isEnabled()); QVERIFY(!panel.m_quantity->isEnabled());
        QVERIFY(!panel.m_order->isEnabled()); QVERIFY(!panel.m_confirmation); QVERIFY(!panel.m_funding_dialog);
        QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_unlock); QCOMPARE(unlock.count(), 0);
        // A repeated click or direct action dispatch cannot replace the first
        // review intent or start a mutation while the read marker exists.
        panel.begin(B3FlowMeshTrading::Operation::Cancel);
        panel.openFunding(true);
        B3FlowMeshTrading::Action action;
        action.market = panel.m_market_data.front();
        panel.startJob(action);
        QCOMPARE(panel.m_deferred_review->operation, B3FlowMeshTrading::Operation::Order);
        QCOMPARE(panel.m_thread, read); QVERIFY(!panel.m_active_result);
        panel.cancelAndWait();
        QVERIFY(!panel.m_thread); QVERIFY(!panel.m_deferred_review);
        QVERIFY(!panel.m_confirmation); QVERIFY(!panel.m_unlock);
        QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
    }
    void fundingClickDuringReadQueuesOnlyDialogOpening()
    {
        B3FlowMeshTradingPanel panel;
        AttachOfflineWallet(panel);
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        ReadInFlight(panel);
        panel.m_deposit->click();
        QVERIFY(panel.m_deferred_review);
        QCOMPARE(panel.m_deferred_review->operation, B3FlowMeshTrading::Operation::Deposit);
        QVERIFY(panel.m_deferred_review->funding);
        QVERIFY(panel.m_busy); QVERIFY(!panel.m_funding_dialog); QVERIFY(!panel.m_confirmation);
        QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_unlock);
        panel.cancelAndWait();
        QVERIFY(!panel.m_deferred_review); QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
    }
    void depositDraftRemainsAvailableDuringReconciliation()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        Observe(panel, Parse(RemoteData())); panel.m_market_data.front().remote = true;
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        panel.m_market_data.front().ready = false;
        panel.m_snapshot->paused = true;
        panel.m_snapshot->error = QStringLiteral("FlowMesh service is not active at the current B3 tip");
        panel.m_amount->setText(QStringLiteral("1.25"));
        for (int condition{0}; condition < 3; ++condition) {
            panel.m_snapshot->chain_reconciling = condition == 1; // Older servers omit this flag.
            panel.m_read_failed = condition == 2;
            panel.updateMarketText();
            QVERIFY(panel.m_deposit->isEnabled());
            QVERIFY(panel.m_order->isEnabled()); QVERIFY(!panel.orderReady()); QVERIFY(panel.m_withdraw->isEnabled()); QVERIFY(!panel.withdrawalReady());
            bool opened{false}, retained{false}, explained{false};
            QTimer::singleShot(0, &panel, [&] {
                if (!panel.m_funding_dialog) return;
                opened = true;
                if (auto* amount{panel.m_funding_dialog->findChild<QLineEdit*>(QStringLiteral("flowMeshFundingDraftAmount"))}) retained = amount->text() == QStringLiteral("1.25");
                for (const auto* label : panel.m_funding_dialog->findChildren<QLabel*>()) explained |= label->text().contains(QStringLiteral("draft")) && label->text().contains(QStringLiteral("readiness"));
                panel.m_funding_dialog->reject();
            });
            panel.m_deposit->click();
            QVERIFY(opened); QVERIFY(retained); QVERIFY(explained);
            QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_deferred_review);
            QVERIFY(!panel.m_confirmation); QVERIFY(!panel.m_unlock); QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
        }
    }
    void withdrawalDraftRemainsAvailableDuringReconciliation()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        for (int condition{0}; condition < 4; ++condition) {
            panel.m_market_data.front().ready = condition >= 2;
            panel.m_snapshot->paused = condition < 2; panel.m_snapshot->chain_reconciling = condition == 1;
            panel.m_snapshot->error = condition < 2 ? QStringLiteral("FlowMesh service is not active at the current B3 tip") : QString{};
            panel.m_response_age.restart(); if (condition == 2) panel.m_response_age.invalidate();
            panel.m_read_failed = condition == 3; panel.updateMarketText();
            QVERIFY(panel.m_withdraw->isEnabled()); QVERIFY(panel.m_order->isEnabled()); QVERIFY(!panel.orderReady());
            bool opened{false};
            QTimer::singleShot(0, &panel, [&] {
                if (panel.m_funding_dialog) { opened = true; panel.m_funding_dialog->reject(); }
            });
            panel.m_withdraw->click();
            QVERIFY(opened); QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_deferred_review);
            QVERIFY(!panel.m_confirmation); QVERIFY(!panel.m_unlock); QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
        }
    }
    void withdrawalContinueUsesFreshBalanceAndFrozenInputs_data()
    {
        QTest::addColumn<int>("outcome");
        QTest::newRow("still paused") << 0;
        QTest::newRow("read failed") << 1;
        QTest::newRow("ready token") << 2;
        QTest::newRow("fresh balance too small") << 3;
        QTest::newRow("invalid destination") << 4;
        QTest::newRow("ready native B3") << 5;
        QTest::newRow("destination changed while waiting") << 6;
        QTest::newRow("amount changed while waiting") << 7;
    }
    void withdrawalContinueUsesFreshBalanceAndFrozenInputs()
    {
        QFETCH(int, outcome);
        FundingReadProbe probe; probe.fail = outcome == 1; probe.hold = outcome >= 6;
        if (outcome == 0) { auto status{probe.data["snapshot"]}; status.pushKV("paused", true); probe.data.pushKV("snapshot", status); }
        if (outcome == 3) { auto account{probe.data["account"]}; account.pushKV("base_available", 250'000); probe.data.pushKV("account", account); }
        uint160 hash; hash.begin()[0] = 42;
        const QString destination{outcome == 4 ? QStringLiteral("not-a-B3-address") : QString::fromStdString(EncodeDestination(PKHash{hash}))};
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        panel.m_snapshot->paused = true; panel.m_market_data.front().ready = false;
        panel.m_amount->setText(QStringLiteral("0.50")); panel.m_asset->setCurrentIndex(1); // Independent retained Deposit draft.
        panel.updateMarketText(); QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        bool reviewed{false}, cancel_default{false}, exact_destination{false};
        QTimer review_guard;
        connect(&review_guard, &QTimer::timeout, &panel, [&] {
            if (!panel.m_confirmation) return;
            reviewed = true; cancel_default = panel.m_confirmation->defaultButton() == panel.m_confirmation->button(QMessageBox::Cancel);
            exact_destination = panel.m_confirmation->text().contains(destination) && panel.m_confirmation->text().contains(QStringLiteral("1.25"));
            panel.m_confirmation->done(QMessageBox::Cancel);
        });
        review_guard.start(1);
        QTimer::singleShot(0, &panel, [&] {
            if (!panel.m_funding_dialog) return;
            panel.m_funding_dialog->findChild<QLineEdit*>(QStringLiteral("flowMeshFundingDraftAmount"))->setText(QStringLiteral("1.25"));
            panel.m_funding_dialog->findChild<QLineEdit*>(QStringLiteral("flowMeshFundingDraftDestination"))->setText(destination);
            panel.m_funding_dialog->findChild<QComboBox*>(QStringLiteral("flowMeshFundingDraftAsset"))->setCurrentIndex(outcome == 5 ? 1 : 0);
            panel.m_funding_dialog->accept();
        });
        panel.m_withdraw->click();
        QVERIFY(panel.m_thread); QVERIFY(panel.m_deferred_review && !panel.m_deferred_review->funding);
        QCOMPARE(panel.m_deferred_review->operation, B3FlowMeshTrading::Operation::Withdraw);
        QVERIFY(panel.m_active_result && !panel.m_active_result->action); QVERIFY(!panel.m_destination->isEnabled());
        if (outcome >= 6) {
            QVERIFY(probe.entered.tryAcquire(1, 2000));
            if (outcome == 6) panel.m_destination->setText(QStringLiteral("Changed after Continue"));
            else panel.m_withdrawal_draft_amount = QStringLiteral("2");
            probe.release.release();
        }
        QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000); review_guard.stop();
        const bool success{outcome == 2 || outcome == 5};
        QCOMPARE(reviewed, success); QCOMPARE(cancel_default, success); QCOMPARE(exact_destination, success);
        QCOMPARE(probe.snapshots.load(), 1); QCOMPARE(probe.writes.load(), 0); QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
        QCOMPARE(panel.m_amount->text(), QStringLiteral("0.50")); QCOMPARE(panel.m_asset->currentIndex(), 1);
        QVERIFY(!panel.m_confirmation); QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_deferred_review); QVERIFY(!panel.m_unlock);
        if (outcome < 6) {
            bool retained{false};
            QTimer::singleShot(0, &panel, [&] {
                if (!panel.m_funding_dialog) return;
                retained = panel.m_funding_dialog->findChild<QLineEdit*>(QStringLiteral("flowMeshFundingDraftAmount"))->text() == QStringLiteral("1.25") &&
                    panel.m_funding_dialog->findChild<QLineEdit*>(QStringLiteral("flowMeshFundingDraftDestination"))->text() == destination &&
                    panel.m_funding_dialog->findChild<QComboBox*>(QStringLiteral("flowMeshFundingDraftAsset"))->currentIndex() == (outcome == 5 ? 1 : 0);
                panel.m_funding_dialog->reject();
            });
            panel.m_withdraw->click(); QVERIFY(retained); QCOMPARE(probe.snapshots.load(), 1);
        }
    }
    void withdrawalPendingPolicyPreservesExplicitAcknowledgement()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        auto saved{SavedReadFixture(panel)};
        QVERIFY(panel.withdrawalReady()); QVERIFY(panel.orderReady()); QVERIFY(!panel.m_retry_receipt->isEnabled()); // Sticky no-resubmit history is not pending.
        saved.actions[0].receipt.no_resubmit = false; panel.m_saved_actions = saved;
        panel.m_receipt = saved.actions[0].receipt; panel.m_receipt_wallet = panel.m_wallet;
        panel.m_pending_market = saved.actions[0].market; panel.m_pending_account = saved.account; panel.m_pending_sequence = saved.actions[0].sequence;
        panel.updateControls(); QVERIFY(!panel.m_withdraw->isEnabled()); QVERIFY(!panel.m_order->isEnabled());
        panel.m_uncertain = true; panel.m_uncertain_refreshed = true; panel.m_uncertain_wallet = panel.m_wallet;
        panel.m_uncertain_action_id = saved.actions[0].receipt.action_id; panel.m_uncertain_market = saved.actions[0].market; panel.m_uncertain_account = saved.account;
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        QTimer::singleShot(0, &panel, [&] { if (panel.m_confirmation) panel.m_confirmation->done(QMessageBox::Yes); });
        panel.reviewUncertain(); // Existing explicit acknowledgement, never a signed withdrawal approval.
        QVERIFY(!panel.m_uncertain); QVERIFY(!panel.m_pending_sequence); QVERIFY(panel.withdrawalReady()); QVERIFY(panel.orderReady());
        QCOMPARE(panel.m_saved_actions.actions[0].receipt.state, QStringLiteral("unknown")); QVERIFY(!panel.m_saved_actions.actions[0].receipt.no_resubmit);
        QVERIFY(!panel.m_thread); QVERIFY(!panel.m_unlock); QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
    }
    void passiveRefreshUsesBoundedAuxiliaryPhases()
    {
        FundingReadProbe probe;
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        for (int phase{0}; phase < 4; ++phase) {
            panel.startJob(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
            QVERIFY2(!panel.m_read_failed, qPrintable(panel.m_read_error));
            QCOMPARE(probe.snapshots.load(), phase + 1);
            QCOMPARE(probe.catalogs.load(), 1);
            QCOMPARE(probe.balances.load(), phase >= 1 ? 1 : 0);
            QCOMPARE(probe.effects.load(), phase >= 2 ? 1 : 0);
        }
        for (const auto& [method, priority] : probe.priorities) QVERIFY(priority == node::FlowMeshClientWorkPriority::PASSIVE);
        QCOMPARE(probe.writes.load(), 0); QVERIFY(m_wallet->IsLocked());
    }
    void passiveRefreshFairnessAndAuxiliaryOrder()
    {
        FundingReadProbe probe; probe.held_method = "listflowmeshmarkets";
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        panel.m_receipt = B3FlowMeshTrading::Receipt{}; panel.m_receipt->action_id = QString::fromStdString(H(42).GetHex());
        panel.m_receipt_wallet = panel.m_wallet; panel.m_pending_market = panel.m_snapshot->market; panel.m_pending_account = panel.m_snapshot->account;
        for (int phase{0}; phase < 4; ++phase) {
            panel.m_catalog_age.invalidate(); // Overdue discovery must not starve later phases or receipts.
            probe.methods.clear(); panel.startJob();
            if (phase == 0) {
                QVERIFY(probe.auxiliary_entered.tryAcquire(1, 2000)); QCOMPARE(probe.snapshots.load(), 0);
                probe.auxiliary_release.release();
            }
            QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
            std::vector<std::string> remote;
            for (const auto& method : probe.methods) if (method != "getflowmeshclientinfo" && method != "listflowmeshactions") remote.push_back(method);
            const std::array<const char*, 4> expected{"listflowmeshmarkets", "getflowmeshbalance", "listflowmeshvaultoperations", "getflowmeshactionstatus"};
            QCOMPARE(remote.size(), size_t{2}); QCOMPARE(remote.front(), std::string{expected[phase]}); QCOMPARE(remote.back(), std::string{"getflowmeshmarketdata"});
        }
        QCOMPARE(probe.catalogs.load(), 1); QCOMPARE(probe.receipts.load(), 1); QCOMPARE(probe.snapshots.load(), 4);
        QCOMPARE(probe.writes.load(), 0); QVERIFY(m_wallet->IsLocked());
    }
    void passiveAuxiliaryFailureAdvancesWithoutRenewingFreshness()
    {
        using Phase = B3FlowMeshTradingPanel::RefreshPhase;
        for (const auto& [phase, method, next] : {std::tuple{Phase::Catalog, "listflowmeshmarkets", Phase::Status},
                 std::tuple{Phase::Status, "getflowmeshbalance", Phase::Effects}, std::tuple{Phase::Effects, "listflowmeshvaultoperations", Phase::Receipt}}) {
            FundingReadProbe probe; probe.failed_method = method;
            B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
            panel.m_refresh_phase = phase;
            const auto observed{panel.m_response_age.msecsSinceReference()};
            panel.startJob(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
            QVERIFY(panel.m_read_failed); QCOMPARE(probe.snapshots.load(), 1);
            QCOMPARE(panel.m_response_age.msecsSinceReference(), observed); QCOMPARE(panel.m_refresh_phase, next);
            probe.failed_method.clear(); panel.m_catalog_age.invalidate();
            panel.startJob(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
            QVERIFY(!panel.m_read_failed); QCOMPARE(probe.snapshots.load(), 2);
            QVERIFY(panel.m_refresh_phase != Phase::Catalog); QCOMPARE(probe.writes.load(), 0);
        }
    }
    void effectsFallbackUsesExactlyOneSnapshotAttempt()
    {
        for (int condition{0}; condition < 3; ++condition) {
            FundingReadProbe probe; probe.effects_misc_error = true; probe.fail = condition == 2;
            if (condition == 0) {
                auto status{probe.data["snapshot"]}; status.pushKV("chain_reconciling", true); status.pushKV("paused", true); probe.data.pushKV("snapshot", status);
            }
            B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
            panel.m_refresh_phase = B3FlowMeshTradingPanel::RefreshPhase::Effects;
            panel.startJob(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
            QCOMPARE(probe.effects.load(), 1); QCOMPARE(probe.snapshots.load(), 1);
            QCOMPARE(panel.m_read_failed, condition != 0); // A later healthy response never conceals the original effects error.
            if (condition == 0) QVERIFY(panel.m_snapshot->chain_reconciling);
            QCOMPARE(probe.writes.load(), 0);
        }
    }
    void phasedDiscoveryPreservesVerifiedBindingNotNewCheckpointClaims()
    {
        for (bool existing : {false, true}) {
            FundingReadProbe probe; probe.discovery_unproven = true; probe.checkpoint = true;
            B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
            if (existing) {
                panel.m_market_data.front().checkpoint_pending = true; panel.m_market_data.front().checkpoint = QString::fromStdString(H(70).GetHex());
            } else {
                panel.m_market_data.clear(); panel.m_snapshot.reset(); QSignalBlocker blocked{panel.m_market}; panel.m_market->clear();
                probe.held_method = "getflowmeshbalance"; // First selection immediately starts its next read.
            }
            panel.startJob();
            if (existing) { QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000); }
            else { QTRY_VERIFY_WITH_TIMEOUT(probe.auxiliary_entered.available() > 0, 1000); }
            QVERIFY(!panel.m_read_failed); QVERIFY(panel.market()); QVERIFY(panel.market()->has_account);
            QCOMPARE(panel.market()->account, Parse(probe.data).account);
            QCOMPARE(panel.market()->checkpoint_pending, existing);
            QCOMPARE(panel.market()->checkpoint, existing ? QString::fromStdString(H(70).GetHex()) : QString{});
            if (existing) panel.startJob();
            else probe.auxiliary_release.release();
            QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
            QVERIFY(panel.market()->checkpoint_pending); QCOMPARE(panel.market()->checkpoint, QString::fromStdString(H(80).GetHex()));
        }
        FundingReadProbe probe; probe.discovery_unproven = true;
        auto account{probe.data["account"]}; account.pushKV("account_id", H(99).GetHex()); account.pushKV("curves", UniValue{UniValue::VARR}); probe.data.pushKV("account", account);
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        const auto original{panel.market()->account}; panel.startJob(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QVERIFY(panel.m_read_failed); QCOMPARE(panel.market()->account, original); QCOMPARE(panel.m_snapshot->account, original);
        QCOMPARE(probe.writes.load(), 0);
    }
    void snapshotReceiptClockSurvivesDelayedUiDelivery()
    {
        FundingReadProbe probe; probe.hold = true;
        auto status{probe.data["snapshot"]}; status.pushKV("pending_actions", 1); probe.data.pushKV("snapshot", status);
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        panel.m_snapshot->head = QString::fromStdString(H(98).GetHex()); // Observe a newly received certified head.
        panel.startJob(); const auto result{panel.m_active_result};
        QVERIFY(probe.entered.tryAcquire(1, 2000)); probe.release.release(); QVERIFY(panel.m_thread->wait(2000));
        const auto received{result->snapshot_age.msecsSinceReference()};
        QTest::qSleep(3050); // Bounded blocked GUI delivery: do not process the queued finish before the freshness limit.
        QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QCOMPARE(panel.m_response_age.msecsSinceReference(), received); QCOMPARE(panel.m_certificate_age.msecsSinceReference(), received); QCOMPARE(panel.m_queue_age.msecsSinceReference(), received);
        QVERIFY(panel.m_response_age.elapsed() > 3000); QVERIFY(panel.m_order->isEnabled()); QVERIFY(!panel.orderReady()); QVERIFY(!panel.depositReady());
        QVERIFY(panel.m_status->text().contains(QStringLiteral("Updates delayed"))); QCOMPARE(probe.snapshots.load(), 1); QCOMPARE(probe.writes.load(), 0);
    }
    void obsoleteRefreshCannotAdvanceResetSchedule()
    {
        using Phase = B3FlowMeshTradingPanel::RefreshPhase;
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        auto result{std::make_shared<B3FlowMeshTradingPanel::Result>()};
        result->refresh_phase = Phase::Status; result->refresh_epoch = panel.m_refresh_epoch;
        result->error = QStringLiteral("Synthetic old read failure");
        panel.resetRefreshSchedule(); panel.finishJob(result);
        QCOMPARE(panel.m_refresh_phase, Phase::Catalog);
        panel.m_refresh_phase = Phase::Effects; panel.cancelAndWait(); QCOMPARE(panel.m_refresh_phase, Phase::Catalog);
        QVERIFY(!panel.m_thread); QVERIFY(!panel.m_wallet); QVERIFY(m_wallet->IsLocked());
    }
    void depositDraftContinueRefreshesOnceBeforeReview_data()
    {
        QTest::addColumn<int>("outcome");
        QTest::newRow("still paused") << 0;
        QTest::newRow("read failed") << 1;
        QTest::newRow("recovered ready") << 2;
        QTest::newRow("changed units") << 3;
        QTest::newRow("changed config") << 4;
        QTest::newRow("hard halt") << 5;
    }
    void depositDraftContinueRefreshesOnceBeforeReview()
    {
        QFETCH(int, outcome);
        FundingReadProbe probe;
        if (outcome == 0 || outcome == 5) {
            auto status{probe.data["snapshot"]}; status.pushKV("paused", true);
            status.pushKV("error", "FlowMesh service is not active at the current B3 tip");
            if (outcome == 5) status.pushKV("halt", "invalid-checkpoint");
            probe.data.pushKV("snapshot", status);
        }
        probe.fail = outcome == 1;
        if (outcome == 3) { auto units{probe.data["base_metadata"]}; units.pushKV("decimals", 7); probe.data.pushKV("base_metadata", units); }
        if (outcome == 4) probe.data.pushKV("execution_config_id", H(90).GetHex());
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        panel.m_market_data.front().ready = false; panel.m_snapshot->paused = true;
        panel.m_snapshot->error = QStringLiteral("FlowMesh service is not active at the current B3 tip");
        panel.m_catalog_age.start(); panel.updateMarketText();
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        bool reviewed{false}, cancel_default{false}, draft_opened{false};
        QTimer review_guard;
        connect(&review_guard, &QTimer::timeout, &panel, [&] {
            if (!panel.m_confirmation) return;
            reviewed = true; cancel_default = panel.m_confirmation->defaultButton() == panel.m_confirmation->button(QMessageBox::Cancel);
            panel.m_confirmation->done(QMessageBox::Cancel); // Never approve preparation or unlock.
        });
        review_guard.start(1);
        QTimer::singleShot(0, &panel, [&] {
            if (!panel.m_funding_dialog) return;
            draft_opened = true;
            if (auto* amount{panel.m_funding_dialog->findChild<QLineEdit*>(QStringLiteral("flowMeshFundingDraftAmount"))}) amount->setText(QStringLiteral("1.25"));
            panel.m_funding_dialog->accept();
        });
        panel.m_deposit->click();
        QVERIFY(draft_opened); QVERIFY(panel.m_thread); QVERIFY(panel.m_deferred_review);
        QVERIFY(!panel.m_deferred_review->funding); QVERIFY(panel.m_deferred_review->funding_context);
        QVERIFY(panel.m_active_result && !panel.m_active_result->action);
        QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        review_guard.stop();
        QCOMPARE(probe.catalogs.load(), 1); QCOMPARE(probe.snapshots.load(), 1);
        QCOMPARE(reviewed, outcome == 2); QCOMPARE(cancel_default, outcome == 2);
        const QString retained_amount{outcome == 3 || outcome == 4 ? QString{} : QStringLiteral("1.25")};
        QCOMPARE(panel.m_amount->text(), retained_amount); // Never carry amounts into changed units/configuration.
        QVERIFY(!panel.m_deferred_review); QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_confirmation); QVERIFY(!panel.m_unlock);
        QCOMPARE(probe.writes.load(), 0); QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
        if (outcome != 5) {
            bool retained{false};
            QTimer::singleShot(0, &panel, [&] {
                if (!panel.m_funding_dialog) return;
                if (auto* amount{panel.m_funding_dialog->findChild<QLineEdit*>(QStringLiteral("flowMeshFundingDraftAmount"))}) retained = amount->text() == retained_amount;
                panel.m_funding_dialog->reject();
            });
            panel.m_deposit->click(); QVERIFY(retained);
            QCOMPARE(probe.snapshots.load(), 1); QCOMPARE(probe.writes.load(), 0); // Cancel is read-only and does not refresh.
        }
    }
    void depositDraftCannotBypassActionReadiness()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        panel.m_snapshot->paused = true; panel.m_amount->setText(QStringLiteral("1.25"));
        panel.updateMarketText();
        QVERIFY(panel.m_deposit->isEnabled()); QVERIFY(!panel.depositReady());
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        bool reviewed{false};
        QTimer::singleShot(0, &panel, [&] {
            if (panel.m_confirmation) { reviewed = true; panel.m_confirmation->done(QMessageBox::Cancel); }
        });
        panel.begin(B3FlowMeshTrading::Operation::Deposit);
        QVERIFY(!reviewed); QVERIFY(!panel.m_confirmation); QVERIFY(!panel.m_thread); QVERIFY(!panel.m_unlock);
        QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
    }
    void fundingDraftClickSurvivesFailedPassiveRead()
    {
        for (bool withdrawal : {false, true}) {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        ReadInFlight(panel); (withdrawal ? panel.m_withdraw : panel.m_deposit)->click();
        QVERIFY(panel.m_deferred_review && panel.m_deferred_review->funding);
        bool opened{false};
        QTimer::singleShot(0, &panel, [&] {
            if (panel.m_funding_dialog) { opened = true; panel.m_funding_dialog->reject(); }
        });
        auto failure{std::make_shared<B3FlowMeshTradingPanel::Result>()}; failure->error = QStringLiteral("Synthetic passive read failed");
        panel.finishJob(failure);
        QVERIFY(opened); QVERIFY(panel.m_read_failed); QVERIFY(panel.m_deposit->isEnabled()); QVERIFY(!panel.depositReady());
        QVERIFY(!panel.m_deferred_review); QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_unlock);
        QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
        }
    }
    void depositDraftRejectsUnsafeContext()
    {
        for (int condition{0}; condition < 8; ++condition) {
            B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
            switch (condition) {
            case 0: panel.m_uncertain = true; break;
            case 1: panel.m_security_warning = QStringLiteral("Synthetic relock failure"); break;
            case 2: panel.m_snapshot.reset(); break;
            case 3: panel.m_snapshot->certified = false; break;
            case 4: panel.m_snapshot->account_state_verified = false; break;
            case 5: panel.m_snapshot->config = QString::fromStdString(H(90).GetHex()); break;
            case 6: panel.m_snapshot->halt = QStringLiteral("invalid-checkpoint"); break;
            case 7: panel.m_saved_actions_ready = false; break;
            }
            panel.updateControls(); QVERIFY(!panel.m_deposit->isEnabled());
            QVERIFY(!panel.m_withdraw->isEnabled()); panel.openFunding(true); QVERIFY(!panel.m_funding_dialog);
            QVERIFY(!panel.m_order->isEnabled()); panel.requestOrderReview(); QVERIFY(!panel.m_deferred_review);
            panel.openFunding(false); QVERIFY(!panel.m_funding_dialog); QVERIFY(!panel.m_thread);
        }
        auto watch{MakeOfflineWallet("deposit-watch-only")};
        { LOCK(watch.wallet->cs_wallet); watch.wallet->SetWalletFlag(wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS); }
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel, *watch.model, watch.wallet);
        QVERIFY(panel.m_backend->privateKeysDisabled()); panel.updateControls(); QVERIFY(!panel.m_deposit->isEnabled());
        panel.openFunding(false); QVERIFY(!panel.m_funding_dialog); QVERIFY(!panel.m_thread);
        panel.openFunding(true); QVERIFY(!panel.m_withdraw->isEnabled()); QVERIFY(!panel.m_funding_dialog);
        QVERIFY(!panel.m_order->isEnabled()); panel.requestOrderReview(); QVERIFY(!panel.m_thread);
    }
    void fundingDraftWalletSwitchDiscardsFormAndContinuation()
    {
        FundingReadProbe probe;
        for (bool withdrawal : {false, true}) for (bool during_read : {false, true}) {
            B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
            QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
            probe.hold = during_read;
            QTimer::singleShot(0, &panel, [&] {
                if (!panel.m_funding_dialog) return;
                if (auto* amount{panel.m_funding_dialog->findChild<QLineEdit*>(QStringLiteral("flowMeshFundingDraftAmount"))}) amount->setText(QStringLiteral("1.25"));
                if (during_read) panel.m_funding_dialog->accept();
                else panel.setWalletModel(nullptr);
            });
            (withdrawal ? panel.m_withdraw : panel.m_deposit)->click();
            if (during_read) {
                QVERIFY(panel.m_deferred_review); QVERIFY(probe.entered.tryAcquire(1, 2000));
                probe.release.release(); panel.setWalletModel(nullptr);
            }
            QCoreApplication::processEvents();
            QVERIFY(!panel.m_wallet); QVERIFY(panel.m_amount->text().isEmpty());
            QVERIFY(panel.m_withdrawal_draft_amount.isEmpty()); QVERIFY(panel.m_destination->text().isEmpty());
            QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_deferred_review);
            QVERIFY(!panel.m_funding_dialog); QVERIFY(!panel.m_confirmation); QVERIFY(!panel.m_unlock);
            QCOMPARE(unlock.count(), 0); QCOMPARE(probe.writes.load(), 0); QVERIFY(m_wallet->IsLocked());
        }
    }
    void deferredVaultReviewCannotFollowEffectFallback()
    {
        for (bool missing_identity : {false, true}) {
            B3FlowMeshTradingPanel panel;
            AttachOfflineWallet(panel);
            QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
            for (int i{0}; i < 2; ++i) {
                const QString id{QString::fromStdString(H(static_cast<unsigned char>(70 + i)).GetHex())};
                UniValue effect{UniValue::VOBJ};
                effect.pushKV("market_id", panel.m_snapshot->market.toStdString());
                effect.pushKV("account_id", panel.m_snapshot->account.toStdString());
                effect.pushKV("asset", panel.m_snapshot->base.toStdString());
                effect.pushKV("effect_id", id.toStdString());
                effect.pushKV("kind", "deposit-sweep"); effect.pushKV("amount", 1);
                panel.m_effect_data.push_back(effect);
                panel.m_effect->addItem(id, i);
                panel.m_effect->setItemData(i, id, Qt::UserRole + 1);
            }
            ReadInFlight(panel);
            QVERIFY(panel.m_publish->isEnabled());
            panel.m_publish->click();
            QVERIFY(panel.m_deferred_review);
            QCOMPARE(panel.m_deferred_review->effect, panel.m_effect->itemData(0, Qt::UserRole + 1).toString());
            panel.stopWorker(); panel.m_busy = false;
            if (missing_identity) panel.m_effect->setItemData(0, QVariant{}, Qt::UserRole + 1);
            else panel.m_effect->setCurrentIndex(1);
            panel.updateControls();
            QVERIFY(panel.m_publish->isEnabled()); // A new review is still possible.
            bool opened{false};
            QTimer::singleShot(0, &panel, [&] {
                if (panel.m_confirmation) { opened = true; panel.m_confirmation->done(QMessageBox::Cancel); }
            });
            panel.resumeReview();
            QVERIFY(!opened); QVERIFY(!panel.m_deferred_review); QVERIFY(!panel.m_confirmation);
            QVERIFY(!panel.m_thread); QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_unlock);
            QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
        }
    }
    void marketSwitchClearsPriorDataButSameMarketFailureRetainsIt()
    {
        B3FlowMeshTradingPanel panel;
        Observe(panel, Parse(Data()));
        auto* retained{panel.m_history_view->item(0, 0)};
        const QString balance{panel.m_balances->text()};
        QVERIFY(!balance.isEmpty());
        panel.m_read_failed = true;
        panel.updateMarketText();
        QVERIFY(panel.m_snapshot); QCOMPARE(panel.m_chart->pricePointCount(), 1);
        QCOMPARE(panel.m_history_view->item(0, 0), retained);
        QCOMPARE(panel.m_balances->text(), balance);
        auto other{panel.m_market_data.front()};
        other.base = QString::fromStdString(H(90).GetHex());
        other.id = QString::fromStdString(flowmesh::ComputeFlowMeshMarketId(H(1), H(90))->GetHex());
        other.vault = QString::fromStdString(flowmesh::ComputeFlowMeshVaultId(H(1), *uint256::FromHex(other.id.toStdString()))->GetHex());
        panel.m_market_data.push_back(other);
        panel.m_market->addItem(QStringLiteral("Other synthetic market"), other.id);
        panel.m_queue_age.start();
        panel.m_withdrawal_draft_amount = QStringLiteral("1.25"); panel.m_destination->setText(QStringLiteral("old market destination"));
        panel.m_market->setCurrentIndex(1); // Exercise the real selection callback.
        QVERIFY(panel.m_withdrawal_draft_amount.isEmpty()); QVERIFY(panel.m_destination->text().isEmpty());
        QVERIFY(!panel.m_snapshot); QVERIFY(!panel.m_response_age.isValid());
        QVERIFY(!panel.m_queue_age.isValid()); QCOMPARE(panel.m_chart->pricePointCount(), 0);
        QCOMPARE(panel.m_depth_view->rowCount(), 0); QCOMPARE(panel.m_history_view->rowCount(), 0);
        QCOMPARE(panel.m_own_view->rowCount(), 0); QVERIFY(panel.m_balances->text().isEmpty());
        QVERIFY(!panel.m_order->isEnabled()); QVERIFY(!panel.m_thread); QVERIFY(!panel.m_wallet);
    }
    void marketSwitchYieldsThePassiveReadAndReadsTheNewMarketAtOnce()
    {
        // A passive read of the market being left stops at its next RPC (the
        // catalog call under way completes) and the new market is read at
        // once; the old market's snapshot is never read.
        FundingReadProbe probe; probe.held_method = "listflowmeshmarkets";
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        struct Cleanup { FundingReadProbe& probe; B3FlowMeshTradingPanel& panel; ~Cleanup() { probe.auxiliary_release.release(8); panel.cancelAndWait(); } } cleanup{probe, panel};
        auto other{panel.m_market_data.front()};
        other.base = QString::fromStdString(H(90).GetHex());
        other.id = QString::fromStdString(flowmesh::ComputeFlowMeshMarketId(H(1), H(90))->GetHex());
        other.vault = QString::fromStdString(flowmesh::ComputeFlowMeshVaultId(H(1), *uint256::FromHex(other.id.toStdString()))->GetHex());
        panel.m_market_data.push_back(other);
        { QSignalBlocker blocked{panel.m_market}; panel.m_market->addItem(QStringLiteral("Other synthetic market"), other.id); }
        panel.m_refresh_phase = B3FlowMeshTradingPanel::RefreshPhase::Catalog;
        panel.startJob(); QVERIFY(probe.auxiliary_entered.tryAcquire(1, 2000));
        const auto passive{panel.m_active_result};
        QVERIFY(passive && passive->passive);
        panel.m_market->setCurrentIndex(1);
        QVERIFY(panel.m_yield_refresh->load());
        probe.auxiliary_release.release();
        // The next read is of the new market, without waiting for a tick.
        QTRY_VERIFY_WITH_TIMEOUT(panel.m_active_result && panel.m_active_result != passive, 2000);
        QVERIFY(passive->refresh_yielded);
        QCOMPARE(panel.m_active_result->views_market, other.id);
        QVERIFY(!panel.m_timer->isActive());
        QCOMPARE(probe.snapshots.load(), 0);
        QCOMPARE(probe.writes.load(), 0);
    }
    void backgroundReadDoesNotWeakenReadinessGuards()
    {
        for (int condition{0}; condition < 7; ++condition) {
            B3FlowMeshTradingPanel panel;
            AttachOfflineWallet(panel);
            ReadInFlight(panel);
            switch (condition) {
            case 0: panel.m_read_failed = true; break;
            case 1: panel.m_response_age.invalidate(); break;
            case 2: panel.m_uncertain = true; break;
            case 3: panel.m_security_warning = QStringLiteral("Unverified relock in isolated test"); break;
            case 4: panel.m_snapshot->paused = true; break;
            case 5: panel.m_snapshot->handoff = true; break;
            case 6: panel.m_snapshot->certified = false; break;
            }
            panel.updateControls();
            QVERIFY(!panel.m_busy); // Not merely disabled by a modal operation.
            for (auto* button : {panel.m_cancel_order, panel.m_admit}) QVERIFY(!button->isEnabled());
            QCOMPARE(panel.m_order->isEnabled(), condition == 0 || condition == 1 || condition == 4 || condition == 5);
            QVERIFY(!panel.orderReady());
            QCOMPARE(panel.m_deposit->isEnabled(), condition == 0 || condition == 1 || condition == 4 || condition == 5);
            QCOMPARE(panel.m_withdraw->isEnabled(), condition == 0 || condition == 1 || condition == 4 || condition == 5);
            QVERIFY(!panel.depositReady()); QVERIFY(!panel.withdrawalReady());
            QVERIFY(!panel.m_deferred_review); QVERIFY(!panel.m_active_result);
        }
    }
    void deferredReviewRequiresSuccessfulUnchangedContext_data()
    {
        QTest::addColumn<int>("condition");
        QTest::newRow("read failed") << 0;
        QTest::newRow("freshness lost") << 1;
        QTest::newRow("uncertain submission") << 2;
        QTest::newRow("security warning") << 3;
        QTest::newRow("paused snapshot") << 4;
        QTest::newRow("generation changed") << 5;
        QTest::newRow("market changed") << 6;
        QTest::newRow("account request still pending") << 7;
        QTest::newRow("price text changed") << 8;
        QTest::newRow("quantity text changed") << 9;
        QTest::newRow("display side changed") << 10;
        QTest::newRow("orientation changed") << 11;
        QTest::newRow("asset units changed") << 12;
        QTest::newRow("configuration changed") << 13;
        QTest::newRow("account changed") << 14;
        QTest::newRow("remote provenance changed") << 15;
        QTest::newRow("vault changed") << 16;
        QTest::newRow("wallet switched") << 17;
        QTest::newRow("shutdown") << 18;
    }
    void deferredReviewRequiresSuccessfulUnchangedContext()
    {
        QFETCH(int, condition);
        B3FlowMeshTradingPanel panel;
        AttachOfflineWallet(panel);
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        ReadInFlight(panel);
        panel.m_order->click();
        QVERIFY(panel.m_deferred_review);
        panel.stopWorker();
        panel.m_busy = false;
        switch (condition) {
        case 0: panel.m_read_failed = true; break;
        case 1: panel.m_response_age.invalidate(); break;
        case 2: panel.m_uncertain = true; break;
        case 3: panel.m_security_warning = QStringLiteral("Unverified relock in isolated test"); break;
        case 4: panel.m_snapshot->paused = true; break;
        case 5: ++panel.m_generation; break;
        case 6: { QSignalBlocker blocked{panel.m_market}; panel.m_market->setCurrentIndex(-1); break; }
        case 7:
            panel.m_pending_market = panel.m_snapshot->market;
            panel.m_pending_account = panel.m_snapshot->account;
            panel.m_pending_sequence = panel.m_snapshot->account_sequence;
            break;
        case 8: panel.m_price->setText(QStringLiteral("1.0")); break; // Even equivalent numeric spelling is frozen.
        case 9: panel.m_quantity->setText(QStringLiteral("1.0")); break;
        case 10: panel.m_side->setCurrentIndex(1); break;
        case 11: { QSignalBlocker blocked{panel.m_orientation}; panel.m_orientation->setCurrentIndex(1); break; }
        case 12: ++panel.m_snapshot->units.decimals; break;
        case 13: panel.m_market_data.front().config = panel.m_snapshot->config = QString::fromStdString(H(92).GetHex()); break;
        case 14: panel.m_market_data.front().account = panel.m_snapshot->account = QString::fromStdString(H(93).GetHex()); break;
        case 15:
            panel.m_market_data.front().remote = panel.m_snapshot->remote = true;
            panel.m_snapshot->certificate_verified = panel.m_snapshot->account_state_verified = true;
            panel.m_snapshot->endpoint = QStringLiteral("https://changed.invalid"); break;
        case 16: panel.m_market_data.front().vault = QString::fromStdString(H(94).GetHex()); break;
        case 17: panel.setWalletModel(nullptr); break;
        case 18: panel.cancelAndWait(); break;
        }
        // If a regression opens a modal review, dismiss it without approving
        // anything; the explicit assertion then fails without hanging tests.
        bool opened{false};
        QTimer::singleShot(0, &panel, [&] {
            if (panel.m_confirmation) { opened = true; panel.m_confirmation->done(QMessageBox::Cancel); }
        });
        panel.resumeReview();
        QVERIFY(!opened); QVERIFY(!panel.m_deferred_review); QVERIFY(!panel.m_confirmation);
        QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_thread); QVERIFY(!panel.m_unlock);
        QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
        if (condition < 8 && condition != 5) QVERIFY(!panel.orderReady());
        if (condition == 0 || condition == 1 || condition == 4) QVERIFY(panel.m_order->isEnabled());
        if (condition == 2 || condition == 3 || condition == 6 || condition == 7 || condition >= 17) QVERIFY(!panel.m_order->isEnabled());
    }
    void successfulReadResumesExplicitReviewButNeverApprovesIt()
    {
        B3FlowMeshTradingPanel panel;
        AttachOfflineWallet(panel);
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        ReadInFlight(panel);
        panel.m_order->click();
        QVERIFY(panel.m_deferred_review);
        panel.stopWorker(); panel.m_busy = false; panel.m_response_age.restart();
        bool reviewed{false}, cancel_default{false};
        QTimer::singleShot(0, &panel, [&] {
            if (panel.m_confirmation) {
                reviewed = true;
                cancel_default = panel.m_confirmation->defaultButton() == panel.m_confirmation->button(QMessageBox::Cancel);
                panel.m_confirmation->done(QMessageBox::Cancel);
            }
        });
        panel.resumeReview();
        QVERIFY(reviewed); QVERIFY(cancel_default); QVERIFY(!panel.m_deferred_review); QVERIFY(!panel.m_confirmation);
        QVERIFY(!panel.m_active_result); QVERIFY(!panel.m_thread); QVERIFY(!panel.m_unlock);
        QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
        panel.resumeReview(); // A consumed intent cannot replay.
        QVERIFY(!panel.m_confirmation); QVERIFY(!panel.m_active_result); QCOMPARE(unlock.count(), 0);
    }
    void technicalDetailsAreHiddenUntilExplicitlyRequested()
    {
        B3FlowMeshTradingPanel panel;
        panel.resize(1200, 850);
        Observe(panel, Parse(Data())); // No wallet: opening Details cannot start RPC.
        panel.show();
        QCoreApplication::processEvents();
        auto* details{panel.findChild<QPushButton*>(QStringLiteral("flowMeshDetails"))};
        QVERIFY(details); QVERIFY(details->isVisible()); QVERIFY(!details->isChecked());
        for (QWidget* technical : std::array<QWidget*, 7>{panel.m_advanced, panel.m_identity_detail,
             panel.m_progress, panel.m_grid_note, panel.m_refresh, panel.m_checkpoint, panel.m_publish}) QVERIFY(!technical->isVisible());
        auto* book{panel.findChild<B3FlowMeshOrderBook*>(QStringLiteral("flowMeshOrderBook"))};
        auto* mode{panel.findChild<QComboBox*>(QStringLiteral("flowMeshLiquidityMode"))};
        QVERIFY(book); QVERIFY(mode); QCOMPARE(mode->currentIndex(), 0);
        QVERIFY(panel.m_chart->isVisible()); QVERIFY(book->isVisible()); QVERIFY(!panel.m_depth_view->isVisible());
        mode->setCurrentIndex(1); QCoreApplication::processEvents();
        QVERIFY(panel.m_depth_view->isVisible()); QVERIFY(!book->isVisible());
        mode->setCurrentIndex(0); QCoreApplication::processEvents();
        QVERIFY(book->isVisible()); QVERIFY(!panel.m_depth_view->isVisible());
        QVERIFY(panel.m_price->isVisible()); QVERIFY(panel.m_quantity->isVisible());
        QVERIFY(panel.m_deposit->isVisible()); QVERIFY(panel.m_order->isVisible());
        QVERIFY(panel.m_balances->isVisible()); QVERIFY(panel.m_own_view->isVisible());
        // Simplifying the default view must not hide the test-asset warning.
        QVERIFY(panel.m_status->isVisible());
        QVERIFY(panel.m_status->text().contains(QStringLiteral("test"), Qt::CaseInsensitive));
        QVERIFY(panel.m_status->text().contains(QStringLiteral("unbacked"), Qt::CaseInsensitive));
        details->click();
        QCoreApplication::processEvents();
        QVERIFY(details->isChecked()); QVERIFY(panel.m_advanced->isVisible());
        QVERIFY(panel.m_identity_detail->isVisible()); QVERIFY(panel.m_grid_note->isVisible());
        QVERIFY(panel.m_refresh->isVisible()); QVERIFY(panel.m_checkpoint->isVisible());
        QVERIFY(panel.m_publish->isVisible()); QVERIFY(!panel.m_thread);
        details->click();
        QVERIFY(!panel.m_advanced->isVisible()); QVERIFY(!panel.m_refresh->isVisible());
        QVERIFY(!panel.m_order->isEnabled()); QVERIFY(!panel.m_wallet);
    }
    void conciseStatusStillDisclosesPausedUnknownAndPartialData()
    {
        auto snapshot{Parse(Data())};
        snapshot.curves_complete = false;
        snapshot.history_page_partial = true;
        B3FlowMeshTradingPanel panel;
        Observe(panel, snapshot);
        QVERIFY(panel.m_liquidity_note->text().contains(QStringLiteral("Partial")));
        QVERIFY(panel.m_history_note->text().contains(QStringLiteral("partial")));
        QVERIFY(panel.m_history_note->toolTip().contains(QStringLiteral("unknown, not zero")));
        QCOMPARE(panel.m_history_view->rowCount(), 1);
        panel.m_snapshot->paused = true;
        panel.updateMarketText();
        QVERIFY(panel.m_status->text().contains(QStringLiteral("paused")));
        QVERIFY(panel.m_status->text().contains(QStringLiteral("unbacked")));
        panel.m_uncertain = true;
        panel.updateMarketText();
        QVERIFY(panel.m_status->text().contains(QStringLiteral("outcome unknown")));
        QVERIFY(panel.m_status->text().contains(QStringLiteral("review before trading")));
        QVERIFY(panel.m_status->text().contains(QStringLiteral("unbacked")));
        QVERIFY(!panel.m_order->isEnabled());
    }
    void unchangedReplyRefreshesStatusWithoutRebuildingViews()
    {
        // A quiet watch timeout or a same-head poll: status fields update in
        // place; the views are rebuilt only when something they draw changed.
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        auto shown{WithLevels(Parse(RemoteDataWithTimes()), {}, H(5))};
        QVERIFY(shown.levels && shown.levels->available);
        Observe(panel, shown, true);
        const auto curves{panel.m_snapshot->curves};
        const auto reply = [&] {
            auto fresh{shown}; fresh.unchanged = true; fresh.curves.clear(); fresh.own_curves.clear(); fresh.history.clear(); fresh.depth.clear();
            fresh.levels.reset(); fresh.ticks = TickPage{}; fresh.ticks->available = true; fresh.ticks->remote = true;
            return fresh;
        };
        const auto apply = [&](Snapshot fresh) {
            QElapsedTimer age; age.start();
            QCOMPARE(panel.applySelectedSnapshot(std::move(fresh), age, panel.m_snapshot->head), B3FlowMeshTradingPanel::SnapshotApply::APPLIED);
        };
        apply(reply());
        const auto builds{panel.m_view_builds};
        // Status only: the queue and the runtime's pause.
        auto status{reply()}; status.pending_actions = 3; status.paused = true;
        apply(status);
        QCOMPARE(panel.m_view_builds, builds);
        QCOMPARE(panel.m_snapshot->pending_actions, uint64_t{3}); QVERIFY(panel.m_snapshot->paused);
        QCOMPARE(panel.m_snapshot->curves, curves); QVERIFY(panel.m_snapshot->levels == shown.levels);
        QVERIFY(!panel.m_snapshot->unchanged);
        // The head's row alone (a same-head reply's observed times): the
        // rows of its history and ticks stay, and nothing is drawn again.
        const auto observed{panel.m_snapshot->observed};
        QVERIFY(observed && observed->rows.size() >= 2);
        auto head_only{reply()}; head_only.observed->rows.erase(head_only.observed->rows.begin());
        apply(head_only); QCOMPARE(panel.m_view_builds, builds);
        QCOMPARE(panel.m_snapshot->observed, observed);
        // Something drawn: an event gap, other observed times, other units.
        auto gap{reply()}; gap.event_gap = true;
        apply(gap); QCOMPARE(panel.m_view_builds, builds + 1);
        apply(gap); QCOMPARE(panel.m_view_builds, builds + 1);
        auto times{gap}; times.observed->rows.back().ms += 1000;
        apply(times); QCOMPARE(panel.m_view_builds, builds + 2);
        auto units{times}; units.units.ticker = QStringLiteral("tUSD2");
        apply(units); QCOMPARE(panel.m_view_builds, builds + 3);
        QCOMPARE(panel.m_snapshot->units.ticker, QStringLiteral("tUSD2"));
        // Levels asked for again (another grouping) are drawn at once.
        auto levels{units}; levels.levels = shown.levels; levels.levels->total_curves += 1;
        apply(levels); QCOMPARE(panel.m_view_builds, builds + 4);
        QCOMPARE(panel.m_snapshot->levels->total_curves, shown.levels->total_curves + 1);
    }
    void unchangedReplySupersededByNewerSnapshotIsNotAFailure()
    {
        using Apply = B3FlowMeshTradingPanel::SnapshotApply;
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        const auto old_head{panel.m_snapshot->head};
        // A newer head is displayed (as a live update would apply it).
        auto newer{*panel.m_snapshot}; newer.head = QString::fromStdString(H(9).GetHex()); newer.next_sequence = 4;
        panel.m_snapshot = newer;
        auto unchanged{Parse(RemoteData())}; unchanged.unchanged = true;
        QElapsedTimer age; age.start();
        const auto responded{panel.m_response_age.msecsSinceReference()};
        // The reply only says the old head was still current: superseded.
        QCOMPARE(panel.applySelectedSnapshot(unchanged, age, old_head), Apply::SUPERSEDED);
        QVERIFY(!panel.m_read_failed);
        QCOMPARE(panel.m_snapshot->head, newer.head);
        QCOMPARE(panel.m_response_age.msecsSinceReference(), responded);
        // Against the displayed head, the same mismatch is a real failure.
        QCOMPARE(panel.applySelectedSnapshot(unchanged, age, newer.head), Apply::MISMATCH);
        QVERIFY(panel.m_read_failed);
    }
    void olderFullSnapshotNeverReplacesNewerHead()
    {
        using Apply = B3FlowMeshTradingPanel::SnapshotApply;
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        auto newer{*panel.m_snapshot}; newer.head = QString::fromStdString(H(9).GetHex()); newer.next_sequence = 5;
        panel.m_snapshot = newer;
        QElapsedTimer age; age.start();
        auto older{Parse(RemoteData())};
        QCOMPARE(older.next_sequence, uint64_t{3});
        QCOMPARE(panel.applySelectedSnapshot(older, age, {}), Apply::SUPERSEDED);
        QCOMPARE(panel.m_snapshot->next_sequence, uint64_t{5});
        QVERIFY(!panel.m_read_failed);
        // The same or a newer head still applies.
        auto same{newer}; same.pending_actions = 1;
        QCOMPARE(panel.applySelectedSnapshot(same, age, {}), Apply::APPLIED);
        QCOMPARE(panel.m_snapshot->pending_actions, uint64_t{1});
    }
    void olderAuxJobNeverRevertsWatchDerivedRow()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        // A job started against sequence 3 and captured the catalog rows then.
        B3FlowMeshTradingPanel::Result result;
        result.markets = panel.m_market_data; result.basis_sequence = panel.m_snapshot->next_sequence; result.catalog = true;
        // A newer head was displayed meanwhile, with a newer account state.
        auto newer{*panel.m_snapshot}; newer.head = QString::fromStdString(H(9).GetHex()); newer.next_sequence = 4;
        newer.account_sequence = 9; newer.b3_available = 1'000'000'000;
        panel.m_snapshot = newer;
        panel.DeriveRowFromSnapshot(panel.m_market_data[0], newer);
        QCOMPARE(panel.market()->sequence, uint64_t{9});
        // The older job's catalog does not roll the selected row back.
        panel.applyMarketCatalog(result);
        QCOMPARE(panel.market()->sequence, uint64_t{9});
        QCOMPARE(panel.market()->b3_available, CAmount{1'000'000'000});
        QCOMPARE(panel.m_snapshot->head, newer.head);
        // Without a newer display, the job's rows are applied as before.
        result.basis_sequence = 4; result.markets[0].sequence = 12;
        panel.applyMarketCatalog(result);
        QCOMPARE(panel.market()->sequence, uint64_t{12});
    }
    void pollingModeIsTodaysBehaviourAgainstOldEndpoints()
    {
        FundingReadProbe probe; probe.live_ms = 0;
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        Live(panel, probe);
        QCOMPARE(panel.m_watch.mode, B3FlowMeshWatch::Mode::Polling);
        for (int i{0}; i < 5; ++i) { panel.updateWatchState(); panel.armWatch(); QVERIFY(!panel.m_watch_thread); QVERIFY(!panel.liveSkipsPoll()); }
        QCOMPARE(probe.watches.load(), 0);
        QVERIFY(panel.liveUpdateText().contains(QStringLiteral("offers no live updates")));
        // Today's passive job still ends with the market read.
        panel.startJob(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QCOMPARE(probe.snapshots.load(), 1); QCOMPARE(probe.watches.load(), 0); QCOMPARE(probe.writes.load(), 0);
    }
    void liveWatchAppliesAChangedHeadAndStaysLive()
    {
        FundingReadProbe probe; probe.live_ms = 2000;
        probe.watch_data = NewerRemoteData(); probe.watch_object = WatchObject("ready", true, true);
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        Live(panel, probe);
        QCOMPARE(panel.m_watch.mode, B3FlowMeshWatch::Mode::Probing);
        panel.m_response_age.invalidate();
        probe.hold_watch = true;
        panel.armWatch(); QVERIFY(panel.m_watch_thread);
        QVERIFY(probe.watch_entered.tryAcquire(1, 2000));
        probe.watch_release.release(); // Only this read returns; the next one is held.
        QTRY_COMPARE_WITH_TIMEOUT(panel.m_snapshot->head, QString::fromStdString(H(10).GetHex()), 3000);
        QCOMPARE(panel.m_watch.last, B3FlowMeshWatch::Outcome::Changed);
        // Live: the next watch was armed at once against the new head.
        QVERIFY(probe.watch_entered.tryAcquire(1, 2000));
        QCOMPARE(panel.m_snapshot->next_sequence, uint64_t{4});
        QCOMPARE(panel.market()->sequence, uint64_t{9});
        QVERIFY(panel.m_response_age.isValid() && panel.m_response_age.elapsed() < 3000);
        QCOMPARE(panel.m_watch.mode, B3FlowMeshWatch::Mode::Live);
        QVERIFY(panel.liveUpdateText().contains(QStringLiteral("live updates")));
        QVERIFY(!panel.m_read_failed);
        // Live: a passive refresh without an auxiliary phase is skipped.
        panel.m_refresh_phase = B3FlowMeshTradingPanel::RefreshPhase::None; panel.m_catalog_age.start();
        QVERIFY(panel.liveSkipsPoll());
        panel.m_watch.Disable(); probe.hold_watch = false; probe.watch_release.release(); AwaitWatchWorker(panel);
        QCOMPARE(probe.writes.load(), 0); QVERIFY(m_wallet->IsLocked());
    }
    void probeOnABusyMarketReachesLiveBesideThePollingTimer()
    {
        // Every ordinary market read beside a live-update read supersedes it,
        // as on a busy validator. The ticking panel still gets its probe
        // through: the probe is armed first and holds the tick's market read.
        FundingReadProbe probe; probe.live_ms = 2000; probe.watch_object = WatchObject("timeout", true, true);
        probe.superseded_object = WatchObject("superseded", false, false); probe.supersede_on_concurrent_read = true;
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        Live(panel, probe);
        QCOMPARE(panel.m_watch.mode, B3FlowMeshWatch::Mode::Probing);
        panel.m_timer->start();
        QTRY_COMPARE_WITH_TIMEOUT(panel.m_watch.mode, B3FlowMeshWatch::Mode::Live, 4000);
        QCOMPARE(probe.superseded.load(), 0);
        QVERIFY(probe.watches.load() >= 1);
        panel.m_timer->stop();
        panel.m_watch.Disable(); AwaitWatchWorker(panel);
        QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 3000);
        QCOMPARE(probe.writes.load(), 0); QVERIFY(m_wallet->IsLocked());
    }
    void probeWaitsForAnInFlightReadAndHoldsTheNextPoll()
    {
        FundingReadProbe probe; probe.live_ms = 2000; probe.watch_object = WatchObject("timeout", true, true); probe.hold_watch = true;
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        panel.m_refresh_phase = B3FlowMeshTradingPanel::RefreshPhase::None; panel.m_catalog_age.start();
        Live(panel, probe);
        QCOMPARE(panel.m_watch.mode, B3FlowMeshWatch::Mode::Probing);
        // A job in flight may be reading the market: no probe beside it.
        ReadInFlight(panel);
        panel.armWatch(); QVERIFY(!panel.m_watch_thread); QCOMPARE(probe.watches.load(), 0);
        delete panel.m_thread; panel.m_thread = nullptr;
        QVERIFY(!panel.liveSkipsPoll());
        // Once it is armed, the probe holds the market read for a bounded time.
        panel.armWatch(); QVERIFY(panel.m_watch_thread);
        QVERIFY(probe.watch_entered.tryAcquire(1, 2000));
        QVERIFY(panel.m_watch.ProbeHoldsPoll(panel.m_watch_clock.elapsed()));
        QVERIFY(panel.liveSkipsPoll());
        panel.m_watch.armed_at = panel.m_watch_clock.elapsed() - B3FlowMeshWatch::Policy::PROBE_HOLD_MS;
        QVERIFY(!panel.liveSkipsPoll());
        panel.m_watch.Disable(); probe.hold_watch = false; probe.watch_release.release(); AwaitWatchWorker(panel);
        QCOMPARE(probe.snapshots.load(), 0); QCOMPARE(probe.writes.load(), 0);
    }
    void probeThatFallsBackToPollingClosesItsConnection()
    {
        // Answered without waiting, again and again (superseded): the client
        // keeps its live-update connection after such replies, so the panel
        // closes it when it gives up probing.
        FundingReadProbe probe; probe.live_ms = 2000; probe.watch_object = WatchObject("superseded", false, false);
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        Live(panel, probe);
        QCOMPARE(panel.m_watch.mode, B3FlowMeshWatch::Mode::Probing);
        for (unsigned i{0}; i < B3FlowMeshWatch::Policy::NOT_WAITED_LIMIT; ++i) {
            // Pacing is covered by the policy tests.
            panel.m_watch.retry_at = 0; panel.m_watch.tokens = B3FlowMeshWatch::Policy::ARM_BURST;
            panel.armWatch(); QVERIFY(panel.m_watch_thread); AwaitWatchWorker(panel);
        }
        QCOMPARE(panel.m_watch.mode, B3FlowMeshWatch::Mode::Polling);
        QVERIFY(panel.m_watch_release_pending);
        QCOMPARE(probe.releases.load(), 0);
        // The next tick sends the release read (watch_ms 0) once.
        panel.armWatch(/*release=*/true); AwaitWatchWorker(panel);
        QCOMPARE(probe.releases.load(), 1);
        QVERIFY(!panel.m_watch_release_pending);
        QCOMPARE(probe.watches.load(), int(B3FlowMeshWatch::Policy::NOT_WAITED_LIMIT)); QCOMPARE(probe.writes.load(), 0);
    }
    void liveUpdatesRearmWhenAllowedNotOnTheNextTick()
    {
        // Replies that return at once (a busy endpoint): after the burst of
        // two, each read starts when the policy's token refills, on the
        // panel's precise re-arm, with no tick at all; never faster than a
        // burst of two and then one per 500 ms.
        FundingReadProbe probe; probe.live_ms = 2000; probe.watch_object = WatchObject("timeout", true, true);
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        Live(panel, probe);
        QVERIFY(!panel.m_timer->isActive());
        QCOMPARE(panel.m_rearm->timerType(), Qt::PreciseTimer);
        panel.armWatch(); QVERIFY(panel.m_watch_thread);
        // Six reads with the tick stopped throughout: all but the burst of two
        // were started by the precise re-arm.
        QTRY_VERIFY_WITH_TIMEOUT(probe.watches.load() >= 6, 10000);
        QVERIFY(!panel.m_timer->isActive());
        panel.m_watch.Disable(); AwaitWatchWorker(panel);
        QVERIFY(!panel.m_rearm->isActive() || panel.m_watch.NextArmIn(panel.m_watch_clock.elapsed()) < 0);
        std::vector<int64_t> times;
        { std::lock_guard lock{probe.watch_mutex}; times = probe.watch_times; }
        QVERIFY(times.size() >= 6);
        // Measured where each read enters its RPC on a fresh worker thread:
        // to the millisecond only in strict mode, else less 100 ms of thread
        // start jitter (reads back to back, or a faster pacing, still fail).
        const int64_t slack{UiTimingStrict() ? 1 : 100};
        for (size_t i{2}; i < times.size(); ++i) QVERIFY2(times[i] - times[i - 2] >= B3FlowMeshWatch::Policy::ARM_REFILL_MS - slack, "a burst of two, then one per 500 ms");
        // Each refill is used when it arrives, not up to a tick later.
        if (UiTimingStrict()) {
            for (size_t i{3}; i < times.size(); ++i) QVERIFY2(times[i] - times[i - 1] < B3FlowMeshWatch::Policy::ARM_REFILL_MS + 150, "re-armed when allowed");
        }
        QCOMPARE(panel.m_watch.last, B3FlowMeshWatch::Outcome::Quiet);
        QCOMPARE(probe.writes.load(), 0); QCOMPARE(probe.snapshots.load(), 0);
    }
    void localEngineLiveUpdatesStayAtTwoPerSecond()
    {
        // The local engine names no endpoint and is read as the local engine,
        // but at the remote pacing: a burst of two, then one per 500 ms. Ten
        // a second is held back while each read copies the market under the
        // node's market mutex, which the FlowMesh worker's tick also takes
        // (review of 2026-09-30, C7). A reply that names an endpoint (a
        // remote endpoint after all) is classified remote at once.
        using B3FlowMeshWatch::Policy;
        FundingReadProbe probe;
        probe.data = Data(); probe.watch_data = Data();
        UniValue local_watch{UniValue::VOBJ};
        local_watch.pushKV("status", "timeout"); local_watch.pushKV("supported", true); local_watch.pushKV("waited", true);
        local_watch.pushKV("gap", false); local_watch.pushKV("endpoint", ""); local_watch.pushKV("endpoint_live_ms", 0);
        probe.watch_object = local_watch;
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(Data()));
        panel.show(); QCoreApplication::processEvents();
        UniValue info{UniValue::VOBJ}; info.pushKV("backend", "local"); info.pushKV("engine_enabled", true); info.pushKV("endpoints", UniValue{UniValue::VARR});
        panel.m_client_info = info;
        panel.updateWatchState();
        QVERIFY(panel.m_watch.local); QCOMPARE(panel.m_watch.RefillMs(), Policy::ARM_REFILL_MS);
        // On the panel's own policy and clock (no timing): after the burst of
        // two, the next read is due one whole 500 ms refill later.
        {
            auto policy{panel.m_watch};
            const auto now{panel.m_watch_clock.elapsed()};
            for (int i{0}; i < Policy::ARM_BURST; ++i) {
                QVERIFY(policy.MayArm(now)); policy.Armed(now); policy.Finished(now, B3FlowMeshWatch::Outcome::Changed, 0.0);
            }
            QVERIFY(!policy.MayArm(now));
            QCOMPARE(policy.NextArmIn(now), Policy::ARM_REFILL_MS);
            QVERIFY(!policy.MayArm(now + Policy::ARM_REFILL_MS - 1));
            QVERIFY(policy.MayArm(now + Policy::ARM_REFILL_MS));
        }
        // A reply that names an endpoint: classified remote from then on.
        auto remote_watch{local_watch}; remote_watch.pushKV("endpoint", "https://operator.invalid:18443"); remote_watch.pushKV("endpoint_live_ms", 2000);
        probe.watch_object = remote_watch;
        panel.armWatch(); QVERIFY(panel.m_watch_thread);
        QTRY_VERIFY_WITH_TIMEOUT(!panel.m_watch.local, 5000);
        QCOMPARE(panel.m_watch.RefillMs(), Policy::ARM_REFILL_MS);
        panel.m_watch.Disable(); AwaitWatchWorker(panel);
        QCOMPARE(probe.writes.load(), 0); QCOMPARE(probe.snapshots.load(), 0);
    }
    void localBookFollowsLiveUpdatesDuringAReviewButGatesDoNot()
    {
        // An order review is open (nothing in flight yet): with the local
        // engine the book, tape and own-order rows keep following live
        // updates, for display only. The gates, balances, freshness and the
        // catalog row stay on the data of before the review.
        FundingReadProbe probe; probe.data = Data(); probe.watch_data = NewerLocalData(); probe.watch_object = LocalWatchObject();
        probe.hold_watch = true;
        B3FlowMeshTradingPanel panel; panel.resize(1320, 940); AttachOfflineWallet(panel);
        Observe(panel, WithLevels(Parse(Data())), false);
        LocalLive(panel);
        QVERIFY(panel.m_watch.local);
        const auto gate_head{panel.m_snapshot->head};
        const auto observed{panel.m_response_age.msecsSinceReference()};
        const auto row{panel.m_market_data[0]};
        const auto own_before{panel.m_own_view->item(0, 2)->text()};
        panel.m_busy = true; panel.updateControls(); // A review is open.
        QVERIFY(panel.liveWhileBusy());
        panel.armWatch(); QVERIFY(panel.m_watch_thread);
        QVERIFY(probe.watch_entered.tryAcquire(1, 2000));
        // Read after the handler signalled entry (its options were recorded first).
        QCOMPARE(QString::fromStdString(probe.market_options.back()["known_head"].get_str()), gate_head);
        probe.watch_release.release();
        QTRY_VERIFY_WITH_TIMEOUT(panel.m_display.has_value(), 3000);
        const auto shown_head{QString::fromStdString(H(10).GetHex())};
        // Shown: the book, the own-order rows.
        QCOMPARE(panel.displayedSnapshot()->head, shown_head);
        QCOMPARE(panel.m_order_book->head(), shown_head);
        QVERIFY(panel.m_own_view->item(0, 2)->text() != own_before);
        // Not applied: the gates' snapshot, its freshness, the catalog row.
        QCOMPARE(panel.m_snapshot->head, gate_head);
        QCOMPARE(panel.m_response_age.msecsSinceReference(), observed);
        QCOMPARE(panel.m_market_data[0].sequence, row.sequence);
        QCOMPARE(panel.m_market_data[0].b3_reserved, row.b3_reserved);
        QVERIFY(panel.m_busy);
        // The views' stale label follows what they show: confirmed by the
        // live update, while the gates' own data has aged out.
        auto* stale{panel.m_order_book->findChild<QLabel*>(QStringLiteral("flowMeshBookStale"))};
        panel.m_response_age.invalidate(); panel.updateControls();
        QVERIFY(stale->isHidden()); QVERIFY(!panel.dataReady());
        panel.m_busy = false; panel.updateControls(); QVERIFY(!stale->isHidden());
        panel.m_busy = true; panel.m_response_age.start(); panel.updateControls();
        // The next display read follows the shown head.
        QVERIFY(probe.watch_entered.tryAcquire(1, 2000));
        QCOMPARE(QString::fromStdString(probe.market_options.back()["known_head"].get_str()), shown_head);
        // A book click while busy is ignored; once idle, the book shows
        // newer data than the gates, so a click is refused with a notice.
        panel.m_watch.Disable(); probe.hold_watch = false; probe.watch_release.release(); AwaitWatchWorker(panel);
        panel.m_busy = false; panel.updateControls();
        const int spread{panel.m_order_book->model()->spreadRow()};
        panel.m_price->setText(QStringLiteral("7"));
        panel.m_order_book->activateRow(spread + 1);
        QCOMPARE(panel.m_price->text(), QStringLiteral("7"));
        QVERIFY(panel.m_log->toPlainText().contains(QStringLiteral("its price was not used")));
        // Idle: a live update is armed against the gates' head, and its
        // reply is applied to them; the display is then dropped.
        probe.hold_watch = true;
        panel.updateWatchState(); panel.m_watch.mode = B3FlowMeshWatch::Mode::Live; panel.m_watch.retry_at = 0; panel.m_watch.tokens = B3FlowMeshWatch::Policy::ARM_BURST;
        panel.armWatch(); QVERIFY(panel.m_watch_thread);
        QVERIFY(probe.watch_entered.tryAcquire(1, 2000));
        QCOMPARE(QString::fromStdString(probe.market_options.back()["known_head"].get_str()), gate_head);
        probe.watch_release.release();
        QTRY_COMPARE_WITH_TIMEOUT(panel.m_snapshot->head, shown_head, 3000);
        QVERIFY(!panel.m_display);
        QCOMPARE(panel.m_market_data[0].sequence, uint64_t{9});
        panel.m_watch.Disable(); AwaitWatchWorker(panel);
        QCOMPARE(probe.writes.load(), 0); QVERIFY(m_wallet->IsLocked());
    }
    void localBookFreezesWhileTheOrderIsSent()
    {
        // Review of 2026-09-30: display-only live reads run while a review is
        // open, never while the user's own order is in flight. A local read
        // takes the node's market mutex, which the submission also needs and
        // nothing can preempt: no display read is armed then, and one armed
        // during the review that returns while the order is sent is dropped.
        FundingReadProbe probe; probe.data = Data(); probe.watch_data = NewerLocalData(); probe.watch_object = LocalWatchObject();
        probe.hold_watch = true;
        B3FlowMeshTradingPanel panel; panel.resize(1320, 940); AttachOfflineWallet(panel);
        Observe(panel, WithLevels(Parse(Data())), false);
        LocalLive(panel);
        const auto gate_head{panel.m_snapshot->head};
        const auto book_head{panel.m_order_book->head()};
        QVERIFY(!book_head.isEmpty());
        panel.m_busy = true; panel.updateControls(); // A review is open.
        QVERIFY(panel.liveWhileBusy());
        panel.armWatch(); QVERIFY(panel.m_watch_thread);
        QVERIFY(probe.watch_entered.tryAcquire(1, 2000));
        // Approved: the order is being sent. The in-flight job as startJob
        // leaves it (a never-started owned thread; nothing runs or writes).
        auto sending{std::make_shared<B3FlowMeshTradingPanel::Result>()};
        B3FlowMeshTrading::Action action;
        action.operation = B3FlowMeshTrading::Operation::Order; action.market = panel.m_market_data.front();
        action.side = QStringLiteral("bid"); action.price = 1000; action.amount = 1;
        sending->action = action; sending->wallet = panel.m_wallet_name;
        panel.m_active_result = sending;
        panel.m_thread = new QThread{&panel};
        QVERIFY(!panel.liveWhileBusy());
        // The display read armed during the review returns now: dropped.
        probe.hold_watch = false; probe.watch_release.release(); AwaitWatchWorker(panel);
        QVERIFY(!panel.m_display);
        QCOMPARE(panel.displayedSnapshot()->head, gate_head);
        QCOMPARE(panel.m_order_book->head(), book_head);
        // No other is armed while it is in flight: directly, by the precise
        // re-arm or by the tick.
        panel.m_watch.retry_at = 0; panel.m_watch.tokens = B3FlowMeshWatch::Policy::ARM_BURST;
        panel.armWatch(); QVERIFY(!panel.m_watch_thread);
        panel.rearmWatch(); QVERIFY(!panel.m_watch_thread);
        panel.tick(); QVERIFY(!panel.m_watch_thread);
        QCOMPARE(probe.watches.load(), 1);
        QCOMPARE(panel.m_snapshot->head, gate_head);
        delete panel.m_thread; panel.m_thread = nullptr; panel.m_active_result.reset();
        panel.m_busy = false; panel.m_watch.Disable(); panel.updateControls();
        QCOMPARE(probe.writes.load(), 0); QVERIFY(m_wallet->IsLocked());
    }
    void remoteBookStaysFrozenDuringAReview()
    {
        // A remote client's reads hold its work gate: no display read may
        // delay the order being sent, so nothing is armed while busy.
        FundingReadProbe probe; probe.live_ms = 2000; probe.watch_data = NewerRemoteData(); probe.watch_object = WatchObject("head_changed", true, true);
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        Live(panel, probe);
        panel.m_watch.mode = B3FlowMeshWatch::Mode::Live; panel.m_watch.last_ok_at = panel.m_watch_clock.elapsed();
        panel.m_busy = true;
        QVERIFY(!panel.liveWhileBusy());
        panel.armWatch(); QVERIFY(!panel.m_watch_thread);
        QCOMPARE(probe.watches.load(), 0);
        panel.m_busy = false;
    }
    //! NewerLocalData() as a remote endpoint serves it (head H(10), sequence
    //! 3, a new bid at 998), with its tick and the endpoint's times of its
    //! history and tick. `unchanged`: the reply to a read that already held
    //! that head, as the remote client builds it: status, and the endpoint's
    //! time of the head only (no new tick, no book).
    static UniValue NewerRemoteBook(bool unchanged = false)
    {
        auto data{NewerLocalData()}; data.pushKV("verification", RemoteData()["verification"]);
        std::vector<UniValue> ticks, rows;
        if (!unchanged) { ticks.push_back(TickJson(3, H(10), true, "bid")); rows.push_back(StampJson(2, H(4), "live", OBSERVED_T0 + 61'000)); }
        rows.push_back(StampJson(3, H(10), "live", OBSERVED_T0 + 62'000));
        data.pushKV("clearing_ticks", TickPageJson(ticks));
        data.pushKV("observed_time", ObservedJson(rows));
        if (unchanged) data.pushKV("unchanged", true);
        return data;
    }
    void remotePollingSameHeadRepliesKeepTheViews()
    {
        // The owner's remote setup against an endpoint without live updates
        // (-flowmeshapiwatch is off by default): the panel polls every 0.5 s,
        // and a same-head poll is answered unchanged, with the endpoint's time
        // of the head only. After the head's own reply those replies neither
        // rebuild the views nor repaint the book, and every time stays shown.
        FundingReadProbe probe; probe.live_ms = 0; probe.data = RemoteDataWithTimes();
        probe.candles_response = CandlesJson("1m", 60'000, OBSERVED_T0 + 120'000, 300,
            {CandleJson(OBSERVED_T0, 1, 1, 1000, 1000, 1000, 1000, 250'000), CandleJson(OBSERVED_T0 + 60'000, 2, 2, 1000, 1000, 1000, 1000, 250'000)});
        B3FlowMeshTradingPanel panel; panel.resize(1320, 940); AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        panel.m_candle_interval = flowmesh::CandleInterval::M1;
        Live(panel, probe);
        QCOMPARE(panel.m_watch.mode, B3FlowMeshWatch::Mode::Polling);
        // The head's own reply, then (next job) the candles window.
        for (int i{0}; i < 2; ++i) { panel.startJob(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 5000); }
        QVERIFY(panel.m_candles && panel.m_candles->available);
        const auto observed{panel.m_snapshot->observed};
        QVERIFY(observed && observed->rows.size() == 2);
        auto unchanged{RemoteDataWithTimes()}; unchanged.pushKV("unchanged", true);
        unchanged.pushKV("clearing_ticks", TickPageJson({}));
        unchanged.pushKV("observed_time", ObservedJson({StampJson(2, H(4), "live", OBSERVED_T0 + 61'000)}));
        probe.data = unchanged;
        QCoreApplication::processEvents();
        int book_calls{0};
        panel.m_order_book->setViewObserver([&](const QString&, bool) { ++book_calls; });
        const auto builds{panel.m_view_builds};
        const auto candle_reads{probe.candle_options.size()};
        const int reads{probe.snapshots.load()};
        for (int i{0}; i < 3; ++i) { panel.startJob(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 5000); QCoreApplication::processEvents(); }
        QCOMPARE(probe.snapshots.load(), reads + 3);
        QCOMPARE(probe.candle_options.size(), candle_reads);
        QVERIFY(!panel.m_read_failed);
        QCOMPARE(panel.m_view_builds, builds);
        QCOMPARE(book_calls, 0); // Neither installed nor painted.
        QCOMPARE(panel.m_snapshot->observed, observed);
        QVERIFY(panel.m_response_age.isValid() && panel.m_response_age.elapsed() < 3000);
        QCOMPARE(probe.writes.load(), 0);
    }
    void remoteLiveUpdateRepaintsTheBookAndQuietRepliesDoNot()
    {
        // A remote endpoint with live updates (-flowmeshapiwatch on): a new
        // head that arrives on the watch is installed and painted by the book
        // at once, with no tick or poll; the quiet timeouts that follow (the
        // endpoint's unchanged replies, with the head's time only) neither
        // rebuild the views nor install or paint the book.
        B3FlowMeshOrderBook::SetAnimationSetting(false); // No flash frames in the counts.
        struct RestoreAnimation { ~RestoreAnimation() { B3FlowMeshOrderBook::SetAnimationSetting(true); } } restore;
        FundingReadProbe probe; probe.live_ms = 2000;
        probe.watch_data = NewerRemoteBook(); probe.watch_object = WatchObject("ready", true, true); probe.hold_watch = true;
        B3FlowMeshTradingPanel panel; panel.resize(1320, 940); AttachOfflineWallet(panel); Observe(panel, Parse(RemoteDataWithTimes()));
        Live(panel, probe);
        panel.m_watch.mode = B3FlowMeshWatch::Mode::Live; panel.m_watch.last_ok_at = panel.m_watch_clock.elapsed();
        QVERIFY(!panel.m_timer->isActive());
        QCoreApplication::processEvents();
        const QString new_head{QString::fromStdString(H(10).GetHex())};
        std::vector<std::pair<QString, bool>> calls;
        panel.m_order_book->setViewObserver([&](const QString& head, bool painted) { calls.emplace_back(head, painted); });
        panel.armWatch(); QVERIFY(panel.m_watch_thread);
        QVERIFY(probe.watch_entered.tryAcquire(1, 5000));
        probe.watch_release.release();
        QTRY_COMPARE_WITH_TIMEOUT(panel.m_snapshot->head, new_head, 5000);
        QCOMPARE(panel.m_order_book->head(), new_head);
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(calls.begin(), calls.end(), [&](const auto& call) { return call.first == new_head && call.second; }), 5000);
        // The next watch was armed at once against the new head.
        QVERIFY(probe.watch_entered.tryAcquire(1, 5000));
        QCOMPARE(QString::fromStdString(probe.market_options.back()["known_head"].get_str()), new_head);
        probe.watch_data = NewerRemoteBook(/*unchanged=*/true); probe.watch_object = WatchObject("timeout", true, true);
        QCoreApplication::processEvents();
        calls.clear();
        const auto builds{panel.m_view_builds};
        const auto observed{panel.m_snapshot->observed};
        QVERIFY(observed && observed->rows.size() == 2);
        // That one and the next (after the pacing's refill, on the GUI
        // thread's precise re-arm) are quiet.
        probe.watch_release.release();
        bool entered{false}; // QTRY evaluates its expression once more at the end.
        QTRY_VERIFY_WITH_TIMEOUT(entered || (entered = probe.watch_entered.tryAcquire(1)), 5000);
        probe.hold_watch = false; probe.watch_release.release();
        QTRY_VERIFY_WITH_TIMEOUT(probe.watches.load() >= 4, 5000);
        panel.m_watch.Disable(); AwaitWatchWorker(panel);
        QCoreApplication::processEvents();
        QCOMPARE(panel.m_watch.last, B3FlowMeshWatch::Outcome::Quiet);
        QCOMPARE(panel.m_view_builds, builds);
        QVERIFY(calls.empty());
        QCOMPARE(panel.m_snapshot->head, new_head);
        QCOMPARE(panel.m_snapshot->observed, observed);
        QVERIFY(!panel.m_read_failed);
        QCOMPARE(probe.snapshots.load(), 0); QCOMPARE(probe.writes.load(), 0);
    }
    void ownOrderSelectionBindsToTheRowShown()
    {
        // The rows show m_display while busy: a selection binds to the
        // order shown in that row, and Edit requires exactly that order in
        // the gates' data (here, only once they have caught up).
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        Observe(panel, WithLevels(Parse(Data())), false);
        LocalLive(panel);
        const auto newer{WithLevels(Parse(NewerLocalData()))};
        panel.m_busy = true;
        QElapsedTimer age; age.start();
        QCOMPARE(panel.applyDisplayOnly(newer, age), B3FlowMeshTradingPanel::SnapshotApply::APPLIED);
        QCOMPARE(panel.m_shown_own_curves, newer.own_curves);
        panel.m_own_view->selectRow(0);
        QCOMPARE(panel.m_edit_points, newer.own_curves[0].points);
        QCOMPARE(panel.m_edit_side, newer.own_curves[0].side);
        QVERIFY(newer.own_curves[0].points != panel.m_snapshot->own_curves[0].points);
        panel.m_busy = false; panel.updateControls();
        // The gates' data still holds the replaced order: nothing to edit
        // (never the gates' order in that row index, which is not shown).
        QVERIFY(!panel.editCurve()); QVERIFY(!panel.editAvailable());
        // The gates catch up: the display is dropped, the binding holds.
        QVERIFY(panel.applySelectedSnapshot(newer, age, panel.m_snapshot->head) == B3FlowMeshTradingPanel::SnapshotApply::APPLIED);
        QVERIFY(!panel.m_display);
        QVERIFY(panel.editCurve()); QCOMPARE(panel.editCurve()->points, newer.own_curves[0].points);
    }
    void tickCatchupKeepsLiveModeAndRearmsAtOnce()
    {
        // A tick-less read moved the head: the client reads the missing
        // clearing ticks once instead of watching (ticks_catchup, waited).
        FundingReadProbe probe; probe.live_ms = 2000; probe.watch_object = WatchObject("ticks_catchup", false, true);
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        Live(panel, probe);
        panel.m_watch.mode = B3FlowMeshWatch::Mode::Live; panel.m_watch.last_ok_at = panel.m_watch_clock.elapsed();
        probe.hold_watch = true;
        panel.armWatch(); QVERIFY(probe.watch_entered.tryAcquire(1, 2000));
        probe.watch_release.release(); // Only this read returns; the next one is held.
        // Re-armed by the result itself (the timer is stopped here), not a
        // tick later, and still live with no strike.
        QTRY_COMPARE_WITH_TIMEOUT(probe.watches.load(), 2, 2000);
        QCOMPARE(panel.m_watch.last, B3FlowMeshWatch::Outcome::Refreshed);
        QCOMPARE(panel.m_watch.mode, B3FlowMeshWatch::Mode::Live);
        QCOMPARE(panel.m_watch.not_waited, 0U);
        QVERIFY(!panel.m_watch_release_pending);
        panel.m_watch.Disable(); probe.hold_watch = false; probe.watch_release.release(); AwaitWatchWorker(panel);
        QCOMPARE(probe.watches.load(), 2); QCOMPARE(probe.writes.load(), 0);
    }
    void liveModeSkipsMarketPollingButKeepsAuxiliaryCycleAndStatus()
    {
        FundingReadProbe probe; probe.live_ms = 2000; probe.checkpoint = true;
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        Live(panel, probe);
        panel.m_watch.mode = B3FlowMeshWatch::Mode::Live; panel.m_watch.last_ok_at = panel.m_watch_clock.elapsed();
        // Catalog, Status, Effects and Receipt still run, without the market read.
        panel.m_receipt = B3FlowMeshTrading::Receipt{}; panel.m_receipt->action_id = QString::fromStdString(H(42).GetHex());
        panel.m_receipt_wallet = panel.m_wallet; panel.m_pending_market = panel.m_snapshot->market; panel.m_pending_account = panel.m_snapshot->account;
        for (int phase{0}; phase < 4; ++phase) {
            panel.startJob(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        }
        QCOMPARE(probe.catalogs.load(), 1); QCOMPARE(probe.balances.load(), 1); QCOMPARE(probe.effects.load(), 1); QCOMPARE(probe.receipts.load(), 1);
        QCOMPARE(probe.snapshots.load(), 0);
        // The Status phase still refreshes checkpoint eligibility while live.
        QVERIFY(panel.market()->checkpoint_pending);
        QCOMPARE(panel.market()->checkpoint, QString::fromStdString(H(80).GetHex()));
        QCOMPARE(probe.writes.load(), 0);
    }
    void watchResultAgainstOtherHeadMarketOrWalletIsDroppedSilently()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        const auto head{panel.m_snapshot->head};
        for (int condition{0}; condition < 5; ++condition) {
            auto result{std::make_shared<B3FlowMeshTradingPanel::WatchResult>()};
            result->scope = {panel.m_wallet, panel.m_generation, panel.m_watch_generation, panel.m_snapshot->market, panel.m_snapshot->account, head};
            result->snapshot = Parse(NewerRemoteData()); result->status = QStringLiteral("ready"); result->supported = result->waited = true; result->endpoint_live_ms = 2000;
            switch (condition) {
            case 0: result->scope.known_head = QString::fromStdString(H(77).GetHex()); break;
            case 1: result->scope.market = QString::fromStdString(H(78).GetHex()); break;
            case 2: ++result->scope.generation; break;
            case 3: ++result->scope.watch_generation; break;
            case 4: panel.m_busy = true; break; // Never applied while an operation runs.
            }
            panel.m_watch.outstanding = true;
            panel.finishWatch(result);
            QCOMPARE(panel.m_snapshot->head, head);
            QVERIFY(!panel.m_read_failed);
            QVERIFY(!panel.m_watch.outstanding);
            panel.m_busy = false;
        }
    }
    void hiddenPanelArmsNoWatch()
    {
        FundingReadProbe probe; probe.live_ms = 2000; probe.watch_object = WatchObject("timeout", true, true);
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        panel.m_client_info = UniValue{UniValue::VOBJ}; // Parsed below by updateWatchState.
        Live(panel, probe); panel.hide();
        for (int i{0}; i < 3; ++i) { panel.armWatch(); QVERIFY(!panel.m_watch_thread); }
        QCOMPARE(probe.watches.load(), 0);
    }
    void walletSwitchDoesNotBlockOnOutstandingWatch()
    {
        FundingReadProbe probe; probe.live_ms = 2000; probe.watch_object = WatchObject("timeout", true, true); probe.hold_watch = true;
        auto panel{std::make_unique<B3FlowMeshTradingPanel>()}; AttachOfflineWallet(*panel); Observe(*panel, Parse(RemoteData()));
        Live(*panel, probe);
        panel->armWatch(); QVERIFY(panel->m_watch_thread);
        QVERIFY(probe.watch_entered.tryAcquire(1, 2000));
        // A wallet switch returns at once; the outstanding read is dropped later.
        QElapsedTimer switching; switching.start();
        panel->cancelAndWait();
        QVERIFY(switching.elapsed() < 1000);
        QVERIFY(panel->m_watch_thread);
        probe.watch_release.release();
        AwaitWatchWorker(*panel);
        QVERIFY(!panel->m_wallet);
        // Closing the panel joins an outstanding read (bounded by that read).
        AttachOfflineWallet(*panel); Observe(*panel, Parse(RemoteData())); Live(*panel, probe);
        panel->armWatch(); QVERIFY(panel->m_watch_thread);
        QVERIFY(probe.watch_entered.tryAcquire(1, 2000));
        std::thread release{[&] { std::this_thread::sleep_for(std::chrono::milliseconds{300}); probe.watch_release.release(); }};
        QElapsedTimer closing; closing.start();
        panel.reset();
        QVERIFY(closing.elapsed() >= 250);
        release.join();
        QCOMPARE(probe.writes.load(), 0); QVERIFY(m_wallet->IsLocked());
    }
    void watchFailureBacksOffAndReconcilesTheTrackedRequestFirst()
    {
        FundingReadProbe probe; probe.live_ms = 2000; probe.fail_watch = true; probe.watch_object = WatchObject("timeout", true, true);
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        panel.m_receipt = B3FlowMeshTrading::Receipt{}; panel.m_receipt->action_id = QString::fromStdString(H(42).GetHex());
        panel.m_receipt_wallet = panel.m_wallet; panel.m_pending_market = panel.m_snapshot->market; panel.m_pending_account = panel.m_snapshot->account;
        panel.m_refresh_phase = B3FlowMeshTradingPanel::RefreshPhase::Status; panel.m_catalog_age.start();
        Live(panel, probe);
        panel.armWatch(); AwaitWatchWorker(panel);
        QCOMPARE(probe.watches.load(), 1);
        QCOMPARE(panel.m_watch.mode, B3FlowMeshWatch::Mode::Backoff);
        QVERIFY(panel.m_watch.PollEveryTick());
        QVERIFY(panel.liveUpdateText().contains(QStringLiteral("reconnecting")));
        // The tracked request is read first, and the scheduled phase is kept.
        QVERIFY(panel.m_reconcile_pending);
        QCOMPARE(panel.nextPassivePhase(), B3FlowMeshTradingPanel::RefreshPhase::Receipt);
        QCOMPARE(panel.m_refresh_phase, B3FlowMeshTradingPanel::RefreshPhase::Status);
        // The display itself is untouched by a failed live update.
        QVERIFY(!panel.m_read_failed);
        // No new watch before its backoff.
        panel.armWatch(); QVERIFY(!panel.m_watch_thread); QCOMPARE(probe.watches.load(), 1);
    }
    void busyEndpointPollsAndNamesItsCapacity()
    {
        FundingReadProbe probe; probe.live_ms = 2000; probe.watch_object = WatchObject("busy", true, false);
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        Live(panel, probe);
        panel.armWatch(); AwaitWatchWorker(panel);
        QCOMPARE(panel.m_watch.mode, B3FlowMeshWatch::Mode::Polling);
        QVERIFY(panel.liveUpdateText().contains(QStringLiteral("capacity full")));
        QVERIFY(!panel.liveSkipsPoll());
        // The busy reply was still fresh verified data.
        QVERIFY(panel.m_response_age.isValid() && panel.m_response_age.elapsed() < 3000);
    }
    void gapMarkerAddsHistorySeparatorLogLineAndReconcileFirst()
    {
        FundingReadProbe probe; probe.live_ms = 2000;
        // A newer head whose history has a trade after the gap.
        auto newer{NewerRemoteData()};
        auto history{newer["history"]}; auto entries{history["entries"]};
        UniValue trade{entries[0]}; trade.pushKV("sequence", 3); trade.pushKV("microblock_hash", H(10).GetHex()); entries.push_back(trade);
        history.pushKV("entries", entries); newer.pushKV("history", history);
        probe.watch_data = newer; probe.watch_object = WatchObject("gap", true, true, 2000, /*gap=*/true);
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        panel.m_receipt = B3FlowMeshTrading::Receipt{}; panel.m_receipt->action_id = QString::fromStdString(H(42).GetHex());
        panel.m_receipt_wallet = panel.m_wallet; panel.m_pending_market = panel.m_snapshot->market; panel.m_pending_account = panel.m_snapshot->account;
        panel.m_refresh_phase = B3FlowMeshTradingPanel::RefreshPhase::Status; panel.m_catalog_age.start();
        Live(panel, probe);
        const auto before{panel.m_history_view->rowCount()};
        panel.m_watch.mode = B3FlowMeshWatch::Mode::Live;
        probe.hold_watch = true;
        panel.armWatch(); QVERIFY(probe.watch_entered.tryAcquire(1, 2000));
        probe.watch_release.release(); // Only the first read returns; the next one is held.
        QTRY_COMPARE_WITH_TIMEOUT(panel.m_gap_markers.size(), size_t{1}, 3000);
        QCOMPARE(panel.m_gap_markers.front().from_sequence, uint64_t{2});
        QCOMPARE(panel.m_gap_markers.front().to_sequence, uint64_t{3});
        // A log line, a separator between the trades before and after, and
        // the tracked request is reconciled before anything else.
        QVERIFY(panel.m_log->toPlainText().contains(QStringLiteral("missed events between auctions #2 and #3")));
        QVERIFY(panel.m_log->toPlainText().contains(QStringLiteral("No request was resent")));
        QCOMPARE(panel.m_history_view->rowCount(), before + 2);
        // No observed time here: a dash, the auction's number in its tooltip.
        QCOMPARE(panel.m_history_view->item(0, 0)->text(), QStringLiteral("—"));
        QVERIFY(panel.m_history_view->item(0, 0)->toolTip().contains(QStringLiteral("Auction #3")));
        QVERIFY(panel.m_history_view->item(1, 0)->text().contains(QStringLiteral("events missed here")));
        QVERIFY(panel.m_history_view->item(1, 0)->toolTip().contains(QStringLiteral("between auctions #2 and #3")));
        QCOMPARE(panel.m_history_view->item(2, 0)->text(), QStringLiteral("—"));
        QVERIFY(panel.m_history_view->item(2, 0)->toolTip().contains(QStringLiteral("Auction #2")));
        QVERIFY(!panel.m_history_view->item(2, 0)->toolTip().contains(QStringLiteral("missed")));
        QVERIFY(panel.m_reconcile_pending);
        QCOMPARE(panel.nextPassivePhase(), B3FlowMeshTradingPanel::RefreshPhase::Receipt);
        QCOMPARE(panel.m_refresh_phase, B3FlowMeshTradingPanel::RefreshPhase::Status);
        QVERIFY(panel.liveUpdateText().contains(QStringLiteral("event gap")));
        panel.m_watch.Disable(); probe.hold_watch = false; probe.watch_release.release(); AwaitWatchWorker(panel);
        // Per market: another market's view never shows them, and a market
        // switch clears them.
        panel.m_market->currentIndexChanged(0);
        QVERIFY(panel.m_gap_markers.empty());
        QVERIFY(!panel.liveUpdateText().contains(QStringLiteral("event gap")));
        QCOMPARE(probe.writes.load(), 0);
    }
    void lostSnapshotReadReconcilesTrackedActionBeforeSnapshot()
    {
        using Phase = B3FlowMeshTradingPanel::RefreshPhase;
        FundingReadProbe probe;
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        TrackUnknown(panel); panel.m_catalog_age.start(); panel.m_refresh_phase = Phase::Status;
        probe.fail = true; // The selected-market read itself fails.
        panel.startJob(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QVERIFY(panel.m_read_failed); QVERIFY(panel.m_reconcile_pending); QCOMPARE(panel.m_refresh_phase, Phase::Effects);
        // The next passive job reads the tracked request before its market
        // read, replacing (not advancing) the scheduled phase: same read count.
        probe.fail = false; probe.receipt_response = CertifiedStatus(panel.m_snapshot->market); probe.methods.clear();
        panel.startJob(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QCOMPARE(Remote(probe), (std::vector<std::string>{"getflowmeshactionstatus", "getflowmeshmarketdata"}));
        QVERIFY(!panel.m_read_failed); QVERIFY(!panel.m_reconcile_pending);
        QVERIFY(panel.m_receipt->Included()); QCOMPARE(panel.m_refresh_phase, Phase::Effects);
        // The scheduled phase runs next, and no second override follows.
        probe.methods.clear(); panel.startJob(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QCOMPARE(Remote(probe).front(), std::string{"listflowmeshvaultoperations"});
        QCOMPARE(probe.writes.load(), 0); QVERIFY(m_wallet->IsLocked());
    }
    void auxiliaryFailureDoesNotTriggerReconcileFirst()
    {
        using Phase = B3FlowMeshTradingPanel::RefreshPhase;
        FundingReadProbe probe; probe.failed_method = "getflowmeshbalance";
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        TrackUnknown(panel); panel.m_catalog_age.start(); panel.m_refresh_phase = Phase::Status;
        panel.startJob(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        // The market read itself succeeded: nothing was lost.
        QVERIFY(panel.m_read_failed); QCOMPARE(probe.snapshots.load(), 1);
        QVERIFY(!panel.m_reconcile_pending); QCOMPARE(panel.nextPassivePhase(), Phase::Effects);
        QCOMPARE(probe.writes.load(), 0);
    }
    void reconcileFirstIsOneShotAndNeverStarvesCatalog()
    {
        using Phase = B3FlowMeshTradingPanel::RefreshPhase;
        FundingReadProbe probe; probe.fail = true; // A persistent market-read failure; status reads fail too.
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        TrackUnknown(panel); panel.m_catalog_age.start(); panel.m_refresh_phase = Phase::Status;
        std::vector<std::string> first;
        const auto run = [&] {
            probe.methods.clear(); panel.startJob(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
            const auto remote{Remote(probe)}; first.push_back(remote.empty() ? std::string{} : remote.front());
        };
        run(); QVERIFY(panel.m_reconcile_pending);
        // A Connect completes: the schedule resets and discovery is due. It
        // runs at once, before any reconcile, however long the failure lasts.
        panel.resetRefreshSchedule(); panel.m_catalog_age.invalidate();
        for (int i{0}; i < 5; ++i) run();
        QCOMPARE(first, (std::vector<std::string>{"getflowmeshbalance", "listflowmeshmarkets", "getflowmeshactionstatus",
                                                  "getflowmeshbalance", "getflowmeshactionstatus", "listflowmeshvaultoperations"}));
        QVERIFY(panel.m_reconcile_pending); QVERIFY(panel.m_read_failed);
        // A status read that completes ends it, although the market read
        // still fails, and its verified inclusion is kept.
        probe.receipt_response = CertifiedStatus(panel.m_snapshot->market);
        run(); QCOMPARE(first.back(), std::string{"getflowmeshactionstatus"});
        QVERIFY(!panel.m_reconcile_pending); QVERIFY(panel.m_receipt->Included()); QVERIFY(panel.m_read_failed);
        QCOMPARE(probe.writes.load(), 0); QVERIFY(m_wallet->IsLocked());
    }
    void pendingRowsShowStagesWithoutChurn()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        auto* view{panel.findChild<QTableWidget*>(QStringLiteral("flowMeshPendingActions"))};
        QVERIFY(view); panel.updateControls(); QVERIFY(view->isHidden());
        // History already settled before this session saw it never appears.
        auto earlier{SavedBid(panel, 60, "certified_inclusion", "certified", 100)}; Certify(earlier.receipt); earlier.receipt.no_resubmit = true;
        B3FlowMeshTrading::SavedActions saved; saved.account = panel.m_snapshot->account;
        saved.actions = {SavedBid(panel, 61, "unknown", "sent"), earlier};
        panel.restoreSavedActions(saved);
        QVERIFY(!view->isHidden()); QCOMPARE(view->rowCount(), 1);
        QCOMPARE(RowText(view, 0), (QStringList{QStringLiteral("Buy 1 tUSD @ 1 B3/tUSD"), QStringLiteral("✓"), QStringLiteral("✓"), QStringLiteral("!"), QString{}, QString{},
                                                 QStringLiteral("Delivery unconfirmed · outcome unknown · never re-signed")}));
        QCOMPARE(view->item(0, 2)->data(Qt::AccessibleTextRole).toString(), QStringLiteral("Sent: done"));
        QCOMPARE(view->item(0, 3)->data(Qt::AccessibleTextRole).toString(), QStringLiteral("Admitted: unknown"));
        QVERIFY(view->item(0, 0)->toolTip().contains(saved.actions[0].receipt.action_id));
        // Identical updates keep the model and its items.
        QSignalSpy reset{view->model(), &QAbstractItemModel::modelReset};
        const auto* first{view->item(0, 0)};
        for (int i{0}; i < 3; ++i) { panel.updateControls(); panel.observeRequests(); }
        QCOMPARE(view->item(0, 0), first); QCOMPARE(reset.count(), 0); QCOMPARE(view->rowCount(), 1);
        // Verified inclusion certifies it; the row lingers once it settles.
        auto certified{saved.actions[0].receipt}; Certify(certified);
        panel.applyReceipt(certified);
        QCOMPARE(RowText(view, 0).mid(1, 4), (QStringList{QStringLiteral("✓"), QStringLiteral("✓"), QStringLiteral("✓"), QStringLiteral("✓")}));
        QCOMPARE(view->item(0, 5)->text(), QStringLiteral("not reported"));
        QVERIFY(view->item(0, 6)->text().contains(QStringLiteral("applied or rejected is not reported")));
        QCOMPARE(view->item(0, 6)->foreground().color(), B3Theme::kPositive);
        QCOMPARE(view->item(0, 0), first);
        // After the linger interval it leaves the view (Saved requests keeps it).
        for (auto& [key, seen] : panel.m_request_seen) seen.settled_age.invalidate();
        panel.updateControls(); QVERIFY(view->isHidden()); QCOMPARE(view->rowCount(), 0);
        QVERIFY(!panel.m_thread); QVERIFY(m_wallet->IsLocked());
    }
    void pendingRowsKeepHighWaterButNeverCertifyWithoutProof()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        auto* view{panel.findChild<QTableWidget*>(QStringLiteral("flowMeshPendingActions"))};
        B3FlowMeshTrading::SavedActions saved; saved.account = panel.m_snapshot->account;
        saved.actions = {SavedBid(panel, 62, "admitted", "admitted")};
        panel.restoreSavedActions(saved);
        QCOMPARE(RowText(view, 0).mid(1, 5), (QStringList{QStringLiteral("✓"), QStringLiteral("✓"), QStringLiteral("✓"), QStringLiteral("…"), QString{}}));
        QVERIFY(view->item(0, 6)->text().contains(QStringLiteral("unverified")));
        // A later status read that cannot confirm it keeps "was sent and
        // admitted", shows the unknown, and never claims certification.
        auto later{saved.actions[0].receipt}; later.state = QStringLiteral("unknown"); later.stage = QStringLiteral("certified");
        panel.applyReceipt(later);
        QCOMPARE(RowText(view, 0).mid(1, 5), (QStringList{QStringLiteral("✓"), QStringLiteral("✓"), QStringLiteral("✓"), QStringLiteral("!"), QString{}}));
        QVERIFY(view->item(0, 6)->text().contains(QStringLiteral("outcome unknown")));
        QCOMPARE(view->item(0, 6)->foreground().color(), B3Theme::kAccent);
        // A reported refusal is never terminal and never "not executed".
        later.state = QStringLiteral("rejected"); later.stage = QStringLiteral("refused");
        panel.applyReceipt(later);
        QVERIFY(view->item(0, 6)->text().startsWith(QStringLiteral("Refusal reported")));
        QVERIFY(!view->item(0, 5)->text().contains(QStringLiteral("not executed")));
        QCOMPARE(view->rowCount(), 1); QVERIFY(!panel.m_thread); QVERIFY(m_wallet->IsLocked());
    }
    void pendingRowsShowTheInFlightRequestFromTheGuiThreadOnly()
    {
        FundingReadProbe probe;
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        auto* view{panel.findChild<QTableWidget*>(QStringLiteral("flowMeshPendingActions"))};
        B3FlowMeshTrading::Action action; action.operation = B3FlowMeshTrading::Operation::Order; action.market = panel.m_market_data.front();
        action.side = QStringLiteral("bid"); action.price = 1000; action.amount = 1'000'000;
        action.display_decimals = 6; action.display_ticker = QStringLiteral("tUSD");
        // The regtest sync check fails this job before any write.
        panel.startJob(action); QVERIFY(panel.m_thread);
        QCOMPARE(RowText(view, 0).first(), QStringLiteral("Buy 1 tUSD @ 1 B3/tUSD"));
        QCOMPARE(view->item(0, 6)->text(), QStringLiteral("Checking, signing and sending…"));
        QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QVERIFY(panel.m_inflight_title.isEmpty()); QVERIFY(view->isHidden());
        QVERIFY(!panel.m_uncertain); QCOMPARE(probe.writes.load(), 0); QVERIFY(m_wallet->IsLocked());
    }
    void toastsAreDeduplicatedBoundedAndExpire()
    {
        using Kind = B3FlowMeshToasts::Kind;
        using std::chrono::milliseconds;
        QWidget host; auto* toasts{new B3FlowMeshToasts{&host}};
        host.show(); QCoreApplication::processEvents();
        QVERIFY(toasts->isHidden());
        QVERIFY(toasts->push(QStringLiteral("a"), Kind::Info, QStringLiteral("first"), milliseconds{60'000}));
        QVERIFY(!toasts->push(QStringLiteral("a"), Kind::Info, QStringLiteral("again"), milliseconds{60'000})); // Once per key.
        QVERIFY(toasts->push(QStringLiteral("b"), Kind::Success, QStringLiteral("second"), milliseconds{60'000}));
        QVERIFY(toasts->push(QStringLiteral("c"), Kind::Warning, QStringLiteral("third"), milliseconds{60'000}));
        QVERIFY(toasts->push(QStringLiteral("d"), Kind::Error, QStringLiteral("fourth"), milliseconds{60'000}));
        QCOMPARE(toasts->texts(), (QStringList{QStringLiteral("second"), QStringLiteral("third"), QStringLiteral("fourth")})); // At most three.
        QVERIFY(!toasts->isHidden());
        // Each expires on its own; a dismissed or expired one never returns.
        QVERIFY(toasts->push(QStringLiteral("e"), Kind::Info, QStringLiteral("brief"), milliseconds{20}));
        QTRY_VERIFY_WITH_TIMEOUT(!toasts->texts().contains(QStringLiteral("brief")), 2000);
        auto* dismiss{toasts->findChild<QAbstractButton*>()}; QVERIFY(dismiss); dismiss->click();
        QTRY_COMPARE_WITH_TIMEOUT(toasts->texts().size(), 1, 2000);
        toasts->clear(); QVERIFY(toasts->texts().isEmpty()); QVERIFY(toasts->isHidden());
        QVERIFY(toasts->push(QStringLiteral("a"), Kind::Info, QStringLiteral("after a wallet switch"), milliseconds{60'000}));
        // Keys are FIFO-bounded.
        for (size_t i{0}; i < B3FlowMeshToasts::MAX_KEYS; ++i) toasts->push(QStringLiteral("k%1").arg(i), Kind::Info, QStringLiteral("n"), milliseconds{60'000});
        QVERIFY(toasts->push(QStringLiteral("a"), Kind::Info, QStringLiteral("evicted key"), milliseconds{60'000}));
    }
    void toastsOverlayTheVisiblePageWithoutMovingTheTicket()
    {
        using Kind = B3FlowMeshToasts::Kind;
        using std::chrono::milliseconds;
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        panel.resize(1100, 520); panel.show(); QCoreApplication::processEvents();
        auto* scroll{panel.findChild<QScrollArea*>()}; QVERIFY(scroll);
        auto* viewport{scroll->viewport()}; auto* strip{panel.m_toasts};
        const auto at = [&](const QWidget* widget) { return QRect{widget->mapTo(&panel, QPoint{}), widget->size()}; };
        const std::vector<const QWidget*> page{panel.m_buy, panel.m_price, panel.m_quantity, panel.m_order, panel.m_chart, panel.m_activity, panel.m_edit_order};
        std::vector<QRect> before; for (const auto* widget : page) before.push_back(at(widget));
        const auto unmoved = [&] {
            for (size_t i{0}; i < page.size(); ++i) if (at(page[i]) != before[i]) return false;
            return true;
        };
        // Whole, inside the visible page, on top of the content under it and,
        // at these widths, clear of the order button.
        const auto overlays = [&] {
            if (!strip->isVisible() || !viewport->rect().contains(strip->geometry())) return false;
            if (strip->geometry().intersects(QRect{panel.m_order->mapTo(viewport, QPoint{}), panel.m_order->size()})) return false;
            for (const auto* frame : strip->findChildren<QFrame*>(QString{}, Qt::FindDirectChildrenOnly)) {
                if (!strip->rect().contains(frame->geometry())) return false;
            }
            const auto* hit{viewport->childAt(strip->geometry().center())};
            return hit && (hit == strip || strip->isAncestorOf(hit));
        };
        panel.toast(QStringLiteral("a"), Kind::Info, QStringLiteral("Certified: Buy 1 tUSD @ 1 B3/tUSD · included in auction #9"), milliseconds{60'000});
        panel.toast(QStringLiteral("b"), Kind::Warning, QStringLiteral("Connection lost · last verified data is shown and new orders are paused. Your requests are re-checked first when it returns."), milliseconds{50});
        QCoreApplication::processEvents();
        QCOMPARE(strip->texts().size(), 2);
        QVERIFY(unmoved()); QVERIFY(overlays());
        QTRY_COMPARE_WITH_TIMEOUT(strip->texts().size(), 1, 2000);
        QCoreApplication::processEvents();
        QVERIFY(unmoved()); QVERIFY(overlays());
        // Scrolled to the end of the page, a notice is still in view.
        auto* bar{scroll->verticalScrollBar()}; QVERIFY(bar->maximum() > 0);
        bar->setValue(bar->maximum()); QCoreApplication::processEvents();
        panel.toast(QStringLiteral("c"), Kind::Success, QStringLiteral("Executed: Buy 1 tUSD @ 1 B3/tUSD · filled 0.25 tUSD"), milliseconds{60'000});
        QCoreApplication::processEvents(); QVERIFY(overlays());
        // It follows a resized page.
        panel.resize(760, 480); QCoreApplication::processEvents(); QVERIFY(overlays());
        strip->clear(); QVERIFY(strip->isHidden());
        QVERIFY(!panel.m_thread); QVERIFY(m_wallet->IsLocked());
    }
    void terminalToastsFireOncePerActionAndNotForHistoricalRows()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        panel.show(); QCoreApplication::processEvents();
        // History at attach is only seeded: settled, uncertain or refused.
        auto earlier{SavedBid(panel, 70, "certified_inclusion", "certified", 100)}; Certify(earlier.receipt); earlier.receipt.no_resubmit = true;
        auto refused{SavedBid(panel, 72, "rejected", "refused", 90)};
        B3FlowMeshTrading::SavedActions saved; saved.account = panel.m_snapshot->account; saved.actions = {earlier, refused};
        panel.restoreSavedActions(saved);
        QVERIFY(panel.m_toasts->texts().isEmpty());
        // A new request certifies: one notice, however often it is read.
        saved.actions.insert(saved.actions.begin(), SavedBid(panel, 74, "queued", "queued", 300));
        panel.restoreSavedActions(saved);
        QVERIFY(panel.m_toasts->texts().isEmpty());
        auto receipt{saved.actions[0].receipt}; Certify(receipt, 9);
        for (int i{0}; i < 3; ++i) panel.applyReceipt(receipt);
        QCOMPARE(panel.m_toasts->texts(), (QStringList{QStringLiteral("Certified: Buy 1 tUSD @ 1 B3/tUSD · included in auction #9")}));
        QVERIFY(panel.m_log->toPlainText().contains(QStringLiteral("Certified: Buy 1 tUSD @ 1 B3/tUSD")));
        // A verified outcome of the same request is announced once more.
        B3FlowMeshTrading::Outcome outcome; outcome.status = QStringLiteral("applied"); outcome.final = true; outcome.reason_code = QStringLiteral("APPLIED");
        outcome.microblock_sequence = 9; outcome.microblock_hash = receipt.microblock_hash;
        outcome.fill = B3FlowMeshTrading::Outcome::Fill{QStringLiteral("bid"), true, false, 250'000, 1000, 250'000'000, 25'000, 249'975'000};
        receipt.outcome = outcome; receipt.outcome_verified = true; receipt.stage = QStringLiteral("outcome_verified");
        panel.applyReceipt(receipt); panel.applyReceipt(receipt);
        QCOMPARE(panel.m_toasts->texts().size(), 2);
        QCOMPARE(panel.m_toasts->texts().back(), QStringLiteral("Executed: Buy 1 tUSD @ 1 B3/tUSD · filled 0.25 tUSD"));
        // An unknown outcome is a warning that says it is never re-signed.
        saved.actions[0].receipt = receipt; saved.actions[0].receipt.no_resubmit = true; // The outbox now retains its certification.
        saved.actions.insert(saved.actions.begin(), SavedBid(panel, 76, "unknown", "sent", 400));
        panel.restoreSavedActions(saved);
        QCOMPARE(panel.m_toasts->texts().size(), 3);
        QVERIFY(panel.m_toasts->texts().back().startsWith(QStringLiteral("Outcome unknown: Buy 1 tUSD @ 1 B3/tUSD")));
        QVERIFY(panel.m_toasts->texts().back().contains(QStringLiteral("never re-signed")));
        QVERIFY(!panel.m_thread); QVERIFY(m_wallet->IsLocked());
    }
    void notFinalOutcomeStaysInProgressWithoutNotExecutedNotice()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        panel.show(); QCoreApplication::processEvents();
        auto* view{panel.findChild<QTableWidget*>(QStringLiteral("flowMeshPendingActions"))};
        B3FlowMeshTrading::SavedActions saved; saved.account = panel.m_snapshot->account; saved.actions = {SavedBid(panel, 90, "queued", "queued")};
        panel.restoreSavedActions(saved);
        QVERIFY(panel.m_toasts->texts().isEmpty());
        // Verified inclusion first; then its outcome: included, but its
        // sequence was not next. The same bytes may still execute later, so a
        // new order could execute twice.
        auto receipt{saved.actions[0].receipt}; Certify(receipt, 9);
        panel.applyReceipt(receipt);
        QCOMPARE(view->item(0, 5)->text(), QStringLiteral("not reported"));
        B3FlowMeshTrading::Outcome outcome; outcome.status = QStringLiteral("included_not_executed"); outcome.final = false;
        outcome.reason_code = QStringLiteral("INCLUDED_BAD_SEQUENCE"); outcome.reason_text = QStringLiteral("Included but not executed: its sequence was not next.");
        outcome.microblock_sequence = 9; outcome.microblock_hash = receipt.microblock_hash;
        receipt.outcome = outcome; receipt.outcome_verified = true; receipt.stage = QStringLiteral("outcome_verified");
        panel.applyReceipt(receipt); panel.applyReceipt(receipt);
        QCOMPARE(RowText(view, 0).mid(1, 5), (QStringList{QStringLiteral("✓"), QStringLiteral("✓"), QStringLiteral("✓"), QStringLiteral("✓"), QStringLiteral("not executed yet · may still execute")}));
        QVERIFY(view->item(0, 6)->text().contains(QStringLiteral("may still execute")));
        QCOMPARE(view->item(0, 6)->foreground().color(), B3Theme::kAccent);
        // Only the verified inclusion is announced, never "not executed".
        QCOMPARE(panel.m_toasts->texts(), (QStringList{QStringLiteral("Certified: Buy 1 tUSD @ 1 B3/tUSD · included in auction #9")}));
        // It is not settled: it stays in progress after the linger interval.
        for (auto& [key, seen] : panel.m_request_seen) seen.settled_age.invalidate();
        panel.m_last_settled.invalidate(); panel.updateControls();
        QVERIFY(!view->isHidden()); QCOMPARE(view->rowCount(), 1);
        // A proven final outcome settles it: announced once, then it lingers
        // afresh, although its inclusion settled it before.
        receipt.state = QStringLiteral("rejected"); receipt.stage = QStringLiteral("not_executed"); receipt.certificate_verified = false;
        receipt.microblock_hash.clear(); receipt.microblock_sequence = 0;
        outcome.status = QStringLiteral("not_executed"); outcome.final = true; outcome.reason_code = QStringLiteral("NOT_EXECUTED_SEQUENCE_CONSUMED");
        outcome.consumed_by = QString::fromStdString(H(92).GetHex()); outcome.consumed_in_sequence = 10;
        receipt.outcome = outcome;
        panel.applyReceipt(receipt); panel.applyReceipt(receipt);
        QCOMPARE(view->item(0, 5)->text(), QStringLiteral("✕ not executed"));
        QCOMPARE(view->item(0, 6)->foreground().color(), B3Theme::kNegative);
        QCOMPARE(panel.m_toasts->texts().size(), 2);
        QVERIFY(panel.m_toasts->texts().back().startsWith(QStringLiteral("Buy 1 tUSD @ 1 B3/tUSD · Not executed (")));
        panel.updateControls(); QVERIFY(!view->isHidden());
        for (auto& [key, seen] : panel.m_request_seen) seen.settled_age.invalidate();
        panel.updateControls(); QVERIFY(view->isHidden());
        QVERIFY(!panel.m_thread); QVERIFY(m_wallet->IsLocked());
    }
    void noToastsDuringTeardownOrWalletSwitch()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        B3FlowMeshTrading::SavedActions saved; saved.account = panel.m_snapshot->account; saved.actions = {SavedBid(panel, 80, "queued", "queued")};
        panel.restoreSavedActions(saved);
        // A certified receipt delivered while the panel tears down is kept
        // (as today) but never announced.
        auto receipt{saved.actions[0].receipt}; Certify(receipt);
        panel.m_cancel->store(true);
        panel.applyReceipt(receipt);
        QVERIFY(panel.m_toasts->texts().isEmpty()); QVERIFY(panel.m_receipt->Included());
        // A wallet switch clears notices and seeds the next wallet again.
        panel.m_cancel->store(false);
        panel.m_toasts->push(QStringLiteral("old"), B3FlowMeshToasts::Kind::Info, QStringLiteral("old wallet"), std::chrono::milliseconds{60'000});
        panel.setWalletModel(nullptr);
        QVERIFY(panel.m_toasts->texts().isEmpty()); QVERIFY(!panel.m_requests_seeded); QVERIFY(panel.m_request_seen.empty());
        QVERIFY(m_wallet->IsLocked());
    }
    void linkToastsFollowLostAndRestoredReadsAtMostOncePer30Seconds()
    {
        FundingReadProbe probe;
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        panel.m_catalog_age.start(); panel.m_refresh_phase = B3FlowMeshTradingPanel::RefreshPhase::None;
        const auto run = [&] { panel.startJob(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000); };
        probe.fail = true; run(); run();
        QCOMPARE(panel.m_toasts->texts().size(), 1);
        QVERIFY(panel.m_toasts->texts().front().startsWith(QStringLiteral("Connection lost")));
        probe.fail = false; run();
        QCOMPARE(panel.m_toasts->texts().size(), 2);
        QVERIFY(panel.m_toasts->texts().back().startsWith(QStringLiteral("Connection restored")));
        // A second loss within 30 s is not announced again, nor its recovery.
        probe.fail = true; run(); probe.fail = false; run();
        QCOMPARE(panel.m_toasts->texts().size(), 2);
        QCOMPARE(probe.writes.load(), 0); QVERIFY(m_wallet->IsLocked());
    }
    void marketableModeFillsExactPriceButNeverSignsOrUnlocks()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        auto* marketable{panel.findChild<QCheckBox*>(QStringLiteral("flowMeshMarketable"))};
        auto* slippage{panel.findChild<QLineEdit*>(QStringLiteral("flowMeshMaxSlippage"))};
        auto* note{panel.findChild<QLabel*>(QStringLiteral("flowMeshMarketableNote"))};
        QVERIFY(marketable && slippage && note); QVERIFY(!marketable->isChecked()); QCOMPARE(slippage->text(), QStringLiteral("1.00"));
        panel.m_price->setText(QStringLiteral("0.5"));
        // Buy against the other account's best ask of 1 B3/tUSD: at most 1% above.
        marketable->setChecked(true);
        QCOMPARE(panel.m_price->text(), QStringLiteral("1.01")); QVERIFY(panel.m_price->isReadOnly());
        QVERIFY(note->text().contains(QStringLiteral("not a fill estimate"))); QVERIFY(note->text().contains(QStringLiteral("rests")));
        slippage->setText(QStringLiteral("10")); QCOMPARE(panel.m_price->text(), QStringLiteral("1.1"));
        slippage->setText(QStringLiteral("11")); QVERIFY(panel.m_price->text().isEmpty()); QVERIFY(note->text().contains(QStringLiteral("0.01 to 10 percent")));
        slippage->setText(QStringLiteral("1.00"));
        // The only bid is this account's own: nothing to sell into.
        panel.m_sell->click(); QVERIFY(panel.m_price->text().isEmpty()); QVERIFY(note->text().contains(QStringLiteral("No certified buy orders")));
        panel.m_buy->click(); QCOMPARE(panel.m_price->text(), QStringLiteral("1.01"));
        // Stale data never prices.
        panel.m_read_failed = true; panel.updateTicket(); QVERIFY(panel.m_price->text().isEmpty()); QVERIFY(note->text().contains(QStringLiteral("fresh certified")));
        panel.m_read_failed = false; panel.updateTicket(); QCOMPARE(panel.m_price->text(), QStringLiteral("1.01"));
        // Nor does a market that is not tradable, however fresh its data.
        panel.m_snapshot->paused = true; panel.updateTicket(); QVERIFY(panel.m_price->text().isEmpty()); QVERIFY(note->text().contains(QStringLiteral("tradable market")));
        panel.m_snapshot->paused = false; panel.updateTicket(); QCOMPARE(panel.m_price->text(), QStringLiteral("1.01"));
        marketable->setChecked(false); QVERIFY(!panel.m_price->isReadOnly()); QVERIFY(note->isHidden());
        QVERIFY(!panel.m_thread); QVERIFY(!panel.m_confirmation); QCOMPARE(unlock.count(), 0); QVERIFY(m_wallet->IsLocked());
    }
    void marketableReviewSurvivesBookMoveDuringForegroundRead()
    {
        FundingReadProbe probe; probe.hold = true;
        // The book moves during the review's foreground read: the best ask
        // becomes 1.1 B3/tUSD.
        auto liquidity{probe.data["liquidity"]}; UniValue curves{UniValue::VARR};
        for (const auto& curve : liquidity["curves"].getValues()) {
            auto moved{curve};
            if (curve["side"].get_str() == "ask") {
                UniValue points{UniValue::VARR};
                for (const auto [price, quantity] : {std::pair{1099, 0}, std::pair{1100, 1'000'000}}) { UniValue p{UniValue::VOBJ}; p.pushKV("price", price); p.pushKV("quantity", quantity); points.push_back(p); }
                moved.pushKV("points", points);
            }
            curves.push_back(moved);
        }
        liquidity.pushKV("curves", curves); probe.data.pushKV("liquidity", liquidity);
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        panel.m_catalog_age.start(); panel.m_refresh_phase = B3FlowMeshTradingPanel::RefreshPhase::None;
        panel.m_quantity->setText(QStringLiteral("0.5"));
        panel.m_marketable->setChecked(true); QCOMPARE(panel.m_price->text(), QStringLiteral("1.01"));
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        QString review; QTimer review_guard;
        connect(&review_guard, &QTimer::timeout, &panel, [&] {
            if (!panel.m_confirmation) return;
            review = panel.m_confirmation->text(); panel.m_confirmation->done(QMessageBox::Cancel);
        });
        review_guard.start(1);
        panel.m_order->click(); QVERIFY(panel.m_deferred_review && panel.m_deferred_review->order_context->marketable);
        QVERIFY(probe.entered.tryAcquire(1, 2000)); probe.release.release();
        QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000); QTRY_VERIFY_WITH_TIMEOUT(!review.isEmpty(), 2000); review_guard.stop();
        // Re-priced from the fresh read, never refused as changed input.
        QVERIFY2(review.contains(QStringLiteral("at 1.111 B3 / tUSD")), qPrintable(review));
        QVERIFY(review.contains(QStringLiteral("best certified opposite order at 1.1 B3 / tUSD with at most 1.00% slippage")));
        QVERIFY(review.contains(QStringLiteral("0.75 tUSD (visible depth, not a fill estimate)")));
        QVERIFY(review.contains(QStringLiteral("stays as a standing order until you cancel it")));
        QVERIFY(review.contains(QStringLiteral("not a market order")));
        QVERIFY(!panel.m_log->toPlainText().contains(QStringLiteral("order inputs")));
        QCOMPARE(panel.m_price->text(), QStringLiteral("1.111"));
        QCOMPARE(unlock.count(), 0); QCOMPARE(probe.writes.load(), 0); QVERIFY(!panel.m_unlock); QVERIFY(m_wallet->IsLocked());
    }
    void editPrefillsRemainingOfCertifiedCurveAndReviewSaysReplaces_data()
    {
        QTest::addColumn<bool>("inverse");
        QTest::newRow("canonical") << false;
        QTest::newRow("inverse") << true;
    }
    void editPrefillsRemainingOfCertifiedCurveAndReviewSaysReplaces()
    {
        QFETCH(bool, inverse);
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()), inverse);
        auto* edit{panel.findChild<QPushButton*>(QStringLiteral("flowMeshEditOrder"))};
        QVERIFY(edit); QVERIFY(!edit->isEnabled()); // Nothing selected.
        panel.m_marketable->setChecked(true); panel.m_sell->click(); panel.m_quantity->setText(QStringLiteral("3"));
        // The certified own bid: 1 B3/tUSD, 0.25 filled, 0.75 remaining.
        panel.m_own_view->selectRow(0);
        QCOMPARE(panel.m_edit_side, QStringLiteral("bid")); QVERIFY(edit->isEnabled());
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        edit->click();
        QVERIFY(!panel.m_marketable->isChecked()); QVERIFY(!panel.m_price->isReadOnly());
        // A canonical bid is Buy, or Sell B3 in the inverse view.
        QCOMPARE(panel.m_side->currentIndex(), inverse ? 1 : 0);
        QCOMPARE(panel.m_price->text(), QStringLiteral("1")); QCOMPARE(panel.m_quantity->text(), QStringLiteral("0.75"));
        QVERIFY(panel.m_log->toPlainText().contains(QStringLiteral("Nothing was signed or sent")));
        QVERIFY(!panel.m_thread); QVERIFY(!panel.m_confirmation); QCOMPARE(unlock.count(), 0);
        // The ordinary review names what the new order replaces.
        QString review;
        QTimer::singleShot(0, &panel, [&] { if (panel.m_confirmation) { review = panel.m_confirmation->text(); panel.m_confirmation->done(QMessageBox::Cancel); } });
        panel.begin(B3FlowMeshTrading::Operation::Order);
        QVERIFY2(review.contains(QStringLiteral("Replaces your current %1 order (0.75 tUSD left; 0.25 tUSD already filled)").arg(inverse ? QStringLiteral("Sell B3") : QStringLiteral("Buy"))), qPrintable(review));
        QVERIFY(review.contains(QStringLiteral("counts from zero")));
        QCOMPARE(unlock.count(), 0); QVERIFY(!panel.m_unlock); QVERIFY(!panel.m_thread); QVERIFY(m_wallet->IsLocked());
    }
    void editUnavailableForGeneralExhaustedPendingOrUncertainCurves()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        const auto select = [&] { panel.m_own_view->clearSelection(); panel.m_own_view->selectRow(0); panel.updateControls(); return panel.m_edit_order->isEnabled(); };
        QVERIFY(select());
        // Not a simple limit.
        auto general{*panel.m_snapshot}; general.own_curves[0].points = {{900, 1'000'000}, {1000, 500'000}, {1001, 0}};
        panel.m_snapshot = general; panel.updateDataViews(); QVERIFY(!select());
        // Exhausted: nothing remains to replace.
        auto exhausted{Parse(RemoteData())}; exhausted.own_curves[0].filled = 1'000'000; exhausted.own_curves[0].remaining = 0;
        panel.m_snapshot = exhausted; panel.updateDataViews(); QVERIFY(!select());
        panel.m_snapshot = Parse(RemoteData()); panel.updateDataViews(); QVERIFY(select());
        // An unresolved request of this account, or an uncertain one.
        TrackUnknown(panel); panel.m_pending_sequence = panel.m_snapshot->account_sequence;
        panel.updateControls(); QVERIFY(!panel.m_edit_order->isEnabled());
        panel.m_receipt.reset(); panel.m_pending_sequence.reset(); panel.updateControls(); QVERIFY(panel.m_edit_order->isEnabled());
        panel.m_uncertain = true; panel.updateControls(); QVERIFY(!panel.m_edit_order->isEnabled());
        panel.m_uncertain = false; panel.m_busy = true; panel.updateControls(); QVERIFY(!panel.m_edit_order->isEnabled());
        panel.m_busy = false; panel.updateControls(); QVERIFY(panel.m_edit_order->isEnabled());
        QVERIFY(!panel.m_thread); QVERIFY(m_wallet->IsLocked());
    }
    void editBindsBySideAndRefusesChangedCurve()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        panel.m_own_view->selectRow(0); QCOMPARE(panel.m_edit_side, QStringLiteral("bid"));
        panel.m_price->setText(QStringLiteral("0.7")); panel.m_quantity->setText(QStringLiteral("2"));
        // The certified bid changes while selected: the edit refuses.
        auto moved{*panel.m_snapshot}; moved.own_curves[0].points = {{1005, 1'000'000}, {1006, 0}};
        panel.m_snapshot = moved; panel.updateDataViews(); panel.updateControls();
        QVERIFY(!panel.m_edit_order->isEnabled()); QVERIFY(panel.m_own_view->selectionModel()->selectedRows().isEmpty());
        panel.editOrder();
        QCOMPARE(panel.m_price->text(), QStringLiteral("0.7")); QCOMPARE(panel.m_quantity->text(), QStringLiteral("2"));
        QVERIFY(panel.m_log->toPlainText().contains(QStringLiteral("Nothing was prefilled, signed or sent")));
        // Selecting it again binds the order now shown.
        panel.m_own_view->selectRow(0); QVERIFY(panel.m_edit_order->isEnabled());
        panel.editOrder(); QCOMPARE(panel.m_price->text(), QStringLiteral("1.005")); QCOMPARE(panel.m_quantity->text(), QStringLiteral("0.75"));
        // The same row now showing the other side is never edited as it.
        auto ask{Parse(RemoteData())}; ask.own_curves = {LimitCurve("ask", 5, 1200, 400'000)};
        panel.m_snapshot = ask; panel.updateDataViews(); panel.updateControls();
        QVERIFY(panel.m_own_view->selectionModel()->selectedRows().isEmpty()); QVERIFY(panel.m_edit_side.isEmpty());
        QVERIFY(!panel.m_edit_order->isEnabled());
        QVERIFY(!panel.m_thread); QVERIFY(m_wallet->IsLocked());
    }
    void editStaysAvailableWhilePassiveReadsRun()
    {
        FundingReadProbe probe; probe.hold = true;
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        struct Cleanup { FundingReadProbe& probe; B3FlowMeshTradingPanel& panel; ~Cleanup() { probe.release.release(8); panel.cancelAndWait(); } } cleanup{probe, panel};
        // After a first order its certified request stays tracked, so every
        // passive job reads it before the market read.
        TrackUnknown(panel); Certify(*panel.m_receipt); panel.m_receipt->no_resubmit = true;
        probe.receipt_response = CertifiedStatus(panel.m_snapshot->market);
        panel.m_catalog_age.start(); panel.m_refresh_phase = B3FlowMeshTradingPanel::RefreshPhase::None;
        panel.m_price->setText(QStringLiteral("0.7")); panel.m_quantity->setText(QStringLiteral("2"));
        panel.m_own_view->selectRow(0); panel.updateControls(); QVERIFY(panel.m_edit_order->isEnabled());
        QSignalSpy unlock{m_model.get(), &WalletModel::requireUnlock};
        // A passive read is in flight: Edit stays enabled and a click prefills.
        panel.startJob(); QVERIFY(probe.entered.tryAcquire(1, 2000));
        QVERIFY(panel.m_thread); QVERIFY(panel.m_active_result && panel.m_active_result->passive);
        QVERIFY(panel.m_edit_order->isEnabled());
        panel.m_edit_order->click();
        QCOMPARE(panel.m_price->text(), QStringLiteral("1")); QCOMPARE(panel.m_quantity->text(), QStringLiteral("0.75"));
        QVERIFY(panel.m_log->toPlainText().contains(QStringLiteral("Nothing was signed or sent")));
        QVERIFY(!panel.m_confirmation); QVERIFY(!panel.m_busy);
        probe.release.release(); QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QCOMPARE(Remote(probe), (std::vector<std::string>{"getflowmeshactionstatus", "getflowmeshmarketdata"}));
        // The read keeps the prefilled replacement and the selection.
        QCOMPARE(panel.m_price->text(), QStringLiteral("1")); QCOMPARE(panel.m_quantity->text(), QStringLiteral("0.75"));
        QVERIFY(panel.m_edit_order->isEnabled());
        // Work that may sign still excludes it.
        panel.m_busy = true; panel.updateControls(); QVERIFY(!panel.m_edit_order->isEnabled());
        panel.m_busy = false; panel.updateControls(); QVERIFY(panel.m_edit_order->isEnabled());
        QCOMPARE(unlock.count(), 0); QCOMPARE(probe.writes.load(), 0); QVERIFY(!panel.m_unlock); QVERIFY(m_wallet->IsLocked());
    }
    void submissionNoticeNamesNoRpcAndDetailsKeepTheCommand()
    {
        FundingReadProbe probe;
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel);
        auto result{std::make_shared<B3FlowMeshTradingPanel::Result>()};
        B3FlowMeshTrading::Action action; action.operation = B3FlowMeshTrading::Operation::Order; action.market = panel.m_market_data.front();
        action.side = QStringLiteral("bid"); action.price = 1000; action.amount = 1;
        result->action = action; result->wallet = panel.m_wallet_name; result->write_attempted = true;
        B3FlowMeshTrading::Receipt receipt; receipt.action_id = QString::fromStdString(H(99).GetHex()); receipt.market = action.market.id;
        receipt.state = QStringLiteral("queued"); result->receipt = receipt;
        panel.m_busy = true; panel.finishJob(result);
        QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        const auto log{panel.m_log->toPlainText()};
        QVERIFY(log.contains(QStringLiteral("No automatic retry"))); QVERIFY(log.contains(QStringLiteral("Your orders")));
        QVERIFY(!log.contains(QStringLiteral("getflowmeshactionstatus"))); QVERIFY(!log.contains(QStringLiteral("retryflowmeshaction")));
        QVERIFY(panel.m_identity_detail->text().contains(QStringLiteral("getflowmeshactionstatus %1 %2").arg(action.market.id, receipt.action_id)));
        QCOMPARE(probe.writes.load(), 0); QVERIFY(m_wallet->IsLocked());
    }
    void connectionBannerShowsLiveReconnectingAndCheckingState()
    {
        using Apply = B3FlowMeshTradingPanel::SnapshotApply;
        FundingReadProbe probe; // Any read the panel starts stays in this process.
        B3FlowMeshTradingPanel panel;
        auto* banner{panel.findChild<QLabel*>(QStringLiteral("flowMeshLinkBanner"))};
        QVERIFY(banner); panel.updateControls(); QVERIFY(banner->isHidden()); // No wallet: no banner.
        AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        QElapsedTimer age; age.start();
        QCOMPARE(panel.applySelectedSnapshot(Parse(RemoteData()), age, {}), Apply::APPLIED);
        panel.show(); QCoreApplication::processEvents();
        // Not polling (hidden or just shown): checking, never a stale verdict.
        panel.m_polling_since.invalidate(); panel.updateControls();
        QCOMPARE(banner->text(), QStringLiteral("Checking connection…")); QVERIFY(banner->isVisible());
        panel.m_polling_since.start(); panel.updateControls();
        QCOMPARE(banner->text(), QStringLiteral("Connected · operator.invalid · polling"));
        QCOMPARE(banner->property("b3role").toString(), QStringLiteral("positive"));
        QVERIFY(panel.orderReady());
        panel.m_watch.mode = B3FlowMeshWatch::Mode::Live; panel.updateControls();
        QCOMPARE(banner->text(), QStringLiteral("Connected · operator.invalid · live updates"));
        panel.m_watch.mode = B3FlowMeshWatch::Mode::Polling;
        // A failed read: the attempt, the panel's own next try and the time
        // of the last verified data (display only).
        panel.m_read_failed = true; panel.m_read_failures = 2; panel.m_link_attempts = 3; panel.m_attempt_age.start();
        panel.updateControls();
        QVERIFY2(banner->text().startsWith(QStringLiteral("Reconnecting · attempt 3 · next try in 2 s · showing verified data from ")), qPrintable(banner->text()));
        QCOMPARE(banner->property("b3role").toString(), QStringLiteral("accent"));
        QVERIFY(!panel.orderReady());
        // An event gap is named while it is recent.
        panel.m_read_failed = false; panel.addGapMarker(panel.m_snapshot->market, 2, 3); panel.updateControls();
        QVERIFY(banner->text().endsWith(QStringLiteral(" · event gap: request outcomes are being re-checked")));
        // The panel's own tick: hidden stops polling, shown starts it again.
        panel.hide(); QVERIFY(QMetaObject::invokeMethod(panel.m_timer, "timeout")); QVERIFY(!panel.m_polling_since.isValid());
        panel.show(); QCoreApplication::processEvents(); QVERIFY(QMetaObject::invokeMethod(panel.m_timer, "timeout"));
        QVERIFY(panel.m_polling_since.isValid());
        QTRY_VERIFY_WITH_TIMEOUT(!panel.m_thread, 2000);
        QCOMPARE(probe.writes.load(), 0); QVERIFY(m_wallet->IsLocked());
    }
    void bannerLiveFollowsFreshDataDespiteSelectedEndpointFailure()
    {
        B3FlowMeshTradingPanel panel; AttachOfflineWallet(panel); Observe(panel, Parse(RemoteData()));
        // The client failed over: the selected endpoint's transport failed,
        // while another endpoint served the verified data now displayed.
        panel.m_client_info = ConnectionInfo("https://trading.invalid", true, false, false);
        panel.m_polling_since.start(); panel.show(); QCoreApplication::processEvents(); panel.updateControls();
        QVERIFY(panel.m_connection_status->text().contains(QStringLiteral("HTTPS connection failed")));
        auto* banner{panel.findChild<QLabel*>(QStringLiteral("flowMeshLinkBanner"))};
        QVERIFY(banner->text().startsWith(QStringLiteral("Connected · operator.invalid")));
        QVERIFY(panel.orderReady());
        // And whenever the gate's freshness fails, the banner is never Live.
        panel.m_read_failed = true; panel.updateControls();
        QVERIFY(!banner->text().startsWith(QStringLiteral("Connected"))); QVERIFY(!panel.orderReady());
        QVERIFY(!panel.m_thread); QVERIFY(m_wallet->IsLocked());
    }
    void displayedPricesAreRoundedWithExactTooltips()
    {
        B3FlowMeshTradingPanel panel; panel.resize(1320, 940); panel.m_timer->stop();
        ShowSyntheticClearings(panel);
        QVERIFY(panel.inverted());
        // The latest clearing, 1009 B3 atoms per tUSD atom, is 1000/1009 tUSD per B3.
        QCOMPARE(panel.m_last_price->text(), QStringLiteral("≈0.99108 tUSD / B3"));
        QCOMPARE(panel.m_last_price->toolTip(), QStringLiteral("Exact last trade price: 1000/1009 tUSD / B3"));
        QCOMPARE(panel.m_history_view->item(0, 1)->text(), QStringLiteral("≈0.99108"));
        QVERIFY(panel.m_history_view->item(0, 1)->toolTip().contains(QStringLiteral("1000/1009")));
        const int oldest{panel.m_history_view->rowCount() - 1};
        QCOMPARE(panel.m_history_view->item(oldest, 1)->text(), QStringLiteral("1")); // 1000: exact
        QVERIFY(panel.m_history_view->item(oldest, 1)->toolTip().isEmpty());
        auto* last{panel.findChild<QLabel*>(QStringLiteral("flowMeshBookLast"))};
        QCOMPARE(last->text(), QStringLiteral("Last trade  ≈0.99108 tUSD"));
        QVERIFY(last->toolTip().contains(QStringLiteral("Exact price: 1000/1009 tUSD")));
        // Candles: OHLC and axis labels, their exact forms on hover.
        panel.m_chart->grab();
        QVERIFY(panel.m_chart->exactPrices().contains(QStringLiteral("C 1000/1009")));
        QVERIFY(panel.m_chart->exactPrices().contains(QStringLiteral("(tUSD / B3)")));
        // Depth: the liquidity axis runs from 1000/1001 to 1000/999.
        auto* liquidity{panel.findChild<QPushButton*>(QStringLiteral("flowMeshChartLiquidity"))};
        QVERIFY(liquidity); liquidity->click(); panel.m_chart->grab();
        QVERIFY(panel.m_chart->exactPrices().contains(QStringLiteral("axis left 1000/1001")));
        QVERIFY(panel.m_chart->exactPrices().contains(QStringLiteral("axis right 1000/999")));
        QCOMPARE(panel.m_depth_view->item(0, 0)->text(), QStringLiteral("≈0.999"));
        QVERIFY(panel.m_depth_view->item(0, 0)->toolTip().contains(QStringLiteral("1000/1001")));
        QHelpEvent help{QEvent::ToolTip, QPoint{20, 20}, panel.m_chart->mapToGlobal(QPoint{20, 20})};
        QCoreApplication::sendEvent(panel.m_chart, &help);
        QVERIFY(QToolTip::text().startsWith(panel.m_chart->toolTip()));
        QVERIFY(QToolTip::text().endsWith(panel.m_chart->exactPrices()));
        QToolTip::hideText();
        // A spread between rounded prices is the difference of the prices
        // beside it, as shown (1.000 - 0.990), its exact value on hover.
        B3FlowMeshOrderBook book; book.setSnapshot(SyntheticLimitBook(), true);
        QCOMPARE(SpreadText(book), QStringLiteral("Spread  ≈0.010 tUSD · 0.995%"));
        QVERIFY(book.model()->row(book.model()->spreadRow()).tooltip.endsWith(QStringLiteral("Exact spread: 1/101 tUSD / B3")));
        book.setSnapshot(SyntheticLimitBook(), false);
        QCOMPARE(SpreadText(book), QStringLiteral("Spread  0.010 B3 · 0.995%"));
        QVERIFY(!book.model()->row(book.model()->spreadRow()).tooltip.contains(QStringLiteral("Exact spread")));
        // Clearing strip and indicative line list what they rounded.
        auto s{Parse(RemoteDataWithViews())};
        auto tick{s.ticks->ticks[1]}; tick.price = 1009; QString exact;
        QVERIFY(ClearingTickText(tick, s.units, true, false, &exact).contains(QStringLiteral("at ≈0.99108 tUSD / B3")));
        QCOMPARE(exact, QStringLiteral("Exact price: 1000/1009 tUSD / B3"));
        ClearingTickText(tick, s.units, false, false, &exact); QVERIFY(exact.isEmpty());
        auto view{*s.indicative}; view.price = 1009; view.best_bid = 1009; view.best_ask = 1010;
        QVERIFY(IndicativeText(view, s.units, true, &exact).contains(QStringLiteral("would clear at ≈0.99108 tUSD / B3")));
        QVERIFY(exact.startsWith(QStringLiteral("Exact prices: "))); QVERIFY(exact.contains(QStringLiteral("1000/1009"))); QVERIFY(exact.contains(QStringLiteral("100/101")));
        QVERIFY(!panel.m_wallet); QVERIFY(!panel.m_thread);
    }
    void exportOptInSyntheticVisualFixtures()
    {
        const QString directory{qEnvironmentVariable("FLOWMESH_UI_SNAPSHOT_DIR")};
        if (directory.isEmpty()) QSKIP("Set FLOWMESH_UI_SNAPSHOT_DIR to export explicitly synthetic, walletless QA PNGs.");
        QVERIFY(QDir::isAbsolutePath(directory)); QDir output{directory}; QVERIFY(output.mkpath(QStringLiteral(".")));
        B3FlowMeshTradingPanel panel; panel.resize(1320, 940); panel.m_timer->stop();
        ShowSyntheticClearings(panel);
        QCOMPARE(panel.m_own_view->item(0, 3)->text(), QStringLiteral("0.75 B3"));
        QVERIFY(panel.m_balances->text().contains(QStringLiteral("In orders  0 tUSD · 0.75 B3")));
        const auto save = [&](const QString& name) { QImage image{panel.size(), QImage::Format_ARGB32}; image.fill(Qt::transparent); panel.render(&image); return image.save(output.filePath(name)); };
        QVERIFY(save(QStringLiteral("synthetic-flowmesh-pending.png")));
        auto* liquidity{panel.findChild<QPushButton*>(QStringLiteral("flowMeshChartLiquidity"))};
        QVERIFY(liquidity); liquidity->click(); QVERIFY(liquidity->isChecked());
        QVERIFY(save(QStringLiteral("synthetic-flowmesh-liquidity.png")));
        panel.m_snapshot->paused = true; panel.m_market_data[0].ready = false; panel.m_market_data[0].checkpoint_pending = true; panel.m_market_data[0].checkpoint = QString::fromStdString(H(80).GetHex()); panel.updateMarketText();
        QVERIFY(save(QStringLiteral("synthetic-flowmesh-paused.png")));
        panel.m_read_failed = true; panel.updateMarketText(); QVERIFY(save(QStringLiteral("synthetic-flowmesh-stale.png")));
        // Observed time: a realistic multi-account book with ladder orders,
        // UTC candles and the trades tape, locally and through an endpoint,
        // and an older server's auction-order fallback.
        const auto render = [&](QWidget& widget, const QString& name) {
            QImage image{widget.size(), QImage::Format_ARGB32}; image.fill(Qt::transparent); widget.render(&image);
            return image.save(output.filePath(name));
        };
        {
            B3FlowMeshTradingPanel timed; timed.resize(1320, 940); timed.m_timer->stop();
            ShowSyntheticTimeMarket(timed, SyntheticSource::LOCAL);
            QVERIFY(timed.m_chart->timeMode());
            QVERIFY(timed.m_chart->timeSeries().candles.size() > 70);
            QVERIFY(render(timed, QStringLiteral("synthetic-flowmesh-time-candles.png")));
            QVERIFY(render(*timed.m_chart, QStringLiteral("synthetic-flowmesh-time-chart.png")));
            auto* mode{timed.findChild<QComboBox*>(QStringLiteral("flowMeshLiquidityMode"))};
            QVERIFY(mode); mode->setCurrentIndex(2); QCoreApplication::processEvents();
            QCOMPARE(timed.m_trade_tape->table()->rowCount(), 48);
            QVERIFY(render(timed, QStringLiteral("synthetic-flowmesh-time-tape.png")));
            mode->setCurrentIndex(1); QCoreApplication::processEvents();
            QVERIFY(render(timed, QStringLiteral("synthetic-flowmesh-time-curve-depth.png")));
            mode->setCurrentIndex(0);
            timed.m_activity->setCurrentIndex(1); QCoreApplication::processEvents();
            QVERIFY(render(timed, QStringLiteral("synthetic-flowmesh-time-history.png")));
            auto* interval{timed.findChild<QComboBox*>(QStringLiteral("flowMeshCandleInterval"))};
            timed.m_chart->zoomTime(0.5); QCoreApplication::processEvents();
            QVERIFY(render(*timed.m_chart, QStringLiteral("synthetic-flowmesh-time-chart-zoomed-out.png")));
            QVERIFY(interval && interval->count() == 6);
        }
        {
            B3FlowMeshTradingPanel remote; remote.resize(1320, 940); remote.m_timer->stop();
            ShowSyntheticTimeMarket(remote, SyntheticSource::REMOTE);
            QVERIFY(remote.m_chart->timeMode()); QVERIFY(remote.m_chart->timeSeries().remote);
            auto* mode{remote.findChild<QComboBox*>(QStringLiteral("flowMeshLiquidityMode"))};
            QVERIFY(mode); mode->setCurrentIndex(2); QCoreApplication::processEvents();
            QVERIFY(render(remote, QStringLiteral("synthetic-flowmesh-time-remote.png")));
        }
        {
            // The order book: the node's exact levels of 24 accounts' limit
            // orders, a four-price ladder per side and the viewer's own bid,
            // laid out as Hyperliquid's, beside UTC candles and the tape.
            // At the standard 1320 x 940, as the owner sees it: the ladder
            // holds at least ten levels a side.
            B3FlowMeshTradingPanel book; book.resize(1320, 940); book.m_timer->stop();
            ShowSyntheticTimeMarket(book, SyntheticSource::LOCAL);
            QCoreApplication::processEvents();
            QVERIFY(book.m_order_book->showsVerifiedLevels());
            QVERIFY(book.m_snapshot->levels->bids.levels.size() >= 10 && book.m_snapshot->levels->asks.levels.size() >= 10);
            QVERIFY(book.m_order_book->rowsPerSide() >= 10);
            QVERIFY(render(book, QStringLiteral("synthetic-flowmesh-book-levels.png")));
            QVERIFY(render(*book.m_order_book, QStringLiteral("synthetic-flowmesh-book-ladder.png")));
            // The canonical orientation, then 0.01 B3 groups (the node's
            // grouped levels, as the next read after choosing them brings).
            book.m_orientation->setCurrentIndex(0); QCoreApplication::processEvents();
            QVERIFY(render(*book.m_order_book, QStringLiteral("synthetic-flowmesh-book-canonical.png")));
            // Hover from the spread to the fifth bid.
            const int spread{book.m_order_book->model()->spreadRow()};
            book.m_order_book->setHoverRow(spread + 5);
            QVERIFY(render(*book.m_order_book, QStringLiteral("synthetic-flowmesh-book-hover.png")));
            book.m_order_book->setHoverRow(-1);
            const auto grouping{Grouping(false, 1, -2, 6)};
            book.m_book_groupings[{book.m_snapshot->market, false}] = grouping;
            book.m_snapshot = WithLevels(*book.m_snapshot, grouping, H(5));
            book.updateDataViews(); QCoreApplication::processEvents();
            QVERIFY(book.m_order_book->showsVerifiedLevels());
            QCOMPARE(book.m_order_book->groupingSelector()->currentText(), QStringLiteral("0.01"));
            QVERIFY(render(*book.m_order_book, QStringLiteral("synthetic-flowmesh-book-grouped.png")));
            // A ladder order in the ticket: four prices, one order.
            book.m_book_groupings.clear(); book.m_snapshot = WithLevels(*book.m_snapshot, {}, H(5));
            book.m_orientation->setCurrentIndex(1); QCoreApplication::processEvents();
            // Sell B3 (a canonical bid ladder); the walletless fixture's
            // buttons are disabled, so the side is set directly.
            book.m_side->setCurrentIndex(1); book.m_sell->setChecked(true);
            book.m_ladder->setChecked(true);
            book.m_price->setText(QStringLiteral("1.002")); book.m_quantity->setText(QStringLiteral("0.3"));
            const std::array<std::pair<const char*, const char*>, 3> rows{{{"1.004", "0.2"}, {"1.006", "0.2"}, {"1.01", "0.1"}}};
            for (size_t i{0}; i < rows.size(); ++i) { book.m_ladder_prices[i]->setText(QLatin1String(rows[i].first)); book.m_ladder_quantities[i]->setText(QLatin1String(rows[i].second)); }
            QVERIFY(book.ladderSteps());
            book.updateTicket(); QCoreApplication::processEvents();
            QVERIFY(book.m_order_book->showsVerifiedLevels());
            QVERIFY(book.m_ticket_total->text().contains(QStringLiteral("Reserves exactly")));
            QVERIFY(render(book, QStringLiteral("synthetic-flowmesh-ladder-ticket.png")));
            QVERIFY(!book.m_thread); QVERIFY(!book.m_wallet);
        }
        {
            // The book alone, tall: rows fill the height.
            auto snapshot{SyntheticLimitBook(14)};
            snapshot.curves.push_back(LadderCurve("bid", 40, {{966, 150'000}, {969, 150'000}, {972, 150'000}, {975, 150'000}}, 50'000));
            snapshot.curves.push_back(LadderCurve("ask", 41, {{1041, 90'000}, {1043, 90'000}, {1045, 90'000}, {1047, 90'000}}));
            snapshot = WithLevels(snapshot, {}, H(20));
            B3FlowMeshOrderBook alone; alone.resize(460, 900); alone.setSnapshot(snapshot, false);
            alone.show(); QCoreApplication::processEvents(); QCoreApplication::processEvents();
            QVERIFY(alone.rowsPerSide() >= 17);
            QVERIFY(render(alone, QStringLiteral("synthetic-flowmesh-orderbook-tall.png")));
            alone.setSnapshot(snapshot, true); QCoreApplication::processEvents();
            QVERIFY(render(alone, QStringLiteral("synthetic-flowmesh-orderbook-tall-b3.png")));
        }
        {
            B3FlowMeshTradingPanel older; older.resize(1320, 940); older.m_timer->stop();
            ShowSyntheticTimeMarket(older, SyntheticSource::OLDER_SERVER);
            QVERIFY(!older.m_chart->timeMode());
            auto* mode{older.findChild<QComboBox*>(QStringLiteral("flowMeshLiquidityMode"))};
            QVERIFY(mode); mode->setCurrentIndex(2); QCoreApplication::processEvents();
            QVERIFY(render(older, QStringLiteral("synthetic-flowmesh-auction-order-fallback.png")));
        }
        {
            // Responsiveness: the same realistic book (24 accounts' limit
            // orders, a four-price ladder a side, the viewer's own bid),
            // UTC candles and the tape, then 36 more certified auctions
            // through the live-update handler as the local engine brings
            // them (a partial fill each, a new price every third). The book
            // updates in place and flashes new prices; renders stay at the
            // update rate.
            B3FlowMeshTradingPanel live; live.resize(1320, 940); live.m_timer->stop();
            ShowSyntheticTimeMarket(live, SyntheticSource::LOCAL);
            live.m_client_info = [] { UniValue info{UniValue::VOBJ}; info.pushKV("backend", "local"); info.pushKV("engine_enabled", true); info.pushKV("endpoints", UniValue{UniValue::VARR}); return info; }();
            QApplication::setEffectEnabled(Qt::UI_General, true); B3FlowMeshOrderBook::SetAnimationSetting(true);
            const auto apply = [&](uint64_t n, std::optional<CAmount> new_bid = std::nullopt) {
                auto next{NextSyntheticHead(*live.m_snapshot, live.m_ticks, n, new_bid)};
                QElapsedTimer age; age.start();
                QVERIFY(live.applySelectedSnapshot(std::move(next), age, live.m_snapshot->head, true) == B3FlowMeshTradingPanel::SnapshotApply::APPLIED);
                QCoreApplication::processEvents();
            };
            for (uint64_t n{1}; n <= 35; ++n) apply(n);
            apply(36, 1000); // A new price beside the spread: its flash runs now.
            QVERIFY(live.m_order_book->flashing());
            QVERIFY(live.m_order_book->showsVerifiedLevels());
            QVERIFY(live.m_snapshot->levels->bids.levels.size() >= 10 && live.m_snapshot->levels->asks.levels.size() >= 10);
            QVERIFY(live.m_order_book->rowsPerSide() >= 10);
            QTest::qWait(B3FlowMeshOrderBook::FLASH_MS / 2);
            QVERIFY(render(live, QStringLiteral("synthetic-flowmesh-responsive-live-book.png")));
            // The book, the UTC candles and the tape side by side, as they
            // stand after the stream (display only; nothing is signed).
            QWidget side;
            auto* row{new QHBoxLayout{&side}};
            auto* chart{new B3FlowMeshChart{&side}};
            chart->setInverted(true); chart->setSnapshot(*live.m_snapshot); chart->setTimeSeries(live.m_chart->timeSeries());
            chart->setNowMs(OBSERVED_T0 + 90'000); row->addWidget(chart, 1);
            auto* book{new B3FlowMeshOrderBook{&side}}; book->setFixedWidth(430);
            book->setSnapshot(live.m_snapshot, true); row->addWidget(book);
            auto* tape{new B3FlowMeshTradeTape{&side}}; tape->setFixedWidth(360);
            tape->setRows(B3FlowMeshTradeTape::BuildRows(live.m_ticks, live.m_tick_times), live.m_snapshot->units, true,
                          QStringLiteral("this_node"), false, {});
            row->addWidget(tape);
            side.resize(1760, 900); side.show(); QCoreApplication::processEvents(); QCoreApplication::processEvents();
            QVERIFY(chart->timeMode());
            QVERIFY(book->rowsPerSide() >= 10);
            QVERIFY(tape->table()->rowCount() >= 40);
            QVERIFY(render(side, QStringLiteral("synthetic-flowmesh-responsive-book-chart-tape.png")));
            // An order review open with the local engine: the book keeps
            // following live updates (display only); the ticket, balances
            // and every gate stay on the data of before the review.
            live.m_busy = true; live.updateControls();
            live.m_watch.Enable(live.m_watch_clock.elapsed(), true, true); live.m_watch.mode = B3FlowMeshWatch::Mode::Live;
            QVERIFY(live.liveWhileBusy());
            const auto gate_head{live.m_snapshot->head};
            for (uint64_t n{37}; n <= 40; ++n) {
                auto next{NextSyntheticHead(*live.displayedSnapshot(), live.m_ticks, n)};
                QElapsedTimer age; age.start();
                live.applyDisplayOnly(std::move(next), age);
                QCoreApplication::processEvents();
            }
            QCOMPARE(live.m_snapshot->head, gate_head);
            QVERIFY(live.m_order_book->head() != gate_head);
            QVERIFY(render(live, QStringLiteral("synthetic-flowmesh-responsive-review-live-book.png")));
            live.m_busy = false; live.m_watch.Disable();
            QVERIFY(!live.m_thread); QVERIFY(!live.m_watch_thread);
        }
        panel.hide(); QVERIFY(!panel.m_order->isEnabled()); QVERIFY(!panel.m_wallet); // Fixture cannot submit.
    }
    void cleanupTestCase()
    {
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
    QCoreApplication::setApplicationName(QStringLiteral("FlowMeshWorkspace"));
    B3Theme::apply(app);
    B3FlowMeshWorkspaceTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "b3flowmeshworkspacetests.moc"
