// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include "flowmeshclosedtest_policy.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrl>
#include <openssl/pem.h>
#include <memory>

#ifdef Q_OS_WIN
#include "flowmeshclosedtest_windows.h"
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace FlowMeshClosedTest {
namespace {
bool Fail(QString& error, const QString& text) { error = text; return false; }
bool CleanText(const QString& text, int maximum)
{
    if (text.isEmpty() || text.size() > maximum || text != text.trimmed()) return false;
    for (const auto c : text) if (c.unicode() < 32 || c.unicode() == 127) return false;
    return true;
}
bool PublicCertificate(const QByteArray& pem)
{
    // The Windows Qt dependency has no TLS backend. Native FlowMesh HTTPS
    // already uses OpenSSL, so certificate parsing must not depend on Qt TLS.
    const std::unique_ptr<BIO, decltype(&BIO_free)> input{BIO_new_mem_buf(pem.constData(), int(pem.size())), BIO_free};
    if (!input) return false;
    const std::unique_ptr<X509, decltype(&X509_free)> certificate{PEM_read_bio_X509(input.get(), nullptr, nullptr, nullptr), X509_free};
    return bool(certificate);
}
bool PrivatePeer(const QString& peer)
{
    static const QRegularExpression form{QStringLiteral(R"(^([0-9.]+):([0-9]{1,5})$)")};
    const auto match{form.match(peer)};
    if (!match.hasMatch()) return false;
    bool ok{false};
    const auto port{match.captured(2).toUInt(&ok)};
    if (!ok || port == 0 || port > 65535) return false;
    const QHostAddress address{match.captured(1)};
    const auto ip{address.toIPv4Address(&ok)};
    if (!ok || address.toString() != match.captured(1)) return false;
    return (ip >> 24) == 127 || (ip >> 24) == 10 ||
        (ip & 0xfff00000U) == 0xac100000U || (ip & 0xffff0000U) == 0xc0a80000U;
}
bool OwnedDirectory(const QString& path, bool create, QString& error)
{
#ifdef Q_OS_WIN
    WindowsStorage::PrivateSecurity security;
    if (!security.valid()) return Fail(error, QStringLiteral("Cannot establish private Windows storage permissions. Run as your normal user without elevation."));
    if (create && !CreateDirectoryW(WindowsStorage::Wide(QDir::toNativeSeparators(path)), security.get()) && GetLastError() != ERROR_ALREADY_EXISTS)
        return Fail(error, QStringLiteral("Cannot create the dedicated test directory."));
    WindowsStorage::Handle directory{WindowsStorage::Open(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING)};
    if (!directory.valid() || !WindowsStorage::PrivateObject(directory.get(), true, security))
        return Fail(error, QStringLiteral("Test directories must be owned by this user, private, and not reparse points."));
#else
    const auto name{QFile::encodeName(path)};
    struct stat st{};
    if (::lstat(name.constData(), &st) != 0) {
        if (!create || errno != ENOENT || ::mkdir(name.constData(), 0700) != 0)
            return Fail(error, QStringLiteral("Cannot create or inspect the dedicated test directory."));
        if (::lstat(name.constData(), &st) != 0)
            return Fail(error, QStringLiteral("Cannot inspect the created test directory."));
    }
    if (!S_ISDIR(st.st_mode) || st.st_uid != ::geteuid() || (st.st_mode & 0077) != 0)
        return Fail(error, QStringLiteral("Test directories must be owned by this user, private, and not symlinks."));
#endif
    return true;
}
bool ExactPrivateFile(const QString& path, const QByteArray& expected, bool create, QString& error)
{
#ifdef Q_OS_WIN
    WindowsStorage::PrivateSecurity security;
    if (!security.valid()) return Fail(error, QStringLiteral("Cannot establish private Windows storage permissions."));
    HANDLE raw{WindowsStorage::Open(path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING)};
    if (raw == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND && create) {
        WindowsStorage::Handle created{WindowsStorage::Open(path, GENERIC_READ | GENERIC_WRITE, 0, CREATE_NEW, security.get())};
        if (!created.valid() || !WindowsStorage::PrivateObject(created.get(), false, security))
            return Fail(error, QStringLiteral("Cannot exclusively create a private test profile file."));
        DWORD done{0};
        while (done < expected.size()) {
            DWORD count{0};
            if (!WriteFile(created.get(), expected.constData() + done, DWORD(expected.size() - done), &count, nullptr) || count == 0)
                return Fail(error, QStringLiteral("Cannot write the test profile file; no data was removed."));
            done += count;
        }
        if (!FlushFileBuffers(created.get())) return Fail(error, QStringLiteral("Cannot durably save the test profile file."));
        LARGE_INTEGER beginning{};
        if (!SetFilePointerEx(created.get(), beginning, nullptr, FILE_BEGIN)) return Fail(error, QStringLiteral("Cannot inspect the saved test profile file."));
        raw = created.release();
    }
    WindowsStorage::Handle file{raw}; quint64 size{0};
    if (!file.valid() || !WindowsStorage::PrivateObject(file.get(), false, security, &size) || size != quint64(expected.size()))
        return Fail(error, QStringLiteral("Missing, redirected or unsafe test profile file; nothing was overwritten."));
    QByteArray bytes;
    if (!WindowsStorage::Read(file.get(), size, bytes)) return Fail(error, QStringLiteral("Cannot read the bounded test profile file."));
#else
    const auto name{QFile::encodeName(path)};
    int fd{::open(name.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC)};
    if (fd < 0 && errno == ENOENT && create) {
        fd = ::open(name.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) return Fail(error, QStringLiteral("Cannot exclusively create a test profile file."));
        qsizetype done{0};
        while (done < expected.size()) {
            const auto n{::write(fd, expected.constData() + done, expected.size() - done)};
            if (n <= 0) { ::close(fd); return Fail(error, QStringLiteral("Cannot write the test profile file; no data was removed.")); }
            done += n;
        }
        const bool synced{::fsync(fd) == 0};
        ::close(fd);
        if (!synced) return Fail(error, QStringLiteral("Cannot durably save the test profile file."));
        fd = ::open(name.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    }
    if (fd < 0) return Fail(error, QStringLiteral("Missing or redirected test profile file; refusing to adopt existing data."));
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != ::geteuid() ||
        st.st_nlink != 1 || (st.st_mode & 0077) != 0 || st.st_size != expected.size()) {
        ::close(fd);
        return Fail(error, QStringLiteral("Test profile file ownership, type or size differs; nothing was overwritten."));
    }
    QByteArray bytes(expected.size(), '\0');
    qsizetype done{0};
    while (done < bytes.size()) {
        const auto n{::read(fd, bytes.data() + done, bytes.size() - done)};
        if (n <= 0) { ::close(fd); return Fail(error, QStringLiteral("Cannot read the bounded test profile file.")); }
        done += n;
    }
    ::close(fd);
#endif
    return bytes == expected || Fail(error, QStringLiteral("The embedded profile changed. Existing test data is preserved; coordinator review is required."));
}
bool PrivateNodeSettings(const QString& path, QString& error)
{
#ifdef Q_OS_WIN
    WindowsStorage::PrivateSecurity security;
    if (!security.valid()) return Fail(error, QStringLiteral("Cannot inspect private Windows storage permissions."));
    WindowsStorage::Handle file{WindowsStorage::Open(path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING)};
    if (!file.valid()) return GetLastError() == ERROR_FILE_NOT_FOUND || Fail(error, QStringLiteral("Cannot inspect the dedicated node settings file."));
    quint64 size{0};
    if (!WindowsStorage::PrivateObject(file.get(), false, security, &size) || size == 0 || size > 65536)
        return Fail(error, QStringLiteral("Dedicated node settings must be a private, bounded, same-user regular file without links."));
    QByteArray bytes;
    if (!WindowsStorage::Read(file.get(), size, bytes)) return Fail(error, QStringLiteral("Cannot read the bounded dedicated node settings."));
#else
    const auto name{QFile::encodeName(path)};
    struct stat st{};
    if (::lstat(name.constData(), &st) != 0)
        return errno == ENOENT || Fail(error, QStringLiteral("Cannot inspect the dedicated node settings file."));
    if (!S_ISREG(st.st_mode))
        return Fail(error, QStringLiteral("Dedicated node settings must be a regular file, not a link, directory or device."));
    const int fd{::open(name.constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)};
    if (fd < 0) return Fail(error, QStringLiteral("Dedicated node settings cannot be opened without following links."));
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != ::geteuid() ||
        st.st_nlink != 1 || (st.st_mode & 0077) != 0 || st.st_size <= 0 || st.st_size > 65536) {
        ::close(fd);
        return Fail(error, QStringLiteral("Dedicated node settings must be a private, bounded, same-user regular file without links."));
    }
    QByteArray bytes(st.st_size, '\0');
    qsizetype done{0};
    while (done < bytes.size()) {
        const auto n{::read(fd, bytes.data() + done, bytes.size() - done)};
        if (n <= 0) { ::close(fd); return Fail(error, QStringLiteral("Cannot read the bounded dedicated node settings.")); }
        done += n;
    }
    ::close(fd);
#endif
    QJsonParseError parse_error;
    const auto document{QJsonDocument::fromJson(bytes, &parse_error)};
    if (parse_error.error != QJsonParseError::NoError || !document.isObject())
        return Fail(error, QStringLiteral("Dedicated node settings are malformed; review the preserved file without resetting it."));
    const auto object{document.object()};
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (it.key() == QStringLiteral("_warning_") && it.value().isString() && it.value().toString().size() <= 2048) continue;
        if (it.key() == QStringLiteral("wallet") && it.value().isArray()) {
            const auto wallets{it.value().toArray()};
            if (wallets.isEmpty() || wallets == QJsonArray{QStringLiteral("closed-test")}) continue;
        }
        return Fail(error, QStringLiteral("Dedicated node settings contain an unsupported option or wallet. No settings or wallet data were changed."));
    }
    return true;
}
} // namespace

