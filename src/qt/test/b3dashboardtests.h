// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_TEST_B3DASHBOARDTESTS_H
#define BITCOIN_QT_TEST_B3DASHBOARDTESTS_H

#include <QObject>

class B3DashboardTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void noWalletShowsHonestEmptyState();
    void balancesRenderLargeValuesAndPrivacy();
    void immatureRowOnlyWhenNonZero();
    void privacyMasksActivity();
    void clientViewSlotsRenderWithoutNode();
};

#endif // BITCOIN_QT_TEST_B3DASHBOARDTESTS_H
