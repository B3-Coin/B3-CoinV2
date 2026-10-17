// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <crypto/bls.h>
#include <flowmesh2/admission/admission.h>
#include <flowmesh2/core/action.h>
#include <flowmesh2/core/facts.h>
#include <flowmesh2/net/client_api.h>
#include <flowmesh2/read/read_service.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/sim/cluster.h>
#include <flowmesh2/sim/faults.h>
#include <flowmesh2/sim/scenario.h>
#include <flowmesh2/sim/sim_client.h>
#include <flowmesh2/sim/sim_node.h>
#include <flowmesh2/sim/trace.h>
#include <flowmesh2/test/util/require.h>
#include <flowmesh2/test/util/slot_fixtures.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::sim;
using namespace flowmesh2::test;

namespace {

//! Every failed or missing check of `report`, one per line.
std::string Failures(const RunReport& report)
{
    std::string out{std::string{ToString(report.variant)} + " seed " + std::to_string(report.seed) + ":"};
    if (!report.harness_error.empty()) out += " harness: " + report.harness_error + ";";
    for (const auto& [id, result] : report.checks) {
        if (!result.ran || !result.passed) out += " " + id + (result.ran ? " failed: " : " not run: ") + result.detail + ";";
    }
    return out;
}

void RequirePassed(const RunReport& report)
{
    BOOST_CHECK_MESSAGE(report.Passed(), Failures(report));
    for (const std::string_view id : ManifestOf(report.variant)) {
        const auto it{report.checks.find(id)};
        BOOST_CHECK_MESSAGE(it != report.checks.end() && it->second.ran && it->second.passed, std::string{id} + " " + Failures(report));
    }
}

/** The walking skeleton's slot-5 header hash and final state root (seed 1),
 * pinned from the outline's first run; every seed gives the same values.
 */
constexpr std::string_view GOLDEN_SLOT5_HEADER{"4e27a3ac8497f85c5aed0d0f62ed85b42b54bc40f28c1e8c008ec497ae520c60"};
constexpr std::string_view GOLDEN_FINAL_ROOT{"97f6e50f89a06ef8a109d94222e45c9e0f8caecef20d51a96dffcc9b104299ea"};

//! A node whose slot 1 credits alice 1e9 rUSD, and alice's client.
struct ClientRig {
    TraceRecorder trace;
    std::unique_ptr<SimNode> node;
    std::optional<SimClient> alice;

    ClientRig()
    {
        Result<std::unique_ptr<SimNode>> created{SimNode::Create(SimNodeConfig{}, trace)};
        RequireOk(created, "SimNode::Create");
        node = std::move(created).value();
        const std::vector<MarketDescriptor> markets{node->Markets().begin(), node->Markets().end()};
        Result<SimClient> client{SimClient::Create("alice", node->Profile(), node->Domain(), node->Seats(), markets)};
        RequireOk(client, "SimClient::Create");
        alice.emplace(std::move(client).value());
        DepositFact fact;
        fact.cursor = FactCursor{SIM_BASE_HEIGHT + 1, 0};
        fact.outpoint = Outpoint{SimLabelHash("fm2/test/sim/T1"), 0};
        fact.account = alice->Account();
        fact.asset = SimAsset("rUSD");
        fact.amount = 1'000'000'000;
        RequireOk(node->Chain().AppendBlock(SimBlockHash(SIM_BASE_HEIGHT + 1), {Fact{fact}}), "AppendBlock");
        RequireOk(node->SealSlot(), "SealSlot 1");
    }

