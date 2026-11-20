// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/storage/file_journal.h>

#include <flowmesh2/storage/journal_commit.h>

#include <crypto/common.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <limits>
#include <optional>
#include <system_error>
#include <utility>

#ifdef WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace flowmesh2::storage {

namespace {

//! Write all of `bytes` at `offset`; STORAGE_FATAL on any error or zero-length write.
Result<void> WriteAllAt(int fd, std::span<const unsigned char> bytes, uint64_t offset)
{
#ifdef WIN32
    if (_lseeki64(fd, static_cast<__int64>(offset), SEEK_SET) < 0) return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_WRITE};
    while (!bytes.empty()) {
        const unsigned int chunk{static_cast<unsigned int>(std::min<size_t>(bytes.size(), 1U << 30))};
        const int written{_write(fd, bytes.data(), chunk)};
        if (written <= 0) return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_WRITE};
        bytes = bytes.subspan(static_cast<size_t>(written));
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

//! Read the whole file at `path` (at most `max_bytes`); STORAGE_FATAL if it cannot be opened or read.
Result<std::vector<unsigned char>> ReadWholeFile(const fs::path& path, uint64_t max_bytes)
{
#ifdef WIN32
    const int fd{_wopen(path.c_str(), _O_RDONLY | _O_BINARY)};
#else
    const int fd{open(path.c_str(), O_RDONLY | O_CLOEXEC)};
#endif
    if (fd < 0) return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_READ};
    std::vector<unsigned char> out;
    std::array<unsigned char, 64 * 1024> buffer{};
    while (out.size() < max_bytes) {
#ifdef WIN32
        const int got{_read(fd, buffer.data(), static_cast<unsigned int>(std::min<uint64_t>(buffer.size(), max_bytes - out.size())))};
#else
        const ssize_t got{read(fd, buffer.data(), static_cast<size_t>(std::min<uint64_t>(buffer.size(), max_bytes - out.size())))};
#endif
        if (got < 0) {
#ifdef WIN32
            _close(fd);
#else
            close(fd);
#endif
            return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_READ};
        }
        if (got == 0) break;
        out.insert(out.end(), buffer.begin(), buffer.begin() + got);
    }
#ifdef WIN32
    _close(fd);
#else
    close(fd);
#endif
    return Ok(std::move(out));
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
    if (fd < 0) return;
#ifdef WIN32
    _close(fd);
#else
    close(fd);
#endif
}

//! Whether `path` is a regular file. A stat error other than "it does not exist" stays in `ec`.
bool RegularFileExists(const fs::path& path, std::error_code& ec)
{
    const std::filesystem::file_status status{std::filesystem::status(path, ec)};
    if (ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory) ec.clear();
    return !ec && std::filesystem::is_regular_file(status);
}

//! The directory's lock file: a FileJournal holds an exclusive advisory lock on it for its lifetime.
constexpr std::string_view LOCK_FILE_NAME{".lock"};
//! The name a segment is created and filled under before it is renamed into place.
std::string TemporarySegmentName(uint64_t index) { return SegmentFileName(index) + ".tmp"; }
//! The index of `<8 hex digits>.seg`, or nullopt for any other name.
std::optional<uint64_t> SegmentIndexOf(std::string_view name)
{
    if (name.size() != 12 || !name.ends_with(".seg")) return std::nullopt;
    uint64_t index{0};
    for (const char c : name.substr(0, 8)) {
        if (c >= '0' && c <= '9') {
            index = index * 16 + static_cast<uint64_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            index = index * 16 + static_cast<uint64_t>(c - 'a' + 10);
        } else {
            return std::nullopt;
        }
    }
    return index;
}