bool ParseProfile(const QByteArray& json, Profile& result, QString& error)
{
    result = {};
    if (json.isEmpty() || json.size() > 32768) return Fail(error, QStringLiteral("Embedded test profile missing or oversized."));
    QJsonParseError parse_error;
    const auto document{QJsonDocument::fromJson(json, &parse_error)};
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) return Fail(error, QStringLiteral("Embedded test profile is not a JSON object."));
    const auto object{document.object()};
    const bool public_session{object.value(QStringLiteral("schema")) == QJsonValue{2}};
    QStringList allowed{QStringLiteral("schema"), QStringLiteral("ready"), QStringLiteral("profile_id"),
        QStringLiteral("network"), QStringLiteral("regtest_profile"), QStringLiteral("reason"), QStringLiteral("operator"),
        QStringLiteral("availability"), QStringLiteral("b3_peer"), QStringLiteral("https_endpoints"), QStringLiteral("ca_pem")};
    if (public_session) allowed.push_back(QStringLiteral("public_session_approval"));
    for (auto i = object.begin(); i != object.end(); ++i)
        if (!allowed.contains(i.key())) return Fail(error, QStringLiteral("Embedded profile contains unsupported fields."));
    static const QRegularExpression id{QStringLiteral("^[a-z0-9][a-z0-9-]{0,47}$")};
    if ((!public_session && object.value(QStringLiteral("schema")) != QJsonValue{1}) || !object.value(QStringLiteral("ready")).isBool() ||
        !id.match(object.value(QStringLiteral("profile_id")).toString()).hasMatch() ||
        object.value(QStringLiteral("network")).toString() != QStringLiteral("regtest") ||
        object.value(QStringLiteral("regtest_profile")).toString() != QStringLiteral("flowmesh-client-v1"))
        return Fail(error, QStringLiteral("Only the named, isolated FlowMesh regtest profile is supported."));
    result.id = object.value(QStringLiteral("profile_id")).toString();
    result.hash = QCryptographicHash::hash(json, QCryptographicHash::Sha256).toHex();
    // Schema 2 is a single reviewed public session, not a general escape hatch
    // from the private-peer rule. Its identity, approval, peer, endpoint and CA
    // must all agree with this build-time allowlist.
    if (public_session && (result.id != QStringLiteral("vps-regtest-20260926-test4") ||
        object.value(QStringLiteral("public_session_approval")).toString() != result.id ||
        !object.value(QStringLiteral("ready")).toBool()))
        return Fail(error, QStringLiteral("The public regtest session requires its exact approved profile identity."));
    if (!object.value(QStringLiteral("ready")).toBool()) {
        result.reason = object.value(QStringLiteral("reason")).toString();
        if (!CleanText(result.reason, 500)) return Fail(error, QStringLiteral("Unready profile requires a clear bounded reason."));
        return true; // No filesystem, network, wallet, or key access.
    }
    if (!CleanText(object.value(QStringLiteral("operator")).toString(), 160) ||
        !CleanText(object.value(QStringLiteral("availability")).toString(), 300))
        return Fail(error, QStringLiteral("Test operator and availability must be explicitly identified."));
    result.peer = object.value(QStringLiteral("b3_peer")).toString();
    if (public_session) {
        if (result.peer != QStringLiteral("88.216.63.161:18547"))
            return Fail(error, QStringLiteral("The public regtest session peer differs from the approved address."));
    } else if (!PrivatePeer(result.peer)) return Fail(error, QStringLiteral("The B3 test peer must be one explicit private/loopback IPv4 address and port."));
    const auto endpoints{object.value(QStringLiteral("https_endpoints"))};
    if (!endpoints.isArray() || endpoints.toArray().isEmpty() || endpoints.toArray().size() > 2)
        return Fail(error, QStringLiteral("One or two reviewed HTTPS TEST endpoints are required."));
    for (const auto value : endpoints.toArray()) {
        const auto text{value.toString()};
        const QUrl url{text, QUrl::StrictMode};
        if (!CleanText(text, 1024) || !url.isValid() || url.scheme() != QStringLiteral("https") ||
            url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment() ||
            (url.port(-1) != -1 && (url.port() < 1 || url.port() > 65535)) ||
            text.contains(QLatin1Char('\\')) || result.endpoints.contains(text))
            return Fail(error, QStringLiteral("HTTPS TEST endpoints must be distinct strict URLs without credentials, query or fragment."));
        result.endpoints.push_back(text);
    }
    if (public_session && result.endpoints != QStringList{QStringLiteral("https://88.216.63.161:18580/flowmesh/v1")})
        return Fail(error, QStringLiteral("The public regtest session requires its single approved HTTPS endpoint."));
    result.ca = object.value(QStringLiteral("ca_pem")).toString().toUtf8();
    if (result.ca.size() > 16384 || result.ca.contains("PRIVATE KEY") ||
        !result.ca.startsWith("-----BEGIN CERTIFICATE-----\n") ||
        !result.ca.endsWith("-----END CERTIFICATE-----\n") ||
        !PublicCertificate(result.ca))
        return Fail(error, QStringLiteral("A bounded public PEM CA certificate is required; private keys are never accepted."));
    if (public_session && QCryptographicHash::hash(result.ca, QCryptographicHash::Sha256).toHex() !=
        QByteArrayLiteral("03d252bba9e5723f943415a74038d9367ffe0bfd9e6a683d3a65d43922b23d83"))
        return Fail(error, QStringLiteral("The public regtest session CA differs from the approved public certificate."));
    result.ready = true;
    return true;
}

