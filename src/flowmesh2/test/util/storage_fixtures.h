// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_TEST_UTIL_STORAGE_FIXTURES_H
#define B3_FLOWMESH2_TEST_UTIL_STORAGE_FIXTURES_H

#include <flowmesh2/core/result.h>
#include <flowmesh2/storage/journal.h>
#include <flowmesh2/storage/record.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

/** TEST fixtures for the storage suites (slice R8). */
namespace flowmesh2::test {

//! Fails the test (reading error() only on failure) unless `result` holds a value.
template <class T>
void RequireStorageOk(const Result<T>& result, std::string_view what)
{
    if (!result.has_value()) {
        BOOST_FAIL(std::string{what} + " failed: " + std::string{ToString(result.error().code)} + " " +
                   std::string{result.error().detail});
    }
}

//! Requires an error with exactly `code` and `detail`.
template <class T>
void RequireRefusal(const Result<T>& result, ErrorCode code, std::string_view detail, std::string_view what)
{
    if (result.has_value()) BOOST_FAIL(std::string{what} + " unexpectedly succeeded");
    BOOST_CHECK_MESSAGE(result.error().code == code && result.error().detail == detail,
                        std::string{what} + ": got " + std::string{ToString(result.error().code)} + " (" +
                            std::string{result.error().detail} + "), want " + std::string{ToString(code)} + " (" +
                            std::string{detail} + ")");
}

//! Requires an error with exactly `code` (any detail).
template <class T>
void RequireCode(const Result<T>& result, ErrorCode code, std::string_view what)
{
    if (result.has_value()) BOOST_FAIL(std::string{what} + " unexpectedly succeeded");
    BOOST_CHECK_MESSAGE(result.error().code == code, std::string{what} + ": got " + std::string{ToString(result.error().code)} +
                                                         " (" + std::string{result.error().detail} + "), want " +
                                                         std::string{ToString(code)});
}

inline std::vector<unsigned char> Bytes(std::string_view text) { return {text.begin(), text.end()}; }

/** Records every observer callback as one line:
 *   "append <ROLE> <instance> <TYPE> <seq>", "barrier-start <ROLE> <instance> <target>",
 *   "barrier-end <ROLE> <instance> <target> ok|fail".
 * `on_barrier_start`, when set, runs inside OnBarrierStart (re-entrancy tests).
 */
class RecordingObserver : public storage::BarrierObserver
{
public:
    std::vector<std::string> events;
    std::function<void()> on_barrier_start;

    void OnAppend(storage::JournalRole role, uint64_t instance_id, storage::RecordType type, uint64_t seq) override
    {
        events.push_back("append " + std::string{ToString(role)} + " " + std::to_string(instance_id) + " " +
                         std::string{ToString(type)} + " " + std::to_string(seq));
    }
    void OnBarrierStart(storage::JournalRole role, uint64_t instance_id, uint64_t target_seq) override
    {
        events.push_back("barrier-start " + std::string{ToString(role)} + " " + std::to_string(instance_id) + " " +
                         std::to_string(target_seq));
        if (on_barrier_start) on_barrier_start();
    }
    void OnBarrierEnd(storage::JournalRole role, uint64_t instance_id, uint64_t target_seq, bool ok) override
    {
        events.push_back("barrier-end " + std::string{ToString(role)} + " " + std::to_string(instance_id) + " " +
                         std::to_string(target_seq) + (ok ? " ok" : " fail"));
    }
};

} // namespace flowmesh2::test

#endif // B3_FLOWMESH2_TEST_UTIL_STORAGE_FIXTURES_H
