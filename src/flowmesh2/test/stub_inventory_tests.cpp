// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** Stub inventory: one Boost case per outline stub, plus the self-check.
 *
 * Every typed stub in non-test fm2 code (`return NotImplemented("<ID>");` or
 * `return Unsupported("<ID>");`, tagged `// TODO(rebirth:<ID>)`) gets exactly
 * one case here, written as
 *
 *     FM2_STUB_CASE(M2_LED_05, "M2-LED-05")
 *     {
 *         LedgerState ledger{...};
 *         return Observe(ledger.RequestWithdrawal(...));
 *     }
 *
 * The case name is the task ID with '-' replaced by '_'. The case passes only
 * if the probe returns an error whose code is NOT_IMPLEMENTED or UNSUPPORTED
 * and whose detail is exactly the task ID: a stub can never fake success.
 * When Codex finishes a task it deletes the stub, its tag, its case here and
 * its line in INVENTORY.txt in one commit (TODO.md rule 3).
 *
 * Certified typed refusals (an OutcomeCode such as
 * REJ_RESIDUAL_CROSS_UNSUPPORTED) are covered by the slice that adds them.
 *
 * Slice R1 added the registry and its self-check. Slice R3 adds the four
 * stubs of core/params Validate(): the price band (M3-ENG-03), PER_SIDE fees
 * (M3-ENG-08), futures (M9-FUT-01) and folded signing (M4-STO-05). Slice R4
 * adds batch BIP340 verification (M4-ADM-01) and the five fact kinds that
 * later tasks define (M7-SEAT-01, M7-GOV-01, MB1-B3D-02, M8-BRG-02,
 * M8-EXIT-01); each fact probe checks EncodeFact, FactId and DecodeFact.
 * Slice R5 adds the ledger's withdrawal (M2-LED-05) and spot/futures
 * transfers (M2-LED-06; the probe checks both directions), and commitment
 * v1 (M2-LED-02), the liability sum root (M2-LED-03) and exit_value
 * (M2-LED-04). Slice R6 adds the residual-cross call (M3-ENG-02) and
 * extends M3-ENG-03 to probe the engine's PriceBandCheck as well. Slice R7
 * adds the engine's CANCEL_ALL branch (M3-ENG-09) and futures phase 7
 * (M9-FUT-04), and extends M2-LED-05 and M2-LED-06 to probe the engine
 * branches of WITHDRAW and the spot/futures transfers (engine PhaseOf) as
 * well. Slice R8 adds the file journal's reopen and read-back and the logs'
 * recovery over a journal with history (M4-STO-01), segment rollover
 * (M4-STO-06; the probe also appends past the end of a one-page segment),
 * the group-commit writer (M4-STO-02), snapshots (M4-STO-03) and the view
 * state records (M4-STO-07), and extends M4-STO-05 to probe the signer
 * log's folded signing as well. Slice R9 adds the state owner's pipelined
 * Preview over an uncommitted candidate (M4-AGR-10), and extends M4-STO-03
 * to probe StateOwner::Restore and the replay of a SNAPSHOT_MARK record and
 * M4-STO-07 to probe the replay of a PREPARED_QC record as well. Slice
 * R10 adds the b3d chain adapters: trusted facts on REGTEST (MB1-ADP-06)
 * and the finalized adapter (M7-ADP-01); PBFT safety (M4-AGR-01; the probe
 * checks MayPrepare, MayCommit and OnViewChange), the PBFT agreement
 * (M4-AGR-02; the probe also checks the dev certifier's OnMessage, which
 * returns Unsupported), the leader schedule (M4-AGR-03), inclusion lists (M4-AGR-04;
 * Build and Check), the restart fence (M4-AGR-06) and the evidence store
 * (M4-AGR-07; Record and Load). Slice R11 adds the three b3verify stubs:
 * liability paths (M5-VFY-03), checkpoints (M8-CKP-01) and anchor proofs
 * (M7-ADP-02). Slice R12 adds the credit budget (M4-ADM-02), and extends
 * M4-ADM-01 to probe admission's batch path (Admission::AdmitBatch) and
 * M2-LED-05, M2-LED-06 and M3-ENG-09 to probe admission's kind rule
 * (KindAdmissible, whose Unsupported refusals become UNSUPPORTED_KIND).
 * Slice R13 adds the client API's HTTPS + SSE transport (M5-API-01) and
 * streams (M5-API-02, InProcessApi::Subscribe); the in-process (M4-SIM-01)
 * and TCP (M4-NET-01) seat meshes, short IDs (M4-NET-02; Encode and
 * Decode) and sentry fan-out (M4-NET-03); the bridge's mint (M8-BRG-01)
 * and release limits (M8-BRG-03); the futures oracle (M9-FUT-02) and
 * margin (M9-FUT-03); removals (M7-GOV-02) and versioned parameters
 * (M7-GOV-03). It extends M8-BRG-02 to probe BuildWithdrawalPacket,
 * M8-CKP-01 CheckpointBuilder::Build, M8-EXIT-01 ForcedExit::Open and
 * Redeem, M9-FUT-01 FuturesDomain::PlaceFuture, M9-FUT-04
 * FuturesDomain::Sweep and M7-GOV-01 Governance::ApplyListing.
 */

