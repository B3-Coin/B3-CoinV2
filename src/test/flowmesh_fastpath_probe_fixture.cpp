// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <test/flowmesh_fastpath_probe_fixture.h>

#include <flowmesh/auth.h>
#include <flowmesh/microblock.h>
#include <streams.h>
#include <test/util/flowmesh.h>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <utility>

namespace fastprobe {
namespace {

constexpr size_t MAX_REQUEST_BYTES{4096};
constexpr size_t MAX_RECEIPT_BYTES{4096};
constexpr CAmount BUYER_QUOTE{MAX_REQUESTS * TRADE_PRICE};
constexpr CAmount FEE{10};
constexpr CAmount TREASURY_FEE{2};
constexpr CAmount SEAT_REWARD{2};

void Require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error("fastprobe: " + message);
}

uint256 Identity(const std::string& label, uint64_t index = 0)
{
    HashWriter writer;
    writer << std::string{"B3/TEST-ONLY/FASTPATH-PROBE/2026-09-26"} << label << index;
    return writer.GetHash();
}

CKey AccountKey(unsigned char seed)
{
    std::array<unsigned char, 32> bytes{};
    bytes.fill(seed);
    bytes.back() = 1;
    CKey key;
    key.Set(bytes.begin(), bytes.end(), true);
    Require(key.IsValid(), "invalid generated account key");
    return key;
}

template <typename T> Bytes Encode(const T& value)
{
    Bytes bytes;
    VectorWriter writer{bytes, 0};
    writer << value;
    return bytes;
}

flowmesh::Action Bid(const Fixture& fixture, uint64_t sequence)
{
    Require(sequence < MAX_REQUESTS, "request count exceeds bounded fixture funding");
    flowmesh::Action action;
    action.signer = fixture.buyer;
    action.sequence = sequence;
    action.type = static_cast<uint8_t>(flowmesh::ActionType::SUBMIT_BID);
    const auto curve{flowmesh::MakeLimitBidCurve(TRADE_PRICE, TRADE_QUANTITY)};
    Require(curve.has_value(), "invalid bid curve");
    action.curve = *curve;
    return action;
}

flowmesh::Action DecodeRequest(const Fixture& fixture,
                              std::span<const unsigned char> bytes)
{
    Require(!bytes.empty() && bytes.size() <= MAX_REQUEST_BYTES, "request byte bound");
    SpanReader reader{bytes};
    flowmesh::Action action;
    reader >> action;
    Require(reader.empty(), "trailing request bytes");
    Require(action.ShapeIsCanonical(), "noncanonical request");
    Require(action.sequence < MAX_REQUESTS, "request sequence outside fixture");
    Require(action.Id() == Bid(fixture, action.sequence).Id(), "unexpected request semantics");
    Require(flowmesh::CheckProductionActionCredential(
                action, fixture.domain, fixture.ConfigId(), fixture.market),
            "request Schnorr authentication failed");
    return action;
}

flowmesh::ProductionEntryCore DecodeEntry(std::span<const unsigned char> bytes)
{
    const auto entry{flowmesh::DecodeProductionEntry(bytes)};
    Require(entry.has_value(), "entry decode failed");
    return *entry;
}

void CheckRequestEntry(const Fixture& fixture, const flowmesh::Action& action,
                       const flowmesh::ProductionEntryCore& entry)
{
    Require(entry.version == flowmesh::FLOWMESH_PRODUCTION_ENTRY_VERSION_V1 &&
                entry.kind == static_cast<uint8_t>(flowmesh::ProductionEntryKind::EXECUTION),
            "wrong entry version/kind");
    Require(entry.domain == fixture.domain && entry.market_id == fixture.market &&
                entry.epoch == fixture.seats.epoch && entry.seat_set_hash == fixture.seats.set_hash,
            "entry context mismatch");
    Require(entry.anchor == fixture.anchor && entry.sequence == action.sequence + 1,
            "entry sequence/anchor mismatch");
    Require(entry.actions.size() == 1 && entry.actions[0].credential.empty() &&
                entry.actions[0].ShapeIsCanonicalSansCredential() &&
                entry.actions[0].Id() == action.Id(), "request not exactly bound to entry");
    Require(entry.actions_root == flowmesh::ComputeProductionActionsRoot(entry.actions),
            "entry actions root mismatch");
    Require(!entry.result_root.IsNull() && !entry.state_root.IsNull() &&
                !entry.previous_state_root.IsNull() && !entry.parent_hash.IsNull(),
            "entry missing state/result context");
}

std::vector<AccountDelta> ExpectedDeltas(const Fixture& fixture, uint64_t sequence)
{
    const CAmount done{static_cast<CAmount>(sequence)};
    const auto quote{modern::NativeAsset()};
    std::vector<AccountDelta> deltas{
        {fixture.buyer, fixture.base, done, done + 1, 0, 0},
        {fixture.buyer, quote, BUYER_QUOTE - done * TRADE_PRICE,
         BUYER_QUOTE - (done + 1) * TRADE_PRICE, 0, 0},
        {fixture.seller, fixture.base, 0, 0, MAKER_QUANTITY - done, MAKER_QUANTITY - done - 1},
        {fixture.seller, quote, done * (TRADE_PRICE - FEE),
         (done + 1) * (TRADE_PRICE - FEE), 0, 0},
    };
    const auto context{flowmesh::BuildProductionFlowMeshFeeContext(
        fixture.domain, fixture.seats, fixture.treasury)};
    Require(context.has_value(), "fee context invalid");
    deltas.push_back({flowmesh::FlowMeshTreasuryFeeAccount(*context), quote,
                      done * TREASURY_FEE, (done + 1) * TREASURY_FEE, 0, 0});
    for (const auto& seat : context->seats) {
        deltas.push_back({flowmesh::FlowMeshSeatRewardAccount(*context, seat), quote,
                          done * SEAT_REWARD, (done + 1) * SEAT_REWARD, 0, 0});
    }
    Require(deltas.size() == 9, "expected exactly four fee seats");
    return deltas;
}

Receipt CheckExecution(const Fixture& fixture, const flowmesh::Action& action,
                       const flowmesh::ProductionEntryCore& entry,
                       const flowmesh::FlowMeshState& before,
                       const flowmesh::FlowMeshState& after,
                       const flowmesh::BatchResult& result)
{
    Require(result.applied == std::vector<uint256>{action.Id()} && result.rejected.empty(),
            "action did not apply exactly once");
    const auto& clearing{result.clearing};
    Require(clearing.cleared && clearing.price == TRADE_PRICE &&
                clearing.volume == TRADE_QUANTITY &&
                clearing.bid_fill == std::map<flowmesh::AccountId, CAmount>{{fixture.buyer, 1}} &&
                clearing.ask_fill == std::map<flowmesh::AccountId, CAmount>{{fixture.seller, 1}},
            "actual matched fill missing");
    Require(clearing.fees.matched_b3_quote_notional == TRADE_PRICE &&
                clearing.fees.fee_total == FEE && clearing.fees.treasury_fee == TREASURY_FEE &&
                clearing.fees.seat_fee == 4 * SEAT_REWARD,
            "actual fee allocation mismatch");
    Require(before.Root() == entry.previous_state_root && after.Root() == entry.state_root &&
                after.NextSequence(fixture.buyer) == action.sequence + 1,
            "execution state binding mismatch");
    const auto deltas{ExpectedDeltas(fixture, action.sequence)};
    for (const auto& delta : deltas) {
        Require(before.LedgerView().Available(delta.account, delta.asset) == delta.available_before &&
                    after.LedgerView().Available(delta.account, delta.asset) == delta.available_after &&
                    before.LedgerView().Reserved(delta.account, delta.asset) == delta.reserved_before &&
                    after.LedgerView().Reserved(delta.account, delta.asset) == delta.reserved_after,
                "actual account balance/reservation delta mismatch");
    }
    for (const auto& asset : {fixture.base, modern::NativeAsset()}) {
        Require(before.LedgerView().Custody(asset) == after.LedgerView().Custody(asset),
                "trading changed synthetic custody");
    }
    Require(after.LedgerView().SolvencyHolds(), "ledger invariant failed");
    Require(after.AccountCurves(fixture.buyer).empty(), "filled buyer order remains live");
    const auto maker_curves{after.AccountCurves(fixture.seller)};
    Require(maker_curves.size() == 1 && maker_curves[0].side == flowmesh::ClearingEngine::Side::ASK &&
                maker_curves[0].remaining_quantity == MAKER_QUANTITY - static_cast<CAmount>(action.sequence + 1),
            "maker standing curve accounting mismatch");
    return {1, fixture.domain, fixture.market, fixture.ConfigId(), action.Id(),
            entry.GetHash(), entry.result_root, entry.previous_state_root, entry.state_root,
            entry.sequence, action.sequence, TRADE_PRICE, TRADE_QUANTITY, TRADE_PRICE,
            FEE, TREASURY_FEE, 4 * SEAT_REWARD, deltas};
}

class SyntheticAnchorPolicy final : public flowmesh::AnchorPolicy {
public:
    explicit SyntheticAnchorPolicy(flowmesh::AnchorRef anchor) : m_anchor{anchor} {}
    bool Acceptable(const flowmesh::AnchorRef& anchor) const override { return StillCanonical(anchor); }
    bool StillCanonical(const flowmesh::AnchorRef& anchor) const override { return anchor == m_anchor; }
    flowmesh::AnchorRef Current() const override { return m_anchor; }
private:
    flowmesh::AnchorRef m_anchor;
};

} // namespace

