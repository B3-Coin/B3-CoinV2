// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/core/slot.h>

#include <flowmesh2/core/codec.h>
#include <flowmesh2/core/hashes.h>
#include <flowmesh2/core/merkle.h>

namespace flowmesh2 {

uint256 SlotHeaderV2::Hash() const { return TaggedObject(HashTag::SLOT, *this); }

uint256 SlotHeaderV2::CommitDigest() const { return TaggedObject(HashTag::COMMIT, *this); }

AuthorityVersion SlotHeaderV2::Authority() const
{
    return AuthorityVersion{authority_epoch, seat_set_root, profile_hash};
}

std::vector<unsigned char> SlotHeaderV2::Encode() const { return EncodeBytes(*this); }

Result<SlotHeaderV2> SlotHeaderV2::Decode(std::span<const unsigned char> bytes)
{
    return DecodeExact<SlotHeaderV2>(bytes, SLOT_HEADER_BYTES);
}

Result<SlotHeaderV2> GenesisHeader(const uint256& domain, const AuthorityVersion& authority, const Anchor& anchor,
                                   const FactCursor& facts_start, const uint256& genesis_state_root)
{
    if (domain.IsNull()) return Error{ErrorCode::INVALID_INPUT, "genesis domain is zero"};
    if (authority.profile_hash.IsNull()) return Error{ErrorCode::INVALID_INPUT, "genesis profile hash is zero"};
    if (authority.seat_set_root.IsNull()) return Error{ErrorCode::INVALID_INPUT, "genesis seat-set root is zero"};
    if (genesis_state_root.IsNull()) return Error{ErrorCode::INVALID_INPUT, "genesis state root is zero"};

    const uint256 empty_root{MerkleRoot({})};
    SlotHeaderV2 header;
    header.version = SLOT_HEADER_VERSION;
    header.domain = domain;
    header.profile_hash = authority.profile_hash;
    header.authority_epoch = authority.epoch;
    header.height = 0;
    header.parent_hash = uint256::ZERO;
    header.parent_cert_hash = uint256::ZERO;
    header.anchor = anchor;
    header.facts_from = facts_start;
    header.facts_to = facts_start;
    header.seat_set_root = authority.seat_set_root;
    header.facts_root = empty_root;
    header.actions_root = empty_root;
    header.outcomes_root = empty_root;
    header.state_root = genesis_state_root;
    header.action_count = 0;
    header.outcome_count = 0;
    header.commitment_version = COMMITMENT_VERSION_V0;
    return Ok(header);
}

} // namespace flowmesh2