bool CheckInvocation(int argc, QString& error)
{
    return argc == 1 || Fail(error, QStringLiteral("This CLOSED TEST app accepts no command-line options, wallet paths or payment URIs."));
}

Storage::~Storage()
{
#ifdef Q_OS_WIN
    if (lock_handle) CloseHandle(lock_handle);
#else
    if (lock_fd >= 0) ::close(lock_fd);
#endif
}

bool PrepareStorage(const Profile& profile, const QString& base, Storage& storage, QString& error)
{
    if (!profile.ready) return Fail(error, QStringLiteral("NOT READY: no test data or wallet will be opened."));
#ifdef Q_OS_WIN
    if (storage.lock_handle) return Fail(error, QStringLiteral("Storage guard is already active."));
    if (!WindowsStorage::PlainLocalAncestors(base))
        return Fail(error, QStringLiteral("The platform application-data parent must be local and cannot redirect through reparse points."));
#else
    if (storage.lock_fd >= 0) return Fail(error, QStringLiteral("Storage guard is already active."));
#endif
    const QFileInfo base_info{base};
    if (!base_info.isAbsolute() || !base_info.isDir() || base_info.canonicalFilePath() != QDir::cleanPath(base))
        return Fail(error, QStringLiteral("The platform application-data parent must exist and cannot redirect through symlinks."));
    const QString parent{QDir{base}.filePath(QStringLiteral("B3FlowMeshClosedTest"))};
    if (!OwnedDirectory(parent, true, error)) return false;
    storage.root = QDir{parent}.filePath(profile.id);
    const bool fresh{!QFileInfo::exists(storage.root)};
    if (!OwnedDirectory(storage.root, true, error)) return false;
    const QByteArray marker{QByteArrayLiteral("B3 FlowMesh CLOSED TEST\nprofile-sha256=") + profile.hash + '\n'};
    if (!ExactPrivateFile(QDir{storage.root}.filePath(QStringLiteral("profile.identity")), marker, fresh, error)) return false;

#ifdef Q_OS_WIN
    WindowsStorage::PrivateSecurity security;
    if (!security.valid()) return Fail(error, QStringLiteral("Cannot establish the private ownership lock."));
    // Deny sharing for the lifetime of Storage. A leftover filename is harmless;
    // the kernel handle owns the lock, and no stale lock file is ever deleted.
    WindowsStorage::Handle lock{WindowsStorage::Open(QDir{storage.root}.filePath(QStringLiteral("launcher.lock")),
                                                   GENERIC_READ | GENERIC_WRITE, 0, OPEN_ALWAYS, security.get())};
    if (!lock.valid() || !WindowsStorage::PrivateObject(lock.get(), false, security))
        return Fail(error, QStringLiteral("The dedicated test instance is already open or its ownership lock is unsafe. No lockfile was deleted."));
    storage.lock_handle = lock.release();
#else
    const auto lock_name{QFile::encodeName(QDir{storage.root}.filePath(QStringLiteral("launcher.lock")))};
    storage.lock_fd = ::open(lock_name.constData(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    struct stat lock_stat{};
    if (storage.lock_fd < 0 || ::fstat(storage.lock_fd, &lock_stat) != 0 || !S_ISREG(lock_stat.st_mode) ||
        lock_stat.st_uid != ::geteuid() || lock_stat.st_nlink != 1 || (lock_stat.st_mode & 0077) != 0 ||
        ::flock(storage.lock_fd, LOCK_EX | LOCK_NB) != 0)
        return Fail(error, QStringLiteral("The dedicated test instance is already open or its ownership lock is unsafe. No lockfile was deleted."));
#endif

    storage.node = QDir{storage.root}.filePath(QStringLiteral("node"));
    const QString network{QDir{storage.node}.filePath(QStringLiteral("regtest"))};
    storage.wallets = QDir{network}.filePath(QStringLiteral("wallets"));
    storage.settings = QDir{storage.root}.filePath(QStringLiteral("qt-settings"));
    for (const auto& path : {storage.node, network, storage.wallets, storage.settings})
        if (!OwnedDirectory(path, true, error)) return false;
    // Never silently select another wallet or follow an imported wallet link.
    const auto entries{QDir{storage.wallets}.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)};
    for (const auto& entry : entries)
        if (entry != QStringLiteral("closed-test")) return Fail(error, QStringLiteral("Unexpected wallet entry in the dedicated test wallet directory; review it without deleting data."));
    QDirIterator it{storage.wallets, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories};
    int count{0};
    while (it.hasNext()) {
        it.next();
        if (++count > 1024 || it.fileInfo().isSymLink())
            return Fail(error, QStringLiteral("Test wallet ownership/link inspection failed or exceeded its bound; no wallet was opened."));
#ifdef Q_OS_WIN
        WindowsStorage::Handle entry{WindowsStorage::Open(it.filePath(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, OPEN_EXISTING)};
        if (!entry.valid() || !WindowsStorage::PrivateObject(entry.get(), it.fileInfo().isDir(), security))
            return Fail(error, QStringLiteral("Test wallet ownership/link inspection failed; no wallet was opened."));
#else
        if (it.fileInfo().ownerId() != ::geteuid())
            return Fail(error, QStringLiteral("Test wallet ownership/link inspection failed; no wallet was opened."));
#endif
    }
    storage.ca = QDir{storage.root}.filePath(QStringLiteral("test-endpoint-ca.pem"));
    // The core must persist CreateWallet's load-on-startup setting. Never load
    // normal settings, and refuse redirected or unsupported dedicated content.
    const auto settings{QDir{storage.root}.filePath(QStringLiteral("node-settings.json"))};
    for (const auto& suffix : {QString{}, QStringLiteral(".tmp"), QStringLiteral(".bak"), QStringLiteral(".bak.tmp")})
        if (!PrivateNodeSettings(settings + suffix, error)) return false;
    return ExactPrivateFile(storage.ca, profile.ca, fresh, error);
}

QStringList NodeArguments(const Profile& profile, const Storage& storage)
{
    if (!profile.ready || storage.node.isEmpty()) return {};
    QStringList args{
        QStringLiteral("-regtest=1"), QStringLiteral("-enableflowmeshvalidator=0"), QStringLiteral("-flowmeshapi=0"),
        QStringLiteral("-server=0"), QStringLiteral("-listen=0"), QStringLiteral("-listenonion=0"),
        QStringLiteral("-discover=0"), QStringLiteral("-dnsseed=0"), QStringLiteral("-fixedseeds=0"),
        QStringLiteral("-noconf"), QStringLiteral("-noincludeconf"),
        QStringLiteral("-settings=") + QDir{storage.root}.filePath(QStringLiteral("node-settings.json")),
        QStringLiteral("-choosedatadir=0"), QStringLiteral("-disablewallet=0"),
        // List settings merge unless explicitly negated: suppress persisted
        // wallet entries before selecting the sole permitted test wallet.
        QStringLiteral("-nowallet"), QStringLiteral("-wallet=closed-test"),
        QStringLiteral("-datadir=") + storage.node, QStringLiteral("-walletdir=") + storage.wallets,
        QStringLiteral("-connect=") + profile.peer, QStringLiteral("-flowmeshendpointca=") + storage.ca,
        QStringLiteral("-b3modernregtest=1"), QStringLiteral("-b3flowmeshtest=1"),
        QStringLiteral("-b3corridorlength=130"), QStringLiteral("-b3corridorreward=2000000000000"),
        QStringLiteral("-b3blockinterval=1"), QStringLiteral("-b3roundseconds=1"),
        QStringLiteral("-b3epochlength=200"), QStringLiteral("-b3checkpointinterval=5"),
        QStringLiteral("-b3checkpointdepth=3"), QStringLiteral("-b3maxepochextension=200"),
        QStringLiteral("-b3minfinalityset=4"), QStringLiteral("-fallbackfee=0.00001"),
        QStringLiteral("-addresstype=legacy"), QStringLiteral("-changetype=legacy"),
        QStringLiteral("-uacomment=flowmesh-closed-test-") + profile.id};
    for (const auto& endpoint : profile.endpoints) args.push_back(QStringLiteral("-flowmeshendpoint=") + endpoint);
    return args;
}
} // namespace FlowMeshClosedTest
