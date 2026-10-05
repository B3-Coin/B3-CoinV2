// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3COIN_BRIDGE_STATE_PROOF_H
#define B3COIN_BRIDGE_STATE_PROOF_H

#include <bridge/deposit.h>
#include <bridge/mpt.h>
#include <bridge/rlp.h>
#include <crypto/keccak256.h>
#include <uint256.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

/** Ethereum account and storage proofs (EIP-1186, eth_getProof) for the
 *  bridge V2 kind 9 ETH_STATE_OBSERVATION records.
 *
 *  An observation reads a contract's storage words at one retained execution
 *  anchor: the anchor's stateRoot proves the account (key keccak(address)),
 *  whose storageRoot proves each slot (key keccak(slot)). Both tries are
 *  "secure" Merkle-Patricia tries: every key is exactly 32 bytes (64
 *  nibbles), so a valid trie never stores a value in a branch node and never
 *  ends a key at a branch or extension.
 *
 *  Unlike mpt.h (receipt inclusion only), this verifier also proves
 *  ABSENCE, because Ethereum clients never store a zero word: a zero slot, a
 *  false flag or an unset mapping entry exists only as an exclusion proof.
 *  (anvil's in-memory state does keep slots a transaction merely read as
 *  leaves holding the RLP of zero; VerifyStorageValue reads those as zero
 *  only when told to, for the regtest origin chain: StoredZero.) The
 *  verifier is strict. Every proof node must be consumed, every hash link
 *  must match, every node must be canonical RLP in a shape a real trie
 *  produces, and an absence must be shown at the last node. Any other proof
 *  is refused with a stable reason rather than read as "absent".
 */
