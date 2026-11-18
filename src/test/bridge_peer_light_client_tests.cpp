// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Native peer-proof authority tests use real captured Ethereum BLS/SSZ proofs.
// Peer identity and backend status are never substituted for verification.

#include <bridge/peer_light_client.h>
#include <bridge/lc_json.h>
#include <bridge/ssz.h>
#include <test/data/eth_lc_fixture.h>
#include <uint256.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

using namespace bridge;
namespace fx = eth_lc_fixture;

BOOST_AUTO_TEST_SUITE(bridge_peer_light_client_tests)

namespace {

std::vector<unsigned char> Hex(const std::string& h)
{
    auto v{TryParseHex<unsigned char>(h)};
    BOOST_REQUIRE_MESSAGE(v.has_value(), "bad hex in fixture");
    return *v;
}

uint256 Hex32(const std::string& h)
{
    const auto v{Hex(h)};
    BOOST_REQUIRE_EQUAL(v.size(), 32U);
    return uint256{std::span<const unsigned char>{v}};
}

template <size_t N>
std::array<unsigned char, N> HexArr(const std::string& h)
{
    const auto v{Hex(h)};
    BOOST_REQUIRE_EQUAL(v.size(), N);
    std::array<unsigned char, N> out;
    std::copy(v.begin(), v.end(), out.begin());
    return out;
}

std::vector<uint256> Branch(const std::vector<std::string>& b)
{
    std::vector<uint256> out;
    out.reserve(b.size());
    for (const auto& r : b) out.push_back(Hex32(r));
    return out;
}

ssz::BeaconBlockHeader Beacon(const fx::BeaconHeaderHex& h)
{
    return {h.slot, h.proposer_index, Hex32(h.parent_root), Hex32(h.state_root), Hex32(h.body_root)};
}

ssz::ExecutionPayloadHeader Exec(const fx::ExecHeaderHex& e)
{
    ssz::ExecutionPayloadHeader out;
    out.parent_hash = Hex32(e.parent_hash);
    out.fee_recipient = HexArr<20>(e.fee_recipient);
    out.state_root = Hex32(e.state_root);
    out.receipts_root = Hex32(e.receipts_root);
    out.logs_bloom = HexArr<256>(e.logs_bloom);
    out.prev_randao = Hex32(e.prev_randao);
    out.block_number = e.block_number;
    out.gas_limit = e.gas_limit;
    out.gas_used = e.gas_used;
    out.timestamp = e.timestamp;
    out.extra_data = Hex(e.extra_data);
    out.base_fee_per_gas = Hex32(e.base_fee_le);
    out.block_hash = Hex32(e.block_hash);
    out.transactions_root = Hex32(e.transactions_root);
    out.withdrawals_root = Hex32(e.withdrawals_root);
    out.blob_gas_used = e.blob_gas_used;
    out.excess_blob_gas = e.excess_blob_gas;
    return out;
}

ssz::SyncCommittee Committee(const std::string& pubkeys_hex, const std::string& agg_hex)
{
    const auto raw{Hex(pubkeys_hex)};
    BOOST_REQUIRE_EQUAL(raw.size(), ssz::SYNC_COMMITTEE_SIZE * 48);
    ssz::SyncCommittee c;
    c.pubkeys.resize(ssz::SYNC_COMMITTEE_SIZE);
    for (size_t i = 0; i < ssz::SYNC_COMMITTEE_SIZE; ++i) {
        std::copy(raw.begin() + i * 48, raw.begin() + (i + 1) * 48, c.pubkeys[i].begin());
    }
    c.aggregate_pubkey = HexArr<48>(agg_hex);
    return c;
}

LightClientConfig Config()
{
    LightClientConfig cfg;
    cfg.genesis_validators_root = Hex32(fx::GENESIS_VALIDATORS_ROOT);
    for (const auto& f : fx::FORKS) {
        ForkVersion fv;
        fv.epoch = f.epoch;
        fv.version = HexArr<4>(f.version);
        cfg.forks.push_back(fv);
        if (fv.version[0] == 0x05) cfg.electra_epoch = fv.epoch; // Electra moved the gindices
    }
    BOOST_REQUIRE(cfg.electra_epoch != UINT64_MAX);
    return cfg;
}

LightClientUpdate Update1()
{
    LightClientUpdate u;
    u.attested = {Beacon(fx::U1_ATTESTED_BEACON), Exec(fx::U1_ATTESTED_EXEC), Branch(fx::U1_ATTESTED_EXEC_BRANCH)};
    u.finalized = {Beacon(fx::U1_FINALIZED_BEACON), Exec(fx::U1_FINALIZED_EXEC), Branch(fx::U1_FINALIZED_EXEC_BRANCH)};
    u.finality_branch = Branch(fx::U1_FINALITY_BRANCH);
    u.has_next = true;
    u.next_committee = Committee(fx::U1_NEXT_PUBKEYS, fx::U1_NEXT_AGG);
    u.next_branch = Branch(fx::U1_NEXT_BRANCH);
    u.sync_aggregate.bits = HexArr<64>(fx::U1_SYNC_BITS);
    u.sync_aggregate.signature = HexArr<96>(fx::U1_SYNC_SIG);
    u.signature_slot = fx::U1_SIGNATURE_SLOT;
    return u;
}

LightClientUpdate Update2()
{
    LightClientUpdate u;
    u.attested = {Beacon(fx::U2_ATTESTED_BEACON), Exec(fx::U2_ATTESTED_EXEC), Branch(fx::U2_ATTESTED_EXEC_BRANCH)};
    u.finalized = {Beacon(fx::U2_FINALIZED_BEACON), Exec(fx::U2_FINALIZED_EXEC), Branch(fx::U2_FINALIZED_EXEC_BRANCH)};
    u.finality_branch = Branch(fx::U2_FINALITY_BRANCH);
    u.has_next = true;
    u.next_committee = Committee(fx::U2_NEXT_PUBKEYS, fx::U2_NEXT_AGG);
    u.next_branch = Branch(fx::U2_NEXT_BRANCH);
    u.sync_aggregate.bits = HexArr<64>(fx::U2_SYNC_BITS);
    u.sync_aggregate.signature = HexArr<96>(fx::U2_SYNC_SIG);
    u.signature_slot = fx::U2_SIGNATURE_SLOT;
    return u;
}

LightClientStore Bootstrapped(const LightClientConfig& cfg)
{
    LightClientStore store;
    // The bootstrap endpoint serves the header without an execution proof in
    // some fork versions; our InitStore demands one, so bootstrap from the
    // identical header carried by update 1's finalized header (same root).
    LightClientHeader header{Beacon(fx::U1_FINALIZED_BEACON), Exec(fx::U1_FINALIZED_EXEC), Branch(fx::U1_FINALIZED_EXEC_BRANCH)};
    const auto res{InitStore(store, cfg, Hex32(fx::BOOTSTRAP_ROOT), header,
                             Committee(fx::BOOTSTRAP_COMMITTEE_PUBKEYS, fx::BOOTSTRAP_COMMITTEE_AGG),
                             Branch(fx::BOOTSTRAP_COMMITTEE_BRANCH))};
    BOOST_REQUIRE(res == LcResult::OK);
    return store;
}

} // namespace

