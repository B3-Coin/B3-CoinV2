// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// B3 Hive update system: download-safety matrix with a scripted fake
// transport -- no network, no real installer, ever.

#include <crypto/sha256.h>
#include <test/util/setup_common.h>
#include <update/downloader.h>
#include <update/manifest.h>
#include <util/fs.h>

#include <boost/test/unit_test.hpp>

#include <fstream>

using namespace update;

BOOST_FIXTURE_TEST_SUITE(update_downloader_tests, BasicTestingSetup)

namespace {

//! Scripted transport: optional redirect hops, then a body delivered in
//! fixed-size chunks; policy callbacks decide whether it proceeds.
class FakeTransport : public UpdateTransport
{
public:
    std::vector<std::string> redirects;
    std::string body;
    size_t chunk_size{7};
    bool network_fail{false};

    bool Fetch(const std::string& url,
               const std::function<bool(std::string_view)>& sink,
               const std::function<bool(const std::string&)>& on_redirect,
               std::string& error) override
    {
        for (const auto& r : redirects) {
            if (!on_redirect(r)) { error = "update-fetch-redirect"; return false; }
        }
        if (network_fail) { error = "update-fetch-failed"; return false; }
        std::string_view rest{body};
        while (!rest.empty()) {
            const size_t n{std::min(chunk_size, rest.size())};
            if (!sink(rest.substr(0, n))) { error.clear(); return false; }
            rest = rest.substr(n);
        }
        return true;
    }
};

uint256 Sha(const std::string& s)
{
    uint256 out;
    CSHA256().Write(reinterpret_cast<const unsigned char*>(s.data()), s.size()).Finalize(out.begin());
    return out;
}

Artifact Art(const std::string& body, const std::string& url = "https://rel.example/hive.dmg")
{
    Artifact a;
    a.os = "macos";
    a.arch = "arm64";
    a.format = "dmg";
    a.version = *ParseVersion("31.1.1");
    a.size = body.size();
    a.sha256 = Sha(body);
    a.url = url;
    return a;
}

FetchPolicy Policy() { return {{"rel.example", "cdn.example"}}; }

} // namespace

BOOST_AUTO_TEST_CASE(artifact_happy_path)
{
    const std::string body(1000, 'Q');
    FakeTransport t;
    t.body = body;
    const fs::path dir{m_args.GetDataDirBase() / "updates"};
    fs::create_directories(dir);
    std::string err;
    const auto path{FetchArtifact(t, Art(body), dir, Policy(), err)};
    BOOST_REQUIRE_MESSAGE(path, err);
    // Final name is digest-derived (no attacker-controlled path bytes).
    BOOST_CHECK_EQUAL(fs::PathToString(path->filename()), "hive-update-" + Sha(body).GetHex() + ".dmg");
    std::ifstream in{fs::PathToString(*path), std::ios::binary};
    std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    BOOST_CHECK(got == body);
    // No temp litter.
    size_t files{0};
    for (auto const& e : fs::directory_iterator{dir}) { (void)e; ++files; }
    BOOST_CHECK_EQUAL(files, 1U);
}

BOOST_AUTO_TEST_CASE(redirect_policy)
{
    const std::string body(64, 'R');
    const fs::path dir{m_args.GetDataDirBase() / "updates_r"};
    fs::create_directories(dir);
    std::string err;

    { // approved-host redirect is fine
        FakeTransport t;
        t.body = body;
        t.redirects = {"https://cdn.example/hive.dmg"};
        BOOST_CHECK(FetchArtifact(t, Art(body), dir, Policy(), err));
    }
    { // hostile redirect target aborts
        FakeTransport t;
        t.body = body;
        t.redirects = {"https://evil.example/hive.dmg"};
        BOOST_CHECK(!FetchArtifact(t, Art(body), dir, Policy(), err));
    }
    { // https downgrade on redirect aborts
        FakeTransport t;
        t.body = body;
        t.redirects = {"http://rel.example/hive.dmg"};
        BOOST_CHECK(!FetchArtifact(t, Art(body), dir, Policy(), err));
    }
    { // too many redirects abort
        FakeTransport t;
        t.body = body;
        t.redirects = std::vector<std::string>(MAX_REDIRECTS + 1, "https://cdn.example/x");
        BOOST_CHECK(!FetchArtifact(t, Art(body), dir, Policy(), err));
    }
    { // initial URL must be https on an approved host
        FakeTransport t;
        t.body = body;
        BOOST_CHECK(!FetchArtifact(t, Art(body, "https://evil.example/x.dmg"), dir, Policy(), err));
        BOOST_CHECK_EQUAL(err, "update-fetch-host");
        BOOST_CHECK(!FetchArtifact(t, Art(body, "http://rel.example/x.dmg"), dir, Policy(), err));
        BOOST_CHECK_EQUAL(err, "update-fetch-host");
    }
}

