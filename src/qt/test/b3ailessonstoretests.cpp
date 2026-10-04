// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <qt/b3ailessonstore.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>

namespace {

QByteArray Read(const QString& path)
{
    QFile file{path};
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

bool Write(const QString& path, const QByteArray& bytes)
{
    QFile file{path};
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

} // namespace

class B3AiLessonStoreTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void draftsNeedExplicitApprovalAndSurviveRestart();
    void tamperedContentBlocksLaterSaves();
    void invalidSchemasAreRejected();
    void failedWritesRollBackEveryMutation();
    void concurrentDiskChangesArePreserved();
    void busyLockRejectsMutations();
    void builtinLessonsAreImmutableAndContextIsBounded();
    void canonicalContentHasStableIds();
};

void B3AiLessonStoreTests::draftsNeedExplicitApprovalAndSurviveRestart()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path{dir.filePath(QStringLiteral("lessons.json"))};
    B3AiLessonStore store{path};
    QString error;
    QVERIFY2(store.load(&error), qPrintable(error));
    const qsizetype seed_count{store.lessons().size()};
    QVERIFY(seed_count >= 3);
    QVERIFY(store.addDraft(QStringLiteral("Explain the zephyr mnemonic"), QStringLiteral("zephyr is a test fixture."),
                           QStringLiteral("teacher:test-fixture"), &error));
    const QString id{store.lessons().last().id};
    QVERIFY(!store.lessons().last().approved);
    QVERIFY(store.contextFor(QStringLiteral("zephyr")).isEmpty());
    B3AiLessonStore restarted{path};
    QVERIFY(restarted.load(&error));
    QCOMPARE(restarted.lessons().size(), seed_count + 1);
    QVERIFY(!restarted.lessons().last().approved);
    QVERIFY(restarted.approve(id, &error));
    QVERIFY(restarted.contextFor(QStringLiteral("zephyr")).contains(QStringLiteral("teacher:test-fixture")));
    QVERIFY(restarted.contextFor(QStringLiteral("zephyr")).contains(id));
    QVERIFY(restarted.contextFor(QStringLiteral("saffron")).isEmpty());
    QVERIFY(restarted.addDraft(QStringLiteral("zephyr unrelated draft"), QStringLiteral("SECRET_DRAFT"),
                               QStringLiteral("teacher:draft"), &error));
    QVERIFY(!restarted.contextFor(QStringLiteral("zephyr")).contains(QStringLiteral("SECRET_DRAFT")));
    B3AiLessonStore approved_restart{path};
    QVERIFY(approved_restart.load(&error));
    QVERIFY(approved_restart.contextFor(QStringLiteral("zephyr")).contains(id));
    QVERIFY(approved_restart.remove(id, &error));
    QVERIFY(approved_restart.contextFor(QStringLiteral("zephyr")).isEmpty());
    const QJsonObject disk{QJsonDocument::fromJson(Read(path)).object()};
    QCOMPARE(disk.value(QStringLiteral("version")).toInt(), 1);
    QCOMPARE(disk.value(QStringLiteral("lessons")).toArray().size(), 1);
}

void B3AiLessonStoreTests::tamperedContentBlocksLaterSaves()
{
    QTemporaryDir dir;
    const QString path{dir.filePath(QStringLiteral("lessons.json"))};
    B3AiLessonStore store{path};
    QVERIFY(store.addDraft(QStringLiteral("zephyr"), QStringLiteral("Original reference"), QStringLiteral("teacher:test")));
    QJsonObject root{QJsonDocument::fromJson(Read(path)).object()};
    QJsonArray rows{root.value(QStringLiteral("lessons")).toArray()};
    QJsonObject lesson{rows[0].toObject()};
    lesson.insert(QStringLiteral("answer"), QStringLiteral("Altered reference"));
    rows[0] = lesson;
    root.insert(QStringLiteral("lessons"), rows);
    const QByteArray tampered{QJsonDocument{root}.toJson()};
    QVERIFY(Write(path, tampered));
    B3AiLessonStore rejected{path};
    QString error;
    QVERIFY(!rejected.load(&error));
    QVERIFY(!error.isEmpty());
    QVERIFY(rejected.contextFor(QStringLiteral("zephyr")).isEmpty());
    QVERIFY(!rejected.addDraft(QStringLiteral("new lesson"), QStringLiteral("new answer"), QStringLiteral("teacher:test"), &error));
    QVERIFY(error.contains(QStringLiteral("unavailable")));
    QCOMPARE(Read(path), tampered);
}

