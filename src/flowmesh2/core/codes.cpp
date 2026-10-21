// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/core/codes.h>

#include <array>
#include <cstddef>

namespace flowmesh2 {
namespace {

constexpr std::array<CodeInfo<OutcomeCode>, 24> OUTCOME_CODES{{
    {OutcomeCode::DEPOSIT_CREDITED, "DEPOSIT_CREDITED", false},
    {OutcomeCode::RESTING, "RESTING", false},
    {OutcomeCode::FILLED, "FILLED", false},
    {OutcomeCode::PARTIAL_RESTING, "PARTIAL_RESTING", false},
    {OutcomeCode::IOC_DONE, "IOC_DONE", false},
    {OutcomeCode::CANCELLED, "CANCELLED", false},
    {OutcomeCode::TRANSFERRED, "TRANSFERRED", false},
    {OutcomeCode::FEES_DISTRIBUTED, "FEES_DISTRIBUTED", false},
    {OutcomeCode::WITHDRAWAL_PENDING, "WITHDRAWAL_PENDING", false},
    {OutcomeCode::WITHDRAWAL_PAID, "WITHDRAWAL_PAID", false},
    {OutcomeCode::REJ_INSUFFICIENT_AVAILABLE, "REJ_INSUFFICIENT_AVAILABLE", true},
    {OutcomeCode::REJ_UNKNOWN_MARKET, "REJ_UNKNOWN_MARKET", true},
    {OutcomeCode::REJ_UNKNOWN_ORDER, "REJ_UNKNOWN_ORDER", true},
    {OutcomeCode::REJ_NOT_OWN_ORDER, "REJ_NOT_OWN_ORDER", true},
    {OutcomeCode::REJ_ORDER_CLOSED, "REJ_ORDER_CLOSED", true},
    {OutcomeCode::REJ_WOULD_CROSS, "REJ_WOULD_CROSS", true},
    {OutcomeCode::REJ_BOOK_FULL, "REJ_BOOK_FULL", true},
    {OutcomeCode::REJ_FILL_CAP, "REJ_FILL_CAP", true},
    {OutcomeCode::REJ_PRICE_BAND, "REJ_PRICE_BAND", true},
    {OutcomeCode::REJ_OPEN_ORDER_CAP, "REJ_OPEN_ORDER_CAP", true},
    {OutcomeCode::REJ_BAD_PARAMS, "REJ_BAD_PARAMS", true},
    {OutcomeCode::REJ_UNKNOWN_ASSET, "REJ_UNKNOWN_ASSET", true},
    {OutcomeCode::REJ_NO_RECIPIENT, "REJ_NO_RECIPIENT", true},
    {OutcomeCode::REJ_RESIDUAL_CROSS_UNSUPPORTED, "REJ_RESIDUAL_CROSS_UNSUPPORTED", true},
}};

constexpr std::array<CodeInfo<AdmissionCode>, 19> ADMISSION_CODES{{
    {AdmissionCode::ADMITTED, "ADMITTED", false},
    {AdmissionCode::DUPLICATE, "DUPLICATE", false},
    {AdmissionCode::MALFORMED, "MALFORMED", true},
    {AdmissionCode::OVERSIZE, "OVERSIZE", true},
    {AdmissionCode::WRONG_DOMAIN, "WRONG_DOMAIN", true},
    {AdmissionCode::UNKNOWN_KIND, "UNKNOWN_KIND", true},
    {AdmissionCode::UNSUPPORTED_KIND, "UNSUPPORTED_KIND", true},
    {AdmissionCode::NO_ACCOUNT, "NO_ACCOUNT", true},
    {AdmissionCode::EXPIRED, "EXPIRED", true},
    {AdmissionCode::REF_SLOT_AHEAD, "REF_SLOT_AHEAD", true},
    {AdmissionCode::BAD_EXPIRY, "BAD_EXPIRY", true},
    {AdmissionCode::NONCE_TOO_LOW, "NONCE_TOO_LOW", true},
    {AdmissionCode::NONCE_USED, "NONCE_USED", true},
    {AdmissionCode::NONCE_PENDING, "NONCE_PENDING", true},
    {AdmissionCode::BUDGET, "BUDGET", true},
    {AdmissionCode::BAD_SIG, "BAD_SIG", true},
    {AdmissionCode::BUSY, "BUSY", true},
    {AdmissionCode::POOL_FULL, "POOL_FULL", true},
    {AdmissionCode::HALTED, "HALTED", true},
}};

//! Band of an outcome integer: 0 executed, 1 rule rejection, 2 typed refusal, -1 none.
constexpr int OutcomeBand(unsigned value)
{
    if (value >= 1 && value <= 63) return 0;
    if (value >= 64 && value <= 95) return 1;
    if (value >= 96 && value <= 127) return 2;
    return -1;
}

//! Band of an admission integer: 0 queued or known, 1 not admitted, -1 none.
constexpr int AdmissionBand(unsigned value)
{
    if (value >= 1 && value <= 15) return 0;
    if (value >= 16 && value <= 63) return 1;
    return -1;
}

// Registry shape: strictly increasing integers, unique non-reserved names, and
// a rejection flag that agrees with the band of the integer.
template <class Registry, class BandFn>
constexpr bool WellFormed(const Registry& registry, BandFn band)
{
    for (size_t i{0}; i < registry.size(); ++i) {
        const auto value{static_cast<unsigned>(registry[i].code)};
        const int b{band(value)};
        if (b < 0) return false;
        if (registry[i].rejection != (b > 0)) return false;
        if (registry[i].name.empty() || registry[i].name == "UNKNOWN") return false;
        if (i > 0 && static_cast<unsigned>(registry[i - 1].code) >= value) return false;
        for (size_t j{0}; j < i; ++j) {
            if (registry[j].name == registry[i].name) return false;
        }
    }
    return true;
}
static_assert(WellFormed(OUTCOME_CODES, OutcomeBand), "OutcomeCode registry is malformed");
static_assert(WellFormed(ADMISSION_CODES, AdmissionBand), "AdmissionCode registry is malformed");

template <class Registry, class Code>
constexpr const CodeInfo<Code>* Find(const Registry& registry, Code code)
{
    for (const auto& info : registry) {
        if (info.code == code) return &info;
    }
    return nullptr;
}

} // namespace

std::span<const CodeInfo<OutcomeCode>> OutcomeCodeRegistry() { return OUTCOME_CODES; }
std::span<const CodeInfo<AdmissionCode>> AdmissionCodeRegistry() { return ADMISSION_CODES; }

bool IsKnown(OutcomeCode code) { return Find(OUTCOME_CODES, code) != nullptr; }
bool IsKnown(AdmissionCode code) { return Find(ADMISSION_CODES, code) != nullptr; }

std::string_view ToString(OutcomeCode code)
{
    const auto* info{Find(OUTCOME_CODES, code)};
    return info ? info->name : std::string_view{"UNKNOWN"};
}

std::string_view ToString(AdmissionCode code)
{
    const auto* info{Find(ADMISSION_CODES, code)};
    return info ? info->name : std::string_view{"UNKNOWN"};
}

bool IsRejection(OutcomeCode code)
{
    const auto* info{Find(OUTCOME_CODES, code)};
    return info != nullptr && info->rejection;
}

bool IsRejection(AdmissionCode code)
{
    const auto* info{Find(ADMISSION_CODES, code)};
    return info != nullptr && info->rejection;
}

} // namespace flowmesh2
