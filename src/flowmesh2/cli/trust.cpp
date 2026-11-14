// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/cli/trust.h>

#include <flowmesh2/verify/b3verify.h>

#include <utility>

namespace flowmesh2::cli {

Trust::Trust(ProfileParams profile, uint256 domain, SeatSet seats, std::vector<MarketDescriptor> markets)
    : m_profile{profile}, m_profile_hash{ProfileHash(profile)}, m_domain{domain}, m_seats{std::move(seats)}, m_markets{std::move(markets)}
{
}

Result<Trust> Trust::FromGenesis(const netio::GenesisDoc& genesis)
{
    const Result<ProfileParams> profile{DevRegtestProfile(genesis.network.chain)};
    if (!profile.has_value()) return profile.error();
    Result<SeatSet> seats{genesis.SeatSetOf()};
    if (!seats.has_value()) return seats.error();
    Result<std::vector<MarketDescriptor>> markets{genesis.Descriptors()};
    if (!markets.has_value()) return markets.error();
    return Ok(Trust{*profile, genesis.Domain(), std::move(*seats), std::move(*markets)});
}

Result<void> Trust::VerifyEnvelope(const read::CertifiedEnvelope& envelope) const
{
    if (const Result<void> cert{verify::VerifyCertificate(envelope.header, envelope.cert, m_seats)}; !cert.has_value()) return cert.error();
    if (envelope.header.domain != m_domain) return Error{ErrorCode::VERIFY_FAILED, TRUST_HEAD_DOMAIN};
    if (envelope.header.profile_hash != m_profile_hash) return Error{ErrorCode::VERIFY_FAILED, TRUST_HEAD_PROFILE};
    return Ok();
}

Result<void> Trust::VerifyHead(const read::ReplyHead& head) const
{
    if (const Result<void> envelope{VerifyEnvelope(head.envelope)}; !envelope.has_value()) return envelope.error();
    const SlotHeaderV2& header{head.envelope.header};
    // The commit identity and scope a reply carries are claims; they must be the certified header's.
    if (head.commit.id != header.Hash() || head.commit.parent != header.parent_hash || head.commit.state_root != header.state_root ||
        head.commit.sequence != header.height) {
        return Error{ErrorCode::VERIFY_FAILED, TRUST_HEAD_COMMIT};
    }
    if (head.scope.domain != header.domain || head.scope.configuration != header.profile_hash ||
        head.scope.validator_set != header.seat_set_root || head.scope.anchor != header.anchor.hash ||
        head.scope.epoch != header.authority_epoch || head.scope.authority_revision != header.anchor.height) {
        return Error{ErrorCode::VERIFY_FAILED, TRUST_HEAD_SCOPE};
    }
    if (head.label != read::ReadLabel::CERTIFIED) return Error{ErrorCode::VERIFY_FAILED, TRUST_HEAD_LABEL};
    return Ok();
}

Result<AccountLeafV0> Trust::VerifyAccount(const read::ReplyHead& head, const read::AccountWitness& witness, const AccountId& account) const
{
    if (const Result<void> verified{VerifyHead(head)}; !verified.has_value()) return verified.error();
    if (witness.leaf.account != account) return Error{ErrorCode::VERIFY_FAILED, TRUST_WRONG_ACCOUNT};
    if (const Result<void> leaf{verify::VerifyAccountLeaf(head.envelope.header, witness.leaf, witness.roots, witness.proof, m_profile)};
        !leaf.has_value()) {
        return leaf.error();
    }
    return Ok(witness.leaf);
}

Result<OutcomeV2> Trust::VerifyIncluded(const read::IncludedOutcome& included, const ActionId& id) const
{
    if (const Result<void> envelope{VerifyEnvelope(included.envelope)}; !envelope.has_value()) return envelope.error();
    const SlotHeaderV2& header{included.envelope.header};
    if (const Result<void> outcome{verify::VerifyOutcome(header, included.outcome.outcome, included.outcome.proof)}; !outcome.has_value()) {
        return outcome.error();
    }
    if (included.outcome.outcome.source != OutcomeSource::ACTION || included.outcome.outcome.subject != id.id) {
        return Error{ErrorCode::VERIFY_FAILED, TRUST_OUTCOME_SUBJECT};
    }
    if (included.ref.height != header.height || included.ref.index != included.outcome.index ||
        included.outcome.proof.index != included.outcome.index) {
        return Error{ErrorCode::VERIFY_FAILED, TRUST_REF_MISMATCH};
    }
    if (const Result<void> fills{VerifyFills(included.outcome.outcome)}; !fills.has_value()) return fills.error();
    return Ok(included.outcome.outcome);
}

Result<void> Trust::VerifyExpired(const ActionV2& action, const read::ReplyHead& head, const read::AccountWitness& witness) const
{
    if (const Result<void> verified{VerifyHead(head)}; !verified.has_value()) return verified.error();
    return verify::VerifyExpired(action, head.envelope.header, head.envelope.cert, m_seats, witness.leaf, witness.roots, witness.proof, m_profile);
}

Result<void> Trust::VerifyFills(const OutcomeV2& outcome) const
{
    for (const Fill& fill : outcome.fills) {
        const MarketDescriptor* market{nullptr};
        for (const MarketDescriptor& m : m_markets) {
            if (m.Id() == fill.market) market = &m;
        }
        if (market == nullptr) return Error{ErrorCode::VERIFY_FAILED, TRUST_UNKNOWN_MARKET};
        if (const Result<void> checked{verify::VerifyFill(fill, *market, m_profile)}; !checked.has_value()) return checked.error();
    }
    return Ok();
}

} // namespace flowmesh2::cli
