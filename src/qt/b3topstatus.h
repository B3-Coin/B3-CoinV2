// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_B3TOPSTATUS_H
#define BITCOIN_QT_B3TOPSTATUS_H

#include <QFrame>

QT_BEGIN_NAMESPACE
class QHBoxLayout;
class QLabel;
QT_END_NAMESPACE

/**
 * Top status area of the shell: B3FlowMesh identity, active network, sync
 * state, connection status, wallet status and staking status. It is a
 * pure view — the surrounding window drives it from the existing
 * ClientModel/WalletModel status slots, so no data is ever fabricated
 * here. A slot is provided to host the existing wallet selector so
 * multi-wallet selection keeps working.
 */
class B3TopStatus : public QFrame
{
    Q_OBJECT

public:
    explicit B3TopStatus(QWidget* parent = nullptr);

    //! Network identity. `title_add` is the NetworkStyle suffix (empty on
    //! mainnet, e.g. "[testnet]"/"[regtest]" otherwise).
    void setNetwork(const QString& app_name, const QString& title_add);

    void setConnections(int count, bool network_active);
    void setSync(const QString& text, int permille /* 0..1000, <0 = hide bar */);
    void setWalletStatus(const QString& text);
    //! Empty text hides the staking chip (no staking model available).
    void setStakingStatus(const QString& text);

    //! Host an externally-owned widget (the wallet selector) on the right.
    void addTrailingWidget(QWidget* widget);

private:
    QLabel* makeChip(const QString& objectName);

    QHBoxLayout* m_layout{nullptr};
    QLabel* m_brand{nullptr};
    QLabel* m_netBadge{nullptr};
    QLabel* m_sync{nullptr};
    QLabel* m_connections{nullptr};
    QLabel* m_wallet{nullptr};
    QLabel* m_staking{nullptr};
    QHBoxLayout* m_trailing{nullptr};
};

#endif // BITCOIN_QT_B3TOPSTATUS_H
