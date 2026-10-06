// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// Bridge V2 pure burn rules (doc/design/b3-bridge-v2-node-changes.md):
// - the burn liveness gate, rows C1-C4 and E1-E8 (sections 8.4 and 8.6;
//   bridge/burn_gate_v2.h);
// - the per-vault reserve ledger and the reserve bound (sections 7.1 and
//   7.2 check 2);
// - the wind-down bound (section 7.2 check 3);
// - invariant R (section 8.4);
// - transit crediting on proven arrival (section 7.6; bridge/reserve_v2.h).
//
// Every row has a refusal case, and each time or height limit is tested at
// the limit (accepted) and one second or one block past it (refused). The
// reason strings are checked verbatim because getbridgev2info and the
// mempool, block and wallet errors report them.

#include <bridge/burn_gate_v2.h>
#include <bridge/proof.h>
#include <bridge/reserve_v2.h>
#include <bridge/state_observation.h>
#include <consensus/amount.h>
#include <consensus/bridge_params.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

using namespace bridge;
using Consensus::BridgeV2GateParams;

BOOST_AUTO_TEST_SUITE(bridge_burn_gate_v2_tests)

namespace {

constexpr int64_t T{1'800'000'000};
constexpr int64_t INT64_MAX_V{std::numeric_limits<int64_t>::max()};

//! Profile M (the defaults): max age 172,800 s, rotation age limit
//! 1,900,800 s, set expiry 2,592,000 s, epoch gap 2, finality lag 90.
const BridgeV2GateParams GATE_M{};

/**
 * A READY gate input at height 1,000 and now_eff T: lineage epoch 5 with 4
 * members of weight 250 in both committees (bounds 4-64, weight >= 900), a
 * checkpoint 10 blocks back in epoch 5, and three observations 600 s old
 * of a LIVE verifier at epoch 5 that rotated an hour ago.
 */
BurnGateInputsV2 ReadyInputs()
{
    BurnGateInputsV2 in;
    in.height = 1'000;
    in.burn_height = 900;
    in.now_eff = T;
    in.lineage.configured = true;
    in.lineage.bootstrapped = true;
    in.lineage.broken = false;
    in.lineage.epoch = 5;
    in.lineage.current_count = 4;
    in.lineage.current_weight = 1'000;
    in.lineage.next_count = 4;
    in.lineage.next_weight = 1'000;
    in.lineage.finalized_height = 990;
    in.lineage.finalized_epoch = 5;
    in.bounds = {4, 64, 900};

    VerifierStateV2 verifier;
    verifier.current_epoch = 5;
    verifier.last_rotation_time = T - 3'600;
    verifier.last_certificate_time = T - 600;
    verifier.latest_bridge_height = 980;
    verifier.status = VERIFIER_STATUS_LIVE;
    in.verifier = BurnGateObservationV2<VerifierStateV2>{T - 600, verifier, 0};

    in.token = BurnGateObservationV2<TokenFlagsV2>{T - 600, TokenFlagsV2{}, 0};

    ReserveStateV2 reserve;
    reserve.next_deposit_id = 3;
    reserve.locked = 1'000;
    reserve.balance = 1'000;
    in.reserve = BurnGateObservationV2<ReserveStateV2>{T - 600, reserve, 0};
    return in;
}

void CheckReady(const BurnGateInputsV2& in, const BridgeV2GateParams& gate = GATE_M)
{
    const auto decision{EvaluateBurnGateV2(in, gate)};
    BOOST_CHECK_MESSAGE(decision.ready && decision.row == BurnGateRowV2::NONE &&
                            decision.reason.empty(),
                        "expected READY, got " + decision.Describe());
}

void CheckRefused(const BurnGateInputsV2& in, const BurnGateRowV2 row,
                  const std::string_view reason,
                  const BridgeV2GateParams& gate = GATE_M)
{
    const auto decision{EvaluateBurnGateV2(in, gate)};
    BOOST_CHECK_MESSAGE(!decision.ready && decision.row == row && decision.reason == reason,
                        "expected " + std::string{BurnGateRowName(row)} + ": " +
                            std::string{reason} + ", got " + decision.Describe());
    BOOST_CHECK(!decision.closes_at);
}

std::optional<int64_t> ClosesAt(const BurnGateInputsV2& in, const BridgeV2GateParams& gate = GATE_M)
{
    const auto decision{EvaluateBurnGateV2(in, gate)};
    BOOST_REQUIRE_MESSAGE(decision.ready, decision.Describe());
    return decision.closes_at;
}

} // namespace

// ------------------------------------------------------------------- gate

BOOST_AUTO_TEST_CASE(ready_inputs_pass_with_closes_at)
{
    const BurnGateInputsV2 in{ReadyInputs()};
    const auto decision{EvaluateBurnGateV2(in, GATE_M)};
    BOOST_CHECK(decision.ready);
    BOOST_CHECK(decision.row == BurnGateRowV2::NONE);
    BOOST_CHECK(decision.reason.empty());
    BOOST_CHECK_EQUAL(decision.Describe(), "ready");
    // The three observations (600 s old) expire first: T - 600 + 172,800 + 1.
    BOOST_REQUIRE(decision.closes_at);
    BOOST_CHECK_EQUAL(*decision.closes_at, T + 172'201);

    // Profile M and profile R derived limits.
    BOOST_CHECK_EQUAL(GATE_M.RotationAgeLimit(), 1'900'800);
    BOOST_CHECK_EQUAL(BridgeV2GateParams::RegtestProfileR().RotationAgeLimit(), 52'800);
}

BOOST_AUTO_TEST_CASE(row_names_and_describe)
{
    BOOST_CHECK_EQUAL(BurnGateRowName(BurnGateRowV2::NONE), "none");
    BOOST_CHECK_EQUAL(BurnGateRowName(BurnGateRowV2::C1), "C1");
    BOOST_CHECK_EQUAL(BurnGateRowName(BurnGateRowV2::C2), "C2");
    BOOST_CHECK_EQUAL(BurnGateRowName(BurnGateRowV2::C3), "C3");
    BOOST_CHECK_EQUAL(BurnGateRowName(BurnGateRowV2::C4), "C4");
    BOOST_CHECK_EQUAL(BurnGateRowName(BurnGateRowV2::E1), "E1");
    BOOST_CHECK_EQUAL(BurnGateRowName(BurnGateRowV2::E2), "E2");
    BOOST_CHECK_EQUAL(BurnGateRowName(BurnGateRowV2::E3), "E3");
    BOOST_CHECK_EQUAL(BurnGateRowName(BurnGateRowV2::E4), "E4");
    BOOST_CHECK_EQUAL(BurnGateRowName(BurnGateRowV2::E5), "E5");
    BOOST_CHECK_EQUAL(BurnGateRowName(BurnGateRowV2::E6), "E6");
    BOOST_CHECK_EQUAL(BurnGateRowName(BurnGateRowV2::E7), "E7");
    BOOST_CHECK_EQUAL(BurnGateRowName(BurnGateRowV2::E8), "E8");

    BurnGateInputsV2 in{ReadyInputs()};
    in.verifier.reset();
    BOOST_CHECK_EQUAL(EvaluateBurnGateV2(in, GATE_M).Describe(), "E1: no VERIFIER_E observation");
}

BOOST_AUTO_TEST_CASE(c1_inputs_parameters_w2_and_impairment)
{
    // Inputs out of range.
    {
        BurnGateInputsV2 in{ReadyInputs()};
        in.now_eff = -1;
        CheckRefused(in, BurnGateRowV2::C1, "burn gate inputs are out of range");
    }
    {
        BurnGateInputsV2 in{ReadyInputs()};
        in.verifier->t_obs = -1;
        CheckRefused(in, BurnGateRowV2::C1, "burn gate inputs are out of range");
    }
    {
        BurnGateInputsV2 in{ReadyInputs()};
        in.verifier->state.last_rotation_time = -1;
        CheckRefused(in, BurnGateRowV2::C1, "burn gate inputs are out of range");
    }
    {
        BurnGateInputsV2 in{ReadyInputs()};
        in.token->t_obs = -1;
        CheckRefused(in, BurnGateRowV2::C1, "burn gate inputs are out of range");
    }
    {
        BurnGateInputsV2 in{ReadyInputs()};
        in.reserve->t_obs = -1;
        CheckRefused(in, BurnGateRowV2::C1, "burn gate inputs are out of range");
    }
    // Invalid gate parameters.
    {
        BridgeV2GateParams bad{GATE_M};
        bad.observation_max_age_seconds = 0;
        CheckRefused(ReadyInputs(), BurnGateRowV2::C1, "bridge V2 gate parameters are invalid", bad);
        bad = GATE_M;
        bad.burn_margin_seconds = bad.set_expiry_seconds - bad.release_margin_seconds;
        CheckRefused(ReadyInputs(), BurnGateRowV2::C1, "bridge V2 gate parameters are invalid", bad);
    }
    // W2: the first burn height is accepted, one block below it refused.
    {
        BurnGateInputsV2 in{ReadyInputs()};
        in.burn_height = in.height;
        CheckReady(in);
        in.burn_height = in.height + 1;
        CheckRefused(in, BurnGateRowV2::C1, "bridge V2 burns are not active at this height");
    }
    // A halted or cancelled verifier impairs the entry at any age, ahead of
    // every other row (here a broken lineage and a stale observation).
    {
        BurnGateInputsV2 in{ReadyInputs()};
        in.verifier->state.status = VERIFIER_STATUS_HALTED_CONFLICT;
        in.verifier->t_obs = T - 10 * GATE_M.observation_max_age_seconds;
        in.lineage.broken = true;
        CheckRefused(in, BurnGateRowV2::C1, "verifier is halted by a conflict");
        in.verifier->state.status = VERIFIER_STATUS_CANCELLED;
        CheckRefused(in, BurnGateRowV2::C1, "verifier is cancelled");
    }
    // Expiry at the observation: t_obs - lastRotationTime == set_expiry is
    // not impaired (E2 refuses it instead), one second more is.
    {
        BurnGateInputsV2 in{ReadyInputs()};
        in.verifier->state.last_rotation_time = in.verifier->t_obs - GATE_M.set_expiry_seconds;
        CheckRefused(in, BurnGateRowV2::E2, "verifier rotation is older than the rotation age limit");
        in.verifier->state.last_rotation_time -= 1;
        CheckRefused(in, BurnGateRowV2::C1, "verifier signing set had expired at the observation");
    }
}

BOOST_AUTO_TEST_CASE(verifier_impairment_is_c1s_reason)
{
    // VerifierImpairmentV2, which node::BridgeV2DepositsClosedReason also
    // reads, returns exactly the gate's C1 impairment strings, and the gate
    // refuses with them unchanged.
    const BurnGateInputsV2 ready{ReadyInputs()};
    BOOST_CHECK(!VerifierImpairmentV2(*ready.verifier, GATE_M));
    const auto same_as_gate{[](const BurnGateInputsV2& in, const std::string_view reason) {
        const auto impairment{VerifierImpairmentV2(*in.verifier, GATE_M)};
        BOOST_REQUIRE(impairment);
        BOOST_CHECK_EQUAL(*impairment, reason);
        CheckRefused(in, BurnGateRowV2::C1, reason);
    }};
    BurnGateInputsV2 in{ready};
    in.verifier->state.status = VERIFIER_STATUS_HALTED_CONFLICT;
    same_as_gate(in, "verifier is halted by a conflict");
    in.verifier->state.status = VERIFIER_STATUS_CANCELLED;
    in.verifier->state.deposits_closed = true;
    same_as_gate(in, "verifier is cancelled");
    // Expiry: at set_expiry not impaired, one second more impaired.
    in = ready;
    in.verifier->state.last_rotation_time = in.verifier->t_obs - GATE_M.set_expiry_seconds;
    BOOST_CHECK(!VerifierImpairmentV2(*in.verifier, GATE_M));
    in.verifier->state.last_rotation_time -= 1;
    same_as_gate(in, "verifier signing set had expired at the observation");
    // depositsClosed, UNINITIALIZED and a stale observation are not C1
    // impairments (E1 reads the last two).
    in = ready;
    in.verifier->state.deposits_closed = true;
    BOOST_CHECK(!VerifierImpairmentV2(*in.verifier, GATE_M));
    in.verifier->state.status = VERIFIER_STATUS_UNINITIALIZED;
    BOOST_CHECK(!VerifierImpairmentV2(*in.verifier, GATE_M));
    in = ready;
    in.verifier->t_obs = T - 10 * GATE_M.observation_max_age_seconds;
    in.verifier->state.last_rotation_time = in.verifier->t_obs;
    BOOST_CHECK(!VerifierImpairmentV2(*in.verifier, GATE_M));
    // A negative time skips the expiry comparison (no overflow); the gate
    // refuses it first as out of range.
    in = ready;
    in.verifier->state.last_rotation_time = std::numeric_limits<int64_t>::min();
    BOOST_CHECK(!VerifierImpairmentV2(*in.verifier, GATE_M));
    CheckRefused(in, BurnGateRowV2::C1, "burn gate inputs are out of range");
    in = ready;
    in.verifier->t_obs = -1;
    BOOST_CHECK(!VerifierImpairmentV2(*in.verifier, GATE_M));
    CheckRefused(in, BurnGateRowV2::C1, "burn gate inputs are out of range");
    in.verifier->t_obs = INT64_MAX_V;
    in.verifier->state.last_rotation_time = 0;
    same_as_gate(in, "verifier signing set had expired at the observation");
}

BOOST_AUTO_TEST_CASE(c2_lineage_and_committees)
{
    {
        BurnGateInputsV2 in{ReadyInputs()};
        in.lineage.configured = false;
        CheckRefused(in, BurnGateRowV2::C2, "fresh finality lineage is not configured");
    }
    {
        BurnGateInputsV2 in{ReadyInputs()};
        in.lineage.bootstrapped = false;
        CheckRefused(in, BurnGateRowV2::C2, "fresh finality lineage is not bootstrapped");
    }
    {
        BurnGateInputsV2 in{ReadyInputs()};
        in.lineage.broken = true;
        CheckRefused(in, BurnGateRowV2::C2, "fresh finality lineage is broken");
    }
    {
        BurnGateInputsV2 in{ReadyInputs()};
        in.bounds.min_validators = 0;
        CheckRefused(in, BurnGateRowV2::C2, "bridge committee bounds are invalid");
        in = ReadyInputs();
        in.bounds.max_validators = 3;
        CheckRefused(in, BurnGateRowV2::C2, "bridge committee bounds are invalid");
        in = ReadyInputs();
        in.bounds.min_weight = 0;
        CheckRefused(in, BurnGateRowV2::C2, "bridge committee bounds are invalid");
    }
    // Member counts at both bounds pass, one beyond each fails.
    const auto counts{[](uint32_t BurnGateLineageV2::*field, const char* reason) {
        BurnGateInputsV2 in{ReadyInputs()};
        in.lineage.*field = 4;
        CheckReady(in);
        in.lineage.*field = 64;
        CheckReady(in);
        in.lineage.*field = 3;
        CheckRefused(in, BurnGateRowV2::C2, reason);
        in.lineage.*field = 65;
        CheckRefused(in, BurnGateRowV2::C2, reason);
        in.lineage.*field = 0;
        CheckRefused(in, BurnGateRowV2::C2, reason);
    }};
    counts(&BurnGateLineageV2::current_count, "current committee member count is outside the bridge bounds");
    counts(&BurnGateLineageV2::next_count, "next committee member count is outside the bridge bounds");
    // Weight at the minimum passes, one unit below fails.
    const auto weights{[](uint64_t BurnGateLineageV2::*field, const char* reason) {
        BurnGateInputsV2 in{ReadyInputs()};
        in.lineage.*field = 900;
        CheckReady(in);
        in.lineage.*field = 899;
        CheckRefused(in, BurnGateRowV2::C2, reason);
    }};
    weights(&BurnGateLineageV2::current_weight, "current committee weight is below the bridge minimum");
    weights(&BurnGateLineageV2::next_weight, "next committee weight is below the bridge minimum");
}

BOOST_AUTO_TEST_CASE(c3_finality_lag)
{
    {
        BurnGateInputsV2 in{ReadyInputs()};
        in.lineage.finalized_height.reset();
        CheckRefused(in, BurnGateRowV2::C3, "fresh finality lineage has no finalized checkpoint");
    }
    {
        BurnGateInputsV2 in{ReadyInputs()};
        in.lineage.finalized_height = in.height - 90;
        CheckReady(in);
        in.lineage.finalized_height = in.height - 91;
        CheckRefused(in, BurnGateRowV2::C3, "finality lags the candidate height by more than the limit");
        in.lineage.finalized_height = in.height + 1;
        CheckRefused(in, BurnGateRowV2::C3, "finalized checkpoint is above the candidate height");
    }
    // Profile R: 30 blocks.
    {
        const auto gate_r{BridgeV2GateParams::RegtestProfileR()};
        BurnGateInputsV2 in{ReadyInputs()};
        in.lineage.finalized_height = in.height - 30;
        CheckReady(in, gate_r);
        in.lineage.finalized_height = in.height - 31;
        CheckRefused(in, BurnGateRowV2::C3, "finality lags the candidate height by more than the limit", gate_r);
    }
}

BOOST_AUTO_TEST_CASE(c4_sticky_conflict)
{
    BurnGateInputsV2 in{ReadyInputs()};
    in.conflict_reason = BRIDGE_V2_CONFLICT_E5;
    CheckRefused(in, BurnGateRowV2::C4, "sticky E5 conflict: the verifier accepted a non-canonical certificate");
    in.conflict_reason = BRIDGE_V2_CONFLICT_E8;
    CheckRefused(in, BurnGateRowV2::C4, "sticky E8 conflict: the vault failed invariant R");
    in.conflict_reason = BRIDGE_V2_CONFLICT_KIND13;
    CheckRefused(in, BurnGateRowV2::C4, "sticky conflict: finality conflict evidence");
    in.conflict_reason = 0;
    CheckRefused(in, BurnGateRowV2::C4, "sticky conflict");

    // The conflict outranks every E row, including a fresh good verdict and
    // a CERT_CONFLICT verdict, and missing observations.
    in.conflict_reason = BRIDGE_V2_CONFLICT_E5;
    in.verifier->verdict = ETH_OBSERVATION_VERDICT_CERT_CONFLICT;
    CheckRefused(in, BurnGateRowV2::C4, "sticky E5 conflict: the verifier accepted a non-canonical certificate");
    in.verifier.reset();
    in.token.reset();
    in.reserve.reset();
    CheckRefused(in, BurnGateRowV2::C4, "sticky E5 conflict: the verifier accepted a non-canonical certificate");
}

BOOST_AUTO_TEST_CASE(e1_verifier_observation)
{
    {
        BurnGateInputsV2 in{ReadyInputs()};
        in.verifier.reset();
        CheckRefused(in, BurnGateRowV2::E1, "no VERIFIER_E observation");
    }
    // Age exactly at the limit passes; one second more is stale.
    {
        BurnGateInputsV2 in{ReadyInputs()};
        in.verifier->t_obs = T - GATE_M.observation_max_age_seconds;
        CheckReady(in);
        in.verifier->t_obs -= 1;
        CheckRefused(in, BurnGateRowV2::E1, "VERIFIER_E observation is stale");
    }
    // t_obs == now_eff passes; later than now_eff is refused.
    {
        BurnGateInputsV2 in{ReadyInputs()};
        in.verifier->t_obs = T;
        CheckReady(in);
        in.verifier->t_obs = T + 1;
        CheckRefused(in, BurnGateRowV2::E1, "VERIFIER_E observation is newer than now_eff");
    }
    // UNINITIALIZED is not LIVE (HALTED and CANCELLED are C1's).
    {
        BurnGateInputsV2 in{ReadyInputs()};
        in.verifier->state.status = VERIFIER_STATUS_UNINITIALIZED;
        CheckRefused(in, BurnGateRowV2::E1, "verifier status is not LIVE");
    }
    // Profile R: 4,800 s.
    {
        const auto gate_r{BridgeV2GateParams::RegtestProfileR()};
        BurnGateInputsV2 in{ReadyInputs()};
        in.verifier->t_obs = T - 4'800;
        in.token->t_obs = T - 60;
        in.reserve->t_obs = T - 60;
        CheckReady(in, gate_r);
        in.verifier->t_obs -= 1;
        CheckRefused(in, BurnGateRowV2::E1, "VERIFIER_E observation is stale", gate_r);
    }
}

BOOST_AUTO_TEST_CASE(e2_rotation_age)
{
    BurnGateInputsV2 in{ReadyInputs()};
    in.verifier->state.last_rotation_time = T - GATE_M.RotationAgeLimit();
    CheckReady(in);
    // The rotation term is now the earliest closing time.
    BOOST_CHECK_EQUAL(*ClosesAt(in), T + 1);
    in.verifier->state.last_rotation_time -= 1;
    CheckRefused(in, BurnGateRowV2::E2, "verifier rotation is older than the rotation age limit");

    // Profile R: 72,000 - 16,800 - 2,400 = 52,800 s.
    const auto gate_r{BridgeV2GateParams::RegtestProfileR()};
    in = ReadyInputs();
    in.verifier->t_obs = T - 60;
    in.token->t_obs = T - 60;
    in.reserve->t_obs = T - 60;
    in.verifier->state.last_rotation_time = T - 52'800;
    CheckReady(in, gate_r);
    in.verifier->state.last_rotation_time -= 1;
    CheckRefused(in, BurnGateRowV2::E2, "verifier rotation is older than the rotation age limit", gate_r);
}

BOOST_AUTO_TEST_CASE(e3_epoch_gap)
{
    BurnGateInputsV2 in{ReadyInputs()};
    // Below: the verifier one epoch ahead of B3.
    in.verifier->state.current_epoch = 6;
    CheckRefused(in, BurnGateRowV2::E3, "verifier epoch is ahead of the lineage epoch");
    // Gap 2 passes (the handover into epoch 5 is certified: E6), gap 3 fails.
    // On a chain, node::VerifierVerdictV2 judges a new LIVE observation at
    // e_E in [e_B3 - 2, e_B3] (bridge_state_tests: v2_verifier_verdicts).
    in.verifier->state.current_epoch = 3;
    CheckReady(in);
    in.verifier->state.current_epoch = 2;
    CheckRefused(in, BurnGateRowV2::E3, "verifier epoch trails the lineage epoch by more than the limit");
    // A zero gap limit refuses gap 1.
    BridgeV2GateParams strict{GATE_M};
    strict.max_epoch_gap = 0;
    in.verifier->state.current_epoch = 4;
    CheckRefused(in, BurnGateRowV2::E3, "verifier epoch trails the lineage epoch by more than the limit", strict);
    in.verifier->state.current_epoch = 5;
    CheckReady(in, strict);
}

BOOST_AUTO_TEST_CASE(e4_e5_verdict)
{
    BurnGateInputsV2 in{ReadyInputs()};
    in.verifier->verdict = ETH_OBSERVATION_VERDICT_SETS_MISMATCH;
    CheckRefused(in, BurnGateRowV2::E4, "observed verifier set hashes differ from the lineage's sets");
    in.verifier->verdict = 0x04;
    CheckRefused(in, BurnGateRowV2::E4, "VERIFIER_E verdict has unknown bits");
    in.verifier->verdict = 0x80 | ETH_OBSERVATION_VERDICT_CERT_CONFLICT;
    CheckRefused(in, BurnGateRowV2::E4, "VERIFIER_E verdict has unknown bits");
    // Both bits: E4 is checked first.
    in.verifier->verdict = ETH_OBSERVATION_VERDICT_SETS_MISMATCH | ETH_OBSERVATION_VERDICT_CERT_CONFLICT;
    CheckRefused(in, BurnGateRowV2::E4, "observed verifier set hashes differ from the lineage's sets");
    // E5 by the verdict alone (the index also sets the sticky conflict).
    in.verifier->verdict = ETH_OBSERVATION_VERDICT_CERT_CONFLICT;
    CheckRefused(in, BurnGateRowV2::E5, "observed latestCertId differs from B3's certificate");
    in.verifier->verdict = 0;
    CheckReady(in);
}

BOOST_AUTO_TEST_CASE(e6_handover_certified)
{
    // Reachable states: the lineage rotated into e_B3 = 5 only after it
    // applied an epoch-4 certificate (node::ProjectFreshLineage), which is
    // then the finalized checkpoint, so finalized_epoch is 4 (the minimum a
    // chain produces) or 5 once Set'_5 certifies. E6 passes for both.
    BurnGateInputsV2 in{ReadyInputs()};
    in.verifier->state.current_epoch = 4; // e_B3 5 > e_E 4
    in.lineage.finalized_epoch = 4;       // >= e_B3 - 1
    CheckReady(in);
    in.lineage.finalized_epoch = 5;
    CheckReady(in);
    // Defensive inputs no lineage can produce (a rotation without the
    // handover certificate): the row refuses them.
    in.lineage.finalized_epoch = 3;
    CheckRefused(in, BurnGateRowV2::E6, "the handover into the lineage epoch is not certified on B3");
    in.lineage.finalized_epoch.reset();
    CheckRefused(in, BurnGateRowV2::E6, "the handover into the lineage epoch is not certified on B3");
    // Not applicable when the verifier is at B3's epoch.
    in.verifier->state.current_epoch = 5;
    CheckReady(in);
}

BOOST_AUTO_TEST_CASE(e7_token_flags)
{
    {
        BurnGateInputsV2 in{ReadyInputs()};
        in.token.reset();
        CheckRefused(in, BurnGateRowV2::E7, "no TOKEN_FLAGS observation");
    }
    {
        BurnGateInputsV2 in{ReadyInputs()};
        in.token->t_obs = T - GATE_M.observation_max_age_seconds;
        CheckReady(in);
        in.token->t_obs -= 1;
        CheckRefused(in, BurnGateRowV2::E7, "TOKEN_FLAGS observation is stale");
        in.token->t_obs = T + 1;
        CheckRefused(in, BurnGateRowV2::E7, "TOKEN_FLAGS observation is newer than now_eff");
    }
    {
        BurnGateInputsV2 in{ReadyInputs()};
        in.token->state.paused = true;
        CheckRefused(in, BurnGateRowV2::E7, "token is paused");
    }
    {
        BurnGateInputsV2 in{ReadyInputs()};
        in.token->state.deprecated = true;
        CheckRefused(in, BurnGateRowV2::E7, "token is deprecated");
    }
    {
        BurnGateInputsV2 in{ReadyInputs()};
        in.token->state.vault_blacklisted = true;
        CheckRefused(in, BurnGateRowV2::E7, "vault is blacklisted by the token");
    }
}

BOOST_AUTO_TEST_CASE(e8_reserve_observation)
{
    {
        BurnGateInputsV2 in{ReadyInputs()};
        in.reserve.reset();
        CheckRefused(in, BurnGateRowV2::E8, "no RESERVE_E observation");
    }
    {
        BurnGateInputsV2 in{ReadyInputs()};
        in.reserve->t_obs = T - GATE_M.observation_max_age_seconds;
        CheckReady(in);
        in.reserve->t_obs -= 1;
        CheckRefused(in, BurnGateRowV2::E8, "RESERVE_E observation is stale");
        in.reserve->t_obs = T + 1;
        CheckRefused(in, BurnGateRowV2::E8, "RESERVE_E observation is newer than now_eff");
    }
    // A shortfall (balance below locked) bounds burns through the wind-down
    // margin, not the gate.
    {
        BurnGateInputsV2 in{ReadyInputs()};
        in.reserve->state.balance = 0;
        CheckReady(in);
    }
}

BOOST_AUTO_TEST_CASE(rows_report_the_first_failure_in_order)
{
    BurnGateInputsV2 in{ReadyInputs()};
    in.verifier.reset();
    in.token.reset();
    in.reserve.reset();
    in.lineage.finalized_height.reset();
    in.lineage.broken = true;
    CheckRefused(in, BurnGateRowV2::C2, "fresh finality lineage is broken");
    in.lineage.broken = false;
    CheckRefused(in, BurnGateRowV2::C3, "fresh finality lineage has no finalized checkpoint");
    in.lineage.finalized_height = 990;
    CheckRefused(in, BurnGateRowV2::E1, "no VERIFIER_E observation");
    in.verifier = ReadyInputs().verifier;
    CheckRefused(in, BurnGateRowV2::E7, "no TOKEN_FLAGS observation");
    in.token = ReadyInputs().token;
    in.token->state.paused = true;
    CheckRefused(in, BurnGateRowV2::E7, "token is paused");
    in.token->state.paused = false;
    CheckRefused(in, BurnGateRowV2::E8, "no RESERVE_E observation");
    in.reserve = ReadyInputs().reserve;
    CheckReady(in);
}

BOOST_AUTO_TEST_CASE(now_eff_from_b3_or_light_client_time)
{
    BOOST_CHECK_EQUAL(BurnGateNowEffV2(T, std::nullopt), T);
    BOOST_CHECK_EQUAL(BurnGateNowEffV2(T, uint64_t(T - 5)), T);
    BOOST_CHECK_EQUAL(BurnGateNowEffV2(T, uint64_t(T + 5)), T + 5);
    BOOST_CHECK_EQUAL(BurnGateNowEffV2(T, std::numeric_limits<uint64_t>::max()), INT64_MAX_V);

    const int64_t max_age{GATE_M.observation_max_age_seconds};
    BurnGateInputsV2 in{ReadyInputs()};
    in.verifier->t_obs = T - max_age; // fresh at T exactly
    // B3 time decides: the light client lags.
    in.now_eff = BurnGateNowEffV2(T, uint64_t(T - 1'000));
    CheckReady(in);
    in.now_eff = BurnGateNowEffV2(T + 1, uint64_t(T - 1'000));
    CheckRefused(in, BurnGateRowV2::E1, "VERIFIER_E observation is stale");
    // Light-client time decides: B3's clock lags proven L1 time.
    in.now_eff = BurnGateNowEffV2(T - 1'000, uint64_t(T));
    CheckReady(in);
    in.now_eff = BurnGateNowEffV2(T - 1'000, uint64_t(T + 1));
    CheckRefused(in, BurnGateRowV2::E1, "VERIFIER_E observation is stale");
    // A light-client time beyond int64 can only close the gate.
    in.now_eff = BurnGateNowEffV2(T, std::numeric_limits<uint64_t>::max());
    CheckRefused(in, BurnGateRowV2::E1, "VERIFIER_E observation is stale");
}

BOOST_AUTO_TEST_CASE(closes_at_is_the_earliest_time_row)
{
    const int64_t max_age{GATE_M.observation_max_age_seconds};
    // Each observation in turn the oldest.
    for (int which{0}; which < 3; ++which) {
        BurnGateInputsV2 in{ReadyInputs()};
        int64_t& t_obs{which == 0 ? in.verifier->t_obs : which == 1 ? in.token->t_obs : in.reserve->t_obs};
        t_obs = T - 50'000;
        const auto closes{ClosesAt(in)};
        BOOST_REQUIRE(closes);
        BOOST_CHECK_EQUAL(*closes, T - 50'000 + max_age + 1);
        // One second earlier still READY; at closes_at the gate is closed.
        in.now_eff = *closes - 1;
        CheckReady(in);
        in.now_eff = *closes;
        BOOST_CHECK(!EvaluateBurnGateV2(in, GATE_M).ready);
    }
    // The rotation age the earliest.
    {
        BurnGateInputsV2 in{ReadyInputs()};
        in.verifier->state.last_rotation_time = T - GATE_M.RotationAgeLimit() + 100;
        const auto closes{ClosesAt(in)};
        BOOST_REQUIRE(closes);
        BOOST_CHECK_EQUAL(*closes, T + 101);
        in.now_eff = *closes - 1;
        CheckReady(in);
        in.now_eff = *closes;
        CheckRefused(in, BurnGateRowV2::E2, "verifier rotation is older than the rotation age limit");
    }
    // No representable closing time: limits that overflow int64 never close.
    {
        BridgeV2GateParams wide{GATE_M};
        wide.observation_max_age_seconds = INT64_MAX_V;
        wide.set_expiry_seconds = INT64_MAX_V;
        wide.release_margin_seconds = 1;
        wide.burn_margin_seconds = 1;
        BOOST_REQUIRE(wide.Valid());
        const BurnGateInputsV2 in{ReadyInputs()};
        CheckReady(in, wide);
        BOOST_CHECK(!ClosesAt(in, wide));
    }
}

// ---------------------------------------------------------- reserve ledger

BOOST_AUTO_TEST_CASE(ledger_validity_and_reserve)
{
    ReserveLedgerV2 ledger;
    BOOST_CHECK(ledger.Valid());
    BOOST_CHECK_EQUAL(*ledger.Reserve(), 0);

    ledger = {.minted = 100, .burned = 120, .credited = 50, .transit = 7};
    BOOST_CHECK(ledger.Valid());
    BOOST_CHECK_EQUAL(*ledger.Reserve(), 30); // transit never counts
    ledger.burned = 150;
    BOOST_CHECK_EQUAL(*ledger.Reserve(), 0);
    ledger.burned = 151;
    BOOST_CHECK(!ledger.Valid());
    BOOST_CHECK(!ledger.Reserve());

    for (int field{0}; field < 4; ++field) {
        ReserveLedgerV2 negative{.minted = 10, .burned = 0, .credited = 0, .transit = 0};
        (field == 0 ? negative.minted : field == 1 ? negative.burned : field == 2 ? negative.credited : negative.transit) = -1;
        BOOST_CHECK(!negative.Valid());
        BOOST_CHECK(!negative.Reserve());
    }
    // minted + credited out of range.
    const ReserveLedgerV2 overflow{.minted = INT64_MAX_V, .burned = 0, .credited = 1, .transit = 0};
    BOOST_CHECK(!overflow.Valid());
    BOOST_CHECK(!overflow.Reserve());
}

BOOST_AUTO_TEST_CASE(record_mint_and_burn)
{
    ReserveLedgerV2 ledger;
    BOOST_CHECK(RecordMintV2(ledger, 1'000));
    BOOST_CHECK_EQUAL(ledger.minted, 1'000);
    BOOST_CHECK(!RecordMintV2(ledger, 0));
    BOOST_CHECK(!RecordMintV2(ledger, -5));
    BOOST_CHECK_EQUAL(ledger.minted, 1'000);

    BOOST_CHECK(RecordBurnV2(ledger, 400));
    BOOST_CHECK_EQUAL(ledger.burned, 400);
    BOOST_CHECK(!RecordBurnV2(ledger, 601)); // above the reserve of 600
    BOOST_CHECK(!RecordBurnV2(ledger, 0));
    BOOST_CHECK_EQUAL(ledger.burned, 400);
    BOOST_CHECK(RecordBurnV2(ledger, 600));
    BOOST_CHECK_EQUAL(*ledger.Reserve(), 0);

    // Checked overflow: minted alone, and minted + credited.
    ReserveLedgerV2 high{.minted = INT64_MAX_V - 5, .burned = 0, .credited = 0, .transit = 0};
    const ReserveLedgerV2 before{high};
    BOOST_CHECK(!RecordMintV2(high, 6));
    BOOST_CHECK(high == before);
    BOOST_CHECK(RecordMintV2(high, 5));
    BOOST_CHECK_EQUAL(high.minted, INT64_MAX_V);
    ReserveLedgerV2 credited{.minted = 10, .burned = 0, .credited = INT64_MAX_V - 10, .transit = 0};
    BOOST_CHECK(credited.Valid());
    BOOST_CHECK(!RecordMintV2(credited, 1));
    BOOST_CHECK_EQUAL(credited.minted, 10);
    // An invalid ledger takes no update.
    ReserveLedgerV2 invalid{.minted = 1, .burned = 2, .credited = 0, .transit = 0};
    BOOST_CHECK(!RecordMintV2(invalid, 1));
    BOOST_CHECK(!RecordBurnV2(invalid, 1));
}

BOOST_AUTO_TEST_CASE(burn_within_reserve_counts_block_burns_and_credit)
{
    ReserveLedgerV2 ledger{.minted = 100, .burned = 0, .credited = 0, .transit = 0};
    BOOST_CHECK(BurnWithinReserve(100, ledger));
    BOOST_CHECK(!BurnWithinReserve(101, ledger));
    BOOST_CHECK(!BurnWithinReserve(0, ledger));
    BOOST_CHECK(!BurnWithinReserve(-1, ledger));
    // An earlier burn of the block, already in burned_v, shrinks the bound.
    BOOST_REQUIRE(RecordBurnV2(ledger, 60));
    BOOST_CHECK(BurnWithinReserve(40, ledger));
    BOOST_CHECK(!BurnWithinReserve(41, ledger));
    // Credited migration amounts back burns; transit does not.
    ledger.credited = 20;
    ledger.transit = 1'000;
    BOOST_CHECK(BurnWithinReserve(60, ledger));
    BOOST_CHECK(!BurnWithinReserve(61, ledger));
}

BOOST_AUTO_TEST_CASE(sum_proven_deposits_splits_at_the_cut)
{
    const ProvenDepositMapV2 base{{0, 10}, {1, 20}, {5, 30}};
    const ProvenDepositMapV2 overlay{{2, 5}, {6, 40}};
    const auto sums{SumProvenDeposits(base, overlay, 3)};
    BOOST_REQUIRE(sums);
    BOOST_CHECK_EQUAL(sums->cut, 3U);
    BOOST_CHECK_EQUAL(sums->below_cut, 35);
    BOOST_CHECK_EQUAL(sums->from_cut, 70);
    // The cut id itself is "from" the cut.
    const auto at_five{SumProvenDeposits(base, overlay, 5)};
    BOOST_REQUIRE(at_five);
    BOOST_CHECK_EQUAL(at_five->below_cut, 35);
    BOOST_CHECK_EQUAL(at_five->from_cut, 70);
    const auto at_six{SumProvenDeposits(base, overlay, 6)};
    BOOST_REQUIRE(at_six);
    BOOST_CHECK_EQUAL(at_six->below_cut, 65);
    BOOST_CHECK_EQUAL(at_six->from_cut, 40);
    const auto none_below{SumProvenDeposits(base, overlay, 0)};
    BOOST_REQUIRE(none_below);
    BOOST_CHECK_EQUAL(none_below->below_cut, 0);
    BOOST_CHECK_EQUAL(none_below->from_cut, 105);
    const auto empty{SumProvenDeposits({}, {}, 7)};
    BOOST_REQUIRE(empty);
    BOOST_CHECK(*empty == (ProvenDepositSumsV2{7, 0, 0}));

    // Refusals: an id in both maps, a non-positive amount, an overflow.
    BOOST_CHECK(!SumProvenDeposits(base, {{1, 20}}, 3));
    BOOST_CHECK(!SumProvenDeposits({{0, 0}}, {}, 3));
    BOOST_CHECK(!SumProvenDeposits({}, {{4, -1}}, 3));
    const CAmount half{INT64_MAX_V / 2 + 1};
    BOOST_CHECK(!SumProvenDeposits({{0, half}}, {{1, half}}, 3));
    BOOST_CHECK(!SumProvenDeposits({{8, half}, {9, half}}, {}, 3));
    // The same amounts split across the cut fit.
    const auto split{SumProvenDeposits({{0, half}}, {{9, half}}, 3)};
    BOOST_REQUIRE(split);
    BOOST_CHECK_EQUAL(split->below_cut, half);
    BOOST_CHECK_EQUAL(split->from_cut, half);
}

BOOST_AUTO_TEST_CASE(split_proven_deposits_at_cut_matches_the_full_scan)
{
    // SplitProvenDepositsAtCut (consensus: walks only ids >= cut and takes
    // P_pre from the running totals) equals SumProvenDeposits (the full
    // scan) on every cut, for deterministic pseudo-random maps.
    const auto total_of{[](const ProvenDepositMapV2& map) {
        CAmount total{0};
        for (const auto& [id, amount] : map) total += amount;
        return total;
    }};
    uint64_t seed{0x5eed};
    const auto next_random{[&seed]() {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        return seed >> 33;
    }};
    for (int round{0}; round < 50; ++round) {
        ProvenDepositMapV2 base;
        ProvenDepositMapV2 overlay;
        const uint64_t count{next_random() % 40};
        for (uint64_t i{0}; i < count; ++i) {
            const uint64_t id{next_random() % 64};
            const CAmount amount{static_cast<CAmount>(next_random() % 1'000'000 + 1)};
            if (base.contains(id) || overlay.contains(id)) continue;
            (next_random() % 3 == 0 ? overlay : base).emplace(id, amount);
        }
        for (uint64_t cut{0}; cut <= 65; ++cut) {
            const auto full{SumProvenDeposits(base, overlay, cut)};
            const auto split{SplitProvenDepositsAtCut(base, total_of(base), overlay, total_of(overlay), cut)};
            BOOST_REQUIRE(full && split);
            BOOST_CHECK(*full == *split);
        }
    }
    const ProvenDepositMapV2 base{{0, 10}, {1, 20}, {5, 30}};
    const ProvenDepositMapV2 overlay{{2, 5}, {6, 40}};
    BOOST_CHECK(SplitProvenDepositsAtCut(base, 60, overlay, 45, 3) == (std::optional<ProvenDepositSumsV2>{{3, 35, 70}}));
    BOOST_CHECK(SplitProvenDepositsAtCut({}, 0, {}, 0, 7) == (std::optional<ProvenDepositSumsV2>{{7, 0, 0}}));
    // Refusals: a walked id in both maps, a non-positive walked amount,
    // negative totals, totals below P_post, and overflow.
    BOOST_CHECK(!SplitProvenDepositsAtCut(base, 60, {{5, 30}}, 30, 3));
    BOOST_CHECK(!SplitProvenDepositsAtCut({{4, 0}}, 0, {}, 0, 3));
    BOOST_CHECK(!SplitProvenDepositsAtCut({}, 0, {{4, -1}}, -1, 3));
    BOOST_CHECK(!SplitProvenDepositsAtCut(base, -1, overlay, 45, 3));
    BOOST_CHECK(!SplitProvenDepositsAtCut(base, 60, overlay, -1, 3));
    BOOST_CHECK(!SplitProvenDepositsAtCut(base, 10, overlay, 0, 3));
    const CAmount half{INT64_MAX_V / 2 + 1};
    BOOST_CHECK(!SplitProvenDepositsAtCut({{0, half}}, half, {{1, half}}, half, 3));
    BOOST_CHECK(!SplitProvenDepositsAtCut({{8, half}, {9, half}}, INT64_MAX_V, {}, 0, 3));
    const auto fits{SplitProvenDepositsAtCut({{0, half - 1}}, half - 1, {{9, half}}, half, 3)};
    BOOST_REQUIRE(fits);
    BOOST_CHECK_EQUAL(fits->below_cut, half - 1);
    BOOST_CHECK_EQUAL(fits->from_cut, half);
}

// -------------------------------------------------------------- wind-down

namespace {

ReserveStateV2 Observed(const uint64_t cut, const CAmount locked, const CAmount balance,
                        const CAmount absorbed = 0)
{
    ReserveStateV2 state;
    state.next_deposit_id = cut;
    state.locked = locked;
    state.balance = balance;
    state.absorbed_total = absorbed;
    return state;
}

ProvenDepositSumsV2 Sums(const ReserveStateV2& reserve, const ProvenDepositMapV2& base,
                         const ProvenDepositMapV2& overlay = {})
{
    const auto sums{SumProvenDeposits(base, overlay, reserve.next_deposit_id)};
    BOOST_REQUIRE(sums);
    return *sums;
}

constexpr uint32_t PPM{100'000}; // profile M and R: 10 %

} // namespace

BOOST_AUTO_TEST_CASE(wind_down_bound_at_the_limit_and_one_unit_above)
{
    // Ethereum: deposits 0..2 (400 + 300 + 300) locked, nothing released.
    // B3: all three proven and minted.
    const ReserveStateV2 reserve{Observed(3, 1'000, 1'000)};
    const ProvenDepositMapV2 base{{0, 400}, {1, 300}, {2, 300}};
    const ProvenDepositSumsV2 sums{Sums(reserve, base)};
    ReserveLedgerV2 ledger{.minted = 1'000, .burned = 0, .credited = 0, .transit = 0};

    const auto terms{ComputeWindDownTerms(reserve, sums, ledger, PPM)};
    BOOST_REQUIRE(terms);
    BOOST_CHECK(*terms == (WindDownTermsV2{.observed = 1'000, .released_lower_bound = 0,
                                           .unreleased = 0, .margin = 0, .headroom = 1'000}));
    BOOST_CHECK(WindDownAllows(1'000, reserve, sums, ledger, PPM));
    BOOST_CHECK(!WindDownAllows(1'001, reserve, sums, ledger, PPM));
    BOOST_CHECK(!WindDownAllows(0, reserve, sums, ledger, PPM));
    BOOST_CHECK(!WindDownAllows(-1, reserve, sums, ledger, PPM));

    // Same-block burns count: an earlier burn of 600 in the block is
    // unreleased at the observation.
    BOOST_REQUIRE(RecordBurnV2(ledger, 600));
    BOOST_CHECK_EQUAL(ComputeWindDownTerms(reserve, sums, ledger, PPM)->unreleased, 600);
    BOOST_CHECK(WindDownAllows(400, reserve, sums, ledger, PPM));
    BOOST_CHECK(!WindDownAllows(401, reserve, sums, ledger, PPM));

    // A release the observation already reflects (locked and balance down
    // by 600) is subtracted through R_v, so the remaining 400 still redeem.
    const ReserveStateV2 released{Observed(3, 400, 400)};
    const auto after{ComputeWindDownTerms(released, Sums(released, base), ledger, PPM)};
    BOOST_REQUIRE(after);
    BOOST_CHECK_EQUAL(after->released_lower_bound, 600);
    BOOST_CHECK_EQUAL(after->unreleased, 0);
    BOOST_CHECK_EQUAL(after->headroom, 400);
    BOOST_CHECK(WindDownAllows(400, released, Sums(released, base), ledger, PPM));
    BOOST_CHECK(!WindDownAllows(401, released, Sums(released, base), ledger, PPM));
}

BOOST_AUTO_TEST_CASE(wind_down_late_proof_below_the_cut_and_deposit_after_it)
{
    // Ethereum: deposits 0..2 (400 + 300 + 300), then a release of 200;
    // locked = balance = 800 at the observation (cut 3).
    const ReserveStateV2 reserve{Observed(3, 800, 800)};
    // B3 has proven only deposits 0 and 1 (P_pre 700) and burned 200.
    ProvenDepositMapV2 base{{0, 400}, {1, 300}};
    const ReserveLedgerV2 ledger{.minted = 700, .burned = 200, .credited = 0, .transit = 0};
    auto terms{ComputeWindDownTerms(reserve, Sums(reserve, base), ledger, PPM)};
    BOOST_REQUIRE(terms);
    // The unproven deposit hides the release: R_v = max(0, 700 - 800) = 0.
    BOOST_CHECK_EQUAL(terms->released_lower_bound, 0);
    BOOST_CHECK_EQUAL(terms->unreleased, 200);
    BOOST_CHECK_EQUAL(terms->headroom, 600);

    // The late proof of deposit 2 (below the cut) loosens the bound.
    const ProvenDepositMapV2 overlay{{2, 300}};
    terms = ComputeWindDownTerms(reserve, Sums(reserve, base, overlay), ledger, PPM);
    BOOST_REQUIRE(terms);
    BOOST_CHECK_EQUAL(terms->released_lower_bound, 200);
    BOOST_CHECK_EQUAL(terms->unreleased, 0);
    BOOST_CHECK_EQUAL(terms->headroom, 800);

    // A deposit after the cut (id 3, 250) redeems at once through P_post.
    base.emplace(2, 300);
    terms = ComputeWindDownTerms(reserve, Sums(reserve, base, {{3, 250}}), ledger, PPM);
    BOOST_REQUIRE(terms);
    BOOST_CHECK_EQUAL(terms->observed, 1'050);
    BOOST_CHECK_EQUAL(terms->headroom, 1'050);
    BOOST_CHECK(WindDownAllows(1'050, reserve, Sums(reserve, base, {{3, 250}}), ledger, PPM));
    BOOST_CHECK(!WindDownAllows(1'051, reserve, Sums(reserve, base, {{3, 250}}), ledger, PPM));
}

BOOST_AUTO_TEST_CASE(wind_down_margin_zero_then_ten_percent_rounded_up)
{
    // Balance covers locked: no margin.
    BOOST_CHECK_EQUAL(*WindDownMarginV2(Observed(0, 1'000, 1'000), PPM), 0);
    BOOST_CHECK_EQUAL(*WindDownMarginV2(Observed(0, 1'000, 1'500), PPM), 0);
    // A shortfall: 10 % of the balance, rounded up.
    BOOST_CHECK_EQUAL(*WindDownMarginV2(Observed(0, 1'000, 900), PPM), 90);
    BOOST_CHECK_EQUAL(*WindDownMarginV2(Observed(0, 1'000, 905), PPM), 91);
    BOOST_CHECK_EQUAL(*WindDownMarginV2(Observed(0, 1'000, 1), PPM), 1);
    BOOST_CHECK_EQUAL(*WindDownMarginV2(Observed(0, 1'000, 0), PPM), 0);
    BOOST_CHECK_EQUAL(*WindDownMarginV2(Observed(0, 1'000, 900), 0), 0);
    BOOST_CHECK_EQUAL(*WindDownMarginV2(Observed(0, 1'000, 900), 1'000'000), 900);
    // Exact at the int64 edge.
    BOOST_CHECK_EQUAL(*WindDownMarginV2(Observed(0, INT64_MAX_V, INT64_MAX_V - 1), PPM),
                      922'337'203'685'477'581);
    BOOST_CHECK_EQUAL(*WindDownMarginV2(Observed(0, INT64_MAX_V, INT64_MAX_V - 1), 1'000'000),
                      INT64_MAX_V - 1);
    BOOST_CHECK(!WindDownMarginV2(Observed(0, 1'000, 900), 1'000'001));
    BOOST_CHECK(!WindDownMarginV2(Observed(0, 1'000, -1), PPM));

    // The margin bounds burns after a partial seizure: 1,000 minted and
    // locked, 100 seized (balance 900): headroom 900 - 90 = 810.
    const ReserveStateV2 seized{Observed(1, 1'000, 900)};
    const ProvenDepositSumsV2 sums{Sums(seized, {{0, 1'000}})};
    const ReserveLedgerV2 ledger{.minted = 1'000, .burned = 0, .credited = 0, .transit = 0};
    BOOST_CHECK(WindDownAllows(810, seized, sums, ledger, PPM));
    BOOST_CHECK(!WindDownAllows(811, seized, sums, ledger, PPM));
    // Unreleased leaves beyond the headroom refuse every burn.
    const ReserveLedgerV2 heavy{.minted = 1'000, .burned = 900, .credited = 0, .transit = 0};
    const auto terms{ComputeWindDownTerms(seized, sums, heavy, PPM)};
    BOOST_REQUIRE(terms);
    BOOST_CHECK_EQUAL(terms->headroom, -90);
    BOOST_CHECK(!WindDownAllows(1, seized, sums, heavy, PPM));
}

BOOST_AUTO_TEST_CASE(wind_down_refusals)
{
    const ReserveStateV2 reserve{Observed(3, 1'000, 1'000)};
    const ProvenDepositSumsV2 sums{Sums(reserve, {{0, 1'000}})};
    const ReserveLedgerV2 ledger{.minted = 1'000, .burned = 0, .credited = 0, .transit = 0};
    BOOST_REQUIRE(WindDownAllows(1, reserve, sums, ledger, PPM));
    // Sums split at another cut than the observation's.
    ProvenDepositSumsV2 other{sums};
    other.cut = 4;
    BOOST_CHECK(!ComputeWindDownTerms(reserve, other, ledger, PPM));
    BOOST_CHECK(!WindDownAllows(1, reserve, other, ledger, PPM));
    // An invalid ledger, a negative amount, an invalid ppm.
    const ReserveLedgerV2 invalid{.minted = 1, .burned = 2, .credited = 0, .transit = 0};
    BOOST_CHECK(!WindDownAllows(1, reserve, sums, invalid, PPM));
    BOOST_CHECK(!WindDownAllows(1, Observed(3, -1, 1'000), sums, ledger, PPM));
    BOOST_CHECK(!WindDownAllows(1, reserve, sums, ledger, 1'000'001));
    ProvenDepositSumsV2 negative{sums};
    negative.from_cut = -1;
    BOOST_CHECK(!WindDownAllows(1, reserve, negative, ledger, PPM));
    // Checked overflow: B + P_post, and P_pre + C_v.
    const ReserveStateV2 rich{Observed(3, INT64_MAX_V, INT64_MAX_V)};
    ProvenDepositSumsV2 post{3, 0, 1};
    BOOST_CHECK(!ComputeWindDownTerms(rich, post, ledger, PPM));
    BOOST_CHECK(!WindDownAllows(1, rich, post, ledger, PPM));
    const ReserveLedgerV2 credited{.minted = 0, .burned = 0, .credited = 2, .transit = 0};
    const ProvenDepositSumsV2 pre{3, INT64_MAX_V - 1, 0};
    BOOST_CHECK(!ComputeWindDownTerms(reserve, pre, credited, PPM));
}

// ----------------------------------------------------- transit, invariant R

BOOST_AUTO_TEST_CASE(transit_credits_only_after_absorb)
{
    // v' receives a migration leaf of 1,000: transit, not reserve.
    ReserveLedgerV2 ledger{.minted = 0, .burned = 0, .credited = 0, .transit = 1'000};
    BOOST_CHECK_EQUAL(*ledger.Reserve(), 0);

    // An observation anchored before absorb(): absorbedTotal 0 credits
    // nothing, and invariant R holds (locked 0 >= 0 + 0 - 0): no conflict.
    auto moved{CreditTransit(ledger, 0)};
    BOOST_REQUIRE(moved);
    BOOST_CHECK_EQUAL(*moved, 0);
    BOOST_CHECK(ledger == (ReserveLedgerV2{.minted = 0, .burned = 0, .credited = 0, .transit = 1'000}));
    BOOST_CHECK(InvariantRHolds(/*locked=*/0, /*p_below=*/0, ledger));
    BOOST_CHECK_EQUAL(*ledger.Reserve(), 0);

    // absorb() counted 990 (a USDT fee lost 10): credit 990, never more.
    moved = CreditTransit(ledger, 990);
    BOOST_REQUIRE(moved);
    BOOST_CHECK_EQUAL(*moved, 990);
    BOOST_CHECK(ledger == (ReserveLedgerV2{.minted = 0, .burned = 0, .credited = 990, .transit = 10}));
    BOOST_CHECK(InvariantRHolds(/*locked=*/990, /*p_below=*/0, ledger));
    BOOST_CHECK_EQUAL(*ledger.Reserve(), 990);
    // The same absorbedTotal again credits nothing more.
    BOOST_CHECK_EQUAL(*CreditTransit(ledger, 990), 0);
    BOOST_CHECK_EQUAL(ledger.credited, 990);
    // Later absorbed strays credit the remaining transit, never beyond it.
    BOOST_CHECK_EQUAL(*CreditTransit(ledger, 2'000), 10);
    BOOST_CHECK(ledger == (ReserveLedgerV2{.minted = 0, .burned = 0, .credited = 1'000, .transit = 0}));
    BOOST_CHECK_EQUAL(*CreditTransit(ledger, 5'000), 0);
    BOOST_CHECK_EQUAL(ledger.credited, 1'000);

    // Absorbed strays before the arrival credit earlier, up to absorbedTotal.
    ReserveLedgerV2 early{.minted = 0, .burned = 0, .credited = 0, .transit = 1'000};
    BOOST_CHECK_EQUAL(*CreditTransit(early, 50), 50);
    BOOST_CHECK_EQUAL(early.credited, 50);
    BOOST_CHECK_EQUAL(early.transit, 950);

    // Refusals leave the ledger unchanged.
    ReserveLedgerV2 kept{.minted = 0, .burned = 0, .credited = 0, .transit = 1'000};
    BOOST_CHECK(!CreditTransit(kept, -1));
    BOOST_CHECK_EQUAL(kept.transit, 1'000);
    ReserveLedgerV2 invalid{.minted = 0, .burned = 5, .credited = 0, .transit = 1'000};
    BOOST_CHECK(!CreditTransit(invalid, 1'000));
    BOOST_CHECK_EQUAL(invalid.transit, 1'000);
    // Checked overflow: the credit would push minted + credited out of range.
    ReserveLedgerV2 high{.minted = INT64_MAX_V, .burned = 0, .credited = 0, .transit = 1};
    BOOST_CHECK(!CreditTransit(high, 1));
    BOOST_CHECK_EQUAL(high.transit, 1);
}

BOOST_AUTO_TEST_CASE(invariant_r_holds_and_fails)
{
    // locked >= P + C_v - burned_v.
    const ReserveLedgerV2 ledger{.minted = 1'000, .burned = 300, .credited = 100, .transit = 0};
    // 1,000 + 100 - 300 = 800.
    BOOST_CHECK(InvariantRHolds(800, 1'000, ledger));
    BOOST_CHECK_EQUAL(*InvariantRMarginV2(800, 1'000, ledger), 0);
    // A forged drain beyond the leaves: locked one unit short.
    BOOST_CHECK(!InvariantRHolds(799, 1'000, ledger));
    BOOST_CHECK_EQUAL(*InvariantRMarginV2(799, 1'000, ledger), -1);
    BOOST_CHECK(InvariantRHolds(900, 1'000, ledger));
    BOOST_CHECK_EQUAL(*InvariantRMarginV2(900, 1'000, ledger), 100);
    // Unproven deposits (P below the true sum) only add slack.
    BOOST_CHECK(InvariantRHolds(800, 700, ledger));

    // Negative inputs and an invalid ledger fail closed.
    BOOST_CHECK(!InvariantRHolds(-1, 0, ReserveLedgerV2{}));
    BOOST_CHECK(!InvariantRHolds(0, -1, ReserveLedgerV2{}));
    const ReserveLedgerV2 invalid{.minted = 0, .burned = 1, .credited = 0, .transit = 0};
    BOOST_CHECK(!InvariantRHolds(1'000, 0, invalid));
    BOOST_CHECK(!InvariantRMarginV2(1'000, 0, invalid));

    // Exact at the int64 edge, where the margin is out of range.
    const ReserveLedgerV2 big{.minted = INT64_MAX_V, .burned = INT64_MAX_V, .credited = 0, .transit = 0};
    BOOST_CHECK(InvariantRHolds(INT64_MAX_V, INT64_MAX_V, big));
    BOOST_CHECK_EQUAL(*InvariantRMarginV2(INT64_MAX_V, INT64_MAX_V, big), INT64_MAX_V);
    BOOST_CHECK(InvariantRHolds(INT64_MAX_V, 0, big));
    BOOST_CHECK(!InvariantRMarginV2(INT64_MAX_V, 0, big)); // 2 * INT64_MAX
    const ReserveLedgerV2 owed{.minted = 0, .burned = 0, .credited = 1, .transit = 0};
    BOOST_CHECK(!InvariantRHolds(INT64_MAX_V, INT64_MAX_V, owed)); // short by one
    BOOST_CHECK(!InvariantRMarginV2(INT64_MAX_V, INT64_MAX_V, owed));
    BOOST_CHECK(InvariantRHolds(INT64_MAX_V, INT64_MAX_V - 1, owed));
}

BOOST_AUTO_TEST_SUITE_END()