namespace bridge {

//! keccak(rlp("")): the root of an empty trie, and the storageRoot of an
//! account without storage.
inline constexpr std::array<unsigned char, 32> EMPTY_TRIE_ROOT_BYTES{
    0x56, 0xe8, 0x1f, 0x17, 0x1b, 0xcc, 0x55, 0xa6,
    0xff, 0x83, 0x45, 0xe6, 0x92, 0xc0, 0xf8, 0x6e,
    0x5b, 0x48, 0xe0, 0x1b, 0x99, 0x6c, 0xad, 0xc0,
    0x01, 0x62, 0x2f, 0xb5, 0xe3, 0x63, 0xb4, 0x21,
};
inline constexpr uint256 EMPTY_TRIE_ROOT{
    std::span<const unsigned char>{EMPTY_TRIE_ROOT_BYTES}};

//! keccak(""): the codeHash of an account without code.
inline constexpr std::array<unsigned char, 32> EMPTY_CODE_HASH_BYTES{
    0xc5, 0xd2, 0x46, 0x01, 0x86, 0xf7, 0x23, 0x3c,
    0x92, 0x7e, 0x7d, 0xb2, 0xdc, 0xc7, 0x03, 0xc0,
    0xe5, 0x00, 0xb6, 0x53, 0xca, 0x82, 0x27, 0x3b,
    0x7b, 0xfa, 0xd8, 0x04, 0x5d, 0x85, 0xa4, 0x70,
};
inline constexpr uint256 EMPTY_CODE_HASH{
    std::span<const unsigned char>{EMPTY_CODE_HASH_BYTES}};

//! One EVM storage word, big-endian (byte 0 is the most significant).
using StorageWord = std::array<unsigned char, 32>;

/** Size bounds for one account or storage proof. A branch node with 16
 *  hash children is 532 bytes and a secure-trie path has at most 64
 *  nibbles, so the bounds leave room for any real proof while capping the
 *  work an adversarial proof can cause. */
struct StateProofLimits {
    size_t max_nodes{64};
    size_t max_node_size{1024};
    size_t max_total_bytes{11264};
};
inline constexpr StateProofLimits STATE_PROOF_LIMITS{};

//! Result of a secure-trie lookup: the leaf value when present.
struct SecureMptLookup {
    bool present{false};
    std::vector<unsigned char> value{};
};

//! A decoded Ethereum account (the state-trie leaf value).
struct EthAccount {
    uint64_t nonce{0};
    StorageWord balance{};
    uint256 storage_root{};
    uint256 code_hash{};
};

namespace state_proof_detail {

//! True when `item` is the full encoding of a canonical RLP integer of at
//! most `max_bytes` bytes (zero is the empty string, no leading zero byte).
inline bool CanonicalInteger(const RlpItem& item, size_t max_bytes)
{
    if (item.is_list || item.payload.size() > max_bytes) return false;
    return item.payload.empty() || item.payload[0] != 0x00;
}

//! The 32-byte big-endian word whose low-order bytes are `bytes`.
inline StorageWord LeftPad(std::span<const unsigned char> bytes)
{
    assert(bytes.size() <= 32);
    StorageWord out{};
    std::copy(bytes.begin(), bytes.end(), out.end() - bytes.size());
    return out;
}

inline uint256 KeccakWord(std::span<const unsigned char> data)
{
    return mpt_detail::KeccakOf(data);
}

} // namespace state_proof_detail

/** Verify a secure Merkle-Patricia trie proof for `hashed_key` (the 32-byte
 *  trie key, already keccak-hashed) against `root`.
 *
 *  `nodes` are the trie nodes from the root down, each the full RLP node
 *  encoding, as eth_getProof returns them (nodes shorter than 32 bytes are
 *  embedded in their parent and never listed). Returns {present, value} with
 *  the leaf value bytes when the key is present, {absent} when the proof
 *  shows the key is not in the trie, and std::nullopt (with `error`) for any
 *  malformed, mismatched or incomplete proof.
 */
inline std::optional<SecureMptLookup> VerifySecureMptProof(
    const uint256& root, const uint256& hashed_key,
    std::span<const std::vector<unsigned char>> nodes, std::string& error)
{
    using namespace mpt_detail;
    const StateProofLimits& limits{STATE_PROOF_LIMITS};
    if (nodes.size() > limits.max_nodes) {
        error = "state proof has too many nodes";
        return std::nullopt;
    }
    size_t total{0};
    for (const auto& node : nodes) {
        if (node.empty() || node.size() > limits.max_node_size) {
            error = "state proof node size is out of range";
            return std::nullopt;
        }
        total += node.size();
    }
    if (total > limits.max_total_bytes) {
        error = "state proof exceeds the byte limit";
        return std::nullopt;
    }
    if (nodes.empty()) {
        if (root == EMPTY_TRIE_ROOT) return SecureMptLookup{};
        error = "state proof is empty for a non-empty root";
        return std::nullopt;
    }
    if (root == EMPTY_TRIE_ROOT) {
        error = "state proof has nodes for the empty root";
        return std::nullopt;
    }
    if (KeccakOf(nodes[0]) != root) {
        error = "state proof root node does not match the root";
        return std::nullopt;
    }

    const std::vector<unsigned char> nib{
        ToNibbles(std::span<const unsigned char>{hashed_key.begin(), 32})};
    size_t key_pos{0};
    size_t next{1};
    std::span<const unsigned char> node_bytes{nodes[0]};
    bool after_extension{false};

    // A child reference is the empty string (no child), a 32-byte hash of a
    // listed node of at least 32 bytes, or an inline node shorter than 32
    // bytes. Anything else cannot appear in a real trie.
    auto valid_reference = [](const RlpItem& child,
                              std::span<const unsigned char> parent) {
        if (!child.is_list) {
            return child.payload.empty() || child.payload.size() == 32;
        }
        return RlpEncodedSpan(child, parent).size() < 32;
    };
    auto descend = [&](const RlpItem& child) -> bool {
        if (child.is_list) {
            node_bytes = RlpEncodedSpan(child, node_bytes);
            return true;
        }
        if (next >= nodes.size()) {
            error = "state proof is missing a node";
            return false;
        }
        const uint256 want{std::span<const unsigned char>{child.payload}};
        if (KeccakOf(nodes[next]) != want) {
            error = "state proof node does not match its reference";
            return false;
        }
        if (nodes[next].size() < 32) {
            error = "state proof hashed node is shorter than 32 bytes";
            return false;
        }
        node_bytes = nodes[next];
        ++next;
        return true;
    };
    auto absent = [&]() -> std::optional<SecureMptLookup> {
        if (next != nodes.size()) {
            error = "state proof proves absence before its last node";
            return std::nullopt;
        }
        return SecureMptLookup{};
    };

    while (true) {
        const auto node{RlpDecode(node_bytes)};
        if (!node) {
            error = "state proof node is not canonical RLP";
            return std::nullopt;
        }
        const auto items{node->is_list ? RlpChildren(*node) : std::nullopt};
        if (!items || (items->size() != 17 && items->size() != 2)) {
            error = "state proof node has an invalid shape";
            return std::nullopt;
        }
        if (after_extension && items->size() != 17) {
            error = "state proof extension does not lead to a branch";
            return std::nullopt;
        }
        after_extension = false;
        const size_t remaining{nib.size() - key_pos};

        if (items->size() == 17) { // branch
            const RlpItem& value{(*items)[16]};
            if (value.is_list || !value.payload.empty()) {
                error = "state proof branch carries a value";
                return std::nullopt;
            }
            for (size_t i = 0; i < 16; ++i) {
                if (!valid_reference((*items)[i], node_bytes)) {
                    error = "state proof child reference is malformed";
                    return std::nullopt;
                }
            }
            if (remaining == 0) {
                error = "state proof key ends inside the trie";
                return std::nullopt;
            }
            const RlpItem& child{(*items)[nib[key_pos]]};
            ++key_pos;
            if (!child.is_list && child.payload.empty()) return absent();
            if (!descend(child)) return std::nullopt;
            continue;
        }

        // Leaf or extension.
        const RlpItem& hp{(*items)[0]};
        bool is_leaf{false};
        const auto path{hp.is_list ? std::nullopt
                                   : DecodeHexPrefix(hp.payload, is_leaf)};
        if (!path) {
            error = "state proof node has an invalid path";
            return std::nullopt;
        }
        const RlpItem& second{(*items)[1]};
        if (is_leaf) {
            if (second.is_list || second.payload.empty()) {
                error = "state proof leaf value is malformed";
                return std::nullopt;
            }
            if (path->size() != remaining) {
                error = "state proof leaf path length does not match the key";
                return std::nullopt;
            }
            if (!std::equal(path->begin(), path->end(), nib.begin() + key_pos)) {
                return absent();
            }
            if (next != nodes.size()) {
                error = "state proof has unused nodes";
                return std::nullopt;
            }
            return SecureMptLookup{
                true, std::vector<unsigned char>(second.payload.begin(),
                                                 second.payload.end())};
        }
        if (path->empty()) {
            error = "state proof extension has an empty path";
            return std::nullopt;
        }
        if (second.payload.empty() || !valid_reference(second, node_bytes)) {
            error = "state proof child reference is malformed";
            return std::nullopt;
        }
        // The extension's child is a branch, which needs one more nibble.
        if (path->size() >= remaining) {
            error = "state proof key ends inside the trie";
            return std::nullopt;
        }
        if (!std::equal(path->begin(), path->end(), nib.begin() + key_pos)) {
            return absent();
        }
        key_pos += path->size();
        if (!descend(second)) return std::nullopt;
        after_extension = true;
    }
}

//! The storage-trie key of an EVM storage slot: keccak(slot).
inline uint256 StorageTrieKey(const StorageWord& slot)
{
    return state_proof_detail::KeccakWord(slot);
}

//! The state-trie key of an Ethereum account: keccak(address).
inline uint256 AccountTrieKey(const EthAddress& address)
{
    return state_proof_detail::KeccakWord(address);
}

/** Verify an account proof against an execution header's stateRoot and
 *  decode the account. An absent account is refused ("account is absent"):
 *  the contracts an observation reads always exist. */
inline std::optional<EthAccount> VerifyAccountProof(
    const uint256& state_root, const EthAddress& address,
    std::span<const std::vector<unsigned char>> nodes, std::string& error)
{
    const auto lookup{
        VerifySecureMptProof(state_root, AccountTrieKey(address), nodes, error)};
    if (!lookup) return std::nullopt;
    if (!lookup->present) {
        error = "account is absent";
        return std::nullopt;
    }
    const auto top{RlpDecode(lookup->value)};
    const auto fields{top && top->is_list ? RlpChildren(*top) : std::nullopt};
    if (!fields || fields->size() != 4 ||
        !state_proof_detail::CanonicalInteger((*fields)[0], 8) ||
        !state_proof_detail::CanonicalInteger((*fields)[1], 32) ||
        (*fields)[2].is_list || (*fields)[2].payload.size() != 32 ||
        (*fields)[3].is_list || (*fields)[3].payload.size() != 32) {
        error = "account value is malformed";
        return std::nullopt;
    }
    EthAccount out;
    for (unsigned char b : (*fields)[0].payload) out.nonce = (out.nonce << 8) | b;
    out.balance = state_proof_detail::LeftPad((*fields)[1].payload);
    out.storage_root = uint256{(*fields)[2].payload};
    out.code_hash = uint256{(*fields)[3].payload};
    return out;
}

/** How VerifyStorageValue treats a present leaf holding the RLP of zero
 *  (0x80, the empty string). Ethereum clients delete a slot set to zero, so
 *  node changes section 8.2 calls a present zero malformed (REFUSE). anvil's
 *  in-memory state keeps a leaf of zero for every slot a transaction only
 *  read (seen in RT4 for the token's slot 10 and isBlackListed[vault] after
 *  a deposit; anvil 1.7.1 eth_getProof), so only the regtest origin chain
 *  (anvil, 31337) reads it as zero (ACCEPT). The root commits to such a
 *  leaf as firmly as to an absence, so ACCEPT is sound; it is a deviation
 *  from section 8.2 limited to anvil. */
enum class StoredZero : uint8_t {
    REFUSE,
    ACCEPT,
};

/** Verify a storage proof against an account's storageRoot and return the
 *  slot's word. An absent slot reads as zero. A present value must be the
 *  RLP string of the word with leading zero bytes stripped (1-32 bytes, first
 *  byte non-zero); the RLP of zero itself is refused unless `stored_zero` is
 *  ACCEPT, and a padded value, an over-long value or a list is refused. */
inline std::optional<StorageWord> VerifyStorageValue(
    const uint256& storage_root, const StorageWord& slot,
    std::span<const std::vector<unsigned char>> nodes, std::string& error,
    const StoredZero stored_zero = StoredZero::REFUSE)
{
    const auto lookup{
        VerifySecureMptProof(storage_root, StorageTrieKey(slot), nodes, error)};
    if (!lookup) return std::nullopt;
    if (!lookup->present) return StorageWord{};
    const auto item{RlpDecode(lookup->value)};
    if (!item || item->is_list || item->payload.size() > 32 ||
        (!item->payload.empty() && item->payload[0] == 0x00)) {
        error = "storage value is malformed";
        return std::nullopt;
    }
    if (item->payload.empty() && stored_zero != StoredZero::ACCEPT) {
        error = "storage value is a stored zero, which only anvil keeps";
        return std::nullopt;
    }
    // An empty payload here is anvil's stored zero described above.
    return state_proof_detail::LeftPad(item->payload);
}

//! The storage word of a uint64 (slot numbers, mapping slot bases).
inline StorageWord SlotWord(uint64_t value)
{
    StorageWord out{};
    for (size_t i = 0; i < 8; ++i) {
        out[31 - i] = static_cast<unsigned char>(value >> (8 * i));
    }
    return out;
}

//! The ABI word of an address: 12 zero bytes, then the 20 address bytes.
inline StorageWord AddressWord(const EthAddress& address)
{
    return state_proof_detail::LeftPad(address);
}

//! Solidity's slot for `mapping[key]` declared at slot `slot`:
//! keccak(key_word ‖ SlotWord(slot)), for value-type keys.
inline StorageWord MappingSlot(const StorageWord& key_word, uint64_t slot)
{
    const StorageWord slot_word{SlotWord(slot)};
    Keccak256 hasher;
    hasher.Write(key_word);
    hasher.Write(slot_word);
    uint256 hash;
    hasher.Finalize(hash);
    StorageWord out;
    std::copy(hash.begin(), hash.end(), out.begin());
    return out;
}

/** Read `bits` bits (1-64) starting at `bit_offset` from a storage word,
 *  where bit 0 is the least significant bit. Solidity packs a struct's
 *  first member into the lowest-order bytes of its slot, so a member at byte
 *  offset k (forge inspect "offset") starts at bit 8k. */
inline uint64_t WordField(const StorageWord& word, unsigned bit_offset, unsigned bits)
{
    assert(bits >= 1 && bits <= 64 && bit_offset + bits <= 256);
    uint64_t out{0};
    for (unsigned i = 0; i < bits; ++i) {
        const unsigned bit{bit_offset + i};
        const unsigned char byte{word[31 - bit / 8]};
        out |= static_cast<uint64_t>((byte >> (bit % 8)) & 1) << i;
    }
    return out;
}

} // namespace bridge

#endif // B3COIN_BRIDGE_STATE_PROOF_H
