// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3COIN_BRIDGE_ADMISSION_H
#define B3COIN_BRIDGE_ADMISSION_H

#include <bridge/deposit.h>
#include <consensus/amount.h>
#include <consensus/bridge_params.h>
#include <consensus/params.h>
#include <modern/bridge_asset.h>
#include <modern/policy.h>
#include <script/script.h>
#include <uint256.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <tuple>
#include <vector>

/**
 * Activation-inert stage-4 bridge admission primitives.
 *
 * These types encode only owner-ratified semantic rules from
 * doc/design/b3-bridge-threat-model.md section 5:
 *
 *  - a mint is admitted only by one ACTIVE full registry tuple;
 *  - the exact log-emitting vault and token must match that tuple;
 *  - the replay key is (origin chain, vault, deposit id), not a ticker or a
 *    relayer-selected transaction identifier;
 *  - origin raw units convert exactly, never by rounded arithmetic; and
 *  - proposed registrations never authorize minting.
 *
 * Nothing in this header is connected to the MPA registry, transaction
 * validation, chainstate, or chain parameters. In particular it does not
 * choose a payload type, activation height, mint cap, Ethereum checkpoint,
 * USDT address, vault address, or adapter identity. Those production values
 * remain fail-closed until they are explicitly pinned.
 */
namespace bridge {

inline constexpr uint8_t RECIPIENT_V1_P2PKH{
    Consensus::BRIDGE_RECIPIENT_VERSION_P2PKH_V1};
inline constexpr size_t RECIPIENT_V1_PADDING_SIZE{11};
inline constexpr size_t RECIPIENT_V1_HASH_SIZE{20};

struct RecipientV1 {
    std::array<unsigned char, RECIPIENT_V1_HASH_SIZE> pubkey_hash{};

    friend bool operator==(const RecipientV1&, const RecipientV1&) = default;
};

/** Proposed RECIPIENT_V1 codec. Keeping it here does not activate it. */
inline std::optional<RecipientV1> DecodeRecipientV1(
    const std::array<unsigned char, 32>& encoded)
{
    if (std::any_of(encoded.begin(), encoded.begin() + RECIPIENT_V1_PADDING_SIZE,
                    [](const unsigned char byte) { return byte != 0; })) {
        return std::nullopt;
    }
    if (encoded[RECIPIENT_V1_PADDING_SIZE] != RECIPIENT_V1_P2PKH) {
        return std::nullopt;
    }

    RecipientV1 out;
    std::copy(encoded.begin() + RECIPIENT_V1_PADDING_SIZE + 1, encoded.end(),
              out.pubkey_hash.begin());
    return out;
}

inline std::array<unsigned char, 32> EncodeRecipientV1(const RecipientV1& recipient)
{
    std::array<unsigned char, 32> out{};
    out[RECIPIENT_V1_PADDING_SIZE] = RECIPIENT_V1_P2PKH;
    std::copy(recipient.pubkey_hash.begin(), recipient.pubkey_hash.end(),
              out.begin() + RECIPIENT_V1_PADDING_SIZE + 1);
    return out;
}

inline CScript RecipientV1Script(const RecipientV1& recipient)
{
    return CScript{} << OP_DUP << OP_HASH160
                     << std::vector<unsigned char>{recipient.pubkey_hash.begin(),
                                                   recipient.pubkey_hash.end()}
                     << OP_EQUALVERIFY << OP_CHECKSIG;
}

/**
 * Convert an origin-chain uint256 raw amount to B3 asset units using only an
 * exact power-of-ten scale. Down-scaling rejects a remainder; up-scaling
 * rejects overflow. This utility selects no production decimal values.
 */
inline std::optional<CAmount> ConvertRawUnitsExact(
    std::array<unsigned char, 32> amount, const uint8_t origin_decimals,
    const uint8_t asset_decimals)
{
    if (origin_decimals > 18 || asset_decimals > 18) return std::nullopt;

    if (origin_decimals > asset_decimals) {
        for (uint8_t scale{0}; scale < origin_decimals - asset_decimals; ++scale) {
            unsigned remainder{0};
            for (unsigned char& byte : amount) {
                const unsigned current{(remainder << 8) | byte};
                byte = static_cast<unsigned char>(current / 10);
                remainder = current % 10;
            }
            if (remainder != 0) return std::nullopt;
        }
    }

    CAmount converted{0};
    for (const unsigned char byte : amount) {
        if (converted > (MAX_MONEY - byte) / 256) return std::nullopt;
        converted = converted * 256 + byte;
    }
    if (converted <= 0) return std::nullopt;

    if (asset_decimals > origin_decimals) {
        for (uint8_t scale{0}; scale < asset_decimals - origin_decimals; ++scale) {
            if (converted > MAX_MONEY / 10) return std::nullopt;
            converted *= 10;
        }
    }
    return converted;
}

enum class BridgeRegistryState : uint8_t {
    PROPOSED = 0,
    ACTIVE = 1,
    RETIRED = 2,
};

/** Full mint-admission tuple required by threat-model section 5. */
struct BridgeAssetRegistryEntry {
    uint64_t origin_chain_id{0};
    EthAddress vault_address{};
    EthAddress token_address{};
    modern::AssetId b3_asset_id{};
    uint8_t origin_decimals{0};
    uint8_t asset_decimals{0};
    /** Commitment to the approved token implementation or explicit adapter. */
    uint256 implementation_or_adapter{};
    uint32_t adapter_version{0};
    int32_t approval_first_height{-1};
    std::optional<int32_t> approval_last_height{}; // inclusive
    BridgeRegistryState state{BridgeRegistryState::PROPOSED};
};

inline bool EthAddressIsNull(const EthAddress& address)
{
    return std::all_of(address.begin(), address.end(),
                       [](const unsigned char byte) { return byte == 0; });
}

inline bool BridgeAssetRegistryEntryValid(const BridgeAssetRegistryEntry& entry)
{
    return entry.origin_chain_id != 0 && !EthAddressIsNull(entry.vault_address) &&
           !entry.b3_asset_id.IsNull() &&
           entry.origin_decimals <= 18 && entry.asset_decimals <= 18 &&
           !entry.implementation_or_adapter.IsNull() && entry.adapter_version != 0 &&
           entry.approval_first_height >= 0 &&
           (!entry.approval_last_height ||
            *entry.approval_last_height >= entry.approval_first_height);
}

inline bool BridgeAssetRegistryEntryActiveAt(const BridgeAssetRegistryEntry& entry,
                                             const int32_t height)
{
    return BridgeAssetRegistryEntryValid(entry) &&
           entry.state == BridgeRegistryState::ACTIVE &&
           height >= entry.approval_first_height &&
           (!entry.approval_last_height || height <= *entry.approval_last_height);
}

/** Collision-free logical replay key; persistent wire encoding remains open. */
struct BridgeDepositKey {
    uint64_t origin_chain_id{0};
    EthAddress vault_address{};
    uint64_t deposit_id{0};

