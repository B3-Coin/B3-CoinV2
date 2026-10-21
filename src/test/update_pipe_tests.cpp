// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The B3 Hive app's update helper: the pipe transport (the app carries
// bytes, every policy stays in src/update/), the file-backed rollback
// record, and the full manager flow over a scripted pipe. No network.

#include <hash.h>
#include <key.h>
#include <test/util/setup_common.h>
#include <update/downloader.h>
#include <update/filestore.h>
#include <update/manager.h>
#include <update/pipe.h>
#include <util/fs.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <fstream>

using namespace update;

BOOST_FIXTURE_TEST_SUITE(update_pipe_tests, BasicTestingSetup)

namespace {

//! The parent's side, scripted: `input` is everything it will send, and
//! `output` collects what the helper answers.
class ScriptedChannel : public PipeChannel
{
public:
    explicit ScriptedChannel(std::string input) : m_input{std::move(input)} {}
    std::string output;

    bool ReadLine(std::string& line) override
    {
        const size_t end{m_input.find('\n', m_pos)};
        if (end == std::string::npos || end - m_pos > MAX_PIPE_LINE) return false;
        line = m_input.substr(m_pos, end - m_pos);
        m_pos = end + 1;
        return true;
    }
    bool ReadExact(size_t n, std::string& out) override
    {
        if (m_input.size() - m_pos < n) return false;
        out = m_input.substr(m_pos, n);
        m_pos += n;
        return true;
    }
    bool Write(std::string_view bytes) override
    {
        output.append(bytes);
        return true;
    }

private:
    std::string m_input;
    size_t m_pos{0};
};

std::string Data(std::string_view body)
{
    return "DATA " + std::to_string(body.size()) + "\n" + std::string{body};
}

const FetchPolicy POLICY{{"rel.example", "cdn.example"}};

} // namespace

BOOST_AUTO_TEST_CASE(body_frames_reach_the_sink)
{
    ScriptedChannel channel{Data("hello ") + Data("world") + "DONE\n"};
    PipeTransport transport{channel};
    std::string error;
    const auto bytes{FetchManifestBytes(transport, "https://rel.example/m.txt", POLICY, error)};
    BOOST_REQUIRE_MESSAGE(bytes, error);
    BOOST_CHECK_EQUAL(*bytes, "hello world");
    BOOST_CHECK_EQUAL(channel.output, "FETCH https://rel.example/m.txt\nMORE\nMORE\n");
    BOOST_CHECK(!transport.broken());
}

BOOST_AUTO_TEST_CASE(redirects_are_decided_by_the_policy)
{
    {
        ScriptedChannel channel{"REDIRECT https://cdn.example/m.txt\n" + Data("ok") + "DONE\n"};
        PipeTransport transport{channel};
        std::string error;
        const auto bytes{FetchManifestBytes(transport, "https://rel.example/m.txt", POLICY, error)};
        BOOST_REQUIRE_MESSAGE(bytes, error);
        BOOST_CHECK_EQUAL(channel.output, "FETCH https://rel.example/m.txt\nALLOW\nMORE\n");
    }
    for (const char* target : {"https://evil.example/m.txt", "http://cdn.example/m.txt",
                               "https://user@cdn.example/m.txt"}) {
        ScriptedChannel channel{std::string{"REDIRECT "} + target + "\n"};
        PipeTransport transport{channel};
        std::string error;
        BOOST_CHECK(!FetchManifestBytes(transport, "https://rel.example/m.txt", POLICY, error));
        BOOST_CHECK_EQUAL(error, "update-fetch-redirect");
        BOOST_CHECK_EQUAL(channel.output, "FETCH https://rel.example/m.txt\nDENY\n");
        // A denied redirect ends the fetch cleanly; the pipe stays usable.
        BOOST_CHECK(!transport.broken());
    }
    // More than MAX_REDIRECTS hops is refused even between approved hosts.
    std::string hops;
    for (unsigned i = 0; i <= MAX_REDIRECTS; ++i) hops += "REDIRECT https://cdn.example/" + std::to_string(i) + "\n";
    ScriptedChannel channel{hops};
    PipeTransport transport{channel};
    std::string error;
    BOOST_CHECK(!FetchManifestBytes(transport, "https://rel.example/m.txt", POLICY, error));
    BOOST_CHECK_EQUAL(error, "update-fetch-redirect");
}

BOOST_AUTO_TEST_CASE(oversize_bodies_are_stopped_mid_stream)
{
    const std::string chunk(40 * 1024, 'x');
    ScriptedChannel channel{Data(chunk) + Data(chunk) + "DONE\n"};
    PipeTransport transport{channel};
    std::string error;
    BOOST_CHECK(!FetchManifestBytes(transport, "https://rel.example/m.txt", POLICY, error));
    BOOST_CHECK_EQUAL(error, "update-fetch-oversize");
    BOOST_CHECK_EQUAL(channel.output, "FETCH https://rel.example/m.txt\nMORE\nSTOP\n");
}

