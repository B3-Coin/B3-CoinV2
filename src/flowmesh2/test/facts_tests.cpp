// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <crypto/hex_base.h>
#include <flowmesh2/core/body.h>
#include <flowmesh2/core/facts.h>
#include <flowmesh2/core/hashes.h>
#include <flowmesh2/core/merkle.h>
#include <flowmesh2/test/util/keys.h>

#include <functional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::test;

namespace {

template <class T>
void CheckError(const Result<T>& result, ErrorCode expected)
{
    BOOST_REQUIRE_MESSAGE(!result.has_value(), "unexpected success");
    BOOST_CHECK_EQUAL(ToString(result.error().code), ToString(expected));
}

//! The golden deposit of evidence/R4/golden.py.
DepositFact Sample()
{
    return DepositFact{FactCursor{130, 0}, Outpoint{LabelHash("fm2/test/txid"), 1},
                       AccountId{LabelHash("fm2/test/alice-account")}, AssetId{LabelHash("fm2/test/rusd")},
                       1'000'000'000};
}

void Flip(uint256& value) { value.data()[0] ^= 0x01; }

uint256 IdOf(const Fact& fact) { return FactId(fact).value(); }

} // namespace

BOOST_AUTO_TEST_SUITE(facts_tests)

BOOST_AUTO_TEST_CASE(kind_registry_pinned)
{
    const std::vector<std::string> names{"UNKNOWN", "DEPOSIT", "SEAT_CHANGE", "LISTING", "CHECKPOINT_ACCEPTED",
                                         "PAYOUT_CONFIRMED", "EXIT_NULLIFIER", "SOURCE_RANGE", "UNKNOWN"};
    for (int v{0}; v <= 8; ++v) {
        BOOST_CHECK_EQUAL(std::string{ToString(static_cast<FactKind>(v))}, names[v]);
        BOOST_CHECK_EQUAL(IsKnown(static_cast<FactKind>(v)), v >= 1 && v <= 7);
    }
    const FactCursor at{7, 3};
    const std::vector<std::pair<Fact, FactKind>> kinds{
        {DepositFact{at, {}, {}, {}, 0}, FactKind::DEPOSIT},
        {SeatChangeFact{at}, FactKind::SEAT_CHANGE},
        {ListingFact{at}, FactKind::LISTING},
        {CheckpointAcceptedFact{at}, FactKind::CHECKPOINT_ACCEPTED},
        {PayoutConfirmedFact{at}, FactKind::PAYOUT_CONFIRMED},
        {ExitNullifierFact{at}, FactKind::EXIT_NULLIFIER},
        {SourceRangeFact{at}, FactKind::SOURCE_RANGE},
    };
    BOOST_CHECK_EQUAL(kinds.size(), std::variant_size_v<Fact>);
    for (const auto& [fact, kind] : kinds) {
        BOOST_CHECK(KindOf(fact) == kind);
        BOOST_CHECK(CursorOf(fact) == at);
    }
}

BOOST_AUTO_TEST_CASE(deposit_golden_bytes_and_fact_id)
{
    // Goldens from evidence/R4/golden.py (independent of the C++ code).
    const auto bytes{EncodeFact(Sample()).value()};
    BOOST_CHECK_EQUAL(bytes.size(), 117U);
    BOOST_CHECK_EQUAL(HexStr(bytes),
                      "018200000000000000948a2cd734d5964d468accc8a7ec762fa809854baa098a7bd2b95581eb8dc3110100000057ce"
                      "18f5adc74afd420707bebef578bfc681e003a6bfea041163200e6dd7483b351cc7086ec37dd5bebb5775297a065b4f78"
                      "5b3bac0375eed91baf048aa96e8500ca9a3b00000000");
    BOOST_CHECK_EQUAL(HexStr(IdOf(Sample())), "453e473591170fd810116318cca9bf63ee104c44f68dfffef19d81720604db38");
    BOOST_CHECK(IdOf(Sample()) == Tagged(HashTag::FACT, bytes));
    // The variant and the typed encoder agree.
    BOOST_CHECK(EncodeFact(Fact{Sample()}).value() == bytes);
}

BOOST_AUTO_TEST_CASE(deposit_round_trip_and_refusals)
{
    const auto bytes{EncodeFact(Sample()).value()};
    const auto decoded{DecodeFact(bytes)};
    BOOST_REQUIRE(decoded.has_value());
    BOOST_REQUIRE(std::holds_alternative<DepositFact>(*decoded));
    BOOST_CHECK(std::get<DepositFact>(*decoded) == Sample());

    for (const unsigned char kind : {0x00, 0x08, 0xff}) {
        auto bad{bytes};
        bad[0] = kind;
        CheckError(DecodeFact(bad), ErrorCode::DECODE);
    }
    CheckError(DecodeFact({}), ErrorCode::DECODE);
    auto trailing{bytes};
    trailing.push_back(0x00);
    CheckError(DecodeFact(trailing), ErrorCode::OVERSIZE); // longer than a deposit
    const std::vector<unsigned char> truncated(bytes.begin(), bytes.end() - 1);
    CheckError(DecodeFact(truncated), ErrorCode::DECODE);
    CheckError(DecodeFact(std::vector<unsigned char>(MAX_FACT_BYTES + 1, 0x01)), ErrorCode::OVERSIZE);

    // Amount outside [0, MAX_ATOMS]: the decoder and the encoder refuse it.
    for (const Atoms bad_amount : {Atoms{-1}, MAX_ATOMS + 1}) {
        DepositFact d{Sample()};
        d.amount = bad_amount;
        CheckError(EncodeFact(d), ErrorCode::INVALID_INPUT);
        auto bad{bytes};
        for (size_t i{0}; i < 8; ++i) bad[109 + i] = static_cast<unsigned char>(static_cast<uint64_t>(bad_amount) >> (8 * i));
        CheckError(DecodeFact(bad), ErrorCode::DECODE);
    }
    DepositFact max{Sample()};
    max.amount = MAX_ATOMS;
    BOOST_CHECK(std::get<DepositFact>(DecodeFact(EncodeFact(max).value()).value()) == max);
}

BOOST_AUTO_TEST_CASE(fact_id_binds_the_outpoint_and_every_field)
{
    const std::vector<std::pair<std::string, std::function<void(DepositFact&)>>> mutations{
        {"cursor.height", [](DepositFact& d) { ++d.cursor.height; }},
        {"cursor.index", [](DepositFact& d) { ++d.cursor.index; }},
        {"outpoint.txid", [](DepositFact& d) { Flip(d.outpoint.txid); }},
        {"outpoint.vout", [](DepositFact& d) { ++d.outpoint.vout; }},
        {"account", [](DepositFact& d) { Flip(d.account.id); }},
        {"asset", [](DepositFact& d) { Flip(d.asset.id); }},
        {"amount", [](DepositFact& d) { ++d.amount; }},
    };
    const DepositFact base{Sample()};
    const auto& [f1, f2, f3, f4, f5] = base; // 5 fields; cursor and outpoint have two members each
    (void)f1, (void)f2, (void)f3, (void)f4, (void)f5;
    std::set<uint256> ids{IdOf(base)};
    for (const auto& [name, mutate] : mutations) {
        DepositFact changed{base};
        mutate(changed);
        BOOST_CHECK_MESSAGE(ids.insert(IdOf(changed)).second, name << ": FactId unchanged or repeated");
    }
}

BOOST_AUTO_TEST_CASE(body_facts_root_is_the_merkle_root_of_fact_ids)
{
    DepositFact second{Sample()};
    second.cursor.index = 1;
    second.outpoint.vout = 2;
    SlotBody body;
    body.facts = {EncodeFact(Sample()).value(), EncodeFact(second).value()};
    const std::vector<uint256> ids{IdOf(Sample()), IdOf(second)};
    BOOST_CHECK(body.FactsRoot() == MerkleRoot(ids));
    BOOST_CHECK(FactLeaf(body.facts[0]) == ids[0]);

    SlotBody one;
    one.facts = {EncodeFact(Sample()).value()};
    BOOST_CHECK_EQUAL(HexStr(one.FactsRoot()), "ac5ed96845ad3c8bf209dffe0d23a204d5cd58e401ccc9cf746d93aae63786c3");
}

BOOST_AUTO_TEST_CASE(stub_facts_never_encode)
{
    // Every codec entry of a stub kind returns that kind's task ID; the
    // stub_inventory cases pin the same IDs.
    const FactCursor at{1, 0};
    const std::vector<std::pair<Fact, std::string>> stubs{
        {SeatChangeFact{at}, "M7-SEAT-01"},       {ListingFact{at}, "M7-GOV-01"},
        {CheckpointAcceptedFact{at}, "MB1-B3D-02"},
        {ExitNullifierFact{at}, "M8-EXIT-01"},
    };
    for (const auto& [fact, id] : stubs) {
        const auto encoded{EncodeFact(fact)};
        BOOST_REQUIRE(!encoded.has_value());
        BOOST_CHECK(encoded.error().code == ErrorCode::NOT_IMPLEMENTED);
        BOOST_CHECK_EQUAL(std::string{encoded.error().detail}, id);
        const auto fact_id{FactId(fact)};
        BOOST_REQUIRE(!fact_id.has_value());
        BOOST_CHECK_EQUAL(std::string{fact_id.error().detail}, id);
        const std::vector<unsigned char> kind_only{static_cast<unsigned char>(KindOf(fact))};
        const auto decoded{DecodeFact(kind_only)};
        BOOST_REQUIRE(!decoded.has_value());
        BOOST_CHECK(decoded.error().code == ErrorCode::NOT_IMPLEMENTED);
        BOOST_CHECK_EQUAL(std::string{decoded.error().detail}, id);
    }
}

BOOST_AUTO_TEST_CASE(payout_confirmation_roundtrip_is_exact_and_binds_every_field)
{
    PayoutConfirmedFact fact;
    fact.cursor = FactCursor{131, 2};
    fact.withdrawal = ActionId{LabelHash("withdrawal")};
    fact.account = AccountId{LabelHash("withdrawer")};
    fact.asset = AssetId{LabelHash("synthetic-token")};
    fact.amount = 1234;
    fact.recipient.fill(0x42);
    fact.payout_event = Outpoint{LabelHash("payout-tx"), 3};
    fact.domain = LabelHash("custody-domain");
    const auto bytes{EncodeFact(fact).value()};
    BOOST_CHECK_EQUAL(bytes.size(), 201U);
    BOOST_CHECK(std::get<PayoutConfirmedFact>(DecodeFact(bytes).value()) == fact);
    BOOST_CHECK(FactLeaf(bytes) == FactId(fact).value());
    auto short_bytes{bytes};
    short_bytes.pop_back();
    CheckError(DecodeFact(short_bytes), ErrorCode::DECODE);
    auto trailing{bytes};
    trailing.push_back(0);
    CheckError(DecodeFact(trailing), ErrorCode::OVERSIZE);
    const std::vector<std::function<void(PayoutConfirmedFact&)>> mutations{
        [](auto& f) { ++f.cursor.height; }, [](auto& f) { ++f.cursor.index; },
        [](auto& f) { Flip(f.withdrawal.id); }, [](auto& f) { Flip(f.account.id); },
        [](auto& f) { Flip(f.asset.id); }, [](auto& f) { ++f.amount; },
        [](auto& f) { f.recipient[0] ^= 1; }, [](auto& f) { Flip(f.payout_event.txid); },
        [](auto& f) { ++f.payout_event.vout; }, [](auto& f) { Flip(f.domain); }};
    std::set<uint256> ids{FactId(fact).value()};
    for (const auto& mutate : mutations) {
        auto changed{fact};
        mutate(changed);
        BOOST_CHECK(ids.insert(FactId(changed).value()).second);
    }
    for (int field{0}; field < 6; ++field) {
        auto bad{fact};
        if (field == 0) bad.amount = 0;
        if (field == 1) bad.amount = -1;
        if (field == 2) bad.recipient.fill(0);
        if (field == 3) bad.withdrawal = ActionId{};
        if (field == 4) bad.payout_event = Outpoint{};
        if (field == 5) bad.domain = uint256{};
        CheckError(EncodeFact(bad), ErrorCode::INVALID_INPUT);
    }
}

BOOST_AUTO_TEST_CASE(source_range_roundtrip_commits_every_boundary)
{
    SourceRangeFact source{FactCursor{130, 3}, LabelHash("source/domain"), 31337, 10,
                           LabelHash("source/previous"), 11, LabelHash("source/to"), 2};
    const auto bytes{EncodeFact(source).value()};
    BOOST_CHECK_EQUAL(bytes.size(), 133U);
    BOOST_CHECK_EQUAL(bytes.front(), 7U);
    BOOST_CHECK(std::get<SourceRangeFact>(DecodeFact(bytes).value()) == source);
    BOOST_CHECK(FactId(Fact{source}).value() == FactLeaf(bytes));
    BOOST_CHECK_EQUAL(bytes[41], 0x69U); // chain31337 encoded little-endian at its fixed offset
    BOOST_CHECK_EQUAL(bytes[42], 0x7aU);
    const std::vector<std::function<void(SourceRangeFact&)>> mutations{
        [](auto& f) { ++f.cursor.height; }, [](auto& f) { ++f.cursor.index; },
        [](auto& f) { Flip(f.domain); }, [](auto& f) { ++f.chain_id; },
        [](auto& f) { --f.previous_height; }, [](auto& f) { Flip(f.previous_hash); },
        [](auto& f) { ++f.to_height; }, [](auto& f) { Flip(f.to_hash); }, [](auto& f) { ++f.confirmations; }};
    std::set<uint256> ids{FactId(Fact{source}).value()};
    for (const auto& mutate : mutations) {
        auto changed{source}; mutate(changed);
        BOOST_CHECK(ids.insert(FactId(Fact{changed}).value()).second);
    }
    auto short_bytes{bytes}; short_bytes.pop_back();
    CheckError(DecodeFact(short_bytes), ErrorCode::DECODE);
    auto trailing{bytes}; trailing.push_back(0);
    CheckError(DecodeFact(trailing), ErrorCode::OVERSIZE);
}

BOOST_AUTO_TEST_CASE(source_range_refuses_ambiguous_or_noncanonical_boundaries)
{
    SourceRangeFact source{FactCursor{130, 0}, LabelHash("source/domain"), 31337, 10,
                           LabelHash("source/previous"), 11, LabelHash("source/to"), 2};
    const std::vector<std::function<void(SourceRangeFact&)>> mutations{
        [](auto& f) { f.domain.SetNull(); }, [](auto& f) { f.chain_id = 0; },
        [](auto& f) { f.previous_hash.SetNull(); }, [](auto& f) { f.to_hash.SetNull(); },
        [](auto& f) { f.to_height = f.previous_height - 1; },
        [](auto& f) { f.to_height = f.previous_height; }, [](auto& f) { f.confirmations = 0; }};
    const auto encoded{EncodeFact(source).value()};
    for (const auto& mutate : mutations) {
        auto changed{source}; mutate(changed);
        BOOST_CHECK(!changed.IsCanonical());
        CheckError(EncodeFact(changed), ErrorCode::INVALID_INPUT);
    }
    // Equal endpoints are canonical only with exactly the same block hash.
    source.to_height = source.previous_height; source.to_hash = source.previous_hash;
    BOOST_CHECK(source.IsCanonical());
    BOOST_CHECK(std::get<SourceRangeFact>(DecodeFact(EncodeFact(source).value()).value()) == source);
    for (const size_t offset : {size_t{9}, size_t{41}, size_t{57}, size_t{97}, size_t{129}}) {
        auto wrong{encoded};
        const size_t count{offset == 41 ? 8U : offset == 129 ? 4U : 32U};
        std::fill(wrong.begin() + offset, wrong.begin() + offset + count, 0);
        CheckError(DecodeFact(wrong), ErrorCode::DECODE);
    }
}
BOOST_AUTO_TEST_SUITE_END()
