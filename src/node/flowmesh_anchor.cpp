// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/flowmesh_anchor.h>

#include <chain.h>
#include <validation.h>

namespace node {

ChainAnchorPolicy::ChainAnchorPolicy(const ChainstateManager& chainman, const int min_depth)
    : m_chainman{chainman}, m_min_depth{min_depth}
{
}

bool ChainAnchorPolicy::Acceptable(const flowmesh::AnchorRef& anchor) const
{
    if (anchor.height < 0 || anchor.hash.IsNull()) return false;
    LOCK(cs_main);
    const CBlockIndex* index{m_chainman.m_blockman.LookupBlockIndex(anchor.hash)};
    if (index == nullptr || index->nHeight != anchor.height) return false;
    const CChain& active{m_chainman.ActiveChain()};
    if (!active.Contains(index)) return false;
    return active.Height() - index->nHeight >= m_min_depth;
}

flowmesh::AnchorRef ChainAnchorPolicy::Current() const
{
    LOCK(cs_main);
    const CChain& active{m_chainman.ActiveChain()};
    const int height{active.Height() - m_min_depth};
    if (height < 0) return {};
    const CBlockIndex* index{active[height]};
    if (index == nullptr) return {};
    return {index->nHeight, index->GetBlockHash()};
}

} // namespace node
