// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// B3 Hive update manager: state machine, explicit-approval gating,
// shutdown-gated installer, fail-closed-unconfigured, rollback floor.
// Fake transport + fake installer -- no network, no real installer.

#include <hash.h>
#include <key.h>
#include <test/util/setup_common.h>
#include <update/manager.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

using namespace update;

BOOST_FIXTURE_TEST_SUITE(update_manager_tests, BasicTestingSetup)

namespace {

class ScriptedTransport : public UpdateTransport
{
public:
    std::string manifest_bytes;
    std::string artifact_bytes;
    int fetches{0};
    bool fail_artifact{false};

    bool Fetch(const std::string& url, const std::function<bool(std::string_view)>& sink,
               const std::function<bool(const std::string&)>&, std::string& error) override
    {
        ++fetches;
        const bool is_manifest{url.find("manifest") != std::string::npos};
        if (!is_manifest && fail_artifact) { error = "update-fetch-failed"; return false; }
        const std::string& body{is_manifest ? manifest_bytes : artifact_bytes};
        std::string_view rest{body};
        while (!rest.empty()) {
            const size_t n{std::min<size_t>(64, rest.size())};
            if (!sink(rest.substr(0, n))) { error.clear(); return false; }
            rest = rest.substr(n);
        }
        return true;
    }
};

class RecordingInstaller : public UpdateInstaller
{
public:
    int launches{0};
    bool fail{false};
    bool supported{true};
    fs::path last_path;
    bool Supported() const override { return supported; }
    bool Launch(const fs::path& artifact, std::string& error) override
    {
        ++launches;
        last_path = artifact;
        if (fail) { error = "installer-failed"; return false; }
        return true;
    }
};

class MemorySequences : public SequenceStore
{
public:
    SequenceState value;
    bool fail_load{false};
    bool fail_store{false};
    int stores{0};

    bool Load(SequenceState& out, std::string& error) override
    {
        if (fail_load) { error = "test-sequence-load"; return false; }
        out = value;
        return true;
    }
    bool Store(const SequenceState& in, std::string& error) override
    {
        ++stores;
        if (fail_store) { error = "test-sequence-store"; return false; }
        value = in;
        return true;
    }
};

struct Rig {
    CKey k1, k2;
    XOnlyPubKey p1, p2;
    ScriptedTransport transport;
    RecordingInstaller installer;
    MemorySequences sequences;
    int64_t now{1'500'000};
    std::string artifact_body{std::string(777, 'A')};

    Rig()
    {
        k1.MakeNewKey(true);
        k2.MakeNewKey(true);
        p1 = XOnlyPubKey{k1.GetPubKey()};
        p2 = XOnlyPubKey{k2.GetPubKey()};
    }

    std::string SignedManifest(uint64_t sequence, const std::string& version)
    {
        uint256 sha;
        CSHA256().Write(reinterpret_cast<const unsigned char*>(artifact_body.data()),
                        artifact_body.size()).Finalize(sha.begin());
        std::string payload{std::string{MANIFEST_MAGIC} + "\n" +
                            "channel=stable\n" +
                            "sequence=" + std::to_string(sequence) + "\n" +
                            "published=1000000\nexpires=2000000\n" +
                            "notes_sha256=" + std::string(64, 'c') + "\n" +
                            "artifact.os=macos\nartifact.arch=arm64\nartifact.format=dmg\n" +
                            "artifact.version=" + version + "\n" +
                            "artifact.size=" + std::to_string(artifact_body.size()) + "\n" +
                            "artifact.sha256=" + HexStr(sha) + "\n" +
                            "artifact.url=https://rel.example/hive.dmg\n"};
        std::string file{payload + std::string{SIGNATURE_SEPARATOR} + "\n"};
        for (CKey* k : {&k1, &k2}) {
            HashWriter hw{TaggedHash(std::string{MANIFEST_TAG})};
            hw << std::span<const unsigned char>{reinterpret_cast<const unsigned char*>(payload.data()), payload.size()};
            const uint256 msg{hw.GetSHA256()};
            unsigned char sig[64];
            BOOST_REQUIRE(k->SignSchnorr(msg, sig, nullptr, {}));
            file += "sig=" + ReleaseKeyId(XOnlyPubKey{k->GetPubKey()}) + ":" + HexStr(sig) + "\n";
        }
        return file;
    }

