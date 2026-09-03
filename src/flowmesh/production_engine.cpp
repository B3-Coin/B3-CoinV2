// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <flowmesh/production_engine.h>

#include <crypto/common.h>
#include <flowmesh/auth.h>
#include <hash.h>
#include <streams.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace flowmesh {
namespace {

bool SameWithdrawalRequest(const modern::WithdrawalReceipt& a,
                           const modern::WithdrawalReceipt& b)
{
    return a.receipt_id == b.receipt_id && a.asset == b.asset &&
           a.amount == b.amount && a.destination == b.destination &&
           a.finalized_slot == b.finalized_slot &&
           a.vault_commitment == b.vault_commitment;
}

bool SameHistoricalMember(const ActiveFnBlsSeat& a,
                          const ActiveFnBlsSeat& b)
{
    return a.seat_id == b.seat_id && a.outpoint == b.outpoint &&
           a.key.Key() == b.key.Key();
}

ProductionEntryCheck CheckBasicShape(const ProductionEntryCore& entry)
{
    if (entry.version != FLOWMESH_PRODUCTION_ENTRY_VERSION_V1 ||
        (entry.kind != static_cast<uint8_t>(ProductionEntryKind::EXECUTION) &&
         entry.kind != static_cast<uint8_t>(ProductionEntryKind::EPOCH_HANDOFF)) ||
        entry.domain.IsNull() || entry.market_id.IsNull() || entry.seat_set_hash.IsNull()) {
        return ProductionEntryCheck::BAD_VERSION_OR_KIND;
    }
    if (entry.actions.size() > FLOWMESH_V1_MAX_MICROBLOCK_ACTIONS) {
        return ProductionEntryCheck::TOO_MANY_ACTIONS;
    }
    if (!ActionsAreCanonical(entry.actions)) {
        return ProductionEntryCheck::NON_CANONICAL_ACTIONS;
    }
    for (const Action& action : entry.actions) {
        if (!action.ShapeIsCanonicalSansCredential() ||
            action.curve.size() > FLOWMESH_V1_MAX_CURVE_POINTS) {
            return ProductionEntryCheck::NON_CANONICAL_ACTIONS;
        }
    }
    if (entry.sequence == 0 ? !entry.parent_hash.IsNull() : entry.parent_hash.IsNull()) {
        return ProductionEntryCheck::WRONG_PARENT;
    }
    if (entry.anchor.IsNull()) return ProductionEntryCheck::BAD_ANCHOR;
    if (entry.actions_root != ComputeProductionActionsRoot(entry.actions)) {
        return ProductionEntryCheck::WRONG_ACTIONS_ROOT;
    }
    if (entry.effect_count > modern::FLOWMESH_MAX_CHECKPOINT_EFFECTS ||
        entry.effect_start >
            std::numeric_limits<uint64_t>::max() - entry.effect_count ||
        (entry.effect_count == 0
             ? entry.effect_root !=
                   modern::EmptyFlowMeshEffectRoot(entry.effect_start)
             : entry.effect_root.IsNull())) {
        return ProductionEntryCheck::WRONG_EFFECT_RANGE;
    }

    if (entry.kind == static_cast<uint8_t>(ProductionEntryKind::EXECUTION)) {
        if (entry.next_epoch != 0 || !entry.next_anchor.IsNull() ||
            !entry.next_seat_set_hash.IsNull()) {
            return ProductionEntryCheck::BAD_HANDOFF_FIELDS;
        }
        return ProductionEntryCheck::OK;
    }

    if (!entry.actions.empty() || entry.epoch == std::numeric_limits<uint64_t>::max() ||
        entry.next_epoch != entry.epoch + 1 || entry.next_anchor.IsNull() ||
        entry.next_seat_set_hash.IsNull() ||
        entry.state_root != entry.previous_state_root ||
        entry.result_root != ComputeProductionHandoffResultRoot(entry)) {
        return ProductionEntryCheck::BAD_HANDOFF_FIELDS;
    }
    return ProductionEntryCheck::OK;
}

std::optional<AnchorRef> SeatSetAnchor(const ActiveFnBlsSeatSet& seats)
{
    if (seats.anchor_height > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) ||
        seats.anchor_hash.IsNull()) {
        return std::nullopt;
    }
    return AnchorRef{static_cast<int32_t>(seats.anchor_height), seats.anchor_hash};
}

ProductionEntryCheck CheckActiveSeatContext(
    const uint256& domain, const MarketId& market_id, const uint64_t epoch,
    const uint256& set_hash, const ActiveFnBlsSeatSet& seats)
{
    if (CheckActiveFnBlsSeatSet(domain, seats) != BlsSeatSetCheck::OK) {
        return ProductionEntryCheck::INVALID_SEAT_SET;
    }
    if (seats.market_id != market_id) return ProductionEntryCheck::WRONG_MARKET;
    if (seats.epoch != epoch) return ProductionEntryCheck::WRONG_EPOCH;
    if (seats.set_hash != set_hash) return ProductionEntryCheck::WRONG_SEAT_SET;
    return ProductionEntryCheck::OK;
}

ProductionEntryCheck CheckSeatAnchor(
    const ActiveFnBlsSeatSet& seats, const AnchorRef& entry_anchor,
    const ProductionAnchorContext& anchor_context)
{
    const auto seat_anchor{SeatSetAnchor(seats)};
    if (!seat_anchor || seat_anchor->height > entry_anchor.height) {
        return ProductionEntryCheck::BAD_ANCHOR;
    }
    ProductionAnchorContext seat_context{anchor_context};
    seat_context.previous_anchor.reset();
    return CheckProductionAnchor(*seat_anchor, seat_context) == ProductionAnchorCheck::OK
               ? ProductionEntryCheck::OK
               : ProductionEntryCheck::BAD_ANCHOR;
}

std::optional<std::vector<WithdrawalSettlementFactV1>>
ResolveWithdrawalSettlements(
    const FlowMeshState& state, const MarketId& market_id,
    const AnchorRef& anchor, const ProductionAnchorContext& anchor_context,
    const DepositVerifier* chain_facts)
{
    if (chain_facts == nullptr) {
        return std::vector<WithdrawalSettlementFactV1>{};
    }
    auto settlements{chain_facts->GetWithdrawalSettlements(
        anchor_context.previous_anchor, anchor)};
    if (!settlements ||
        settlements->size() >
            FLOWMESH_MAX_WITHDRAWAL_SETTLEMENTS_PER_ENTRY) {
        return std::nullopt;
    }
    const uint256& vault{state.LedgerView().VaultCommitment()};
    for (size_t i{0}; i < settlements->size(); ++i) {
        const WithdrawalSettlementFactV1& fact{(*settlements)[i]};
        if (!WithdrawalSettlementFactIsCanonical(fact) ||
            fact.receipt.market_id != market_id ||
            fact.receipt.vault_id != vault ||
            fact.connected_height > anchor.height ||
            (anchor_context.previous_anchor &&
             fact.connected_height <=
                 anchor_context.previous_anchor->height) ||
            (i > 0 &&
             !((*settlements)[i - 1].receipt.receipt_id <
               fact.receipt.receipt_id))) {
            return std::nullopt;
        }
    }
    return settlements;
}

bool LockPermitsSigning(const ProductionLockResult result)
{
    return result == ProductionLockResult::LOCKED ||
           result == ProductionLockResult::ALREADY_LOCKED_SAME;
}

