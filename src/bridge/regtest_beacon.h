// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_BRIDGE_REGTEST_BEACON_H
#define B3COIN_BRIDGE_REGTEST_BEACON_H

#include <bridge/eth_light_client.h>
#include <bridge/proof.h>
#include <bridge/ssz.h>
#include <consensus/bridge_params.h>
#include <crypto/bls.h>
#include <crypto/common.h>
#include <crypto/sha256.h>
#include <span.h>
#include <uint256.h>
#include <util/strencodings.h>
#include <util/string.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * REGTEST ONLY synthetic Ethereum beacon chain laid over real execution
 * headers (a local anvil), so the unchanged consensus light client
 * (bridge/eth_light_client.h) and its B3 wiring run end to end without a
 * beacon node (regtest plan section 3.4). For consecutive, parent-linked
 * execution blocks B0..Nmax:
 *
 *   slot(N)      = slot_offset + N
 *   E(N)         = the execution payload header of block N
 *   body(N)      = sparse BeaconBlockBody, gindex 25 = HTR(E(N))
 *   state(N)     = sparse BeaconState at the configured light-client gindices:
 *                  genesis validators root, slot, finalized checkpoint
 *                  {epoch(slot(N-1)), root(N-1)} (zero at B0), current
 *                  sync committee of period(slot(N)) and the next one
 *   H(N)         = {slot(N), proposer 0, root(N-1) (zero at B0),
 *                   HTR(state(N)), HTR(body(N))}
 *   committee(p) = 512 keys; secret i = KeyGen(SHA256(tag || LE64(p) || LE32(i)))
 *
 * Sparse trees are ordinary SSZ trees whose other fields are zero, so every
 * branch is genuine. The keys are public and worthless anywhere else. The
 * domain guard refuses the Ethereum mainnet genesis validators root, any
 * fork version outside the synthetic 0xb3 namespace (which no real network
 * uses) and any electra_epoch other than 0, so no output can verify on a real
 * network.
 *
 * Tooling only: included by b3-bridge-regtest-beacon (B3_BRIDGE_TOOLS) and
 * its unit test, never by node code.
 */
namespace bridge {
namespace regtest_beacon {

//! Committee key derivation tag (regtest plan section 3.4).
inline constexpr std::string_view COMMITTEE_KEY_TAG{"B3-REGTEST-SYNC-COMMITTEE-V1"};
//! First byte of every accepted fork version.
inline constexpr unsigned char FORK_VERSION_NAMESPACE{0xb3};
//! The fork version the functional harness pins.
inline constexpr std::array<unsigned char, 4> DEFAULT_FORK_VERSION{0xb3, 0x00, 0x00, 0x01};
//! BeaconState field indices (phase0..electra layout).
inline constexpr uint64_t STATE_GENESIS_VALIDATORS_ROOT_FIELD{1};
inline constexpr uint64_t STATE_SLOT_FIELD{2};

//! The genesis validators root the functional harness pins:
//! SHA256("B3-REGTEST-SYNC-COMMITTEE-V1/genesis_validators_root").
inline uint256 DefaultGenesisValidatorsRoot()
{
    static constexpr std::string_view preimage{
        "B3-REGTEST-SYNC-COMMITTEE-V1/genesis_validators_root"};
    uint256 out;
    CSHA256().Write(UCharCast(preimage.data()), preimage.size()).Finalize(out.begin());
    return out;
}

inline unsigned GindexDepth(const uint64_t gindex)
{
    return static_cast<unsigned>(std::bit_width(gindex)) - 1;
}

/**
 * Generalized-index Merkle tree with only a few non-zero leaves. An empty
 * node at depth d stands for an all-zero subtree of height leaf_depth - d
 * (a zero chunk below leaf_depth), exactly as SSZ merkleizes a container
 * whose remaining fields are zero.
 */
class SparseTree
{
public:
    explicit SparseTree(const unsigned leaf_depth) : m_leaf_depth{leaf_depth} {}

    void Set(const uint64_t gindex, const uint256& leaf)
    {
        if (gindex < 2) throw std::logic_error("sparse tree leaf gindex must be at least 2");
        for (const auto& [other, value] : m_leaves) {
            if (other != gindex && (IsUnder(other, gindex) || IsUnder(gindex, other))) {
                throw std::logic_error("sparse tree leaves must not nest");
            }
        }
        m_leaves[gindex] = leaf;
    }

