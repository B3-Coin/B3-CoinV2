// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CONSENSUS_CONSENSUS_H
#define BITCOIN_CONSENSUS_CONSENSUS_H

#include <cstdint>
#include <cstdlib>

/** The maximum allowed size for a serialized block, in bytes (only for buffer size limits) */
static const unsigned int MAX_BLOCK_SERIALIZED_SIZE = 5000000;
/** The maximum allowed weight for a block, see BIP 141 (network rule) */
static const unsigned int MAX_BLOCK_WEIGHT = 4000000;
/** The maximum allowed number of signature check operations in a block (network rule) */
static const int64_t MAX_BLOCK_SIGOPS_COST = 80000;
/** Coinbase transaction outputs can only be spent after this number of new blocks (network rule) */
static const int COINBASE_MATURITY = 30;

static const int WITNESS_SCALE_FACTOR = 4;

/** B3 Modern Payload Area (MPA) and payload verification-cost budget —
 *  owner-frozen constants (2026-08-23). Declared ahead of the MPA codec and
 *  the cost-accounting rules (implementation plan, Commits 5/8); nothing
 *  consults them yet. The MPA carries large evidence bytes OUTSIDE policy
 *  state (policy_params stays <= 80 bytes, permanently). */
/** Hard ceiling for one MPA record payload, bytes (per-type maxima are <= this). */
static const size_t MAX_PAYLOAD_RECORD_SIZE = 32768;
/** Maximum serialized MPA section per transaction, bytes. */
static const size_t MAX_PAYLOAD_SECTION_SIZE = 65536;
/** Maximum number of MPA records per transaction (ratified 64). */
static const size_t MAX_PAYLOAD_RECORDS_PER_TX = 64;
/** MPA bytes count at the full scale factor (x4): historical chain data, no witness discount. */
static const int MPA_WEIGHT_FACTOR = WITNESS_SCALE_FACTOR;
/** Sum of declared record verification costs allowed per block / per transaction (1 unit ~ 1 us reference). */
static const int64_t MAX_BLOCK_PAYLOAD_COST = 120000;
static const int64_t MAX_TX_PAYLOAD_COST = 12000;
/** Relay/fee accounting: virtual bytes charged per cost unit (vsize = max(weight/4, cost * this)). */
static const int64_t PAYLOAD_COST_TO_VBYTES = 1;

static const size_t MIN_TRANSACTION_WEIGHT = WITNESS_SCALE_FACTOR * 60; // 60 is the lower bound for the size of a valid serialized CTransaction
static const size_t MIN_SERIALIZABLE_TRANSACTION_WEIGHT = WITNESS_SCALE_FACTOR * 10; // 10 is the lower bound for the size of a serialized CTransaction

/** Flags for nSequence and nLockTime locks */
/** Interpret sequence numbers as relative lock-time constraints. */
static constexpr unsigned int LOCKTIME_VERIFY_SEQUENCE = (1 << 0);

/**
 * Maximum number of seconds that the timestamp of the first
 * block of a difficulty adjustment period is allowed to
 * be earlier than the last block of the previous period (BIP94).
 */
static constexpr int64_t MAX_TIMEWARP = 600;

#endif // BITCOIN_CONSENSUS_CONSENSUS_H
