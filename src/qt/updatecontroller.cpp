// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/updatecontroller.h>

#include <clientversion.h>
#include <common/args.h>
#include <random.h>
#include <update/downloader.h>
#include <util/strencodings.h>
#include <util/string.h>

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include <thread>

namespace {

constexpr int64_t AUTO_CHECK_BASE_SECONDS{6 * 60 * 60};
constexpr int NETWORK_TIMEOUT_MS{30 * 1000};
constexpr int INITIAL_CHECK_DELAY_MS{5 * 1000};
constexpr const char* SETTING_AUTO_CHECK{"fHiveUpdateAutoCheck"};
constexpr const char* SETTING_SEQUENCE{"nHiveUpdateSequence"};
constexpr const char* SETTING_SEQUENCE_RECORD{"strHiveUpdateSequenceState"};

//! Qt transport: NO policy of its own — redirects are surfaced to the core
//! policy callback before being followed; the body streams through sink.
class QtUpdateTransport : public update::UpdateTransport
{
public:
    bool Fetch(const std::string& url, const std::function<bool(std::string_view)>& sink,
               const std::function<bool(const std::string&)>& on_redirect,
               std::string& error) override
    {
        QNetworkAccessManager net; // created on the calling (worker) thread
        QString current{QString::fromStdString(url)};
        for (int hop = 0; hop <= 4; ++hop) {
            QNetworkRequest req{QUrl{current}};
            req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::ManualRedirectPolicy);
            req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("B3Hive-Update"));
            QEventLoop loop;
            QNetworkReply* reply{net.get(req)};
            QTimer timeout;
            timeout.setSingleShot(true);
            bool aborted{false}, timed_out{false};
            QObject::connect(reply, &QNetworkReply::readyRead, [&] {
                // Redirect response bodies are not part of the requested
                // artifact and must never be forwarded to the verified sink.
                if (reply->attribute(QNetworkRequest::RedirectionTargetAttribute).isValid()) {
                    reply->readAll();
                    return;
                }
                const QByteArray chunk{reply->readAll()};
                if (!sink(std::string_view{chunk.constData(), static_cast<size_t>(chunk.size())})) {
                    aborted = true;
                    reply->abort();
                }
            });
            QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            QObject::connect(&timeout, &QTimer::timeout, [&] {
                timed_out = true;
                reply->abort();
            });
            timeout.start(NETWORK_TIMEOUT_MS);
            loop.exec();
            timeout.stop();
            const QVariant redirect{reply->attribute(QNetworkRequest::RedirectionTargetAttribute)};
            const auto net_error{reply->error()};
            reply->deleteLater();
            if (timed_out) { error = "update-fetch-timeout"; return false; }
            if (aborted) return false;
            if (redirect.isValid()) {
                const QString target{QUrl{current}.resolved(redirect.toUrl()).toString()};
                if (!on_redirect(target.toStdString())) { error = "update-fetch-redirect"; return false; }
                current = target;
                continue;
            }
            if (net_error != QNetworkReply::NoError) { error = "update-fetch-failed"; return false; }
            return true;
        }
        error = "update-fetch-redirect";
        return false;
    }
};

//! No platform has a complete, audited replacement-and-restart flow yet.
//! Opening a DMG/package is deliberately not represented as installation.
class PlatformUpdateInstaller : public update::UpdateInstaller
{
public:
    bool Supported() const override { return false; }

    bool Launch(const fs::path& artifact, std::string& error) override
    {
        (void)artifact;
        error = "update-install-unsupported-platform";
        return false;
    }
};

class QSettingsSequenceStore : public update::SequenceStore
{
public:
    bool Load(update::SequenceState& state, std::string& error) override
    {
        error.clear();
        state = {};
        QSettings settings;
        settings.sync();
        if (settings.status() != QSettings::NoError) {
            error = "update-sequence-load";
            return false;
        }

        if (!settings.contains(SETTING_SEQUENCE_RECORD)) {
            // One-way compatibility with the original floor-only setting.
            bool ok{false};
            const qulonglong floor{settings.value(SETTING_SEQUENCE, qulonglong{0}).toULongLong(&ok)};
            if (!ok) {
                error = "update-sequence-load";
                return false;
            }
            state.floor = static_cast<uint64_t>(floor);
            return true;
        }

        const QStringList fields{settings.value(SETTING_SEQUENCE_RECORD).toString().split(':')};
        bool floor_ok{false};
        if (fields.size() < 3 || fields[0] != QStringLiteral("v1")) {
            error = "update-sequence-state";
            return false;
        }
        const qulonglong floor{fields[1].toULongLong(&floor_ok)};
        if (!floor_ok) {
            error = "update-sequence-state";
            return false;
        }
        state.floor = static_cast<uint64_t>(floor);
        if (fields.size() == 3 && fields[2] == QStringLiteral("none")) return true;
        if (fields.size() != 4) {
            error = "update-sequence-state";
            return false;
        }
        bool pending_ok{false};
        const qulonglong pending{fields[2].toULongLong(&pending_ok)};
        const auto hash{uint256::FromHex(fields[3].toStdString())};
        if (!pending_ok || pending == 0 || pending != floor || !hash) {
            error = "update-sequence-state";
            return false;
        }
        state.pending = update::PendingOffer{static_cast<uint64_t>(pending), *hash};
        return true;
    }

