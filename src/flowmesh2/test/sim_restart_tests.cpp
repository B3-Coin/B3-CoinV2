// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** Restart of the simulated node from its journals and a snapshot store
 * (M4-STO-03; ARCHITECTURE.md O-71, O-89, O-95). A restart is byte-equal
 * to a run that never stopped, its pool answers an exact resend of a
 * retained included action DUPLICATE without a signature check, a decided
 * slot without APPLIED is completed under check 3, a check-3 refusal
 * leaves the live head and a node that seals nothing, and a snapshot that
 * does not reproduce its certified header is never a base.
 */

#include <boost/test/unit_test.hpp>

#include <flowmesh2/admission/admission.h>
#include <flowmesh2/admission/pool.h>
#include <flowmesh2/core/action.h>
#include <flowmesh2/core/facts.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/sim/sim_client.h>
#include <flowmesh2/sim/sim_node.h>
#include <flowmesh2/sim/trace.h>
#include <flowmesh2/state/replay.h>
#include <flowmesh2/state/snapshot.h>
#include <flowmesh2/state/state_owner.h>
#include <flowmesh2/storage/commit_log.h>
#include <flowmesh2/storage/memory_journal.h>
#include <flowmesh2/storage/record.h>
#include <flowmesh2/storage/signer_log.h>
#include <flowmesh2/storage/snapshot_mark.h>
#include <flowmesh2/storage/snapshot_store.h>
#include <flowmesh2/test/util/ledger_fixtures.h>
#include <flowmesh2/test/util/state_fixtures.h>
#include <flowmesh2/test/util/storage_fixtures.h>
#include <util/fs.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::sim;
using namespace flowmesh2::test;

namespace {

//! The retained window of every node here (SimNodeConfig::action_window_slots; the sim clients use max_delay 16).
constexpr uint16_t WINDOW{16};
//! Slots after which every run stores a snapshot.
const std::set<uint64_t> MARKED{8, 16, 24, 32};

SimNodeConfig Config()
{
    SimNodeConfig config;
    config.action_window_slots = WINDOW;
    return config;
}

std::unique_ptr<storage::SnapshotStore> OpenStore(const fs::path& dir)
{
    Result<std::unique_ptr<storage::SnapshotStore>> store{storage::SnapshotStore::Open(dir)};
    RequireOk(store, "SnapshotStore::Open");
    return std::move(store).value();
}

/** The same deterministic workload for every run: Alice and Bob trade one
 * lot of M1 every third slot from slot 3 on, after the deposits of slot 2.
 * Each action is admitted just before the slot that includes it, with the
 * committed head as its ref_slot.
 */
struct Workload {
    SimClient alice;
    SimClient bob;
    //! The label of every admitted action and the slot it was admitted for.
    std::map<uint64_t, std::vector<std::string>> labels;

    static Workload For(const SimNode& node)
    {
        const std::vector<MarketDescriptor> markets{node.Markets().begin(), node.Markets().end()};
        Result<SimClient> alice{SimClient::Create("alice", node.Profile(), node.Domain(), node.Seats(), markets)};
        Result<SimClient> bob{SimClient::Create("bob", node.Profile(), node.Domain(), node.Seats(), markets)};
        RequireOk(alice, "SimClient alice");
        RequireOk(bob, "SimClient bob");
        return Workload{std::move(alice).value(), std::move(bob).value(), {}};
    }

    //! Admit the actions of `slot` (the node's head is slot - 1).
    void Admit(SimNode& node, uint64_t slot)
    {
        if (slot < 3 || slot % 3 != 0) return;
        const uint64_t nonce{slot / 3};
        const std::string buy{"buy-" + std::to_string(slot)};
        const std::string sell{"sell-" + std::to_string(slot)};
        for (const auto& [label, bytes] :
             {std::pair{buy, alice.Place(buy, nonce, SimMarketM1(), Side::BUY, TimeInForce::GTC, 250, 1, slot - 1)},
              std::pair{sell, bob.Place(sell, nonce, SimMarketM1(), Side::SELL, TimeInForce::GTC, 250, 1, slot - 1)}}) {
            RequireOk(bytes, label + " bytes");
            const Result<admission::AdmissionReply> reply{node.Admitter().Admit(*bytes)};
            RequireOk(reply, label + " Admit");
            BOOST_REQUIRE_MESSAGE(reply->code == AdmissionCode::ADMITTED, label + " was not ADMITTED");
            labels[slot].push_back(label);
        }
    }

