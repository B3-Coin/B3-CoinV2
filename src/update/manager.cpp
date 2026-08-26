// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <update/manager.h>

namespace update {

const char* UpdateStateName(UpdateState s)
{
    switch (s) {
    case UpdateState::UNCONFIGURED: return "unconfigured";
    case UpdateState::IDLE: return "idle";
    case UpdateState::CHECKING: return "checking";
    case UpdateState::UPDATE_AVAILABLE: return "update-available";
    case UpdateState::DOWNLOADING: return "downloading";
    case UpdateState::READY_TO_INSTALL: return "ready-to-install";
    case UpdateState::AWAITING_SHUTDOWN: return "awaiting-shutdown";
    case UpdateState::INSTALLING: return "installing";
    case UpdateState::FAILED: return "failed";
    }
    return "?";
}

int64_t JitteredCheckInterval(int64_t base_seconds, uint64_t entropy)
{
    if (base_seconds <= 0) return 0;
    const int64_t quarter{base_seconds / 4};
    if (quarter == 0) return base_seconds;
    const int64_t offset{static_cast<int64_t>(entropy % (2 * static_cast<uint64_t>(quarter) + 1)) - quarter};
    return base_seconds + offset;
}

UpdateManager::UpdateManager(UpdateConfig config, UpdateTransport& transport,
                             UpdateInstaller& installer, SequenceStore& sequences,
                             std::function<int64_t()> clock)
    : m_config{std::move(config)},
      m_transport{transport},
      m_installer{installer},
      m_sequences{sequences},
      m_clock{std::move(clock)}
{
    if (!m_config.Configured()) {
        m_state = UpdateState::UNCONFIGURED;
        return;
    }
    if (!m_sequences.Load(m_sequences_state, m_error)) {
        if (m_error.empty()) m_error = "update-sequence-load";
        m_state = UpdateState::FAILED;
        return;
    }
    if (m_sequences_state.pending &&
        (m_sequences_state.pending->sequence == 0 ||
         m_sequences_state.pending->sequence != m_sequences_state.floor)) {
        m_error = "update-sequence-state";
        m_state = UpdateState::FAILED;
        return;
    }
    m_persistence_ready = true;
    m_state = UpdateState::IDLE;
}

const Artifact* UpdateManager::available() const
{
    return m_offered;
}

std::optional<uint256> UpdateManager::notes_digest() const
{
    if (!m_manifest) return std::nullopt;
    return m_manifest->notes_sha256;
}

HostPolicy UpdateManager::MakeHostPolicy(bool exact_pending) const
{
    HostPolicy h;
    h.os = m_config.os;
    h.arch = m_config.arch;
    h.format = m_config.format;
    h.channel = m_config.channel;
    h.installed = m_config.installed;
    // The one exact pending manifest at the floor may be re-offered. Lower
    // sequences and different bytes at the same sequence remain rejected.
    h.last_accepted_sequence = exact_pending ? m_sequences_state.floor - 1
                                             : m_sequences_state.floor;
    h.now = m_clock();
    h.allowed_hosts = m_config.allowed_hosts;
    h.max_artifact_bytes = m_config.max_artifact_bytes;
    return h;
}

bool UpdateManager::CheckNow()
{
    if (m_state == UpdateState::UNCONFIGURED) return false; // quiet fail-closed
    if (!m_persistence_ready) return false; // never fetch without rollback state
    if (m_state == UpdateState::DOWNLOADING || m_state == UpdateState::AWAITING_SHUTDOWN ||
        m_state == UpdateState::INSTALLING) {
        return false; // never disturb an in-flight step
    }
    m_state = UpdateState::CHECKING;
    m_error.clear();
    m_manifest.reset();
    m_offered = nullptr;
    m_downloaded.reset();

    const auto bytes{FetchManifestBytes(m_transport, m_config.manifest_url,
                                        {m_config.allowed_hosts}, m_error)};
    if (!bytes) { m_state = UpdateState::FAILED; return false; }
    auto manifest{ParseAndVerifyManifest(*bytes, m_config.keys, m_error)};
    if (!manifest) { m_state = UpdateState::FAILED; return false; }
    m_manifest = std::move(*manifest);
    const bool exact_pending{
        m_sequences_state.pending &&
        m_manifest->sequence == m_sequences_state.pending->sequence &&
        m_manifest->payload_hash == m_sequences_state.pending->payload_hash};
    const Artifact* a{SelectArtifact(*m_manifest, MakeHostPolicy(exact_pending), m_error)};
    if (!a) {
        // "Nothing newer" outcomes are IDLE, not failures.
        const bool benign{m_error == "update-reject-not-newer" ||
                          m_error == "update-reject-rollback"};
        m_state = benign ? UpdateState::IDLE : UpdateState::FAILED;
        m_manifest.reset();
        return false;
    }
    if (!exact_pending) {
        // Persist before publishing the offer. Storage failure is observable
        // and fail-closed: no UI-visible offer may escape without a durable
        // rollback floor and exact retry identity.
        SequenceState next{m_manifest->sequence,
                           PendingOffer{m_manifest->sequence, m_manifest->payload_hash}};
        if (!m_sequences.Store(next, m_error)) {
            if (m_error.empty()) m_error = "update-sequence-store";
            m_manifest.reset();
            m_state = UpdateState::FAILED;
            return false;
        }
        m_sequences_state = next;
    }
    m_offered = a;
    m_state = UpdateState::UPDATE_AVAILABLE;
    return true;
}

bool UpdateManager::StartDownload()
{
    if (m_state != UpdateState::UPDATE_AVAILABLE || !m_offered) return false;
    m_state = UpdateState::DOWNLOADING;
    m_error.clear();
    const auto path{FetchArtifact(m_transport, *m_offered, m_config.download_dir,
                                  {m_config.allowed_hosts}, m_error)};
    if (!path) {
        m_state = UpdateState::FAILED;
        return false;
    }
    m_downloaded = path;
    m_state = UpdateState::READY_TO_INSTALL;
    return true;
}

bool UpdateManager::RequestInstall()
{
    if (m_state != UpdateState::READY_TO_INSTALL || !m_downloaded) return false;
    if (!m_installer.Supported()) {
        m_error = "update-install-unsupported-platform";
        return false;
    }
    m_state = UpdateState::AWAITING_SHUTDOWN;
    return true;
}

bool UpdateManager::OnShutdownComplete()
{
    if (m_state != UpdateState::AWAITING_SHUTDOWN || !m_downloaded) return false;
    m_state = UpdateState::INSTALLING;
    if (!m_installer.Launch(*m_downloaded, m_error)) {
        // The previous installation remains usable; the verified artifact is
        // kept so the user may retry.
        m_state = UpdateState::FAILED;
        return false;
    }
    return true;
}

} // namespace update
