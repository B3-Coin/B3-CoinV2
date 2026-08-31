// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <flowmesh/auth.h>
#include <flowmesh/market.h>
#include <flowmesh/production_engine.h>
#include <streams.h>
#include <test/util/flowmesh.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <numeric>
#include <vector>

namespace {

uint256 Filled(const unsigned char value)
{
    uint256 out;
    std::fill(out.begin(), out.end(), value);
    return out;
}

bls::SecretKey Key(const uint32_t index, const unsigned char salt)
{
    std::array<unsigned char, 32> ikm{};
    for (size_t i{0}; i < ikm.size(); ++i) {
        ikm[i] = static_cast<unsigned char>(salt + index * 17 + i * 11);
    }
    const auto key{bls::SecretKey::FromIKM(ikm)};
    BOOST_REQUIRE(key.has_value());
    return *key;
}

struct SeatFixture {
    uint256 domain;
    flowmesh::MarketId market;
    std::vector<bls::SecretKey> secrets;
    std::vector<flowmesh::BlsSeatBinding> bindings;
    flowmesh::ActiveFnBlsSeatSet seats;
};

SeatFixture Seats(const uint256& domain, const flowmesh::MarketId& market,
                  const size_t count, const uint64_t epoch,
                  const uint64_t anchor_height, const uint256& anchor_hash,
                  const unsigned char salt)
{
    struct Entry {
        bls::SecretKey secret;
        flowmesh::BlsSeatBinding binding;
        flowmesh::SeatId seat_id;
    };
    std::vector<Entry> entries;
    for (size_t i{0}; i < count; ++i) {
        const bls::SecretKey secret{Key(i, salt)};
        flowmesh::BlsSeatBinding binding;
        binding.outpoint = COutPoint{
            Txid::FromUint256(Filled(static_cast<unsigned char>(salt + i + 30))),
            static_cast<uint32_t>(i + salt)};
        binding.public_key = secret.GetPublicKey().Compressed();
        binding.proof_of_possession = secret.SignPoP().Compressed();
        entries.push_back({secret, binding,
                           flowmesh::ComputeFlowMeshSeatId(domain, binding.outpoint)});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return a.seat_id < b.seat_id ||
               (a.seat_id == b.seat_id && a.binding.outpoint < b.binding.outpoint);
    });

    SeatFixture fixture;
    fixture.domain = domain;
    fixture.market = market;
    for (const Entry& entry : entries) {
        fixture.secrets.push_back(entry.secret);
        fixture.bindings.push_back(entry.binding);
    }
    flowmesh::BlsSeatSetCheck check{flowmesh::BlsSeatSetCheck::BAD_SET_HASH};
    const auto seats{flowmesh::BuildActiveFnBlsSeatSet(
        domain, market, epoch, anchor_height, anchor_hash, fixture.bindings, check)};
    BOOST_REQUIRE(seats.has_value());
    BOOST_REQUIRE(check == flowmesh::BlsSeatSetCheck::OK);
    fixture.seats = *seats;
    return fixture;
}

class TestAnchorPolicy final : public flowmesh::AnchorPolicy
{
public:
    explicit TestAnchorPolicy(const int32_t tip) : m_tip{tip} {}

    void Add(const flowmesh::AnchorRef& anchor) { m_canonical[anchor.height] = anchor.hash; }

    bool Acceptable(const flowmesh::AnchorRef& anchor) const override
    {
        return StillCanonical(anchor) && m_tip >= anchor.height &&
               m_tip - anchor.height >= flowmesh::FLOWMESH_PRODUCTION_MIN_ANCHOR_DEPTH;
    }

    bool StillCanonical(const flowmesh::AnchorRef& anchor) const override
    {
        const auto it{m_canonical.find(anchor.height)};
        return it != m_canonical.end() && it->second == anchor.hash;
    }

    flowmesh::AnchorRef Current() const override { return {m_tip, Filled(0xfe)}; }

private:
    int32_t m_tip;
    std::map<int32_t, uint256> m_canonical;
};

class MemoryLockJournal final : public flowmesh::DurableProductionLockJournal
{
public:
    flowmesh::ProductionLockResult LockOnce(
        const flowmesh::ProductionSignPosition& position,
        const uint256& entry_hash) override
    {
        if (fail) return flowmesh::ProductionLockResult::STORAGE_FAILURE;
        const auto [it, inserted]{locks.emplace(position, entry_hash)};
        if (inserted) return flowmesh::ProductionLockResult::LOCKED;
        return it->second == entry_hash
                   ? flowmesh::ProductionLockResult::ALREADY_LOCKED_SAME
                   : flowmesh::ProductionLockResult::CONFLICT;
    }

    bool fail{false};
    std::map<flowmesh::ProductionSignPosition, uint256> locks;
};

struct MarketFixture {
    uint256 domain{Filled(0x11)};
    modern::AssetId base{Filled(0x31)};
    flowmesh::MarketId market{*flowmesh::ComputeFlowMeshMarketId(domain, base)};
    flowmesh::VaultId vault{*flowmesh::ComputeFlowMeshVaultId(domain, market)};
    uint256 treasury{Filled(0x51)};
    flowmesh::AccountId buyer{Filled(0x61)};
    flowmesh::AccountId seller{Filled(0x62)};
};

flowmesh::Action Bid(const flowmesh::AccountId& account, const uint64_t sequence,
                     const CAmount price, const CAmount amount)
{
    flowmesh::Action action;
    action.signer = account;
    action.sequence = sequence;
    action.type = static_cast<uint8_t>(flowmesh::ActionType::SUBMIT_BID);
    action.curve = *flowmesh::MakeLimitBidCurve(price, amount);
    return action;
}

flowmesh::Action Ask(const flowmesh::AccountId& account, const uint64_t sequence,
                     const CAmount price, const CAmount amount)
{
    flowmesh::Action action;
    action.signer = account;
    action.sequence = sequence;
    action.type = static_cast<uint8_t>(flowmesh::ActionType::SUBMIT_ASK);
    action.curve = *flowmesh::MakeLimitAskCurve(price, amount);
    return action;
}

