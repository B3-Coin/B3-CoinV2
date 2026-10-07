// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3COIN_TEST_UTIL_BRIDGE_ETH_TRIE_H
#define B3COIN_TEST_UTIL_BRIDGE_ETH_TRIE_H

#include <bridge/rlp.h>
#include <bridge/state_proof.h>
#include <crypto/keccak256.h>
#include <uint256.h>

#include <algorithm>
#include <map>
#include <span>
#include <stdexcept>
#include <vector>

/** A minimal Ethereum Merkle-Patricia trie builder for tests only.
 *
 *  It holds the key/value pairs in memory and rebuilds the trie from scratch
 *  on every Root() or Proof() call, so it is slow and only fit for small
 *  synthetic tries. Keys may have any length (the classic do/dog/doge/horse
 *  vector stores a value in a branch node); the Secure* helpers hash keys
 *  the way the state and storage tries do. Proof() returns the nodes an
 *  eth_getProof response would list for the key, present or absent: the root
 *  node, then every hash-referenced node on the key's path, with nodes
 *  shorter than 32 bytes left embedded in their parent.
 *
 *  The builder walks the key while it constructs the trie, independently of
 *  the walk in bridge/state_proof.h, so the two cross-check each other.
 */
namespace bridge_test {

class EthTrie
{
public:
    using Bytes = std::vector<unsigned char>;

    void Insert(std::span<const unsigned char> key, std::span<const unsigned char> value)
    {
        if (value.empty()) throw std::invalid_argument("EthTrie stores no empty values");
        m_entries[ToNibbles(key)] = Bytes(value.begin(), value.end());
    }
    void Erase(std::span<const unsigned char> key) { m_entries.erase(ToNibbles(key)); }

    uint256 Root() const
    {
        std::vector<Bytes> ignored;
        return HashOf(RootEncoding(nullptr, ignored));
    }

    //! The proof for `key`, root first.
    std::vector<Bytes> Proof(std::span<const unsigned char> key) const
    {
        const Bytes target{ToNibbles(key)};
        std::vector<Bytes> reversed;
        RootEncoding(&target, reversed);
        return {reversed.rbegin(), reversed.rend()};
    }

    //! Secure-trie helpers: the key is hashed with keccak first.
    void InsertSecure(std::span<const unsigned char> key, std::span<const unsigned char> value)
    {
        const uint256 hashed{HashOf(key)};
        Insert(std::span<const unsigned char>{hashed.begin(), 32}, value);
    }
    std::vector<Bytes> ProofSecure(std::span<const unsigned char> key) const
    {
        const uint256 hashed{HashOf(key)};
        return Proof(std::span<const unsigned char>{hashed.begin(), 32});
    }

    static uint256 HashOf(std::span<const unsigned char> data)
    {
        uint256 out;
        Keccak256().Write(data).Finalize(out);
        return out;
    }

    //! Key bytes to nibbles, high nibble first.
    static Bytes ToNibbles(std::span<const unsigned char> key)
    {
        Bytes out;
        for (unsigned char b : key) {
            out.push_back(b >> 4);
            out.push_back(b & 0x0f);
        }
        return out;
    }

    //! Hex-prefix encoding of a nibble path (leaf flag 2, odd flag 1).
    static Bytes HexPrefix(std::span<const unsigned char> nibbles, bool leaf)
    {
        Bytes out;
        const bool odd{nibbles.size() % 2 == 1};
        const unsigned char flags{static_cast<unsigned char>((leaf ? 2 : 0) | (odd ? 1 : 0))};
        size_t i{0};
        if (odd) {
            out.push_back(static_cast<unsigned char>((flags << 4) | nibbles[0]));
            i = 1;
        } else {
            out.push_back(static_cast<unsigned char>(flags << 4));
        }
        for (; i < nibbles.size(); i += 2) {
            out.push_back(static_cast<unsigned char>((nibbles[i] << 4) | nibbles[i + 1]));
        }
        return out;
    }

private:
    using Entry = std::pair<const Bytes, Bytes>;
    std::map<Bytes, Bytes> m_entries;

    //! How a parent refers to an encoded child: inline when shorter than 32
    //! bytes, else rlp(keccak(child)).
    static Bytes Reference(const Bytes& encoded)
    {
        if (encoded.size() < 32) return encoded;
        const uint256 hash{HashOf(encoded)};
        return bridge::RlpEncodeBytes(std::span<const unsigned char>{hash.begin(), 32});
    }

    Bytes RootEncoding(const Bytes* target, std::vector<Bytes>& reversed) const
    {
        std::vector<const Entry*> all;
        for (const auto& e : m_entries) all.push_back(&e);
        if (all.empty()) return bridge::RlpEncodeBytes(std::span<const unsigned char>{});
        return Build(all, 0, target, reversed, /*is_root=*/true);
    }