    bool Store(const update::SequenceState& state, std::string& error) override
    {
        error.clear();
        if (state.pending &&
            (state.pending->sequence == 0 || state.pending->sequence != state.floor)) {
            error = "update-sequence-state";
            return false;
        }
        QString record{QStringLiteral("v1:%1:").arg(static_cast<qulonglong>(state.floor))};
        if (state.pending) {
            record += QStringLiteral("%1:%2")
                          .arg(static_cast<qulonglong>(state.pending->sequence))
                          .arg(QString::fromStdString(state.pending->payload_hash.GetHex()));
        } else {
            record += QStringLiteral("none");
        }
        QSettings settings;
        settings.setValue(SETTING_SEQUENCE_RECORD, record);
        settings.sync();
        if (settings.status() != QSettings::NoError ||
            settings.value(SETTING_SEQUENCE_RECORD).toString() != record) {
            error = "update-sequence-store";
            return false;
        }
        return true;
    }
};

update::UpdateConfig BuildConfig()
{
    update::UpdateConfig c;
    // Tagged releases use only the endpoint and authorities pinned at build
    // time. Runtime trust injection remains available solely to non-release
    // developer builds for offline/integration testing.
#if B3_UPDATE_CHANNEL_CONFIGURED
    c.manifest_url = B3_UPDATE_MANIFEST_URL;
    c.keys.threshold = B3_UPDATE_SIGNATURE_THRESHOLD;
    for (const std::string& hex : util::SplitString(B3_UPDATE_PUBLIC_KEYS, ',')) {
        const auto bytes{TryParseHex<unsigned char>(hex)};
        if (bytes && bytes->size() == 32) c.keys.keys.emplace_back(std::span<const unsigned char>{*bytes});
    }
    c.allowed_hosts = util::SplitString(B3_UPDATE_ALLOWED_HOSTS, ',');
#elif !CLIENT_VERSION_IS_RELEASE
    c.manifest_url = gArgs.GetArg("-hiveupdateurl", "");
    c.keys.threshold = static_cast<unsigned>(gArgs.GetIntArg("-hiveupdatethreshold", 2));
    for (const std::string& hex : gArgs.GetArgs("-hiveupdatekey")) {
        const auto bytes{TryParseHex<unsigned char>(hex)};
        if (bytes && bytes->size() == 32) c.keys.keys.emplace_back(std::span<const unsigned char>{*bytes});
    }
    for (const std::string& host : gArgs.GetArgs("-hiveupdatehost")) {
        c.allowed_hosts.push_back(host);
    }
    if (c.allowed_hosts.empty() && !c.manifest_url.empty()) {
        const std::string h{update::HttpsHost(c.manifest_url)};
        if (!h.empty()) c.allowed_hosts.push_back(h);
    }
#endif
#if defined(Q_OS_MACOS)
    c.os = "macos";
    c.format = "zip";
#elif defined(Q_OS_WIN)
    c.os = "windows";
    c.format = "exe";
#else
    c.os = "linux";
    c.format = "targz";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    c.arch = "arm64";
#elif defined(__i386__) || defined(_M_IX86)
    c.arch = "x86";
#else
    c.arch = "x86_64";
#endif
    // CLIENT_VERSION encodes MAJOR*10000 + MINOR*100 + BUILD.
    {
        update::Version v;
        v.major = CLIENT_VERSION / 10000;
        v.minor = (CLIENT_VERSION / 100) % 100;
        v.patch = CLIENT_VERSION % 100;
        c.installed = v;
    }
    const QString dir{QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
                      QStringLiteral("/hive-updates")};
    c.download_dir = fs::PathFromString(dir.toStdString());
    return c;
}

} // namespace