BOOST_AUTO_TEST_CASE(parent_failures_are_reported_with_safe_tokens)
{
    for (const auto& [sent, expected] : std::vector<std::pair<std::string, std::string>>{
             {"update-fetch-timeout", "update-fetch-timeout"},
             {"update-fetch-tls", "update-fetch-tls"},
             {"update-sequence-store", "update-fetch-failed"},
             {"update-fetch-<b>", "update-fetch-failed"},
             {"update-fetch-", "update-fetch-failed"}}) {
        ScriptedChannel channel{"FAIL " + sent + "\n"};
        PipeTransport transport{channel};
        std::string error;
        BOOST_CHECK(!FetchManifestBytes(transport, "https://rel.example/m.txt", POLICY, error));
        BOOST_CHECK_EQUAL(error, expected);
        BOOST_CHECK(!transport.broken());
    }
}

BOOST_AUTO_TEST_CASE(broken_frames_break_the_transport_for_good)
{
    {
        // The app went away mid-frame.
        ScriptedChannel channel{"DATA 10\nshort"};
        PipeTransport transport{channel};
        std::string error;
        BOOST_CHECK(!FetchManifestBytes(transport, "https://rel.example/m.txt", POLICY, error));
        BOOST_CHECK_EQUAL(error, "update-pipe-protocol");
        BOOST_CHECK(transport.broken());
    }
    for (const std::string input : {"DATA 0\n", "DATA 65537\n", "DATA x\n", "DATA -1\n",
                                    "HELLO\n", "done\n", "REDIRECT https://cdn.example/\x01\n"}) {
        ScriptedChannel channel{input + Data("never read") + "DONE\n"};
        PipeTransport transport{channel};
        std::string error;
        BOOST_CHECK(!FetchManifestBytes(transport, "https://rel.example/m.txt", POLICY, error));
        if (input.starts_with("REDIRECT")) {
            // An unprintable target is refused like any other bad redirect.
            BOOST_CHECK_EQUAL(error, "update-fetch-redirect");
            continue;
        }
        BOOST_CHECK_EQUAL(error, "update-pipe-protocol");
        BOOST_CHECK(transport.broken());
        BOOST_CHECK(!FetchManifestBytes(transport, "https://rel.example/m.txt", POLICY, error));
        BOOST_CHECK_EQUAL(error, "update-pipe-protocol");
    }
}

BOOST_AUTO_TEST_CASE(urls_that_cannot_travel_in_a_line_are_refused)
{
    ScriptedChannel channel{""};
    PipeTransport transport{channel};
    const std::function<bool(std::string_view)> sink{[](std::string_view) { return true; }};
    const std::function<bool(const std::string&)> redirect{[](const std::string&) { return true; }};
    std::string error;
    for (const std::string url : {"https://rel.example/a b", "https://rel.example/\nDONE", ""}) {
        BOOST_CHECK(!transport.Fetch(url, sink, redirect, error));
        BOOST_CHECK_EQUAL(error, "update-fetch-url");
    }
    BOOST_CHECK(channel.output.empty());
}

BOOST_AUTO_TEST_CASE(sequence_records_round_trip_and_reject_damage)
{
    const uint256 hash{uint256::FromHex(std::string(64, 'a')).value()};
    for (const SequenceState& state : {SequenceState{}, SequenceState{7, std::nullopt},
                                       SequenceState{7, PendingOffer{7, hash}}}) {
        const std::string record{EncodeSequenceRecord(state)};
        BOOST_CHECK(DecodeSequenceRecord(record) == state);
    }
    BOOST_CHECK_EQUAL(EncodeSequenceRecord({3, std::nullopt}), "v1:3:none\n");
    for (const std::string& bad : std::vector<std::string>{
             "v1:3:none", "v2:3:none\n", "v1:x:none\n", "v1:3:4:" + std::string(64, 'a') + "\n",
             "v1:0:0:" + std::string(64, 'a') + "\n", "v1:3:3:zz\n", "v1:3\n", ""}) {
        BOOST_CHECK(!DecodeSequenceRecord(bad));
    }
}

BOOST_AUTO_TEST_CASE(file_store_keeps_the_floor_and_fails_closed)
{
    const fs::path dir{m_args.GetDataDirBase() / "update_store"};
    fs::create_directories(dir);
    const fs::path path{dir / "sequence"};
    FileSequenceStore store{path};
    SequenceState state{99, std::nullopt};
    std::string error;
    // No file yet: a fresh install with floor 0.
    BOOST_REQUIRE(store.Load(state, error));
    BOOST_CHECK(state == SequenceState{});

    const SequenceState next{5, PendingOffer{5, uint256::ONE}};
    BOOST_REQUIRE_MESSAGE(store.Store(next, error), error);
    BOOST_REQUIRE(store.Load(state, error));
    BOOST_CHECK(state == next);
    // Only the record is left in the folder: the temporary file was renamed.
    size_t entries{0};
    for ([[maybe_unused]] const auto& e : fs::directory_iterator{dir}) ++entries;
    BOOST_CHECK_EQUAL(entries, 1U);

    // A pending offer must sit at the floor.
    BOOST_CHECK(!store.Store({6, PendingOffer{5, uint256::ONE}}, error));
    BOOST_CHECK_EQUAL(error, "update-sequence-state");

    // A damaged record never reads as a lower floor.
    std::ofstream{path.std_path(), std::ios::binary | std::ios::trunc} << "garbage";
    BOOST_CHECK(!store.Load(state, error));
    BOOST_CHECK_EQUAL(error, "update-sequence-state");
}

