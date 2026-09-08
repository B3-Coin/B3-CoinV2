// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_B3ASSETSENDDIALOG_H
#define BITCOIN_QT_B3ASSETSENDDIALOG_H

#include <qt/b3assetmodel.h>
#include <qt/b3assettransfer.h>
#include <qt/walletmodel.h>

#include <QDialog>
#include <QPointer>

#include <atomic>
#include <memory>
#include <optional>
#include <string>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QThread;
namespace interfaces {
class Node;
class Wallet;
} // namespace interfaces

/** Prepare once, review exact outputs/fee, then submit the same signed bytes. */
class B3AssetSendDialog : public QDialog
{
    Q_OBJECT

public:
    explicit B3AssetSendDialog(WalletModel* wallet, const B3AssetRecord& asset,
                               QWidget* parent = nullptr);
    ~B3AssetSendDialog() override;
    static int execForAsset(WalletModel* wallet, const B3AssetRecord& asset, QWidget* parent = nullptr);

public Q_SLOTS:
    void reject() override;
    //! Wallet detach/shutdown boundary: cancel and drain before node teardown.
    //! Workers never wait for GUI callbacks, so this cannot form a UI deadlock.
    void cancelAndWait();

Q_SIGNALS:
    void transactionSubmitted(const QString& txid);
    void submissionUncertain(const QString& txid);

private:
    enum class Phase { Editing, Preparing, Review, Submitting, Finished };
    struct JobResult;
    void prepare();
    void submit();
    void startJob(bool broadcast);
    void finishJob(const std::shared_ptr<JobResult>& result);
    void stopWorker();
    void setStatus(const QString& text);
    void setEditing(bool editing);

    QPointer<WalletModel> m_wallet;
    const B3AssetRecord m_asset;
    const QString m_wallet_name;
    const QString m_wallet_display;
    const std::string m_wallet_uri;
    interfaces::Node* m_node{nullptr};
    std::shared_ptr<interfaces::Wallet> m_backend;
    std::unique_ptr<WalletModel::UnlockContext> m_unlock;
    std::shared_ptr<std::atomic_bool> m_cancel{std::make_shared<std::atomic_bool>(false)};
    std::optional<B3AssetTransfer::Prepared> m_prepared;
    QThread* m_thread{nullptr};
    Phase m_phase{Phase::Editing};
    bool m_close_requested{false};
    uint64_t m_generation{0};
    CAmount m_raw_amount{0};
    QString m_recipient;

    QLineEdit* m_address{nullptr};
    QLineEdit* m_amount{nullptr};
    QLabel* m_fee{nullptr};
    QLabel* m_review_amount{nullptr};
    QLineEdit* m_txid{nullptr};
    QLabel* m_status{nullptr};
    QCheckBox* m_confirm{nullptr};
    QPushButton* m_action{nullptr};
    QPushButton* m_close{nullptr};
};

#endif // BITCOIN_QT_B3ASSETSENDDIALOG_H
