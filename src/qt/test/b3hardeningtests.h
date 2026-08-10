// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_TEST_B3HARDENINGTESTS_H
#define BITCOIN_QT_TEST_B3HARDENINGTESTS_H

#include <QObject>

class B3HardeningTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void sidebarKeyboardNavigationAndAccessibility();
    void themeContrastIsReadable();
    void topStatusNetworkIdentitiesAreUnmistakable();
    void shellSurvivesWalletWidgetReplacementAndPageCycling();
    void tradeBackendReplacementFallsBackToNull();
    void chartRendersExtremeValues();
    void dashboardSurvivesModelChurn();
    void assetAndStakePagesSurviveWalletChurn();
};

#endif // BITCOIN_QT_TEST_B3HARDENINGTESTS_H
