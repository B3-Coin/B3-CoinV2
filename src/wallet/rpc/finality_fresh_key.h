// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_WALLET_RPC_FINALITY_FRESH_KEY_H
#define BITCOIN_WALLET_RPC_FINALITY_FRESH_KEY_H

#include <crypto/bls.h>
#include <key.h>
#include <modern/finality_fresh_key.h>
#include <node/finality_fresh_key_store.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <random.h>
#include <script/script.h>
#include <tinyformat.h>
#include <uint256.h>
#include <util/fs.h>
#include <util/result.h>
#include <util/strencodings.h>
#include <util/translation.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace wallet {

/**
 * B-37 per-epoch fresh-lineage keys, wallet side (bindfreshfinalitykey;
 * b3-bridge-v2-finality-dependency.md section 6.4, node changes section 7.7).
 *
 * The wallet contributes ONLY the validator identity: its BIP340 signature
 * authorizes a binding (modern::FreshFinalityBindDigest). The BLS key itself
 * is generated from fresh randomness by the node's fresh key store
 * (node::FreshKeyStore, in <datadir>/<net>/finality_signer, the directory the
 * staking loop's signer reads), never derived from the identity secret and
 * never written to the wallet file. A wallet backup therefore holds no fresh
 * key and cannot regenerate one (unlike V1's DeriveFinalityBlsKey); it can
 * still bind a new key for a coming epoch, which is why a double binding
 * voids the seat and the status RPCs alert on bindings the store does not
 * hold.
 */

//! The two halves of one fresh binding plus the data they encode.
struct FreshFinalityKeyBindingParts {
    modern::FreshFinalityKeyParams params;
    modern::FreshFinalityKeyEvidence evidence;
    CScript cell;      //!< FINALITY_KEY policy-version-2 metadata cell (zero value)
    CMpaRecord record; //!< MPA type-12 evidence (BIP340 + identity-bound proof of possession)
};

/**
 * Build (and check, modern::CheckFreshKeyTransition) the cell and the type-12
 * evidence binding `key` for `target_epoch` under the validator identity
 * `identity` on the chain `chain_domain`. Pure apart from the BIP340 aux
 * randomness; no wallet or store access.
 */
inline util::Result<FreshFinalityKeyBindingParts> MakeFreshFinalityKeyBinding(const CKey& identity,
                                                                              const uint256& chain_domain,
                                                                              const bls::SecretKey& key,
                                                                              const uint64_t target_epoch)
{
    if (!identity.IsValid()) return util::Error{Untranslated("the validator identity key is invalid")};
    if (chain_domain.IsNull()) return util::Error{Untranslated("the modern chain domain is not derivable")};
    FreshFinalityKeyBindingParts parts;
    const XOnlyPubKey xonly{identity.GetPubKey()};
    modern::ValidatorKeyBytes validator_key{};
    std::copy(xonly.begin(), xonly.end(), validator_key.begin());
    parts.params.bls_pubkey = key.GetPublicKey().Compressed();
    parts.params.target_epoch = target_epoch;
    parts.evidence.validator_key = validator_key;
    parts.evidence.bls_pubkey = parts.params.bls_pubkey;
    parts.evidence.target_epoch = target_epoch;
    const uint256 digest{
        modern::FreshFinalityBindDigest(chain_domain, validator_key, parts.params.bls_pubkey, target_epoch)};
    const uint256 aux{GetRandHash()};
    if (!identity.SignSchnorr(digest, parts.evidence.bip340_sig, nullptr, aux)) {
        return util::Error{Untranslated("BIP340 identity authorization failed")};
    }
    // The identity-bound proof of possession: the fresh key signs this exact
    // claim (validator, chain, epoch). It never publishes a bare IETF PoP, so
    // no copy of this binding can claim the key elsewhere.
    const uint256 pop_digest{
        modern::FreshFinalityPopDigest(chain_domain, validator_key, parts.params.bls_pubkey, target_epoch)};
    parts.evidence.pop = key.Sign(std::span<const unsigned char>(pop_digest.begin(), 32)).Compressed();
    uint256 commitment;
    std::copy(validator_key.begin(), validator_key.end(), commitment.begin());
    const modern::FreshKeyCheck check{
        modern::CheckFreshKeyTransition(chain_domain, commitment, parts.params, parts.evidence)};
    if (check != modern::FreshKeyCheck::OK) {
        return util::Error{Untranslated(strprintf("fresh binding self-check failed: %s", modern::FreshKeyCheckName(check)))};
    }
    parts.cell = modern::MakeFreshFinalityKeyCell(validator_key, parts.params);
    parts.record = modern::MakeFreshFinalityKeyRecord(parts.evidence);
    return parts;
}

/**
 * Why a HELD key of the target epoch may be replaced rather than block a new
 * one (bindfreshfinalitykey decides): a reason to erase it, or nullopt to
 * keep it and refuse.
 */
using FreshKeyReplacePolicy = std::function<std::optional<std::string>(const node::FreshKeyRecord&)>;

/**
 * Open the fresh key store of (`chain_domain`, `validator_key`) in the
 * signer directory `signer_directory` (an absent file is an empty store) and
 * generate a new random key for `target_epoch` in it: durable before this
 * returns. A HELD key for that epoch refuses (a second binding would void
 * the seat) unless `replace` gives a reason to replace it; such a key is
 * erased first only if the observed store state is still current. A stale
 * store refuses: replacement eligibility must be re-evaluated by a new
 * call, never carried across a reopen. "<pubkey>: <reason>" is appended
 * to `replaced`. The store
 * instance is returned open so the caller can record the binding
 * transaction (SetBindTxid).
 */
inline util::Result<bls::SecretKey> GenerateFreshFinalityKey(node::FreshKeyStore& store,
                                                             const fs::path& signer_directory,
                                                             const uint256& chain_domain,
                                                             const modern::ValidatorKeyBytes& validator_key,
                                                             const uint64_t target_epoch,
                                                             const FreshKeyReplacePolicy& replace = {},
                                                             std::vector<std::string>* replaced = nullptr)
{
    std::string error;
    if (!store.Open(signer_directory, chain_domain, validator_key, error)) {
        return util::Error{Untranslated(strprintf("cannot open the fresh key store: %s", error))};
    }
    for (const node::FreshKeyRecord& record : store.Records()) {
        if (!record.Held() || record.target_epoch != target_epoch) continue;
        const std::optional<std::string> reason{replace ? replace(record) : std::nullopt};
        if (!reason) {
            return util::Error{Untranslated(strprintf(
                "this node's fresh key store already holds key %s for fresh epoch %u%s; a second binding for one "
                "epoch would void the seat, so rebroadcast or wait for that binding instead",
                HexStr(record.bls_pubkey), target_epoch,
                record.bind_txid.IsNull() ? std::string{}
                                          : strprintf(" (binding transaction %s)", record.bind_txid.GetHex())))};
        }
        if (!store.Erase(record.bls_pubkey, error)) {
            return util::Error{Untranslated(strprintf("cannot erase the replaceable fresh key %s (%s): %s",
                                                      HexStr(record.bls_pubkey), *reason, error))};
        }
        if (replaced) replaced->push_back(strprintf("%s: %s", HexStr(record.bls_pubkey), *reason));
    }
    auto key{store.Generate(target_epoch, error)};
    if (!key) return util::Error{Untranslated(strprintf("cannot generate a fresh key: %s", error))};
    return *key;
}

} // namespace wallet

#endif // BITCOIN_WALLET_RPC_FINALITY_FRESH_KEY_H
