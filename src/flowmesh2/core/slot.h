// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_CORE_SLOT_H
#define B3_FLOWMESH2_CORE_SLOT_H

#include <flowmesh2/core/authority.h>
#include <flowmesh2/core/result.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <ios>
#include <span>
#include <vector>

/** The FlowMesh V2 slot header (slice R3; ARCHITECTURE.md section 9).
 *
 * A certified slot is identified by Hash() = Tagged(SLOT, bytes), which the
 * next header names as parent_hash. Seats sign CommitDigest() =
 * Tagged(COMMIT, bytes); the different tag keeps a header hash from ever
 * being a valid signing message. Both cover every field.
 *
 * Fixed-size encoding, SLOT_HEADER_BYTES = 366, in field order:
 *   u8 version (= 1) | domain | profile_hash | u64 authority_epoch | u64 height
 *   | parent_hash | parent_cert_hash | anchor (u32 height, hash)
 *   | facts_from (u32, u32) | facts_to (u32, u32) | seat_set_root | facts_root
 *   | actions_root | outcomes_root | state_root | u32 action_count
 *   | u32 outcome_count | u8 commitment_version (= 0)
 * Integers are little-endian, hashes 32 raw bytes. Decode refuses an unknown
 * version or commitment_version (DECODE), trailing bytes (DECODE) and longer
 * input (OVERSIZE).
 *
 * Field meaning:
 * - domain: DomainHash(network, fm_version); profile_hash: ProfileHash.
 * - authority_epoch, seat_set_root, profile_hash: the AuthorityVersion the slot
 *   was certified under (Authority()).
 * - height: 0 for the genesis header, parent height + 1 after.
 * - parent_cert_hash: hash of the parent's certificate (R4); zero at genesis.
 * - facts_from/facts_to: the half-open range of chain facts consumed.
 * - facts_root, actions_root (exact original action bytes in proposal order),
 *   outcomes_root: committed Merkle roots (core/merkle.h).
 * - state_root: commitment version `commitment_version` (0 = sorted RFC 6962
 *   trees, R5; 1 = sparse Merkle tree, M2-LED-02).
 * The fields profile_hash, authority_epoch and facts_root are additions to the
 * plan's header text (O-20).
 */
namespace flowmesh2 {

inline constexpr uint8_t SLOT_HEADER_VERSION{1};
inline constexpr uint8_t COMMITMENT_VERSION_V0{0};
inline constexpr size_t SLOT_HEADER_BYTES{366};

struct SlotHeaderV2 {
    uint8_t version{SLOT_HEADER_VERSION};
    uint256 domain;
    uint256 profile_hash;
    uint64_t authority_epoch{0};
    uint64_t height{0};
    uint256 parent_hash;
    uint256 parent_cert_hash;
    Anchor anchor;
    FactCursor facts_from;
    FactCursor facts_to;
    uint256 seat_set_root;
    uint256 facts_root;
    uint256 actions_root;
    uint256 outcomes_root;
    uint256 state_root;
    uint32_t action_count{0};
    uint32_t outcome_count{0};
    uint8_t commitment_version{COMMITMENT_VERSION_V0};

    //! Tagged(SLOT, Encode()): identity and parent link.
    uint256 Hash() const;
    //! Tagged(COMMIT, Encode()): the message the seats sign.
    uint256 CommitDigest() const;
    //! {authority_epoch, seat_set_root, profile_hash}.
    AuthorityVersion Authority() const;
    //! The SLOT_HEADER_BYTES canonical bytes.
    std::vector<unsigned char> Encode() const;
    //! Exact decode of SLOT_HEADER_BYTES bytes.
    static Result<SlotHeaderV2> Decode(std::span<const unsigned char> bytes);

    friend bool operator==(const SlotHeaderV2&, const SlotHeaderV2&) = default;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << version << domain << profile_hash << authority_epoch << height << parent_hash << parent_cert_hash
          << anchor << facts_from << facts_to << seat_set_root << facts_root << actions_root << outcomes_root
          << state_root << action_count << outcome_count << commitment_version;
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> version;
        if (version != SLOT_HEADER_VERSION) throw std::ios_base::failure("unknown SlotHeaderV2 version");
        s >> domain >> profile_hash >> authority_epoch >> height >> parent_hash >> parent_cert_hash >> anchor >>
            facts_from >> facts_to >> seat_set_root >> facts_root >> actions_root >> outcomes_root >> state_root >>
            action_count >> outcome_count >> commitment_version;
        if (commitment_version != COMMITMENT_VERSION_V0) {
            throw std::ios_base::failure("unknown SlotHeaderV2 commitment_version");
        }
    }
};

/** The height-0 header of a FlowMesh instance: no parent (zero hashes), the
 * empty fact range [facts_start, facts_start), empty facts, actions and
 * outcomes trees, zero counts and the genesis state root (computed by the
 * state code, R9). INVALID_INPUT for a zero domain, profile hash, seat-set
 * root or state root.
 */
Result<SlotHeaderV2> GenesisHeader(const uint256& domain, const AuthorityVersion& authority, const Anchor& anchor,
                                   const FactCursor& facts_start, const uint256& genesis_state_root);

} // namespace flowmesh2

#endif // B3_FLOWMESH2_CORE_SLOT_H
