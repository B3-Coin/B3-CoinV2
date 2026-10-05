// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3COIN_TEST_UTIL_BRIDGE_OBSERVATION_WORLD_H
#define B3COIN_TEST_UTIL_BRIDGE_OBSERVATION_WORLD_H

#include <bridge/proof.h>
#include <bridge/state_observation.h>
#include <bridge/state_proof.h>
#include <consensus/amount.h>
#include <test/util/bridge_eth_trie.h>
#include <uint256.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <stdexcept>
#include <tuple>
#include <vector>

/**
 * A synthetic Ethereum state holding the three accounts kind 9 observations
 * read (Verifier V2, Vault V2 and the token), with filler accounts so the
 * state trie has branches. Observe() builds a kind 9 record exactly as a
 * relayer would from eth_getProof: the subject's plan (accounts and slots in
 * order, bridge::ObservationRequests), with zero words proven by exclusion.
 *
 * Test-only. The storage layouts are the ones bridge/state_observation.h
 * decodes (contracts spec sections 3.2 and 6.6; Tether slots 0, 2, 6, 10);
 * bridge_state_observation_tests checks them against the shared vectors.
 */
namespace bridge_test {

//! A storage word holding each {offset, bits, value} field, bit 0 the least
//! significant (Solidity's packing order). Values must fit their width.
inline bridge::StorageWord PackWord(
    std::initializer_list<std::tuple<unsigned, unsigned, uint64_t>> fields)
{
    bridge::StorageWord out{};
    for (const auto& [offset, bits, value] : fields) {
        if (bits < 64 && (value >> bits) != 0) throw std::invalid_argument("field overflow");
        for (unsigned i{0}; i < bits && i < 64; ++i) {
            if ((value >> i) & 1) {
                const unsigned bit{offset + i};
                out[31 - bit / 8] |= static_cast<unsigned char>(1U << (bit % 8));
            }
        }
    }
    return out;
}

//! A 32-byte hash stored as a raw word (the byte order observations decode).
inline bridge::StorageWord HashWord(const uint256& hash)
{
    bridge::StorageWord out;
    std::copy(hash.begin(), hash.end(), out.begin());
    return out;
}

class ObservationWorld
{
public:
    bridge::ObservationPins pins;
    std::map<bridge::EthAddress, EthStorageTrie> storage;
    std::map<bridge::EthAddress, uint256> code;

