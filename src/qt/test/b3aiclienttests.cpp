// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <qt/b3aiclient.h>

#include <QHostAddress>
#include <QJsonDocument>
#include <QMap>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <stdexcept>

namespace {
QJsonObject ToolCall(const QString& name, const QString& id = QStringLiteral("call_1"),
                     const QString& arguments = QStringLiteral("{}"))
{
    return QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("type"), QStringLiteral("function")},
        {QStringLiteral("function"), QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("arguments"), arguments}}}};
}

QByteArray Completion(const QString& text, const QJsonArray& calls = QJsonArray{})
{
    QJsonObject message{{QStringLiteral("role"), QStringLiteral("assistant")},
        {QStringLiteral("content"), calls.isEmpty() ? QJsonValue(text) : QJsonValue(QJsonValue::Null)}};
    if (!calls.isEmpty()) message.insert(QStringLiteral("tool_calls"), calls);
    return QJsonDocument(QJsonObject{{QStringLiteral("choices"), QJsonArray{QJsonObject{
        {QStringLiteral("message"), message},
        {QStringLiteral("finish_reason"), calls.isEmpty() ? QStringLiteral("stop") : QStringLiteral("tool_calls")}}}}})
        .toJson(QJsonDocument::Compact);
}

/** In-process loopback model fixture. No B3 node, wallet, real key or external model is used. */
class MockModel : public QObject
{
public:
    struct Request { QByteArray target; QMap<QByteArray, QByteArray> headers; QJsonObject body; };
    struct Reply { QByteArray body; int status{200}; int delay_ms{0}; QByteArray extra_headers; };

    explicit MockModel(QObject* parent = nullptr) : QObject(parent), server(this)
    {
        listening = server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* socket = server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { read(socket); });
            }
        });
    }

    QUrl url() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort())); }
    void enqueue(const QByteArray& body, int status = 200, int delay_ms = 0, const QByteArray& headers = {})
    {
        replies.append(Reply{body, status, delay_ms, headers});
    }

    bool listening{false};
    QList<Request> requests;
    QList<Reply> replies;

private:
    void read(QTcpSocket* socket)
    {
        QByteArray bytes = socket->property("received").toByteArray() + socket->readAll();
        socket->setProperty("received", bytes);
        if (socket->property("handled").toBool()) return;
        const qsizetype boundary = bytes.indexOf("\r\n\r\n");
        if (boundary < 0) return;
        const QList<QByteArray> lines = bytes.left(boundary).split('\n');
        QMap<QByteArray, QByteArray> headers;
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const qsizetype colon = lines[i].indexOf(':');
            if (colon > 0) headers.insert(lines[i].left(colon).trimmed().toLower(), lines[i].mid(colon + 1).trimmed());
        }
        const qsizetype body_length = headers.value("content-length").toInt();
        if (bytes.size() - boundary - 4 < body_length) return;
        socket->setProperty("handled", true);
        requests.append(Request{lines.first().split(' ').value(1), headers,
            QJsonDocument::fromJson(bytes.mid(boundary + 4, body_length)).object()});
        const Reply reply = replies.isEmpty() ? Reply{QByteArray{"{}"}, 500, 0, {}} : replies.takeFirst();
        QPointer<QTcpSocket> guarded{socket};
        QTimer::singleShot(reply.delay_ms, this, [guarded, reply] {
            if (!guarded || guarded->state() != QAbstractSocket::ConnectedState) return;
            const QByteArray response = QByteArray{"HTTP/1.1 "} + QByteArray::number(reply.status) +
                " Fixture\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
                QByteArray::number(reply.body.size()) + "\r\n" + reply.extra_headers + "\r\n" + reply.body;
            guarded->write(response);
            guarded->disconnectFromHost();
        });
    }

    QTcpServer server;
};
} // namespace

