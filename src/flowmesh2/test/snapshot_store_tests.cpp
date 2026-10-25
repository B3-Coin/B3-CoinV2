// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/state/candidate.h>
#include <flowmesh2/state/snapshot.h>
#include <flowmesh2/storage/snapshot_store.h>
#include <flowmesh2/test/util/state_fixtures.h>
#include <flowmesh2/test/util/storage_fixtures.h>
#include <util/fs.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::test;

namespace {

std::unique_ptr<storage::SnapshotStore> MustOpen(const fs::path& dir)
{
    Result<std::unique_ptr<storage::SnapshotStore>> store{storage::SnapshotStore::Open(dir)};
    RequireOk(store, "SnapshotStore::Open");
    return std::move(store).value();
}

storage::SnapshotMark Mark(uint64_t slot)
{
    return storage::SnapshotMark{slot, LabelHash("fm2/test/snapshot/header/" + std::to_string(slot)),
                                 LabelHash("fm2/test/snapshot/root/" + std::to_string(slot))};
}

std::vector<unsigned char> StateBytes(uint64_t slot, size_t size)
{
    std::vector<unsigned char> bytes(size);
    for (size_t i{0}; i < size; ++i) bytes[i] = static_cast<unsigned char>((slot * 131 + i * 7) & 0xff);
    return bytes;
}

std::vector<uint64_t> Slots(const storage::SnapshotStore& store)
{
    Result<std::vector<storage::SnapshotMark>> marks{store.Marks()};
    RequireOk(marks, "Marks");
    std::vector<uint64_t> slots;
    for (const storage::SnapshotMark& mark : *marks) slots.push_back(mark.slot);
    return slots;
}

} // namespace

BOOST_AUTO_TEST_SUITE(snapshot_store_tests)