//! Create or open `<dir>/.lock` and take an exclusive, non-blocking lock on it (review of 2026-11-16, defect 6):
//! BUSY JOURNAL_DIR_LOCKED while another FileJournal, in this process or another, holds it.
Result<int> LockJournalDir(const fs::path& dir)
{
    const fs::path lock_path{dir / fs::u8path(std::string{LOCK_FILE_NAME})};
#ifdef WIN32
    const int fd{_wopen(lock_path.c_str(), _O_RDWR | _O_CREAT | _O_BINARY, _S_IREAD | _S_IWRITE)};
    if (fd < 0) return Error{ErrorCode::STORAGE_FATAL, JOURNAL_DIR_LOCK};
    return Ok(fd); // no advisory lock on this branch, which is not compiled on this host (O-24 pattern)
#else
    const int fd{open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600)};
    if (fd < 0) return Error{ErrorCode::STORAGE_FATAL, JOURNAL_DIR_LOCK};
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int saved{errno};
        close(fd);
        if (saved == EWOULDBLOCK) return Error{ErrorCode::BUSY, JOURNAL_DIR_LOCKED};
        return Error{ErrorCode::STORAGE_FATAL, JOURNAL_DIR_LOCK};
    }
    return Ok(fd);
#endif
}

} // namespace

std::string SegmentFileName(uint64_t index)
{
    static constexpr char DIGITS[]{"0123456789abcdef"};
    std::string name(8, '0');
    for (size_t i{0}; i < 8; ++i) {
        name[7 - i] = DIGITS[index & 0xf];
        index >>= 4;
    }
    return name + ".seg";
}

FileJournal::FileJournal(JournalRole role, uint64_t instance_id, BarrierObserver* observer, fs::path dir, int lock_fd, int first_fd,
                         uint64_t segment_bytes, SegmentCommitHook commit)
    : Journal(ImplKey{}, role, instance_id, observer),
      m_dir{std::move(dir)},
      m_first_segment_path{m_dir / fs::u8path(FIRST_SEGMENT_NAME)},
      m_segment_bytes{segment_bytes},
      m_commit{commit},
      m_lock_fd{lock_fd},
      m_fds{first_fd}
{
}

FileJournal::FileJournal(JournalRole role, uint64_t instance_id, BarrierObserver* observer, fs::path dir, int lock_fd, std::vector<int> fds,
                         uint64_t segment_bytes, const RecoveredState& recovered, const RecoveryReport& report)
    : Journal(ImplKey{}, role, instance_id, observer, recovered),
      m_dir{std::move(dir)},
      m_first_segment_path{m_dir / fs::u8path(FIRST_SEGMENT_NAME)},
      m_segment_bytes{segment_bytes},
      m_commit{&JournalCommit},
      m_recovery{report},
      m_lock_fd{lock_fd},
      m_fds{std::move(fds)}
{
}

FileJournal::~FileJournal()
{
    for (const int fd : m_fds) CloseFd(fd);
    CloseFd(m_lock_fd); // releases the directory lock
}

fs::path FileJournal::SegmentPathOf(uint64_t index) const { return m_dir / fs::u8path(SegmentFileName(index)); }

uint64_t FileJournal::SegmentCount() const
{
    LOCK(m_segments_mutex);
    return m_fds.size();
}

uint64_t FileJournal::CurrentSegment() const
{
    const uint64_t end{ImageEnd()};
    LOCK(m_segments_mutex);
    uint64_t segment{end / m_segment_bytes};
    if (m_rollover_requested && end % m_segment_bytes != 0) ++segment;
    return segment;
}

