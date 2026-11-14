// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/cli/signer.h>

#include <flowmesh2/netio/api_json.h>
#include <random.h>
#include <support/cleanse.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <optional>
#include <string_view>

namespace flowmesh2::cli {

ClientKey::ClientKey(CKey key) : m_key{std::move(key)}, m_xonly{m_key.GetPubKey()}, m_account{AccountId::FromKey(m_xonly)} {}

Result<ClientKey> ClientKey::Generate(const fs::path& file)
{
    if (fs::exists(file)) return Error{ErrorCode::INVALID_INPUT, KEY_EXISTS};
    CKey key;
    key.MakeNewKey(true);
    std::array<unsigned char, 32> bytes{};
    std::copy(key.begin(), key.end(), reinterpret_cast<std::byte*>(bytes.data()));
    std::string line{netio::ToHex(bytes) + "\n"};
    memory_cleanse(bytes.data(), bytes.size());
    const int fd{::open(fs::PathToString(file).c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600)};
    if (fd < 0) {
        memory_cleanse(line.data(), line.size());
        return Error{ErrorCode::STORAGE_FATAL, KEY_WRITE};
    }
    const bool ok{::write(fd, line.data(), line.size()) == static_cast<ssize_t>(line.size()) && ::fsync(fd) == 0};
    ::close(fd);
    memory_cleanse(line.data(), line.size());
    if (!ok) return Error{ErrorCode::STORAGE_FATAL, KEY_WRITE};
    return Ok(ClientKey{key});
}

Result<ClientKey> ClientKey::Load(const fs::path& file)
{
    struct stat st{};
    if (::stat(fs::PathToString(file).c_str(), &st) != 0) return Error{ErrorCode::DECODE, KEY_FILE};
    if ((st.st_mode & (S_IRWXG | S_IRWXO)) != 0) return Error{ErrorCode::INVALID_INPUT, KEY_MODE};
    // Read with the C API into one fixed buffer, so no library copy of the secret survives this function.
    const int fd{::open(fs::PathToString(file).c_str(), O_RDONLY)};
    if (fd < 0) return Error{ErrorCode::DECODE, KEY_FILE};
    std::array<char, 128> text{};
    const ssize_t got{::read(fd, text.data(), text.size())};
    ::close(fd);
    if (got <= 0 || static_cast<size_t>(got) == text.size()) return Error{ErrorCode::DECODE, KEY_FILE}; // a key file is 65 bytes
    std::string_view hex{text.data(), static_cast<size_t>(got)};
    while (!hex.empty() && (hex.back() == '\n' || hex.back() == '\r' || hex.back() == ' ')) hex.remove_suffix(1);
    std::optional<std::vector<unsigned char>> bytes{netio::FromHex(hex)};
    memory_cleanse(text.data(), text.size());
    if (!bytes.has_value() || bytes->size() != 32) {
        if (bytes.has_value()) memory_cleanse(bytes->data(), bytes->size());
        return Error{ErrorCode::DECODE, KEY_FILE};
    }
    CKey key;
    key.Set(bytes->begin(), bytes->end(), true);
    memory_cleanse(bytes->data(), bytes->size());
    if (!key.IsValid()) return Error{ErrorCode::INVALID_INPUT, KEY_INVALID};
    return Ok(ClientKey{key});
}

Result<std::vector<unsigned char>> ClientKey::SignAction(ActionV2 action) const
{
    action.signer = m_xonly;
    const ActionId id{action.Id()};
    const uint256 aux{GetRandHash()};
    if (!m_key.SignSchnorr(id.id, action.sig, /*merkle_root=*/nullptr, aux)) return Error{ErrorCode::INVARIANT, KEY_SIGN};
    return Ok(action.Encode());
}

} // namespace flowmesh2::cli
