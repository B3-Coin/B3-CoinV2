// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_MODERN_BRIDGE_FORWARDER_H
#define B3COIN_MODERN_BRIDGE_FORWARDER_H

#include <consensus/bridge_params.h>
#include <consensus/params.h>
#include <crypto/common.h>
#include <modern/bridge_v2.h>
#include <modern/withdrawal_tree.h>
#include <uint256.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

namespace modern {

/**
 * Bridge V2 forwarder accounts (doc/design/b3-bridge-v2-contracts-spec.md
 * section 7.2, normative; node changes sections 7.2 and 11.2).
 *
 * A forwarder account is the CREATE2 address of B3Forwarder deployed by its
 * factory, so a wallet derives it offline from the factory and
 * FORWARDER_INIT_CODE_HASH pins and the user can pay it before it exists:
 *
 *   depositSalt = keccak256(abi.encode(DEPOSIT_TAG, address vault,
 *                     bytes32 b3Recipient, address refundEOA, uint96 feeCap,
 *                     uint64 notAfter))
 *   exitSalt    = keccak256(abi.encode(EXIT_TAG, uint32 dstEid, bytes32 to,
 *                     uint16 maxBridgeFeeBps, uint96 feeCap,
 *                     address refundEOA))
 *   account     = keccak256(0xff || factory || salt || initCodeHash)[12:]
 *
 * Each salt preimage is six 32-byte words (192 bytes), every field
 * left-padded big-endian; the CREATE2 preimage is 85 bytes. Hashes and
 * addresses keep their raw EVM byte order. contracts/src/v2/
 * B3ForwarderTypesV2.sol is the on-chain twin; the shared vectors
 * (contracts/test/vectors/bridge_v2_vectors.json "forwarder_v2") pin both.
 *
 * The fee caps are uint96 on chain; B3 carries them as uint64 (a subset), the
 * width of the burn record's exit block.
 */
inline constexpr std::string_view FORWARDER_DEPOSIT_TAG_STRING{"B3/FWD/DEPOSIT/V2"};
inline constexpr std::string_view FORWARDER_EXIT_TAG_STRING{"B3/FWD/EXIT/OFT/V2"};
inline constexpr size_t FORWARDER_SALT_PREIMAGE_SIZE{6 * 32};
inline constexpr size_t FORWARDER_CREATE2_PREIMAGE_SIZE{1 + 20 + 32 + 32};
static_assert(FORWARDER_SALT_PREIMAGE_SIZE == 192);
static_assert(FORWARDER_CREATE2_PREIMAGE_SIZE == 85);
//! B3OftExitFactory MAX_BRIDGE_FEE_BPS: the OFT slippage bound (0.5%).
inline constexpr uint16_t FORWARDER_MAX_BRIDGE_FEE_BPS{50};

using ForwarderWord = std::array<unsigned char, 32>;

namespace bridge_forwarder_detail {

inline uint256 KeccakOf(const std::string_view text)
{
    return WithdrawalKeccak(std::span<const unsigned char>{
        reinterpret_cast<const unsigned char*>(text.data()), text.size()});
}

//! Writes `value` big-endian into the low 8 bytes of the 32-byte word at
//! `word`; the high 24 bytes stay zero (the array is zero-initialised).
inline void WriteUintWord(unsigned char* word, const uint64_t value)
{
    WriteBE64(word + 24, value);
}

//! An address left-padded into the 32-byte word at `word`.
inline void WriteAddressWord(unsigned char* word,
                             const Consensus::BridgeEthAddress& address)
{
    std::copy(address.begin(), address.end(), word + 12);
}

inline bool IsZero(const std::span<const unsigned char> bytes)
{
    return std::all_of(bytes.begin(), bytes.end(),
                       [](const unsigned char byte) { return byte == 0; });
}

} // namespace bridge_forwarder_detail

//! keccak256("B3/FWD/DEPOSIT/V2") = 0xcc2b88b5...01d2a (raw bytes).
inline const uint256& DepositForwarderTagV2()
{
    static const uint256 tag{bridge_forwarder_detail::KeccakOf(FORWARDER_DEPOSIT_TAG_STRING)};
    return tag;
}

//! keccak256("B3/FWD/EXIT/OFT/V2") = 0x0fdc29b9...b4c87 (raw bytes).
inline const uint256& ExitForwarderTagV2()
{
    static const uint256 tag{bridge_forwarder_detail::KeccakOf(FORWARDER_EXIT_TAG_STRING)};
    return tag;
}

/** B3ForwarderTypesV2.DepositParams: one route (b) deposit forwarder F_d. */
struct DepositForwarderParamsV2 {
    Consensus::BridgeEthAddress vault{};
    ForwarderWord b3_recipient{}; // RECIPIENT_V1 word
    Consensus::BridgeEthAddress refund_eoa{};
    uint64_t fee_cap{0};   // lifetime fee bound (uint96 on chain)
    uint64_t not_after{0}; // unix seconds: flush until, returnToOwner after