ProductionEntryCheck CheckHandoff(
    const FlowMeshState& current_state, const ProductionEntryCore& handoff,
    const uint256& domain, const MarketId& market_id,
    const ActiveFnBlsSeatSet& outgoing_seats,
    const ActiveFnBlsSeatSet& next_seats,
    const BlsMicroblockCertificate& outgoing_certificate,
    const uint64_t expected_sequence, const uint64_t expected_effect_start,
    const uint256& expected_parent,
    const ProductionAnchorContext& anchor_context)
{
    if (const auto shape{CheckBasicShape(handoff)}; shape != ProductionEntryCheck::OK) {
        return shape;
    }
    if (handoff.kind != static_cast<uint8_t>(ProductionEntryKind::EPOCH_HANDOFF)) {
        return ProductionEntryCheck::BAD_VERSION_OR_KIND;
    }
    if (handoff.domain != domain) return ProductionEntryCheck::WRONG_DOMAIN;
    if (handoff.market_id != market_id) return ProductionEntryCheck::WRONG_MARKET;
    if (const auto seats{CheckActiveSeatContext(domain, market_id, handoff.epoch,
                                                handoff.seat_set_hash, outgoing_seats)};
        seats != ProductionEntryCheck::OK) {
        return seats;
    }
    if (handoff.sequence != expected_sequence) return ProductionEntryCheck::WRONG_SEQUENCE;
    if (handoff.effect_start != expected_effect_start ||
        handoff.effect_count != 0) {
        return ProductionEntryCheck::WRONG_EFFECT_RANGE;
    }
    if (handoff.parent_hash != expected_parent) return ProductionEntryCheck::WRONG_PARENT;
    if (handoff.previous_state_root != current_state.Root()) {
        return ProductionEntryCheck::WRONG_PREVIOUS_STATE_ROOT;
    }
    if (CheckProductionAnchor(handoff.anchor, anchor_context) != ProductionAnchorCheck::OK) {
        return ProductionEntryCheck::BAD_ANCHOR;
    }
    if (const auto seat_anchor{CheckSeatAnchor(outgoing_seats, handoff.anchor,
                                               anchor_context)};
        seat_anchor != ProductionEntryCheck::OK) {
        return seat_anchor;
    }

    if (CheckActiveFnBlsSeatSet(domain, next_seats) != BlsSeatSetCheck::OK) {
        return ProductionEntryCheck::INVALID_NEXT_SEAT_SET;
    }
    if (next_seats.market_id != market_id) return ProductionEntryCheck::WRONG_MARKET;
    if (next_seats.epoch != handoff.epoch + 1 ||
        handoff.next_epoch != next_seats.epoch) {
        return ProductionEntryCheck::WRONG_NEXT_EPOCH;
    }
    const auto next_anchor{SeatSetAnchor(next_seats)};
    if (!next_anchor || handoff.next_anchor != *next_anchor) {
        return ProductionEntryCheck::WRONG_NEXT_ANCHOR;
    }
    ProductionAnchorContext next_anchor_context{anchor_context};
    next_anchor_context.previous_anchor = handoff.anchor;
    if (CheckProductionAnchor(*next_anchor, next_anchor_context) !=
        ProductionAnchorCheck::OK) {
        return ProductionEntryCheck::WRONG_NEXT_ANCHOR;
    }
    if (handoff.next_seat_set_hash != next_seats.set_hash) {
        return ProductionEntryCheck::WRONG_NEXT_SEAT_SET;
    }
    if (CheckProductionEntryCertificate(handoff, outgoing_seats,
                                        outgoing_certificate) !=
        BlsCertificateCheck::OK) {
        return ProductionEntryCheck::BAD_CERTIFICATE;
    }
    return ProductionEntryCheck::OK;
}

} // namespace

uint256 ComputeProductionActionsRoot(const std::span<const Action> actions)
{
    HashWriter writer{TaggedHash(FLOWMESH_PRODUCTION_ACTIONS_TAG)};
    writer << static_cast<uint64_t>(actions.size());
    for (const Action& action : actions) writer << action;
    return writer.GetSHA256();
}

uint256 ComputeProductionHandoffResultRoot(const ProductionEntryCore& entry)
{
    HashWriter writer{TaggedHash(FLOWMESH_PRODUCTION_HANDOFF_TAG)};
    writer << entry.domain << entry.market_id << entry.epoch << entry.seat_set_hash
           << entry.sequence << entry.parent_hash << entry.anchor
           << entry.previous_state_root << entry.next_epoch << entry.next_anchor
           << entry.next_seat_set_hash;
    return writer.GetSHA256();
}

uint256 ComputeProductionDepositAcceptanceId(
    const modern::FlowMeshDepositAcceptanceV1& acceptance)
{
    std::array<unsigned char, 30> numbers{};
    WriteBE64(numbers.data(), acceptance.epoch);
    WriteBE64(numbers.data() + 8, acceptance.sequence);
    WriteBE32(numbers.data() + 16, acceptance.deposit_outpoint.n);
    WriteBE64(numbers.data() + 20, static_cast<uint64_t>(acceptance.amount));
    WriteBE16(numbers.data() + 28, acceptance.shard);
    const uint256 txid{acceptance.deposit_outpoint.hash.ToUint256()};
    HashWriter writer{TaggedHash(FLOWMESH_PRODUCTION_DEPOSIT_ACCEPTANCE_TAG)};
    writer << std::span<const unsigned char>{acceptance.market_id.begin(), 32}
           << std::span<const unsigned char>{numbers.data(), 16}
           << std::span<const unsigned char>{txid.begin(), 32}
           << std::span<const unsigned char>{numbers.data() + 16, 4}
           << std::span<const unsigned char>{acceptance.account.begin(), 32}
           << std::span<const unsigned char>{acceptance.asset.begin(), 32}
           << std::span<const unsigned char>{numbers.data() + 20, 10}
           << std::span<const unsigned char>{acceptance.vault_id.begin(), 32};
    return writer.GetSHA256();
}

uint16_t ComputeProductionWithdrawalChangeShard(
    const VaultId& vault_id, const uint256& receipt_id)
{
    if (vault_id.IsNull() || receipt_id.IsNull()) return 0;
    HashWriter writer{TaggedHash(FLOWMESH_PRODUCTION_WITHDRAWAL_SHARD_TAG)};
    writer << std::span<const unsigned char>{vault_id.begin(), 32}
           << std::span<const unsigned char>{receipt_id.begin(), 32};
    const uint256 digest{writer.GetSHA256()};
    return ReadLE16(digest.begin()) % 256;
}

