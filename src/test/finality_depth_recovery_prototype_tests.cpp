// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include "../../contrib/b3finality/experimental_depth_recovery.h"

#include <crypto/bls.h>
#include <modern/finality_certificate.h>
#include <modern/finality_schedule.h>
#include <node/finality_binding_index.h>
#include <node/validator_set.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {
namespace prototype = b3finality::experimental;
using Status = prototype::ProposalStatus;

uint256 TestHash(const uint8_t value) { return uint256{value}; }

prototype::SignedCheckpoint Record(const int height, const uint8_t hash)
{
    return {height, TestHash(hash), TestHash(31), 0, TestHash(41), TestHash(42)};
}

/** In-memory evidence only: no wallet, files, clocks, network or mining. */
struct Model {
    prototype::ProposalRequest request;
    std::map<int, uint256> active;

    Model()
    {
        request.explicitly_enabled = true;
        request.complete_signed_history = true;
        request.max_ever_signed_height = 101;
        request.signed_history = {Record(81, 81), Record(91, 91), Record(101, 101)};
        request.tip_height = 131;
        request.checkpoint_start_height = 1;
        request.checkpoint_interval = 10;
        request.finalized_anchor = {71, TestHash(71)};
        request.candidate = Record(111, 211);
        active = {{71, TestHash(71)}, {81, TestHash(81)}, {91, TestHash(91)},
                  {101, TestHash(201)}, {111, TestHash(211)}};
    }

    std::optional<uint256> HashAt(const int height) const
    {
        const auto it{active.find(height)};
        return it == active.end() ? std::nullopt : std::optional<uint256>{it->second};
    }

    prototype::ProposalResult Select() const
    {
        return prototype::SelectDepthRecoveryProposal(
            request, [&](const int height) { return HashAt(height); });
    }
};

struct RealBlsSet {
    std::vector<bls::SecretKey> keys;
    std::optional<node::ValidatorSetSnapshot> snapshot;

    RealBlsSet()
    {
        node::FinalityBindingIndex bindings;
        std::vector<node::FinalityBindingIndex::Transition> transitions;
        std::map<node::ValidatorKey, CAmount> weights;
        for (uint8_t i{1}; i <= 4; ++i) {
            std::array<unsigned char, 32> ikm{};
            ikm[0] = i;
            ikm[31] = 0xD7;
            const auto key{bls::SecretKey::FromIKM(ikm)};
            BOOST_REQUIRE(key);
            BOOST_REQUIRE(bls::VerifiedPublicKey::FromPoP(key->GetPublicKey(), key->SignPoP()));
            keys.push_back(*key);
            node::ValidatorKey identity{};
            identity[0] = i;
            transitions.push_back({identity, {key->GetPublicKey().Compressed(), 0, 1}});
            weights.emplace(identity, 333 * modern::FINALITY_WEIGHT_UNIT);
        }
        bindings.ConnectBlock(1, transitions);
        snapshot = node::ValidatorSetSnapshot::Build(0, weights, bindings);
        BOOST_REQUIRE(snapshot);
    }

    modern::FinalityCertificate Certificate(const modern::FinalizedBlock& block,
                                           const uint256& domain,
                                           const size_t signer_count) const
    {
        modern::FinalityCertificate result;
        result.signer_bitmap = {0};
        const auto digest{modern::FinalityDigest(domain, block)};
        std::vector<bls::Signature> signatures;
        for (size_t i{0}; i < signer_count; ++i) {
            BOOST_REQUIRE_LT(i, keys.size());
            const auto& member{snapshot->Members()[i]};
            BOOST_REQUIRE(member.bls_pubkey == keys[i].GetPublicKey().Compressed());
            result.signer_bitmap[0] |= static_cast<unsigned char>(1U << i);
            signatures.push_back(keys[i].Sign(std::span<const unsigned char>{digest.begin(), 32}));
        }
        const auto aggregate{bls::AggregateSignatures(signatures)};
        BOOST_REQUIRE(aggregate);
        result.aggregate_sig = aggregate->Compressed();
        return result;
    }
};
} // namespace

BOOST_AUTO_TEST_SUITE(finality_depth_recovery_prototype_tests)