Result<int> FileJournal::CreateSegment(const fs::path& dir, uint64_t index, uint64_t segment_bytes)
{
    const fs::path segment_path{dir / fs::u8path(SegmentFileName(index))};
    // Filled and committed under a temporary name, then renamed into place: a crash during the fill leaves a `.tmp`
    // that the next OpenExisting removes, never a short segment that blocks every later open (review 2026-11-16, R7).
    const fs::path temporary_path{dir / fs::u8path(TemporarySegmentName(index))};
    std::error_code ec;
    if (RegularFileExists(segment_path, ec) || ec) return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_CREATE}; // exists already
    fs::remove(temporary_path, ec); // a leftover of an earlier crash, if any
    const int fd{OpenNewSegment(temporary_path)};
    if (fd < 0) return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_CREATE};

    // Explicit zero pages for the whole segment (plan section 3.7, S-9): the
    // blocks are allocated now, not on the first barrier, and the unused tail
    // reads back as zeros.
    static constexpr std::array<unsigned char, 64 * 1024> ZEROS{};
    for (uint64_t offset{0}; offset < segment_bytes; offset += ZEROS.size()) {
        const size_t chunk{static_cast<size_t>(std::min<uint64_t>(ZEROS.size(), segment_bytes - offset))};
        if (const Result<void> written{WriteAllAt(fd, std::span{ZEROS}.first(chunk), offset)}; !written.has_value()) {
            CloseFd(fd);
            return written.error();
        }
    }
    // Creation always uses the real strict commit; a hook only replaces the barriers' commit.
    if (const Result<void> committed{JournalCommit(fd)}; !committed.has_value()) {
        CloseFd(fd);
        return committed.error();
    }
    fs::rename(temporary_path, segment_path, ec);
    if (ec) {
        CloseFd(fd);
        fs::remove(temporary_path, ec);
        return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_CREATE};
    }
    if (const Result<void> dir_committed{CommitDirectory(dir)}; !dir_committed.has_value()) {
        CloseFd(fd);
        return dir_committed.error();
    }
    return Ok(fd);
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

    const Result<int> lock{LockJournalDir(empty_dir)};
    if (!lock.has_value()) return lock.error();
    const Result<int> fd{CreateSegment(empty_dir, 0, segment_bytes)};
    if (!fd.has_value()) {
        CloseFd(*lock);
        fs::remove(empty_dir / fs::u8path(std::string{LOCK_FILE_NAME}), ec); // the directory is empty again
        return fd.error();
    }
    return Ok(std::unique_ptr<FileJournal>{new FileJournal{role, instance_id, observer, empty_dir, *lock, *fd, segment_bytes, commit}});
}

namespace {

bool AllZeroBytes(std::span<const unsigned char> bytes)
{
    return std::all_of(bytes.begin(), bytes.end(), [](unsigned char b) { return b == 0; });
}

int OpenSegmentForWriting(const fs::path& path)
{
#ifdef WIN32
    return _wopen(path.c_str(), _O_RDWR | _O_BINARY);
#else
    return open(path.c_str(), O_RDWR | O_CLOEXEC);
#endif
}

} // namespace

Result<std::unique_ptr<FileJournal>> FileJournal::OpenExisting(const fs::path& dir, JournalRole role, uint64_t instance_id,
                                                               BarrierObserver* observer)
{
    if (!IsKnown(role)) return Error{ErrorCode::INVALID_INPUT, "unknown journal role"};
    std::error_code ec;
    if (!fs::is_directory(dir, ec) || ec) return Error{ErrorCode::INVALID_INPUT, JOURNAL_DIR_MISSING};
    // The lock first: nothing is read, repaired or written on a directory another journal holds.
    const Result<int> lock{LockJournalDir(dir)};
    if (!lock.has_value()) return lock.error();
    Result<std::unique_ptr<FileJournal>> opened{OpenLocked(dir, role, instance_id, observer, *lock)};
    if (!opened.has_value()) CloseFd(*lock);
    return opened;
}