void B3AiLessonStoreTests::invalidSchemasAreRejected()
{
    QTemporaryDir dir;
    const QString path{dir.filePath(QStringLiteral("lessons.json"))};
    B3AiLessonStore valid{path};
    QVERIFY(valid.addDraft(QStringLiteral("zephyr"), QStringLiteral("fixture"), QStringLiteral("teacher:test")));
    const QJsonObject base{QJsonDocument::fromJson(Read(path)).object()};
    QList<QJsonObject> invalid;
    QJsonObject unsupported{base};
    unsupported.insert(QStringLiteral("version"), 2);
    invalid.append(unsupported);
    QJsonObject wrong_type{base};
    QJsonArray rows{base.value(QStringLiteral("lessons")).toArray()};
    QJsonObject row{rows[0].toObject()};
    row.insert(QStringLiteral("approved"), QStringLiteral("true"));
    rows[0] = row;
    wrong_type.insert(QStringLiteral("lessons"), rows);
    invalid.append(wrong_type);
    QJsonObject duplicate{base};
    rows = base.value(QStringLiteral("lessons")).toArray();
    rows.append(rows[0]);
    duplicate.insert(QStringLiteral("lessons"), rows);
    invalid.append(duplicate);
    QJsonObject oversized{base};
    rows = base.value(QStringLiteral("lessons")).toArray();
    row = rows[0].toObject();
    row.insert(QStringLiteral("question"), QString(4097, QLatin1Char('x')));
    rows[0] = row;
    oversized.insert(QStringLiteral("lessons"), rows);
    invalid.append(oversized);
    for (const QJsonObject& value : invalid) {
        const QByteArray bytes{QJsonDocument{value}.toJson()};
        QVERIFY(Write(path, bytes));
        B3AiLessonStore store{path};
        QString error;
        QVERIFY(!store.load(&error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!store.addDraft(QStringLiteral("other"), QStringLiteral("answer"), QStringLiteral("source"), &error));
        QCOMPARE(Read(path), bytes);
    }
}

void B3AiLessonStoreTests::failedWritesRollBackEveryMutation()
{
    QTemporaryDir dir;
    const QString blocker{dir.filePath(QStringLiteral("blocked"))};
    QVERIFY(Write(blocker, QByteArray{"not a directory"}));
    B3AiLessonStore unavailable{blocker + QStringLiteral("/lessons.json")};
    const qsizetype seed_count{unavailable.lessons().size()};
    QString error;
    QVERIFY(!unavailable.addDraft(QStringLiteral("zephyr"), QStringLiteral("fixture"), QStringLiteral("teacher:test"), &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(unavailable.lessons().size(), seed_count);

    const QString path{dir.filePath(QStringLiteral("lessons.json"))};
    B3AiLessonStore store{path};
    QVERIFY(store.addDraft(QStringLiteral("zephyr"), QStringLiteral("fixture"), QStringLiteral("teacher:test")));
    const QString id{store.lessons().last().id};
    // A removed/externally changed target deterministically fails before an atomic write.
    QVERIFY(QFile::remove(path));
    QVERIFY(!store.approve(id, &error));
    QVERIFY(!store.lessons().last().approved);
    QVERIFY(!store.remove(id, &error));
    QCOMPARE(store.lessons().last().id, id);
    QCOMPARE(store.lessons().size(), seed_count + 1);
}

void B3AiLessonStoreTests::concurrentDiskChangesArePreserved()
{
    QTemporaryDir dir;
    const QString path{dir.filePath(QStringLiteral("lessons.json"))};
    B3AiLessonStore first{path};
    QVERIFY(first.addDraft(QStringLiteral("zephyr"), QStringLiteral("fixture"), QStringLiteral("teacher:test")));
    B3AiLessonStore second{path};
    QVERIFY(second.load());
    QVERIFY(first.addDraft(QStringLiteral("saffron"), QStringLiteral("fixture"), QStringLiteral("teacher:test")));
    const QByteArray changed{Read(path)};
    QString error;
    QVERIFY(!second.approve(second.lessons().last().id, &error));
    QVERIFY(error.contains(QStringLiteral("changed on disk")));
    QVERIFY(!second.lessons().last().approved);
    QCOMPARE(Read(path), changed);
    QVERIFY(second.load());
    QCOMPARE(second.lessons().size(), first.lessons().size());
}

void B3AiLessonStoreTests::builtinLessonsAreImmutableAndContextIsBounded()
{
    QTemporaryDir dir;
    B3AiLessonStore store{dir.filePath(QStringLiteral("lessons.json"))};
    const B3AiLesson seed{store.lessons().first()};
    QVERIFY(seed.approved);
    QString error;
    QVERIFY(!store.remove(seed.id, &error));
    QVERIFY(error.contains(QStringLiteral("immutable")));
    QVERIFY(!store.approve(seed.id, &error));
    QVERIFY(store.contextFor(QStringLiteral("B3 continuous blockchain")).contains(seed.source));
    for (int budget : {-1, 0, 1, 100, 500, 1000, 10000}) {
        const QString context{store.contextFor(QStringLiteral("B3 FlowMesh node anchor wallet"), budget)};
        QVERIFY(context.size() <= std::max(0, budget));
        if (!context.isEmpty()) {
            QVERIFY(context.contains(QStringLiteral("lesson_id")));
            QVERIFY(context.contains(QStringLiteral("baseline30825f9:")));
            QVERIFY(context.contains(QStringLiteral("quoted data")));
        }
    }
    QVERIFY(store.contextFor(QStringLiteral("what is it?")).isEmpty());
}

void B3AiLessonStoreTests::busyLockRejectsMutations()
{
    QTemporaryDir dir;
    const QString path{dir.filePath(QStringLiteral("lessons.json"))};
    B3AiLessonStore store{path};
    QVERIFY(store.addDraft(QStringLiteral("zephyr"), QStringLiteral("fixture"), QStringLiteral("teacher:test")));
    const QString id{store.lessons().last().id};
    const qsizetype count{store.lessons().size()};
    const QByteArray original{Read(path)};
    QLockFile lock{path + QStringLiteral(".lock")};
    QVERIFY(lock.tryLock(0));
    QString error;
    QVERIFY(!store.addDraft(QStringLiteral("saffron"), QStringLiteral("fixture"), QStringLiteral("teacher:test"), &error));
    QVERIFY(error.contains(QStringLiteral("busy")));
    QCOMPARE(store.lessons().size(), count);
    QVERIFY(!store.approve(id, &error));
    QVERIFY(!store.lessons().last().approved);
    QVERIFY(!store.remove(id, &error));
    QCOMPARE(store.lessons().size(), count);
    QCOMPARE(store.lessons().last().id, id);
    QCOMPARE(Read(path), original);
    lock.unlock();
    QVERIFY(store.approve(id, &error));
}

void B3AiLessonStoreTests::canonicalContentHasStableIds()
{
    QTemporaryDir dir;
    B3AiLessonStore store{dir.filePath(QStringLiteral("lessons.json"))};
    QVERIFY(store.addDraft(QStringLiteral("  zephyr\r\nfixture  "), QStringLiteral("  answer  "), QStringLiteral(" teacher:test ")));
    const B3AiLesson lesson{store.lessons().last()};
    QCOMPARE(lesson.question, QStringLiteral("zephyr\nfixture"));
    QCOMPARE(lesson.answer, QStringLiteral("answer"));
    QCOMPARE(lesson.id.size(), 64);
    QString error;
    QVERIFY(!store.addDraft(QStringLiteral("zephyr\nfixture"), QStringLiteral("answer"), QStringLiteral("teacher:test"), &error));
    QVERIFY(error.contains(QStringLiteral("already exists")));
    QVERIFY(!store.addDraft(QStringLiteral(""), QStringLiteral("answer"), QStringLiteral("source"), &error));
    QVERIFY(!store.addDraft(QStringLiteral("question"), QString(32769, QLatin1Char('x')), QStringLiteral("source"), &error));
    QVERIFY(!store.addDraft(QStringLiteral("question"), QStringLiteral("answer"), QStringLiteral(""), &error));
}

QTEST_APPLESS_MAIN(B3AiLessonStoreTests)
#include "b3ailessonstoretests.moc"