flowmesh::Action Deposit(const COutPoint& outpoint)
{
    flowmesh::Action action;
    action.type = static_cast<uint8_t>(flowmesh::ActionType::DEPOSIT);
    action.outpoint = outpoint;
    return action;
}

flowmesh::Action Withdraw(const flowmesh::AccountId& account,
                          const uint64_t sequence,
                          const modern::AssetId& asset, const CAmount amount,
                          const uint256& destination)
{
    flowmesh::Action action;
    action.signer = account;
    action.sequence = sequence;
    action.type = static_cast<uint8_t>(flowmesh::ActionType::WITHDRAW);
    action.asset = asset;
    action.amount = amount;
    action.destination = destination;
    return action;
}

class MapDeposits final : public flowmesh::DepositVerifier
{
public:
    std::map<COutPoint, flowmesh::DepositInfo> entries;
    flowmesh::AnchorRef required_anchor;
    std::optional<flowmesh::AnchorRef> required_settlement_after;
    flowmesh::AnchorRef required_settlement_through;
    std::vector<flowmesh::WithdrawalSettlementFactV1> settlements;

    std::optional<flowmesh::DepositInfo> GetDeposit(
        const COutPoint& outpoint,
        const flowmesh::AnchorRef& anchor) const override
    {
        if (!(anchor == required_anchor)) return std::nullopt;
        const auto it{entries.find(outpoint)};
        return it == entries.end()
                   ? std::nullopt
                   : std::optional<flowmesh::DepositInfo>{it->second};
    }

    std::optional<CAmount> GetWithdrawalCapacity(
        const modern::AssetId&, const flowmesh::AnchorRef&) const override
    {
        return withdrawal_capacity;
    }

    std::optional<std::vector<flowmesh::WithdrawalSettlementFactV1>>
    GetWithdrawalSettlements(
        const std::optional<flowmesh::AnchorRef>& after,
        const flowmesh::AnchorRef& through) const override
    {
        if (!settlements.empty() &&
            (after != required_settlement_after ||
             !(through == required_settlement_through))) {
            return std::nullopt;
        }
        return settlements;
    }

    CAmount withdrawal_capacity{MAX_MONEY};
};

CKey SchnorrKey(const unsigned char seed)
{
    std::array<unsigned char, 32> bytes{};
    bytes.fill(seed);
    bytes.back() = 1;
    CKey key;
    key.Set(bytes.begin(), bytes.end(), true);
    BOOST_REQUIRE(key.IsValid());
    return key;
}

flowmesh::BlsMicroblockCertificate Certify(
    const flowmesh::ProductionEntryCore& entry, const SeatFixture& fixture)
{
    std::vector<flowmesh::IndexedBlsSignature> partials;
    for (uint32_t i{0}; i < flowmesh::FlowMeshBlsThreshold(fixture.seats.Size()); ++i) {
        MemoryLockJournal journal;
        flowmesh::ProductionSigningGuard guard{journal};
        flowmesh::ProductionLockResult lock;
        const auto partial{flowmesh::SignProductionEntryAttestation(
            fixture.secrets[i], i, entry, fixture.seats, guard, lock)};
        BOOST_REQUIRE(partial.has_value());
        BOOST_REQUIRE(lock == flowmesh::ProductionLockResult::LOCKED);
        partials.push_back(*partial);
    }
    flowmesh::BlsMicroblockCertificate certificate;
    BOOST_REQUIRE(flowmesh::AssembleProductionEntryCertificate(
                      entry, fixture.seats, partials, certificate) ==
                  flowmesh::BlsCertificateAssemblyCheck::OK);
    return certificate;
}

struct BuiltFixture {
    MarketFixture market;
    SeatFixture seats;
    flowmesh::FlowMeshState previous;
    flowmesh::ProductionEpochGate gate;
    TestAnchorPolicy anchors;
    flowmesh::ProductionAnchorContext anchor_context;
    flowmesh::BuiltProductionExecution built;

