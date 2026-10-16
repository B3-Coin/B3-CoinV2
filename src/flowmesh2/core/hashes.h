// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_CORE_HASHES_H
#define B3_FLOWMESH2_CORE_HASHES_H

#include <hash.h>
#include <uint256.h>

#include <cstdint>
#include <span>
#include <string_view>

/** Tagged hashes of the FlowMesh V2 clean core (rebirth outline, slice R3).
 *
 * Every fm2 hash is a BIP340 tagged hash, SHA256(SHA256(t) || SHA256(t) || m),
 * computed with Bitcoin's HashWriter/TaggedHash (hash.h). The tag text t is
 * always "B3/FM2/" followed by the registry name. No tag text may equal any
 * other "B3/" tag string in the repository (hashes_tests scans the tree).
 *
 * The registry below is the ONLY place that names a tag. It is APPEND-ONLY and
 * owner-protected (TODO.md rule 5): never renumber, rename or remove an entry;
 * a new tag takes the next integer. hashes_tests pins every integer, text and
 * the digest Tagged(tag, empty).
 *
 * Deterministic: no allocation besides HashWriter state, no I/O, clock or
 * locale.
 */
namespace flowmesh2 {

//! Tag registry. The enumerator for "B3/FM2/DOMAIN" is NET_DOMAIN, because the
//! macOS <math.h> defines DOMAIN as a macro (ARCHITECTURE.md O-21).
enum class HashTag : uint8_t {
    NET_DOMAIN = 1,        //!< DomainHash: network and FlowMesh version
    ACCOUNT = 2,           //!< AccountId from an x-only key
    ACTION = 3,            //!< ActionId (R4)
    ACTION_LEAF = 4,       //!< leaf of the exact original action bytes (R4)
    BODY = 5,              //!< slot body (R4)
    FACT = 6,              //!< FactId (R4)
    FACTS = 7,             //!< facts list (R4)
    MARKET = 8,            //!< MarketId
    ORDER = 9,             //!< within-slot canonical order key (R7)
    SLOT = 10,             //!< SlotHeaderV2::Hash, identity and parent link
    COMMIT = 11,           //!< SlotHeaderV2::CommitDigest, the BLS message
    PREPARE = 12,          //!< PBFT prepare digest (M4-AGR-02)
    CERT = 13,             //!< certificate hash (R4)
    OUTCOME = 14,          //!< outcome leaf (R4)
    SEATSET = 15,          //!< seat-set root (R4)
    PROFILE = 16,          //!< ProfileHash
    STATE = 17,            //!< state root of roots (R5)
    STATE_ACCOUNTS = 18,   //!< commitment v0 component (R5)
    STATE_ORDERS = 19,     //!< commitment v0 component (R5)
    STATE_MARKETS = 20,    //!< commitment v0 component (R5)
    STATE_NULLIFIERS = 21, //!< commitment v0 component (R5)
    STATE_TOTALS = 22,     //!< commitment v0 component (R5)
    MERKLE_LEAF = 23,      //!< Merkle leaf node
    MERKLE_NODE = 24,      //!< Merkle inner node
    MERKLE_EMPTY = 25,     //!< Merkle tree hash of zero leaves
    MERKLE_ROOT = 26,      //!< committed Merkle root: leaf count and tree hash
    PROTOCOL_ACCOUNT = 27, //!< seat pool and treasury accounts (R5)
    JOURNAL = 28,          //!< first chain value of a journal (R8)
    TEST_ASSET = 29,       //!< synthetic test and simulator assets only
};

//! The prefix of every tag text.
inline constexpr std::string_view HASH_TAG_PREFIX{"B3/FM2/"};

struct HashTagInfo {
    HashTag tag;
    std::string_view name; //!< registry name, e.g. "ACCOUNT"
    std::string_view text; //!< full tag text, e.g. "B3/FM2/ACCOUNT"
};

//! The registry in ascending integer order, one entry per HashTag value.
std::span<const HashTagInfo> HashTagRegistry();
//! True only for a value listed in HashTagRegistry().
bool IsKnown(HashTag tag);
//! The full tag text, or "" for an unregistered value.
std::string_view TagText(HashTag tag);

//! Tagged(tag, bytes). Asserts that the tag is registered.
uint256 Tagged(HashTag tag, std::span<const unsigned char> bytes);

//! A HashWriter already primed with SHA256(t) || SHA256(t) for the tag; write
//! the message (bytes or serializable objects) and call GetSHA256().
HashWriter TaggedWriter(HashTag tag);

//! Tagged hash of the Bitcoin serialization of `obj`.
template <class T>
uint256 TaggedObject(HashTag tag, const T& obj)
{
    HashWriter writer{TaggedWriter(tag)};
    writer << obj;
    return writer.GetSHA256();
}

} // namespace flowmesh2

#endif // B3_FLOWMESH2_CORE_HASHES_H
