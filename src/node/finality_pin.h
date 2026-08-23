// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
#ifndef B3COIN_NODE_FINALITY_PIN_H
#define B3COIN_NODE_FINALITY_PIN_H

#include <protocol.h>
#include <uint256.h>
#include <util/fs.h>

#include <optional>
#include <utility>

namespace node {

/**
 * Persisted finality pin (plan Commit 14A; b3-cross-chain-finality-v1.md
 * section 4 "Finality pin"). The highest checkpoint ever pinned by this node
 * as (height, hash), stored in `<blocksdir>/finality_pin.dat` so that:
 *
 *   - once (height, hash) is final, a restart cannot forget it;
 *   - an allowed reorg above the checkpoint that removes the certificate
 *     carrier cannot remove the protection (the file only ever grows);
 *   - load / restart / -reindex / -reindex-chainstate cannot reopen a fork
 *     below previously accepted finality (the file lives beside the block
 *     files, outside both wiped databases);
 *   - the write is atomic and crash-safe: temp file, fsync, rename-over
 *     (the peers.dat / anchors.dat pattern);
 *   - the content is monotone: a lower height is never written over a
 *     higher one (WriteFinalityPin refuses it);
 *   - the stored hash is exactly the hash of the pinned checkpoint.
 *
 * Format: network message-start magic || u8 version (1) || i32 height ||
 * uint256 hash || SHA256d checksum over the preceding bytes. A file for a
 * different network, a corrupt file or an unknown version is ignored.
 * There is no administrative override and no automatic rollback.
 */
inline constexpr uint8_t FINALITY_PIN_FILE_VERSION{1};
inline const char* const FINALITY_PIN_FILENAME{"finality_pin.dat"};

struct FinalityPin {
    int height{-1};
    uint256 hash{};
    friend bool operator==(const FinalityPin& a, const FinalityPin& b) { return a.height == b.height && a.hash == b.hash; }
};

//! Read the pin file; nullopt when absent, corrupt, for another network or
//! of an unknown version (each logged).
std::optional<FinalityPin> ReadFinalityPin(const fs::path& path, const MessageStartChars& magic);

/**
 * Persist `pin` atomically unless the file already holds a pin at an equal
 * or greater height (monotone; returns true without writing in that case).
 * Returns false only on an I/O failure.
 */
bool WriteFinalityPin(const fs::path& path, const MessageStartChars& magic, const FinalityPin& pin);

} // namespace node

#endif // B3COIN_NODE_FINALITY_PIN_H
