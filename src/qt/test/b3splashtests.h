// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_TEST_B3SPLASHTESTS_H
#define BITCOIN_QT_TEST_B3SPLASHTESTS_H

#include <QObject>

namespace interfaces {
class Node;
} // namespace interfaces

class B3SplashTests : public QObject
{
    Q_OBJECT

public:
    explicit B3SplashTests(interfaces::Node& node) : m_node(node) {}

private Q_SLOTS:
    void reducedMotionShowsStaticFrame();
    void animationRunsAndStopsOnDestruction();
    void repeatedProgressUpdatesAreSafe();
    void closeDuringInitRequestsShutdown();
    void teardownWithNodeHandlersIsClean();

private:
    interfaces::Node& m_node;
};

#endif // BITCOIN_QT_TEST_B3SPLASHTESTS_H
