// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_QT_B3FLOWMESHTRADINGPANEL_H
#define BITCOIN_QT_B3FLOWMESHTRADINGPANEL_H

#include <qt/b3flowmeshtrading.h>
#include <qt/b3flowmeshmarketdata.h>
#include <qt/walletmodel.h>
#include <QElapsedTimer>
#include <QPointer>
#include <QWidget>
#include <atomic>
#include <memory>
#include <optional>

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
    struct StatusRead {
        // Attribution only; a queued read must never own a wallet/backend.
        QPointer<WalletModel> wallet;
        uint64_t generation;
        interfaces::Node* node;
        QString domain, config, market, account, action_id, selected_market;
        bool operator==(const StatusRead&) const = default;
    };
    struct Result {
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
    };
    struct DeferredReview {
        B3FlowMeshTrading::Operation operation;
        bool funding;
        uint64_t generation;
        QString market;
        QString effect;
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
    void updateControls();
    void updateMarketText();
    void updateDataViews();
    void updateTicket();
    void openFunding(bool withdrawal);
    void begin(B3FlowMeshTrading::Operation operation);
    bool confirm(const QString& text, bool final_transaction);
    void startJob(std::optional<B3FlowMeshTrading::Action> action = std::nullopt,
                  std::optional<B3AssetTransfer::Prepared> prepared = std::nullopt, bool exact_retry = false, bool receipt_only = false,
                  std::optional<StatusRead> status_read = std::nullopt, const QString& connect_url = {});
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
    static std::vector<UniValue> ReadEffectsForRefresh(
        const B3FlowMeshTrading::RpcCall& rpc, const QString& market_id,
        B3FlowMeshMarketData::Snapshot& snapshot);
    void finishJob(const std::shared_ptr<Result>& result);
    void applyJobResult(const std::shared_ptr<Result>& result);
    void applyMarketCatalog(const Result& result);
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
    QPointer<QMessageBox> m_confirmation;
    QPointer<QDialog> m_funding_dialog;
    QThread* m_thread{nullptr};
    QTimer* m_timer{nullptr};
    bool m_busy{false}, m_restore_locked{false}, m_uncertain{false};
    bool m_uncertain_refreshed{false};
    bool m_route_pending{false}, m_requested_withdrawal{false};
    uint64_t m_generation{0};
    std::optional<B3FlowMeshMarketData::Snapshot> m_snapshot;
    QElapsedTimer m_response_age, m_certificate_age, m_catalog_age, m_attempt_age, m_queue_age;
    unsigned m_read_failures{0};
    bool m_loading{false}, m_read_failed{false};
    QString m_read_error;
    std::vector<B3FlowMeshTrading::Market> m_market_data;
    std::vector<UniValue> m_effect_data;
    B3FlowMeshTrading::SavedActions m_saved_actions;
    bool m_saved_actions_ready{false};
    QString m_pending_market, m_pending_account;
    std::optional<B3FlowMeshTrading::Receipt> m_receipt;
    QString m_receipt_error;
    std::optional<uint64_t> m_pending_sequence;
    QComboBox* m_market{nullptr};
    QComboBox* m_saved_selector{nullptr};
    QComboBox* m_orientation{nullptr};
    QComboBox* m_side{nullptr};
    QComboBox* m_asset{nullptr};
    QComboBox* m_effect{nullptr};
    QLineEdit* m_price{nullptr};
    QLineEdit* m_endpoint{nullptr};
    QPushButton* m_connect{nullptr};
    QLabel* m_connection_status{nullptr};
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
    QTableWidget *m_depth_view{nullptr}, *m_history_view{nullptr}, *m_own_view{nullptr};
    QTabWidget* m_activity{nullptr};
    QWidget* m_advanced{nullptr};
    QPushButton *m_buy{nullptr}, *m_sell{nullptr};
    QPlainTextEdit* m_log{nullptr};
    QPushButton *m_refresh{nullptr}, *m_order{nullptr}, *m_cancel_order{nullptr},
        *m_deposit{nullptr}, *m_admit{nullptr}, *m_withdraw{nullptr},
        *m_checkpoint{nullptr}, *m_publish{nullptr}, *m_review_uncertain{nullptr}, *m_retry_receipt{nullptr}, *m_check_receipt{nullptr};
};
#endif
