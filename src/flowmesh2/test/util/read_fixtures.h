// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_TEST_UTIL_READ_FIXTURES_H
#define B3_FLOWMESH2_TEST_UTIL_READ_FIXTURES_H

#include <flowmesh2/core/authority.h>
#include <flowmesh2/core/certificate.h>
#include <flowmesh2/core/outcome.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/seat_set.h>
#include <flowmesh2/read/read_service.h>
#include <flowmesh2/read/views.h>
#include <flowmesh2/state/candidate.h>
#include <flowmesh2/state/state_owner.h>
#include <flowmesh2/storage/commit_log.h>
#include <flowmesh2/storage/durable.h>
#include <flowmesh2/storage/memory_journal.h>
#include <flowmesh2/test/util/state_fixtures.h>
#include <flowmesh2/test/util/verify_fixtures.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

/** TEST fixtures of the read suites (slice R11). A ReadRig is a state owner
 * over a genesis certified by a real BLS seat set (TestSeats, default the
 * one seat "s0"), with its own commit journal and log. Commit runs one slot
 * the way the sealing path will: certificate, DECISION, B-DECISION,
 * Confirm, Apply, APPLIED, B-APPLIED, Confirm; then it builds the slot's
 * SlotView, and returns the view with the DurableApplied token that
 * ReadService::Publish needs. Certificates are real aggregates of every
 * seat, or (real_certificates = false, for the long concurrency run) the
 * unsigned TestCertificate, which neither the commit log, the state owner
 * nor the read service verifies.
 */
namespace flowmesh2::test {

struct ReadRigOptions {
    ProfileParams profile{RegtestProfile()};
    std::vector<std::string> seat_names{"s0"};
    bool real_certificates{true};
    uint64_t journal_instance{7};
};

//! One committed slot, ready to publish: its head and outcomes, certificate, view and APPLIED token.
struct PublishableSlot {
    CommittedSlot slot;
    SlotCertificate cert;
    std::shared_ptr<const read::SlotView> view;
    DurableApplied applied;
};

struct ReadRig {
    ProfileParams profile;
    std::vector<std::string> seat_names;
    SeatSet seats;
    AuthorityVersion authority;
    bool real_certificates;
    SlotFixture actions;
    state::HeadView genesis;
    std::unique_ptr<storage::MemoryJournal> journal;
    storage::CommitLog log;
    std::unique_ptr<state::StateOwner> owner;

    explicit ReadRig(const ReadRigOptions& options = {})
        : profile{options.profile},
          seat_names{options.seat_names},
          seats{TestSeats(options.seat_names)},
          authority{AuthorityVersion{0, seats.Root(), ProfileHash(options.profile)}},
          real_certificates{options.real_certificates},
          actions{options.profile},
          genesis{state::GenesisHead(options.profile, std::vector<MarketDescriptor>{MarketM1(), MarketM2()}, TestDomain(), authority,
                                     TestAnchor(129), FactCursor{130, 0})
                      .value()},
          journal{NewCommitJournal(options.journal_instance)},
          log{storage::CommitLog::Create(*journal).value()},
          owner{state::StateOwner::Create(options.profile, genesis, options.journal_instance).value()}
    {
        Sync();
    }
    ReadRig(const ReadRig&) = delete;
    ReadRig& operator=(const ReadRig&) = delete;

    std::shared_ptr<const state::HeadView> Head() const { return owner->Head(); }

    //! Make `actions` follow the owner's head (ref_slot defaults to its height).
    void Sync()
    {
        const std::shared_ptr<const state::HeadView> head{owner->Head()};
        actions.header = head->Header();
        actions.state = head->State().engine;
    }

    //! Inputs after the head: facts_from = its facts_to, facts_to = the next deposit cursor.
    state::SlotInputs Inputs(std::vector<std::vector<unsigned char>> facts, std::vector<std::vector<unsigned char>> originals) const
    {
        state::SlotInputs inputs;
        inputs.authority = authority;
        inputs.anchor = TestAnchor(actions.anchor_height);
        inputs.facts_from = owner->Head()->Header().facts_to;
        inputs.facts_to = FactCursor{actions.anchor_height, actions.next_fact_index};
        inputs.body.facts = std::move(facts);
        inputs.body.originals = std::move(originals);
        return inputs;
    }

    state::Candidate Preview(const state::SlotInputs& inputs) const
    {
        Result<state::Candidate> candidate{owner->Preview(state::ParentRef::Committed(*owner->Head()), inputs)};
        RequireOk(candidate, "Preview");
        return std::move(candidate).value();
    }

    //! The certificate of `header`: every seat signs, or the unsigned test certificate.
    SlotCertificate CertificateOf(const SlotHeaderV2& header) const
    {
        if (!real_certificates) return TestCertificate(header);
        std::vector<uint32_t> all;
        for (uint32_t i{0}; i < seat_names.size(); ++i) all.push_back(i);
        return SignCertificate(header, seat_names, all);
    }

    //! Decide, apply and mark applied; then build the view (see the file comment).
    PublishableSlot Commit(state::Candidate candidate)
    {
        const SlotCertificate cert{CertificateOf(candidate.Header())};
        const Result<storage::PendingDecision> pending{log.AppendDecision(candidate.Header(), cert, candidate.Body())};
        RequireOk(pending, "AppendDecision");
        RequireOk(log.Barrier(), "B-DECISION");
        Result<DurableDecision> decision{log.Confirm(*pending)};
        RequireOk(decision, "Confirm DECISION");
        std::vector<OutcomeV2> outcomes{candidate.Outcomes()};
        Result<std::shared_ptr<const state::HeadView>> head{owner->Apply(std::move(candidate), *decision, authority)};
        RequireOk(head, "Apply");
        const Result<storage::PendingApplied> applied{log.AppendApplied(*decision, (*head)->Header().state_root)};
        RequireOk(applied, "AppendApplied");
        RequireOk(log.Barrier(), "B-APPLIED");
        Result<DurableApplied> token{log.Confirm(*applied)};
        RequireOk(token, "Confirm APPLIED");
        Result<std::shared_ptr<const read::SlotView>> view{read::SlotView::Build(**head, cert, outcomes)};
        RequireOk(view, "SlotView::Build");
        Sync();
        return PublishableSlot{CommittedSlot{*head, std::move(outcomes)}, cert, std::move(view).value(), std::move(token).value()};
    }

    PublishableSlot Run(std::vector<std::vector<unsigned char>> facts, std::vector<std::vector<unsigned char>> originals)
    {
        return Commit(Preview(Inputs(std::move(facts), std::move(originals))));
    }
};

//! ReadLimits for the regtest profile: 8 readers, the envelope cap, `view_slots` views, outcome index = max_delay_slots.
inline read::ReadLimits TestReadLimits(size_t view_slots = 8, const ProfileParams& profile = RegtestProfile())
{
    return read::ReadLimits{8, read::CertifiedEnvelope::MAX_ENCODED_BYTES, view_slots, profile.max_delay_slots};
}

inline std::unique_ptr<read::ReadService> NewReadService(const read::ReadLimits& limits, const ProfileParams& profile = RegtestProfile())
{
    Result<std::unique_ptr<read::ReadService>> service{read::ReadService::Create(limits, profile)};
    RequireOk(service, "ReadService::Create");
    return std::move(service).value();
}

inline void Publish(read::ReadService& service, const PublishableSlot& slot)
{
    RequireOk(service.Publish(slot.applied, slot.view), "Publish");
}

} // namespace flowmesh2::test

#endif // B3_FLOWMESH2_TEST_UTIL_READ_FIXTURES_H
