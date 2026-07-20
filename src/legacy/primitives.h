// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_LEGACY_PRIMITIVES_H
#define B3COIN_LEGACY_PRIMITIVES_H

#include <primitives/transaction.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <vector>

namespace legacy {

/** Transaction wire format used by the existing B3Coin chain. */
struct Transaction {
    int32_t version{1};
    uint32_t time{0};
    std::vector<CTxIn> vin;
    std::vector<CTxOut> vout;
    uint32_t lock_time{0};

    SERIALIZE_METHODS(Transaction, obj)
    {
        READWRITE(obj.version, obj.time, obj.vin, obj.vout, obj.lock_time);
    }

    uint256 GetHash() const;
    bool IsCoinStake() const;
};

/** Header hash rules used before the B3Coin hard-fork height. */
struct BlockHeader {
    int32_t version{0};
    uint256 previous_block;
    uint256 merkle_root;
    uint32_t time{0};
    uint32_t bits{0};
    uint32_t nonce{0};

    SERIALIZE_METHODS(BlockHeader, obj)
    {
        READWRITE(obj.version, obj.previous_block, obj.merkle_root,
                  obj.time, obj.bits, obj.nonce);
    }

    uint256 GetHash() const;
};

/** Complete legacy wire/disk block, including its post-transaction signature. */
struct Block : BlockHeader {
    std::vector<Transaction> transactions;
    std::vector<unsigned char> signature;

    SERIALIZE_METHODS(Block, obj)
    {
        READWRITE(AsBase<BlockHeader>(obj), obj.transactions, obj.signature);
    }

    bool IsProofOfStake() const;
};

uint256 ComputeMerkleRoot(const Block& block);
Block CreateGenesisBlock();

} // namespace legacy

#endif // B3COIN_LEGACY_PRIMITIVES_H
