// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// B3 Hive update system: manifest codec + threshold verifier rejection
// matrix (doc/design/b3-hive-update-system.md). Keys here are generated
// per-run test keys — never trusted release material.

#include <hash.h>
#include <key.h>
#include <test/util/setup_common.h>
#include <update/manifest.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <string>
#include <vector>

using namespace update;

BOOST_FIXTURE_TEST_SUITE(update_manifest_tests, BasicTestingSetup)

namespace {

struct Signer {
    CKey key;
    XOnlyPubKey pub;
    Signer()
    {
        key.MakeNewKey(true);
        pub = XOnlyPubKey{key.GetPubKey()};
    }
    std::string SigLine(const std::string& payload) const
    {
        HashWriter hw{TaggedHash(std::string{MANIFEST_TAG})};
        hw << std::span<const unsigned char>{reinterpret_cast<const unsigned char*>(payload.data()), payload.size()};
        const uint256 msg{hw.GetSHA256()};
        unsigned char sig[64];
        BOOST_REQUIRE(key.SignSchnorr(msg, sig, nullptr, {}));
        return "sig=" + ReleaseKeyId(pub) + ":" + HexStr(sig) + "\n";
    }
};

std::string Payload(const std::string& channel = "stable", uint64_t sequence = 5,
                    int64_t published = 1'000'000, int64_t expires = 2'000'000,
                    const std::string& version = "31.1.1",
                    const std::string& url = "https://releases.b3flowmesh.example/hive.dmg")
{
    return std::string{MANIFEST_MAGIC} + "\n" +
           "channel=" + channel + "\n" +
           "sequence=" + std::to_string(sequence) + "\n" +
           "published=" + std::to_string(published) + "\n" +
           "expires=" + std::to_string(expires) + "\n" +
           "notes_sha256=" + std::string(64, 'a') + "\n" +
           "artifact.os=macos\n"
           "artifact.arch=arm64\n"
           "artifact.format=dmg\n"
           "artifact.version=" + version + "\n" +
           "artifact.size=1000\n"
           "artifact.sha256=" + std::string(64, 'b') + "\n" +
           "artifact.url=" + url + "\n";
}

std::string File(const std::string& payload, const std::vector<const Signer*>& signers)
{
    std::string out{payload};
    out += std::string{SIGNATURE_SEPARATOR} + "\n";
    for (const auto* s : signers) out += s->SigLine(payload);
    return out;
}

HostPolicy Host()
{
    HostPolicy h;
    h.os = "macos";
    h.arch = "arm64";
    h.format = "dmg";
    h.installed = *ParseVersion("31.1.0");
    h.last_accepted_sequence = 4;
    h.now = 1'500'000;
    h.allowed_hosts = {"releases.b3flowmesh.example"};
    return h;
}

} // namespace

BOOST_AUTO_TEST_CASE(valid_newer_release_accepted)
{
    Signer s1, s2, s3;
    const ReleaseKeys keys{{s1.pub, s2.pub, s3.pub}, 2};
    std::string err;
    const auto m{ParseAndVerifyManifest(File(Payload(), {&s1, &s3}), keys, err)};
    BOOST_REQUIRE_MESSAGE(m.has_value(), err);
    BOOST_CHECK_EQUAL(m->sequence, 5U);
    BOOST_REQUIRE_EQUAL(m->artifacts.size(), 1U);
    const auto* a{SelectArtifact(*m, Host(), err)};
    BOOST_REQUIRE_MESSAGE(a, err);
    BOOST_CHECK_EQUAL(a->version.ToString(), "31.1.1");
    BOOST_CHECK_EQUAL(a->size, 1000U);
}

BOOST_AUTO_TEST_CASE(unconfigured_fails_closed)
{
    Signer s1;
    std::string err;
    BOOST_CHECK(!ParseAndVerifyManifest(File(Payload(), {&s1}), ReleaseKeys{}, err));
    BOOST_CHECK_EQUAL(err, "update-keys-unconfigured");
    // Threshold above available keys is also unconfigured.
    BOOST_CHECK(!ParseAndVerifyManifest(File(Payload(), {&s1}), ReleaseKeys{{s1.pub}, 2}, err));
    BOOST_CHECK_EQUAL(err, "update-keys-unconfigured");
}

