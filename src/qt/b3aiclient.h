// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_B3AICLIENT_H
#define BITCOIN_QT_B3AICLIENT_H

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

#include <atomic>
#include <functional>
#include <memory>

template <typename T> class QFutureWatcher;
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

/** Opt-in model service client. No configuration, credentials or chat are persisted. */
class B3AiClient : public QObject
{
    Q_OBJECT
public:
    using ToolExecutor = std::function<QJsonValue(const QString&, const QJsonObject&)>;

    explicit B3AiClient(QObject* parent = nullptr);
    ~B3AiClient() override;
    /** The executor runs on a worker thread and must not access GUI objects. */
    void setToolExecutor(ToolExecutor executor);
    /** Accept a service base URL, a /v1 base URL, or a full /chat/completions URL. */
    bool configure(const QUrl& endpoint, const QString& model, const QString& apiKey,
                   bool toolsEnabled, QString* error = nullptr);
    bool sendMessage(const QString& user, const QString& lessonContext, QString* error = nullptr);
    /** Abort any request and discard all in-memory conversation, including an incomplete turn. */
    void clearConversation();
    /** Before node shutdown, cancel the session, drain node reads, and detach all node tools. */
    void stopAndDrain();
    bool busy() const { return m_busy; }

Q_SIGNALS:
    void assistantMessage(const QString& message);
    void toolResult(const QString& name, const QString& json);
    void errorOccurred(const QString& error);
    void busyChanged(bool busy);

private:
    struct ToolBatchResult {
        enum class Failure { None, Exception, Undefined, Oversized, Cancelled };
        QList<QByteArray> results;
        Failure failure{Failure::None};
    };
    void postRequest();
    void processResponse(const QByteArray& response);
    void fail(const QString& error);
    void cancelRequest();
    void finish(const QString& content, const QJsonObject& message);
    QJsonArray messages() const;

    QNetworkAccessManager* m_network{nullptr};
    QTimer* m_timeout{nullptr};
    QPointer<QNetworkReply> m_reply;
    QUrl m_endpoint;
    QString m_model;
    QString m_api_key;
    ToolExecutor m_executor;
    bool m_tools_enabled{false};
    bool m_busy{false};
    quint64 m_session{0};
    int m_rounds{0};
    int m_tool_calls{0};
    QByteArray m_response;
    QString m_lesson_context;
    QList<QJsonArray> m_turns;
    QJsonArray m_pending_turn;
    QList<QString> m_tool_ids;
    QList<QFutureWatcher<ToolBatchResult>*> m_tool_jobs;
    std::shared_ptr<std::atomic_bool> m_tool_cancel;
};

#endif // BITCOIN_QT_B3AICLIENT_H
