// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <flowmesh/auth.h>
#include <flowmesh/production_wire.h>
#include <hash.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace {

uint256 Filled(const unsigned char value)
{
    uint256 out;
    std::fill(out.begin(), out.end(), value);
    return out;
}

bls::SecretKey Secret()
{
    // Same deterministic, test-only IKM as flowmesh_production_wire_tests.
    std::array<unsigned char, 32> ikm{};
    for (size_t i{0}; i < ikm.size(); ++i) ikm[i] = i + 1;
    const auto key{bls::SecretKey::FromIKM(ikm)};
    BOOST_REQUIRE(key);
    return *key;
}

flowmesh::Action Action()
{
    flowmesh::Action action;
    action.signer = Filled(9);
    action.sequence = 4;
    action.type = static_cast<uint8_t>(flowmesh::ActionType::CANCEL_BID);
    action.credential = {1, 2, 3};
    return action;
}

flowmesh::ProductionEntryCore Entry()
{
    // Extend the existing production-wire fixture with one semantic action.
    // This is a codec fixture, not a claim of a valid funded market execution.
    flowmesh::ProductionEntryCore entry;
    entry.domain = Filled(1);
    entry.market_id = Filled(2);
    entry.epoch = 3;
    entry.seat_set_hash = Filled(4);
    entry.anchor = {100, Filled(5)};
    entry.previous_state_root = Filled(6);
    auto semantic{Action()};
    semantic.credential.clear();
    entry.actions = {semantic};
    entry.actions_root = flowmesh::ComputeProductionActionsRoot(entry.actions);
    entry.result_root = Filled(7);
    entry.state_root = Filled(8);
    entry.effect_root = modern::EmptyFlowMeshEffectRoot(0);
    return entry;
}

uint256 AttestationDigest(const flowmesh::ProductionEntryCore& entry)
{
    return flowmesh::FlowMeshBlsCertificateDigest(
        {entry.domain, entry.market_id, entry.epoch, entry.seat_set_hash,
         entry.sequence, entry.GetHash()});
}

flowmesh::WireMessage Message(const flowmesh::WireMessageKind kind)
{
    const auto entry{Entry()};
    const auto digest{AttestationDigest(entry)};
    const auto signature{Secret().Sign(std::span<const unsigned char>{digest.begin(), 32})};
    flowmesh::WireMessage message;
    message.kind = kind;
    message.header = {flowmesh::FLOWMESH_WIRE_VERSION_V1,
                      entry.market_id, entry.epoch, entry.sequence};
    std::optional<std::vector<unsigned char>> payload;
    switch (kind) {
    case flowmesh::WireMessageKind::ACTION:
        payload = flowmesh::EncodeProductionActionPayload(Action());
        break;
    case flowmesh::WireMessageKind::PROPOSAL: {
        const uint256 proposal_digest{flowmesh::ProductionProposalDigest(entry, 7)};
        const flowmesh::ProductionProposalEnvelope proposal{
            entry, 7, 2,
            Secret().Sign(std::span<const unsigned char>{proposal_digest.begin(), 32}).Compressed()};
        payload = flowmesh::EncodeProductionProposalPayload(proposal);
        break;
    }
    case flowmesh::WireMessageKind::ATTESTATION:
        payload = flowmesh::EncodeProductionAttestationPayload({3, signature});
        break;
    case flowmesh::WireMessageKind::CERTIFICATE: {
        // Deliberately a structural codec vector, not a quorum proof: a
        // transport must preserve bytes but must NEVER infer eligibility or
        // certificate validity from successful framing/decode alone.
        const flowmesh::ProductionCertifiedEnvelope certified{
            entry, {entry.epoch, entry.sequence, entry.GetHash(), {0x07},
                    signature.Compressed()}};
        payload = flowmesh::EncodeProductionCertifiedPayload(certified, 4);
        break;
    }
    default:
        BOOST_FAIL("unexpected fixture kind");
    }
    BOOST_REQUIRE(payload);
    message.payload = *payload;
    return message;
}

