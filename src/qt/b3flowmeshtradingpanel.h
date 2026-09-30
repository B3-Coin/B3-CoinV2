// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_QT_B3FLOWMESHTRADINGPANEL_H
#define BITCOIN_QT_B3FLOWMESHTRADINGPANEL_H

#include <qt/b3flowmeshtrading.h>
#include <qt/b3flowmeshmarketdata.h>
#include <qt/b3flowmeshnotices.h>
#include <qt/b3flowmeshorderbook.h>
#include <qt/b3flowmeshtradetape.h>
#include <qt/b3flowmeshwatch.h>
#include <qt/walletmodel.h>
#include <QElapsedTimer>
#include <QPointer>
#include <QWidget>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <utility>

class QCheckBox;
class QComboBox;
class QDialog;
class QTableWidget;
class QTabWidget;
class B3FlowMeshChart;
class QLabel;
class QLineEdit;
class QMessageBox;
class QPlainTextEdit;
class QPushButton;
class QThread;
class QTimer;
namespace interfaces { class Wallet; class Node; }

class B3FlowMeshTradingPanel : public QWidget
{
    Q_OBJECT
public:
    explicit B3FlowMeshTradingPanel(QWidget* parent = nullptr);
    ~B3FlowMeshTradingPanel() override;
    void setWalletModel(WalletModel* wallet);
    //! Empty base selects native B3 funding, never a fabricated native market.
    void selectBaseAsset(const QString& asset_id, bool withdrawal);
public Q_SLOTS:
    void cancelAndWait();
Q_SIGNALS:
    void securityWarning(const QString& warning);
private:
    friend class B3FlowMeshWorkspaceTests;
    enum class RefreshPhase { None, Catalog, Status, Effects, Receipt };
    struct StatusRead {
        // Attribution only; a queued read must never own a wallet/backend.
        QPointer<WalletModel> wallet;
        uint64_t generation;
        interfaces::Node* node;
        QString domain, config, market, account, action_id, selected_market;
        bool operator==(const StatusRead&) const = default;
    };
    struct Result {
        Result() { snapshot_age.start(); }
        std::optional<B3FlowMeshTrading::Action> action;
        std::optional<B3AssetTransfer::Prepared> prepared;
        std::vector<B3FlowMeshTrading::Market> markets;
        std::vector<UniValue> effects;
        std::optional<B3FlowMeshMarketData::Snapshot> snapshot;
        std::optional<B3FlowMeshTrading::Receipt> receipt;
        std::optional<StatusRead> receipt_scope;
        UniValue response;
        std::optional<UniValue> client_info;
        QString connect_url, client_error;
        QString wallet, error, receipt_error, read_market;
        QString receipt_market, receipt_account, receipt_action_id;
        bool broadcast{false}, write_attempted{false}, catalog{false}, exact_retry{false}, receipt_only{false};
        // Foreground read-only status of the tracked receipt right after its
        // signed action was sent, waiting briefly for certified inclusion.
        bool receipt_wait{false};
        bool passive{false}, foreground_read{false}, refresh_yielded{false};
        // A passive job while live updates supply the market read: only its
        // auxiliary phase runs, never the final selected-market read.
        bool live{false};
        // This passive job's Receipt phase replaced its scheduled phase to
        // reconcile the tracked request first; the schedule does not advance.
        bool reconcile_first{false};
        std::optional<RefreshPhase> refresh_phase;
        uint64_t refresh_epoch{0};
        QElapsedTimer snapshot_age;
        // What the selected-market read was requested against: the head
        // sent as known_head (empty: none), and the displayed snapshot's
        // next sequence for that market when the job started (0: none).
        QString basis_head;
        uint64_t basis_sequence{0};
        //! Display sections the selected-market read asks for, captured on
        //! the GUI thread: the ticks after the last one held for `views_market`
        //! and the ticket's draft for the indicative clearing (null: none).
        QString views_market;
        std::optional<uint64_t> ticks_after;
        UniValue draft;
        //! Observed-time candles this job reads for `views_market`, when due
        //! (captured on the GUI thread), and what it read. A failed candles
        //! read never fails the job: it only leaves `candles_error`.
        std::optional<flowmesh::CandleInterval> candles_interval;
        size_t candles_limit{0};
        std::optional<B3FlowMeshMarketData::CandlePage> candles;
        QString candles_error;
    };
    //! How a verified selected-market read was applied.
    enum class SnapshotApply { APPLIED, SUPERSEDED, MISMATCH };
    //! What one live-update read was armed against. Attribution only: the
    //! watch worker owns no wallet, backend or panel state.
    struct WatchScope {
        QPointer<WalletModel> wallet;
        uint64_t generation{0}, watch_generation{0};
        QString market, account, known_head;
    };
    struct WatchResult {
        WatchResult() { snapshot_age.start(); }
        WatchScope scope;
        std::optional<B3FlowMeshMarketData::Snapshot> snapshot;
        QString error, status, endpoint;
        bool supported{false}, waited{false}, gap{false}, release{false};
        uint32_t endpoint_live_ms{0};
        QElapsedTimer snapshot_age;
        //! As Result's, for the watched market.
        std::optional<uint64_t> ticks_after;
        UniValue draft;
    };
    struct DeferredReview {
        B3FlowMeshTrading::Operation operation;
        bool funding;
        uint64_t generation;
        QString market;
        QString effect;
        struct FundingContext {
            B3FlowMeshTrading::Market market;
            B3FlowMeshMarketData::Units units;
            QString amount, destination;
            int asset;
        };
        std::optional<FundingContext> funding_context;
        struct OrderContext {
            QPointer<WalletModel> wallet;
            B3FlowMeshTrading::Market market;
            B3FlowMeshMarketData::Units units;
            QString price, quantity;
            int side;
            bool inverse, needs_market_read;
            // A marketable-limit ticket re-prices from the fresh read, so its
            // shown price may change; its slippage and mode may not.
            bool marketable{false};
            QString slippage;
        };
        std::optional<OrderContext> order_context;
    };
    struct DeferredConnect {
        QPointer<WalletModel> wallet;
        uint64_t generation;
        interfaces::Node* node;
        QString url;
    };
    void requestConnect();
    void resumeConnect();
    void updateConnectionState();
    bool deferReview(B3FlowMeshTrading::Operation operation, bool funding = false);
    void resumeReview();
    void refresh();
    void resetRefreshSchedule();
    void updateControls();
    void updateMarketText();
    void updateDataViews();
    void updateTicket();
    void openFunding(bool withdrawal);
    bool depositDraftAvailable() const;
    bool depositReady() const;
    bool withdrawalDraftAvailable() const;
    bool withdrawalReady() const;
    bool orderReviewAvailable() const;
    bool orderReady() const;
    //! The marketable-limit price for the ticket's side, orientation,
    //! slippage and quantity from the displayed certified snapshot. Freshness
    //! is the caller's gate.
    std::optional<B3FlowMeshMarketData::MarketableLimit> marketableLimit(QString* error) const;
    //! The ticket's order as the indicative draft {side, price, quantity} in
    //! canonical units; null when it does not parse.
    UniValue indicativeDraft() const;
    //! The last held tick of `market`, when the held ticks are that market's.
    std::optional<uint64_t> ticksAfter(const QString& market) const;
    //! Extends the held ticks (and their observed times) from the displayed
    //! snapshot's page.
    void mergeHeldTicks();
    //! The candles page the next job should read for the displayed market
    //! ({interval, limit}), or nothing: a whole window after a market,
    //! interval or source change (or when the forming candle could not be
    //! extended from the held ticks), else the newest few intervals about
    //! every 10 s. Only while the displayed data carries observed times.
    std::optional<std::pair<flowmesh::CandleInterval, size_t>> candlesDue() const;
    //! Applies a candles page read by a job for the displayed market.
    void applyCandles(const B3FlowMeshMarketData::CandlePage& page, size_t limit);
    void requestOrderReview();
    //! The certified own curve an Edit is bound to: the selected row's
    //! canonical side at selection time, and only while its points are
    //! unchanged since then.
    std::optional<B3FlowMeshMarketData::Curve> editCurve() const;
    //! Edit is available for a certified, simple-limit, not exhausted own
    //! order while nothing is pending, uncertain or under review.
    bool editAvailable() const;
    //! Prefills the ticket with a replacement of the selected own order: its
    //! side, exact limit and remaining quantity. Signs and sends nothing; the
    //! replacement is the ordinary reviewed order (a native per-side replace).
    void editOrder();
    void begin(B3FlowMeshTrading::Operation operation);
    bool confirm(const QString& text, bool final_transaction);
    void startJob(std::optional<B3FlowMeshTrading::Action> action = std::nullopt,
                  std::optional<B3AssetTransfer::Prepared> prepared = std::nullopt, bool exact_retry = false, bool receipt_only = false,
                  std::optional<StatusRead> status_read = std::nullopt, const QString& connect_url = {}, bool foreground_read = false,
                  bool receipt_wait = false);
    std::optional<StatusRead> selectedStatusRead() const;
    bool statusReadValid(const StatusRead& scope, bool selected) const;
    void requestStatusRead();
    void resumeStatusRead();
    void updateStatusReadState();
    void retryReceipt();
    void applyReceipt(const B3FlowMeshTrading::Receipt& receipt);
    void applyReceiptError(const Result& result, const QString& error);
    void restoreSavedActions(const B3FlowMeshTrading::SavedActions& saved);
    void selectSavedAction();
    void updateReceiptCard();
    //! Records each request's display stage where receipts arrive (never from
    //! updateControls): the Signed/Sent/Admitted high-water mark and when it
    //! first settled. The first observation after a wallet attach only
    //! seeds, so earlier history never appears as new.
    void observeRequests();
    //! Request rows: the in-flight signed job, an uncertain write without an
    //! action ID, the tracked request and this market's unsettled saved
    //! requests, plus requests that settled in the last minute. Display only.
    void updatePendingView();
    //! A request's short title: the reviewed action's own title when this
    //! panel signed it, else its saved instruction, else its ID.
    QString requestTitle(const QString& key, const B3FlowMeshTrading::SavedAction* saved, const QString& action_id) const;
    //! Exact display units, only for the selected market's verified asset.
    std::pair<std::optional<int>, QString> requestUnits(const QString& market) const;
    //! Shows a toast once per key and mirrors it to the Activity log; never
    //! while the panel is tearing down or switching wallets.
    void toast(const QString& key, B3FlowMeshToasts::Kind kind, const QString& text, std::chrono::milliseconds ttl);
    //! A lost (false) or restored (true) market read; at most one lost
    //! notice per 30 s, and a restored one only after a shown lost one.
    void linkToast(bool restored);
    //! ActionId is semantic and excludes market, domain and config.
    static QString RequestKey(const QString& market, const QString& account, const QString& action_id)
    {
        return market + QLatin1Char(':') + account + QLatin1Char(':') + action_id;
    }
    static std::vector<UniValue> ReadEffectsForRefresh(
        const B3FlowMeshTrading::RpcCall& rpc, const QString& market_id,
        B3FlowMeshMarketData::Snapshot& snapshot);
    void finishJob(const std::shared_ptr<Result>& result);
    void applyJobResult(const std::shared_ptr<Result>& result);
    void applyMarketCatalog(const Result& result);
    //! The passive phase the next refresh would run (None: market read only),
    //! including a due reconcile-first override.
    RefreshPhase nextPassivePhase() const;
    //! The phase of the bounded passive cycle, without any override.
    RefreshPhase scheduledPassivePhase() const;
    //! A pending reconcile may replace the next passive job's phase now.
    bool reconcileDue() const;
    //! The watch supplies the selected market's read (Live, or a probe that
    //! holds the poll), so a passive job reads only its auxiliary phase.
    bool watchSuppliesRead() const;
    //! ... and a passive refresh without an auxiliary phase is skipped.
    bool liveSkipsPoll() const;
    //! Re-keys the live-update policy on wallet, market and the endpoint the
    //! displayed data came from; runs the watchdog.
    void updateWatchState();
    //! Starts one bounded live-update read on the watch worker when allowed.
    //! `release` sends watch_ms 0: an ordinary read that closes the remote
    //! client's live-update connection.
    void armWatch(bool release = false);
    void finishWatch(const std::shared_ptr<WatchResult>& result);
    //! Drops any outstanding live-update result; joins its worker only when
    //! the panel closes (a wallet or market switch never blocks on it).
    void stopWatchWorker(bool join);
    //! The label suffix for the current live-update mode.
    QString liveUpdateText() const;
    //! The one-line connection banner. Display only: it never gates, and
    //! Live is exactly the trading gate's freshness (see ClassifyLink).
    void updateLinkBanner();
    //! The panel's own read backoff after consecutive failed reads.
    unsigned readBackoffMs() const { return std::min(10000U, 500U << std::min(4U, m_read_failures)); }
    //! A live update reported that events were missed between two heads.
    void addGapMarker(const QString& market, uint64_t from_sequence, uint64_t to_sequence);
    //! Applies one verified read of the selected market, never moving the
    //! display backwards: a reply against a head the display has left, or an
    //! older full snapshot, is superseded (dropped, not a failure).
    SnapshotApply applySelectedSnapshot(B3FlowMeshMarketData::Snapshot fresh, const QElapsedTimer& age, const QString& basis_head);
    //! The selected catalog row's fields a verified market read determines.
    static void DeriveRowFromSnapshot(B3FlowMeshTrading::Market& row, const B3FlowMeshMarketData::Snapshot& s);
    void stopWorker();
    bool restoreLock();
    void notice(const QString& text);
    void markUncertain(const std::shared_ptr<Result>& result);
    void reviewUncertain();
    std::optional<B3FlowMeshTrading::Market> market() const;
    bool inverted() const;
    bool receiptWalletSelected() const { return m_wallet && m_receipt_wallet == m_wallet; }
    bool uncertainWalletSelected() const { return m_wallet && m_uncertain_wallet == m_wallet; }

