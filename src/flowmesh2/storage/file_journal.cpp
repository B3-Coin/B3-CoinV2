// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/storage/file_journal.h>

#include <flowmesh2/storage/journal_commit.h>

#include <crc32c/crc32c.h>
#include <crypto/common.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#ifdef WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/locking.h>
#include <windows.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace flowmesh2::storage {

namespace {

//! Write all of `bytes` at `offset`; STORAGE_FATAL on any error or zero-length write.
Result<void> WriteAllAt(int fd, std::span<const unsigned char> bytes, uint64_t offset)
{
#ifdef WIN32
    const HANDLE handle{reinterpret_cast<HANDLE>(_get_osfhandle(fd))};
    while (!bytes.empty()) {
        OVERLAPPED at{};
        at.Offset = static_cast<DWORD>(offset);
        at.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD written{0};
        const DWORD chunk{static_cast<DWORD>(std::min<size_t>(bytes.size(), 1U << 30))};
        if (!WriteFile(handle, bytes.data(), chunk, &written, &at) || written == 0) {
            return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_WRITE};
        }
        bytes = bytes.subspan(written);
        offset += written;
    }
#else
    while (!bytes.empty()) {
        if (offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
            return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_WRITE};
        }
        const ssize_t written{pwrite(fd, bytes.data(), bytes.size(), static_cast<off_t>(offset))};
        if (written <= 0) return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_WRITE};
        bytes = bytes.subspan(static_cast<size_t>(written));
        offset += static_cast<uint64_t>(written);
    }
#endif
    return Ok();
}

int OpenNewSegment(const fs::path& path)
{
#ifdef WIN32
    return _wopen(path.c_str(), _O_RDWR | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
    return open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
#endif
}

void CloseFd(int fd)
{
#ifdef WIN32
    _close(fd);
#else
    close(fd);
#endif
}

//! Every journal instance holds this lock until descriptor destruction.
bool LockSegment(int fd)
{
#ifdef WIN32
    return _lseeki64(fd, 0, SEEK_SET) == 0 && _locking(fd, _LK_NBLCK, 1) == 0;
#else
    return flock(fd, LOCK_EX | LOCK_NB) == 0;
#endif
}

class OwnedFd
{
public:
    explicit OwnedFd(int fd) : m_fd{fd} {}
    ~OwnedFd() { if (m_fd >= 0) CloseFd(m_fd); }
    OwnedFd(const OwnedFd&) = delete;
    OwnedFd& operator=(const OwnedFd&) = delete;
    void Release() { m_fd = -1; }
private:
    int m_fd;
};

Result<uint64_t> SegmentSize(int fd)
{
#ifdef WIN32
    struct _stat64 info;
    if (_fstat64(fd, &info) != 0 || (info.st_mode & _S_IFREG) == 0 || info.st_size < 0) {
#else
    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0) {
#endif
        return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_READ};
    }
    return Ok(static_cast<uint64_t>(info.st_size));
}

//! Read an immutable prefix. POSIX pread does not share the writer's offset.
Result<std::vector<unsigned char>> ReadPrefix(int fd, uint64_t bytes)
{
    std::vector<unsigned char> image(static_cast<size_t>(bytes));
    size_t offset{0};
    while (offset < image.size()) {
#ifdef WIN32
        const HANDLE handle{reinterpret_cast<HANDLE>(_get_osfhandle(fd))};
        OVERLAPPED at{};
        at.Offset = static_cast<DWORD>(offset);
        at.OffsetHigh = static_cast<DWORD>(static_cast<uint64_t>(offset) >> 32);
        DWORD count{0};
        const DWORD chunk{static_cast<DWORD>(std::min<size_t>(image.size() - offset, 1U << 30))};
        if (!ReadFile(handle, image.data() + offset, chunk, &count, &at)) {
            return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_READ};
        }
#else
        const ssize_t count{pread(fd, image.data() + offset, image.size() - offset, static_cast<off_t>(offset))};
        if (count < 0 && errno == EINTR) continue;
#endif
        if (count <= 0) return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_READ};
        offset += static_cast<size_t>(count);
    }
    return Ok(std::move(image));
}