Result<std::unique_ptr<FileJournal>> FileJournal::OpenLocked(const fs::path& dir, JournalRole role, uint64_t instance_id,
                                                             BarrierObserver* observer, int lock_fd)
{
    std::error_code ec;
    // Leftovers of a crash during a segment's creation are removed; the highest segment number present decides below
    // whether the numbering has a gap.
    std::vector<fs::path> leftovers;
    std::optional<uint64_t> highest_index;
    for (fs::directory_iterator it{dir, ec}; !ec && it != fs::directory_iterator{}; it.increment(ec)) {
        const std::string name{fs::PathToString(it->path().filename())};
        if (name.ends_with(".seg.tmp")) leftovers.push_back(it->path());
        if (const std::optional<uint64_t> index{SegmentIndexOf(name)}) highest_index = std::max(highest_index.value_or(0), *index);
    }
    if (ec) return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_READ};
    for (const fs::path& leftover : leftovers) {
        if (!fs::remove(leftover, ec) || ec) return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_CREATE};
    }
    const fs::path first{dir / fs::u8path(FIRST_SEGMENT_NAME)};
    if (!fs::is_regular_file(first, ec) || ec) return Error{ErrorCode::INVALID_INPUT, JOURNAL_SEGMENT_MISSING};
    const uint64_t segment_bytes{fs::file_size(first, ec)};
    if (ec || segment_bytes < SEGMENT_PAGE_BYTES || segment_bytes > MAX_SEGMENT_BYTES || segment_bytes % SEGMENT_PAGE_BYTES != 0) {
        return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_SIZE};
    }
    // The segments: consecutively numbered, every one of the first's size.
    std::vector<std::vector<unsigned char>> images;
    for (uint64_t index{0};; ++index) {
        const fs::path path{dir / fs::u8path(SegmentFileName(index))};
        const bool present{RegularFileExists(path, ec)};
        if (ec) return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_READ}; // a stat error is not the end of the numbering
        if (!present) break;                                                     // the numbering ends here
        if (fs::file_size(path, ec) != segment_bytes || ec) return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_SIZE};
        Result<std::vector<unsigned char>> image{ReadWholeFile(path, segment_bytes)};
        if (!image.has_value()) return image.error();
        if (image->size() != segment_bytes) return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_SIZE};
        images.push_back(std::move(image).value());
    }
    // A segment numbered beyond the first missing one: a gap. The records after it would be forgotten silently (for a
    // signer journal: votes it signed), so the journal is refused as the file comment promises (review 2026-11-16, defect 7).
    if (highest_index.has_value() && *highest_index >= images.size()) return Error{ErrorCode::INVALID_INPUT, JOURNAL_SEGMENT_MISSING};
    // Scan in order, the chain continued across segments; the first refusal ends the journal.
    RecoveredState recovered;
    recovered.last_chain = JournalGenesisChain(role, instance_id);
    RecoveryReport report;
    std::optional<std::pair<uint64_t, size_t>> torn; // (segment, start) of the torn bytes to zero
    for (uint64_t index{0}; index < images.size(); ++index) {
        const std::vector<unsigned char>& image{images[index]};
        const PartialScan scan{ScanRecordsPartial(image, role, recovered.last_chain, recovered.records)};
        recovered.records += scan.accepted.records.size();
        recovered.last_chain = scan.accepted.last_chain;
        if (!scan.accepted.records.empty()) recovered.image_end = index * segment_bytes + scan.accepted.end_offset;
        if (!scan.refusal.has_value()) continue;
        const Error refusal{*scan.refusal};
        const size_t p{scan.refused_at};
        // The first record of the journal that is not this role's or instance's: another journal's files.
        if (index == 0 && p == 0 && (refusal.detail == RECORD_ROLE || refusal.detail == RECORD_CHAIN)) {
            return Error{ErrorCode::VERIFY_FAILED, JOURNAL_IDENTITY_MISMATCH};
        }
        const Error corrupt{ErrorCode::STORAGE_FATAL, role == JournalRole::SIGNER ? SIGNER_KEY_RETIRED : JOURNAL_CORRUPT};
        if (refusal.detail != RECORD_LENGTH && refusal.detail != RECORD_CRC) return corrupt; // chain, role, type, tail: never a torn write
        // q: one past the last non-zero byte of this segment.
        size_t q{image.size()};
        while (q > p && image[q - 1] == 0) --q;
        if (q <= p) return corrupt; // cannot happen: a refusal needs a non-zero byte
        const std::span<const unsigned char> broken{std::span{image}.subspan(p, q - p)};
        bool within_one_record{broken.size() < RECORD_HEADER_BYTES};
        if (!within_one_record) {
            const uint32_t len{ReadLE32(broken.data())};
            within_one_record = len <= MAX_RECORD_PAYLOAD_BYTES && broken.size() <= RECORD_OVERHEAD_BYTES + size_t{len};
        }
        if (!within_one_record) return corrupt;
        for (uint64_t later{index + 1}; later < images.size(); ++later) {
            if (!AllZeroBytes(images[later])) return corrupt;
        }
        torn = std::pair{index, p};
        report.truncated_bytes = broken.size();
        break;
    }
    report.records = recovered.records;
    // Descriptors: the segment holding the next write and every later one; earlier ones stay closed.
    const uint64_t current{recovered.image_end / segment_bytes};
    std::vector<int> fds(images.size(), -1);
    const auto close_all = [&fds] {
        for (const int fd : fds) CloseFd(fd);
    };
    for (uint64_t index{current}; index < images.size(); ++index) {
        fds[index] = OpenSegmentForWriting(dir / fs::u8path(SegmentFileName(index)));
        if (fds[index] < 0) {
            close_all();
            return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_CREATE};
        }
    }
    // Zero the torn bytes again and commit, so the image is exactly the clean records plus zeros.
    if (torn.has_value()) {
        const auto [index, start]{*torn};
        if (fds[index] < 0) {
            fds[index] = OpenSegmentForWriting(dir / fs::u8path(SegmentFileName(index)));
            if (fds[index] < 0) {
                close_all();
                return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_CREATE};
            }
        }
        const std::vector<unsigned char> zeros(static_cast<size_t>(report.truncated_bytes), 0);
        if (const Result<void> written{WriteAllAt(fds[index], zeros, start)}; !written.has_value()) {
            close_all();
            return written.error();
        }
        if (const Result<void> committed{JournalCommit(fds[index])}; !committed.has_value()) {
            close_all();
            return committed.error();
        }
        if (index != current) {
            CloseFd(fds[index]);
            fds[index] = -1;
        }
    }
    // The spare, as after a barrier: the segment the next record can go to exists before the journal does.
    const uint64_t end{recovered.image_end};
    const uint64_t needed{end / segment_bytes + (end % segment_bytes == 0 ? 0 : 1)};
    while (fds.size() <= needed) {
        const Result<int> fd{CreateSegment(dir, fds.size(), segment_bytes)};
        if (!fd.has_value()) {
            close_all();
            return fd.error();
        }
        fds.push_back(*fd);
    }
    return Ok(std::unique_ptr<FileJournal>{new FileJournal{role, instance_id, observer, dir, lock_fd, std::move(fds), segment_bytes, recovered, report}});
}