Fixture::Fixture()
    : domain{Identity("domain")}, base{Identity("base")},
      market{*flowmesh::ComputeFlowMeshMarketId(domain, base)},
      vault{*flowmesh::ComputeFlowMeshVaultId(domain, market)},
      treasury{Identity("treasury")}, buyer_key{AccountKey(0x31)}, seller_key{AccountKey(0x32)},
      buyer{flowmesh::AccountForKey(XOnlyPubKey{buyer_key.GetPubKey()})},
      seller{flowmesh::AccountForKey(XOnlyPubKey{seller_key.GetPubKey()})},
      anchor{100, Identity("synthetic-anchor")}
{
    struct Seat { bls::SecretKey key; flowmesh::BlsSeatBinding binding; flowmesh::SeatId id; };
    std::vector<Seat> generated;
    for (uint32_t i{0}; i < 4; ++i) {
        const auto seed{Identity("TEST-ONLY-BLS-IKM", i)};
        const auto key{bls::SecretKey::FromIKM(std::span<const unsigned char>{seed.begin(), 32})};
        Require(key.has_value(), "BLS generation failed");
        flowmesh::BlsSeatBinding binding;
        binding.outpoint = {Txid::FromUint256(Identity("synthetic-seat-outpoint", i)), i};
        binding.public_key = key->GetPublicKey().Compressed();
        binding.proof_of_possession = key->SignPoP().Compressed();
        generated.push_back({*key, binding, flowmesh::ComputeFlowMeshSeatId(domain, binding.outpoint)});
    }
    std::sort(generated.begin(), generated.end(), [](const Seat& a, const Seat& b) {
        return a.id < b.id || (a.id == b.id && a.binding.outpoint < b.binding.outpoint);
    });
    std::vector<flowmesh::BlsSeatBinding> bindings;
    for (const auto& seat : generated) {
        seat_keys.push_back(seat.key);
        bindings.push_back(seat.binding);
    }
    flowmesh::BlsSeatSetCheck check;
    const auto set{flowmesh::BuildActiveFnBlsSeatSet(
        domain, market, 0, anchor.height, anchor.hash, bindings, check)};
    Require(set.has_value() && check == flowmesh::BlsSeatSetCheck::OK, "seat set construction failed");
    seats = *set;
}