    UpdateConfig Config(const fs::path& dir)
    {
        UpdateConfig c;
        c.manifest_url = "https://rel.example/manifest.txt";
        c.keys = ReleaseKeys{{p1, p2}, 2};
        c.os = "macos";
        c.arch = "arm64";
        c.format = "dmg";
        c.installed = *ParseVersion("31.1.0");
        c.allowed_hosts = {"rel.example"};
        c.download_dir = dir;
        return c;
    }

    UpdateManager Make(const UpdateConfig& c)
    {
        return UpdateManager{c, transport, installer, sequences, [this] { return now; }};
    }
};

} // namespace

BOOST_AUTO_TEST_CASE(unconfigured_fails_closed_and_quiet)
{
    Rig rig;
    UpdateConfig empty; // no url, no keys
    auto m{rig.Make(empty)};
    BOOST_CHECK(m.state() == UpdateState::UNCONFIGURED);
    BOOST_CHECK(!m.CheckNow());
    BOOST_CHECK(!m.StartDownload());
    BOOST_CHECK(!m.RequestInstall());
    BOOST_CHECK(!m.OnShutdownComplete());
    BOOST_CHECK_EQUAL(rig.transport.fetches, 0);  // never touched the network
    BOOST_CHECK_EQUAL(rig.installer.launches, 0);
    BOOST_CHECK(m.state() == UpdateState::UNCONFIGURED);
}

BOOST_AUTO_TEST_CASE(full_flow_with_explicit_approvals)
{
    Rig rig;
    const fs::path dir{m_args.GetDataDirBase() / "hive_updates"};
    fs::create_directories(dir);
    rig.transport.manifest_bytes = rig.SignedManifest(7, "31.1.1");
    rig.transport.artifact_bytes = rig.artifact_body;
    auto m{rig.Make(rig.Config(dir))};
    BOOST_CHECK(m.state() == UpdateState::IDLE);

    // Approval gates hold before anything exists.
    BOOST_CHECK(!m.StartDownload());
    BOOST_CHECK(!m.RequestInstall());
    BOOST_CHECK(!m.OnShutdownComplete());

    BOOST_REQUIRE_MESSAGE(m.CheckNow(), m.last_error());
    BOOST_CHECK(m.state() == UpdateState::UPDATE_AVAILABLE);
    BOOST_REQUIRE(m.available());
    BOOST_CHECK_EQUAL(m.available()->version.ToString(), "31.1.1");
    BOOST_CHECK(m.notes_digest().has_value());
    BOOST_CHECK_EQUAL(rig.sequences.value.floor, 7U);
    BOOST_REQUIRE(rig.sequences.value.pending);
    BOOST_CHECK_EQUAL(rig.sequences.value.pending->sequence, 7U);

    // Install cannot be requested before a download exists.
    BOOST_CHECK(!m.RequestInstall());
    BOOST_CHECK(!m.OnShutdownComplete());

    BOOST_REQUIRE_MESSAGE(m.StartDownload(), m.last_error());
    BOOST_CHECK(m.state() == UpdateState::READY_TO_INSTALL);
    BOOST_REQUIRE(m.downloaded_path());
    // Only the download dir is ever written.
    BOOST_CHECK(fs::PathToString(*m.downloaded_path()).find(fs::PathToString(dir)) == 0);

    // Installer MUST NOT run before the node has shut down.
    BOOST_CHECK(!m.OnShutdownComplete());
    BOOST_CHECK_EQUAL(rig.installer.launches, 0);

    BOOST_REQUIRE(m.RequestInstall());
    BOOST_CHECK(m.state() == UpdateState::AWAITING_SHUTDOWN);
    BOOST_CHECK_EQUAL(rig.installer.launches, 0); // still gated

    BOOST_REQUIRE(m.OnShutdownComplete());
    BOOST_CHECK(m.state() == UpdateState::INSTALLING);
    BOOST_CHECK_EQUAL(rig.installer.launches, 1);
    BOOST_CHECK(rig.installer.last_path == *m.downloaded_path());
}

