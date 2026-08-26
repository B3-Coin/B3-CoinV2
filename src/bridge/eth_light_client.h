// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3COIN_BRIDGE_ETH_LIGHT_CLIENT_H
#define B3COIN_BRIDGE_ETH_LIGHT_CLIENT_H

#include <bridge/ssz.h>
#include <crypto/bls.h>
#include <uint256.h>

#include <array>
#include <bit>
#include <cstdint>
#include <optional>
#include <vector>

/** Ethereum sync-committee light client — pure verification functions
 *  (bridge proposal stage 3; header-only, NOT reachable from consensus).
 *
 *  This is the inbound (ETH -> B3 deposit) verifier the owner ordered first
 *  on 2026-08-24: finalized-headers-only, supermajority participation by
 *  default, every header carrying an execution-payload proof so the
 *  finalized receipts_root (the anchor for bridge/mpt.h receipt proofs)
 *  is itself proven, never trusted.
 *
 *  Trust root: the bootstrap checkpoint (a Core assumevalid-class trust
 *  decision, per the bridge proposal's threat notes). Committee keys enter
 *  aggregation via TrustedFromValidatedChain because they are proven by an
 *  SSZ Merkle branch from that root; Ethereum enforces proof-of-possession
 *  at validator deposit.
 */
namespace bridge {

static constexpr uint64_t SLOTS_PER_EPOCH{32};
static constexpr uint64_t EPOCHS_PER_SYNC_COMMITTEE_PERIOD{256};
static constexpr uint64_t SLOTS_PER_PERIOD{SLOTS_PER_EPOCH * EPOCHS_PER_SYNC_COMMITTEE_PERIOD};

//! Generalized indices. Altair..Deneb state: depth-5 container; Electra grew
//! the state container, moving the light-client gindices. The execution
//! payload lives at gindex 25 of the block body from Capella onward.
static constexpr uint64_t FINALIZED_ROOT_GINDEX{105};
static constexpr uint64_t CURRENT_SYNC_COMMITTEE_GINDEX{54};
static constexpr uint64_t NEXT_SYNC_COMMITTEE_GINDEX{55};
static constexpr uint64_t FINALIZED_ROOT_GINDEX_ELECTRA{169};
static constexpr uint64_t CURRENT_SYNC_COMMITTEE_GINDEX_ELECTRA{86};
static constexpr uint64_t NEXT_SYNC_COMMITTEE_GINDEX_ELECTRA{87};
static constexpr uint64_t EXECUTION_PAYLOAD_GINDEX{25};

struct ForkVersion {
    uint64_t epoch{0};
    std::array<unsigned char, 4> version{};
};

struct LightClientConfig {
    uint256 genesis_validators_root{};
    std::vector<ForkVersion> forks{};  // ascending activation epoch
    uint64_t electra_epoch{UINT64_MAX}; // gindex switch
    //! Minimum participating committee members. Default: supermajority
    //! (> 2/3 of 512), per the bridge proposal's high-threshold threat note.
    unsigned min_participants{342};

    std::array<unsigned char, 4> ForkVersionAt(uint64_t epoch) const
    {
        std::array<unsigned char, 4> v{};
        for (const auto& f : forks) {
            if (f.epoch <= epoch) v = f.version;
        }
        return v;
    }
    uint64_t FinalityGindex(uint64_t epoch) const
    {
        return epoch >= electra_epoch ? FINALIZED_ROOT_GINDEX_ELECTRA : FINALIZED_ROOT_GINDEX;
    }
    uint64_t CurrentCommitteeGindex(uint64_t epoch) const
    {
        return epoch >= electra_epoch ? CURRENT_SYNC_COMMITTEE_GINDEX_ELECTRA : CURRENT_SYNC_COMMITTEE_GINDEX;
    }
    uint64_t NextCommitteeGindex(uint64_t epoch) const
    {
        return epoch >= electra_epoch ? NEXT_SYNC_COMMITTEE_GINDEX_ELECTRA : NEXT_SYNC_COMMITTEE_GINDEX;
    }
};

inline uint64_t EpochAtSlot(uint64_t slot) { return slot / SLOTS_PER_EPOCH; }
inline uint64_t PeriodAtSlot(uint64_t slot) { return slot / SLOTS_PER_PERIOD; }

//! Beacon header + proven execution payload header (Capella+ LightClientHeader).
struct LightClientHeader {
    ssz::BeaconBlockHeader beacon{};
    ssz::ExecutionPayloadHeader execution{};
    std::vector<uint256> execution_branch{};

