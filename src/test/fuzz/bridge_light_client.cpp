// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! The bridge's Ethereum light client. Every node advances it from anyone's
//! bootstrap and update records, and its finalized header anchors every
//! mint. Random bytes never get past its signature check, so here a
//! synthetic beacon chain, signed by committees whose keys the target holds,
//! feeds it updates in any order: replayed, stale, from the next period,
//! conflicting, or with a signature or proof broken. On every update the
//! store either stays as it was or moves forward as the rules say: its
//! finalized header never goes back or changes at the same slot, it rotates
//! only to the next committee it proved, and it never replaces that
//! committee. The documented rules refuse what they must, and an honest
//! update that extends the store is accepted, also from a committee that
//! holds a key twice, as Ethereum's may. A bootstrap that does not prove
//! itself against the trusted root is refused.

#include <bridge/eth_light_client.h>
#include <bridge/ssz.h>
#include <crypto/bls.h>
#include <crypto/common.h>
#include <crypto/sha256.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <uint256.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace {

using bridge::EpochAtSlot;
using bridge::LcResult;
using bridge::PeriodAtSlot;
using bridge::SLOTS_PER_PERIOD;
using bridge::ssz::SYNC_COMMITTEE_SIZE;

//! Updates finalize in periods 0 to 3, and may attest and be signed just
//! past them. Committees exist up to period 5, the next committee of the
//! last period an update can attest in.
constexpr uint64_t PERIODS{4};
constexpr uint64_t COMMITTEES{PERIODS + 2};

//! A secret key's scalar, big-endian, below the group order.
using Scalar = std::array<unsigned char, 32>;

//! The order of the BLS12-381 groups.
constexpr Scalar ORDER{0x73, 0xed, 0xa7, 0x53, 0x29, 0x9d, 0x7d, 0x48, 0x33, 0x39, 0xd8, 0x08, 0x09, 0xa1, 0xd8, 0x05,
                       0x53, 0xbd, 0xa4, 0x02, 0xff, 0xfe, 0x5b, 0xfe, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01};

//! Member i of period p's committee holds a secret hashed from p and i; in
//! odd periods members 2k and 2k + 1 hold the same key. A set of members
//! signs with the sum of their secrets modulo the group order, and as with
//! real keys no other choice of keys sums to it, so a signature is valid
//! only for the members and the committee that made it.
Scalar Secret(const uint64_t period, const size_t member)
{
    unsigned char in[16];
    WriteLE64(in, period);
    WriteLE64(in + 8, period % 2 ? member & ~size_t{1} : member);
    Scalar out;
    CSHA256().Write(in, sizeof(in)).Finalize(out.data());
    out[0] &= 0x3f; // below 2^254, and so below the group order
    return out;
}

//! a + b modulo the group order, for a and b below it.
Scalar Add(const Scalar& a, const Scalar& b)
{
    Scalar sum;
    unsigned carry{0};
    for (size_t i{sum.size()}; i-- > 0;) {
        carry += unsigned{a[i]} + b[i];
        sum[i] = static_cast<unsigned char>(carry);
        carry >>= 8;
    }
    // The sum is below twice the order, which is below 2^256, so at most one
    // subtraction brings it below the order.
    if (!std::ranges::lexicographical_compare(sum, ORDER)) {
        unsigned borrow{0};
        for (size_t i{sum.size()}; i-- > 0;) {
            const unsigned difference{unsigned{sum[i]} - ORDER[i] - borrow};
            sum[i] = static_cast<unsigned char>(difference);
            borrow = difference >> 31; // it went below zero
        }
    }
    return sum;
}

struct Chain {
    //! Two fork versions, the second from within period 1, and the Electra
    //! light-client indices from within period 2.
    bridge::LightClientConfig config;
    std::vector<bridge::ssz::SyncCommittee> committees; //!< by period
    std::vector<uint256> roots;                         //!< their hash tree roots
};

const Chain* g_chain{nullptr};

