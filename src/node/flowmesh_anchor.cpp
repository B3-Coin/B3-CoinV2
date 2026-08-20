// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/flowmesh_anchor.h>

#include <chain.h>
#include <validation.h>

#include <stdexcept>

namespace node {

ChainAnchorPolicy::ChainAnchorPolicy(const ChainstateManager& chainman, const int min_depth)
    : m_chainman{chainman}, m_min_depth{min_depth}
{
    // The finality depth is a safety-critical owner input: a negative
    // value would silently accept unburied tips while Current() walks
    // past the tip. Fail loudly instead of warping semantics.
    if (min_depth < 0) {
        throw std::invalid_argument("flowmesh anchor depth must be non-negative");
    }
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

bool ChainAnchorPolicy::StillCanonical(const flowmesh::AnchorRef& anchor) const
{
    if (anchor.IsNull()) return true; // references no B3 state
    if (anchor.height < 0 || anchor.hash.IsNull()) return false;
    LOCK(cs_main);
    const CBlockIndex* index{m_chainman.m_blockman.LookupBlockIndex(anchor.hash)};
    if (index == nullptr || index->nHeight != anchor.height) return false;
    return m_chainman.ActiveChain().Contains(index); // depth-free: canonicality only
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