namespace {
peer::Policy Policy()
{
    peer::Policy policy;
    policy.config = Config();
    policy.trusted_root = Hex32(fx::BOOTSTRAP_ROOT);
    policy.trusted_slot = fx::U1_FINALIZED_BEACON.slot;
    policy.fork_schedule_valid_through_epoch = 479'999;
    policy.max_sync_lag_slots = SLOTS_PER_PERIOD;
    policy.max_recovery_lag_slots = SLOTS_PER_PERIOD;
    BOOST_REQUIRE(policy.Valid());
    return policy;
}

std::vector<unsigned char> Bootstrap()
{
    const BridgeBootstrapV1 bootstrap{
        {Beacon(fx::U1_FINALIZED_BEACON), Exec(fx::U1_FINALIZED_EXEC), Branch(fx::U1_FINALIZED_EXEC_BRANCH)},
        Committee(fx::BOOTSTRAP_COMMITTEE_PUBKEYS, fx::BOOTSTRAP_COMMITTEE_AGG),
        Branch(fx::BOOTSTRAP_COMMITTEE_BRANCH)};
    auto record{EncodeBridgeRecordV1({BridgeRecordKindV1::BOOTSTRAP, bootstrap})};
    BOOST_REQUIRE(record);
    return *record;
}

std::vector<unsigned char> Update(const LightClientUpdate& update)
{
    auto record{EncodeBridgeRecordV1({BridgeRecordKindV1::UPDATE, BridgeUpdateV1{update}})};
    BOOST_REQUIRE(record);
    return *record;
}

struct Journal {
    std::vector<std::vector<unsigned char>> entries;
    bool Append(std::span<const unsigned char> bytes)
    {
        entries.emplace_back(bytes.begin(), bytes.end());
        return true;
    }
    auto Sink() { return [this](auto bytes) { return Append(bytes); }; }
};

void Bootstrap(peer::LightClient& client, Journal& journal)
{
    BOOST_REQUIRE(client.EndReplay());
    BOOST_REQUIRE(client.Submit(Bootstrap(), fx::U1_SIGNATURE_SLOT, journal.Sink()).result == peer::Result::OK);
}
} // namespace