Result<std::vector<JournalRecord>> FileJournal::ReadAll() const
{
    const uint64_t durable{DurableSeq()};
    uint64_t segments{0};
    {
        LOCK(m_segments_mutex);
        segments = m_fds.size();
    }
    std::vector<JournalRecord> out;
    uint256 prev{JournalGenesisChain(Role(), InstanceId())};
    for (uint64_t index{0}; index < segments && out.size() < durable; ++index) {
        const Result<std::vector<unsigned char>> image{ReadWholeFile(SegmentPathOf(index), m_segment_bytes)};
        if (!image.has_value()) return image.error();
        Result<ScanResult> scan{ScanRecordsFrom(*image, Role(), prev, out.size())};
        if (!scan.has_value()) return scan.error();
        out.insert(out.end(), std::make_move_iterator(scan->records.begin()), std::make_move_iterator(scan->records.end()));
        prev = scan->last_chain;
    }
    // A barrier in flight may have written records that are not durable yet.
    if (out.size() > durable) out.resize(static_cast<size_t>(durable));
    return Ok(std::move(out));
}

Result<void> FileJournal::EnsureSegments(uint64_t index)
{
    for (;;) {
        uint64_t next{0};
        {
            LOCK(m_segments_mutex);
            if (m_fds.size() > index) return Ok();
            next = m_fds.size();
        }
        // I/O without any fm2 lock: creation is the owner thread's (Barrier, Rollover).
        const Result<int> fd{CreateSegment(m_dir, next, m_segment_bytes)};
        if (!fd.has_value()) return fd.error();
        LOCK(m_segments_mutex);
        if (m_fds.size() != next) {
            CloseFd(*fd);
            return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_CREATE}; // two creators: not the contract
        }
        m_fds.push_back(*fd);
    }
}

