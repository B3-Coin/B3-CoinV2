// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3COIN_BRIDGE_ARBITRUM_PROOF_CODEC_H
#define B3COIN_BRIDGE_ARBITRUM_PROOF_CODEC_H

#include <bridge/arbitrum_source.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

/** Bounded transport for UNTRUSTED Arbitrum proof data, not source authority.
 * No policy, Ethereum anchor, trusted cursor/request, or "verified" flag is
 * accepted here. Decode success establishes framing and allocation bounds only.
 * The production verifier retains its separate UNREVIEWED_DEPLOYMENT refusal.
 *
 * V1: ASCII B3ARPR01 || u32BE payload length || payload. Hashes/storage words
 * are their exact 32 raw bytes, addresses their 20 raw bytes. Every integer is
 * fixed-width big endian. Blobs are u32BE length followed by nonempty bytes;
 * vectors have u32BE counts. The payload is AssertionPreimage in declaration
 * order, l1_accounts, blocks, confirmed_accounts, historical_accounts.
 * Account = address || nodes || storage; storage item = slot || nodes.
 * Block = header blob || receipts || transactions; indexed item = u64BE index
 * || encoded blob. Indices equal their zero-based vector positions and each
 * block has equal receipt/transaction counts. No extensions or trailing bytes.
 */
