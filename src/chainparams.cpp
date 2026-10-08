// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>

#include <bridge/proof.h>
#include <chainparamsbase.h>
#include <common/args.h>
#include <consensus/amount.h>
#include <consensus/bridge_params.h>
#include <consensus/params.h>
#include <crypto/sha256.h>
#include <deploymentinfo.h>
#include <logging.h>
#include <span.h>
#include <tinyformat.h>
#include <univalue.h>
#include <util/chaintype.h>
#include <util/fs.h>
#include <util/strencodings.h>
#include <util/string.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using util::SplitString;

namespace {

//! The only accepted -b3bridgepins schema.
constexpr std::string_view B3_BRIDGE_PINS_FORMAT{"b3-regtest-bridge-pins/1"};
//! Local anvil chain id; the synthetic regtest bUSD keeps this origin chain.
constexpr uint64_t B3_BRIDGE_PINS_CHAIN_ID{31'337};
//! First byte of every synthetic beacon fork version. No real Ethereum
//! network uses this namespace, so a pin file cannot borrow a real domain.
constexpr unsigned char B3_BRIDGE_PINS_FORK_NAMESPACE{0xb3};

[[noreturn]] void ThrowInvalidBridgePins(const std::string& reason)
{
    throw std::runtime_error(strprintf("Invalid -b3bridgepins file: %s", reason));
}

//! Require a JSON object holding every required key, at most the listed
//! optional ones, and no unknown or duplicate key.
void CheckBridgePinsKeys(const UniValue& object, const std::string& where,
                         std::initializer_list<std::string_view> required,
                         std::initializer_list<std::string_view> optional = {})
{
    if (!object.isObject()) {
        ThrowInvalidBridgePins(strprintf("%s must be a JSON object", where));
    }
    std::set<std::string, std::less<>> seen;
    for (const std::string& key : object.getKeys()) {
        if (!seen.insert(key).second) {
            ThrowInvalidBridgePins(strprintf("duplicate key %s.%s", where, key));
        }
        const std::string_view name{key};
        if (std::ranges::find(required, name) == required.end() &&
            std::ranges::find(optional, name) == optional.end()) {
            ThrowInvalidBridgePins(strprintf("unknown key %s.%s", where, key));
        }
    }
    for (const std::string_view key : required) {
        if (!seen.contains(key)) {
            ThrowInvalidBridgePins(strprintf("missing key %s.%s", where, key));
        }
    }
}

template <typename Int>
Int GetBridgePinsUInt(const UniValue& object, const std::string& where,
                      std::string_view key)
{
    static_assert(std::is_unsigned_v<Int>);
    const UniValue& value{object.find_value(key)};
    if (value.isNum()) {
        try {
            return value.getInt<Int>();
        } catch (const std::runtime_error&) {
        }
    }
    ThrowInvalidBridgePins(strprintf("%s.%s must be an unsigned integer of at most %u bits",
                                     where, key, std::numeric_limits<Int>::digits));
}

//! 0x-prefixed hex in raw Ethereum byte order (never uint256::FromHex's
//! reversed display order).
template <size_t N>
std::array<unsigned char, N> GetBridgePinsHex(const UniValue& object,
                                              const std::string& where,
                                              std::string_view key)
{
    const UniValue& value{object.find_value(key)};
    if (value.isStr()) {
        const std::string_view text{value.get_str()};
        if (text.size() == 2 + 2 * N && text.starts_with("0x") &&
            IsHex(text.substr(2))) {
            const auto bytes{TryParseHex<unsigned char>(text.substr(2))};
            if (bytes && bytes->size() == N) {
                std::array<unsigned char, N> out;
                std::ranges::copy(*bytes, out.begin());
                return out;
            }
        }
    }
    ThrowInvalidBridgePins(strprintf("%s.%s must be a 0x-prefixed %u-byte hex string",
                                     where, key, N));
}

uint256 GetBridgePinsHash(const UniValue& object, const std::string& where,
                          std::string_view key)
{
    const auto bytes{GetBridgePinsHex<32>(object, where, key)};
    const uint256 hash{std::span<const unsigned char>{bytes}};
    if (hash.IsNull()) {
        ThrowInvalidBridgePins(strprintf("%s.%s must be non-zero", where, key));
    }
    return hash;
}

CAmount GetBridgePinsAmount(const UniValue& object, const std::string& where,
                            std::string_view key)
{
    const uint64_t value{GetBridgePinsUInt<uint64_t>(object, where, key)};
    if (value > static_cast<uint64_t>(MAX_MONEY)) {
        ThrowInvalidBridgePins(strprintf("%s.%s exceeds MAX_MONEY", where, key));
    }
    return static_cast<CAmount>(value);
}

CChainParams::B3RegTestBridgePins ReadB3RegTestBridgePinsFile(const std::string& value)
{
    const fs::path path{fs::PathFromString(value)};
    if (value.empty() || !path.is_absolute()) {
        throw std::runtime_error(strprintf("-b3bridgepins requires an absolute path, not '%s'", value));
    }
    std::ifstream file{path.std_path(), std::ios::binary};
    if (!file.is_open()) {
        throw std::runtime_error(strprintf("Cannot open -b3bridgepins file %s", fs::quoted(fs::PathToString(path))));
    }
    std::string contents(MAX_B3_BRIDGE_PINS_FILE_SIZE + 1, '\0');
    file.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (file.bad()) {
        throw std::runtime_error(strprintf("Cannot read -b3bridgepins file %s", fs::quoted(fs::PathToString(path))));
    }
    contents.resize(static_cast<size_t>(file.gcount()));
    if (contents.size() > MAX_B3_BRIDGE_PINS_FILE_SIZE) {
        throw std::runtime_error(strprintf("-b3bridgepins file %s is larger than %u bytes",
                                           fs::quoted(fs::PathToString(path)), MAX_B3_BRIDGE_PINS_FILE_SIZE));
    }
    return ParseB3RegTestBridgePins(contents);
}

//! origin_chain_id must be the local anvil chain.
void CheckBridgePinsChainId(const UniValue& object, const std::string& where)
{
    const uint64_t chain_id{GetBridgePinsUInt<uint64_t>(object, where, "origin_chain_id")};
    if (chain_id != B3_BRIDGE_PINS_CHAIN_ID) {
        ThrowInvalidBridgePins(strprintf("%s.origin_chain_id must be %u (the local anvil chain), not %u",
                                         where, B3_BRIDGE_PINS_CHAIN_ID, chain_id));
    }
}

//! Non-zero, pairwise distinct vault, token and verifier addresses.
void CheckBridgePinsAddresses(const std::string& prefix,
                              const Consensus::BridgeEthAddress& vault,
                              const Consensus::BridgeEthAddress& token,
                              const Consensus::BridgeEthAddress& verifier)
{
    if (Consensus::BridgeAddressIsNull(vault) ||
        Consensus::BridgeAddressIsNull(token) ||
        Consensus::BridgeAddressIsNull(verifier)) {
        ThrowInvalidBridgePins(strprintf("%svault, token and verifier addresses must be non-zero", prefix));
    }
    if (vault == token || vault == verifier || token == verifier) {
        ThrowInvalidBridgePins(strprintf("%svault, token and verifier addresses must be pairwise distinct", prefix));
    }
}

std::optional<CAmount> GetOptionalBridgePinsAmount(const UniValue& object, const std::string& where,
                                                   std::string_view key)
{
    if (object.find_value(key).isNull()) return std::nullopt;
    return GetBridgePinsAmount(object, where, key);
}

// Regtest plan section 3.3 check 1 and node-changes section 3.4 assertion
// 3: a deposit of the vault's MAX_DEPOSIT_RAW must fit one block, or it
// would wait forever.
void CheckBridgePinsCaps(const std::string& label, CAmount max_deposit_raw,
                         CAmount block_cap, CAmount epoch_cap)
{
    if (max_deposit_raw <= 0 || max_deposit_raw > block_cap || block_cap > epoch_cap) {
        ThrowInvalidBridgePins(strprintf("%s must satisfy 0 < max_deposit_raw (%d) <= max_per_block (%d) <= max_per_epoch (%d) <= MAX_MONEY",
                                         label, max_deposit_raw, block_cap, epoch_cap));
    }
}

//! The light_client object of a section whose vault deploys at (or after)
//! deployment_block. Shared by v1_decentralized and v2.
Consensus::EthereumLightClientPins ParseBridgePinsLightClient(const UniValue& lc, const std::string& lc_where,
                                                              uint64_t deployment_block)
{
    CheckBridgePinsKeys(lc, lc_where,
                        {"trusted_checkpoint_root", "trusted_checkpoint_slot",
                         "checkpoint_execution_block", "genesis_validators_root",
                         "fork_schedule", "fork_schedule_valid_through_epoch",
                         "electra_epoch"},
                        {"min_sync_committee_participants", "max_sync_lag_slots"});
    Consensus::EthereumLightClientPins light;
    light.trusted_checkpoint_root = GetBridgePinsHash(lc, lc_where, "trusted_checkpoint_root");
    light.trusted_checkpoint_slot = GetBridgePinsUInt<uint64_t>(lc, lc_where, "trusted_checkpoint_slot");

    // Regtest plan section 3.3 check 4: the bootstrap's execution block must
    // reach the vault deployment block within the cumulative backfill bound,
    // as the bootstrap record is judged in consensus.
    const uint64_t checkpoint_block{GetBridgePinsUInt<uint64_t>(lc, lc_where, "checkpoint_execution_block")};
    if (checkpoint_block < deployment_block ||
        checkpoint_block - deployment_block > bridge::MAX_BRIDGE_CUMULATIVE_BACKFILL_BLOCKS) {
        ThrowInvalidBridgePins(strprintf("%s.checkpoint_execution_block (%u) must lie within %u blocks at or after origin_deployment_block (%u)",
                                         lc_where, checkpoint_block, bridge::MAX_BRIDGE_CUMULATIVE_BACKFILL_BLOCKS,
                                         deployment_block));
    }

    light.genesis_validators_root = GetBridgePinsHash(lc, lc_where, "genesis_validators_root");
    if (light.genesis_validators_root == Consensus::ETHEREUM_MAINNET_GENESIS_VALIDATORS_ROOT) {
        ThrowInvalidBridgePins(strprintf("%s.genesis_validators_root must not be the Ethereum mainnet root", lc_where));
    }
    const UniValue& forks{lc.find_value("fork_schedule")};
    if (!forks.isArray() || forks.empty()) {
        ThrowInvalidBridgePins(strprintf("%s.fork_schedule must be a non-empty array", lc_where));
    }
    for (size_t i{0}; i < forks.size(); ++i) {
        const std::string fork_where{strprintf("%s.fork_schedule[%u]", lc_where, i)};
        CheckBridgePinsKeys(forks[i], fork_where, {"activation_epoch", "fork_version"});
        Consensus::EthereumForkVersionPin fork;
        fork.activation_epoch = GetBridgePinsUInt<uint64_t>(forks[i], fork_where, "activation_epoch");
        fork.fork_version = GetBridgePinsHex<4>(forks[i], fork_where, "fork_version");
        if (fork.fork_version[0] != B3_BRIDGE_PINS_FORK_NAMESPACE) {
            ThrowInvalidBridgePins(strprintf("%s.fork_version must use the synthetic 0xb3 namespace, not 0x%s",
                                             fork_where, HexStr(fork.fork_version)));
        }
        light.fork_schedule.push_back(fork);
    }
    light.fork_schedule_valid_through_epoch =
        GetBridgePinsUInt<uint64_t>(lc, lc_where, "fork_schedule_valid_through_epoch");
    // The synthetic beacon (b3-bridge-regtest-beacon) builds Electra objects
    // from genesis and refuses any other value, so require it here instead
    // of failing at the first bootstrap.
    light.electra_epoch = GetBridgePinsUInt<uint64_t>(lc, lc_where, "electra_epoch");
    if (light.electra_epoch != 0) {
        ThrowInvalidBridgePins(strprintf("%s.electra_epoch must be 0 (the synthetic beacon uses Electra from genesis)",
                                         lc_where));
    }
    light.min_sync_committee_participants = Consensus::ETHEREUM_SYNC_COMMITTEE_SUPERMAJORITY;
    if (!lc.find_value("min_sync_committee_participants").isNull()) {
        light.min_sync_committee_participants =
            GetBridgePinsUInt<unsigned>(lc, lc_where, "min_sync_committee_participants");
    }
    if (light.min_sync_committee_participants < Consensus::ETHEREUM_SYNC_COMMITTEE_SUPERMAJORITY ||
        light.min_sync_committee_participants > Consensus::ETHEREUM_SYNC_COMMITTEE_SIZE) {
        ThrowInvalidBridgePins(strprintf("%s.min_sync_committee_participants must be between %u and %u",
                                         lc_where, Consensus::ETHEREUM_SYNC_COMMITTEE_SUPERMAJORITY,
                                         Consensus::ETHEREUM_SYNC_COMMITTEE_SIZE));
    }
    light.max_sync_lag_slots = 8'192;
    if (!lc.find_value("max_sync_lag_slots").isNull()) {
        light.max_sync_lag_slots = GetBridgePinsUInt<uint64_t>(lc, lc_where, "max_sync_lag_slots");
    }
    if (!light.Valid()) {
        ThrowInvalidBridgePins(strprintf("%s pins are inconsistent (fork schedule must start at epoch 0 and increase, "
                                         "and cover the checkpoint epoch and electra_epoch; max_sync_lag_slots must be non-zero)",
                                         lc_where));
    }
    return light;
}

//! The v1_decentralized section (regtest plan section 3.3).
void ParseBridgePinsV1(const UniValue& v1, CChainParams::B3RegTestBridgePins& pins)
{
    const std::string v1_where{"v1_decentralized"};
    CheckBridgePinsKeys(v1, v1_where,
                        {"origin_chain_id", "vault", "token", "origin_deployment_block",
                         "vault_runtime_code_hash", "token_runtime_code_hash",
                         "max_deposit_raw", "light_client", "verifier",
                         "verifier_runtime_code_hash", "bootstrap_validator_set_hash",
                         "min_bridge_validators", "max_bridge_validators",
                         "min_bridge_total_weight", "max_epoch_lag"},
                        {"max_per_block", "max_per_epoch"});

    CheckBridgePinsChainId(v1, v1_where);

    pins.vault = GetBridgePinsHex<20>(v1, v1_where, "vault");
    pins.token = GetBridgePinsHex<20>(v1, v1_where, "token");
    const Consensus::BridgeEthAddress verifier{GetBridgePinsHex<20>(v1, v1_where, "verifier")};
    CheckBridgePinsAddresses("", pins.vault, pins.token, verifier);

    pins.origin_deployment_block = GetBridgePinsUInt<uint64_t>(v1, v1_where, "origin_deployment_block");
    if (pins.origin_deployment_block < 1) {
        ThrowInvalidBridgePins(strprintf("%s.origin_deployment_block must be at least 1", v1_where));
    }
    pins.vault_runtime_code_hash = GetBridgePinsHash(v1, v1_where, "vault_runtime_code_hash");
    pins.token_runtime_code_hash = GetBridgePinsHash(v1, v1_where, "token_runtime_code_hash");

    const CAmount max_deposit_raw{GetBridgePinsAmount(v1, v1_where, "max_deposit_raw")};
    pins.max_per_block = GetOptionalBridgePinsAmount(v1, v1_where, "max_per_block");
    pins.max_per_epoch = GetOptionalBridgePinsAmount(v1, v1_where, "max_per_epoch");
    CheckBridgePinsCaps("caps", max_deposit_raw,
                        pins.max_per_block.value_or(CChainParams::B3RegTestBridgePins::DEFAULT_MAX_PER_BLOCK),
                        pins.max_per_epoch.value_or(CChainParams::B3RegTestBridgePins::DEFAULT_MAX_PER_EPOCH));

    pins.light_client = ParseBridgePinsLightClient(v1.find_value("light_client"), v1_where + ".light_client",
                                                   pins.origin_deployment_block);

    Consensus::BridgeDecentralizedWithdrawalPins& withdrawal{pins.withdrawal};
    withdrawal.ethereum_verifier_address = verifier;
    withdrawal.ethereum_verifier_code_hash = GetBridgePinsHash(v1, v1_where, "verifier_runtime_code_hash");
    withdrawal.bootstrap_validator_set_hash = GetBridgePinsHash(v1, v1_where, "bootstrap_validator_set_hash");
    withdrawal.withdrawal_rules_version = Consensus::DECENTRALIZED_WITHDRAWAL_RULES_VERSION_V1;
    withdrawal.withdrawal_rules_commitment = Consensus::DECENTRALIZED_WITHDRAWAL_RULES_COMMITMENT_V1;
    withdrawal.min_bridge_validators = GetBridgePinsUInt<uint32_t>(v1, v1_where, "min_bridge_validators");
    withdrawal.max_bridge_validators = GetBridgePinsUInt<uint32_t>(v1, v1_where, "max_bridge_validators");
    withdrawal.min_bridge_total_weight = GetBridgePinsUInt<uint64_t>(v1, v1_where, "min_bridge_total_weight");
    withdrawal.max_epoch_lag = GetBridgePinsUInt<uint32_t>(v1, v1_where, "max_epoch_lag");
    if (!withdrawal.Valid()) {
        ThrowInvalidBridgePins(strprintf("%s withdrawal pins need %u <= min_bridge_validators <= max_bridge_validators <= %u, "
                                         "min_bridge_total_weight > 0 and max_epoch_lag > 0",
                                         v1_where, Consensus::MIN_BRIDGE_VALIDATORS_V1,
                                         Consensus::MAX_PROVEN_BRIDGE_VALIDATORS_V1));
    }
}

/**
 * The optional `v2.successor` object (B-23; node changes section 7.6): the
 * successor vault v' a regtest migration drill deploys, pinned before the
 * validators first start exactly as v is (OI-3), so the init.cpp record
 * guard freezes it with the rest of the file. Keys are v' itself (its CREATE
 * address), its predecessor (must be v2.vault), the bUSD V2 AssetId it
 * carries (raw EVM bytes; RegTest() requires v's recomputed id) and h_m,
 * plus an optional deployment-block lower bound (default v's, which is safe:
 * v' can only be deployed once v holds code). Everything else is inherited
 * from v, so naming the token, chain, caps or a vault code hash (OI-3) is an
 * unknown key.
 */
CChainParams::B3RegTestBridgePins::V2Section::SuccessorSection ParseBridgePinsV2Successor(
    const UniValue& successor, const CChainParams::B3RegTestBridgePins::V2Section& v2)
{
    const std::string where{"v2.successor"};
    CheckBridgePinsKeys(successor, where, {"vault", "predecessor", "asset_id", "migration_height"},
                        {"origin_deployment_block"});

    CChainParams::B3RegTestBridgePins::V2Section::SuccessorSection section;
    section.vault = GetBridgePinsHex<20>(successor, where, "vault");
    section.predecessor = GetBridgePinsHex<20>(successor, where, "predecessor");
    if (Consensus::BridgeAddressIsNull(section.vault)) {
        ThrowInvalidBridgePins(strprintf("%s.vault must be non-zero", where));
    }
    if (Consensus::BridgeAddressIsNull(section.predecessor)) {
        ThrowInvalidBridgePins(strprintf("%s.predecessor must be non-zero", where));
    }
    if (section.vault == v2.vault || section.vault == v2.token || section.vault == v2.verifier) {
        ThrowInvalidBridgePins(strprintf("%s.vault must differ from v2.vault, v2.token and v2.verifier", where));
    }
    if (section.predecessor != v2.vault) {
        ThrowInvalidBridgePins(strprintf("%s.predecessor must equal v2.vault (the launch vault it retires)", where));
    }
    section.asset_id = GetBridgePinsHash(successor, where, "asset_id");

    // h_m > V and h_m >= W2: the migration retires an entry that has been
    // minting and burning. RegTest() checks nothing further (V >= A3, R_f).
    constexpr uint32_t max_height{static_cast<uint32_t>(std::numeric_limits<int>::max())};
    const uint32_t migration_height{GetBridgePinsUInt<uint32_t>(successor, where, "migration_height")};
    const uint32_t activation_height{static_cast<uint32_t>(v2.activation_height)};
    const uint32_t burn_height{static_cast<uint32_t>(v2.burn_height.value_or(v2.activation_height))};
    if (migration_height <= activation_height || migration_height < burn_height ||
        migration_height > max_height) {
        ThrowInvalidBridgePins(strprintf("%s.migration_height (%u) must be above activation_height (%u), "
                                         "at or above burn_height (%u) and at most %u",
                                         where, migration_height, activation_height, burn_height, max_height));
    }
    section.migration_height = static_cast<int>(migration_height);

    if (!successor.find_value("origin_deployment_block").isNull()) {
        const uint64_t deployment_block{GetBridgePinsUInt<uint64_t>(successor, where, "origin_deployment_block")};
        if (deployment_block < 1) {
            ThrowInvalidBridgePins(strprintf("%s.origin_deployment_block must be at least 1", where));
        }
        section.origin_deployment_block = deployment_block;
    }
    return section;
}

/**
 * A forwarder factory address (`v2.deposit_forwarder.factory` or
 * `v2.exit_forwarder.factory`): non-zero and a contract of its own, never
 * v2.vault, v2.token, v2.verifier or a pinned successor vault.
 */
Consensus::BridgeEthAddress GetBridgePinsForwarderFactory(
    const UniValue& object, const std::string& where,
    const CChainParams::B3RegTestBridgePins::V2Section& v2)
{
    const Consensus::BridgeEthAddress factory{GetBridgePinsHex<20>(object, where, "factory")};
    if (Consensus::BridgeAddressIsNull(factory)) {
        ThrowInvalidBridgePins(strprintf("%s.factory must be non-zero", where));
    }
    if (factory == v2.vault || factory == v2.token || factory == v2.verifier ||
        (v2.successor && factory == v2.successor->vault)) {
        ThrowInvalidBridgePins(strprintf("%s.factory must differ from v2.vault, v2.token, v2.verifier "
                                         "and v2.successor.vault", where));
    }
    return factory;
}

/**
 * The optional `v2.deposit_forwarder` object (route (b); node changes
 * section 3.1, contracts spec section 7): the B3DepositForwarderFactory
 * address and its FORWARDER_INIT_CODE_HASH, raw EVM bytes as
 * token_runtime_code_hash. Both are DeployV2 outputs knowable before the
 * first start (the factory is the deployer's CREATE address at nonce n+4,
 * the hash comes from the forge artifact), so the init.cpp record guard
 * freezes them with v (OI-3). Derivation only, never a mint condition.
 */
CChainParams::B3RegTestBridgePins::V2Section::DepositForwarderSection ParseBridgePinsV2DepositForwarder(
    const UniValue& object, const CChainParams::B3RegTestBridgePins::V2Section& v2)
{
    const std::string where{"v2.deposit_forwarder"};
    CheckBridgePinsKeys(object, where, {"factory", "init_code_hash"});
    CChainParams::B3RegTestBridgePins::V2Section::DepositForwarderSection section;
    section.factory = GetBridgePinsForwarderFactory(object, where, v2);
    section.init_code_hash = GetBridgePinsHash(object, where, "init_code_hash");
    return section;
}

/**
 * The optional `v2.exit_forwarder` object (route (d3); node changes
 * sections 3.1 and 7.2, contracts spec section 8): the B3OftExitFactory
 * address (deployer nonce n+5), its FORWARDER_INIT_CODE_HASH (raw EVM bytes)
 * and its immutable DST_EID, the LayerZero endpoint id every exit block must
 * name. The EXIT_TAG is a protocol constant, not a key.
 */
CChainParams::B3RegTestBridgePins::V2Section::ExitForwarderSection ParseBridgePinsV2ExitForwarder(
    const UniValue& object, const CChainParams::B3RegTestBridgePins::V2Section& v2)
{
    const std::string where{"v2.exit_forwarder"};
    CheckBridgePinsKeys(object, where, {"factory", "init_code_hash", "dst_eid"});
    CChainParams::B3RegTestBridgePins::V2Section::ExitForwarderSection section;
    section.factory = GetBridgePinsForwarderFactory(object, where, v2);
    section.init_code_hash = GetBridgePinsHash(object, where, "init_code_hash");
    section.dst_eid = GetBridgePinsUInt<uint32_t>(object, where, "dst_eid");
    if (section.dst_eid == 0) {
        ThrowInvalidBridgePins(strprintf("%s.dst_eid must be non-zero", where));
    }
    return section;
}

/**
 * The bridge V2 section (node changes sections 3.1-3.3 and 12.2): one anvil
 * Vault V2 entry, its fresh light-client store and the Verifier V2 member
 * bounds, plus V and W2. OI-3: only values knowable before the chain first
 * stores a block at or above A3 are keys. The vault and verifier runtime
 * code hashes and the lineage id commit to blockHash(R_f - 1), so naming
 * them is refused as an unknown key. The verifier's MAX_EPOCH_LAG is a
 * deployment parameter fixed in advance and may be named
 * (verifier_max_epoch_lag); RegTest() requires it to equal the gate's set
 * expiry. An optional `successor` object pins a B-23 successor vault
 * (ParseBridgePinsV2Successor); optional `deposit_forwarder` and
 * `exit_forwarder` objects pin the route (b) and (d3) forwarder factories
 * (ParseBridgePinsV2DepositForwarder, ParseBridgePinsV2ExitForwarder).
 */
CChainParams::B3RegTestBridgePins::V2Section ParseBridgePinsV2(const UniValue& v2)
{
    const std::string v2_where{"v2"};
    CheckBridgePinsKeys(v2, v2_where,
                        {"origin_chain_id", "vault", "token", "token_runtime_code_hash",
                         "origin_deployment_block", "max_deposit_raw", "light_client",
                         "verifier", "min_bridge_validators", "max_bridge_validators",
                         "min_bridge_total_weight", "activation_height"},
                        {"burn_height", "max_per_block", "max_per_epoch", "verifier_max_epoch_lag",
                         "successor", "deposit_forwarder", "exit_forwarder"});

    CChainParams::B3RegTestBridgePins::V2Section section;
    CheckBridgePinsChainId(v2, v2_where);
    section.origin_chain_id = B3_BRIDGE_PINS_CHAIN_ID;

    section.vault = GetBridgePinsHex<20>(v2, v2_where, "vault");
    section.token = GetBridgePinsHex<20>(v2, v2_where, "token");
    section.verifier = GetBridgePinsHex<20>(v2, v2_where, "verifier");
    CheckBridgePinsAddresses(v2_where + " ", section.vault, section.token, section.verifier);

    section.origin_deployment_block = GetBridgePinsUInt<uint64_t>(v2, v2_where, "origin_deployment_block");
    if (section.origin_deployment_block < 1) {
        ThrowInvalidBridgePins(strprintf("%s.origin_deployment_block must be at least 1", v2_where));
    }
    section.token_runtime_code_hash = GetBridgePinsHash(v2, v2_where, "token_runtime_code_hash");

    section.max_deposit_raw = GetBridgePinsAmount(v2, v2_where, "max_deposit_raw");
    section.max_per_block = GetOptionalBridgePinsAmount(v2, v2_where, "max_per_block");
    section.max_per_epoch = GetOptionalBridgePinsAmount(v2, v2_where, "max_per_epoch");
    CheckBridgePinsCaps(v2_where + " caps", section.max_deposit_raw,
                        section.max_per_block.value_or(CChainParams::B3RegTestBridgePins::DEFAULT_V2_MAX_PER_BLOCK),
                        section.max_per_epoch.value_or(CChainParams::B3RegTestBridgePins::DEFAULT_V2_MAX_PER_EPOCH));

    section.light_client = ParseBridgePinsLightClient(v2.find_value("light_client"), v2_where + ".light_client",
                                                      section.origin_deployment_block);

    section.min_bridge_validators = GetBridgePinsUInt<uint32_t>(v2, v2_where, "min_bridge_validators");
    section.max_bridge_validators = GetBridgePinsUInt<uint32_t>(v2, v2_where, "max_bridge_validators");
    section.min_bridge_total_weight = GetBridgePinsUInt<uint64_t>(v2, v2_where, "min_bridge_total_weight");
    if (section.min_bridge_validators < Consensus::MIN_BRIDGE_VALIDATORS_V1 ||
        section.max_bridge_validators < section.min_bridge_validators ||
        section.max_bridge_validators > Consensus::MAX_PROVEN_BRIDGE_VALIDATORS_V1 ||
        section.min_bridge_total_weight == 0) {
        ThrowInvalidBridgePins(strprintf("%s verifier pins need %u <= min_bridge_validators <= max_bridge_validators <= %u "
                                         "and min_bridge_total_weight > 0",
                                         v2_where, Consensus::MIN_BRIDGE_VALIDATORS_V1,
                                         Consensus::MAX_PROVEN_BRIDGE_VALIDATORS_V1));
    }

    // V and W2 are B3 heights. RegTest() checks them against A3 and R_f.
    constexpr uint32_t max_height{static_cast<uint32_t>(std::numeric_limits<int>::max())};
    const uint32_t activation_height{GetBridgePinsUInt<uint32_t>(v2, v2_where, "activation_height")};
    if (activation_height < 1 || activation_height > max_height) {
        ThrowInvalidBridgePins(strprintf("%s.activation_height must be between 1 and %u", v2_where, max_height));
    }
    section.activation_height = static_cast<int>(activation_height);
    if (!v2.find_value("burn_height").isNull()) {
        const uint32_t burn_height{GetBridgePinsUInt<uint32_t>(v2, v2_where, "burn_height")};
        if (burn_height < activation_height || burn_height > max_height) {
            ThrowInvalidBridgePins(strprintf("%s.burn_height (%u) must be between activation_height (%u) and %u",
                                             v2_where, burn_height, activation_height, max_height));
        }
        section.burn_height = static_cast<int>(burn_height);
    }
    if (!v2.find_value("verifier_max_epoch_lag").isNull()) {
        const uint32_t lag{GetBridgePinsUInt<uint32_t>(v2, v2_where, "verifier_max_epoch_lag")};
        if (lag == 0) ThrowInvalidBridgePins(strprintf("%s.verifier_max_epoch_lag must be positive", v2_where));
        section.verifier_max_epoch_lag = int64_t{lag};
    }
    // A present successor key must be an object; JSON null is not "absent".
    if (v2.exists("successor")) {
        section.successor = ParseBridgePinsV2Successor(v2.find_value("successor"), section);
    }
    // Forwarder factories, after the successor so that neither may sit on
    // v'; present keys must be objects as well.
    if (v2.exists("deposit_forwarder")) {
        section.deposit_forwarder = ParseBridgePinsV2DepositForwarder(v2.find_value("deposit_forwarder"), section);
    }
    if (v2.exists("exit_forwarder")) {
        section.exit_forwarder = ParseBridgePinsV2ExitForwarder(v2.find_value("exit_forwarder"), section);
    }
    if (section.deposit_forwarder && section.exit_forwarder &&
        section.deposit_forwarder->factory == section.exit_forwarder->factory) {
        ThrowInvalidBridgePins("v2.deposit_forwarder.factory and v2.exit_forwarder.factory must differ");
    }
    return section;
}

} // namespace

CChainParams::B3RegTestBridgePins ParseB3RegTestBridgePins(std::string_view json)
{
    UniValue root;
    if (!root.read(json)) ThrowInvalidBridgePins("not a valid JSON document");
    CheckBridgePinsKeys(root, "file", {"format"}, {"v1_decentralized", "v2"});
    const bool has_v1{root.exists("v1_decentralized")};
    const bool has_v2{root.exists("v2")};
    if (!has_v1 && !has_v2) ThrowInvalidBridgePins("file needs v1_decentralized or v2");
    const UniValue& format{root.find_value("format")};
    if (!format.isStr() || format.get_str() != B3_BRIDGE_PINS_FORMAT) {
        ThrowInvalidBridgePins(strprintf("format must be \"%s\"", B3_BRIDGE_PINS_FORMAT));
    }

    CChainParams::B3RegTestBridgePins pins;
    pins.has_v1 = has_v1;
    if (has_v1) ParseBridgePinsV1(root.find_value("v1_decentralized"), pins);
    if (has_v2) pins.v2 = ParseBridgePinsV2(root.find_value("v2"));

    CSHA256().Write(UCharCast(json.data()), json.size()).Finalize(pins.file_sha256.begin());
    return pins;
}

void ReadSigNetArgs(const ArgsManager& args, CChainParams::SigNetOptions& options)
{
    if (!args.GetArgs("-signetseednode").empty()) {
        options.seeds.emplace(args.GetArgs("-signetseednode"));
    }
    if (!args.GetArgs("-signetchallenge").empty()) {
        const auto signet_challenge = args.GetArgs("-signetchallenge");
        if (signet_challenge.size() != 1) {
            throw std::runtime_error("-signetchallenge cannot be multiple values.");
        }
        const auto val{TryParseHex<uint8_t>(signet_challenge[0])};
        if (!val) {
            throw std::runtime_error(strprintf("-signetchallenge must be hex, not '%s'.", signet_challenge[0]));
        }
        options.challenge.emplace(*val);
    }
}

void ReadRegTestArgs(const ArgsManager& args, CChainParams::RegTestOptions& options)
{
    if (auto value = args.GetBoolArg("-fastprune")) options.fastprune = *value;
    if (args.IsArgSet("-b3bridgepins") &&
        !(args.GetBoolArg("-b3modernregtest", false) &&
          args.GetBoolArg("-b3flowmeshtest", false))) {
        throw std::runtime_error("-b3bridgepins requires -b3modernregtest and -b3flowmeshtest (regtest-only)");
    }
    // Fresh finality lineage knobs (bridge V2 regtest plan section 3.3):
    // like -b3bridgepins they only ever apply to the FlowMesh regtest.
    for (const char* knob : {"-b3freshlineageheight", "-b3freshmaxepochextension", "-b3freshkeyheight"}) {
        if (args.IsArgSet(knob) &&
            !(args.GetBoolArg("-b3modernregtest", false) &&
              args.GetBoolArg("-b3flowmeshtest", false))) {
            throw std::runtime_error(strprintf("%s requires -b3modernregtest and -b3flowmeshtest (regtest-only)", knob));
        }
    }
    if (args.GetBoolArg("-b3flowmeshtest", false) &&
        !args.GetBoolArg("-b3modernregtest", false)) {
        throw std::runtime_error("-b3flowmeshtest requires -b3modernregtest");
    }
    // B3 modern-era regtest (contract section 64 activation overrides;
    // REGTEST ONLY). -b3modernregtest enables the pinned H=0/X=genesis
    // configuration with scaled scaffolding constants; the numeric knobs
    // refine it and are ignored without the switch.
    if (args.GetBoolArg("-b3modernregtest", false)) {
        CChainParams::B3ModernRegTestOptions b3{};
        b3.flowmesh_test = args.GetBoolArg("-b3flowmeshtest", false);
        const auto read_int{[&](const char* name, auto& field) {
            if (args.IsArgSet(name)) {
                const int64_t v{args.GetIntArg(name, field)};
                if (v <= 0) throw std::runtime_error(strprintf("Invalid value for %s: must be positive", name));
                field = static_cast<std::decay_t<decltype(field)>>(v);
            }
        }};
        read_int("-b3corridorlength", b3.corridor_length);
        read_int("-b3corridorspacing", b3.corridor_spacing);
        read_int("-b3corridorreward", b3.corridor_reward);
        read_int("-b3blockinterval", b3.block_interval);
        read_int("-b3roundseconds", b3.round_seconds);
        read_int("-b3epochlength", b3.epoch_length);
        read_int("-b3checkpointinterval", b3.checkpoint_interval);
        read_int("-b3checkpointdepth", b3.checkpoint_depth);
        read_int("-b3maxepochextension", b3.max_epoch_extension);
        read_int("-b3minfinalityset", b3.min_finality_set);
        read_int("-b3reorghorizon", b3.reorg_horizon);
        read_int("-b3minstake", b3.min_stake_amount);
        if (args.IsArgSet("-b3modernreward")) read_int("-b3modernreward", b3.modern_reward);
        if (args.IsArgSet("-b3halvinginterval")) read_int("-b3halvinginterval", b3.halving_interval);
        if (args.IsArgSet("-b3treasurypercent")) read_int("-b3treasurypercent", b3.treasury_percent);
        if (const auto hexscript{args.GetArg("-b3treasuryscript")}) {
            const auto bytes{TryParseHex<unsigned char>(*hexscript)};
            if (!bytes || bytes->empty()) throw std::runtime_error("Invalid -b3treasuryscript hex");
            b3.treasury_script = *bytes;
        }
        // External V1 decentralized-bridge pins for a local anvil deployment
        // (regtest plan section 3.3); only reachable with -b3flowmeshtest.
        if (args.IsArgSet("-b3bridgepins")) {
            b3.bridge_pins = ReadB3RegTestBridgePinsFile(args.GetArg("-b3bridgepins", ""));
        }
        // Fresh finality lineage at R_f (B-8) with the B-7 staging signer;
        // RegTest() enforces the grid and the flowmesh requirement.
        const auto read_fresh{[&](const char* name, int64_t min_value, std::optional<int>& field) {
            if (args.IsArgSet(name)) {
                const int64_t v{args.GetIntArg(name, 0)};
                if (v < min_value || v > std::numeric_limits<int>::max()) {
                    throw std::runtime_error(strprintf("Invalid value for %s: must be in [%d, %d]",
                                                       name, min_value, std::numeric_limits<int>::max()));
                }
                field = static_cast<int>(v);
            }
        }};
        read_fresh("-b3freshlineageheight", 2, b3.fresh_lineage_height);
        read_fresh("-b3freshmaxepochextension", 1, b3.fresh_max_epoch_extension);
        // B-37 per-epoch keys from R_key; RegTest() bounds it by R_f.
        read_fresh("-b3freshkeyheight", 1, b3.fresh_key_height);
        options.b3_modern = b3;
    }
    if (HasTestOption(args, "bip94")) options.enforce_bip94 = true;

    for (const std::string& arg : args.GetArgs("-testactivationheight")) {
        const auto found{arg.find('@')};
        if (found == std::string::npos) {
            throw std::runtime_error(strprintf("Invalid format (%s) for -testactivationheight=name@height.", arg));
        }

        const auto value{arg.substr(found + 1)};
        const auto height{ToIntegral<int32_t>(value)};
        if (!height || *height < 0 || *height >= std::numeric_limits<int>::max()) {
            throw std::runtime_error(strprintf("Invalid height value (%s) for -testactivationheight=name@height.", arg));
        }

        const auto deployment_name{arg.substr(0, found)};
        if (const auto buried_deployment = GetBuriedDeployment(deployment_name)) {
            options.activation_heights[*buried_deployment] = *height;
        } else {
            throw std::runtime_error(strprintf("Invalid name (%s) for -testactivationheight=name@height.", arg));
        }
    }

    for (const std::string& strDeployment : args.GetArgs("-vbparams")) {
        std::vector<std::string> vDeploymentParams = SplitString(strDeployment, ':');
        if (vDeploymentParams.size() < 3 || 4 < vDeploymentParams.size()) {
            throw std::runtime_error("Version bits parameters malformed, expecting deployment:start:end[:min_activation_height]");
        }
        CChainParams::VersionBitsParameters vbparams{};
        const auto start_time{ToIntegral<int64_t>(vDeploymentParams[1])};
        if (!start_time) {
            throw std::runtime_error(strprintf("Invalid nStartTime (%s)", vDeploymentParams[1]));
        }
        vbparams.start_time = *start_time;
        const auto timeout{ToIntegral<int64_t>(vDeploymentParams[2])};
        if (!timeout) {
            throw std::runtime_error(strprintf("Invalid nTimeout (%s)", vDeploymentParams[2]));
        }
        vbparams.timeout = *timeout;
        if (vDeploymentParams.size() >= 4) {
            const auto min_activation_height{ToIntegral<int64_t>(vDeploymentParams[3])};
            if (!min_activation_height) {
                throw std::runtime_error(strprintf("Invalid min_activation_height (%s)", vDeploymentParams[3]));
            }
            vbparams.min_activation_height = *min_activation_height;
        } else {
            vbparams.min_activation_height = 0;
        }
        bool found = false;
        for (int j=0; j < (int)Consensus::MAX_VERSION_BITS_DEPLOYMENTS; ++j) {
            if (vDeploymentParams[0] == VersionBitsDeploymentInfo[j].name) {
                options.version_bits_parameters[Consensus::DeploymentPos(j)] = vbparams;
                found = true;
                LogInfo("Setting version bits activation parameters for %s to start=%ld, timeout=%ld, min_activation_height=%d",
                        vDeploymentParams[0], vbparams.start_time, vbparams.timeout, vbparams.min_activation_height);
                break;
            }
        }
        if (!found) {
            throw std::runtime_error(strprintf("Invalid deployment (%s)", vDeploymentParams[0]));
        }
    }
}

static std::unique_ptr<const CChainParams> globalChainParams;

const CChainParams &Params() {
    assert(globalChainParams);
    return *globalChainParams;
}

std::unique_ptr<const CChainParams> CreateChainParams(const ArgsManager& args, const ChainType chain)
{
    // The bridge pin file only ever re-points the synthetic regtest bridge.
    // Fail closed elsewhere: a main or test-network config file naming it is
    // a startup error (before this knob existed it was ignored as unknown).
    if (chain != ChainType::REGTEST && args.IsArgSet("-b3bridgepins")) {
        throw std::runtime_error("-b3bridgepins is regtest-only");
    }
    // The fresh finality lineage knobs likewise configure the regtest
    // staging lineage only; every other network leaves fresh_lineage unset.
    for (const char* knob : {"-b3freshlineageheight", "-b3freshmaxepochextension", "-b3freshkeyheight"}) {
        if (chain != ChainType::REGTEST && args.IsArgSet(knob)) {
            throw std::runtime_error(strprintf("%s is regtest-only", knob));
        }
    }
    switch (chain) {
    case ChainType::MAIN:
        return CChainParams::Main();
    case ChainType::TESTNET:
        return CChainParams::TestNet();
    case ChainType::TESTNET4:
        return CChainParams::TestNet4();
    case ChainType::SIGNET: {
        auto opts = CChainParams::SigNetOptions{};
        ReadSigNetArgs(args, opts);
        return CChainParams::SigNet(opts);
    }
    case ChainType::REGTEST: {
        auto opts = CChainParams::RegTestOptions{};
        ReadRegTestArgs(args, opts);
        return CChainParams::RegTest(opts);
    }
    }
    assert(false);
}

void SelectParams(const ChainType chain)
{
    SelectBaseParams(chain);
    globalChainParams = CreateChainParams(gArgs, chain);
}
