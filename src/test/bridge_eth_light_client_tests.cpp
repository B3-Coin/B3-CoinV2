// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Bridge stage 3 (ETH -> B3 mint leg): the sync-committee light client
// driven end to end over REAL Ethereum mainnet data — bootstrap at a
// finalized checkpoint, one full update, a committee rotation across a
// period boundary, and the proven finalized execution receipts_root that
// anchors bridge/mpt.h receipt proofs. Fixture:
// src/test/data/eth_lc_fixture.h (captured by contrib/b3bridge).

#include <bridge/eth_light_client.h>
#include <bridge/lc_json.h>
#include <bridge/ssz.h>
#include <test/data/eth_lc_fixture.h>
#include <uint256.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

using namespace bridge;
namespace fx = eth_lc_fixture;

BOOST_AUTO_TEST_SUITE(bridge_eth_light_client_tests)

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

BOOST_AUTO_TEST_CASE(mainnet_bootstrap_and_rotation)
{
    const auto cfg{Config()};
    auto store{Bootstrapped(cfg)};
    const uint64_t p0{store.period};
    BOOST_CHECK(!store.next);

    // Update for the bootstrap period: full verification incl. the real
    // 512-member BLS aggregate, finality + execution proofs, next committee.
    const auto u1{Update1()};
    BOOST_REQUIRE(VerifyUpdate(store, cfg, u1) == LcResult::OK);
    BOOST_REQUIRE(ProcessUpdate(store, cfg, u1) == LcResult::OK);
    BOOST_CHECK_EQUAL(store.period, p0);
    BOOST_REQUIRE(store.next.has_value());

    // Update for the NEXT period: signed by the handed-over committee;
    // applying it rotates the store across the period boundary.
    const auto u2{Update2()};
    BOOST_REQUIRE(VerifyUpdate(store, cfg, u2) == LcResult::OK);
    BOOST_REQUIRE(ProcessUpdate(store, cfg, u2) == LcResult::OK);
    BOOST_CHECK_EQUAL(store.period, p0 + 1);
    BOOST_REQUIRE(store.next.has_value()); // adopted u2's next committee

    // The deposit-leg anchor: a PROVEN finalized execution receipts_root.
    BOOST_CHECK(store.finalized_header.execution.receipts_root != uint256{});
    BOOST_CHECK(store.finalized_header.execution.block_number > 0);
    BOOST_CHECK(store.finalized_header.beacon.slot >= u2.finalized.beacon.slot);
}

BOOST_AUTO_TEST_CASE(mainnet_rejections)
{
    const auto cfg{Config()};
    auto store{Bootstrapped(cfg)};
    const auto u1{Update1()};

    { // Any flipped sync bit changes the participant set: signature must die.
        auto bad{u1};
        bad.sync_aggregate.bits[7] ^= 0x10;
        BOOST_CHECK(VerifyUpdate(store, cfg, bad) == LcResult::SIGNATURE);
    }
    { // Mutated aggregate signature.
        auto bad{u1};
        bad.sync_aggregate.signature[10] ^= 0x01;
        const auto r{VerifyUpdate(store, cfg, bad)};
        BOOST_CHECK(r == LcResult::SIGNATURE || r == LcResult::BAD_STRUCTURE);
    }
    { // Mutated finalized header: finality proof fails.
        auto bad{u1};
        bad.finalized.beacon.state_root = uint256::ONE;
        BOOST_CHECK(VerifyUpdate(store, cfg, bad) == LcResult::FINALITY_PROOF);
    }
    { // Mutated finalized execution payload (a forged receipts_root!).
        auto bad{u1};
        bad.finalized.execution.receipts_root = uint256::ONE;
        BOOST_CHECK(VerifyUpdate(store, cfg, bad) == LcResult::EXECUTION_PROOF);
    }
    { // Mutated next committee: handover proof fails.
        auto bad{u1};
        bad.next_committee.pubkeys[0][5] ^= 0x01;
        BOOST_CHECK(VerifyUpdate(store, cfg, bad) == LcResult::NEXT_PROOF);
    }
    { // Skipping ahead: the period-P+1 update is unusable before u1.
        BOOST_CHECK(VerifyUpdate(store, cfg, Update2()) == LcResult::PERIOD);
    }
    { // Participation floor: demand more signers than participated.
        auto strict{cfg};
        strict.min_participants = 513;
        BOOST_CHECK(VerifyUpdate(store, strict, u1) == LcResult::PARTICIPATION);
    }
    { // Wrong chain (genesis validators root): domain separation must bite.
        auto wrong{cfg};
        wrong.genesis_validators_root = uint256::ONE;
        BOOST_CHECK(VerifyUpdate(store, wrong, u1) == LcResult::SIGNATURE);
    }
    { // Signature slot not after the attested slot.
        auto bad{u1};
        bad.signature_slot = bad.attested.beacon.slot;
        BOOST_CHECK(VerifyUpdate(store, cfg, bad) == LcResult::MONOTONICITY);
    }
}

