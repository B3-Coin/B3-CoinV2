// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/flowmesh_action_stage.h>
#include <node/flowmesh_client.h>
#include <node/flowmesh_client_outcome.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <string>

namespace {

interfaces::FlowMeshActionReceipt Receipt(const std::string& state, bool verified = false)
{
    interfaces::FlowMeshActionReceipt out;
    out.action_id = uint256{uint8_t{1}};
    out.state = state;
    out.certificate_verified = verified;
    if (state == "certified_inclusion") {
        out.microblock_sequence = 5;
        out.microblock_hash = uint256{uint8_t{9}};
    }
    return out;
}

interfaces::FlowMeshActionOutcome Outcome(const std::string& status)
{
    interfaces::FlowMeshActionOutcome out;
    out.status = status;
    out.final = true;
    out.reason_code = status == "not_executed" ? "NOT_EXECUTED_SEQUENCE_CONSUMED" : "APPLIED";
    out.microblock_sequence = 5;
    out.microblock_hash = uint256{uint8_t{9}};
    if (status == "not_executed") {
        out.consumed_by = interfaces::FlowMeshActionOutcome::ConsumedBy{uint256{uint8_t{2}}, 5, uint256{uint8_t{9}}};
    }
    return out;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(flowmesh_client_outcome_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(stage_mapping_table)
{
    using node::FlowMeshReceiptStage;
    auto applied{Receipt("certified_inclusion", true)};
    applied.outcome = Outcome("applied");
    applied.outcome_verified = true;
    BOOST_CHECK_EQUAL(FlowMeshReceiptStage(applied, true, true), "outcome_verified");
    auto not_executed{Receipt("rejected")};
    not_executed.outcome = Outcome("not_executed");
    not_executed.outcome_verified = true;
    BOOST_CHECK_EQUAL(FlowMeshReceiptStage(not_executed, true, true), "not_executed");
    BOOST_CHECK_EQUAL(FlowMeshReceiptStage(Receipt("certified_inclusion", true), true, true), "certified");
    // An unverified inclusion claim is not "certified".
    BOOST_CHECK_EQUAL(FlowMeshReceiptStage(Receipt("certified_inclusion", false), true, true), "sent");
    BOOST_CHECK_EQUAL(FlowMeshReceiptStage(Receipt("admitted"), true, true), "admitted");
    BOOST_CHECK_EQUAL(FlowMeshReceiptStage(Receipt("queued"), false, false), "queued");
    BOOST_CHECK_EQUAL(FlowMeshReceiptStage(Receipt("rejected"), true, true), "refused");
    BOOST_CHECK_EQUAL(FlowMeshReceiptStage(Receipt("unknown"), true, true), "sent");
    BOOST_CHECK_EQUAL(FlowMeshReceiptStage(Receipt("unknown"), false, true), "signed");
    BOOST_CHECK_EQUAL(FlowMeshReceiptStage(Receipt("unknown"), false, false), "unknown");
    // An outcome without the verified flag is not a verified stage.
    auto unverified{Receipt("certified_inclusion", true)};
    unverified.outcome = Outcome("applied");
    BOOST_CHECK_EQUAL(FlowMeshReceiptStage(unverified, true, true), "certified");
}

BOOST_AUTO_TEST_CASE(bound_receipt_strips_inconsistent_outcome)
{
    const auto strip = [](interfaces::FlowMeshActionReceipt receipt) {
        node::FlowMeshStripInconsistentOutcome(receipt);
        return receipt;
    };
    auto applied{Receipt("certified_inclusion", true)};
    applied.outcome = Outcome("applied");
    applied.outcome_verified = true;
    BOOST_CHECK(strip(applied).outcome && strip(applied).outcome_verified);
    // Another entry's outcome, an unverified inclusion, a missing flag or a
    // non-inclusion state is stripped.
    auto other_entry{applied};
    other_entry.outcome->microblock_hash = uint256{uint8_t{8}};
    BOOST_CHECK(!strip(other_entry).outcome && !strip(other_entry).outcome_verified);
    auto unverified{applied};
    unverified.certificate_verified = false;
    BOOST_CHECK(!strip(unverified).outcome);
    auto flagless{applied};
    flagless.outcome_verified = false;
    BOOST_CHECK(!strip(flagless).outcome);
    auto queued{applied};
    queued.state = "queued";
    BOOST_CHECK(!strip(queued).outcome);
    auto no_outcome{Receipt("certified_inclusion", true)};
    no_outcome.outcome_verified = true;
    BOOST_CHECK(!strip(no_outcome).outcome_verified);
    // not_executed needs "rejected" and a consumer.
    auto proven{Receipt("rejected")};
    proven.outcome = Outcome("not_executed");
    proven.outcome_verified = true;
    BOOST_CHECK(strip(proven).outcome && strip(proven).outcome_verified);
    auto no_consumer{proven};
    no_consumer.outcome->consumed_by.reset();
    BOOST_CHECK(!strip(no_consumer).outcome);
    auto included_state{proven};
    included_state.state = "certified_inclusion";
    included_state.certificate_verified = true;
    included_state.microblock_hash = uint256{uint8_t{9}};
    BOOST_CHECK(!strip(included_state).outcome);
}

BOOST_AUTO_TEST_CASE(receipt_json_scopes)
{
    auto receipt{Receipt("certified_inclusion", true)};
    receipt.reason_code = "APPLIED";
    receipt.stage = "outcome_verified";
    receipt.outcome = Outcome("applied");
    receipt.outcome->fill = interfaces::FlowMeshActionOutcome::Fill{"bid", true, false, 3, 7, 21, 0, 21};
    receipt.outcome_verified = true;
    // PUBLIC_V1 keeps exactly the previous keys.
    const auto v1{node::FlowMeshClientReceiptJson(receipt)};
    for (const auto* key : {"reason_code", "stage", "outcome"}) BOOST_CHECK(!v1.exists(key));
    const auto opt_in{node::FlowMeshClientReceiptJson(receipt, node::FlowMeshReceiptJsonScope::PUBLIC_OUTCOME_V1)};
    BOOST_CHECK_EQUAL(opt_in["reason_code"].get_str(), "APPLIED");
    BOOST_CHECK(!opt_in.exists("stage") && !opt_in.exists("outcome"));
    const auto local{node::FlowMeshClientReceiptJson(receipt, node::FlowMeshReceiptJsonScope::LOCAL_RPC)};
    BOOST_CHECK_EQUAL(local["stage"].get_str(), "outcome_verified");
    BOOST_CHECK_EQUAL(local["outcome"]["status"].get_str(), "applied");
    BOOST_CHECK_EQUAL(local["outcome"]["reason_text"].get_str(), "Executed in a certified batch.");
    BOOST_CHECK_EQUAL(local["outcome"]["fill"]["quantity"].getInt<int64_t>(), 3);
    BOOST_CHECK(!local["outcome"].exists("consumed_by"));
    // An unknown reason name is displayed as unrecognised, never as final text.
    receipt.outcome->reason_code = "SOMETHING_NEW";
    const auto unknown{node::FlowMeshClientReceiptJson(receipt, node::FlowMeshReceiptJsonScope::LOCAL_RPC)};
    BOOST_CHECK_EQUAL(unknown["outcome"]["reason_text"].get_str(), "Unrecognised reason; outcome unchanged.");
}

BOOST_AUTO_TEST_SUITE_END()