UpdateController::UpdateController(QObject* parent) : QObject{parent}
{
    m_auto_timer = new QTimer(this);
    m_auto_timer->setSingleShot(true);
    connect(m_auto_timer, &QTimer::timeout, this, [this] {
        if (m_configured && autoCheckEnabled() && !m_busy.load()) checkNow();
        if (m_configured && autoCheckEnabled()) ScheduleAutoCheck();
    });
    m_transport = std::make_unique<QtUpdateTransport>();
    m_installer = std::make_unique<PlatformUpdateInstaller>();
    m_sequences = std::make_unique<QSettingsSequenceStore>();
    update::UpdateConfig config{BuildConfig()};
    m_configured = config.Configured();
    if (m_configured) {
        std::error_code ec;
        fs::create_directories(config.download_dir, ec);
        if (ec) {
            m_configured = false;
            config.download_dir.clear();
        }
    }
    m_manager = std::make_unique<update::UpdateManager>(
        std::move(config), *m_transport, *m_installer, *m_sequences,
        [] { return static_cast<int64_t>(::time(nullptr)); });
    m_state = m_manager->state();
    m_error = QString::fromStdString(m_manager->last_error());
    if (m_configured && autoCheckEnabled()) {
        QTimer::singleShot(INITIAL_CHECK_DELAY_MS, this, [this] {
            if (m_configured && autoCheckEnabled() && !m_busy.load()) checkNow();
        });
        ScheduleAutoCheck();
    }
}

UpdateController::~UpdateController()
{
    if (m_auto_timer) m_auto_timer->stop();
    // The worker owns no QObject and is bounded by the network timeout. Join
    // before members are destroyed so shutdown can never race a detached
    // fetch against this controller or its manager.
    if (m_worker.joinable()) m_worker.join();
}

QString UpdateController::installedVersion() const
{
    return QStringLiteral("%1.%2.%3")
        .arg(CLIENT_VERSION / 10000)
        .arg((CLIENT_VERSION / 100) % 100)
        .arg(CLIENT_VERSION % 100);
}

bool UpdateController::autoCheckEnabled() const
{
    return QSettings{}.value(SETTING_AUTO_CHECK, true).toBool();
}

bool UpdateController::installSupported() const
{
    return m_manager && m_manager->install_supported();
}

void UpdateController::setAutoCheckEnabled(bool enabled)
{
    QSettings{}.setValue(SETTING_AUTO_CHECK, enabled);
    if (!m_auto_timer) return;
    if (enabled && m_configured) {
        ScheduleAutoCheck();
    } else {
        m_auto_timer->stop();
    }
}

void UpdateController::ScheduleAutoCheck()
{
    // Randomized so a fleet never contacts the release host in unison.
    const int64_t delay{update::JitteredCheckInterval(AUTO_CHECK_BASE_SECONDS, FastRandomContext{}.rand64())};
    const int milliseconds{static_cast<int>(delay * 1000 > INT32_MAX ? INT32_MAX : delay * 1000)};
    m_auto_timer->start(milliseconds);
}

void UpdateController::SyncSnapshot()
{
    m_state = m_manager->state();
    m_error = QString::fromStdString(m_manager->last_error());
    if (const update::Artifact* a{m_manager->available()}) {
        m_latest_version = QString::fromStdString(a->version.ToString());
        m_download_size = a->size;
    } else {
        m_latest_version.clear();
        m_download_size = 0;
    }
    if (const auto d{m_manager->notes_digest()}) {
        m_notes_digest = QString::fromStdString(d->GetHex());
    } else {
        m_notes_digest.clear();
    }
    Q_EMIT stateChanged();
}

void UpdateController::RunOnWorker(int op)
{
    bool expected{false};
    if (!m_busy.compare_exchange_strong(expected, true)) return;
    if (m_worker.joinable()) m_worker.join();
    m_state = op == 0 ? update::UpdateState::CHECKING : update::UpdateState::DOWNLOADING;
    Q_EMIT stateChanged();
    m_worker = std::thread{[this, op] {
        if (op == 0) {
            m_manager->CheckNow();
        } else {
            m_manager->StartDownload();
        }
        QMetaObject::invokeMethod(this, [this] {
            m_busy.store(false);
            SyncSnapshot();
        }, Qt::QueuedConnection);
    }};
}

void UpdateController::checkNow()
{
    if (!m_configured) return; // quiet fail-closed
    RunOnWorker(0);
}

void UpdateController::startDownload()
{
    if (m_state != update::UpdateState::UPDATE_AVAILABLE) return;
    RunOnWorker(1);
}

void UpdateController::requestInstallAndRestart()
{
    if (m_busy.load()) return;
    if (!installSupported()) return;
    if (!m_manager->RequestInstall()) return;
    SyncSnapshot();
    Q_EMIT shutdownRequested();
}

void UpdateController::onNodeShutdownComplete()
{
    // Only acts when an install was explicitly armed; the manager refuses
    // every other state, so this is safe to call on every shutdown.
    if (m_manager->state() != update::UpdateState::AWAITING_SHUTDOWN) return;
    m_manager->OnShutdownComplete();
}
