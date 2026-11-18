// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license.
#ifndef B3COIN_BRIDGE_GLOAS_JOURNAL_H
#define B3COIN_BRIDGE_GLOAS_JOURNAL_H
#include <bridge/gloas_peer_light_client.h>
#include <filesystem>
#include <stdexcept>
#include <string>
#ifndef WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
namespace bridge::peer::gloas::journal {
inline constexpr size_t MAX_JSON_BYTES{262'144};
/** Linux/macOS owned proof journal. No symlink component, hardlink alias,
 * unsafe writable directory, shared file, or parallel writer is accepted.
 * Root-owned sticky temporary-directory ancestors are permitted; the immediate
 * journal directory must belong to the caller and forbid other writers.
 * The entire proof stream is replayed; corrupt/truncated tails are not pruned.
 */
class File final {
public:
    File(const std::string& path, bool journal)
    {
        const std::filesystem::path target{path};
        if (!target.is_absolute() || target.lexically_normal() != target ||
            target.filename().empty()) throw std::runtime_error("ABSOLUTE_NORMAL_PATH_REQUIRED");
        int directory{::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
        if (directory < 0) throw std::runtime_error("DIRECTORY_OPEN_FAILED");
        const auto safe_ancestor{[](int fd) {
            struct stat info{};
            if (::fstat(fd, &info) || (info.st_uid != ::getuid() && info.st_uid != 0)) return false;
            return !(info.st_mode & 0022) || (info.st_uid == 0 && (info.st_mode & S_ISVTX));
        }};
        if (!safe_ancestor(directory)) { ::close(directory); throw std::runtime_error("UNSAFE_PATH_COMPONENT"); }
        const auto parent{target.parent_path()};
        for (const auto& part : parent.relative_path()) {
            const int next{::openat(directory, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
            ::close(directory);
            if (next < 0) throw std::runtime_error("UNSAFE_PATH_COMPONENT");
            if (!safe_ancestor(next)) { ::close(next); throw std::runtime_error("UNSAFE_PATH_COMPONENT"); }
            directory = next;
        }
        struct stat parent_stat{};
        if (::fstat(directory, &parent_stat) ||
            (parent_stat.st_uid != ::getuid() && parent_stat.st_uid != 0) ||
            (parent_stat.st_mode & 0022) || (journal && parent_stat.st_uid != ::getuid())) {
            ::close(directory);
            throw std::runtime_error("UNSAFE_JOURNAL_DIRECTORY");
        }
        bool created{false};
        if (journal) {
            m_fd = ::openat(directory, target.filename().c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_CREAT | O_EXCL, 0600);
            if (m_fd >= 0) created = true;
            else if (errno == EEXIST) m_fd = ::openat(directory, target.filename().c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC);
        } else m_fd = ::openat(directory, target.filename().c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (m_fd < 0) { ::close(directory); throw std::runtime_error("FILE_OPEN_FAILED"); }
        struct stat info{};
        const bool unsafe{::fstat(m_fd, &info) || !S_ISREG(info.st_mode) || info.st_nlink != 1 ||
                          (info.st_uid != ::getuid() && info.st_uid != 0) || (info.st_mode & 0022) ||
                          (journal && (info.st_uid != ::getuid() || (info.st_mode & 0077)))};
        if (unsafe || (journal && ::flock(m_fd, LOCK_EX | LOCK_NB))) {
            ::close(directory);
            ::close(m_fd); m_fd = -1;
            throw std::runtime_error("UNSAFE_OR_LOCKED_FILE");
        }
        if (journal && ((created && (::fsync(m_fd) || ::fsync(directory))) ||
                        (!created && info.st_size == 0))) {
            ::close(directory);
            ::close(m_fd); m_fd = -1;
            throw std::runtime_error("EMPTY_OR_UNCOMMITTED_JOURNAL");
        }
        ::close(directory);
    }
    ~File() { if (m_fd >= 0) ::close(m_fd); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    bool ReadExact(std::span<unsigned char> output, bool eof_allowed = false)
    {
        size_t done{0};
        while (done < output.size()) {
            const auto got{::read(m_fd, output.data() + done, output.size() - done)};
            if (got < 0 && errno == EINTR) continue;
            if (got == 0 && !done && eof_allowed) return false;
            if (got <= 0) throw std::runtime_error("TRUNCATED_OR_UNREADABLE_FILE");
            done += static_cast<size_t>(got);
        }
        return true;
    }
    std::string ReadJson()
    {
        struct stat info{};
        if (::fstat(m_fd, &info) || info.st_size <= 0 || info.st_size > static_cast<off_t>(MAX_JSON_BYTES)) throw std::runtime_error("JSON_SIZE_LIMIT");
        std::string text(static_cast<size_t>(info.st_size), '\0');
        ReadExact(std::span<unsigned char>{reinterpret_cast<unsigned char*>(text.data()), text.size()});
        std::array<unsigned char, 1> extra{};
        if (ReadExact(extra, true)) throw std::runtime_error("POLICY_CHANGED_DURING_READ");
        return text;
    }
    bool Append(std::span<const unsigned char> entry)
    {
        size_t done{0};
        while (done < entry.size()) {
            const auto wrote{::write(m_fd, entry.data() + done, entry.size() - done)};
            if (wrote < 0 && errno == EINTR) continue;
            if (wrote <= 0) return false;
            done += static_cast<size_t>(wrote);
        }
        return ::fsync(m_fd) == 0;
    }
private:
    int m_fd{-1};
};

inline void Replay(File& journal, LightClient& client)
{
    for (;;) {
        std::array<unsigned char, ENTRY_PREFIX_SIZE> prefix{};
        if (!journal.ReadExact(prefix, true)) break;
        const size_t n{prefix.size()};
        const uint32_t size{(uint32_t{prefix[n - 4]} << 24) | (uint32_t{prefix[n - 3]} << 16) |
                             (uint32_t{prefix[n - 2]} << 8) | uint32_t{prefix[n - 1]}};
        if (size < 2 || size > MAX_RECORD_BYTES) throw std::runtime_error("INVALID_JOURNAL_LENGTH");
        std::vector<unsigned char> entry{prefix.begin(), prefix.end()};
        entry.resize(prefix.size() + size + 32);
        journal.ReadExact(std::span<unsigned char>{entry}.subspan(prefix.size()));
        if (client.ReplayEntry(entry).result != Result::OK) throw std::runtime_error("JOURNAL_VERIFICATION_FAILED");
    }
    if (!client.EndReplay()) throw std::runtime_error("JOURNAL_FENCED");
}
} // namespace bridge::peer::gloas::journal
#endif
#endif