    friend bool operator==(const DepositForwarderParamsV2&,
                           const DepositForwarderParamsV2&) = default;
};

/** B3ForwarderTypesV2.ExitParams: one route (d3) exit forwarder F_x. */
struct ExitForwarderParamsV2 {
    uint32_t dst_eid{0}; // LayerZero endpoint id (Arbitrum 30110)
    ForwarderWord to{};  // destination address word
    uint16_t max_bridge_fee_bps{0};
    uint64_t fee_cap{0}; // per-forward fee bound (uint96 on chain)
    Consensus::BridgeEthAddress refund_eoa{};

    friend bool operator==(const ExitForwarderParamsV2&,
                           const ExitForwarderParamsV2&) = default;
};

/** abi.encode(DEPOSIT_TAG, vault, b3Recipient, refundEOA, feeCap, notAfter). */
inline std::array<unsigned char, FORWARDER_SALT_PREIMAGE_SIZE>
DepositForwarderSaltPreimageV2(const DepositForwarderParamsV2& p)
{
    using namespace bridge_forwarder_detail;
    std::array<unsigned char, FORWARDER_SALT_PREIMAGE_SIZE> out{};
    const uint256& tag{DepositForwarderTagV2()};
    std::copy(tag.begin(), tag.end(), out.begin());
    WriteAddressWord(out.data() + 32, p.vault);
    std::copy(p.b3_recipient.begin(), p.b3_recipient.end(), out.begin() + 64);
    WriteAddressWord(out.data() + 96, p.refund_eoa);
    WriteUintWord(out.data() + 128, p.fee_cap);
    WriteUintWord(out.data() + 160, p.not_after);
    return out;
}

/** abi.encode(EXIT_TAG, dstEid, to, maxBridgeFeeBps, feeCap, refundEOA). */
inline std::array<unsigned char, FORWARDER_SALT_PREIMAGE_SIZE>
ExitForwarderSaltPreimageV2(const ExitForwarderParamsV2& p)
{
    using namespace bridge_forwarder_detail;
    std::array<unsigned char, FORWARDER_SALT_PREIMAGE_SIZE> out{};
    const uint256& tag{ExitForwarderTagV2()};
    std::copy(tag.begin(), tag.end(), out.begin());
    WriteUintWord(out.data() + 32, p.dst_eid);
    std::copy(p.to.begin(), p.to.end(), out.begin() + 64);
    WriteUintWord(out.data() + 96, p.max_bridge_fee_bps);
    WriteUintWord(out.data() + 128, p.fee_cap);
    WriteAddressWord(out.data() + 160, p.refund_eoa);
    return out;
}

inline uint256 DepositForwarderSaltV2(const DepositForwarderParamsV2& p)
{
    return WithdrawalKeccak(DepositForwarderSaltPreimageV2(p));
}

inline uint256 ExitForwarderSaltV2(const ExitForwarderParamsV2& p)
{
    return WithdrawalKeccak(ExitForwarderSaltPreimageV2(p));
}

/** EIP-1014: keccak256(0xff || factory || salt || initCodeHash)[12:]. */
inline Consensus::BridgeEthAddress Create2AddressV2(
    const Consensus::BridgeEthAddress& factory, const uint256& salt,
    const uint256& init_code_hash)
{
    std::array<unsigned char, FORWARDER_CREATE2_PREIMAGE_SIZE> preimage{};
    preimage[0] = 0xff;
    std::copy(factory.begin(), factory.end(), preimage.begin() + 1);
    std::copy(salt.begin(), salt.end(), preimage.begin() + 21);
    std::copy(init_code_hash.begin(), init_code_hash.end(), preimage.begin() + 53);
    const uint256 hash{WithdrawalKeccak(preimage)};
    Consensus::BridgeEthAddress out{};
    std::copy(hash.begin() + 12, hash.end(), out.begin());
    return out;
}

/** F_d = B3DepositForwarderFactory.accountOf(p) under the pinned factory. */
inline Consensus::BridgeEthAddress DepositForwarderAddressV2(
    const Consensus::BridgeV2DepositForwarderPins& pins,
    const DepositForwarderParamsV2& p)
{
    return Create2AddressV2(pins.factory, DepositForwarderSaltV2(p),
                            pins.init_code_hash);
}

/** F_x = B3OftExitFactory.accountOf(p) under the pinned factory. */
inline Consensus::BridgeEthAddress ExitForwarderAddressV2(
    const Consensus::BridgeV2ExitForwarderPins& pins,
    const ExitForwarderParamsV2& p)
{
    return Create2AddressV2(pins.factory, ExitForwarderSaltV2(p),
                            pins.init_code_hash);
}

/** RECIPIENT_V1 (B3ForwarderTypesV2.recipientV1Valid): 11 zero bytes,
 *  version byte 1, a non-zero 20-byte key hash. */
inline bool ForwarderRecipientV1Valid(const ForwarderWord& recipient)
{
    using namespace bridge_forwarder_detail;
    return IsZero(std::span{recipient}.first(11)) && recipient[11] == 1 &&
           !IsZero(std::span{recipient}.subspan(12));
}

/** An EVM address word: upper 12 bytes zero, lower 20 bytes non-zero. */
inline bool ForwarderAddressWordValid(const ForwarderWord& word)
{
    using namespace bridge_forwarder_detail;
    return IsZero(std::span{word}.first(12)) && !IsZero(std::span{word}.subspan(12));
}

/** The address word of `address` (left-padded). */
inline ForwarderWord ForwarderAddressWord(const Consensus::BridgeEthAddress& address)
{
    ForwarderWord out{};
    bridge_forwarder_detail::WriteAddressWord(out.data(), address);
    return out;
}

/**
 * B3DepositForwarderFactory.isValid(p) for a factory over `vault` and
 * `token`: p.vault is the vault, b3_recipient is RECIPIENT_V1, and
 * refund_eoa is non-null and none of the vault, the token or the factory
 * (OI-6: returnToOwner must never pay a contract that gives no claim back).
 * EOA-ness of refund_eoa is not checkable here; wallets prove it with the
 * EIP-191 signature of bridge/eip191.h.
 */
inline bool DepositForwarderParamsValidV2(
    const DepositForwarderParamsV2& p, const Consensus::BridgeEthAddress& vault,
    const Consensus::BridgeEthAddress& token,
    const Consensus::BridgeEthAddress& factory)
{
    return p.vault == vault && ForwarderRecipientV1Valid(p.b3_recipient) &&
           !Consensus::BridgeAddressIsNull(p.refund_eoa) &&
           p.refund_eoa != vault && p.refund_eoa != token &&
           p.refund_eoa != factory;
}

/**
 * B3OftExitFactory.isValid(p) except EOA-ness (contracts spec section 8;
 * node changes section 7.2 item 5): a non-zero pinned dst_eid that p names,
 * `to` an address word, max_bridge_fee_bps <= 50 and a non-null refund_eoa.
 */
inline bool ExitForwarderParamsValidV2(const ExitForwarderParamsV2& p,
                                       const uint32_t pinned_dst_eid)
{
    return pinned_dst_eid != 0 && p.dst_eid == pinned_dst_eid &&
           ForwarderAddressWordValid(p.to) &&
           p.max_bridge_fee_bps <= FORWARDER_MAX_BRIDGE_FEE_BPS &&
           !Consensus::BridgeAddressIsNull(p.refund_eoa);
}

// ------------------------------------------------------------- exit block
//
// The optional exit block of a V2 burn record (route d3; node changes
// section 7.2 item 5): u8 exit_version = 1 || u32 dst_eid || 32-byte `to` word
// || u16 max_bridge_fee_bps || u64 fee_cap || 20-byte refund_eoa, big-endian,
// 67 bytes. The codec checks only the version and the exact length; the
// parameter rules are ExitForwarderParamsValidV2 against the pinned exit
// forwarder.

inline constexpr uint8_t EXIT_FORWARDER_BLOCK_VERSION_V1{1};
inline constexpr size_t EXIT_FORWARDER_BLOCK_SIZE{1 + 4 + 32 + 2 + 8 + 20};
static_assert(EXIT_FORWARDER_BLOCK_SIZE == 67);

inline std::array<unsigned char, EXIT_FORWARDER_BLOCK_SIZE>
EncodeExitForwarderBlockV2(const ExitForwarderParamsV2& p)
{
    std::array<unsigned char, EXIT_FORWARDER_BLOCK_SIZE> out{};
    out[0] = EXIT_FORWARDER_BLOCK_VERSION_V1;
    WriteBE32(out.data() + 1, p.dst_eid);
    std::copy(p.to.begin(), p.to.end(), out.begin() + 5);
    WriteBE16(out.data() + 37, p.max_bridge_fee_bps);
    WriteBE64(out.data() + 39, p.fee_cap);
    std::copy(p.refund_eoa.begin(), p.refund_eoa.end(), out.begin() + 47);
    return out;
}

inline std::optional<ExitForwarderParamsV2> DecodeExitForwarderBlockV2(
    const std::span<const unsigned char> bytes)
{
    if (bytes.size() != EXIT_FORWARDER_BLOCK_SIZE ||
        bytes[0] != EXIT_FORWARDER_BLOCK_VERSION_V1) {
        return std::nullopt;
    }
    ExitForwarderParamsV2 out;
    out.dst_eid = ReadBE32(bytes.data() + 1);
    std::copy(bytes.begin() + 5, bytes.begin() + 37, out.to.begin());
    out.max_bridge_fee_bps = ReadBE16(bytes.data() + 37);
    out.fee_cap = ReadBE64(bytes.data() + 39);
    std::copy(bytes.begin() + 47, bytes.end(), out.refund_eoa.begin());
    return out;
}

// ----------------------------------------------------------- OI-2 fee cap
//
// OI-2 (arbitrum proposal section 20; contracts spec section 7.4): a route
// (b) delivery that needs more paid flushes than the lifetime feeCap covers
// strands its remainder. Resolution, OI-2's second direction: the wallet caps
// a route (b) delivery at the vault's max_deposit_raw per forwarder, so one
// flush deposits all of it, min(free - fee, MAX_DEPOSIT_RAW) = free - fee,
// whatever fee within feeCap its flusher takes. The lifetime feeCap is the
// spec default, fee_per_tx x 4: deploy (recouped by the first flush), two
// flushes (the second for a repeat arrival) and a returnToOwner.
//
// The first direction, feeCap = fee_per_tx x (ceil(amount / MAX_DEPOSIT_RAW)
// + 3), holds only for cooperative flushers: flush lets any codeless EOA take
// the whole remaining cap in one call, so a greedy first flusher leaves every
// later chunk to a zero-fee flush, a refund or returnToOwner(p, 0). Bounding
// each flush's fee in p would change the salt layout and needs an owner
// ruling. Nothing changes on chain.

//! Transactions the lifetime fee cap pays: deploy, two flushes, returnToOwner.
inline constexpr uint64_t DEPOSIT_FORWARDER_FEE_CAP_TXS{4};

//! fee_per_tx x 4; nullopt on a zero fee or beyond uint64.
inline std::optional<uint64_t> DepositForwarderFeeCapV2(const uint64_t fee_per_tx)
{
    if (fee_per_tx == 0 ||
        fee_per_tx > std::numeric_limits<uint64_t>::max() / DEPOSIT_FORWARDER_FEE_CAP_TXS) {
        return std::nullopt;
    }
    return fee_per_tx * DEPOSIT_FORWARDER_FEE_CAP_TXS;
}

// ------------------------------------------------- light-client horizon
//
// Node changes section 12.3; contracts spec section 7.4 (wallet policy, not
// consensus). The V2 light client follows Ethereum only through the pinned
// fork schedule (fork_schedule_valid_through_epoch); beyond that horizon
// mints stop until an extension hard fork, and a deposit made then waits for
// it. Wallets therefore refuse route (b) forwarders within 7 days of the
// horizon, and an F_d's notAfter ends 24 hours before it.

//! Ethereum's slot time, as the mint rules' freshness bound uses it.
inline constexpr uint64_t BRIDGE_ETHEREUM_SECONDS_PER_SLOT{12};
//! Default and longest F_d lifetime: notAfter = creation + 30 days (C-6).
inline constexpr int64_t DEPOSIT_FORWARDER_MAX_LIFETIME_SECONDS{30 * 24 * 60 * 60};
//! notAfter ends this long before the light-client horizon.
inline constexpr int64_t DEPOSIT_FORWARDER_HORIZON_MARGIN_SECONDS{24 * 60 * 60};
//! Route (b) forwarders are refused this close to the horizon.
inline constexpr int64_t DEPOSIT_FORWARDER_HORIZON_REFUSAL_SECONDS{7 * 24 * 60 * 60};

/**
 * The light-client horizon as Unix time: the start of the first beacon slot
 * beyond `valid_through_epoch`, extrapolated from the finalized head (its
 * beacon slot and execution timestamp) at 12 seconds per slot. On a regtest
 * synthetic beacon slots are not timed, so it is only an estimate there.
 * Saturates at INT64_MAX.
 */
inline int64_t BridgeV2LightClientHorizonTime(const uint64_t valid_through_epoch,
                                              const uint64_t finalized_slot,
                                              const uint64_t finalized_timestamp)
{
    constexpr uint64_t u64_max{std::numeric_limits<uint64_t>::max()};
    constexpr auto i64_max{static_cast<uint64_t>(std::numeric_limits<int64_t>::max())};
    if (valid_through_epoch >= u64_max / Consensus::ETHEREUM_SLOTS_PER_EPOCH) {
        return std::numeric_limits<int64_t>::max();
    }
    const uint64_t end_slot{(valid_through_epoch + 1) * Consensus::ETHEREUM_SLOTS_PER_EPOCH};
    const uint64_t slots{end_slot > finalized_slot ? end_slot - finalized_slot : 0};
    if (slots > i64_max / BRIDGE_ETHEREUM_SECONDS_PER_SLOT ||
        finalized_timestamp > i64_max - slots * BRIDGE_ETHEREUM_SECONDS_PER_SLOT) {
        return std::numeric_limits<int64_t>::max();
    }
    return static_cast<int64_t>(finalized_timestamp + slots * BRIDGE_ETHEREUM_SECONDS_PER_SLOT);
}

/**
 * The latest notAfter of a route (b) forwarder derived at `now` (Unix
 * seconds, positive): now + 30 days and, with a known light-client horizon,
 * at most horizon - 24 hours. nullopt when the wallet refuses the forwarder:
 * the horizon is less than 7 days away (or `now` is out of range).
 */
inline std::optional<int64_t> DepositForwarderLatestNotAfterV2(
    const int64_t now, const std::optional<int64_t>& horizon_time)
{
    if (now <= 0 ||
        now > std::numeric_limits<int64_t>::max() - DEPOSIT_FORWARDER_MAX_LIFETIME_SECONDS) {
        return std::nullopt;
    }
    int64_t latest{now + DEPOSIT_FORWARDER_MAX_LIFETIME_SECONDS};
    if (horizon_time) {
        if (*horizon_time - now < DEPOSIT_FORWARDER_HORIZON_REFUSAL_SECONDS) return std::nullopt;
        latest = std::min(latest, *horizon_time - DEPOSIT_FORWARDER_HORIZON_MARGIN_SECONDS);
    }
    return latest;
}

// --------------------------------------------------- route availability
//
// Static and height refusals of the wallet's forwarder routes, before any
// B3 state is read (node changes section 11.2): the same text on every
// network, so unit tests pin it.

/**
 * Why `bridgeforwarderaddress` cannot derive a route (b) forwarder for the
 * block at `next_height`, or nullopt: bridge V2 is configured, a deposit
 * forwarder is pinned, and the launch vault (the only one the deposit factory
 * deposits into) still takes deposits, i.e. no B-23 migration is active.
 */
inline std::optional<std::string_view> DepositForwarderUnavailableReasonV2(
    const Consensus::Params& params, const int next_height)
{
    if (!params.bridge_v2 || !ConfiguredBridgeV2AssetId(params)) {
        return "Bridge V2 is not configured on this chain";
    }
    if (!params.bridge_v2->deposit_forwarder) {
        return "Bridge V2 pins no deposit forwarder (route b) on this chain";
    }
    if (ActiveBridgeV2Role(params, next_height) != Consensus::BridgeV2EntryRole::LAUNCH) {
        return "The bridge V2 launch vault is retired for deposits and the deposit forwarder factory deposits only "
               "into it; use getbridgedepositrecipient for the successor vault";
    }
    return std::nullopt;
}

/**
 * Why `bridgewithdraw` cannot take options.arbitrum (route d3) for the block
 * at `next_height`, or nullopt: an exit forwarder is pinned and bridge V2
 * burn rules are active (from W2).
 */
inline std::optional<std::string_view> ExitForwarderRouteUnavailableReasonV2(
    const Consensus::Params& params, const int next_height)
{
    if (!params.bridge_v2 || !params.bridge_v2->exit_forwarder) {
        return "options.arbitrum (route d3) needs a pinned bridge V2 exit forwarder; this chain pins none";
    }
    if (!params.BridgeV2BurnRulesActive(next_height)) {
        return "options.arbitrum (route d3) needs bridge V2 burn rules for the next block";
    }
    return std::nullopt;
}

// ------------------------------------------------------- OI-6 reserved
//
// OI-6 (arbitrum proposal section 20; contracts spec section 6.2): plain
// transfers or route (b) sends to Vault V2 are lost. Wallets refuse every
// bridge V2 contract as a withdrawal `to`, an exit `to` or a refund EOA, and
// no forwarder derivation is offered that yields one. Consensus refuses one
// as the refund EOA of a route (d3) exit block (VerifyBridgeBurnV2 check 5):
// none of them can call withdrawOnEthereum, so F_x would lose its escape.

/**
 * The bridge V2 role `address` holds in `params`, or nullopt when it holds
 * none: the launch vault, the successor vault, the origin token, the
 * verifier, the deposit forwarder factory or the exit forwarder factory.
 * The null address is not a role; callers refuse it on its own.
 */
inline std::optional<std::string_view> BridgeV2ReservedEthAddressReason(
    const Consensus::BridgeV2Params& params,
    const Consensus::BridgeEthAddress& address)
{
    if (Consensus::BridgeAddressIsNull(address)) return std::nullopt;
    if (address == params.vault.vault_address) return "the bridge V2 launch vault";
    if (params.successor && address == params.successor->entry.vault_address) {
        return "the bridge V2 successor vault";
    }
    if (address == params.vault.token_address ||
        (params.successor && address == params.successor->entry.token_address)) {
        return "the bridge V2 origin token";
    }
    if (address == params.verifier.address) return "the bridge V2 verifier";
    if (params.deposit_forwarder && address == params.deposit_forwarder->factory) {
        return "the bridge V2 deposit forwarder factory";
    }
    if (params.exit_forwarder && address == params.exit_forwarder->factory) {
        return "the bridge V2 exit forwarder factory";
    }
    return std::nullopt;
}

} // namespace modern

#endif // B3COIN_MODERN_BRIDGE_FORWARDER_H