    //! Accounts whose code hashes equal their pins (a stand-in runtime hash
    //! where a pin is unset) and whose observed slots are all zero until the
    //! setters below fill them.
    explicit ObservationWorld(const bridge::ObservationPins& observation_pins)
        : pins{observation_pins}
    {
        code[pins.verifier] = pins.verifier_code_hash.value_or(EthTrie::HashOf(std::vector<unsigned char>{'v'}));
        code[pins.vault] = pins.vault_code_hash.value_or(EthTrie::HashOf(std::vector<unsigned char>{'w'}));
        code[pins.token] = pins.token_code_hash;
        // Storage no observation proves, so the tries are not trivial.
        Set(pins.verifier, 3, bridge::SlotWord(0xa3));
        Set(pins.verifier, 5, bridge::SlotWord(0xa5));
        Set(pins.vault, 2, bridge::SlotWord(0xb2));
        for (unsigned char holder{1}; holder <= 12; ++holder) {
            bridge::EthAddress other;
            other.fill(holder);
            SetWord(pins.token, bridge::MappingSlot(bridge::AddressWord(other), 2),
                    bridge::SlotWord(holder * 1'000ULL));
        }
    }

    void Set(const bridge::EthAddress& account, const uint64_t slot, const bridge::StorageWord& word)
    {
        SetWord(account, bridge::SlotWord(slot), word);
    }
    void SetWord(const bridge::EthAddress& account, const bridge::StorageWord& slot,
                 const bridge::StorageWord& word)
    {
        storage[account].Set(slot, word);
    }

    //! Verifier V2 slots 0, 1, 2, 4 and 6 from a decoded state.
    void SetVerifier(const bridge::VerifierStateV2& v)
    {
        using namespace bridge::observation_layout;
        Set(pins.verifier, VERIFIER_SLOT_LIVENESS,
            PackWord({{LIVENESS_CURRENT_EPOCH.offset, LIVENESS_CURRENT_EPOCH.bits, v.current_epoch},
                      {LIVENESS_LAST_ROTATION_TIME.offset, LIVENESS_LAST_ROTATION_TIME.bits,
                       static_cast<uint64_t>(v.last_rotation_time)},
                      {LIVENESS_LAST_CERTIFICATE_TIME.offset, LIVENESS_LAST_CERTIFICATE_TIME.bits,
                       static_cast<uint64_t>(v.last_certificate_time)},
                      {LIVENESS_LATEST_BRIDGE_HEIGHT.offset, LIVENESS_LATEST_BRIDGE_HEIGHT.bits,
                       v.latest_bridge_height},
                      {LIVENESS_STATUS.offset, LIVENESS_STATUS.bits, v.status},
                      {LIVENESS_DEPOSITS_CLOSED.offset, LIVENESS_DEPOSITS_CLOSED.bits,
                       v.deposits_closed ? 1U : 0U}}));
        Set(pins.verifier, VERIFIER_SLOT_CURRENT_SET_HASH, HashWord(v.current_set_hash));
        Set(pins.verifier, VERIFIER_SLOT_NEXT_SET_HASH, HashWord(v.next_set_hash));
        Set(pins.verifier, VERIFIER_SLOT_LATEST,
            PackWord({{LATEST_HEIGHT.offset, LATEST_HEIGHT.bits, v.latest_height},
                      {LATEST_INDEX.offset, LATEST_INDEX.bits, v.latest_index},
                      {LATEST_INIT_TIME.offset, LATEST_INIT_TIME.bits,
                       static_cast<uint64_t>(v.latest_init_time)}}));
        Set(pins.verifier, VERIFIER_SLOT_LATEST_CERT_ID, HashWord(v.latest_cert_id));
    }

    //! Vault V2 slots 0, 1, 3 and the token's balances[vault].
    void SetReserve(const uint64_t next_deposit_id, const CAmount locked, const CAmount absorbed_total,
                    const CAmount balance)
    {
        using namespace bridge::observation_layout;
        Set(pins.vault, VAULT_SLOT_NEXT_DEPOSIT_ID, bridge::SlotWord(next_deposit_id));
        Set(pins.vault, VAULT_SLOT_LOCKED, bridge::SlotWord(static_cast<uint64_t>(locked)));
        Set(pins.vault, VAULT_SLOT_ABSORBED_TOTAL, bridge::SlotWord(static_cast<uint64_t>(absorbed_total)));
        SetWord(pins.token, bridge::MappingSlot(bridge::AddressWord(pins.vault), TOKEN_SLOT_BALANCES),
                bridge::SlotWord(static_cast<uint64_t>(balance)));
    }

    //! The token's slots 0 (owner 0x66.., paused) and 10 (deprecated) and
    //! isBlackListed[vault].
    void SetTokenFlags(const bool paused, const bool deprecated, const bool vault_blacklisted)
    {
        using namespace bridge::observation_layout;
        bridge::StorageWord owner_paused{
            PackWord({{TOKEN_PAUSED.offset, TOKEN_PAUSED.bits, paused ? 1U : 0U}})};
        std::fill(owner_paused.end() - 20, owner_paused.end(), 0x66);
        Set(pins.token, TOKEN_SLOT_OWNER_PAUSED, owner_paused);
        Set(pins.token, TOKEN_SLOT_UPGRADED_DEPRECATED,
            PackWord({{TOKEN_DEPRECATED.offset, TOKEN_DEPRECATED.bits, deprecated ? 1U : 0U}}));
        SetWord(pins.token, bridge::MappingSlot(bridge::AddressWord(pins.vault), TOKEN_SLOT_IS_BLACK_LISTED),
                bridge::SlotWord(vault_blacklisted ? 1U : 0U));
    }

    EthTrie State() const
    {
        EthTrie state;
        for (const auto& [address, code_hash] : code) {
            const auto it{storage.find(address)};
            const uint256 storage_root{it == storage.end() ? bridge::EMPTY_TRIE_ROOT : it->second.Root()};
            state.InsertSecure(address, EncodeAccount(1, bridge::SlotWord(0), storage_root, code_hash));
        }
        for (unsigned char i{1}; i <= 16; ++i) {
            bridge::EthAddress filler;
            filler.fill(static_cast<unsigned char>(0x80 + i));
            state.InsertSecure(filler, EncodeAccount(i, bridge::SlotWord(i * 1'000ULL), bridge::EMPTY_TRIE_ROOT,
                                                     bridge::EMPTY_CODE_HASH));
        }
        return state;
    }

    uint256 StateRoot() const { return State().Root(); }

    //! The kind 9 record of `subject` against this state, naming `anchor_hash`.
    bridge::BridgeEthStateObservationV2 Observe(const uint8_t subject, const uint256& anchor_hash,
                                                const uint8_t verdict = 0) const
    {
        const EthTrie state{State()};
        bridge::BridgeEthStateObservationV2 record;
        record.subject = subject;
        record.anchor_hash = anchor_hash;
        record.verdict = verdict;
        for (const bridge::ObservationAccountRequest& request : bridge::ObservationRequests(subject, pins)) {
            bridge::BridgeEthAccountProofV2 account;
            account.account_nodes = state.ProofSecure(request.address);
            const auto it{storage.find(request.address)};
            for (const bridge::StorageWord& slot : request.slots) {
                account.storage_nodes.push_back(
                    it == storage.end() ? std::vector<std::vector<unsigned char>>{} : it->second.Proof(slot));
            }
            record.accounts.push_back(std::move(account));
        }
        return record;
    }
};

} // namespace bridge_test

#endif // B3COIN_TEST_UTIL_BRIDGE_OBSERVATION_WORLD_H