std::vector<unsigned char> Encode(const flowmesh::WireMessage& message)
{
    flowmesh::WireCheck check;
    const auto bytes{flowmesh::EncodeWireMessage(message, check)};
    BOOST_REQUIRE(bytes);
    BOOST_REQUIRE(check == flowmesh::WireCheck::OK);
    return *bytes;
}

} // namespace

BOOST_AUTO_TEST_SUITE(flowmesh_transport_compat_tests)

BOOST_AUTO_TEST_CASE(frozen_v1_inner_bytes_survive_transport_round_trip)
{
    // Frozen from the pre-FMNET a90a40ccc344be26d3007a4b4ca8674150f9e149
    // codecs, not recomputed expected values. Size + SHA256d commits to every
    // byte, including credentials/signatures which semantic IDs omit.
    struct Vector {
        flowmesh::WireMessageKind kind;
        const char* command;
        size_t size;
        const char* sha256d;
    };
    const std::array vectors{
        Vector{flowmesh::WireMessageKind::ACTION, "fmaction", 204,
               "925d516faebb9bfe031e2e1c809dcd861a6cf8f889b103c4eee49820e7eb8eb5"},
        Vector{flowmesh::WireMessageKind::PROPOSAL, "fmprop", 737,
               "05f55af9aa677cce697e8ecf9efe70d814420da619a5c742841249c5335466e4"},
        Vector{flowmesh::WireMessageKind::ATTESTATION, "fmattest", 150,
               "cda242743e7dd19b49e19d5e3cfa97357dd3194d75e6a8199c4d9db38b3b7f92"},
        Vector{flowmesh::WireMessageKind::CERTIFICATE, "fmcert", 782,
               "6b446cf6bc4049bab5e9d774bb79a644bee340f4aa540be8771b00bb822f7679"},
    };
    for (const auto& vector : vectors) {
        BOOST_TEST_CONTEXT(vector.command) {
            const auto message{Message(vector.kind)};
            const auto bytes{Encode(message)};
            BOOST_CHECK_EQUAL(flowmesh::WireCommand(vector.kind), vector.command);
            BOOST_CHECK_EQUAL(bytes.size(), vector.size);
            BOOST_CHECK_EQUAL(Hash(bytes).GetHex(), vector.sha256d);
            BOOST_CHECK_EQUAL(HexStr(std::span{bytes}.first(flowmesh::FLOWMESH_WIRE_HEADER_SIZE)),
                              "00010202020202020202020202020202020202020202020202020202020202020202"
                              "00000000000000030000000000000000");
            flowmesh::WireCheck check;
            const auto decoded{flowmesh::DecodeWireMessage(vector.kind, bytes, check)};
            BOOST_REQUIRE(decoded);
            BOOST_CHECK(check == flowmesh::WireCheck::OK);
            BOOST_CHECK(*decoded == message);
            BOOST_CHECK(Encode(*decoded) == bytes);
        }
    }
}

