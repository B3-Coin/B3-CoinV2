// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <update/downloader.h>

#include <crypto/sha256.h>
#include <random.h>
#include <util/fs.h>

#include <algorithm>
#include <cstdio>

namespace update {
namespace {

bool HostApproved(const std::string& url, const FetchPolicy& policy)
{
    const std::string host{HttpsHost(url)};
    if (host.empty()) return false;
    return std::find(policy.allowed_hosts.begin(), policy.allowed_hosts.end(), host) !=
           policy.allowed_hosts.end();
}

} // namespace

std::optional<std::string> FetchManifestBytes(UpdateTransport& transport, const std::string& url,
                                              const FetchPolicy& policy, std::string& error)
{
    error.clear();
    if (!HostApproved(url, policy)) { error = "update-fetch-host"; return std::nullopt; }
    std::string out;
    unsigned redirects{0};
    bool overflow{false};
    const bool ok{transport.Fetch(
        url,
        [&](std::string_view chunk) {
            if (out.size() + chunk.size() > MAX_MANIFEST_BYTES) { overflow = true; return false; }
            out.append(chunk);
            return true;
        },
        [&](const std::string& redirect_url) {
            if (++redirects > MAX_REDIRECTS) return false;
            return HostApproved(redirect_url, policy);
        },
        error)};
    if (overflow) { error = "update-fetch-oversize"; return std::nullopt; }
    if (!ok) { if (error.empty()) error = "update-fetch-failed"; return std::nullopt; }
    return out;
}

std::optional<fs::path> FetchArtifact(UpdateTransport& transport, const Artifact& artifact,
                                      const fs::path& dir, const FetchPolicy& policy,
                                      std::string& error)
{
    error.clear();
    if (!HostApproved(artifact.url, policy)) { error = "update-fetch-host"; return std::nullopt; }

    // Random O_EXCL temp name: a pre-planted file or symlink at the path
    // makes creation fail instead of following it.
    uint64_t rnd[2];
    GetStrongRandBytes(std::span<unsigned char>{reinterpret_cast<unsigned char*>(rnd), sizeof(rnd)});
    const fs::path tmp{dir / fs::u8path("hive-update-" + std::to_string(rnd[0]) +
                                        std::to_string(rnd[1]) + ".part")};
    FILE* f{fsbridge::fopen(tmp, "wbx")};
    if (!f) { error = "update-fetch-tempfile"; return std::nullopt; }

    CSHA256 hasher;
    uint64_t received{0};
    bool overflow{false}, write_failed{false};
    unsigned redirects{0};
    const bool ok{transport.Fetch(
        artifact.url,
        [&](std::string_view chunk) {
            if (received + chunk.size() > artifact.size) { overflow = true; return false; }
            if (std::fwrite(chunk.data(), 1, chunk.size(), f) != chunk.size()) {
                write_failed = true;
                return false;
            }
            hasher.Write(reinterpret_cast<const unsigned char*>(chunk.data()), chunk.size());
            received += chunk.size();
            return true;
        },
        [&](const std::string& redirect_url) {
            if (++redirects > MAX_REDIRECTS) return false;
            return HostApproved(redirect_url, policy);
        },
        error)};

    auto fail = [&](const char* token) -> std::optional<fs::path> {
        std::fclose(f);
        fs::remove(tmp);
        if (error.empty() || token[0] != '\0') error = token;
        return std::nullopt;
    };
    if (overflow) return fail("update-fetch-oversize");
    if (write_failed) return fail("update-fetch-write");
    if (!ok) { if (error.empty()) error = "update-fetch-failed"; std::fclose(f); fs::remove(tmp); return std::nullopt; }
    if (received != artifact.size) return fail("update-fetch-truncated");
    if (std::fflush(f) != 0) return fail("update-fetch-write");
    uint256 got;
    hasher.Finalize(got.begin());
    if (got != artifact.sha256) return fail("update-fetch-digest");
    std::fclose(f);

    const fs::path final_path{dir / fs::u8path("hive-update-" + got.GetHex() + "." + artifact.format)};
    std::error_code ec;
    fs::rename(tmp, final_path, ec); // atomic within the directory
    if (ec) { fs::remove(tmp); error = "update-fetch-rename"; return std::nullopt; }
    return final_path;
}

} // namespace update
