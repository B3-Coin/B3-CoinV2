// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_CORE_PARAMS_H
#define B3_FLOWMESH2_CORE_PARAMS_H

#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/result.h>
#include <uint256.h>
#include <util/chaintype.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/** Network identity and the provisional engine profile (slice R3).
 *
 * PROVISIONAL REGTEST. Every value of DevRegtestProfile() is a plan section
 * 12.1 recommended default used provisionally under the owner's instruction
 * of 2026-10-16 (TODO.md, "Provisional decisions"). None is valid for mainnet
 * until the owner ratifies it. PROFILE_DEX1_REGTEST.json mirrors the values
 * and its profile_hash is checked against ProfileHash(DevRegtestProfile()).
 *
 * Two hashes bind a slot to its rules:
 * - DomainHash(network, fm_version) = Tagged(DOMAIN, modern_chain_domain ||
 *   u32 LE fm_version || 4-byte magic): WHERE. Every ActionV2 and slot header
 *   carries it, so an action is invalid on another network or FlowMesh
 *   version (D1: the same B3 chain, through its ModernChainDomain).
 * - ProfileHash(params) = Tagged(PROFILE, EncodeProfile(params)): WHICH RULES.
 *   It covers every ProfileParams field and nothing of the network except the
 *   chain type, so the JSON profile file has one hash for every regtest.
 */
namespace flowmesh2 {

//! The FlowMesh V2 protocol version bound into DomainHash.
inline constexpr uint32_t FM_VERSION{1};
//! Version byte that starts EncodeProfile's output.
inline constexpr uint8_t PROFILE_ENCODING_VERSION{1};
//! Encoding 2 appends stable-Q and withdrawal policy; encoding 1 stays frozen.
inline constexpr uint8_t STABLE_Q_PROFILE_ENCODING_VERSION{2};
inline constexpr std::string_view STABLE_Q_PROFILE_ID{"fm2-regtest-stable-q/1"};
//! Encoding 3 explicitly requires view-bound certificates; encoding 1/2 stay frozen.
inline constexpr uint8_t STABLE_Q_VIEW_PROFILE_ENCODING_VERSION{3};
inline constexpr std::string_view STABLE_Q_VIEW_PROFILE_ID{"fm2-regtest-stable-q/2"};
//! Encoding 4 commits every observed source range, including empty event batches.
inline constexpr uint8_t STABLE_Q_NETWORK_PROFILE_ENCODING_VERSION{4};
inline constexpr std::string_view STABLE_Q_NETWORK_PROFILE_ID{"fm2-regtest-stable-q/3"};
//! Longest accepted profile_id, in bytes.
inline constexpr size_t MAX_PROFILE_ID_BYTES{64};

/** Where FlowMesh runs: the B3 chain type, its 4-byte network magic and its
 * ModernChainDomain (src/modern/chain_domain.h; passed in, never computed
 * here). Tests and the simulator use synthetic values labelled TEST.
 */
struct NetworkId {
    ChainType chain{ChainType::REGTEST};
    std::array<unsigned char, 4> magic{};
    uint256 modern_chain_domain;
};

//! D4 matching profile. APPEND-ONLY registry; zero is never a value.
enum class MatchingProfile : uint8_t {
    //! Makers rest in price-time FIFO; same-slot takers are filled pro-rata per
    //! level (largest remainder, ties by canonical rank). PROVISIONAL (D4).
    A2_PRORATA = 1,
};

//! D5 fee incidence. APPEND-ONLY registry; zero is never a value.
enum class FeeIncidence : uint8_t {
    SELLER = 1,   //!< the seller pays fee_ppm once, in the quote asset (D5)
    PER_SIDE = 2, //!< both sides pay; Validate refuses it (M3-ENG-08)
};

/** Signer barrier mode. APPEND-ONLY registry; zero is never a value. The
 * enumerators carry a suffix because Windows headers define STRICT as a macro;
 * the registry names (ToString, JSON) are "STRICT" and "FOLDED".
 */
enum class SignerMode : uint8_t {
    STRICT_SIGNING = 1, //!< intent barrier, sign, signed-vote barrier, send (DUR-1)
    FOLDED_SIGNING = 2, //!< sign, then one barrier; Validate refuses it (M4-STO-05, D15)
};

/** The engine profile: every rule parameter of FlowMesh V2 that is not part of
 * the network identity. Field order is the EncodeProfile order.
 */
struct ProfileParams {
    // Identity.
    std::string profile_id;     //!< "fm2-dex1-regtest-provisional/1"
    uint32_t fm_version{0};     //!< FM_VERSION
    ChainType chain{ChainType::REGTEST}; //!< REGTEST only (D8: regtest exchange first)

    // D4 matching.
    MatchingProfile matching{MatchingProfile::A2_PRORATA};

    // D5 fees.
    uint32_t fee_ppm{0};        //!< 100: fee per million of matched quote notional
    FeeIncidence fee_incidence{FeeIncidence::SELLER};
    uint32_t seat_share_percent{0}; //!< 80: floor share of slot fees to the seat pool, rest treasury

    // D6 engine caps and price band.
    uint32_t max_actions_per_slot{0};        //!< 1,024
    uint32_t max_action_payload_bytes{0};    //!< 4,096
    uint32_t max_fills_per_order{0};         //!< 1,024 (remainder: REJ_FILL_CAP)
    uint32_t max_fills_per_slot{0};          //!< 16,384
    uint32_t max_changed_leaves_per_slot{0}; //!< 8,192
    uint32_t max_open_orders_per_account{0}; //!< 1,000
    bool price_band_enabled{false};          //!< false; true is refused (M3-ENG-03)
    uint32_t price_band_percent{0};          //!< 50: the +/- band once M3-ENG-03 lands

