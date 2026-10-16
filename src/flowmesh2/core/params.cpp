// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/core/params.h>

#include <flowmesh2/core/hashes.h>
#include <serialize.h>
#include <streams.h>

#include <limits>
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

Result<void> Validate(const ProfileParams& params)
{
    if (params.chain != ChainType::REGTEST) {
        return Error{ErrorCode::UNSUPPORTED_NETWORK, "the outline profile runs on regtest only"};
    }

    if (params.profile_id.empty() || params.profile_id.size() > MAX_PROFILE_ID_BYTES) {
        return Invalid("profile_id empty or longer than 64 bytes");
    }
    if (params.fm_version != FM_VERSION) return Invalid("unknown fm_version");
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
    writer << PROFILE_ENCODING_VERSION;
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
