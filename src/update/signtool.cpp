// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// b3hive-sign: offline, deterministic manifest signing tool for B3 Hive
// release operators (doc/design/b3-hive-update-system.md).
//
//   b3hive-sign genkey <keyfile>          create a release signing key (0600)
//   b3hive-sign pubkey <keyfile>          print x-only pubkey + key id
//   b3hive-sign sign <payload> <keyfile>  print one "sig=..." line for the
//                                         EXACT bytes of <payload>
//   b3hive-sign verify <manifest> <threshold> <pubkey_hex>[,...]
//                                         run the production verifier
//
// Private keys never enter the source tree, build, installer or update
// server: genkey writes to the operator-chosen path only. Test keys are
// test keys -- never ship one as trusted production material.

#include <hash.h>
#include <key.h>
#include <pubkey.h>
#include <update/manifest.h>
#include <util/fs_helpers.h>
#include <util/strencodings.h>

#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

std::optional<std::string> ReadFile(const std::string& path)
{
    std::ifstream f{path, std::ios::binary};
    if (!f) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::optional<CKey> LoadKey(const std::string& path)
{
    const auto hex{ReadFile(path)};
    if (!hex) return std::nullopt;
    std::string t{*hex};
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
    const auto bytes{TryParseHex<unsigned char>(t)};
    if (!bytes || bytes->size() != 32) return std::nullopt;
    CKey key;
    key.Set(bytes->begin(), bytes->end(), true);
    return key.IsValid() ? std::optional<CKey>{key} : std::nullopt;
}

int Usage()
{
    std::fprintf(stderr,
                 "usage: b3hive-sign genkey <keyfile>\n"
                 "       b3hive-sign pubkey <keyfile>\n"
                 "       b3hive-sign sign <payload-file> <keyfile>\n"
                 "       b3hive-sign verify <manifest-file> <threshold> <pubkey_hex>[,<pubkey_hex>...]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) return Usage();
    const std::string cmd{argv[1]};
    ECC_Context ecc;

    if (cmd == "genkey" && argc == 3) {
        CKey key;
        key.MakeNewKey(true);
        const std::string path{argv[2]};
        const std::string key_text{HexStr(std::span<const unsigned char>{
            reinterpret_cast<const unsigned char*>(key.begin()),
            static_cast<size_t>(key.size())}) + "\n"};

        // Create the secret file atomically and exclusively. Never truncate
        // an existing path or follow a pre-planted symlink.
#ifdef _WIN32
        const int fd{::_open(path.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                             _S_IREAD | _S_IWRITE)};
#else
        int flags{O_WRONLY | O_CREAT | O_EXCL};
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        const int fd{::open(path.c_str(), flags, 0600)};
#endif
        if (fd < 0) { std::fprintf(stderr, "cannot create %s (path must not exist)\n", path.c_str()); return 1; }
#ifdef _WIN32
        FILE* file{::_fdopen(fd, "wb")};
#else
        FILE* file{::fdopen(fd, "wb")};
#endif
        if (!file) {
#ifdef _WIN32
            ::_close(fd);
#else
            ::close(fd);
#endif
            std::remove(path.c_str());
            std::fprintf(stderr, "cannot open %s\n", path.c_str());
            return 1;
        }
        const bool written{std::fwrite(key_text.data(), 1, key_text.size(), file) == key_text.size() &&
                           FileCommit(file)};
        const bool closed{std::fclose(file) == 0};
        if (!written || !closed) {
            std::remove(path.c_str());
            std::fprintf(stderr, "cannot safely write %s\n", path.c_str());
            return 1;
        }
        const XOnlyPubKey pub{key.GetPubKey()};
        std::printf("pubkey=%s\nkeyid=%s\n", HexStr(pub).c_str(), update::ReleaseKeyId(pub).c_str());
        return 0;
    }
    if (cmd == "pubkey" && argc == 3) {
        const auto key{LoadKey(argv[2])};
        if (!key) { std::fprintf(stderr, "bad key file\n"); return 1; }
        const XOnlyPubKey pub{key->GetPubKey()};
        std::printf("pubkey=%s\nkeyid=%s\n", HexStr(pub).c_str(), update::ReleaseKeyId(pub).c_str());
        return 0;
    }
    if (cmd == "sign" && argc == 4) {
        const auto payload{ReadFile(argv[2])};
        if (!payload) { std::fprintf(stderr, "cannot read payload\n"); return 1; }
        if (payload->empty() || payload->back() != '\n') {
            std::fprintf(stderr, "payload must end with a newline (byte-exact signing)\n");
            return 1;
        }
        const auto key{LoadKey(argv[3])};
        if (!key) { std::fprintf(stderr, "bad key file\n"); return 1; }
        HashWriter hw{TaggedHash(std::string{update::MANIFEST_TAG})};
        hw << std::span<const unsigned char>{reinterpret_cast<const unsigned char*>(payload->data()), payload->size()};
        const uint256 msg{hw.GetSHA256()};
        unsigned char sig[64];
        if (!key->SignSchnorr(msg, sig, nullptr, {})) { std::fprintf(stderr, "signing failed\n"); return 1; }
        const XOnlyPubKey pub{key->GetPubKey()};
        std::printf("sig=%s:%s\n", update::ReleaseKeyId(pub).c_str(), HexStr(sig).c_str());
        return 0;
    }
    if (cmd == "verify" && argc == 5) {
        const auto file{ReadFile(argv[2])};
        if (!file) { std::fprintf(stderr, "cannot read manifest\n"); return 1; }
        update::ReleaseKeys keys;
        keys.threshold = static_cast<unsigned>(std::stoul(argv[3]));
        std::stringstream ss{argv[4]};
        std::string part;
        while (std::getline(ss, part, ',')) {
            const auto bytes{TryParseHex<unsigned char>(part)};
            if (!bytes || bytes->size() != 32) { std::fprintf(stderr, "bad pubkey hex\n"); return 1; }
            keys.keys.emplace_back(std::span<const unsigned char>{*bytes});
        }
        std::string err;
        const auto m{update::ParseAndVerifyManifest(*file, keys, err)};
        if (!m) { std::fprintf(stderr, "REJECT: %s\n", err.c_str()); return 1; }
        std::printf("OK channel=%s sequence=%llu artifacts=%zu\n", m->channel.c_str(),
                    (unsigned long long)m->sequence, m->artifacts.size());
        return 0;
    }
    return Usage();
}
