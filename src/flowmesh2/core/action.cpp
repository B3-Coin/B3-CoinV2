// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/core/action.h>

#include <flowmesh2/core/codec.h>
#include <flowmesh2/core/hashes.h>
#include <streams.h>

#include <utility>

namespace flowmesh2 {

namespace detail {
void CheckAtomsOnWire(Atoms amount)
{
    if (!InAtomRange(amount)) throw std::ios_base::failure("amount outside [0, MAX_ATOMS]");
}
} // namespace detail

bool IsKnown(ActionKind kind)
{
    switch (kind) {
    case ActionKind::PLACE:
    case ActionKind::CANCEL:
    case ActionKind::CANCEL_ALL:
    case ActionKind::WITHDRAW:
    case ActionKind::TRANSFER:
    case ActionKind::SPOT_TO_FUTURES:
    case ActionKind::FUTURES_TO_SPOT:
        return true;
    }
    return false;
}

bool IsKnown(Side side) { return side == Side::BUY || side == Side::SELL; }

bool IsKnown(TimeInForce tif)
{
    return tif == TimeInForce::GTC || tif == TimeInForce::IOC || tif == TimeInForce::POST_ONLY;
}

std::string_view ToString(ActionKind kind)
{
    switch (kind) {
    case ActionKind::PLACE: return "PLACE";
    case ActionKind::CANCEL: return "CANCEL";
    case ActionKind::CANCEL_ALL: return "CANCEL_ALL";
    case ActionKind::WITHDRAW: return "WITHDRAW";
    case ActionKind::TRANSFER: return "TRANSFER";
    case ActionKind::SPOT_TO_FUTURES: return "SPOT_TO_FUTURES";
    case ActionKind::FUTURES_TO_SPOT: return "FUTURES_TO_SPOT";
    }
    return "UNKNOWN";
}

std::string_view ToString(Side side)
{
    switch (side) {
    case Side::BUY: return "BUY";
    case Side::SELL: return "SELL";
    }
    return "UNKNOWN";
}

std::string_view ToString(TimeInForce tif)
{
    switch (tif) {
    case TimeInForce::GTC: return "GTC";
    case TimeInForce::IOC: return "IOC";
    case TimeInForce::POST_ONLY: return "POST_ONLY";
    }
    return "UNKNOWN";
}

bool KindSupported(ActionKind kind)
{
    return kind == ActionKind::PLACE || kind == ActionKind::CANCEL || kind == ActionKind::TRANSFER;
}

bool KindSupported(ActionKind kind, const ProfileParams& profile)
{
    return KindSupported(kind) || (kind == ActionKind::WITHDRAW && profile.withdrawals_enabled);
}

namespace {

Error InvalidPayload(const char* detail) { return Error{ErrorCode::INVALID_INPUT, detail}; }

template <class Payload>
Result<Payload> DecodePayload(const ActionV2& action, ActionKind expected, size_t max_bytes)
{
    if (action.kind != expected) return Error{ErrorCode::INVALID_INPUT, "payload requested for another action kind"};
    return DecodeExact<Payload>(action.payload, max_bytes);
}

} // namespace

Result<std::vector<unsigned char>> EncodePayload(const PlacePayload& payload)
{
    if (!IsKnown(payload.side)) return InvalidPayload("unknown Side");
    if (!IsKnown(payload.tif)) return InvalidPayload("unknown TimeInForce");
    return Ok(EncodeBytes(payload));
}

Result<std::vector<unsigned char>> EncodePayload(const CancelPayload& payload) { return Ok(EncodeBytes(payload)); }

Result<std::vector<unsigned char>> EncodePayload(const TransferPayload& payload)
{
    if (!InAtomRange(payload.amount)) return InvalidPayload("transfer amount outside [0, MAX_ATOMS]");
    return Ok(EncodeBytes(payload));
}

Result<std::vector<unsigned char>> EncodePayload(const WithdrawPayload& payload)
{
    if (!InAtomRange(payload.amount)) return InvalidPayload("withdraw amount outside [0, MAX_ATOMS]");
    if (payload.destination_script.size() > MAX_WITHDRAW_SCRIPT_BYTES) {
        return Error{ErrorCode::OVERSIZE, "destination script above MAX_WITHDRAW_SCRIPT_BYTES"};
    }
    return Ok(EncodeBytes(payload));
}

std::vector<unsigned char> ActionV2::UnsignedMsg() const
{
    std::vector<unsigned char> out;
    VectorWriter writer{out, 0};
    SerializeUnsigned(writer);
    return out;
}

std::vector<unsigned char> ActionV2::Encode() const
{
    std::vector<unsigned char> out{UnsignedMsg()};
    out.insert(out.end(), sig.begin(), sig.end());
    return out;
}

size_t ActionV2::MaxEncodedSize(const ProfileParams& profile)
{
    const size_t cap{profile.max_action_payload_bytes};
    return ACTION_FIXED_PREFIX_BYTES + GetSizeOfCompactSize(cap) + cap + ACTION_SIG_BYTES;
}

Result<ActionV2> ActionV2::Decode(std::span<const unsigned char> bytes, const ProfileParams& profile)
{
    const size_t max_bytes{MaxEncodedSize(profile)};
    if (bytes.size() > max_bytes) return Error{ErrorCode::OVERSIZE, "ActionV2 longer than its maximum encoding"};
    // The envelope bytes have fixed positions, so the two typed refusals are
    // decided before parsing (admission maps them to WRONG_DOMAIN/UNKNOWN_KIND).
    if (!bytes.empty() && bytes[0] != ACTION_VERSION) return Error{ErrorCode::DECODE, ACTION_UNKNOWN_VERSION};
    if (bytes.size() > ACTION_KIND_OFFSET && !IsKnown(static_cast<ActionKind>(bytes[ACTION_KIND_OFFSET]))) {
        return Error{ErrorCode::DECODE, ACTION_UNKNOWN_KIND};
    }
    const size_t payload_cap{profile.max_action_payload_bytes};
    return DecodeExactWith(bytes, max_bytes, [payload_cap](SpanReader& reader) {
        ActionV2 action;
        reader >> action.version;
        if (action.version != ACTION_VERSION) throw std::ios_base::failure("unknown ActionV2 version");
        uint8_t kind_byte{0};
        reader >> action.domain >> action.signer >> action.nonce >> action.ref_slot >> action.max_delay >> kind_byte;
        action.kind = detail::ReadEnumByte<ActionKind>(kind_byte, "unknown ActionV2 kind");
        ReadCappedVector(reader, action.payload, payload_cap);
        reader >> action.sig;
        return action;
    });
}

ActionId ActionV2::Id() const { return ActionId{Tagged(HashTag::ACTION, UnsignedMsg())}; }

AccountId ActionV2::Account() const { return AccountId::FromKey(signer); }

bool ActionV2::VerifySignature() const { return signer.VerifySchnorr(Id().id, sig); }

bool ActionV2::MaxDelayValid(const ProfileParams& profile) const
{
    return max_delay >= 1 && max_delay <= profile.max_delay_slots;
}

Result<PlacePayload> ActionV2::Place() const
{
    return DecodePayload<PlacePayload>(*this, ActionKind::PLACE, PlacePayload::ENCODED_BYTES);
}

Result<CancelPayload> ActionV2::Cancel() const
{
    return DecodePayload<CancelPayload>(*this, ActionKind::CANCEL, CancelPayload::ENCODED_BYTES);
}

Result<TransferPayload> ActionV2::Transfer() const
{
    return DecodePayload<TransferPayload>(*this, ActionKind::TRANSFER, TransferPayload::ENCODED_BYTES);
}

Result<WithdrawPayload> ActionV2::Withdraw() const
{
    return DecodePayload<WithdrawPayload>(*this, ActionKind::WITHDRAW, WithdrawPayload::MAX_ENCODED_BYTES);
}

VerifiedAction::VerifiedAction(ActionV2 action, const ActionId& id, const AccountId& account)
    : m_action{std::move(action)}, m_id{id}, m_account{account}
{
}

Result<VerifiedAction> VerifiedAction::Verify(ActionV2 action)
{
    const ActionId id{action.Id()};
    if (!action.signer.VerifySchnorr(id.id, action.sig)) {
        return Error{ErrorCode::VERIFY_FAILED, "BIP340 signature does not verify over the ActionId"};
    }
    const AccountId account{action.Account()};
    return Ok(VerifiedAction{std::move(action), id, account});
}

Result<std::vector<Result<VerifiedAction>>> BatchVerifier::VerifyBatch(std::vector<ActionV2> actions) const
{
    (void)actions;
    return NotImplemented("M4-ADM-01"); // TODO(rebirth:M4-ADM-01) CCheckQueue batch BIP340 plus CuckooCache
}

} // namespace flowmesh2
