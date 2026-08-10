// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_TEST_B3SHELLTESTS_H
#define BITCOIN_QT_TEST_B3SHELLTESTS_H

#include <QObject>
#include <QTest>

//! Focused tests for the B3FlowMesh visual system and navigation shell.
class B3ShellTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void sidebarEmitsCanonicalPages();
    void shellRoutesNavigationAndSwitchesContent();
    void placeholderPagesAreHonest();
    void topStatusReflectsNetworkAndPeers();
    void reducedMotionUnderOffscreen();
};

#endif // BITCOIN_QT_TEST_B3SHELLTESTS_H