    BuiltFixture()
        : seats{Seats(market.domain, market.market, 4, 7, 100, Filled(0x71), 1)},
          previous{market.vault, market.base, modern::NativeAsset(),
                   flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS},
          gate{market.domain, market.market, seats.seats},
          anchors{260},
          anchor_context{260, flowmesh::AnchorRef{190, Filled(0x73)}, &anchors},
          built{[&] {
              const flowmesh::AnchorRef set_anchor{100, Filled(0x71)};
              const flowmesh::AnchorRef previous_anchor{190, Filled(0x73)};
              const flowmesh::AnchorRef entry_anchor{200, Filled(0x74)};
              anchors.Add(set_anchor);
              anchors.Add(previous_anchor);
              anchors.Add(entry_anchor);
              BOOST_REQUIRE(flowmesh::test_only::StateFunding::Fund(
                  previous, market.buyer, modern::NativeAsset(), 20'000));
              BOOST_REQUIRE(flowmesh::test_only::StateFunding::Fund(
                  previous, market.seller, market.base, 200));
              const std::vector<flowmesh::Action> actions{
                  Ask(market.seller, 0, 100, 200),
                  Bid(market.buyer, 0, 100, 200)};
              flowmesh::ProductionEntryCheck check;
              const auto built{flowmesh::BuildProductionExecutionEntry(
                  previous, market.domain, market.market, seats.seats, gate,
                  0, 0, uint256{}, entry_anchor, anchor_context, market.treasury,
                  actions, nullptr, check)};
              BOOST_REQUIRE_MESSAGE(built.has_value(),
                                    flowmesh::ProductionEntryCheckName(check));
              return *built;
          }()}
    {
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(flowmesh_production_engine_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(four_seat_execution_is_fee_mandatory_and_bounded)
{
    BuiltFixture fixture;
    const auto& entry{fixture.built.entry};
    BOOST_CHECK_EQUAL(entry.version, flowmesh::FLOWMESH_PRODUCTION_ENTRY_VERSION_V1);
    BOOST_CHECK_EQUAL(entry.kind,
                      static_cast<uint8_t>(flowmesh::ProductionEntryKind::EXECUTION));
    BOOST_CHECK(entry.domain == fixture.market.domain);
    BOOST_CHECK(entry.market_id == fixture.market.market);
    BOOST_CHECK_EQUAL(entry.epoch, 7U);
    BOOST_CHECK(entry.seat_set_hash == fixture.seats.seats.set_hash);
    BOOST_CHECK_EQUAL(entry.actions.size(), 2U);

    BOOST_CHECK_EQUAL(fixture.built.result.clearing.volume, 200);
    BOOST_CHECK_EQUAL(fixture.built.result.clearing.price, 100);
    BOOST_CHECK_EQUAL(fixture.built.result.clearing.fees.matched_b3_quote_notional,
                      20'000);
    BOOST_CHECK_EQUAL(fixture.built.result.clearing.fees.fee_total, 2);
    BOOST_REQUIRE_EQUAL(fixture.built.result.clearing.fees.seat_rewards.size(), 4U);

    const auto encoded{flowmesh::EncodeProductionEntry(entry)};
    BOOST_REQUIRE(encoded.has_value());
    BOOST_CHECK_LE(encoded->size(), flowmesh::FLOWMESH_V1_MAX_MICROBLOCK_BYTES);
    const auto decoded{flowmesh::DecodeProductionEntry(*encoded)};
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK(decoded->GetHash() == entry.GetHash());

    flowmesh::ProductionEntryCheck check;
    const auto executed{flowmesh::ExecuteProductionEntry(
        fixture.previous, entry, fixture.market.domain, fixture.market.market,
        fixture.seats.seats, fixture.gate, 0, 0, uint256{}, fixture.anchor_context,
        fixture.market.treasury, nullptr, check)};
    BOOST_REQUIRE_MESSAGE(executed.has_value(), flowmesh::ProductionEntryCheckName(check));
    const auto result_root{flowmesh::ComputeProductionExecutionResultRoot(
        executed->result.result_commitment, executed->effects)};
    BOOST_REQUIRE(result_root.has_value());
    BOOST_CHECK(*result_root == entry.result_root);
    BOOST_CHECK(executed->next_state.Root() == entry.state_root);

    std::vector<unsigned char> too_large(
        flowmesh::FLOWMESH_V1_MAX_MICROBLOCK_BYTES + 1, 0);
    BOOST_CHECK(!flowmesh::DecodeProductionEntry(too_large));

    flowmesh::ProductionEntryCore too_many{entry};
    too_many.actions.assign(flowmesh::FLOWMESH_V1_MAX_MICROBLOCK_ACTIONS + 1,
                            flowmesh::Action{});
    std::vector<unsigned char> raw;
    VectorWriter writer{raw, 0};
    writer << too_many;
    BOOST_CHECK(!flowmesh::DecodeProductionEntry(raw));
    BOOST_CHECK(!flowmesh::EncodeProductionEntry(too_many));
}

BOOST_AUTO_TEST_CASE(context_proposal_and_certificate_checks_are_strict_for_k4_and_k5)
{
    BuiltFixture fixture;
    flowmesh::ProductionEntryCheck entry_check;
    const auto run = [&](const flowmesh::ProductionEntryCore& candidate,
                         const flowmesh::ProductionEntryCheck expected) {
        const auto result{flowmesh::ExecuteProductionEntry(
            fixture.previous, candidate, fixture.market.domain,
            fixture.market.market, fixture.seats.seats, fixture.gate, 0,
            0, uint256{}, fixture.anchor_context, fixture.market.treasury, nullptr,
            entry_check)};
        BOOST_CHECK(!result);
        BOOST_CHECK(entry_check == expected);
    };

    auto wrong{fixture.built.entry};
    wrong.domain = Filled(0x91);
    run(wrong, flowmesh::ProductionEntryCheck::WRONG_DOMAIN);
    wrong = fixture.built.entry;
    wrong.market_id = Filled(0x92);
    run(wrong, flowmesh::ProductionEntryCheck::WRONG_MARKET);
    wrong = fixture.built.entry;
    ++wrong.epoch;
    run(wrong, flowmesh::ProductionEntryCheck::WRONG_EPOCH);
    wrong = fixture.built.entry;
    wrong.seat_set_hash = Filled(0x93);
    run(wrong, flowmesh::ProductionEntryCheck::WRONG_SEAT_SET);
    wrong = fixture.built.entry;
    ++wrong.sequence;
    wrong.parent_hash = Filled(0x94);
    run(wrong, flowmesh::ProductionEntryCheck::WRONG_SEQUENCE);
    wrong = fixture.built.entry;
    wrong.anchor.hash = Filled(0x95);
    run(wrong, flowmesh::ProductionEntryCheck::BAD_ANCHOR);
    wrong = fixture.built.entry;
    wrong.result_root = Filled(0x96);
    run(wrong, flowmesh::ProductionEntryCheck::WRONG_RESULT_ROOT);
    wrong = fixture.built.entry;
    wrong.state_root = Filled(0x97);
    run(wrong, flowmesh::ProductionEntryCheck::WRONG_STATE_ROOT);

    for (const size_t count : {size_t{4}, size_t{5}}) {
        const SeatFixture seats{count == 4
                                    ? fixture.seats
                                    : Seats(fixture.market.domain,
                                            fixture.market.market, 5, 7, 100,
                                            Filled(0x71), 22)};
        flowmesh::ProductionEntryCore entry{fixture.built.entry};
        entry.seat_set_hash = seats.seats.set_hash;
        entry.epoch = seats.seats.epoch;
        const auto certificate{Certify(entry, seats)};
        BOOST_CHECK(flowmesh::CheckProductionEntryCertificate(
                        entry, seats.seats, certificate) ==
                    flowmesh::BlsCertificateCheck::OK);
        BOOST_CHECK_EQUAL(certificate.signer_bitmap.size(), 1U);

        constexpr uint32_t round{3};
        const uint32_t proposer{flowmesh::ProductionProposerSeatIndex(
            entry.sequence, round, seats.seats.Size())};
        MemoryLockJournal journal;
        flowmesh::ProductionSigningGuard guard{journal};
        flowmesh::ProductionProposalCheck proposal_check;
        const auto proposal{flowmesh::SignProductionProposal(
            seats.secrets[proposer], entry, round, seats.seats, guard,
            proposal_check)};
        BOOST_REQUIRE(proposal.has_value());
        BOOST_CHECK(proposal_check == flowmesh::ProductionProposalCheck::OK);
        BOOST_CHECK(flowmesh::CheckProductionProposal(
                        *proposal, fixture.market.domain, fixture.market.market,
                        7, round, seats.seats) ==
                    flowmesh::ProductionProposalCheck::OK);

        auto bad_proposer{*proposal};
        bad_proposer.proposer_seat_index = (proposer + 1) % seats.seats.Size();
        BOOST_CHECK(flowmesh::CheckProductionProposal(
                        bad_proposer, fixture.market.domain,
                        fixture.market.market, 7, round, seats.seats) ==
                    flowmesh::ProductionProposalCheck::WRONG_PROPOSER);
        BOOST_CHECK(flowmesh::CheckProductionProposal(
                        *proposal, fixture.market.domain, fixture.market.market,
                        7, round + 1, seats.seats) ==
                    flowmesh::ProductionProposalCheck::WRONG_ROUND);

        auto bad_signature{*proposal};
        const uint32_t other{static_cast<uint32_t>(
            (proposer + 1) % seats.seats.Size())};
        const uint256 digest{flowmesh::ProductionProposalDigest(entry, round)};
        bad_signature.proposer_signature =
            seats.secrets[other]
                .Sign(std::span<const unsigned char>{digest.begin(), 32})
                .Compressed();
        BOOST_CHECK(flowmesh::CheckProductionProposal(
                        bad_signature, fixture.market.domain,
                        fixture.market.market, 7, round, seats.seats) ==
                    flowmesh::ProductionProposalCheck::BAD_SIGNATURE);

        auto wrong_domain{*proposal};
        wrong_domain.entry.domain = Filled(0xa1);
        BOOST_CHECK(flowmesh::CheckProductionProposal(
                        wrong_domain, fixture.market.domain,
                        fixture.market.market, 7, round, seats.seats) ==
                    flowmesh::ProductionProposalCheck::WRONG_DOMAIN);
        auto wrong_market{*proposal};
        wrong_market.entry.market_id = Filled(0xa2);
        BOOST_CHECK(flowmesh::CheckProductionProposal(
                        wrong_market, fixture.market.domain,
                        fixture.market.market, 7, round, seats.seats) ==
                    flowmesh::ProductionProposalCheck::WRONG_MARKET);
        auto wrong_epoch{*proposal};
        ++wrong_epoch.entry.epoch;
        BOOST_CHECK(flowmesh::CheckProductionProposal(
                        wrong_epoch, fixture.market.domain,
                        fixture.market.market, 7, round, seats.seats) ==
                    flowmesh::ProductionProposalCheck::WRONG_EPOCH);
        auto wrong_set{*proposal};
        wrong_set.entry.seat_set_hash = Filled(0xa3);
        BOOST_CHECK(flowmesh::CheckProductionProposal(
                        wrong_set, fixture.market.domain,
                        fixture.market.market, 7, round, seats.seats) ==
                    flowmesh::ProductionProposalCheck::WRONG_SEAT_SET);
    }
}

BOOST_AUTO_TEST_CASE(deposit_and_user_withdrawal_effects_are_fully_derived)
{
    MarketFixture market;
    const SeatFixture seats{
        Seats(market.domain, market.market, 4, 7, 100, Filled(0xc1), 61)};
    flowmesh::FlowMeshState previous{
        market.vault, market.base, modern::NativeAsset(),
        flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
    flowmesh::ProductionEpochGate gate{market.domain, market.market,
                                       seats.seats};
    TestAnchorPolicy anchors{260};
    const flowmesh::AnchorRef set_anchor{100, Filled(0xc1)};
    const flowmesh::AnchorRef prior_anchor{190, Filled(0xc2)};
    const flowmesh::AnchorRef entry_anchor{200, Filled(0xc3)};
    anchors.Add(set_anchor);
    anchors.Add(prior_anchor);
    anchors.Add(entry_anchor);
    const flowmesh::ProductionAnchorContext anchor_context{
        260, prior_anchor, &anchors};

    const COutPoint deposit_outpoint{
        Txid::FromUint256(Filled(0xc4)), 7};
    const uint256 destination{Filled(0xc5)};
    MapDeposits deposits;
    deposits.required_anchor = entry_anchor;
    deposits.entries.emplace(
        deposit_outpoint,
        flowmesh::DepositInfo{market.base, 100, market.buyer});
    const std::vector<flowmesh::Action> actions{
        Withdraw(market.buyer, 0, market.base, 40, destination),
        Deposit(deposit_outpoint)};

    flowmesh::ProductionEntryCheck check;
    const auto built{flowmesh::BuildProductionExecutionEntry(
        previous, market.domain, market.market, seats.seats, gate, 0,
        0, uint256{}, entry_anchor, anchor_context, market.treasury, actions,
        &deposits, check)};
    BOOST_REQUIRE_MESSAGE(built.has_value(),
                          flowmesh::ProductionEntryCheckName(check));
    BOOST_REQUIRE_EQUAL(built->result.credited_deposits.size(), 1U);
    BOOST_REQUIRE_EQUAL(built->result.account_withdrawal_requests.size(), 1U);
    BOOST_REQUIRE_EQUAL(built->effects.size(), 2U);

    const auto* acceptance{
        std::get_if<modern::FlowMeshDepositAcceptanceV1>(&built->effects[0])};
    BOOST_REQUIRE(acceptance != nullptr);
    BOOST_CHECK(acceptance->market_id == market.market);
    BOOST_CHECK_EQUAL(acceptance->epoch, 7U);
    BOOST_CHECK_EQUAL(acceptance->sequence, 0U);
    BOOST_CHECK(acceptance->deposit_outpoint == deposit_outpoint);
    BOOST_CHECK(acceptance->account == market.buyer);
    BOOST_CHECK(acceptance->asset == market.base);
    BOOST_CHECK_EQUAL(acceptance->amount, 100);
    BOOST_CHECK(acceptance->vault_id == market.vault);
    BOOST_CHECK_EQUAL(
        acceptance->shard,
        modern::FlowMeshUserDepositShard(market.vault, market.buyer));
    BOOST_CHECK(acceptance->acceptance_id ==
                flowmesh::ComputeProductionDepositAcceptanceId(*acceptance));

    const auto* receipt{
        std::get_if<modern::FlowMeshWithdrawalReceiptV1>(&built->effects[1])};
    BOOST_REQUIRE(receipt != nullptr);
    BOOST_CHECK(receipt->market_id == market.market);
    BOOST_CHECK_EQUAL(receipt->epoch, 7U);
    BOOST_CHECK_EQUAL(receipt->sequence, 0U);
    BOOST_CHECK(receipt->account == market.buyer);
    BOOST_CHECK(receipt->asset == market.base);
    BOOST_CHECK_EQUAL(receipt->amount, 40);
    BOOST_CHECK(receipt->destination_owner_commitment == destination);
    BOOST_CHECK(receipt->vault_id == market.vault);
    BOOST_CHECK_EQUAL(
        receipt->deterministic_change_shard,
        flowmesh::ComputeProductionWithdrawalChangeShard(
            market.vault, receipt->receipt_id));

    const auto committed{flowmesh::ComputeProductionExecutionResultRoot(
        built->result.result_commitment, built->effects)};
    BOOST_REQUIRE(committed.has_value());
    BOOST_CHECK(*committed == built->entry.result_root);

    // Arrival order is irrelevant, including the derived effect bytes.
    const std::vector<flowmesh::Action> reversed{actions.rbegin(),
                                                 actions.rend()};
    const auto rebuilt{flowmesh::BuildProductionExecutionEntry(
        previous, market.domain, market.market, seats.seats, gate, 0,
        0, uint256{}, entry_anchor, anchor_context, market.treasury, reversed,
        &deposits, check)};
    BOOST_REQUIRE(rebuilt.has_value());
    BOOST_CHECK(rebuilt->effects == built->effects);
    BOOST_CHECK(rebuilt->entry.result_root == built->entry.result_root);

    const auto executed{flowmesh::ExecuteProductionEntry(
        previous, built->entry, market.domain, market.market, seats.seats,
        gate, 0, 0, uint256{}, anchor_context, market.treasury, &deposits,
        check)};
    BOOST_REQUIRE_MESSAGE(executed.has_value(),
                          flowmesh::ProductionEntryCheckName(check));
    BOOST_CHECK(executed->effects == built->effects);

    // Self-consistent alternate account/shard facts and a redirected
    // withdrawal all produce different production result commitments.
    auto changed_account{built->effects};
    auto& changed_acceptance{
        std::get<modern::FlowMeshDepositAcceptanceV1>(changed_account[0])};
    changed_acceptance.account = Filled(0xc6);
    changed_acceptance.shard = modern::FlowMeshUserDepositShard(
        changed_acceptance.vault_id, changed_acceptance.account);
    changed_acceptance.acceptance_id =
        flowmesh::ComputeProductionDepositAcceptanceId(changed_acceptance);
    const auto account_root{flowmesh::ComputeProductionExecutionResultRoot(
        built->result.result_commitment, changed_account)};
    BOOST_REQUIRE(account_root.has_value());
    BOOST_CHECK(*account_root != built->entry.result_root);

    auto changed_shard{built->effects};
    auto& shard_acceptance{
        std::get<modern::FlowMeshDepositAcceptanceV1>(changed_shard[0])};
    shard_acceptance.shard = (shard_acceptance.shard + 1) % 256;
    shard_acceptance.acceptance_id =
        flowmesh::ComputeProductionDepositAcceptanceId(shard_acceptance);
    const auto shard_root{flowmesh::ComputeProductionExecutionResultRoot(
        built->result.result_commitment, changed_shard)};
    BOOST_REQUIRE(shard_root.has_value());
    BOOST_CHECK(*shard_root != built->entry.result_root);

    auto redirected{built->effects};
    std::get<modern::FlowMeshWithdrawalReceiptV1>(redirected[1])
        .destination_owner_commitment = Filled(0xc7);
    const auto redirected_root{
        flowmesh::ComputeProductionExecutionResultRoot(
            built->result.result_commitment, redirected)};
    BOOST_REQUIRE(redirected_root.has_value());
    BOOST_CHECK(*redirected_root != built->entry.result_root);

    auto forged_entry{built->entry};
    forged_entry.result_root = *redirected_root;
    BOOST_CHECK(!flowmesh::ExecuteProductionEntry(
        previous, forged_entry, market.domain, market.market, seats.seats,
        gate, 0, 0, uint256{}, anchor_context, market.treasury, &deposits,
        check));
    BOOST_CHECK(check == flowmesh::ProductionEntryCheck::WRONG_RESULT_ROOT);
}

BOOST_AUTO_TEST_CASE(anchor_derived_withdrawal_settlement_is_dedicated_and_exact)
{
    MarketFixture market;
    const SeatFixture seats{
        Seats(market.domain, market.market, 4, 9, 100, Filled(0xd1), 71)};
    flowmesh::FlowMeshState initial{
        market.vault, market.base, modern::NativeAsset(),
        flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
    BOOST_REQUIRE(flowmesh::test_only::StateFunding::Fund(
        initial, market.seller, market.base, 100));
    flowmesh::ProductionEpochGate gate{market.domain, market.market,
                                       seats.seats};
    TestAnchorPolicy anchors{300};
    const flowmesh::AnchorRef set_anchor{100, Filled(0xd1)};
    const flowmesh::AnchorRef prior_anchor{190, Filled(0xd2)};
    const flowmesh::AnchorRef request_anchor{200, Filled(0xd3)};
    const flowmesh::AnchorRef settlement_anchor{210, Filled(0xd4)};
    anchors.Add(set_anchor);
    anchors.Add(prior_anchor);
    anchors.Add(request_anchor);
    anchors.Add(settlement_anchor);

    MapDeposits chain_facts;
    flowmesh::ProductionEntryCheck check;
    const std::vector<flowmesh::Action> request_actions{
        Withdraw(market.seller, 0, market.base, 40, Filled(0xd5))};
    const flowmesh::ProductionAnchorContext request_context{
        300, prior_anchor, &anchors};
    const auto request{flowmesh::BuildProductionExecutionEntry(
        initial, market.domain, market.market, seats.seats, gate, 0, 0,
        uint256{}, request_anchor, request_context, market.treasury,
        request_actions, &chain_facts, check)};
    BOOST_REQUIRE_MESSAGE(request.has_value(),
                          flowmesh::ProductionEntryCheckName(check));
    BOOST_REQUIRE_EQUAL(request->effects.size(), 1U);
    const auto* receipt{std::get_if<modern::FlowMeshWithdrawalReceiptV1>(
        &request->effects.front())};
    BOOST_REQUIRE(receipt != nullptr);
    BOOST_CHECK_EQUAL(request->next_state.LedgerView().Custody(market.base),
                      100);
    BOOST_CHECK_EQUAL(
        request->next_state.LedgerView().Liabilities(market.base), 100);

    flowmesh::WithdrawalSettlementFactV1 settlement;
    settlement.receipt = *receipt;
    settlement.checkpoint_id = Filled(0xd6);
    settlement.transaction_id = Txid::FromUint256(Filled(0xd7));
    settlement.connected_height = 205;
    settlement.connected_block = Filled(0xd8);
    chain_facts.required_settlement_after = request_anchor;
    chain_facts.required_settlement_through = settlement_anchor;
    chain_facts.settlements = {settlement};

    const flowmesh::ProductionAnchorContext settlement_context{
        300, request_anchor, &anchors};
    const std::vector<flowmesh::Action> no_actions;
    const auto settled{flowmesh::BuildProductionExecutionEntry(
        request->next_state, market.domain, market.market, seats.seats, gate,
        1, 1, request->entry.GetHash(), settlement_anchor,
        settlement_context, market.treasury, no_actions, &chain_facts,
        check)};
    BOOST_REQUIRE_MESSAGE(settled.has_value(),
                          flowmesh::ProductionEntryCheckName(check));
    BOOST_REQUIRE_EQUAL(settled->settlements.size(), 1U);
    BOOST_REQUIRE_EQUAL(settled->result.settled_withdrawals.size(), 1U);
    BOOST_CHECK(settled->effects.empty());
    BOOST_CHECK_EQUAL(settled->entry.effect_count, 0U);
    BOOST_CHECK_EQUAL(settled->next_state.Slot(),
                      request->next_state.Slot());
    BOOST_CHECK_EQUAL(settled->next_state.LedgerView().Custody(market.base),
                      60);
    BOOST_CHECK_EQUAL(
        settled->next_state.LedgerView().Liabilities(market.base), 60);
    BOOST_CHECK(!settled->next_state.LedgerView()
                     .GetRequest(receipt->receipt_id)
                     .has_value());

    const auto replayed{flowmesh::ExecuteProductionEntry(
        request->next_state, settled->entry, market.domain, market.market,
        seats.seats, gate, 1, 1, request->entry.GetHash(),
        settlement_context, market.treasury, &chain_facts, check)};
    BOOST_REQUIRE_MESSAGE(replayed.has_value(),
                          flowmesh::ProductionEntryCheckName(check));
    BOOST_CHECK(replayed->next_state.Root() == settled->next_state.Root());
    BOOST_CHECK(replayed->settlements == settled->settlements);

    // A mandatory chain settlement cannot be mixed with optional user work.
    const std::vector<flowmesh::Action> mixed{
        Withdraw(market.seller, 1, market.base, 1, Filled(0xd9))};
    BOOST_CHECK(!flowmesh::BuildProductionExecutionEntry(
        request->next_state, market.domain, market.market, seats.seats, gate,
        1, 1, request->entry.GetHash(), settlement_anchor,
        settlement_context, market.treasury, mixed, &chain_facts, check));
    BOOST_CHECK(check ==
                flowmesh::ProductionEntryCheck::CHAIN_SETTLEMENT_MISMATCH);

    // Matching the receipt id is not enough: every value fact must agree.
    ++chain_facts.settlements.front().receipt.amount;
    BOOST_CHECK(!flowmesh::BuildProductionExecutionEntry(
        request->next_state, market.domain, market.market, seats.seats, gate,
        1, 1, request->entry.GetHash(), settlement_anchor,
        settlement_context, market.treasury, no_actions, &chain_facts,
        check));
    BOOST_CHECK(check == flowmesh::ProductionEntryCheck::EXECUTION_FAILED);
}

BOOST_AUTO_TEST_CASE(historical_bls_seat_reward_claim_survives_rebinding)
{
    BuiltFixture fixture;
    const auto reward_it{std::find_if(
        fixture.built.result.clearing.fees.seat_rewards.begin(),
        fixture.built.result.clearing.fees.seat_rewards.end(),
        [](const flowmesh::SeatFeeShare& share) { return share.reward > 0; })};
    BOOST_REQUIRE(reward_it !=
                  fixture.built.result.clearing.fees.seat_rewards.end());
    const size_t historical_index{static_cast<size_t>(std::distance(
        fixture.built.result.clearing.fees.seat_rewards.begin(), reward_it))};
    const flowmesh::ActiveFnBlsSeat& historical_member{
        fixture.seats.seats.members[historical_index]};
    const flowmesh::FlowMeshFeeSeat fee_seat{
        historical_member.seat_id,
        historical_member.key.Key().Compressed()};
    const flowmesh::AccountId reward_account{
        flowmesh::FlowMeshSeatRewardAccount(
            fixture.market.market, fixture.seats.seats.epoch, fee_seat)};
    BOOST_CHECK_EQUAL(
        fixture.built.next_state.LedgerView().Available(
            reward_account, modern::NativeAsset()),
        reward_it->reward);

    flowmesh::Action claim;
    claim.sequence = 0;
    claim.type = static_cast<uint8_t>(
        flowmesh::ActionType::CLAIM_SEAT_REWARD);
    claim.asset = modern::NativeAsset();
    claim.amount = reward_it->reward;
    claim.destination = Filled(0xd1);
    BOOST_REQUIRE(flowmesh::SignProductionSeatRewardClaim(
        fixture.seats.secrets[historical_index], fixture.market.domain,
        fixture.built.next_state.ConfigId(), fixture.market.market,
        fixture.seats.seats, historical_member, claim));
    BOOST_CHECK(claim.signer == reward_account);
    BOOST_CHECK_EQUAL(claim.credential.size(),
                      flowmesh::FLOWMESH_SEAT_REWARD_CREDENTIAL_SIZE);
    BOOST_CHECK(flowmesh::CheckProductionActionCredential(
        claim, fixture.market.domain, fixture.built.next_state.ConfigId(),
        fixture.market.market, &fixture.seats.seats, &historical_member));

    auto redirected{claim};
    redirected.destination = Filled(0xd2);
    BOOST_CHECK(!flowmesh::CheckProductionActionCredential(
        redirected, fixture.market.domain,
        fixture.built.next_state.ConfigId(), fixture.market.market,
        &fixture.seats.seats, &historical_member));
    auto wrong_asset{claim};
    wrong_asset.asset = fixture.market.base;
    BOOST_CHECK(!wrong_asset.ShapeIsCanonicalSansCredential());

    const size_t other_index{(historical_index + 1) %
                             fixture.seats.seats.Size()};
    auto cross_key{claim};
    const uint256 digest{flowmesh::ProductionSeatRewardClaimDigest(
        fixture.market.domain, fixture.built.next_state.ConfigId(),
        fixture.market.market, fixture.seats.seats, historical_member,
        cross_key)};
    const auto wrong_signature{
        fixture.seats.secrets[other_index]
            .Sign(std::span<const unsigned char>{digest.begin(), 32})
            .Compressed()};
    cross_key.credential.assign(wrong_signature.begin(),
                                wrong_signature.end());
    BOOST_CHECK(flowmesh::CheckProductionSeatRewardClaimCredential(
                    cross_key, fixture.market.domain,
                    fixture.built.next_state.ConfigId(),
                    fixture.market.market, fixture.seats.seats,
                    historical_member) ==
                flowmesh::SeatRewardClaimCredentialCheck::BAD_SIGNATURE);

    // Ordinary user actions remain on the existing BIP340 path.
    const CKey user_key{SchnorrKey(0x39)};
    flowmesh::Action ordinary{Withdraw(
        flowmesh::AccountForKey(XOnlyPubKey{user_key.GetPubKey()}), 0,
        modern::NativeAsset(), 1, Filled(0xd3))};
    BOOST_REQUIRE(flowmesh::SignAction(
        user_key, fixture.market.domain, fixture.built.next_state.ConfigId(),
        ordinary));
    BOOST_CHECK(flowmesh::CheckProductionActionCredential(
        ordinary, fixture.market.domain, fixture.built.next_state.ConfigId(),
        fixture.market.market));

    // The live set has moved on and contains different FN outpoints/keys.
    // The old credential still validates against immutable epoch-7 history,
    // then executes under the current epoch-8 committee.
    const flowmesh::AnchorRef current_set_anchor{220, Filled(0xd4)};
    const SeatFixture current{Seats(
        fixture.market.domain, fixture.market.market, 4, 8,
        current_set_anchor.height, current_set_anchor.hash, 91)};
    BOOST_CHECK(!flowmesh::CheckProductionActionCredential(
        claim, fixture.market.domain, fixture.built.next_state.ConfigId(),
        fixture.market.market, &current.seats, &current.seats.members[0]));
    BOOST_CHECK(flowmesh::CheckProductionActionCredential(
        claim, fixture.market.domain, fixture.built.next_state.ConfigId(),
        fixture.market.market, &fixture.seats.seats, &historical_member));

    flowmesh::ProductionEpochGate current_gate{
        fixture.market.domain, fixture.market.market, current.seats};
    TestAnchorPolicy anchors{300};
    const flowmesh::AnchorRef prior_anchor{200, Filled(0x74)};
    const flowmesh::AnchorRef execution_anchor{250, Filled(0xd5)};
    anchors.Add(current_set_anchor);
    anchors.Add(prior_anchor);
    anchors.Add(execution_anchor);
    const flowmesh::ProductionAnchorContext anchor_context{
        300, prior_anchor, &anchors};
    flowmesh::ProductionEntryCheck check;
    const std::vector<flowmesh::Action> actions{claim};
    MapDeposits chain_facts;
    const auto built{flowmesh::BuildProductionExecutionEntry(
        fixture.built.next_state, fixture.market.domain,
        fixture.market.market, current.seats, current_gate, 1,
        fixture.built.entry.effect_start + fixture.built.entry.effect_count,
        fixture.built.entry.GetHash(), execution_anchor, anchor_context,
        fixture.market.treasury, actions, &chain_facts, check)};
    BOOST_REQUIRE_MESSAGE(built.has_value(),
                          flowmesh::ProductionEntryCheckName(check));
    BOOST_REQUIRE_EQUAL(built->effects.size(), 1U);
    const auto* receipt{
        std::get_if<modern::FlowMeshWithdrawalReceiptV1>(&built->effects[0])};
    BOOST_REQUIRE(receipt != nullptr);
    BOOST_CHECK(receipt->account == reward_account);
    BOOST_CHECK(receipt->asset == modern::NativeAsset());
    BOOST_CHECK_EQUAL(receipt->amount, reward_it->reward);
    BOOST_CHECK(receipt->destination_owner_commitment == claim.destination);
    BOOST_CHECK_EQUAL(receipt->epoch, 8U);
    BOOST_CHECK_EQUAL(receipt->sequence, 1U);

    const auto committed{flowmesh::ComputeProductionExecutionResultRoot(
        built->result.result_commitment, built->effects)};
    BOOST_REQUIRE(committed.has_value());
    BOOST_CHECK(*committed == built->entry.result_root);
    auto redirected_effect{built->effects};
    std::get<modern::FlowMeshWithdrawalReceiptV1>(redirected_effect[0])
        .destination_owner_commitment = Filled(0xd6);
    const auto redirected_root{flowmesh::ComputeProductionExecutionResultRoot(
        built->result.result_commitment, redirected_effect)};
    BOOST_REQUIRE(redirected_root.has_value());
    BOOST_CHECK(*redirected_root != built->entry.result_root);
}

BOOST_AUTO_TEST_CASE(certified_handoff_rebinds_the_set_only_after_b3_connection)
{
    BuiltFixture fixture;
    const flowmesh::AnchorRef handoff_anchor{210, Filled(0xb1)};
    const flowmesh::AnchorRef next_anchor{220, Filled(0xb2)};
    fixture.anchors.Add(handoff_anchor);
    fixture.anchors.Add(next_anchor);
    const SeatFixture next{Seats(fixture.market.domain, fixture.market.market,
                                 4, 8, next_anchor.height, next_anchor.hash, 44)};
    BOOST_CHECK(next.seats.set_hash != fixture.seats.seats.set_hash);
    BOOST_CHECK(next.seats.members[0].outpoint != fixture.seats.seats.members[0].outpoint);

    flowmesh::ProductionAnchorContext handoff_context{
        260, flowmesh::AnchorRef{200, Filled(0x74)}, &fixture.anchors};
    flowmesh::ProductionEntryCheck check;
    const auto handoff{flowmesh::BuildProductionHandoffEntry(
        fixture.built.next_state, fixture.market.domain, fixture.market.market,
        fixture.seats.seats, next.seats, 1,
        fixture.built.entry.effect_start + fixture.built.entry.effect_count,
        fixture.built.entry.GetHash(),
        handoff_anchor, handoff_context, check)};
    BOOST_REQUIRE_MESSAGE(handoff.has_value(), flowmesh::ProductionEntryCheckName(check));
    BOOST_CHECK(handoff->actions.empty());
    BOOST_CHECK_EQUAL(handoff->next_epoch, 8U);
    BOOST_CHECK(handoff->next_anchor == next_anchor);
    BOOST_CHECK(handoff->next_seat_set_hash == next.seats.set_hash);
    const auto certificate{Certify(*handoff, fixture.seats)};

    TestAnchorPolicy next_policy{280};
    next_policy.Add(next_anchor);
    const flowmesh::AnchorRef next_execution_anchor{230, Filled(0xb3)};
    next_policy.Add(next_execution_anchor);
    flowmesh::ProductionAnchorContext next_context{280, handoff_anchor, &next_policy};
    const std::vector<flowmesh::Action> no_actions;
    const auto before_connection{flowmesh::BuildProductionExecutionEntry(
        fixture.built.next_state, fixture.market.domain, fixture.market.market,
        next.seats, fixture.gate, 2,
        handoff->effect_start + handoff->effect_count, handoff->GetHash(),
        next_execution_anchor,
        next_context, fixture.market.treasury, no_actions, nullptr, check)};
    BOOST_CHECK(!before_connection);
    BOOST_CHECK(check == flowmesh::ProductionEntryCheck::HANDOFF_NOT_CONNECTED);

    BOOST_CHECK(fixture.gate.StageHandoff(
                    fixture.built.next_state, *handoff, fixture.seats.seats,
                    next.seats, certificate, 1, handoff->effect_start,
                    fixture.built.entry.GetHash(),
                    handoff_context) == flowmesh::ProductionEntryCheck::OK);
    BOOST_CHECK(fixture.gate.Paused());
    BOOST_CHECK(!fixture.gate.CanExecute(fixture.seats.seats));
    BOOST_CHECK(!fixture.gate.CanExecute(next.seats));
    BOOST_CHECK(!fixture.gate.MarkHandoffB3Connected(Filled(0xbc)));
    BOOST_CHECK(fixture.gate.MarkHandoffB3Connected(handoff->GetHash()));
    BOOST_CHECK(fixture.gate.CanExecute(next.seats));

    const auto after_connection{flowmesh::BuildProductionExecutionEntry(
        fixture.built.next_state, fixture.market.domain, fixture.market.market,
        next.seats, fixture.gate, 2,
        handoff->effect_start + handoff->effect_count, handoff->GetHash(),
        next_execution_anchor,
        next_context, fixture.market.treasury, no_actions, nullptr, check)};
    BOOST_REQUIRE_MESSAGE(after_connection.has_value(),
                          flowmesh::ProductionEntryCheckName(check));

    flowmesh::ProductionEpochGate insufficient_gate{
        fixture.market.domain, fixture.market.market, fixture.seats.seats};
    auto three{next.seats};
    three.members.erase(three.members.begin() + 3, three.members.end());
    BOOST_CHECK(insufficient_gate.StageHandoff(
                    fixture.built.next_state, *handoff, fixture.seats.seats,
                    three, certificate, 1, handoff->effect_start,
                    fixture.built.entry.GetHash(),
                    handoff_context) ==
                flowmesh::ProductionEntryCheck::INVALID_NEXT_SEAT_SET);
    BOOST_CHECK(insufficient_gate.Paused());
    BOOST_CHECK(!insufficient_gate.CanExecute(fixture.seats.seats));
}

BOOST_AUTO_TEST_CASE(durable_lock_is_permanent_across_guards)
{
    BuiltFixture fixture;
    MemoryLockJournal journal;
    flowmesh::ProductionSigningGuard first{journal};
    BOOST_CHECK(first.Lock(fixture.built.entry) ==
                flowmesh::ProductionLockResult::LOCKED);
    BOOST_CHECK(first.Lock(fixture.built.entry) ==
                flowmesh::ProductionLockResult::ALREADY_LOCKED_SAME);

    auto conflict{fixture.built.entry};
    conflict.result_root = Filled(0xdd);
    BOOST_CHECK(first.Lock(conflict) == flowmesh::ProductionLockResult::CONFLICT);

    // A fresh guard backed by the same durable journal remains locked. There
    // is intentionally no unlock API or round-based escape hatch.
    flowmesh::ProductionSigningGuard after_restart{journal};
    BOOST_CHECK(after_restart.Lock(conflict) ==
                flowmesh::ProductionLockResult::CONFLICT);
    BOOST_CHECK(after_restart.Lock(fixture.built.entry) ==
                flowmesh::ProductionLockResult::ALREADY_LOCKED_SAME);

    MemoryLockJournal failing;
    failing.fail = true;
    flowmesh::ProductionSigningGuard fail_guard{failing};
    BOOST_CHECK(fail_guard.Lock(fixture.built.entry) ==
                flowmesh::ProductionLockResult::STORAGE_FAILURE);
}

BOOST_AUTO_TEST_SUITE_END()