BOOST_AUTO_TEST_CASE(signature_matrix)
{
    Signer s1, s2, s3, rogue;
    const ReleaseKeys keys{{s1.pub, s2.pub, s3.pub}, 2};
    const std::string payload{Payload()};
    std::string err;

    // Insufficient: one pinned signature.
    BOOST_CHECK(!ParseAndVerifyManifest(File(payload, {&s1}), keys, err));
    BOOST_CHECK_EQUAL(err, "update-manifest-sig-threshold");

    // Duplicate signatures by the same key count once.
    BOOST_CHECK(!ParseAndVerifyManifest(File(payload, {&s1, &s1}), keys, err));
    BOOST_CHECK_EQUAL(err, "update-manifest-sig-threshold");

    // Unknown (rogue) key is ignored, never counted.
    BOOST_CHECK(!ParseAndVerifyManifest(File(payload, {&s1, &rogue}), keys, err));
    BOOST_CHECK_EQUAL(err, "update-manifest-sig-threshold");

    // A pinned key with an INVALID signature is a hard reject.
    {
        std::string file{payload};
        file += std::string{SIGNATURE_SEPARATOR} + "\n";
        file += s1.SigLine(payload);
        std::string bad{s2.SigLine(payload)};
        bad[20] = bad[20] == 'a' ? 'b' : 'a';
        file += bad;
        BOOST_CHECK(!ParseAndVerifyManifest(file, keys, err));
        BOOST_CHECK_EQUAL(err, "update-manifest-sig-invalid");
    }

    // Signature over DIFFERENT payload bytes does not verify.
    {
        const std::string other{Payload("stable", 6)};
        std::string file{payload};
        file += std::string{SIGNATURE_SEPARATOR} + "\n";
        Signer* both[2]{&s1, &s2};
        for (auto* s : both) {
            std::string line{s->SigLine(other)}; // signed the WRONG bytes
            file += line;
        }
        BOOST_CHECK(!ParseAndVerifyManifest(file, keys, err));
        BOOST_CHECK_EQUAL(err, "update-manifest-sig-invalid");
    }
}

BOOST_AUTO_TEST_CASE(malformed_manifest_matrix)
{
    Signer s1, s2;
    const ReleaseKeys keys{{s1.pub, s2.pub}, 2};
    std::string err;
    auto reject = [&](const std::string& file, const std::string& want) {
        BOOST_CHECK(!ParseAndVerifyManifest(file, keys, err));
        BOOST_CHECK_EQUAL(err, want);
    };

    reject(File("WRONG-MAGIC\n" + Payload().substr(Payload().find('\n') + 1), {&s1, &s2}),
           "update-manifest-magic");

    // Oversize file.
    reject(std::string(MAX_MANIFEST_BYTES + 1, 'x'), "update-manifest-oversize");

    // CRLF anywhere.
    {
        std::string p{Payload()};
        p.insert(p.find('\n'), "\r");
        reject(File(p, {&s1, &s2}), "update-manifest-crlf");
    }
    // Field order violated (sequence before channel).
    {
        std::string p{std::string{MANIFEST_MAGIC} + "\nsequence=5\nchannel=stable\n"};
        reject(File(p, {&s1, &s2}), "update-manifest-field-order");
    }
    // Leading-zero number (non-canonical).
    {
        std::string p{Payload()};
        p.replace(p.find("sequence=5"), 10, "sequence=05");
        reject(File(p, {&s1, &s2}), "update-manifest-number");
    }
    // Bad hex length.
    {
        std::string p{Payload()};
        p.replace(p.find(std::string(64, 'a')), 64, std::string(63, 'a'));
        reject(File(p, {&s1, &s2}), "update-manifest-hex");
    }
    // Unknown OS.
    {
        std::string p{Payload()};
        p.replace(p.find("artifact.os=macos"), 17, "artifact.os=haiku");
        reject(File(p, {&s1, &s2}), "update-manifest-artifact");
    }
    // Non-https URL.
    {
        std::string p{Payload("stable", 5, 1'000'000, 2'000'000, "31.1.1",
                              "http://releases.b3flowmesh.example/hive.dmg")};
        reject(File(p, {&s1, &s2}), "update-manifest-url");
    }
    // Missing signature section.
    reject(Payload(), "update-manifest-no-signatures");
    // Garbage in the signature section.
    reject(Payload() + std::string{SIGNATURE_SEPARATOR} + "\nnot-a-sig\n", "update-manifest-sig-format");
    // No trailing newline (truncated).
    {
        std::string f{File(Payload(), {&s1, &s2})};
        f.pop_back();
        reject(f, "update-manifest-truncated");
    }
    // Duplicate (os, arch, format) target.
    {
        std::string p{Payload()};
        const std::string block{p.substr(p.find("artifact.os="))};
        reject(File(p + block, {&s1, &s2}), "update-manifest-duplicate-target");
    }
}