BOOST_AUTO_TEST_CASE(size_and_digest_enforcement)
{
    const std::string body(500, 'S');
    const fs::path dir{m_args.GetDataDirBase() / "updates_s"};
    fs::create_directories(dir);
    std::string err;

    auto litter_free = [&] {
        size_t n{0};
        for (auto const& e : fs::directory_iterator{dir}) { (void)e; ++n; }
        return n == 0;
    };

    { // oversized stream aborts mid-transfer, temp removed
        FakeTransport t;
        t.body = body + "EXTRA";
        BOOST_CHECK(!FetchArtifact(t, Art(body), dir, Policy(), err));
        BOOST_CHECK_EQUAL(err, "update-fetch-oversize");
        BOOST_CHECK(litter_free());
    }
    { // truncated/interrupted stream rejected, temp removed
        FakeTransport t;
        t.body = body.substr(0, 100);
        BOOST_CHECK(!FetchArtifact(t, Art(body), dir, Policy(), err));
        BOOST_CHECK_EQUAL(err, "update-fetch-truncated");
        BOOST_CHECK(litter_free());
    }
    { // right size, wrong bytes: digest mismatch, temp removed
        std::string tampered{body};
        tampered[250] = 'X';
        FakeTransport t;
        t.body = tampered;
        BOOST_CHECK(!FetchArtifact(t, Art(body), dir, Policy(), err));
        BOOST_CHECK_EQUAL(err, "update-fetch-digest");
        BOOST_CHECK(litter_free());
    }
    { // network failure leaves nothing
        FakeTransport t;
        t.network_fail = true;
        BOOST_CHECK(!FetchArtifact(t, Art(body), dir, Policy(), err));
        BOOST_CHECK(litter_free());
    }
}

BOOST_AUTO_TEST_CASE(symlink_and_replacement_safety)
{
    const std::string body(64, 'L');
    const fs::path dir{m_args.GetDataDirBase() / "updates_l"};
    fs::create_directories(dir);
    std::string err;

    // Plant a symlink at the FINAL path pointing at a victim file: the
    // atomic rename must replace the symlink itself, never write through it.
    const fs::path victim{m_args.GetDataDirBase() / "victim.txt"};
    { std::ofstream v{fs::PathToString(victim)}; v << "precious"; }
    const fs::path final_path{dir / fs::u8path("hive-update-" + Sha(body).GetHex() + ".dmg")};
    std::error_code ec;
    fs::create_symlink(victim, final_path, ec);
    BOOST_REQUIRE(!ec);

    FakeTransport t;
    t.body = body;
    const auto path{FetchArtifact(t, Art(body), dir, Policy(), err)};
    BOOST_REQUIRE_MESSAGE(path, err);
    BOOST_CHECK(!fs::is_symlink(*path));
    std::ifstream v{fs::PathToString(victim)};
    std::string vbody((std::istreambuf_iterator<char>(v)), std::istreambuf_iterator<char>());
    BOOST_CHECK_EQUAL(vbody, "precious"); // untouched
}

BOOST_AUTO_TEST_CASE(disk_write_failure_is_clean)
{
    const std::string body(64, 'D');
    const fs::path dir{m_args.GetDataDirBase() / "updates_ro"};
    fs::create_directories(dir);
    fs::permissions(dir, fs::perms::owner_read | fs::perms::owner_exec);
    std::string err;
    FakeTransport t;
    t.body = body;
    const auto path{FetchArtifact(t, Art(body), dir, Policy(), err)};
    fs::permissions(dir, fs::perms::owner_all); // restore for teardown
    BOOST_CHECK(!path);
    BOOST_CHECK_EQUAL(err, "update-fetch-tempfile");
    size_t n{0};
    for (auto const& e : fs::directory_iterator{dir}) { (void)e; ++n; }
    BOOST_CHECK_EQUAL(n, 0U);
}

BOOST_AUTO_TEST_CASE(manifest_fetch_bounds)
{
    std::string err;
    { // happy path
        FakeTransport t;
        t.body = "hello manifest";
        const auto bytes{FetchManifestBytes(t, "https://rel.example/m.txt", Policy(), err)};
        BOOST_REQUIRE(bytes);
        BOOST_CHECK_EQUAL(*bytes, "hello manifest");
    }
    { // oversize manifest rejected
        FakeTransport t;
        t.body = std::string(MAX_MANIFEST_BYTES + 1, 'x');
        BOOST_CHECK(!FetchManifestBytes(t, "https://rel.example/m.txt", Policy(), err));
        BOOST_CHECK_EQUAL(err, "update-fetch-oversize");
    }
    { // unapproved manifest host rejected before any fetch
        FakeTransport t;
        t.body = "x";
        BOOST_CHECK(!FetchManifestBytes(t, "https://evil.example/m.txt", Policy(), err));
        BOOST_CHECK_EQUAL(err, "update-fetch-host");
    }
}

BOOST_AUTO_TEST_SUITE_END()
