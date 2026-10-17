// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/sim/sim_client.h>

#include <crypto/sha256.h>
#include <flowmesh2/verify/b3verify.h>

#include <span>
#include <string>
#include <utility>

namespace flowmesh2::sim {

namespace {

uint256 LabelHash(std::string_view label)
{
    uint256 out;
    CSHA256{}.Write(reinterpret_cast<const unsigned char*>(label.data()), label.size()).Finalize(out.data());
    return out;
}

} // namespace

SimClient::SimClient(std::string name, const ProfileParams& profile, const uint256& domain, const SeatSet& trusted,
                     std::vector<MarketDescriptor> markets, CKey key)
    : m_name{std::move(name)},
      m_profile{profile},
      m_profile_hash{ProfileHash(profile)},
      m_domain{domain},
      m_trusted{trusted},
      m_markets{std::move(markets)},
      m_key{std::move(key)},
      m_xonly{m_key.GetPubKey()},
      m_account{AccountId::FromKey(m_xonly)}
{
}

namespace {

//! The TEST key of client `name`: secret SHA256("fm2/sim/<name>").
Result<CKey> ClientKey(std::string_view name)
{
    const uint256 secret{LabelHash("fm2/sim/" + std::string{name})};
    CKey key;
    key.Set(secret.begin(), secret.end(), /*fCompressedIn=*/true);
    if (!key.IsValid()) return Error{ErrorCode::INVALID_INPUT, CLIENT_KEY};
    return Ok(std::move(key));
}

} // namespace

Result<AccountId> SimClient::AccountOf(std::string_view name)
{
    Result<CKey> key{ClientKey(name)};
    if (!key.has_value()) return key.error();
    return Ok(AccountId::FromKey(XOnlyPubKey{key->GetPubKey()}));
}

Result<SimClient> SimClient::Create(std::string name, const ProfileParams& profile, const uint256& domain, const SeatSet& trusted,
                                    std::vector<MarketDescriptor> markets)
{
    Result<CKey> key{ClientKey(name)};
    if (!key.has_value()) return key.error();
    return Ok(SimClient{std::move(name), profile, domain, trusted, std::move(markets), std::move(key).value()});
}

Result<std::vector<unsigned char>> SimClient::SignOnce(const std::string& label, ActionV2 action)
{
    if (m_outbox.contains(label)) return Error{ErrorCode::INVALID_INPUT, CLIENT_LABEL_TAKEN};
    action.signer = m_xonly;
    const ActionId id{action.Id()};
    const uint256 aux{LabelHash("fm2/sim/aux/" + m_name + "/" + label)};
    if (!m_key.SignSchnorr(id.id, action.sig, /*merkle_root=*/nullptr, aux)) return Error{ErrorCode::INVARIANT, CLIENT_SIGN_FAILED};
    std::vector<unsigned char> bytes{action.Encode()};
    m_outbox.emplace(label, bytes);
    return Ok(std::move(bytes));
}

Result<std::vector<unsigned char>> SimClient::Place(const std::string& label, uint64_t nonce, const MarketDescriptor& market, Side side,
                                                    TimeInForce tif, uint64_t price_ticks, uint64_t lots, uint64_t ref_slot,
                                                    uint16_t max_delay, std::optional<uint256> domain)
{
    Result<std::vector<unsigned char>> payload{EncodePayload(PlacePayload{market.Id(), side, tif, price_ticks, lots})};
    if (!payload.has_value()) return payload.error();
    ActionV2 action;
    action.domain = domain.value_or(m_domain);
    action.nonce = nonce;
    action.ref_slot = ref_slot;
    action.max_delay = max_delay;
    action.kind = ActionKind::PLACE;
    action.payload = std::move(payload).value();
    return SignOnce(label, std::move(action));
}

Result<std::vector<unsigned char>> SimClient::Cancel(const std::string& label, uint64_t nonce, const OrderId& order, uint64_t ref_slot)
{
    Result<std::vector<unsigned char>> payload{EncodePayload(CancelPayload{order})};
    if (!payload.has_value()) return payload.error();
    ActionV2 action;
    action.domain = m_domain;
    action.nonce = nonce;
    action.ref_slot = ref_slot;
    action.max_delay = 16;
    action.kind = ActionKind::CANCEL;
    action.payload = std::move(payload).value();
    return SignOnce(label, std::move(action));
}

Result<std::vector<unsigned char>> SimClient::Transfer(const std::string& label, uint64_t nonce, const AccountId& to, const AssetId& asset,
                                                       Atoms amount, uint64_t ref_slot)
{
    Result<std::vector<unsigned char>> payload{EncodePayload(TransferPayload{to, asset, amount})};
    if (!payload.has_value()) return payload.error();
    ActionV2 action;
    action.domain = m_domain;
    action.nonce = nonce;
    action.ref_slot = ref_slot;
    action.max_delay = 16;
    action.kind = ActionKind::TRANSFER;
    action.payload = std::move(payload).value();
    return SignOnce(label, std::move(action));
}

Result<std::vector<unsigned char>> SimClient::Withdraw(const std::string& label, uint64_t nonce, const AssetId& asset, Atoms amount,
                                                       std::vector<unsigned char> destination_script, uint64_t ref_slot)
{
    Result<std::vector<unsigned char>> payload{EncodePayload(WithdrawPayload{asset, amount, std::move(destination_script)})};
    if (!payload.has_value()) return payload.error();
    ActionV2 action;
    action.domain = m_domain;
    action.nonce = nonce;
    action.ref_slot = ref_slot;
    action.max_delay = 16;
    action.kind = ActionKind::WITHDRAW;
    action.payload = std::move(payload).value();
    return SignOnce(label, std::move(action));
}

Result<std::vector<unsigned char>> SimClient::Bytes(std::string_view label) const
{
    const auto it{m_outbox.find(label)};
    if (it == m_outbox.end()) return Error{ErrorCode::INVALID_INPUT, CLIENT_LABEL_UNKNOWN};
    return Ok(it->second);
}

Result<ActionV2> SimClient::Action(std::string_view label) const
{
    Result<std::vector<unsigned char>> bytes{Bytes(label)};
    if (!bytes.has_value()) return bytes.error();
    return ActionV2::Decode(*bytes, m_profile);
}

Result<std::vector<unsigned char>> SimClient::TamperedSignature(std::string_view label) const
{
    Result<ActionV2> action{Action(label)};
    if (!action.has_value()) return action.error();
    ActionV2 tampered{std::move(action).value()};
    tampered.sig[0] ^= 1;
    return Ok(tampered.Encode());
}

Result<void> SimClient::VerifyEnvelope(const read::CertifiedEnvelope& envelope) const
{
    if (const Result<void> cert{verify::VerifyCertificate(envelope.header, envelope.cert, m_trusted)}; !cert.has_value()) {
        return cert.error();
    }
    if (envelope.header.domain != m_domain) return Error{ErrorCode::VERIFY_FAILED, CLIENT_HEAD_DOMAIN};
    if (envelope.header.profile_hash != m_profile_hash) return Error{ErrorCode::VERIFY_FAILED, CLIENT_HEAD_PROFILE};
    return Ok();
}

Result<void> SimClient::VerifyHead(const read::ReplyHead& head) const
{
    if (const Result<void> envelope{VerifyEnvelope(head.envelope)}; !envelope.has_value()) return envelope.error();
    const SlotHeaderV2& header{head.envelope.header};
    // The commit identity and scope a C1 lease carries are claims; they must be the certified header's.
    if (head.commit.id != header.Hash() || head.commit.parent != header.parent_hash || head.commit.state_root != header.state_root ||
        head.commit.sequence != header.height) {
        return Error{ErrorCode::VERIFY_FAILED, CLIENT_HEAD_COMMIT};
    }
    if (head.scope.domain != header.domain || head.scope.configuration != header.profile_hash ||
        head.scope.validator_set != header.seat_set_root || head.scope.anchor != header.anchor.hash ||
        head.scope.epoch != header.authority_epoch || head.scope.authority_revision != header.anchor.height) {
        return Error{ErrorCode::VERIFY_FAILED, CLIENT_HEAD_SCOPE};
    }
    if (head.label != read::ReadLabel::CERTIFIED) return Error{ErrorCode::VERIFY_FAILED, CLIENT_HEAD_LABEL};
    return Ok();
}

Result<AccountLeafV0> SimClient::VerifyAccount(const read::AccountReply& reply, const AccountId& account) const
{
    if (const Result<void> head{VerifyHead(reply.head)}; !head.has_value()) return head.error();
    if (reply.status != read::LookupStatus::FOUND || !reply.witness.has_value()) return Error{ErrorCode::VERIFY_FAILED, CLIENT_NOT_FOUND};
    const read::AccountWitness& witness{*reply.witness};
    if (witness.leaf.account != account) return Error{ErrorCode::VERIFY_FAILED, CLIENT_WRONG_ACCOUNT};
    if (const Result<void> leaf{verify::VerifyAccountLeaf(reply.head.envelope.header, witness.leaf, witness.roots, witness.proof, m_profile)};
        !leaf.has_value()) {
        return leaf.error();
    }
    return Ok(witness.leaf);
}

Result<OutcomeV2> SimClient::VerifyIncluded(const read::IncludedOutcome& included, const ActionId& id) const
{
    if (const Result<void> envelope{VerifyEnvelope(included.envelope)}; !envelope.has_value()) return envelope.error();
    const SlotHeaderV2& header{included.envelope.header};
    if (const Result<void> outcome{verify::VerifyOutcome(header, included.outcome.outcome, included.outcome.proof)}; !outcome.has_value()) {
        return outcome.error();
    }
    if (included.outcome.outcome.source != OutcomeSource::ACTION || included.outcome.outcome.subject != id.id) {
        return Error{ErrorCode::VERIFY_FAILED, CLIENT_OUTCOME_SUBJECT};
    }
    if (included.ref.height != header.height || included.ref.index != included.outcome.index ||
        included.outcome.proof.index != included.outcome.index) {
        return Error{ErrorCode::VERIFY_FAILED, CLIENT_REF_MISMATCH};
    }
    return Ok(included.outcome.outcome);
}

Result<void> SimClient::VerifyExpiredReply(std::string_view label, const net::ActionStatusReply& reply) const
{
    Result<ActionV2> action{Action(label)};
    if (!action.has_value()) return action.error();
    if (reply.status != net::ApiActionStatus::EXPIRED || !reply.head.has_value() || !reply.expiry.has_value()) {
        return Error{ErrorCode::VERIFY_FAILED, CLIENT_NOT_EXPIRED};
    }
    if (const Result<void> head{VerifyHead(*reply.head)}; !head.has_value()) return head.error();
    const read::AccountWitness& witness{*reply.expiry};
    return verify::VerifyExpired(*action, reply.head->envelope.header, reply.head->envelope.cert, m_trusted, witness.leaf, witness.roots,
                                 witness.proof, m_profile);
}

Result<void> SimClient::VerifyFills(const OutcomeV2& outcome) const
{
    for (const Fill& fill : outcome.fills) {
        const MarketDescriptor* market{nullptr};
        for (const MarketDescriptor& m : m_markets) {
            if (m.Id() == fill.market) market = &m;
        }
        if (market == nullptr) return Error{ErrorCode::VERIFY_FAILED, CLIENT_UNKNOWN_MARKET};
        if (const Result<void> checked{verify::VerifyFill(fill, *market, m_profile)}; !checked.has_value()) return checked.error();
    }
    return Ok();
}

} // namespace flowmesh2::sim