class B3AiClientTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void configurationPolicy_data()
    {
        QTest::addColumn<QString>("url");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("https service") << QStringLiteral("https://model.example/v1") << true;
        QTest::newRow("IPv4 loopback") << QStringLiteral("http://127.0.0.1:11434") << true;
        QTest::newRow("IPv6 loopback") << QStringLiteral("http://[::1]:8000/v1/chat/completions") << true;
        QTest::newRow("unencrypted remote") << QStringLiteral("http://model.example/v1") << false;
        QTest::newRow("unencrypted LAN") << QStringLiteral("http://192.168.1.2:8000") << false;
        QTest::newRow("URL credentials") << QStringLiteral("https://user:secret@model.example/v1") << false;
        QTest::newRow("URL query") << QStringLiteral("https://model.example/v1?api_key=secret") << false;
        QTest::newRow("URL fragment") << QStringLiteral("https://model.example/v1#secret") << false;
        QTest::newRow("unsupported scheme") << QStringLiteral("file:///tmp/model") << false;
        QTest::newRow("relative URL") << QStringLiteral("/v1/chat/completions") << false;
        QTest::newRow("zero port") << QStringLiteral("http://127.0.0.1:0") << false;
    }

    void configurationPolicy()
    {
        QFETCH(QString, url);
        QFETCH(bool, accepted);
        B3AiClient client;
        QString error;
        QCOMPARE(client.configure(QUrl(url), QStringLiteral("fixture-model"), {}, false, &error), accepted);
        QCOMPARE(error.isEmpty(), accepted);
        QVERIFY(!client.busy());
    }

    void rejectsMissingConfigurationAndOversizedInputs()
    {
        MockModel model;
        QVERIFY(model.listening);
        B3AiClient client;
        QString error;
        QVERIFY(!client.sendMessage(QStringLiteral("hello"), {}, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!client.configure(model.url(), {}, {}, false, &error));
        QVERIFY(!client.configure(model.url(), QStringLiteral("fixture-model"), QStringLiteral("key\r\ninjected"), false, &error));
        QVERIFY(client.configure(model.url(), QStringLiteral("fixture-model"), {}, false, &error));
        QVERIFY(!client.sendMessage(QStringLiteral("  "), {}, &error));
        QVERIFY(!client.sendMessage(QString(9 * 1024, QLatin1Char('a')), {}, &error));
        QVERIFY(!client.sendMessage(QStringLiteral("hello"), QString(25 * 1024, QLatin1Char('b')), &error));
        QCOMPARE(model.requests.size(), 0);
    }

    void chatPreservesSuccessfulTurnsAndSourceContext()
    {
        MockModel model;
        QVERIFY(model.listening);
        model.enqueue(Completion(QStringLiteral("First answer")));
        model.enqueue(Completion(QStringLiteral("Second answer")));
        B3AiClient client;
        QVERIFY(client.configure(model.url(), QStringLiteral("fixture-model"), QStringLiteral("fixture-key"), false));
        QSignalSpy answers(&client, &B3AiClient::assistantMessage);
        QSignalSpy errors(&client, &B3AiClient::errorOccurred);
        QSignalSpy busy(&client, &B3AiClient::busyChanged);
        QVERIFY(client.sendMessage(QStringLiteral("First question"), QStringLiteral("[doc/example.md] Reviewed project knowledge.")));
        QTRY_COMPARE(answers.count(), 1);
        QVERIFY(!client.busy());
        QCOMPARE(answers.first().first().toString(), QStringLiteral("First answer"));
        QCOMPARE(model.requests.first().target, QByteArray{"/v1/chat/completions"});
        QCOMPARE(model.requests.first().headers.value("authorization"), QByteArray{"Bearer fixture-key"});
        QCOMPARE(model.requests.first().body.value(QStringLiteral("model")).toString(), QStringLiteral("fixture-model"));
        QVERIFY(!model.requests.first().body.contains(QStringLiteral("tools")));
        const QString prompt = model.requests.first().body.value(QStringLiteral("messages")).toArray().first().toObject().value(QStringLiteral("content")).toString();
        QVERIFY(prompt.contains(QStringLiteral("doc/example.md")));
        QVERIFY(prompt.contains(QStringLiteral("untrusted reference data")));
        QVERIFY(prompt.contains(QStringLiteral("Node tools are disabled")));
        QVERIFY(client.sendMessage(QStringLiteral("Second question"), QStringLiteral("[doc/current.md] New source context.")));
        QTRY_COMPARE(answers.count(), 2);
        const QJsonArray messages = model.requests.at(1).body.value(QStringLiteral("messages")).toArray();
        QCOMPARE(messages.size(), 4);
        QCOMPARE(messages.at(1).toObject().value(QStringLiteral("content")).toString(), QStringLiteral("First question"));
        QCOMPARE(messages.at(2).toObject().value(QStringLiteral("content")).toString(), QStringLiteral("First answer"));
        QCOMPARE(messages.at(3).toObject().value(QStringLiteral("content")).toString(), QStringLiteral("Second question"));
        QVERIFY(messages.first().toObject().value(QStringLiteral("content")).toString().contains(QStringLiteral("doc/current.md")));
        QCOMPARE(errors.count(), 0);
        QCOMPARE(busy.count(), 4);
    }

    void readOnlyToolFollowup_data()
    {
        QTest::addColumn<QString>("name");
        QTest::newRow("chain") << QStringLiteral("getblockchaininfo");
        QTest::newRow("network") << QStringLiteral("getnetworkinfo");
        QTest::newRow("finality") << QStringLiteral("getfinalitystatus");
        QTest::newRow("markets") << QStringLiteral("listflowmeshmarkets");
    }

    void readOnlyToolFollowup()
    {
        QFETCH(QString, name);
        MockModel model;
        QVERIFY(model.listening);
        model.enqueue(Completion({}, QJsonArray{ToolCall(name)}));
        model.enqueue(Completion(QStringLiteral("The observed fixture value is 42.")));
        B3AiClient client;
        int executions = 0;
        client.setToolExecutor([&](const QString& called, const QJsonObject& args) -> QJsonValue {
            ++executions;
            if (called != name || !args.isEmpty()) return QJsonValue(QJsonValue::Undefined);
            return QJsonObject{{QStringLiteral("fixture_value"), 42}};
        });
        QVERIFY(client.configure(model.url().resolved(QUrl(QStringLiteral("/v1"))), QStringLiteral("fixture-model"), {}, true));
        QSignalSpy answers(&client, &B3AiClient::assistantMessage);
        QSignalSpy tools(&client, &B3AiClient::toolResult);
        QSignalSpy errors(&client, &B3AiClient::errorOccurred);
        QVERIFY(client.sendMessage(QStringLiteral("Inspect the node"), {}));
        QTRY_COMPARE(answers.count(), 1);
        QCOMPARE(executions, 1);
        QCOMPARE(tools.count(), 1);
        QCOMPARE(tools.first().at(0).toString(), name);
        QCOMPARE(QJsonDocument::fromJson(tools.first().at(1).toString().toUtf8()).object().value(QStringLiteral("fixture_value")).toInt(), 42);
        QCOMPARE(model.requests.size(), 2);
        QCOMPARE(model.requests.at(0).target, QByteArray{"/v1/chat/completions"});
        const QJsonArray messages = model.requests.at(1).body.value(QStringLiteral("messages")).toArray();
        QCOMPARE(messages.size(), 4);
        const QJsonObject assistant = messages.at(2).toObject();
        QCOMPARE(assistant.value(QStringLiteral("role")).toString(), QStringLiteral("assistant"));
        QCOMPARE(assistant.value(QStringLiteral("tool_calls")).toArray().first().toObject().value(QStringLiteral("id")).toString(), QStringLiteral("call_1"));
        const QJsonObject result = messages.at(3).toObject();
        QCOMPARE(result.value(QStringLiteral("role")).toString(), QStringLiteral("tool"));
        QCOMPARE(result.value(QStringLiteral("tool_call_id")).toString(), QStringLiteral("call_1"));
        QCOMPARE(QJsonDocument::fromJson(result.value(QStringLiteral("content")).toString().toUtf8()).object().value(QStringLiteral("fixture_value")).toInt(), 42);
        const QJsonArray definitions = model.requests.first().body.value(QStringLiteral("tools")).toArray();
        QCOMPARE(definitions.size(), 4);
        QStringList allowed;
        for (const QJsonValue value : definitions) allowed.append(value.toObject().value(QStringLiteral("function")).toObject().value(QStringLiteral("name")).toString());
        QCOMPARE(allowed, QStringList({QStringLiteral("getblockchaininfo"), QStringLiteral("getnetworkinfo"), QStringLiteral("getfinalitystatus"), QStringLiteral("listflowmeshmarkets")}));
        QCOMPARE(errors.count(), 0);
    }

    void unsafeToolsNeverRun_data()
    {
        QTest::addColumn<QJsonArray>("calls");
        QTest::addColumn<bool>("enabled");
        QTest::newRow("write command") << QJsonArray{ToolCall(QStringLiteral("sendtoaddress"))} << true;
        QTest::newRow("extra arguments") << QJsonArray{ToolCall(QStringLiteral("getnetworkinfo"), QStringLiteral("call_1"), QStringLiteral("{\"wallet\":\"private\"}"))} << true;
        QTest::newRow("array arguments") << QJsonArray{ToolCall(QStringLiteral("getnetworkinfo"), QStringLiteral("call_1"), QStringLiteral("[]"))} << true;
        QTest::newRow("bad arguments JSON") << QJsonArray{ToolCall(QStringLiteral("getnetworkinfo"), QStringLiteral("call_1"), QStringLiteral("{"))} << true;
        QTest::newRow("tools disabled") << QJsonArray{ToolCall(QStringLiteral("getnetworkinfo"))} << false;
        QTest::newRow("whole batch validated") << QJsonArray{ToolCall(QStringLiteral("getnetworkinfo")), ToolCall(QStringLiteral("walletpassphrase"), QStringLiteral("call_2"))} << true;
        QTest::newRow("duplicate IDs") << QJsonArray{ToolCall(QStringLiteral("getnetworkinfo")), ToolCall(QStringLiteral("getblockchaininfo"))} << true;
        QTest::newRow("missing ID") << QJsonArray{ToolCall(QStringLiteral("getnetworkinfo"), {})} << true;
        QTest::newRow("too many calls") << QJsonArray{ToolCall(QStringLiteral("getnetworkinfo"), QStringLiteral("1")), ToolCall(QStringLiteral("getnetworkinfo"), QStringLiteral("2")), ToolCall(QStringLiteral("getnetworkinfo"), QStringLiteral("3")), ToolCall(QStringLiteral("getnetworkinfo"), QStringLiteral("4")), ToolCall(QStringLiteral("getnetworkinfo"), QStringLiteral("5"))} << true;
    }

    void unsafeToolsNeverRun()
    {
        QFETCH(QJsonArray, calls);
        QFETCH(bool, enabled);
        MockModel model;
        QVERIFY(model.listening);
        model.enqueue(Completion({}, calls));
        B3AiClient client;
        int executions = 0;
        client.setToolExecutor([&](const QString&, const QJsonObject&) -> QJsonValue { ++executions; return QJsonObject{}; });
        QVERIFY(client.configure(model.url(), QStringLiteral("fixture-model"), {}, enabled));
        QSignalSpy errors(&client, &B3AiClient::errorOccurred);
        QSignalSpy answers(&client, &B3AiClient::assistantMessage);
        QVERIFY(client.sendMessage(QStringLiteral("unsafe fixture"), {}));
        QTRY_COMPARE(errors.count(), 1);
        QCOMPARE(executions, 0);
        QCOMPARE(answers.count(), 0);
        QCOMPARE(model.requests.size(), 1);
        QVERIFY(!client.busy());
    }

    void providerFailureRollsBackTurn_data()
    {
        QTest::addColumn<QByteArray>("response");
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("headers");
        QTest::newRow("HTTP error") << QByteArray{"{\"error\":\"fixture-key should never be shown\"}"} << 401 << QByteArray{};
        QTest::newRow("provider error") << QByteArray{"{\"error\":{\"message\":\"fixture-key should never be shown\"}}"} << 200 << QByteArray{};
        QTest::newRow("malformed JSON") << QByteArray{"not JSON fixture-key"} << 200 << QByteArray{};
        QTest::newRow("wrong envelope") << QByteArray{"{\"choices\":[]}"} << 200 << QByteArray{};
        QTest::newRow("truncated answer") << QByteArray{"{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"partial\"},\"finish_reason\":\"length\"}]}"} << 200 << QByteArray{};
        QTest::newRow("redirect") << QByteArray{} << 302 << QByteArray{"Location: /redirected\r\n"};
        QTest::newRow("oversized reply") << QByteArray(1024 * 1024 + 1, 'x') << 200 << QByteArray{};
    }

    void providerFailureRollsBackTurn()
    {
        QFETCH(QByteArray, response);
        QFETCH(int, status);
        QFETCH(QByteArray, headers);
        MockModel model;
        QVERIFY(model.listening);
        model.enqueue(Completion(QStringLiteral("Known answer")));
        model.enqueue(response, status, 0, headers);
        model.enqueue(Completion(QStringLiteral("After failure")));
        B3AiClient client;
        QVERIFY(client.configure(model.url(), QStringLiteral("fixture-model"), QStringLiteral("fixture-key"), false));
        QSignalSpy errors(&client, &B3AiClient::errorOccurred);
        QSignalSpy answers(&client, &B3AiClient::assistantMessage);
        QVERIFY(client.sendMessage(QStringLiteral("Known question"), {}));
        QTRY_COMPARE(answers.count(), 1);
        QVERIFY(client.sendMessage(QStringLiteral("Failed question must disappear"), {}));
        QTRY_COMPARE(errors.count(), 1);
        QVERIFY(!errors.first().first().toString().contains(QStringLiteral("fixture-key")));
        QVERIFY(!client.busy());
        QVERIFY(client.sendMessage(QStringLiteral("New question"), {}));
        QTRY_COMPARE(answers.count(), 2);
        QCOMPARE(model.requests.size(), 3);
        const QJsonArray messages = model.requests.at(2).body.value(QStringLiteral("messages")).toArray();
        QCOMPARE(messages.size(), 4);
        QCOMPARE(messages.at(1).toObject().value(QStringLiteral("content")).toString(), QStringLiteral("Known question"));
        QCOMPARE(messages.at(2).toObject().value(QStringLiteral("content")).toString(), QStringLiteral("Known answer"));
        QCOMPARE(messages.at(3).toObject().value(QStringLiteral("content")).toString(), QStringLiteral("New question"));
    }

    void failedToolFollowupRollsBackEntireTurn()
    {
        MockModel model;
        QVERIFY(model.listening);
        model.enqueue(Completion({}, QJsonArray{ToolCall(QStringLiteral("getnetworkinfo"))}));
        model.enqueue(QByteArray{"{}"}, 500);
        model.enqueue(Completion(QStringLiteral("New answer")));
        B3AiClient client;
        client.setToolExecutor([](const QString&, const QJsonObject&) -> QJsonValue { return QJsonObject{{QStringLiteral("observed"), true}}; });
        QVERIFY(client.configure(model.url(), QStringLiteral("fixture-model"), {}, true));
        QSignalSpy errors(&client, &B3AiClient::errorOccurred);
        QSignalSpy answers(&client, &B3AiClient::assistantMessage);
        QSignalSpy tools(&client, &B3AiClient::toolResult);
        QVERIFY(client.sendMessage(QStringLiteral("Failed task"), {}));
        QTRY_COMPARE(errors.count(), 1);
        QCOMPARE(tools.count(), 1);
        QVERIFY(client.sendMessage(QStringLiteral("Fresh task"), {}));
        QTRY_COMPARE(answers.count(), 1);
        const QJsonArray messages = model.requests.at(2).body.value(QStringLiteral("messages")).toArray();
        QCOMPARE(messages.size(), 2);
        QCOMPARE(messages.last().toObject().value(QStringLiteral("content")).toString(), QStringLiteral("Fresh task"));
    }

    void resetCancelsAndSuppressesLateReply()
    {
        MockModel model;
        QVERIFY(model.listening);
        model.enqueue(Completion(QStringLiteral("Stale answer")), 200, 350);
        model.enqueue(Completion(QStringLiteral("Current answer")));
        B3AiClient client;
        QVERIFY(client.configure(model.url(), QStringLiteral("fixture-model"), {}, false));
        QSignalSpy answers(&client, &B3AiClient::assistantMessage);
        QSignalSpy errors(&client, &B3AiClient::errorOccurred);
        QVERIFY(client.sendMessage(QStringLiteral("Old question"), {}));
        QTRY_COMPARE(model.requests.size(), 1);
        QVERIFY(client.busy());
        QString error;
        QVERIFY(!client.sendMessage(QStringLiteral("Overlapping question"), {}, &error));
        client.clearConversation();
        QVERIFY(!client.busy());
        QVERIFY(client.sendMessage(QStringLiteral("Current question"), {}));
        QTRY_COMPARE(answers.count(), 1);
        QTest::qWait(450);
        QCOMPARE(answers.count(), 1);
        QCOMPARE(answers.first().first().toString(), QStringLiteral("Current answer"));
        QCOMPARE(errors.count(), 0);
        const QJsonArray messages = model.requests.at(1).body.value(QStringLiteral("messages")).toArray();
        QCOMPARE(messages.size(), 2);
        QCOMPARE(messages.last().toObject().value(QStringLiteral("content")).toString(), QStringLiteral("Current question"));
    }

    void configurationChangesDoNotShareOldConversation()
    {
        MockModel first;
        MockModel second;
        QVERIFY(first.listening && second.listening);
        first.enqueue(Completion(QStringLiteral("Private old answer")));
        first.enqueue(Completion(QStringLiteral("Model changed answer")));
        second.enqueue(Completion(QStringLiteral("Service changed answer")));
        B3AiClient client;
        QSignalSpy answers(&client, &B3AiClient::assistantMessage);
        QVERIFY(client.configure(first.url(), QStringLiteral("old-model"), {}, false));
        QVERIFY(client.sendMessage(QStringLiteral("Private old conversation"), {}));
        QTRY_COMPARE(answers.count(), 1);
        QVERIFY(client.configure(first.url(), QStringLiteral("new-model"), {}, false));
        QVERIFY(client.sendMessage(QStringLiteral("New model task"), {}));
        QTRY_COMPARE(answers.count(), 2);
        QCOMPARE(first.requests.at(1).body.value(QStringLiteral("messages")).toArray().size(), 2);
        QCOMPARE(first.requests.at(1).body.value(QStringLiteral("model")).toString(), QStringLiteral("new-model"));
        QVERIFY(client.configure(second.url(), QStringLiteral("new-model"), {}, false));
        QVERIFY(client.sendMessage(QStringLiteral("New service task"), {}));
        QTRY_COMPARE(answers.count(), 3);
        QCOMPARE(second.requests.first().body.value(QStringLiteral("messages")).toArray().size(), 2);
        QVERIFY(!QJsonDocument(second.requests.first().body).toJson().contains("Private old"));
    }

    void reconfigurationCancelsBusyRequest()
    {
        MockModel first;
        MockModel second;
        QVERIFY(first.listening && second.listening);
        first.enqueue(Completion(QStringLiteral("Old service answer")), 200, 350);
        second.enqueue(Completion(QStringLiteral("New service answer")));
        B3AiClient client;
        QSignalSpy answers(&client, &B3AiClient::assistantMessage);
        QVERIFY(client.configure(first.url(), QStringLiteral("fixture-model"), {}, false));
        QVERIFY(client.sendMessage(QStringLiteral("Old service task"), {}));
        QTRY_COMPARE(first.requests.size(), 1);
        QVERIFY(client.configure(second.url(), QStringLiteral("fixture-model"), {}, false));
        QVERIFY(!client.busy());
        QVERIFY(client.sendMessage(QStringLiteral("New service task"), {}));
        QTRY_COMPARE(answers.count(), 1);
        QTest::qWait(450);
        QCOMPARE(answers.count(), 1);
        QCOMPARE(answers.first().first().toString(), QStringLiteral("New service answer"));
        QCOMPARE(second.requests.first().body.value(QStringLiteral("messages")).toArray().size(), 2);
    }

    void boundedToolLoopStopsWithoutAResult()
    {
        MockModel model;
        QVERIFY(model.listening);
        for (int i = 0; i < 5; ++i) model.enqueue(Completion({}, QJsonArray{ToolCall(QStringLiteral("getnetworkinfo"), QString::number(i))}));
        model.enqueue(Completion(QStringLiteral("Fresh answer")));
        B3AiClient client;
        int executions = 0;
        client.setToolExecutor([&](const QString&, const QJsonObject&) -> QJsonValue { ++executions; return QJsonObject{}; });
        QVERIFY(client.configure(model.url(), QStringLiteral("fixture-model"), {}, true));
        QSignalSpy errors(&client, &B3AiClient::errorOccurred);
        QSignalSpy answers(&client, &B3AiClient::assistantMessage);
        QVERIFY(client.sendMessage(QStringLiteral("Looping task"), {}));
        QTRY_COMPARE(errors.count(), 1);
        QCOMPARE(model.requests.size(), 5);
        QCOMPARE(executions, 4);
        QVERIFY(!client.busy());
        QVERIFY(client.sendMessage(QStringLiteral("Fresh task"), {}));
        QTRY_COMPARE(answers.count(), 1);
        QCOMPARE(model.requests.last().body.value(QStringLiteral("messages")).toArray().size(), 2);
    }

    void toolFailuresAreContained_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("throws") << 0;
        QTest::newRow("undefined") << 1;
        QTest::newRow("oversized") << 2;
    }

    void toolFailuresAreContained()
    {
        QFETCH(int, kind);
        MockModel model;
        QVERIFY(model.listening);
        model.enqueue(Completion({}, QJsonArray{ToolCall(QStringLiteral("getnetworkinfo"))}));
        B3AiClient client;
        client.setToolExecutor([kind](const QString&, const QJsonObject&) -> QJsonValue {
            if (kind == 0) throw std::runtime_error("fixture-private-data");
            if (kind == 1) return QJsonValue(QJsonValue::Undefined);
            return QString(17 * 1024, QLatin1Char('x'));
        });
        QVERIFY(client.configure(model.url(), QStringLiteral("fixture-model"), {}, true));
        QSignalSpy errors(&client, &B3AiClient::errorOccurred);
        QSignalSpy tools(&client, &B3AiClient::toolResult);
        QVERIFY(client.sendMessage(QStringLiteral("Inspect fixture"), {}));
        QTRY_COMPARE(errors.count(), 1);
        QVERIFY(!errors.first().first().toString().contains(QStringLiteral("fixture-private-data")));
        QCOMPARE(tools.count(), 0);
        QCOMPARE(model.requests.size(), 1);
        QVERIFY(!client.busy());
    }

    void resetFromToolSignalPreventsFollowup()
    {
        MockModel model;
        QVERIFY(model.listening);
        model.enqueue(Completion({}, QJsonArray{ToolCall(QStringLiteral("getnetworkinfo"))}));
        B3AiClient client;
        client.setToolExecutor([](const QString&, const QJsonObject&) -> QJsonValue { return QJsonObject{}; });
        QVERIFY(client.configure(model.url(), QStringLiteral("fixture-model"), {}, true));
        connect(&client, &B3AiClient::toolResult, &client, [&] { client.clearConversation(); });
        QSignalSpy tools(&client, &B3AiClient::toolResult);
        QSignalSpy errors(&client, &B3AiClient::errorOccurred);
        QVERIFY(client.sendMessage(QStringLiteral("Cancelled fixture"), {}));
        QTRY_COMPARE(tools.count(), 1);
        QVERIFY(!client.busy());
        QTest::qWait(100);
        QCOMPARE(model.requests.size(), 1);
        QCOMPARE(errors.count(), 0);
    }

    void slowNodeCheckKeepsUiResponsiveAndIgnoresCancelledResult()
    {
        MockModel model;
        QVERIFY(model.listening);
        model.enqueue(Completion({}, QJsonArray{ToolCall(QStringLiteral("getnetworkinfo"))}));
        B3AiClient client;
        std::atomic_bool entered{false};
        std::atomic_bool exited{false};
        const QThread* main_thread = QThread::currentThread();
        std::atomic_bool ran_off_main{false};
        client.setToolExecutor([&](const QString&, const QJsonObject&) -> QJsonValue {
            ran_off_main.store(QThread::currentThread() != main_thread);
            entered.store(true);
            QThread::msleep(350);
            exited.store(true);
            return QJsonObject{{QStringLiteral("observed"), true}};
        });
        QVERIFY(client.configure(model.url(), QStringLiteral("fixture-model"), {}, true));
        QSignalSpy tools(&client, &B3AiClient::toolResult);
        QSignalSpy answers(&client, &B3AiClient::assistantMessage);
        QSignalSpy errors(&client, &B3AiClient::errorOccurred);
        QVERIFY(client.sendMessage(QStringLiteral("Slow node fixture"), {}));
        QTRY_VERIFY(entered.load());
        bool timer_ran_during_check = false;
        QTimer::singleShot(20, &client, [&] { timer_ran_during_check = !exited.load(); });
        QTRY_VERIFY(timer_ran_during_check);
        QVERIFY(ran_off_main.load());
        client.clearConversation();
        QVERIFY(!client.busy());
        QTRY_VERIFY(exited.load());
        QTest::qWait(100);
        QCOMPARE(tools.count(), 0);
        QCOMPARE(answers.count(), 0);
        QCOMPARE(errors.count(), 0);
        QCOMPARE(model.requests.size(), 1);
    }

    void destructionWaitsForNodeReadLifetime()
    {
        MockModel model;
        QVERIFY(model.listening);
        model.enqueue(Completion({}, QJsonArray{ToolCall(QStringLiteral("getnetworkinfo"))}));
        std::atomic_bool entered{false};
        std::atomic_bool exited{false};
        auto client = std::make_unique<B3AiClient>();
        client->setToolExecutor([&](const QString&, const QJsonObject&) -> QJsonValue {
            entered.store(true);
            QThread::msleep(200);
            exited.store(true);
            return QJsonObject{};
        });
        QVERIFY(client->configure(model.url(), QStringLiteral("fixture-model"), {}, true));
        QVERIFY(client->sendMessage(QStringLiteral("Node lifetime fixture"), {}));
        QTRY_VERIFY(entered.load());
        client.reset();
        QVERIFY(exited.load());
        QCOMPARE(model.requests.size(), 1);
    }

    void stopAndDrainWaitsForActiveReadAndDetachesTools()
    {
        MockModel model;
        QVERIFY(model.listening);
        model.enqueue(Completion({}, QJsonArray{ToolCall(QStringLiteral("getnetworkinfo")),
            ToolCall(QStringLiteral("getblockchaininfo"), QStringLiteral("call_2"))}));
        model.enqueue(Completion({}, QJsonArray{ToolCall(QStringLiteral("getnetworkinfo"))}));
        B3AiClient client;
        std::atomic_bool entered{false};
        std::atomic_bool exited{false};
        std::atomic_int executions{0};
        client.setToolExecutor([&](const QString&, const QJsonObject&) -> QJsonValue {
            ++executions;
            entered.store(true);
            QThread::msleep(250);
            exited.store(true);
            return QJsonObject{{QStringLiteral("observed"), true}};
        });
        QVERIFY(client.configure(model.url(), QStringLiteral("fixture-model"), {}, true));
        QSignalSpy tools(&client, &B3AiClient::toolResult);
        QSignalSpy answers(&client, &B3AiClient::assistantMessage);
        QSignalSpy errors(&client, &B3AiClient::errorOccurred);
        QVERIFY(client.sendMessage(QStringLiteral("Node task before shutdown"), {}));
        QTRY_VERIFY(entered.load());
        QVERIFY(!exited.load());
        client.stopAndDrain();
        QVERIFY(exited.load());
        QCOMPARE(executions.load(), 1); // The second queued command never starts.
        QVERIFY(!client.busy());
        QTest::qWait(100);
        QCOMPARE(tools.count(), 0);
        QCOMPARE(answers.count(), 0);
        QCOMPARE(errors.count(), 0);
        QCOMPARE(model.requests.size(), 1);

        QVERIFY(client.sendMessage(QStringLiteral("Task after node detach"), {}));
        QTRY_COMPARE(errors.count(), 1);
        QCOMPARE(executions.load(), 1);
        QCOMPARE(model.requests.size(), 2);
        QVERIFY(!model.requests.last().body.contains(QStringLiteral("tools")));
        QCOMPARE(model.requests.last().body.value(QStringLiteral("messages")).toArray().size(), 2);
        client.stopAndDrain(); // Repeated detach remains safe.
        QVERIFY(!client.busy());
    }
};

QTEST_GUILESS_MAIN(B3AiClientTests)
#include <qt/test/b3aiclienttests.moc>
