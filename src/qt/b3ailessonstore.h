// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_QT_B3AILESSONSTORE_H
#define B3COIN_QT_B3AILESSONSTORE_H

#include <QByteArray>
#include <QList>
#include <QString>

struct B3AiLesson {
    QString id;
    QString question;
    QString answer;
    QString source;
    bool approved{false};
};

/** Local, explicitly reviewed reference lessons; this does not train model weights.
 * IDs hash a compact JSON tuple of question/answer/source after trimming outer
 * whitespace and normalizing CR/CRLF line endings to LF; approval is not content.
 * Built-in source lessons never enter the custom JSON file. A failed or invalid
 * load prevents mutation until a subsequent successful load, preserving evidence.
 * The caller owns creation of the storage directory and the store's lifetime.
 */
class B3AiLessonStore
{
public:
    explicit B3AiLessonStore(const QString& path);

    bool load(QString* error = nullptr);
    bool addDraft(const QString& question, const QString& answer, const QString& source,
                  QString* error = nullptr);
    bool approve(const QString& id, QString* error = nullptr);
    bool remove(const QString& id, QString* error = nullptr);
    QList<B3AiLesson> lessons() const;
    QString contextFor(const QString& query, int maxChars = 10000) const;

private:
    bool ensureLoaded(QString* error);
    bool persist(QString* error);
    bool isBuiltin(const QString& id) const;

    QString m_path;
    QList<B3AiLesson> m_builtin;
    QList<B3AiLesson> m_custom;
    QByteArray m_disk_digest;
    QString m_load_error;
    bool m_loaded{false};
    bool m_disk_exists{false};
};

#endif // B3COIN_QT_B3AILESSONSTORE_H
