// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_AUTH_H
#define B3COIN_FLOWMESH_AUTH_H

#include <flowmesh/batch.h>
#include <hash.h>
#include <key.h>
#include <pubkey.h>
#include <uint256.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace flowmesh {

/**
 * Production action authorization over the repository's existing BIP340
 * Schnorr primitives (no new cryptography, no BLS).
 *
 * Account binding: an account id IS the tagged hash of its x-only public
 * key, so a credential cannot vouch for someone else's account.
 * Credential layout (exactly 96 bytes): 32-byte x-only pubkey || 64-byte
 * BIP340 signature over the domain-bound action digest. The digest binds
 * the FlowMesh domain, so an action signed for one domain can never be
 * replayed into another; the action id itself binds the signer's
 * sequence, so it cannot be replayed within the domain either.
 */

inline constexpr size_t SCHNORR_CREDENTIAL_SIZE{96};

inline AccountId AccountForKey(const XOnlyPubKey& key)
{
    HashWriter h;
    h << std::string{"b3/flowmesh/account/v1"} << key;
    return h.GetHash();
}

//! The signature digest binds the FlowMesh DOMAIN and the immutable
//! MARKET/EXECUTION CONFIGURATION (vault, base, quote, curve bound —
//! state.h ComputeExecutionConfigId) around the semantic action id
//! (signer, sequence, type and all execution-relevant fields). An
//! authorization therefore cannot be replayed or substituted into a
//! different domain, market pair, vault or execution configuration.
//! (v2: the execution-config id joined the preimage.)
inline uint256 ActionSignatureDigest(const uint256& domain, const uint256& execution_config_id,
                                     const Action& action)
{
    HashWriter h;
    h << std::string{"b3/flowmesh/action-sig/v2"} << domain << execution_config_id
      << action.Id();
    return h.GetHash();
}

class SchnorrActionAuthenticator final : public ActionAuthenticator
{
public:
    SchnorrActionAuthenticator(const uint256& domain, const uint256& execution_config_id)
        : m_domain{domain}, m_config{execution_config_id}
    {
    }

    bool Authenticate(const Action& action) const override
    {
        if (action.credential.size() != SCHNORR_CREDENTIAL_SIZE) return false;
        const XOnlyPubKey key{std::span<const unsigned char>{action.credential.data(), 32}};
        if (!key.IsFullyValid()) return false;
        if (AccountForKey(key) != action.signer) return false;
        return key.VerifySchnorr(ActionSignatureDigest(m_domain, m_config, action),
                                 std::span<const unsigned char>{action.credential.data() + 32, 64});
    }

private:
    const uint256 m_domain;
    const uint256 m_config;
};

//! Attach a valid credential to an action (wallet/tooling/tests).
inline bool SignAction(const CKey& key, const uint256& domain,
                       const uint256& execution_config_id, Action& action)
{
    const XOnlyPubKey xonly{key.GetPubKey()};
    if (AccountForKey(xonly) != action.signer) return false;
    std::array<unsigned char, 64> sig;
    if (!key.SignSchnorr(ActionSignatureDigest(domain, execution_config_id, action), sig,
                         nullptr, uint256::ZERO)) {
        return false;
    }
    action.credential.assign(xonly.data(), xonly.data() + 32);
    action.credential.insert(action.credential.end(), sig.begin(), sig.end());
    return true;
}

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_AUTH_H