BOOST_AUTO_TEST_CASE(acceptance_matrix)
{
    Signer s1, s2;
    const ReleaseKeys keys{{s1.pub, s2.pub}, 2};
    std::string err;
    auto parse = [&](const std::string& payload) {
        auto m{ParseAndVerifyManifest(File(payload, {&s1, &s2}), keys, err)};
        BOOST_REQUIRE_MESSAGE(m, err);
        return *m;
    };
    auto reject = [&](const Manifest& m, const HostPolicy& h, const std::string& want) {
        BOOST_CHECK(!SelectArtifact(m, h, err));
        BOOST_CHECK_EQUAL(err, want);
    };
    const Manifest good{parse(Payload())};

    { // same version ignored
        auto h{Host()};
        h.installed = *ParseVersion("31.1.1");
        reject(good, h, "update-reject-not-newer");
    }
    { // older manifest version ignored
        auto h{Host()};
        h.installed = *ParseVersion("31.2.0");
        reject(good, h, "update-reject-not-newer");
    }
    { // rollback / replayed sequence
        auto h{Host()};
        h.last_accepted_sequence = 5;
        reject(good, h, "update-reject-rollback");
    }
    { // expired
        auto h{Host()};
        h.now = 2'000'001;
        reject(good, h, "update-reject-expired");
    }
    { // published in the future beyond skew
        auto h{Host()};
        h.now = 1'000'000 - MAX_PUBLISH_SKEW - 10;
        reject(good, h, "update-reject-future");
    }
    { // wrong channel
        auto h{Host()};
        h.channel = "beta";
        reject(good, h, "update-reject-channel");
    }
    { // wrong platform (os / arch / format)
        auto h{Host()};
        h.arch = "x86_64";
        reject(good, h, "update-reject-platform");
        h = Host();
        h.format = "pkg";
        reject(good, h, "update-reject-platform");
    }
    { // hostile host (not in the approved list)
        const Manifest evil{parse(Payload("stable", 5, 1'000'000, 2'000'000, "31.1.1",
                                          "https://evil.example/hive.dmg"))};
        reject(evil, Host(), "update-reject-host");
    }
    { // artifact size bound
        auto h{Host()};
        h.max_artifact_bytes = 999;
        reject(good, h, "update-reject-artifact-size");
    }
}

BOOST_AUTO_TEST_CASE(https_host_strictness)
{
    BOOST_CHECK_EQUAL(HttpsHost("https://Releases.Example/x"), "releases.example");
    BOOST_CHECK_EQUAL(HttpsHost("https://releases.example:443/x"), "releases.example");
    BOOST_CHECK(HttpsHost("http://releases.example/x").empty());       // downgrade
    BOOST_CHECK(HttpsHost("https://releases.example:8443/x").empty()); // odd port
    BOOST_CHECK(HttpsHost("https://user@releases.example/x").empty()); // userinfo trick
    BOOST_CHECK(HttpsHost("https:///x").empty());
    BOOST_CHECK(HttpsHost("ftp://releases.example/x").empty());
    BOOST_CHECK(HttpsHost("https://").empty());
}

BOOST_AUTO_TEST_CASE(version_parsing)
{
    BOOST_CHECK(ParseVersion("31.1.1"));
    BOOST_CHECK(*ParseVersion("31.1.0") < *ParseVersion("31.1.1"));
    BOOST_CHECK(*ParseVersion("31.1.9") < *ParseVersion("31.2.0"));
    BOOST_CHECK(!ParseVersion("31.1"));
    BOOST_CHECK(!ParseVersion("31.01.1")); // leading zero
    BOOST_CHECK(!ParseVersion("v31.1.1"));
    BOOST_CHECK(!ParseVersion("31.1.1.1"));
    BOOST_CHECK(!ParseVersion(""));
}

BOOST_AUTO_TEST_SUITE_END()