    QPointer<WalletModel> m_wallet;
    std::shared_ptr<interfaces::Wallet> m_backend, m_relock_backend;
    // Attribution only: public receipt/uncertainty data must not keep a wallet
    // alive after detach/unload, including while this panel is hidden.
    QPointer<WalletModel> m_receipt_wallet, m_uncertain_wallet;
    std::shared_ptr<Result> m_active_result;
    std::optional<DeferredReview> m_deferred_review;
    std::optional<StatusRead> m_deferred_status;
    std::optional<DeferredConnect> m_deferred_connect;
    std::optional<UniValue> m_client_info;
    QString m_connection_error, m_connect_error;
    QString m_wallet_name, m_security_warning, m_requested_base, m_relock_wallet_name, m_uncertain_details, m_uncertain_action_id;
    QString m_uncertain_market, m_uncertain_account;
    std::unique_ptr<WalletModel::UnlockContext> m_unlock;
    std::shared_ptr<std::atomic_bool> m_cancel{std::make_shared<std::atomic_bool>(false)};
    // Yield only unstarted passive RPCs, never an in-flight request or action.
    std::shared_ptr<std::atomic_bool> m_yield_refresh{std::make_shared<std::atomic_bool>(false)};
    QPointer<QMessageBox> m_confirmation;
    QPointer<QDialog> m_funding_dialog;
    QThread* m_thread{nullptr};
    QTimer* m_timer{nullptr};
    // Live updates: a second worker keeps at most one bounded watch read
    // outstanding for the selected market (see B3FlowMeshWatch::Policy).
    QThread* m_watch_thread{nullptr};
    B3FlowMeshWatch::Policy m_watch;
    QElapsedTimer m_watch_clock;
    // Bumped whenever an outstanding watch result must be dropped.
    uint64_t m_watch_generation{0};
    // Wallet generation, market and watched endpoint the policy is keyed on.
    QString m_watch_key;
    bool m_watch_advertised{false}, m_watch_watchdog{false}, m_watch_release_pending{false};
    // Reconcile-first, one shot per loss: after a lost market read (its
    // first failure), a failed live update, a watchdog trip or an event gap,
    // the next passive job reads the tracked request's status before its
    // market read. It replaces that job's phase without advancing the
    // schedule, never runs in consecutive jobs, and waits for discovery after
    // a reset, so it can delay the Catalog, Status or Effects phases by at
    // most one job and never freeze them. Cleared once a status read of the
    // tracked request completes.
    bool m_reconcile_pending{false}, m_last_job_was_reconcile{false};
    // Display only, per market, cleared on a market or wallet switch.
    struct GapMarker {
        QString market;
        uint64_t from_sequence{0}, to_sequence{0};
        QElapsedTimer age;
    };
    static constexpr size_t MAX_GAP_MARKERS{16};
    static constexpr int64_t GAP_SUFFIX_MS{30'000};
    std::deque<GapMarker> m_gap_markers;
    // Set while the panel closes: only then is the watch worker joined.
    bool m_closing{false};
    bool m_busy{false}, m_restore_locked{false}, m_uncertain{false};
    bool m_uncertain_refreshed{false};
    bool m_route_pending{false}, m_requested_withdrawal{false};
    uint64_t m_generation{0};
    RefreshPhase m_refresh_phase{RefreshPhase::Catalog};
    // Local UI fairness only. After eight yields let one passive phase finish
    // even if another click arrives, so repeated clicks cannot starve data.
    static constexpr unsigned MAX_REFRESH_YIELDS{8};
    // Longest wait of the post-submit status read; the remote client bounds it
    // again, and the local engine answers at once.
    static constexpr int POST_SUBMIT_RECEIPT_WAIT_MS{2000};
    unsigned m_refresh_yields{0};
    uint64_t m_refresh_epoch{0};
    std::optional<B3FlowMeshMarketData::Snapshot> m_snapshot;
    QElapsedTimer m_response_age, m_certificate_age, m_catalog_age, m_attempt_age, m_queue_age;
    // Restarted when the panel is shown and polls again; invalid while hidden,
    // so a hidden interval never reads as a stale connection.
    QElapsedTimer m_polling_since;
    // Display only: when the selected market's last verified reply arrived,
    // by the wall clock, and for which market. Never used by any gate.
    int64_t m_last_live_wall_ms{-1};
    QString m_last_live_market;
    // Consecutive failed reads for display (m_read_failures caps at 5).
    unsigned m_link_attempts{0};
    unsigned m_read_failures{0};
    bool m_loading{false}, m_read_failed{false};
    QString m_read_error;
    QString m_withdrawal_draft_amount;
    int m_withdrawal_draft_asset{0};
    std::vector<B3FlowMeshTrading::Market> m_market_data;
    std::vector<UniValue> m_effect_data;
    B3FlowMeshTrading::SavedActions m_saved_actions;
    bool m_saved_actions_ready{false};
    QString m_pending_market, m_pending_account;
    std::optional<B3FlowMeshTrading::Receipt> m_receipt;
    QString m_receipt_error;
    // Request rows (display only). Keys are market:account:action_id; the
    // map is FIFO-bounded. Signed, Sent and Admitted are historical facts, so
    // a later "unknown" read never erases "was sent"; Certified is never kept.
    struct RequestSeen {
        B3FlowMeshTrading::Stage high{B3FlowMeshTrading::Stage::Signing};
        bool settled{false};
        unsigned events{0}; // Toast events already observed (see observeRequests).
        QElapsedTimer settled_age; // Invalid when it had settled before this session saw it.
    };
    static constexpr size_t MAX_REQUEST_KEYS{1024}, MAX_REQUEST_ROWS{8};
    static constexpr int64_t REQUEST_LINGER_MS{60'000};
    std::map<QString, RequestSeen> m_request_seen;
    std::deque<QString> m_request_order;
    bool m_requests_seeded{false};
    QElapsedTimer m_last_settled; // When any request last settled in view.
    // Captured on the GUI thread when a signed job starts: the in-flight row
    // never reads worker-owned Result fields.
    QString m_inflight_title, m_inflight_market;
    // Titles of the request this panel signed (by key) and of an uncertain
    // write that returned no action ID.
    QString m_tracked_title, m_tracked_title_key, m_uncertain_title;
    B3FlowMeshTrading::RequestKind m_tracked_kind{B3FlowMeshTrading::RequestKind::Other};
    std::optional<uint64_t> m_pending_sequence;
    QComboBox* m_market{nullptr};
    QComboBox* m_saved_selector{nullptr};
    QComboBox* m_orientation{nullptr};
    QComboBox* m_side{nullptr};
    QComboBox* m_asset{nullptr};
    QComboBox* m_effect{nullptr};
    QLineEdit* m_price{nullptr};
    // The own order an Edit is bound to (see editCurve).
    QString m_edit_side;
    std::vector<B3FlowMeshMarketData::Point> m_edit_points;
    QCheckBox* m_marketable{nullptr};
    QLineEdit* m_slippage{nullptr};
    QLabel* m_marketable_note{nullptr};
    QLineEdit* m_endpoint{nullptr};
    QPushButton* m_connect{nullptr};
    QLabel* m_connection_status{nullptr};
    QLabel* m_link_banner{nullptr};
    QLineEdit* m_quantity{nullptr};
    QLineEdit* m_amount{nullptr};
    QLineEdit* m_destination{nullptr};
    QLineEdit* m_deposit_txid{nullptr};
    QLineEdit* m_deposit_vout{nullptr};
    QLabel* m_status{nullptr};
    QLabel* m_balances{nullptr};
    QLabel* m_receipt_card{nullptr};
    QLabel* m_status_read_state{nullptr};
    QLabel *m_pair_title{nullptr}, *m_last_price{nullptr}, *m_liquidity_note{nullptr}, *m_progress{nullptr},
        *m_quantity_label{nullptr}, *m_price_label{nullptr}, *m_ticket_total{nullptr}, *m_ticket_fee{nullptr}, *m_ticket_available{nullptr}, *m_grid_note{nullptr},
        *m_history_note{nullptr}, *m_own_note{nullptr}, *m_identity_detail{nullptr};
    B3FlowMeshChart* m_chart{nullptr};
    //! Observed-time intervals (1m…1d) while the displayed data carries
    //! times, else auction-order buckets (every 1, 5 or 20 auctions).
    QComboBox* m_candle_interval_combo{nullptr};
    //! Rebuilds the interval choices for the mode the data allows.
    void updateIntervalChoices(bool time);
    //! The observed stamp of a displayed entry: its held tick's, else the
    //! displayed reply's (same hash when given).
    std::optional<flowmesh::ObservedStamp> observedStamp(uint64_t sequence, const QString& hash = {}) const;
    //! Local "HH:mm:ss" of an entry observed live, else empty.
    QString observedClock(uint64_t sequence, const QString& hash = {}) const;
    //! Clearing ticks held for m_ticks_market (see MergeHeldTicks); display only.
    std::deque<B3FlowMeshMarketData::Tick> m_ticks;
    QString m_ticks_market;
    //! Observed times of held ticks from one source (m_tick_times_source),
    //! never part of tick equality; cleared when the held ticks are replaced
    //! or the source changes. Display only.
    std::map<uint64_t, flowmesh::ObservedStamp> m_tick_times;
    QString m_tick_times_source;
    //! Tape only, for a source that reports no observed times: when this app
    //! received a held tick live, one new auction per reply while the link
    //! was Live (never older rows, a catch-up or a reconnect). Never a chart
    //! input; dropped with the held tick, and when times become available.
    std::map<uint64_t, int64_t> m_received_times;
    //! Observed-time candles of m_candles_market at m_candle_interval, as the
    //! source served them (never recomputed here), and when they were read.
    std::optional<B3FlowMeshMarketData::CandlePage> m_candles;
    QString m_candles_market;
    flowmesh::CandleInterval m_candle_interval{flowmesh::CandleInterval::M5};
    QElapsedTimer m_candles_age;
    //! The forming candle could not be extended from the held ticks.
    bool m_candles_refetch{false};
    QLabel *m_clearing_strip{nullptr}, *m_indicative{nullptr};
    QTableWidget *m_depth_view{nullptr}, *m_history_view{nullptr}, *m_own_view{nullptr}, *m_pending_view{nullptr};
    B3FlowMeshToasts* m_toasts{nullptr};
    QElapsedTimer m_link_toast_age;
    bool m_link_toast_down{false};
    uint64_t m_link_toast_episode{0};
    B3FlowMeshOrderBook* m_order_book{nullptr};
    B3FlowMeshTradeTape* m_trade_tape{nullptr};
    QTabWidget* m_activity{nullptr};
    QWidget* m_advanced{nullptr};
    QPushButton *m_buy{nullptr}, *m_sell{nullptr};
    QPlainTextEdit* m_log{nullptr};
    QPushButton *m_refresh{nullptr}, *m_order{nullptr}, *m_cancel_order{nullptr}, *m_edit_order{nullptr},
        *m_deposit{nullptr}, *m_admit{nullptr}, *m_withdraw{nullptr},
        *m_checkpoint{nullptr}, *m_publish{nullptr}, *m_review_uncertain{nullptr}, *m_retry_receipt{nullptr}, *m_check_receipt{nullptr};
};
#endif