BOOST_AUTO_TEST_CASE(same_or_older_release_is_idle)
{
    Rig rig;
    const fs::path dir{m_args.GetDataDirBase() / "hive_updates2"};
    fs::create_directories(dir);
    rig.transport.manifest_bytes = rig.SignedManifest(7, "31.1.0"); // == installed
    auto m{rig.Make(rig.Config(dir))};
    BOOST_CHECK(!m.CheckNow());
    BOOST_CHECK(m.state() == UpdateState::IDLE); // benign, not FAILED
    BOOST_CHECK(!m.available());
    BOOST_CHECK_EQUAL(rig.sequences.value.floor, 0U); // floor NOT raised on a non-offer
}

BOOST_AUTO_TEST_CASE(exact_pending_offer_retries_but_sequence_reuse_does_not)
{
    Rig rig;
    const fs::path dir{m_args.GetDataDirBase() / "hive_updates3"};
    fs::create_directories(dir);
    rig.transport.manifest_bytes = rig.SignedManifest(7, "31.1.1");
    rig.transport.artifact_bytes = rig.artifact_body;
    {
        auto m{rig.Make(rig.Config(dir))};
        BOOST_REQUIRE(m.CheckNow());
        BOOST_CHECK_EQUAL(rig.sequences.value.floor, 7U);
        BOOST_CHECK_EQUAL(rig.sequences.stores, 1);
    }
    // Restart: the exact signed bytes at the pending floor remain offerable,
    // and do not rewrite the durable record.
    {
        auto m{rig.Make(rig.Config(dir))};
        BOOST_REQUIRE_MESSAGE(m.CheckNow(), m.last_error());
        BOOST_CHECK(m.state() == UpdateState::UPDATE_AVAILABLE);
        BOOST_CHECK_EQUAL(rig.sequences.stores, 1);
    }
    // Reusing sequence 7 for different valid signed bytes is still rollback.
    rig.transport.manifest_bytes = rig.SignedManifest(7, "31.1.2");
    {
        auto m{rig.Make(rig.Config(dir))};
        BOOST_CHECK(!m.CheckNow());
        BOOST_CHECK(m.state() == UpdateState::IDLE);
        BOOST_CHECK_EQUAL(m.last_error(), "update-reject-rollback");
        BOOST_CHECK(!m.available());
    }
}

BOOST_AUTO_TEST_CASE(bad_signature_is_failed_state)
{
    Rig rig;
    const fs::path dir{m_args.GetDataDirBase() / "hive_updates4"};
    fs::create_directories(dir);
    std::string mf{rig.SignedManifest(7, "31.1.1")};
    mf[mf.size() - 10] ^= 1; // corrupt a signature byte
    rig.transport.manifest_bytes = mf;
    auto m{rig.Make(rig.Config(dir))};
    BOOST_CHECK(!m.CheckNow());
    BOOST_CHECK(m.state() == UpdateState::FAILED);
    BOOST_CHECK(!m.last_error().empty());
    BOOST_CHECK_EQUAL(rig.sequences.value.floor, 0U);
}

BOOST_AUTO_TEST_CASE(download_failure_recovers_and_install_failure_preserves)
{
    Rig rig;
    const fs::path dir{m_args.GetDataDirBase() / "hive_updates5"};
    fs::create_directories(dir);
    rig.transport.manifest_bytes = rig.SignedManifest(7, "31.1.1");
    rig.transport.artifact_bytes = rig.artifact_body;
    rig.transport.fail_artifact = true;
    // First process accepts the offer, then its download fails.
    {
        auto m{rig.Make(rig.Config(dir))};
        BOOST_REQUIRE(m.CheckNow());
        BOOST_CHECK(!m.StartDownload());
        BOOST_CHECK(m.state() == UpdateState::FAILED);
        BOOST_CHECK_EQUAL(rig.installer.launches, 0);
    }
    rig.transport.fail_artifact = false;
    // A fresh process reloads the durable pending identity and retries the
    // same release without resetting or weakening the rollback floor.
    {
        auto m{rig.Make(rig.Config(dir))};
        BOOST_REQUIRE(m.CheckNow());
        BOOST_REQUIRE(m.StartDownload());
        BOOST_REQUIRE(m.RequestInstall());
        rig.installer.fail = true;
        BOOST_CHECK(!m.OnShutdownComplete());
        BOOST_CHECK(m.state() == UpdateState::FAILED);
        // The verified artifact is retained for a retry; nothing else changed.
        BOOST_CHECK(fs::exists(*m.downloaded_path()));
    }
}

