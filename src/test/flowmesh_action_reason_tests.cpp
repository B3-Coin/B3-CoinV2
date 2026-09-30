// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <flowmesh/action_reason.h>

#include <boost/test/unit_test.hpp>

#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

BOOST_AUTO_TEST_SUITE(flowmesh_action_reason_tests)

BOOST_AUTO_TEST_CASE(reason_codes_are_unique_stable_and_append_only)
{
    // Golden number<->name table. A changed number or name breaks every
    // client that stored or displayed it: append only, never edit a row.
    const std::vector<std::pair<unsigned, std::string>> golden{
        {1, "ADMITTED"}, {2, "DUPLICATE_PENDING"}, {3, "MALFORMED_SHAPE"},
        {4, "UNAUTHENTICATED"}, {5, "DUPLICATE_ID_OTHER_CREDENTIAL"},
        {6, "SEQUENCE_CONFLICT"}, {7, "DEPOSIT_CONFLICT"}, {8, "POOL_FULL"},
        {9, "PEER_QUOTA"}, {10, "STALE_SEQUENCE"}, {11, "FAR_FUTURE_SEQUENCE"},
        {12, "HEADER_NOT_CURRENT"}, {13, "HEAD_INTERVAL_UNAVAILABLE"},
        {14, "ALREADY_INCLUDED"}, {15, "AUTHORITY_UNAVAILABLE"},
        {16, "DEPOSIT_FACTS_UNAVAILABLE"}, {17, "DEPOSIT_ALREADY_CREDITED"},
        {18, "QUEUE_UNAVAILABLE"}, {19, "STALE_SEQUENCE_UNATTRIBUTED"},
        {20, "SUBMIT_LANE_FULL"}, {21, "QUEUE_RATE_LIMITED"},
        {32, "DROPPED_SEQUENCE_CONSUMED"},
        {64, "APPLIED"}, {65, "REJECTED_BY_STATE"}, {66, "INCLUDED_BAD_SEQUENCE"},
        {67, "INCLUDED_EQUIVOCATION"}, {68, "INCLUDED_MALFORMED"},
        {69, "DEPOSIT_NOT_CREDITED"}, {96, "NOT_EXECUTED_SEQUENCE_CONSUMED"},
        {112, "OUTCOME_EVIDENCE_EXPIRED"}, {113, "OUTCOME_EVIDENCE_TOO_LARGE"},
        {114, "OUTCOME_EVIDENCE_INVALID"}, {115, "OUTCOME_NOT_OFFERED"},
        {116, "OUTCOME_EVIDENCE_BUDGET"},
    };
    BOOST_REQUIRE_EQUAL(std::size(flowmesh::ACTION_REASONS), golden.size());
    std::set<unsigned> numbers;
    std::set<std::string> names;
    for (size_t i{0}; i < golden.size(); ++i) {
        const auto& info{flowmesh::ACTION_REASONS[i]};
        BOOST_CHECK_EQUAL(static_cast<unsigned>(info.code), golden[i].first);
        BOOST_CHECK_EQUAL(std::string{info.name}, golden[i].second);
        BOOST_CHECK(numbers.insert(static_cast<unsigned>(info.code)).second);
        BOOST_CHECK(names.insert(std::string{info.name}).second);
        const auto* by_code{flowmesh::ActionReasonLookup(info.code)};
        const auto* by_name{flowmesh::ActionReasonLookup(info.name)};
        BOOST_CHECK(by_code == &info);
        BOOST_CHECK(by_name == &info);
        BOOST_CHECK_EQUAL(std::string{flowmesh::ActionReasonName(info.code)}, golden[i].second);
    }
    // Reserved numbers are never registered, so they cannot be emitted.
    for (const unsigned reserved : {0U, 33U, 34U, 35U}) {
        BOOST_CHECK(!flowmesh::ActionReasonLookup(static_cast<flowmesh::ActionReasonCode>(reserved)));
    }
    BOOST_CHECK(flowmesh::ActionReasonName(flowmesh::ActionReasonCode::NONE).empty());
}