void initialize_bridge_light_client()
{
    static Chain chain;
    chain.config.genesis_validators_root = uint256::ONE;
    chain.config.forks = {{0, {0x01, 0x00, 0x00, 0x00}}, {300, {0x02, 0x00, 0x00, 0x00}}};
    chain.config.electra_epoch = 600;
    chain.committees.reserve(COMMITTEES);
    chain.roots.reserve(COMMITTEES);
    for (uint64_t period{0}; period < COMMITTEES; ++period) {
        bridge::ssz::SyncCommittee committee;
        committee.pubkeys.reserve(SYNC_COMMITTEE_SIZE);
        Scalar sum{};
        for (size_t member{0}; member < SYNC_COMMITTEE_SIZE; ++member) {
            const auto secret{bls::SecretKey::FromBytes(Secret(period, member))};
            assert(secret);
            committee.pubkeys.push_back(secret->GetPublicKey().Compressed());
            sum = Add(sum, Secret(period, member));
        }
        committee.aggregate_pubkey = bls::SecretKey::FromBytes(sum)->GetPublicKey().Compressed();
        chain.roots.push_back(committee.HashTreeRoot());
        chain.committees.push_back(std::move(committee));
    }
    g_chain = &chain;
}

//! A Merkle tree holding the given leaves at their generalized indices, and
//! a fixed filler value at every other node no leaf lies under.
class Tree
{
public:
    void Set(const uint64_t gindex, const uint256& leaf) { m_leaves[gindex] = leaf; }
    uint256 Root() const { return Node(1); }
    std::vector<uint256> Branch(const uint64_t gindex) const
    {
        std::vector<uint256> branch;
        for (uint64_t node{gindex}; node > 1; node /= 2) branch.push_back(Node(node ^ 1));
        return branch;
    }

private:
    // NOLINTNEXTLINE(misc-no-recursion)
    uint256 Node(const uint64_t gindex) const
    {
        if (const auto leaf{m_leaves.find(gindex)}; leaf != m_leaves.end()) return leaf->second;
        for (const auto& [leaf, value] : m_leaves) {
            for (uint64_t above{leaf}; above >= gindex; above /= 2) {
                if (above == gindex) return bridge::ssz::HashPair(Node(2 * gindex), Node(2 * gindex + 1));
            }
        }
        unsigned char index[8];
        WriteLE64(index, gindex);
        uint256 filler;
        CSHA256().Write(index, sizeof(index)).Finalize(filler.begin());
        return filler;
    }

    std::map<uint64_t, uint256> m_leaves;
};

//! A value unique to one field of the header at `slot` on `fork`.
uint256 Tag(const uint64_t slot, const uint8_t fork, const uint8_t field)
{
    unsigned char in[10];
    WriteLE64(in, slot);
    in[8] = fork;
    in[9] = field;
    uint256 out;
    CSHA256().Write(in, sizeof(in)).Finalize(out.begin());
    return out;
}

//! The header at `slot` on `fork` (0 is the chain), its state tree holding
//! `state`'s leaves and its body proving its execution header.
bridge::LightClientHeader Header(const uint64_t slot, const uint8_t fork, const Tree& state)
{
    bridge::LightClientHeader header;
    header.beacon.slot = slot;
    header.beacon.proposer_index = slot + fork;
    header.beacon.parent_root = Tag(slot, fork, 0);
    header.beacon.state_root = state.Root();
    bridge::ssz::ExecutionPayloadHeader& execution{header.execution};
    execution.parent_hash = Tag(slot, fork, 1);
    execution.state_root = Tag(slot, fork, 2);
    execution.receipts_root = Tag(slot, fork, 3);
    execution.block_hash = Tag(slot, fork, 4);
    execution.block_number = slot;
    execution.timestamp = 1'600'000'000 + 12 * slot;
    execution.extra_data = {fork};
    Tree body;
    body.Set(bridge::EXECUTION_PAYLOAD_GINDEX, execution.HashTreeRoot());
    header.beacon.body_root = body.Root();
    header.execution_branch = body.Branch(bridge::EXECUTION_PAYLOAD_GINDEX);
    return header;
}

