// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef B3_FLOWMESH2_STORAGE_CONTROL_LOG_H
#define B3_FLOWMESH2_STORAGE_CONTROL_LOG_H

#include <flowmesh2/core/result.h>
#include <flowmesh2/storage/control_types.h>
#include <flowmesh2/storage/durable.h>
#include <flowmesh2/storage/journal.h>

#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace flowmesh2::storage {

inline constexpr size_t MAX_CONTROL_MESSAGE_BYTES{8 * 1024 * 1024};
inline constexpr uint64_t MAX_CONTROL_VIEW{1'024};
inline constexpr size_t MAX_CONTROL_INSTANCES{4'096};
inline constexpr size_t MAX_CONTROL_HISTORY_BYTES{64 * 1024 * 1024};

//! A trusted fixed-committee configuration, independently checked by agreement.
//! The control journal has its own exclusive SIGNER instance and directory.
struct ControlConfig {
    uint256 domain;
    uint256 profile_hash;
    uint256 seat_set_root;
    uint64_t epoch;
    uint32_t self_seat;
    uint32_t seat_count;
    bls::PublicKey signer_key;
};

//! A VIEW_CHANGE intent is also the atomic durable entry to CHANGING.
//! The exact report bytes are immutable even if a later preparation is learned.
struct ControlIntentRecord {
    ControlKey key;
    std::vector<unsigned char> message;
    uint256 Digest() const;
    std::vector<unsigned char> Encode() const;
    static Result<ControlIntentRecord> Decode(std::span<const unsigned char> bytes);
    friend bool operator==(const ControlIntentRecord&, const ControlIntentRecord&) = default;
};
struct SignedControlRecord {
    ControlKey key;
    uint256 digest;
    VoteSignature signature{};
    uint64_t intent_seq{0};
    std::vector<unsigned char> Encode() const;
    static Result<SignedControlRecord> Decode(std::span<const unsigned char> bytes);
};
struct AcceptedControlView {
    ControlKey key; //!< scheduled leader's NEW_VIEW tuple
    std::vector<unsigned char> message; //!< complete authenticated message bytes
    std::vector<unsigned char> Encode() const;
    static Result<AcceptedControlView> Decode(std::span<const unsigned char> bytes);
};
struct ControlView {
    uint256 parent_hash;
    uint64_t view{0};
    bool changing{false};
    std::optional<AcceptedControlView> accepted;
};

/** Strict durable control-message capability; it does not decide PBFT validity.
 * Agreement must validate the complete evidence, truthful highest preparation,
 * scheduled leader, report quorum and selected value before calling it.
 * This class supplies immutable intents, monotone view storage, exact accepted
 * bundles and the same intent/signature barriers as ordinary vote signing.
 * A valid token does not by itself authorize a PREPARE or COMMIT.
 */
class ControlLog
{
public:
    static Result<ControlLog> Create(Journal& journal, const ControlConfig& config);
    ControlLog(ControlLog&&) noexcept = default;
    ControlLog& operator=(ControlLog&&) = delete;
    ControlLog(const ControlLog&) = delete;
    ControlLog& operator=(const ControlLog&) = delete;

    bool IsFatal() const { return m_journal->IsFatal(); }
    uint64_t JournalInstance() const { return m_journal->InstanceId(); }
    const ControlConfig& Config() const { return m_config; }
    const bls::PublicKey& SignerKey() const { return m_config.signer_key; }

    Result<PendingControlIntent> AppendIntent(const ControlIntentRecord& record);
    Result<PendingSignedControl> AppendSigned(const DurableControlIntent& intent, const VoteSignature& signature);
    Result<uint64_t> AcceptNewView(const AcceptedControlView& record);
    Result<uint64_t> Barrier() { return m_journal->Barrier(); }
    Result<DurableControlIntent> Confirm(const PendingControlIntent& pending) const { return m_journal->Confirm(pending); }
    Result<DurableSignedControl> Confirm(const PendingSignedControl& pending) const { return m_journal->Confirm(pending); }
    bool Knows(const DurableControlIntent& intent) const;
    Result<void> MaySign(const DurableControlIntent& intent) const;
    Result<void> AllowsVote(const uint256& domain, uint64_t epoch, uint64_t slot, uint64_t view) const;

    std::optional<ControlIntentRecord> IntentAt(const ControlKey& key) const;
    std::optional<PendingSignedControl> SignedAt(const ControlKey& key) const;
    std::optional<ControlView> ViewAt(uint64_t slot) const;
    std::optional<AcceptedControlView> AcceptedAt(uint64_t slot, uint64_t view) const;

private:
    ControlLog(Journal& journal, ControlConfig config) : m_journal{&journal}, m_config{std::move(config)} {}
    Result<void> CheckKey(const ControlKey& key, bool own) const;
    Result<void> CheckTransition(const ControlIntentRecord& record) const;
    Result<void> CheckAccepted(const AcceptedControlView& record) const;
    void InstallIntent(const ControlIntentRecord& record, uint64_t seq);
    void InstallAccepted(const AcceptedControlView& record);

    struct Entry {
        ControlIntentRecord intent;
        uint64_t intent_seq;
        std::optional<VoteSignature> signature;
        uint64_t signed_seq{0};
    };
    Journal* m_journal;
    const ControlConfig m_config;
    std::map<ControlKey, Entry> m_intents;
    std::map<uint64_t, ControlView> m_views;
    std::map<std::pair<uint64_t, uint64_t>, std::pair<AcceptedControlView, uint64_t>> m_accepted;
    size_t m_history_bytes{0};
};

} // namespace flowmesh2::storage
#endif