    //! Encode the node holding `entries` (all sharing their first `depth`
    //! nibbles). When `target` runs through this node, append the node's
    //! encoding to `reversed` after its children's (so the list is deepest
    //! first) if the node is the root (always listed, whatever its size) or
    //! is referenced by hash.
    static Bytes Build(const std::vector<const Entry*>& entries, size_t depth,
                       const Bytes* target, std::vector<Bytes>& reversed,
                       bool is_root)
    {
        Bytes enc;
        if (entries.size() == 1) {
            const Bytes& key{entries[0]->first};
            const std::span<const unsigned char> rest{key.data() + depth, key.size() - depth};
            enc = bridge::RlpEncodeList({bridge::RlpEncodeBytes(HexPrefix(rest, true)),
                                         bridge::RlpEncodeBytes(entries[0]->second)});
        } else {
            // Longest common prefix from `depth`, bounded by the shortest key.
            size_t common{SIZE_MAX};
            const Bytes& first{entries[0]->first};
            for (const Entry* e : entries) {
                const Bytes& k{e->first};
                size_t n{0};
                while (depth + n < k.size() && depth + n < first.size() &&
                       k[depth + n] == first[depth + n]) {
                    ++n;
                }
                common = std::min(common, n);
            }
            if (common > 0) {
                const std::span<const unsigned char> prefix{first.data() + depth, common};
                const bool on_path{target != nullptr && target->size() >= depth + common &&
                                   std::equal(prefix.begin(), prefix.end(),
                                              target->begin() + depth)};
                const Bytes child{Build(entries, depth + common, on_path ? target : nullptr,
                                        reversed, /*is_root=*/false)};
                enc = bridge::RlpEncodeList({bridge::RlpEncodeBytes(HexPrefix(prefix, false)),
                                             Reference(child)});
            } else {
                std::vector<Bytes> items(17, bridge::RlpEncodeBytes(std::span<const unsigned char>{}));
                for (unsigned char n = 0; n < 16; ++n) {
                    std::vector<const Entry*> group;
                    for (const Entry* e : entries) {
                        if (e->first.size() > depth && e->first[depth] == n) group.push_back(e);
                    }
                    if (group.empty()) continue;
                    const bool on_path{target != nullptr && target->size() > depth &&
                                       (*target)[depth] == n};
                    items[n] = Reference(Build(group, depth + 1, on_path ? target : nullptr,
                                               reversed, /*is_root=*/false));
                }
                for (const Entry* e : entries) {
                    if (e->first.size() == depth) items[16] = bridge::RlpEncodeBytes(e->second);
                }
                enc = bridge::RlpEncodeList(items);
            }
        }
        if (target != nullptr && (is_root || enc.size() >= 32)) reversed.push_back(enc);
        return enc;
    }
};

//! The RLP account value a state-trie leaf stores.
inline std::vector<unsigned char> EncodeAccount(uint64_t nonce, const bridge::StorageWord& balance,
                                                const uint256& storage_root, const uint256& code_hash)
{
    auto trimmed = [](std::span<const unsigned char> word) {
        size_t i{0};
        while (i < word.size() && word[i] == 0) ++i;
        return std::vector<unsigned char>(word.begin() + i, word.end());
    };
    return bridge::RlpEncodeList({bridge::RlpEncodeUint64(nonce),
                                  bridge::RlpEncodeBytes(trimmed(balance)),
                                  bridge::RlpEncodeBytes(std::span<const unsigned char>{storage_root.begin(), 32}),
                                  bridge::RlpEncodeBytes(std::span<const unsigned char>{code_hash.begin(), 32})});
}

//! The RLP value a storage-trie leaf stores for a non-zero word.
inline std::vector<unsigned char> EncodeStorageValue(const bridge::StorageWord& word)
{
    size_t i{0};
    while (i < word.size() && word[i] == 0) ++i;
    if (i == word.size()) throw std::invalid_argument("a zero word is never stored");
    return bridge::RlpEncodeBytes(std::span<const unsigned char>{word.data() + i, word.size() - i});
}

/** A storage trie keyed the way Ethereum keys it: Set(slot, word) inserts
 *  keccak(slot) -> rlp(word without leading zeros); a zero word deletes. */
class EthStorageTrie
{
public:
    void Set(const bridge::StorageWord& slot, const bridge::StorageWord& word)
    {
        if (std::all_of(word.begin(), word.end(), [](unsigned char b) { return b == 0; })) {
            m_trie.Erase(Key(slot));
        } else {
            m_trie.Insert(Key(slot), EncodeStorageValue(word));
        }
    }
    uint256 Root() const { return m_trie.Root(); }
    std::vector<std::vector<unsigned char>> Proof(const bridge::StorageWord& slot) const
    {
        return m_trie.Proof(Key(slot));
    }
    EthTrie& Raw() { return m_trie; }

private:
    EthTrie m_trie;
    static std::vector<unsigned char> Key(const bridge::StorageWord& slot)
    {
        const uint256 hashed{EthTrie::HashOf(slot)};
        return {hashed.begin(), hashed.end()};
    }
};

} // namespace bridge_test

#endif // B3COIN_TEST_UTIL_BRIDGE_ETH_TRIE_H
