// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.

#include <qt/bitcoin.h>

#include <compat/compat.h>
#include <util/translation.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStringList>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>

#ifndef B3_FLOWMESHCLIENT_GUI_TEST_DATADIR
#define B3_FLOWMESHCLIENT_GUI_TEST_DATADIR ""
#endif

// Same entry-point symbols as qt/main.cpp; no test wallet or mock node replaces
// the real GuiMain application. Only its QSettings storage is redirected.
extern const TranslateFn G_TRANSLATION_FUN = [](const char* text) {
    return QCoreApplication::translate("bitcoin-core", text).toStdString();
};
const std::function<std::string()> G_TEST_GET_FULL_NAME{};

namespace {
int Refuse(const char* reason)
{
    std::fprintf(stderr, "FlowMesh disposable GUI launcher refused: %s\n", reason);
    return EXIT_FAILURE;
}

QString ExistingAbsoluteDirectory(const QString& path)
{
    const QFileInfo info{path};
    return info.isAbsolute() && info.isDir() ? info.canonicalFilePath() : QString{};
}
} // namespace

MAIN_FUNCTION
{
    // This runs BEFORE GuiMain constructs QApplication, initializes default
    // translations, or reads either default- or regtest-named QSettings.
    constexpr auto prefix{"--test-settings-dir="};
    if (argc < 4 || !QString::fromLocal8Bit(argv[1]).startsWith(QLatin1String{prefix})) {
        return Refuse("first argument must be --test-settings-dir=<fresh absolute directory>, followed by -regtest and -datadir=<pinned fixture>");
    }
    const QString settings_arg{QFile::decodeName(argv[1]).mid(static_cast<int>(std::char_traits<char>::length(prefix)))};
    const QString settings_dir{ExistingAbsoluteDirectory(settings_arg)};
    if (settings_dir.isEmpty() || !QFileInfo{settings_dir}.isWritable() ||
        !QDir{settings_dir}.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System).isEmpty()) {
        return Refuse("settings directory must already exist, be absolute, writable and empty; no files are removed or reused");
    }
    const QString pinned_dir{ExistingAbsoluteDirectory(QString::fromUtf8(B3_FLOWMESHCLIENT_GUI_TEST_DATADIR))};
    if (pinned_dir.isEmpty() || QDir{pinned_dir}.isRoot()) {
        return Refuse("configure B3_FLOWMESHCLIENT_GUI_TEST_DATADIR to the exact disposable fixture directory before building this target");
    }

    bool regtest{false};
    QString datadir;
    const QStringList forbidden{
        QStringLiteral("chain"), QStringLiteral("main"), QStringLiteral("testnet"),
        QStringLiteral("testnet4"), QStringLiteral("signet"), QStringLiteral("conf"),
        QStringLiteral("includeconf"), QStringLiteral("blocksdir"), QStringLiteral("walletdir"),
        QStringLiteral("settings"), QStringLiteral("rpccookiefile"), QStringLiteral("pid"),
        QStringLiteral("debuglogfile"), QStringLiteral("loadblock"), QStringLiteral("choosedatadir"),
        QStringLiteral("resetguisettings"), QStringLiteral("test-settings-dir")};
    for (int i{2}; i < argc; ++i) {
        QString arg{QFile::decodeName(argv[i])};
        if (!arg.startsWith(QLatin1Char('-'))) return Refuse("positional arguments and payment URIs are not accepted by this test launcher");
        // ArgsManager stops parsing at a lone '-'; accepting one could hide
        // the required regtest/datadir arguments from the real application.
        if (arg == QStringLiteral("-") || arg == QStringLiteral("--")) return Refuse("argument terminators cannot precede or hide the fixture arguments");
        if (arg.startsWith(QStringLiteral("--"))) arg.remove(0, 1);
        const auto equals{arg.indexOf(QLatin1Char('='))};
        const QString key{arg.mid(1, equals < 0 ? -1 : equals - 1).toLower()};
        const QString value{equals < 0 ? QString{} : arg.mid(equals + 1)};
        const QString positive_key{key.startsWith(QStringLiteral("no")) ? key.mid(2) : key};
        if (forbidden.contains(positive_key)) return Refuse("alternate chains, external storage/config overrides and directory-selection options are forbidden");
        if (positive_key == QStringLiteral("regtest")) {
            if (regtest || key != positive_key || (equals >= 0 && value != QStringLiteral("1"))) return Refuse("exactly one explicit -regtest or -regtest=1 is required");
            regtest = true;
        } else if (positive_key == QStringLiteral("datadir")) {
            if (!datadir.isEmpty() || key != positive_key || equals < 0) return Refuse("exactly one explicit -datadir=<absolute fixture> is required");
            datadir = ExistingAbsoluteDirectory(value);
            if (datadir.isEmpty() || datadir != pinned_dir) return Refuse("datadir does not match this test binary's exact disposable fixture pin");
        } else if (positive_key == QStringLiteral("wallet")) {
            if (key != positive_key || value.contains(QLatin1Char('/')) || value.contains(QLatin1Char('\\')) || value.contains(QLatin1Char(':')) || value.contains(QStringLiteral(".."))) return Refuse("wallet arguments must be names within the disposable fixture, not external paths");
        }
    }
    if (!regtest || datadir.isEmpty()) return Refuse("both explicit regtest and the pinned datadir are required");
    const QString regtest_dir{ExistingAbsoluteDirectory(QDir{datadir}.filePath(QStringLiteral("regtest")))};
    if (regtest_dir.isEmpty() || !regtest_dir.startsWith(datadir + QLatin1Char('/'))) return Refuse("the pinned fixture must already contain its own nonsymlink-escaping regtest directory");
    if (settings_dir == datadir || settings_dir == regtest_dir) return Refuse("settings and chain-data directories must be distinct");

    // Both fallback scopes must be isolated. Changing only UserScope would
    // permit an organization/system INI fallback outside the test directory.
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir);
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings_dir);

    // Remove only the wrapper's first argument. All ordinary node/GUI arguments
    // and the real application's locking, RPC, wallet and shutdown paths stay
    // unchanged. The fixture config itself must be prepared/reviewed by the
    // test operator; this wrapper is an accidental-use guard, not a sandbox.
    for (int i{1}; i < argc - 1; ++i) argv[i] = argv[i + 1];
    --argc;
    argv[argc] = nullptr;
    return GuiMain(argc, argv);
}
