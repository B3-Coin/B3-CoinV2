// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/dev/dev_chain_file.h>

#include <crc32c/crc32c.h>
#include <crypto/sha256.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <string>

namespace flowmesh2::dev {

namespace {

void PutU32(std::vector<unsigned char>& out, uint32_t v)
{
    for (int i{0}; i < 4; ++i) out.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xff));
}
uint32_t GetU32(const unsigned char* p)
{
    uint32_t v{0};
    for (int i{0}; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
    return v;
}
uint256 Label(const std::string& label)
{
    uint256 out;
    CSHA256{}.Write(reinterpret_cast<const unsigned char*>(label.data()), label.size()).Finalize(out.data());
    return out;
}

//! Parse every complete record of `bytes` from `offset`; stops at a partial tail; CORRUPT on a bad CRC or shape.
Result<std::vector<DevChainBlock>> ParseRecords(const std::vector<unsigned char>& bytes, uint64_t& offset)
{
    std::vector<DevChainBlock> blocks;
    while (offset + 8 <= bytes.size()) {
        const uint32_t len{GetU32(bytes.data() + offset)};
        const uint32_t crc{GetU32(bytes.data() + offset + 4)};
        if (len == 0 || len > DEVCHAIN_MAX_RECORD_BYTES) return Error{ErrorCode::STORAGE_FATAL, DEVCHAIN_CORRUPT};
        if (offset + 8 + len > bytes.size()) break; // a partial tail: wait for the rest
        const std::span<const unsigned char> payload{bytes.data() + offset + 8, len};
        if (crc32c::Crc32c(payload.data(), payload.size()) != crc) return Error{ErrorCode::STORAGE_FATAL, DEVCHAIN_CORRUPT};
        Result<DevChainBlock> block{DecodeDevChainBlock(payload)};
        if (!block.has_value()) return Error{ErrorCode::STORAGE_FATAL, DEVCHAIN_CORRUPT};
        blocks.push_back(std::move(block).value());
        offset += 8 + len;
    }
    return Ok(std::move(blocks));
}

Result<std::vector<unsigned char>> ReadWhole(const fs::path& file)
{
    const int fd{::open(fs::PathToString(file).c_str(), O_RDONLY)};
    if (fd < 0) {
        if (errno == ENOENT) return Ok(std::vector<unsigned char>{});
        return Error{ErrorCode::STORAGE_FATAL, DEVCHAIN_OPEN};
    }
    std::vector<unsigned char> bytes;
    unsigned char buf[65536];
    for (;;) {
        const ssize_t n{::read(fd, buf, sizeof(buf))};
        if (n < 0) {
            ::close(fd);
            return Error{ErrorCode::STORAGE_FATAL, DEVCHAIN_OPEN};
        }
        if (n == 0) break;
        bytes.insert(bytes.end(), buf, buf + n);
    }
    ::close(fd);
    return Ok(std::move(bytes));
}

} // namespace

uint256 DevChainBlockHash(uint32_t height) { return Label("fm2/devchain/block/" + std::to_string(height)); }

Outpoint DevChainOutpoint(uint32_t height, uint32_t index)
{
    return Outpoint{Label("fm2/devchain/deposit/" + std::to_string(height) + "/" + std::to_string(index)), 0};
}

Result<std::vector<unsigned char>> EncodeDevChainBlock(const DevChainBlock& block)
{
    std::vector<unsigned char> out;
    PutU32(out, block.height);
    out.insert(out.end(), block.hash.begin(), block.hash.end());
    PutU32(out, static_cast<uint32_t>(block.facts.size()));
    for (const Fact& fact : block.facts) {
        const Result<std::vector<unsigned char>> bytes{EncodeFact(fact)};
        if (!bytes.has_value()) return bytes.error(); // a block no reader could decode is never written (review 2026-11-16, R8)
        PutU32(out, static_cast<uint32_t>(bytes->size()));
        out.insert(out.end(), bytes->begin(), bytes->end());
    }
    return Ok(std::move(out));
}

