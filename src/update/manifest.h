// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3COIN_UPDATE_MANIFEST_H
#define B3COIN_UPDATE_MANIFEST_H

#include <pubkey.h>
#include <uint256.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/** B3 Hive update manifest: strict byte-canonical codec and threshold
 *  verifier (doc/design/b3-hive-update-system.md).
 *
 *  ISOLATION: this component must never include consensus, wallet,
 *  FlowMesh, bridge or P2P headers. Release-signing keys are
 *  update-specific and disjoint from every other key domain.
 *
 *  Canonical-by-construction: signatures sign the RAW payload bytes of
 *  the manifest file (the magic line through the newline preceding the
 *  signature separator) under TaggedHash("B3/HIVE/UPDATE/V1", payload).
 *  The strict parser rejects any deviation, so exactly one byte string
 *  corresponds to any accepted manifest.
 */
namespace update {

//! Hard bound on a manifest file; anything larger is rejected unread.
inline constexpr size_t MAX_MANIFEST_BYTES{64 * 1024};
inline constexpr size_t MAX_ARTIFACTS{32};
//! Allowed clock skew for `published` being in the future.
inline constexpr int64_t MAX_PUBLISH_SKEW{2 * 60 * 60};
inline constexpr std::string_view MANIFEST_MAGIC{"B3-HIVE-MANIFEST-V1"};
inline constexpr std::string_view SIGNATURE_SEPARATOR{"-----SIGNATURES-----"};
inline constexpr std::string_view MANIFEST_TAG{"B3/HIVE/UPDATE/V1"};

struct Version {
    uint32_t major{0}, minor{0}, patch{0};
    friend auto operator<=>(const Version&, const Version&) = default;
    std::string ToString() const;
};
//! Strict semver "X.Y.Z", decimal, no leading zeros.
std::optional<Version> ParseVersion(std::string_view s);

struct Artifact {
    std::string os;      // macos | windows | linux
    std::string arch;    // arm64 | x86 | x86_64
    std::string format;  // dmg | pkg | exe | appimage | targz
    Version version{};
    uint64_t size{0};
    uint256 sha256{};
    std::string url;     // https only; host checked against the approved list
};

struct Manifest {
    std::string channel;
    uint64_t sequence{0};
    int64_t published{0};
    int64_t expires{0};
    uint256 notes_sha256{};
    std::vector<Artifact> artifacts;
    //! TaggedHash(MANIFEST_TAG, payload bytes) — the signed message.
    uint256 payload_hash{};
};

//! Pinned release keys. An EMPTY key set or zero threshold means the
//! updater is unconfigured and must fail closed (no parse, no network).
struct ReleaseKeys {
    std::vector<XOnlyPubKey> keys;
    unsigned threshold{0};
    bool Configured() const
    {
        if (threshold == 0 || keys.size() < threshold) return false;
        for (size_t i = 0; i < keys.size(); ++i) {
            if (!keys[i].IsFullyValid()) return false;
            for (size_t j = 0; j < i; ++j) {
                // A threshold counts distinct release authorities, never
                // repeated vector slots containing the same authority.
                if (keys[i] == keys[j]) return false;
            }
        }
        return true;
    }
};

//! First 4 bytes of the x-only key, lower-case hex — the manifest key id.
std::string ReleaseKeyId(const XOnlyPubKey& key);

/** Parse the FULL manifest file (payload + signature section) strictly and
 *  verify the signature threshold against the pinned keys. Returns the
 *  manifest on success; otherwise nullopt with a stable error token in
 *  `error` (e.g. "update-manifest-oversize", "update-manifest-magic",
 *  "update-manifest-field-order", "update-manifest-sig-threshold"). */
std::optional<Manifest> ParseAndVerifyManifest(std::string_view file_bytes,
                                               const ReleaseKeys& keys,
                                               std::string& error);

//! What this installation is and will accept.
struct HostPolicy {
    std::string os;
    std::string arch;
    std::string format;
    std::string channel{"stable"};
    Version installed{};
    uint64_t last_accepted_sequence{0};
    int64_t now{0};
    std::vector<std::string> allowed_hosts;
    uint64_t max_artifact_bytes{512ull * 1024 * 1024};
};

/** Apply the acceptance matrix to a verified manifest: channel, expiry,
 *  publish skew, sequence monotonicity, platform match, newer version,
 *  https + approved host, artifact size bound. Returns the artifact to
 *  offer, or nullptr with a stable error token. */
const Artifact* SelectArtifact(const Manifest& manifest, const HostPolicy& host,
                               std::string& error);

//! Lower-case host of an https URL (empty if not strictly https or malformed;
//! userinfo, empty host and embedded credentials are rejected).
std::string HttpsHost(std::string_view url);

} // namespace update

#endif // B3COIN_UPDATE_MANIFEST_H
