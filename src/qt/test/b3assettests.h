// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_TEST_B3ASSETTESTS_H
#define BITCOIN_QT_TEST_B3ASSETTESTS_H

#include <QObject>

class B3AssetTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void noWalletShowsEmptyState();
    void amountFormattingIsIntegerExact();
    void nativeOnlySourceEnablesOnlySupportedActions();
    void unknownMetadataAndLongNamesRenderSafely();
    void insertionRemovalAndResetUpdateModel();
    void sourceDestructionDetachesModel();
};

#endif // BITCOIN_QT_TEST_B3ASSETTESTS_H