    uint256 Root() const { return Node(1); }

    //! Sibling path from the leaf up, as ssz::VerifyBranch consumes it.
    std::vector<uint256> Branch(const uint64_t gindex) const
    {
        if (gindex < 2) throw std::logic_error("sparse tree branch gindex must be at least 2");
        std::vector<uint256> branch;
        for (uint64_t node{gindex}; node > 1; node >>= 1) branch.push_back(Node(node ^ 1));
        return branch;
    }

private:
    //! True when `descendant` lies strictly below `ancestor`.
    static bool IsUnder(const uint64_t descendant, const uint64_t ancestor)
    {
        const unsigned below{GindexDepth(descendant)};
        const unsigned above{GindexDepth(ancestor)};
        return below > above && (descendant >> (below - above)) == ancestor;
    }

    uint256 Node(const uint64_t gindex) const
    {
        if (const auto leaf{m_leaves.find(gindex)}; leaf != m_leaves.end()) return leaf->second;
        const bool occupied{std::any_of(m_leaves.begin(), m_leaves.end(), [&](const auto& leaf) {
            return IsUnder(leaf.first, gindex);
        })};
        if (!occupied) {
            const unsigned depth{GindexDepth(gindex)};
            return ssz::ZeroSubtree(depth < m_leaf_depth ? m_leaf_depth - depth : 0);
        }
        return ssz::HashPair(Node(2 * gindex), Node(2 * gindex + 1));
    }

