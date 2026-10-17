// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_TEST_UTIL_ADMISSION_FIXTURES_H
#define B3_FLOWMESH2_TEST_UTIL_ADMISSION_FIXTURES_H

#include <flowmesh2/admission/admission.h>
#include <flowmesh2/admission/budget.h>
#include <flowmesh2/admission/pool.h>
#include <flowmesh2/core/action.h>
#include <flowmesh2/core/body.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/read/read_service.h>
#include <flowmesh2/state/candidate.h>
#include <flowmesh2/test/util/read_fixtures.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/** TEST fixtures of the admission and pool suites (slice R12).
 *
 * A PoolRig is a ReadRig (a state owner with its commit journal, slot
 * signing helpers that follow the head) plus a read service and an action
 * pool created at the owner's head. Seal runs one slot the way the sealing
 * path of ARCHITECTURE.md section 3 will: TakeForSlot(head + 1), Preview,
 * the certificate, DECISION, Apply, APPLIED (ReadRig::Commit), read
 * Publish, then pool OnCommitted with the slot's outcomes and canonical
 * body. SealOriginals commits chosen originals without TakeForSlot (an
 * action the pool never held, as another seat's proposal would include it).
 *
 * An AdmissionRig adds a TestBudget and an Admission over the rig's owner,
 * read service and pool; Fresh gives another pool and admission on the
 * same owner and read service (one per precedence scenario).
 */
namespace flowmesh2::test {

struct PoolRigOptions {
    ReadRigOptions rig{ReadRigOptions{RegtestProfile(), {"s0"}, /*real_certificates=*/false, /*journal_instance=*/7}};
    std::optional<admission::PoolLimits> limits; //!< default PoolLimits::FromProfile(rig.profile)
};

inline std::unique_ptr<admission::ActionPool> NewPool(const admission::PoolLimits& limits, const ProfileParams& profile,
                                                      uint64_t head_height)
{
    Result<std::unique_ptr<admission::ActionPool>> pool{admission::ActionPool::Create(limits, profile, head_height)};
    RequireOk(pool, "ActionPool::Create");
    return std::move(pool).value();
}

//! The VerifiedAction of exact original bytes (the bytes must decode and carry a valid signature).
inline VerifiedAction VerifiedOf(const std::vector<unsigned char>& bytes, const ProfileParams& profile = RegtestProfile())
{
    Result<ActionV2> action{ActionV2::Decode(bytes, profile)};
    RequireOk(action, "ActionV2::Decode");
    Result<VerifiedAction> verified{VerifiedAction::Verify(std::move(action).value())};
    RequireOk(verified, "VerifiedAction::Verify");
    return std::move(verified).value();
}

//! The ActionId of exact original bytes.
inline ActionId IdOfBytes(const std::vector<unsigned char>& bytes, const ProfileParams& profile = RegtestProfile())
{
    Result<ActionV2> action{ActionV2::Decode(bytes, profile)};
    RequireOk(action, "ActionV2::Decode");
    return action->Id();
}

struct PoolRig {
    ProfileParams profile;
    ReadRig rig;
    std::unique_ptr<read::ReadService> read;
    std::unique_ptr<admission::ActionPool> pool;

    //! One sealed slot: what was taken, the committed slot, its canonical body and the pool's report.
    struct Sealed {
        admission::TakenSlot taken;
        PublishableSlot slot;
        SlotBody body;
        admission::CommitReport report;
    };

    explicit PoolRig(const PoolRigOptions& options = {})
        : profile{options.rig.profile},
          rig{options.rig},
          read{NewReadService(TestReadLimits(8, options.rig.profile), options.rig.profile)},
          pool{NewPool(options.limits.value_or(admission::PoolLimits::FromProfile(options.rig.profile)), options.rig.profile,
                       rig.Head()->Header().height)}
    {
    }
    PoolRig(const PoolRig&) = delete;
    PoolRig& operator=(const PoolRig&) = delete;

    SlotFixture& Actions() { return rig.actions; }
    std::shared_ptr<const state::HeadView> Head() const { return rig.Head(); }
    uint64_t Height() const { return rig.Head()->Header().height; }

    //! Insert exact bytes into the pool as checked against the owner's head.
    admission::InsertReply Insert(const std::vector<unsigned char>& bytes)
    {
        return pool->Insert(VerifiedOf(bytes, profile), Height());
    }

    //! Commit `originals` after the head (no TakeForSlot), publish, and tell the pool.
    Sealed SealOriginals(std::vector<std::vector<unsigned char>> facts, std::vector<std::vector<unsigned char>> originals)
    {
        state::Candidate candidate{rig.Preview(rig.Inputs(std::move(facts), std::move(originals)))};
        SlotBody body{candidate.Body()};
        PublishableSlot slot{rig.Commit(std::move(candidate))};
        Publish(*read, slot);
        Result<admission::CommitReport> report{pool->OnCommitted(slot.applied, *slot.slot.head, slot.slot.outcomes, body)};
        RequireOk(report, "ActionPool::OnCommitted");
        return Sealed{admission::TakenSlot{}, std::move(slot), std::move(body), *report};
    }

    //! TakeForSlot(head + 1), then SealOriginals of what was taken.
    Sealed Seal(std::vector<std::vector<unsigned char>> facts = {}, const admission::WithholdFilter& withhold = {},
                size_t max = std::numeric_limits<size_t>::max())
    {
        Result<admission::TakenSlot> taken{pool->TakeForSlot(Height() + 1, max, withhold)};
        RequireOk(taken, "ActionPool::TakeForSlot");
        Sealed sealed{SealOriginals(std::move(facts), taken->originals)};
        sealed.taken = std::move(taken).value();
        return sealed;
    }

    //! Slot 1: deposits for alice (rUSD), bob (B3 and TOK) and carol (rUSD).
    Sealed Fund() { return SealOriginals(FundingFacts(rig.actions), {}); }
};

/** A budget for tests: ALLOW unless `deny`, an INVARIANT error if `fail`;
 * counts every call; `on_check` (if set) runs inside Check, which lets a
 * test move the head between admission's snapshot and its pool insert.
 */
class TestBudget final : public admission::BudgetPolicy
{
public:
    std::atomic<bool> deny{false};
    std::atomic<bool> fail{false};
    std::atomic<uint64_t> checks{0};
    std::atomic<uint64_t> admitted{0};
    std::atomic<uint64_t> refused{0};
    std::function<void()> on_check;

    Result<admission::BudgetVerdict> Check(const admission::BudgetRequest&) override
    {
        ++checks;
        if (on_check) on_check();
        if (fail.load()) return Error{ErrorCode::INVARIANT, "test budget failure"};
        return Ok(deny.load() ? admission::BudgetVerdict::DENY : admission::BudgetVerdict::ALLOW);
    }
    void Settle(const admission::BudgetRequest&, bool was_admitted) override
    {
        if (was_admitted) {
            ++admitted;
        } else {
            ++refused;
        }
    }
};

inline std::unique_ptr<admission::Admission> NewAdmission(const ProfileParams& profile, const state::StateOwner& owner,
                                                          const read::ReadService& read, admission::ActionPool& pool,
                                                          admission::BudgetPolicy& budget)
{
    Result<std::unique_ptr<admission::Admission>> admission{admission::Admission::Create(profile, owner, read, pool, budget)};
    RequireOk(admission, "Admission::Create");
    return std::move(admission).value();
}

struct AdmissionRig : PoolRig {
    TestBudget budget;
    std::unique_ptr<admission::Admission> admission;

    explicit AdmissionRig(const PoolRigOptions& options = {})
        : PoolRig{options}, admission{NewAdmission(profile, *rig.owner, *read, *pool, budget)}
    {
    }

    //! Admit exact bytes; requires a reply (not an Error).
    admission::AdmissionReply Admit(const std::vector<unsigned char>& bytes)
    {
        Result<admission::AdmissionReply> reply{admission->Admit(bytes)};
        RequireOk(reply, "Admission::Admit");
        return std::move(reply).value();
    }
    admission::AdmissionStats Stats() const { return admission->Stats(); }
};

/** Requires exactly one more reply, of `code`, between `before` and `after`,
 * and `signatures` more signature checks; every other counter unchanged.
 */
inline void RequireOneReply(const admission::AdmissionStats& before, const admission::AdmissionStats& after, AdmissionCode code,
                            uint64_t signatures, std::string_view what)
{
    BOOST_CHECK_MESSAGE(after.replies == before.replies + 1, std::string{what} + ": not exactly one more reply");
    BOOST_CHECK_MESSAGE(after.errors == before.errors, std::string{what} + ": errors changed");
    BOOST_CHECK_MESSAGE(after.sig_verifications == before.sig_verifications + signatures,
                        std::string{what} + ": " + std::to_string(after.sig_verifications - before.sig_verifications) +
                            " signature checks, want " + std::to_string(signatures));
    for (const CodeInfo<AdmissionCode>& info : AdmissionCodeRegistry()) {
        const uint64_t want{before.Count(info.code) + (info.code == code ? 1 : 0)};
        BOOST_CHECK_MESSAGE(after.Count(info.code) == want, std::string{what} + ": counter " + std::string{info.name} + " is " +
                                                                std::to_string(after.Count(info.code)) + ", want " +
                                                                std::to_string(want));
    }
}

//! Requires `reply` to have `code`.
inline void RequireCode(const admission::AdmissionReply& reply, AdmissionCode code, std::string_view what)
{
    BOOST_CHECK_MESSAGE(reply.code == code, std::string{what} + ": got " + std::string{ToString(reply.code)} + " (" +
                                                std::string{reply.reason} + "), want " + std::string{ToString(code)});
}

//! The status the pool reports for `id`; fails the test if it reports none.
inline admission::PoolEntryStatus RequireStatus(const admission::ActionPool& pool, const ActionId& id)
{
    const std::optional<admission::PoolEntryStatus> status{pool.Status(id)};
    if (!status) BOOST_FAIL("the pool knows nothing about the action");
    return *status;
}

} // namespace flowmesh2::test

#endif // B3_FLOWMESH2_TEST_UTIL_ADMISSION_FIXTURES_H
