// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include "flowmeshclosedtest_policy.h"
#include <qt/bitcoin.h>
#include <compat/compat.h>
#include <util/translation.h>

#include <QApplication>
#include <QFile>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

extern const TranslateFn G_TRANSLATION_FUN = [](const char* text) {
    return QCoreApplication::translate("bitcoin-core", text).toStdString();
};
const std::function<std::string()> G_TEST_GET_FULL_NAME{};

namespace {
int Refuse(const QString& reason)
{
    std::fprintf(stderr, "B3 FlowMesh REGTEST TEST4 — NOT READY: %s\n", reason.toUtf8().constData());
    // A Finder-opened unconfigured package must visibly explain the block. No
    // rejected caller arguments reach Qt, and GuiMain/node/wallet never starts.
    int count{1};
    char name[]{"B3 FlowMesh REGTEST TEST4"};
    char* values[]{name, nullptr};
    QApplication app{count, values};
    QMessageBox box{QMessageBox::Warning, QStringLiteral("B3 FlowMesh REGTEST TEST4 — NOT READY"),
        reason + QStringLiteral("\n\nNo B3 node or wallet was opened. Do not use a normal wallet. Ask the test coordinator for the reviewed test environment."), QMessageBox::Ok};
    box.setTextFormat(Qt::PlainText);
    box.exec();
    return EXIT_FAILURE;
}
} // namespace

MAIN_FUNCTION
{
    QString error;
    if (!FlowMeshClosedTest::CheckInvocation(argc, error)) return Refuse(error);
    QFile embedded{QStringLiteral(":/closed-test/profile.json")};
    if (!embedded.open(QIODevice::ReadOnly) || embedded.size() > 32768) return Refuse(QStringLiteral("Embedded test profile is missing or oversized."));
    FlowMeshClosedTest::Profile profile;
    if (!FlowMeshClosedTest::ParseProfile(embedded.readAll(), profile, error)) return Refuse(error);
    if (!profile.ready) return Refuse(profile.reason);

    FlowMeshClosedTest::Storage storage;
    if (!FlowMeshClosedTest::PrepareStorage(profile, QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation), storage, error)) return Refuse(error);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, storage.settings);
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, storage.settings);

    // No user-provided node arguments are forwarded. This calls the real GUI,
    // wallet locks, TLS verifier and lifecycle unchanged. Each tester creates
    // a fresh 'closed-test' wallet using the normal Qt Create Wallet flow.
    const auto args{FlowMeshClosedTest::NodeArguments(profile, storage)};
    std::vector<QByteArray> encoded;
    encoded.reserve(args.size() + 1);
    encoded.emplace_back(argv[0]);
    // Core paths and option values use UTF-8, including Windows account paths.
    for (const auto& arg : args) encoded.push_back(arg.toUtf8());
    std::vector<char*> pointers;
    for (auto& arg : encoded) pointers.push_back(arg.data());
    pointers.push_back(nullptr);
    std::fprintf(stderr, "B3 FlowMesh REGTEST TEST4 profile=%s network=regtest validator=0 admin_rpc=0; test tokens have no value; futures informational only\n", profile.id.toUtf8().constData());
    const int result{GuiMain(static_cast<int>(encoded.size()), pointers.data())};
    std::fprintf(stderr, "B3 FlowMesh REGTEST TEST4 GuiMain exit status=%d\n", result);
    return result;
}