    //! Submit exact bytes through the API, pump and take the admission reply.
    admission::AdmissionReply Admit(const std::vector<unsigned char>& bytes)
    {
        const Result<net::RequestId> id{node->Api().Submit(bytes)};
        RequireOk(id, "Submit");
        for (int i{0}; i < 8; ++i) {
            RequireOk(node->Pump(1), "Pump");
            if (std::optional<net::ApiReply> reply{node->Api().TakeReply(*id)}) {
                const auto* admitted{std::get_if<admission::AdmissionReply>(&reply->body)};
                BOOST_REQUIRE(admitted != nullptr);
                return *admitted;
            }
        }
        BOOST_FAIL("no reply");
        return admission::AdmissionReply{};
    }
};

//! A run of `variant` and `seed` with the given digest, headers and root (for EvaluateP6).
RunReport FakeRun(Variant variant, uint64_t seed, uint8_t digest, uint8_t header, uint8_t root)
{
    RunReport run;
    run.variant = variant;
    run.seed = seed;
    run.trace_digest = uint256{digest};
    run.headers = {uint256{header}};
    run.final_state_root = uint256{root};
    return run;
}

} // namespace

BOOST_AUTO_TEST_SUITE(sim_walking_skeleton_tests)

BOOST_AUTO_TEST_CASE(p6_fails_for_every_planted_difference)
{
    const Variant v{Variant::WALKING_SKELETON};
    std::vector<RunReport> first;
    for (uint64_t seed{1}; seed <= 8; ++seed) first.push_back(FakeRun(v, seed, static_cast<uint8_t>(seed), 7, 9));
    const std::vector<RunReport> second{first};
    const CheckResult good{EvaluateP6(first, second, true)};
    BOOST_CHECK(good.ran && good.passed);

    std::vector<RunReport> other_digest{second};
    other_digest[3].trace_digest = uint256{99};
    BOOST_CHECK(!EvaluateP6(first, other_digest, true).passed); // same seed twice, another digest
    std::vector<RunReport> other_root{second};
    other_root[2].final_state_root = uint256{1};
    BOOST_CHECK(!EvaluateP6(first, other_root, true).passed);
    std::vector<RunReport> other_headers{first};
    other_headers[5].headers = {uint256{8}};
    BOOST_CHECK(!EvaluateP6(other_headers, other_headers, true).passed); // a seed with other headers
    std::vector<RunReport> one_digest{first};
    for (RunReport& run : one_digest) run.trace_digest = uint256{5};
    BOOST_CHECK(!EvaluateP6(one_digest, one_digest, true).passed);  // eight seeds, one trace: the seed permutes nothing
    BOOST_CHECK(EvaluateP6(one_digest, one_digest, false).passed);  // not required for the fault variants
    const std::vector<RunReport> seven{one_digest.begin(), one_digest.begin() + 7};
    BOOST_CHECK(EvaluateP6(seven, seven, true).passed);              // fewer than eight seeds
    const std::vector<RunReport> shorter{second.begin(), second.begin() + 7};
    BOOST_CHECK(!EvaluateP6(first, shorter, true).passed);          // a seed ran once
    std::vector<RunReport> shifted{second};
    shifted[0].seed = 2;
    BOOST_CHECK(!EvaluateP6(first, shifted, true).passed);
    const CheckResult none{EvaluateP6({}, {}, true)};
    BOOST_CHECK(!none.ran && !none.passed);
}

