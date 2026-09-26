// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include "flowmeshclosedtest_policy.h"
#include <chainparamsbase.h>
#include <clientversion.h>
#include <common/args.h>
#include <univalue.h>
#include <util/translation.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <set>
#ifdef Q_OS_WIN
#include "flowmeshclosedtest_windows.h"
#include <winioctl.h>
#include <cstring>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

const TranslateFn G_TRANSLATION_FUN{nullptr};

using namespace FlowMeshClosedTest;
namespace {
// Public, expired-shortly certificate retained only as a parser unit vector.
// It is NOT an enabled package profile or a test endpoint availability claim.
constexpr auto TEST_CA = R"(-----BEGIN CERTIFICATE-----
MIIDHjCCAgagAwIBAgIUPKxdEX5/ebuhZTn14+QtPSEaP9wwDQYJKoZIhvcNAQEL
BQAwJzElMCMGA1UEAwwcRmxvd01lc2ggaXNvbGF0ZWQgcmVndGVzdCBDQTAeFw0y
NjA5MTMwNjU4MDlaFw0yNjA5MTUwNjU4MDlaMCcxJTAjBgNVBAMMHEZsb3dNZXNo
IGlzb2xhdGVkIHJlZ3Rlc3QgQ0EwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEK
AoIBAQDK+pEb5gkeXPLaK7TRh+WW3n+6E10la5BVKinB+9ReGgcZH0FoWMncX1+4
lwSZWiRxCUBXCFyNXySwJiwPXWYxYs4NMaEU9fkxMi3hYOe9L+12FtdbxsvLviUO
4GzG8p5gSh7SUGiAcszJnayvLTO7WWUKtx0lKF+/yjiizzgnF+ZKTuaH6CgBNLnU
hUXCNv0Hvfv+LaDi8uInxM3qx2d5U270RGNjoAD6O3vPXBa/PsC6JsIKl4J6SiM9
sA2c84BnKandADNfdLWSchLMs9exQSJbygshHxVJ2AASVyDUTxogazlvQBbr/m2q
fzof3vBCUazeKSMCl7J4P7UqRyMzAgMBAAGjQjBAMA8GA1UdEwEB/wQFMAMBAf8w
DgYDVR0PAQH/BAQDAgEGMB0GA1UdDgQWBBQvFmrD/l8hdu//sCRJViSt5DMInTAN
BgkqhkiG9w0BAQsFAAOCAQEATHtEZjcVMeAPlaaDOes8l+FH9YwwBzoJOP/6Esh4
jNMvmSjtKw+bN+mtlgGw7XmdD2LItrYy8qzp5i/5vMP/y9WNeECxC8PXHihpK70u
qId1oqSrQMZaAJgO3h/gVUr+j3Zp1JF3QzFz7bdNSYCOTB2ahKndmPagTYFNYo+s
8RexU9b8YRs+M/dqBf2u9R9d/cF5gAYBeuCtLIVtPWWdi16G6VZmuRovSsFlOMMZ
4/oqs5mXHptdMuh4SiPzTsGfgtO5+izl/sD4ffl+TnlaoYADxVuDLZCXB5jjnATM
cD+wIqBFy5sDlNg3EOMb7gdNhPspWeGbX+EjXWPcr7N6iA==
-----END CERTIFICATE-----
)";
QJsonObject Good()
{
    return {{"schema", 1}, {"ready", true}, {"profile_id", "unit-profile"},
        {"network", "regtest"}, {"regtest_profile", "flowmesh-client-v1"},
        {"operator", "Unit test only"}, {"availability", "No server is started by this test"},
        {"b3_peer", "127.0.0.1:12345"}, {"https_endpoints", QJsonArray{"https://localhost:12346"}},
        {"ca_pem", TEST_CA}};
}
QJsonObject PublicGood()
{
    QFile fixture{QStringLiteral(FLOWMESH_CLOSED_TEST_SOURCE_PROFILE_PATH)};
    if (!fixture.open(QIODevice::ReadOnly)) qFatal("approved public profile fixture is missing");
    return QJsonDocument::fromJson(fixture.readAll()).object();
}
QByteArray Json(const QJsonObject& object) { return QJsonDocument{object}.toJson(); }
Profile Parsed()
{
    Profile profile; QString error;
    if (!ParseProfile(Json(Good()), profile, error)) qFatal("valid unit profile rejected: %s", qPrintable(error));
    return profile;
}
QString Base(QTemporaryDir& temp) { return QFileInfo{temp.path()}.canonicalFilePath(); }
bool PrivateDirectory(const QString& path)
{
#ifdef Q_OS_WIN
    WindowsStorage::PrivateSecurity security;
    if (!security.valid()) return false;
    return CreateDirectoryW(WindowsStorage::Wide(QDir::toNativeSeparators(path)), security.get());
#else
    return QDir{}.mkdir(path) && ::chmod(QFile::encodeName(path).constData(), 0700) == 0;
#endif
}
bool HardLink(const QString& source, const QString& path)
{
#ifdef Q_OS_WIN
    return CreateHardLinkW(WindowsStorage::Wide(QDir::toNativeSeparators(path)), WindowsStorage::Wide(QDir::toNativeSeparators(source)), nullptr);
#else
    return ::link(QFile::encodeName(source).constData(), QFile::encodeName(path).constData()) == 0;
#endif
}
bool SymbolicLink(const QString& source, const QString& path, bool directory)
{
#ifdef Q_OS_WIN
    if (directory) {
        // Junction creation needs no symlink privilege. This ensures directory
        // reparse points and redirected ancestors are exercised on ordinary
        // Windows accounts as well as privileged CI runners.
        if (!QDir{}.mkdir(path)) return false;
        WindowsStorage::Handle junction{WindowsStorage::Open(path, GENERIC_WRITE, 0, OPEN_EXISTING)};
        if (!junction.valid()) return false;
        const QString substitute{QStringLiteral("\\??\\") + QDir::toNativeSeparators(source)};
        const QString display{QDir::toNativeSeparators(source)};
        struct Junction {
            DWORD tag;
            WORD data_length, reserved;
            WORD substitute_offset, substitute_length, display_offset, display_length;
            wchar_t names[1];
        };
        const qsizetype size{16 + (substitute.size() + display.size() + 2) * qsizetype(sizeof(wchar_t))};
        if (size > MAXIMUM_REPARSE_DATA_BUFFER_SIZE) return false;
        QByteArray bytes(size, '\0');
        auto* data{reinterpret_cast<Junction*>(bytes.data())};
        data->tag = IO_REPARSE_TAG_MOUNT_POINT;
        data->data_length = WORD(size - 8);
        data->substitute_length = WORD(substitute.size() * sizeof(wchar_t));
        data->display_offset = WORD(data->substitute_length + sizeof(wchar_t));
        data->display_length = WORD(display.size() * sizeof(wchar_t));
        std::memcpy(data->names, substitute.utf16(), data->substitute_length);
        std::memcpy(bytes.data() + 16 + data->display_offset, display.utf16(), data->display_length);
        DWORD count{0};
        return DeviceIoControl(junction.get(), FSCTL_SET_REPARSE_POINT, bytes.data(), DWORD(bytes.size()), nullptr, 0, &count, nullptr);
    }
    // Native links, not QFile::link's Windows .lnk shortcuts. Current Windows
    // supports the unprivileged flag when Developer Mode is enabled; retry
    // without it for older systems and privileged CI runners.
    const DWORD flags{directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : DWORD{0}};
    if (CreateSymbolicLinkW(WindowsStorage::Wide(QDir::toNativeSeparators(path)), WindowsStorage::Wide(QDir::toNativeSeparators(source)), flags | 0x2)) return true;
    return CreateSymbolicLinkW(WindowsStorage::Wide(QDir::toNativeSeparators(path)), WindowsStorage::Wide(QDir::toNativeSeparators(source)), flags);
#else
    Q_UNUSED(directory);
    return QFile::link(source, path);
#endif
}
#ifdef Q_OS_WIN
#define REQUIRE_SYMBOLIC_LINK(source, path, directory) do { \
    const bool linked{SymbolicLink(source, path, directory)}; \
    if (!linked && !directory && GetLastError() == ERROR_PRIVILEGE_NOT_HELD) \
        QSKIP("File symlink creation requires Developer Mode or the Windows symlink privilege; directory junction cases still run."); \
    QVERIFY(linked); \
} while (false)
#else
#define REQUIRE_SYMBOLIC_LINK(source, path, directory) QVERIFY(SymbolicLink(source, path, directory))
#endif
bool PublicReadPermission(const QString& path)
{
#ifdef Q_OS_WIN
    BYTE sid[SECURITY_MAX_SID_SIZE]; DWORD size{sizeof(sid)};
    if (!CreateWellKnownSid(WinWorldSid, nullptr, sid, &size)) return false;
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = FILE_GENERIC_READ;
    access.grfAccessMode = GRANT_ACCESS;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);
    PACL old_acl{nullptr}; PACL new_acl{nullptr}; PSECURITY_DESCRIPTOR descriptor{nullptr};
    auto native{QDir::toNativeSeparators(path)};
    auto name{reinterpret_cast<LPWSTR>(native.data())};
    if (GetNamedSecurityInfoW(name, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &old_acl, nullptr, &descriptor) != ERROR_SUCCESS) return false;
    const bool result{SetEntriesInAclW(1, &access, old_acl, &new_acl) == ERROR_SUCCESS &&
        SetNamedSecurityInfoW(name, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                             nullptr, nullptr, new_acl, nullptr) == ERROR_SUCCESS};
    if (new_acl) LocalFree(new_acl);
    LocalFree(descriptor);
    return result;
