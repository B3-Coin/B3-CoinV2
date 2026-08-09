// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_LEGACY_CODEC_H
#define B3COIN_LEGACY_CODEC_H

#include <primitives/transaction.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>

class CBlock;

namespace legacy {

/**
 * PROTOTYPE-ONLY SKELETON. Interfaces isolating the historical B3Coin wire
 * formats (transactions carrying nTime, blocks carrying a trailing
 * proof-of-stake signature) behind src/legacy.
 *
 * NOTE ON FORMAT SELECTION: a decoder can never be chosen with
 * Consensus::GetB3Era(), because the height of a not-yet-decoded blob is
 * unknown until after decoding. Today the tree sidesteps the problem with a
 * chain-level unified serialization in primitives/ that tolerates the legacy
 * fields on every height of a legacy-B3Coin chain. If the MODERN format ever
 * diverges incompatibly, selection must come from the transport context
 * (which peer/protocol delivered the bytes, or which era of storage they were
 * read from) — never from consensus height. See
 * doc/design/b3-era-architecture.md.
 */
class TransactionDecoder
{
public:
    virtual ~TransactionDecoder() = default;

    /**
     * Decode one historical transaction (nVersion, nTime, vin, vout,
     * nLockTime), preserving txid, outpoints, values and scriptPubKeys
     * exactly. Returns std::nullopt when the bytes are not a well-formed
     * legacy transaction.
     */
    virtual std::optional<CTransactionRef> Decode(std::span<const std::byte> bytes) const = 0;
};

class BlockDecoder
{
public:
    virtual ~BlockDecoder() = default;

    /**
     * Decode one historical block, including the trailing legacy
     * proof-of-stake block signature. Returns nullptr when the bytes are not
     * a well-formed legacy block.
     */
    virtual std::shared_ptr<CBlock> Decode(std::span<const std::byte> bytes) const = 0;
};

} // namespace legacy

#endif // B3COIN_LEGACY_CODEC_H