std::optional<std::vector<modern::FlowMeshEffectV1>>
DeriveProductionEffects(const ProductionEntryCore& entry,
                        const BatchResult& result)
{
    if (entry.kind != static_cast<uint8_t>(ProductionEntryKind::EXECUTION) ||
        entry.domain.IsNull() || entry.market_id.IsNull() ||
        result.withdrawal_requests.size() !=
            result.account_withdrawal_requests.size()) {
        return std::nullopt;
    }
    const auto vault{ComputeFlowMeshVaultId(entry.domain, entry.market_id)};
    if (!vault) return std::nullopt;

    std::vector<modern::FlowMeshEffectV1> effects;
    effects.reserve(result.credited_deposits.size() +
                    result.account_withdrawal_requests.size());
    for (const BatchResult::CreditedDeposit& credited :
         result.credited_deposits) {
        if (credited.outpoint.IsNull() || credited.account.IsNull() ||
            credited.amount <= 0 || credited.amount > MAX_MONEY) {
            return std::nullopt;
        }
        modern::FlowMeshDepositAcceptanceV1 acceptance;
        acceptance.market_id = entry.market_id;
        acceptance.epoch = entry.epoch;
        acceptance.sequence = entry.sequence;
        acceptance.deposit_outpoint = credited.outpoint;
        acceptance.account = credited.account;
        acceptance.asset = credited.asset;
        acceptance.amount = credited.amount;
        acceptance.vault_id = *vault;
        acceptance.shard = modern::FlowMeshUserDepositShard(
            acceptance.vault_id, acceptance.account);
        acceptance.acceptance_id =
            ComputeProductionDepositAcceptanceId(acceptance);
        modern::FlowMeshEffectV1 effect{acceptance};
        if (!modern::EncodeFlowMeshEffectV1(effect)) return std::nullopt;
        effects.push_back(std::move(effect));
    }

    for (size_t i{0}; i < result.account_withdrawal_requests.size(); ++i) {
        const auto& retained{result.account_withdrawal_requests[i]};
        const modern::WithdrawalReceipt& request{retained.request};
        if (!SameWithdrawalRequest(request, result.withdrawal_requests[i]) ||
            retained.account.IsNull() || request.receipt_id.IsNull() ||
            request.amount <= 0 || request.amount > MAX_MONEY ||
            request.destination.IsNull() || request.vault_commitment != *vault) {
            return std::nullopt;
        }
        modern::FlowMeshWithdrawalReceiptV1 receipt;
        receipt.receipt_id = request.receipt_id;
        receipt.market_id = entry.market_id;
        receipt.epoch = entry.epoch;
        receipt.sequence = entry.sequence;
        receipt.account = retained.account;
        receipt.asset = request.asset;
        receipt.amount = request.amount;
        receipt.destination_owner_commitment = request.destination;
        receipt.vault_id = *vault;
        receipt.deterministic_change_shard =
            ComputeProductionWithdrawalChangeShard(*vault, request.receipt_id);
        modern::FlowMeshEffectV1 effect{receipt};
        if (!modern::EncodeFlowMeshEffectV1(effect)) return std::nullopt;
        effects.push_back(std::move(effect));
    }
    if (effects.size() > modern::FLOWMESH_MAX_CHECKPOINT_EFFECTS) {
        return std::nullopt;
    }
    return effects;
}

std::optional<uint256> ComputeProductionExecutionResultRoot(
    const uint256& batch_result_commitment,
    const std::span<const modern::FlowMeshEffectV1> effects)
{
    if (batch_result_commitment.IsNull() ||
        effects.size() > modern::FLOWMESH_MAX_CHECKPOINT_EFFECTS) {
        return std::nullopt;
    }
    std::array<unsigned char, 8> count{};
    WriteBE64(count.data(), effects.size());
    HashWriter writer{TaggedHash(FLOWMESH_PRODUCTION_RESULT_TAG)};
    writer << std::span<const unsigned char>{batch_result_commitment.begin(), 32}
           << std::span<const unsigned char>{count};
    for (const modern::FlowMeshEffectV1& effect : effects) {
        const auto encoded{modern::EncodeFlowMeshEffectV1(effect)};
        if (!encoded) return std::nullopt;
        std::array<unsigned char, 4> size{};
        WriteBE32(size.data(), encoded->size());
        writer << std::span<const unsigned char>{size}
               << std::span<const unsigned char>{*encoded};
    }
    return writer.GetSHA256();
}

uint256 ProductionSeatRewardClaimDigest(
    const uint256& domain, const uint256& execution_config_id,
    const MarketId& market_id, const ActiveFnBlsSeatSet& historical_seats,
    const ActiveFnBlsSeat& historical_member, const Action& action)
{
    std::array<unsigned char, 12> numbers{};
    WriteBE64(numbers.data(), historical_seats.epoch);
    WriteBE32(numbers.data() + 8, historical_member.outpoint.n);
    const uint256 outpoint_hash{historical_member.outpoint.hash.ToUint256()};
    const auto key{historical_member.key.Key().Compressed()};
    const uint256 action_id{action.Id()};
    HashWriter writer{TaggedHash(FLOWMESH_SEAT_REWARD_CLAIM_TAG)};
    writer << std::span<const unsigned char>{domain.begin(), 32}
           << std::span<const unsigned char>{execution_config_id.begin(), 32}
           << std::span<const unsigned char>{market_id.begin(), 32}
           << std::span<const unsigned char>{numbers.data(), 8}
           << std::span<const unsigned char>{historical_seats.set_hash.begin(), 32}
           << std::span<const unsigned char>{historical_member.seat_id.begin(), 32}
           << std::span<const unsigned char>{outpoint_hash.begin(), 32}
           << std::span<const unsigned char>{numbers.data() + 8, 4}
           << std::span<const unsigned char>{key}
           << std::span<const unsigned char>{action_id.begin(), 32};
    return writer.GetSHA256();
}

SeatRewardClaimCredentialCheck CheckProductionSeatRewardClaimCredential(
    const Action& action, const uint256& domain,
    const uint256& execution_config_id, const MarketId& market_id,
    const ActiveFnBlsSeatSet& historical_seats,
    const ActiveFnBlsSeat& historical_member)
{
    if (static_cast<ActionType>(action.type) !=
        ActionType::CLAIM_SEAT_REWARD) {
        return SeatRewardClaimCredentialCheck::NOT_REWARD_CLAIM;
    }
    if (!action.ShapeIsCanonicalSansCredential() || domain.IsNull() ||
        execution_config_id.IsNull() || market_id.IsNull()) {
        return SeatRewardClaimCredentialCheck::MALFORMED_ACTION;
    }
    if (CheckActiveFnBlsSeatSet(domain, historical_seats) !=
            BlsSeatSetCheck::OK ||
        historical_seats.market_id != market_id) {
        return SeatRewardClaimCredentialCheck::INVALID_HISTORICAL_SEAT_SET;
    }
    const auto member{std::find_if(
        historical_seats.members.begin(), historical_seats.members.end(),
        [&](const ActiveFnBlsSeat& candidate) {
            return SameHistoricalMember(candidate, historical_member);
        })};
    if (member == historical_seats.members.end()) {
        return SeatRewardClaimCredentialCheck::MEMBER_NOT_IN_SET;
    }
    const FlowMeshFeeSeat fee_seat{member->seat_id,
                                   member->key.Key().Compressed()};
    if (action.signer != FlowMeshSeatRewardAccount(
                             market_id, historical_seats.epoch, fee_seat)) {
        return SeatRewardClaimCredentialCheck::WRONG_REWARD_ACCOUNT;
    }
    if (action.credential.size() != FLOWMESH_SEAT_REWARD_CREDENTIAL_SIZE) {
        return SeatRewardClaimCredentialCheck::MALFORMED_SIGNATURE;
    }
    std::array<unsigned char, bls::SIGNATURE_SIZE> signature_bytes{};
    std::copy(action.credential.begin(), action.credential.end(),
              signature_bytes.begin());
    const auto signature{bls::Signature::Decode(signature_bytes)};
    if (!signature) {
        return SeatRewardClaimCredentialCheck::MALFORMED_SIGNATURE;
    }
    const uint256 digest{ProductionSeatRewardClaimDigest(
        domain, execution_config_id, market_id, historical_seats, *member,
        action)};
    if (!bls::Verify(member->key.Key(),
                     std::span<const unsigned char>{digest.begin(), 32},
                     *signature)) {
        return SeatRewardClaimCredentialCheck::BAD_SIGNATURE;
    }
    return SeatRewardClaimCredentialCheck::OK;
}