//! The state tree of a header at `slot`: it holds its period's committee.
Tree StateAt(const uint64_t slot)
{
    Tree state;
    state.Set(g_chain->config.CurrentCommitteeGindex(EpochAtSlot(slot)), g_chain->roots[PeriodAtSlot(slot)]);
    return state;
}

//! The header the chain (fork 0), or a conflicting fork, finalizes at `slot`.
bridge::LightClientHeader Finalized(const uint64_t slot, const uint8_t fork) { return Header(slot, fork, StateAt(slot)); }

//! A slot of periods 0 to 3, often at or next to a period's first or last
//! slot.
uint64_t Slot(FuzzedDataProvider& fdp)
{
    const uint64_t period{fdp.ConsumeIntegralInRange<uint64_t>(0, PERIODS - 1)};
    return period * SLOTS_PER_PERIOD +
           fdp.PickValueInArray<uint64_t>({0, 1, 2, 31, 32, 4096, SLOTS_PER_PERIOD - 2, SLOTS_PER_PERIOD - 1,
                                           fdp.ConsumeIntegralInRange<uint64_t>(0, SLOTS_PER_PERIOD - 1)});
}

struct Planned {
    bridge::LightClientUpdate update;
    bool tampered{false}; //!< a signature or a proof is broken
};

//! An update the chain's committees sign: its finalized header the chain's
//! (or now and then a conflicting fork's), its next committee the chain's
//! (or now and then one the chain never had), its signer the committee of
//! its signature slot's period. Now and then a signature or proof is broken.
//! Where the input runs out, the update is an honest one.
Planned Plan(FuzzedDataProvider& fdp)
{
    const bridge::LightClientConfig& config{g_chain->config};
    Planned planned;
    bridge::LightClientUpdate& update{planned.update};
    const uint64_t finalized{Slot(fdp)};
    const uint64_t attested{fdp.ConsumeBool() ? Slot(fdp) : finalized + fdp.ConsumeIntegralInRange<uint64_t>(0, 64)};
    update.signature_slot = fdp.ConsumeBool() ? Slot(fdp) : attested + fdp.ConsumeIntegralInRange<uint64_t>(1, 64);
    update.finalized = Finalized(finalized, fdp.ConsumeIntegralInRange<int>(0, 15) == 15 ? 1 : 0);

    const uint64_t attested_epoch{EpochAtSlot(attested)};
    Tree state{StateAt(attested)};
    state.Set(config.FinalityGindex(attested_epoch), update.finalized.beacon.HashTreeRoot());
    update.has_next = fdp.ConsumeBool();
    if (update.has_next) {
        const uint64_t period{PeriodAtSlot(attested)};
        const uint64_t next{fdp.ConsumeIntegralInRange<int>(0, 7) == 7 ? (period + 3) % COMMITTEES : period + 1};
        update.next_committee = g_chain->committees[next];
        state.Set(config.NextCommitteeGindex(attested_epoch), g_chain->roots[next]);
    }
    update.attested = Header(attested, fdp.ConsumeIntegral<uint8_t>(), state);
    update.finality_branch = state.Branch(config.FinalityGindex(attested_epoch));
    if (update.has_next) update.next_branch = state.Branch(config.NextCommitteeGindex(attested_epoch));

    // A few members, at least one; now and then none, or a supermajority or
    // just under one.
    std::array<unsigned char, 64>& bits{update.sync_aggregate.bits};
    switch (fdp.ConsumeIntegralInRange<int>(0, 15)) {
    case 14:
        break;
    case 15: {
        const size_t count{fdp.PickValueInArray<size_t>({341, 342, SYNC_COMMITTEE_SIZE})};
        for (size_t member{0}; member < count; ++member) bits[member / 8] |= 1 << (member % 8);
        break;
    }
    default:
        LIMITED_WHILE(update.sync_aggregate.Participation() == 0 || fdp.ConsumeBool(), 8) {
            const size_t member{fdp.ConsumeIntegralInRange<size_t>(0, SYNC_COMMITTEE_SIZE - 1)};
            bits[member / 8] |= 1 << (member % 8);
        }
        break;
    }

    uint64_t signer{PeriodAtSlot(update.signature_slot)};
    std::array<unsigned char, 4> version{config.ForkVersionAt(EpochAtSlot(std::max<uint64_t>(update.signature_slot, 1) - 1))};
    const auto corrupt{[&](std::vector<uint256>& nodes) {
        nodes[fdp.ConsumeIntegralInRange<size_t>(0, nodes.size() - 1)].data()[0] ^= 1;
        planned.tampered = true;
    }};
    switch (fdp.ConsumeIntegralInRange<int>(0, 15)) {
    case 10: // another period's committee signs
        signer = (signer + 1) % COMMITTEES;
        planned.tampered = true;
        break;
    case 11: // over another fork version
        version[0] ^= 0x80;
        planned.tampered = true;
        break;
    case 12: corrupt(update.finality_branch); break;
    case 13: corrupt(update.attested.execution_branch); break;
    case 14: corrupt(update.finalized.execution_branch); break;
    case 15:
        if (update.has_next) corrupt(update.next_branch);
        break;
    default:
        break;
    }
    Scalar secret{};
    for (size_t member{0}; member < SYNC_COMMITTEE_SIZE; ++member) {
        if (update.sync_aggregate.Participant(member)) secret = Add(secret, Secret(signer, member));
    }
    if (secret != Scalar{}) {
        const auto domain{bridge::ssz::SyncCommitteeDomain(version, config.genesis_validators_root)};
        const uint256 root{bridge::ssz::SigningRoot(update.attested.beacon.HashTreeRoot(), domain)};
        update.sync_aggregate.signature = bls::SecretKey::FromBytes(secret)->Sign(root).Compressed();
    }
    if (fdp.ConsumeIntegralInRange<int>(0, 15) == 15 && secret != Scalar{}) {
        // One member more or fewer than signed.
        const size_t member{fdp.ConsumeIntegralInRange<size_t>(0, SYNC_COMMITTEE_SIZE - 1)};
        bits[member / 8] ^= 1 << (member % 8);
        planned.tampered = true;
    }
    return planned;
}

