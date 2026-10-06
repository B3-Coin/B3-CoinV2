// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_BRIDGE_STATE_OBSERVATION_H
#define B3COIN_BRIDGE_STATE_OBSERVATION_H

#include <bridge/deposit.h>
#include <bridge/proof.h>
#include <bridge/state_proof.h>
#include <consensus/amount.h>
#include <consensus/bridge_params.h>
#include <uint256.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

/**
 * Kind 9 ETH_STATE_OBSERVATION decoding (doc/design/b3-bridge-v2-node-changes.md
 * section 8.2): which Ethereum accounts and storage slots each subject reads
 * (the eth_getProof plan), the EIP-1186 verification of a record against one
 * execution anchor's state root, and the strict decoding of the proven words
 * into the values the burn gate (section 8.4) and the reserve ledger
 * (sections 7.1-7.2, 7.6) use.
 *
 * Pure: no chain state, no I/O. Which anchor a record may name, freshness,
 * monotone anchors and the VERIFIER_E verdict are the bridge index's rules.
 *
 * Storage layouts are normative and shared with the contracts:
 * - Verifier V2: contracts spec section 3.2 (Liveness slot 0, set hashes 1-2,
 *   Latest slot 4, latestCertId slot 6).
 * - Vault V2: contracts spec section 6.6 (nextDepositId slot 0, locked slot
 *   1, absorbedTotal slot 3).
 * - Tether (mainnet USDT and contracts/regtest/MockTetherToken.sol): slot 0
 *   owner and paused, slot 2 balances, slot 6 isBlackListed, slot 10
 *   upgradedAddress and deprecated.
 * contracts/test/v2/StorageLayoutV2.t.sol checks every slot and bit offset
 * against the deployed contracts and the shared vector file
 * contracts/test/vectors/bridge_v2_vectors.json ("storage_layout_v2"), whose
 * values bridge_state_observation_tests copies.
 */