namespace bridge::arbitrum::proof_codec {

inline constexpr std::array<unsigned char, 8> MAGIC{'B', '3', 'A', 'R', 'P', 'R', '0', '1'};
inline constexpr size_t FRAME_HEADER_BYTES{12};
inline constexpr size_t MAX_FRAME_BYTES{MAX_PROOF_BYTES};
inline constexpr size_t MAX_TOTAL_RECEIPTS{65'536};
inline constexpr size_t MAX_TOTAL_TRANSACTIONS{65'536};
inline constexpr size_t MAX_TOTAL_NODES{8'192};
inline constexpr size_t MAX_NODES_PER_WITNESS{64};
inline constexpr size_t MAX_NODE_BYTES{8'192};
// The extra slot belongs to the rollup's latestConfirmed witness. Only the
// actual proof policy can decide whether it is permitted in a given account.
inline constexpr size_t MAX_STORAGE_PER_ACCOUNT{MAX_STORAGE + 1};
inline constexpr std::string_view DIGEST_TAG{"B3/arbitrum-proof-wire/1"};

enum class Result { OK, LIMIT, BAD_MAGIC, TRUNCATED, TRAILING, BAD_STRUCTURE };

inline const char* Name(Result result)
{
    switch (result) {
    case Result::OK: return "OK";
    case Result::LIMIT: return "LIMIT";
    case Result::BAD_MAGIC: return "BAD_MAGIC";
    case Result::TRUNCATED: return "TRUNCATED";
    case Result::TRAILING: return "TRAILING";
    case Result::BAD_STRUCTURE: return "BAD_STRUCTURE";
    }
    return "BAD_STRUCTURE";
}

struct EncodedProof {
    Result result{Result::BAD_STRUCTURE};
    std::optional<Bytes> bytes;
    uint256 digest{}; // integrity/diagnostic link only; never proof authority
};
struct DecodedProof {
    Result result{Result::BAD_STRUCTURE};
    std::optional<Proof> proof;
    uint256 digest{};
};

namespace detail {

struct Budget {
    size_t receipts{0}, transactions{0}, nodes{0};
    static bool Add(size_t& used, size_t amount, size_t limit)
    {
        if (amount > limit - used) return false;
        used += amount;
        return true;
    }
};

// Validate the whole object and size it before the encoder allocates its
// output. Count and byte arithmetic is checked independently of allocator size.
class Sizer final {
public:
    Result Check(const Proof& proof)
    {
        if (!Add(FRAME_HEADER_BYTES + 177) || !Accounts(proof.l1_accounts)) return m_result;
        if (proof.blocks.empty()) return Result::BAD_STRUCTURE;
        if (proof.blocks.size() > MAX_HEADERS || !Add(4)) return Result::LIMIT;
        for (const auto& block : proof.blocks) {
            if (!Blob(block.header, MAX_HEADER_BYTES)) return m_result;
            if (block.receipts.size() != block.transactions.size()) return Result::BAD_STRUCTURE;
            if (block.receipts.size() > MAX_RECEIPTS ||
                !Budget::Add(m_budget.receipts, block.receipts.size(), MAX_TOTAL_RECEIPTS) ||
                !Budget::Add(m_budget.transactions, block.transactions.size(), MAX_TOTAL_TRANSACTIONS)) return Result::LIMIT;
            if (!Indexed(block.receipts, MAX_RECEIPT_BYTES) ||
                !Indexed(block.transactions, MAX_TRANSACTION_BYTES)) return m_result;
        }
        if (!Accounts(proof.confirmed_accounts) || !Accounts(proof.historical_accounts)) return m_result;
        return Result::OK;
    }
    size_t Size() const { return m_size; }

private:
    bool Fail(Result result) { m_result = result; return false; }
    bool Add(size_t size)
    {
        return size <= MAX_FRAME_BYTES - m_size ? (m_size += size, true) : Fail(Result::LIMIT);
    }
    bool Blob(const Bytes& bytes, size_t maximum)
    {
        if (bytes.empty()) return Fail(Result::BAD_STRUCTURE);
        if (bytes.size() > maximum) return Fail(Result::LIMIT);
        return Add(4) && Add(bytes.size());
    }
    bool NodesOf(const Nodes& nodes)
    {
        if (nodes.size() > MAX_NODES_PER_WITNESS ||
            !Budget::Add(m_budget.nodes, nodes.size(), MAX_TOTAL_NODES)) return Fail(Result::LIMIT);
        if (!Add(4)) return false;
        for (const auto& node : nodes) if (!Blob(node, MAX_NODE_BYTES)) return false;
        return true;
    }
    bool Accounts(const std::vector<AccountWitness>& accounts)
    {
        if (accounts.size() > MAX_ACCOUNTS) return Fail(Result::LIMIT);
        if (!Add(4)) return false;
        for (const auto& account : accounts) {
            if (account.storage.size() > MAX_STORAGE_PER_ACCOUNT) return Fail(Result::LIMIT);
            if (!Add(20) || !NodesOf(account.nodes) || !Add(4)) return false;
            for (const auto& storage : account.storage) if (!Add(32) || !NodesOf(storage.nodes)) return false;
        }
        return true;
    }
    template <typename T>
    bool Indexed(const std::vector<T>& items, size_t maximum)
    {
        if (!Add(4)) return false;
        for (size_t i{0}; i < items.size(); ++i) {
            if (items[i].index != i) return Fail(Result::BAD_STRUCTURE);
            if (!Add(8) || !Blob(items[i].encoded, maximum)) return false;
        }
        return true;
    }
    size_t m_size{0};
    Budget m_budget;
    Result m_result{Result::OK};
};

class Writer final {
public:
    explicit Writer(size_t size) { m_bytes.reserve(size); }
    void Raw(std::span<const unsigned char> bytes) { m_bytes.insert(m_bytes.end(), bytes.begin(), bytes.end()); }
    void U8(uint8_t value) { m_bytes.push_back(value); }
    void U32(uint32_t value) { for (int shift{24}; shift >= 0; shift -= 8) U8(static_cast<uint8_t>(value >> shift)); }
    void U64(uint64_t value) { for (int shift{56}; shift >= 0; shift -= 8) U8(static_cast<uint8_t>(value >> shift)); }
    void Root(const uint256& root) { Raw({root.begin(), 32}); }
    void Blob(const Bytes& bytes) { U32(static_cast<uint32_t>(bytes.size())); Raw(bytes); }
    void NodesOf(const Nodes& nodes)
    {
        U32(static_cast<uint32_t>(nodes.size()));
        for (const auto& node : nodes) Blob(node);
    }
    void Accounts(const std::vector<AccountWitness>& accounts)
    {
        U32(static_cast<uint32_t>(accounts.size()));
        for (const auto& account : accounts) {
            Raw(account.address); NodesOf(account.nodes); U32(static_cast<uint32_t>(account.storage.size()));
            for (const auto& storage : account.storage) { Raw(storage.slot); NodesOf(storage.nodes); }
        }
    }
    template <typename T> void Indexed(const std::vector<T>& items)
    {
        U32(static_cast<uint32_t>(items.size()));
        for (const auto& item : items) { U64(item.index); Blob(item.encoded); }
    }
    Bytes Take() { return std::move(m_bytes); }
private:
    Bytes m_bytes;
};

class Reader final {
public:
    explicit Reader(std::span<const unsigned char> bytes) : m_bytes{bytes} {}
    Result Status() const { return m_result; }
    size_t Remaining() const { return m_bytes.size() - m_offset; }
    bool Fail(Result result) { m_result = result; return false; }
    bool Raw(std::span<unsigned char> output)
    {
        if (output.size() > Remaining()) return Fail(Result::TRUNCATED);
        std::copy_n(m_bytes.begin() + m_offset, output.size(), output.begin());
        m_offset += output.size();
        return true;
    }
    bool U8(uint8_t& value)
    {
        if (!Remaining()) return Fail(Result::TRUNCATED);
        value = m_bytes[m_offset++]; return true;
    }
    bool U32(uint32_t& value)
    {
        if (Remaining() < 4) return Fail(Result::TRUNCATED);
        value = 0;
        for (size_t i{0}; i < 4; ++i) value = (value << 8) | m_bytes[m_offset++];
        return true;
    }
    bool U64(uint64_t& value)
    {
        if (Remaining() < 8) return Fail(Result::TRUNCATED);
        value = 0;
        for (size_t i{0}; i < 8; ++i) value = (value << 8) | m_bytes[m_offset++];
        return true;
    }
    bool Root(uint256& value) { return Raw({value.begin(), 32}); }
    bool Blob(Bytes& output, size_t maximum)
    {
        uint32_t size{};
        if (!U32(size)) return false;
        if (!size) return Fail(Result::BAD_STRUCTURE);
        if (size > maximum) return Fail(Result::LIMIT);
        if (size > Remaining()) return Fail(Result::TRUNCATED);
        output.assign(m_bytes.begin() + m_offset, m_bytes.begin() + m_offset + size);
        m_offset += size;
        return true;
    }
    // Check both the semantic count bound and the minimum encoded bytes BEFORE
    // reserve. A forged count in a tiny packet cannot allocate its vector tree.
    bool Count(uint32_t& count, size_t maximum, size_t minimum_item_bytes)
    {
        if (!U32(count)) return false;
        if (count > maximum) return Fail(Result::LIMIT);
        if (count > Remaining() / minimum_item_bytes) return Fail(Result::TRUNCATED);
        return true;
    }
    bool NodesOf(Nodes& output)
    {
        uint32_t count{};
        if (!Count(count, MAX_NODES_PER_WITNESS, 5)) return false;
        if (!Budget::Add(m_budget.nodes, count, MAX_TOTAL_NODES)) return Fail(Result::LIMIT);
        output.reserve(count);
        for (uint32_t i{0}; i < count; ++i) {
            Bytes node;
            if (!Blob(node, MAX_NODE_BYTES)) return false;
            output.push_back(std::move(node));
        }
        return true;
    }
    bool Accounts(std::vector<AccountWitness>& output)
    {
        uint32_t count{};
        if (!Count(count, MAX_ACCOUNTS, 28)) return false;
        output.reserve(count);
        for (uint32_t i{0}; i < count; ++i) {
            AccountWitness account;
            if (!Raw(account.address) || !NodesOf(account.nodes)) return false;
            uint32_t storage_count{};
            if (!Count(storage_count, MAX_STORAGE_PER_ACCOUNT, 36)) return false;
            account.storage.reserve(storage_count);
            for (uint32_t j{0}; j < storage_count; ++j) {
                StorageWitness storage;
                if (!Raw(storage.slot) || !NodesOf(storage.nodes)) return false;
                account.storage.push_back(std::move(storage));
            }
            output.push_back(std::move(account));
        }
        return true;
    }
    bool Blocks(std::vector<BlockWitness>& output)
    {
        uint32_t count{};
        if (!Count(count, MAX_HEADERS, 13)) return false;
        if (!count) return Fail(Result::BAD_STRUCTURE);
        output.reserve(count);
        for (uint32_t i{0}; i < count; ++i) {
            BlockWitness block;
            if (!Blob(block.header, MAX_HEADER_BYTES) ||
                !Indexed(block.receipts, MAX_RECEIPT_BYTES, m_budget.receipts, MAX_TOTAL_RECEIPTS) ||
                !Indexed(block.transactions, MAX_TRANSACTION_BYTES, m_budget.transactions, MAX_TOTAL_TRANSACTIONS)) return false;
            if (block.receipts.size() != block.transactions.size()) return Fail(Result::BAD_STRUCTURE);
            output.push_back(std::move(block));
        }
        return true;
    }
private:
    template <typename T>
    bool Indexed(std::vector<T>& output, size_t maximum_blob, size_t& used, size_t maximum_total)
    {
        uint32_t count{};
        if (!Count(count, MAX_RECEIPTS, 13)) return false;
        if (!Budget::Add(used, count, maximum_total)) return Fail(Result::LIMIT);
        output.reserve(count);
        for (uint32_t i{0}; i < count; ++i) {
            T item;
            if (!U64(item.index)) return false;
            if (item.index != i) return Fail(Result::BAD_STRUCTURE);
            if (!Blob(item.encoded, maximum_blob)) return false;
            output.push_back(std::move(item));
        }
        return true;
    }
    std::span<const unsigned char> m_bytes;
    size_t m_offset{0};
    Result m_result{Result::OK};
    Budget m_budget;
};
} // namespace detail

[[nodiscard]] inline EncodedProof EncodeProofV1(const Proof& proof)
{
    detail::Sizer size;
    const auto result{size.Check(proof)};
    if (result != Result::OK) return {result, std::nullopt, {}};
    try {
        detail::Writer writer{size.Size()};
        writer.Raw(MAGIC); writer.U32(static_cast<uint32_t>(size.Size() - FRAME_HEADER_BYTES));
        const auto& assertion{proof.assertion};
        writer.Root(assertion.parent_assertion); writer.Root(assertion.block_hash); writer.Root(assertion.send_root);
        writer.U64(assertion.inbox_position); writer.U64(assertion.position_in_message); writer.U8(assertion.machine_status);
        writer.Root(assertion.end_history_root); writer.Root(assertion.inbox_accumulator);
        writer.Accounts(proof.l1_accounts);
        writer.U32(static_cast<uint32_t>(proof.blocks.size()));
        for (const auto& block : proof.blocks) { writer.Blob(block.header); writer.Indexed(block.receipts); writer.Indexed(block.transactions); }
        writer.Accounts(proof.confirmed_accounts); writer.Accounts(proof.historical_accounts);
        auto bytes{writer.Take()};
        const auto digest{peer::TaggedDigest(DIGEST_TAG, bytes)};
        return {Result::OK, std::move(bytes), digest};
    } catch (const std::bad_alloc&) {
        return {Result::LIMIT, std::nullopt, {}};
    } catch (const std::length_error&) {
        return {Result::LIMIT, std::nullopt, {}};
    }
}

[[nodiscard]] inline DecodedProof DecodeProofV1(std::span<const unsigned char> bytes)
{
    if (bytes.size() > MAX_FRAME_BYTES) return {Result::LIMIT, std::nullopt, {}};
    if (bytes.size() < FRAME_HEADER_BYTES) return {Result::TRUNCATED, std::nullopt, {}};
    if (!std::equal(MAGIC.begin(), MAGIC.end(), bytes.begin())) return {Result::BAD_MAGIC, std::nullopt, {}};
    detail::Reader reader{bytes.subspan(MAGIC.size())};
    uint32_t payload_size{};
    if (!reader.U32(payload_size)) return {reader.Status(), std::nullopt, {}};
    if (payload_size > MAX_FRAME_BYTES - FRAME_HEADER_BYTES) return {Result::LIMIT, std::nullopt, {}};
    if (payload_size > reader.Remaining()) return {Result::TRUNCATED, std::nullopt, {}};
    if (payload_size < reader.Remaining()) return {Result::TRAILING, std::nullopt, {}};
    try {
        Proof proof;
        auto& assertion{proof.assertion};
        if (!reader.Root(assertion.parent_assertion) || !reader.Root(assertion.block_hash) || !reader.Root(assertion.send_root) ||
            !reader.U64(assertion.inbox_position) || !reader.U64(assertion.position_in_message) || !reader.U8(assertion.machine_status) ||
            !reader.Root(assertion.end_history_root) || !reader.Root(assertion.inbox_accumulator) ||
            !reader.Accounts(proof.l1_accounts) || !reader.Blocks(proof.blocks) ||
            !reader.Accounts(proof.confirmed_accounts) || !reader.Accounts(proof.historical_accounts)) return {reader.Status(), std::nullopt, {}};
        if (reader.Remaining()) return {Result::TRAILING, std::nullopt, {}};
        return {Result::OK, std::move(proof), peer::TaggedDigest(DIGEST_TAG, bytes)};
    } catch (const std::bad_alloc&) {
        return {Result::LIMIT, std::nullopt, {}};
    } catch (const std::length_error&) {
        return {Result::LIMIT, std::nullopt, {}};
    }
}

} // namespace bridge::arbitrum::proof_codec

#endif // B3COIN_BRIDGE_ARBITRUM_PROOF_CODEC_H