bool SignProductionSeatRewardClaim(
    const bls::SecretKey& key, const uint256& domain,
    const uint256& execution_config_id, const MarketId& market_id,
    const ActiveFnBlsSeatSet& historical_seats,
    const ActiveFnBlsSeat& historical_member, Action& action)
{
    if (key.GetPublicKey() != historical_member.key.Key()) return false;
    const FlowMeshFeeSeat fee_seat{historical_member.seat_id,
                                   historical_member.key.Key().Compressed()};
    action.signer = FlowMeshSeatRewardAccount(
        market_id, historical_seats.epoch, fee_seat);
    if (!action.ShapeIsCanonicalSansCredential()) return false;
    const uint256 digest{ProductionSeatRewardClaimDigest(
        domain, execution_config_id, market_id, historical_seats,
        historical_member, action)};
    const auto signature{key.Sign(
        std::span<const unsigned char>{digest.begin(), 32}).Compressed()};
    action.credential.assign(signature.begin(), signature.end());
    return CheckProductionSeatRewardClaimCredential(
               action, domain, execution_config_id, market_id,
               historical_seats, historical_member) ==
           SeatRewardClaimCredentialCheck::OK;
}

bool CheckProductionActionCredential(
    const Action& action, const uint256& domain,
    const uint256& execution_config_id, const MarketId& market_id,
    const ActiveFnBlsSeatSet* historical_reward_seats,
    const ActiveFnBlsSeat* historical_reward_member)
{
    if (!action.ShapeIsCanonical()) return false;
    const ActionType type{static_cast<ActionType>(action.type)};
    if (type == ActionType::DEPOSIT) return action.credential.empty();
    if (type == ActionType::CLAIM_SEAT_REWARD) {
        return historical_reward_seats != nullptr &&
               historical_reward_member != nullptr &&
               CheckProductionSeatRewardClaimCredential(
                   action, domain, execution_config_id, market_id,
                   *historical_reward_seats, *historical_reward_member) ==
                   SeatRewardClaimCredentialCheck::OK;
    }
    const SchnorrActionAuthenticator schnorr{domain, execution_config_id};
    return schnorr.Authenticate(action);
}

std::optional<ProductionEntryCommitmentV1>
ProductionEntryCore::Commitment() const
{
    if (anchor.height < 0) return std::nullopt;
    ProductionEntryCommitmentV1 out;
    out.version = version;
    out.kind = kind;
    out.domain = domain;
    out.market_id = market_id;
    out.epoch = epoch;
    out.seat_set_hash = seat_set_hash;
    out.sequence = sequence;
    out.parent_hash = parent_hash;
    out.production_anchor = {static_cast<uint64_t>(anchor.height),
                             anchor.hash};
    out.previous_state_root = previous_state_root;
    out.actions_root = actions_root;
    out.result_root = result_root;
    out.state_root = state_root;
    out.effect_start = effect_start;
    out.effect_count = effect_count;
    out.effect_root = effect_root;
    if (kind == static_cast<uint8_t>(ProductionEntryKind::EPOCH_HANDOFF)) {
        if (next_anchor.height < 0) return std::nullopt;
        out.next_epoch = next_epoch;
        out.next_seat_anchor = {static_cast<uint64_t>(next_anchor.height),
                                next_anchor.hash};
        out.next_seat_set_hash = next_seat_set_hash;
    }
    if (!ComputeProductionEntryIdentityV1(out)) return std::nullopt;
    return out;
}

uint256 ProductionEntryCore::GetHash() const
{
    const auto commitment{Commitment()};
    const auto identity{commitment
                            ? ComputeProductionEntryIdentityV1(*commitment)
                            : std::nullopt};
    return identity.value_or(uint256{});
}

std::optional<std::vector<unsigned char>> EncodeProductionEntry(
    const ProductionEntryCore& entry)
{
    if (CheckBasicShape(entry) != ProductionEntryCheck::OK) return std::nullopt;
    std::vector<unsigned char> bytes;
    VectorWriter writer{bytes, 0};
    writer << entry;
    if (bytes.size() > FLOWMESH_V1_MAX_MICROBLOCK_BYTES) return std::nullopt;
    return bytes;
}

