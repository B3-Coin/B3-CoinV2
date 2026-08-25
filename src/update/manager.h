// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3COIN_UPDATE_MANAGER_H
#define B3COIN_UPDATE_MANAGER_H

#include <update/downloader.h>
#include <update/manifest.h>

#include <functional>
#include <optional>
#include <string>

/** B3 Hive update manager state machine
 *  (doc/design/b3-hive-update-system.md).
 *
 *  Explicit-approval discipline: checking may be automatic; DOWNLOAD and
 *  INSTALL each require their own user-initiated call. The installer can
 *  ONLY be reached through RequestInstall() -> (orderly node shutdown by
 *  the caller) -> OnShutdownComplete(); there is no other path, so the
 *  updater can never run while databases or wallets are open. This
 *  component never touches wallet files, keys, datadirs, block
 *  databases or configuration -- its only writable location is the
 *  configured download directory.
 */
namespace update {

enum class UpdateState {
    UNCONFIGURED,      // no endpoint or keys: fail closed, quiet, no network
    IDLE,              // configured; nothing to offer
    CHECKING,
    UPDATE_AVAILABLE,  // verified manifest offers a newer release
    DOWNLOADING,
    READY_TO_INSTALL,  // artifact on disk, size+digest verified
    AWAITING_SHUTDOWN, // user approved install; waiting for orderly node stop
    INSTALLING,        // installer handed off
    FAILED,            // last step failed; installed app unchanged
};

const char* UpdateStateName(UpdateState s);

//! Launches the platform installer/updater helper AFTER shutdown. Given an
//! executable artifact path; implementations must exec with argv arrays,
//! never shell strings, and must not elevate privileges silently.
class UpdateInstaller
{
public:
    virtual ~UpdateInstaller() = default;
    virtual bool Launch(const fs::path& verified_artifact, std::string& error) = 0;
};

//! Persistence for the last ACCEPTED manifest sequence (rollback floor).
class SequenceStore
{
public:
    virtual ~SequenceStore() = default;
    virtual uint64_t Load() = 0;
    virtual void Store(uint64_t sequence) = 0;
};

struct UpdateConfig {
    std::string manifest_url;   // empty => unconfigured
    ReleaseKeys keys;           // unconfigured unless threshold satisfiable
    std::string os, arch, format;
    std::string channel{"stable"};
    Version installed{};
    std::vector<std::string> allowed_hosts;
    uint64_t max_artifact_bytes{512ull * 1024 * 1024};
    fs::path download_dir;

    bool Configured() const
    {
        return !manifest_url.empty() && keys.Configured() && !allowed_hosts.empty() &&
               !download_dir.empty();
    }
};

/** Randomized check interval: base +/- up to 25%, so a fleet of clients
 *  never contacts the release host simultaneously. `entropy` is any
 *  caller-supplied random value. */
int64_t JitteredCheckInterval(int64_t base_seconds, uint64_t entropy);

class UpdateManager
{
public:
    UpdateManager(UpdateConfig config, UpdateTransport& transport, UpdateInstaller& installer,
                  SequenceStore& sequences, std::function<int64_t()> clock);

    UpdateState state() const { return m_state; }
    const std::string& last_error() const { return m_error; }
    //! The offered artifact (UPDATE_AVAILABLE and later), if any.
    const Artifact* available() const;
    //! Release-notes digest of the offered manifest (for signed-notes display).
    std::optional<uint256> notes_digest() const;
    const std::optional<fs::path>& downloaded_path() const { return m_downloaded; }

    /** Automatic or "Check now". Quiet no-op when unconfigured. Network
     *  request carries nothing but the manifest URL itself (privacy). */
    bool CheckNow();

    //! Explicit user approval #1. Only from UPDATE_AVAILABLE.
    bool StartDownload();

    //! Explicit user approval #2 ("Install and restart"): announces intent;
    //! the CALLER then performs an orderly node/wallet shutdown.
    bool RequestInstall();

    //! Called by the host application ONLY after databases and wallets are
    //! fully closed. The single path that launches the installer.
    bool OnShutdownComplete();

private:
    UpdateConfig m_config;
    UpdateTransport& m_transport;
    UpdateInstaller& m_installer;
    SequenceStore& m_sequences;
    std::function<int64_t()> m_clock;

    UpdateState m_state{UpdateState::UNCONFIGURED};
    std::string m_error;
    std::optional<Manifest> m_manifest;
    const Artifact* m_offered{nullptr};
    std::optional<fs::path> m_downloaded;

    HostPolicy MakeHostPolicy() const;
};

} // namespace update

#endif // B3COIN_UPDATE_MANAGER_H
