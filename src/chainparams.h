// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CHAINPARAMS_H
#define BITCOIN_CHAINPARAMS_H

#include <kernel/chainparams.h> // IWYU pragma: export

#include <cstddef>
#include <memory>
#include <string_view>

class ArgsManager;

/** Largest -b3bridgepins file the node reads (regtest-only). */
inline constexpr size_t MAX_B3_BRIDGE_PINS_FILE_SIZE{64 * 1024};

/**
 * Parse and validate the contents of a -b3bridgepins file (REGTEST ONLY):
 * strict JSON {"format": "b3-regtest-bridge-pins/1", "v1_decentralized":
 * {...}, "v2": {...}} holding at least one of the two sections, with
 * 0x-prefixed hex in raw Ethereum byte order. The result's file_sha256 is
 * the SHA256 of exactly these bytes, and has_v1 records whether the V1
 * section was present.
 *
 * @throws std::runtime_error("Invalid -b3bridgepins file: ...") on any
 *         schema or validation failure.
 */
CChainParams::B3RegTestBridgePins ParseB3RegTestBridgePins(std::string_view json);

/**
 * Creates and returns a std::unique_ptr<CChainParams> of the chosen chain.
 */
std::unique_ptr<const CChainParams> CreateChainParams(const ArgsManager& args, ChainType chain);

/**
 * Return the currently selected parameters. This won't change after app
 * startup, except for unit tests.
 */
const CChainParams &Params();

/**
 * Sets the params returned by Params() to those for the given chain type.
 */
void SelectParams(ChainType chain);

#endif // BITCOIN_CHAINPARAMS_H