BOOST_AUTO_TEST_CASE(a_client_refuses_doctored_replies)
{
    ClientRig rig;
    SimClient& alice{*rig.alice};
    const Result<read::AccountReply> state{rig.node->Api().GetState(alice.Account())};
    RequireOk(state, "GetState");
    RequireOk(alice.VerifyAccount(*state, alice.Account()), "the real reply");

    const auto refused = [&](read::AccountReply reply, std::string_view what) {
        const Result<AccountLeafV0> leaf{alice.VerifyAccount(reply, alice.Account())};
        BOOST_CHECK_MESSAGE(!leaf.has_value() && leaf.error().code == ErrorCode::VERIFY_FAILED, std::string{what} + " was accepted");
    };
    read::AccountReply doctored{*state};
    doctored.head.commit.id = uint256::ONE;
    refused(doctored, "another commit id");
    doctored = *state;
    doctored.head.commit.sequence += 1;
    refused(doctored, "another commit sequence");
    doctored = *state;
    doctored.head.scope.anchor = uint256::ONE;
    refused(doctored, "another scope anchor");
    doctored = *state;
    doctored.head.scope.epoch = 1;
    refused(doctored, "another scope epoch");
    doctored = *state;
    doctored.head.label = static_cast<read::ReadLabel>(2);
    refused(doctored, "an unregistered label");
    doctored = *state;
    doctored.head.envelope.cert.aggregate[5] ^= 1;
    refused(doctored, "a tampered aggregate");
    doctored = *state;
    doctored.head.envelope.header.anchor.height += 1; // the certificate no longer names the header
    refused(doctored, "a tampered header");
    doctored = *state;
    doctored.witness->leaf.balances[0].available += 1;
    refused(doctored, "a doctored leaf");
    doctored = *state;
    doctored.status = read::LookupStatus::UNKNOWN;
    doctored.witness.reset();
    refused(doctored, "no leaf");
    const Result<AccountLeafV0> other{alice.VerifyAccount(*state, SimClient::AccountOf("bob").value())};
    BOOST_CHECK(!other.has_value() && other.error().detail == CLIENT_WRONG_ACCOUNT);

    // A client that trusts another seat set refuses the real head.
    const uint256 ikm{SimLabelHash("fm2/test/sim/other-seat")};
    const std::optional<bls::SecretKey> key{bls::SecretKey::FromIKM(std::span<const unsigned char>{ikm.data(), ikm.size()})};
    BOOST_REQUIRE(key.has_value());
    const std::vector<SeatRegistration> other_seats{SeatRegistration{key->GetPublicKey(), key->SignPoP()}};
    const std::vector<MarketDescriptor> markets{rig.node->Markets().begin(), rig.node->Markets().end()};
    Result<SimClient> stranger{SimClient::Create("alice", rig.node->Profile(), rig.node->Domain(), SeatSet::FromPoP(other_seats).value(), markets)};
    RequireOk(stranger, "a client with another trusted seat set");
    RequireError(stranger->VerifyHead(state->head), ErrorCode::VERIFY_FAILED, "another trusted seat set");
    // A client of another domain refuses it too.
    Result<SimClient> foreign{SimClient::Create("alice", rig.node->Profile(), DomainHash(SimForeignNetwork(), FM_VERSION),
                                                rig.node->Seats(), markets)};
    RequireOk(foreign, "a client of another domain");
    const Result<void> foreign_head{foreign->VerifyHead(state->head)};
    BOOST_CHECK(!foreign_head.has_value() && foreign_head.error().detail == CLIENT_HEAD_DOMAIN);

    // An included outcome: the real one verifies; a doctored one, another id or another reference do not.
    const Result<std::vector<unsigned char>> place{alice.Place("p", 1, SimMarketM1(), Side::BUY, TimeInForce::GTC, 250, 10, 1)};
    RequireOk(place, "Place");
    BOOST_CHECK(rig.Admit(*place).code == AdmissionCode::ADMITTED);
    RequireOk(rig.node->SealSlot(), "SealSlot 2");
    const ActionV2 action{alice.Action("p").value()};
    const Result<net::ActionStatusReply> got{rig.node->Api().GetAction(action)};
    RequireOk(got, "GetAction");
    BOOST_REQUIRE(got->status == net::ApiActionStatus::INCLUDED && got->included.has_value());
    RequireOk(alice.VerifyIncluded(*got->included, action.Id()), "the real inclusion");
    read::IncludedOutcome bad{*got->included};
    bad.outcome.outcome.remaining_lots += 1;
    RequireError(alice.VerifyIncluded(bad, action.Id()), ErrorCode::VERIFY_FAILED, "a doctored outcome");
    const Result<OutcomeV2> wrong_id{alice.VerifyIncluded(*got->included, ActionId{uint256::ONE})};
    BOOST_CHECK(!wrong_id.has_value() && wrong_id.error().detail == CLIENT_OUTCOME_SUBJECT);
    bad = *got->included;
    bad.ref.height += 1;
    const Result<OutcomeV2> wrong_ref{alice.VerifyIncluded(bad, action.Id())};
    BOOST_CHECK(!wrong_ref.has_value() && wrong_ref.error().detail == CLIENT_REF_MISMATCH);

    // An EXPIRED claim: refused when the status is not EXPIRED, and when the proof does not prove it.
    const Result<void> not_expired{alice.VerifyExpiredReply("p", *got)};
    BOOST_CHECK(!not_expired.has_value() && not_expired.error().detail == CLIENT_NOT_EXPIRED);
    const Result<read::AccountReply> now{rig.node->Api().GetState(alice.Account())};
    RequireOk(now, "GetState at 2");
    net::ActionStatusReply claim;
    claim.status = net::ApiActionStatus::EXPIRED;
    claim.head = now->head;
    claim.expiry = now->witness;
    RequireError(alice.VerifyExpiredReply("p", claim), ErrorCode::VERIFY_FAILED, "an EXPIRED claim for an included action");
    // The fill check refuses a market the client does not trust.
    OutcomeV2 foreign_fill;
    foreign_fill.fills.push_back(Fill{});
    const Result<void> fills{alice.VerifyFills(foreign_fill)};
    BOOST_CHECK(!fills.has_value() && fills.error().detail == CLIENT_UNKNOWN_MARKET);
}

