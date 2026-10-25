// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/verify/b3verify.h>

#include <crypto/bls.h>
#include <flowmesh2/core/fee_math.h>
#include <flowmesh2/core/nonce_rule.h>
#include <util/overflow.h>

#include <map>
#include <optional>
#include <utility>

namespace flowmesh2::verify {
namespace {

Error Failed(std::string_view detail) { return Error{ErrorCode::VERIFY_FAILED, detail}; }

//! Validate(profile), then the header must name it.
Result<void> CheckProfile(const SlotHeaderV2& header, const ProfileParams& profile)
{
    if (Result<void> valid{Validate(profile)}; !valid.has_value()) return valid.error();
    if (ProfileHash(profile) != header.profile_hash) return Failed(PROFILE_MISMATCH);
    return Ok();
}

//! Strictly ascending by (account, asset), no all-zero entry: the engine's output form (R7).
bool DeltasCanonical(std::span<const BalanceDelta> deltas)
{
    for (size_t i{0}; i < deltas.size(); ++i) {
        const BalanceDelta& d{deltas[i]};
        if (d.available_delta == 0 && d.reserved_delta == 0) return false;
        if (i == 0) continue;
        const BalanceDelta& p{deltas[i - 1]};
        if (!(p.account < d.account || (p.account == d.account && p.asset < d.asset))) return false;
    }
    return true;
}

//! Per-asset (available, reserved) running sums for one account.
using Sums = std::map<AssetId, std::pair<int64_t, int64_t>>;

bool AddTo(Sums& sums, const AssetId& asset, int64_t available, int64_t reserved)
{
    auto& [a, r] = sums[asset];
    const std::optional<int64_t> next_a{CheckedAdd(a, available)};
    const std::optional<int64_t> next_r{CheckedAdd(r, reserved)};
    if (!next_a || !next_r) return false;
    a = *next_a;
    r = *next_r;
    return true;
}

} // namespace

Result<void> VerifyCertificate(const SlotHeaderV2& header, const SlotCertificate& cert, const SeatSet& seats)
{
    if (header.seat_set_root != seats.Root()) return Failed(CERT_SEAT_SET);
    if (cert.header_hash != header.Hash()) return Failed(CERT_HEADER);
    const Result<std::vector<uint32_t>> signers{cert.SignerIndices(seats.Size())};
    if (!signers.has_value()) return Failed(CERT_BITMAP);
    if (signers->size() < seats.Quorum()) return Failed(CERT_QUORUM);
    const std::optional<bls::Signature> aggregate{bls::Signature::Decode(cert.aggregate)};
    if (!aggregate) return Failed(CERT_AGGREGATE);
    std::vector<bls::VerifiedPublicKey> keys;
    keys.reserve(signers->size());
    const std::span<const bls::VerifiedPublicKey> all{seats.Keys()};
    for (const uint32_t index : *signers) keys.push_back(all[index]);
    const auto digest{CertificateDigest(header, cert)};
    if (!digest.has_value()) return Failed(CERT_VIEW);
    if (!bls::FastAggregateVerify(keys, std::span<const unsigned char>{digest->begin(), digest->end()}, *aggregate)) {
        return Failed(CERT_SIGNATURE);
    }
    return Ok();
}

Result<void> VerifyOutcome(const SlotHeaderV2& header, const OutcomeV2& outcome, const MerkleProof& proof)
{
    if (proof.size != header.outcome_count) return Failed(OUTCOME_PROOF_SIZE);
    if (!MerkleVerify(outcome.LeafHash(), proof, header.outcomes_root)) return Failed(OUTCOME_PROOF);
    if (!DeltasCanonical(outcome.deltas)) return Failed(OUTCOME_DELTAS);
    return Ok();
}

Result<void> VerifyOutcome(const SlotHeaderV2& header, const OutcomeV2& outcome, const MerkleProof& proof,
                           const ProfileParams& profile)
{
    if (const Result<void> valid{CheckProfile(header, profile)}; !valid.has_value()) return valid.error();
    if (const Result<void> valid{VerifyOutcome(header, outcome, proof)}; !valid.has_value()) return valid.error();
    if (profile.stable_fee_asset.has_value() && outcome.code == OutcomeCode::FEES_DISTRIBUTED) {
        const AssetId& q{*profile.stable_fee_asset};
        if (outcome.subject != q.id) return Failed(FEE_OUTCOME_ASSET);
        for (const BalanceDelta& delta : outcome.deltas) {
            if (delta.asset != q) return Failed(FEE_OUTCOME_ASSET);
            if (delta.available_delta <= 0 || delta.reserved_delta != 0) return Failed(FEE_OUTCOME_DELTA);
        }
    }
    return Ok();
}

Result<void> VerifyAccountLeaf(const SlotHeaderV2& header, const AccountLeafV0& leaf, const StateRootsV0& roots,
                               const MerkleProof& proof, const ProfileParams& profile)
{
    if (Result<void> ok{CheckProfile(header, profile)}; !ok.has_value()) return ok.error();
    const uint8_t expected_version{ExpectedCommitmentVersion(profile)};
    if (header.commitment_version != expected_version || roots.version != expected_version ||
        (expected_version == COMMITMENT_VERSION_V0 && !roots.withdrawals.IsNull())) return Failed(LEAF_STATE_ROOTS);
    if (roots.Root() != header.state_root) return Failed(LEAF_STATE_ROOTS);
    if (!leaf.IsCanonical(profile)) return Failed(LEAF_NOT_CANONICAL);
    if (!MerkleVerify(leaf.LeafValue(), proof, roots.accounts)) return Failed(LEAF_PROOF);
    return Ok();
}

Result<void> VerifyAccountProofV1(const SlotHeaderV2& header, const AccountProofV1& proof, const StateRootsV1& roots,
                                  const ProfileParams& profile)
{
    if (Result<void> ok{CheckProfile(header, profile)}; !ok.has_value()) return ok.error();
    if (header.commitment_version != COMMITMENT_VERSION_SMT) return Failed(V1_NOT_COMMITTED);
    if (roots.Root() != header.state_root) return Failed(LEAF_STATE_ROOTS);
    if (proof.leaf.has_value() && !proof.leaf->IsCanonical(profile)) return Failed(LEAF_NOT_CANONICAL);
    if (!proof.VerifiesUnder(roots.accounts)) return Failed(LEAF_PROOF);
    return Ok();
}

Result<void> VerifyWithdrawal(const SlotHeaderV2& header, const WithdrawalRecord& record, const StateRootsV0& roots,
                              const MerkleProof& proof, const ProfileParams& profile)
{
    if (Result<void> ok{CheckProfile(header, profile)}; !ok.has_value()) return ok.error();
    if (!profile.withdrawals_enabled || !profile.stable_fee_asset.has_value() ||
        header.commitment_version != ExpectedCommitmentVersion(profile) || roots.version != ExpectedCommitmentVersion(profile) ||
        !record.IsCanonical() || record.asset != *profile.stable_fee_asset || record.fee != profile.withdrawal_fee_atoms ||
        record.slot > header.height) return Failed(WITHDRAWAL_POLICY);
    if (roots.Root() != header.state_root) return Failed(LEAF_STATE_ROOTS);
    if (!MerkleVerify(record.LeafValue(), proof, roots.withdrawals)) return Failed(WITHDRAWAL_PROOF);
    return Ok();
}

Result<void> VerifyFill(const Fill& fill, const MarketDescriptor& market, const ProfileParams& profile)
{
    if (Result<void> valid{Validate(profile)}; !valid.has_value()) return valid.error();
    if (const Result<void> policy{ValidateMarketForProfile(market, profile)}; !policy.has_value()) return Failed(FILL_FEE_ASSET);
    if (fill.market != market.Id()) return Failed(FILL_MARKET);
    if (fill.lots == 0) return Failed(FILL_LOTS);
    const Result<Atoms> quote{QuoteAtoms(market, fill.lots, fill.price_ticks)};
    if (!quote.has_value() || *quote != fill.quote_atoms) return Failed(FILL_QUOTE);
    const Result<Atoms> after{AddAtoms(fill.seller_quote_before, fill.quote_atoms)};
    if (!after.has_value()) return Failed(FILL_FEE);
    const Result<Atoms> fee{SellerFeeIncrement(fill.seller_quote_before, *after, profile.fee_ppm)};
    if (!fee.has_value() || *fee != fill.seller_fee) return Failed(FILL_FEE);
    return Ok();
}

Result<void> VerifyBalanceTransition(const AccountLeafV0& before, const AccountLeafV0& after,
                                     std::span<const OutcomeV2> outcomes)
{
    if (before.account != after.account) return Failed(TRANSITION_ACCOUNT);
    // expected = before + deltas, per asset; then compare with after.
    Sums expected;
    for (const AssetBalance& b : before.balances) {
        if (!AddTo(expected, b.asset, b.available, b.reserved)) return Failed(TRANSITION_OVERFLOW);
    }
    for (const OutcomeV2& outcome : outcomes) {
        for (const BalanceDelta& d : outcome.deltas) {
            if (d.account != before.account) continue;
            if (!AddTo(expected, d.asset, d.available_delta, d.reserved_delta)) return Failed(TRANSITION_OVERFLOW);
        }
    }
    for (const AssetBalance& a : after.balances) expected.try_emplace(a.asset, 0, 0);
    for (const auto& [asset, sums] : expected) {
        if (sums.first != after.Available(asset) || sums.second != after.Reserved(asset)) return Failed(TRANSITION_BALANCE);
    }
    return Ok();
}

Result<void> VerifyExpired(const ActionV2& action, const SlotHeaderV2& header, const SlotCertificate& cert,
                           const SeatSet& seats, const AccountLeafV0& leaf, const StateRootsV0& roots,
                           const MerkleProof& proof, const ProfileParams& profile)
{
    if (Result<void> ok{VerifyCertificate(header, cert, seats)}; !ok.has_value()) return ok.error();
    if (Result<void> ok{VerifyAccountLeaf(header, leaf, roots, proof, profile)}; !ok.has_value()) return ok.error();
    if (action.domain != header.domain) return Failed(EXPIRED_DOMAIN);
    if (leaf.account != action.Account()) return Failed(EXPIRED_ACCOUNT);
    if (!action.MaxDelayValid(profile)) return Failed(EXPIRED_MAX_DELAY);
    if (!ExpiredAt(action.ref_slot, action.max_delay, header.height)) return Failed(EXPIRED_NOT_YET);
    const Result<NonceVerdict> verdict{ClassifyNonce(leaf.used_nonces, action.nonce, profile.nonce_window)};
    if (!verdict.has_value()) return verdict.error();
    if (*verdict == NonceVerdict::USED) return Failed(EXPIRED_NONCE_USED);
    if (*verdict == NonceVerdict::TOO_LOW) return Failed(EXPIRED_NONCE_BELOW);
    return Ok();
}

Result<void> VerifyLiabilityPath(const SlotHeaderV2&, const AccountLeafV0&, const LiabilityPath&)
{
    return NotImplemented("M5-VFY-03"); // TODO(rebirth:M5-VFY-03)
}

Result<void> VerifyCheckpoint(const CheckpointClaim&, const SlotHeaderV2&, const SlotCertificate&, const SeatSet&)
{
    return NotImplemented("M8-CKP-01"); // TODO(rebirth:M8-CKP-01)
}

Result<void> VerifyAnchorProof(const SlotHeaderV2&, const AnchorProof&)
{
    return NotImplemented("M7-ADP-02"); // TODO(rebirth:M7-ADP-02)
}

} // namespace flowmesh2::verify