Result<void> FileJournal::Rollover()
{
    if (IsFatal()) return Error{ErrorCode::FENCED, JOURNAL_FENCED};
    const uint64_t end{ImageEnd()};
    if (end % m_segment_bytes == 0) return Ok(); // at a segment start already: nothing to pad
    if (const Result<void> created{EnsureSegments(end / m_segment_bytes + 1)}; !created.has_value()) {
        SetFatal();
        return Error{ErrorCode::STORAGE_FATAL, created.error().detail};
    }
    LOCK(m_segments_mutex);
    m_rollover_requested = true;
    return Ok();
}

Result<uint64_t> FileJournal::AdmitAppend(uint64_t /*append_number*/, uint64_t offset, size_t size)
{
    if (size > m_segment_bytes) return Error{ErrorCode::OVERSIZE, JOURNAL_RECORD_EXCEEDS_SEGMENT};
    LOCK(m_segments_mutex);
    const uint64_t in_segment{offset % m_segment_bytes};
    uint64_t place{offset};
    if ((m_rollover_requested && in_segment != 0) || size > m_segment_bytes - in_segment) {
        place = (offset / m_segment_bytes + 1) * m_segment_bytes; // byte 0 of the next segment
    }
    if (place / m_segment_bytes >= m_fds.size()) return Error{ErrorCode::BUSY, JOURNAL_SEGMENT_FULL};
    m_rollover_requested = false;
    return Ok(place);
}

Result<void> FileJournal::Persist(uint64_t /*barrier_number*/, uint64_t offset, std::span<const unsigned char> bytes)
{
    std::vector<int> fds;
    {
        LOCK(m_segments_mutex);
        fds = m_fds;
    }
    // Write, split by segment; a record never straddles one, but the batch may.
    std::vector<uint64_t> touched;
    uint64_t pos{offset};
    std::span<const unsigned char> rest{bytes};
    while (!rest.empty()) {
        const uint64_t segment{pos / m_segment_bytes};
        const uint64_t in_segment{pos % m_segment_bytes};
        const size_t chunk{static_cast<size_t>(std::min<uint64_t>(rest.size(), m_segment_bytes - in_segment))};
        if (segment >= fds.size() || fds[segment] < 0) return Error{ErrorCode::STORAGE_FATAL, JOURNAL_SEGMENT_WRITE};
        if (const Result<void> written{WriteAllAt(fds[segment], rest.first(chunk), in_segment)}; !written.has_value()) {
            return written.error();
        }
        if (touched.empty() || touched.back() != segment) touched.push_back(segment);
        pos += chunk;
        rest = rest.subspan(chunk);
    }
    // Every barrier commits, also with nothing new: then the segment the next byte goes to.
    if (touched.empty()) touched.push_back(std::min<uint64_t>(offset / m_segment_bytes, fds.size() - 1));
    for (const uint64_t segment : touched) {
        if (const Result<void> committed{m_commit(fds[segment])}; !committed.has_value()) return committed.error();
    }
    // The spare: after every barrier the segment the next record can go to
    // exists. At a segment start that is the segment itself (any record fits
    // a fresh segment); elsewhere also the one after it.
    const uint64_t end{offset + bytes.size()};
    const uint64_t needed{end / m_segment_bytes + (end % m_segment_bytes == 0 ? 0 : 1)};
    if (const Result<void> created{EnsureSegments(needed)}; !created.has_value()) return created.error();
    // Segments entirely before the next write are never written again.
    LOCK(m_segments_mutex);
    for (uint64_t segment{0}; segment < end / m_segment_bytes && segment < m_fds.size(); ++segment) {
        CloseFd(m_fds[segment]);
        m_fds[segment] = -1;
    }
    return Ok();
}

} // namespace flowmesh2::storage