namespace bridge {

// The codec bounds each node list exactly as the proof verifier does.
static_assert(STATE_PROOF_LIMITS.max_nodes == MAX_BRIDGE_MPT_NODES);
static_assert(STATE_PROOF_LIMITS.max_node_size == MAX_BRIDGE_RLP_ITEM_SIZE);
static_assert(STATE_PROOF_LIMITS.max_total_bytes == MAX_BRIDGE_MPT_BYTES);

//! A field of a packed storage word: `bits` bits from bit `offset`, bit 0
//! the least significant (Solidity packs a struct's first member lowest).
struct StorageFieldV2 {
    unsigned offset;
    unsigned bits;
};

namespace observation_layout {

// Verifier V2 (contracts spec section 3.2).
inline constexpr uint64_t VERIFIER_SLOT_LIVENESS{0};
inline constexpr uint64_t VERIFIER_SLOT_CURRENT_SET_HASH{1};
inline constexpr uint64_t VERIFIER_SLOT_NEXT_SET_HASH{2};
inline constexpr uint64_t VERIFIER_SLOT_LATEST{4};
inline constexpr uint64_t VERIFIER_SLOT_LATEST_CERT_ID{6};
inline constexpr StorageFieldV2 LIVENESS_CURRENT_EPOCH{0, 64};
inline constexpr StorageFieldV2 LIVENESS_LAST_ROTATION_TIME{64, 40};
inline constexpr StorageFieldV2 LIVENESS_LAST_CERTIFICATE_TIME{104, 40};
inline constexpr StorageFieldV2 LIVENESS_LATEST_BRIDGE_HEIGHT{144, 64};
inline constexpr StorageFieldV2 LIVENESS_STATUS{208, 8};
inline constexpr StorageFieldV2 LIVENESS_DEPOSITS_CLOSED{216, 8};
inline constexpr unsigned LIVENESS_UNUSED_FROM_BIT{224};
inline constexpr StorageFieldV2 LATEST_HEIGHT{0, 64};
inline constexpr StorageFieldV2 LATEST_INDEX{64, 64};
inline constexpr StorageFieldV2 LATEST_INIT_TIME{128, 40};
inline constexpr unsigned LATEST_UNUSED_FROM_BIT{168};

// Vault V2 (contracts spec section 6.6).
inline constexpr uint64_t VAULT_SLOT_NEXT_DEPOSIT_ID{0};
inline constexpr uint64_t VAULT_SLOT_LOCKED{1};
inline constexpr uint64_t VAULT_SLOT_ABSORBED_TOTAL{3};
inline constexpr unsigned VAULT_NEXT_DEPOSIT_ID_BITS{64};

// Tether (mainnet USDT; MockTetherToken mirrors it on regtest).
inline constexpr uint64_t TOKEN_SLOT_OWNER_PAUSED{0};
inline constexpr uint64_t TOKEN_SLOT_BALANCES{2};
inline constexpr uint64_t TOKEN_SLOT_IS_BLACK_LISTED{6};
inline constexpr uint64_t TOKEN_SLOT_UPGRADED_DEPRECATED{10};
inline constexpr StorageFieldV2 TOKEN_OWNER{0, 160};
inline constexpr StorageFieldV2 TOKEN_PAUSED{160, 8};
inline constexpr StorageFieldV2 TOKEN_UPGRADED_ADDRESS{0, 160};
inline constexpr StorageFieldV2 TOKEN_DEPRECATED{160, 8};
inline constexpr unsigned TOKEN_FLAG_WORD_UNUSED_FROM_BIT{168};

} // namespace observation_layout

//! Verifier V2 status values (B3TypesV2.STATUS_*).
inline constexpr uint8_t VERIFIER_STATUS_UNINITIALIZED{0};
inline constexpr uint8_t VERIFIER_STATUS_LIVE{1};
inline constexpr uint8_t VERIFIER_STATUS_HALTED_CONFLICT{2};
inline constexpr uint8_t VERIFIER_STATUS_CANCELLED{3};

/**
 * OI-3: the verifier's and vault's runtime code hashes commit to
 * blockHash(R_f - 1), so a regtest fixture cannot pin them. Only on this
 * chain (anvil) may an observation accept an unpinned verifier or vault,
 * and then only one with code. BridgeV2Params::Valid() already requires
 * both pins on Ethereum mainnet; this refuses every other chain too.
 */
inline constexpr uint64_t OBSERVATION_UNPINNED_CODE_CHAIN_ID{31'337};

/**
 * The same chain is the only one whose storage proofs may show a present
 * leaf holding zero (bridge::StoredZero): anvil keeps one for every slot a
 * transaction only read, while Ethereum clients delete zero slots, so every
 * other chain keeps node changes section 8.2's strict rule.
 */
inline constexpr uint64_t OBSERVATION_STORED_ZERO_CHAIN_ID{31'337};

/** VERIFIER_E: Verifier V2 slots 0, 1, 2, 4 and 6. Hashes are raw EVM bytes. */
struct VerifierStateV2 {
    uint64_t current_epoch{0};
    int64_t last_rotation_time{0};
    int64_t last_certificate_time{0};
    uint64_t latest_bridge_height{0};
    uint8_t status{VERIFIER_STATUS_UNINITIALIZED};
    bool deposits_closed{false};
    uint256 current_set_hash{};
    uint256 next_set_hash{};
    uint64_t latest_height{0};
    uint64_t latest_index{0};
    int64_t latest_init_time{0};
    uint256 latest_cert_id{};
    uint256 verifier_code_hash{};

    friend bool operator==(const VerifierStateV2&, const VerifierStateV2&) = default;
};

/** RESERVE_E: Vault V2 slots 0, 1, 3 and the token's balances[vault]. */
struct ReserveStateV2 {
    uint64_t next_deposit_id{0};
    CAmount locked{0};
    CAmount absorbed_total{0};
    //! USDT balance of the vault (B in node changes section 7.2).
    CAmount balance{0};
    uint256 vault_code_hash{};
    uint256 token_code_hash{};

    friend bool operator==(const ReserveStateV2&, const ReserveStateV2&) = default;
};

/** TOKEN_FLAGS: the token's slots 0 and 10 and isBlackListed[vault]. */
struct TokenFlagsV2 {
    EthAddress owner{};
    bool paused{false};
    EthAddress upgraded_address{};
    bool deprecated{false};
    bool vault_blacklisted{false};
    uint256 token_code_hash{};

    friend bool operator==(const TokenFlagsV2&, const TokenFlagsV2&) = default;
};

//! One decoded observation; the alternative follows the record's base subject
//! (EthObservationBaseSubject), so a successor subject decodes as its base.
using ObservedStateV2 = std::variant<VerifierStateV2, ReserveStateV2, TokenFlagsV2>;

/**
 * The accounts an observation may read and their code-hash pins. The token
 * pin is always required; the verifier and vault pins may be unset only on
 * OBSERVATION_UNPINNED_CODE_CHAIN_ID (OI-3).
 *
 * `entry` is the vault entry `vault` belongs to: the launch vault v, or the
 * B-23 successor v'. A subject is planned and verified only with the pins
 * of its own entry (EthObservationEntryRole), so a successor subject can
 * never read v's state, nor a launch subject v''s.
 */
struct ObservationPins {
    uint64_t origin_chain_id{0};
    EthAddress verifier{};
    EthAddress vault{};
    EthAddress token{};
    std::optional<uint256> verifier_code_hash{};
    std::optional<uint256> vault_code_hash{};
    uint256 token_code_hash{};
    Consensus::BridgeV2EntryRole entry{Consensus::BridgeV2EntryRole::LAUNCH};
};

/**
 * The pins of a bridge V2 envelope's vault entry `role` and its verifier.
 * SUCCESSOR names v''s vault address and its optional code hash (OI-3), with
 * the verifier, token, token code hash and chain that SuccessorValid()
 * requires equal to v's. Without a pinned successor the SUCCESSOR pins name
 * no vault and no token, which VerifyEthStateObservationV2 refuses for every
 * subject of that entry.
 */
inline ObservationPins ObservationPinsFromParams(const Consensus::BridgeV2Params& params,
                                                 const Consensus::BridgeV2EntryRole role)
{
    ObservationPins pins;
    pins.entry = role;
    pins.origin_chain_id = params.vault.origin_chain_id;
    pins.verifier = params.verifier.address;
    pins.verifier_code_hash = params.verifier.runtime_code_hash;
    const Consensus::BridgeV2VaultEntry* entry{params.Entry(role)};
    if (entry == nullptr) return pins;
    pins.origin_chain_id = entry->origin_chain_id;
    pins.vault = entry->vault_address;
    pins.token = entry->token_address;
    pins.vault_code_hash = entry->vault_runtime_code_hash;
    pins.token_code_hash = entry->token_runtime_code_hash;
    return pins;
}

//! The pins of a bridge V2 envelope's launch vault entry and its verifier.
inline ObservationPins ObservationPinsFromParams(const Consensus::BridgeV2Params& params)
{
    return ObservationPinsFromParams(params, Consensus::BridgeV2EntryRole::LAUNCH);
}

enum class ObservationAccountRole : uint8_t {
    VERIFIER,
    VAULT,
    TOKEN,
};

inline const char* ObservationAccountRoleName(const ObservationAccountRole role)
{
    switch (role) {
    case ObservationAccountRole::VERIFIER:
        return "verifier";
    case ObservationAccountRole::VAULT:
        return "vault";
    case ObservationAccountRole::TOKEN:
        return "token";
    }
    return "unknown";
}

/** One eth_getProof call of the plan: an account and its slots, in order. */
struct ObservationAccountRequest {
    ObservationAccountRole role{ObservationAccountRole::VERIFIER};
    EthAddress address{};
    std::vector<StorageWord> slots{};
};

/**
 * The eth_getProof plan of a subject, in the record's account and slot
 * order (EthObservationShapeV2 gives the same counts):
 * - VERIFIER_E: verifier slots 0, 1, 2, 4, 6.
 * - RESERVE_E: vault slots 0, 1, 3; token slot MappingSlot(vault, 2).
 * - TOKEN_FLAGS: token slots 0, 10, MappingSlot(vault, 6).
 * - RESERVE_E_SUCCESSOR and TOKEN_FLAGS_SUCCESSOR: the same plans with
 *   `pins.vault` the successor vault v'.
 * Empty for an unknown subject and for pins of another vault entry than the
 * subject's (EthObservationEntryRole).
 */
inline std::vector<ObservationAccountRequest> ObservationRequests(
    const uint8_t subject, const ObservationPins& pins)
{
    using namespace observation_layout;
    if (EthObservationEntryRole(subject) != pins.entry) return {};
    const StorageWord vault_key{AddressWord(pins.vault)};
    switch (EthObservationBaseSubject(subject)) {
    case ETH_OBSERVATION_SUBJECT_VERIFIER:
        return {{ObservationAccountRole::VERIFIER, pins.verifier,
                 {SlotWord(VERIFIER_SLOT_LIVENESS),
                  SlotWord(VERIFIER_SLOT_CURRENT_SET_HASH),
                  SlotWord(VERIFIER_SLOT_NEXT_SET_HASH),
                  SlotWord(VERIFIER_SLOT_LATEST),
                  SlotWord(VERIFIER_SLOT_LATEST_CERT_ID)}}};
    case ETH_OBSERVATION_SUBJECT_RESERVE:
        return {{ObservationAccountRole::VAULT, pins.vault,
                 {SlotWord(VAULT_SLOT_NEXT_DEPOSIT_ID),
                  SlotWord(VAULT_SLOT_LOCKED),
                  SlotWord(VAULT_SLOT_ABSORBED_TOTAL)}},
                {ObservationAccountRole::TOKEN, pins.token,
                 {MappingSlot(vault_key, TOKEN_SLOT_BALANCES)}}};
    case ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS:
        return {{ObservationAccountRole::TOKEN, pins.token,
                 {SlotWord(TOKEN_SLOT_OWNER_PAUSED),
                  SlotWord(TOKEN_SLOT_UPGRADED_DEPRECATED),
                  MappingSlot(vault_key, TOKEN_SLOT_IS_BLACK_LISTED)}}};
    }
    return {};
}

namespace state_observation_detail {

//! True when every bit from `from_bit` to 255 is zero.
inline bool HighBitsZero(const StorageWord& word, const unsigned from_bit)
{
    for (unsigned bit{from_bit}; bit < 256; ++bit) {
        if ((word[31 - bit / 8] >> (bit % 8)) & 1) return false;
    }
    return true;
}

inline uint64_t Field(const StorageWord& word, const StorageFieldV2& field)
{
    return WordField(word, field.offset, field.bits);
}

inline uint256 RawWord(const StorageWord& word)
{
    return uint256{std::span<const unsigned char>{word}};
}

//! The low 20 bytes of a word (an address member at bit offset 0).
inline EthAddress LowAddress(const StorageWord& word)
{
    EthAddress out{};
    std::copy(word.end() - 20, word.end(), out.begin());
    return out;
}

//! A uint256 amount as CAmount; nullopt above INT64_MAX.
inline std::optional<CAmount> Amount(const StorageWord& word)
{
    if (!HighBitsZero(word, 63)) return std::nullopt;
    return static_cast<CAmount>(WordField(word, 0, 63));
}

inline bool CodeHashAccepted(const ObservationAccountRole role,
                             const uint256& code_hash,
                             const ObservationPins& pins, std::string& error)
{
    const char* name{ObservationAccountRoleName(role)};
    const std::optional<uint256>* pin{nullptr};
    switch (role) {
    case ObservationAccountRole::VERIFIER:
        pin = &pins.verifier_code_hash;
        break;
    case ObservationAccountRole::VAULT:
        pin = &pins.vault_code_hash;
        break;
    case ObservationAccountRole::TOKEN:
        if (pins.token_code_hash.IsNull() || code_hash != pins.token_code_hash) {
            error = "observation token code hash does not match its pin";
            return false;
        }
        return true;
    }
    if (*pin) {
        if (code_hash != **pin) {
            error = std::string{"observation "} + name + " code hash does not match its pin";
            return false;
        }
        return true;
    }
    if (pins.origin_chain_id != OBSERVATION_UNPINNED_CODE_CHAIN_ID) {
        error = std::string{"observation "} + name + " code hash is unpinned off the regtest chain";
        return false;
    }
    if (code_hash == EMPTY_CODE_HASH) {
        error = std::string{"observation "} + name + " has no code";
        return false;
    }
    return true;
}

} // namespace state_observation_detail

/** Decode VERIFIER_E's five words (slots 0, 1, 2, 4, 6) strictly. */
inline std::optional<VerifierStateV2> DecodeVerifierStateV2(
    std::span<const StorageWord> words, const uint256& verifier_code_hash,
    std::string& error)
{
    using namespace observation_layout;
    using namespace state_observation_detail;
    if (words.size() != 5) {
        error = "verifier observation needs five storage words";
        return std::nullopt;
    }
    const StorageWord& liveness{words[0]};
    const StorageWord& latest{words[3]};
    if (!HighBitsZero(liveness, LIVENESS_UNUSED_FROM_BIT)) {
        error = "verifier liveness word has unused bits set";
        return std::nullopt;
    }
    if (!HighBitsZero(latest, LATEST_UNUSED_FROM_BIT)) {
        error = "verifier latest word has unused bits set";
        return std::nullopt;
    }
    const uint64_t status{Field(liveness, LIVENESS_STATUS)};
    if (status > VERIFIER_STATUS_CANCELLED) {
        error = "verifier status is out of range";
        return std::nullopt;
    }
    const uint64_t deposits_closed{Field(liveness, LIVENESS_DEPOSITS_CLOSED)};
    if (deposits_closed > 1) {
        error = "verifier depositsClosed is not a boolean";
        return std::nullopt;
    }
    VerifierStateV2 out;
    out.current_epoch = Field(liveness, LIVENESS_CURRENT_EPOCH);
    out.last_rotation_time = static_cast<int64_t>(Field(liveness, LIVENESS_LAST_ROTATION_TIME));
    out.last_certificate_time = static_cast<int64_t>(Field(liveness, LIVENESS_LAST_CERTIFICATE_TIME));
    out.latest_bridge_height = Field(liveness, LIVENESS_LATEST_BRIDGE_HEIGHT);
    out.status = static_cast<uint8_t>(status);
    out.deposits_closed = deposits_closed == 1;
    out.current_set_hash = RawWord(words[1]);
    out.next_set_hash = RawWord(words[2]);
    out.latest_height = Field(latest, LATEST_HEIGHT);
    out.latest_index = Field(latest, LATEST_INDEX);
    out.latest_init_time = static_cast<int64_t>(Field(latest, LATEST_INIT_TIME));
    out.latest_cert_id = RawWord(words[4]);
    out.verifier_code_hash = verifier_code_hash;
    return out;
}

/** Decode RESERVE_E's vault words (slots 0, 1, 3) and token balance strictly. */
inline std::optional<ReserveStateV2> DecodeReserveStateV2(
    std::span<const StorageWord> vault_words, const StorageWord& balance_word,
    const uint256& vault_code_hash, const uint256& token_code_hash,
    std::string& error)
{
    using namespace observation_layout;
    using namespace state_observation_detail;
    if (vault_words.size() != 3) {
        error = "reserve observation needs three vault storage words";
        return std::nullopt;
    }
    if (!HighBitsZero(vault_words[0], VAULT_NEXT_DEPOSIT_ID_BITS)) {
        error = "vault nextDepositId word has unused bits set";
        return std::nullopt;
    }
    const auto locked{Amount(vault_words[1])};
    const auto absorbed{Amount(vault_words[2])};
    const auto balance{Amount(balance_word)};
    if (!locked || !absorbed || !balance) {
        error = "reserve observation amount exceeds the int64 range";
        return std::nullopt;
    }
    ReserveStateV2 out;
    out.next_deposit_id = WordField(vault_words[0], 0, VAULT_NEXT_DEPOSIT_ID_BITS);
    out.locked = *locked;
    out.absorbed_total = *absorbed;
    out.balance = *balance;
    out.vault_code_hash = vault_code_hash;
    out.token_code_hash = token_code_hash;
    return out;
}

/** Decode TOKEN_FLAGS's three words (slots 0, 10, isBlackListed[vault]) strictly. */
inline std::optional<TokenFlagsV2> DecodeTokenFlagsV2(
    std::span<const StorageWord> words, const uint256& token_code_hash,
    std::string& error)
{
    using namespace observation_layout;
    using namespace state_observation_detail;
    if (words.size() != 3) {
        error = "token flags observation needs three storage words";
        return std::nullopt;
    }
    const StorageWord& owner_paused{words[0]};
    const StorageWord& upgraded_deprecated{words[1]};
    const StorageWord& blacklisted{words[2]};
    if (!HighBitsZero(owner_paused, TOKEN_FLAG_WORD_UNUSED_FROM_BIT)) {
        error = "token slot 0 has unused bits set";
        return std::nullopt;
    }
    if (!HighBitsZero(upgraded_deprecated, TOKEN_FLAG_WORD_UNUSED_FROM_BIT)) {
        error = "token slot 10 has unused bits set";
        return std::nullopt;
    }
    const uint64_t paused{Field(owner_paused, TOKEN_PAUSED)};
    const uint64_t deprecated{Field(upgraded_deprecated, TOKEN_DEPRECATED)};
    if (paused > 1 || deprecated > 1 || !HighBitsZero(blacklisted, 1)) {
        error = "token flag is not a boolean";
        return std::nullopt;
    }
    TokenFlagsV2 out;
    out.owner = LowAddress(owner_paused);
    out.paused = paused == 1;
    out.upgraded_address = LowAddress(upgraded_deprecated);
    out.deprecated = deprecated == 1;
    out.vault_blacklisted = WordField(blacklisted, 0, 1) == 1;
    out.token_code_hash = token_code_hash;
    return out;
}

/**
 * Verify a kind 9 record against the execution anchor's `state_root` and
 * decode it. For each account of the subject's plan: VerifyAccountProof
 * (the account must exist), the code-hash rule, then VerifyStorageValue for
 * each slot (absent slots read as zero, through strict exclusion proofs; a
 * present zero leaf only on OBSERVATION_STORED_ZERO_CHAIN_ID).
 * The verdict byte is not judged here. Returns nullopt with a stable
 * `error` for any proof, pin, shape or layout failure.
 *
 * `pins` must be the pins of the subject's vault entry
 * (ObservationPinsFromParams(params, EthObservationEntryRole(subject))): a
 * successor subject decodes into the same ReserveStateV2 or TokenFlagsV2,
 * read from v''s accounts and slots.
 */
inline std::optional<ObservedStateV2> VerifyEthStateObservationV2(
    const BridgeEthStateObservationV2& record, const uint256& state_root,
    const ObservationPins& pins, std::string& error)
{
    using namespace state_observation_detail;
    const uint8_t base_subject{EthObservationBaseSubject(record.subject)};
    if (base_subject == 0) {
        error = "observation subject is unknown";
        return std::nullopt;
    }
    if (EthObservationEntryRole(record.subject) != pins.entry) {
        error = "observation pins belong to another vault entry";
        return std::nullopt;
    }
    if (base_subject != ETH_OBSERVATION_SUBJECT_VERIFIER &&
        (!proof_detail::AddressNonzero(pins.vault) ||
         !proof_detail::AddressNonzero(pins.token))) {
        error = "observation pins name no vault entry";
        return std::nullopt;
    }
    const auto requests{ObservationRequests(record.subject, pins)};
    if (requests.empty()) {
        error = "observation subject is unknown";
        return std::nullopt;
    }
    if (record.accounts.size() != requests.size()) {
        error = "observation account count does not match its subject";
        return std::nullopt;
    }
    if (state_root.IsNull()) {
        error = "observation anchor has no state root";
        return std::nullopt;
    }

    const StoredZero stored_zero{pins.origin_chain_id == OBSERVATION_STORED_ZERO_CHAIN_ID
                                     ? StoredZero::ACCEPT
                                     : StoredZero::REFUSE};
    std::vector<std::vector<StorageWord>> words(requests.size());
    std::vector<uint256> code_hashes(requests.size());
    for (size_t i{0}; i < requests.size(); ++i) {
        const ObservationAccountRequest& request{requests[i]};
        const BridgeEthAccountProofV2& proof{record.accounts[i]};
        const std::string name{ObservationAccountRoleName(request.role)};
        if (proof.storage_nodes.size() != request.slots.size()) {
            error = "observation " + name + " slot count does not match its subject";
            return std::nullopt;
        }
        std::string detail;
        const auto account{VerifyAccountProof(state_root, request.address,
                                              proof.account_nodes, detail)};
        if (!account) {
            error = "observation " + name + " account proof: " + detail;
            return std::nullopt;
        }
        if (!CodeHashAccepted(request.role, account->code_hash, pins, error)) {
            return std::nullopt;
        }
        code_hashes[i] = account->code_hash;
        for (size_t j{0}; j < request.slots.size(); ++j) {
            const auto word{VerifyStorageValue(account->storage_root,
                                               request.slots[j],
                                               proof.storage_nodes[j], detail,
                                               stored_zero)};
            if (!word) {
                error = "observation " + name + " storage proof " +
                        std::to_string(j) + ": " + detail;
                return std::nullopt;
            }
            words[i].push_back(*word);
        }
    }

    switch (base_subject) {
    case ETH_OBSERVATION_SUBJECT_VERIFIER: {
        auto state{DecodeVerifierStateV2(words[0], code_hashes[0], error)};
        if (!state) return std::nullopt;
        return ObservedStateV2{std::move(*state)};
    }
    case ETH_OBSERVATION_SUBJECT_RESERVE: {
        auto state{DecodeReserveStateV2(words[0], words[1][0], code_hashes[0],
                                        code_hashes[1], error)};
        if (!state) return std::nullopt;
        return ObservedStateV2{std::move(*state)};
    }
    case ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS: {
        auto state{DecodeTokenFlagsV2(words[0], code_hashes[0], error)};
        if (!state) return std::nullopt;
        return ObservedStateV2{std::move(*state)};
    }
    }
    error = "observation subject is unknown";
    return std::nullopt;
}

} // namespace bridge

#endif // B3COIN_BRIDGE_STATE_OBSERVATION_H
