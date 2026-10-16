// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <crypto/hex_base.h>
#include <flowmesh2/core/action.h>
#include <flowmesh2/core/codec.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/test/util/keys.h>
#include <serialize.h>
#include <util/strencodings.h>

#include <algorithm>
#include <functional>
#include <set>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::test;

// VerifiedAction is constructible only through VerifiedAction::Verify (and the
// BatchVerifier friend, M4-ADM-01): no default, no conversion, no aggregate.
static_assert(!std::is_default_constructible_v<VerifiedAction>);
static_assert(!std::is_constructible_v<VerifiedAction, ActionV2>);
static_assert(!std::is_constructible_v<VerifiedAction, const ActionV2&>);
static_assert(!std::is_constructible_v<VerifiedAction, ActionV2, ActionId, AccountId>);
static_assert(!std::is_constructible_v<VerifiedAction, ActionV2, const ActionId&, const AccountId&>);
static_assert(!std::is_aggregate_v<VerifiedAction>);
static_assert(!std::is_convertible_v<ActionV2, VerifiedAction>);
// Copies of a verified action stay verified.
static_assert(std::is_copy_constructible_v<VerifiedAction>);

namespace {

ProfileParams Profile() { return DevRegtestProfile(ChainType::REGTEST).value(); }

template <class T>
void CheckError(const Result<T>& result, ErrorCode expected)
{
    BOOST_REQUIRE_MESSAGE(!result.has_value(), "unexpected success");
    BOOST_CHECK_EQUAL(ToString(result.error().code), ToString(expected));
}

template <class T>
void CheckErrorDetail(const Result<T>& result, ErrorCode expected, std::string_view detail)
{
    BOOST_REQUIRE_MESSAGE(!result.has_value(), "unexpected success");
    BOOST_CHECK_EQUAL(ToString(result.error().code), ToString(expected));
    BOOST_CHECK_EQUAL(std::string{result.error().detail}, std::string{detail});
}

std::vector<unsigned char> SamplePlacePayload()
{
    return EncodePayload(PlacePayload{MarketId{LabelHash("fm2/test/market")}, Side::SELL, TimeInForce::GTC, 250, 5'000})
        .value();
}

//! The golden action of the Python script: Alice, nonce 7, ref_slot 3, max_delay 16, SELL GTC 5,000 lots at 250.
ActionV2 SampleUnsigned()
{
    ActionV2 a;
    a.version = ACTION_VERSION;
    a.domain = LabelHash("fm2/test/domain");
    a.signer = XOnlyFromLabel("fm2/test/alice");
    a.nonce = 7;
    a.ref_slot = 3;
    a.max_delay = 16;
    a.kind = ActionKind::PLACE;
    a.payload = SamplePlacePayload();
    return a;
}

void Sign(ActionV2& action, std::string_view key_label, std::string_view aux_label)
{
    action.sig = SignDigest(KeyFromLabel(key_label), action.Id().id, LabelHash(aux_label));
}

ActionV2 SampleSigned()
{
    ActionV2 a{SampleUnsigned()};
    Sign(a, "fm2/test/alice", "fm2/test/aux");
    return a;
}

void Flip(uint256& value) { value.data()[0] ^= 0x01; }

//! One mutation per signed field (the structured binding below counts fields).
std::vector<std::pair<std::string, std::function<void(ActionV2&)>>> FieldMutations()
{
    return {
        {"version", [](ActionV2& a) { ++a.version; }},
        {"domain", [](ActionV2& a) { Flip(a.domain); }},
        {"signer", [](ActionV2& a) { a.signer = XOnlyFromLabel("fm2/test/bob"); }},
        {"nonce", [](ActionV2& a) { ++a.nonce; }},
        {"ref_slot", [](ActionV2& a) { ++a.ref_slot; }},
        {"max_delay", [](ActionV2& a) { ++a.max_delay; }},
        {"kind", [](ActionV2& a) { a.kind = ActionKind::CANCEL; }},
        {"payload byte", [](ActionV2& a) { a.payload.back() ^= 0x01; }},
        {"payload length", [](ActionV2& a) { a.payload.push_back(0); }},
    };
}

} // namespace

BOOST_AUTO_TEST_SUITE(action_tests)

BOOST_AUTO_TEST_CASE(kind_registries_pinned)
{
    BOOST_CHECK_EQUAL(static_cast<int>(ActionKind::PLACE), 1);
    BOOST_CHECK_EQUAL(static_cast<int>(ActionKind::CANCEL), 2);
    BOOST_CHECK_EQUAL(static_cast<int>(ActionKind::CANCEL_ALL), 3);
    BOOST_CHECK_EQUAL(static_cast<int>(ActionKind::WITHDRAW), 4);
    BOOST_CHECK_EQUAL(static_cast<int>(ActionKind::TRANSFER), 5);
    BOOST_CHECK_EQUAL(static_cast<int>(ActionKind::SPOT_TO_FUTURES), 6);
    BOOST_CHECK_EQUAL(static_cast<int>(ActionKind::FUTURES_TO_SPOT), 7);
    BOOST_CHECK_EQUAL(static_cast<int>(Side::BUY), 1);
    BOOST_CHECK_EQUAL(static_cast<int>(Side::SELL), 2);
    BOOST_CHECK_EQUAL(static_cast<int>(TimeInForce::GTC), 1);
    BOOST_CHECK_EQUAL(static_cast<int>(TimeInForce::IOC), 2);
    BOOST_CHECK_EQUAL(static_cast<int>(TimeInForce::POST_ONLY), 3);

    const std::vector<std::string> kind_names{"UNKNOWN", "PLACE", "CANCEL", "CANCEL_ALL", "WITHDRAW",
                                              "TRANSFER", "SPOT_TO_FUTURES", "FUTURES_TO_SPOT", "UNKNOWN"};
    for (int v{0}; v <= 8; ++v) {
        const auto kind{static_cast<ActionKind>(v)};
        BOOST_CHECK_EQUAL(std::string{ToString(kind)}, kind_names[v]);
        BOOST_CHECK_EQUAL(IsKnown(kind), v >= 1 && v <= 7);
    }
    BOOST_CHECK_EQUAL(std::string{ToString(static_cast<ActionKind>(255))}, "UNKNOWN");
    BOOST_CHECK_EQUAL(std::string{ToString(Side::SELL)}, "SELL");
    BOOST_CHECK_EQUAL(std::string{ToString(static_cast<Side>(3))}, "UNKNOWN");
    BOOST_CHECK_EQUAL(std::string{ToString(TimeInForce::POST_ONLY)}, "POST_ONLY");
    BOOST_CHECK_EQUAL(std::string{ToString(static_cast<TimeInForce>(0))}, "UNKNOWN");
    BOOST_CHECK(!IsKnown(static_cast<Side>(0)));
    BOOST_CHECK(!IsKnown(static_cast<TimeInForce>(4)));
}

BOOST_AUTO_TEST_CASE(kind_supported_only_place_cancel_transfer)
{
    std::set<int> supported;
    for (int v{0}; v <= 255; ++v) {
        if (KindSupported(static_cast<ActionKind>(v))) supported.insert(v);
    }
    BOOST_CHECK(supported == (std::set<int>{1, 2, 5}));
}

BOOST_AUTO_TEST_CASE(bip340_sign_verify_round_trip)
{
    const ActionV2 a{SampleSigned()};
    BOOST_CHECK(a.VerifySignature());
    BOOST_CHECK(a.Account() == AccountId::FromKey(XOnlyFromLabel("fm2/test/alice")));

    const auto verified{VerifiedAction::Verify(a)};
    BOOST_REQUIRE(verified.has_value());
    BOOST_CHECK(verified->Action() == a);
    BOOST_CHECK(verified->Id() == a.Id());
    BOOST_CHECK(verified->Account() == a.Account());

    // Round trip through the wire format keeps the signature valid.
    const auto decoded{ActionV2::Decode(a.Encode(), Profile())};
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK(*decoded == a);
    BOOST_CHECK(decoded->VerifySignature());
}

BOOST_AUTO_TEST_CASE(flipped_signature_bit_fails)
{
    const ActionV2 a{SampleSigned()};
    for (size_t bit{0}; bit < ACTION_SIG_BYTES * 8; ++bit) {
        ActionV2 tampered{a};
        tampered.sig[bit / 8] ^= static_cast<unsigned char>(1U << (bit % 8));
        BOOST_CHECK_MESSAGE(!tampered.VerifySignature(), "bit " << bit << " still verifies");
    }
    ActionV2 one_bit{a};
    one_bit.sig[0] ^= 0x01;
    CheckError(VerifiedAction::Verify(one_bit), ErrorCode::VERIFY_FAILED);
}

BOOST_AUTO_TEST_CASE(signature_over_another_domain_fails)
{
    // Signed under domain A, presented under domain B: the ActionId differs.
    ActionV2 a{SampleUnsigned()};
    a.domain = LabelHash("fm2/test/other-domain");
    Sign(a, "fm2/test/alice", "fm2/test/aux");
    BOOST_REQUIRE(a.VerifySignature());
    a.domain = LabelHash("fm2/test/domain");
    BOOST_CHECK(!a.VerifySignature());
    CheckError(VerifiedAction::Verify(a), ErrorCode::VERIFY_FAILED);

    // Signed by Bob, claiming Alice as signer.
    ActionV2 b{SampleUnsigned()};
    Sign(b, "fm2/test/bob", "fm2/test/aux");
    BOOST_CHECK(!b.VerifySignature());

    // An x-only key that is not on the curve never verifies.
    ActionV2 c{SampleSigned()};
    c.signer = XOnlyPubKey{};
    BOOST_CHECK(!c.VerifySignature());
}

BOOST_AUTO_TEST_CASE(every_field_changes_id_and_id_excludes_signature)
{
    const ActionV2 base{SampleSigned()};
    // 9 fields: a new field breaks this line until it gets a mutation.
    const auto& [f1, f2, f3, f4, f5, f6, f7, f8, f9] = base;
    (void)f1, (void)f2, (void)f3, (void)f4, (void)f5, (void)f6, (void)f7, (void)f8, (void)f9;

    std::set<uint256> ids{base.Id().id};
    for (const auto& [name, mutate] : FieldMutations()) {
        ActionV2 changed{base};
        mutate(changed);
        BOOST_CHECK_MESSAGE(ids.insert(changed.Id().id).second, name << ": Id unchanged or repeated");
        BOOST_CHECK_MESSAGE(!changed.VerifySignature(), name << ": the old signature still verifies");
    }

    ActionV2 resigned{base};
    Sign(resigned, "fm2/test/alice", "fm2/test/aux2");
    BOOST_CHECK(resigned.sig != base.sig);
    BOOST_CHECK(resigned.Id() == base.Id());
    resigned.sig = {};
    BOOST_CHECK(resigned.Id() == base.Id());

    // UnsignedMsg is a strict prefix of Encode: everything but the 64 signature bytes.
    const auto encoded{base.Encode()};
    const auto unsigned_msg{base.UnsignedMsg()};
    BOOST_REQUIRE_EQUAL(encoded.size(), unsigned_msg.size() + ACTION_SIG_BYTES);
    BOOST_CHECK(std::equal(unsigned_msg.begin(), unsigned_msg.end(), encoded.begin()));
}

BOOST_AUTO_TEST_CASE(payload_cap_4096_accepted_4097_oversize)
{
    const ProfileParams profile{Profile()};
    BOOST_REQUIRE_EQUAL(profile.max_action_payload_bytes, 4'096U);
    BOOST_CHECK_EQUAL(ActionV2::MaxEncodedSize(profile), 84U + 3U + 4'096U + 64U);

    ActionV2 at_cap{SampleUnsigned()};
    at_cap.payload.assign(4'096, 0xab);
    Sign(at_cap, "fm2/test/alice", "fm2/test/aux");
    BOOST_CHECK_EQUAL(at_cap.Encode().size(), ActionV2::MaxEncodedSize(profile));
    const auto ok{ActionV2::Decode(at_cap.Encode(), profile)};
    BOOST_REQUIRE(ok.has_value());
    BOOST_CHECK(*ok == at_cap);

    ActionV2 over{SampleUnsigned()};
    over.payload.assign(4'097, 0xab);
    Sign(over, "fm2/test/alice", "fm2/test/aux");
    CheckError(ActionV2::Decode(over.Encode(), profile), ErrorCode::OVERSIZE);

    // The payload cap itself (not only the total length) refuses: a smaller
    // profile cap with a payload one byte over it.
    ProfileParams small{profile};
    small.max_action_payload_bytes = 100;
    ActionV2 small_over{SampleUnsigned()};
    small_over.payload.assign(101, 0x01);
    auto bytes{small_over.Encode()};
    bytes.resize(bytes.size() - 10); // still short enough for the total-length check
    BOOST_REQUIRE(bytes.size() <= ActionV2::MaxEncodedSize(small));
    CheckError(ActionV2::Decode(bytes, small), ErrorCode::OVERSIZE);

    // A count prefix above the cap with no payload behind it is OVERSIZE too:
    // the cap is checked before anything is read or allocated.
    std::vector<unsigned char> huge_count{SampleUnsigned().UnsignedMsg()};
    huge_count.resize(ACTION_FIXED_PREFIX_BYTES);
    huge_count.insert(huge_count.end(), {0xfe, 0xff, 0xff, 0xff, 0x7f});
    CheckError(ActionV2::Decode(huge_count, profile), ErrorCode::OVERSIZE);
}

BOOST_AUTO_TEST_CASE(max_delay_zero_and_1025_flagged)
{
    const ProfileParams profile{Profile()};
    for (const uint16_t max_delay : {uint16_t{0}, uint16_t{1}, uint16_t{16}, uint16_t{1'024}, uint16_t{1'025}, uint16_t{65'535}}) {
        ActionV2 a{SampleUnsigned()};
        a.max_delay = max_delay;
        Sign(a, "fm2/test/alice", "fm2/test/aux");
        const bool valid{max_delay >= 1 && max_delay <= 1'024};
        BOOST_CHECK_MESSAGE(a.MaxDelayValid(profile) == valid, "max_delay " << max_delay);
        // Flagged, not a codec error: the envelope still decodes, so admission
        // can answer BAD_EXPIRY (R12) instead of MALFORMED.
        const auto decoded{ActionV2::Decode(a.Encode(), profile)};
        BOOST_REQUIRE(decoded.has_value());
        BOOST_CHECK_EQUAL(decoded->max_delay, max_delay);
    }
}

BOOST_AUTO_TEST_CASE(unknown_kind_version_and_trailing_bytes_refused)
{
    const ProfileParams profile{Profile()};
    const auto good{SampleSigned().Encode()};
    BOOST_REQUIRE(ActionV2::Decode(good, profile).has_value());
    BOOST_REQUIRE_EQUAL(good[ACTION_KIND_OFFSET], static_cast<unsigned char>(ActionKind::PLACE));

    for (const unsigned char kind : {0x00, 0x08, 0x7f, 0xff}) {
        auto bytes{good};
        bytes[ACTION_KIND_OFFSET] = kind;
        CheckErrorDetail(ActionV2::Decode(bytes, profile), ErrorCode::DECODE, ACTION_UNKNOWN_KIND);
    }
    for (const unsigned char version : {0x00, 0x02, 0xff}) {
        auto bytes{good};
        bytes[0] = version;
        CheckErrorDetail(ActionV2::Decode(bytes, profile), ErrorCode::DECODE, ACTION_UNKNOWN_VERSION);
    }

    auto trailing{good};
    trailing.push_back(0x00);
    CheckError(ActionV2::Decode(trailing, profile), ErrorCode::DECODE);

    for (size_t cut : {size_t{1}, size_t{64}, good.size() - 1}) {
        const std::vector<unsigned char> truncated(good.begin(), good.begin() + (good.size() - cut));
        CheckError(ActionV2::Decode(truncated, profile), ErrorCode::DECODE);
    }
    CheckError(ActionV2::Decode({}, profile), ErrorCode::DECODE);

    // A non-canonical CompactSize payload length (0xfd 0x32 0x00 for 50).
    std::vector<unsigned char> non_canonical(good.begin(), good.begin() + ACTION_FIXED_PREFIX_BYTES);
    non_canonical.insert(non_canonical.end(), {0xfd, 0x32, 0x00});
    non_canonical.insert(non_canonical.end(), good.begin() + ACTION_FIXED_PREFIX_BYTES + 1, good.end());
    CheckError(ActionV2::Decode(non_canonical, profile), ErrorCode::DECODE);
}

BOOST_AUTO_TEST_CASE(golden_bytes_id_and_signature)
{
    // Goldens from an independent Python script (stdlib only, BIP340 reference
    // algorithm re-typed from the BIP): evidence/R4/golden.py.
    const ActionV2 a{SampleSigned()};
    BOOST_CHECK_EQUAL(HexStr(std::span<const unsigned char>{a.signer.data(), a.signer.size()}), "e8e45aa57fac27b5313beddb36f3491a9c4672e7bb1672b6fda6d21ed36d58ad");
    BOOST_CHECK_EQUAL(HexStr(a.payload),
                      "629d31dd15ec7d49cb9eb7ec7244a8dbfa8cfdc9da7973340b8eb780e1f8af970201fa000000000000008813000000000000");
    BOOST_CHECK_EQUAL(HexStr(a.Id().id), "fc5a341dc9cd0cd0b65f4c8099e67f1b0c04a710f396061b1c3189e545d26521");
    BOOST_CHECK_EQUAL(HexStr(a.sig),
                      "641108d854e71832bc4ec516d3b4a35748a1e7d0fa5eedd83c4837246c1598db"
                      "4c7390e808ea45f01d89d04ff4cea80e7e75988bad387162aec17c4c80f2d125");
    BOOST_CHECK_EQUAL(HexStr(a.Encode()),
                      "014f9cfaa7394620a6c3a45dac67ecb79cee9af65b59dbcd3095205bb5bed63a7ce8e45aa57fac27b5313beddb36f349"
                      "1a9c4672e7bb1672b6fda6d21ed36d58ad0700000000000000030000000000000010000132629d31dd15ec7d49cb9eb7"
                      "ec7244a8dbfa8cfdc9da7973340b8eb780e1f8af970201fa000000000000008813000000000000641108d854e71832bc"
                      "4ec516d3b4a35748a1e7d0fa5eedd83c4837246c1598db4c7390e808ea45f01d89d04ff4cea80e7e75988bad387162ae"
                      "c17c4c80f2d125");
    BOOST_CHECK_EQUAL(a.Encode().size(), 199U);
    BOOST_CHECK_EQUAL(HexStr(a.Account().id), "f427557954aa05641268e4f692a1545be48df3defd8600eee432ca3604a8a85a");
}

BOOST_AUTO_TEST_CASE(payload_codecs_round_trip_and_refusals)
{
    const PlacePayload place{MarketId{LabelHash("m")}, Side::BUY, TimeInForce::POST_ONLY, 600, 100'000};
    const CancelPayload cancel{OrderId{LabelHash("order")}};
    const TransferPayload transfer{AccountId{LabelHash("to")}, AssetId{LabelHash("rusd")}, MAX_ATOMS};
    const WithdrawPayload withdraw{AssetId{}, 5, std::vector<unsigned char>(MAX_WITHDRAW_SCRIPT_BYTES, 0x51)};

    ActionV2 a{SampleUnsigned()};
    a.kind = ActionKind::PLACE;
    a.payload = EncodePayload(place).value();
    BOOST_CHECK_EQUAL(a.payload.size(), PlacePayload::ENCODED_BYTES);
    BOOST_CHECK(a.Place().value() == place);
    CheckError(a.Cancel(), ErrorCode::INVALID_INPUT);
    CheckError(a.Transfer(), ErrorCode::INVALID_INPUT);
    CheckError(a.Withdraw(), ErrorCode::INVALID_INPUT);

    a.kind = ActionKind::CANCEL;
    a.payload = EncodePayload(cancel).value();
    BOOST_CHECK_EQUAL(a.payload.size(), CancelPayload::ENCODED_BYTES);
    BOOST_CHECK(a.Cancel().value() == cancel);
    CheckError(a.Place(), ErrorCode::INVALID_INPUT);

    a.kind = ActionKind::TRANSFER;
    a.payload = EncodePayload(transfer).value();
    BOOST_CHECK_EQUAL(a.payload.size(), TransferPayload::ENCODED_BYTES);
    BOOST_CHECK(a.Transfer().value() == transfer);

    a.kind = ActionKind::WITHDRAW;
    a.payload = EncodePayload(withdraw).value();
    BOOST_CHECK_EQUAL(a.payload.size(), WithdrawPayload::MAX_ENCODED_BYTES);
    BOOST_CHECK(a.Withdraw().value() == withdraw);

    // Unknown side or time in force: DECODE; the encoder refuses to produce them.
    ActionV2 p{SampleUnsigned()};
    p.payload[32] = 0x03; // side
    CheckError(p.Place(), ErrorCode::DECODE);
    p = SampleUnsigned();
    p.payload[33] = 0x00; // tif
    CheckError(p.Place(), ErrorCode::DECODE);
    p = SampleUnsigned();
    p.payload.push_back(0x00); // trailing payload byte
    CheckError(p.Place(), ErrorCode::OVERSIZE);
    p.payload.resize(PlacePayload::ENCODED_BYTES - 1);
    CheckError(p.Place(), ErrorCode::DECODE);
    CheckError(EncodePayload(PlacePayload{MarketId{}, static_cast<Side>(9), TimeInForce::GTC, 1, 1}), ErrorCode::INVALID_INPUT);
    CheckError(EncodePayload(PlacePayload{MarketId{}, Side::BUY, static_cast<TimeInForce>(0), 1, 1}), ErrorCode::INVALID_INPUT);

    // Amounts outside [0, MAX_ATOMS] are refused on both sides of the codec.
    for (const Atoms bad : {Atoms{-1}, MAX_ATOMS + 1}) {
        CheckError(EncodePayload(TransferPayload{AccountId{}, AssetId{}, bad}), ErrorCode::INVALID_INPUT);
        CheckError(EncodePayload(WithdrawPayload{AssetId{}, bad, {}}), ErrorCode::INVALID_INPUT);
        ActionV2 t{SampleUnsigned()};
        t.kind = ActionKind::TRANSFER;
        t.payload = EncodePayload(transfer).value();
        std::vector<unsigned char> amount;
        VectorWriter{amount, 0, bad};
        std::copy(amount.begin(), amount.end(), t.payload.begin() + 64);
        CheckError(t.Transfer(), ErrorCode::DECODE);
    }

    // Destination script one byte over the cap.
    CheckError(EncodePayload(WithdrawPayload{AssetId{}, 1, std::vector<unsigned char>(MAX_WITHDRAW_SCRIPT_BYTES + 1, 0x51)}),
               ErrorCode::OVERSIZE);
}

BOOST_AUTO_TEST_SUITE_END()
