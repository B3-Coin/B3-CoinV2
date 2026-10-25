// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/storage/snapshot_store.h>

#include <leveldb/db.h>
#include <leveldb/iterator.h>
#include <leveldb/options.h>
#include <leveldb/slice.h>
#include <leveldb/status.h>
#include <serialize.h>
#include <span.h>
#include <streams.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <utility>

namespace flowmesh2::storage {

namespace {

constexpr char KEY_PREFIX{'S'};
constexpr size_t KEY_BYTES{1 + 8};
constexpr size_t VALUE_HEADER_BYTES{4 + 32 + 32};

std::string Key(uint64_t slot)
{
    std::string key(KEY_BYTES, '\0');
    key[0] = KEY_PREFIX;
    for (size_t i{0}; i < 8; ++i) key[1 + i] = static_cast<char>((slot >> (8 * (7 - i))) & 0xff);
    return key;
}

std::optional<uint64_t> SlotOfKey(const leveldb::Slice& key)
{
    if (key.size() != KEY_BYTES || key[0] != KEY_PREFIX) return std::nullopt;
    uint64_t slot{0};
    for (size_t i{0}; i < 8; ++i) slot = (slot << 8) | static_cast<unsigned char>(key[1 + i]);
    return slot;
}

std::string Value(const SnapshotMark& mark, std::span<const unsigned char> bytes)
{
    std::vector<unsigned char> out;
    VectorWriter{out, 0, SNAPSHOT_RECORD_VERSION, mark.header_hash, mark.state_root};
    out.insert(out.end(), bytes.begin(), bytes.end());
    return std::string{out.begin(), out.end()};
}

struct Parsed {
    SnapshotMark mark;
    std::span<const unsigned char> bytes;
};

//! The mark and bytes of a stored value; nullopt if it is not one.
std::optional<Parsed> Parse(uint64_t slot, const leveldb::Slice& value)
{
    if (value.size() <= VALUE_HEADER_BYTES) return std::nullopt;
    const std::span<const unsigned char> all{reinterpret_cast<const unsigned char*>(value.data()), value.size()};
    SpanReader reader{all.first(VALUE_HEADER_BYTES)};
    uint32_t version{0};
    Parsed parsed;
    parsed.mark.slot = slot;
    reader >> version >> parsed.mark.header_hash >> parsed.mark.state_root;
    if (version != SNAPSHOT_RECORD_VERSION) return std::nullopt;
    parsed.bytes = all.subspan(VALUE_HEADER_BYTES);
    return parsed;
}

Error FromStatus(const leveldb::Status& status)
{
    if (status.IsCorruption()) return Error{ErrorCode::VERIFY_FAILED, SNAPSHOT_STORE_CORRUPT};
    return Error{ErrorCode::STORAGE_FATAL, "snapshot store I/O error"};
}

leveldb::ReadOptions Checked()
{
    leveldb::ReadOptions options;
    options.verify_checksums = true;
    return options;
}

leveldb::WriteOptions Synced()
{
    leveldb::WriteOptions options;
    options.sync = true;
    return options;
}

} // namespace

SnapshotStore::SnapshotStore(fs::path dir, std::unique_ptr<leveldb::DB> db) : m_dir{std::move(dir)}, m_db{std::move(db)} {}

SnapshotStore::~SnapshotStore() = default;

Result<std::unique_ptr<SnapshotStore>> SnapshotStore::Open(fs::path dir)
{
    leveldb::Options options;
    options.create_if_missing = true;
    options.paranoid_checks = true;
    options.max_open_files = 64;
    leveldb::DB* raw{nullptr};
    const leveldb::Status status{leveldb::DB::Open(options, fs::PathToString(dir), &raw)};
    std::unique_ptr<leveldb::DB> db{raw};
    if (!status.ok()) return FromStatus(status);
    return Ok(std::unique_ptr<SnapshotStore>{new SnapshotStore{std::move(dir), std::move(db)}});
}

Result<void> SnapshotStore::Write(const SnapshotMark& mark, std::span<const unsigned char> state_bytes)
{
    if (state_bytes.empty()) return Error{ErrorCode::INVALID_INPUT, SNAPSHOT_STORE_EMPTY};
    const std::string key{Key(mark.slot)};
    const std::string value{Value(mark, state_bytes)};
    std::string stored;
    const leveldb::Status found{m_db->Get(Checked(), key, &stored)};
    if (found.ok()) {
        if (stored == value) return Ok();
        return Error{ErrorCode::INVALID_INPUT, SNAPSHOT_STORE_CONFLICT};
    }
    if (!found.IsNotFound()) return FromStatus(found);
    const leveldb::Status put{m_db->Put(Synced(), key, value)};
    if (!put.ok()) return FromStatus(put);
    return Ok();
}

Result<std::vector<unsigned char>> SnapshotStore::Load(const SnapshotMark& mark) const
{
    std::string stored;
    const leveldb::Status found{m_db->Get(Checked(), Key(mark.slot), &stored)};
    if (found.IsNotFound()) return Error{ErrorCode::INVALID_INPUT, SNAPSHOT_STORE_ABSENT};
    if (!found.ok()) return FromStatus(found);
    const std::optional<Parsed> parsed{Parse(mark.slot, stored)};
    if (!parsed) return Error{ErrorCode::VERIFY_FAILED, SNAPSHOT_STORE_CORRUPT};
    if (!(parsed->mark == mark)) return Error{ErrorCode::VERIFY_FAILED, SNAPSHOT_STORE_MARK};
    return Ok(std::vector<unsigned char>{parsed->bytes.begin(), parsed->bytes.end()});
}

Result<std::vector<SnapshotMark>> SnapshotStore::Marks() const
{
    std::vector<SnapshotMark> marks;
    const std::unique_ptr<leveldb::Iterator> it{m_db->NewIterator(Checked())};
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        const std::optional<uint64_t> slot{SlotOfKey(it->key())};
        if (!slot) return Error{ErrorCode::VERIFY_FAILED, SNAPSHOT_STORE_CORRUPT};
        const std::optional<Parsed> parsed{Parse(*slot, it->value())};
        if (!parsed) return Error{ErrorCode::VERIFY_FAILED, SNAPSHOT_STORE_CORRUPT};
        marks.push_back(parsed->mark);
    }
    if (!it->status().ok()) return FromStatus(it->status());
    return Ok(std::move(marks));
}

Result<void> SnapshotStore::Erase(uint64_t slot)
{
    const leveldb::Status status{m_db->Delete(Synced(), Key(slot))};
    if (!status.ok()) return FromStatus(status);
    return Ok();
}

} // namespace flowmesh2::storage
