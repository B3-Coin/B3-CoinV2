// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// b3-hive-update: the update helper of the B3 Hive app
// (doc/design/b3-hive-update-system.md).
//
//   b3-hive-update --state-dir=<dir>
//
// The app starts it and writes one command per line on its stdin:
//
//   STATUS     report the current state
//   CHECK      fetch and verify the signed manifest
//   DOWNLOAD   download and verify the offered artifact (approval #1)
//
// Every command is answered by one line "RESULT <json>". While a CHECK or
// DOWNLOAD runs, the helper asks the app to fetch bytes (update/pipe.h);
// every policy decision stays here, in the same update manager the Qt
// client runs. The helper writes only inside <dir>: the rollback record and
// downloads/. It installs nothing, as no platform has an audited
// replacement-and-restart flow yet.

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <clientversion.h>
#include <key.h>
#include <univalue.h>
#include <update/filestore.h>
#include <update/manager.h>
#include <update/pipe.h>
#include <util/fs.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <util/translation.h>

#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>

#ifdef WIN32
#include <fcntl.h>
#include <io.h>
#endif

const TranslateFn G_TRANSLATION_FUN{nullptr};

namespace {

//! Developer builds may name a channel at run time, as the Qt client's
//! -hiveupdate* options allow; release builds use only the pinned one.
constexpr bool RUNTIME_CHANNEL{!B3_UPDATE_CHANNEL_CONFIGURED && !CLIENT_VERSION_IS_RELEASE};

class UnsupportedInstaller : public update::UpdateInstaller
{
public:
    bool Supported() const override { return false; }
    bool Launch(const fs::path&, std::string& error) override
    {
        error = "update-install-unsupported-platform";
        return false;
    }
};

int Usage()
{
    std::fprintf(stderr, "usage: b3-hive-update --state-dir=<dir>\n");
    if (RUNTIME_CHANNEL) {
        std::fprintf(stderr,
                     "developer channel (unpinned builds only):\n"
                     "  --manifest-url=<https url> --key=<x-only hex> (repeat)\n"
                     "  --threshold=<n> --host=<host> (repeat) --installed-version=<X.Y.Z>\n");
    }
    return 2;
}

void AddKey(update::ReleaseKeys& keys, std::string_view hex)
{
    const auto bytes{TryParseHex<unsigned char>(hex)};
    // A malformed key is left out, so the threshold fails closed.
    if (bytes && bytes->size() == 32) keys.keys.emplace_back(std::span<const unsigned char>{*bytes});
}

void SetHostPlatform(update::UpdateConfig& c)
{
#if defined(__APPLE__)
    c.os = "macos";
    c.format = "zip";
#elif defined(WIN32)
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
}

UniValue Result(const update::UpdateManager& manager, const update::UpdateConfig& config)
{
    UniValue r{UniValue::VOBJ};
    r.pushKV("state", update::UpdateStateName(manager.state()));
    r.pushKV("error", manager.last_error());
    r.pushKV("installed", config.installed.ToString());
    UniValue offer{UniValue::VNULL};
    if (const update::Artifact* a{manager.available()}) {
        offer.setObject();
        offer.pushKV("version", a->version.ToString());
        offer.pushKV("size", a->size);
        offer.pushKV("sha256", HexStr(a->sha256));
        offer.pushKV("url", a->url);
        const auto notes{manager.notes_digest()};
        offer.pushKV("notes_sha256", notes ? HexStr(*notes) : "");
    }
    r.pushKV("offer", offer);
    const auto& path{manager.downloaded_path()};
    r.pushKV("download", path ? UniValue{fs::PathToString(*path)} : UniValue{});
    return r;
}

} // namespace

int main(int argc, char** argv)
{
#ifdef WIN32
    // The protocol carries raw body bytes: no CRLF translation either way.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    update::UpdateConfig config;
#if B3_UPDATE_CHANNEL_CONFIGURED
    config.manifest_url = B3_UPDATE_MANIFEST_URL;
    config.keys.threshold = B3_UPDATE_SIGNATURE_THRESHOLD;
    for (const std::string& hex : util::SplitString(B3_UPDATE_PUBLIC_KEYS, ',')) AddKey(config.keys, hex);
    config.allowed_hosts = util::SplitString(B3_UPDATE_ALLOWED_HOSTS, ',');
#else
    config.keys.threshold = 2;
#endif
    config.installed = {CLIENT_VERSION_MAJOR, CLIENT_VERSION_MINOR, CLIENT_VERSION_BUILD};
    SetHostPlatform(config);

    fs::path state_dir;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        const auto value = [&](std::string_view name) -> std::optional<std::string> {
            if (!arg.starts_with(name)) return std::nullopt;
            return std::string{arg.substr(name.size())};
        };
        if (arg == "--version") {
            std::printf("b3-hive-update %s\n", FormatFullVersion().c_str());
            return 0;
        } else if (auto dir{value("--state-dir=")}) {
            state_dir = fs::u8path(*dir);
        } else if (auto url{RUNTIME_CHANNEL ? value("--manifest-url=") : std::nullopt}) {
            config.manifest_url = *url;
        } else if (auto key{RUNTIME_CHANNEL ? value("--key=") : std::nullopt}) {
            AddKey(config.keys, *key);
        } else if (auto threshold{RUNTIME_CHANNEL ? value("--threshold=") : std::nullopt}) {
            const auto n{ToIntegral<unsigned>(*threshold)};
            if (!n) return Usage();
            config.keys.threshold = *n;
        } else if (auto host{RUNTIME_CHANNEL ? value("--host=") : std::nullopt}) {
            config.allowed_hosts.push_back(*host);
        } else if (auto installed{RUNTIME_CHANNEL ? value("--installed-version=") : std::nullopt}) {
            const auto v{update::ParseVersion(*installed)};
            if (!v) return Usage();
            config.installed = *v;
        } else {
            return Usage();
        }
    }
    if (state_dir.empty()) return Usage();
    if (RUNTIME_CHANNEL && config.allowed_hosts.empty() && !config.manifest_url.empty()) {
        const std::string host{update::HttpsHost(config.manifest_url)};
        if (!host.empty()) config.allowed_hosts.push_back(host);
    }
    config.download_dir = state_dir / fs::u8path("downloads");
    std::error_code ec;
    fs::create_directories(config.download_dir, ec);
    if (ec) {
        std::fprintf(stderr, "cannot create %s\n", fs::PathToString(config.download_dir).c_str());
        return 1;
    }

    ECC_Context ecc;
    update::FilePipeChannel channel{stdin, stdout};
    update::PipeTransport transport{channel};
    UnsupportedInstaller installer;
    update::FileSequenceStore sequences{state_dir / fs::u8path("sequence")};
    update::UpdateManager manager{config, transport, installer, sequences,
                                  [] { return static_cast<int64_t>(::time(nullptr)); }};

    std::string line;
    while (channel.ReadLine(line)) {
        if (line == "CHECK") {
            manager.CheckNow();
        } else if (line == "DOWNLOAD") {
            manager.StartDownload();
        } else if (line != "STATUS") {
            std::fprintf(stderr, "unknown command\n");
            return 1;
        }
        if (!channel.Write("RESULT " + Result(manager, config).write() + "\n")) return 1;
        // After a broken frame the stream position is unknown: stop here.
        if (transport.broken()) return 1;
    }
    return 0;
}