BOOST_AUTO_TEST_CASE(real_proofs_publish_only_after_durable_append_and_full_replay)
{
    const auto policy{Policy()};
    peer::LightClient client{policy};
    Journal journal;
    BOOST_CHECK(!client.Latest(fx::U1_SIGNATURE_SLOT));
    BOOST_REQUIRE(client.EndReplay());
    bool called{false};
    const auto boot{client.Submit(Bootstrap(), fx::U1_SIGNATURE_SLOT, [&](auto bytes) {
        called = true;
        BOOST_CHECK_EQUAL(client.Sequence(), 0U);
        BOOST_CHECK(!client.Store());
        BOOST_CHECK(!client.Latest(fx::U1_SIGNATURE_SLOT));
        return journal.Append(bytes);
    })};
    BOOST_CHECK(called);
    BOOST_REQUIRE(boot.result == peer::Result::OK);
    BOOST_REQUIRE_EQUAL(client.Sequence(), 1U);
    const auto first{client.Latest(policy.trusted_slot)};
    BOOST_REQUIRE(first);
    BOOST_CHECK(first->Header() == Bootstrapped(policy.config).finalized_header.execution);
    BOOST_REQUIRE(client.Submit(Update(Update1()), fx::U1_SIGNATURE_SLOT, journal.Sink()).result == peer::Result::OK);
    const auto final{client.Latest(fx::U1_SIGNATURE_SLOT)};
    BOOST_REQUIRE(final);
    BOOST_CHECK(final->Header() == Update1().finalized.execution);
    BOOST_CHECK(!client.IsCurrent(*first, fx::U1_SIGNATURE_SLOT));
    BOOST_CHECK(client.IsCurrent(*final, fx::U1_SIGNATURE_SLOT));

    peer::LightClient reopened{policy};
    for (const auto& entry : journal.entries) {
        BOOST_REQUIRE(reopened.ReplayEntry(entry).result == peer::Result::OK);
        BOOST_CHECK(!reopened.Latest(fx::U1_SIGNATURE_SLOT));
    }
    BOOST_REQUIRE(reopened.EndReplay());
    BOOST_REQUIRE(reopened.Latest(fx::U1_SIGNATURE_SLOT));
    BOOST_CHECK(reopened.Store() == client.Store());
    BOOST_CHECK(reopened.JournalDigest() == client.JournalDigest());
    BOOST_CHECK(reopened.IsCurrent(*final, fx::U1_SIGNATURE_SLOT));
}