    unsigned m_leaf_depth;
    std::map<uint64_t, uint256> m_leaves{};
};

struct SyntheticCommittee {
    std::vector<bls::SecretKey> secrets{};
    ssz::SyncCommittee committee{};
    uint256 root{};
};

inline bls::SecretKey CommitteeSecret(const uint64_t period, const uint32_t index)
{
    std::array<unsigned char, 8> period_le{};
    std::array<unsigned char, 4> index_le{};
    WriteLE64(period_le.data(), period);
    WriteLE32(index_le.data(), index);
    std::array<unsigned char, CSHA256::OUTPUT_SIZE> ikm{};
    CSHA256()
        .Write(UCharCast(COMMITTEE_KEY_TAG.data()), COMMITTEE_KEY_TAG.size())
        .Write(period_le.data(), period_le.size())
        .Write(index_le.data(), index_le.size())
        .Finalize(ikm.data());
    auto secret{bls::SecretKey::FromIKM(ikm)};
    if (!secret) throw std::runtime_error("synthetic committee key derivation failed");
    return *secret;
}

//! All 512 members of committee(p); the aggregate key admits only
//! PoP-verified members.
inline SyntheticCommittee MakeCommittee(const uint64_t period)
{
    SyntheticCommittee out;
    out.secrets.reserve(ssz::SYNC_COMMITTEE_SIZE);
    out.committee.pubkeys.reserve(ssz::SYNC_COMMITTEE_SIZE);
    std::vector<bls::VerifiedPublicKey> verified;
    verified.reserve(ssz::SYNC_COMMITTEE_SIZE);
    for (uint32_t i{0}; i < ssz::SYNC_COMMITTEE_SIZE; ++i) {
        const bls::SecretKey secret{CommitteeSecret(period, i)};
        const bls::PublicKey pubkey{secret.GetPublicKey()};
        const auto member{bls::VerifiedPublicKey::FromPoP(pubkey, secret.SignPoP())};
        if (!member) throw std::runtime_error("synthetic committee proof of possession failed");
        verified.push_back(*member);
        out.committee.pubkeys.push_back(pubkey.Compressed());
        out.secrets.push_back(secret);
    }
    const auto aggregate{bls::AggregatePublicKeys(verified)};
    if (!aggregate) throw std::runtime_error("synthetic committee aggregate key is invalid");
    out.committee.aggregate_pubkey = aggregate->Compressed();
    out.root = out.committee.HashTreeRoot();
    return out;
}

struct SyntheticDomain {
    uint256 genesis_validators_root{};
    std::array<unsigned char, 4> fork_version{};
    uint64_t electra_epoch{0};
};

inline void CheckGenesisValidatorsRoot(const uint256& root)
{
    if (root.IsNull()) {
        throw std::runtime_error("genesis validators root must be non-zero");
    }
    if (root == Consensus::ETHEREUM_MAINNET_GENESIS_VALIDATORS_ROOT) {
        throw std::runtime_error(
            "refusing the Ethereum mainnet genesis validators root (regtest only)");
    }
}

inline void CheckForkVersion(std::span<const unsigned char, 4> version)
{
    if (version[0] != FORK_VERSION_NAMESPACE) {
        throw std::runtime_error("refusing fork version 0x" + HexStr(version) +
                                 " outside the synthetic 0xb3 namespace (regtest only)");
    }
}

inline void CheckDomain(const SyntheticDomain& domain)
{
    CheckGenesisValidatorsRoot(domain.genesis_validators_root);
    CheckForkVersion(domain.fork_version);
    if (domain.electra_epoch != 0) {
        throw std::runtime_error("electra_epoch must be 0 (regtest only)");
    }
}

//! Variants of one update, for negative tests. Defaults build the canonical
//! update finalizing N with attested N+1.
struct UpdateOptions {
    //! Signers: committee members 0..participants-1 (1..512).
    unsigned participants{ssz::SYNC_COMMITTEE_SIZE};
    //! Unset: carry the next committee exactly when the attested and
    //! signature slots share a period. Set: force it on or off.
    std::optional<bool> next_committee{};
    //! Unset: slot(N+1) + 1.
    std::optional<uint64_t> signature_slot{};
    //! Added to slot_offset for this output's whole chain.
    uint64_t slot_shift{0};
    std::optional<uint256> sign_genesis_validators_root{};
    std::optional<std::array<unsigned char, 4>> sign_fork_version{};
    //! Replace the finalized header's proposer (a conflicting header at the
    //! same slot, still proven under its attested state).
    std::optional<uint64_t> finalized_proposer_index{};
};

class SyntheticBeacon
{
public:
    SyntheticBeacon(const SyntheticDomain& domain, const uint64_t slot_offset,
                    std::vector<ssz::ExecutionPayloadHeader> headers)
        : m_domain{domain}, m_slot_offset{slot_offset}, m_headers{std::move(headers)}
    {
        CheckDomain(m_domain);
        if (m_headers.empty()) throw std::runtime_error("no execution headers");
        for (size_t i{0}; i < m_headers.size(); ++i) {
            const ssz::ExecutionPayloadHeader& header{m_headers[i]};
            if (!header.ValidForHashTreeRoot()) {
                throw std::runtime_error("execution header " + util::ToString(header.block_number) +
                                         " has more than 32 bytes of extra data");
            }
            if (i == 0) continue;
            const ssz::ExecutionPayloadHeader& parent{m_headers[i - 1]};
            if (parent.block_number == std::numeric_limits<uint64_t>::max() ||
                header.block_number != parent.block_number + 1) {
                throw std::runtime_error("execution headers are not consecutive at block " +
                                         util::ToString(header.block_number));
            }
            if (header.parent_hash != parent.block_hash) {
                throw std::runtime_error("execution header " + util::ToString(header.block_number) +
                                         " does not link to its parent hash");
            }
        }
        if (m_slot_offset > std::numeric_limits<uint64_t>::max() - LastBlock()) {
            throw std::runtime_error("slot_offset overflows the last slot");
        }
        if (Slot(BaseBlock()) == 0) throw std::runtime_error("the base block slot must be non-zero");
        m_body_roots.reserve(m_headers.size());
        m_execution_branches.reserve(m_headers.size());
        for (const ssz::ExecutionPayloadHeader& header : m_headers) {
            SparseTree body{GindexDepth(EXECUTION_PAYLOAD_GINDEX)};
            body.Set(EXECUTION_PAYLOAD_GINDEX, header.HashTreeRoot());
            m_body_roots.push_back(body.Root());
            m_execution_branches.push_back(body.Branch(EXECUTION_PAYLOAD_GINDEX));
        }
    }

    uint64_t BaseBlock() const { return m_headers.front().block_number; }
    uint64_t LastBlock() const { return m_headers.back().block_number; }
    uint64_t SlotOffset() const { return m_slot_offset; }

    uint64_t Slot(const uint64_t block, const uint64_t slot_shift = 0) const
    {
        const uint64_t max{std::numeric_limits<uint64_t>::max()};
        if (m_slot_offset > max - slot_shift || m_slot_offset + slot_shift > max - block) {
            throw std::runtime_error("slot overflows uint64");
        }
        return m_slot_offset + slot_shift + block;
    }