#else
    return ::chmod(QFile::encodeName(path).constData(), QFileInfo{path}.isDir() ? 0755 : 0644) == 0;
#endif
}
bool ConfigureArgs(ArgsManager& args, const QStringList& values, QString& error)
{
    std::set<std::string> registered;
    std::vector<std::string> strings{"closed-test-policy"};
    for (const auto& value : values) {
        auto name{value.section(QLatin1Char('='), 0, 0)};
        if (name.startsWith(QStringLiteral("-no"))) name = QLatin1Char('-') + name.mid(3);
        if (registered.insert(name.toStdString()).second)
            args.AddArg(name.toStdString(), "Isolated test only", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
        strings.push_back(value.toStdString());
    }
    std::vector<const char*> argv;
    for (const auto& value : strings) argv.push_back(value.c_str());
    std::string parse_error;
    const bool result{args.ParseParameters(argv.size(), argv.data(), parse_error)};
    error = QString::fromStdString(parse_error);
    return result;
}
} // namespace

class ClosedTestPolicyTests : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        SelectBaseParams(ChainType::REGTEST);
#ifndef Q_OS_WIN
        ::umask(0077);
#endif
    }
    void standaloneCoreTranslations()
    {
        // Exercise the linked core translation path even when the linker
        // would otherwise discard it, so every platform checks the callback.
        QVERIFY(LicenseInfo().find("This is experimental software.") != std::string::npos);
    }
    void approvedPublicProfile()
    {
        Profile profile; QString error;
        QVERIFY2(ParseProfile(Json(PublicGood()), profile, error), qPrintable(error));
        QVERIFY(profile.ready);
        QCOMPARE(profile.id, QStringLiteral("vps-regtest-20260926-test4"));
        QCOMPARE(profile.peer, QStringLiteral("88.216.63.161:18547"));
        QCOMPARE(profile.endpoints, QStringList{QStringLiteral("https://88.216.63.161:18580/flowmesh/v1")});
        QTemporaryDir temp; Storage storage;
        QVERIFY2(PrepareStorage(profile, Base(temp), storage, error), qPrintable(error));
        const auto values{NodeArguments(profile, storage)};
        const QStringList fixed{
            "-regtest=1", "-b3modernregtest=1", "-b3flowmeshtest=1", "-b3corridorlength=130",
            "-b3corridorreward=2000000000000", "-b3blockinterval=1", "-b3roundseconds=1", "-b3epochlength=200",
            "-b3checkpointinterval=5", "-b3checkpointdepth=3", "-b3maxepochextension=200", "-b3minfinalityset=4",
            "-enableflowmeshvalidator=0", "-flowmeshapi=0", "-server=0", "-listen=0", "-noconf", "-noincludeconf",
            "-nowallet", "-wallet=closed-test", "-connect=88.216.63.161:18547", "-flowmeshendpoint=https://88.216.63.161:18580/flowmesh/v1"};
        for (const auto& arg : fixed) QCOMPARE(values.count(arg), 1);
        ArgsManager args; QVERIFY2(ConfigureArgs(args, values, error), qPrintable(error));
        args.LockSettings([](common::Settings& settings) {
            settings.rw_settings["regtest"] = false;
            settings.rw_settings["datadir"] = "/external/data";
            settings.rw_settings["walletdir"] = "/external/wallets";
            settings.rw_settings["conf"] = "/external/bitcoin.conf";
            settings.rw_settings["includeconf"] = "/external/included.conf";
        });
        QVERIFY(args.GetBoolArg("-regtest", false));
        QCOMPARE(args.GetArg("-datadir", ""), storage.node.toStdString());
        QCOMPARE(args.GetArg("-walletdir", ""), storage.wallets.toStdString());
        QVERIFY(args.IsArgNegated("-conf")); QVERIFY(args.IsArgNegated("-includeconf"));
    }
    void invalidPublicProfile_data()
    {
        QTest::addColumn<QByteArray>("input");
        auto add = [](const char* label, const char* field, QJsonValue value) {
            auto object{PublicGood()}; object[field] = value; QTest::newRow(label) << Json(object);
        };
        add("schema-downgrade", "schema", 1);
        add("unknown-schema", "schema", 3);
        add("unready-public", "ready", false);
        add("missing-approval", "public_session_approval", QJsonValue{QJsonValue::Undefined});
        add("wrong-approval", "public_session_approval", "another-session");
        add("boolean-approval", "public_session_approval", true);
        add("different-session", "profile_id", "another-session");
        add("mainnet", "network", "main");
        add("wrong-overrides", "regtest_profile", "flowmesh-client-v2");
        add("peer-ip", "b3_peer", "88.216.63.162:18547");
        add("peer-port", "b3_peer", "88.216.63.161:18548");
        add("private-peer", "b3_peer", "127.0.0.1:18547");
        add("endpoint-ip", "https_endpoints", QJsonArray{"https://88.216.63.162:18580/flowmesh/v1"});
        add("endpoint-port", "https_endpoints", QJsonArray{"https://88.216.63.161:18581/flowmesh/v1"});
        add("endpoint-path", "https_endpoints", QJsonArray{"https://88.216.63.161:18580/flowmesh/v2"});
        add("endpoint-extra", "https_endpoints", QJsonArray{"https://88.216.63.161:18580/flowmesh/v1", "https://localhost"});
        add("different-valid-ca", "ca_pem", TEST_CA);
        add("datadir", "datadir", "/external/data");
        add("walletdir", "walletdir", "/external/wallets");
        add("wallet", "wallet", "external-wallet");
        add("conf", "conf", "/external/bitcoin.conf");
        add("includeconf", "includeconf", "/external/included.conf");
        add("args", "args", QJsonArray{"-regtest=0"});
    }
    void invalidPublicProfile()
    {
        QFETCH(QByteArray, input); Profile profile; QString error;
        QVERIFY(!ParseProfile(input, profile, error)); QVERIFY(!profile.ready); QVERIFY(!error.isEmpty());
    }
    void walletStartupSettingsPersistAndReopen()
    {
        const auto profile{Parsed()}; QString error; QTemporaryDir temp; QString root;
        {
            Storage storage; QVERIFY2(PrepareStorage(profile, Base(temp), storage, error), qPrintable(error));
            root = storage.root;
            ArgsManager args; QVERIFY2(ConfigureArgs(args, NodeArguments(profile, storage), error), qPrintable(error));
            args.LockSettings([](common::Settings& settings) {
                UniValue wallets{UniValue::VARR}; wallets.push_back("closed-test");
                settings.rw_settings["wallet"] = wallets;
            });
            // Same ArgsManager persistence reached by CreateWallet/AddWalletSetting.
            // The candidate-02 -nosettings guard throws here, before any wallet test data is used.
            try { QVERIFY(args.WriteSettingsFile()); }
            catch (const std::exception& failure) { QFAIL(failure.what()); }
            QCOMPARE(args.GetArgs("-wallet"), std::vector<std::string>{"closed-test"});
            fs::path file; QVERIFY(args.GetSettingsPath(&file));
            QCOMPARE(QString::fromStdString(fs::PathToString(file)), root + QStringLiteral("/node-settings.json"));
        }
        Storage reopened; QVERIFY2(PrepareStorage(profile, Base(temp), reopened, error), qPrintable(error));
        ArgsManager args; QVERIFY2(ConfigureArgs(args, NodeArguments(profile, reopened), error), qPrintable(error));
        QVERIFY(args.ReadSettingsFile());
        QCOMPARE(args.GetArgs("-wallet"), std::vector<std::string>{"closed-test"});
        QVERIFY(args.GetPersistentSetting("wallet").isArray());
        QCOMPARE(args.GetPersistentSetting("wallet").size(), size_t{1});
        // Even hostile in-memory lower-priority settings cannot override fixed scalars or add a wallet.
        args.LockSettings([](common::Settings& settings) {
            settings.rw_settings["server"] = true;
            settings.rw_settings["enableflowmeshvalidator"] = true;
            settings.rw_settings["listen"] = true;
            UniValue wallets{UniValue::VARR}; wallets.push_back("/external/wallet");
            settings.rw_settings["wallet"] = wallets;
        });
        QVERIFY(!args.GetBoolArg("-server", true));
        QVERIFY(!args.GetBoolArg("-enableflowmeshvalidator", true));
        QVERIFY(!args.GetBoolArg("-listen", true));
        QCOMPARE(args.GetArgs("-wallet"), std::vector<std::string>{"closed-test"});
    }
    void unreadyTouchesNothing()
    {
        auto input{Good()}; input["ready"] = false; input["reason"] = "Endpoints unavailable";
        Profile profile; QString error;
        QVERIFY(ParseProfile(Json(input), profile, error)); QVERIFY(!profile.ready);
        QTemporaryDir temp; QVERIFY(temp.isValid());
        Storage storage;
        QVERIFY(!PrepareStorage(profile, Base(temp), storage, error));
        QVERIFY(QDir{temp.path()}.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
        QVERIFY(NodeArguments(profile, storage).isEmpty());
    }
    void noCallerArguments()
    {
        QString error;
        QVERIFY(CheckInvocation(1, error));
        for (const auto count : {0, 2, 3, 100}) QVERIFY(!CheckInvocation(count, error));
    }
    void invalidProfile_data()
    {
        QTest::addColumn<QByteArray>("input");
        auto add = [](const char* label, const char* field, QJsonValue value) {
            auto object{Good()}; object[field] = value; QTest::newRow(label) << Json(object);
        };
        add("mainnet", "network", "main");
        add("wrong-regtest", "regtest_profile", "unknown");
        add("traversal", "profile_id", "../wallet");
        add("false-as-string", "ready", "false");
        add("config-injection", "conf", "/normal/bitcoin.conf");
        add("args-injection", "args", QJsonArray{"-server=1"});
        add("public-peer", "b3_peer", "8.8.8.8:5647");
        add("peer-dns", "b3_peer", "localhost:12345");
        add("peer-port", "b3_peer", "127.0.0.1:65536");
        add("operator-missing", "operator", "");
        add("http", "https_endpoints", QJsonArray{"http://localhost:12346"});
        add("credentials", "https_endpoints", QJsonArray{"https://user:pass@localhost:12346"});
        add("query", "https_endpoints", QJsonArray{"https://localhost:12346?key=secret"});
        add("duplicate", "https_endpoints", QJsonArray{"https://localhost:12346", "https://localhost:12346"});
        add("zero-endpoints", "https_endpoints", QJsonArray{});
        add("three-endpoints", "https_endpoints", QJsonArray{"https://a", "https://b", "https://c"});
        add("private-key", "ca_pem", "-----BEGIN PRIVATE KEY-----\nsecret\n-----END PRIVATE KEY-----\n");
        add("invalid-ca", "ca_pem", "-----BEGIN CERTIFICATE-----\ninvalid\n-----END CERTIFICATE-----\n");
        QTest::newRow("oversize") << QByteArray(32769, ' ');
    }
    void invalidProfile()
    {
        QFETCH(QByteArray, input); Profile profile; QString error;
        QVERIFY(!ParseProfile(input, profile, error)); QVERIFY(!error.isEmpty()); QVERIFY(!profile.ready);
    }
    void fixedArguments()
    {
        const auto profile{Parsed()}; QString error;
        QTemporaryDir temp; Storage storage;
        QVERIFY2(PrepareStorage(profile, Base(temp), storage, error), qPrintable(error));
        const auto args{NodeArguments(profile, storage)};
        for (const auto& required : {"-regtest=1", "-enableflowmeshvalidator=0", "-flowmeshapi=0", "-server=0", "-listen=0",
                 "-noconf", "-noincludeconf", "-nowallet", "-wallet=closed-test", "-dnsseed=0", "-fixedseeds=0"})
            QVERIFY(args.contains(QString::fromLatin1(required)));
        QCOMPARE(args.filter(QStringLiteral("-wallet=")).size(), 1);
        QCOMPARE(args.filter(QStringLiteral("-datadir=")).size(), 1);
        QCOMPARE(args.filter(QStringLiteral("-connect=")).size(), 1);
        QVERIFY(args.contains(QStringLiteral("-walletdir=") + storage.wallets));
        QVERIFY(args.contains(QStringLiteral("-settings=") + storage.root + QStringLiteral("/node-settings.json")));
        QVERIFY(!args.contains(QStringLiteral("-nosettings")));
        QVERIFY(!args.contains(QStringLiteral("-server=1")));
        QVERIFY(!args.contains(QStringLiteral("-flowmeshendpointinsecure=1")));
        QVERIFY(storage.root.startsWith(Base(temp) + QStringLiteral("/B3FlowMeshClosedTest/")));
        QVERIFY(!QFileInfo::exists(storage.wallets + QStringLiteral("/closed-test")));
    }
    void reopenAndExclusiveOwnership()
    {
        const auto profile{Parsed()}; QString error;
        QTemporaryDir temp; QByteArray marker;
        {
            Storage first; QVERIFY2(PrepareStorage(profile, Base(temp), first, error), qPrintable(error));
            QFile file{first.root + QStringLiteral("/profile.identity")}; QVERIFY(file.open(QIODevice::ReadOnly)); marker = file.readAll();
            Storage second; QVERIFY(!PrepareStorage(profile, Base(temp), second, error));
            QVERIFY(error.contains(QStringLiteral("already open")));
        }
        Storage reopened; QVERIFY2(PrepareStorage(profile, Base(temp), reopened, error), qPrintable(error));
        QFile file{reopened.root + QStringLiteral("/profile.identity")}; QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), marker);
    }
    void sameStorageCannotBePreparedTwice()
    {
        const auto profile{Parsed()}; QString error; QTemporaryDir temp; Storage storage;
        QVERIFY2(PrepareStorage(profile, Base(temp), storage, error), qPrintable(error));
        QVERIFY(!PrepareStorage(profile, Base(temp), storage, error));
        QVERIFY(error.contains(QStringLiteral("already active")));
    }
    void inheritedWalletFilesReopen()
    {
        const auto profile{Parsed()}; QString error; QTemporaryDir temp; QString wallet;
        {
            Storage first; QVERIFY2(PrepareStorage(profile, Base(temp), first, error), qPrintable(error));
            wallet = first.wallets + QStringLiteral("/closed-test");
            // Match core directory/file creation, including Windows ACL
            // inheritance, without constructing or opening an actual wallet.
            QVERIFY(QDir{}.mkdir(wallet));
            QFile fixture{wallet + QStringLiteral("/unit-fixture.txt")};
            QVERIFY(fixture.open(QIODevice::WriteOnly)); QCOMPARE(fixture.write("unit fixture"), qint64{12});
        }
        Storage reopened; QVERIFY2(PrepareStorage(profile, Base(temp), reopened, error), qPrintable(error));
        QFile fixture{wallet + QStringLiteral("/unit-fixture.txt")};
        QVERIFY(fixture.open(QIODevice::ReadOnly)); QCOMPARE(fixture.readAll(), QByteArray{"unit fixture"});
    }
    void redirectedBaseRefused()
    {
        const auto profile{Parsed()}; QString error; QTemporaryDir temp; QTemporaryDir other;
        const auto path{Base(temp) + QStringLiteral("/redirected")};
        REQUIRE_SYMBOLIC_LINK(Base(other), path, true);
        Storage storage; QVERIFY(!PrepareStorage(profile, path, storage, error));
        QVERIFY(!error.isEmpty());
        QVERIFY(QDir{other.path()}.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    }
    void unsafeGuardFileRefused_data()
    {
        QTest::addColumn<QString>("filename"); QTest::addColumn<QString>("kind");
        for (const auto& name : {"profile.identity", "launcher.lock", "test-endpoint-ca.pem"})
            for (const auto& kind : {"symlink", "hardlink", "public"})
                QTest::newRow(qPrintable(QString::fromLatin1(name) + QLatin1Char(':') + QString::fromLatin1(kind)))
                    << QString::fromLatin1(name) << QString::fromLatin1(kind);
    }
    void unsafeGuardFileRefused()
    {
        QFETCH(QString, filename); QFETCH(QString, kind);
        const auto profile{Parsed()}; QString error; QTemporaryDir temp; QTemporaryDir other; QString root;
        { Storage first; QVERIFY(PrepareStorage(profile, Base(temp), first, error)); root = first.root; }
        const auto path{root + QLatin1Char('/') + filename};
        QFile original{path}; QVERIFY(original.open(QIODevice::ReadOnly)); const auto bytes{original.readAll()}; original.close();
        if (kind == QStringLiteral("public")) QVERIFY(PublicReadPermission(path));
        else {
            const auto outside{Base(other) + QStringLiteral("/untouched")};
            QFile fixture{outside}; QVERIFY(fixture.open(QIODevice::WriteOnly)); QCOMPARE(fixture.write(bytes), qint64(bytes.size())); fixture.close();
            QVERIFY(QFile::remove(path));
            if (kind == QStringLiteral("symlink")) REQUIRE_SYMBOLIC_LINK(outside, path, false);
            else QVERIFY(HardLink(outside, path));
        }
        Storage reopened; QVERIFY(!PrepareStorage(profile, Base(temp), reopened, error)); QVERIFY(!error.isEmpty());
        QVERIFY(original.open(QIODevice::ReadOnly)); QCOMPARE(original.readAll(), bytes);
    }
    void publicDirectoryRefused()
    {
        const auto profile{Parsed()}; QString error; QTemporaryDir temp; QString root;
        { Storage first; QVERIFY(PrepareStorage(profile, Base(temp), first, error)); root = first.root; }
        QVERIFY(PublicReadPermission(root));
        Storage reopened; QVERIFY(!PrepareStorage(profile, Base(temp), reopened, error));
        QVERIFY(!error.isEmpty()); QVERIFY(QFileInfo::exists(root + QStringLiteral("/profile.identity")));
#ifndef Q_OS_WIN
        // Restore search permission only to let the temporary-directory owner
        // clean up the test fixture after the rejection assertion.
        QCOMPARE(::chmod(QFile::encodeName(root).constData(), 0700), 0);
#endif
    }
    void changedProfilePreservesExistingFiles()
    {
        auto profile{Parsed()}; QString error; QTemporaryDir temp; QString root;
        { Storage first; QVERIFY(PrepareStorage(profile, Base(temp), first, error)); root = first.root; }
        profile.hash[0] = profile.hash[0] == 'a' ? 'b' : 'a';
        Storage second; QVERIFY(!PrepareStorage(profile, Base(temp), second, error));
        QVERIFY(error.contains(QStringLiteral("profile changed")));
        QVERIFY(QFileInfo::exists(root + QStringLiteral("/profile.identity")));
        QVERIFY(QFileInfo::exists(root + QStringLiteral("/launcher.lock")));
    }
    void unmarkedExistingDirectoryRefused()
    {
        const auto profile{Parsed()}; QString error; QTemporaryDir temp;
        const auto parent{Base(temp) + QStringLiteral("/B3FlowMeshClosedTest")};
        QVERIFY(PrivateDirectory(parent));
        const auto root{parent + QLatin1Char('/') + profile.id};
        QVERIFY(PrivateDirectory(root));
        Storage storage; QVERIFY(!PrepareStorage(profile, Base(temp), storage, error));
        QVERIFY(!QFileInfo::exists(root + QStringLiteral("/profile.identity")));
    }
    void walletSymlinkRefused()
    {
        const auto profile{Parsed()}; QString error; QTemporaryDir temp; QTemporaryDir other; QString wallets;
        { Storage first; QVERIFY(PrepareStorage(profile, Base(temp), first, error)); wallets = first.wallets; }
        REQUIRE_SYMBOLIC_LINK(other.path(), wallets + QStringLiteral("/closed-test"), true);
        Storage next; QVERIFY(!PrepareStorage(profile, Base(temp), next, error));
        QVERIFY(error.contains(QStringLiteral("ownership/link")));
        QVERIFY(QDir{other.path()}.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    }
    void unsafeSettingsRefused_data()
    {
        QTest::addColumn<QString>("suffix"); QTest::addColumn<QString>("kind");
        QStringList kinds{"symlink", "hardlink", "directory", "public", "oversize", "malformed", "external-wallet", "duplicate-wallet", "network-option", "datadir-option", "config-option", "mainnet-option"};
#ifndef Q_OS_WIN
        kinds.push_back(QStringLiteral("fifo"));
#endif
        for (const auto& suffix : {QString{}, QStringLiteral(".tmp"), QStringLiteral(".bak"), QStringLiteral(".bak.tmp")})
            for (const auto& kind : kinds)
                QTest::newRow(qPrintable(suffix + QLatin1Char(':') + kind)) << suffix << kind;
    }
    void unsafeSettingsRefused()
    {
        QFETCH(QString, suffix); QFETCH(QString, kind);
        const auto profile{Parsed()}; QString error; QTemporaryDir temp; QTemporaryDir other; QString root;
        { Storage first; QVERIFY(PrepareStorage(profile, Base(temp), first, error)); root = first.root; }
        const auto path{root + QStringLiteral("/node-settings.json") + suffix};
        const auto outside{other.path() + QStringLiteral("/untouched.json")};
        QFile external{outside}; QVERIFY(external.open(QIODevice::WriteOnly)); external.write("{}"); external.close();
        QByteArray bytes{"{}"};
        if (kind == QStringLiteral("symlink")) REQUIRE_SYMBOLIC_LINK(outside, path, false);
        else if (kind == QStringLiteral("hardlink")) QVERIFY(HardLink(outside, path));
        else if (kind == QStringLiteral("directory")) QVERIFY(QDir{}.mkdir(path));
#ifndef Q_OS_WIN
        else if (kind == QStringLiteral("fifo")) QCOMPARE(::mkfifo(QFile::encodeName(path).constData(), 0600), 0);
#endif
        else {
            if (kind == QStringLiteral("oversize")) bytes = QByteArray(65537, ' ');
            if (kind == QStringLiteral("malformed")) bytes = "{";
            if (kind == QStringLiteral("external-wallet")) bytes = "{\"wallet\":[\"/external/wallet\"]}";
            if (kind == QStringLiteral("duplicate-wallet")) bytes = "{\"wallet\":[\"closed-test\",\"closed-test\"]}";
            if (kind == QStringLiteral("network-option")) bytes = "{\"server\":true}";
            if (kind == QStringLiteral("datadir-option")) bytes = "{\"datadir\":\"/external/data\"}";
            if (kind == QStringLiteral("config-option")) bytes = "{\"conf\":\"/external/bitcoin.conf\"}";
            if (kind == QStringLiteral("mainnet-option")) bytes = "{\"regtest\":false}";
            QFile file{path}; QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), qint64(bytes.size())); file.close();
            if (kind == QStringLiteral("public")) QVERIFY(PublicReadPermission(path));
        }
        Storage reopened; QVERIFY(!PrepareStorage(profile, Base(temp), reopened, error));
        QVERIFY(!error.isEmpty()); QVERIFY(QFileInfo::exists(path));
        QVERIFY(external.open(QIODevice::ReadOnly)); QCOMPARE(external.readAll(), QByteArray{"{}"});
        if (kind != QStringLiteral("directory") && kind != QStringLiteral("fifo") && kind != QStringLiteral("symlink") && kind != QStringLiteral("hardlink")) {
            QFile file{path}; QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), bytes);
        }
    }
};
QTEST_GUILESS_MAIN(ClosedTestPolicyTests)
#include "flowmeshclosedtest_policytests.moc"