#include <boost/test/unit_test.hpp>

#include <flowmesh2/admission/admission.h>
#include <flowmesh2/admission/budget.h>
#include <flowmesh2/agreement/evidence.h>
#include <flowmesh2/agreement/inclusion.h>
#include <flowmesh2/agreement/leader.h>
#include <flowmesh2/agreement/pbft.h>
#include <flowmesh2/agreement/restart_fence.h>
#include <flowmesh2/bridge/bridge_manager.h>
#include <flowmesh2/bridge/checkpoint.h>
#include <flowmesh2/bridge/forced_exit.h>
#include <flowmesh2/chain/b3d_adapter.h>
#include <flowmesh2/core/action.h>
#include <flowmesh2/core/facts.h>
#include <flowmesh2/core/market.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/dev/dev_certifier.h>
#include <flowmesh2/engine/book.h>
#include <flowmesh2/engine/engine.h>
#include <flowmesh2/engine/matcher.h>
#include <flowmesh2/engine/residual.h>
#include <flowmesh2/futures/futures.h>
#include <flowmesh2/governance/governance.h>
#include <flowmesh2/ledger/ledger.h>
#include <flowmesh2/ledger/state_commitment.h>
#include <flowmesh2/net/client_api.h>
#include <flowmesh2/net/https_sse_api.h>
#include <flowmesh2/net/in_process_api.h>
#include <flowmesh2/net/in_process_mesh.h>
#include <flowmesh2/net/sentry.h>
#include <flowmesh2/net/short_id.h>
#include <flowmesh2/net/tcp_mesh.h>
#include <flowmesh2/sim/cluster.h>
#include <flowmesh2/sim/faults.h>
#include <flowmesh2/sim/trace.h>
#include <flowmesh2/state/candidate.h>
#include <flowmesh2/state/replay.h>
#include <flowmesh2/state/state_owner.h>
#include <flowmesh2/storage/commit_log.h>
#include <flowmesh2/storage/file_journal.h>
#include <flowmesh2/storage/log_writer.h>
#include <flowmesh2/storage/memory_journal.h>
#include <flowmesh2/storage/signer_log.h>
#include <flowmesh2/storage/snapshot_store.h>
#include <flowmesh2/test/util/admission_fixtures.h>
#include <flowmesh2/test/util/agreement_fixtures.h>
#include <flowmesh2/test/util/api_fixtures.h>
#include <flowmesh2/test/util/mesh_fixtures.h>
#include <flowmesh2/test/util/storage_fixtures.h>
#include <flowmesh2/test/util/verify_fixtures.h>
#include <flowmesh2/verify/b3verify.h>

#include <exception>
#include <functional>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace flowmesh2::test {

//! What one stub probe returned.
struct StubObservation {
    bool succeeded{false};
    std::optional<Error> error;
};

template <class T>
StubObservation Observe(const Result<T>& result)
{
    if (result.has_value()) return StubObservation{true, std::nullopt};
    return StubObservation{false, result.error()};
}
inline StubObservation Observe(const Error& error) { return StubObservation{false, error}; }

struct StubEntry {
    std::string_view task_id;
    std::string_view case_name;
    std::function<StubObservation()> probe;
};

std::vector<StubEntry>& StubRegistry()
{
    static std::vector<StubEntry> registry;
    return registry;
}

struct StubRegistrar {
    //! TaskId is consteval: a malformed ID literal does not compile.
    StubRegistrar(TaskId task, std::string_view case_name, std::function<StubObservation()> probe)
    {
        StubRegistry().push_back(StubEntry{task.View(), case_name, std::move(probe)});
    }
};

std::string ExpectedCaseName(std::string_view task_id)
{
    std::string name{task_id};
    for (char& c : name) {
        if (c == '-') c = '_';
    }
    return name;
}

//! Every problem of every entry; empty means the inventory is consistent.
std::vector<std::string> InventoryProblems(std::span<const StubEntry> entries)
{
    std::vector<std::string> problems;
    std::set<std::string_view> seen;
    for (const auto& entry : entries) {
        const std::string id{entry.task_id};
        if (!IsWellFormedTaskId(entry.task_id)) problems.push_back(id + ": malformed task id");
        if (!seen.insert(entry.task_id).second) problems.push_back(id + ": duplicate task id");
        if (entry.case_name != ExpectedCaseName(entry.task_id)) {
            problems.push_back(id + ": case name " + std::string{entry.case_name} + " should be " + ExpectedCaseName(entry.task_id));
        }
        StubObservation observed;
        try {
            observed = entry.probe();
        } catch (const std::exception& e) {
            problems.push_back(id + ": probe threw " + e.what());
            continue;
        } catch (...) {
            problems.push_back(id + ": probe threw a non-standard exception");
            continue;
        }
        if (observed.succeeded || !observed.error.has_value()) {
            problems.push_back(id + ": stub returned success");
            continue;
        }
        const Error& error{*observed.error};
        if (error.code != ErrorCode::NOT_IMPLEMENTED && error.code != ErrorCode::UNSUPPORTED) {
            problems.push_back(id + ": code " + std::string{ToString(error.code)} + " is not NOT_IMPLEMENTED or UNSUPPORTED");
        }
        if (error.detail != entry.task_id) {
            problems.push_back(id + ": detail '" + std::string{error.detail} + "' is not the task id");
        }
    }
    return problems;
}

