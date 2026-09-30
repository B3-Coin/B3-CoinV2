// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <flowmesh/outcome_evidence.h>

#include <flowmesh/market.h>
#include <flowmesh/microblock.h>
#include <flowmesh/production_engine.h>
#include <random.h>
#include <test/util/flowmesh.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <map>
#include <set>
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
    for (size_t i{0}; i < ikm.size(); ++i) ikm[i] = static_cast<unsigned char>(salt + index * 17 + i * 11);
    const auto key{bls::SecretKey::FromIKM(ikm)};
    BOOST_REQUIRE(key.has_value());
    return *key;
}

flowmesh::ActiveFnBlsSeatSet Seats(const uint256& domain, const flowmesh::MarketId& market)
{
    std::vector<std::pair<flowmesh::SeatId, flowmesh::BlsSeatBinding>> entries;
    for (uint32_t i{0}; i < 4; ++i) {
        const bls::SecretKey secret{Key(i, 91)};
        flowmesh::BlsSeatBinding binding;
        binding.outpoint = COutPoint{Txid::FromUint256(Filled(static_cast<unsigned char>(0x90 + i))), i};
        binding.public_key = secret.GetPublicKey().Compressed();
        binding.proof_of_possession = secret.SignPoP().Compressed();
        entries.emplace_back(flowmesh::ComputeFlowMeshSeatId(domain, binding.outpoint), binding);
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<flowmesh::BlsSeatBinding> bindings;
    for (const auto& [id, binding] : entries) bindings.push_back(binding);
    flowmesh::BlsSeatSetCheck check;
    const auto seats{flowmesh::BuildActiveFnBlsSeatSet(domain, market, 3, 100, Filled(0x71), bindings, check)};
    BOOST_REQUIRE(seats && check == flowmesh::BlsSeatSetCheck::OK);
    return *seats;
}

class Anchors final : public flowmesh::AnchorPolicy
{
public:
    std::map<int32_t, uint256> canonical;
    bool Acceptable(const flowmesh::AnchorRef& anchor) const override
    {
        return StillCanonical(anchor) && 260 - anchor.height >= flowmesh::FLOWMESH_PRODUCTION_MIN_ANCHOR_DEPTH;
    }
    bool StillCanonical(const flowmesh::AnchorRef& anchor) const override
    {
        const auto it{canonical.find(anchor.height)};
        return it != canonical.end() && it->second == anchor.hash;
    }
    flowmesh::AnchorRef Current() const override { return {260, Filled(0xfe)}; }
};

class Deposits final : public flowmesh::DepositVerifier
{
public:
    std::map<COutPoint, flowmesh::DepositInfo> entries;
    flowmesh::AnchorRef anchor;
    CAmount capacity{MAX_MONEY};
    std::optional<flowmesh::DepositInfo> GetDeposit(const COutPoint& outpoint,
                                                    const flowmesh::AnchorRef& at) const override
    {
        if (!(at == anchor)) return std::nullopt;
        const auto it{entries.find(outpoint)};
        return it == entries.end() ? std::nullopt : std::optional{it->second};
    }
    std::optional<CAmount> GetWithdrawalCapacity(const modern::AssetId&, const flowmesh::AnchorRef&) const override
    {
        return capacity;
    }
    std::optional<std::vector<flowmesh::WithdrawalSettlementFactV1>> GetWithdrawalSettlements(
        const std::optional<flowmesh::AnchorRef>&, const flowmesh::AnchorRef&) const override
    {
        return std::vector<flowmesh::WithdrawalSettlementFactV1>{};
    }
};

struct OutcomeFixture {
    const uint256 domain{Filled(0x11)};
    const modern::AssetId base{Filled(0x31)};
    const flowmesh::MarketId market{*flowmesh::ComputeFlowMeshMarketId(domain, base)};
    const flowmesh::VaultId vault{*flowmesh::ComputeFlowMeshVaultId(domain, market)};
    const uint256 treasury{Filled(0x51)};
    const flowmesh::ActiveFnBlsSeatSet seats{Seats(domain, market)};
    const flowmesh::AnchorRef entry_anchor{200, Filled(0x74)};
    Anchors anchors;
    Deposits deposits;

    OutcomeFixture()
    {
        anchors.canonical = {{100, Filled(0x71)}, {190, Filled(0x73)}, {200, Filled(0x74)}};
        deposits.anchor = entry_anchor;
    }

    flowmesh::FlowMeshState State() const
    {
        return flowmesh::FlowMeshState{vault, base, modern::NativeAsset(), flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
    }

    flowmesh::BuiltProductionExecution Build(const flowmesh::FlowMeshState& state,
                                             const std::vector<flowmesh::Action>& actions)
    {
        flowmesh::ProductionEpochGate gate{domain, market, seats};
        flowmesh::ProductionEntryCheck check;
        const flowmesh::ProductionAnchorContext context{260, flowmesh::AnchorRef{190, Filled(0x73)}, &anchors};
        const auto built{flowmesh::BuildProductionExecutionEntry(state, domain, market, seats, gate, 5, 11,
            Filled(0x05), entry_anchor, context, treasury, actions, &deposits, check)};
        BOOST_REQUIRE_MESSAGE(built, flowmesh::ProductionEntryCheckName(check));
        return *built;
    }

    static flowmesh::ExecutionOutcomeEvidenceV1 Evidence(const flowmesh::BuiltProductionExecution& built)
    {
        return {built.entry.sequence, built.entry.GetHash(),
                flowmesh::MakeExecutionOutcomePreimageV1(built.result), built.effects};
    }
};

flowmesh::AccountId Account(const unsigned char n) { return Filled(n); }

flowmesh::Action Bid(const flowmesh::AccountId& account, uint64_t sequence, CAmount price, CAmount qty)
{
    flowmesh::Action action;
    action.signer = account;
    action.sequence = sequence;
    action.type = static_cast<uint8_t>(flowmesh::ActionType::SUBMIT_BID);
    action.curve = *flowmesh::MakeLimitBidCurve(price, qty);
    return action;
}

flowmesh::Action Ask(const flowmesh::AccountId& account, uint64_t sequence, CAmount price, CAmount qty)
{
    auto action{Bid(account, sequence, 1, 1)};
    action.type = static_cast<uint8_t>(flowmesh::ActionType::SUBMIT_ASK);
    action.curve = *flowmesh::MakeLimitAskCurve(price, qty);
    return action;
}

flowmesh::Action Cancel(const flowmesh::AccountId& account, uint64_t sequence, bool bid)
{
    flowmesh::Action action;
    action.signer = account;
    action.sequence = sequence;
    action.type = static_cast<uint8_t>(bid ? flowmesh::ActionType::CANCEL_BID : flowmesh::ActionType::CANCEL_ASK);
    return action;
}

flowmesh::Action Withdraw(const flowmesh::AccountId& account, uint64_t sequence, const modern::AssetId& asset,
                          CAmount amount, const uint256& destination)
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

flowmesh::Action Deposit(const COutPoint& outpoint)
{
    flowmesh::Action action;
    action.type = static_cast<uint8_t>(flowmesh::ActionType::DEPOSIT);
    action.outpoint = outpoint;
    return action;
}

void Fund(flowmesh::FlowMeshState& state, const flowmesh::AccountId& account, const modern::AssetId& asset, CAmount amount)
{
    BOOST_REQUIRE(flowmesh::test_only::StateFunding::Fund(state, account, asset, amount));
}

const flowmesh::Action& Find(const flowmesh::BuiltProductionExecution& built, const flowmesh::Action& action)
{
    const auto it{std::find_if(built.entry.actions.begin(), built.entry.actions.end(),
                               [&](const auto& item) { return item.Id() == action.Id(); })};
    BOOST_REQUIRE(it != built.entry.actions.end());
    return *it;
}

flowmesh::DerivedActionOutcome Derive(const flowmesh::BuiltProductionExecution& built, const flowmesh::Action& action)
{
    const auto evidence{OutcomeFixture::Evidence(built)};
    BOOST_REQUIRE(flowmesh::VerifyExecutionOutcomeEvidence(built.entry, evidence) == flowmesh::OutcomeEvidenceCheck::OK);
    const auto derived{flowmesh::DeriveActionOutcome(built.entry, evidence, Find(built, action))};
    BOOST_REQUIRE(derived);
    return *derived;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(flowmesh_outcome_evidence_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(commitment_recomputation_matches_executor_randomized)
{
    OutcomeFixture f;
    FastRandomContext rng{uint256{7}};
    std::array<flowmesh::AccountId, 6> accounts;
    for (size_t i{0}; i < accounts.size(); ++i) accounts[i] = Account(static_cast<unsigned char>(0x60 + i));
    size_t cleared{0}, zero_price{0}, fees{0}, equivocations{0}, bad_sequences{0}, withdrawals{0}, deposits{0},
        state_rejections{0}, cancels{0};
    for (size_t round{0}; round < 520; ++round) {
        auto state{f.State()};
        std::map<flowmesh::AccountId, uint64_t> next;
        for (const auto& account : accounts) {
            if (rng.randbool()) Fund(state, account, modern::NativeAsset(), 1 + rng.randrange(200'000));
            if (rng.randbool()) Fund(state, account, f.base, 1 + rng.randrange(400));
            next[account] = rng.randrange(3);
            flowmesh::test_only::StateFunding::SetNextSequence(state, account, next[account]);
        }
        f.deposits.entries.clear();
        f.deposits.capacity = rng.randbool() ? MAX_MONEY : rng.randrange(200);
        std::vector<flowmesh::Action> actions;
        const size_t count{1 + rng.randrange(size_t{14})};
        for (size_t i{0}; i < count; ++i) {
            const auto& account{accounts[rng.randrange(accounts.size())]};
            uint64_t sequence{next[account]};
            if (rng.randrange(8) == 0) sequence += 1 + rng.randrange(2); // bad sequence
            const CAmount price{rng.randrange(6) == 0 ? 0 : static_cast<CAmount>(1 + rng.randrange(600))};
            const CAmount qty{static_cast<CAmount>(1 + rng.randrange(120))};
            switch (rng.randrange(7)) {
            case 0: actions.push_back(Bid(account, sequence, price, qty)); break;
            case 1: actions.push_back(Ask(account, sequence, price, qty)); break;
            case 2: actions.push_back(Cancel(account, sequence, rng.randbool())); break;
            case 3:
                actions.push_back(Withdraw(account, sequence, rng.randbool() ? f.base : modern::NativeAsset(),
                                           1 + rng.randrange(300), Filled(0xd0)));
                break;
            case 4: {
                const COutPoint outpoint{Txid::FromUint256(Filled(0xe0)), static_cast<uint32_t>(rng.randrange(1000))};
                if (rng.randbool()) f.deposits.entries[outpoint] = {f.base, 1 + static_cast<CAmount>(rng.randrange(90)), account};
                actions.push_back(Deposit(outpoint));
                continue;
            }
            default:
                // Equivocation: two different actions for one sequence.
                actions.push_back(Bid(account, sequence, price, qty));
                actions.push_back(Ask(account, sequence, price, qty));
                break;
            }
            if (sequence == next[account] && rng.randrange(3) != 0) ++next[account];
        }
        const auto built{f.Build(state, actions)};
        const auto preimage{flowmesh::MakeExecutionOutcomePreimageV1(built.result)};
        BOOST_REQUIRE(flowmesh::ComputeExecResultCommitmentV1(preimage) == built.result.result_commitment);
        const auto evidence{OutcomeFixture::Evidence(built)};
        BOOST_REQUIRE(flowmesh::VerifyExecutionOutcomeEvidence(built.entry, evidence) == flowmesh::OutcomeEvidenceCheck::OK);
        const auto encoded{flowmesh::EncodeExecutionOutcomeEvidenceV1(evidence)};
        BOOST_REQUIRE(encoded);
        const auto decoded{flowmesh::DecodeExecutionOutcomeEvidenceV1(*encoded)};
        BOOST_REQUIRE(decoded);
        BOOST_CHECK(decoded->preimage == preimage);
        BOOST_CHECK(decoded->effects == built.effects);
        BOOST_CHECK(flowmesh::VerifyExecutionOutcomeEvidence(built.entry, *decoded) == flowmesh::OutcomeEvidenceCheck::OK);
        // Every body action derives, consistently with the executor.
        for (const auto& action : built.entry.actions) {
            const auto derived{flowmesh::DeriveActionOutcome(built.entry, *decoded, action)};
            BOOST_REQUIRE(derived);
            const bool applied{std::ranges::find(built.result.applied, action.Id()) != built.result.applied.end()};
            BOOST_CHECK_EQUAL(applied, derived->status == flowmesh::ActionOutcomeStatus::APPLIED);
            if (derived->code == flowmesh::ActionReasonCode::INCLUDED_EQUIVOCATION) ++equivocations;
            if (derived->code == flowmesh::ActionReasonCode::INCLUDED_BAD_SEQUENCE) ++bad_sequences;
            if (derived->code == flowmesh::ActionReasonCode::REJECTED_BY_STATE) ++state_rejections;
            if (derived->withdrawal) ++withdrawals;
            if (derived->deposit) ++deposits;
            const auto type{static_cast<flowmesh::ActionType>(action.type)};
            if (applied && (type == flowmesh::ActionType::CANCEL_BID || type == flowmesh::ActionType::CANCEL_ASK)) ++cancels;
            if (applied && (type == flowmesh::ActionType::WITHDRAW)) BOOST_CHECK(derived->withdrawal);
            if (applied && action.IsDeposit()) BOOST_CHECK(derived->deposit);
            if (derived->fill && !derived->fill->superseded_in_entry) {
                const auto& fills{derived->fill->side == flowmesh::ClearingEngine::Side::BID
                                      ? built.result.clearing.bid_fill : built.result.clearing.ask_fill};
                const auto fill{fills.find(action.signer)};
                BOOST_CHECK_EQUAL(derived->fill->quantity, fill == fills.end() ? 0 : fill->second);
            }
        }
        cleared += built.result.clearing.cleared;
        zero_price += built.result.clearing.cleared && built.result.clearing.price == 0;
        fees += !built.result.clearing.fees.seller_fees.empty();
    }
    // The generator exercised every branch the commitment covers.
    BOOST_CHECK_GT(cleared, 20U);
    BOOST_CHECK_GT(zero_price, 0U);
    BOOST_CHECK_GT(fees, 10U);
    BOOST_CHECK_GT(equivocations, 20U);
    BOOST_CHECK_GT(bad_sequences, 20U);
    BOOST_CHECK_GT(withdrawals, 10U);
    BOOST_CHECK_GT(deposits, 10U);
    BOOST_CHECK_GT(state_rejections, 20U);
    BOOST_CHECK_GT(cancels, 5U);

    // MALFORMED never reaches a certified body; the executor still hashes it.
    auto state{f.State()};
    flowmesh::BatchExecutor executor{state, &f.deposits};
    auto malformed{Cancel(accounts[0], 0, true)};
    malformed.amount = 1;
    const auto result{executor.ExecuteSlot({malformed, Bid(accounts[1], 0, 3, 4)}, f.entry_anchor)};
    BOOST_REQUIRE(result);
    BOOST_REQUIRE(std::ranges::any_of(result->rejected, [](const auto& item) {
        return item.second == flowmesh::ActionReject::MALFORMED;
    }));
    BOOST_CHECK(flowmesh::ComputeExecResultCommitmentV1(flowmesh::MakeExecutionOutcomePreimageV1(*result)) ==
                result->result_commitment);
}

BOOST_AUTO_TEST_CASE(codec_roundtrip_is_canonical_and_strict)
{
    OutcomeFixture f;
    auto state{f.State()};
    Fund(state, Account(1), modern::NativeAsset(), 10'000);
    Fund(state, Account(2), f.base, 100);
    const COutPoint outpoint{Txid::FromUint256(Filled(0xe1)), 3};
    f.deposits.entries[outpoint] = {f.base, 50, Account(3)};
    const auto built{f.Build(state, {Bid(Account(1), 0, 10, 20), Ask(Account(2), 0, 10, 20),
                                     Withdraw(Account(1), 1, modern::NativeAsset(), 5, Filled(0xd1)), Deposit(outpoint)})};
    const auto evidence{OutcomeFixture::Evidence(built)};
    const auto encoded{flowmesh::EncodeExecutionOutcomeEvidenceV1(evidence)};
    BOOST_REQUIRE(encoded);
    BOOST_CHECK_EQUAL((*encoded)[0], flowmesh::OUTCOME_EVIDENCE_VERSION_V1);
    const auto decoded{flowmesh::DecodeExecutionOutcomeEvidenceV1(*encoded)};
    BOOST_REQUIRE(decoded);
    BOOST_CHECK(flowmesh::EncodeExecutionOutcomeEvidenceV1(*decoded) == encoded);

    // Trailing data, truncation, an unknown version and a non-boolean clear
    // flag are all refused.
    auto trailing{*encoded};
    trailing.push_back(0);
    BOOST_CHECK(!flowmesh::DecodeExecutionOutcomeEvidenceV1(trailing));
    for (size_t cut : {size_t{1}, size_t{9}, encoded->size() / 2, encoded->size() - 1}) {
        BOOST_CHECK(!flowmesh::DecodeExecutionOutcomeEvidenceV1(std::span{*encoded}.first(cut)));
    }
    auto version{*encoded};
    version[0] = 2;
    BOOST_CHECK(!flowmesh::DecodeExecutionOutcomeEvidenceV1(version));
    BOOST_CHECK(!flowmesh::DecodeExecutionOutcomeEvidenceV1({}));

    // Non-canonical map order and impossible reject reasons never encode.
    auto unsorted{evidence};
    unsorted.preimage.bid_fill = {{Filled(9), 1}, {Filled(8), 1}};
    BOOST_CHECK(!flowmesh::EncodeExecutionOutcomeEvidenceV1(unsorted));
    auto duplicate_seller{evidence};
    duplicate_seller.preimage.seller_fees = {{Filled(9), 1, 0, 1}, {Filled(9), 1, 0, 1}};
    BOOST_CHECK(!flowmesh::EncodeExecutionOutcomeEvidenceV1(duplicate_seller));
    auto unauthenticated{evidence};
    unauthenticated.preimage.rejected = {{Filled(9), flowmesh::ActionReject::UNAUTHENTICATED}};
    BOOST_CHECK(!flowmesh::EncodeExecutionOutcomeEvidenceV1(unauthenticated));
}

BOOST_AUTO_TEST_CASE(codec_refuses_oversize_counts_before_allocation)
{
    // version, sequence, hash, slot, then a count claiming 2^32 applied ids.
    std::vector<unsigned char> bytes(1 + 8 + 32 + 8, 0);
    bytes[0] = flowmesh::OUTCOME_EVIDENCE_VERSION_V1;
    for (const auto b : {0xfe, 0xff, 0xff, 0xff, 0xff}) bytes.push_back(static_cast<unsigned char>(b));
    BOOST_CHECK(!flowmesh::DecodeExecutionOutcomeEvidenceV1(bytes));
    // A count within the action bound but larger than the remaining bytes.
    bytes.resize(1 + 8 + 32 + 8);
    bytes.push_back(0xfd);
    bytes.push_back(0x00);
    bytes.push_back(0x04); // 1024 applied ids, none present
    BOOST_CHECK(!flowmesh::DecodeExecutionOutcomeEvidenceV1(bytes));
    // More than the byte bound is refused before decoding starts.
    std::vector<unsigned char> huge(flowmesh::FLOWMESH_OUTCOME_EVIDENCE_MAX_BYTES + 1, 0);
    huge[0] = flowmesh::OUTCOME_EVIDENCE_VERSION_V1;
    BOOST_CHECK(!flowmesh::DecodeExecutionOutcomeEvidenceV1(huge));
    // An encoder never produces more than the bound.
    flowmesh::ExecutionOutcomePreimageV1 wide;
    for (uint32_t i{0}; i < 30'000; ++i) {
        uint256 account;
        account.data()[0] = static_cast<unsigned char>(i >> 8);
        account.data()[1] = static_cast<unsigned char>(i);
        account.data()[2] = static_cast<unsigned char>(i >> 16);
        wide.bid_fill.emplace_back(account, 1);
    }
    std::ranges::sort(wide.bid_fill);
    BOOST_CHECK(!flowmesh::EncodeExecutionOutcomeEvidenceV1(0, uint256{}, wide, {}));
}

BOOST_AUTO_TEST_CASE(tampered_fill_fee_order_or_effect_fails_verification)
{
    OutcomeFixture f;
    auto state{f.State()};
    Fund(state, Account(1), modern::NativeAsset(), 10'000);
    Fund(state, Account(2), f.base, 100);
    Fund(state, Account(4), f.base, 100);
    const auto built{f.Build(state, {Bid(Account(1), 0, 10, 30), Ask(Account(2), 0, 10, 20), Ask(Account(4), 0, 10, 20),
                                     Withdraw(Account(4), 1, f.base, 5, Filled(0xd1))})};
    const auto good{OutcomeFixture::Evidence(built)};
    BOOST_REQUIRE(flowmesh::VerifyExecutionOutcomeEvidence(built.entry, good) == flowmesh::OutcomeEvidenceCheck::OK);
    BOOST_REQUIRE(!good.preimage.seller_fees.empty());
    BOOST_REQUIRE(good.preimage.applied.size() >= 2);
    const auto check = [&](const flowmesh::ExecutionOutcomeEvidenceV1& ev) {
        return flowmesh::VerifyExecutionOutcomeEvidence(built.entry, ev);
    };
    auto fill{good};
    ++fill.preimage.bid_fill.front().second;
    BOOST_CHECK(check(fill) == flowmesh::OutcomeEvidenceCheck::RESULT_ROOT_MISMATCH);
    auto fee{good};
    ++fee.preimage.seller_fees.front().fee;
    BOOST_CHECK(check(fee) == flowmesh::OutcomeEvidenceCheck::RESULT_ROOT_MISMATCH);
    auto order{good};
    std::swap(order.preimage.applied[0], order.preimage.applied[1]);
    BOOST_CHECK(check(order) == flowmesh::OutcomeEvidenceCheck::RESULT_ROOT_MISMATCH);
    auto price{good};
    ++price.preimage.price;
    BOOST_CHECK(check(price) == flowmesh::OutcomeEvidenceCheck::RESULT_ROOT_MISMATCH);
    BOOST_REQUIRE(!good.effects.empty());
    auto effect{good};
    std::get<modern::FlowMeshWithdrawalReceiptV1>(effect.effects.front()).amount += 1;
    BOOST_CHECK(check(effect) == flowmesh::OutcomeEvidenceCheck::RESULT_ROOT_MISMATCH);
    auto dropped{good};
    dropped.effects.clear();
    BOOST_CHECK(check(dropped) == flowmesh::OutcomeEvidenceCheck::RESULT_ROOT_MISMATCH);
    auto wrong_hash{good};
    wrong_hash.entry_hash = Filled(1);
    BOOST_CHECK(check(wrong_hash) == flowmesh::OutcomeEvidenceCheck::WRONG_ENTRY);
    auto wrong_sequence{good};
    ++wrong_sequence.entry_sequence;
    BOOST_CHECK(check(wrong_sequence) == flowmesh::OutcomeEvidenceCheck::WRONG_ENTRY);
    auto handoff{built.entry};
    handoff.kind = static_cast<uint8_t>(flowmesh::ProductionEntryKind::EPOCH_HANDOFF);
    BOOST_CHECK(flowmesh::VerifyExecutionOutcomeEvidence(handoff, good) == flowmesh::OutcomeEvidenceCheck::WRONG_ENTRY);
}

BOOST_AUTO_TEST_CASE(every_body_action_must_be_disposed_exactly_once)
{
    OutcomeFixture f;
    auto state{f.State()};
    Fund(state, Account(1), modern::NativeAsset(), 10'000);
    const auto built{f.Build(state, {Bid(Account(1), 0, 10, 3), Cancel(Account(2), 0, true)})};
    auto evidence{OutcomeFixture::Evidence(built)};
    BOOST_REQUIRE(flowmesh::VerifyExecutionOutcomeEvidence(built.entry, evidence) == flowmesh::OutcomeEvidenceCheck::OK);
    // A consistent preimage whose disposition set differs from the body is
    // refused even when an entry is forged to carry its root.
    auto forged_entry{built.entry};
    auto extra{evidence};
    extra.preimage.applied.push_back(Filled(0x77));
    forged_entry.result_root = *flowmesh::ComputeProductionExecutionResultRoot(
        flowmesh::ComputeExecResultCommitmentV1(extra.preimage), extra.effects);
    extra.entry_hash = forged_entry.GetHash();
    BOOST_CHECK(flowmesh::VerifyExecutionOutcomeEvidence(forged_entry, extra) == flowmesh::OutcomeEvidenceCheck::BODY_MISMATCH);
    auto twice{evidence};
    twice.preimage.applied.push_back(twice.preimage.applied.front());
    forged_entry = built.entry;
    forged_entry.result_root = *flowmesh::ComputeProductionExecutionResultRoot(
        flowmesh::ComputeExecResultCommitmentV1(twice.preimage), twice.effects);
    twice.entry_hash = forged_entry.GetHash();
    BOOST_CHECK(flowmesh::VerifyExecutionOutcomeEvidence(forged_entry, twice) == flowmesh::OutcomeEvidenceCheck::BODY_MISMATCH);
    auto missing{evidence};
    missing.preimage.rejected.clear();
    forged_entry = built.entry;
    forged_entry.result_root = *flowmesh::ComputeProductionExecutionResultRoot(
        flowmesh::ComputeExecResultCommitmentV1(missing.preimage), missing.effects);
    missing.entry_hash = forged_entry.GetHash();
    BOOST_CHECK(flowmesh::VerifyExecutionOutcomeEvidence(forged_entry, missing) == flowmesh::OutcomeEvidenceCheck::BODY_MISMATCH);
    // An action outside the body never derives.
    BOOST_CHECK(!flowmesh::DeriveActionOutcome(built.entry, evidence, Cancel(Account(3), 0, true)));
}

BOOST_AUTO_TEST_CASE(submit_outcome_reports_fill_price_and_seller_fee)
{
    OutcomeFixture f;
    auto state{f.State()};
    Fund(state, Account(1), modern::NativeAsset(), 100'000);
    Fund(state, Account(2), f.base, 100);
    const auto bid{Bid(Account(1), 0, 5000, 10)};
    const auto ask{Ask(Account(2), 0, 5000, 6)};
    const auto built{f.Build(state, {bid, ask})};
    BOOST_REQUIRE(built.result.clearing.cleared);
    const auto buyer{Derive(built, bid)};
    BOOST_CHECK(buyer.status == flowmesh::ActionOutcomeStatus::APPLIED);
    BOOST_CHECK(buyer.final);
    BOOST_CHECK(buyer.code == flowmesh::ActionReasonCode::APPLIED);
    BOOST_REQUIRE(buyer.fill);
    BOOST_CHECK(buyer.fill->side == flowmesh::ClearingEngine::Side::BID);
    BOOST_CHECK_EQUAL(buyer.fill->quantity, 6);
    BOOST_CHECK_EQUAL(buyer.fill->price, built.result.clearing.price);
    BOOST_CHECK_EQUAL(buyer.fill->quote_atoms, 6 * built.result.clearing.price);
    BOOST_CHECK_EQUAL(buyer.fill->fee_atoms, 0);
    const auto seller{Derive(built, ask)};
    BOOST_REQUIRE(seller.fill);
    BOOST_CHECK(seller.fill->side == flowmesh::ClearingEngine::Side::ASK);
    BOOST_CHECK_EQUAL(seller.fill->quantity, 6);
    BOOST_REQUIRE_EQUAL(built.result.clearing.fees.seller_fees.size(), 1U);
    const auto& share{built.result.clearing.fees.seller_fees.front()};
    BOOST_CHECK_EQUAL(seller.fill->quote_atoms, share.gross_quote_proceeds);
    BOOST_CHECK_EQUAL(seller.fill->fee_atoms, share.fee);
    BOOST_CHECK_EQUAL(seller.fill->net_quote_atoms, share.net_quote_proceeds);
    BOOST_CHECK_GT(share.fee, 0);

    // An unmatched order rests: applied with a zero fill.
    auto quiet{f.State()};
    Fund(quiet, Account(1), modern::NativeAsset(), 100'000);
    const auto resting{Bid(Account(1), 0, 5, 10)};
    const auto rest{Derive(f.Build(quiet, {resting}), resting)};
    BOOST_REQUIRE(rest.fill);
    BOOST_CHECK_EQUAL(rest.fill->quantity, 0);
    BOOST_CHECK(!rest.fill->superseded_in_entry);
}

BOOST_AUTO_TEST_CASE(submit_superseded_or_cancelled_in_same_entry_reports_no_fill)
{
    OutcomeFixture f;
    auto state{f.State()};
    Fund(state, Account(1), modern::NativeAsset(), 100'000);
    Fund(state, Account(2), f.base, 100);
    const auto first{Bid(Account(1), 0, 50, 10)};
    const auto second{Bid(Account(1), 1, 50, 4)};
    const auto ask_first{Ask(Account(2), 0, 50, 6)};
    const auto ask_cancel{Cancel(Account(2), 1, false)};
    const auto built{f.Build(state, {first, second, ask_first, ask_cancel})};
    const auto replaced{Derive(built, first)};
    BOOST_CHECK(replaced.status == flowmesh::ActionOutcomeStatus::APPLIED);
    BOOST_REQUIRE(replaced.fill);
    BOOST_CHECK(replaced.fill->superseded_in_entry);
    BOOST_CHECK_EQUAL(replaced.fill->quantity, 0);
    const auto live{Derive(built, second)};
    BOOST_REQUIRE(live.fill);
    BOOST_CHECK(!live.fill->superseded_in_entry);
    const auto cancelled{Derive(built, ask_first)};
    BOOST_REQUIRE(cancelled.fill);
    BOOST_CHECK(cancelled.fill->superseded_in_entry);
    const auto cancel{Derive(built, ask_cancel)};
    BOOST_CHECK(cancel.status == flowmesh::ActionOutcomeStatus::APPLIED);
    BOOST_CHECK(!cancel.fill);
}

BOOST_AUTO_TEST_CASE(cancel_withdraw_and_deposit_outcomes)
{
    OutcomeFixture f;
    auto state{f.State()};
    Fund(state, Account(1), f.base, 100);
    Fund(state, Account(1), modern::NativeAsset(), 100);
    BOOST_REQUIRE(state.SubmitCurve(Account(1), flowmesh::ClearingEngine::Side::ASK, *flowmesh::MakeLimitAskCurve(90, 5)));
    const COutPoint credited{Txid::FromUint256(Filled(0xe2)), 1};
    const COutPoint unknown{Txid::FromUint256(Filled(0xe2)), 2};
    f.deposits.entries[credited] = {f.base, 40, Account(5)};
    const auto cancel{Cancel(Account(1), 0, false)};
    const auto first{Withdraw(Account(1), 1, f.base, 10, Filled(0xd1))};
    const auto second{Withdraw(Account(1), 2, modern::NativeAsset(), 20, Filled(0xd2))};
    const auto deposit{Deposit(credited)};
    const auto missing{Deposit(unknown)};
    const auto built{f.Build(state, {cancel, first, second, deposit, missing})};
    const auto cancelled{Derive(built, cancel)};
    BOOST_CHECK(cancelled.status == flowmesh::ActionOutcomeStatus::APPLIED && cancelled.final);
    const auto w1{Derive(built, first)};
    BOOST_REQUIRE(w1.withdrawal);
    BOOST_CHECK_EQUAL(w1.withdrawal->amount, 10);
    BOOST_CHECK(w1.withdrawal->asset == f.base);
    BOOST_CHECK(w1.withdrawal->destination_owner_commitment == Filled(0xd1));
    const auto w2{Derive(built, second)};
    BOOST_REQUIRE(w2.withdrawal);
    BOOST_CHECK_EQUAL(w2.withdrawal->amount, 20);
    BOOST_CHECK(w2.withdrawal->receipt_id != w1.withdrawal->receipt_id);
    const auto d{Derive(built, deposit)};
    BOOST_CHECK(d.status == flowmesh::ActionOutcomeStatus::APPLIED && d.final);
    BOOST_REQUIRE(d.deposit);
    BOOST_CHECK(d.deposit->deposit_outpoint == credited);
    BOOST_CHECK_EQUAL(d.deposit->amount, 40);
    const auto m{Derive(built, missing)};
    BOOST_CHECK(m.status == flowmesh::ActionOutcomeStatus::REJECTED);
    BOOST_CHECK(!m.final);
    BOOST_CHECK(m.code == flowmesh::ActionReasonCode::DEPOSIT_NOT_CREDITED);
}

BOOST_AUTO_TEST_CASE(bad_sequence_and_equivocation_are_included_not_final)
{
    OutcomeFixture f;
    auto state{f.State()};
    const auto early{Cancel(Account(1), 3, true)};
    const auto eq_a{Cancel(Account(2), 0, true)};
    const auto eq_b{Cancel(Account(2), 0, false)};
    const auto built{f.Build(state, {early, eq_a, eq_b})};
    const auto bad{Derive(built, early)};
    BOOST_CHECK(bad.status == flowmesh::ActionOutcomeStatus::INCLUDED_NOT_EXECUTED);
    BOOST_CHECK(!bad.final);
    BOOST_CHECK(bad.code == flowmesh::ActionReasonCode::INCLUDED_BAD_SEQUENCE);
    for (const auto& action : {eq_a, eq_b}) {
        const auto eq{Derive(built, action)};
        BOOST_CHECK(eq.status == flowmesh::ActionOutcomeStatus::INCLUDED_NOT_EXECUTED);
        BOOST_CHECK(!eq.final);
        BOOST_CHECK(eq.code == flowmesh::ActionReasonCode::INCLUDED_EQUIVOCATION);
    }
}

BOOST_AUTO_TEST_CASE(rejected_by_state_is_final)
{
    OutcomeFixture f;
    auto state{f.State()};
    const auto broke{Bid(Account(1), 0, 10, 10)};
    const auto nothing{Cancel(Account(2), 0, false)};
    const auto overdraw{Withdraw(Account(3), 0, f.base, 10, Filled(0xd1))};
    const auto built{f.Build(state, {broke, nothing, overdraw})};
    for (const auto& action : {broke, nothing, overdraw}) {
        const auto outcome{Derive(built, action)};
        BOOST_CHECK(outcome.status == flowmesh::ActionOutcomeStatus::REJECTED);
        BOOST_CHECK(outcome.final);
        BOOST_CHECK(outcome.code == flowmesh::ActionReasonCode::REJECTED_BY_STATE);
        BOOST_CHECK(!outcome.fill && !outcome.withdrawal);
    }
}

BOOST_AUTO_TEST_CASE(consumer_proof_requires_other_action_same_signer_sequence_and_consumption)
{
    OutcomeFixture f;
    auto state{f.State()};
    Fund(state, Account(1), modern::NativeAsset(), 10'000);
    const auto consumer{Bid(Account(1), 0, 5, 5)};        // applied
    const auto rejected{Cancel(Account(2), 0, true)};     // rejected by state: consumes too
    const auto bad{Cancel(Account(3), 4, true)};          // bad sequence: consumes nothing
    const auto built{f.Build(state, {consumer, rejected, bad})};
    const auto evidence{OutcomeFixture::Evidence(built)};
    BOOST_REQUIRE(flowmesh::VerifyExecutionOutcomeEvidence(built.entry, evidence) == flowmesh::OutcomeEvidenceCheck::OK);
    const auto loser{Ask(Account(1), 0, 7, 1)};           // same signer and sequence, never executes
    BOOST_CHECK(flowmesh::ProvesSequenceConsumedByOther(built.entry, evidence, loser, consumer.Id()));
    BOOST_CHECK(flowmesh::ProvesSequenceConsumedByOther(built.entry, evidence, Cancel(Account(2), 0, false), rejected.Id()));
    // The action itself, another sequence, another signer, a non-consuming
    // consumer or an id outside the body prove nothing.
    BOOST_CHECK(!flowmesh::ProvesSequenceConsumedByOther(built.entry, evidence, consumer, consumer.Id()));
    BOOST_CHECK(!flowmesh::ProvesSequenceConsumedByOther(built.entry, evidence, Ask(Account(1), 1, 7, 1), consumer.Id()));
    BOOST_CHECK(!flowmesh::ProvesSequenceConsumedByOther(built.entry, evidence, Ask(Account(4), 0, 7, 1), consumer.Id()));
    BOOST_CHECK(!flowmesh::ProvesSequenceConsumedByOther(built.entry, evidence, Cancel(Account(3), 4, false), bad.Id()));
    BOOST_CHECK(!flowmesh::ProvesSequenceConsumedByOther(built.entry, evidence, loser, Filled(0x99)));
    BOOST_CHECK(!flowmesh::ProvesSequenceConsumedByOther(built.entry, evidence, Deposit(COutPoint{}), consumer.Id()));
}

BOOST_AUTO_TEST_CASE(consumer_proof_fails_for_equivocating_pair)
{
    OutcomeFixture f;
    auto state{f.State()};
    const auto a{Cancel(Account(2), 0, true)};
    const auto b{Cancel(Account(2), 0, false)};
    const auto built{f.Build(state, {a, b})};
    const auto evidence{OutcomeFixture::Evidence(built)};
    BOOST_REQUIRE(flowmesh::VerifyExecutionOutcomeEvidence(built.entry, evidence) == flowmesh::OutcomeEvidenceCheck::OK);
    // Both are in the same entry: EQUIVOCATION consumed nothing.
    BOOST_CHECK(!flowmesh::ProvesSequenceConsumedByOther(built.entry, evidence, a, b.Id()));
    BOOST_CHECK(!flowmesh::ProvesSequenceConsumedByOther(built.entry, evidence, b, a.Id()));
}

BOOST_AUTO_TEST_SUITE_END()