    //! The light-client configuration a store following this chain uses.
    LightClientConfig Config() const
    {
        LightClientConfig cfg;
        cfg.genesis_validators_root = m_domain.genesis_validators_root;
        cfg.forks = {ForkVersion{0, m_domain.fork_version}};
        cfg.electra_epoch = m_domain.electra_epoch;
        cfg.min_participants = Consensus::ETHEREUM_SYNC_COMMITTEE_SUPERMAJORITY;
        return cfg;
    }

    //! committee(p), derived once per instance.
    const SyntheticCommittee& Committee(const uint64_t period) const
    {
        auto it{m_committees.find(period)};
        if (it == m_committees.end()) it = m_committees.emplace(period, MakeCommittee(period)).first;
        return it->second;
    }

    //! HTR(H(B0)): the trusted checkpoint root a pin file names.
    uint256 CheckpointRoot() const { return Walk(BaseBlock(), 0, std::nullopt).back().HashTreeRoot(); }

    BridgeBootstrapV1 Bootstrap() const
    {
        const ssz::BeaconBlockHeader beacon{Walk(BaseBlock(), 0, std::nullopt).back()};
        const SparseTree state{StateTree(beacon.slot, 0, uint256{})};
        if (state.Root() != beacon.state_root) throw std::logic_error("synthetic bootstrap state mismatch");
        BridgeBootstrapV1 out;
        out.header = LightClientHeaderAt(beacon, BaseBlock());
        out.current_committee = Committee(PeriodAtSlot(beacon.slot)).committee;
        out.current_committee_branch =
            state.Branch(Config().CurrentCommitteeGindex(EpochAtSlot(beacon.slot)));
        return out;
    }

    //! The update finalizing execution block N, attested by N+1.
    LightClientUpdate Update(const uint64_t finalized_block, const UpdateOptions& options = {}) const
    {
        if (finalized_block < BaseBlock() || finalized_block >= LastBlock()) {
            throw std::runtime_error("finalized block " + util::ToString(finalized_block) +
                                     " needs itself and its attested successor within the headers");
        }
        if (options.participants == 0 || options.participants > ssz::SYNC_COMMITTEE_SIZE) {
            throw std::runtime_error("participants must be between 1 and 512");
        }
        const uint256 sign_root{
            options.sign_genesis_validators_root.value_or(m_domain.genesis_validators_root)};
        CheckGenesisValidatorsRoot(sign_root);
        if (options.sign_fork_version) CheckForkVersion(*options.sign_fork_version);

        std::optional<std::pair<uint64_t, uint64_t>> proposer;
        if (options.finalized_proposer_index) {
            proposer.emplace(finalized_block, *options.finalized_proposer_index);
        }
        const std::vector<ssz::BeaconBlockHeader> chain{
            Walk(finalized_block + 1, options.slot_shift, proposer)};
        const ssz::BeaconBlockHeader& finalized{chain[chain.size() - 2]};
        const ssz::BeaconBlockHeader& attested{chain.back()};
        const SparseTree attested_state{
            StateTree(attested.slot, EpochAtSlot(finalized.slot), finalized.HashTreeRoot())};
        if (attested_state.Root() != attested.state_root) {
            throw std::logic_error("synthetic attested state mismatch");
        }
        const LightClientConfig cfg{Config()};
        const uint64_t attested_epoch{EpochAtSlot(attested.slot)};
        const uint64_t attested_period{PeriodAtSlot(attested.slot)};

        LightClientUpdate update;
        update.attested = LightClientHeaderAt(attested, finalized_block + 1);
        update.finalized = LightClientHeaderAt(finalized, finalized_block);
        update.finality_branch = attested_state.Branch(cfg.FinalityGindex(attested_epoch));
        if (options.signature_slot) {
            update.signature_slot = *options.signature_slot;
        } else {
            if (attested.slot == std::numeric_limits<uint64_t>::max()) {
                throw std::runtime_error("signature slot overflows uint64");
            }
            update.signature_slot = attested.slot + 1;
        }
        update.has_next = options.next_committee.value_or(
            attested_period == PeriodAtSlot(update.signature_slot));
        if (update.has_next) {
            update.next_committee = Committee(attested_period + 1).committee;
            update.next_branch = attested_state.Branch(cfg.NextCommitteeGindex(attested_epoch));
        }

        const auto fork_version{options.sign_fork_version.value_or(cfg.ForkVersionAt(
            EpochAtSlot(std::max<uint64_t>(update.signature_slot, 1) - 1)))};
        const auto domain{ssz::SyncCommitteeDomain(fork_version, sign_root)};
        const uint256 signing_root{ssz::SigningRoot(attested.HashTreeRoot(), domain)};
        const SyntheticCommittee& signers{Committee(PeriodAtSlot(update.signature_slot))};
        std::vector<bls::Signature> signatures;
        signatures.reserve(options.participants);
        for (unsigned i{0}; i < options.participants; ++i) {
            signatures.push_back(signers.secrets[i].Sign(
                std::span<const unsigned char>{signing_root.begin(), signing_root.size()}));
            update.sync_aggregate.bits[i / 8] |= static_cast<unsigned char>(1U << (i % 8));
        }
        const auto aggregate{bls::AggregateSignatures(signatures)};
        if (!aggregate) throw std::runtime_error("synthetic sync aggregate is invalid");
        update.sync_aggregate.signature = aggregate->Compressed();
        return update;
    }

private:
    LightClientHeader LightClientHeaderAt(const ssz::BeaconBlockHeader& beacon,
                                          const uint64_t block) const
    {
        const size_t index{static_cast<size_t>(block - BaseBlock())};
        return LightClientHeader{beacon, m_headers[index], m_execution_branches[index]};
    }

