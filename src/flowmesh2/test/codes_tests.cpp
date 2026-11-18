// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// <cmath> first on purpose: see result_tests.cpp (macOS OVERFLOW/DOMAIN macros).
#include <cmath>

#include <boost/test/unit_test.hpp>

#include <flowmesh2/core/codes.h>

#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

using namespace flowmesh2;

namespace {

using Pin = std::tuple<int, std::string_view, bool>;

//! Append-only pin of every OutcomeCode: integer, name, rejection flag. These
//! values are certified under outcomes_root. A new code is appended here in the
//! same commit; renumbering or renaming fails this test.
const std::vector<Pin> PINNED_OUTCOME_CODES{
    {1, "DEPOSIT_CREDITED", false},
    {2, "RESTING", false},
    {3, "FILLED", false},
    {4, "PARTIAL_RESTING", false},
    {5, "IOC_DONE", false},
    {6, "CANCELLED", false},
    {7, "TRANSFERRED", false},
    {8, "FEES_DISTRIBUTED", false},
    {9, "WITHDRAWAL_PENDING", false},
    {10, "WITHDRAWAL_PAID", false},
    {64, "REJ_INSUFFICIENT_AVAILABLE", true},
    {65, "REJ_UNKNOWN_MARKET", true},
    {66, "REJ_UNKNOWN_ORDER", true},
    {67, "REJ_NOT_OWN_ORDER", true},
    {68, "REJ_ORDER_CLOSED", true},
    {69, "REJ_WOULD_CROSS", true},
    {70, "REJ_BOOK_FULL", true},
    {71, "REJ_FILL_CAP", true},
    {72, "REJ_PRICE_BAND", true},
    {73, "REJ_OPEN_ORDER_CAP", true},
    {74, "REJ_BAD_PARAMS", true},
    {75, "REJ_UNKNOWN_ASSET", true},
    {76, "REJ_NO_RECIPIENT", true},
    {96, "REJ_RESIDUAL_CROSS_UNSUPPORTED", true},
};

//! Append-only pin of every AdmissionCode (not certified, but a client API).
const std::vector<Pin> PINNED_ADMISSION_CODES{
    {1, "ADMITTED", false},
    {2, "DUPLICATE", false},
    {16, "MALFORMED", true},
    {17, "OVERSIZE", true},
    {18, "WRONG_DOMAIN", true},
    {19, "UNKNOWN_KIND", true},
    {20, "UNSUPPORTED_KIND", true},
    {21, "NO_ACCOUNT", true},
    {22, "EXPIRED", true},
    {23, "REF_SLOT_AHEAD", true},
    {24, "BAD_EXPIRY", true},
    {25, "NONCE_TOO_LOW", true},
    {26, "NONCE_USED", true},
    {27, "NONCE_PENDING", true},
    {28, "BUDGET", true},
    {29, "BAD_SIG", true},
    {30, "BUSY", true},
    {31, "POOL_FULL", true},
    {32, "HALTED", true},
};

template <class Code>
void CheckPinned(std::span<const CodeInfo<Code>> registry, const std::vector<Pin>& pins)
{
    BOOST_REQUIRE_EQUAL(registry.size(), pins.size());
    std::set<std::string_view> names;
    for (size_t i{0}; i < registry.size(); ++i) {
        const auto& [value, name, rejection] = pins[i];
        BOOST_CHECK_EQUAL(static_cast<int>(registry[i].code), value);
        BOOST_CHECK_EQUAL(std::string{registry[i].name}, std::string{name});
        BOOST_CHECK_EQUAL(registry[i].rejection, rejection);
        BOOST_CHECK(IsKnown(registry[i].code));
        BOOST_CHECK_EQUAL(std::string{ToString(registry[i].code)}, std::string{name});
        BOOST_CHECK_EQUAL(IsRejection(registry[i].code), rejection);
        BOOST_CHECK(names.insert(registry[i].name).second);
    }
}

template <class Code>
void CheckTotal(const std::vector<Pin>& pins)
{
    for (uint32_t raw{0}; raw <= 0xFFFF; ++raw) {
        const auto code{static_cast<Code>(raw)};
        const Pin* pin{nullptr};
        for (const auto& candidate : pins) {
            if (static_cast<uint32_t>(std::get<0>(candidate)) == raw) pin = &candidate;
        }
        const std::string_view expected{pin ? std::get<1>(*pin) : std::string_view{"UNKNOWN"}};
        const bool expected_rejection{pin ? std::get<2>(*pin) : false};
        if (ToString(code) != expected || IsKnown(code) != (pin != nullptr) || IsRejection(code) != expected_rejection) {
            BOOST_ERROR("code " << raw << " maps to " << std::string{ToString(code)});
        }
    }
}

} // namespace

BOOST_AUTO_TEST_SUITE(codes_tests)

BOOST_AUTO_TEST_CASE(outcome_codes_pinned)
{
    CheckPinned<OutcomeCode>(OutcomeCodeRegistry(), PINNED_OUTCOME_CODES);
}

BOOST_AUTO_TEST_CASE(outcome_to_string_total)
{
    CheckTotal<OutcomeCode>(PINNED_OUTCOME_CODES);
    BOOST_CHECK_EQUAL(std::string{ToString(static_cast<OutcomeCode>(0))}, "UNKNOWN");
    BOOST_CHECK(!IsRejection(static_cast<OutcomeCode>(0)));
    BOOST_CHECK(!IsKnown(static_cast<OutcomeCode>(77)));
}

BOOST_AUTO_TEST_CASE(admission_codes_pinned)
{
    CheckPinned<AdmissionCode>(AdmissionCodeRegistry(), PINNED_ADMISSION_CODES);
}

BOOST_AUTO_TEST_CASE(admission_to_string_total)
{
    CheckTotal<AdmissionCode>(PINNED_ADMISSION_CODES);
    BOOST_CHECK_EQUAL(std::string{ToString(static_cast<AdmissionCode>(0))}, "UNKNOWN");
    BOOST_CHECK(!IsRejection(static_cast<AdmissionCode>(0)));
}

BOOST_AUTO_TEST_CASE(duplicate_and_admitted_are_not_rejections)
{
    // DUPLICATE means the original is pending or included; it is not a refusal
    // of the action, and it never verifies a signature (ARCHITECTURE.md 10.1).
    BOOST_CHECK(!IsRejection(AdmissionCode::ADMITTED));
    BOOST_CHECK(!IsRejection(AdmissionCode::DUPLICATE));
    BOOST_CHECK(IsRejection(AdmissionCode::BAD_SIG));
    BOOST_CHECK(IsRejection(AdmissionCode::UNSUPPORTED_KIND));
    BOOST_CHECK(IsRejection(OutcomeCode::REJ_RESIDUAL_CROSS_UNSUPPORTED));
    BOOST_CHECK(!IsRejection(OutcomeCode::FEES_DISTRIBUTED));
}

BOOST_AUTO_TEST_SUITE_END()
