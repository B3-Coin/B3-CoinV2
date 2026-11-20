// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_SIM_CHECKS_H
#define B3_FLOWMESH2_SIM_CHECKS_H

#include <flowmesh2/core/result.h>
#include <flowmesh2/sim/scenario.h>

#include <string>
#include <string_view>

/** The check bookkeeping the scenario drivers share (slice R14; the cluster
 * scenario of M4-SIM-01 uses it too). Internal to fm2_simlib.
 */
namespace flowmesh2::sim::detail {

inline std::string ErrorText(const Error& error) { return std::string{ToString(error.code)} + " " + std::string{error.detail}; }

//! The results of the checks of one run (or of a suite).
class Checks
{
public:
    void Expect(std::string_view id, bool ok, std::string_view what)
    {
        CheckResult& result{m_results[std::string{id}]};
        if (!result.ran) {
            result.ran = true;
            result.passed = true;
        }
        if (!ok && result.passed) {
            result.passed = false;
            result.detail = std::string{what};
        }
    }
    template <class T>
    void ExpectOk(std::string_view id, const Result<T>& result, std::string_view what)
    {
        if (result.has_value()) {
            Expect(id, true, what);
        } else {
            Expect(id, false, std::string{what} + ": " + ErrorText(result.error()));
        }
    }
    const CheckMap& Results() const { return m_results; }

private:
    CheckMap m_results;
};

} // namespace flowmesh2::sim::detail

#endif // B3_FLOWMESH2_SIM_CHECKS_H
