// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <primitives/block.h>

#include <consensus/hardfork.h>
#include <consensus/params.h>
#include <hash.h>
#include <legacy/scrypt.h>
#include <streams.h>
#include <tinyformat.h>

#include <cassert>
#include <memory>
#include <span>
#include <sstream>

uint256 CBlockHeader::GetHash() const
{
    return (HashWriter{} << *this).GetHash();
}

uint256 CBlockHeader::GetLegacyB3Hash() const
{
    DataStream stream;
    stream << *this;
    assert(stream.size() == 80);
    return legacy::ScryptHash({reinterpret_cast<const unsigned char*>(stream.data()), stream.size()});
}

uint256 CBlockHeader::GetHash(const Consensus::Params& consensus_params, const int height) const
{
    if (consensus_params.legacy_b3coin &&
        Consensus::GetEra(consensus_params, height) == Consensus::Era::LEGACY) {
        return GetLegacyB3Hash();
    }
    return GetHash();
}

std::string CBlock::ToString() const
{
    std::stringstream s;
    s << strprintf("CBlock(hash=%s, ver=0x%08x, hashPrevBlock=%s, hashMerkleRoot=%s, nTime=%u, nBits=%08x, nNonce=%u, vtx=%u)\n",
        GetHash().ToString(),
        nVersion,
        hashPrevBlock.ToString(),
        hashMerkleRoot.ToString(),
        nTime, nBits, nNonce,
        vtx.size());
    for (const auto& tx : vtx) {
        s << "  " << tx->ToString() << "\n";
    }
    return s.str();
}
