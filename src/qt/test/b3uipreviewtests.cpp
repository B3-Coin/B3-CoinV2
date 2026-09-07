// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <qt/b3theme.h>
#include <qt/test/b3assettests.h>
#include <qt/test/b3dashboardtests.h>
#include <qt/test/b3hardeningtests.h>
#include <qt/test/b3shelltests.h>
#include <qt/test/b3stakesettingstests.h>
#include <qt/test/b3tradetests.h>
#include <test/util/setup_common.h>

#include <QApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

#include <functional>
#include <string>
#include <vector>

const std::function<void(const std::string&)> G_TEST_LOG_FUN{};
const std::function<std::vector<const char*>()> G_TEST_COMMAND_LINE_ARGUMENTS{};
const std::function<std::string()> G_TEST_GET_FULL_NAME{[] { return "b3_ui_preview"; }};

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "minimal");
    QApplication app(argc, argv);
    QTemporaryDir settings;
    if (!settings.isValid()) return 1;
    QCoreApplication::setOrganizationName(QStringLiteral("B3HiveIsolatedTests"));
    QCoreApplication::setApplicationName(QStringLiteral("ui-preview"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    // Basic setup only selects regtest parameters, temporary data and crypto
    // context. No node, wallet, RPC, networking or mining is started.
    BasicTestingSetup setup{ChainType::REGTEST, TestOpts{.setup_net = false}};
    Q_INIT_RESOURCE(bitcoin);
    B3Theme::apply(app);
    const QString selection{qEnvironmentVariable("B3_QT_SUITE")};
    int failures{0};
    const auto run = [&](QObject& suite, const QString& name) {
        if (selection.isEmpty() || selection == name) failures += QTest::qExec(&suite, argc, argv);
    };
    B3ShellTests shell;
    B3DashboardTests dashboard;
    B3AssetTests assets;
    B3StakeSettingsTests settings_tests;
    B3TradeTests trade;
    B3HardeningTests hardening;
    run(shell, QStringLiteral("shell"));
    run(dashboard, QStringLiteral("dashboard"));
    run(assets, QStringLiteral("assets"));
    run(settings_tests, QStringLiteral("settings"));
    run(trade, QStringLiteral("trade"));
    run(hardening, QStringLiteral("hardening"));
    return failures ? 1 : 0;
}
