// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_TEST_UTIL_STORAGE_FIXTURES_H
#define B3_FLOWMESH2_TEST_UTIL_STORAGE_FIXTURES_H

#include <flowmesh2/core/body.h>
#include <flowmesh2/core/certificate.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/slot.h>
#include <flowmesh2/storage/journal.h>
#include <flowmesh2/storage/memory_journal.h>
#include <flowmesh2/storage/record.h>
#include <flowmesh2/storage/tokens.h>
#include <flowmesh2/test/util/keys.h>
#include <flowmesh2/test/util/require.h>
#include <util/fs.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <unistd.h>

#ifndef FM2_TEST_TMP_DIR
#error "test_flowmesh2 is compiled with FM2_TEST_TMP_DIR (build-dir/fm2-test-tmp)"
#endif

/** TEST fixtures for the storage suites (slice R8).
 *
 * Temporary directories live under FM2_TEST_TMP_DIR/storage/<name>-<pid>
 * (build-rebirth/fm2-test-tmp/, ARCHITECTURE.md 13.2); TestDir removes its
 * directory before and after use.
 */
namespace flowmesh2::test {

//! A fresh, empty directory FM2_TEST_TMP_DIR/storage/<name>-<pid>, removed again on destruction.
class TestDir
{
public:
    fs::path path;

    explicit TestDir(std::string_view name)
        : path{fs::PathFromString(FM2_TEST_TMP_DIR) / fs::u8path("storage") /
               fs::u8path(std::string{name} + "-" + std::to_string(getpid()))}
    {
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TestDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TestDir(const TestDir&) = delete;
    TestDir& operator=(const TestDir&) = delete;

    //! A new empty subdirectory.
    fs::path Sub(std::string_view name) const
    {
        const fs::path sub{path / fs::u8path(name)};
        fs::create_directories(sub);
        return sub;
    }
};

inline std::vector<unsigned char> ReadFile(const fs::path& file)
{
    std::ifstream in{static_cast<const std::filesystem::path&>(file), std::ios::binary};
    BOOST_REQUIRE(in.good());
    return std::vector<unsigned char>{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

inline void WriteFile(const fs::path& file, const std::vector<unsigned char>& bytes)
{
    std::ofstream out{static_cast<const std::filesystem::path&>(file), std::ios::binary};
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    BOOST_REQUIRE(out.good());
}

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

// RequireStub (NOT_IMPLEMENTED with exactly the task id) is in test/util/require.h.

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

//! A MemoryJournal (no faults unless given), failing the test if Create fails.
inline std::unique_ptr<storage::MemoryJournal> NewMemoryJournal(storage::JournalRole role, uint64_t instance,
                                                                storage::MemoryJournalFaults faults = {},
                                                                storage::BarrierObserver* observer = nullptr)
{
    Result<std::unique_ptr<storage::MemoryJournal>> journal{storage::MemoryJournal::Create(role, instance, faults, observer)};
    BOOST_REQUIRE(journal.has_value());
    return std::move(journal).value();
}

/** A consistent slot for the logs: a body with one opaque original, a header
 * whose roots and count match it, and a certificate naming the header (seat 0;
 * the aggregate is not a signature, the logs do not verify it). `label`
 * changes the original, the parent hash and the state root, so two labels give
 * two different headers for one height.
 */
struct StorageSlot {
    SlotHeaderV2 header;
    SlotBody body;
    SlotCertificate certificate;
};

inline StorageSlot MakeStorageSlot(uint64_t height, uint64_t epoch, std::string_view label)
{
    StorageSlot slot;
    slot.body.originals = {Bytes("fm2/test/storage/original/" + std::string{label})};
    SlotHeaderV2& h{slot.header};
    h.domain = LabelHash("fm2/test/storage/domain");
    h.profile_hash = LabelHash("fm2/test/storage/profile");
    h.authority_epoch = epoch;
    h.height = height;
    h.parent_hash = LabelHash("fm2/test/storage/parent/" + std::string{label});
    h.seat_set_root = LabelHash("fm2/test/storage/seats");
    h.facts_root = slot.body.FactsRoot();
    h.actions_root = slot.body.ActionsRoot();
    h.outcomes_root = LabelHash("fm2/test/storage/outcomes");
    h.state_root = LabelHash("fm2/test/storage/state/" + std::string{label});
    h.action_count = 1;
    slot.certificate.header_hash = h.Hash();
    slot.certificate.bitmap = {0x01};
    return slot;
}

//! The COMMIT-phase VoteKey of a header in `view`.
inline storage::VoteKey CommitVoteKey(const SlotHeaderV2& header, uint64_t view)
{
    return storage::VoteKey{header.domain, header.authority_epoch, header.height, view, storage::VotePhase::COMMIT};
}

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
