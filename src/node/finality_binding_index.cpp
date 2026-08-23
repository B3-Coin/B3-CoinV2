// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/finality_binding_index.h>

#include <cassert>

namespace node {

std::optional<modern::BindingRecord> FinalityBindingIndex::Get(const modern::ValidatorKeyBytes& validator_key) const
{
    const auto it{m_by_validator.find(validator_key)};
    if (it == m_by_validator.end()) return std::nullopt;
    return it->second;
}

std::optional<modern::ValidatorKeyBytes> FinalityBindingIndex::OwnerOf(const modern::BlsPubkeyBytes& bls_pubkey) const
{
    if (modern::IsZeroBlsKey(bls_pubkey)) return std::nullopt;
    const auto it{m_owner_of_key.find(bls_pubkey)};
    if (it == m_owner_of_key.end()) return std::nullopt;
    return it->second;
}

modern::BlsKeyOwnerLookup FinalityBindingIndex::OwnerLookup() const
{
    return [this](const modern::BlsPubkeyBytes& pk) { return OwnerOf(pk); };
}

void FinalityBindingIndex::Set(const modern::ValidatorKeyBytes& validator_key, const modern::BindingRecord& record)
{
    // Release the validator's previous nonzero key, then claim the new one.
    if (const auto it{m_by_validator.find(validator_key)}; it != m_by_validator.end() && !it->second.IsRevoked()) {
        const auto owner{m_owner_of_key.find(it->second.bls_pubkey)};
        if (owner != m_owner_of_key.end() && owner->second == validator_key) m_owner_of_key.erase(owner);
    }
    m_by_validator[validator_key] = record;
    if (!record.IsRevoked()) m_owner_of_key[record.bls_pubkey] = validator_key;
}

void FinalityBindingIndex::Restore(const UndoEntry& undo)
{
    // Release whatever the validator holds now.
    if (const auto it{m_by_validator.find(undo.validator_key)}; it != m_by_validator.end()) {
        if (!it->second.IsRevoked()) {
            const auto owner{m_owner_of_key.find(it->second.bls_pubkey)};
            if (owner != m_owner_of_key.end() && owner->second == undo.validator_key) m_owner_of_key.erase(owner);
        }
        m_by_validator.erase(it);
    }
    // Reinstate the previous record, if there was one.
    if (undo.previous) {
        m_by_validator[undo.validator_key] = *undo.previous;
        if (!undo.previous->IsRevoked()) m_owner_of_key[undo.previous->bls_pubkey] = undo.validator_key;
    }
}

void FinalityBindingIndex::ConnectBlock(const int height, const std::vector<Transition>& transitions)
{
    assert(m_heights.empty() || height > m_heights.back());
    std::vector<UndoEntry> undo;
    undo.reserve(transitions.size());
    for (const Transition& t : transitions) {
        undo.push_back(UndoEntry{t.validator_key, Get(t.validator_key)});
        Set(t.validator_key, t.record);
    }
    m_heights.push_back(height);
    m_undo.push_back(std::move(undo));
}

void FinalityBindingIndex::DisconnectBlock(const int height)
{
    assert(!m_heights.empty() && m_heights.back() == height);
    const std::vector<UndoEntry>& undo{m_undo.back()};
    for (auto it = undo.rbegin(); it != undo.rend(); ++it) Restore(*it);
    m_undo.pop_back();
    m_heights.pop_back();
}

void FinalityBindingIndex::Clear()
{
    m_by_validator.clear();
    m_owner_of_key.clear();
    m_heights.clear();
    m_undo.clear();
}

std::map<modern::ValidatorKeyBytes, modern::BindingRecord> FinalityBindingIndex::SnapshotActive() const
{
    std::map<modern::ValidatorKeyBytes, modern::BindingRecord> out;
    for (const auto& [vk, rec] : m_by_validator) {
        if (!rec.IsRevoked()) out.emplace(vk, rec);
    }
    return out;
}

} // namespace node