BOOST_AUTO_TEST_CASE(frozen_semantic_ids_and_signing_digests)
{
    const auto action{Action()};
    const auto entry{Entry()};
    BOOST_CHECK_EQUAL(action.Id().GetHex(),
                      "5f2cc1316a90fd07099238679079e5bb70993162808b477302874eb8162c4e37");
    BOOST_CHECK_EQUAL(flowmesh::ActionSignatureDigest(entry.domain, Filled(0x0a), action).GetHex(),
                      "2cc68d1a72cf5a6c62f1a05774c0f4250ebd87a20bc621a63a24caf63d7a85c7");
    BOOST_CHECK_EQUAL(entry.GetHash().GetHex(),
                      "643daf7c6807b984514d9df2be572f7a5d74e0f6262db446ffa98200dfcb35e5");
    BOOST_CHECK_EQUAL(flowmesh::ProductionProposalDigest(entry, 7).GetHex(),
                      "b0b89ebb71f876d6bf95592e1b8d65cc446b51cf8b1f0beba4b8171b617e76f5");
    BOOST_CHECK_EQUAL(AttestationDigest(entry).GetHex(),
                      "ef4caa78d253fce8c1601de3e7d40a39af7f18a0b6e301af5444089a10c351b1");

    auto changed_evidence{action};
    changed_evidence.credential = {4, 5, 6};
    BOOST_CHECK(changed_evidence.Id() == action.Id());
    BOOST_CHECK(flowmesh::ActionSignatureDigest(entry.domain, Filled(0x0a), changed_evidence) ==
                flowmesh::ActionSignatureDigest(entry.domain, Filled(0x0a), action));
    BOOST_CHECK(flowmesh::EncodeProductionActionPayload(changed_evidence) !=
                flowmesh::EncodeProductionActionPayload(action));

    const auto proposal{flowmesh::DecodeProductionProposalPayload(
        Message(flowmesh::WireMessageKind::PROPOSAL).payload)};
    BOOST_REQUIRE(proposal);
    BOOST_CHECK(proposal->entry.GetHash() == entry.GetHash());
    BOOST_CHECK(proposal->entry.actions.front().credential.empty());
    BOOST_CHECK(flowmesh::ProductionProposalDigest(proposal->entry, proposal->round) ==
                flowmesh::ProductionProposalDigest(entry, 7));
    const auto certificate{flowmesh::DecodeProductionCertifiedPayload(
        Message(flowmesh::WireMessageKind::CERTIFICATE).payload, 4)};
    BOOST_REQUIRE(certificate);
    BOOST_CHECK(certificate->entry.GetHash() == entry.GetHash());
    BOOST_CHECK(AttestationDigest(certificate->entry) == AttestationDigest(entry));
}

BOOST_AUTO_TEST_CASE(frozen_header_integer_endianness)
{
    auto message{Message(flowmesh::WireMessageKind::ACTION)};
    message.header.epoch = 0x0102030405060708ULL;
    message.header.sequence = 0x8899aabbccddeeffULL;
    const auto bytes{Encode(message)};
    BOOST_CHECK_EQUAL(HexStr(std::span{bytes}.subspan(34, 16)),
                      "01020304050607088899aabbccddeeff");
    flowmesh::WireCheck check;
    const auto decoded{flowmesh::DecodeWireMessage(message.kind, bytes, check)};
    BOOST_REQUIRE(decoded);
    BOOST_CHECK(decoded->header == message.header);
}

BOOST_AUTO_TEST_CASE(inner_frame_limits_remain_identical_for_every_transport)
{
    using namespace flowmesh;
    const auto message{Message(WireMessageKind::ACTION)};
    const auto valid{Encode(message)};
    WireCheck check;
    BOOST_CHECK(!WireKindForCommand("fmunknown"));
    BOOST_CHECK(!DecodeWireMessage(message.kind, std::span{valid}.first(49), check));
    BOOST_CHECK(check == WireCheck::WRONG_LENGTH);
    auto wrong_version{valid};
    wrong_version[1] = 2;
    BOOST_CHECK(!DecodeWireMessage(message.kind, wrong_version, check));
    BOOST_CHECK(check == WireCheck::BAD_VERSION);
    auto wrong_market{valid};
    std::fill(wrong_market.begin() + 2, wrong_market.begin() + 34, 0);
    BOOST_CHECK(!DecodeWireMessage(message.kind, wrong_market, check));
    BOOST_CHECK(check == WireCheck::NULL_MARKET);

    for (const auto kind : {WireMessageKind::ACTION, WireMessageKind::PROPOSAL,
                            WireMessageKind::ATTESTATION, WireMessageKind::CERTIFICATE,
                            WireMessageKind::HELLO, WireMessageKind::GET,
                            WireMessageKind::ENTRIES}) {
        auto oversized{message};
        oversized.kind = kind;
        oversized.payload.assign(PayloadLimitForWireKind(kind) + 1, 0);
        BOOST_CHECK(!EncodeWireMessage(oversized, check));
        BOOST_CHECK(check == WireCheck::TOO_LARGE);
        auto oversized_bytes{valid};
        oversized_bytes.resize(FLOWMESH_WIRE_HEADER_SIZE + PayloadLimitForWireKind(kind) + 1);
        BOOST_CHECK(!DecodeWireMessage(kind, oversized_bytes, check));
        BOOST_CHECK(check == WireCheck::TOO_LARGE);
    }
}