uint256 Fixture::ConfigId() const
{
    return flowmesh::ComputeExecutionConfigId(vault, base, modern::NativeAsset(),
                                             flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS);
}

struct Engine::Impl {
    Fixture fixture;
    flowmesh::FlowMeshState state;
    flowmesh::ProductionEpochGate gate;
    SyntheticAnchorPolicy anchors;
    uint64_t next_sequence{0};
    uint64_t next_effect{0};
    uint256 head;

    flowmesh::ProductionAnchorContext Context() const
    {
        return {fixture.anchor.height + flowmesh::FLOWMESH_PRODUCTION_MIN_ANCHOR_DEPTH,
                fixture.anchor, &anchors};
    }

    explicit Impl(const Fixture& supplied)
        : fixture{supplied}, state{fixture.vault, fixture.base, modern::NativeAsset(),
                                  flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS},
          gate{fixture.domain, fixture.market, fixture.seats}, anchors{fixture.anchor}
    {
        Require(flowmesh::test_only::StateFunding::Fund(state, fixture.buyer,
                    modern::NativeAsset(), BUYER_QUOTE) &&
                    flowmesh::test_only::StateFunding::Fund(state, fixture.seller,
                    fixture.base, MAKER_QUANTITY), "synthetic test funding failed");
        flowmesh::Action ask;
        ask.signer = fixture.seller;
        ask.type = static_cast<uint8_t>(flowmesh::ActionType::SUBMIT_ASK);
        ask.curve = *flowmesh::MakeLimitAskCurve(TRADE_PRICE, MAKER_QUANTITY);
        Require(flowmesh::SignAction(fixture.seller_key, fixture.domain, state.ConfigId(), ask) &&
                    flowmesh::CheckProductionActionCredential(ask, fixture.domain, state.ConfigId(), fixture.market),
                "maker setup Schnorr authentication failed");
        flowmesh::ProductionEntryCheck check;
        const std::array actions{ask};
        const auto built{flowmesh::BuildProductionExecutionEntry(
            state, fixture.domain, fixture.market, fixture.seats, gate, 0, 0, {},
            fixture.anchor, Context(), fixture.treasury, actions, nullptr, check)};
        Require(built.has_value(), std::string{"maker setup: "} + flowmesh::ProductionEntryCheckName(check));
        Require(built->result.applied == std::vector<uint256>{ask.Id()} && built->result.rejected.empty() &&
                    built->result.clearing.volume == 0, "maker setup was not a standing ask");
        state = built->next_state;
        head = built->entry.GetHash();
        next_sequence = 1;
        next_effect = built->entry.effect_start + built->entry.effect_count;
        Require(state.LedgerView().Reserved(fixture.seller, fixture.base) == MAKER_QUANTITY,
                "maker setup reservation missing");
    }
};

