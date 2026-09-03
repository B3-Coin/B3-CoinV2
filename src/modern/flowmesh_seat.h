// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_MODERN_FLOWMESH_SEAT_H
#define B3COIN_MODERN_FLOWMESH_SEAT_H

#include <consensus/era.h>
#include <consensus/params.h>
#include <crypto/bls.h>
#include <crypto/common.h>
#include <modern/asset_output.h>
#include <modern/chain_domain.h>
#include <modern/creation_action.h>
#include <modern/fn.h>
#include <primitives/transaction.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace modern {

/**
 * FlowMesh FN-seat pre-binding (A2, frozen v1). Full FlowMesh service remains
 * separately gated at A3 after the anchor-depth runway.
 *
 * An active seat is an ordinary amount-1 FN owner carrier using policy FN-v2:
 * its policy params are the canonical 48-byte BLS public key and its owner
 * suffix remains the sole spending authorization. The matching MPA type-7
 * record is exactly:
 *
 *     output_index u32 BE || proof_of_possession[96]
 *
 * The public key is not repeated in evidence; it is read from the named
 * output, so a record cannot be transplanted across keys. Spending a seat to
 * FN-v1 simply omits a new FN-v2 output and ends the seat. This layer does not
 * maintain the later global active-seat index and deliberately does not yet
 * reject a BLS key already present in another UTXO.
 */
inline constexpr size_t FLOWMESH_SEAT_BINDING_ACTION_V1_SIZE{
    sizeof(uint32_t) + bls::SIGNATURE_SIZE};
static_assert(FLOWMESH_SEAT_BINDING_ACTION_V1_SIZE == 100);
static_assert(FN_SEAT_POLICY_PARAMS_SIZE == bls::PUBKEY_SIZE);

struct FlowMeshSeatBindingV1 {
    uint32_t output_index{0};
    std::array<unsigned char, bls::SIGNATURE_SIZE> pop{};

    friend bool operator==(const FlowMeshSeatBindingV1& a,
                           const FlowMeshSeatBindingV1& b)
    {
        return a.output_index == b.output_index && a.pop == b.pop;
    }
};

/**
 * One transaction-local FN-v2 output whose type-7 evidence has been
 * structurally matched and whose BLS proof of possession has been verified.
 * The node seat index consumes this view; it never reparses or trusts a key
 * without the matching PoP.
 */
struct VerifiedFlowMeshSeatBinding {
    uint32_t output_index{0};
    std::array<unsigned char, bls::PUBKEY_SIZE> public_key{};
    std::array<unsigned char, bls::SIGNATURE_SIZE> proof_of_possession{};
};

inline std::vector<unsigned char> EncodeFlowMeshSeatBindingPayload(
    const FlowMeshSeatBindingV1& binding)
{
    std::vector<unsigned char> payload(FLOWMESH_SEAT_BINDING_ACTION_V1_SIZE);
    WriteBE32(payload.data(), binding.output_index);
    std::copy(binding.pop.begin(), binding.pop.end(), payload.begin() + sizeof(uint32_t));
    return payload;
}

inline CMpaRecord MakeFlowMeshSeatBindingRecord(
    const uint32_t output_index,
    std::span<const unsigned char, bls::SIGNATURE_SIZE> pop)
{
    FlowMeshSeatBindingV1 binding;
    binding.output_index = output_index;
    std::copy(pop.begin(), pop.end(), binding.pop.begin());
    return CMpaRecord{CREATION_ACTION_FLOWMESH_SEAT_BINDING,
                      FLOWMESH_SEAT_BINDING_ACTION_VERSION_V1,
                      EncodeFlowMeshSeatBindingPayload(binding)};
}

inline bool DecodeFlowMeshSeatBindingRecord(const CMpaRecord& record,
                                            FlowMeshSeatBindingV1& out,
                                            std::string& error)
{
    if (record.payload_type != CREATION_ACTION_FLOWMESH_SEAT_BINDING ||
        record.payload_version != FLOWMESH_SEAT_BINDING_ACTION_VERSION_V1) {
        error = "not a FlowMesh seat-binding record";
        return false;
    }
    if (record.payload.size() != FLOWMESH_SEAT_BINDING_ACTION_V1_SIZE) {
        error = "FlowMesh seat-binding record has the wrong size";
        return false;
    }
    out.output_index = ReadBE32(record.payload.data());
    std::copy(record.payload.begin() + sizeof(uint32_t), record.payload.end(),
              out.pop.begin());
    return true;
}

inline bool IsFlowMeshSeatOutput(const ModernOutput& out)
{
    return out.policy_type == static_cast<uint16_t>(PolicyType::FN) &&
           out.policy_version == FN_SEAT_POLICY_VERSION_V2;
}