//! Explicit zero pages for a whole segment (plan section 3.7, S-9): the blocks
//! are allocated now, not on the first barrier, and the unused tail reads back
//! as zeros.
Result<void> WriteZeros(int fd, uint64_t segment_bytes)
{
    static constexpr std::array<unsigned char, 64 * 1024> ZEROS{};
    for (uint64_t offset{0}; offset < segment_bytes; offset += ZEROS.size()) {
        const size_t chunk{static_cast<size_t>(std::min<uint64_t>(ZEROS.size(), segment_bytes - offset))};
        if (const Result<void> written{WriteAllAt(fd, std::span{ZEROS}.first(chunk), offset)}; !written.has_value()) {
            return written.error();
        }
    }
    return Ok();
}

constexpr std::array<unsigned char, 8> SEGMENT_MAGIC{'B', '3', 'F', 'M', '2', 'S', 'E', 'G'};
constexpr size_t HEADER_CRC_AT{SEGMENT_HEADER_BYTES - 4};

Result<void> WriteHeader(int fd, JournalRole role, uint64_t instance_id, uint64_t index, uint64_t segment_bytes)
{
    const std::array<unsigned char, SEGMENT_HEADER_BYTES> header{EncodeSegmentHeader(role, instance_id, index, segment_bytes)};
    return WriteAllAt(fd, header, 0);
}

//! The header of segment `index` names this journal and size.
Result<void> CheckHeader(std::span<const unsigned char> image, JournalRole role, uint64_t instance_id, uint64_t index,
                         uint64_t segment_bytes)
{
    const Error bad{ErrorCode::VERIFY_FAILED, JOURNAL_SEGMENT_HEADER};
    if (image.size() < SEGMENT_HEADER_BYTES) return bad;
    const unsigned char* h{image.data()};
    if (ReadLE32(h + HEADER_CRC_AT) != crc32c::Crc32c(h, HEADER_CRC_AT)) return bad;
    if (!std::equal(SEGMENT_MAGIC.begin(), SEGMENT_MAGIC.end(), h) || ReadLE32(h + 8) != SEGMENT_FORMAT_VERSION) return bad;
    const auto zero = [](const unsigned char* from, size_t count) { return std::all_of(from, from + count, [](unsigned char b) { return b == 0; }); };
    if (!zero(h + 13, 3) || !zero(h + 40, 20)) return bad;
    if (h[12] != static_cast<unsigned char>(role)) return Error{ErrorCode::ORDER_VIOLATION, JOURNAL_SEGMENT_HEADER};
    if (ReadLE64(h + 16) != instance_id || ReadLE64(h + 24) != index || ReadLE64(h + 32) != segment_bytes) return bad;
    return Ok();
}

int OpenSegment(const fs::path& path)
{
#ifdef WIN32
    return _wopen(path.c_str(), _O_RDWR | _O_BINARY);
#else
    return open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
#endif
}

//! Open (truncating) the temporary file of a segment being created.
int OpenTempSegment(const fs::path& path)
{
#ifdef WIN32
    return _wopen(path.c_str(), _O_RDWR | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
    return open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
#endif
}

/** Create segment `index` of `dir`: zero-fill its temporary file and write
 * its header, commit it, rename it into place (never over an existing
 * segment) and commit the directory. Always the strict commits. Returns the
 * open descriptor. A crash before the rename leaves only the temporary file,
 * which OpenExisting ignores; a failure after the rename leaves a segment
 * that is not in the journal's list, so the next creation of that index
 * refuses.
 */
Result<int> CreateSegmentFile(const fs::path& dir, JournalRole role, uint64_t instance_id, uint64_t index, uint64_t segment_bytes)
{
    const std::string name{SegmentName(index)};
    const fs::path final_path{dir / fs::u8path(name)};
    const fs::path temp_path{dir / fs::u8path(name + std::string{SEGMENT_TEMP_SUFFIX})};
    const int fd{OpenTempSegment(temp_path)};
    if (fd < 0) return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_CREATE};
    OwnedFd owned{fd};
    if (const Result<void> zeroed{WriteZeros(fd, segment_bytes)}; !zeroed.has_value()) return zeroed.error();
    if (const Result<void> headed{WriteHeader(fd, role, instance_id, index, segment_bytes)}; !headed.has_value()) return headed.error();
    if (const Result<void> committed{JournalCommit(fd)}; !committed.has_value()) return committed.error();
    std::error_code ec;
    // Never over an existing file (not_found is the only answer that proceeds; it may come with ec set).
    if (fs::symlink_status(final_path, ec).type() != fs::file_type::not_found) return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_CREATE};
    ec.clear();
    fs::rename(temp_path, final_path, ec);
    if (ec) return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_CREATE};
    if (const Result<void> committed{CommitDirectory(dir)}; !committed.has_value()) return committed.error();
    owned.Release();
    return Ok(fd);
}

