// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <interfaces/node.h>
#include <qt/b3theme.h>
#include <qt/test/b3splashtests.h>
#include <test/util/setup_common.h>

#include <QApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

#include <functional>
#include <memory>
#include <string>
#include <vector>

const std::function<void(const std::string&)> G_TEST_LOG_FUN{};
const std::function<std::vector<const char*>()> G_TEST_COMMAND_LINE_ARGUMENTS{};
const std::function<std::string()> G_TEST_GET_FULL_NAME{[] { return "b3_splash_only"; }};

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "minimal");
    QApplication app(argc, argv);
    QTemporaryDir settings;
    if (!settings.isValid()) return 1;
    QCoreApplication::setOrganizationName(QStringLiteral("B3HiveIsolatedTests"));
    QCoreApplication::setApplicationName(QStringLiteral("splash-only"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    // Regtest parameters and a temporary root only: no wallet, database load,
    // sockets, RPC, producer or validator engine is started.
    BasicTestingSetup setup{ChainType::REGTEST, TestOpts{.setup_net = false}};
    auto node = interfaces::MakeNode(setup.m_node);
    Q_INIT_RESOURCE(bitcoin);
    B3Theme::apply(app);
    B3SplashTests tests(*node);
    return QTest::qExec(&tests, argc, argv);
}