    //! The resend bytes of `label`.
    std::vector<unsigned char> Resend(const std::string& label) const
    {
        const SimClient& client{label.rfind("buy-", 0) == 0 ? alice : bob};
        Result<std::vector<unsigned char>> bytes{client.Bytes(label)};
        RequireOk(bytes, "resend " + label);
        return std::move(bytes).value();
    }
};

//! A node and the trace it records into (the trace outlives the node).
struct Run {
    std::unique_ptr<TraceRecorder> trace{std::make_unique<TraceRecorder>()};
    std::unique_ptr<SimNode> node;
    std::optional<Workload> workload;

    static Run Fresh(const SimNodeConfig& config = Config())
    {
        Run run;
        Result<std::unique_ptr<SimNode>> created{SimNode::Create(config, *run.trace)};
        RequireOk(created, "SimNode::Create");
        run.node = std::move(created).value();
        RequireOk(run.node->Chain().AppendBlock(SimBlockHash(SIM_BASE_HEIGHT + 1), {}), "AppendBlock 130");
        run.workload = Workload::For(*run.node);
        return run;
    }

    //! Seal slots up to `to`, with the workload's deposits and actions, storing a snapshot after every MARKED slot.
    void SealTo(uint64_t to, storage::SnapshotStore* store)
    {
        for (uint64_t slot{node->Owner().Head()->Header().height + 1}; slot <= to; ++slot) {
            if (slot == 2) {
                std::vector<Fact> facts;
                facts.push_back(DepositFact{FactCursor{SIM_BASE_HEIGHT + 2, 0}, Outpoint{SimLabelHash("fm2/test/restart/0"), 0},
                                            workload->alice.Account(), SimAsset("rUSD"), 1'000'000'000});
                facts.push_back(DepositFact{FactCursor{SIM_BASE_HEIGHT + 2, 1}, Outpoint{SimLabelHash("fm2/test/restart/1"), 0},
                                            workload->bob.Account(), AssetId::Native(), 1'000'000'000});
                RequireOk(node->Chain().AppendBlock(SimBlockHash(SIM_BASE_HEIGHT + 2), std::move(facts)), "AppendBlock 131");
            }
            workload->Admit(*node, slot);
            RequireOk(node->SealSlot(), "SealSlot " + std::to_string(slot));
            if (store != nullptr && MARKED.contains(slot)) RequireOk(node->Snapshot(*store), "Snapshot " + std::to_string(slot));
        }
    }

    //! Crash the node and restart it over what is left; the workload (the clients) survives.
    void CrashAndRestart(const storage::SnapshotStore* store, const SimNodeConfig& config = Config())
    {
        SimCrash crash{SimNode::Crash(std::move(node))};
        Restart(std::move(crash), store, config);
    }

    void Restart(SimCrash crash, const storage::SnapshotStore* store, const SimNodeConfig& config = Config())
    {
        trace = std::make_unique<TraceRecorder>();
        Result<std::unique_ptr<SimNode>> restarted{SimNode::Restart(std::move(crash), config, *trace, store)};
        RequireOk(restarted, "SimNode::Restart");
        node = std::move(restarted).value();
    }

    std::vector<std::string> Steps() const
    {
        std::vector<std::string> steps;
        for (const TraceEvent& event : trace->Events()) steps.push_back(event.Step() + (event.detail.empty() ? "" : " " + event.detail));
        return steps;
    }

    bool Traced(const std::string& step) const
    {
        const std::vector<std::string> steps{Steps()};
        return std::find(steps.begin(), steps.end(), step) != steps.end();
    }
};

//! Both runs reached the same committed state with the same durable bytes.
void RequireSameRun(const Run& a, const Run& b, std::string_view what)
{
    const auto head_a{a.node->Owner().Head()};
    const auto head_b{b.node->Owner().Head()};
    BOOST_CHECK_MESSAGE(head_a->HeaderHash() == head_b->HeaderHash(), std::string{what} + ": header");
    BOOST_CHECK_MESSAGE(head_a->CertHash() == head_b->CertHash(), std::string{what} + ": certificate");
    BOOST_CHECK_MESSAGE(head_a->State() == head_b->State(), std::string{what} + ": state");
    BOOST_CHECK_MESSAGE(a.node->CommitJournal().DurableImage() == b.node->CommitJournal().DurableImage(), std::string{what} + ": commit journal bytes");
    BOOST_CHECK_MESSAGE(a.node->SignerJournal().DurableImage() == b.node->SignerJournal().DurableImage(), std::string{what} + ": signer journal bytes");
    BOOST_CHECK_MESSAGE(a.node->Read().PublishedHeight() == b.node->Read().PublishedHeight(), std::string{what} + ": published height");
    BOOST_CHECK_MESSAGE(a.node->Pool().IncludedRecords() == b.node->Pool().IncludedRecords(), std::string{what} + ": retained INCLUDED records");
    BOOST_CHECK_MESSAGE(a.node->Pool().HeadHeight() == b.node->Pool().HeadHeight(), std::string{what} + ": pool head");
}

//! A commit journal image without its last record (what a crash before that record's barrier leaves).
std::vector<unsigned char> WithoutLastRecord(SimNode& node)
{
    const Result<std::vector<storage::JournalRecord>> records{node.CommitJournal().ReadAll()};
    RequireOk(records, "commit ReadAll");
    Result<std::unique_ptr<storage::MemoryJournal>> journal{
        storage::MemoryJournal::Create(storage::JournalRole::COMMIT, node.CommitJournal().InstanceId())};
    RequireOk(journal, "MemoryJournal::Create");
    for (size_t i{0}; i + 1 < records->size(); ++i) RequireOk((*journal)->Append((*records)[i].type, (*records)[i].payload), "Append");
    RequireOk((*journal)->Barrier(), "Barrier");
    return (*journal)->DurableImage();
}

} // namespace

BOOST_AUTO_TEST_SUITE(sim_restart_tests)

BOOST_AUTO_TEST_CASE(a_restart_from_a_snapshot_is_byte_equal_to_an_uninterrupted_run)
{
    const TestDir dir{"sim-restart-equal"};
    const auto store_a{OpenStore(dir.Sub("a"))};
    const auto store_b{OpenStore(dir.Sub("b"))};
    Run a{Run::Fresh()};
    Run b{Run::Fresh()};
    a.SealTo(34, store_a.get());
    b.SealTo(34, store_b.get());
    RequireSameRun(a, b, "before the crash");

    // The last applied slot is 34 and the window 16, so the base is the newest mark at or below 18: slot 16.
    b.CrashAndRestart(store_b.get());
    BOOST_REQUIRE(b.node->Restarted().has_value());
    BOOST_CHECK_EQUAL(b.node->Restarted()->base_height, 16U);
    BOOST_CHECK_EQUAL(b.node->Restarted()->applied_height, 34U);
    BOOST_CHECK_EQUAL(b.node->Restarted()->replayed, 18U);
    BOOST_CHECK_EQUAL(b.node->Restarted()->tail, 0U);
    BOOST_CHECK_EQUAL(b.node->Restarted()->skipped_marks, 0U);
    BOOST_CHECK(b.Traced("restart.base slot=16"));
    BOOST_CHECK(!b.node->Fenced());
    RequireSameRun(a, b, "after the restart");

    // Both nodes answer every resend alike. The actions of slot 30 are
    // retained: DUPLICATE from the pool, before any signature check (A3,
    // O-71). Those of slot 12 left the window: EXPIRED on both.
    for (const uint64_t slot : {12U, 30U, 33U}) {
        for (const std::string& label : a.workload->labels.at(slot)) {
            const std::vector<unsigned char> bytes{a.workload->Resend(label)};
            const uint64_t checked_a{a.node->Admitter().Stats().sig_verifications};
            const uint64_t checked_b{b.node->Admitter().Stats().sig_verifications};
            const Result<admission::AdmissionReply> reply_a{a.node->Admitter().Admit(bytes)};
            const Result<admission::AdmissionReply> reply_b{b.node->Admitter().Admit(bytes)};
            RequireOk(reply_a, label + " uninterrupted");
            RequireOk(reply_b, label + " restarted");
            BOOST_CHECK_MESSAGE(reply_b->code == reply_a->code, label + ": restarted " + std::string{ToString(reply_b->code)} +
                                                                    ", uninterrupted " + std::string{ToString(reply_a->code)});
            BOOST_CHECK(reply_b->included == reply_a->included);
            BOOST_CHECK_EQUAL(b.node->Admitter().Stats().sig_verifications - checked_b,
                              a.node->Admitter().Stats().sig_verifications - checked_a);
            if (slot >= 30) {
                BOOST_CHECK_MESSAGE(reply_b->code == AdmissionCode::DUPLICATE, label + " is not DUPLICATE after the restart");
                BOOST_CHECK_EQUAL(b.node->Admitter().Stats().sig_verifications, checked_b);
            } else {
                BOOST_CHECK_MESSAGE(reply_b->code == AdmissionCode::EXPIRED, label + " is not EXPIRED");
            }
        }
    }

    // Both keep sealing alike.
    a.SealTo(40, store_a.get());
    b.SealTo(40, store_b.get());
    RequireSameRun(a, b, "six slots after the restart");
}

BOOST_AUTO_TEST_CASE(a_restart_without_snapshots_replays_from_genesis_to_the_same_bytes)
{
    Run a{Run::Fresh()};
    Run b{Run::Fresh()};
    a.SealTo(20, nullptr);
    b.SealTo(20, nullptr);
    b.CrashAndRestart(nullptr);
    BOOST_REQUIRE(b.node->Restarted().has_value());
    BOOST_CHECK_EQUAL(b.node->Restarted()->base_height, 0U);
    BOOST_CHECK_EQUAL(b.node->Restarted()->replayed, 20U);
    RequireSameRun(a, b, "after the restart");
    a.SealTo(23, nullptr);
    b.SealTo(23, nullptr);
    RequireSameRun(a, b, "after three more slots");
}

BOOST_AUTO_TEST_CASE(a_restart_completes_a_decided_slot_under_check_3)
{
    // A crash after B-DECISION of slot 11, before its B-APPLIED.
    Run a{Run::Fresh()};
    Run b{Run::Fresh()};
    a.SealTo(12, nullptr);
    b.SealTo(11, nullptr);
    {
        const Result<std::vector<storage::JournalRecord>> records{b.node->CommitJournal().ReadAll()};
        RequireOk(records, "commit ReadAll");
        BOOST_REQUIRE(records->back().type == storage::RecordType::APPLIED);
    }
    SimCrash crash{SimNode::Crash(std::move(b.node))};
    {
        Run probe{Run::Fresh()};
        probe.SealTo(11, nullptr);
        crash.commit_image = WithoutLastRecord(*probe.node);
    }
    b.Restart(std::move(crash), nullptr);
    BOOST_REQUIRE(b.node->Restarted().has_value());
    BOOST_CHECK_EQUAL(b.node->Restarted()->applied_height, 10U);
    BOOST_CHECK_EQUAL(b.node->Restarted()->tail, 1U);
    BOOST_CHECK(b.Traced("restart.check3 slot=11,OK"));
    BOOST_CHECK(!b.node->Fenced());
    BOOST_CHECK_EQUAL(b.node->Owner().Head()->Header().height, 11U);
    BOOST_CHECK(b.node->Read().PublishedHeight() == std::optional<uint64_t>{11});
    b.SealTo(12, nullptr);
    RequireSameRun(a, b, "the completed slot and the next");
}

BOOST_AUTO_TEST_CASE(a_restart_after_a_check_3_refusal_keeps_the_live_head_and_seals_nothing)
{
    SimNodeConfig faulty{Config()};
    faulty.faults.bump_authority_before_apply_at = 2;
    Run run{Run::Fresh(faulty)};
    RequireOk(run.node->SealSlot(), "SealSlot 1");
    BOOST_REQUIRE(!run.node->SealSlot().has_value());
    BOOST_REQUIRE(run.node->Fenced());
    const auto live{run.node->Owner().Head()};
    BOOST_REQUIRE_EQUAL(live->Header().height, 1U);

    // The chain keeps its new authority across the crash; check 3 refuses slot 2 again.
    run.CrashAndRestart(nullptr);
    BOOST_REQUIRE(run.node->Restarted().has_value());
    BOOST_CHECK_EQUAL(run.node->Restarted()->tail, 1U);
    BOOST_CHECK(run.Traced("restart.check3 slot=2,AUTHORITY_CHANGED"));
    BOOST_CHECK(run.node->Fenced());
    BOOST_CHECK(run.node->Owner().Head()->HeaderHash() == live->HeaderHash());
    BOOST_CHECK(run.node->Owner().Head()->State() == live->State());
    BOOST_CHECK(run.node->Read().PublishedHeight() == std::optional<uint64_t>{1});

    // Nothing is signed, decided or applied at height 2 again.
    const storage::JournalStatus signer_before{run.node->SignerJournal().Status()};
    const storage::JournalStatus commit_before{run.node->CommitJournal().Status()};
    const std::vector<unsigned char> commit_bytes{run.node->CommitJournal().DurableImage()};
    const Result<SealReport> sealed{run.node->SealSlot()};
    BOOST_REQUIRE(!sealed.has_value());
    BOOST_CHECK(sealed.error().code == ErrorCode::FENCED);
    BOOST_CHECK(run.node->SignerJournal().Status() == signer_before);
    BOOST_CHECK(run.node->CommitJournal().Status() == commit_before);
    BOOST_CHECK(run.node->CommitJournal().DurableImage() == commit_bytes);
    BOOST_CHECK_EQUAL(run.node->Owner().Head()->Header().height, 1U);
    const Result<std::vector<storage::JournalRecord>> records{run.node->CommitJournal().ReadAll()};
    RequireOk(records, "commit ReadAll");
    BOOST_CHECK_EQUAL(std::count_if(records->begin(), records->end(),
                                    [](const storage::JournalRecord& r) { return r.type == storage::RecordType::APPLIED; }),
                      1);
}

BOOST_AUTO_TEST_CASE(a_snapshot_that_does_not_reproduce_its_header_is_never_a_base)
{
    const TestDir dir{"sim-restart-untrusted"};
    const auto store_a{OpenStore(dir.Sub("a"))};
    const auto store_b{OpenStore(dir.Sub("b"))};
    Run a{Run::Fresh()};
    Run b{Run::Fresh()};
    a.SealTo(34, store_a.get());
    b.SealTo(34, store_b.get());

    // A store holding the state of slot 8 under the mark of slot 16: refused, slot 8 is the base.
    const auto forged{OpenStore(dir.Sub("forged"))};
    const Result<std::vector<storage::SnapshotMark>> marks{store_b->Marks()};
    RequireOk(marks, "Marks");
    BOOST_REQUIRE_EQUAL(marks->size(), MARKED.size());
    for (const storage::SnapshotMark& mark : *marks) {
        const storage::SnapshotMark& source{mark.slot == 16 ? marks->front() : mark};
        const Result<std::vector<unsigned char>> bytes{store_b->Load(source)};
        RequireOk(bytes, "Load");
        RequireOk(forged->Write(mark, *bytes), "forged Write");
    }
    SimCrash crash{SimNode::Crash(std::move(b.node))};
    b.Restart(std::move(crash), forged.get());
    BOOST_REQUIRE(b.node->Restarted().has_value());
    BOOST_CHECK_EQUAL(b.node->Restarted()->base_height, 8U);
    BOOST_CHECK_EQUAL(b.node->Restarted()->skipped_marks, 1U);
    BOOST_CHECK(b.Traced("restart.snapshot_refused slot=16,VERIFY_FAILED"));
    RequireSameRun(a, b, "after a restart past a forged snapshot");

    // A store that lost every snapshot: genesis.
    const auto empty{OpenStore(dir.Sub("empty"))};
    b.CrashAndRestart(empty.get());
    BOOST_REQUIRE(b.node->Restarted().has_value());
    BOOST_CHECK_EQUAL(b.node->Restarted()->base_height, 0U);
    BOOST_CHECK_EQUAL(b.node->Restarted()->skipped_marks, 2U);
    RequireSameRun(a, b, "after a restart with no snapshot left");
}

BOOST_AUTO_TEST_CASE(restore_takes_only_a_marked_base_of_its_own_authority)
{
    const TestDir dir{"sim-restart-restore"};
    const auto store{OpenStore(dir.Sub("store"))};
    Run run{Run::Fresh()};
    run.SealTo(20, store.get());
    SimNode& node{*run.node};
    const Result<std::vector<storage::JournalRecord>> records{node.CommitJournal().ReadAll()};
    RequireOk(records, "commit ReadAll");
    const auto owner_at_genesis{[&] {
        Result<std::unique_ptr<state::StateOwner>> owner{state::StateOwner::Create(node.Profile(), node.Genesis(), node.CommitJournal().InstanceId())};
        RequireOk(owner, "StateOwner::Create");
        return std::move(owner).value();
    }};
    const auto sealed_head{[&](uint64_t height) { return node.Sealed().at(height - 1).head; }};

    // The base of the mark of slot 16: Restore installs slot 20.
    const storage::SnapshotMark mark{16, sealed_head(16)->HeaderHash(), sealed_head(16)->Header().state_root};
    const Result<state::HeadView> base{
        state::SnapshotHead(store->Load(mark).value(), node.Profile(), sealed_head(16)->Header(), sealed_head(16)->CertHash())};
    RequireOk(base, "SnapshotHead 16");
    {
        auto owner{owner_at_genesis()};
        const Result<state::ReplayResult> restored{owner->Restore(*base, *records, node.Seats())};
        RequireOk(restored, "Restore from slot 16");
        BOOST_CHECK(owner->Head()->HeaderHash() == node.Owner().Head()->HeaderHash());
        BOOST_CHECK(owner->Head()->State() == node.Owner().Head()->State());
        // Only once: the owner no longer holds its genesis.
        RequireError(owner->Restore(*base, *records, node.Seats()), ErrorCode::STALE_PARENT, "a second Restore");
    }
    // A committed head without a durable mark is no base.
    {
        auto owner{owner_at_genesis()};
        RequireFailure(owner->Restore(*sealed_head(17), *records, node.Seats()), ErrorCode::VERIFY_FAILED, state::RESTORE_BASE_UNMARKED,
                       "unmarked slot 17");
        BOOST_CHECK_EQUAL(owner->Head()->Header().height, 0U);
    }
    // Another node's seat set (a node of commit instance 9 has the same seat, so its records are what differ).
    {
        auto owner{owner_at_genesis()};
        SimNodeConfig foreign{Config()};
        foreign.commit_journal_instance = 9;
        Run other{Run::Fresh(foreign)};
        other.SealTo(17, nullptr);
        const Result<std::vector<storage::JournalRecord>> theirs{other.node->CommitJournal().ReadAll()};
        RequireOk(theirs, "other commit ReadAll");
        RequireFailure(owner->Restore(*base, *theirs, node.Seats()), ErrorCode::VERIFY_FAILED, state::RESTORE_RECORD_CHAIN,
                       "another journal instance's records");
        BOOST_CHECK_EQUAL(owner->Head()->Header().height, 0U);
    }
    // A base whose state is not its header's: the state of slot 8 under the header of slot 16.
    {
        auto owner{owner_at_genesis()};
        const Result<state::HeadView> mixed{state::HeadView::Make(sealed_head(16)->Header(), sealed_head(16)->CertHash(),
                                                                  std::make_shared<const state::CoreState>(sealed_head(8)->State()))};
        RequireOk(mixed, "HeadView::Make");
        RequireFailure(owner->Restore(*mixed, *records, node.Seats()), ErrorCode::VERIFY_FAILED, state::OWNER_STATE_ROOT_MISMATCH,
                       "the state of another slot");
        BOOST_CHECK_EQUAL(owner->Head()->Header().height, 0U);
    }
}

BOOST_AUTO_TEST_SUITE_END()