//! The index of `name` if it is exactly SegmentName(index) + `suffix`.
std::optional<uint64_t> ParseSegmentName(const std::string& name, std::string_view suffix)
{
    static constexpr size_t DIGITS{8};
    if (name.size() != DIGITS + 4 + suffix.size()) return std::nullopt;
    uint64_t index{0};
    for (size_t i{0}; i < DIGITS; ++i) {
        if (name[i] < '0' || name[i] > '9') return std::nullopt;
        index = index * 10 + static_cast<uint64_t>(name[i] - '0');
    }
    if (name != SegmentName(index) + std::string{suffix}) return std::nullopt;
    return index;
}

//! Whether a record area starts with an all-zero record header, the zero tail of ScanRecords.
bool NoRecordAtStart(std::span<const unsigned char> image)
{
    const size_t header{std::min(image.size(), RECORD_HEADER_BYTES)};
    return std::all_of(image.begin(), image.begin() + static_cast<std::ptrdiff_t>(header), [](unsigned char b) { return b == 0; });
}

/** Scans the segments of one image in order, as the writer produced them:
 * the chain continues across segments, a segment without records comes only
 * after the last record, and a segment's first record did not fit in the
 * rest of the one before. Result().end_offset is the virtual offset after the
 * last record. One segment's bytes at a time, so a reopen holds one segment.
 */
class SegmentScan
{
public:
    SegmentScan(JournalRole role, uint64_t instance_id, uint64_t segment_bytes)
        : m_role{role}, m_instance_id{instance_id}, m_segment_bytes{segment_bytes}
    {
        m_out.last_chain = JournalGenesisChain(role, instance_id);
        m_out.end_offset = SEGMENT_HEADER_BYTES; // where the first record goes
    }

    //! The next segment's bytes: the whole segment, or a prefix of at least its header.
    Result<void> Feed(std::span<const unsigned char> image)
    {
        const uint64_t k{m_fed++};
        if (const Result<void> header{CheckHeader(image, m_role, m_instance_id, k, m_segment_bytes)}; !header.has_value()) {
            return header.error();
        }
        const std::span<const unsigned char> records{image.subspan(SEGMENT_HEADER_BYTES)};
        if (m_empty_seen && !NoRecordAtStart(records)) return Error{ErrorCode::INVALID_INPUT, JOURNAL_SEGMENT_LAYOUT};
        Result<ScanResult> scanned{ScanRecordsFrom(records, m_role, m_out.last_chain, m_out.records.size() + 1)};
        if (!scanned.has_value()) return scanned.error();
        if (scanned->records.empty()) {
            m_empty_seen = true;
            return Ok();
        }
        if (k > 0 && m_previous_end + RECORD_OVERHEAD_BYTES + scanned->records.front().payload.size() <= m_segment_bytes) {
            return Error{ErrorCode::INVALID_INPUT, JOURNAL_SEGMENT_LAYOUT};
        }
        m_previous_end = SEGMENT_HEADER_BYTES + scanned->end_offset;
        m_out.end_offset = k * m_segment_bytes + m_previous_end;
        m_out.last_chain = scanned->last_chain;
        for (JournalRecord& record : scanned->records) m_out.records.push_back(std::move(record));
        return Ok();
    }