//! One update applied to the store, `before` to `after`.
void CheckStep(const bridge::LightClientStore& before, const bridge::LightClientStore& after,
               const bridge::LightClientConfig& config, const Planned& planned, const LcResult result)
{
    const bridge::LightClientUpdate& update{planned.update};
    const uint64_t finalized{update.finalized.beacon.slot};
    const uint64_t attested{update.attested.beacon.slot};
    const uint64_t signature{update.signature_slot};
    const uint64_t store_slot{before.finalized_header.beacon.slot};
    const unsigned participants{update.sync_aggregate.Participation()};

    // A refused update changes nothing.
    if (result != LcResult::OK) {
        assert(after == before);
    } else {
        assert(!planned.tampered && participants >= config.min_participants);
        // The finalized header moves forward or stays, never to another
        // header at its slot, and stays in the store's period.
        assert(after.finalized_header == before.finalized_header || after.finalized_header == update.finalized);
        assert(after.finalized_header.beacon.slot >= store_slot);
        if (after.finalized_header.beacon.slot == store_slot) {
            assert(after.finalized_header.beacon.HashTreeRoot() == before.finalized_header.beacon.HashTreeRoot());
        }
        assert(PeriodAtSlot(after.finalized_header.beacon.slot) == after.period);
        // Committees rotate one period at a time, to the proven next
        // committee, which is never replaced.
        if (after.period == before.period) {
            assert(after.current == before.current);
            if (before.next) assert(after.next == before.next);
        } else {
            assert(after.period == before.period + 1 && before.next && after.current == *before.next);
        }
        if (after.next && after.next != before.next) assert(update.has_next && *after.next == update.next_committee);
    }

    // What the documented rules refuse.
    const bool equal_slot_conflict{finalized == store_slot && update.finalized.beacon.HashTreeRoot() !=
                                                                  before.finalized_header.beacon.HashTreeRoot()};
    const bool next_conflict{update.has_next && before.next && PeriodAtSlot(attested) == before.period &&
                             update.next_committee != *before.next};
    const bool has_committee{PeriodAtSlot(signature) == before.period ||
                             (PeriodAtSlot(signature) == before.period + 1 && before.next)};
    const bool refused{planned.tampered || participants < config.min_participants || participants == 0 ||
                       signature <= attested || attested < finalized ||
                       (update.has_next && PeriodAtSlot(attested) != PeriodAtSlot(signature)) ||
                       equal_slot_conflict || next_conflict || !has_committee};
    // An older finalized header only fills the store's missing next committee.
    const bool fills_next{update.has_next && !before.next && PeriodAtSlot(attested) == before.period &&
                          PeriodAtSlot(finalized) == before.period && PeriodAtSlot(signature) == before.period};
    if (refused || (finalized < store_slot && !fills_next)) assert(result != LcResult::OK);

    // An honest update that extends the store is accepted: signed by the
    // committee the store holds for its signature period, finalizing in the
    // store's period or the next one the store can rotate to.
    const bridge::ssz::SyncCommittee* committee{PeriodAtSlot(signature) == before.period ? &before.current
                                                : before.next                           ? &*before.next
                                                                                        : nullptr};
    const bool signed_by_store{has_committee && committee && *committee == g_chain->committees[PeriodAtSlot(signature)]};
    const bool rotates{PeriodAtSlot(finalized) == before.period ||
                       (PeriodAtSlot(finalized) == before.period + 1 && before.next)};
    if (!refused && (finalized >= store_slot || fills_next) && signed_by_store && rotates) {
        assert(result == LcResult::OK);
    }
}

} // namespace

