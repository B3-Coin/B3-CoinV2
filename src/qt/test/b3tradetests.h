// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_TEST_B3TRADETESTS_H
#define BITCOIN_QT_TEST_B3TRADETESTS_H

#include <QObject>

class B3TradeTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void emptyChartRendersHonestState();
    void oneCandleAndFlatDataRender();
    void thousandsOfCandlesRenderClipped();
    void zoomAndPanRespectBounds();
    void malformedAndOutOfOrderInputIsSanitized();
    void incrementalAppendFollowsTail();
    void orderBookCumulativeAndStates();
    void backendUnavailableDisablesSubmission();
    void ticketTotalsAreIntegerExactAcrossPrecisions();
    void chartSurvivesSeriesDestructionAndReplacement();
};

#endif // BITCOIN_QT_TEST_B3TRADETESTS_H