    ScanResult& Out() { return m_out; }

private:
    const JournalRole m_role;
    const uint64_t m_instance_id;
    const uint64_t m_segment_bytes;
    ScanResult m_out;
    uint64_t m_fed{0};
    bool m_empty_seen{false};
    uint64_t m_previous_end{0}; //!< within the last segment that held records, header included
};

} // namespace

std::array<unsigned char, SEGMENT_HEADER_BYTES> EncodeSegmentHeader(JournalRole role, uint64_t instance_id, uint64_t index,
                                                                    uint64_t segment_bytes)
{
    std::array<unsigned char, SEGMENT_HEADER_BYTES> header{};
    std::copy(SEGMENT_MAGIC.begin(), SEGMENT_MAGIC.end(), header.begin());
    WriteLE32(header.data() + 8, SEGMENT_FORMAT_VERSION);
    header[12] = static_cast<unsigned char>(role);
    WriteLE64(header.data() + 16, instance_id);
    WriteLE64(header.data() + 24, index);
    WriteLE64(header.data() + 32, segment_bytes);
    WriteLE32(header.data() + HEADER_CRC_AT, crc32c::Crc32c(header.data(), HEADER_CRC_AT));
    return header;
}

std::string SegmentName(uint64_t index)
{
    char name[24];
    std::snprintf(name, sizeof(name), "%08llu.seg", static_cast<unsigned long long>(index));
    return std::string{name};
}

FileJournal::FileJournal(JournalRole role, uint64_t instance_id, BarrierObserver* observer, fs::path dir, std::vector<int> fds,
                         uint64_t segment_bytes, SegmentCommitHook commit, const ScanResult* recovered)
    : Journal(ImplKey{}, role, instance_id, observer, recovered),
      m_dir{std::move(dir)},
      m_segment_path{m_dir / fs::u8path(FIRST_SEGMENT_NAME)},
      m_segment_bytes{segment_bytes},
      m_commit{commit},
      m_fds{std::move(fds)}
{
}

FileJournal::~FileJournal()
{
    // Closing segment 0 last releases the advisory lock last.
    for (auto fd{m_fds.rbegin()}; fd != m_fds.rend(); ++fd) CloseFd(*fd);
}

Result<std::unique_ptr<FileJournal>> FileJournal::CreateNew(const fs::path& empty_dir, JournalRole role, uint64_t instance_id,
                                                            uint64_t segment_bytes, BarrierObserver* observer)
{
    return CreateNewWithCommitHook(empty_dir, role, instance_id, segment_bytes, &JournalCommit, observer);
}

Result<std::unique_ptr<FileJournal>> FileJournal::CreateNewWithCommitHook(const fs::path& empty_dir, JournalRole role,
                                                                          uint64_t instance_id, uint64_t segment_bytes,
                                                                          SegmentCommitHook commit, BarrierObserver* observer)
{
    if (!IsKnown(role)) return Error{ErrorCode::INVALID_INPUT, "unknown journal role"};
    if (commit == nullptr) return Error{ErrorCode::INVALID_INPUT, "missing segment commit"};
    if (segment_bytes < SEGMENT_PAGE_BYTES || segment_bytes > MAX_SEGMENT_BYTES || segment_bytes % SEGMENT_PAGE_BYTES != 0) {
        return Error{ErrorCode::INVALID_INPUT, JOURNAL_SEGMENT_SIZE};
    }
    std::error_code ec;
    if (!fs::is_directory(empty_dir, ec) || ec) return Error{ErrorCode::INVALID_INPUT, JOURNAL_DIR_MISSING};
    const fs::directory_iterator first{empty_dir, ec};
    if (ec) return Error{ErrorCode::INVALID_INPUT, JOURNAL_DIR_MISSING};
    if (first != fs::directory_iterator{}) return Error{ErrorCode::INVALID_INPUT, JOURNAL_DIR_NOT_EMPTY};

    const int fd{OpenNewSegment(empty_dir / fs::u8path(FIRST_SEGMENT_NAME))};
    if (fd < 0) return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_CREATE};
    OwnedFd owned{fd};
    if (!LockSegment(fd)) return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_LOCKED};
    if (const Result<void> zeroed{WriteZeros(fd, segment_bytes)}; !zeroed.has_value()) return zeroed.error();
    if (const Result<void> headed{WriteHeader(fd, role, instance_id, 0, segment_bytes)}; !headed.has_value()) return headed.error();
    // Creation always uses the real strict commit; a hook only replaces the barriers' commit.
    if (const Result<void> committed{JournalCommit(fd)}; !committed.has_value()) {
        return committed.error();
    }
    if (const Result<void> dir_committed{CommitDirectory(empty_dir)}; !dir_committed.has_value()) {
        return dir_committed.error();
    }
    ScanResult fresh;
    fresh.last_chain = JournalGenesisChain(role, instance_id);
    fresh.end_offset = SEGMENT_HEADER_BYTES; // the first record follows the header
    auto journal{std::unique_ptr<FileJournal>{
        new FileJournal{role, instance_id, observer, empty_dir, std::vector<int>{fd}, segment_bytes, commit, &fresh}}};
    owned.Release();
    return Ok(std::move(journal));
}