FUZZ_TARGET(bridge_light_client, .init = initialize_bridge_light_client)
{
    FuzzedDataProvider fdp{buffer.data(), buffer.size()};
    bridge::LightClientConfig config{g_chain->config};
    config.min_participants = fdp.PickValueInArray<unsigned>({1, 2, 5, 342});

    // Bootstrap from the chain's header at some slot, now and then with a
    // proof broken, the wrong committee or another trusted root.
    const uint64_t start{Slot(fdp)};
    const uint64_t period{PeriodAtSlot(start)};
    bridge::LightClientHeader header{Finalized(start, 0)};
    uint256 trusted{header.beacon.HashTreeRoot()};
    const bridge::ssz::SyncCommittee* current{&g_chain->committees[period]};
    std::vector<uint256> branch{StateAt(start).Branch(config.CurrentCommitteeGindex(EpochAtSlot(start)))};
    bool broken{true};
    switch (fdp.ConsumeIntegralInRange<int>(0, 15)) {
    case 12: branch[fdp.ConsumeIntegralInRange<size_t>(0, branch.size() - 1)].data()[0] ^= 1; break;
    case 13: current = &g_chain->committees[period + 1]; break;
    case 14: trusted = Finalized(start, 1).beacon.HashTreeRoot(); break;
    case 15: header.execution_branch[fdp.ConsumeIntegralInRange<size_t>(0, 3)].data()[0] ^= 1; break;
    default: broken = false; break;
    }
    bridge::LightClientStore store;
    const LcResult bootstrapped{bridge::InitStore(store, config, trusted, header, *current, branch)};
    if (broken) {
        assert(bootstrapped != LcResult::OK && store == bridge::LightClientStore{});
        return;
    }
    assert(bootstrapped == LcResult::OK);
    assert(store.period == period && store.finalized_header == header && store.current == *current && !store.next);

    // Updates, new or replayed.
    std::vector<Planned> history;
    LIMITED_WHILE(history.empty() || fdp.ConsumeBool(), 6) {
        const Planned planned{!history.empty() && fdp.ConsumeBool()
                                  ? history[fdp.ConsumeIntegralInRange<size_t>(0, history.size() - 1)]
                                  : Plan(fdp)};
        const bridge::LightClientStore before{store};
        const LcResult result{bridge::ProcessUpdate(store, config, planned.update)};
        CheckStep(before, store, config, planned, result);
        history.push_back(planned);
    }
}