BOOST_AUTO_TEST_CASE(valid_outer_framing_is_not_semantic_or_signature_approval)
{
    using namespace flowmesh;
    auto action{Message(WireMessageKind::ACTION)};
    action.payload[41] = MAX_ACTION_CURVE_POINTS + 1;
    const auto framed{Encode(action)}; // cheap transport framing is valid
    WireCheck check;
    BOOST_REQUIRE(DecodeWireMessage(action.kind, framed, check));
    BOOST_CHECK(!DecodeProductionActionPayload(action.payload));

    auto attestation{Message(WireMessageKind::ATTESTATION)};
    std::fill(attestation.payload.begin() + 4, attestation.payload.end(), 0);
    BOOST_REQUIRE(DecodeWireMessage(attestation.kind, Encode(attestation), check));
    BOOST_CHECK(!DecodeProductionAttestationPayload(attestation.payload));

    const auto proposal{Message(WireMessageKind::PROPOSAL)};
    auto mismatched_header{proposal.header};
    ++mismatched_header.sequence;
    const auto decoded{DecodeProductionProposalPayload(proposal.payload)};
    BOOST_REQUIRE(decoded);
    BOOST_CHECK(!ProductionWireHeaderMatches(mismatched_header, decoded->entry));
    auto truncated{proposal.payload};
    truncated.resize(FLOWMESH_PROPOSAL_PAYLOAD_PREFIX_SIZE);
    BOOST_CHECK(!DecodeProductionProposalPayload(truncated));

    const auto certified{Message(WireMessageKind::CERTIFICATE)};
    BOOST_CHECK(!DecodeProductionCertifiedPayload(certified.payload, 9));
    auto bad_length{certified.payload};
    bad_length[3] ^= 1;
    BOOST_CHECK(!DecodeProductionCertifiedPayload(bad_length, 4));
    auto trailing{certified.payload};
    trailing.push_back(0);
    BOOST_CHECK(!DecodeProductionCertifiedPayload(trailing, 4));
}

BOOST_AUTO_TEST_CASE(bulk_wrapper_preserves_exact_certified_entry_bytes)
{
    using namespace flowmesh;
    const auto certified{Message(WireMessageKind::CERTIFICATE)};
    const std::vector<std::vector<unsigned char>> entries{certified.payload};
    const auto bulk{EncodeCatchupEntries(entries)};
    BOOST_REQUIRE(bulk);
    BOOST_CHECK_EQUAL(HexStr(std::span{*bulk}.first(6)), "0001000002dc");
    const auto decoded{DecodeCatchupEntries(*bulk)};
    BOOST_REQUIRE(decoded);
    BOOST_CHECK(*decoded == entries);
    BOOST_REQUIRE(DecodeProductionCertifiedPayload(decoded->front(), 4));
    auto impossible_count{*bulk};
    impossible_count[1] = FLOWMESH_CATCHUP_MAX_ENTRIES + 1;
    BOOST_CHECK(!DecodeCatchupEntries(impossible_count));
    auto impossible_size{*bulk};
    std::fill(impossible_size.begin() + 2, impossible_size.begin() + 6, 0xff);
    BOOST_CHECK(!DecodeCatchupEntries(impossible_size));
}