// TODO(rebirth:M4-STO-01) strict recovery is real; high-water/key-retirement qualification remains.
Result<std::unique_ptr<FileJournal>> FileJournal::OpenExisting(const fs::path& dir, JournalRole role,
                                                               uint64_t instance_id)
{
    if (!IsKnown(role)) return Error{ErrorCode::INVALID_INPUT, "unknown journal role"};
    std::error_code ec;
    const fs::file_status dir_status{fs::symlink_status(dir, ec)};
    if (ec || !fs::is_directory(dir_status)) return Error{ErrorCode::INVALID_INPUT, JOURNAL_DIR_MISSING};

    // The layout: regular files 0..n-1 and at most the temporary file of n.
    std::vector<uint64_t> segments;
    std::vector<uint64_t> temporaries;
    for (fs::directory_iterator entry{dir, ec}, end; !ec && entry != end; entry.increment(ec)) {
        const fs::file_status status{fs::symlink_status(entry->path(), ec)};
        if (ec || !fs::is_regular_file(status)) return Error{ErrorCode::INVALID_INPUT, JOURNAL_SEGMENT_LAYOUT};
        const std::string name{fs::PathToString(entry->path().filename())};
        if (const std::optional<uint64_t> index{ParseSegmentName(name, "")}) {
            segments.push_back(*index);
        } else if (const std::optional<uint64_t> temp{ParseSegmentName(name, SEGMENT_TEMP_SUFFIX)}) {
            temporaries.push_back(*temp);
        } else {
            return Error{ErrorCode::INVALID_INPUT, JOURNAL_SEGMENT_LAYOUT};
        }
    }
    if (ec) return Error{ErrorCode::INVALID_INPUT, JOURNAL_SEGMENT_LAYOUT};
    std::sort(segments.begin(), segments.end());
    if (segments.empty()) return Error{ErrorCode::INVALID_INPUT, JOURNAL_SEGMENT_LAYOUT};
    for (size_t i{0}; i < segments.size(); ++i) {
        if (segments[i] != i) return Error{ErrorCode::INVALID_INPUT, JOURNAL_SEGMENT_LAYOUT};
    }
    if (temporaries.size() > 1 || (temporaries.size() == 1 && temporaries.front() != segments.size())) {
        return Error{ErrorCode::INVALID_INPUT, JOURNAL_SEGMENT_LAYOUT};
    }

    std::vector<int> fds;
    struct Closer {
        std::vector<int>& fds;
        bool armed{true};
        ~Closer() { if (armed) for (auto fd{fds.rbegin()}; fd != fds.rend(); ++fd) CloseFd(*fd); }
    } closer{fds};
    std::optional<SegmentScan> scan;
    uint64_t segment_bytes{0};
    for (size_t i{0}; i < segments.size(); ++i) {
        const int fd{OpenSegment(dir / fs::u8path(SegmentName(i)))};
        if (fd < 0) return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_OPEN};
        fds.push_back(fd);
        if (i == 0 && !LockSegment(fd)) return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_LOCKED};
        const Result<uint64_t> size{SegmentSize(fd)};
        if (!size.has_value()) return size.error();
        if (i == 0) {
            if (*size < SEGMENT_PAGE_BYTES || *size > MAX_SEGMENT_BYTES || *size % SEGMENT_PAGE_BYTES != 0) {
                return Error{ErrorCode::INVALID_INPUT, JOURNAL_SEGMENT_SIZE};
            }
            segment_bytes = *size;
            scan.emplace(role, instance_id, segment_bytes);
        } else if (*size != segment_bytes) {
            return Error{ErrorCode::INVALID_INPUT, JOURNAL_SEGMENT_SIZE};
        }
        const Result<std::vector<unsigned char>> image{ReadPrefix(fd, *size)};
        if (!image.has_value()) return image.error();
        if (const Result<void> fed{scan->Feed(*image)}; !fed.has_value()) return fed.error();
    }
    // A fully written record may have survived an unacknowledged barrier.
    // Commit the verified image now before it can mint any durable token.
    for (const int fd : fds) {
        if (const Result<void> committed{JournalCommit(fd)}; !committed.has_value()) return committed.error();
    }
    if (const Result<void> committed{CommitDirectory(dir)}; !committed.has_value()) return committed.error();
    auto journal{std::unique_ptr<FileJournal>{
        new FileJournal{role, instance_id, nullptr, dir, fds, segment_bytes, &JournalCommit, &scan->Out()}}};
    closer.armed = false;
    return Ok(std::move(journal));
}

