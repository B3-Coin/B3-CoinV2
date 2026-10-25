// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <flowmesh2/agreement/agreement.h>
#include <flowmesh2/agreement/messages.h>
#include <flowmesh2/agreement/pbft.h>
#include <flowmesh2/agreement/safety.h>
#include <flowmesh2/core/seat_set.h>
#include <flowmesh2/storage/control_log.h>
#include <flowmesh2/test/util/agreement_fixtures.h>
#include <flowmesh2/test/util/state_fixtures.h>
#include <util/chaintype.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

/** The seat counts the regtest PBFT accepts (M4-AGR-13 groundwork, O-98):
 * a full-view instance constructs for 4..7 seats, and the control journal
 * and PbftAgreement::Create refuse every other count. Decisions with seven
 * seats are in pbft_view_change_tests (the view-change harness).
 */

using namespace flowmesh2;
using namespace flowmesh2::agreement;
using namespace flowmesh2::test;

namespace {

class DiscardSink final : public MessageSink
{
public:
    Result<void> Send(uint32_t, std::span<const unsigned char>) override { return Ok(); }
};

//! Seat 0 of a k-seat full-view regtest PBFT, in memory, built one step at a time.
struct SeatCountRig {
    uint32_t k;
    ProfileParams profile{StableQViewRegtestProfile(ChainType::REGTEST).value()};
    SeatSet seats;
    AuthorityVersion authority;
    std::unique_ptr<dev::FakeChainAdapter> adapter;
    std::unique_ptr<storage::MemoryJournal> signer_journal;
    std::unique_ptr<storage::MemoryJournal> commit_journal;
    std::unique_ptr<storage::MemoryJournal> control_journal;
    storage::SignerLog signer_log;
    storage::CommitLog commit_log;
    state::HeadView genesis;
    std::unique_ptr<state::StateOwner> owner;
    PbftSafetyRules safety;
    DiscardSink sink;
    std::optional<storage::ControlLog> control_log;
    std::optional<storage::SeatSigner> signer;

    explicit SeatCountRig(uint32_t seat_count)
        : k{seat_count}, seats{TestSeats(Names(seat_count))}, authority{0, seats.Root(), ProfileHash(profile)},
          adapter{dev::FakeChainAdapter::Create(TestNetwork(), authority, seats, TestAnchor(129)).value()},
          signer_journal{NewMemoryJournal(storage::JournalRole::SIGNER, 10, {}, nullptr)},
          commit_journal{NewMemoryJournal(storage::JournalRole::COMMIT, 20, {}, nullptr)},
          control_journal{NewMemoryJournal(storage::JournalRole::SIGNER, 30, {}, nullptr)},
          signer_log{storage::SignerLog::Create(*signer_journal, profile).value()},
          commit_log{storage::CommitLog::Create(*commit_journal, profile).value()},
          genesis{state::GenesisHead(profile, std::vector<MarketDescriptor>{MarketM1(), MarketM2()}, TestDomain(), authority, TestAnchor(129), {130, 0}).value()},
          owner{state::StateOwner::Create(profile, genesis, commit_journal->InstanceId()).value()}
    {
    }

    static std::vector<std::string> Names(uint32_t k)
    {
        std::vector<std::string> names;
        for (uint32_t i{0}; i < k; ++i) names.push_back("s" + std::to_string(i));
        return names;
    }

    Result<void> OpenControlLog()
    {
        auto created{storage::ControlLog::Create(*control_journal, {TestDomain(), ProfileHash(profile), seats.Root(), 0, 0, k, seats.Keys()[0].Key()})};
        if (!created.has_value()) return created.error();
        control_log.emplace(std::move(*created));
        return Ok();
    }

    //! The full-view agreement of seat 0; the control log must be open.
    Result<std::unique_ptr<Agreement>> CreateFullView()
    {
        auto bound{storage::SeatSigner::FromIKM(AgreementRig::SeatIkm("s0"), signer_log, *control_log)};
        if (!bound.has_value()) return bound.error();
        signer.emplace(std::move(*bound));
        PbftConfig config;
        config.self_seat = 0;
        config.regtest_full_view = true;
        config.view_timeout_floor_ms = 500;
        const PbftContext context{*adapter, *owner, *signer, signer_log, commit_log, safety, sink, &*control_log};
        return PbftAgreement::Create(profile, config, context);
    }

    //! PbftAgreement::Create without a control journal: the seat-count gate comes first.
    Result<std::unique_ptr<Agreement>> CreateWithoutControl()
    {
        auto plain{storage::SeatSigner::FromIKM(AgreementRig::SeatIkm("s0"), signer_log)};
        if (!plain.has_value()) return plain.error();
        signer.emplace(std::move(*plain));
        PbftConfig config;
        config.self_seat = 0;
        config.regtest_full_view = true;
        config.view_timeout_floor_ms = 500;
        const PbftContext context{*adapter, *owner, *signer, signer_log, commit_log, safety, sink, nullptr};
        return PbftAgreement::Create(profile, config, context);
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(pbft_seat_count_tests)

BOOST_AUTO_TEST_CASE(full_view_pbft_constructs_with_four_to_seven_seats)
{
    // The largest frame a seat accepts grows with MAX_CONTROL_REPORTS (a
    // CONTROL_NEW_VIEW carries a quorum of reports); printed for the evidence.
    for (const auto& profile : {StableQViewRegtestProfile(ChainType::REGTEST).value(), StableQNetworkRegtestProfile(ChainType::REGTEST).value()}) {
        BOOST_TEST_MESSAGE("MaxMessageBytes " << MaxMessageBytes(profile));
    }
    for (uint32_t k{4}; k <= 7; ++k) {
        BOOST_TEST_CONTEXT("k = " << k)
        {
            SeatCountRig rig{k};
            BOOST_CHECK_EQUAL(rig.seats.Size(), k);
            const Result<void> opened{rig.OpenControlLog()};
            BOOST_REQUIRE_MESSAGE(opened.has_value(), "control journal for " << k << " seats: " << (opened.has_value() ? "" : std::string{opened.error().detail}));
            const auto agreement{rig.CreateFullView()};
            BOOST_REQUIRE_MESSAGE(agreement.has_value(), "agreement for " << k << " seats: " << (agreement.has_value() ? "" : std::string{agreement.error().detail}));
            BOOST_CHECK_EQUAL((*agreement)->CurrentView(), 0U);
            BOOST_CHECK(!(*agreement)->ChangingView());
        }
    }
}

BOOST_AUTO_TEST_CASE(other_seat_counts_are_refused_by_the_control_journal_and_the_agreement)
{
    for (const uint32_t k : {1U, 2U, 3U, 8U, 10U}) {
        BOOST_TEST_CONTEXT("k = " << k)
        {
            SeatCountRig rig{k};
            RequireFailure(rig.OpenControlLog(), ErrorCode::INVALID_INPUT, "CONTROL_REGTEST_VIEW_PROFILE", "control journal");
            RequireFailure(rig.CreateWithoutControl(), ErrorCode::INVALID_INPUT, "PBFT_SEAT_COUNT", "agreement");
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