    bool VerifyExecution() const
    {
        return ssz::VerifyBranch(execution.HashTreeRoot(), execution_branch,
                                 EXECUTION_PAYLOAD_GINDEX, beacon.body_root);
    }
};

struct SyncAggregate {
    std::array<unsigned char, 64> bits{};  // Bitvector[512], LSB-first per byte
    std::array<unsigned char, 96> signature{};

    unsigned Participation() const
    {
        unsigned n{0};
        for (unsigned char b : bits) n += std::popcount(b);
        return n;
    }
    bool Participant(size_t i) const { return (bits[i / 8] >> (i % 8)) & 1; }
};

struct LightClientUpdate {
    LightClientHeader attested{};
    LightClientHeader finalized{};
    std::vector<uint256> finality_branch{};
    bool has_next{false};
    ssz::SyncCommittee next_committee{};
    std::vector<uint256> next_branch{};
    SyncAggregate sync_aggregate{};
    uint64_t signature_slot{0};
};

struct LightClientStore {
    LightClientHeader finalized_header{};
    uint64_t period{0};
    ssz::SyncCommittee current{};
    std::optional<ssz::SyncCommittee> next{};
};

enum class LcResult {
    OK,
    BAD_STRUCTURE,     // sizes/counts malformed
    BOOTSTRAP_PROOF,   // bootstrap committee or header proof failed
    MONOTONICITY,      // slots not ordered (finalized <= attested < signature)
    FINALITY_PROOF,    // finalized header not proven under attested state
    EXECUTION_PROOF,   // execution payload header not proven under body root
    NEXT_PROOF,        // next-committee branch failed
    PERIOD,            // signature period unusable from this store
    PARTICIPATION,     // below the configured minimum
    SIGNATURE,         // BLS aggregate did not verify
};

//! Initialize a store from a trusted checkpoint root + bootstrap data.
inline LcResult InitStore(LightClientStore& store, const LightClientConfig& cfg,
                          const uint256& trusted_root, const LightClientHeader& header,
                          const ssz::SyncCommittee& current,
                          std::span<const uint256> committee_branch)
{
    if (current.pubkeys.size() != ssz::SYNC_COMMITTEE_SIZE) return LcResult::BAD_STRUCTURE;
    if (header.beacon.HashTreeRoot() != trusted_root) return LcResult::BOOTSTRAP_PROOF;
    if (!header.VerifyExecution()) return LcResult::EXECUTION_PROOF;
    const uint64_t epoch{EpochAtSlot(header.beacon.slot)};
    if (!ssz::VerifyBranch(current.HashTreeRoot(), committee_branch,
                           cfg.CurrentCommitteeGindex(epoch), header.beacon.state_root)) {
        return LcResult::BOOTSTRAP_PROOF;
    }
    store.finalized_header = header;
    store.period = PeriodAtSlot(header.beacon.slot);
    store.current = current;
    store.next.reset();
    return LcResult::OK;
}

//! Verify an update against the store WITHOUT mutating it.
inline LcResult VerifyUpdate(const LightClientStore& store, const LightClientConfig& cfg,
                             const LightClientUpdate& u)
{
    // Structure.
    if (u.has_next && u.next_committee.pubkeys.size() != ssz::SYNC_COMMITTEE_SIZE) {
        return LcResult::BAD_STRUCTURE;
    }
    // Slot ordering; finalized-only client: a finality proof is mandatory.
    if (!(u.signature_slot > u.attested.beacon.slot)) return LcResult::MONOTONICITY;
    if (!(u.attested.beacon.slot >= u.finalized.beacon.slot)) return LcResult::MONOTONICITY;
    if (u.finalized.beacon.slot < store.finalized_header.beacon.slot) return LcResult::MONOTONICITY;

    const uint64_t attested_epoch{EpochAtSlot(u.attested.beacon.slot)};

    // Finalized header proven under the attested state.
    if (!ssz::VerifyBranch(u.finalized.beacon.HashTreeRoot(), u.finality_branch,
                           cfg.FinalityGindex(attested_epoch), u.attested.beacon.state_root)) {
        return LcResult::FINALITY_PROOF;
    }
    // Execution payload headers proven for both headers.
    if (!u.attested.VerifyExecution() || !u.finalized.VerifyExecution()) {
        return LcResult::EXECUTION_PROOF;
    }
    // Next committee, when present, proven under the attested state and only
    // for the store's own period (one-period-lookahead discipline).
    const uint64_t sig_period{PeriodAtSlot(u.signature_slot)};
    if (u.has_next) {
        if (PeriodAtSlot(u.attested.beacon.slot) != sig_period) return LcResult::PERIOD;
        if (!ssz::VerifyBranch(u.next_committee.HashTreeRoot(), u.next_branch,
                               cfg.NextCommitteeGindex(attested_epoch),
                               u.attested.beacon.state_root)) {
            return LcResult::NEXT_PROOF;
        }
    }
    // Committee selection.
    const ssz::SyncCommittee* committee{nullptr};
    if (sig_period == store.period) {
        committee = &store.current;
    } else if (sig_period == store.period + 1 && store.next) {
        committee = &*store.next;
    } else {
        return LcResult::PERIOD;
    }
    // Participation threshold.
    const unsigned participants{u.sync_aggregate.Participation()};
    if (participants < cfg.min_participants || participants == 0) return LcResult::PARTICIPATION;

    // BLS: fast-aggregate-verify the participants over the attested root.
    const auto fork_epoch{EpochAtSlot(std::max<uint64_t>(u.signature_slot, 1) - 1)};
    const auto version{cfg.ForkVersionAt(fork_epoch)};
    const auto domain{ssz::SyncCommitteeDomain(version, cfg.genesis_validators_root)};
    const uint256 signing_root{ssz::SigningRoot(u.attested.beacon.HashTreeRoot(), domain)};

    std::vector<bls::VerifiedPublicKey> keys;
    keys.reserve(participants);
    for (size_t i = 0; i < ssz::SYNC_COMMITTEE_SIZE; ++i) {
        if (!u.sync_aggregate.Participant(i)) continue;
        const auto pk{bls::PublicKey::Decode(committee->pubkeys[i])};
        if (!pk) return LcResult::BAD_STRUCTURE;
        keys.push_back(bls::VerifiedPublicKey::TrustedFromValidatedChain(*pk));
    }
    const auto sig{bls::Signature::Decode(u.sync_aggregate.signature)};
    if (!sig) return LcResult::BAD_STRUCTURE;
    if (!bls::FastAggregateVerify(keys, std::span<const unsigned char>{signing_root.begin(), 32}, *sig)) {
        return LcResult::SIGNATURE;
    }
    return LcResult::OK;
}

//! Verify and apply. The store advances by at most one committee period.
inline LcResult ProcessUpdate(LightClientStore& store, const LightClientConfig& cfg,
                              const LightClientUpdate& u)
{
    const LcResult r{VerifyUpdate(store, cfg, u)};
    if (r != LcResult::OK) return r;

    const uint64_t new_period{PeriodAtSlot(u.finalized.beacon.slot)};
    if (new_period == store.period + 1) {
        if (!store.next) return LcResult::PERIOD; // cannot rotate blind
        store.current = *store.next;
        store.next.reset();
        store.period = new_period;
    } else if (new_period != store.period) {
        return LcResult::PERIOD; // no multi-period jumps
    }
    if (u.finalized.beacon.slot >= store.finalized_header.beacon.slot) {
        store.finalized_header = u.finalized;
    }
    if (u.has_next && !store.next && PeriodAtSlot(u.attested.beacon.slot) == store.period) {
        store.next = u.next_committee;
    }
    return LcResult::OK;
}

} // namespace bridge

#endif // B3COIN_BRIDGE_ETH_LIGHT_CLIENT_H