inline std::optional<CTxOut> MakeFlowMeshSeatOutput(const AssetId& fn_asset,
                                                   const CScript& owner_script,
                                                   const bls::PublicKey& bls_pubkey)
{
    ModernOutput out;
    out.asset = fn_asset;
    out.amount = 1;
    out.policy_type = static_cast<uint16_t>(PolicyType::FN);
    out.policy_version = FN_SEAT_POLICY_VERSION_V2;
    out.policy_commitment = AssetOwnerCommitment(owner_script);
    out.policy_params.assign(bls_pubkey.Compressed().begin(),
                             bls_pubkey.Compressed().end());
    return MakeAssetOwnerOutput(out, owner_script);
}

/**
 * Exact transaction-local FN-v2 <-> type-7 evidence bijection and PoP check.
 * Structural/activation checks run before any BLS verification. An empty pair
 * of sets is valid; either side appearing alone is invalid.
 */
inline bool ExtractVerifiedFlowMeshSeatBindings(
    const CTransaction& tx, const int height, const Consensus::Params& params,
    std::vector<VerifiedFlowMeshSeatBinding>& out, std::string& error)
{
    out.clear();
    std::map<uint32_t, bls::PublicKey> seats;
    for (size_t i{0}; i < tx.vout.size(); ++i) {
        if (!ClaimsAssetOutput(tx.vout[i])) continue;
        std::string parse_error;
        const auto parsed{ParseAssetOutput(tx.vout[i], parse_error)};
        if (!parsed) {
            error = "FlowMesh seat output parse failed at output " +
                    std::to_string(i) + ": " + parse_error;
            return false;
        }
        if (!IsFlowMeshSeatOutput(*parsed)) continue;
        if (i > UINT32_MAX) {
            error = "FlowMesh seat output index exceeds u32";
            return false;
        }
        if (parsed->amount != 1) {
            error = "FlowMesh seat output amount is not 1";
            return false;
        }
        if (parsed->policy_params.size() != bls::PUBKEY_SIZE) {
            error = "FlowMesh seat output BLS key has the wrong size";
            return false;
        }
        const auto key{bls::PublicKey::Decode(parsed->policy_params)};
        if (!key) {
            error = "FlowMesh seat output BLS key is not canonical";
            return false;
        }
        seats.emplace(static_cast<uint32_t>(i), *key);
    }

    size_t binding_records{0};
    for (const CMpaRecord& record : tx.mpa) {
        if (record.payload_type == CREATION_ACTION_FLOWMESH_SEAT_BINDING) {
            ++binding_records;
        }
    }
    if (seats.empty() && binding_records == 0) return true;
    if (!Consensus::FlowMeshSeatBindingRulesActive(height, params)) {
        error = "FlowMesh seat binding is not active";
        return false;
    }

    const auto domain{params.legacy_final_hash
                          ? ModernChainDomain(params.hashGenesisBlock,
                                              *params.legacy_final_hash)
                          : std::nullopt};
    if (!domain) {
        error = "FlowMesh seat FN asset id is unavailable";
        return false;
    }
    const AssetId expected_fn_asset{FnAssetId(*domain)};
    for (const auto& entry : seats) {
        const uint32_t index{entry.first};
        const auto parsed{ParseAssetOutput(tx.vout[index])};
        if (!parsed || parsed->asset != expected_fn_asset) {
            error = "FlowMesh seat output carries the wrong FN asset id";
            return false;
        }
    }

    std::map<uint32_t, FlowMeshSeatBindingV1> evidence_by_output;
    for (const CMpaRecord& record : tx.mpa) {
        if (record.payload_type != CREATION_ACTION_FLOWMESH_SEAT_BINDING) continue;
        FlowMeshSeatBindingV1 binding;
        if (!DecodeFlowMeshSeatBindingRecord(record, binding, error)) return false;
        const auto seat{seats.find(binding.output_index)};
        if (seat == seats.end()) {
            error = "orphan FlowMesh seat-binding record";
            return false;
        }
        if (!evidence_by_output.emplace(binding.output_index, binding).second) {
            error = "duplicate FlowMesh seat-binding record";
            return false;
        }
    }
    if (evidence_by_output.size() != seats.size()) {
        error = "FlowMesh seat output is missing binding evidence";
        return false;
    }
    // Cryptography runs only after the complete transaction-local bijection
    // has passed its cheap structural checks.
    for (const auto& [output_index, binding] : evidence_by_output) {
        const auto pop{bls::Signature::Decode(binding.pop)};
        if (!pop || !bls::VerifyPoP(seats.at(output_index), *pop)) {
            error = "invalid FlowMesh seat proof of possession";
            return false;
        }
        VerifiedFlowMeshSeatBinding verified;
        verified.output_index = output_index;
        verified.public_key = seats.at(output_index).Compressed();
        verified.proof_of_possession = binding.pop;
        out.push_back(verified);
    }
    return true;
}

inline bool CheckFlowMeshSeatBindings(const CTransaction& tx, const int height,
                                      const Consensus::Params& params,
                                      std::string& error)
{
    std::vector<VerifiedFlowMeshSeatBinding> ignored;
    return ExtractVerifiedFlowMeshSeatBindings(tx, height, params, ignored, error);
}

} // namespace modern

#endif // B3COIN_MODERN_FLOWMESH_SEAT_H
