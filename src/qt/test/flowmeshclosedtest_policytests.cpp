// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include "flowmeshclosedtest_policy.h"
#include <chainparamsbase.h>
#include <common/args.h>
#include <univalue.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <sys/stat.h>
#include <set>
#include <unistd.h>

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
QByteArray Json(const QJsonObject& object) { return QJsonDocument{object}.toJson(); }
Profile Parsed()
{
    Profile profile; QString error;
    if (!ParseProfile(Json(Good()), profile, error)) qFatal("valid unit profile rejected: %s", qPrintable(error));
    return profile;
}
QString Base(QTemporaryDir& temp) { return QFileInfo{temp.path()}.canonicalFilePath(); }
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
    void initTestCase() { SelectBaseParams(ChainType::REGTEST); ::umask(0077); }
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
        QVERIFY(QDir{}.mkdir(parent)); QCOMPARE(::chmod(QFile::encodeName(parent).constData(), 0700), 0);
        const auto root{parent + QLatin1Char('/') + profile.id};
        QVERIFY(QDir{}.mkdir(root)); QCOMPARE(::chmod(QFile::encodeName(root).constData(), 0700), 0);
        Storage storage; QVERIFY(!PrepareStorage(profile, Base(temp), storage, error));
        QVERIFY(!QFileInfo::exists(root + QStringLiteral("/profile.identity")));
    }
    void walletSymlinkRefused()
    {
        const auto profile{Parsed()}; QString error; QTemporaryDir temp; QTemporaryDir other; QString wallets;
        { Storage first; QVERIFY(PrepareStorage(profile, Base(temp), first, error)); wallets = first.wallets; }
        QVERIFY(QFile::link(other.path(), wallets + QStringLiteral("/closed-test")));
        Storage next; QVERIFY(!PrepareStorage(profile, Base(temp), next, error));
        QVERIFY(error.contains(QStringLiteral("ownership/link")));
        QVERIFY(QDir{other.path()}.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    }
    void unsafeSettingsRefused_data()
    {
        QTest::addColumn<QString>("suffix"); QTest::addColumn<QString>("kind");
        for (const auto& suffix : {QString{}, QStringLiteral(".tmp"), QStringLiteral(".bak"), QStringLiteral(".bak.tmp")})
            for (const auto& kind : {"symlink", "hardlink", "directory", "fifo", "public", "oversize", "malformed", "external-wallet", "duplicate-wallet", "network-option"})
                QTest::newRow(qPrintable(suffix + QLatin1Char(':') + QString::fromLatin1(kind))) << suffix << QString::fromLatin1(kind);
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
        if (kind == QStringLiteral("symlink")) QVERIFY(QFile::link(outside, path));
        else if (kind == QStringLiteral("hardlink")) QCOMPARE(::link(QFile::encodeName(outside).constData(), QFile::encodeName(path).constData()), 0);
        else if (kind == QStringLiteral("directory")) QVERIFY(QDir{}.mkdir(path));
        else if (kind == QStringLiteral("fifo")) QCOMPARE(::mkfifo(QFile::encodeName(path).constData(), 0600), 0);
        else {
            if (kind == QStringLiteral("oversize")) bytes = QByteArray(65537, ' ');
            if (kind == QStringLiteral("malformed")) bytes = "{";
            if (kind == QStringLiteral("external-wallet")) bytes = "{\"wallet\":[\"/external/wallet\"]}";
            if (kind == QStringLiteral("duplicate-wallet")) bytes = "{\"wallet\":[\"closed-test\",\"closed-test\"]}";
            if (kind == QStringLiteral("network-option")) bytes = "{\"server\":true}";
            QFile file{path}; QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), qint64(bytes.size())); file.close();
            if (kind == QStringLiteral("public")) QCOMPARE(::chmod(QFile::encodeName(path).constData(), 0644), 0);
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
