// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_TEST_UTIL_AGREEMENT_FIXTURES_H
#define B3_FLOWMESH2_TEST_UTIL_AGREEMENT_FIXTURES_H

#include <crypto/bls.h>
#include <flowmesh2/core/authority.h>
#include <flowmesh2/core/body.h>
#include <flowmesh2/core/facts.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/seat_set.h>
#include <flowmesh2/dev/fake_chain.h>
#include <flowmesh2/state/candidate.h>
#include <flowmesh2/state/state_owner.h>
#include <flowmesh2/storage/commit_log.h>
#include <flowmesh2/storage/journal.h>
#include <flowmesh2/storage/memory_journal.h>
#include <flowmesh2/storage/seat_signer.h>
#include <flowmesh2/storage/signer_log.h>
#include <flowmesh2/test/util/keys.h>
#include <flowmesh2/test/util/ledger_fixtures.h>
#include <flowmesh2/test/util/slot_fixtures.h>
#include <flowmesh2/test/util/storage_fixtures.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/** TEST fixtures of the chain adapter and agreement suites (slice R10).
 * Synthetic values only: the TEST network of test/util/slot_fixtures.h,
 * BLS seats from IKM = SHA256("fm2/test/seat/<label>"), label block hashes.
 * Needs FM2_DEV_COMPONENTS (test_flowmesh2 links fm2_dev): AgreementRig
 * uses the fake chain adapter.
 */