std::vector<int> FileJournal::SegmentFds() const
{
    LOCK(m_segments_mutex);
    return m_fds;
}

size_t FileJournal::SegmentCount() const
{
    LOCK(m_segments_mutex);
    return m_fds.size();
}

Result<std::vector<JournalRecord>> FileJournal::ReadAll() const
{
    const uint64_t durable_bytes{DurableImageSize()};
    const std::vector<int> fds{SegmentFds()};
    SegmentScan scan{Role(), InstanceId(), m_segment_bytes};
    for (size_t k{0}; static_cast<uint64_t>(k) * m_segment_bytes < durable_bytes; ++k) {
        if (k >= fds.size()) return Error{ErrorCode::INVARIANT, JOURNAL_SEGMENT_LAYOUT};
        const Result<uint64_t> size{SegmentSize(fds[k])};
        if (!size.has_value()) return size.error();
        if (*size != m_segment_bytes) return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_SIZE};
        Result<std::vector<unsigned char>> image{
            ReadPrefix(fds[k], std::min(m_segment_bytes, durable_bytes - static_cast<uint64_t>(k) * m_segment_bytes))};
        if (!image.has_value()) return image.error();
        if (const Result<void> fed{scan.Feed(*image)}; !fed.has_value()) return fed.error();
    }
    // A zeroed suffix inside a previously committed prefix must never silently
    // turn an acknowledged history into a shorter valid journal.
    if (scan.Out().end_offset != durable_bytes) return Error{ErrorCode::VERIFY_FAILED, RECORD_TAIL};
    return Ok(std::move(scan.Out().records));
}

Result<int> FileJournal::SegmentFd(uint64_t index)
{
    {
        LOCK(m_segments_mutex);
        if (index < m_fds.size()) return Ok(m_fds[static_cast<size_t>(index)]);
        if (index != m_fds.size()) return Error{ErrorCode::INVARIANT, JOURNAL_SEGMENT_LAYOUT};
    }
    if (index >= MAX_SEGMENTS) return Error{ErrorCode::OVERSIZE, JOURNAL_SEGMENTS_EXHAUSTED};
    // Disk work without the lock; only the disk owner creates segments.
    const Result<int> created{CreateSegmentFile(m_dir, Role(), InstanceId(), index, m_segment_bytes)};
    if (!created.has_value()) return created.error();
    LOCK(m_segments_mutex);
    m_fds.push_back(*created);
    return created;
}