BOOST_AUTO_TEST_CASE(storage_failure_fences_previous_and_new_authority)
{
    peer::LightClient client{Policy()};
    Journal journal;
    Bootstrap(client, journal);
    const auto before{client.Latest(Policy().trusted_slot)};
    BOOST_REQUIRE(before);
    const auto reply{client.Submit(Update(Update1()), fx::U1_SIGNATURE_SLOT, [&](auto bytes) {
        journal.Append(bytes); // Model bytes written but an fsync error.
        BOOST_CHECK(client.IsCurrent(*before, Policy().trusted_slot));
        return false;
    })};
    BOOST_CHECK(reply.result == peer::Result::STORAGE_FENCED);
    BOOST_CHECK(client.Fenced());
    BOOST_CHECK_EQUAL(client.Sequence(), 1U);
    BOOST_CHECK(!client.Latest(Policy().trusted_slot));
    BOOST_CHECK(!client.IsCurrent(*before, Policy().trusted_slot));
    BOOST_CHECK(client.Submit(Update(Update1()), fx::U1_SIGNATURE_SLOT, journal.Sink()).result == peer::Result::STORAGE_FENCED);

    // A subsequent process may recover a completely persisted uncertain write,
    // but must independently verify it and cross EndReplay before publication.
    peer::LightClient reopened{Policy()};
    for (const auto& entry : journal.entries) BOOST_REQUIRE(reopened.ReplayEntry(entry).result == peer::Result::OK);
    BOOST_REQUIRE(reopened.EndReplay());
    BOOST_CHECK_EQUAL(reopened.Sequence(), 2U);
    BOOST_REQUIRE(reopened.Latest(Policy().trusted_slot));
}

BOOST_AUTO_TEST_CASE(sink_exception_fences_instead_of_publishing)
{
    peer::LightClient client{Policy()};
    BOOST_REQUIRE(client.EndReplay());
    const auto reply{client.Submit(Bootstrap(), fx::U1_SIGNATURE_SLOT, [](auto) -> bool { throw std::runtime_error("disk failure"); })};
    BOOST_CHECK(reply.result == peer::Result::STORAGE_FENCED);
    BOOST_CHECK(client.Fenced());
    BOOST_CHECK_EQUAL(client.Sequence(), 0U);
    BOOST_CHECK(!client.Store());
}

BOOST_AUTO_TEST_CASE(bootstrap_must_match_exact_pinned_root_and_slot)
{
    for (int mutation{0}; mutation < 3; ++mutation) {
        auto policy{Policy()};
        if (mutation == 0) policy.trusted_root = uint256::ONE;
        if (mutation == 1) ++policy.trusted_slot;
        peer::LightClient client{policy};
        BOOST_REQUIRE(client.EndReplay());
        auto record{Bootstrap()};
        if (mutation == 2) record.back() ^= 1;
        Journal journal;
        const auto reply{client.Submit(record, fx::U1_SIGNATURE_SLOT, journal.Sink())};
        BOOST_CHECK(reply.result == peer::Result::BAD_PROOF || reply.result == peer::Result::BAD_RECORD);
        BOOST_CHECK(journal.entries.empty());
        BOOST_CHECK(!client.Store());
        BOOST_CHECK(!client.Fenced()); // A bad peer cannot corrupt a good journal.
    }
}

BOOST_AUTO_TEST_CASE(peer_signatures_branches_domains_and_participation_are_native_checked)
{
    for (int mutation{0}; mutation < 6; ++mutation) {
        peer::LightClient client{Policy()};
        Journal journal;
        Bootstrap(client, journal);
        auto update{Update1()};
        if (mutation == 0) update.sync_aggregate.signature[10] ^= 1;
        if (mutation == 1) update.sync_aggregate.bits[7] ^= 0x10;
        if (mutation == 2) update.finalized.execution.receipts_root = uint256::ONE;
        if (mutation == 3) update.finalized.beacon.state_root = uint256::ONE;
        if (mutation == 4) update.next_committee.pubkeys[0][5] ^= 1;
        if (mutation == 5) update.sync_aggregate.bits.fill(0);
        const auto before{client.JournalDigest()};
        const auto reply{client.Submit(Update(update), fx::U1_SIGNATURE_SLOT, journal.Sink())};
        BOOST_CHECK(reply.result == peer::Result::BAD_PROOF);
        BOOST_CHECK_EQUAL(journal.entries.size(), 1U);
        BOOST_CHECK(client.JournalDigest() == before);
        BOOST_CHECK(!client.Fenced());
    }
}