BOOST_AUTO_TEST_CASE(every_code_has_bounded_ui_text_and_class)
{
    for (const auto& info : flowmesh::ACTION_REASONS) {
        BOOST_CHECK(!info.ui_text.empty());
        BOOST_CHECK_LE(info.ui_text.size(), 160U);
        // An outcome text never asks for a resend: no client path offers
        // one for an included action (G5, an exact resend, is not built).
        if (info.cls == flowmesh::ActionReasonClass::OUTCOME_FINAL ||
            info.cls == flowmesh::ActionReasonClass::OUTCOME_NOT_FINAL) {
            BOOST_CHECK_MESSAGE(info.ui_text.find("resend") == std::string_view::npos, info.name);
        }
        BOOST_CHECK_LE(info.name.size(), 40U);
        for (const char c : info.name) BOOST_CHECK((c >= 'A' && c <= 'Z') || c == '_');
        const auto number{static_cast<unsigned>(info.code)};
        const auto expected{number < 32 ? flowmesh::ActionReasonClass::OBSERVATION
                            : number < 64 ? flowmesh::ActionReasonClass::DROP
                            : number < 96 ? info.cls
                            : number < 112 ? flowmesh::ActionReasonClass::PROVEN_TERMINAL
                                           : flowmesh::ActionReasonClass::EVIDENCE};
        BOOST_CHECK(info.cls == expected);
        if (number >= 64 && number < 96) {
            BOOST_CHECK(info.cls == flowmesh::ActionReasonClass::OUTCOME_FINAL ||
                        info.cls == flowmesh::ActionReasonClass::OUTCOME_NOT_FINAL);
        }
    }
    using flowmesh::ActionReasonClass;
    using flowmesh::ActionReasonCode;
    const auto cls = [](ActionReasonCode code) { return flowmesh::ActionReasonLookup(code)->cls; };
    BOOST_CHECK(cls(ActionReasonCode::APPLIED) == ActionReasonClass::OUTCOME_FINAL);
    BOOST_CHECK(cls(ActionReasonCode::REJECTED_BY_STATE) == ActionReasonClass::OUTCOME_FINAL);
    BOOST_CHECK(cls(ActionReasonCode::INCLUDED_MALFORMED) == ActionReasonClass::OUTCOME_FINAL);
    BOOST_CHECK(cls(ActionReasonCode::INCLUDED_BAD_SEQUENCE) == ActionReasonClass::OUTCOME_NOT_FINAL);
    BOOST_CHECK(cls(ActionReasonCode::INCLUDED_EQUIVOCATION) == ActionReasonClass::OUTCOME_NOT_FINAL);
    BOOST_CHECK(cls(ActionReasonCode::DEPOSIT_NOT_CREDITED) == ActionReasonClass::OUTCOME_NOT_FINAL);
}

BOOST_AUTO_TEST_CASE(rejected_by_state_text_covers_every_action_type)
{
    std::set<std::string> texts;
    for (unsigned type{0}; type <= 8; ++type) {
        const auto text{flowmesh::RejectedByStateText(static_cast<flowmesh::ActionType>(type))};
        BOOST_CHECK(!text.empty());
        BOOST_CHECK_LE(text.size(), 160U);
        texts.emplace(text);
    }
    // Orders, cancels, withdrawals, reward claims, deposits and reserved
    // types each get their own type-level sentence.
    BOOST_CHECK_EQUAL(texts.size(), 6U);
}

BOOST_AUTO_TEST_CASE(unknown_names_are_not_terminal)
{
    for (const char* name : {"", "NONE", "applied", "APPLIED_LATER", "DROPPED_EXPIRED", "DROPPED_EVICTED",
                             "DROPPED_DEPOSIT_CREDITED", "ALREADY_CERTIFIED"}) {
        BOOST_CHECK(!flowmesh::ActionReasonLookup(std::string_view{name}));
        BOOST_CHECK(!flowmesh::ActionReasonIsTerminal(name));
    }
    size_t terminal{0};
    for (const auto& info : flowmesh::ACTION_REASONS) {
        const bool is_terminal{flowmesh::ActionReasonIsTerminal(info.name)};
        terminal += is_terminal;
        BOOST_CHECK_EQUAL(is_terminal, info.cls == flowmesh::ActionReasonClass::OUTCOME_FINAL ||
                                           info.cls == flowmesh::ActionReasonClass::PROVEN_TERMINAL);
    }
    // APPLIED, REJECTED_BY_STATE, INCLUDED_MALFORMED and NOT_EXECUTED only.
    BOOST_CHECK_EQUAL(terminal, 4U);
    BOOST_CHECK(!flowmesh::ActionReasonIsTerminal("STALE_SEQUENCE"));
    BOOST_CHECK(!flowmesh::ActionReasonIsTerminal("DROPPED_SEQUENCE_CONSUMED"));
    BOOST_CHECK(!flowmesh::ActionReasonIsTerminal("OUTCOME_EVIDENCE_EXPIRED"));
}

BOOST_AUTO_TEST_SUITE_END()
