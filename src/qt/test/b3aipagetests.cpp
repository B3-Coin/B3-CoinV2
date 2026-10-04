// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.

#include <qt/b3aiclient.h>
#include <qt/b3ailessonstore.h>
#include <qt/b3aipage.h>

#include <QCheckBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
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
        QCOMPARE(tabs->count(), 4);
        QCOMPARE(tabs->tabText(3), QStringLiteral("Training"));
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

    void trainingReportDisplaysActualValuesAndRefreshes()
    {
        B3AiPage page{nullptr, path()};
        auto* refresh = page.findChild<QPushButton*>("aiRefreshTrainingReport");
        QVERIFY(refresh && !refresh->isEnabled());
        QVERIFY(page.findChild<QLabel*>("aiTrainingWarning")->text().contains(QStringLiteral("not qualified")));
        const QString reportPath{m_state->filePath(QStringLiteral("report.json"))};
        QVERIFY(writeReport(reportPath, trainingReport()));
        QVERIFY(page.loadTrainingReport(reportPath));
        QVERIFY(refresh->isEnabled());
        QCOMPARE(page.findChild<QLineEdit*>("aiTrainingReportPath")->text(), reportPath);
        QCOMPARE(page.findChild<QLabel*>("aiTrainingParameters")->text(), QLocale().toString(qint64{5395201}));
        QCOMPARE(page.findChild<QLabel*>("aiTrainingSavedUpdates")->text(), QLocale().toString(qint64{1000}));
        QCOMPARE(page.findChild<QLabel*>("aiTrainingTokens")->text(), QLocale().toString(qint64{3556921}));
        QCOMPARE(page.findChild<QLabel*>("aiTrainingCurrentLoss")->text(), QLocale().toString(2.02, 'g', 6));
        auto updated = trainingReport();
        updated.insert(QStringLiteral("status"), QStringLiteral("running"));
        updated.insert(QStringLiteral("attempted_steps"), 1100);
        updated.insert(QStringLiteral("training_tokens"), 3900000);
        auto history = updated.value(QStringLiteral("history")).toArray();
        history.append(QJsonObject{{QStringLiteral("step"), 1100}, {QStringLiteral("train"), 2.1},
            {QStringLiteral("validation"), 2.1}, {QStringLiteral("elapsed_seconds"), 290.0}});
        updated.insert(QStringLiteral("history"), history);
        updated.insert(QStringLiteral("elapsed_seconds"), 291.0);
        QVERIFY(writeReport(reportPath, updated));
        refresh->click();
        QCOMPARE(page.findChild<QLabel*>("aiTrainingAttemptedUpdates")->text(), QLocale().toString(qint64{1100}));
        QCOMPARE(page.findChild<QLabel*>("aiTrainingSavedUpdates")->text(), QLocale().toString(qint64{1000}));
        QCOMPARE(page.findChild<QLabel*>("aiTrainingCurrentLoss")->text(), QLocale().toString(2.1, 'g', 6));
        QCOMPARE(page.findChild<QLabel*>("aiTrainingBestLoss")->text(), QLocale().toString(2.02, 'g', 6));
        QVERIFY(!page.findChild<B3AiClient*>()->busy());
    }

    void continuedTrainingSeparatesLocalAndTotalProgress()
    {
        B3AiPage page{nullptr, path()};
        auto report = trainingReport();
        report.insert(QStringLiteral("base_step"), 1000);
        report.insert(QStringLiteral("base_training_tokens"), 3556921);
        report.insert(QStringLiteral("attempted_total_steps"), 2000);
        report.insert(QStringLiteral("selected_total_step"), 2000);
        report.insert(QStringLiteral("cumulative_training_tokens"), 7113842);
        const QString reportPath{m_state->filePath(QStringLiteral("report.json"))};
        QVERIFY(writeReport(reportPath, report));
        QVERIFY(page.loadTrainingReport(reportPath));
        const QString expected{QStringLiteral("%1 total · %2 in this run")
            .arg(QLocale().toString(qint64{2000}), QLocale().toString(qint64{1000}))};
        QCOMPARE(page.findChild<QLabel*>("aiTrainingSavedUpdates")->text(), expected);
        QCOMPARE(page.findChild<QLabel*>("aiTrainingAttemptedUpdates")->text(), expected);
        QCOMPARE(page.findChild<QLabel*>("aiTrainingTokens")->text(), QLocale().toString(qint64{7113842}));
        report.insert(QStringLiteral("selected_total_step"), 1999);
        QVERIFY(writeReport(reportPath, report));
        QVERIFY(!page.loadTrainingReport(reportPath));
        QCOMPARE(page.findChild<QLabel*>("aiTrainingTokens")->text(), QStringLiteral("—"));
    }

    void teachingReportsKeepFreshOptimizerAndInheritedUpdatesSeparate()
    {
        B3AiPage page{nullptr, path()};
        const QString reportPath{m_state->filePath(QStringLiteral("teaching-report.json"))};
        auto report = trainingReport();
        report.insert(QStringLiteral("stage"), QStringLiteral("b3-supervised-answers"));
        report.insert(QStringLiteral("optimizer_reset"), true);
        report.insert(QStringLiteral("inherited_language_steps"), 4000);
        report.insert(QStringLiteral("training_tokens"), 240000);
        QVERIFY(writeReport(reportPath, report));
        QVERIFY(page.loadTrainingReport(reportPath));
        const QString expected{QStringLiteral("%1 in B3 teaching · %2 prior language updates")
            .arg(QLocale().toString(qint64{1000}), QLocale().toString(qint64{4000}))};
        QCOMPARE(page.findChild<QLabel*>("aiTrainingSavedUpdates")->text(), expected);
        QCOMPARE(page.findChild<QLabel*>("aiTrainingAttemptedUpdates")->text(), expected);
        QCOMPARE(page.findChild<QLabel*>("aiTrainingTokens")->text(), QLocale().toString(qint64{240000}));
        QVERIFY(page.findChild<QLabel*>("aiTrainingStatus")->text().startsWith(QStringLiteral("B3 teaching · ")));
        report.insert(QStringLiteral("optimizer_reset"), false);
        QVERIFY(writeReport(reportPath, report));
        QVERIFY(!page.loadTrainingReport(reportPath));
        QCOMPARE(page.findChild<QLabel*>("aiTrainingSavedUpdates")->text(), QStringLiteral("—"));
        report.insert(QStringLiteral("optimizer_reset"), true);
        report.insert(QStringLiteral("base_step"), 4000);
        QVERIFY(writeReport(reportPath, report));
        QVERIFY(!page.loadTrainingReport(reportPath));
        report.remove(QStringLiteral("base_step"));
        report.insert(QStringLiteral("inherited_language_steps"), (1 << 24));
        QVERIFY(writeReport(reportPath, report));
        QVERIFY(!page.loadTrainingReport(reportPath));
    }

    void invalidTrainingSchemaClearsStaleProgress()
    {
        B3AiPage page{nullptr, path()};
        const QString reportPath{m_state->filePath(QStringLiteral("report.json"))};
        QList<QJsonObject> invalid;
        auto report = trainingReport();
        report.insert(QStringLiteral("stage"), QStringLiteral("sanity-dialogue"));
        invalid.append(report);
        report = trainingReport();
        report.insert(QStringLiteral("experimental"), false);
        invalid.append(report);
        report = trainingReport();
        report.insert(QStringLiteral("parameter_count"), 8000001);
        invalid.append(report);
        report = trainingReport();
        report.insert(QStringLiteral("attempted_steps"), 0.5);
        invalid.append(report);
        report = trainingReport();
        report.insert(QStringLiteral("selected_step"), 1001);
        invalid.append(report);
        report = trainingReport();
        report.insert(QStringLiteral("best_validation_loss"), -1.0);
        invalid.append(report);
        report = trainingReport();
        auto config = report.value(QStringLiteral("config")).toObject();
        config.insert(QStringLiteral("context_length"), 4097);
        report.insert(QStringLiteral("config"), config);
        invalid.append(report);
        report = trainingReport();
        auto history = report.value(QStringLiteral("history")).toArray();
        auto row = history.last().toObject();
        row.insert(QStringLiteral("validation"), QStringLiteral("not a number"));
        history.replace(history.size() - 1, row);
        report.insert(QStringLiteral("history"), history);
        invalid.append(report);
        for (const auto& value : invalid) {
            QVERIFY(writeReport(reportPath, trainingReport()));
            QVERIFY(page.loadTrainingReport(reportPath));
            QVERIFY(writeReport(reportPath, value));
            QVERIFY(!page.loadTrainingReport(reportPath));
            for (const QString& name : {QStringLiteral("aiTrainingParameters"), QStringLiteral("aiTrainingSavedUpdates"),
                    QStringLiteral("aiTrainingTokens"), QStringLiteral("aiTrainingCurrentLoss"), QStringLiteral("aiTrainingElapsed")}) {
                QCOMPARE(page.findChild<QLabel*>(name)->text(), QStringLiteral("—"));
            }
            QCOMPARE(page.findChild<QLabel*>("aiTrainingStatus")->text(), QStringLiteral("Report unavailable"));
            QVERIFY(!page.findChild<QLabel*>("aiTrainingNotice")->text().isEmpty());
        }
    }

    void malformedOversizedAndMissingTrainingReportsClearValues()
    {
        B3AiPage page{nullptr, path()};
        const QString reportPath{m_state->filePath(QStringLiteral("report.json"))};
        const QList<QByteArray> invalid{QByteArray{"{broken"}, QByteArray{"{\"stage\":\"language-pilot\",\"best_validation_loss\":NaN}"},
            QByteArray{"{\"stage\":\"language-pilot\",\"best_validation_loss\":1e999}"}, QByteArray(1024 * 1024 + 1, ' ')};
        for (const auto& bytes : invalid) {
            QVERIFY(writeReport(reportPath, trainingReport()));
            QVERIFY(page.loadTrainingReport(reportPath));
            QFile file{reportPath};
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(file.write(bytes), bytes.size());
            file.close();
            page.findChild<QPushButton*>("aiRefreshTrainingReport")->click();
            QCOMPARE(page.findChild<QLabel*>("aiTrainingTokens")->text(), QStringLiteral("—"));
            QCOMPARE(page.findChild<QLabel*>("aiTrainingStatus")->text(), QStringLiteral("Report unavailable"));
        }
        QVERIFY(writeReport(reportPath, trainingReport()));
        QVERIFY(page.loadTrainingReport(reportPath));
        QVERIFY(QFile::remove(reportPath));
        page.findChild<QPushButton*>("aiRefreshTrainingReport")->click();
        QCOMPARE(page.findChild<QLabel*>("aiTrainingParameters")->text(), QStringLiteral("—"));
        QVERIFY(!page.loadTrainingReport(m_state->path()));
    }

    void reportedBestMustMatchLowestObservedValidationLoss()
    {
        B3AiPage page{nullptr, path()};
        const QString reportPath{m_state->filePath(QStringLiteral("report.json"))};
        QVERIFY(writeReport(reportPath, trainingReport()));
        QVERIFY(page.loadTrainingReport(reportPath));
        auto report = trainingReport();
        auto history = report.value(QStringLiteral("history")).toArray();
        // The selected step and its claimed best loss agree with each other,
        // but another recorded evaluation was better. This must not appear
        // in the UI as a valid best-checkpoint selection.
        history.insert(1, QJsonObject{{QStringLiteral("step"), 500}, {QStringLiteral("train"), 2.0},
            {QStringLiteral("validation"), 1.9}, {QStringLiteral("elapsed_seconds"), 130.0}});
        report.insert(QStringLiteral("history"), history);
        QVERIFY(writeReport(reportPath, report));
        QVERIFY(!page.loadTrainingReport(reportPath));
        QCOMPARE(page.findChild<QLabel*>("aiTrainingBestLoss")->text(), QStringLiteral("—"));
        report.remove(QStringLiteral("best_validation_loss"));
        QVERIFY(writeReport(reportPath, report));
        QVERIFY(!page.loadTrainingReport(reportPath));
        QCOMPARE(page.findChild<QLabel*>("aiTrainingSavedUpdates")->text(), QStringLiteral("—"));
    }

    void trainingPathIsSessionOnlyAndReportStatusIsPlainText()
    {
        const QString reportPath{m_state->filePath(QStringLiteral("PRIVATE_SELECTED_TRAINING_REPORT.json"))};
        auto report = trainingReport();
        const QString status{QStringLiteral("<img src='https://example.invalid/pixel'>running")};
        report.insert(QStringLiteral("status"), status);
        QVERIFY(writeReport(reportPath, report));
        B3AiPage page{nullptr, path()};
        QVERIFY(page.loadTrainingReport(reportPath));
        auto* label = page.findChild<QLabel*>("aiTrainingStatus");
        QCOMPARE(label->text(), status);
        QCOMPARE(label->textFormat(), Qt::PlainText);
        for (const QString& name : QSettings{}.allKeys()) {
            QVERIFY(!QSettings{}.value(name).toString().contains(QStringLiteral("PRIVATE_SELECTED_TRAINING_REPORT")));
        }
        B3AiPage restarted{nullptr, path()};
        QVERIFY(restarted.findChild<QLineEdit*>("aiTrainingReportPath")->text().isEmpty());
        QVERIFY(!restarted.findChild<QPushButton*>("aiRefreshTrainingReport")->isEnabled());
    }

