// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license.
#ifndef B3COIN_BRIDGE_ARBITRUM_SEPOLIA_AVAILABILITY_H
#define B3COIN_BRIDGE_ARBITRUM_SEPOLIA_AVAILABILITY_H

#include <bridge/arbitrum_sepolia.h>

namespace bridge::arbitrum::sepolia {
namespace availability_detail { class Queries; }

/** A native root-bound discovery query, not rollup settlement authority.
 * This authenticates one rollup account's current assertion hash and creation
 * block under a current Sepolia execution state root. It does not authenticate
 * all seventeen dependency accounts, execute Nitro, prove log inclusion or
 * authorize deposits. The result can select exact blocks for peer availability
 * requests; it cannot construct SettledHead, Ancestor or ConfirmedRange.
 */
class CurrentAssertionHash final {
public:
    const uint256& Hash() const { return m_hash; }
    uint64_t CreatedBlock() const { return m_created; }
    const gl::FinalizedExecution& EthereumAnchor() const { return m_anchor; }
    bool IsCurrent(const gl::LightClient& ethereum, uint64_t slot) const
    { return m_owner == &ethereum && ethereum.IsCurrent(m_anchor, slot); }
    bool MainnetEnabled() const { return false; }
private:
    friend class availability_detail::Queries;
    CurrentAssertionHash(const gl::LightClient& owner, gl::FinalizedExecution anchor, uint256 hash, uint64_t created)
        : m_owner{&owner}, m_anchor{std::move(anchor)}, m_hash{hash}, m_created{created} {}
    const gl::LightClient* m_owner;
    gl::FinalizedExecution m_anchor;
    uint256 m_hash;
    uint64_t m_created;
};

namespace availability_detail {
class Queries final {
public:
    static Reply<CurrentAssertionHash> Latest(const gl::LightClient& ethereum,
        const gl::FinalizedExecution& anchor, uint64_t slot, const AccountWitness& witness)
    {
        if (!ethereum.IsCurrent(anchor, slot)) return {Error::STALE_ETHEREUM, {}};
        if (anchor.ChainId() != gl::CHAIN_ID || anchor.GenesisValidatorsRoot() != uint256{std::span{gl::GENESIS_VALIDATORS_ROOT}})
            return {Error::WRONG_NETWORK, {}};
        if (anchor.Header().block_number < pins::DEPLOYMENT_BLOCK) return {Error::BAD_DEPLOYMENT, {}};
        const auto& pin{pins::Registry().front()};
        if (pin.address != pins::Rollup() || witness.address != pin.address ||
            witness.storage.size() != pin.storage.size()+2) return {Error::BAD_STORAGE, {}};
        size_t bytes{20+8+witness.storage.size()*36}, count{0};
        if (!detail::NodesBudget(bytes, count, witness.nodes)) return {Error::LIMIT, {}};
        for (const auto& item : witness.storage)
            if (!detail::NodesBudget(bytes, count, item.nodes)) return {Error::LIMIT, {}};
        std::set<StorageWord> slots;
        for (const auto& item : witness.storage)
            if (!slots.insert(item.slot).second) return {Error::BAD_STORAGE, {}};
        std::string error;
        const auto account{VerifyAccountProof(anchor.Header().state_root, pin.address, witness.nodes, error)};
        if (!account || account->code_hash != pin.code_hash) return {Error::BAD_ACCOUNT, {}};
        for (size_t i{0}; i < pin.storage.size(); ++i) {
            const auto& item{witness.storage[i]};
            if (item.slot != pin.storage[i].slot) return {Error::BAD_STORAGE, {}};
            const auto word{VerifyStorageValue(account->storage_root, item.slot, item.nodes, error)};
            if (!word || *word != pin.storage[i].value) return {Error::BAD_STORAGE, {}};
        }
        const auto& latest{witness.storage[pin.storage.size()]};
        if (latest.slot != SlotWord(pins::LATEST_CONFIRMED_SLOT)) return {Error::BAD_STORAGE, {}};
        const auto word{VerifyStorageValue(account->storage_root, latest.slot, latest.nodes, error)};
        if (!word || !arbitrum::detail::Nonzero(*word)) return {Error::BAD_ASSERTION, {}};
        const uint256 hash{std::span<const unsigned char>{*word}};
        const auto& node{witness.storage[pin.storage.size()+1]};
        if (node.slot != detail::AssertionSlot(hash)) return {Error::BAD_STORAGE, {}};
        const auto node_word{VerifyStorageValue(account->storage_root, node.slot, node.nodes, error)};
        if (!node_word || !detail::ConfirmedNodeWord(*node_word, anchor.Header().block_number))
            return {Error::BAD_ASSERTION, {}};
        uint64_t created{0};
        for (size_t i{8}; i < 16; ++i) created = (created << 8) | (*node_word)[i];
        return {Error::OK, CurrentAssertionHash{ethereum, anchor, hash, created}};
    }
};
} // namespace availability_detail

/** The witness has the exact compiled rollup static slots followed by slot116
 * and the mapping slot derived from the proved latest hash. A transport can
 * obtain a candidate hash first, then use RequiredWitnessesByHash(candidate)
 * to request the complete witness. No assertion preimage is required here.
 */
inline Reply<CurrentAssertionHash> QueryLatestConfirmed(const gl::LightClient& ethereum,
    const gl::FinalizedExecution& anchor, uint64_t slot, const AccountWitness& rollup)
{
    return availability_detail::Queries::Latest(ethereum, anchor, slot, rollup);
}

/** Data-only preimage recovery from untrusted peer receipts or archived logs.
 * The compiled ABI/topic and native assertion hash are checked, but this does
 * not establish log inclusion, Ethereum finality or settlement. Only
 * AuthenticateHead can bind the result to latestConfirmed under its owner.
 */
inline std::optional<AssertionPreimage> DecodeAssertionCreated(const EthLog& log, const uint256& expected_hash)
{
    if (expected_hash.IsNull() || log.address != pins::Rollup() || log.topics.size() != 3 ||
        log.topics[0] != pins::Hash("901c3aee23cf4478825462caaab375c606ab83516060388344f0650340753630") ||
        log.topics[1] != expected_hash || log.topics[2].IsNull() || log.data.size() != 25*32) return std::nullopt;
    const auto word = [&](size_t index) { return std::span{log.data}.subspan(index*32, 32); };
    for (const size_t index : {5, 6, 9, 10, 15, 16, 24})
        if (arbitrum::detail::Nonzero(word(index).first(24))) return std::nullopt;
    for (const size_t index : {4, 23})
        if (arbitrum::detail::Nonzero(word(index).first(12))) return std::nullopt;
    for (const size_t index : {11, 17})
        if (arbitrum::detail::Nonzero(word(index).first(31)) || word(index).back() > 3) return std::nullopt;
    const auto integer = [&](size_t index) {
        uint64_t value{0}; for (const auto byte : word(index).last(8)) value = (value << 8) | byte; return value;
    };
    AssertionPreimage out;
    out.parent_assertion = log.topics[2];
    out.block_hash = uint256{word(13)}; out.send_root = uint256{word(14)};
    out.inbox_position = integer(15); out.position_in_message = integer(16);
    out.machine_status = word(17).back(); out.end_history_root = uint256{word(18)};
    out.inbox_accumulator = uint256{word(19)};
    if (out.machine_status != 1 || out.block_hash.IsNull() || arbitrum::detail::AssertionHash(out) != expected_hash)
        return std::nullopt;
    return out;
}

} // namespace bridge::arbitrum::sepolia
#endif
