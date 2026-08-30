// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <update/manifest.h>

#include <hash.h>
#include <util/strencodings.h>

#include <algorithm>
#include <set>

namespace update {
namespace {

//! Strict decimal u64: non-empty, digits only, no leading zero (except "0").
std::optional<uint64_t> StrictU64(std::string_view s)
{
    if (s.empty() || s.size() > 20) return std::nullopt;
    if (s.size() > 1 && s[0] == '0') return std::nullopt;
    uint64_t v{0};
    for (char c : s) {
        if (c < '0' || c > '9') return std::nullopt;
        const uint64_t d{static_cast<uint64_t>(c - '0')};
        if (v > (UINT64_MAX - d) / 10) return std::nullopt;
        v = v * 10 + d;
    }
    return v;
}

std::optional<uint256> Strict32Hex(std::string_view s)
{
    if (s.size() != 64) return std::nullopt;
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return std::nullopt;
    }
    auto b{TryParseHex<unsigned char>(std::string{s})};
    if (!b || b->size() != 32) return std::nullopt;
    return uint256{std::span<const unsigned char>{*b}};
}

//! Split "key=value"; strict: exactly one '=', non-empty key.
bool SplitKV(std::string_view line, std::string_view& key, std::string_view& value)
{
    const size_t eq{line.find('=')};
    if (eq == std::string_view::npos || eq == 0) return false;
    key = line.substr(0, eq);
    value = line.substr(eq + 1);
    return true;
}

const std::set<std::string_view> VALID_OS{"macos", "windows", "linux"};
const std::set<std::string_view> VALID_ARCH{"arm64", "x86", "x86_64"};
const std::set<std::string_view> VALID_FORMAT{"dmg", "pkg", "exe", "appimage", "targz"};

} // namespace

