// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_QT_B3FLOWMESHTRADINGPANEL_H
#define BITCOIN_QT_B3FLOWMESHTRADINGPANEL_H

#include <qt/b3flowmeshtrading.h>
#include <qt/walletmodel.h>
#include <QPointer>
#include <QWidget>
#include <atomic>
#include <memory>
#include <optional>

class QComboBox;
class QLabel;
class QLineEdit;
class QMessageBox;
class QPlainTextEdit;
class QPushButton;
class QThread;
class QTimer;
namespace interfaces { class Wallet; }

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
    struct Result;
    void refresh();
    void updateControls();
    void updateMarketText();
    void begin(B3FlowMeshTrading::Operation operation);
    bool confirm(const QString& text, bool final_transaction);
    void startJob(std::optional<B3FlowMeshTrading::Action> action = std::nullopt,
                  std::optional<B3AssetTransfer::Prepared> prepared = std::nullopt);
    void finishJob(const std::shared_ptr<Result>& result);
    void stopWorker();
    bool restoreLock();
    void notice(const QString& text);
    void markUncertain(const std::shared_ptr<Result>& result);
    void reviewUncertain();
    std::optional<B3FlowMeshTrading::Market> market() const;

    QPointer<WalletModel> m_wallet;
    std::shared_ptr<interfaces::Wallet> m_backend, m_relock_backend, m_uncertain_backend;
    std::shared_ptr<Result> m_active_result;
    QString m_wallet_name, m_security_warning, m_requested_base, m_relock_wallet_name, m_uncertain_details;
    std::unique_ptr<WalletModel::UnlockContext> m_unlock;
    std::shared_ptr<std::atomic_bool> m_cancel{std::make_shared<std::atomic_bool>(false)};
    QPointer<QMessageBox> m_confirmation;
    QThread* m_thread{nullptr};
    QTimer* m_timer{nullptr};
    bool m_busy{false}, m_restore_locked{false}, m_uncertain{false};
    bool m_uncertain_refreshed{false};
    bool m_route_pending{false}, m_requested_withdrawal{false};
    uint64_t m_generation{0};
    std::vector<B3FlowMeshTrading::Market> m_market_data;
    std::vector<UniValue> m_effect_data;
    QString m_pending_market, m_pending_account;
    std::optional<uint64_t> m_pending_sequence;
    QComboBox* m_market{nullptr};
    QComboBox* m_side{nullptr};
    QComboBox* m_asset{nullptr};
    QComboBox* m_effect{nullptr};
    QLineEdit* m_price{nullptr};
    QLineEdit* m_quantity{nullptr};
    QLineEdit* m_amount{nullptr};
    QLineEdit* m_destination{nullptr};
    QLineEdit* m_deposit_txid{nullptr};
    QLineEdit* m_deposit_vout{nullptr};
    QLabel* m_status{nullptr};
    QLabel* m_balances{nullptr};
    QPlainTextEdit* m_log{nullptr};
    QPushButton *m_refresh{nullptr}, *m_order{nullptr}, *m_cancel_order{nullptr},
        *m_deposit{nullptr}, *m_admit{nullptr}, *m_withdraw{nullptr},
        *m_checkpoint{nullptr}, *m_publish{nullptr}, *m_review_uncertain{nullptr};
};
#endif