BOOST_AUTO_TEST_CASE(manifests_cover_every_check)
{
    BOOST_CHECK_EQUAL(AllChecks().size(), 21U);
    std::multiset<std::string_view> covered;
    for (const Variant variant : AllVariants()) {
        for (const std::string_view id : ManifestOf(variant)) covered.insert(id);
        BOOST_CHECK(VariantFromName(ToString(variant)) == variant);
    }
    for (const std::string_view id : SuiteChecks()) covered.insert(id);
    const std::set<std::string_view> all{AllChecks().begin(), AllChecks().end()};
    BOOST_CHECK_EQUAL(all.size(), 21U);
    // Every check is covered; the variant-specific ones exactly once, the shared ones by each variant.
    for (const std::string_view id : all) BOOST_CHECK_MESSAGE(covered.contains(id), std::string{id} + " is in no manifest");
    for (const std::string_view id : covered) BOOST_CHECK_MESSAGE(all.contains(id), std::string{id} + " is not a check");
    BOOST_CHECK_EQUAL(covered.count("N10"), 1U);
    BOOST_CHECK_EQUAL(covered.count("N11"), 1U);
    BOOST_CHECK_EQUAL(covered.count("P6"), 1U);
    BOOST_CHECK(!VariantFromName("walking").has_value());
    BOOST_CHECK_EQUAL(ToString(Variant::STORAGE_FAULT), "walking_skeleton_storage_fault");
}

BOOST_AUTO_TEST_CASE(a_missing_or_failed_check_fails_the_run)
{
    RunReport report;
    report.variant = Variant::AUTHORITY_FAULT;
    for (const std::string_view id : ManifestOf(report.variant)) report.checks[std::string{id}] = CheckResult{true, true, ""};
    BOOST_CHECK(report.Passed());
    RunReport missing{report};
    missing.checks.erase("N11");
    BOOST_CHECK(!missing.Passed());
    RunReport not_run{report};
    not_run.checks["P3"] = CheckResult{false, false, "not run"};
    BOOST_CHECK(!not_run.Passed());
    RunReport failed_extra{report};
    failed_extra.checks["P1"] = CheckResult{true, false, "planted"};
    BOOST_CHECK(!failed_extra.Passed());
    RunReport harness{report};
    harness.harness_error = "planted";
    BOOST_CHECK(!harness.Passed());
}

BOOST_AUTO_TEST_CASE(walking_skeleton_seed_1_passes_p1_to_p8_and_n1_to_n13)
{
    const RunReport report{RunWalkingSkeleton(1, Variant::WALKING_SKELETON)};
    RequirePassed(report);
    BOOST_CHECK(report.harness_error.empty());
    BOOST_REQUIRE_EQUAL(report.headers.size(), 5U);
    BOOST_CHECK_EQUAL(report.canonical_order.size(), 5U);
    BOOST_CHECK_EQUAL(report.headers[4].GetHex(), GOLDEN_SLOT5_HEADER);
    BOOST_CHECK_EQUAL(report.final_state_root.GetHex(), GOLDEN_FINAL_ROOT);
    BOOST_CHECK_GT(report.trace_events, 0U);
}