BOOST_AUTO_TEST_CASE(mainnet_older_update_can_complete_committee_handover)
{
    const auto cfg{Config()};
    const auto update{Update1()};
    auto store{Bootstrapped(cfg)};

    // Model a trusted bootstrap taken later in the same committee period than
    // the canonical best update returned for that period.
    store.finalized_header.beacon.slot = update.attested.beacon.slot + 1;
    BOOST_REQUIRE_EQUAL(PeriodAtSlot(store.finalized_header.beacon.slot),
                        store.period);
    BOOST_REQUIRE(update.finalized.beacon.slot <
                  store.finalized_header.beacon.slot);
    const auto newer_finalized{store.finalized_header};
    const auto current_committee_root{store.current.HashTreeRoot()};
    const uint64_t current_period{store.period};

    BOOST_REQUIRE(VerifyUpdate(store, cfg, update) == LcResult::OK);
    BOOST_REQUIRE(ProcessUpdate(store, cfg, update) == LcResult::OK);
    BOOST_CHECK(store.finalized_header == newer_finalized);
    BOOST_CHECK_EQUAL(store.period, current_period);
    BOOST_CHECK(store.current.HashTreeRoot() == current_committee_root);
    BOOST_REQUIRE(store.next.has_value());
    BOOST_CHECK(store.next->HashTreeRoot() ==
                update.next_committee.HashTreeRoot());

    // The exception is only for filling an unknown same-period committee.
    // Ordinary stale updates and repeat attempts remain rejected.
    auto without_next{update};
    without_next.has_next = false;
    auto fresh_store{Bootstrapped(cfg)};
    fresh_store.finalized_header = newer_finalized;
    BOOST_CHECK(VerifyUpdate(fresh_store, cfg, without_next) ==
                LcResult::MONOTONICITY);
    BOOST_CHECK(VerifyUpdate(store, cfg, update) == LcResult::MONOTONICITY);

    auto wrong_period{update};
    wrong_period.signature_slot += SLOTS_PER_PERIOD;
    BOOST_CHECK(VerifyUpdate(fresh_store, cfg, wrong_period) ==
                LcResult::MONOTONICITY);

    auto prior_finalized_period{update};
    prior_finalized_period.finalized.beacon.slot -= SLOTS_PER_PERIOD;
    BOOST_CHECK(VerifyUpdate(fresh_store, cfg, prior_finalized_period) ==
                LcResult::MONOTONICITY);

    auto bad_finality{update};
    bad_finality.finalized.beacon.state_root = uint256::ONE;
    BOOST_CHECK(VerifyUpdate(fresh_store, cfg, bad_finality) ==
                LcResult::FINALITY_PROOF);

    auto bad_execution{update};
    bad_execution.finalized.execution.receipts_root = uint256::ONE;
    BOOST_CHECK(VerifyUpdate(fresh_store, cfg, bad_execution) ==
                LcResult::EXECUTION_PROOF);

    auto bad_attested_execution{update};
    bad_attested_execution.attested.execution.receipts_root = uint256::ONE;
    BOOST_CHECK(VerifyUpdate(fresh_store, cfg, bad_attested_execution) ==
                LcResult::EXECUTION_PROOF);

    auto bad_next{update};
    bad_next.next_committee.pubkeys[0][5] ^= 0x01;
    BOOST_CHECK(VerifyUpdate(fresh_store, cfg, bad_next) ==
                LcResult::NEXT_PROOF);

    auto strict{cfg};
    strict.min_participants = 513;
    BOOST_CHECK(VerifyUpdate(fresh_store, strict, update) ==
                LcResult::PARTICIPATION);

    auto bad_signature{update};
    bad_signature.sync_aggregate.signature[10] ^= 0x01;
    const auto bad_signature_result{VerifyUpdate(fresh_store, cfg, bad_signature)};
    BOOST_CHECK(bad_signature_result == LcResult::SIGNATURE ||
                bad_signature_result == LcResult::BAD_STRUCTURE);
}