BOOST_AUTO_TEST_CASE(future_committee_update_and_update_before_bootstrap_are_not_authority)
{
    peer::LightClient client{Policy()};
    Journal journal;
    BOOST_REQUIRE(client.EndReplay());
    BOOST_CHECK(client.Submit(Update(Update1()), fx::U1_SIGNATURE_SLOT, journal.Sink()).result == peer::Result::BOOTSTRAP_REQUIRED);
    BOOST_REQUIRE(client.Submit(Bootstrap(), fx::U1_SIGNATURE_SLOT, journal.Sink()).result == peer::Result::OK);
    const auto reply{client.Submit(Update(Update2()), fx::U1_SIGNATURE_SLOT, journal.Sink())};
    BOOST_CHECK(reply.result == peer::Result::FUTURE_SLOT);
    BOOST_CHECK(VerifyUpdate(Bootstrapped(Policy().config), Policy().config, Update2()) == LcResult::PERIOD);
    BOOST_CHECK_EQUAL(journal.entries.size(), 1U);
}

BOOST_AUTO_TEST_CASE(wrong_fork_signing_domain_is_rejected_by_real_bls)
{
    auto policy{Policy()};
    // Electra and Fulu use the same header/gindex shape, so Merkle proofs
    // still pass. Their BLS signing domains must not be interchangeable.
    policy.config.forks.back().version = {5,0,0,0};
    BOOST_REQUIRE(policy.Valid());
    peer::LightClient client{policy};
    Journal journal;
    Bootstrap(client, journal);
    const auto reply{client.Submit(Update(Update1()), fx::U1_SIGNATURE_SLOT, journal.Sink())};
    BOOST_CHECK(reply.result == peer::Result::BAD_PROOF);
    BOOST_CHECK(reply.proof == LcResult::SIGNATURE);
    BOOST_CHECK_EQUAL(journal.entries.size(), 1U);
}

BOOST_AUTO_TEST_CASE(future_and_stale_local_clock_cannot_publish_a_fresh_anchor)
{
    peer::LightClient client{Policy()};
    Journal journal;
    BOOST_REQUIRE(client.EndReplay());
    BOOST_CHECK(client.Submit(Bootstrap(), Policy().trusted_slot - 1, journal.Sink()).result == peer::Result::FUTURE_SLOT);
    BOOST_REQUIRE(client.Submit(Bootstrap(), fx::U1_SIGNATURE_SLOT, journal.Sink()).result == peer::Result::OK);
    BOOST_CHECK(!client.Latest(Policy().trusted_slot - 1));
    BOOST_REQUIRE(client.Latest(Policy().trusted_slot));
    BOOST_CHECK(!client.Latest(Policy().trusted_slot + Policy().max_sync_lag_slots + 1));
    BOOST_CHECK(client.Submit(Update(Update1()), fx::U1_SIGNATURE_SLOT - 1, journal.Sink()).result == peer::Result::FUTURE_SLOT);
}

BOOST_AUTO_TEST_CASE(old_checkpoint_cannot_bootstrap_then_fast_forward_to_fresh_authority)
{
    const auto policy{Policy()};
    peer::LightClient client{policy};
    Journal journal;
    BOOST_REQUIRE(client.EndReplay());
    const uint64_t late{policy.trusted_slot + policy.max_recovery_lag_slots + 1};
    BOOST_CHECK(client.Submit(Bootstrap(), late, journal.Sink()).result == peer::Result::STALE_AUTHORITY);
    // Even a hypothetical freshly signed update is refused before BLS work;
    // an unsigned peer cannot bootstrap an ancient trusted committee first.
    auto forged{Update1()};
    forged.signature_slot = late;
    BOOST_CHECK(client.Submit(Update(forged), late, journal.Sink()).result == peer::Result::STALE_AUTHORITY);
    BOOST_CHECK(journal.entries.empty());
    BOOST_CHECK(!client.Store());
    BOOST_CHECK(!client.Latest(late));
}