    SparseTree StateTree(const uint64_t slot, const uint64_t checkpoint_epoch,
                         const uint256& checkpoint_root) const
    {
        const LightClientConfig cfg{Config()};
        const uint64_t epoch{EpochAtSlot(slot)};
        const uint64_t current{cfg.CurrentCommitteeGindex(epoch)};
        const uint64_t finalized_root{cfg.FinalityGindex(epoch)};
        const uint64_t fields{std::bit_floor(current)};
        const uint64_t period{PeriodAtSlot(slot)};
        SparseTree state{GindexDepth(current)};
        state.Set(fields + STATE_GENESIS_VALIDATORS_ROOT_FIELD, m_domain.genesis_validators_root);
        state.Set(fields + STATE_SLOT_FIELD, ssz::LeafUint64(slot));
        // Checkpoint {epoch, root}: the root's sibling is the epoch.
        state.Set(finalized_root - 1, ssz::LeafUint64(checkpoint_epoch));
        state.Set(finalized_root, checkpoint_root);
        state.Set(current, Committee(period).root);
        state.Set(cfg.NextCommitteeGindex(epoch), Committee(period + 1).root);
        return state;
    }

    //! H(B0)..H(through) under slot_offset + slot_shift, optionally with one
    //! block's proposer replaced (its descendants then commit to that root).
    std::vector<ssz::BeaconBlockHeader> Walk(
        const uint64_t through, const uint64_t slot_shift,
        const std::optional<std::pair<uint64_t, uint64_t>>& proposer) const
    {
        std::vector<ssz::BeaconBlockHeader> chain;
        chain.reserve(static_cast<size_t>(through - BaseBlock() + 1));
        uint256 parent{};
        uint64_t parent_slot{0};
        for (uint64_t block{BaseBlock()}; block <= through; ++block) {
            const uint64_t slot{Slot(block, slot_shift)};
            const bool base{block == BaseBlock()};
            const SparseTree state{
                StateTree(slot, base ? 0 : EpochAtSlot(parent_slot), parent)};
            ssz::BeaconBlockHeader beacon;
            beacon.slot = slot;
            beacon.proposer_index =
                proposer && proposer->first == block ? proposer->second : 0;
            beacon.parent_root = parent;
            beacon.state_root = state.Root();
            beacon.body_root = m_body_roots[static_cast<size_t>(block - BaseBlock())];
            parent = beacon.HashTreeRoot();
            parent_slot = slot;
            chain.push_back(beacon);
        }
        return chain;
    }

    SyntheticDomain m_domain;
    uint64_t m_slot_offset;
    std::vector<ssz::ExecutionPayloadHeader> m_headers;
    std::vector<uint256> m_body_roots{};
    std::vector<std::vector<uint256>> m_execution_branches{};
    mutable std::map<uint64_t, SyntheticCommittee> m_committees{};
};

} // namespace regtest_beacon
} // namespace bridge

#endif // B3COIN_BRIDGE_REGTEST_BEACON_H