Engine::Engine(const Fixture& fixture) : m_impl{std::make_unique<Impl>(fixture)} {}
Engine::~Engine() = default;
Engine::Engine(Engine&&) noexcept = default;
Engine& Engine::operator=(Engine&&) noexcept = default;

Bytes MakeRequest(const Fixture& fixture, uint64_t buyer_sequence)
{
    auto action{Bid(fixture, buyer_sequence)};
    Require(flowmesh::SignAction(fixture.buyer_key, fixture.domain,
                                fixture.ConfigId(), action), "buyer signing failed");
    return Encode(action);
}

Bytes Engine::MakeRequest(uint64_t buyer_sequence) const
{
    return fastprobe::MakeRequest(m_impl->fixture, buyer_sequence);
}

Execution Engine::Execute(std::span<const unsigned char> request_bytes,
                          std::optional<std::span<const unsigned char>> entry_bytes) const
{
    const auto& f{m_impl->fixture};
    const auto action{DecodeRequest(f, request_bytes)};
    Require(action.sequence == m_impl->state.NextSequence(f.buyer), "stale/out-of-order buyer request");
    flowmesh::ProductionEntryCore entry;
    std::optional<flowmesh::ExecutedProductionEntry> executed;
    flowmesh::ProductionEntryCheck check;
    if (entry_bytes) {
        entry = DecodeEntry(*entry_bytes);
        CheckRequestEntry(f, action, entry);
        executed = flowmesh::ExecuteProductionEntry(
            m_impl->state, entry, f.domain, f.market, f.seats, m_impl->gate,
            m_impl->next_sequence, m_impl->next_effect, m_impl->head,
            m_impl->Context(), f.treasury, nullptr, check);
    } else {
        const std::array actions{action};
        auto built{flowmesh::BuildProductionExecutionEntry(
            m_impl->state, f.domain, f.market, f.seats, m_impl->gate,
            m_impl->next_sequence, m_impl->next_effect, m_impl->head, f.anchor,
            m_impl->Context(), f.treasury, actions, nullptr, check)};
        Require(built.has_value(), std::string{"leader execution: "} + flowmesh::ProductionEntryCheckName(check));
        entry = std::move(built->entry);
        executed.emplace(flowmesh::ExecutedProductionEntry{std::move(built->next_state),
                         std::move(built->result), std::move(built->effects), std::move(built->settlements)});
    }
    Require(executed.has_value(), std::string{"follower execution: "} + flowmesh::ProductionEntryCheckName(check));
    CheckRequestEntry(f, action, entry);
    auto receipt{CheckExecution(f, action, entry, m_impl->state, executed->next_state, executed->result)};
    const auto encoded{flowmesh::EncodeProductionEntry(entry)};
    Require(encoded.has_value(), "executed entry encoding failed");
    return {Bytes{request_bytes.begin(), request_bytes.end()}, *encoded, Encode(receipt),
            entry.GetHash(), entry.result_root, entry.state_root, entry.parent_hash,
            entry.previous_state_root, entry.sequence, entry.effect_start + entry.effect_count,
            std::move(executed->next_state), std::move(receipt)};
}

