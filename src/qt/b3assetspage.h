// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_B3ASSETSPAGE_H
#define BITCOIN_QT_B3ASSETSPAGE_H

#include <qt/b3assetmodel.h>

#include <QWidget>

class WalletModel;

QT_BEGIN_NAMESPACE
class QLabel;
class QGridLayout;
class QLineEdit;
class QPushButton;
class QResizeEvent;
class QSortFilterProxyModel;
class QTableView;
QT_END_NAMESPACE

/**
 * The multi-asset wallet page. B3NativeAssetSource provides real native B3,
 * FN Coin, and coloured-asset wallet balances. Actions without an approved
 * submission path remain honestly disabled.
 */
class B3AssetsPage : public QWidget
{
    Q_OBJECT

public:
    explicit B3AssetsPage(QWidget* parent = nullptr);

    //! Attach the current wallet (may be null for the no-wallet state).
    //! Replaces the model's source; prior connections are dropped.
    void setWalletModel(WalletModel* wallet_model);
    //! Install a custom source (tests / future backends). Ownership stays
    //! with the caller.
    void setSource(B3AssetSource* source);

    B3AssetTableModel* model() const { return m_model; }

Q_SIGNALS:
    void sendRequested();
    void receiveRequested();

private Q_SLOTS:
    void updateDetails();

private:
    void resizeEvent(QResizeEvent* event) override;
    void reflowCards(int width);

    B3AssetTableModel* m_model{nullptr};
    QSortFilterProxyModel* m_proxy{nullptr};
    B3AssetSource* m_owned_source{nullptr};
    bool m_have_wallet{false};
    bool m_model_resetting{false};
    QString m_selected_asset_id;

    QLineEdit* m_search{nullptr};
    QTableView* m_list{nullptr};
    QLabel* m_empty{nullptr};

    QLabel* m_detail_name{nullptr};
    QLabel* m_detail_status{nullptr};
    QLabel* m_detail_id{nullptr};
    QLabel* m_detail_confirmed{nullptr};
    QLabel* m_detail_pending{nullptr};
    QLabel* m_detail_available{nullptr};
    QLabel* m_detail_immature{nullptr};
    QLabel* m_detail_reserved{nullptr};
    QLabel* m_detail_flowmesh{nullptr};
    QPushButton* m_send{nullptr};
    QPushButton* m_receive{nullptr};
    QPushButton* m_deposit{nullptr};
    QPushButton* m_withdraw{nullptr};
    QLabel* m_backend_note{nullptr};
    QLabel* m_activity_note{nullptr};

    QGridLayout* m_columns{nullptr};
    QWidget* m_list_card{nullptr};
    QWidget* m_detail_card{nullptr};
    int m_layout_columns{0};
};

#endif // BITCOIN_QT_B3ASSETSPAGE_H