namespace flowmesh2::test {

inline std::string SeatLabel(std::string_view name) { return "fm2/test/seat/" + std::string{name}; }

//! The PoP-verified seat set of the BLS test keys of `names`, in that order.
inline SeatSet TestSeats(const std::vector<std::string>& names)
{
    std::vector<SeatRegistration> registrations;
    for (const std::string& name : names) {
        const bls::SecretKey key{BlsKeyFromLabel(SeatLabel(name))};
        registrations.push_back(SeatRegistration{key.GetPublicKey(), key.SignPoP()});
    }
    Result<SeatSet> seats{SeatSet::FromPoP(registrations)};
    RequireOk(seats, "SeatSet::FromPoP");
    return std::move(seats).value();
}

//! The authority of `seats` at `epoch` under the regtest profile.
inline AuthorityVersion SeatAuthority(const SeatSet& seats, uint64_t epoch = 0)
{
    return AuthorityVersion{epoch, seats.Root(), ProfileHash(RegtestProfile())};
}

//! The synthetic block hash of `height` on fork `fork`.
inline uint256 BlockHash(uint32_t height, std::string_view fork = "main")
{
    return LabelHash("fm2/test/block/" + std::string{fork} + "/" + std::to_string(height));
}

//! A deposit of `amount` rUSD to alice at `cursor`, from outpoint (label tx, 0).
inline DepositFact DepositAtCursor(const FactCursor& cursor, std::string_view tx, Atoms amount = 1'000)
{
    DepositFact fact{Deposit(TestAccount("alice"), TestAsset("rUSD"), amount, TestOutpoint(tx, 0))};
    fact.cursor = cursor;
    return fact;
}

//! Forwards every journal event to two observers, in order.
class TeeObserver : public storage::BarrierObserver
{
public:
    TeeObserver(storage::BarrierObserver& first, storage::BarrierObserver& second) : m_first{first}, m_second{second} {}
    void OnAppend(storage::JournalRole role, uint64_t instance_id, storage::RecordType type, uint64_t seq) override
    {
        m_first.OnAppend(role, instance_id, type, seq);
        m_second.OnAppend(role, instance_id, type, seq);
    }
    void OnBarrierStart(storage::JournalRole role, uint64_t instance_id, uint64_t target_seq) override
    {
        m_first.OnBarrierStart(role, instance_id, target_seq);
        m_second.OnBarrierStart(role, instance_id, target_seq);
    }
    void OnBarrierEnd(storage::JournalRole role, uint64_t instance_id, uint64_t target_seq, bool ok) override
    {
        m_first.OnBarrierEnd(role, instance_id, target_seq, ok);
        m_second.OnBarrierEnd(role, instance_id, target_seq, ok);
    }

private:
    storage::BarrierObserver& m_first;
    storage::BarrierObserver& m_second;
};

struct RigOptions {
    std::vector<std::string> seat_names{"s0"}; //!< the seat set; seat 0 is this rig's signer
    storage::MemoryJournalFaults signer_faults;
    storage::MemoryJournalFaults commit_faults;
};

/** Everything an agreement needs, in memory: the regtest profile, a seat set
 * (seat 0 signs), the fake chain over base TestAnchor(129) under
 * SeatAuthority(seats), a signer journal (instance 1) and a commit journal
 * (instance 2) whose events go to `recorder` and to the fake's JournalHook,
 * their logs, seat 0's SeatSigner, and a StateOwner over a genesis with
 * markets M1 and M2, anchor TestAnchor(129) and facts from (130, 0). Not
 * movable: the logs and the signer keep pointers into it.
 */
struct AgreementRig {
    ProfileParams profile;
    SeatSet seats;
    AuthorityVersion authority;
    std::unique_ptr<dev::FakeChainAdapter> adapter;
    RecordingObserver recorder;
    TeeObserver tee;
    std::unique_ptr<storage::MemoryJournal> signer_journal;
    std::unique_ptr<storage::MemoryJournal> commit_journal;
    storage::SignerLog signer_log;
    storage::CommitLog commit_log;
    storage::SeatSigner signer;
    state::HeadView genesis;
    std::unique_ptr<state::StateOwner> owner;

    explicit AgreementRig(const RigOptions& options = {})
        : profile{RegtestProfile()},
          seats{TestSeats(options.seat_names)},
          authority{SeatAuthority(seats)},
          adapter{dev::FakeChainAdapter::Create(TestNetwork(), authority, seats, TestAnchor(129)).value()},
          tee{recorder, adapter->JournalHook()},
          signer_journal{NewMemoryJournal(storage::JournalRole::SIGNER, 1, options.signer_faults, &tee)},
          commit_journal{NewMemoryJournal(storage::JournalRole::COMMIT, 2, options.commit_faults, &tee)},
          signer_log{storage::SignerLog::Create(*signer_journal).value()},
          commit_log{storage::CommitLog::Create(*commit_journal).value()},
          signer{storage::SeatSigner::FromIKM(SeatIkm(options.seat_names.front()), signer_log).value()},
          genesis{state::GenesisHead(profile, std::vector<MarketDescriptor>{MarketM1(), MarketM2()}, TestDomain(), authority,
                                     TestAnchor(129), FactCursor{130, 0})
                      .value()},
          owner{state::StateOwner::Create(profile, genesis, commit_journal->InstanceId()).value()}
    {
    }
    AgreementRig(const AgreementRig&) = delete;
    AgreementRig& operator=(const AgreementRig&) = delete;

    static std::vector<unsigned char> SeatIkm(std::string_view name)
    {
        const uint256 ikm{LabelHash(SeatLabel(name))};
        return {ikm.begin(), ikm.end()};
    }

    /** Inputs after the owner's head under `slot_authority`: anchor
     * TestAnchor(130), and `deposits` rUSD deposits to alice at the next
     * cursors of block 130 (labels `tag`/0, `tag`/1, ...).
     */
    state::SlotInputs Inputs(const AuthorityVersion& slot_authority, uint32_t deposits = 0, std::string_view tag = "fund") const
    {
        const std::shared_ptr<const state::HeadView> head{owner->Head()};
        state::SlotInputs inputs;
        inputs.authority = slot_authority;
        inputs.anchor = TestAnchor(130);
        inputs.facts_from = head->Header().facts_to;
        FactCursor next{inputs.facts_from};
        for (uint32_t i{0}; i < deposits; ++i) {
            Result<std::vector<unsigned char>> bytes{EncodeFact(DepositAtCursor(next, std::string{tag} + "/" + std::to_string(i)))};
            RequireOk(bytes, "EncodeFact");
            inputs.body.facts.push_back(std::move(bytes).value());
            next.index += 1;
        }
        inputs.facts_to = next;
        return inputs;
    }

    //! Preview `inputs` on the owner's head.
    state::Candidate Preview(const state::SlotInputs& inputs) const
    {
        Result<state::Candidate> candidate{owner->Preview(state::ParentRef::Committed(*owner->Head()), inputs)};
        RequireOk(candidate, "Preview");
        return std::move(candidate).value();
    }
};

} // namespace flowmesh2::test

#endif // B3_FLOWMESH2_TEST_UTIL_AGREEMENT_FIXTURES_H