std::string Join(const std::vector<std::string>& lines)
{
    std::string out;
    for (const auto& line : lines) out += line + "\n";
    return out;
}

void CheckOneStub(std::string_view case_name)
{
    std::vector<StubEntry> matching;
    for (const auto& entry : StubRegistry()) {
        if (entry.case_name == case_name) matching.push_back(entry);
    }
    BOOST_REQUIRE_EQUAL(matching.size(), 1U);
    const auto problems{InventoryProblems(matching)};
    BOOST_CHECK_MESSAGE(problems.empty(), Join(problems));
}

} // namespace flowmesh2::test

//! Defines the probe, registers it and defines the Boost case; the function
//! body that follows the macro is the probe. See the file comment.
#define FM2_STUB_CASE(case_ident, task_literal)                                                       \
    static flowmesh2::test::StubObservation case_ident##_probe();                                     \
    static const flowmesh2::test::StubRegistrar case_ident##_registrar{flowmesh2::TaskId{task_literal}, #case_ident,     \
                                                                       &case_ident##_probe};          \
    BOOST_AUTO_TEST_CASE(case_ident) { flowmesh2::test::CheckOneStub(#case_ident); }                  \
    static flowmesh2::test::StubObservation case_ident##_probe()

using namespace flowmesh2;
using namespace flowmesh2::test;

namespace {

// Self-check fixtures. They are NOT stubs: they call NotImplemented() with real
// ledger IDs only to prove that the checker catches each kind of bad entry.
StubObservation GoodProbe() { return Observe(Result<int>{NotImplemented("R1")}); }
StubObservation UnsupportedProbe() { return Observe(Unsupported("R2")); }
StubObservation FakeSuccessProbe() { return Observe(Result<int>{Ok(1)}); }
StubObservation FakeVoidSuccessProbe() { return Observe(Result<void>{Ok()}); }
StubObservation WrongIdProbe() { return Observe(NotImplemented("R2")); }
StubObservation WrongCodeProbe() { return Observe(Error{ErrorCode::DECODE, "R1"}); }
StubObservation ThrowingProbe() { throw std::runtime_error{"probe exploded"}; }

size_t ProblemCount(std::vector<StubEntry> entries) { return InventoryProblems(entries).size(); }

// RegtestProfile() (the provisional regtest profile, which Validate()
// accepts unchanged) is test/util/ledger_fixtures.h's since slice R10.

//! The first observation that is not a NOT_IMPLEMENTED/UNSUPPORTED error
//! carrying `task_id`, or the first one if all are; so a probe of several
//! entry points fails if any of them deviates.
StubObservation FirstDeviation(std::string_view task_id, std::vector<StubObservation> observations)
{
    for (const auto& o : observations) {
        const bool stub_error{!o.succeeded && o.error.has_value() && o.error->detail == task_id &&
                              (o.error->code == ErrorCode::NOT_IMPLEMENTED || o.error->code == ErrorCode::UNSUPPORTED)};
        if (!stub_error) return o;
    }
    return observations.front();
}

//! An empty ledger of the provisional regtest profile.
ledger::LedgerState EmptyRegtestLedger() { return ledger::LedgerState::Create(RegtestProfile()).value(); }

//! An unsigned action of `kind` with an empty payload (PhaseOf reads only the kind).
ActionV2 ActionOfKind(ActionKind kind)
{
    ActionV2 action;
    action.kind = kind;
    return action;
}

//! A state owner on a genesis without markets, and heartbeat inputs that follow its head.
struct HeartbeatOwner {
    std::unique_ptr<state::StateOwner> owner;
    state::SlotInputs inputs;
};

HeartbeatOwner MakeHeartbeatOwner()
{
    const ProfileParams profile{RegtestProfile()};
    const AuthorityVersion authority{0, LabelHash("fm2/test/stub/seats"), ProfileHash(profile)};
    const state::HeadView genesis{state::GenesisHead(profile, {}, LabelHash("fm2/test/stub/domain"), authority,
                                                     Anchor{1, LabelHash("fm2/test/stub/anchor")}, FactCursor{2, 0})
                                      .value()};
    HeartbeatOwner heartbeat{state::StateOwner::Create(profile, genesis, 1).value(), {}};
    heartbeat.inputs.authority = authority;
    heartbeat.inputs.anchor = genesis.Header().anchor;
    heartbeat.inputs.facts_from = genesis.Header().facts_to;
    heartbeat.inputs.facts_to = genesis.Header().facts_to;
    return heartbeat;
}

//! EncodeFact, FactId and DecodeFact (of the kind byte) of a stub fact kind.
StubObservation FactStubProbe(const Fact& fact, std::string_view task_id)
{
    const std::vector<unsigned char> kind_only{static_cast<unsigned char>(KindOf(fact))};
    return FirstDeviation(task_id, {Observe(EncodeFact(fact)), Observe(FactId(fact)), Observe(DecodeFact(kind_only))});
}

} // namespace

BOOST_AUTO_TEST_SUITE(stub_inventory)

BOOST_AUTO_TEST_CASE(registry_selfcheck)
{
    const auto problems{InventoryProblems(StubRegistry())};
    BOOST_CHECK_MESSAGE(problems.empty(), Join(problems));
    BOOST_TEST_MESSAGE("stub inventory entries: " << StubRegistry().size());
}

BOOST_AUTO_TEST_CASE(checker_detects_bad_entries)
{
    BOOST_CHECK_EQUAL(ProblemCount({{"R1", "R1", GoodProbe}}), 0U);
    BOOST_CHECK_EQUAL(ProblemCount({{"R2", "R2", UnsupportedProbe}}), 0U);
    BOOST_CHECK_EQUAL(ProblemCount({{"R1", "R1", GoodProbe}, {"R2", "R2", UnsupportedProbe}}), 0U);

    BOOST_CHECK_EQUAL(ProblemCount({{"R1", "R1", FakeSuccessProbe}}), 1U);
    BOOST_CHECK_EQUAL(ProblemCount({{"R1", "R1", FakeVoidSuccessProbe}}), 1U);
    BOOST_CHECK_EQUAL(ProblemCount({{"R1", "R1", WrongIdProbe}}), 1U);
    BOOST_CHECK_EQUAL(ProblemCount({{"R1", "R1", WrongCodeProbe}}), 1U);
    BOOST_CHECK_EQUAL(ProblemCount({{"R1", "R1", ThrowingProbe}}), 1U);
    BOOST_CHECK_EQUAL(ProblemCount({{"R1", "R1", GoodProbe}, {"R1", "R1", GoodProbe}}), 1U);
    BOOST_CHECK_EQUAL(ProblemCount({{"R1", "R_1", GoodProbe}}), 1U);
    // A malformed id also mismatches the detail the probe returns.
    BOOST_CHECK_EQUAL(ProblemCount({{"r1", "r1", GoodProbe}}), 2U);
}

// ---- Slice R3: core/params Validate() ----

FM2_STUB_CASE(M3_ENG_03, "M3-ENG-03")
{
    // Two entry points: the profile switch (R3) and the engine's band check (R6).
    ProfileParams params{RegtestProfile()};
    params.price_band_enabled = true;
    const MarketDescriptor market{AssetId::Native(), AssetId{uint256::ONE}, 1, 1};
    return FirstDeviation("M3-ENG-03", {Observe(Validate(params)),
                                        Observe(engine::PriceBandCheck(market, 100, engine::ReferencePrice{market.Id(), 100}))});
}

FM2_STUB_CASE(M3_ENG_08, "M3-ENG-08")
{
    ProfileParams params{RegtestProfile()};
    params.fee_incidence = FeeIncidence::PER_SIDE;
    return Observe(Validate(params));
}

FM2_STUB_CASE(M9_FUT_01, "M9-FUT-01")
{
    // Two entry points: the profile switch (R3) and the futures domain (R13).
    ProfileParams params{RegtestProfile()};
    params.futures_enabled = true;
    futures::FuturesDomain domain{RegtestProfile()};
    return FirstDeviation("M9-FUT-01", {Observe(Validate(params)), Observe(domain.PlaceFuture(futures::FutureOrder{}))});
}

FM2_STUB_CASE(M4_STO_05, "M4-STO-05")
{
    // Two entry points: the profile switch (R3) and the signer log (R8).
    ProfileParams params{RegtestProfile()};
    params.signer_mode = SignerMode::FOLDED_SIGNING;
    auto journal{NewMemoryJournal(storage::JournalRole::SIGNER, 1)};
    storage::SignerLog log{storage::SignerLog::Create(*journal).value()};
    const StorageSlot slot{MakeStorageSlot(1, 0, "stub")};
    const storage::PendingProposal proposal{log.AppendAcceptedProposal(storage::ProposalKey{0, 1, 0}, slot.header, slot.body).value()};
    return FirstDeviation("M4-STO-05", {Observe(Validate(params)),
                                        Observe(log.AppendSignedFolded(CommitVoteKey(slot.header, 0), slot.header.CommitDigest(),
                                                                       proposal, storage::VoteSignature{}))});
}

// ---- Slice R4: core/action BatchVerifier ----

FM2_STUB_CASE(M4_ADM_01, "M4-ADM-01")
{
    // Two entry points: core BatchVerifier (R4) and admission's batch path (R12).
    AdmissionRig rig;
    return FirstDeviation("M4-ADM-01", {Observe(BatchVerifier{}.VerifyBatch({})), Observe(rig.admission->AdmitBatch({}))});
}

// ---- Slice R12: admission/budget ----

FM2_STUB_CASE(M4_ADM_02, "M4-ADM-02")
{
    return Observe(admission::CreditBudget::Create(admission::CreditBudgetParams{}, RegtestProfile()));
}

// ---- Slice R4: core/facts stub kinds ----

FM2_STUB_CASE(M7_SEAT_01, "M7-SEAT-01")
{
    return FactStubProbe(SeatChangeFact{}, "M7-SEAT-01");
}

FM2_STUB_CASE(M7_GOV_01, "M7-GOV-01")
{
    // The listing fact's codec (R4) and governance's listing (R13).
    governance::Governance governance{RegtestProfile()};
    return FirstDeviation("M7-GOV-01", {FactStubProbe(ListingFact{}, "M7-GOV-01"), Observe(governance.ApplyListing(ListingFact{}))});
}

FM2_STUB_CASE(MB1_B3D_02, "MB1-B3D-02")
{
    return FactStubProbe(CheckpointAcceptedFact{}, "MB1-B3D-02");
}

FM2_STUB_CASE(M8_BRG_02, "M8-BRG-02")
{
    // The payout fact's codec (R4) and the bridge's withdrawal packet (R13).
    bridge::BridgeManagerV2 manager;
    // The payout fact codec is real; production packet export is still gated.
    return Observe(manager.BuildWithdrawalPacket(bridge::WithdrawalRequest{}));
}

FM2_STUB_CASE(M8_EXIT_01, "M8-EXIT-01")
{
    // The exit nullifier fact's codec (R4), and ForcedExit Open and Redeem (R13).
    return FirstDeviation("M8-EXIT-01", {FactStubProbe(ExitNullifierFact{}, "M8-EXIT-01"),
                                         Observe(bridge::ForcedExit::Open(0, bridge::FORCED_EXIT_DELAY_BLOCKS + 1)),
                                         Observe(bridge::ForcedExit::Redeem(bridge::ExitClaim{}, bridge::FORCED_EXIT_DELAY_BLOCKS + 1))});
}

// ---- Slice R5: ledger/ledger LedgerState ----

FM2_STUB_CASE(M2_LED_05, "M2-LED-05")
{
    // Three entry points: the ledger (R5), the engine's WITHDRAW branch (R7) and admission's kind rule (R12).
    ledger::LedgerState ledger{EmptyRegtestLedger()};
    return FirstDeviation("M2-LED-05", {Observe(ledger.RequestWithdrawal(AccountId{}, WithdrawPayload{AssetId::Native(), 1, {0x51}})),
                                        Observe(engine::PhaseOf(ActionOfKind(ActionKind::WITHDRAW))),
                                        Observe(admission::KindAdmissible(ActionKind::WITHDRAW))});
}

FM2_STUB_CASE(M2_LED_06, "M2-LED-06")
{
    // The ledger's two directions (R5), the engine branches of both kinds (R7) and admission's kind rule (R12).
    ledger::LedgerState ledger{EmptyRegtestLedger()};
    return FirstDeviation("M2-LED-06", {Observe(ledger.TransferToFutures(AccountId{}, AssetId::Native(), 1)),
                                        Observe(ledger.TransferFromFutures(AccountId{}, AssetId::Native(), 1)),
                                        Observe(engine::PhaseOf(ActionOfKind(ActionKind::SPOT_TO_FUTURES))),
                                        Observe(engine::PhaseOf(ActionOfKind(ActionKind::FUTURES_TO_SPOT))),
                                        Observe(admission::KindAdmissible(ActionKind::SPOT_TO_FUTURES)),
                                        Observe(admission::KindAdmissible(ActionKind::FUTURES_TO_SPOT))});
}

// ---- Slice R5: ledger/state_commitment ----

FM2_STUB_CASE(M2_LED_02, "M2-LED-02")
{
    return Observe(ledger::CommitV1(EmptyRegtestLedger()));
}

FM2_STUB_CASE(M2_LED_03, "M2-LED-03")
{
    return Observe(ledger::LiabilitySumRoot(EmptyRegtestLedger(), AssetId::Native()));
}

FM2_STUB_CASE(M2_LED_04, "M2-LED-04")
{
    return Observe(ledger::ExitValue(EmptyRegtestLedger(), AccountId{}, AssetId::Native()));
}

// ---- Slice R6: engine/residual ----

FM2_STUB_CASE(M3_ENG_02, "M3-ENG-02")
{
    const MarketDescriptor market{AssetId::Native(), AssetId{uint256::ONE}, 1, 1};
    engine::OrderBook book{engine::OrderBook::Create(market).value()};
    return Observe(engine::ResolveResidualCross(book, {}, {}, RegtestProfile()));
}

// ---- Slice R7: engine/engine ----

FM2_STUB_CASE(M3_ENG_09, "M3-ENG-09")
{
    // The engine branch (R7) and admission's kind rule (R12).
    return FirstDeviation("M3-ENG-09", {Observe(engine::PhaseOf(ActionOfKind(ActionKind::CANCEL_ALL))),
                                        Observe(admission::KindAdmissible(ActionKind::CANCEL_ALL))});
}

FM2_STUB_CASE(M9_FUT_04, "M9-FUT-04")
{
    // Phase 7 with futures enabled. Validate() refuses that profile first
    // (M9-FUT-01), so the engine itself never gets here in the outline.
    // And the futures domain's sweep (R13).
    ProfileParams profile{RegtestProfile()};
    engine::EngineState state{engine::EngineState::Genesis(profile, {}).value()};
    profile.futures_enabled = true;
    futures::FuturesDomain domain{RegtestProfile()};
    return FirstDeviation("M9-FUT-04", {Observe(engine::RunFuturesPhase(state, profile)), Observe(domain.Sweep(1))});
}

// ---- Slice R8: storage/file_journal ----

FM2_STUB_CASE(M4_STO_06, "M4-STO-06")
{
    // Rollover itself, and an append that no longer fits in the segment.
    const TestDir dir{"stub_m4_sto_06"};
    auto journal{storage::FileJournal::CreateNew(dir.path, storage::JournalRole::COMMIT, 1, storage::SEGMENT_PAGE_BYTES).value()};
    const std::vector<unsigned char> too_big(storage::SEGMENT_PAGE_BYTES);
    return FirstDeviation("M4-STO-06", {Observe(journal->Rollover()), Observe(journal->Append(storage::RecordType::DECISION, too_big))});
}

// ---- Slice R8: storage logs, writer and snapshots ----

FM2_STUB_CASE(M4_STO_02, "M4-STO-02")
{
    auto journal{NewMemoryJournal(storage::JournalRole::COMMIT, 1)};
    return Observe(storage::LogWriter::Start(*journal, storage::LogWriterOptions{}));
}

FM2_STUB_CASE(M4_STO_03, "M4-STO-03")
{
    // Write and Load; the store touches no file in the outline. StateOwner::Restore (R9).
    storage::SnapshotStore store{fs::PathFromString(FM2_TEST_TMP_DIR) / fs::u8path("stub_m4_sto_03_never_created")};
    const storage::SnapshotMark mark{1, uint256::ONE, uint256::ONE};
    HeartbeatOwner heartbeat{MakeHeartbeatOwner()};
    const std::shared_ptr<const state::HeadView> head{heartbeat.owner->Head()};
    const std::vector<storage::JournalRecord> marked{{1, storage::RecordType::SNAPSHOT_MARK, {}, uint256{}}};
    return FirstDeviation("M4-STO-03", {Observe(store.Write(mark, Bytes("state"))), Observe(store.Load(mark)),
                                        Observe(heartbeat.owner->Restore(*head, {})),
                                        Observe(state::ReplayDecisions(*head, marked))});
}

FM2_STUB_CASE(M4_STO_07, "M4-STO-07")
{
    // VIEW_STATE and NEW_VIEW_ACCEPTED in the signer log, PREPARED_QC in the
    // legacy commit-log entry point. Certified prepared replay is implemented.
    auto signer_journal{NewMemoryJournal(storage::JournalRole::SIGNER, 1)};
    auto commit_journal{NewMemoryJournal(storage::JournalRole::COMMIT, 2)};
    storage::SignerLog signer{storage::SignerLog::Create(*signer_journal).value()};
    storage::CommitLog commit{storage::CommitLog::Create(*commit_journal).value()};
    HeartbeatOwner heartbeat{MakeHeartbeatOwner()};
    return FirstDeviation("M4-STO-07", {Observe(signer.AppendViewState(storage::ViewStateRecord{0, 1})),
                                        Observe(signer.AppendNewViewAccepted(storage::NewViewAcceptedRecord{0, 1, Bytes("nv")})),
                                        Observe(commit.AppendPreparedQc(storage::PreparedQcRecord{0, 1, 1, uint256::ONE, Bytes("qc"), {}, {}}))});
}

// ---- Slice R9: the state owner ----

FM2_STUB_CASE(M4_AGR_10, "M4-AGR-10")
{
    // Preview over an uncommitted candidate (pipelining).
    HeartbeatOwner heartbeat{MakeHeartbeatOwner()};
    const state::Candidate candidate{
        heartbeat.owner->Preview(state::ParentRef::Committed(*heartbeat.owner->Head()), heartbeat.inputs).value()};
    return Observe(heartbeat.owner->Preview(state::ParentRef::Uncommitted(candidate), heartbeat.inputs));
}

// ---- Slice R10: chain adapters ----

//! A synthetic REGTEST network (TEST values only).
NetworkId StubRegtestNetwork()
{
    NetworkId network;
    network.chain = ChainType::REGTEST;
    network.magic = {0xf2, 0x7e, 0x57, 0x0a};
    network.modern_chain_domain = LabelHash("fm2/test/stub/network");
    return network;
}

FM2_STUB_CASE(MB1_ADP_06, "MB1-ADP-06")
{
    return Observe(chain::B3dTrustedFactsAdapter::Create(StubRegtestNetwork(), chain::B3dAdapterConfig{}));
}

FM2_STUB_CASE(M7_ADP_01, "M7-ADP-01")
{
    return Observe(chain::B3dFinalizedAdapter::Create(StubRegtestNetwork(), chain::B3dAdapterConfig{30}));
}

// ---- Slice R10: agreement ----

//! A sink that refuses everything (the stubs never send).
class StubSink final : public agreement::MessageSink
{
public:
    Result<void> Send(uint32_t, std::span<const unsigned char>) override { return Error{ErrorCode::BUSY, "stub sink"}; }
};

FM2_STUB_CASE(M4_AGR_01, "M4-AGR-01")
{
    agreement::PbftSafetyRules rules;
    const agreement::Proposal proposal{};
    return FirstDeviation("M4-AGR-01", {Observe(rules.MayPrepare(proposal)), Observe(rules.MayCommit(proposal, agreement::PreparedCert{})),
                                        Observe(rules.OnViewChange(agreement::ViewChange{}))});
}

FM2_STUB_CASE(M4_AGR_02, "M4-AGR-02")
{
    AgreementRig rig;
    agreement::PbftSafetyRules rules;
    StubSink sink;
    const agreement::PbftContext context{*rig.adapter, *rig.owner, rig.signer, rig.signer_log, rig.commit_log, rules, sink};
    // The k = 1 dev certifier receives no messages: its OnMessage is PBFT's to replace.
    const auto certifier{dev::DevSingleSeatCertifier::Create(rig.profile, *rig.adapter, rig.seats, rig.signer, rig.signer_log,
                                                             rig.commit_log)
                             .value()};
    const std::vector<unsigned char> message{1};
    return FirstDeviation("M4-AGR-02", {Observe(agreement::PbftAgreement::Create(rig.profile, agreement::PbftConfig{}, context)),
                                        Observe(certifier->OnMessage(0, message))});
}

FM2_STUB_CASE(M4_AGR_03, "M4-AGR-03")
{
    const agreement::LeaderSchedule schedule{4};
    return Observe(schedule.LeaderFor(AuthorityVersion{}, 0, agreement::SlowSet{}));
}

FM2_STUB_CASE(M4_AGR_04, "M4-AGR-04")
{
    return FirstDeviation("M4-AGR-04", {Observe(agreement::InclusionList::Build({}, 1)), Observe(agreement::InclusionList::Check({}, 4, {}, {}))});
}

FM2_STUB_CASE(M4_AGR_06, "M4-AGR-06")
{
    return Observe(agreement::RestartFence::Collect({}, TestSeats({"s0"}), 0));
}

FM2_STUB_CASE(M4_AGR_07, "M4-AGR-07")
{
    agreement::EvidenceStore store;
    return FirstDeviation("M4-AGR-07", {Observe(store.Record(agreement::Evidence{agreement::DoubleVote{}})), Observe(store.Load())});
}

// ---- Slice R11: b3verify ----

//! A certified header of the k = 1 test seat over one synthetic account leaf.
struct StubCertified {
    SeatSet seats{TestSeats({"s0"})};
    SyntheticState state{{LeafOf("alice", 1, {})}};
    SlotHeaderV2 header{SyntheticHeader(seats, 1, state.roots)};
    SlotCertificate cert{SignCertificate(header, {"s0"}, {0})};
};

FM2_STUB_CASE(M5_VFY_03, "M5-VFY-03")
{
    const StubCertified c;
    return Observe(verify::VerifyLiabilityPath(c.header, c.state.leaves.front(), verify::LiabilityPath{}));
}

FM2_STUB_CASE(M8_CKP_01, "M8-CKP-01")
{
    // b3verify's check (R11) and the builder (R13).
    const StubCertified c;
    const verify::CheckpointClaim claim{c.header.height, c.header.Hash(), c.header.state_root, uint256{}};
    HeartbeatOwner heartbeat{MakeHeartbeatOwner()};
    return FirstDeviation("M8-CKP-01", {Observe(verify::VerifyCheckpoint(claim, c.header, c.cert, c.seats)),
                                        Observe(bridge::CheckpointBuilder::Build(*heartbeat.owner->Head(), c.cert, {}))});
}

FM2_STUB_CASE(M7_ADP_02, "M7-ADP-02")
{
    const StubCertified c;
    return Observe(verify::VerifyAnchorProof(c.header, verify::AnchorProof{c.header.anchor, {}, {}}));
}

// ---- Slice R13: net/ client API ----

FM2_STUB_CASE(M5_API_01, "M5-API-01")
{
    ApiRig rig;
    return Observe(net::HttpsSseApi::Create(net::HttpsSseConfig{}, *rig.api));
}

FM2_STUB_CASE(M5_API_02, "M5-API-02")
{
    ApiRig rig;
    return Observe(rig.api->Subscribe(net::StreamRequest{}));
}

// ---- Slice R13: net/ seat mesh ----

FM2_STUB_CASE(M4_SIM_01, "M4-SIM-01")
{
    // R13: the in-process seat mesh; R14: a simulated cluster of more than one node.
    sim::TraceRecorder trace;
    return FirstDeviation("M4-SIM-01", {Observe(net::InProcessSeatMesh::Create(TestMeshConfig(0, 4), 4)),
                                        Observe(sim::SimCluster::Create(4, sim::SimNodeConfig{}, trace))});
}

// Retain the inventoried case name as a real boundary regression. Socket,
// session authentication and lane isolation live in tcp_mesh_tests.
BOOST_AUTO_TEST_CASE(M4_NET_01)
{
    net::TcpMeshConfig config;
    config.mesh = TestMeshConfig(0, 4);
    config.network_magic = {0xfa, 0xbf, 0xb5, 0xda};
    const auto mesh{net::TcpSeatMesh::Create(config)};
    BOOST_REQUIRE(!mesh.has_value());
    BOOST_CHECK(mesh.error().code == ErrorCode::INVALID_INPUT);
}

FM2_STUB_CASE(M4_NET_02, "M4-NET-02")
{
    const net::ShortIdCodec codec{uint256::ONE, 7};
    const std::vector<ActionId> known{ActionId{uint256::ONE}};
    return FirstDeviation("M4-NET-02", {Observe(codec.Encode(ActionId{uint256::ONE})), Observe(codec.Decode(agreement::ShortId{}, known))});
}

FM2_STUB_CASE(M4_NET_03, "M4-NET-03")
{
    auto mesh{RecordingMesh::Make(TestMeshConfig(0, 4))};
    net::SentryFanout fanout{*mesh};
    const std::vector<unsigned char> frame{1, 2, 3};
    StubObservation observed{Observe(fanout.Fanout(frame))};
    if (!mesh->sent.empty()) return StubObservation{true, std::nullopt}; // a stub must not send
    return observed;
}

// ---- Slice R13: bridge/ ----

FM2_STUB_CASE(M8_BRG_01, "M8-BRG-01")
{
    bridge::BridgeManagerV2 manager;
    return Observe(manager.OnFinalizedMint(bridge::FinalizedMint{}));
}

FM2_STUB_CASE(M8_BRG_03, "M8-BRG-03")
{
    const bridge::BridgeManagerV2 manager;
    return Observe(manager.CheckReleaseLimits(bridge::WithdrawalRequest{}, bridge::ReleaseWindow{}, bridge::ReleaseLimits{}));
}

// ---- Slice R13: futures/ ----

FM2_STUB_CASE(M9_FUT_02, "M9-FUT-02")
{
    futures::FuturesDomain domain{RegtestProfile()};
    const std::vector<futures::OracleSubmission> submissions(3);
    return Observe(domain.SubmitOracle(submissions));
}

FM2_STUB_CASE(M9_FUT_03, "M9-FUT-03")
{
    const futures::FuturesDomain domain{RegtestProfile()};
    return Observe(domain.MarginCheck(AccountId{}));
}

// ---- Slice R13: governance/ ----

FM2_STUB_CASE(M7_GOV_02, "M7-GOV-02")
{
    governance::Governance governance{RegtestProfile()};
    return Observe(governance.ApplyRemoval(governance::RemovalRequest{}));
}

FM2_STUB_CASE(M7_GOV_03, "M7-GOV-03")
{
    const governance::Governance governance{RegtestProfile()};
    return Observe(governance.ParamsAt(0));
}

// ---- Slice R14: sim/ ----

FM2_STUB_CASE(M4_SIM_02, "M4-SIM-02")
{
    // Each target-shape fault on its own is the stub; the three real faults are not.
    std::vector<sim::FaultPlan> plans(5);
    plans[0].crash_before_slot = 2;
    plans[1].drop.push_back(sim::DropRule{0, 1, 1'000});
    plans[2].delay.push_back(sim::DelayRule{0, 1, 5});
    plans[3].partition.push_back({0});
    plans[4].skew.push_back(sim::SkewRule{0, 3});
    std::vector<StubObservation> observed;
    for (const sim::FaultPlan& plan : plans) observed.push_back(Observe(sim::CheckFaultPlan(plan)));
    return FirstDeviation("M4-SIM-02", observed);
}

FM2_STUB_CASE(M4_SIM_03, "M4-SIM-03")
{
    sim::TraceRecorder trace;
    return Observe(sim::SimCluster::CreateTwins(4, 1, sim::SimNodeConfig{}, trace));
}

BOOST_AUTO_TEST_SUITE_END()