void Engine::Apply(Execution&& execution)
{
    Require(execution.entry_sequence == m_impl->next_sequence &&
                execution.parent_hash == m_impl->head &&
                execution.previous_state_root == m_impl->state.Root() &&
                execution.next_state.Root() == execution.state_root &&
                execution.next_state.ConfigId() == m_impl->state.ConfigId(),
            "stale or inconsistent verified execution");
    m_impl->state = std::move(execution.next_state);
    m_impl->head = execution.entry_hash;
    m_impl->next_effect = execution.next_effect_start;
    ++m_impl->next_sequence;
}

const flowmesh::FlowMeshState& Engine::State() const { return m_impl->state; }
uint64_t Engine::NextEntrySequence() const { return m_impl->next_sequence; }
uint256 Engine::Head() const { return m_impl->head; }

Receipt ValidateReceipt(const Fixture& fixture, std::span<const unsigned char> request_bytes,
                        std::span<const unsigned char> entry_bytes,
                        std::span<const unsigned char> receipt_bytes)
{
    const auto action{DecodeRequest(fixture, request_bytes)};
    const auto entry{DecodeEntry(entry_bytes)};
    CheckRequestEntry(fixture, action, entry);
    Require(!receipt_bytes.empty() && receipt_bytes.size() <= MAX_RECEIPT_BYTES, "receipt byte bound");
    SpanReader reader{receipt_bytes};
    Receipt receipt;
    reader >> receipt;
    Require(reader.empty(), "trailing receipt bytes");
    Require(receipt.version == 1 && receipt.domain == fixture.domain && receipt.market == fixture.market &&
                receipt.config_id == fixture.ConfigId() && receipt.action_id == action.Id() &&
                receipt.entry_hash == entry.GetHash() && receipt.result_root == entry.result_root &&
                receipt.previous_state_root == entry.previous_state_root && receipt.state_root == entry.state_root &&
                receipt.entry_sequence == entry.sequence && receipt.action_sequence == action.sequence,
            "receipt request/entry binding mismatch");
    Require(receipt.price == TRADE_PRICE && receipt.quantity == TRADE_QUANTITY &&
                receipt.quote_notional == TRADE_PRICE && receipt.fee_total == FEE &&
                receipt.treasury_fee == TREASURY_FEE && receipt.seat_fee == 4 * SEAT_REWARD &&
                receipt.deltas == ExpectedDeltas(fixture, action.sequence),
            "receipt fill/account arithmetic mismatch");
    return receipt;
}

} // namespace fastprobe