BOOST_AUTO_TEST_CASE(seeds_1_to_32_give_identical_headers_and_final_root)
{
    const RunReport first{RunWalkingSkeleton(1, Variant::WALKING_SKELETON)};
    std::set<uint256> digests{first.trace_digest};
    for (uint64_t seed{2}; seed <= 32; ++seed) {
        const RunReport report{RunWalkingSkeleton(seed, Variant::WALKING_SKELETON)};
        RequirePassed(report);
        BOOST_CHECK_MESSAGE(report.headers == first.headers, "seed " + std::to_string(seed) + " gives other header hashes");
        BOOST_CHECK_MESSAGE(report.final_state_root == first.final_state_root, "seed " + std::to_string(seed) + " gives another root");
        BOOST_CHECK(report.canonical_order == first.canonical_order);
        digests.insert(report.trace_digest);
    }
    // The seed does permute the admission order: the traces are not all one.
    BOOST_CHECK_GT(digests.size(), 1U);
}

BOOST_AUTO_TEST_CASE(same_seed_twice_gives_the_same_trace_digest)
{
    for (const uint64_t seed : {1U, 7U}) {
        for (const Variant variant : AllVariants()) {
            const RunReport a{RunWalkingSkeleton(seed, variant)};
            const RunReport b{RunWalkingSkeleton(seed, variant)};
            BOOST_CHECK(a.trace_digest == b.trace_digest);
            BOOST_CHECK_EQUAL(a.trace_events, b.trace_events);
            BOOST_CHECK(a.checks == b.checks);
            BOOST_CHECK(a.headers == b.headers);
        }
    }
}

BOOST_AUTO_TEST_CASE(storage_fault_variant_passes)
{
    for (uint64_t seed{1}; seed <= 4; ++seed) {
        const RunReport report{RunWalkingSkeleton(seed, Variant::STORAGE_FAULT)};
        RequirePassed(report);
        BOOST_CHECK_EQUAL(report.headers.size(), 2U); // slots 1 and 2 committed; 3 failed at B-DECISION
    }
}

BOOST_AUTO_TEST_CASE(authority_fault_variant_passes)
{
    for (uint64_t seed{1}; seed <= 4; ++seed) {
        const RunReport report{RunWalkingSkeleton(seed, Variant::AUTHORITY_FAULT)};
        RequirePassed(report);
        BOOST_CHECK_EQUAL(report.headers.size(), 2U);
    }
}

BOOST_AUTO_TEST_CASE(fault_variants_share_the_committed_prefix)
{
    // Slots 1 and 2 hold the same inputs in every variant (only w is withheld, and only in slot 4).
    const RunReport base{RunWalkingSkeleton(3, Variant::WALKING_SKELETON)};
    const RunReport storage{RunWalkingSkeleton(3, Variant::STORAGE_FAULT)};
    const RunReport authority{RunWalkingSkeleton(3, Variant::AUTHORITY_FAULT)};
    BOOST_REQUIRE_GE(base.headers.size(), 2U);
    BOOST_REQUIRE_EQUAL(storage.headers.size(), 2U);
    BOOST_REQUIRE_EQUAL(authority.headers.size(), 2U);
    BOOST_CHECK(storage.headers[0] == base.headers[0] && storage.headers[1] == base.headers[1]);
    BOOST_CHECK(authority.headers == storage.headers);
}

