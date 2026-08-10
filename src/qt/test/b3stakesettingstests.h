// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_TEST_B3STAKESETTINGSTESTS_H
#define BITCOIN_QT_TEST_B3STAKESETTINGSTESTS_H

#include <QObject>

class B3StakeSettingsTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void stakePageIsHonestWithoutWallet();
    void settingsPageRoutesToExistingDialogs();
    void settingsPageMirrorsWalletActions();
    void shellShowsInstalledSettingsPage();
};

#endif // BITCOIN_QT_TEST_B3STAKESETTINGSTESTS_H