BOOST_AUTO_TEST_CASE(disabled_by_default_never_observes_chain)
{
    prototype::ProposalRequest request;
    int reads{0};
    const auto result{prototype::SelectDepthRecoveryProposal(
        request, [&](int) -> std::optional<uint256> { ++reads; return std::nullopt; })};
    BOOST_CHECK(result.status == Status::DISABLED);
    BOOST_CHECK(!result.proposal);
    BOOST_CHECK_EQUAL(reads, 0);
    static_assert(!prototype::RecoveryProposal::AUTHORIZES_SIGNING);
}

BOOST_AUTO_TEST_CASE(incomplete_history_and_missing_highwater_are_refused)
{
    Model model;
    model.request.complete_signed_history = false;
    BOOST_CHECK(model.Select().status == Status::INCOMPLETE_HISTORY);
    model.request.complete_signed_history = true;
    // A current production journal's single last vote cannot reconstruct the
    // missing earlier signed records from active-chain hashes.
    model.request.signed_history = {model.request.signed_history.back()};
    BOOST_CHECK(model.Select().status == Status::INCOMPLETE_HISTORY);
    model = Model{};
    model.request.max_ever_signed_height = 121;
    BOOST_CHECK(model.Select().status == Status::INCOMPLETE_HISTORY);
    model.request.max_ever_signed_height = 91;
    BOOST_CHECK(model.Select().status == Status::INCOMPLETE_HISTORY);
    model = Model{};
    model.request.signed_history.resize(prototype::MAX_HISTORY_RECORDS + 1);
    BOOST_CHECK(model.Select().status == Status::INCOMPLETE_HISTORY);
}

BOOST_AUTO_TEST_CASE(candidate_depth_19_20_21)
{
    Model model;
    model.request.tip_height = 130;
    BOOST_CHECK(model.Select().status == Status::CANDIDATE_TOO_SHALLOW);
    for (const int depth : {20, 21}) {
        model.request.tip_height = model.request.candidate.height + depth;
        const auto result{model.Select()};
        BOOST_CHECK(result.status == Status::PROPOSAL_ONLY_NOT_AUTHORIZATION);
        BOOST_REQUIRE(result.proposal);
        BOOST_CHECK(!result.proposal->AUTHORIZES_SIGNING);
    }
}

BOOST_AUTO_TEST_CASE(walks_shallow_and_deep_orphans_without_mutating_history)
{
    Model model;
    const auto records{model.request.signed_history};
    auto selected{model.Select()};
    BOOST_REQUIRE(selected.proposal);
    BOOST_CHECK_EQUAL(selected.proposal->common_signed_ancestor.height, 91);
    BOOST_CHECK(selected.proposal->audit_records == records);
    // More than the most recent signed checkpoint can disappear. Waiting for
    // the replacement candidate's depth does not itself cap the fork depth.
    model.active[91] = TestHash(191);
    selected = model.Select();
    BOOST_REQUIRE(selected.proposal);
    BOOST_CHECK_EQUAL(selected.proposal->common_signed_ancestor.height, 81);
    BOOST_CHECK(selected.proposal->audit_records == records);
    BOOST_CHECK(model.request.signed_history == records);
    BOOST_CHECK_EQUAL(model.request.max_ever_signed_height, 101);
    model.active[81] = TestHash(181);
    BOOST_CHECK(model.Select().status == Status::NO_COMMON_SIGNED_ANCESTOR);
}

BOOST_AUTO_TEST_CASE(never_rewinds_or_reuses_highwater)
{
    Model model;
    const auto selected{model.Select()};
    BOOST_REQUIRE(selected.proposal);
    BOOST_CHECK_EQUAL(selected.proposal->max_ever_signed_height, 101);
    BOOST_CHECK_EQUAL(selected.proposal->common_signed_ancestor.height, 91);
    BOOST_CHECK_EQUAL(selected.proposal->candidate.height, 111);
    model.request.candidate = Record(101, 201);
    BOOST_CHECK(model.Select().status == Status::CANDIDATE_NOT_ABOVE_WATERMARK);
    model.request.candidate = Record(91, 91);
    BOOST_CHECK(model.Select().status == Status::CANDIDATE_NOT_ABOVE_WATERMARK);
    BOOST_CHECK_EQUAL(model.request.max_ever_signed_height, 101);
}

