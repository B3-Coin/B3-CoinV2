// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_CORE_ACTION_H
#define B3_FLOWMESH2_CORE_ACTION_H

#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/codec.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <pubkey.h>
#include <serialize.h>
#include <uint256.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <span>
#include <string_view>
#include <vector>

/** The signed user action of FlowMesh V2 (slice R4; plan section 3.6, PLAN-1).
 *
 * Wire format (Bitcoin serialization, little-endian integers):
 *
 *   u8     version     = ACTION_VERSION (1)
 *   32 B   domain      = DomainHash(network, fm_version) (core/params.h)
 *   32 B   signer      x-only BIP340 public key
 *   u64    nonce       D7 window rule (core/nonce_rule.h)
 *   u64    ref_slot    a certified slot height the client has seen
 *   u16    max_delay   valid in slot s iff ref_slot < s <= ref_slot + max_delay
 *   u8     kind        ActionKind (append-only registry below)
 *   bytes  payload     CompactSize length, then the kind-specific payload
 *   64 B   sig         BIP340 signature
 *
 * The first 84 bytes have fixed width, so `kind` is always byte 83.
 *
 * Identity and authentication (O-8):
 * - UnsignedMsg() is the encoding of every field except `sig`; Encode() is
 *   UnsignedMsg() followed by the 64 signature bytes.
 * - Id() = Tagged(ACTION, UnsignedMsg()). The signature is excluded, so two
 *   valid signatures over one action share one ActionId (they are still two
 *   distinct originals; see ActionLeaf in core/body.h).
 * - The BIP340 message is Id(): VerifySignature() is
 *   XOnlyPubKey::VerifySchnorr(Id(), sig) (libsecp256k1 through pubkey.h).
 * - Account() = AccountId::FromKey(signer). OrderId of a PLACE = its Id().
 *
 * Decoding is exact and capped (core/codec.h): input longer than
 * MaxEncodedSize(profile) or a payload longer than
 * profile.max_action_payload_bytes is OVERSIZE; an unknown version, an
 * unknown kind, truncated input and trailing bytes are DECODE. The two
 * envelope refusals carry the static details ACTION_UNKNOWN_VERSION and
 * ACTION_UNKNOWN_KIND, so admission (R12) can answer WRONG_DOMAIN or
 * UNKNOWN_KIND instead of MALFORMED. Decode does not check the domain, the
 * expiry window or the signature; those are separate, cheaper-first steps
 * (ARCHITECTURE.md section 10.1). Encode() is total, so an action with an
 * over-cap payload still has an Id, but Decode refuses its bytes.
 *
 * Only VerifiedAction::Verify (or the batch path M4-ADM-01) makes a
 * VerifiedAction, so code that takes a VerifiedAction can only see actions
 * whose BIP340 signature was checked (TRD-17: authenticate before execution).
 *
 * Deterministic: no clock, randomness, I/O or locale. Verification reads the
 * static libsecp256k1 context only.
 */
