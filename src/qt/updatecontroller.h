// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3COIN_QT_UPDATECONTROLLER_H
#define B3COIN_QT_UPDATECONTROLLER_H

#include <update/manager.h>

#include <QObject>
#include <QString>

#include <atomic>
#include <memory>
#include <thread>

class QTimer;

/** Qt glue for the B3 Hive update system (doc/design/b3-hive-update-system.md).
 *
 *  All policy lives in src/update/ (tested without Qt); this layer only
 *  provides the Qt transport (which performs no policy of its own), the
 *  platform installer, QSettings persistence and thread marshalling.
 *  Wording rule: it is always a "B3 Hive update", never a "new wallet".
 *
 *  Unconfigured (no URL / no pinned keys) => quiet fail-closed: no timer,
 *  no network, no UI beyond a disabled section.
 */
class UpdateController : public QObject
{
    Q_OBJECT

public:
    explicit UpdateController(QObject* parent = nullptr);
    ~UpdateController();

    bool configured() const { return m_configured; }
    //! Cached GUI-thread snapshot (updated on operation completion).
    update::UpdateState state() const { return m_state; }
    QString installedVersion() const;
    QString latestVersion() const { return m_latest_version; }
    QString notesDigest() const { return m_notes_digest; }
    quint64 downloadSize() const { return m_download_size; }
    QString lastError() const { return m_error; }
    bool busy() const { return m_busy.load(); }
    bool installSupported() const;

    bool autoCheckEnabled() const;
    void setAutoCheckEnabled(bool enabled);

public Q_SLOTS:
    //! Automatic or "Check now". No-op when unconfigured or busy.
    void checkNow();
    //! Explicit user approval #1.
    void startDownload();
    //! Explicit user approval #2. This is inert unless the platform provides
    //! a complete replacement-and-restart implementation.
    void requestInstallAndRestart();
    //! Called by the application ONLY after the node fully shut down.
    void onNodeShutdownComplete();

Q_SIGNALS:
    void stateChanged();
    void shutdownRequested();

private:
    void ScheduleAutoCheck();
    void RunOnWorker(int op); // 0 = check, 1 = download
    void SyncSnapshot();

    std::unique_ptr<update::UpdateTransport> m_transport;
    std::unique_ptr<update::UpdateInstaller> m_installer;
    std::unique_ptr<update::SequenceStore> m_sequences;
    std::unique_ptr<update::UpdateManager> m_manager;
    std::thread m_worker;
    QTimer* m_auto_timer{nullptr};
    bool m_configured{false};
    std::atomic<bool> m_busy{false};

    update::UpdateState m_state{update::UpdateState::UNCONFIGURED};
    QString m_latest_version;
    QString m_notes_digest;
    quint64 m_download_size{0};
    QString m_error;
};

#endif // B3COIN_QT_UPDATECONTROLLER_H