std::string Version::ToString() const
{
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

std::optional<Version> ParseVersion(std::string_view s)
{
    Version out;
    uint32_t* fields[3]{&out.major, &out.minor, &out.patch};
    for (int i = 0; i < 3; ++i) {
        const size_t dot{s.find('.')};
        const std::string_view part{i < 2 ? s.substr(0, dot) : s};
        if (i < 2 && dot == std::string_view::npos) return std::nullopt;
        const auto v{StrictU64(part)};
        if (!v || *v > 0xffffffffULL) return std::nullopt;
        *fields[i] = static_cast<uint32_t>(*v);
        if (i < 2) s = s.substr(dot + 1);
    }
    return out;
}

std::string ReleaseKeyId(const XOnlyPubKey& key)
{
    return HexStr(std::span<const unsigned char>{key.data(), 4});
}

std::string HttpsHost(std::string_view url)
{
    static constexpr std::string_view scheme{"https://"};
    if (url.substr(0, scheme.size()) != scheme) return {};
    std::string_view rest{url.substr(scheme.size())};
    const size_t end{rest.find_first_of("/?#")};
    std::string_view authority{end == std::string_view::npos ? rest : rest.substr(0, end)};
    if (authority.empty()) return {};
    if (authority.find('@') != std::string_view::npos) return {}; // no userinfo
    const size_t colon{authority.find(':')};
    if (colon != std::string_view::npos) {
        // Explicit port allowed only if it is exactly 443.
        if (authority.substr(colon + 1) != "443") return {};
        authority = authority.substr(0, colon);
    }
    if (authority.empty()) return {};
    std::string host{authority};
    std::transform(host.begin(), host.end(), host.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return host;
}

std::optional<Manifest> ParseAndVerifyManifest(std::string_view file, const ReleaseKeys& keys,
                                               std::string& error)
{
    error.clear();
    if (!keys.Configured()) { error = "update-keys-unconfigured"; return std::nullopt; }
    if (file.size() > MAX_MANIFEST_BYTES) { error = "update-manifest-oversize"; return std::nullopt; }
    if (file.find('\r') != std::string_view::npos) { error = "update-manifest-crlf"; return std::nullopt; }
    if (file.empty() || file.back() != '\n') { error = "update-manifest-truncated"; return std::nullopt; }

    // Locate the signature separator line; the payload is everything before it.
    const std::string sep_line{std::string{SIGNATURE_SEPARATOR} + "\n"};
    const size_t sep{file.find(sep_line)};
    if (sep == std::string_view::npos || (sep != 0 && file[sep - 1] != '\n')) {
        error = "update-manifest-no-signatures";
        return std::nullopt;
    }
    const std::string_view payload{file.substr(0, sep)};
    const std::string_view sig_section{file.substr(sep + sep_line.size())};

    // ---- payload lines, strict order ----
    std::vector<std::string_view> lines;
    {
        std::string_view rest{payload};
        while (!rest.empty()) {
            const size_t nl{rest.find('\n')};
            if (nl == std::string_view::npos) { error = "update-manifest-truncated"; return std::nullopt; }
            lines.push_back(rest.substr(0, nl));
            rest = rest.substr(nl + 1);
        }
    }
    size_t at{0};
    auto next_line = [&]() -> std::optional<std::string_view> {
        if (at >= lines.size()) return std::nullopt;
        return lines[at++];
    };
    auto expect_field = [&](std::string_view want, std::string_view& value) -> bool {
        const auto line{next_line()};
        if (!line) return false;
        std::string_view k, v;
        if (!SplitKV(*line, k, v) || k != want) return false;
        value = v;
        return true;
    };

    {
        const auto magic{next_line()};
        if (!magic || *magic != MANIFEST_MAGIC) { error = "update-manifest-magic"; return std::nullopt; }
    }

    Manifest m;
    std::string_view v;
    if (!expect_field("channel", v) || v.empty() || v.size() > 32) { error = "update-manifest-field-order"; return std::nullopt; }
    m.channel = std::string{v};
    if (!expect_field("sequence", v)) { error = "update-manifest-field-order"; return std::nullopt; }
    if (auto n{StrictU64(v)}; n) m.sequence = *n; else { error = "update-manifest-number"; return std::nullopt; }
    if (!expect_field("published", v)) { error = "update-manifest-field-order"; return std::nullopt; }
    if (auto n{StrictU64(v)}; n && *n <= uint64_t{INT64_MAX}) m.published = static_cast<int64_t>(*n); else { error = "update-manifest-number"; return std::nullopt; }
    if (!expect_field("expires", v)) { error = "update-manifest-field-order"; return std::nullopt; }
    if (auto n{StrictU64(v)}; n && *n <= uint64_t{INT64_MAX}) m.expires = static_cast<int64_t>(*n); else { error = "update-manifest-number"; return std::nullopt; }
    if (!expect_field("notes_sha256", v)) { error = "update-manifest-field-order"; return std::nullopt; }
    if (auto h{Strict32Hex(v)}; h) m.notes_sha256 = *h; else { error = "update-manifest-hex"; return std::nullopt; }

    // Artifact blocks: at least one, exact field order within each.
    while (at < lines.size()) {
        Artifact a;
        if (!expect_field("artifact.os", v) || !VALID_OS.count(v)) { error = "update-manifest-artifact"; return std::nullopt; }
        a.os = std::string{v};
        if (!expect_field("artifact.arch", v) || !VALID_ARCH.count(v)) { error = "update-manifest-artifact"; return std::nullopt; }
        a.arch = std::string{v};
        if (!expect_field("artifact.format", v) || !VALID_FORMAT.count(v)) { error = "update-manifest-artifact"; return std::nullopt; }
        a.format = std::string{v};
        if (!expect_field("artifact.version", v)) { error = "update-manifest-artifact"; return std::nullopt; }
        if (auto ver{ParseVersion(v)}; ver) a.version = *ver; else { error = "update-manifest-version"; return std::nullopt; }
        if (!expect_field("artifact.size", v)) { error = "update-manifest-artifact"; return std::nullopt; }
        if (auto n{StrictU64(v)}; n && *n > 0) a.size = *n; else { error = "update-manifest-number"; return std::nullopt; }
        if (!expect_field("artifact.sha256", v)) { error = "update-manifest-artifact"; return std::nullopt; }
        if (auto h{Strict32Hex(v)}; h) a.sha256 = *h; else { error = "update-manifest-hex"; return std::nullopt; }
        if (!expect_field("artifact.url", v) || v.size() > 1024) { error = "update-manifest-artifact"; return std::nullopt; }
        a.url = std::string{v};
        if (HttpsHost(a.url).empty()) { error = "update-manifest-url"; return std::nullopt; }
        m.artifacts.push_back(std::move(a));
        if (m.artifacts.size() > MAX_ARTIFACTS) { error = "update-manifest-artifact-count"; return std::nullopt; }
    }
    if (m.artifacts.empty()) { error = "update-manifest-artifact"; return std::nullopt; }
    // No duplicate (os, arch, format) targets — two artifacts for one host
    // would make selection ambiguous.
    {
        std::set<std::string> seen;
        for (const auto& a : m.artifacts) {
            if (!seen.insert(a.os + "/" + a.arch + "/" + a.format).second) {
                error = "update-manifest-duplicate-target";
                return std::nullopt;
            }
        }
    }

    // ---- signed message ----
    HashWriter hw{TaggedHash(std::string{MANIFEST_TAG})};
    hw << std::span<const unsigned char>{reinterpret_cast<const unsigned char*>(payload.data()), payload.size()};
    m.payload_hash = hw.GetSHA256();

    // ---- signature section: only sig= lines, threshold of DISTINCT pinned keys ----
    std::set<size_t> signed_keys;
    {
        std::string_view rest{sig_section};
        size_t sig_lines{0};
        while (!rest.empty()) {
            const size_t nl{rest.find('\n')};
            if (nl == std::string_view::npos) { error = "update-manifest-truncated"; return std::nullopt; }
            const std::string_view line{rest.substr(0, nl)};
            rest = rest.substr(nl + 1);
            std::string_view k, val;
            if (!SplitKV(line, k, val) || k != "sig") { error = "update-manifest-sig-format"; return std::nullopt; }
            if (++sig_lines > 64) { error = "update-manifest-sig-format"; return std::nullopt; }
            const size_t colon{val.find(':')};
            if (colon != 8 || val.size() != 8 + 1 + 128) { error = "update-manifest-sig-format"; return std::nullopt; }
            const std::string keyid{val.substr(0, 8)};
            const auto sigbytes{TryParseHex<unsigned char>(std::string{val.substr(9)})};
            if (!sigbytes || sigbytes->size() != 64) { error = "update-manifest-sig-format"; return std::nullopt; }
            for (size_t i = 0; i < keys.keys.size(); ++i) {
                if (ReleaseKeyId(keys.keys[i]) != keyid) continue;
                if (keys.keys[i].VerifySchnorr(m.payload_hash, *sigbytes)) {
                    signed_keys.insert(i); // duplicates collapse; invalid sigs never count
                } else {
                    error = "update-manifest-sig-invalid";
                    return std::nullopt;
                }
            }
            // Unknown key ids are IGNORED (rotation overlap) — not counted.
        }
    }
    if (signed_keys.size() < keys.threshold) { error = "update-manifest-sig-threshold"; return std::nullopt; }
    return m;
}

const Artifact* SelectArtifact(const Manifest& m, const HostPolicy& host, std::string& error)
{
    error.clear();
    if (m.channel != host.channel) { error = "update-reject-channel"; return nullptr; }
    if (host.now >= m.expires) { error = "update-reject-expired"; return nullptr; }
    if (m.published > host.now + MAX_PUBLISH_SKEW) { error = "update-reject-future"; return nullptr; }
    if (m.expires <= m.published) { error = "update-reject-expired"; return nullptr; }
    if (m.sequence <= host.last_accepted_sequence) { error = "update-reject-rollback"; return nullptr; }

    const Artifact* found{nullptr};
    for (const auto& a : m.artifacts) {
        if (a.os == host.os && a.arch == host.arch && a.format == host.format) { found = &a; break; }
    }
    if (!found) { error = "update-reject-platform"; return nullptr; }
    if (!(host.installed < found->version)) { error = "update-reject-not-newer"; return nullptr; }
    if (found->size > host.max_artifact_bytes) { error = "update-reject-artifact-size"; return nullptr; }
    const std::string h{HttpsHost(found->url)};
    if (h.empty()) { error = "update-reject-url"; return nullptr; }
    if (std::find(host.allowed_hosts.begin(), host.allowed_hosts.end(), h) == host.allowed_hosts.end()) {
        error = "update-reject-host";
        return nullptr;
    }
    return found;
}

} // namespace update
