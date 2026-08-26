// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3COIN_UPDATE_DOWNLOADER_H
#define B3COIN_UPDATE_DOWNLOADER_H

#include <update/manifest.h>
#include <util/fs.h>

#include <functional>
#include <string>

/** Download safety for the B3 Hive update system
 *  (doc/design/b3-hive-update-system.md; isolated from consensus/wallet).
 *
 *  The transport is dependency-injected so every safety property is
 *  testable without the public internet; the PRODUCTION transport is a
 *  thin Qt implementation that performs no policy of its own — all
 *  policy (https-only, approved hosts, redirect restriction, byte
 *  bounds, digest verification, atomicity) lives here and is enforced
 *  regardless of transport behavior.
 */
namespace update {

//! Maximum redirects a fetch may follow.
inline constexpr unsigned MAX_REDIRECTS{3};

/** Abstract byte transport. Implementations MUST:
 *  - call on_redirect for every redirect target BEFORE following it and
 *    abort the transfer if it returns false;
 *  - deliver the body incrementally through sink and abort if it
 *    returns false;
 *  - return false (with error) on any network failure.
 *  Implementations perform NO policy checks of their own. */
class UpdateTransport
{
public:
    virtual ~UpdateTransport() = default;
    virtual bool Fetch(const std::string& url,
                       const std::function<bool(std::string_view chunk)>& sink,
                       const std::function<bool(const std::string& redirect_url)>& on_redirect,
                       std::string& error) = 0;
};

struct FetchPolicy {
    std::vector<std::string> allowed_hosts;
};

/** Fetch the manifest file: https-only, approved hosts (including every
 *  redirect hop), bounded to MAX_MANIFEST_BYTES. Returns the raw bytes
 *  (verification is the caller's next step) or nullopt + error token. */
std::optional<std::string> FetchManifestBytes(UpdateTransport& transport,
                                              const std::string& url,
                                              const FetchPolicy& policy,
                                              std::string& error);

/** Download an artifact into `dir` with full safety:
 *  - url + every redirect hop must be https on an approved host;
 *  - the stream must be EXACTLY artifact.size bytes (over = abort mid-
 *    stream, under = reject at end);
 *  - SHA-256 computed while streaming and must equal artifact.sha256;
 *  - bytes land in a random O_EXCL temp file inside `dir` (symlink-safe)
 *    and are atomically renamed to "hive-update-<sha256>.<format>" only
 *    after full verification;
 *  - ANY failure (network, policy, size, digest, write) removes the
 *    temp file and leaves nothing partially visible. The installed
 *    application is never a write target of this component.
 *  Returns the final path or nullopt + error token. */
std::optional<fs::path> FetchArtifact(UpdateTransport& transport,
                                      const Artifact& artifact,
                                      const fs::path& dir,
                                      const FetchPolicy& policy,
                                      std::string& error);

} // namespace update

#endif // B3COIN_UPDATE_DOWNLOADER_H
