// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/updatecontroller.h>

#include <clientversion.h>
#include <common/args.h>
#include <random.h>
#include <update/downloader.h>
#include <util/strencodings.h>

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include <thread>

namespace {

constexpr int64_t AUTO_CHECK_BASE_SECONDS{6 * 60 * 60};
constexpr const char* SETTING_AUTO_CHECK{"fHiveUpdateAutoCheck"};
constexpr const char* SETTING_SEQUENCE{"nHiveUpdateSequence"};

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
            bool aborted{false};
            QObject::connect(reply, &QNetworkReply::readyRead, [&] {
                const QByteArray chunk{reply->readAll()};
                if (!sink(std::string_view{chunk.constData(), static_cast<size_t>(chunk.size())})) {
                    aborted = true;
                    reply->abort();
                }
            });
            QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            loop.exec();
            const QVariant redirect{reply->attribute(QNetworkRequest::RedirectionTargetAttribute)};
            const auto net_error{reply->error()};
            reply->deleteLater();
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

//! Platform installer. v1 implements macOS (verify the platform code
//! signature, then hand the artifact to the system opener as a detached
//! process with argv arrays); other platforms refuse cleanly. Never
//! elevates privileges.
class PlatformUpdateInstaller : public update::UpdateInstaller
{
public:
    bool Launch(const fs::path& artifact, std::string& error) override
    {
#ifdef Q_OS_MACOS
        const QString path{QString::fromStdString(fs::PathToString(artifact))};
        // Platform code-signature verification before anything runs.
        if (path.endsWith(QStringLiteral(".pkg"))) {
            if (QProcess::execute(QStringLiteral("/usr/sbin/spctl"),
                                  {QStringLiteral("--assess"), QStringLiteral("--type"),
                                   QStringLiteral("install"), path}) != 0) {
                error = "update-install-signature";
                return false;
            }
        } else if (QProcess::execute(QStringLiteral("/usr/bin/codesign"),
                                     {QStringLiteral("--verify"), path}) != 0) {
            error = "update-install-signature";
            return false;
        }
        if (!QProcess::startDetached(QStringLiteral("/usr/bin/open"), {path})) {
            error = "update-install-launch";
            return false;
        }
        return true;
#else
        (void)artifact;
        error = "update-install-unsupported-platform";
        return false;
#endif
    }
};

class QSettingsSequenceStore : public update::SequenceStore
{
public:
    uint64_t Load() override
    {
        return QSettings{}.value(SETTING_SEQUENCE, 0).toULongLong();
    }
    void Store(uint64_t sequence) override
    {
        QSettings{}.setValue(SETTING_SEQUENCE, static_cast<qulonglong>(sequence));
    }
};

update::UpdateConfig BuildConfig()
{
    update::UpdateConfig c;
    // Production values are explicit release inputs; nothing is invented
    // here. Unset => the whole system stays quietly disabled.
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
#if defined(Q_OS_MACOS)
    c.os = "macos";
    c.format = "dmg";
#elif defined(Q_OS_WIN)
    c.os = "windows";
    c.format = "exe";
#else
    c.os = "linux";
    c.format = "appimage";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    c.arch = "arm64";
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
    m_transport = std::make_unique<QtUpdateTransport>();
    m_installer = std::make_unique<PlatformUpdateInstaller>();
    m_sequences = std::make_unique<QSettingsSequenceStore>();
    update::UpdateConfig config{BuildConfig()};
    m_configured = config.Configured();
    if (m_configured) {
        std::error_code ec;
        fs::create_directories(config.download_dir, ec);
    }
    m_manager = std::make_unique<update::UpdateManager>(
        std::move(config), *m_transport, *m_installer, *m_sequences,
        [] { return static_cast<int64_t>(::time(nullptr)); });
    m_state = m_manager->state();
    if (m_configured && autoCheckEnabled()) ScheduleAutoCheck();
}

UpdateController::~UpdateController() = default;

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

void UpdateController::setAutoCheckEnabled(bool enabled)
{
    QSettings{}.setValue(SETTING_AUTO_CHECK, enabled);
    if (enabled && m_configured) ScheduleAutoCheck();
}

void UpdateController::ScheduleAutoCheck()
{
    // Randomized so a fleet never contacts the release host in unison.
    const int64_t delay{update::JitteredCheckInterval(AUTO_CHECK_BASE_SECONDS, FastRandomContext{}.rand64())};
    QTimer::singleShot(static_cast<int>(delay * 1000 > INT32_MAX ? INT32_MAX : delay * 1000), this, [this] {
        if (m_configured && autoCheckEnabled() && !m_busy.load()) checkNow();
        if (m_configured && autoCheckEnabled()) ScheduleAutoCheck();
    });
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
    std::thread{[this, op] {
        if (op == 0) {
            m_manager->CheckNow();
        } else {
            m_manager->StartDownload();
        }
        QMetaObject::invokeMethod(this, [this] {
            m_busy.store(false);
            SyncSnapshot();
        }, Qt::QueuedConnection);
    }}.detach();
}

void UpdateController::checkNow()
{
    if (!m_configured) return; // quiet fail-closed
    RunOnWorker(0);
}

void UpdateController::startDownload()
{
    if (m_manager->state() != update::UpdateState::UPDATE_AVAILABLE) return;
    RunOnWorker(1);
}

void UpdateController::requestInstallAndRestart()
{
    if (m_busy.load()) return;
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