BOOST_AUTO_TEST_CASE(snapshots_survive_a_reopen_and_list_by_slot)
{
    const TestDir dir{"snapshot-store-reopen"};
    // Slots chosen so that a little-endian or text key would list them out of order.
    const std::vector<uint64_t> slots{256, 3, 255, std::numeric_limits<uint64_t>::max(), 0, 65'536};
    {
        const auto store{MustOpen(dir.path)};
        for (const uint64_t slot : slots) RequireOk(store->Write(Mark(slot), StateBytes(slot, 100 + slot % 1000)), "Write");
        // One large snapshot, larger than LevelDB's write buffer.
        RequireOk(store->Write(Mark(7), StateBytes(7, 5 << 20)), "Write large");
    }
    const auto store{MustOpen(dir.path)};
    BOOST_CHECK((Slots(*store) == std::vector<uint64_t>{0, 3, 7, 255, 256, 65'536, std::numeric_limits<uint64_t>::max()}));
    const Result<std::vector<storage::SnapshotMark>> marks{store->Marks()};
    RequireOk(marks, "Marks");
    for (const storage::SnapshotMark& mark : *marks) BOOST_CHECK(mark == Mark(mark.slot));
    for (const uint64_t slot : slots) {
        const Result<std::vector<unsigned char>> loaded{store->Load(Mark(slot))};
        RequireOk(loaded, "Load");
        BOOST_CHECK(*loaded == StateBytes(slot, 100 + slot % 1000));
    }
    const Result<std::vector<unsigned char>> large{store->Load(Mark(7))};
    RequireOk(large, "Load large");
    BOOST_CHECK(*large == StateBytes(7, 5 << 20));
}

BOOST_AUTO_TEST_CASE(a_slot_holds_one_snapshot)
{
    const TestDir dir{"snapshot-store-conflict"};
    const auto store{MustOpen(dir.path)};
    const std::vector<unsigned char> bytes{StateBytes(4, 64)};
    RequireOk(store->Write(Mark(4), bytes), "Write");
    RequireOk(store->Write(Mark(4), bytes), "the same snapshot again");
    std::vector<unsigned char> other{bytes};
    other.back() ^= 1;
    RequireRefusal(store->Write(Mark(4), other), ErrorCode::INVALID_INPUT, storage::SNAPSHOT_STORE_CONFLICT, "other bytes");
    storage::SnapshotMark moved{Mark(4)};
    moved.header_hash = Mark(5).header_hash;
    RequireRefusal(store->Write(moved, bytes), ErrorCode::INVALID_INPUT, storage::SNAPSHOT_STORE_CONFLICT, "another header");
    RequireRefusal(store->Write(Mark(6), {}), ErrorCode::INVALID_INPUT, storage::SNAPSHOT_STORE_EMPTY, "empty bytes");
    RequireRefusal(store->Load(Mark(5)), ErrorCode::INVALID_INPUT, storage::SNAPSHOT_STORE_ABSENT, "an absent slot");
    RequireRefusal(store->Load(moved), ErrorCode::VERIFY_FAILED, storage::SNAPSHOT_STORE_MARK, "another header");
    storage::SnapshotMark rerooted{Mark(4)};
    rerooted.state_root = Mark(5).state_root;
    RequireRefusal(store->Load(rerooted), ErrorCode::VERIFY_FAILED, storage::SNAPSHOT_STORE_MARK, "another root");
    // The refused writes changed nothing.
    const Result<std::vector<unsigned char>> loaded{store->Load(Mark(4))};
    RequireOk(loaded, "Load");
    BOOST_CHECK(*loaded == bytes);
    BOOST_CHECK((Slots(*store) == std::vector<uint64_t>{4}));
    // Erase, then the slot is free again.
    RequireOk(store->Erase(4), "Erase");
    RequireOk(store->Erase(4), "Erase an absent slot");
    BOOST_CHECK(Slots(*store).empty());
    RequireOk(store->Write(Mark(4), other), "Write after Erase");
}

BOOST_AUTO_TEST_CASE(a_mark_decodes_only_from_its_exact_bytes)
{
    const storage::SnapshotMark mark{Mark(0x0102030405060708)};
    const std::vector<unsigned char> bytes{mark.Encode()};
    BOOST_REQUIRE_EQUAL(bytes.size(), storage::SnapshotMark::ENCODED_BYTES);
    BOOST_CHECK_EQUAL(bytes[0], 0x08); // little-endian slot first
    BOOST_CHECK(std::equal(mark.header_hash.begin(), mark.header_hash.end(), bytes.begin() + 8));
    BOOST_CHECK(std::equal(mark.state_root.begin(), mark.state_root.end(), bytes.begin() + 40));
    const Result<storage::SnapshotMark> decoded{storage::SnapshotMark::Decode(bytes)};
    RequireOk(decoded, "Decode");
    BOOST_CHECK(*decoded == mark);
    std::vector<unsigned char> longer{bytes};
    longer.push_back(0);
    RequireCode(storage::SnapshotMark::Decode(longer), ErrorCode::OVERSIZE, "a trailing byte");
    RequireCode(storage::SnapshotMark::Decode(std::span{bytes}.first(bytes.size() - 1)), ErrorCode::DECODE, "one byte short");
    RequireCode(storage::SnapshotMark::Decode({}), ErrorCode::DECODE, "empty");
}

BOOST_AUTO_TEST_CASE(a_store_opened_twice_is_refused)
{
    const TestDir dir{"snapshot-store-twice"};
    const auto store{MustOpen(dir.path)};
    RequireCode(storage::SnapshotStore::Open(dir.path), ErrorCode::STORAGE_FATAL, "a second Open of one directory");
}

BOOST_AUTO_TEST_CASE(damaged_store_files_never_give_other_bytes)
{
    // Bytes of the store's log file are flipped in turn, on a copy: every
    // byte of the first record's framing and payload start, then every 13th
    // byte (all of them would take minutes under TSan). The
    // store may refuse to open, refuse a read, or lose the damaged record,
    // but a read that succeeds gives exactly the bytes written.
    const TestDir dir{"snapshot-store-damage"};
    const fs::path clean{dir.Sub("clean")};
    const std::vector<uint64_t> slots{1, 2, 3};
    {
        const auto store{MustOpen(clean)};
        for (const uint64_t slot : slots) RequireOk(store->Write(Mark(slot), StateBytes(slot, 200)), "Write");
    }
    fs::path log_file;
    for (const auto& entry : fs::directory_iterator{clean}) {
        if (entry.path().extension() == ".log" && fs::file_size(entry.path()) > 0) log_file = entry.path();
    }
    BOOST_REQUIRE_MESSAGE(!log_file.empty(), "the store wrote no log file");
    const std::vector<unsigned char> original{ReadFile(log_file)};
    size_t refused{0};
    size_t lost{0};
    for (size_t i{0}; i < original.size(); i += i < 96 ? 1 : 13) {
        const fs::path copy{dir.path / fs::u8path("damaged-" + std::to_string(i))};
        fs::copy(clean, copy, fs::copy_options::recursive);
        std::vector<unsigned char> damaged{original};
        damaged[i] ^= 0x10;
        WriteFile(copy / fs::u8path(log_file.filename().string()), damaged);
        {
            Result<std::unique_ptr<storage::SnapshotStore>> store{storage::SnapshotStore::Open(copy)};
            if (!store.has_value()) {
                BOOST_CHECK_MESSAGE(store.error().code == ErrorCode::VERIFY_FAILED || store.error().code == ErrorCode::STORAGE_FATAL,
                                    "byte " + std::to_string(i) + ": Open gave " + std::string{ToString(store.error().code)});
                ++refused;
            } else {
                for (const uint64_t slot : slots) {
                    const Result<std::vector<unsigned char>> loaded{(*store)->Load(Mark(slot))};
                    if (loaded.has_value()) {
                        BOOST_CHECK_MESSAGE(*loaded == StateBytes(slot, 200), "byte " + std::to_string(i) + " changed the bytes of slot " + std::to_string(slot));
                    } else {
                        ++lost;
                    }
                }
            }
        } // the store closes before its copy is removed
        fs::remove_all(copy);
    }
    // With paranoid checks, damage is noticed rather than silently absorbed everywhere.
    BOOST_CHECK_GT(refused + lost, 0U);
    BOOST_TEST_MESSAGE("log bytes " << original.size() << ", refused opens " << refused << ", lost reads " << lost);
}

BOOST_AUTO_TEST_CASE(a_stored_snapshot_restores_its_head)
{
    OwnerFixture f;
    RunWalkingSkeleton(f);
    const std::shared_ptr<const state::HeadView> head{f.Head()};
    const Result<std::vector<unsigned char>> bytes{state::EncodeSnapshot(head->State())};
    RequireOk(bytes, "EncodeSnapshot");
    const storage::SnapshotMark mark{head->Header().height, head->HeaderHash(), head->Header().state_root};
    const TestDir dir{"snapshot-store-head"};
    {
        const auto store{MustOpen(dir.path)};
        RequireOk(store->Write(mark, *bytes), "Write");
    }
    const auto store{MustOpen(dir.path)};
    const Result<std::vector<unsigned char>> loaded{store->Load(mark)};
    RequireOk(loaded, "Load");
    const Result<state::HeadView> restored{state::SnapshotHead(*loaded, RegtestProfile(), head->Header(), head->CertHash())};
    RequireOk(restored, "SnapshotHead");
    BOOST_CHECK(restored->State() == head->State());
    BOOST_CHECK(restored->HeaderHash() == head->HeaderHash());
}

BOOST_AUTO_TEST_SUITE_END()