BOOST_AUTO_TEST_CASE(suite_json_is_deterministic_sorted_and_complete)
{
    const SuiteReport a{RunSuite(AllVariants(), 1, 2)};
    const SuiteReport b{RunSuite(AllVariants(), 1, 2)};
    BOOST_CHECK(a.ok);
    BOOST_CHECK_EQUAL(a.runs.size(), 6U);
    BOOST_CHECK_EQUAL(a.manifest.size(), 21U);
    for (const auto& [id, verdict] : a.manifest) BOOST_CHECK_MESSAGE(verdict == "pass", id + " is " + verdict);
    const std::string json{SuiteJson(a)};
    BOOST_CHECK_EQUAL(json, SuiteJson(b));
    BOOST_CHECK(json.ends_with("}\n"));
    // Top-level keys in sorted order, and no path of this checkout.
    const std::vector<std::string_view> keys{"\"distinct_trace_digests\"", "\"format\"", "\"label\"", "\"manifest\"", "\"ok\"",
                                             "\"runs\"", "\"seeds\"", "\"suite\"", "\"variants\""};
    size_t last{0};
    for (const std::string_view key : keys) {
        const size_t at{json.find(std::string{"\n  "} + std::string{key})};
        BOOST_REQUIRE_MESSAGE(at != std::string::npos, std::string{key} + " missing");
        BOOST_CHECK_GT(at, last);
        last = at;
    }
    BOOST_CHECK(json.find(FM2_SOURCE_ROOT) == std::string::npos);
    BOOST_CHECK(json.find("0x") == std::string::npos);

    // A suite that runs nothing, or misses the fault variants, is not ok.
    BOOST_CHECK(!RunSuite({}, 1, 2).ok);
    BOOST_CHECK(!RunSuite(AllVariants(), 3, 2).ok);
    const std::vector<Variant> base_only{Variant::WALKING_SKELETON};
    const SuiteReport partial{RunSuite(base_only, 1, 1)};
    BOOST_CHECK(partial.ok); // its own manifest: base checks and P6
    BOOST_CHECK(!partial.manifest.contains("N10"));
}

BOOST_AUTO_TEST_CASE(cluster_and_node_refusals)
{
    TraceRecorder trace;
    RequireError(SimCluster::Create(0, SimNodeConfig{}, trace), ErrorCode::INVALID_INPUT, "an empty cluster");
    RequireStub(SimCluster::Create(4, SimNodeConfig{}, trace), "M4-SIM-01");
    RequireStub(SimCluster::CreateTwins(4, 1, SimNodeConfig{}, trace), "M4-SIM-03");
    BOOST_CHECK_EQUAL(trace.Size(), 0U); // the stubs build nothing
    Result<std::unique_ptr<SimCluster>> one{SimCluster::Create(1, SimNodeConfig{}, trace)};
    RequireOk(one, "a one-node cluster");
    BOOST_CHECK_EQUAL((*one)->Size(), 1U);

    SimNodeConfig crash;
    crash.faults.crash_before_slot = 2;
    RequireStub(SimNode::Create(crash, trace), "M4-SIM-02");
    SimNodeConfig zero;
    zero.faults.fail_commit_barrier_at = 0;
    RequireError(SimNode::Create(zero, trace), ErrorCode::INVALID_INPUT, "fault at slot 0");
}

BOOST_AUTO_TEST_CASE(a_node_seals_through_the_real_components)
{
    TraceRecorder trace;
    Result<std::unique_ptr<SimNode>> created{SimNode::Create(SimNodeConfig{}, trace)};
    RequireOk(created, "SimNode::Create");
    SimNode& node{**created};
    BOOST_CHECK_EQUAL(node.Seats().Size(), 1U);
    BOOST_CHECK(node.Network().chain == ChainType::REGTEST);
    BOOST_CHECK(node.Domain() == DomainHash(SimNetwork(), FM_VERSION));
    // Before any block the adapter has no data at the first cursor's end: an empty heartbeat batch at the base.
    RequireOk(node.Chain().AppendBlock(SimBlockHash(SIM_BASE_HEIGHT + 1), {}), "AppendBlock");
    const Result<SealReport> first{node.SealSlot()};
    RequireOk(first, "SealSlot 1");
    BOOST_CHECK_EQUAL(first->height, 1U);
    BOOST_CHECK_EQUAL(first->originals, 0U);
    BOOST_CHECK(node.Read().PublishedHeight() == std::optional<uint64_t>{1});
    BOOST_CHECK_EQUAL(node.Pool().HeadHeight(), 1U);
    BOOST_CHECK_EQUAL(node.SignerJournal().Status().durable_seq, 3U); // ACCEPTED_PROPOSAL, INTENT, SIGNED_VOTE
    BOOST_CHECK_EQUAL(node.CommitJournal().Status().durable_seq, 2U); // DECISION, APPLIED
    BOOST_CHECK_EQUAL(trace.BarrierCount(storage::JournalRole::COMMIT), 2U);
    // The second slot re-polls the end of block 130: a heartbeat.
    RequireOk(node.SealSlot(), "SealSlot 2");
    BOOST_CHECK_EQUAL(node.Sealed().size(), 2U);
    RequireError(node.ReapplySealed(1), ErrorCode::STALE_PARENT, "re-apply slot 1");
    RequireError(node.ReapplySealed(9), ErrorCode::INVALID_INPUT, "re-apply an unsealed slot");
    // BUSY from the chain is returned as such and seals nothing.
    node.Chain().QueueBusy(1);
    RequireError(node.SealSlot(), ErrorCode::BUSY, "SealSlot while the chain is busy");
    BOOST_CHECK_EQUAL(node.Sealed().size(), 2U);
    BOOST_CHECK(!node.Fenced());
}