BOOST_AUTO_TEST_CASE(finalized_anchor_and_missing_chain_data_fail_closed)
{
    Model model;
    model.request.finalized_anchor.block_hash = TestHash(171);
    BOOST_CHECK(model.Select().status == Status::FINALIZED_ANCHOR_MISMATCH);
    model = Model{};
    model.request.finalized_anchor = {111, TestHash(211)};
    BOOST_CHECK(model.Select().status == Status::CANDIDATE_NOT_ABOVE_FINALITY);
    for (const int missing : {71, 101, 111}) {
        model = Model{};
        model.active.erase(missing);
        BOOST_CHECK(model.Select().status == Status::CHAIN_DATA_UNAVAILABLE);
    }
    model = Model{};
    model.active[111] = TestHash(212);
    BOOST_CHECK(model.Select().status == Status::CANDIDATE_NOT_ON_ACTIVE_CHAIN);
    model = Model{};
    model.active[101] = TestHash(101);
    BOOST_CHECK(model.Select().status == Status::LATEST_VOTE_NOT_ORPHANED);
}

BOOST_AUTO_TEST_CASE(cross_epoch_or_set_changes_are_refused)
{
    Model model;
    model.request.candidate.epoch = 1;
    BOOST_CHECK(model.Select().status == Status::EPOCH_OR_SET_MISMATCH);
    model = Model{};
    model.request.candidate.signing_set_hash = TestHash(43);
    BOOST_CHECK(model.Select().status == Status::EPOCH_OR_SET_MISMATCH);
    model = Model{};
    model.request.candidate.successor_set_hash = TestHash(44);
    BOOST_CHECK(model.Select().status == Status::EPOCH_OR_SET_MISMATCH);
    model = Model{};
    model.request.signed_history[1].epoch = 1;
    BOOST_CHECK(model.Select().status == Status::EPOCH_OR_SET_MISMATCH);
}

BOOST_AUTO_TEST_CASE(malformed_history_and_checkpoint_schedule_are_refused)
{
    Model model;
    model.request.signed_history[1] = model.request.signed_history[0];
    BOOST_CHECK(model.Select().status == Status::MALFORMED_INPUT);
    model = Model{};
    model.request.signed_history[0].digest.SetNull();
    BOOST_CHECK(model.Select().status == Status::MALFORMED_INPUT);
    model = Model{};
    model.request.candidate.height = 112;
    BOOST_CHECK(model.Select().status == Status::MALFORMED_INPUT);
    model = Model{};
    model.request.checkpoint_interval = 0;
    BOOST_CHECK(model.Select().status == Status::MALFORMED_INPUT);
}