Result<void> FileJournal::Rollover()
{
    if (IsFatal()) return Error{ErrorCode::FENCED, JOURNAL_FENCED};
    int idle{0};
    if (!m_disk_owner.compare_exchange_strong(idle, 2)) return Error{ErrorCode::ORDER_VIOLATION, JOURNAL_BARRIER_CONCURRENT};
    // The segment the next durable record goes to (a full segment's end is the next one's start).
    const uint64_t next{DurableImageSize() / m_segment_bytes};
    const uint64_t count{SegmentCount()};
    Result<void> out{Ok()};
    if (count >= MAX_SEGMENTS) {
        out = Error{ErrorCode::OVERSIZE, JOURNAL_SEGMENTS_EXHAUSTED};
    } else if (count <= next + 1) {
        if (const Result<int> created{SegmentFd(count)}; !created.has_value()) out = created.error();
    }
    m_disk_owner.store(0);
    m_disk_owner.notify_all();
    return out;
}

Result<uint64_t> FileJournal::AdmitAppend(uint64_t /*append_number*/, uint64_t offset, size_t size) const
{
    // A record never spans two segments and never overlaps a header (M4-STO-06).
    if (size > m_segment_bytes - SEGMENT_HEADER_BYTES) return Error{ErrorCode::OVERSIZE, JOURNAL_RECORD_SEGMENT};
    const uint64_t used{offset % m_segment_bytes};
    uint64_t padding{0};
    if (used < SEGMENT_HEADER_BYTES) {
        padding = SEGMENT_HEADER_BYTES - used; // at a segment's start: after its header
    } else if (size > m_segment_bytes - used) {
        padding = m_segment_bytes - used + SEGMENT_HEADER_BYTES; // the next segment, after its header
    }
    if ((offset + padding) / m_segment_bytes >= MAX_SEGMENTS) return Error{ErrorCode::OVERSIZE, JOURNAL_SEGMENTS_EXHAUSTED};
    return Ok(padding);
}

Result<void> FileJournal::Persist(uint64_t /*barrier_number*/, uint64_t offset, std::span<const unsigned char> bytes)
{
    // Wait for a running Rollover; barriers never overlap (the base refuses a second one).
    for (int idle{0}; !m_disk_owner.compare_exchange_strong(idle, 1); idle = 0) m_disk_owner.wait(idle);
    Result<void> out{Ok()};
    if (bytes.empty()) {
        // Nothing to write: commit the segment of `offset` (the last one at its end).
        const uint64_t index{std::min<uint64_t>(offset / m_segment_bytes, SegmentCount() - 1)};
        const Result<int> fd{SegmentFd(index)};
        out = fd.has_value() ? m_commit(*fd) : Result<void>{fd.error()};
    }
    // Segment by segment, each committed before the next is written.
    while (out.has_value() && !bytes.empty()) {
        const uint64_t index{offset / m_segment_bytes};
        const uint64_t within{offset % m_segment_bytes};
        if (within < SEGMENT_HEADER_BYTES) {
            // Queued padding over a header: zeros by construction (AdmitAppend), never written.
            const size_t skip{static_cast<size_t>(std::min<uint64_t>(bytes.size(), SEGMENT_HEADER_BYTES - within))};
            if (!std::all_of(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(skip), [](unsigned char b) { return b == 0; })) {
                out = Error{ErrorCode::INVARIANT, JOURNAL_SEGMENT_HEADER};
                break;
            }
            bytes = bytes.subspan(skip);
            offset += skip;
            continue;
        }
        const size_t chunk{static_cast<size_t>(std::min<uint64_t>(bytes.size(), m_segment_bytes - within))};
        const Result<int> fd{SegmentFd(index)};
        if (!fd.has_value()) {
            out = fd.error();
            break;
        }
        if (const Result<void> written{WriteAllAt(*fd, bytes.first(chunk), within)}; !written.has_value()) {
            out = written.error();
            break;
        }
        out = m_commit(*fd);
        bytes = bytes.subspan(chunk);
        offset += chunk;
    }
    m_disk_owner.store(0);
    m_disk_owner.notify_all();
    return out;
}

} // namespace flowmesh2::storage