    // D7 nonces and expiry.
    uint32_t nonce_window{0};    //!< 100 highest used nonces per account
    uint32_t max_delay_slots{0}; //!< 1,024: largest ActionV2 max_delay (fits its u16)

    // Other.
    bool futures_enabled{false}; //!< false; true is refused (M9-FUT-01)
    SignerMode signer_mode{SignerMode::STRICT_SIGNING}; //!< STRICT; FOLDED is refused (M4-STO-05)
    uint32_t pool_capacity{0};       //!< 4,096 admitted actions per node
    uint32_t outcome_index_slots{0}; //!< 1,024: >= max_delay_slots, so DUPLICATE stays definite

    // Optional encoding-2 extension. Absent preserves every legacy byte.
    // Present only for a versioned stable-Q profile and exactly RegtestQuoteAsset().
    // This names synthetic regtest Q, never a production stablecoin.
    std::optional<AssetId> stable_fee_asset;
    bool withdrawals_enabled{false}; //!< encoding-2 REGTEST Q withdrawals only
    Atoms withdrawal_fee_atoms{0};   //!< test-only Q charge; disabled implies zero
    bool view_bound_commits{false};  //!< encoding 3 only; certificates bind the exact voting view
    bool source_range_commitment{false}; //!< encoding 4: exactly one terminal SourceRangeFact per slot
    uint64_t source_chain_id{0};           //!< /3 pins isolated EVM31337; legacy profiles require zero
    uint32_t source_confirmations{0};      //!< /3 pins two regtest confirmations; no production finality claim

    friend bool operator==(const ProfileParams&, const ProfileParams&) = default;
};

/** The provisional regtest profile fm2-dex1-regtest-provisional/1.
 * UNSUPPORTED_NETWORK for MAIN, TESTNET, TESTNET4 and SIGNET.
 */
Result<ProfileParams> DevRegtestProfile(ChainType chain);

//! Deterministic synthetic Q: Tagged(TEST_ASSET, raw UTF-8 "rUSD"). No backing claim.
AssetId RegtestQuoteAsset();

/** First stable-fee regtest slice. Every admitted market must quote exact Q.
 * Keeps the old profile's unratified rate/incidence/caps as test parameters;
 * a new profile id and encoding bind Q without reinterpreting old signatures
 * or vectors. MAIN/TESTNET/TESTNET4/SIGNET are unsupported.
 * Wider non-Q-quoted markets and futures remain separate product work.
 */
Result<ProfileParams> StableQRegtestProfile(ChainType chain);

//! Separate prerelease profile: same Q policy, commitment 3 and view-bound certificates.
//! Existing /1 profiles and saved journals retain their original rules and bytes.
Result<ProfileParams> StableQViewRegtestProfile(ChainType chain);

//! Authenticated-network prerelease profile: /2 rules plus a committed exact source range.
//! Every earlier profile/signature/journal keeps its original encoding and rules.
Result<ProfileParams> StableQNetworkRegtestProfile(ChainType chain);

/** Checks a profile, in this order:
 * 1. chain is not REGTEST: UNSUPPORTED_NETWORK;
 * 2. a structural rule fails: INVALID_INPUT (empty or over-long profile_id,
 *    fm_version != FM_VERSION, an unknown enum value, a zero cap, fee_ppm above
 *    one million, a share above 100 %, a band outside 1..100 %,
 *    max_delay_slots above 65,535, fills per order above fills per slot,
 *    outcome_index_slots below max_delay_slots);
 * 3. an outline stub: price_band_enabled (M3-ENG-03), PER_SIDE fees
 *    (M3-ENG-08), futures_enabled (M9-FUT-01), FOLDED signing (M4-STO-05) each
 *    return NotImplemented with that task ID.
 */
Result<void> Validate(const ProfileParams& params);

/** Canonical bytes of a profile: u8 PROFILE_ENCODING_VERSION, then every field
 * in original declaration order. If stable_fee_asset is present the first
 * byte is STABLE_Q_PROFILE_ENCODING_VERSION. Append u8 Q-present, Q's raw
 * AssetId if present, u8 withdrawals_enabled and i64 withdrawal_fee_atoms.
 * view_bound_commits selects encoding 3 and appends its explicit u8 flag.
 * Source policy selects encoding 4 and appends u8 source_range_commitment,
 * u64 source_chain_id and u32 source_confirmations after the explicit view flag.
 * Other non-default extensions use encoding 2; all extension defaults preserve
 * exactly encoding 1 (including invalid profiles hashed for diagnostics).
 * Strings are CompactSize-prefixed; the chain is its
 * ChainTypeToString name; integers are little-endian; bools and enums are u8.
 */
std::vector<unsigned char> EncodeProfile(const ProfileParams& params);

//! Tagged(PROFILE, EncodeProfile(params)).
uint256 ProfileHash(const ProfileParams& params);

//! Tagged(DOMAIN, modern_chain_domain || u32 LE fm_version || magic).
uint256 DomainHash(const NetworkId& network, uint32_t fm_version);

//! Registry names; "UNKNOWN" for an unregistered value.
std::string_view ToString(MatchingProfile matching);
std::string_view ToString(FeeIncidence incidence);
std::string_view ToString(SignerMode mode);

} // namespace flowmesh2

#endif // B3_FLOWMESH2_CORE_PARAMS_H
