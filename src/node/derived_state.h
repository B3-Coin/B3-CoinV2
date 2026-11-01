// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
#ifndef B3COIN_NODE_DERIVED_STATE_H
#define B3COIN_NODE_DERIVED_STATE_H

#include <dbwrapper.h>
#include <uint256.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace node {

/**
 * B0b: derived chain state persisted across restarts
 * (doc/design/b3-b0b-durable-derived-state.md).
 *
 * A tracker's state at a block is a pure function of the chain up to that
 * block, so a record naming its block stays valid while that block is on the
 * active chain. Records are a cache. A node uses one only when its format,
 * its build and the consensus parameters its tracker reads all match and its
 * checksum holds; otherwise the tracker rebuilds from blocks as before.
 */
enum class DerivedStateId : uint8_t {
    //! The finality tracker with its private stake registry and binding
    //! index (FinalityTracker::Encode).
    FINALITY_GROUP = 1,
    //! The chainstate's binding index (FinalityBindingIndex::Encode).
    FINALITY_BINDINGS = 2,
    // 3 to 6 are reserved for the bridge, FN seat, FlowMesh vault and
    // FlowMesh checkpoint indexes.
};

struct DerivedStateRecord {
    //! Digest of every consensus parameter the tracker's derivation reads.
    uint256 params_digest;
    uint256 block_hash;
    int height{-1};
    //! The tracker's canonical encoding of its state at the block.
    std::vector<unsigned char> payload;
};

enum class DerivedStateCheck {
    OK,
    MALFORMED,    //!< not exactly one complete record
    BAD_CHECKSUM,
    OTHER_FORMAT,
    OTHER_BUILD,  //!< written by another build
    OTHER_PARAMS, //!< written under other consensus parameters
};
std::string_view DerivedStateCheckName(DerivedStateCheck check);

/**
 * u8 format version || build id || parameters digest || block hash || i32
 * height || payload || SHA256d of everything before it. The build id names
 * the exact sources (SourceBuildId()), so a record written by any other
 * build is refused and no change of derivation can meet a stale record.
 */
std::vector<unsigned char> EncodeDerivedStateRecord(const DerivedStateRecord& record, std::string_view build_id);
DerivedStateCheck DecodeDerivedStateRecord(std::span<const unsigned char> bytes, std::string_view build_id,
                                           const uint256& params_digest, DerivedStateRecord& record);

//! Blocks between two periodic records: after a crash, a node replays at
//! most this many blocks on top of its last record, plus those since its
//! last flush.
inline constexpr int DERIVED_STATE_WRITE_INTERVAL{144};

/**
 * The write rule, applied at each coins flush to a tracker in step with the
 * tip. A forced flush (the shutdown flush, an explicit one) writes;
 * otherwise a record is due when the store has none, when its block has
 * left the active chain, or when it is DERIVED_STATE_WRITE_INTERVAL or
 * more blocks below the tip.
 */
bool DerivedStateWriteDue(bool forced, std::optional<int> written_height, bool written_on_chain, int tip_height);

//! The `derivedstate` LevelDB: one record per tracker, each write synchronous.
class DerivedStateStore
{
public:
    explicit DerivedStateStore(const DBParams& params);

    std::optional<std::vector<unsigned char>> Read(DerivedStateId id) const;
    void Write(DerivedStateId id, const std::vector<unsigned char>& record);
    //! Several records in one synchronous batch: all of them land, or none.
    void Write(std::span<const std::pair<DerivedStateId, std::vector<unsigned char>>> records);
    void Erase(DerivedStateId id);

private:
    CDBWrapper m_db;
};

} // namespace node

#endif // B3COIN_NODE_DERIVED_STATE_H