BOOST_AUTO_TEST_CASE(a_client_signs_each_label_once)
{
    TraceRecorder trace;
    Result<std::unique_ptr<SimNode>> node{SimNode::Create(SimNodeConfig{}, trace)};
    RequireOk(node, "SimNode::Create");
    const std::vector<MarketDescriptor> markets{(*node)->Markets().begin(), (*node)->Markets().end()};
    Result<SimClient> created{SimClient::Create("alice", (*node)->Profile(), (*node)->Domain(), (*node)->Seats(), markets)};
    RequireOk(created, "SimClient::Create");
    SimClient& alice{*created};
    BOOST_CHECK(SimClient::AccountOf("alice").value() == alice.Account());
    BOOST_CHECK(alice.Account() != SimClient::AccountOf("bob").value());
    const Result<std::vector<unsigned char>> bytes{alice.Place("x", 1, SimMarketM1(), Side::BUY, TimeInForce::GTC, 250, 1, 1)};
    RequireOk(bytes, "Place");
    // Never re-signed: the label is taken, and the stored bytes stay the first ones.
    RequireError(alice.Place("x", 1, SimMarketM1(), Side::BUY, TimeInForce::GTC, 250, 1, 1), ErrorCode::INVALID_INPUT, "second x");
    RequireError(alice.Place("x", 2, SimMarketM1(), Side::BUY, TimeInForce::GTC, 999, 1, 1), ErrorCode::INVALID_INPUT, "other x");
    BOOST_CHECK(alice.Bytes("x").value() == *bytes);
    BOOST_CHECK_EQUAL(alice.OutboxSize(), 1U);
    const ActionV2 action{alice.Action("x").value()};
    BOOST_CHECK(action.VerifySignature());
    BOOST_CHECK(action.Account() == alice.Account());
    // Same name and label give the same bytes on another client object (aux is a label).
    Result<SimClient> again{SimClient::Create("alice", (*node)->Profile(), (*node)->Domain(), (*node)->Seats(), markets)};
    RequireOk(again, "SimClient::Create again");
    BOOST_CHECK(again->Place("x", 1, SimMarketM1(), Side::BUY, TimeInForce::GTC, 250, 1, 1).value() == *bytes);
    // The tampered copy differs in signature byte 0 only and does not verify; it is not stored.
    const std::vector<unsigned char> tampered{alice.TamperedSignature("x").value()};
    BOOST_REQUIRE_EQUAL(tampered.size(), bytes->size());
    size_t differing{0};
    for (size_t i{0}; i < tampered.size(); ++i) differing += tampered[i] != (*bytes)[i] ? 1 : 0;
    BOOST_CHECK_EQUAL(differing, 1U);
    BOOST_CHECK(!ActionV2::Decode(tampered, (*node)->Profile()).value().VerifySignature());
    BOOST_CHECK_EQUAL(alice.OutboxSize(), 1U);
    RequireError(alice.Bytes("nope"), ErrorCode::INVALID_INPUT, "unknown label");
}

BOOST_AUTO_TEST_SUITE_END()