namespace flowmesh2 {

inline constexpr uint8_t ACTION_VERSION{1};
inline constexpr size_t ACTION_SIG_BYTES{64};
//! version + domain + signer + nonce + ref_slot + max_delay + kind.
inline constexpr size_t ACTION_FIXED_PREFIX_BYTES{1 + 32 + 32 + 8 + 8 + 2 + 1};
//! Position of the kind byte in every encoding.
inline constexpr size_t ACTION_KIND_OFFSET{ACTION_FIXED_PREFIX_BYTES - 1};
//! Longest WITHDRAW destination script. PROVISIONAL; M2-LED-05 owns withdrawals.
inline constexpr size_t MAX_WITHDRAW_SCRIPT_BYTES{128};

//! Static error details of ActionV2::Decode envelope refusals (code DECODE).
inline constexpr std::string_view ACTION_UNKNOWN_VERSION{"unknown ActionV2 version"};
inline constexpr std::string_view ACTION_UNKNOWN_KIND{"unknown ActionV2 kind"};

/** Action kinds. APPEND-ONLY registry, owner-protected; zero is never a
 * value. The order follows plan section 3.6 (PLACE, CANCEL, CANCEL_ALL,
 * WITHDRAW, TRANSFER, ...). Only PLACE, CANCEL and TRANSFER execute in the
 * outline (KindSupported); the others are refused before execution
 * (CANCEL_ALL: M3-ENG-09, WITHDRAW: M2-LED-05, futures transfers: M2-LED-06).
 */
enum class ActionKind : uint8_t {
    PLACE = 1,
    CANCEL = 2,
    CANCEL_ALL = 3,
    WITHDRAW = 4,
    TRANSFER = 5,
    SPOT_TO_FUTURES = 6,
    FUTURES_TO_SPOT = 7,
};

//! Order side. APPEND-ONLY registry; zero is never a value.
enum class Side : uint8_t {
    BUY = 1,
    SELL = 2,
};

//! Time in force. APPEND-ONLY registry; zero is never a value.
enum class TimeInForce : uint8_t {
    GTC = 1,       //!< match, then rest the remainder
    IOC = 2,       //!< match, then release the remainder
    POST_ONLY = 3, //!< rest only; crossing gives REJ_WOULD_CROSS
};

bool IsKnown(ActionKind kind);
bool IsKnown(Side side);
bool IsKnown(TimeInForce tif);
//! Registry names; "UNKNOWN" for an unregistered value.
std::string_view ToString(ActionKind kind);
std::string_view ToString(Side side);
std::string_view ToString(TimeInForce tif);

//! True for the kinds the outline executes: PLACE, CANCEL and TRANSFER.
bool KindSupported(ActionKind kind);

namespace detail {
template <class Enum>
Enum ReadEnumByte(uint8_t byte, const char* what)
{
    const Enum value{static_cast<Enum>(byte)};
    if (!IsKnown(value)) throw std::ios_base::failure(what);
    return value;
}
void CheckAtomsOnWire(Atoms amount);
} // namespace detail

/** PLACE payload, 50 bytes: market | u8 side | u8 tif | u64 price_ticks | u64 lots.
 * Zero price or lots decodes; the engine rejects it (REJ_BAD_PARAMS, R7).
 */
struct PlacePayload {
    MarketId market;
    Side side{Side::BUY};
    TimeInForce tif{TimeInForce::GTC};
    uint64_t price_ticks{0};
    uint64_t lots{0};

    static constexpr size_t ENCODED_BYTES{32 + 1 + 1 + 8 + 8};
    friend bool operator==(const PlacePayload&, const PlacePayload&) = default;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << market << static_cast<uint8_t>(side) << static_cast<uint8_t>(tif) << price_ticks << lots;
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        uint8_t side_byte{0};
        uint8_t tif_byte{0};
        s >> market >> side_byte >> tif_byte >> price_ticks >> lots;
        side = detail::ReadEnumByte<Side>(side_byte, "unknown Side");
        tif = detail::ReadEnumByte<TimeInForce>(tif_byte, "unknown TimeInForce");
    }
};

//! CANCEL payload, 32 bytes: the OrderId (the PLACE's ActionId) to cancel.
struct CancelPayload {
    OrderId order;

    static constexpr size_t ENCODED_BYTES{32};
    friend bool operator==(const CancelPayload&, const CancelPayload&) = default;
    SERIALIZE_METHODS(CancelPayload, obj) { READWRITE(obj.order); }
};

/** TRANSFER payload, 72 bytes: to | asset | i64 amount. The amount must be in
 * [0, MAX_ATOMS] on the wire (DECODE otherwise). Zero, self-transfer and an
 * unknown recipient are certified engine rejections (O-7, R7).
 */
struct TransferPayload {
    AccountId to;
    AssetId asset;
    Atoms amount{0};

    static constexpr size_t ENCODED_BYTES{32 + 32 + 8};
    friend bool operator==(const TransferPayload&, const TransferPayload&) = default;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << to << asset << amount;
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> to >> asset >> amount;
        detail::CheckAtomsOnWire(amount);
    }
};

/** WITHDRAW payload: asset | i64 amount | CompactSize-capped destination
 * script (at most MAX_WITHDRAW_SCRIPT_BYTES). It decodes; executing it is
 * M2-LED-05, and admission refuses the kind until then.
 */
struct WithdrawPayload {
    AssetId asset;
    Atoms amount{0};
    std::vector<unsigned char> destination_script;

    static constexpr size_t MAX_ENCODED_BYTES{32 + 8 + 1 + MAX_WITHDRAW_SCRIPT_BYTES};
    friend bool operator==(const WithdrawPayload&, const WithdrawPayload&) = default;

    template <typename Stream>
    void Serialize(Stream& s) const;
    template <typename Stream>
    void Unserialize(Stream& s);
};

/** Canonical payload bytes. INVALID_INPUT for a value the decoder would
 * refuse (an unknown side or time in force, an amount outside [0, MAX_ATOMS]),
 * OVERSIZE for a destination script above MAX_WITHDRAW_SCRIPT_BYTES.
 */