private:
    QJsonObject trainingReport() const
    {
        return {{QStringLiteral("stage"), QStringLiteral("language-pilot")},
            {QStringLiteral("experimental"), true},
            {QStringLiteral("status"), QStringLiteral("finished experiment; language quality not assessed")},
            {QStringLiteral("config"), QJsonObject{{QStringLiteral("vocab_size"), 257}, {QStringLiteral("context_length"), 2048},
                {QStringLiteral("width"), 256}, {QStringLiteral("layers"), 6}, {QStringLiteral("heads"), 8},
                {QStringLiteral("ff_multiplier"), 4}, {QStringLiteral("seed"), 42}}},
            {QStringLiteral("parameter_count"), 5395201}, {QStringLiteral("attempted_steps"), 1000},
            {QStringLiteral("selected_step"), 1000}, {QStringLiteral("training_tokens"), 3556921},
            {QStringLiteral("best_validation_loss"), 2.02}, {QStringLiteral("elapsed_seconds"), 265.8},
            {QStringLiteral("history"), QJsonArray{QJsonObject{{QStringLiteral("step"), 0}, {QStringLiteral("train"), 5.71},
                {QStringLiteral("validation"), 5.75}, {QStringLiteral("elapsed_seconds"), 1.2}},
                QJsonObject{{QStringLiteral("step"), 1000}, {QStringLiteral("train"), 2.09},
                {QStringLiteral("validation"), 2.02}, {QStringLiteral("elapsed_seconds"), 265.7}}}}};
    }
    bool writeReport(const QString& reportPath, const QJsonObject& report) const
    {
        QFile file{reportPath};
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        const QByteArray bytes{QJsonDocument(report).toJson()};
        return file.write(bytes) == bytes.size();
    }
    QString path() const { return m_state->filePath(QStringLiteral("lessons.json")); }
    std::unique_ptr<QTemporaryDir> m_state;
};

QTEST_MAIN(B3AiPageTests)
#include "b3aipagetests.moc"
