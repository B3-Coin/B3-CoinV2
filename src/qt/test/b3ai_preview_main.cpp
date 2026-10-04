// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.

#include <qt/b3aipage.h>
#include <qt/b3shell.h>
#include <qt/b3theme.h>
#include <qt/b3topstatus.h>

#include <QApplication>
#include <QDir>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>

// Offline UI qualification. This executable creates no node, wallet, chain
// datadir or model connection, and uses disposable application settings.
int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QTemporaryDir state;
    if (!state.isValid()) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, state.path());
    QCoreApplication::setOrganizationName(QStringLiteral("B3-Assistant-Preview"));
    QCoreApplication::setApplicationName(QStringLiteral("Offline-UI"));
    B3Theme::apply(app);

    B3Shell shell;
    shell.setWindowTitle(QStringLiteral("B3 Student — offline UI preview"));
    shell.setWalletWidget(new QLabel(QStringLiteral("Offline assistant preview")));
    auto* page = new B3AiPage(nullptr, state.filePath(QStringLiteral("lessons.json")), &shell);
    shell.setAssistantPage(page);
    shell.topStatus()->setNetwork(QStringLiteral("Offline UI preview"), QStringLiteral("[regtest]"));
    shell.topStatus()->setConnections(0, false);
    shell.topStatus()->setSync(QStringLiteral("Offline preview"), -1);
    shell.showPage(B3Page::Assistant);
    shell.resize(1280, 860);
    shell.show();

    const QStringList args = app.arguments();
    const int capture = args.indexOf(QStringLiteral("--capture-dir"));
    if (capture >= 0 && capture + 1 < args.size()) {
        const QString output = args.at(capture + 1);
        if (!QDir().mkpath(output)) return 1;
        QTimer::singleShot(300, &shell, [&] {
            bool okay = true;
            auto* tabs = page->findChild<QTabWidget*>();
            const QStringList names{QStringLiteral("chat"), QStringLiteral("lessons"), QStringLiteral("connection")};
            for (int index = 0; index < names.size(); ++index) {
                if (tabs) tabs->setCurrentIndex(index);
                if (index == 1) {
                    if (auto* lessons = page->findChild<QListWidget*>(QStringLiteral("aiLessonList"))) lessons->setCurrentRow(0);
                } else if (index == 0) {
                    if (auto* composer = page->findChild<QPlainTextEdit*>(QStringLiteral("aiComposer"))) composer->setFocus();
                }
                app.processEvents();
                okay = shell.grab().save(QDir(output).filePath(names.at(index) + QStringLiteral(".png"))) && okay;
            }
            if (tabs) tabs->setCurrentIndex(0);
            if (auto* composer = page->findChild<QPlainTextEdit*>(QStringLiteral("aiComposer"))) composer->setFocus();
            shell.resize(760, 860);
            app.processEvents();
            okay = shell.grab().save(QDir(output).filePath(QStringLiteral("compact.png"))) && okay;
            app.exit(okay ? 0 : 1);
        });
    }
    return app.exec();
}