Result<std::vector<unsigned char>> EncodePayload(const PlacePayload& payload);
Result<std::vector<unsigned char>> EncodePayload(const CancelPayload& payload);
Result<std::vector<unsigned char>> EncodePayload(const TransferPayload& payload);
Result<std::vector<unsigned char>> EncodePayload(const WithdrawPayload& payload);

struct ActionV2 {
    uint8_t version{ACTION_VERSION};
    uint256 domain;
    XOnlyPubKey signer;
    uint64_t nonce{0};
    uint64_t ref_slot{0};
    uint16_t max_delay{0};
    ActionKind kind{ActionKind::PLACE};
    std::vector<unsigned char> payload;
    std::array<unsigned char, ACTION_SIG_BYTES> sig{};

    friend bool operator==(const ActionV2&, const ActionV2&) = default;

    //! Every field except the signature, in wire order.
    std::vector<unsigned char> UnsignedMsg() const;
    //! UnsignedMsg() || sig. Total: never refuses.
    std::vector<unsigned char> Encode() const;
    //! Longest encoding Decode accepts under `profile`.
    static size_t MaxEncodedSize(const ProfileParams& profile);
    //! Exact, capped decode (see file comment).
    static Result<ActionV2> Decode(std::span<const unsigned char> bytes, const ProfileParams& profile);

    //! Tagged(ACTION, UnsignedMsg()).
    ActionId Id() const;
    //! AccountId::FromKey(signer).
    AccountId Account() const;
    //! XOnlyPubKey::VerifySchnorr(Id(), sig); false for an invalid key.
    bool VerifySignature() const;
    /** 1 <= max_delay <= profile.max_delay_slots. A zero window can never
     * include the action and a longer one breaks the outcome-index retention
     * bound, so admission answers BAD_EXPIRY for false (R12).
     */
    bool MaxDelayValid(const ProfileParams& profile) const;

    //! Payload decoders: INVALID_INPUT if `kind` differs, else the exact decode
    //! of `payload` (DECODE, or OVERSIZE above the payload's size).
    Result<PlacePayload> Place() const;
    Result<CancelPayload> Cancel() const;
    Result<TransferPayload> Transfer() const;
    Result<WithdrawPayload> Withdraw() const;

    template <typename Stream>
    void SerializeUnsigned(Stream& s) const
    {
        s << version << domain << signer << nonce << ref_slot << max_delay << static_cast<uint8_t>(kind);
        WriteCompactSize(s, payload.size());
        s.write(MakeByteSpan(payload));
    }
};

class BatchVerifier;

/** An ActionV2 whose BIP340 signature has been verified. The constructor is
 * private: Verify() is the only public way to obtain one, and BatchVerifier
 * (M4-ADM-01) the only other. Copies stay verified (the action is immutable
 * through this type).
 */
class VerifiedAction
{
public:
    //! VERIFY_FAILED unless action.VerifySignature().
    static Result<VerifiedAction> Verify(ActionV2 action);

    const ActionV2& Action() const { return m_action; }
    const ActionId& Id() const { return m_id; }
    const AccountId& Account() const { return m_account; }

    friend bool operator==(const VerifiedAction&, const VerifiedAction&) = default;

private:
    friend class BatchVerifier;
    VerifiedAction(ActionV2 action, const ActionId& id, const AccountId& account);

    ActionV2 m_action;
    ActionId m_id;
    AccountId m_account;
};

/** Batch BIP340 verification for admission (plan section 3.2): CCheckQueue
 * workers plus a CuckooCache of verified (ActionId, signature) pairs. The
 * outer error means the batch could not run; each inner Result is one
 * action's verdict, in input order. Outline stub: NotImplemented(M4-ADM-01).
 */
class BatchVerifier
{
public:
    Result<std::vector<Result<VerifiedAction>>> VerifyBatch(std::vector<ActionV2> actions) const;
};

template <typename Stream>
void WithdrawPayload::Serialize(Stream& s) const
{
    s << asset << amount;
    WriteCappedVector(s, destination_script, MAX_WITHDRAW_SCRIPT_BYTES);
}

template <typename Stream>
void WithdrawPayload::Unserialize(Stream& s)
{
    s >> asset >> amount;
    detail::CheckAtomsOnWire(amount);
    ReadCappedVector(s, destination_script, MAX_WITHDRAW_SCRIPT_BYTES);
}

} // namespace flowmesh2

#endif // B3_FLOWMESH2_CORE_ACTION_H
