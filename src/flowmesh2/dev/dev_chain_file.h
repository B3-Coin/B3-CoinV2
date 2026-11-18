// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_DEV_DEV_CHAIN_FILE_H
#define B3_FLOWMESH2_DEV_DEV_CHAIN_FILE_H

#ifndef FM2_DEV_COMPONENTS
#error "dev/dev_chain_file.h is a REGTEST dev component: only targets that link fm2_dev (FM2_DEV_COMPONENTS) may include it"
#endif

#include <flowmesh2/core/authority.h>
#include <flowmesh2/core/facts.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/seat_set.h>
#include <uint256.h>
#include <util/fs.h>

#include <cstdint>
#include <string_view>
#include <vector>

/** The shared dev chain of a regtest cluster (stage 2 step 5, M5-NODE-01;
 * O-105). DEV ONLY (fm2_dev): until the b3d adapter lands (stage 3,
 * MB1-ADP-06) every fm2d of a regtest cluster tails ONE append-only file of
 * scripted blocks and feeds them to its FakeChainAdapter, so every seat sees
 * exactly the same facts in the same order (M4-AGR-12) and a restarted seat
 * replays the whole chain from the file (its anchors come back).
 *
 * Record: `u32 len | u32 crc32c(payload) | payload`, payload =
 * `u32 height | hash (32) | u32 n | n x (u32 len | fact bytes)` (EncodeFact),
 * little-endian. Blocks are appended by `fm2-devchain` (the faucet) with
 * fsync, one writer at a time (an exclusive flock on the file from the read
 * to the fsync; readers hold it shared while they read); a reader that finds a partial record at the tail waits for
 * the rest, a CRC or shape error is STORAGE_FATAL. A complete record is never
 * rewritten; a torn one that a dead writer left is cut by the next append.
 * Heights are consecutive from base + 1; hashes are DevChainBlockHash(height).
 */
namespace flowmesh2::dev {

inline constexpr std::string_view DEVCHAIN_OPEN{"the dev chain file could not be opened"};        //!< (STORAGE_FATAL)
inline constexpr std::string_view DEVCHAIN_CORRUPT{"a dev chain record is corrupt"};              //!< (STORAGE_FATAL)
inline constexpr std::string_view DEVCHAIN_WRITE{"the dev chain file could not be written"};      //!< (STORAGE_FATAL)
inline constexpr std::string_view DEVCHAIN_HEIGHT{"a dev chain block is not the next height"};    //!< (INVALID_INPUT)
inline constexpr std::string_view DEVCHAIN_DEPOSIT_TOTAL{"the deposits of an asset over the dev chain would exceed MAX_ATOMS"}; //!< (INVALID_INPUT)
inline constexpr size_t DEVCHAIN_MAX_RECORD_BYTES{16 * 1024 * 1024};

struct DevChainBlock {
    uint32_t height{0};
    uint256 hash;
    std::vector<Fact> facts;
    friend bool operator==(const DevChainBlock&, const DevChainBlock&) = default;
};

//! SHA256("fm2/devchain/block/<height>").
uint256 DevChainBlockHash(uint32_t height);
//! The outpoint of deposit `index` of block `height`: SHA256("fm2/devchain/deposit/<height>/<index>"):0.
Outpoint DevChainOutpoint(uint32_t height, uint32_t index);

//! INVALID_INPUT when a fact does not encode (a deposit outside [0, MAX_ATOMS]): nothing a reader could not decode is ever written.
Result<std::vector<unsigned char>> EncodeDevChainBlock(const DevChainBlock& block);
Result<DevChainBlock> DecodeDevChainBlock(std::span<const unsigned char> payload);

//! Append one block (creating the file), fsync'd before returning; under the file's lock, so the height check holds against
//! any other writer (DEVCHAIN_HEIGHT if one appended first).
Result<void> AppendDevChainBlock(const fs::path& file, const DevChainBlock& block);
//! Whether every fm2d of the cluster takes `block` after `existing`: Ok, or the refusal a seat would give. The blocks go
//! through a FakeChainAdapter built from the same network, authority, seats and base as fm2d's (MAX_SLOT_FACTS, cursors,
//! protocol accounts, repeated outpoints), and each asset's deposits over the whole file stay within MAX_ATOMS (O-34: a
//! deposit total above it is a ledger error no slot can carry, so DEVCHAIN_DEPOSIT_TOTAL). The faucet checks a block
//! with this before it appends it: a block a seat refuses wedges every seat that tails the file (review workflow of
//! 2026-11-18).
Result<void> CheckDevChainBlock(const NetworkId& network, const AuthorityVersion& authority, const SeatSet& seats, const Anchor& base,
                                const std::vector<DevChainBlock>& existing, const DevChainBlock& block);
//! Every complete block of the file in order (an absent file is an empty chain).
Result<std::vector<DevChainBlock>> ReadDevChain(const fs::path& file);

//! A tail reader: Poll() gives the complete records appended since the last call.
class DevChainTail
{
public:
    explicit DevChainTail(fs::path file) : m_file{std::move(file)} {}
    Result<std::vector<DevChainBlock>> Poll();
    uint64_t Offset() const { return m_offset; }

private:
    fs::path m_file;
    uint64_t m_offset{0};
};

} // namespace flowmesh2::dev

#endif // B3_FLOWMESH2_DEV_DEV_CHAIN_FILE_H
