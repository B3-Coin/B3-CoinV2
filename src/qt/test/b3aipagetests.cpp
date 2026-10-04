// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.

#include <qt/b3aiclient.h>
#include <qt/b3ailessonstore.h>
#include <qt/b3aipage.h>

#include <QCheckBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBrowser>

#include <memory>

class B3AiPageTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void init()
    {
        m_state = std::make_unique<QTemporaryDir>();
        QVERIFY(m_state->isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_state->path());
        QCoreApplication::setOrganizationName(QStringLiteral("B3-Assistant-UI-Tests"));
        QCoreApplication::setApplicationName(QStringLiteral("Isolated-UI"));
        QSettings{}.clear();
    }

    void disconnectedPageAndStarterDoNotInventResults()
    {
        B3AiPage page{nullptr, path()};
        auto* tabs = page.findChild<QTabWidget*>("aiTabs");
        auto* tools = page.findChild<QCheckBox*>("aiNodeTools");
        auto* send = page.findChild<QPushButton*>("aiSend");
        auto* starter = page.findChild<QPushButton*>("aiStarter1");
        auto* composer = page.findChild<QPlainTextEdit*>("aiComposer");
        auto* transcript = page.findChild<QTextBrowser*>("aiTranscript");
        QVERIFY(tabs && tools && send && starter && composer && transcript);
        QCOMPARE(tabs->count(), 3);
        QVERIFY(!tools->isChecked());
        QVERIFY(!tools->isEnabled());
        QVERIFY(!send->isEnabled());
        const QString before = transcript->toPlainText();
        starter->click();
        QVERIFY(composer->toPlainText().contains(QStringLiteral("node health")));
        QCOMPARE(transcript->toPlainText(), before);
        QVERIFY(!send->isEnabled());
        QVERIFY(!page.findChild<B3AiClient*>()->busy());
    }

    void draftNeedsReviewAndSurvivesRestart()
    {
        B3AiPage page{nullptr, path()};
        auto* question = page.findChild<QLineEdit*>("aiLessonQuestion");
        auto* answer = page.findChild<QPlainTextEdit*>("aiLessonAnswer");
        auto* source = page.findChild<QLineEdit*>("aiLessonSource");
        auto* save = page.findChild<QPushButton*>("aiSaveDraft");
        auto* approve = page.findChild<QPushButton*>("aiApproveLesson");
        auto* list = page.findChild<QListWidget*>("aiLessonList");
        QVERIFY(question && answer && source && save && approve && list);
        const int seeds = list->count();
        question->setText(QStringLiteral("Explain the zephyr fixture"));
        answer->setPlainText(QStringLiteral("Zephyr is a UI teaching fixture."));
        source->setText(QStringLiteral("teacher:UI-test"));
        QVERIFY(save->isEnabled());
        save->click();
        QCOMPARE(list->count(), seeds + 1);
        B3AiLessonStore stored{path()};
        QVERIFY(stored.load());
        QVERIFY(stored.contextFor(QStringLiteral("zephyr")).isEmpty());
        list->setCurrentRow(seeds);
        QVERIFY(approve->isEnabled());
        // An unsaved correction must not approve the different saved answer.
        answer->setPlainText(QStringLiteral("Changed but unsaved."));
        QVERIFY(!approve->isEnabled());
        list->setCurrentRow(0);
        list->setCurrentRow(seeds);
        QVERIFY(approve->isEnabled());
        approve->click();
        QVERIFY(stored.load());
        QVERIFY(stored.contextFor(QStringLiteral("zephyr")).contains(QStringLiteral("UI teaching fixture")));
        B3AiPage restarted{nullptr, path()};
        auto* restartedList = restarted.findChild<QListWidget*>("aiLessonList");
        QCOMPARE(restartedList->count(), seeds + 1);
        restartedList->setCurrentRow(seeds);
        QVERIFY(!restarted.findChild<QPushButton*>("aiApproveLesson")->isEnabled());
        restarted.findChild<QPushButton*>("aiRemoveLesson")->click();
        QVERIFY(stored.load());
        QVERIFY(stored.contextFor(QStringLiteral("zephyr")).isEmpty());
    }

    void corruptStoreCannotBeOverwritten()
    {
        QFile file{path()};
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray damaged{"{this is preserved evidence"};
        QCOMPARE(file.write(damaged), damaged.size());
        file.close();
        B3AiPage page{nullptr, path()};
        page.findChild<QLineEdit*>("aiLessonQuestion")->setText(QStringLiteral("zephyr"));
        page.findChild<QPlainTextEdit*>("aiLessonAnswer")->setPlainText(QStringLiteral("fixture"));
        page.findChild<QLineEdit*>("aiLessonSource")->setText(QStringLiteral("teacher:test"));
        auto* save = page.findChild<QPushButton*>("aiSaveDraft");
        QVERIFY(!save->isEnabled());
        QVERIFY(!page.findChild<QLabel*>("aiLessonNotice")->text().isEmpty());
        save->click();
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), damaged);
    }

    void modelOutputIsEscapedAndNeverAutoApproved()
    {
        B3AiPage page{nullptr, path()};
        auto* client = page.findChild<B3AiClient*>();
        auto* transcript = page.findChild<QTextBrowser*>("aiTranscript");
        const QString output = QStringLiteral("<img src='https://example.invalid/pixel'><b>literal text</b>");
        QVERIFY(QMetaObject::invokeMethod(client, "assistantMessage", Qt::DirectConnection, Q_ARG(QString, output)));
        QVERIFY(transcript->toPlainText().contains(output));
        QVERIFY(!transcript->openLinks());
        QVERIFY(!transcript->openExternalLinks());
        page.findChild<QPushButton*>("aiTeachAnswer")->click();
        QCOMPARE(page.findChild<QTabWidget*>("aiTabs")->currentIndex(), 1);
        QCOMPARE(page.findChild<QPlainTextEdit*>("aiLessonAnswer")->toPlainText(), output);
        QVERIFY(!page.findChild<QPushButton*>("aiApproveLesson")->isEnabled());
        QVERIFY(!QFile::exists(path()));
    }

    void credentialsAndToolConsentAreSessionOnly()
    {
        B3AiPage page{nullptr, path()};
        page.findChild<QLineEdit*>("aiEndpoint")->setText(QStringLiteral("http://127.0.0.1:8080/v1/chat/completions"));
        page.findChild<QLineEdit*>("aiModelName")->setText(QStringLiteral("test-model"));
        auto* key = page.findChild<QLineEdit*>("aiApiKey");
        QCOMPARE(key->echoMode(), QLineEdit::Password);
        key->setText(QStringLiteral("UNIT_TEST_SECRET"));
        page.findChild<QPushButton*>("aiApplyConnection")->click();
        QVERIFY(!page.findChild<B3AiClient*>()->busy());
        QSettings settings;
        QCOMPARE(settings.value(QStringLiteral("B3Assistant/model")).toString(), QStringLiteral("test-model"));
        for (const QString& name : settings.allKeys()) {
            QVERIFY(!settings.value(name).toString().contains(QStringLiteral("UNIT_TEST_SECRET")));
            QVERIFY(!name.contains(QStringLiteral("key"), Qt::CaseInsensitive));
        }
        B3AiPage restarted{nullptr, path()};
        QVERIFY(restarted.findChild<QLineEdit*>("aiApiKey")->text().isEmpty());
        QVERIFY(!restarted.findChild<QCheckBox*>("aiNodeTools")->isChecked());
    }

private:
    QString path() const { return m_state->filePath(QStringLiteral("lessons.json")); }
    std::unique_ptr<QTemporaryDir> m_state;
};

QTEST_MAIN(B3AiPageTests)
#include "b3aipagetests.moc"
