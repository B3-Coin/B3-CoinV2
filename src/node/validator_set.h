// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
#ifndef B3COIN_NODE_VALIDATOR_SET_H
#define B3COIN_NODE_VALIDATOR_SET_H

#include <consensus/amount.h>
#include <modern/finality_key.h>
#include <modern/finality_types.h>
#include <node/finality_binding_index.h>
#include <node/stake_registry.h>
#include <uint256.h>

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace node {

class StakeTracker;

/**
 * Deterministic validator-set snapshot (plan Commit 9; normative spec
 * b3-cross-chain-finality-v1.md section 2, section 4 "Snapshot").
 *
 * ONE universe: members are enumerated from the SAME stake registry block
 * eligibility uses (StakeTracker::ActiveWeights: ACTIVE, mature stake per
 * validator_key) joined with the derived FINALITY_KEY binding index
 * (non-revoked binding). A validator without a binding, with a revoked
 * binding, or with weight < 1 whole modern B3 is not a member.
 *
 *   member      = (validator_key, bls_pubkey, w)  with  w = floor(active_base_units / FINALITY_WEIGHT_UNIT), w > 0
 *   order       = ascending validator_key bytes (index i = position)
 *   leaf_i      = keccak(u32 i || bls_pubkey_i || u64 w_i)        (modern::ValidatorSetLeaf)
 *   members_root= depth-13 zero-padded keccak tree                 (modern::ValidatorSetMembersRoot)
 *   header      = {epoch, ruleset 1, n, W = sum w, quorum = floor(2W/3)+1, aggregate_pubkey = sum pk, members_root}
 *   set hash    = keccak(header bytes)                              (modern::ValidatorSetHash)
 *
 * A snapshot is an immutable value (const members): once built it is never
 * mutated; later bindings or stake changes produce later snapshots. Building
 * is a pure function of (epoch, active weights, bindings), so it is
 * reproducible after restart/reindex from the rebuilt trackers.
 *
 * Fail-closed: no eligible member (n = 0) or n > MAX_FINALITY_SET yields no
 * snapshot (the frozen header bounds 1 <= n <= 8,192 admit no other set; a
 * truncation rule is deliberately NOT invented here).
 */
struct ValidatorSetMember {
    modern::ValidatorKeyBytes validator_key{};
    modern::BlsPubkeyBytes bls_pubkey{};
    uint64_t weight{0};
    friend bool operator==(const ValidatorSetMember& a, const ValidatorSetMember& b)
    {
        return a.validator_key == b.validator_key && a.bls_pubkey == b.bls_pubkey && a.weight == b.weight;
    }
};

class ValidatorSetSnapshot
{
public:
    //! Pure construction from the two derived sources.
    static std::optional<ValidatorSetSnapshot> Build(uint64_t epoch, const std::map<ValidatorKey, CAmount>& active_weights,
                                                     const FinalityBindingIndex& bindings);
    //! Convenience: enumerate from a synced StakeTracker at `height` and a synced binding index.
    static std::optional<ValidatorSetSnapshot> BuildAt(uint64_t epoch, const StakeTracker& stakes, int height,
                                                       const FinalityBindingIndex& bindings);

    const modern::ValidatorSetHeader& Header() const { return m_header; }
    const std::vector<ValidatorSetMember>& Members() const { return m_members; }
    const std::vector<uint256>& Leaves() const { return m_leaves; }
    const uint256& SetHash() const { return m_set_hash; }
    uint64_t Epoch() const { return m_header.epoch; }
    size_t Size() const { return m_members.size(); }
    uint64_t TotalWeight() const { return m_header.total_weight; }
    uint64_t QuorumWeight() const { return m_header.quorum_weight; }
    //! Index of a validator in this set, if a member.
    std::optional<uint32_t> IndexOf(const modern::ValidatorKeyBytes& validator_key) const;

    friend bool operator==(const ValidatorSetSnapshot& a, const ValidatorSetSnapshot& b)
    {
        return a.m_header == b.m_header && a.m_members == b.m_members;
    }

private:
    ValidatorSetSnapshot() = default;
    modern::ValidatorSetHeader m_header{};
    std::vector<ValidatorSetMember> m_members;
    std::vector<uint256> m_leaves;
    uint256 m_set_hash{};
};

} // namespace node

#endif // B3COIN_NODE_VALIDATOR_SET_H
