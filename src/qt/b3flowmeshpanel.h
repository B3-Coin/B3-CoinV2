// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_B3FLOWMESHPANEL_H
#define BITCOIN_QT_B3FLOWMESHPANEL_H

#include <qt/b3assetmodel.h>
#include <qt/b3flowmeshoperator.h>
#include <qt/walletmodel.h>
#include <QPointer>
#include <QWidget>
#include <atomic>
#include <memory>
#include <optional>

class B3AssetSendDialog;
class QLabel;
class QMessageBox;
class QPlainTextEdit;
class QPushButton;
class QThread;
class QTimer;
namespace interfaces { class Wallet; }

/** Public FN diagnostics and explicitly confirmed, node-global operator actions. */
class B3FlowMeshPanel : public QWidget
{
    Q_OBJECT
public:
    explicit B3FlowMeshPanel(QWidget* parent = nullptr);
    ~B3FlowMeshPanel() override;
    void setWalletModel(WalletModel* wallet);
    void setFnAsset(const B3AssetRecord& asset);

public Q_SLOTS:
    void cancelAndWait();

private:
    friend class B3FlowMeshPanelTests;
    enum class Operation { Refresh, Arm, Disarm };
    struct Result;
    void refresh();
    void control(Operation operation);
    void bind();
    void startJob(Operation operation, const UniValue& params = UniValue{UniValue::VARR});
    void finishJob(const std::shared_ptr<Result>& result);
    void stopWorker();
    void updateControls();
    void showStatus();
    bool restoreSpendingLock();

    QPointer<WalletModel> m_wallet;
    std::shared_ptr<interfaces::Wallet> m_backend;
    QPointer<B3AssetSendDialog> m_binding_dialog;
    QPointer<QMessageBox> m_confirmation;
    B3AssetRecord m_fn;
    std::optional<B3FlowMeshOperator::Status> m_state;
    std::unique_ptr<WalletModel::UnlockContext> m_unlock;
    std::shared_ptr<std::atomic_bool> m_cancel{std::make_shared<std::atomic_bool>(false)};
    QThread* m_thread{nullptr};
    QTimer* m_timer{nullptr};
    bool m_busy{false};
    bool m_restore_locked{false};
    std::shared_ptr<interfaces::Wallet> m_relock_backend;
    QString m_relock_wallet_name;
    QString m_security_warning;
    uint64_t m_generation{0};
    QLabel* m_wallet_label{nullptr};
    QLabel* m_armed{nullptr};
    QLabel* m_finality{nullptr};
    QLabel* m_message{nullptr};
    QPlainTextEdit* m_keys{nullptr};
    QPlainTextEdit* m_markets{nullptr};
    QPushButton* m_bind{nullptr};
    QPushButton* m_arm{nullptr};
    QPushButton* m_disarm{nullptr};
    QPushButton* m_refresh{nullptr};
};
#endif // BITCOIN_QT_B3FLOWMESHPANEL_H
