// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <flowmesh/production_wire.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <vector>

namespace {

uint256 Filled(const unsigned char value)
{
    uint256 out;
    std::fill(out.begin(), out.end(), value);
    return out;
}

flowmesh::ProductionEntryCore Entry()
{
    flowmesh::ProductionEntryCore entry;
    entry.domain = Filled(1);
    entry.market_id = Filled(2);
    entry.epoch = 3;
    entry.seat_set_hash = Filled(4);
    entry.sequence = 0;
    entry.anchor = {100, Filled(5)};
    entry.previous_state_root = Filled(6);
    entry.actions_root = flowmesh::ComputeProductionActionsRoot(entry.actions);
    entry.result_root = Filled(7);
    entry.state_root = Filled(8);
    entry.effect_root = modern::EmptyFlowMeshEffectRoot(0);
    return entry;
}

bls::SecretKey Secret()
{
    std::array<unsigned char, 32> ikm{};
    for (size_t i{0}; i < ikm.size(); ++i) ikm[i] = i + 1;
    const auto key{bls::SecretKey::FromIKM(ikm)};
    BOOST_REQUIRE(key);
    return *key;
}

} // namespace

BOOST_AUTO_TEST_SUITE(flowmesh_production_wire_tests)

BOOST_AUTO_TEST_CASE(action_payload_is_exact_and_credentials_are_transport_only)
{
    flowmesh::Action action;
    action.signer = Filled(9);
    action.sequence = 4;
    action.type = static_cast<uint8_t>(flowmesh::ActionType::CANCEL_BID);
    action.credential = {1, 2, 3};

    const auto encoded{flowmesh::EncodeProductionActionPayload(action)};
    BOOST_REQUIRE(encoded);
    const auto decoded{flowmesh::DecodeProductionActionPayload(*encoded)};
    BOOST_REQUIRE(decoded);
    BOOST_CHECK(decoded->Id() == action.Id());
    BOOST_CHECK(decoded->credential == action.credential);

    auto trailing{*encoded};
    trailing.push_back(0);
    BOOST_CHECK(!flowmesh::DecodeProductionActionPayload(trailing));
    action.credential.assign(flowmesh::MAX_ACTION_CREDENTIAL_SIZE + 1, 0);
    BOOST_CHECK(!flowmesh::EncodeProductionActionPayload(action));
}

BOOST_AUTO_TEST_CASE(proposal_payload_and_common_header_are_unambiguous)
{
    flowmesh::ProductionProposalEnvelope proposal;
    proposal.entry = Entry();
    proposal.round = 7;
    proposal.proposer_seat_index = 2;
    proposal.proposer_signature.fill(0x44);
    const auto encoded{flowmesh::EncodeProductionProposalPayload(proposal)};
    BOOST_REQUIRE(encoded);
    const auto decoded{flowmesh::DecodeProductionProposalPayload(*encoded)};
    BOOST_REQUIRE(decoded);
    BOOST_CHECK_EQUAL(decoded->round, 7U);
    BOOST_CHECK_EQUAL(decoded->proposer_seat_index, 2U);
    BOOST_CHECK(decoded->entry.GetHash() == proposal.entry.GetHash());

    flowmesh::WireHeader header;
    header.market_id = proposal.entry.market_id;
    header.epoch = proposal.entry.epoch;
    header.sequence = proposal.entry.sequence;
    BOOST_CHECK(flowmesh::ProductionWireHeaderMatches(header, proposal.entry));
    ++header.sequence;
    BOOST_CHECK(!flowmesh::ProductionWireHeaderMatches(header, proposal.entry));

    auto truncated{*encoded};
    truncated.resize(flowmesh::FLOWMESH_PROPOSAL_PAYLOAD_PREFIX_SIZE);
    BOOST_CHECK(!flowmesh::DecodeProductionProposalPayload(truncated));
}

BOOST_AUTO_TEST_CASE(attestation_payload_decodes_only_a_real_bls_signature)
{
    const uint256 digest{Filled(0xa1)};
    flowmesh::IndexedBlsSignature attestation{
        3, Secret().Sign(std::span<const unsigned char>{digest.begin(), 32})};
    const auto encoded{flowmesh::EncodeProductionAttestationPayload(attestation)};
    BOOST_REQUIRE(encoded);
    const auto decoded{flowmesh::DecodeProductionAttestationPayload(*encoded)};
    BOOST_REQUIRE(decoded);
    BOOST_CHECK_EQUAL(decoded->seat_index, 3U);
    BOOST_CHECK(decoded->signature == attestation.signature);

    auto infinity{*encoded};
    std::fill(infinity.begin() + 4, infinity.end(), 0);
    BOOST_CHECK(!flowmesh::DecodeProductionAttestationPayload(infinity));
}

BOOST_AUTO_TEST_CASE(certified_payload_binds_entry_certificate_and_exact_width)
{
    flowmesh::ProductionCertifiedEnvelope certified;
    certified.entry = Entry();
    certified.certificate.seat_epoch = certified.entry.epoch;
    certified.certificate.sequence = certified.entry.sequence;
    certified.certificate.microblock_hash = certified.entry.GetHash();
    certified.certificate.signer_bitmap = {0x0f};
    certified.certificate.aggregate_signature.fill(0x55);

    const auto encoded{
        flowmesh::EncodeProductionCertifiedPayload(certified, 4)};
    BOOST_REQUIRE(encoded);
    const auto decoded{
        flowmesh::DecodeProductionCertifiedPayload(*encoded, 4)};
    BOOST_REQUIRE(decoded);
    BOOST_CHECK(decoded->entry.GetHash() == certified.entry.GetHash());
    BOOST_CHECK(decoded->certificate == certified.certificate);

    auto trailing{*encoded};
    trailing.push_back(0);
    BOOST_CHECK(!flowmesh::DecodeProductionCertifiedPayload(trailing, 4));
    BOOST_CHECK(!flowmesh::DecodeProductionCertifiedPayload(*encoded, 9));

    auto bad_size{*encoded};
    bad_size[3] ^= 1;
    BOOST_CHECK(!flowmesh::DecodeProductionCertifiedPayload(bad_size, 4));

    auto wrong{certified};
    ++wrong.certificate.sequence;
    const auto wrong_bytes{flowmesh::EncodeProductionCertifiedPayload(wrong, 4)};
    BOOST_REQUIRE(wrong_bytes);
    BOOST_CHECK(!flowmesh::DecodeProductionCertifiedPayload(*wrong_bytes, 4));
}

BOOST_AUTO_TEST_SUITE_END()