BOOST_AUTO_TEST_CASE(valid_signer_subsets_have_one_semantic_identity_not_identical_wire_bytes)
{
    using namespace flowmesh;
    auto entry{Entry()};
    std::vector<bls::SecretKey> keys;
    std::vector<BlsSeatBinding> bindings;
    for (uint32_t i{0}; i < 4; ++i) {
        std::array<unsigned char, 32> ikm{};
        ikm.fill(static_cast<unsigned char>(0x40 + i));
        const auto key{bls::SecretKey::FromIKM(ikm)};
        BOOST_REQUIRE(key);
        keys.push_back(*key);
        bindings.push_back({COutPoint{Txid::FromUint256(Filled(0x20 + i)), i},
                            key->GetPublicKey().Compressed(),
                            key->SignPoP().Compressed()});
    }
    std::sort(bindings.begin(), bindings.end(), [&](const auto& a, const auto& b) {
        return ComputeFlowMeshSeatId(entry.domain, a.outpoint) <
               ComputeFlowMeshSeatId(entry.domain, b.outpoint);
    });
    BlsSeatSetCheck seat_check;
    const auto seats{BuildActiveFnBlsSeatSet(
        entry.domain, entry.market_id, entry.epoch, entry.anchor.height,
        entry.anchor.hash, bindings, seat_check)};
    BOOST_REQUIRE(seats);
    BOOST_REQUIRE(seat_check == BlsSeatSetCheck::OK);
    entry.seat_set_hash = seats->set_hash;
    const auto context{ProductionCertificateContext(entry)};
    std::vector<IndexedBlsSignature> signatures;
    for (uint32_t i{0}; i < seats->Size(); ++i) {
        const auto key{std::find_if(keys.begin(), keys.end(), [&](const auto& candidate) {
            return candidate.GetPublicKey() == seats->members[i].key.Key();
        })};
        BOOST_REQUIRE(key != keys.end());
        const auto signature{SignBlsMicroblockCertificate(*key, context, *seats)};
        BOOST_REQUIRE(signature);
        signatures.push_back({i, *signature});
    }

    // Both three-seat subsets are real quorum proofs for the same entry.
    // This codec fixture does not claim that its arbitrary state was executed.
    BlsMicroblockCertificate left, right;
    BOOST_REQUIRE(AssembleProductionEntryCertificate(
        entry, *seats, std::span{signatures}.first(3), left) ==
        BlsCertificateAssemblyCheck::OK);
    BOOST_REQUIRE(AssembleProductionEntryCertificate(
        entry, *seats, std::span{signatures}.subspan(1), right) ==
        BlsCertificateAssemblyCheck::OK);
    BOOST_CHECK(left.microblock_hash == right.microblock_hash);
    BOOST_CHECK(left.signer_bitmap != right.signer_bitmap);
    BOOST_CHECK(left.aggregate_signature != right.aggregate_signature);
    std::vector<std::vector<unsigned char>> encoded;
    for (const auto& certificate : {left, right}) {
        BOOST_REQUIRE(CheckProductionEntryCertificate(entry, *seats, certificate) ==
                      BlsCertificateCheck::OK);
        const auto bytes{EncodeProductionCertifiedPayload({entry, certificate}, seats->Size())};
        BOOST_REQUIRE(bytes);
        encoded.push_back(*bytes);
    }
    BOOST_CHECK(encoded[0] != encoded[1]);
    const auto bulk{EncodeCatchupEntries(encoded)};
    BOOST_REQUIRE(bulk);
    const auto decoded_bulk{DecodeCatchupEntries(*bulk)};
    BOOST_REQUIRE(decoded_bulk);
    BOOST_CHECK(*decoded_bulk == encoded);
    for (const auto& bytes : *decoded_bulk) {
        const auto decoded{DecodeProductionCertifiedPayload(bytes, seats->Size())};
        BOOST_REQUIRE(decoded);
        BOOST_CHECK(decoded->entry.GetHash() == entry.GetHash());
        BOOST_CHECK(ProductionCertificateContext(decoded->entry).microblock_hash ==
                    context.microblock_hash);
        BOOST_CHECK(CheckProductionEntryCertificate(decoded->entry, *seats,
                                                    decoded->certificate) ==
                    BlsCertificateCheck::OK);
    }
    auto changed{entry};
    changed.state_root = Filled(0xef);
    BOOST_CHECK(CheckProductionEntryCertificate(changed, *seats, left) ==
                BlsCertificateCheck::WRONG_MICROBLOCK_HASH);
}

BOOST_AUTO_TEST_SUITE_END()