Result<DevChainBlock> DecodeDevChainBlock(std::span<const unsigned char> payload)
{
    if (payload.size() < 40) return Error{ErrorCode::DECODE, DEVCHAIN_CORRUPT};
    DevChainBlock block;
    size_t pos{0};
    block.height = GetU32(payload.data());
    pos += 4;
    std::memcpy(block.hash.data(), payload.data() + pos, 32);
    pos += 32;
    const uint32_t n{GetU32(payload.data() + pos)};
    pos += 4;
    if (n > 65'536) return Error{ErrorCode::DECODE, DEVCHAIN_CORRUPT};
    for (uint32_t i{0}; i < n; ++i) {
        if (pos + 4 > payload.size()) return Error{ErrorCode::DECODE, DEVCHAIN_CORRUPT};
        const uint32_t len{GetU32(payload.data() + pos)};
        pos += 4;
        if (pos + len > payload.size()) return Error{ErrorCode::DECODE, DEVCHAIN_CORRUPT};
        Result<Fact> fact{DecodeFact(payload.subspan(pos, len))};
        if (!fact.has_value()) return fact.error();
        block.facts.push_back(std::move(fact).value());
        pos += len;
    }
    if (pos != payload.size()) return Error{ErrorCode::DECODE, DEVCHAIN_CORRUPT};
    return Ok(std::move(block));
}

Result<void> AppendDevChainBlock(const fs::path& file, const DevChainBlock& block)
{
    // The next height only: a reader applies blocks in order.
    Result<std::vector<DevChainBlock>> existing{ReadDevChain(file)};
    if (!existing.has_value()) return existing.error();
    if (!existing->empty() && existing->back().height + 1 != block.height) return Error{ErrorCode::INVALID_INPUT, DEVCHAIN_HEIGHT};
    const Result<std::vector<unsigned char>> payload{EncodeDevChainBlock(block)};
    if (!payload.has_value()) return payload.error();
    std::vector<unsigned char> record;
    PutU32(record, static_cast<uint32_t>(payload->size()));
    PutU32(record, crc32c::Crc32c(payload->data(), payload->size()));
    record.insert(record.end(), payload->begin(), payload->end());
    const int fd{::open(fs::PathToString(file).c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644)};
    if (fd < 0) return Error{ErrorCode::STORAGE_FATAL, DEVCHAIN_OPEN};
    size_t written{0};
    while (written < record.size()) {
        const ssize_t n{::write(fd, record.data() + written, record.size() - written)};
        if (n <= 0) {
            ::close(fd);
            return Error{ErrorCode::STORAGE_FATAL, DEVCHAIN_WRITE};
        }
        written += static_cast<size_t>(n);
    }
    if (::fsync(fd) != 0) {
        ::close(fd);
        return Error{ErrorCode::STORAGE_FATAL, DEVCHAIN_WRITE};
    }
    ::close(fd);
    return Ok();
}

Result<std::vector<DevChainBlock>> ReadDevChain(const fs::path& file)
{
    Result<std::vector<unsigned char>> bytes{ReadWhole(file)};
    if (!bytes.has_value()) return bytes.error();
    uint64_t offset{0};
    return ParseRecords(*bytes, offset);
}

Result<std::vector<DevChainBlock>> DevChainTail::Poll()
{
    Result<std::vector<unsigned char>> bytes{ReadWhole(m_file)};
    if (!bytes.has_value()) return bytes.error();
    if (bytes->size() < m_offset) return Error{ErrorCode::STORAGE_FATAL, DEVCHAIN_CORRUPT}; // the file shrank: never rewritten
    uint64_t offset{m_offset};
    Result<std::vector<DevChainBlock>> blocks{ParseRecords(*bytes, offset)};
    if (!blocks.has_value()) return blocks.error();
    m_offset = offset;
    return blocks;
}

} // namespace flowmesh2::dev
