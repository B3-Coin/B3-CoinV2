// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** The chain adapter seam (slice R10): PollReply, CheckBatch,
 * ChainAdapter::CheckAuthority and the b3d adapter factories. The REGTEST
 * fake adapter has its own suite (fake_chain_tests).
 */

#include <boost/test/unit_test.hpp>

#include <flowmesh2/chain/b3d_adapter.h>
#include <flowmesh2/chain/chain_adapter.h>
#include <flowmesh2/core/body.h>
#include <flowmesh2/core/facts.h>
#include <flowmesh2/core/state_root.h>
#include <flowmesh2/test/util/ledger_fixtures.h>
#include <flowmesh2/test/util/slot_fixtures.h>
#include <flowmesh2/test/util/storage_fixtures.h>

#include <memory>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::test;

namespace {

AuthorityVersion Authority(uint64_t epoch = 3)
{
    return AuthorityVersion{epoch, LabelHash("fm2/test/chain/seats"), ProfileHash(RegtestProfile())};
}

DepositFact DepositAt(FactCursor cursor, std::string_view tx, Atoms amount = 1'000)
{
    DepositFact fact{Deposit(TestAccount("alice"), TestAsset("rUSD"), amount, TestOutpoint(tx, 0))};
    fact.cursor = cursor;
    return fact;
}

//! Facts (130, 0..2) under anchor 130, read from (130, 0) up to (131, 0).
chain::FactBatch GoodBatch()
{
    chain::FactBatch batch;
    batch.from = FactCursor{130, 0};
    batch.to = FactCursor{131, 0};
    batch.anchor = TestAnchor(130);
    batch.authority = Authority();
    batch.facts = {DepositAt({130, 0}, "t0"), DepositAt({130, 1}, "t1"), DepositAt({130, 2}, "t2")};
    return batch;
}

//! Checks (as an assertion) that CheckBatch accepts `batch` for Poll(after, max).
void CheckBatchAccepted(const chain::FactBatch& batch, const FactCursor& after, size_t max, std::string_view what)
{
    const Result<void> result{chain::CheckBatch(batch, after, max)};
    BOOST_CHECK_MESSAGE(result.has_value(), std::string{what} + (result.has_value() ? std::string{} : ": " + std::string{result.error().detail}));
}

//! A ChainAdapter whose authority the test sets; only CurrentAuthority matters here.
class StaticAdapter final : public chain::ChainAdapter
{
public:
    AuthorityVersion authority;
    NetworkId Network() const override { return TestNetwork(); }
    AuthorityVersion CurrentAuthority() const override { return authority; }
    Result<chain::PollReply> Poll(const FactCursor&, size_t) override { return Error{ErrorCode::INVALID_INPUT, "unused"}; }
    Result<void> VerifyAnchor(const Anchor&, const Anchor&) const override { return Error{ErrorCode::INVALID_INPUT, "unused"}; }
    Result<SeatSet> SeatSetAt(uint64_t) const override { return Error{ErrorCode::INVALID_INPUT, "unused"}; }
};

} // namespace

BOOST_AUTO_TEST_SUITE(chain_adapter_tests)

BOOST_AUTO_TEST_CASE(poll_status_registry_pinned)
{
    BOOST_CHECK_EQUAL(static_cast<int>(chain::PollStatus::FACTS), 1);
    BOOST_CHECK_EQUAL(static_cast<int>(chain::PollStatus::BUSY), 2);
    BOOST_CHECK_EQUAL(static_cast<int>(chain::PollStatus::NEED_DATA), 3);
    BOOST_CHECK_EQUAL(static_cast<int>(chain::PollStatus::AUTHORITY_CHANGED), 4);
    BOOST_CHECK_EQUAL(chain::ToString(chain::PollStatus::FACTS), "FACTS");
    BOOST_CHECK_EQUAL(chain::ToString(chain::PollStatus::BUSY), "BUSY");
    BOOST_CHECK_EQUAL(chain::ToString(chain::PollStatus::NEED_DATA), "NEED_DATA");
    BOOST_CHECK_EQUAL(chain::ToString(chain::PollStatus::AUTHORITY_CHANGED), "AUTHORITY_CHANGED");
    for (int value : {0, 5, 255}) {
        BOOST_CHECK(!chain::IsKnown(static_cast<chain::PollStatus>(value)));
        BOOST_CHECK_EQUAL(chain::ToString(static_cast<chain::PollStatus>(value)), "UNKNOWN");
    }
}

BOOST_AUTO_TEST_CASE(poll_reply_fields_follow_the_status)
{
    const chain::PollReply facts{chain::PollReply::Facts(GoodBatch())};
    BOOST_CHECK(facts.Status() == chain::PollStatus::FACTS);
    BOOST_REQUIRE(facts.Batch() != nullptr);
    BOOST_CHECK(*facts.Batch() == GoodBatch());
    BOOST_CHECK(facts.Authority() == GoodBatch().authority);
    BOOST_CHECK(!facts.RewindTo().has_value());

    const chain::PollReply busy{chain::PollReply::Busy(Authority(4))};
    BOOST_CHECK(busy.Status() == chain::PollStatus::BUSY);
    BOOST_CHECK(busy.Batch() == nullptr);
    BOOST_CHECK(!busy.RewindTo().has_value());
    BOOST_CHECK(busy.Authority() == Authority(4));

    const chain::PollReply need{chain::PollReply::NeedData(Authority(5))};
    BOOST_CHECK(need.Status() == chain::PollStatus::NEED_DATA);
    BOOST_CHECK(need.Batch() == nullptr);
    BOOST_CHECK(!need.RewindTo().has_value());
    BOOST_CHECK(need.Authority() == Authority(5));

    const chain::PollReply changed{chain::PollReply::AuthorityChanged(Authority(6), FactCursor{128, 0})};
    BOOST_CHECK(changed.Status() == chain::PollStatus::AUTHORITY_CHANGED);
    BOOST_CHECK(changed.Batch() == nullptr);
    BOOST_REQUIRE(changed.RewindTo().has_value());
    BOOST_CHECK(*changed.RewindTo() == (FactCursor{128, 0}));
    BOOST_CHECK(changed.Authority() == Authority(6));
}

BOOST_AUTO_TEST_CASE(check_batch_accepts_complete_batches)
{
    CheckBatchAccepted(GoodBatch(), FactCursor{130, 0}, 3, "three facts");
    CheckBatchAccepted(GoodBatch(), FactCursor{130, 0}, MAX_SLOT_FACTS, "cap");

    chain::FactBatch empty{GoodBatch()};
    empty.facts.clear();
    empty.to = empty.from;
    CheckBatchAccepted(empty, FactCursor{130, 0}, 1, "empty heartbeat batch");

    // A batch cut after the first fact of a block ends right after it.
    chain::FactBatch cut{GoodBatch()};
    cut.facts.resize(1);
    cut.to = FactCursor{130, 1};
    CheckBatchAccepted(cut, FactCursor{130, 0}, 1, "truncated batch");

    // The highest anchor height does not overflow the bound.
    chain::FactBatch top{empty};
    top.anchor.height = std::numeric_limits<uint32_t>::max();
    top.from = top.to = FactCursor{std::numeric_limits<uint32_t>::max(), 7};
    CheckBatchAccepted(top, top.from, 1, "anchor at the highest height");
}

BOOST_AUTO_TEST_CASE(check_batch_refuses_every_bad_shape)
{
    const auto expect = [](const chain::FactBatch& batch, size_t max, ErrorCode code, std::string_view detail, std::string_view what) {
        RequireRefusal(chain::CheckBatch(batch, FactCursor{130, 0}, max), code, detail, what);
    };
    {
        chain::FactBatch b{GoodBatch()};
        b.from = FactCursor{130, 1};
        expect(b, 3, ErrorCode::INVALID_INPUT, chain::BATCH_FROM, "from != after");
    }
    {
        chain::FactBatch b{GoodBatch()};
        b.facts.clear();
        b.to = FactCursor{129, 9};
        expect(b, 3, ErrorCode::INVALID_INPUT, chain::BATCH_REVERSED, "to before from");
    }
    {
        chain::FactBatch b{GoodBatch()};
        b.to = FactCursor{131, 1};
        expect(b, 3, ErrorCode::INVALID_INPUT, chain::BATCH_BEYOND_ANCHOR, "to past (anchor + 1, 0)");
        b.to = FactCursor{131, 0};
        b.anchor = TestAnchor(129);
        expect(b, 3, ErrorCode::INVALID_INPUT, chain::BATCH_BEYOND_ANCHOR, "anchor below the facts");
    }
    expect(GoodBatch(), 2, ErrorCode::INVALID_INPUT, chain::BATCH_TOO_MANY, "more than max_facts");
    {
        chain::FactBatch b{GoodBatch()};
        b.to = FactCursor{131 + MAX_SLOT_FACTS, 0};
        b.anchor = TestAnchor(130 + MAX_SLOT_FACTS);
        b.facts.clear();
        for (uint32_t i{0}; i <= MAX_SLOT_FACTS; ++i) b.facts.push_back(DepositAt({130 + i, 0}, "cap/" + std::to_string(i)));
        expect(b, MAX_SLOT_FACTS + 1, ErrorCode::INVALID_INPUT, chain::BATCH_TOO_MANY, "more than MAX_SLOT_FACTS");
    }
    {
        chain::FactBatch b{GoodBatch()};
        b.facts[0] = DepositAt({129, 5}, "t0");
        expect(b, 3, ErrorCode::INVALID_INPUT, chain::BATCH_CURSOR, "fact below from");
        b = GoodBatch();
        b.facts[2] = DepositAt({131, 0}, "t2");
        expect(b, 3, ErrorCode::INVALID_INPUT, chain::BATCH_CURSOR, "fact at to");
        b = GoodBatch();
        std::swap(b.facts[0], b.facts[1]);
        expect(b, 3, ErrorCode::INVALID_INPUT, chain::BATCH_CURSOR, "facts out of order");
        b = GoodBatch();
        b.facts[1] = DepositAt({130, 0}, "t1");
        expect(b, 3, ErrorCode::INVALID_INPUT, chain::BATCH_CURSOR, "repeated cursor");
    }
    {
        chain::FactBatch b{GoodBatch()};
        b.facts[1] = DepositAt({130, 1}, "t1", 0);
        expect(b, 3, ErrorCode::INVALID_INPUT, chain::BATCH_DEPOSIT_ZERO, "zero deposit (O-34)");
        b.facts[1] = DepositAt({130, 1}, "t1", MAX_ATOMS + 1);
        RequireCode(chain::CheckBatch(b, FactCursor{130, 0}, 3), ErrorCode::INVALID_INPUT, "amount outside the atom range");
    }
    for (const AccountId& protocol : {SeatPoolAccount(Authority().profile_hash), TreasuryAccount(Authority().profile_hash)}) {
        chain::FactBatch b{GoodBatch()};
        std::get<DepositFact>(b.facts[2]).account = protocol;
        expect(b, 3, ErrorCode::INVALID_INPUT, chain::BATCH_DEPOSIT_PROTOCOL, "deposit to a protocol account (O-34)");
    }
    {
        chain::FactBatch b{GoodBatch()};
        b.facts[2] = DepositAt({130, 2}, "t0");
        expect(b, 3, ErrorCode::INVALID_INPUT, chain::BATCH_DUPLICATE_OUTPOINT, "one outpoint twice (O-34)");
    }
    {
        // A stub fact kind cannot be delivered before its task defines it.
        chain::FactBatch b{GoodBatch()};
        b.facts[1] = SeatChangeFact{FactCursor{130, 1}};
        RequireStub(chain::CheckBatch(b, FactCursor{130, 0}, 3), "M7-SEAT-01");
        b.facts[1] = ListingFact{FactCursor{130, 1}};
        RequireStub(chain::CheckBatch(b, FactCursor{130, 0}, 3), "M7-GOV-01");
    }
}

BOOST_AUTO_TEST_CASE(check_authority_compares_the_whole_version)
{
    StaticAdapter adapter;
    adapter.authority = Authority(3);
    BOOST_CHECK(adapter.CheckAuthority(Authority(3)).has_value());
    RequireRefusal(adapter.CheckAuthority(Authority(2)), ErrorCode::AUTHORITY_CHANGED, chain::CHAIN_AUTHORITY_CHANGED, "older epoch");
    RequireRefusal(adapter.CheckAuthority(Authority(4)), ErrorCode::AUTHORITY_CHANGED, chain::CHAIN_AUTHORITY_CHANGED, "newer epoch");
    AuthorityVersion other_seats{Authority(3)};
    other_seats.seat_set_root = LabelHash("fm2/test/chain/other-seats");
    RequireRefusal(adapter.CheckAuthority(other_seats), ErrorCode::AUTHORITY_CHANGED, chain::CHAIN_AUTHORITY_CHANGED, "other seat set");
    AuthorityVersion other_profile{Authority(3)};
    other_profile.profile_hash = LabelHash("fm2/test/chain/other-profile");
    RequireRefusal(adapter.CheckAuthority(other_profile), ErrorCode::AUTHORITY_CHANGED, chain::CHAIN_AUTHORITY_CHANGED, "other profile");
}

BOOST_AUTO_TEST_CASE(b3d_trusted_facts_adapter_refuses_real_networks)
{
    for (const ChainType chain_type : {ChainType::MAIN, ChainType::TESTNET, ChainType::TESTNET4, ChainType::SIGNET}) {
        NetworkId network{TestNetwork()};
        network.chain = chain_type;
        RequireCode(chain::B3dTrustedFactsAdapter::Create(network, chain::B3dAdapterConfig{}), ErrorCode::UNSUPPORTED_NETWORK,
                    "trusted facts on a real network");
    }
    RequireStub(chain::B3dTrustedFactsAdapter::Create(TestNetwork(), chain::B3dAdapterConfig{}), "MB1-ADP-06");
}

BOOST_AUTO_TEST_CASE(b3d_finalized_adapter_is_a_stub)
{
    for (const ChainType chain_type : {ChainType::MAIN, ChainType::REGTEST}) {
        NetworkId network{TestNetwork()};
        network.chain = chain_type;
        RequireStub(chain::B3dFinalizedAdapter::Create(network, chain::B3dAdapterConfig{30}), "M7-ADP-01");
    }
}

BOOST_AUTO_TEST_SUITE_END()