BOOST_AUTO_TEST_CASE(stale_replayed_store_cannot_recover_by_fast_forwarding_old_committee)
{
    const auto policy{Policy()};
    peer::LightClient original{policy};
    Journal journal;
    Bootstrap(original, journal);
    peer::LightClient reopened{policy};
    BOOST_REQUIRE(reopened.ReplayEntry(journal.entries[0]).result == peer::Result::OK);
    BOOST_REQUIRE(reopened.EndReplay());
    const uint64_t late{policy.trusted_slot + policy.max_recovery_lag_slots + 1};
    const auto digest{reopened.JournalDigest()};
    BOOST_CHECK(reopened.Submit(Update(Update1()), late, journal.Sink()).result == peer::Result::STALE_AUTHORITY);
    BOOST_CHECK_EQUAL(journal.entries.size(), 1U);
    BOOST_CHECK(reopened.JournalDigest() == digest);
    BOOST_CHECK(!reopened.Latest(late));
    // A separate recent recovery is valid. The sparse real fixture's next
    // update is cryptographically valid but its 8258-slot outage exceeds the
    // conservative operational recovery bound: do not relax it for fixtures.
    peer::LightClient recent{policy};
    BOOST_REQUIRE(recent.ReplayEntry(journal.entries[0]).result == peer::Result::OK);
    BOOST_REQUIRE(recent.EndReplay());
    BOOST_REQUIRE(recent.Submit(Update(Update1()), fx::U1_SIGNATURE_SLOT, journal.Sink()).result == peer::Result::OK);
    auto verified{*recent.Store()};
    BOOST_REQUIRE(ProcessUpdate(verified, policy.config, Update2()) == LcResult::OK);
    BOOST_CHECK(recent.Submit(Update(Update2()), fx::U2_SIGNATURE_SLOT, journal.Sink()).result == peer::Result::STALE_AUTHORITY);
    BOOST_CHECK(!recent.Latest(fx::U2_SIGNATURE_SLOT));
}

BOOST_AUTO_TEST_CASE(policy_horizon_and_unknown_future_header_containers_fail_closed)
{
    auto policy{Policy()};
    policy.fork_schedule_valid_through_epoch = EpochAtSlot(policy.trusted_slot);
    peer::LightClient client{policy};
    Journal journal;
    BOOST_REQUIRE(client.EndReplay());
    BOOST_REQUIRE(client.Submit(Bootstrap(), policy.trusted_slot, journal.Sink()).result == peer::Result::OK);
    BOOST_CHECK(client.Submit(Update(Update1()), fx::U1_SIGNATURE_SLOT, journal.Sink()).result == peer::Result::UNKNOWN_FORK);
    BOOST_CHECK(!client.Latest(fx::U1_SIGNATURE_SLOT));

    policy = Policy();
    const uint64_t next_epoch{EpochAtSlot(fx::U1_SIGNATURE_SLOT) + 1};
    policy.config.forks.push_back({next_epoch, {7,0,0,0}});
    BOOST_REQUIRE(policy.Valid());
    peer::LightClient future{policy};
    Journal second;
    Bootstrap(future, second);
    BOOST_CHECK(!future.Latest(next_epoch * SLOTS_PER_EPOCH));
    BOOST_CHECK(future.Submit(Update(Update1()), next_epoch * SLOTS_PER_EPOCH, second.Sink()).result == peer::Result::UNKNOWN_FORK);
}

BOOST_AUTO_TEST_CASE(rejected_policy_cannot_be_repaired_by_peer_input)
{
    for (int mutation{0}; mutation < 10; ++mutation) {
        auto policy{Policy()};
        if (mutation == 0) policy.config.min_participants = 341;
        if (mutation == 1) policy.config.min_participants = 513;
        if (mutation == 2) policy.config.genesis_validators_root = uint256::ONE;
        if (mutation == 3) policy.seconds_per_slot = 1;
        if (mutation == 4) policy.genesis_time = 1;
        if (mutation == 5) policy.max_sync_lag_slots = SLOTS_PER_PERIOD + 1;
        if (mutation == 6) policy.config.forks.front().epoch = 1;
        if (mutation == 7) policy.config.forks[1].epoch = policy.config.forks[2].epoch;
        if (mutation == 8) policy.max_recovery_lag_slots = SLOTS_PER_PERIOD + 1;
        if (mutation == 9) policy.max_recovery_lag_slots = 0;
        BOOST_CHECK(!policy.Valid());
        peer::LightClient client{policy};
        BOOST_CHECK(client.Fenced());
        BOOST_CHECK(!client.EndReplay());
        Journal journal;
        BOOST_CHECK(client.Submit(Bootstrap(), fx::U1_SIGNATURE_SLOT, journal.Sink()).result == peer::Result::STORAGE_FENCED);
        BOOST_CHECK(journal.entries.empty());
    }
}

