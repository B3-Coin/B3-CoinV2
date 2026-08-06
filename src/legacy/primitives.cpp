// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <legacy/primitives.h>

#include <arith_uint256.h>
#include <consensus/merkle.h>
#include <hash.h>
#include <legacy/scrypt.h>
#include <script/script.h>
#include <streams.h>

#include <cassert>
#include <span>
#include <string>
#include <vector>

namespace legacy {

uint256 Transaction::GetHash() const
{
    HashWriter writer;
    writer << *this;
    return writer.GetHash();
}

bool Transaction::IsCoinStake() const
{
    return !vin.empty() && !vin[0].prevout.IsNull() && vout.size() >= 2 &&
        vout[0].nValue == 0 && vout[0].scriptPubKey.empty();
}

uint256 BlockHeader::GetHash() const
{
    if (version > 6) {
        HashWriter writer;
        writer << *this;
        return writer.GetHash();
    }

    DataStream stream;
    stream << *this;
    assert(stream.size() == 80);
    return ScryptHash({reinterpret_cast<const unsigned char*>(stream.data()), stream.size()});
}

uint256 ComputeMerkleRoot(const Block& block)
{
    std::vector<uint256> leaves;
    leaves.reserve(block.transactions.size());
    for (const auto& transaction : block.transactions) leaves.push_back(transaction.GetHash());
    return ::ComputeMerkleRoot(std::move(leaves));
}

bool Block::IsProofOfStake() const
{
    return transactions.size() > 1 && transactions[1].IsCoinStake();
}

Block CreateGenesisBlock()
{
    static const std::string timestamp{
        "China launches Gaofen-3 Staellite to get accurate images of earth on 11-august"};

    Transaction transaction;
    transaction.version = 1;
    transaction.time = 1481667355;
    transaction.vin.resize(1);
    transaction.vin[0].scriptSig = CScript() << 0 << CScriptNum{42}
        << std::vector<unsigned char>{timestamp.begin(), timestamp.end()};
    transaction.vout.emplace_back(0, CScript{});

    Block genesis;
    genesis.version = 1;
    genesis.time = 1481667355;
    arith_uint256 proof_of_work_limit{~arith_uint256{0}};
    proof_of_work_limit >>= 20;
    genesis.bits = proof_of_work_limit.GetCompact();
    genesis.nonce = 499515;
    genesis.transactions.push_back(std::move(transaction));
    genesis.merkle_root = ComputeMerkleRoot(genesis);
    return genesis;
}

CBlock CreateCoreGenesisBlock()
{
    static const std::string timestamp{
        "China launches Gaofen-3 Staellite to get accurate images of earth on 11-august"};

    CMutableTransaction transaction;
    transaction.version = 1;
    transaction.nTime = 1481667355;
    transaction.vin.resize(1);
    transaction.vin[0].scriptSig = CScript() << 0 << CScriptNum{42}
        << std::vector<unsigned char>{timestamp.begin(), timestamp.end()};
    transaction.vout.emplace_back(0, CScript{});

    CBlock genesis;
    genesis.nVersion = 1;
    genesis.nTime = 1481667355;
    arith_uint256 proof_of_work_limit{~arith_uint256{0}};
    proof_of_work_limit >>= 20;
    genesis.nBits = proof_of_work_limit.GetCompact();
    genesis.nNonce = 499515;
    genesis.vtx.push_back(MakeTransactionRef(std::move(transaction)));
    genesis.hashMerkleRoot = BlockMerkleRoot(genesis);
    return genesis;
}

} // namespace legacy
