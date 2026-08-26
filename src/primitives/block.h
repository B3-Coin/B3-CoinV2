// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PRIMITIVES_BLOCK_H
#define BITCOIN_PRIMITIVES_BLOCK_H

#include <consensus/block_codec.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <uint256.h>
#include <util/time.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Consensus {
struct Params;
}

/** Nodes collect new transactions into a block, hash them into a hash tree,
 * and scan through nonce values to make the block's hash satisfy proof-of-work
 * requirements.  When they solve the proof-of-work, they broadcast the block
 * to everyone and the block is added to the block chain.  The first transaction
 * in the block is a special one that creates a new coin owned by the creator
 * of the block.
 */
class CBlockHeader
{
public:
    // header
    int32_t nVersion;
    uint256 hashPrevBlock;
    uint256 hashMerkleRoot;
    uint32_t nTime;
    uint32_t nBits;
    uint32_t nNonce;

    CBlockHeader()
    {
        SetNull();
    }

    SERIALIZE_METHODS(CBlockHeader, obj) { READWRITE(obj.nVersion, obj.hashPrevBlock, obj.hashMerkleRoot, obj.nTime, obj.nBits, obj.nNonce); }

    void SetNull()
    {
        nVersion = 0;
        hashPrevBlock.SetNull();
        hashMerkleRoot.SetNull();
        nTime = 0;
        nBits = 0;
        nNonce = 0;
    }

    bool IsNull() const
    {
        return (nBits == 0);
    }

    /** Bitcoin Core's normal SHA256d block-header hash. */
    uint256 GetHash() const;

    /**
     * Consensus-selected block-header hash at a known chain height.
     * Historical B3Coin heights use scrypt while post-fork and non-B3Coin
     * networks retain SHA256d.
     */
    uint256 GetHash(const Consensus::Params& consensus_params, int height) const;

    /** Historical B3Coin scrypt header hash (always scrypt, no version test). */
    uint256 GetLegacyB3Hash() const;

    /**
     * Marker-aware header hash for contexts with no authoritative parent
     * height (an unknown-parent block from the wire, a disk read without a
     * recorded height). The permanent B3 codec marker selects the hash
     * domain: modern SHA256d with it, legacy scrypt without it on a legacy
     * chain. Never assume height zero for an unknown-parent block; once the
     * parent is known the height-aware overload is authoritative.
     */
    uint256 GetMarkerHash(const Consensus::Params& consensus_params) const;

    NodeSeconds Time() const
    {
        return NodeSeconds{std::chrono::seconds{nTime}};
    }

    int64_t GetBlockTime() const
    {
        return (int64_t)nTime;
    }
};


class CBlock : public CBlockHeader
{
public:
    // network and disk
    std::vector<CTransactionRef> vtx;
    /** Legacy proof-of-stake block signature, serialized after transactions. */
    std::vector<unsigned char> vchBlockSig;

    // Memory-only flags for caching expensive checks
    mutable bool fChecked;                            // CheckBlock()
    mutable bool m_checked_witness_commitment{false}; // CheckWitnessCommitment()
    mutable bool m_checked_merkle_root{false};        // CheckMerkleRoot()

    CBlock()
    {
        SetNull();
    }

    CBlock(const CBlockHeader &header)
    {
        SetNull();
        *(static_cast<CBlockHeader*>(this)) = header;
    }

    SERIALIZE_METHODS(CBlock, obj)
    {
        READWRITE(AsBase<CBlockHeader>(obj));
        // The legacy-chain codec (legacy::TX_LEGACY) is marker-aware: the
        // fixed-size header is (de)serialized first, so the permanent B3
        // codec marker (consensus/block_codec.h) can select the body format
        // before any transaction is parsed. A marker-modern block keeps the
        // unmodified Core body; a legacy body carries nTime transactions
        // plus the historical trailing proof-of-stake signature. Whether the
        // marker may appear at a given height is a separate consensus rule
        // (ContextualCheckBlockHeader); this only selects the raw codec.
        if (s.template GetParams<TransactionSerParams>().legacy_time) {
            if (Consensus::HasB3BlockCodecV2(obj.nVersion)) {
                // Marker-modern blocks carry a trailing signature vector,
                // exactly the legacy codec's own trailing-signature pattern:
                // outside the header and merkle commitment, so identity is
                // untouched. Consensus pins its content by phase — empty in
                // the temporary-PoW corridor, a 64-byte BIP340 validator
                // signature in the modern-PoS phase (frozen V1 spec §5).
                // Marker-modern transactions use the B3 Modern full form:
                // witness plus the Modern Payload Area (flag 0x02).
                READWRITE(TX_MODERN(obj.vtx), obj.vchBlockSig);
            } else {
                READWRITE(obj.vtx, obj.vchBlockSig);
            }
        } else {
            SER_READ(obj, obj.vchBlockSig.clear());
            READWRITE(obj.vtx);
        }
    }

    void SetNull()
    {
        CBlockHeader::SetNull();
        vtx.clear();
        vchBlockSig.clear();
        fChecked = false;
        m_checked_witness_commitment = false;
        m_checked_merkle_root = false;
    }

    std::string ToString() const;

    bool IsProofOfStake() const
    {
        return vtx.size() > 1 && vtx[1]->IsCoinStake();
    }

    bool IsProofOfWork() const { return !IsProofOfStake(); }
};

/** Describes a place in the block chain to another node such that if the
 * other node doesn't have the same branch, it can find a recent common trunk.
 * The further back it is, the further before the fork it may be.
 */
struct CBlockLocator
{
    /** Historically CBlockLocator's version field has been written to network
     * streams as the negotiated protocol version and to disk streams as the
     * client version, but the value has never been used.
     *
     * Hard-code to the highest protocol version ever written to a network stream.
     * SerParams can be used if the field requires any meaning in the future,
     **/
    static constexpr int DUMMY_VERSION = 70016;

    std::vector<uint256> vHave;

    CBlockLocator() = default;

    explicit CBlockLocator(std::vector<uint256>&& have) : vHave(std::move(have)) {}

    SERIALIZE_METHODS(CBlockLocator, obj)
    {
        int nVersion = DUMMY_VERSION;
        READWRITE(nVersion);
        READWRITE(obj.vHave);
    }

    void SetNull()
    {
        vHave.clear();
    }

    bool IsNull() const
    {
        return vHave.empty();
    }
};

#endif // BITCOIN_PRIMITIVES_BLOCK_H
