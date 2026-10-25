// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/core/params.h>

#include <flowmesh2/core/hashes.h>
#include <serialize.h>
#include <streams.h>

#include <limits>
#include <algorithm>
#include <span>
#include <utility>

namespace flowmesh2 {
namespace {

Error Invalid(const char* detail) { return Error{ErrorCode::INVALID_INPUT, detail}; }

bool IsKnownMatching(MatchingProfile matching) { return matching == MatchingProfile::A2_PRORATA; }

bool IsKnownIncidence(FeeIncidence incidence)
{
    return incidence == FeeIncidence::SELLER || incidence == FeeIncidence::PER_SIDE;
}

bool IsKnownSignerMode(SignerMode mode)
{
    return mode == SignerMode::STRICT_SIGNING || mode == SignerMode::FOLDED_SIGNING;
}

} // namespace

Result<ProfileParams> DevRegtestProfile(ChainType chain)
{
    if (chain != ChainType::REGTEST) {
        return Error{ErrorCode::UNSUPPORTED_NETWORK, "fm2-dex1-regtest-provisional/1 runs on regtest only"};
    }
    // PROVISIONAL REGTEST values (plan section 12.1 recommended defaults).
    ProfileParams params;
    params.profile_id = "fm2-dex1-regtest-provisional/1";
    params.fm_version = FM_VERSION;
    params.chain = ChainType::REGTEST;
    params.matching = MatchingProfile::A2_PRORATA;          // D4
    params.fee_ppm = 100;                                   // D5
    params.fee_incidence = FeeIncidence::SELLER;            // D5
    params.seat_share_percent = 80;                         // D5
    params.max_actions_per_slot = 1'024;                    // D6
    params.max_action_payload_bytes = 4'096;                // D6
    params.max_fills_per_order = 1'024;                     // D6
    params.max_fills_per_slot = 16'384;                     // D6
    params.max_changed_leaves_per_slot = 8'192;             // D6
    params.max_open_orders_per_account = 1'000;             // D6
    params.price_band_enabled = false;                      // D6 (band is M3-ENG-03)
    params.price_band_percent = 50;                         // D6
    params.nonce_window = 100;                              // D7
    params.max_delay_slots = 1'024;                         // D7
    params.futures_enabled = false;                         // M9-FUT-01
    params.signer_mode = SignerMode::STRICT_SIGNING;        // DUR-1
    params.pool_capacity = 4'096;
    params.outcome_index_slots = 1'024;
    return Ok(std::move(params));
}

AssetId RegtestQuoteAsset()
{
    constexpr std::string_view label{"rUSD"};
    return AssetId{Tagged(HashTag::TEST_ASSET,
                          std::span{reinterpret_cast<const unsigned char*>(label.data()), label.size()})};
}

// TODO(rebirth:M6-PRF-01) regtest stable-quote profiles assume D3 (fees in Q);
// they are replaced by the ratified profile and never selectable off REGTEST.
Result<ProfileParams> StableQRegtestProfile(ChainType chain)
{
    Result<ProfileParams> legacy{DevRegtestProfile(chain)};
    if (!legacy.has_value()) return legacy.error();
    ProfileParams params{std::move(legacy).value()};
    params.profile_id = STABLE_Q_PROFILE_ID;
    params.stable_fee_asset = RegtestQuoteAsset();
    params.withdrawals_enabled = true;
    params.withdrawal_fee_atoms = 1'000; // explicit REGTEST-only fixture, not production economics
    return Ok(std::move(params));
}

Result<ProfileParams> StableQViewRegtestProfile(ChainType chain)
{
    auto legacy{StableQRegtestProfile(chain)};
    if (!legacy.has_value()) return legacy.error();
    ProfileParams params{std::move(*legacy)};
    params.profile_id = STABLE_Q_VIEW_PROFILE_ID;
    params.view_bound_commits = true;
    return Ok(std::move(params));
}

Result<ProfileParams> StableQNetworkRegtestProfile(ChainType chain)
{
    auto legacy{StableQViewRegtestProfile(chain)};
    if (!legacy.has_value()) return legacy.error();
    ProfileParams params{std::move(*legacy)};
    params.profile_id = STABLE_Q_NETWORK_PROFILE_ID;
    params.source_range_commitment = true;
    params.source_chain_id = 31337;
    params.source_confirmations = 2;
    return Ok(std::move(params));
}

AssetId NitroLabQuoteAsset(const NitroLabBinding& binding)
{
    std::vector<unsigned char> bytes;
    VectorWriter{bytes, 0, std::string{"fm2/owned-nitro-lab/USDT0/1"}, NITRO_LAB_CHAIN_ID,
                 binding.source_genesis_hash, binding.quote_token, binding.quote_token_code_hash,
                 binding.quote_implementation, binding.quote_implementation_code_hash};
    return AssetId{Tagged(HashTag::TEST_ASSET, bytes)};
}

Result<ProfileParams> NitroLabRegtestProfile(ChainType chain, const NitroLabBinding& binding)
{
    auto base{StableQRegtestProfile(chain)};
    if (!base.has_value()) return base.error();
    ProfileParams params{std::move(*base)};
    params.profile_id = NITRO_LAB_PROFILE_ID;
    params.nitro_lab = binding;
    params.stable_fee_asset = NitroLabQuoteAsset(binding);
    params.source_range_commitment = true;
    params.source_chain_id = NITRO_LAB_CHAIN_ID;
    params.source_confirmations = 1; // owned local successor only; no settlement/finality claim
    if (auto valid{Validate(params)}; !valid.has_value()) return valid.error();
    return Ok(std::move(params));
}

Result<void> Validate(const ProfileParams& params)
{
    if (params.chain != ChainType::REGTEST) {
        return Error{ErrorCode::UNSUPPORTED_NETWORK, "the outline profile runs on regtest only"};
    }

    if (params.profile_id.empty() || params.profile_id.size() > MAX_PROFILE_ID_BYTES) {
        return Invalid("profile_id empty or longer than 64 bytes");
    }
    if (params.fm_version != FM_VERSION) return Invalid("unknown fm_version");
    const bool nitro_lab{params.profile_id == NITRO_LAB_PROFILE_ID};
    if (nitro_lab) {
        if (!params.nitro_lab || !params.source_range_commitment ||
            params.source_chain_id != NITRO_LAB_CHAIN_ID || params.source_confirmations != 1 ||
            params.view_bound_commits || !params.withdrawals_enabled) {
            return Invalid("owned Nitro lab requires its exact direct-commit source412346 policy");
        }
        const auto& binding{*params.nitro_lab};
        const auto nonzero{[](const auto& address) { return std::any_of(address.begin(), address.end(), [](auto b) { return b != 0; }); }};
        if (binding.source_genesis_hash.IsNull() || !nonzero(binding.quote_token) ||
            binding.quote_token_code_hash.IsNull() || !nonzero(binding.quote_implementation) ||
            binding.quote_implementation_code_hash.IsNull() || binding.quote_token == binding.quote_implementation ||
            !params.stable_fee_asset || *params.stable_fee_asset != NitroLabQuoteAsset(binding) ||
            !InAtomRange(params.withdrawal_fee_atoms)) {
            return Invalid("owned Nitro lab quote must bind exact nonzero distinct proxy/implementation/genesis pins");
        }
    } else if (params.nitro_lab) {
        return Invalid("Nitro lab pins require the separately versioned owned-lab profile");
    }
    const bool source_profile{params.profile_id == STABLE_Q_NETWORK_PROFILE_ID};
    if (source_profile) {
        if (!params.source_range_commitment || params.source_chain_id != 31337 || params.source_confirmations != 2) {
            return Invalid("network source-range profile must pin chain31337 and two isolated confirmations");
        }
    } else if (!nitro_lab && (params.source_range_commitment || params.source_chain_id != 0 || params.source_confirmations != 0)) {
        return Invalid("source-range commitment policy requires the versioned network profile");
    }
    if (params.profile_id == STABLE_Q_PROFILE_ID || params.profile_id == STABLE_Q_VIEW_PROFILE_ID || source_profile) {
        if (params.view_bound_commits != (params.profile_id != STABLE_Q_PROFILE_ID)) {
            return Invalid("view-bound commit policy differs from its versioned stable-Q profile");
        }
        if (!params.stable_fee_asset || *params.stable_fee_asset != RegtestQuoteAsset()) {
            return Invalid("stable-Q profile must pin the exact synthetic rUSD AssetId");
        }
        if (!InAtomRange(params.withdrawal_fee_atoms) || (!params.withdrawals_enabled && params.withdrawal_fee_atoms != 0)) {
            return Invalid("withdrawal fee outside atom range or nonzero while disabled");
        }
    } else if (!nitro_lab && (params.stable_fee_asset.has_value() || params.withdrawals_enabled || params.withdrawal_fee_atoms != 0 || params.view_bound_commits)) {
        return Invalid("stable fee and withdrawal policy require the versioned stable-Q profile");
    }
    if (!IsKnownMatching(params.matching)) return Invalid("unknown matching profile");
    if (!IsKnownIncidence(params.fee_incidence)) return Invalid("unknown fee incidence");
    if (!IsKnownSignerMode(params.signer_mode)) return Invalid("unknown signer mode");
    if (params.fee_ppm > 1'000'000) return Invalid("fee_ppm above one million");
    if (params.seat_share_percent > 100) return Invalid("seat_share_percent above 100");
    if (params.max_actions_per_slot == 0 || params.max_action_payload_bytes == 0 ||
        params.max_fills_per_order == 0 || params.max_fills_per_slot == 0 ||
        params.max_changed_leaves_per_slot == 0 || params.max_open_orders_per_account == 0 ||
        params.nonce_window == 0 || params.max_delay_slots == 0 || params.pool_capacity == 0 ||
        params.outcome_index_slots == 0) {
        return Invalid("a cap is zero");
    }
    if (params.price_band_percent == 0 || params.price_band_percent > 100) {
        return Invalid("price_band_percent outside 1..100");
    }
    if (params.max_delay_slots > std::numeric_limits<uint16_t>::max()) {
        return Invalid("max_delay_slots does not fit ActionV2's u16 max_delay");
    }
    if (params.max_fills_per_order > params.max_fills_per_slot) {
        return Invalid("max_fills_per_order above max_fills_per_slot");
    }
    if (params.outcome_index_slots < params.max_delay_slots) {
        return Invalid("outcome_index_slots below max_delay_slots");
    }

    // Outline stubs: refused with their task ID, never faked.
    if (params.price_band_enabled) {
        // TODO(rebirth:M3-ENG-03) +/- band against the epoch reference price.
        return NotImplemented("M3-ENG-03");
    }
    if (params.fee_incidence == FeeIncidence::PER_SIDE) {
        // TODO(rebirth:M3-ENG-08) PER_SIDE fee incidence and the D6 dust rule.
        return NotImplemented("M3-ENG-08");
    }
    if (params.futures_enabled) {
        // TODO(rebirth:M9-FUT-01) dated isolated linear futures in Q.
        return NotImplemented("M9-FUT-01");
    }
    if (params.signer_mode == SignerMode::FOLDED_SIGNING) {
        // TODO(rebirth:M4-STO-05) folded signer mode (sign, then one barrier).
        return NotImplemented("M4-STO-05");
    }
    return Ok();
}

std::vector<unsigned char> EncodeProfile(const ProfileParams& params)
{
    std::vector<unsigned char> out;
    VectorWriter writer{out, 0};
    const bool source_extended{params.source_range_commitment || params.source_chain_id != 0 || params.source_confirmations != 0};
    const bool nitro_extended{params.nitro_lab.has_value() || params.profile_id == NITRO_LAB_PROFILE_ID};
    const bool extended{params.stable_fee_asset.has_value() || params.withdrawals_enabled || params.withdrawal_fee_atoms != 0 || params.view_bound_commits || source_extended || nitro_extended};
    writer << (nitro_extended ? NITRO_LAB_PROFILE_ENCODING_VERSION : source_extended ? STABLE_Q_NETWORK_PROFILE_ENCODING_VERSION :
               params.view_bound_commits ? STABLE_Q_VIEW_PROFILE_ENCODING_VERSION :
               extended ? STABLE_Q_PROFILE_ENCODING_VERSION : PROFILE_ENCODING_VERSION);
    writer << params.profile_id;
    writer << params.fm_version;
    writer << ChainTypeToString(params.chain);
    writer << static_cast<uint8_t>(params.matching);
    writer << params.fee_ppm;
    writer << static_cast<uint8_t>(params.fee_incidence);
    writer << params.seat_share_percent;
    writer << params.max_actions_per_slot;
    writer << params.max_action_payload_bytes;
    writer << params.max_fills_per_order;
    writer << params.max_fills_per_slot;
    writer << params.max_changed_leaves_per_slot;
    writer << params.max_open_orders_per_account;
    writer << static_cast<uint8_t>(params.price_band_enabled ? 1 : 0);
    writer << params.price_band_percent;
    writer << params.nonce_window;
    writer << params.max_delay_slots;
    writer << static_cast<uint8_t>(params.futures_enabled ? 1 : 0);
    writer << static_cast<uint8_t>(params.signer_mode);
    writer << params.pool_capacity;
    writer << params.outcome_index_slots;
    if (extended) {
        writer << static_cast<uint8_t>(params.stable_fee_asset.has_value() ? 1 : 0);
        if (params.stable_fee_asset.has_value()) writer << *params.stable_fee_asset;
        writer << static_cast<uint8_t>(params.withdrawals_enabled ? 1 : 0);
        writer << params.withdrawal_fee_atoms;
    }
    if (params.view_bound_commits || source_extended || nitro_extended) writer << static_cast<uint8_t>(params.view_bound_commits ? 1 : 0);
    if (source_extended || nitro_extended) {
        writer << static_cast<uint8_t>(params.source_range_commitment ? 1 : 0);
        writer << params.source_chain_id << params.source_confirmations;
    }
    if (nitro_extended) {
        writer << static_cast<uint8_t>(params.nitro_lab.has_value() ? 1 : 0);
        if (params.nitro_lab) {
            const auto& binding{*params.nitro_lab};
            writer << binding.source_genesis_hash << binding.quote_token << binding.quote_token_code_hash
                   << binding.quote_implementation << binding.quote_implementation_code_hash;
        }
    }
    return out;
}

uint256 ProfileHash(const ProfileParams& params)
{
    return Tagged(HashTag::PROFILE, EncodeProfile(params));
}

uint256 DomainHash(const NetworkId& network, uint32_t fm_version)
{
    HashWriter writer{TaggedWriter(HashTag::NET_DOMAIN)};
    writer << network.modern_chain_domain;
    writer << fm_version;
    writer.write(std::as_bytes(std::span{network.magic}));
    return writer.GetSHA256();
}

std::string_view ToString(MatchingProfile matching)
{
    switch (matching) {
    case MatchingProfile::A2_PRORATA: return "A2_PRORATA";
    }
    return "UNKNOWN";
}

std::string_view ToString(FeeIncidence incidence)
{
    switch (incidence) {
    case FeeIncidence::SELLER: return "SELLER";
    case FeeIncidence::PER_SIDE: return "PER_SIDE";
    }
    return "UNKNOWN";
}

std::string_view ToString(SignerMode mode)
{
    switch (mode) {
    case SignerMode::STRICT_SIGNING: return "STRICT";
    case SignerMode::FOLDED_SIGNING: return "FOLDED";
    }
    return "UNKNOWN";
}

} // namespace flowmesh2
