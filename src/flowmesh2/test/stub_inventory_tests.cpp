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
 * well.
 */

#include <boost/test/unit_test.hpp>

#include <flowmesh2/core/action.h>
#include <flowmesh2/core/facts.h>
#include <flowmesh2/core/market.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/engine/book.h>
#include <flowmesh2/engine/engine.h>
#include <flowmesh2/engine/matcher.h>
#include <flowmesh2/engine/residual.h>
#include <flowmesh2/ledger/ledger.h>
#include <flowmesh2/ledger/state_commitment.h>

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
    static const flowmesh2::test::StubRegistrar case_ident##_registrar{task_literal, #case_ident,     \
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

//! The provisional regtest profile, which Validate() accepts unchanged.
ProfileParams RegtestProfile() { return DevRegtestProfile(ChainType::REGTEST).value(); }

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
    ProfileParams params{RegtestProfile()};
    params.futures_enabled = true;
    return Observe(Validate(params));
}

FM2_STUB_CASE(M4_STO_05, "M4-STO-05")
{
    ProfileParams params{RegtestProfile()};
    params.signer_mode = SignerMode::FOLDED_SIGNING;
    return Observe(Validate(params));
}

// ---- Slice R4: core/action BatchVerifier ----

FM2_STUB_CASE(M4_ADM_01, "M4-ADM-01")
{
    return Observe(BatchVerifier{}.VerifyBatch({}));
}

// ---- Slice R4: core/facts stub kinds ----

FM2_STUB_CASE(M7_SEAT_01, "M7-SEAT-01")
{
    return FactStubProbe(SeatChangeFact{}, "M7-SEAT-01");
}

FM2_STUB_CASE(M7_GOV_01, "M7-GOV-01")
{
    return FactStubProbe(ListingFact{}, "M7-GOV-01");
}

FM2_STUB_CASE(MB1_B3D_02, "MB1-B3D-02")
{
    return FactStubProbe(CheckpointAcceptedFact{}, "MB1-B3D-02");
}

FM2_STUB_CASE(M8_BRG_02, "M8-BRG-02")
{
    return FactStubProbe(PayoutConfirmedFact{}, "M8-BRG-02");
}

FM2_STUB_CASE(M8_EXIT_01, "M8-EXIT-01")
{
    return FactStubProbe(ExitNullifierFact{}, "M8-EXIT-01");
}

// ---- Slice R5: ledger/ledger LedgerState ----

FM2_STUB_CASE(M2_LED_05, "M2-LED-05")
{
    // Two entry points: the ledger (R5) and the engine's WITHDRAW branch (R7).
    ledger::LedgerState ledger{EmptyRegtestLedger()};
    return FirstDeviation("M2-LED-05", {Observe(ledger.RequestWithdrawal(AccountId{}, WithdrawPayload{AssetId::Native(), 1, {0x51}})),
                                        Observe(engine::PhaseOf(ActionOfKind(ActionKind::WITHDRAW)))});
}

FM2_STUB_CASE(M2_LED_06, "M2-LED-06")
{
    // The ledger's two directions (R5) and the engine branches of both kinds (R7).
    ledger::LedgerState ledger{EmptyRegtestLedger()};
    return FirstDeviation("M2-LED-06", {Observe(ledger.TransferToFutures(AccountId{}, AssetId::Native(), 1)),
                                        Observe(ledger.TransferFromFutures(AccountId{}, AssetId::Native(), 1)),
                                        Observe(engine::PhaseOf(ActionOfKind(ActionKind::SPOT_TO_FUTURES))),
                                        Observe(engine::PhaseOf(ActionOfKind(ActionKind::FUTURES_TO_SPOT)))});
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
    return Observe(engine::PhaseOf(ActionOfKind(ActionKind::CANCEL_ALL)));
}

FM2_STUB_CASE(M9_FUT_04, "M9-FUT-04")
{
    // Phase 7 with futures enabled. Validate() refuses that profile first
    // (M9-FUT-01), so the engine itself never gets here in the outline.
    ProfileParams profile{RegtestProfile()};
    engine::EngineState state{engine::EngineState::Genesis(profile, {}).value()};
    profile.futures_enabled = true;
    return Observe(engine::RunFuturesPhase(state, profile));
}

BOOST_AUTO_TEST_SUITE_END()