    friend bool operator==(const BridgeDepositKey&, const BridgeDepositKey&) = default;
    friend bool operator<(const BridgeDepositKey& a, const BridgeDepositKey& b)
    {
        return std::tie(a.origin_chain_id, a.vault_address, a.deposit_id) <
               std::tie(b.origin_chain_id, b.vault_address, b.deposit_id);
    }
};

struct ProvenBridgeDeposit {
    uint64_t origin_chain_id{0};
    EthAddress vault_address{};
    DepositEvent event{};
};

struct BridgeMintAuthorization {
    modern::AssetId asset{};
    CAmount amount{0};
    CScript recipient_script{};
    BridgeDepositKey nullifier{};
};

enum class BridgeAdmissionResult {
    OK,
    CONFIGURATION_INCOMPLETE,
    REGISTRY_INACTIVE,
    ORIGIN_MISMATCH,
    VAULT_MISMATCH,
    TOKEN_MISMATCH,
    RECIPIENT_INVALID,
    AMOUNT_INVALID,
    BLOCK_CAP_EXCEEDED,
    EPOCH_CAP_EXCEEDED,
};

/**
 * Apply the frozen tuple-matching rules to an event whose receipt/finality
 * proof has already been verified. Exactly-once state and mint caps are
 * intentionally caller responsibilities until their consensus stores and
 * parameters are specified.
 */
inline BridgeAdmissionResult AdmitProvenDeposit(
    const BridgeAssetRegistryEntry& registry, const ProvenBridgeDeposit& deposit,
    const int32_t b3_height, BridgeMintAuthorization& out)
{
    out = {};
    if (!BridgeAssetRegistryEntryActiveAt(registry, b3_height)) {
        return BridgeAdmissionResult::REGISTRY_INACTIVE;
    }
    if (deposit.origin_chain_id != registry.origin_chain_id) {
        return BridgeAdmissionResult::ORIGIN_MISMATCH;
    }
    if (deposit.vault_address != registry.vault_address) {
        return BridgeAdmissionResult::VAULT_MISMATCH;
    }
    if (deposit.event.token != registry.token_address) {
        return BridgeAdmissionResult::TOKEN_MISMATCH;
    }
    const auto recipient{DecodeRecipientV1(deposit.event.b3_recipient)};
    if (!recipient) return BridgeAdmissionResult::RECIPIENT_INVALID;
    const auto amount{ConvertRawUnitsExact(deposit.event.amount, registry.origin_decimals,
                                           registry.asset_decimals)};
    if (!amount) return BridgeAdmissionResult::AMOUNT_INVALID;

    out.asset = registry.b3_asset_id;
    out.amount = *amount;
    out.recipient_script = RecipientV1Script(*recipient);
    out.nullifier = BridgeDepositKey{registry.origin_chain_id, registry.vault_address,
                                     deposit.event.deposit_id};
    return BridgeAdmissionResult::OK;
}

/**
 * Materialize the single chainparams-approved bUSD registry entry. Requiring
 * BridgeMintParamsReady here means a later consensus call site cannot turn a
 * partial mainnet identity pin into an ACTIVE registry by accident.
 */
inline std::optional<BridgeAssetRegistryEntry> ConfiguredBridgeRegistryEntry(
    const Consensus::Params& params)
{
    if (!params.busd_bridge || !Consensus::BridgeMintParamsReady(*params.busd_bridge)) {
        return std::nullopt;
    }
    const auto asset{modern::ConfiguredBridgeAssetId(params)};
    if (!asset) return std::nullopt;

    const Consensus::BridgeAssetParams& configured{*params.busd_bridge};
    BridgeAssetRegistryEntry entry;
    entry.origin_chain_id = configured.asset.origin_chain_id;
    entry.vault_address = configured.asset.vault_address;
    entry.token_address = configured.asset.token_address;
    entry.b3_asset_id = *asset;
    entry.origin_decimals = configured.asset.origin_decimals;
    entry.asset_decimals = configured.asset.asset_decimals;
    entry.implementation_or_adapter = *configured.implementation_or_adapter;
    entry.adapter_version = *configured.adapter_version;
    entry.approval_first_height = *configured.activation_height;
    entry.approval_last_height = configured.approval_last_height;
    entry.state = BridgeRegistryState::ACTIVE;
    return entry;
}

struct BridgeMintBudget {
    CAmount minted_this_block{0};
    CAmount minted_this_epoch{0};
};

/**
 * Parameter-gated admission including the mandatory per-block/per-epoch cap
 * check. Durable accounting, proof carriers and nullifier persistence remain
 * stage-4 chainstate responsibilities; callers provide the already-consumed
 * budget from that state.
 */
inline BridgeAdmissionResult AdmitConfiguredDeposit(
    const Consensus::Params& params, const ProvenBridgeDeposit& deposit,
    const int32_t b3_height, const BridgeMintBudget& used,
    BridgeMintAuthorization& out)
{
    out = {};
    const auto registry{ConfiguredBridgeRegistryEntry(params)};
    if (!registry) return BridgeAdmissionResult::CONFIGURATION_INCOMPLETE;

    const BridgeAdmissionResult admitted{
        AdmitProvenDeposit(*registry, deposit, b3_height, out)};
    if (admitted != BridgeAdmissionResult::OK) return admitted;

    const Consensus::BridgeMintCaps& caps{*params.busd_bridge->mint_caps};
    if (used.minted_this_block < 0 || used.minted_this_block > caps.max_per_block ||
        out.amount > caps.max_per_block - used.minted_this_block) {
        out = {};
        return BridgeAdmissionResult::BLOCK_CAP_EXCEEDED;
    }
    if (used.minted_this_epoch < 0 || used.minted_this_epoch > caps.max_per_epoch ||
        out.amount > caps.max_per_epoch - used.minted_this_epoch) {
        out = {};
        return BridgeAdmissionResult::EPOCH_CAP_EXCEEDED;
    }
    return BridgeAdmissionResult::OK;
}

/**
 * Atomic in-memory model of the ratified exactly-once rule. The production
 * chainstate index still needs durable serialization, block extraction,
 * reindex, and database undo wiring before activation.
 */
struct BridgeNullifierUndo {
    std::vector<BridgeDepositKey> inserted{};
};

class BridgeNullifierSet {
public:
    bool Contains(const BridgeDepositKey& key) const { return m_consumed.contains(key); }
    size_t Size() const { return m_consumed.size(); }

    bool ApplyBlock(std::span<const BridgeDepositKey> keys, BridgeNullifierUndo& undo)
    {
        undo = {};
        std::set<BridgeDepositKey> unique;
        for (const BridgeDepositKey& key : keys) {
            if (key.origin_chain_id == 0 || !unique.insert(key).second || Contains(key)) {
                return false;
            }
        }
        for (const BridgeDepositKey& key : keys) {
            m_consumed.insert(key);
            undo.inserted.push_back(key);
        }
        return true;
    }

    bool UndoBlock(const BridgeNullifierUndo& undo)
    {
        std::set<BridgeDepositKey> unique;
        for (const BridgeDepositKey& key : undo.inserted) {
            if (!unique.insert(key).second || !Contains(key)) return false;
        }
        for (auto it{undo.inserted.rbegin()}; it != undo.inserted.rend(); ++it) {
            m_consumed.erase(*it);
        }
        return true;
    }

private:
    std::set<BridgeDepositKey> m_consumed{};
};

} // namespace bridge

#endif // B3COIN_BRIDGE_ADMISSION_H