BOOST_AUTO_TEST_CASE(sequence_persistence_failures_are_observable_and_fail_closed)
{
    Rig rig;
    const fs::path dir{m_args.GetDataDirBase() / "hive_updates_storage"};
    fs::create_directories(dir);
    rig.transport.manifest_bytes = rig.SignedManifest(7, "31.1.1");

    rig.sequences.fail_load = true;
    {
        auto m{rig.Make(rig.Config(dir))};
        BOOST_CHECK(m.state() == UpdateState::FAILED);
        BOOST_CHECK_EQUAL(m.last_error(), "test-sequence-load");
        BOOST_CHECK(!m.CheckNow());
        BOOST_CHECK_EQUAL(rig.transport.fetches, 0);
    }

    rig.sequences.fail_load = false;
    rig.sequences.fail_store = true;
    {
        auto m{rig.Make(rig.Config(dir))};
        BOOST_CHECK(m.state() == UpdateState::IDLE);
        BOOST_CHECK(!m.CheckNow());
        BOOST_CHECK(m.state() == UpdateState::FAILED);
        BOOST_CHECK_EQUAL(m.last_error(), "test-sequence-store");
        BOOST_CHECK(!m.available());
        BOOST_CHECK_EQUAL(rig.sequences.value.floor, 0U);
    }

    rig.sequences.fail_store = false;
    rig.sequences.value = SequenceState{7, PendingOffer{6, uint256{}}};
    {
        auto m{rig.Make(rig.Config(dir))};
        BOOST_CHECK(m.state() == UpdateState::FAILED);
        BOOST_CHECK_EQUAL(m.last_error(), "update-sequence-state");
        BOOST_CHECK(!m.CheckNow());
    }
}

BOOST_AUTO_TEST_CASE(unsupported_installer_never_requests_shutdown_or_launches)
{
    Rig rig;
    const fs::path dir{m_args.GetDataDirBase() / "hive_updates_unsupported"};
    fs::create_directories(dir);
    rig.transport.manifest_bytes = rig.SignedManifest(7, "31.1.1");
    rig.transport.artifact_bytes = rig.artifact_body;
    rig.installer.supported = false;
    auto m{rig.Make(rig.Config(dir))};
    BOOST_REQUIRE(m.CheckNow());
    BOOST_REQUIRE(m.StartDownload());
    BOOST_CHECK(!m.install_supported());
    BOOST_CHECK(!m.RequestInstall());
    BOOST_CHECK(m.state() == UpdateState::READY_TO_INSTALL);
    BOOST_CHECK_EQUAL(m.last_error(), "update-install-unsupported-platform");
    BOOST_CHECK(!m.OnShutdownComplete());
    BOOST_CHECK_EQUAL(rig.installer.launches, 0);
}

BOOST_AUTO_TEST_CASE(jittered_interval_bounds)
{
    const int64_t base{6 * 60 * 60};
    for (uint64_t e : {uint64_t{0}, uint64_t{1}, uint64_t{999},
                       uint64_t{123456789}, uint64_t{UINT64_MAX}}) {
        const int64_t v{JitteredCheckInterval(base, e)};
        BOOST_CHECK(v >= base - base / 4);
        BOOST_CHECK(v <= base + base / 4);
    }
    // Different entropy actually spreads.
    BOOST_CHECK(JitteredCheckInterval(base, 1) != JitteredCheckInterval(base, 12345) ||
                JitteredCheckInterval(base, 2) != JitteredCheckInterval(base, 54321));
}

BOOST_AUTO_TEST_SUITE_END()