BOOST_AUTO_TEST_CASE(finalized_store_snapshot_json_roundtrip)
{
    const auto cfg{Config()};
    auto store{Bootstrapped(cfg)};
    const auto without_next{lcjson::ParseStore(lcjson::StoreJson(store))};
    BOOST_CHECK(without_next == store);
    BOOST_CHECK(!without_next.next.has_value());

    BOOST_REQUIRE(ProcessUpdate(store, cfg, Update1()) == LcResult::OK);
    BOOST_REQUIRE(store.next.has_value());

    const uint256 connection_hash{uint256::FromHex(
        "0000000000000000000000000000000000000000000000000000000000000011").value()};
    const uint256 finalized_hash{uint256::FromHex(
        "0000000000000000000000000000000000000000000000000000000000000022").value()};
    const UniValue json{lcjson::StoreSnapshotJson(
        store, 100, connection_hash, 120, finalized_hash)};
    const auto parsed{lcjson::ParseStoreSnapshot(json)};
    BOOST_CHECK(parsed.store == store);
    BOOST_CHECK_EQUAL(parsed.connection_height, 100U);
    BOOST_CHECK(parsed.connection_block_hash == connection_hash);
    BOOST_CHECK_EQUAL(parsed.b3_finalized_height, 120U);
    BOOST_CHECK(parsed.b3_finalized_block_hash == finalized_hash);

    // The decimal-only beacon representation of uint256 base fee is exact.
    BOOST_CHECK(parsed.store.finalized_header.execution.base_fee_per_gas ==
                store.finalized_header.execution.base_fee_per_gas);
}

BOOST_AUTO_TEST_CASE(finalized_store_snapshot_rejects_untrusted_shapes)
{
    const auto cfg{Config()};
    const auto good{Bootstrapped(cfg)};
    const uint256 connection_hash{uint256::FromHex(
        "0000000000000000000000000000000000000000000000000000000000000011").value()};
    const uint256 finalized_hash{uint256::FromHex(
        "0000000000000000000000000000000000000000000000000000000000000022").value()};

    auto bad_period{good};
    ++bad_period.period;
    BOOST_CHECK_THROW(
        lcjson::ParseStoreSnapshot(lcjson::StoreSnapshotJson(
            bad_period, 100, connection_hash, 120, finalized_hash)),
        std::runtime_error);

    auto bad_execution{good};
    bad_execution.finalized_header.execution_branch.pop_back();
    BOOST_CHECK_THROW(
        lcjson::ParseStoreSnapshot(lcjson::StoreSnapshotJson(
            bad_execution, 100, connection_hash, 120, finalized_hash)),
        std::runtime_error);

    auto bad_committee{good};
    bad_committee.current.pubkeys.pop_back();
    BOOST_CHECK_THROW(
        lcjson::ParseStoreSnapshot(lcjson::StoreSnapshotJson(
            bad_committee, 100, connection_hash, 120, finalized_hash)),
        std::runtime_error);

    BOOST_CHECK_THROW(
        lcjson::ParseStoreSnapshot(lcjson::StoreSnapshotJson(
            good, 121, connection_hash, 120, finalized_hash)),
        std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()
