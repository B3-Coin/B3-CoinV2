// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/validator_set.h>

#include <crypto/bls.h>
#include <node/stake_tracker.h>

#include <algorithm>

namespace node {

std::optional<ValidatorSetSnapshot> ValidatorSetSnapshot::Build(const uint64_t epoch,
                                                                const std::map<ValidatorKey, CAmount>& active_weights,
                                                                const FinalityBindingIndex& bindings)
{
    ValidatorSetSnapshot snap;
    // std::map iteration is ascending by validator_key bytes (std::array operator<),
    // which IS the frozen member order.
    for (const auto& [vk, amount] : active_weights) {
        if (amount <= 0) continue;
        const uint64_t w{static_cast<uint64_t>(amount / modern::FINALITY_WEIGHT_UNIT)};
        if (w == 0) continue;
        const auto binding{bindings.Get(vk)};
        if (!binding || binding->IsRevoked()) continue;
        snap.m_members.push_back(ValidatorSetMember{vk, binding->bls_pubkey, w});
    }
    if (snap.m_members.empty() || snap.m_members.size() > modern::MAX_FINALITY_SET) return std::nullopt;

    uint64_t total{0};
    std::vector<bls::VerifiedPublicKey> keys;
    keys.reserve(snap.m_members.size());
    snap.m_leaves.reserve(snap.m_members.size());
    for (size_t i = 0; i < snap.m_members.size(); ++i) {
        const auto& m{snap.m_members[i]};
        total += m.weight;
        const auto pk{bls::PublicKey::Decode(m.bls_pubkey)};
        if (!pk) return std::nullopt; // cannot happen for a binding that passed consensus; fail closed anyway
        // Provenance: every bls_pubkey in the binding index passed its PoP in
        // consensus when the binding connected (or was re-verified on rebuild);
        // this is the permitted TrustedFromValidatedChain use.
        keys.push_back(bls::VerifiedPublicKey::TrustedFromValidatedChain(*pk));
        snap.m_leaves.push_back(modern::ValidatorSetLeaf(static_cast<uint32_t>(i), m.bls_pubkey, m.weight));
    }
    const auto aggregate{bls::AggregatePublicKeys(keys)};
    if (!aggregate) return std::nullopt;
    const auto root{modern::ValidatorSetMembersRoot(snap.m_leaves)};
    if (!root) return std::nullopt;

    snap.m_header.epoch = epoch;
    snap.m_header.ruleset_version = modern::FINALITY_RULESET_V1;
    snap.m_header.validator_count = static_cast<uint32_t>(snap.m_members.size());
    snap.m_header.total_weight = total;
    snap.m_header.quorum_weight = modern::QuorumWeightV1(total);
    snap.m_header.aggregate_pubkey = aggregate->Compressed();
    snap.m_header.members_root = *root;
    snap.m_set_hash = modern::ValidatorSetHash(snap.m_header);
    return snap;
}

std::optional<ValidatorSetSnapshot> ValidatorSetSnapshot::BuildAt(const uint64_t epoch, const StakeTracker& stakes,
                                                                  const int height, const FinalityBindingIndex& bindings)
{
    CAmount total{0};
    return Build(epoch, stakes.ActiveWeights(height, total), bindings);
}

modern::ValidatorSetView ValidatorSetSnapshot::View() const
{
    modern::ValidatorSetView view;
    view.validator_count = m_header.validator_count;
    view.quorum_weight = m_header.quorum_weight;
    view.keys.reserve(m_members.size());
    view.weights.reserve(m_members.size());
    for (const auto& m : m_members) {
        // Provenance: member keys come from bindings whose PoP passed consensus.
        view.keys.push_back(bls::VerifiedPublicKey::TrustedFromValidatedChain(*bls::PublicKey::Decode(m.bls_pubkey)));
        view.weights.push_back(m.weight);
    }
    return view;
}

std::optional<uint32_t> ValidatorSetSnapshot::IndexOf(const modern::ValidatorKeyBytes& validator_key) const
{
    const auto it{std::lower_bound(m_members.begin(), m_members.end(), validator_key,
                                   [](const ValidatorSetMember& m, const modern::ValidatorKeyBytes& k) { return m.validator_key < k; })};
    if (it == m_members.end() || it->validator_key != validator_key) return std::nullopt;
    return static_cast<uint32_t>(it - m_members.begin());
}

} // namespace node