std::optional<ProductionEntryCore> DecodeProductionEntry(
    const std::span<const unsigned char> bytes)
{
    if (bytes.empty() || bytes.size() > FLOWMESH_V1_MAX_MICROBLOCK_BYTES) {
        return std::nullopt;
    }
    try {
        SpanReader reader{bytes};
        ProductionEntryCore entry;
        reader >> entry;
        if (!reader.empty() || CheckBasicShape(entry) != ProductionEntryCheck::OK) {
            return std::nullopt;
        }
        return entry;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

ProductionAnchorCheck CheckProductionAnchor(
    const AnchorRef& anchor, const ProductionAnchorContext& context)
{
    if (anchor.height < 0 || anchor.hash.IsNull()) {
        return ProductionAnchorCheck::NULL_ANCHOR;
    }
    if (context.b3_tip_height < anchor.height) {
        return ProductionAnchorCheck::FUTURE_ANCHOR;
    }
    if (static_cast<int64_t>(context.b3_tip_height) - anchor.height <
        FLOWMESH_PRODUCTION_MIN_ANCHOR_DEPTH) {
        return ProductionAnchorCheck::NOT_DEEP_ENOUGH;
    }
    if (context.previous_anchor) {
        if (anchor.height < context.previous_anchor->height) {
            return ProductionAnchorCheck::NON_MONOTONIC;
        }
        if (anchor.height == context.previous_anchor->height &&
            anchor.hash != context.previous_anchor->hash) {
            return ProductionAnchorCheck::SAME_HEIGHT_DIFFERENT_HASH;
        }
    }
    if (context.policy == nullptr || !context.policy->StillCanonical(anchor)) {
        return ProductionAnchorCheck::NOT_CANONICAL;
    }
    if (!context.policy->Acceptable(anchor)) {
        return ProductionAnchorCheck::POLICY_REJECTED;
    }
    return ProductionAnchorCheck::OK;
}

const char* ProductionEntryCheckName(const ProductionEntryCheck check)
{
    switch (check) {
    case ProductionEntryCheck::OK: return "ok";
    case ProductionEntryCheck::BAD_VERSION_OR_KIND: return "bad-version-or-kind";
    case ProductionEntryCheck::ENTRY_TOO_LARGE: return "entry-too-large";
    case ProductionEntryCheck::TOO_MANY_ACTIONS: return "too-many-actions";
    case ProductionEntryCheck::NON_CANONICAL_ACTIONS: return "non-canonical-actions";
    case ProductionEntryCheck::INVALID_SEAT_SET: return "invalid-seat-set";
    case ProductionEntryCheck::WRONG_DOMAIN: return "wrong-domain";
    case ProductionEntryCheck::WRONG_MARKET: return "wrong-market";
    case ProductionEntryCheck::WRONG_EPOCH: return "wrong-epoch";
    case ProductionEntryCheck::WRONG_SEAT_SET: return "wrong-seat-set";
    case ProductionEntryCheck::WRONG_SEQUENCE: return "wrong-sequence";
    case ProductionEntryCheck::WRONG_PARENT: return "wrong-parent";
    case ProductionEntryCheck::BAD_ANCHOR: return "bad-anchor";
    case ProductionEntryCheck::WRONG_PREVIOUS_STATE_ROOT: return "wrong-previous-state-root";
    case ProductionEntryCheck::WRONG_ACTIONS_ROOT: return "wrong-actions-root";
    case ProductionEntryCheck::WRONG_RESULT_ROOT: return "wrong-result-root";
    case ProductionEntryCheck::WRONG_STATE_ROOT: return "wrong-state-root";
    case ProductionEntryCheck::WRONG_EFFECT_RANGE: return "wrong-effect-range";
    case ProductionEntryCheck::CHAIN_SETTLEMENT_MISMATCH: return "chain-settlement-mismatch";
    case ProductionEntryCheck::BAD_FEE_CONTEXT: return "bad-fee-context";
    case ProductionEntryCheck::EXECUTION_FAILED: return "execution-failed";
    case ProductionEntryCheck::BAD_HANDOFF_FIELDS: return "bad-handoff-fields";
    case ProductionEntryCheck::INVALID_NEXT_SEAT_SET: return "invalid-next-seat-set";
    case ProductionEntryCheck::WRONG_NEXT_EPOCH: return "wrong-next-epoch";
    case ProductionEntryCheck::WRONG_NEXT_ANCHOR: return "wrong-next-anchor";
    case ProductionEntryCheck::WRONG_NEXT_SEAT_SET: return "wrong-next-seat-set";
    case ProductionEntryCheck::BAD_CERTIFICATE: return "bad-certificate";
    case ProductionEntryCheck::HANDOFF_NOT_CONNECTED: return "handoff-not-connected";
    case ProductionEntryCheck::EPOCH_PAUSED: return "epoch-paused";
    }
    return "unknown";
}

std::optional<FlowMeshFeeContext> BuildProductionFlowMeshFeeContext(
    const uint256& domain, const ActiveFnBlsSeatSet& active_seats,
    const uint256& treasury_owner_commitment)
{
    if (treasury_owner_commitment.IsNull() ||
        CheckActiveFnBlsSeatSet(domain, active_seats) != BlsSeatSetCheck::OK) {
        return std::nullopt;
    }
    FlowMeshFeeContext context;
    context.market_id = active_seats.market_id;
    context.epoch = active_seats.epoch;
    context.treasury_owner_commitment = treasury_owner_commitment;
    context.seats.reserve(active_seats.Size());
    for (const ActiveFnBlsSeat& member : active_seats.members) {
        context.seats.push_back(
            FlowMeshFeeSeat{member.seat_id, member.key.Key().Compressed()});
    }
    if (!FlowMeshFeeContextIsCanonical(context)) return std::nullopt;
    return context;
}

std::optional<BuiltProductionExecution> BuildProductionExecutionEntry(
    const FlowMeshState& previous_state, const uint256& domain,
    const MarketId& market_id, const ActiveFnBlsSeatSet& active_seats,
    const ProductionEpochGate& epoch_gate, const uint64_t sequence,
    const uint64_t effect_start, const uint256& parent_hash,
    const AnchorRef& anchor,
    const ProductionAnchorContext& anchor_context,
    const uint256& treasury_owner_commitment, const std::span<const Action> actions,
    const DepositVerifier* deposits, ProductionEntryCheck& check)
{
    if (actions.size() > FLOWMESH_V1_MAX_MICROBLOCK_ACTIONS) {
        check = ProductionEntryCheck::TOO_MANY_ACTIONS;
        return std::nullopt;
    }
    if (domain.IsNull()) {
        check = ProductionEntryCheck::WRONG_DOMAIN;
        return std::nullopt;
    }
    if (market_id.IsNull() || active_seats.market_id != market_id) {
        check = ProductionEntryCheck::WRONG_MARKET;
        return std::nullopt;
    }
    if (const auto seats{CheckActiveSeatContext(domain, market_id, active_seats.epoch,
                                                active_seats.set_hash, active_seats)};
        seats != ProductionEntryCheck::OK) {
        check = seats;
        return std::nullopt;
    }
    if (!epoch_gate.CanExecute(active_seats)) {
        check = epoch_gate.Paused() ? ProductionEntryCheck::EPOCH_PAUSED
                                    : ProductionEntryCheck::HANDOFF_NOT_CONNECTED;
        return std::nullopt;
    }
    if (CheckProductionAnchor(anchor, anchor_context) != ProductionAnchorCheck::OK ||
        CheckSeatAnchor(active_seats, anchor, anchor_context) != ProductionEntryCheck::OK) {
        check = ProductionEntryCheck::BAD_ANCHOR;
        return std::nullopt;
    }
    const auto settlements{ResolveWithdrawalSettlements(
        previous_state, market_id, anchor, anchor_context, deposits)};
    if (!settlements || (!settlements->empty() && !actions.empty())) {
        check = ProductionEntryCheck::CHAIN_SETTLEMENT_MISMATCH;
        return std::nullopt;
    }
    const auto fee_context{BuildProductionFlowMeshFeeContext(
        domain, active_seats, treasury_owner_commitment)};
    if (!fee_context) {
        check = ProductionEntryCheck::BAD_FEE_CONTEXT;
        return std::nullopt;
    }

    ProductionEntryCore entry;
    entry.domain = domain;
    entry.market_id = market_id;
    entry.epoch = active_seats.epoch;
    entry.seat_set_hash = active_seats.set_hash;
    entry.sequence = sequence;
    entry.effect_start = effect_start;
    entry.effect_root = modern::EmptyFlowMeshEffectRoot(effect_start);
    entry.parent_hash = parent_hash;
    entry.anchor = anchor;
    entry.previous_state_root = previous_state.Root();
    entry.actions = CanonicalizeActions(
        std::vector<Action>{actions.begin(), actions.end()});
    entry.actions_root = ComputeProductionActionsRoot(entry.actions);

    // Enforce the v1 body/curve bounds before execution work. Result/state
    // roots are execution claims and may still be null at this construction
    // stage; CheckBasicShape intentionally validates their equality only for
    // handoff entries.
    if (const auto shape{CheckBasicShape(entry)}; shape != ProductionEntryCheck::OK) {
        check = shape;
        return std::nullopt;
    }

    FlowMeshState next{previous_state};
    BatchExecutor executor{next, deposits, &*fee_context};
    const auto result{executor.ExecuteSlot(entry.actions, entry.anchor,
                                           *settlements)};
    if (!result) {
        check = ProductionEntryCheck::EXECUTION_FAILED;
        return std::nullopt;
    }
    const auto effects{DeriveProductionEffects(entry, *result)};
    const auto result_root{
        effects ? ComputeProductionExecutionResultRoot(
                      result->result_commitment, *effects)
                : std::nullopt};
    if (!effects || !result_root) {
        check = ProductionEntryCheck::EXECUTION_FAILED;
        return std::nullopt;
    }
    entry.result_root = *result_root;
    entry.state_root = result->state_root;
    entry.effect_count = static_cast<uint32_t>(effects->size());
    const auto effect_root{
        modern::ComputeFlowMeshEffectRoot(entry.effect_start, *effects)};
    if (!effect_root) {
        check = ProductionEntryCheck::WRONG_EFFECT_RANGE;
        return std::nullopt;
    }
    entry.effect_root = *effect_root;

    if (const auto shape{CheckBasicShape(entry)}; shape != ProductionEntryCheck::OK) {
        check = shape;
        return std::nullopt;
    }
    if (!EncodeProductionEntry(entry)) {
        check = ProductionEntryCheck::ENTRY_TOO_LARGE;
        return std::nullopt;
    }
    check = ProductionEntryCheck::OK;
    return BuiltProductionExecution{std::move(entry), std::move(next), *result,
                                    *effects, *settlements};
}

std::optional<ExecutedProductionEntry> ExecuteProductionEntry(
    const FlowMeshState& previous_state, const ProductionEntryCore& entry,
    const uint256& expected_domain, const MarketId& expected_market,
    const ActiveFnBlsSeatSet& active_seats,
    const ProductionEpochGate& epoch_gate, const uint64_t expected_sequence,
    const uint64_t expected_effect_start, const uint256& expected_parent,
    const ProductionAnchorContext& anchor_context,
    const uint256& treasury_owner_commitment, const DepositVerifier* deposits,
    ProductionEntryCheck& check)
{
    if (const auto shape{CheckBasicShape(entry)}; shape != ProductionEntryCheck::OK) {
        check = shape;
        return std::nullopt;
    }
    if (entry.kind != static_cast<uint8_t>(ProductionEntryKind::EXECUTION)) {
        check = ProductionEntryCheck::BAD_VERSION_OR_KIND;
        return std::nullopt;
    }
    if (!EncodeProductionEntry(entry)) {
        check = ProductionEntryCheck::ENTRY_TOO_LARGE;
        return std::nullopt;
    }
    if (entry.domain != expected_domain) {
        check = ProductionEntryCheck::WRONG_DOMAIN;
        return std::nullopt;
    }
    if (entry.market_id != expected_market) {
        check = ProductionEntryCheck::WRONG_MARKET;
        return std::nullopt;
    }
    if (const auto seats{CheckActiveSeatContext(expected_domain, expected_market,
                                                entry.epoch, entry.seat_set_hash,
                                                active_seats)};
        seats != ProductionEntryCheck::OK) {
        check = seats;
        return std::nullopt;
    }
    if (!epoch_gate.CanExecute(active_seats)) {
        check = epoch_gate.Paused() ? ProductionEntryCheck::EPOCH_PAUSED
                                    : ProductionEntryCheck::HANDOFF_NOT_CONNECTED;
        return std::nullopt;
    }
    if (entry.sequence != expected_sequence) {
        check = ProductionEntryCheck::WRONG_SEQUENCE;
        return std::nullopt;
    }
    if (entry.effect_start != expected_effect_start) {
        check = ProductionEntryCheck::WRONG_EFFECT_RANGE;
        return std::nullopt;
    }
    if (entry.parent_hash != expected_parent) {
        check = ProductionEntryCheck::WRONG_PARENT;
        return std::nullopt;
    }
    if (CheckProductionAnchor(entry.anchor, anchor_context) != ProductionAnchorCheck::OK ||
        CheckSeatAnchor(active_seats, entry.anchor, anchor_context) !=
            ProductionEntryCheck::OK) {
        check = ProductionEntryCheck::BAD_ANCHOR;
        return std::nullopt;
    }
    if (entry.previous_state_root != previous_state.Root()) {
        check = ProductionEntryCheck::WRONG_PREVIOUS_STATE_ROOT;
        return std::nullopt;
    }
    const auto settlements{ResolveWithdrawalSettlements(
        previous_state, expected_market, entry.anchor, anchor_context,
        deposits)};
    if (!settlements ||
        (!settlements->empty() && !entry.actions.empty())) {
        check = ProductionEntryCheck::CHAIN_SETTLEMENT_MISMATCH;
        return std::nullopt;
    }
    const auto fee_context{BuildProductionFlowMeshFeeContext(
        expected_domain, active_seats, treasury_owner_commitment)};
    if (!fee_context) {
        check = ProductionEntryCheck::BAD_FEE_CONTEXT;
        return std::nullopt;
    }

    FlowMeshState next{previous_state};
    BatchExecutor executor{next, deposits, &*fee_context};
    const auto result{executor.ExecuteSlot(entry.actions, entry.anchor,
                                           *settlements)};
    if (!result) {
        check = ProductionEntryCheck::EXECUTION_FAILED;
        return std::nullopt;
    }
    const auto effects{DeriveProductionEffects(entry, *result)};
    const auto result_root{
        effects ? ComputeProductionExecutionResultRoot(
                      result->result_commitment, *effects)
                : std::nullopt};
    const auto effect_root{
        effects ? modern::ComputeFlowMeshEffectRoot(entry.effect_start, *effects)
                : std::nullopt};
    if (!effects || !result_root || !effect_root ||
        entry.result_root != *result_root ||
        entry.effect_count != effects->size() ||
        entry.effect_root != *effect_root) {
        check = ProductionEntryCheck::WRONG_RESULT_ROOT;
        return std::nullopt;
    }
    if (entry.state_root != result->state_root) {
        check = ProductionEntryCheck::WRONG_STATE_ROOT;
        return std::nullopt;
    }
    check = ProductionEntryCheck::OK;
    return ExecutedProductionEntry{std::move(next), *result, *effects,
                                   *settlements};
}

std::optional<ProductionEntryCore> BuildProductionHandoffEntry(
    const FlowMeshState& current_state, const uint256& domain,
    const MarketId& market_id, const ActiveFnBlsSeatSet& outgoing_seats,
    const ActiveFnBlsSeatSet& next_seats, const uint64_t sequence,
    const uint64_t effect_start, const uint256& parent_hash,
    const AnchorRef& anchor,
    const ProductionAnchorContext& anchor_context, ProductionEntryCheck& check)
{
    if (const auto outgoing{CheckActiveSeatContext(domain, market_id,
                                                   outgoing_seats.epoch,
                                                   outgoing_seats.set_hash,
                                                   outgoing_seats)};
        outgoing != ProductionEntryCheck::OK) {
        check = outgoing;
        return std::nullopt;
    }
    if (CheckActiveFnBlsSeatSet(domain, next_seats) != BlsSeatSetCheck::OK) {
        check = ProductionEntryCheck::INVALID_NEXT_SEAT_SET;
        return std::nullopt;
    }
    if (next_seats.market_id != market_id) {
        check = ProductionEntryCheck::WRONG_MARKET;
        return std::nullopt;
    }
    if (outgoing_seats.epoch == std::numeric_limits<uint64_t>::max() ||
        next_seats.epoch != outgoing_seats.epoch + 1) {
        check = ProductionEntryCheck::WRONG_NEXT_EPOCH;
        return std::nullopt;
    }
    if (CheckProductionAnchor(anchor, anchor_context) != ProductionAnchorCheck::OK ||
        CheckSeatAnchor(outgoing_seats, anchor, anchor_context) !=
            ProductionEntryCheck::OK) {
        check = ProductionEntryCheck::BAD_ANCHOR;
        return std::nullopt;
    }
    const auto next_anchor{SeatSetAnchor(next_seats)};
    if (!next_anchor) {
        check = ProductionEntryCheck::WRONG_NEXT_ANCHOR;
        return std::nullopt;
    }
    ProductionAnchorContext next_context{anchor_context};
    next_context.previous_anchor = anchor;
    if (CheckProductionAnchor(*next_anchor, next_context) != ProductionAnchorCheck::OK) {
        check = ProductionEntryCheck::WRONG_NEXT_ANCHOR;
        return std::nullopt;
    }

    ProductionEntryCore entry;
    entry.kind = static_cast<uint8_t>(ProductionEntryKind::EPOCH_HANDOFF);
    entry.domain = domain;
    entry.market_id = market_id;
    entry.epoch = outgoing_seats.epoch;
    entry.seat_set_hash = outgoing_seats.set_hash;
    entry.sequence = sequence;
    entry.effect_start = effect_start;
    entry.parent_hash = parent_hash;
    entry.anchor = anchor;
    entry.previous_state_root = current_state.Root();
    entry.actions_root = ComputeProductionActionsRoot(entry.actions);
    entry.state_root = entry.previous_state_root;
    entry.next_epoch = next_seats.epoch;
    entry.next_anchor = *next_anchor;
    entry.next_seat_set_hash = next_seats.set_hash;
    entry.result_root = ComputeProductionHandoffResultRoot(entry);
    entry.effect_root = modern::EmptyFlowMeshEffectRoot(entry.effect_start);
    if (const auto shape{CheckBasicShape(entry)}; shape != ProductionEntryCheck::OK) {
        check = shape;
        return std::nullopt;
    }
    if (!EncodeProductionEntry(entry)) {
        check = ProductionEntryCheck::ENTRY_TOO_LARGE;
        return std::nullopt;
    }
    check = ProductionEntryCheck::OK;
    return entry;
}

BlsCertificateContext ProductionCertificateContext(
    const ProductionEntryCore& entry)
{
    return BlsCertificateContext{entry.domain, entry.market_id, entry.epoch,
                                 entry.seat_set_hash, entry.sequence,
                                 entry.GetHash()};
}

BlsCertificateCheck CheckProductionEntryCertificate(
    const ProductionEntryCore& entry, const ActiveFnBlsSeatSet& active_seats,
    const BlsMicroblockCertificate& certificate)
{
    return CheckBlsMicroblockCertificate(certificate,
                                         ProductionCertificateContext(entry),
                                         active_seats);
}

BlsCertificateAssemblyCheck AssembleProductionEntryCertificate(
    const ProductionEntryCore& entry, const ActiveFnBlsSeatSet& active_seats,
    const std::span<const IndexedBlsSignature> partials,
    BlsMicroblockCertificate& certificate_out)
{
    BlsCertificateAssemblyCheck check{BlsCertificateAssemblyCheck::AGGREGATION_FAILED};
    const auto certificate{AssembleBlsMicroblockCertificate(
        ProductionCertificateContext(entry), active_seats, partials, check)};
    if (certificate) certificate_out = *certificate;
    return check;
}

std::optional<modern::FlowMeshCheckpointRecordV1>
BuildProductionCheckpointRecord(
    const ProductionEntryCore& entry,
    const BlsMicroblockCertificate& certificate,
    const ActiveFnBlsSeatSet& active_seats,
    const modern::FlowMeshCheckpointId& previous_checkpoint_id)
{
    const auto seat_anchor{SeatSetAnchor(active_seats)};
    if (!seat_anchor || entry.anchor.height < 0 ||
        CheckActiveSeatContext(entry.domain, entry.market_id, entry.epoch,
                               entry.seat_set_hash, active_seats) !=
            ProductionEntryCheck::OK ||
        CheckBasicShape(entry) != ProductionEntryCheck::OK ||
        CheckProductionEntryCertificate(entry, active_seats, certificate) !=
            BlsCertificateCheck::OK) {
        return std::nullopt;
    }

    modern::FlowMeshCheckpointRecordV1 out;
    out.core.kind =
        entry.kind == static_cast<uint8_t>(ProductionEntryKind::EXECUTION)
            ? modern::FlowMeshCheckpointKind::EXECUTION
            : modern::FlowMeshCheckpointKind::EPOCH_HANDOFF;
    out.core.domain = entry.domain;
    out.core.market_id = entry.market_id;
    out.core.epoch = entry.epoch;
    out.core.sequence = entry.sequence;
    out.core.microblock_hash = entry.GetHash();
    out.core.previous_checkpoint_id = previous_checkpoint_id;
    out.core.anchor = {static_cast<uint64_t>(seat_anchor->height),
                       seat_anchor->hash};
    out.core.seat_set_hash = entry.seat_set_hash;
    out.core.production_anchor = {
        static_cast<uint64_t>(entry.anchor.height), entry.anchor.hash};
    out.core.parent_hash = entry.parent_hash;
    out.core.previous_state_root = entry.previous_state_root;
    out.core.actions_root = entry.actions_root;
    out.core.result_root = entry.result_root;
    out.core.state_root = entry.state_root;
    out.core.effect_start = entry.effect_start;
    out.core.effect_count = entry.effect_count;
    out.core.effect_root = entry.effect_root;
    if (out.core.kind == modern::FlowMeshCheckpointKind::EPOCH_HANDOFF) {
        if (entry.next_anchor.height < 0) return std::nullopt;
        out.core.handoff = modern::FlowMeshCheckpointHandoffV1{
            entry.next_epoch,
            {static_cast<uint64_t>(entry.next_anchor.height),
             entry.next_anchor.hash},
            entry.next_seat_set_hash};
    }
    out.certificate = certificate;

    const auto identity{
        modern::FlowMeshCheckpointProductionIdentityV1(out.core)};
    if (!identity || *identity != entry.GetHash() ||
        !modern::EncodeFlowMeshCheckpointRecordV1(out, active_seats.Size())) {
        return std::nullopt;
    }
    return out;
}

ProductionLockResult ProductionSigningGuard::Lock(
    const ProductionEntryCore& entry)
{
    return m_journal.LockOnce(
        ProductionSignPosition{entry.epoch, entry.sequence}, entry.GetHash());
}

uint32_t ProductionProposerSeatIndex(const uint64_t sequence,
                                     const uint32_t round,
                                     const size_t seat_count)
{
    if (seat_count == 0 || seat_count > std::numeric_limits<uint32_t>::max()) {
        return std::numeric_limits<uint32_t>::max();
    }
    return static_cast<uint32_t>((sequence % seat_count + round % seat_count) %
                                 seat_count);
}

uint256 ProductionProposalDigest(const ProductionEntryCore& entry,
                                 const uint32_t round)
{
    HashWriter writer{TaggedHash(FLOWMESH_PRODUCTION_PROPOSAL_TAG)};
    std::array<unsigned char, 12> numbers{};
    WriteBE64(numbers.data(), entry.epoch);
    WriteBE32(numbers.data() + 8, round);
    const uint256 entry_hash{entry.GetHash()};
    writer << std::span<const unsigned char>{entry.domain.begin(), 32}
           << std::span<const unsigned char>{entry.market_id.begin(), 32}
           << std::span<const unsigned char>{numbers.data(), 8}
           << std::span<const unsigned char>{entry.seat_set_hash.begin(), 32}
           << std::span<const unsigned char>{numbers.data() + 8, 4}
           << std::span<const unsigned char>{entry_hash.begin(), 32};
    return writer.GetSHA256();
}

std::optional<ProductionProposalEnvelope> SignProductionProposal(
    const bls::SecretKey& proposer_key, const ProductionEntryCore& entry,
    const uint32_t round, const ActiveFnBlsSeatSet& active_seats,
    ProductionSigningGuard& guard, ProductionProposalCheck& check)
{
    if (CheckActiveFnBlsSeatSet(entry.domain, active_seats) != BlsSeatSetCheck::OK) {
        check = ProductionProposalCheck::INVALID_SEAT_SET;
        return std::nullopt;
    }
    if (entry.market_id != active_seats.market_id) {
        check = ProductionProposalCheck::WRONG_MARKET;
        return std::nullopt;
    }
    if (entry.epoch != active_seats.epoch) {
        check = ProductionProposalCheck::WRONG_EPOCH;
        return std::nullopt;
    }
    if (entry.seat_set_hash != active_seats.set_hash) {
        check = ProductionProposalCheck::WRONG_SEAT_SET;
        return std::nullopt;
    }
    const uint32_t proposer_index{
        ProductionProposerSeatIndex(entry.sequence, round, active_seats.Size())};
    if (proposer_index >= active_seats.Size() ||
        proposer_key.GetPublicKey() != active_seats.members[proposer_index].key.Key()) {
        check = ProductionProposalCheck::NOT_PROPOSER_KEY;
        return std::nullopt;
    }
    const ProductionLockResult lock{guard.Lock(entry)};
    if (!LockPermitsSigning(lock)) {
        check = lock == ProductionLockResult::CONFLICT
                    ? ProductionProposalCheck::LOCK_CONFLICT
                    : ProductionProposalCheck::LOCK_STORAGE_FAILURE;
        return std::nullopt;
    }
    ProductionProposalEnvelope proposal;
    proposal.entry = entry;
    proposal.round = round;
    proposal.proposer_seat_index = proposer_index;
    const uint256 digest{ProductionProposalDigest(entry, round)};
    proposal.proposer_signature = proposer_key
                                      .Sign(std::span<const unsigned char>{digest.begin(), 32})
                                      .Compressed();
    check = ProductionProposalCheck::OK;
    return proposal;
}

ProductionProposalCheck CheckProductionProposal(
    const ProductionProposalEnvelope& proposal, const uint256& expected_domain,
    const MarketId& expected_market, const uint64_t expected_epoch,
    const uint32_t expected_round, const ActiveFnBlsSeatSet& active_seats)
{
    if (proposal.entry.domain != expected_domain) {
        return ProductionProposalCheck::WRONG_DOMAIN;
    }
    if (proposal.entry.market_id != expected_market) {
        return ProductionProposalCheck::WRONG_MARKET;
    }
    if (proposal.entry.epoch != expected_epoch) {
        return ProductionProposalCheck::WRONG_EPOCH;
    }
    if (proposal.entry.seat_set_hash != active_seats.set_hash) {
        return ProductionProposalCheck::WRONG_SEAT_SET;
    }
    if (proposal.round != expected_round) return ProductionProposalCheck::WRONG_ROUND;
    if (CheckActiveFnBlsSeatSet(expected_domain, active_seats) != BlsSeatSetCheck::OK ||
        active_seats.market_id != expected_market || active_seats.epoch != expected_epoch) {
        return ProductionProposalCheck::INVALID_SEAT_SET;
    }
    const uint32_t expected_proposer{ProductionProposerSeatIndex(
        proposal.entry.sequence, proposal.round, active_seats.Size())};
    if (proposal.proposer_seat_index != expected_proposer ||
        expected_proposer >= active_seats.Size()) {
        return ProductionProposalCheck::WRONG_PROPOSER;
    }
    const auto signature{bls::Signature::Decode(proposal.proposer_signature)};
    if (!signature) return ProductionProposalCheck::MALFORMED_SIGNATURE;
    const uint256 digest{ProductionProposalDigest(proposal.entry, proposal.round)};
    if (!bls::Verify(active_seats.members[expected_proposer].key.Key(),
                     std::span<const unsigned char>{digest.begin(), 32}, *signature)) {
        return ProductionProposalCheck::BAD_SIGNATURE;
    }
    return ProductionProposalCheck::OK;
}

std::optional<IndexedBlsSignature> SignProductionEntryAttestation(
    const bls::SecretKey& key, const uint32_t seat_index,
    const ProductionEntryCore& entry, const ActiveFnBlsSeatSet& active_seats,
    ProductionSigningGuard& guard, ProductionLockResult& lock_result)
{
    if (seat_index >= active_seats.Size() ||
        CheckActiveFnBlsSeatSet(entry.domain, active_seats) != BlsSeatSetCheck::OK ||
        entry.market_id != active_seats.market_id ||
        entry.epoch != active_seats.epoch ||
        entry.seat_set_hash != active_seats.set_hash ||
        key.GetPublicKey() != active_seats.members[seat_index].key.Key()) {
        lock_result = ProductionLockResult::CONFLICT;
        return std::nullopt;
    }
    lock_result = guard.Lock(entry);
    if (!LockPermitsSigning(lock_result)) return std::nullopt;
    const auto signature{SignBlsMicroblockCertificate(
        key, ProductionCertificateContext(entry), active_seats)};
    if (!signature) return std::nullopt;
    return IndexedBlsSignature{seat_index, *signature};
}

ProductionEpochGate::ProductionEpochGate(
    const uint256& domain, const MarketId& market_id,
    const ActiveFnBlsSeatSet& initial_seats)
    : m_domain{domain}, m_market_id{market_id},
      m_active_epoch{initial_seats.epoch},
      m_active_set_hash{initial_seats.set_hash}
{
    m_valid = !domain.IsNull() && !market_id.IsNull() &&
              initial_seats.market_id == market_id &&
              CheckActiveFnBlsSeatSet(domain, initial_seats) == BlsSeatSetCheck::OK;
}

bool ProductionEpochGate::CanExecute(
    const ActiveFnBlsSeatSet& seats) const
{
    return m_valid && !m_paused && !m_pending &&
           seats.epoch == m_active_epoch &&
           seats.market_id == m_market_id &&
           seats.set_hash == m_active_set_hash &&
           CheckActiveFnBlsSeatSet(m_domain, seats) == BlsSeatSetCheck::OK;
}

ProductionEntryCheck ProductionEpochGate::StageHandoff(
    const FlowMeshState& current_state, const ProductionEntryCore& handoff,
    const ActiveFnBlsSeatSet& outgoing_seats,
    const ActiveFnBlsSeatSet& next_seats,
    const BlsMicroblockCertificate& outgoing_certificate,
    const uint64_t expected_sequence, const uint64_t expected_effect_start,
    const uint256& expected_parent,
    const ProductionAnchorContext& anchor_context)
{
    if (!m_valid) return ProductionEntryCheck::INVALID_SEAT_SET;
    if (m_paused || m_pending) return ProductionEntryCheck::EPOCH_PAUSED;
    if (!CanExecute(outgoing_seats)) return ProductionEntryCheck::HANDOFF_NOT_CONNECTED;

    const BlsSeatSetCheck next_check{CheckActiveFnBlsSeatSet(m_domain, next_seats)};
    if (next_check == BlsSeatSetCheck::TOO_SMALL ||
        next_check == BlsSeatSetCheck::TOO_LARGE) {
        // This is an anchored epoch-boundary fact supplied by the caller. Do
        // not let the outgoing epoch continue past an unsatisfied membership
        // floor/cap.
        m_paused = true;
        return ProductionEntryCheck::INVALID_NEXT_SEAT_SET;
    }
    if (next_check != BlsSeatSetCheck::OK) {
        return ProductionEntryCheck::INVALID_NEXT_SEAT_SET;
    }

    const ProductionEntryCheck check{CheckHandoff(
        current_state, handoff, m_domain, m_market_id, outgoing_seats,
        next_seats, outgoing_certificate, expected_sequence,
        expected_effect_start, expected_parent, anchor_context)};
    if (check != ProductionEntryCheck::OK) return check;

    m_pending = Pending{handoff.GetHash(), next_seats.epoch, next_seats.set_hash};
    m_paused = true;
    return ProductionEntryCheck::OK;
}

bool ProductionEpochGate::MarkHandoffB3Connected(
    const uint256& handoff_entry_hash)
{
    if (!m_pending || m_pending->handoff_hash != handoff_entry_hash) return false;
    m_active_epoch = m_pending->next_epoch;
    m_active_set_hash = m_pending->next_set_hash;
    m_pending.reset();
    m_paused = false;
    return true;
}

} // namespace flowmesh