namespace {

class NoInstaller : public UpdateInstaller
{
public:
    bool Supported() const override { return false; }
    bool Launch(const fs::path&, std::string& error) override
    {
        error = "update-install-unsupported-platform";
        return false;
    }
};

std::string Sign(const std::string& payload, const std::vector<CKey>& keys)
{
    std::string file{payload + std::string{SIGNATURE_SEPARATOR} + "\n"};
    for (const CKey& k : keys) {
        HashWriter hw{TaggedHash(std::string{MANIFEST_TAG})};
        hw << std::span<const unsigned char>{reinterpret_cast<const unsigned char*>(payload.data()), payload.size()};
        unsigned char sig[64];
        BOOST_REQUIRE(k.SignSchnorr(hw.GetSHA256(), sig, nullptr, {}));
        file += "sig=" + ReleaseKeyId(XOnlyPubKey{k.GetPubKey()}) + ":" + HexStr(sig) + "\n";
    }
    return file;
}

} // namespace

BOOST_AUTO_TEST_CASE(manager_checks_and_downloads_over_the_pipe)
{
    CKey k1, k2;
    k1.MakeNewKey(true);
    k2.MakeNewKey(true);
    const std::string artifact(100 * 1024, 'B'); // more than one frame
    uint256 sha;
    CSHA256().Write(reinterpret_cast<const unsigned char*>(artifact.data()), artifact.size()).Finalize(sha.begin());
    const std::string manifest{Sign(std::string{MANIFEST_MAGIC} + "\n" +
                                        "channel=stable\nsequence=4\npublished=1000000\nexpires=2000000\n" +
                                        "notes_sha256=" + std::string(64, 'c') + "\n" +
                                        "artifact.os=linux\nartifact.arch=x86_64\nartifact.format=targz\n" +
                                        "artifact.version=2.0.0\n" +
                                        "artifact.size=" + std::to_string(artifact.size()) + "\n" +
                                        "artifact.sha256=" + HexStr(sha) + "\n" +
                                        "artifact.url=https://rel.example/hive.tar.gz\n",
                                    {k1, k2})};

    // What the app sends: the manifest, then the artifact behind a redirect
    // to the approved CDN host, in two frames.
    ScriptedChannel channel{Data(manifest) + "DONE\n" +
                            "REDIRECT https://cdn.example/hive.tar.gz\n" +
                            Data(artifact.substr(0, MAX_PIPE_CHUNK)) +
                            Data(artifact.substr(MAX_PIPE_CHUNK)) + "DONE\n"};
    PipeTransport transport{channel};
    NoInstaller installer;
    const fs::path dir{m_args.GetDataDirBase() / "update_flow"};
    fs::create_directories(dir / "downloads");
    FileSequenceStore sequences{dir / "sequence"};

    UpdateConfig config;
    config.manifest_url = "https://rel.example/manifest.txt";
    config.keys = ReleaseKeys{{XOnlyPubKey{k1.GetPubKey()}, XOnlyPubKey{k2.GetPubKey()}}, 2};
    config.os = "linux";
    config.arch = "x86_64";
    config.format = "targz";
    config.installed = *ParseVersion("1.1.5");
    config.allowed_hosts = {"rel.example", "cdn.example"};
    config.download_dir = dir / "downloads";
    UpdateManager manager{config, transport, installer, sequences, [] { return int64_t{1'500'000}; }};

    BOOST_REQUIRE(manager.CheckNow());
    BOOST_CHECK(manager.state() == UpdateState::UPDATE_AVAILABLE);
    BOOST_CHECK_EQUAL(manager.available()->version.ToString(), "2.0.0");
    // The offer's rollback floor is on disk before it is shown.
    std::string error;
    SequenceState stored;
    BOOST_REQUIRE(sequences.Load(stored, error));
    BOOST_CHECK_EQUAL(stored.floor, 4U);

    BOOST_REQUIRE_MESSAGE(manager.StartDownload(), manager.last_error());
    BOOST_CHECK(manager.state() == UpdateState::READY_TO_INSTALL);
    std::ifstream file{manager.downloaded_path()->std_path(), std::ios::binary};
    const std::string saved{std::istreambuf_iterator<char>{file}, {}};
    BOOST_CHECK(saved == artifact);
    BOOST_CHECK_EQUAL(channel.output, "FETCH https://rel.example/manifest.txt\nMORE\n"
                                      "FETCH https://rel.example/hive.tar.gz\nALLOW\nMORE\nMORE\n");

    // Nothing installs: the platform has no audited replacement flow.
    BOOST_CHECK(!manager.RequestInstall());
    BOOST_CHECK_EQUAL(manager.last_error(), "update-install-unsupported-platform");
}

BOOST_AUTO_TEST_SUITE_END()
