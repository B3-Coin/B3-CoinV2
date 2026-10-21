// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3COIN_BRIDGE_ARBITRUM_SOURCE_H
#define B3COIN_BRIDGE_ARBITRUM_SOURCE_H

#include <bridge/deposit.h>
#include <bridge/peer_light_client.h>
#include <bridge/state_proof.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

/** Native building blocks for a SLOW, L1-contract-settled Arbitrum source.
 * This is not Nitro execution, an optimistic sequencer guarantee, or a source
 * activation. A confirmed BoLD assertion additionally relies on honest
 * validation/challenges and trusted rollup upgrade governance. Ethereum
 * finality alone does not prove Arbitrum execution.
 *
 * The production deployment registry is deliberately empty. Fixture snapshots
 * exercise real MPT/RLP/Keccak checks but CANNOT construct ConfirmedRange.
 * Adding reviewed deployed code/storage-layout/governance pins requires a
 * separate code change and review; no caller-controlled boolean enables it.
 */
namespace bridge::arbitrum {

using Bytes = std::vector<unsigned char>;
using Nodes = std::vector<Bytes>;
inline constexpr size_t MAX_HEADERS{128};
inline constexpr size_t MAX_HEADER_BYTES{4096};
inline constexpr size_t MAX_RECEIPTS{8192};
inline constexpr size_t MAX_RECEIPT_BYTES{1 << 20};
inline constexpr size_t MAX_TRANSACTION_BYTES{1 << 20};
inline constexpr size_t MAX_PROOF_BYTES{32 << 20};
inline constexpr size_t MAX_ACCOUNTS{16};
inline constexpr size_t MAX_STORAGE{64};

enum class Error {
    OK, LIMIT, BAD_POLICY, BAD_ACCOUNT, BAD_STORAGE, BAD_ASSERTION,
    BAD_HEADER, BAD_ANCESTRY, BAD_RECEIPTS, BAD_TRANSACTIONS, STALE_ETHEREUM,
    UNREVIEWED_DEPLOYMENT,
};

struct StorageWitness { StorageWord slot{}; Nodes nodes; };
struct AccountWitness {
    EthAddress address{};
    Nodes nodes;
    std::vector<StorageWitness> storage;
};
struct IndexedReceipt { uint64_t index{0}; Bytes encoded; };
struct IndexedTransaction { uint64_t index{0}; Bytes encoded; };
struct BlockWitness {
    Bytes header;
    std::vector<IndexedReceipt> receipts;
    std::vector<IndexedTransaction> transactions;
};
// Trusted caller cursor/request, supplied separately from untrusted Proof.
// The interval is (previous_height, to_height], inclusive of to_height.
struct RangeRequest {
    uint64_t previous_height{0}, to_height{0};
    uint256 previous_hash{}, to_hash{};
};

// Exactly the six ABI words in Nitro AssertionState, plus the outer hash
// preimage. uint256 objects contain RAW hash bytes here, not display-order hex.
struct AssertionPreimage {
    uint256 parent_assertion{}, block_hash{}, send_root{};
    uint64_t inbox_position{0}, position_in_message{0};
    uint8_t machine_status{1}; // MachineStatus.FINISHED; RUNNING/ERRORED refused
    uint256 end_history_root{}, inbox_accumulator{};
};
struct Proof {
    AssertionPreimage assertion;
    std::vector<AccountWitness> l1_accounts;
    // Newest first, starting at assertion.block_hash. Every header's COMPLETE
    // receipt list is supplied, including failed/system/non-vault receipts.
    std::vector<BlockWitness> blocks;
    std::vector<AccountWitness> confirmed_accounts; // newest confirmed state
    std::vector<AccountWitness> historical_accounts; // LAST header's older state
};
struct Header {
    uint256 hash{}, parent{}, state_root{}, transactions_root{}, receipts_root{};
    uint64_t number{0}, timestamp{0}, gas_used{0};
};
struct LocatedReceipt {
    uint64_t block_number{0}, transaction_index{0};
    uint256 transaction_hash{}; // Keccak of EXACT bytes committed by tx trie
    EthReceipt receipt;
};

namespace detail {
inline std::span<const unsigned char> Raw(const uint256& h) { return {h.begin(), 32}; }
inline uint256 Hash(std::span<const unsigned char> bytes)
{
    uint256 h;
    Keccak256().Write(bytes).Finalize(h);
    return h;
}
inline bool Nonzero(std::span<const unsigned char> bytes)
{
    return std::any_of(bytes.begin(), bytes.end(), [](unsigned char b) { return b != 0; });
}
inline void Word(Bytes& out, uint64_t value)
{
    const auto word{SlotWord(value)};
    out.insert(out.end(), word.begin(), word.end());
}
inline void Word(Bytes& out, const uint256& value)
{
    out.insert(out.end(), value.begin(), value.end());
}
inline uint256 AssertionHash(const AssertionPreimage& a)
{
    Bytes state;
    Word(state, a.block_hash); Word(state, a.send_root);
    Word(state, a.inbox_position); Word(state, a.position_in_message);
    Word(state, a.machine_status); Word(state, a.end_history_root);
    Bytes outer;
    Word(outer, a.parent_assertion); Word(outer, Hash(state));
    Word(outer, a.inbox_accumulator);
    return Hash(outer);
}

// Only the documented 16-field Nitro fixture schema is supported here.
// This is not qualification of a deployed chain's current fork/schema.
// Any future extension needs an explicit reviewed schema; >=16 is NOT enough.
inline std::optional<Header> NitroHeader(std::span<const unsigned char> bytes)
{
    if (bytes.empty() || bytes.size() > MAX_HEADER_BYTES) return std::nullopt;
    const auto top{RlpDecode(bytes)};
    const auto f{top && top->is_list ? RlpChildren(*top) : std::nullopt};
    if (!f || f->size() != 16) return std::nullopt;
    const auto fixed = [&](size_t i, size_t size) {
        return !(*f)[i].is_list && (*f)[i].payload.size() == size;
    };
    const auto integer = [&](size_t i, size_t size) {
        const auto& v{(*f)[i]};
        return !v.is_list && v.payload.size() <= size &&
               (v.payload.empty() || v.payload[0] != 0);
    };
    for (size_t i : {0U, 1U, 3U, 4U, 5U, 13U}) if (!fixed(i, 32)) return std::nullopt;
    if (!fixed(2, 20) || !fixed(6, 256) || !fixed(14, 8) ||
        (*f)[12].is_list || (*f)[12].payload.size() > 32 ||
        !integer(7, 32) || !integer(15, 32)) return std::nullopt;
    for (size_t i : {8U, 9U, 10U, 11U}) if (!integer(i, 8)) return std::nullopt;
    const auto number{deposit_detail::RlpUint64((*f)[8])};
    const auto limit{deposit_detail::RlpUint64((*f)[9])};
    const auto used{deposit_detail::RlpUint64((*f)[10])};
    const auto time{deposit_detail::RlpUint64((*f)[11])};
    if (!number || !limit || !used || !time || *used > *limit) return std::nullopt;
    Header h;
    h.hash = Hash(bytes); h.parent = uint256{(*f)[0].payload};
    h.state_root = uint256{(*f)[3].payload};
    h.transactions_root = uint256{(*f)[4].payload};
    h.receipts_root = uint256{(*f)[5].payload};
    if (h.state_root.IsNull() || h.transactions_root.IsNull() || h.receipts_root.IsNull()) return std::nullopt;
    h.number = *number; h.timestamp = *time; h.gas_used = *used;
    return h;
}

struct TrieEntry { Bytes key; const Bytes* value; };
inline Bytes HexPrefix(std::span<const unsigned char> nibbles, bool leaf)
{
    Bytes out;
    const bool odd{nibbles.size() % 2 != 0};
    out.push_back(static_cast<unsigned char>((leaf ? 0x20 : 0) | (odd ? 0x10 | nibbles[0] : 0)));
    for (size_t i{odd ? 1U : 0U}; i < nibbles.size(); i += 2) {
        out.push_back(static_cast<unsigned char>((nibbles[i] << 4) | nibbles[i + 1]));
    }
    return out;
}
inline Bytes Reference(const Bytes& node)
{
    return node.size() < 32 ? node : RlpEncodeBytes(Raw(Hash(node)));
}
inline Bytes TrieNode(std::span<const TrieEntry> entries, size_t depth)
{
    if (entries.size() == 1) {
        return RlpEncodeList({RlpEncodeBytes(HexPrefix(std::span{entries[0].key}.subspan(depth), true)),
                              RlpEncodeBytes(*entries[0].value)});
    }
    size_t prefix{depth};
    const auto& first{entries.front().key};
    const auto& last{entries.back().key};
    while (prefix < first.size() && prefix < last.size() && first[prefix] == last[prefix]) ++prefix;
    if (prefix != depth) {
        return RlpEncodeList({RlpEncodeBytes(HexPrefix(std::span{first}.subspan(depth, prefix - depth), false)),
                              Reference(TrieNode(entries, prefix))});
    }
    std::vector<Bytes> branch(17, RlpEncodeBytes({}));
    size_t pos{0};
    if (entries[pos].key.size() == depth) {
        branch[16] = RlpEncodeBytes(*entries[pos].value);
        ++pos;
    }
    while (pos < entries.size()) {
        const auto nibble{entries[pos].key[depth]};
        const size_t begin{pos++};
        while (pos < entries.size() && entries[pos].key.size() > depth && entries[pos].key[depth] == nibble) ++pos;
        branch[nibble] = Reference(TrieNode(entries.subspan(begin, pos - begin), depth + 1));
    }
    return RlpEncodeList(branch);
}

/** Reconstruct the ENTIRE ordered receipt trie. Index count is not accepted
 * from an untrusted manifest: omission, insertion, reordering or substitution
 * changes the committed root. Empty blocks use keccak(rlp(empty string)). */
template <typename Record, typename Valid>
inline std::optional<uint256> OrderedTrieRoot(const std::vector<Record>& records,
                                              size_t max_value_bytes, Valid valid)
{
    if (records.size() > MAX_RECEIPTS) return std::nullopt;
    size_t total{0};
    std::vector<TrieEntry> entries;
    entries.reserve(records.size());
    for (size_t i{0}; i < records.size(); ++i) {
        const auto& record{records[i]};
        if (record.index != i || record.encoded.empty() || record.encoded.size() > max_value_bytes ||
            record.encoded.size() > MAX_PROOF_BYTES - total || !valid(record.encoded)) return std::nullopt;
        total += record.encoded.size();
        const auto key{RlpEncodeUint64(i)};
        TrieEntry entry{{}, &record.encoded};
        for (auto b : key) { entry.key.push_back(b >> 4); entry.key.push_back(b & 15); }
        entries.push_back(std::move(entry));
    }
    if (entries.empty()) return Hash(RlpEncodeBytes({}));
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
    return Hash(TrieNode(entries, 0));
}
inline std::optional<uint256> ReceiptRoot(const std::vector<IndexedReceipt>& receipts)
{
    return OrderedTrieRoot(receipts, MAX_RECEIPT_BYTES, [](const Bytes& bytes) {
        return DecodeReceipt(bytes, RECEIPT_MAX_TYPE_V2).has_value();
    });
}
// Transaction bytes are authenticated as opaque trie values. This does not
// execute/decode the transaction, validate its signature, or validate Nitro.
// Unknown transaction envelopes remain opaque; the settled header commits to
// their exact bytes, including Arbitrum-specific transaction types.
inline std::optional<uint256> TransactionRoot(const std::vector<IndexedTransaction>& transactions)
{
    return OrderedTrieRoot(transactions, MAX_TRANSACTION_BYTES, [](const Bytes&) { return true; });
}
} // namespace detail

/** Explicitly SYNTHETIC fixture schema. These pins must not be deserialized
 * into a production deployment and this result is not a source capability.
 * All requested accounts/storage have exact ordered witnesses; no unused
 * account/slot can silently replace an expected proxy/governance obligation.
 */
namespace fixture {
struct PinnedStorage { StorageWord slot{}, value{}; };
struct PinnedAccount {
    EthAddress address{};
    uint256 code_hash{};
    std::vector<PinnedStorage> storage;
};
struct Policy {
    EthAddress rollup{};
    StorageWord latest_confirmed_slot{};
    std::vector<PinnedAccount> l1_accounts, confirmed_accounts, historical_accounts;
};
struct Evidence {
    uint256 assertion_hash{};
    RangeRequest request;
    Header confirmed_head{}, oldest_header{};
    std::vector<LocatedReceipt> receipts;
    bool MainnetEnabled() const { return false; }
};
struct Reply { Error error{Error::BAD_POLICY}; std::optional<Evidence> evidence{}; };

inline bool AddSize(size_t& total, size_t bytes)
{
    if (bytes > MAX_PROOF_BYTES - total) return false;
    total += bytes;
    return true;
}
inline bool CountNodes(size_t& total, const Nodes& nodes)
{
    if (nodes.size() > 64) return false;
    for (const auto& node : nodes) if (node.empty() || node.size() > 8192 || !AddSize(total, node.size())) return false;
    return true;
}
inline Error Accounts(const uint256& root, const std::vector<PinnedAccount>& pins,
                      const std::vector<AccountWitness>& witnesses,
                      const Policy* dynamic, const uint256& assertion)
{
    if (pins.size() != witnesses.size() || pins.size() > MAX_ACCOUNTS) return Error::BAD_POLICY;
    bool saw_rollup{false};
    for (size_t i{0}; i < pins.size(); ++i) {
        const auto& pin{pins[i]}; const auto& witness{witnesses[i]};
        if (!detail::Nonzero(pin.address) || pin.code_hash.IsNull() || pin.address != witness.address ||
            pin.storage.size() > MAX_STORAGE) return Error::BAD_POLICY;
        for (size_t j{0}; j < i; ++j) if (pins[j].address == pin.address) return Error::BAD_POLICY;
        const bool rollup{dynamic && dynamic->rollup == pin.address};
        saw_rollup |= rollup;
        if (witness.storage.size() != pin.storage.size() + (rollup ? 1U : 0U)) return Error::BAD_STORAGE;
        std::string error;
        const auto account{VerifyAccountProof(root, pin.address, witness.nodes, error)};
        if (!account || account->code_hash != pin.code_hash) return Error::BAD_ACCOUNT;
        for (size_t j{0}; j < witness.storage.size(); ++j) {
            const bool latest{rollup && j == pin.storage.size()};
            const auto slot{latest ? dynamic->latest_confirmed_slot : pin.storage[j].slot};
            if (slot != witness.storage[j].slot) return Error::BAD_STORAGE;
            for (size_t k{0}; k < j; ++k) if (witness.storage[k].slot == slot) return Error::BAD_POLICY;
            const auto value{VerifyStorageValue(account->storage_root, slot, witness.storage[j].nodes, error)};
            const auto expected{latest ? StorageWord{} : pin.storage[j].value};
            if (!value || (latest ? !std::equal(value->begin(), value->end(), assertion.begin()) : *value != expected)) return Error::BAD_STORAGE;
        }
    }
    return dynamic && !saw_rollup ? Error::BAD_POLICY : Error::OK;
}

// The caller's root/pins are fixture trust input. This API deliberately does
// not accept an Ethereum anchor or produce ConfirmedRange production authority.
inline Reply Verify(const uint256& l1_fixture_state_root, const Policy& policy,
                    const RangeRequest& request, const Proof& proof)
{
    if (l1_fixture_state_root.IsNull() || policy.l1_accounts.empty() ||
        !detail::Nonzero(policy.rollup)) return {Error::BAD_POLICY};
    if (proof.blocks.empty() || proof.blocks.size() > MAX_HEADERS) return {Error::LIMIT};
    if (request.previous_height >= request.to_height || request.previous_hash.IsNull() ||
        request.to_hash.IsNull() || request.to_height - request.previous_height != proof.blocks.size()) return {Error::BAD_ANCESTRY};
    size_t total{0};
    for (const auto* accounts : {&proof.l1_accounts, &proof.confirmed_accounts, &proof.historical_accounts}) {
        if (accounts->size() > MAX_ACCOUNTS) return {Error::LIMIT};
        for (const auto& account : *accounts) {
            if (!CountNodes(total, account.nodes) || account.storage.size() > MAX_STORAGE + 1) return {Error::LIMIT};
            for (const auto& storage : account.storage) if (!CountNodes(total, storage.nodes)) return {Error::LIMIT};
        }
    }
    for (const auto& block : proof.blocks) {
        if (block.header.size() > MAX_HEADER_BYTES || !AddSize(total, block.header.size()) ||
            block.receipts.size() > MAX_RECEIPTS || block.transactions.size() > MAX_RECEIPTS) return {Error::LIMIT};
        for (const auto& r : block.receipts) if (r.encoded.size() > MAX_RECEIPT_BYTES || !AddSize(total, r.encoded.size())) return {Error::LIMIT};
        for (const auto& tx : block.transactions) if (tx.encoded.size() > MAX_TRANSACTION_BYTES || !AddSize(total, tx.encoded.size())) return {Error::LIMIT};
    }
    const auto& a{proof.assertion};
    if (a.machine_status != 1 || a.block_hash.IsNull() || a.parent_assertion.IsNull()) return {Error::BAD_ASSERTION};
    Evidence out;
    out.request = request;
    out.assertion_hash = detail::AssertionHash(a);
    const auto l1{Accounts(l1_fixture_state_root, policy.l1_accounts, proof.l1_accounts, &policy, out.assertion_hash)};
    if (l1 != Error::OK) return {l1};
    uint256 want{a.block_hash};
    Header previous;
    for (size_t i{0}; i < proof.blocks.size(); ++i) {
        const auto& block{proof.blocks[i]};
        const auto h{detail::NitroHeader(block.header)};
        if (!h) return {Error::BAD_HEADER};
        if (h->hash != want || (i && (previous.number == 0 || h->number != previous.number - 1 || h->timestamp > previous.timestamp))) return {Error::BAD_ANCESTRY};
        const auto root{detail::ReceiptRoot(block.receipts)};
        if (!root || *root != h->receipts_root) return {Error::BAD_RECEIPTS};
        if (block.transactions.size() != block.receipts.size()) return {Error::BAD_TRANSACTIONS};
        const auto tx_root{detail::TransactionRoot(block.transactions)};
        if (!tx_root || *tx_root != h->transactions_root) return {Error::BAD_TRANSACTIONS};
        uint64_t gas{0};
        for (const auto& r : block.receipts) {
            auto receipt{DecodeReceipt(r.encoded, RECEIPT_MAX_TYPE_V2)};
            if (!receipt || receipt->cumulative_gas < gas || receipt->cumulative_gas > h->gas_used) return {Error::BAD_RECEIPTS};
            gas = receipt->cumulative_gas;
            out.receipts.push_back({h->number, r.index, detail::Hash(block.transactions[r.index].encoded), std::move(*receipt)});
        }
        if (gas != h->gas_used) return {Error::BAD_RECEIPTS};
        if (i == 0) {
            if (h->number != request.to_height || h->hash != request.to_hash) return {Error::BAD_ANCESTRY};
            out.confirmed_head = *h;
        }
        out.oldest_header = *h;
        previous = *h; want = h->parent;
    }
    if (out.oldest_header.number != request.previous_height + 1 ||
        out.oldest_header.parent != request.previous_hash) return {Error::BAD_ANCESTRY};
    const auto confirmed{Accounts(out.confirmed_head.state_root, policy.confirmed_accounts, proof.confirmed_accounts, nullptr, {})};
    if (confirmed != Error::OK) return {confirmed};
    const auto historical{Accounts(out.oldest_header.state_root, policy.historical_accounts, proof.historical_accounts, nullptr, {})};
    if (historical != Error::OK) return {historical};
    return {Error::OK, std::move(out)};
}
} // namespace fixture

enum class Deployment { ARBITRUM_ONE_BOLD };
class Verifier;
class ConfirmedRange final {
public:
    const Header& OldestHeader() const { return m_header; }
    const RangeRequest& Request() const { return m_request; }
    bool IsCurrent(const peer::LightClient& ethereum, uint64_t current_slot) const
    {
        return &ethereum == m_owner && ethereum.IsCurrent(m_ethereum_anchor, current_slot);
    }
private:
    friend class Verifier;
    ConfirmedRange(const peer::LightClient& owner, peer::FinalizedExecution anchor,
                   RangeRequest request, Header header)
        : m_owner{&owner}, m_ethereum_anchor{std::move(anchor)}, m_request{std::move(request)}, m_header{std::move(header)} {}
    const peer::LightClient* m_owner;
    peer::FinalizedExecution m_ethereum_anchor;
    RangeRequest m_request;
    Header m_header;
};
struct Verification { Error error; std::optional<ConfirmedRange> range; };
class Verifier final {
public:
    static Verification VerifyConfirmedRange(const peer::LightClient& ethereum,
                                              const peer::FinalizedExecution& anchor,
                                              uint64_t current_slot, Deployment,
                                              const RangeRequest&, const Proof&)
    {
        if (!ethereum.IsCurrent(anchor, current_slot)) return {Error::STALE_ETHEREUM, std::nullopt};
        // HARD STOP. No reviewed deployed Arbitrum code/layout/governance
        // registry exists yet. Fixture policy or a peer/RPC cannot fill it.
        return {Error::UNREVIEWED_DEPLOYMENT, std::nullopt};
    }
};

} // namespace bridge::arbitrum
#endif // B3COIN_BRIDGE_ARBITRUM_SOURCE_H