BOOST_AUTO_TEST_CASE(residual_risk_two_conflicting_quorum_certificates_verify_at_20_depth)
{
    // This test PASS demonstrates a SAFETY FAILURE of the proposed fallback,
    // not safety of recovery. Production signing guards are never bypassed or
    // modified; only this in-memory experiment creates both sets of signatures.
    RealBlsSet set;
    const uint256 domain{TestHash(61)};
    const auto successor{set.snapshot->WithEpoch(1).SetHash()};
    const auto& view{set.snapshot->View()};
    BOOST_CHECK_EQUAL(set.snapshot->TotalWeight(), 1332U);
    BOOST_CHECK_EQUAL(view.quorum_weight, 889U);
    BOOST_CHECK_EQUAL(modern::FinalityHeadcountQuorum(view.validator_count), 3U);

    Model model;
    for (auto& record : model.request.signed_history) {
        record.signing_set_hash = set.snapshot->SetHash();
        record.successor_set_hash = successor;
    }
    model.request.candidate.signing_set_hash = set.snapshot->SetHash();
    model.request.candidate.successor_set_hash = successor;
    const modern::FinalizedBlock orphan{101, TestHash(101), TestHash(51), successor, 0};
    const modern::FinalizedBlock replacement{111, TestHash(211), TestHash(52), successor, 0};
    model.request.signed_history.back().digest = modern::FinalityDigest(domain, orphan);
    model.request.candidate.digest = modern::FinalityDigest(domain, replacement);
    const auto proposal{model.Select()};
    BOOST_REQUIRE(proposal.proposal);
    BOOST_CHECK(proposal.status == Status::PROPOSAL_ONLY_NOT_AUTHORIZATION);
    BOOST_CHECK_EQUAL(model.request.tip_height - replacement.height, 20U);
    BOOST_CHECK_EQUAL(proposal.proposal->max_ever_signed_height, 101);
    BOOST_CHECK_EQUAL(proposal.proposal->common_signed_ancestor.height, 91);
    BOOST_CHECK(proposal.proposal->audit_records.back().digest == modern::FinalityDigest(domain, orphan));
    BOOST_CHECK(model.HashAt(101) != std::optional<uint256>{orphan.block_hash});

    // A/B/C are the same three genuine keys on each branch. The two complete
    // certificates are kept separately; votes at different heights never mix.
    const auto old_certificate{set.Certificate(orphan, domain, 3)};
    const auto new_certificate{set.Certificate(replacement, domain, 3)};
    BOOST_CHECK(old_certificate.aggregate_sig != new_certificate.aggregate_sig);
    BOOST_CHECK_EQUAL(modern::SignedWeight(old_certificate.signer_bitmap, view), 999U);
    BOOST_CHECK_EQUAL(modern::SignedWeight(new_certificate.signer_bitmap, view), 999U);
    BOOST_CHECK(modern::VerifyFinalityCertificate(domain, orphan, old_certificate, view, successor) == modern::CertificateCheck::OK);
    BOOST_CHECK(modern::VerifyFinalityCertificate(domain, replacement, new_certificate, view, successor) == modern::CertificateCheck::OK);
    const auto minority{set.Certificate(orphan, domain, 2)};
    BOOST_CHECK(modern::VerifyFinalityCertificate(domain, orphan, minority, view, successor) == modern::CertificateCheck::INSUFFICIENT_WEIGHT);
    auto insufficient_count{old_certificate};
    insufficient_count.signer_bitmap = {0x03};
    auto count_gate_view{view};
    count_gate_view.quorum_weight = 1; // isolate the production headcount gate
    BOOST_CHECK(modern::VerifyFinalityCertificate(domain, orphan, insufficient_count, count_gate_view, successor) == modern::CertificateCheck::INSUFFICIENT_HEADCOUNT);

    // Also exercise the production placement + BLS judgement against each
    // branch, requiring the proposed 20 depth (stricter than current V1's 12).
    // This is not block execution, a network simulation, or an Ethereum run.
    Consensus::ModernPosParams params;
    params.checkpoint_depth = 20;
    params.checkpoint_interval = 10;
    modern::FinalityEpochView epoch_view;
    epoch_view.current_epoch = 0;
    epoch_view.epoch_starts = {1};
    epoch_view.finalized_height = 71;
    epoch_view.current_set = &view;
    epoch_view.current_set_hash = set.snapshot->SetHash();
    epoch_view.next_set_hash = successor;
    const auto old_hash_at = [&](const int height) -> std::optional<uint256> {
        if (height == 101) return orphan.block_hash;
        return model.HashAt(height);
    };
    const auto new_hash_at = [&](const int height) { return model.HashAt(height); };
    std::string error;
    BOOST_CHECK_MESSAGE(modern::JudgeFinalityCertificate(
        domain, orphan, old_certificate, 121, epoch_view, params, old_hash_at, error,
        [&](int) { return std::optional<uint256>{orphan.withdrawal_root}; }), error);
    error.clear();
    BOOST_CHECK_MESSAGE(modern::JudgeFinalityCertificate(
        domain, replacement, new_certificate, 131, epoch_view, params, new_hash_at, error,
        [&](int) { return std::optional<uint256>{replacement.withdrawal_root}; }), error);
    BOOST_CHECK(modern::CheckCertificatePlacement(orphan, 131, epoch_view, params, new_hash_at) == modern::CertificatePlacement::WRONG_BLOCK_HASH);
}

BOOST_AUTO_TEST_SUITE_END()