BOOST_AUTO_TEST_CASE(exact_repeats_do_not_grow_the_durable_journal)
{
    peer::LightClient client{Policy()};
    Journal journal;
    Bootstrap(client, journal);
    BOOST_REQUIRE(client.Submit(Update(Update1()), fx::U1_SIGNATURE_SLOT, journal.Sink()).result == peer::Result::OK);
    const auto digest{client.JournalDigest()};
    BOOST_CHECK(client.Submit(Update(Update1()), fx::U1_SIGNATURE_SLOT, journal.Sink()).result == peer::Result::NO_CHANGE);
    BOOST_CHECK_EQUAL(journal.entries.size(), 2U);
    BOOST_CHECK(client.JournalDigest() == digest);
}

BOOST_AUTO_TEST_CASE(replay_refuses_truncation_wrong_prefix_order_and_policy)
{
    peer::LightClient producer{Policy()};
    Journal journal;
    Bootstrap(producer, journal);
    BOOST_REQUIRE(producer.Submit(Update(Update1()), fx::U1_SIGNATURE_SLOT, journal.Sink()).result == peer::Result::OK);
    for (int mutation{0}; mutation < 5; ++mutation) {
        auto policy{Policy()};
        if (mutation == 4) --policy.max_sync_lag_slots;
        peer::LightClient reopened{policy};
        auto entry{journal.entries[0]};
        if (mutation == 0) entry.pop_back();
        if (mutation == 1) entry[0] ^= 1;
        if (mutation == 2) entry[20] ^= 1;
        if (mutation == 3) entry = journal.entries[1];
        BOOST_CHECK(reopened.ReplayEntry(entry).result == peer::Result::BAD_JOURNAL);
        BOOST_CHECK(reopened.Fenced());
        BOOST_CHECK(!reopened.EndReplay());
        BOOST_CHECK(!reopened.Latest(fx::U1_SIGNATURE_SLOT));
    }
}

BOOST_AUTO_TEST_CASE(replay_reverifies_proofs_even_when_entry_checksum_is_recomputed)
{
    peer::LightClient producer{Policy()};
    Journal journal;
    Bootstrap(producer, journal);
    auto entry{journal.entries[0]};
    entry[entry.size() - 33] ^= 1; // Last byte of the proven committee branch.
    const auto digest{peer::TaggedDigest("B3/ethereum-peer-entry/1", std::span<const unsigned char>{entry}.first(entry.size() - 32))};
    std::copy(digest.begin(), digest.end(), entry.end() - 32);
    peer::LightClient reopened{Policy()};
    BOOST_CHECK(reopened.ReplayEntry(entry).result == peer::Result::BAD_JOURNAL);
    BOOST_CHECK(reopened.Fenced());
    BOOST_CHECK(!reopened.Store());
}

BOOST_AUTO_TEST_CASE(noncanonical_payload_and_unsigned_feed_cannot_become_proofs)
{
    peer::LightClient client{Policy()};
    Journal journal;
    BOOST_REQUIRE(client.EndReplay());
    auto record{Bootstrap()};
    record.push_back(0);
    BOOST_CHECK(client.Submit(record, fx::U1_SIGNATURE_SLOT, journal.Sink()).result == peer::Result::BAD_RECORD);
    const std::string feed{"{\"verified\":true,\"finalized\":true,\"block_hash\":\"0x01\"}"};
    BOOST_CHECK(client.Submit(std::span<const unsigned char>{reinterpret_cast<const unsigned char*>(feed.data()), feed.size()},
                              fx::U1_SIGNATURE_SLOT, journal.Sink()).result == peer::Result::BAD_RECORD);
    BOOST_CHECK(journal.entries.empty());
}

BOOST_AUTO_TEST_SUITE_END()

