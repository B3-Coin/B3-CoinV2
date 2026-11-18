// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3COIN_BRIDGE_PEER_LIGHT_CLIENT_H
#define B3COIN_BRIDGE_PEER_LIGHT_CLIENT_H

#include <bridge/eth_light_client.h>
#include <bridge/proof.h>
#include <crypto/sha256.h>

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

/** Read-only peer proof consumer. This does not activate a B3 bridge, provide
 * Arbitrum execution verification, or authorize a mint/withdrawal. Existing
 * consensus codecs are reused without changing their rules or profiles.
 *
 * Peers supply only untrusted bootstrap/update records. An operator-reviewed
 * weak-subjectivity checkpoint and fork policy remain necessary. A peer's
 * identity, claimed finality, or a backend's "verified" flag is never authority.
 */
namespace bridge::peer {

inline constexpr uint64_t MAX_JOURNAL_ENTRIES{65'536};
inline constexpr size_t ENTRY_PREFIX_SIZE{84};
inline constexpr size_t ENTRY_OVERHEAD{ENTRY_PREFIX_SIZE + 32};
inline constexpr size_t MAX_ENTRY_SIZE{ENTRY_OVERHEAD + MAX_BRIDGE_RECORD_SIZE};
inline constexpr std::array<unsigned char, 8> JOURNAL_MAGIC{'B', '3', 'E', 'T', 'H', 'P', '0', '1'};

inline uint256 TaggedDigest(std::string_view tag, std::span<const unsigned char> bytes)
{
    uint256 out;
    CSHA256().Write(reinterpret_cast<const unsigned char*>(tag.data()), tag.size())
        .Write(bytes.data(), bytes.size()).Finalize(out.begin());
    return out;
}

struct Policy {
    LightClientConfig config{};
    uint256 trusted_root{};
    uint64_t trusted_slot{0};
    uint64_t fork_schedule_valid_through_epoch{0};
    uint64_t max_sync_lag_slots{0};
    // Conservative local recovery limit, capped at the current mainnet
    // withdrawability-delay lower bound (256 epochs). This is not the
    // state-dependent Ethereum weak-subjectivity calculation/security proof.
    // A longer outage requires a newly reviewed checkpoint in a separate
    // store; this journal is never silently rebased.
    uint64_t max_recovery_lag_slots{0};
    // Fixed Ethereum-mainnet clock. Its local current value is supplied by
    // the native executable's clock, never an incoming proof or Python flag.
    uint64_t genesis_time{1'606'824'023};
    uint64_t seconds_per_slot{12};

    bool Valid() const
    {
        if (trusted_root.IsNull() || !trusted_slot ||
            config.genesis_validators_root.IsNull() ||
            config.forks.empty() || config.forks.size() > 32 ||
            config.forks.front().epoch != 0 ||
            config.min_participants < 342 || config.min_participants > 512 ||
            !max_sync_lag_slots || max_sync_lag_slots > SLOTS_PER_PERIOD ||
            !max_recovery_lag_slots || max_recovery_lag_slots > SLOTS_PER_PERIOD ||
            genesis_time != 1'606'824'023 || seconds_per_slot != 12 ||
            EpochAtSlot(trusted_slot) > fork_schedule_valid_through_epoch) return false;
        // Chain ID is not a signing domain. Pin Ethereum's validators genesis
        // root explicitly instead of accepting a peer's chain label.
        static constexpr std::array<unsigned char, 32> ETH_GENESIS{
            0x4b,0x36,0x3d,0xb9,0x4e,0x28,0x61,0x20,0xd7,0x6e,0xb9,0x05,0x34,0x0f,0xdd,0x4e,
            0x54,0xbf,0xe9,0xf0,0x6b,0xf3,0x3f,0xf6,0xcf,0x5a,0xd2,0x7f,0x51,0x1b,0xfe,0x95};
        if (config.genesis_validators_root != uint256{std::span<const unsigned char>{ETH_GENESIS}}) return false;
        bool electra_known{false};
        for (size_t i{0}; i < config.forks.size(); ++i) {
            const auto& fork{config.forks[i]};
            if ((i && fork.epoch <= config.forks[i - 1].epoch) ||
                fork.epoch > fork_schedule_valid_through_epoch) return false;
            if (fork.epoch == config.electra_epoch &&
                fork.version == std::array<unsigned char,4>{5,0,0,0}) electra_known = true;
        }
        return electra_known && KnownSlot(trusted_slot);
    }

    bool KnownSlot(uint64_t slot) const
    {
        const auto epoch{EpochAtSlot(slot)};
        if (epoch > fork_schedule_valid_through_epoch) return false;
        const auto version{config.ForkVersionAt(epoch)};
        // The reused execution SSZ container is Deneb/Electra/Fulu's 17-field
        // header. Unknown future containers/domains must be reviewed in code;
        // extending the policy horizon cannot add a new supported fork.
        return version == std::array<unsigned char,4>{4,0,0,0} ||
               version == std::array<unsigned char,4>{5,0,0,0} ||
               version == std::array<unsigned char,4>{6,0,0,0};
    }

    uint256 Digest() const
    {
        proof_detail::Writer writer;
        writer.Root(config.genesis_validators_root);
        writer.Root(trusted_root);
        writer.U64(trusted_slot);
        writer.U64(fork_schedule_valid_through_epoch);
        writer.U64(max_sync_lag_slots);
        writer.U64(max_recovery_lag_slots);
        writer.U64(genesis_time);
        writer.U64(seconds_per_slot);
        writer.U64(config.electra_epoch);
        writer.U32(config.min_participants);
        writer.U32(static_cast<uint32_t>(config.forks.size()));
        for (const auto& fork : config.forks) {
            writer.U64(fork.epoch);
            writer.Bytes(fork.version);
        }
        return TaggedDigest("B3/ethereum-peer-policy/1", writer.Take());
    }
};

enum class Result {
    OK, NO_CHANGE, BAD_POLICY, BAD_RECORD, UNKNOWN_FORK, FUTURE_SLOT, STALE_AUTHORITY,
    BOOTSTRAP_REQUIRED, BAD_PROOF, BAD_JOURNAL, LIMIT, STORAGE_FENCED,
};

struct Reply {
    Result result{Result::BAD_RECORD};
    LcResult proof{LcResult::BAD_STRUCTURE};
};

class LightClient;

/** Native capability, constructible only by the verified durable store. The
 * JSON diagnostic rendering of this object is not itself a capability.
 * A copied capability becomes stale when its owning store advances/fences.
 */
class FinalizedExecution final {
public:
    const uint256& PolicyDigest() const { return m_policy; }
    const uint256& JournalDigest() const { return m_journal; }
    uint64_t Sequence() const { return m_sequence; }
    uint64_t BeaconSlot() const { return m_slot; }
    const ssz::ExecutionPayloadHeader& Header() const { return m_header; }

private:
    friend class LightClient;
    FinalizedExecution(uint256 policy, uint256 journal, uint64_t sequence,
                       uint64_t slot, ssz::ExecutionPayloadHeader header)
        : m_policy{policy}, m_journal{journal}, m_sequence{sequence},
          m_slot{slot}, m_header{std::move(header)} {}
    uint256 m_policy{}, m_journal{};
    uint64_t m_sequence{0}, m_slot{0};
    ssz::ExecutionPayloadHeader m_header{};
};

/** Single-owner serialized verifier. The durable sink must synchronously append
 * the supplied exact entry, flush it, and commit it before returning true.
 * A false return (even if some bytes reached disk) permanently fences this
 * instance. Reopen replays every proof; no serialized store is trusted.
 * Hash-linked local files detect corruption/prefix mismatch, not a coordinated
 * rollback of an entire disk/backup. External checkpoint custody is separate.
 */
class LightClient final {
public:
    using DurableAppend = std::function<bool(std::span<const unsigned char>)>;

    explicit LightClient(Policy policy) : m_policy{std::move(policy)}
    {
        m_fenced = !m_policy.Valid();
        m_policy_digest = m_policy.Digest();
    }

    bool Fenced() const { return m_fenced; }
    bool Replaying() const { return m_replaying; }
    uint64_t Sequence() const { return m_sequence; }
    const uint256& JournalDigest() const { return m_digest; }
    const uint256& PolicyDigest() const { return m_policy_digest; }
    const std::optional<LightClientStore>& Store() const { return m_store; }

    bool EndReplay()
    {
        if (m_fenced || !m_replaying) return false;
        m_replaying = false;
        return true;
    }

    Reply ReplayEntry(std::span<const unsigned char> bytes)
    {
        if (m_fenced || !m_replaying) return {Result::STORAGE_FENCED};
        auto fail{[&]() { m_fenced = true; return Reply{Result::BAD_JOURNAL}; }};
        if (m_sequence >= MAX_JOURNAL_ENTRIES || bytes.size() < ENTRY_OVERHEAD ||
            bytes.size() > MAX_ENTRY_SIZE) return fail();
        proof_detail::Reader reader{bytes};
        std::array<unsigned char, 8> magic{};
        uint64_t sequence{};
        uint256 policy, previous, digest;
        uint32_t size{};
        std::vector<unsigned char> record;
        if (!reader.Bytes(magic) || magic != JOURNAL_MAGIC ||
            !reader.U64(sequence) || sequence != m_sequence + 1 ||
            !reader.Root(policy) || policy != m_policy_digest ||
            !reader.Root(previous) || previous != m_digest ||
            !reader.U32(size) || size > MAX_BRIDGE_RECORD_SIZE ||
            !reader.Vector(size, record) || !reader.Root(digest) || !reader.Empty() ||
            digest != TaggedDigest("B3/ethereum-peer-entry/1", bytes.first(bytes.size() - 32))) return fail();
        std::optional<LightClientStore> next;
        const auto checked{VerifyRecord(record, next)};
        if (checked.result != Result::OK) return fail();
        m_store = std::move(next);
        m_sequence = sequence;
        m_digest = digest;
        return {Result::OK, LcResult::OK};
    }

    Reply Submit(std::span<const unsigned char> record, uint64_t current_slot,
                 const DurableAppend& append)
    {
        if (m_fenced || m_replaying) return {Result::STORAGE_FENCED};
        if (m_sequence >= MAX_JOURNAL_ENTRIES) return {Result::LIMIT};
        // Head freshness after fast-forward is insufficient: an old committee
        // could fabricate that forward history. Require retained authority to
        // be recent BEFORE accepting the first proof or any later update.
        const uint64_t authority_slot{m_store ? m_store->finalized_header.beacon.slot : m_policy.trusted_slot};
        if (current_slot < authority_slot) return {Result::FUTURE_SLOT};
        if (current_slot - authority_slot > m_policy.max_recovery_lag_slots) return {Result::STALE_AUTHORITY};
        // Wall-clock validity is checked before any expensive BLS work. During
        // replay historical proofs are reverified without a historical clock;
        // publication still applies the current freshness check below.
        const auto decoded{DecodeBridgeRecordV1(record)};
        if (!decoded) return {Result::BAD_RECORD};
        uint64_t newest_slot{};
        if (const auto* b{std::get_if<BridgeBootstrapV1>(&decoded->payload)}) newest_slot = b->header.beacon.slot;
        else if (const auto* u{std::get_if<BridgeUpdateV1>(&decoded->payload)}) newest_slot = u->update.signature_slot;
        else return {Result::BAD_RECORD};
        if (newest_slot > current_slot) return {Result::FUTURE_SLOT};
        if (!m_policy.KnownSlot(current_slot)) return {Result::UNKNOWN_FORK};
        std::optional<LightClientStore> next;
        const auto checked{VerifyRecord(record, next)};
        if (checked.result != Result::OK) return checked;
        proof_detail::Writer writer;
        writer.Bytes(JOURNAL_MAGIC);
        writer.U64(m_sequence + 1);
        writer.Root(m_policy_digest);
        writer.Root(m_digest);
        writer.U32(static_cast<uint32_t>(record.size()));
        writer.Bytes(record);
        auto entry{writer.Take()};
        const auto digest{TaggedDigest("B3/ethereum-peer-entry/1", entry)};
        entry.insert(entry.end(), digest.begin(), digest.end());
        // A sink exception is also an uncertain write: never continue signing
        // or publishing from this instance after it.
        try {
            if (!append(entry)) { m_fenced = true; return {Result::STORAGE_FENCED}; }
        } catch (...) {
            m_fenced = true;
            return {Result::STORAGE_FENCED};
        }
        m_store = std::move(next);
        ++m_sequence;
        m_digest = digest;
        return {Result::OK, LcResult::OK};
    }

    std::optional<FinalizedExecution> Latest(uint64_t current_slot) const
    {
        if (m_fenced || m_replaying || !m_store || !m_policy.KnownSlot(current_slot)) return std::nullopt;
        const auto slot{m_store->finalized_header.beacon.slot};
        if (slot > current_slot || current_slot - slot > m_policy.max_sync_lag_slots) return std::nullopt;
        return FinalizedExecution{m_policy_digest, m_digest, m_sequence, slot,
                                  m_store->finalized_header.execution};
    }

    bool IsCurrent(const FinalizedExecution& anchor, uint64_t current_slot) const
    {
        const auto latest{Latest(current_slot)};
        return latest && anchor.PolicyDigest() == latest->PolicyDigest() &&
               anchor.JournalDigest() == latest->JournalDigest() &&
               anchor.Sequence() == latest->Sequence() && anchor.BeaconSlot() == latest->BeaconSlot() &&
               anchor.Header() == latest->Header();
    }

private:
    Reply VerifyRecord(std::span<const unsigned char> bytes,
                       std::optional<LightClientStore>& next) const
    {
        const auto decoded{DecodeBridgeRecordV1(bytes)};
        if (!decoded) return {Result::BAD_RECORD};
        const auto canonical{EncodeBridgeRecordV1(*decoded)};
        if (!canonical || !std::equal(canonical->begin(), canonical->end(), bytes.begin(), bytes.end())) return {Result::BAD_RECORD};
        if (const auto* b{std::get_if<BridgeBootstrapV1>(&decoded->payload)}) {
            if (m_store || b->header.beacon.slot != m_policy.trusted_slot) return {Result::BAD_RECORD};
            if (!m_policy.KnownSlot(b->header.beacon.slot)) return {Result::UNKNOWN_FORK};
            LightClientStore store;
            const auto proof{InitStore(store, m_policy.config, m_policy.trusted_root,
                                       b->header, b->current_committee, b->current_committee_branch)};
            if (proof != LcResult::OK) return {Result::BAD_PROOF, proof};
            if (!ValidExecution(store.finalized_header.execution)) return {Result::BAD_RECORD};
            next = std::move(store);
        } else if (const auto* u{std::get_if<BridgeUpdateV1>(&decoded->payload)}) {
            if (!m_store) return {Result::BOOTSTRAP_REQUIRED};
            if (!m_policy.KnownSlot(u->update.signature_slot) ||
                !m_policy.KnownSlot(u->update.attested.beacon.slot) ||
                !m_policy.KnownSlot(u->update.finalized.beacon.slot)) return {Result::UNKNOWN_FORK};
            next = *m_store;
            const auto proof{ProcessUpdate(*next, m_policy.config, u->update)};
            if (proof != LcResult::OK) return {Result::BAD_PROOF, proof};
            if (*next == *m_store) return {Result::NO_CHANGE, LcResult::OK};
            const auto& old{m_store->finalized_header};
            const auto& now{next->finalized_header};
            if (!ValidExecution(now.execution) ||
                (now.beacon.slot > old.beacon.slot &&
                 (now.execution.block_number <= old.execution.block_number ||
                  now.execution.timestamp <= old.execution.timestamp))) return {Result::BAD_RECORD};
        } else return {Result::BAD_RECORD};
        return {Result::OK, LcResult::OK};
    }

    static bool ValidExecution(const ssz::ExecutionPayloadHeader& header)
    {
        return header.block_number && header.timestamp && !header.block_hash.IsNull() &&
               !header.state_root.IsNull() && !header.receipts_root.IsNull();
    }

    Policy m_policy;
    uint256 m_policy_digest{}, m_digest{};
    uint64_t m_sequence{0};
    std::optional<LightClientStore> m_store{};
    bool m_replaying{true}, m_fenced{false};
};

} // namespace bridge::peer
#endif // B3COIN_BRIDGE_PEER_LIGHT_CLIENT_H
