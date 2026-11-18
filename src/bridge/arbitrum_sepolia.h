// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license.
#ifndef B3COIN_BRIDGE_ARBITRUM_SEPOLIA_H
#define B3COIN_BRIDGE_ARBITRUM_SEPOLIA_H

#include <bridge/arbitrum_history.h>
#include <bridge/arbitrum_sepolia_pins.h>
#include <bridge/gloas_peer_light_client.h>

/** Ethereum-finalized CONTRACT-SETTLED Arbitrum Sepolia only. This proof
 * authenticates the canonical rollup's latestConfirmed state under an exact
 * reviewed code/layout/config snapshot. Authorized upgrade/force-confirmation
 * governance and honest challenge validation remain trust assumptions. It
 * does not independently execute Nitro or prove the entire fraud-prover graph.
 * Arbitrum One and every existing production hard stop are unchanged.
 */
namespace bridge::arbitrum::sepolia {
namespace gl = peer::gloas;
inline constexpr uint64_t CHAIN_ID{421'614};
enum class Error {
    OK, LIMIT, STALE_ETHEREUM, WRONG_NETWORK, BAD_DEPLOYMENT, BAD_ACCOUNT,
    BAD_STORAGE, BAD_ASSERTION, BAD_HEADER, BAD_ANCESTRY, BAD_RANGE,
};
inline const char* Name(Error error)
{
    switch (error) {
    case Error::OK: return "OK";
    case Error::LIMIT: return "LIMIT";
    case Error::STALE_ETHEREUM: return "STALE_ETHEREUM";
    case Error::WRONG_NETWORK: return "WRONG_NETWORK";
    case Error::BAD_DEPLOYMENT: return "BAD_DEPLOYMENT";
    case Error::BAD_ACCOUNT: return "BAD_ACCOUNT";
    case Error::BAD_STORAGE: return "BAD_STORAGE";
    case Error::BAD_ASSERTION: return "BAD_ASSERTION";
    case Error::BAD_HEADER: return "BAD_HEADER";
    case Error::BAD_ANCESTRY: return "BAD_ANCESTRY";
    case Error::BAD_RANGE: return "BAD_RANGE";
    }
    return "BAD_DEPLOYMENT";
}
template <typename T> struct Reply { Error error; std::optional<T> value; };
class Verifier;

// Separate groups preserve the existing <=16-account wire/availability bound.
// The registry has exactly17 accounts: group0 first16; group1 the last one.
using AccountGroups = std::array<std::vector<AccountWitness>, 2>;
struct HeadProof {
    AssertionPreimage assertion;
    AccountGroups accounts;
    Bytes header;
};

class SettledHead final {
public:
    const arbitrum::Header& Header() const { return m_header; }
    const uint256& AssertionHash() const { return m_assertion; }
    const gl::FinalizedExecution& EthereumAnchor() const { return m_anchor; }
    const uint256& DeploymentDigest() const { return m_deployment; }
    bool IsCurrent(const gl::LightClient& ethereum, uint64_t current_slot) const
    {
        return m_owner == &ethereum && m_deployment == pins::Digest() && ethereum.IsCurrent(m_anchor, current_slot);
    }
    uint64_t ChainId() const { return CHAIN_ID; }
    bool MainnetEnabled() const { return false; }
private:
    friend class Verifier;
    SettledHead(const gl::LightClient& owner, gl::FinalizedExecution anchor, arbitrum::Header header, uint256 assertion)
        : m_owner{&owner}, m_anchor{std::move(anchor)}, m_header{std::move(header)}, m_assertion{assertion}, m_deployment{pins::Digest()} {}
    const gl::LightClient* m_owner;
    gl::FinalizedExecution m_anchor;
    arbitrum::Header m_header;
    uint256 m_assertion;
    uint256 m_deployment;
};

/** One authenticated endpoint in the settled head's parent chain. A caller
 * cannot deserialize a checkpoint/hash into this type. Bounded pages may be
 * replayed from durable raw bytes after restart; the capability itself is not
 * a journal or an antirollback assertion. Each page stores only its endpoint.
 */
class Ancestor final {
public:
    const arbitrum::Header& Header() const { return m_header; }
    const SettledHead& Settlement() const { return m_settlement; }
    bool IsCurrent(const gl::LightClient& ethereum, uint64_t slot) const { return m_settlement.IsCurrent(ethereum, slot); }
private:
    friend class Verifier;
    Ancestor(SettledHead settlement, arbitrum::Header header)
        : m_settlement{std::move(settlement)}, m_header{std::move(header)} {}
    SettledHead m_settlement;
    arbitrum::Header m_header;
};

class ConfirmedRange final {
public:
    const history::Evidence& History() const { return m_history; }
    const RangeRequest& Request() const { return m_history.request; }
    const SettledHead& Settlement() const { return m_endpoint.Settlement(); }
    bool IsCurrent(const gl::LightClient& ethereum, uint64_t slot) const { return m_endpoint.IsCurrent(ethereum, slot); }
    uint64_t ChainId() const { return CHAIN_ID; }
    bool MainnetEnabled() const { return false; }
private:
    friend class Verifier;
    ConfirmedRange(Ancestor endpoint, history::Evidence evidence)
        : m_endpoint{std::move(endpoint)}, m_history{std::move(evidence)} {}
    Ancestor m_endpoint;
    history::Evidence m_history;
};

namespace detail {
inline bool NodesBudget(size_t& bytes, size_t& count, const Nodes& nodes)
{
    if (nodes.size() > 64 || nodes.size() > proof_codec::MAX_TOTAL_NODES - count) return false;
    count += nodes.size();
    for (const auto& node : nodes) {
        if (node.empty() || node.size() > 8192 || node.size() + 4 > MAX_PROOF_BYTES - bytes) return false;
        bytes += node.size() + 4;
    }
    return true;
}
inline bool Budget(const AccountGroups& groups, size_t header_bytes)
{
    if (header_bytes > MAX_HEADER_BYTES) return false;
    size_t bytes{177 + 8 + 4 + header_bytes}, count{0};
    for (const auto& accounts : groups) {
      if (accounts.size() > MAX_ACCOUNTS) return false;
      for (const auto& account : accounts) {
        if (account.storage.size() > MAX_STORAGE) return false;
        const size_t overhead{20 + 4 + 4 + account.storage.size() * 36};
        if (overhead > MAX_PROOF_BYTES - bytes) return false;
        bytes += overhead;
        if (!NodesBudget(bytes, count, account.nodes)) return false;
        for (const auto& slot : account.storage) if (!NodesBudget(bytes, count, slot.nodes)) return false;
      }
    }
    return true;
}
inline StorageWord AssertionSlot(const uint256& assertion)
{
    Bytes bytes{assertion.begin(), assertion.end()};
    const auto base{SlotWord(pins::ASSERTIONS_SLOT)};
    bytes.insert(bytes.end(), base.begin(), base.end());
    const auto hash{arbitrum::detail::Hash(bytes)};
    StorageWord out;
    std::copy(hash.begin(), hash.end(), out.begin());
    return out;
}
inline bool ConfirmedNodeWord(const StorageWord& word, uint64_t l1_number)
{
    // Verified solc0.8.17 AssertionNode layout: uint64 children/creation,
    // bool isFirstChild at offset24, enum status at offset25. Confirmed==2.
    if (std::any_of(word.begin(), word.begin()+6, [](auto byte) { return byte != 0; }) || word[6] != 2 || word[7] > 1) return false;
    uint64_t created{0};
    for (size_t i{8}; i < 16; ++i) created = (created << 8) | word[i];
    return created != 0 && created <= l1_number;
}
} // namespace detail

class Verifier final {
public:
    /** All witnesses are untrusted. Expected addresses/code/config/layout are
     * selected solely by the compiled Sepolia registry, never caller policy.
     * Required witness order/slots are exact; duplicates and unused data refuse.
     */
    static Reply<SettledHead> AuthenticateHead(const gl::LightClient& ethereum,
        const gl::FinalizedExecution& anchor, uint64_t current_slot,
        const HeadProof& proof)
    {
        const auto& assertion{proof.assertion}; const auto& groups{proof.accounts};
        const auto& settled_header{proof.header};
        if (!ethereum.IsCurrent(anchor, current_slot)) return {Error::STALE_ETHEREUM, {}};
        if (anchor.ChainId() != gl::CHAIN_ID || anchor.GenesisValidatorsRoot() != uint256{std::span{gl::GENESIS_VALIDATORS_ROOT}})
            return {Error::WRONG_NETWORK, {}};
        if (anchor.Header().block_number < pins::DEPLOYMENT_BLOCK) return {Error::BAD_DEPLOYMENT, {}};
        if (settled_header.empty() || !detail::Budget(groups, settled_header.size()))
            return {Error::LIMIT, {}};
        if (assertion.machine_status != 1 || assertion.block_hash.IsNull() || assertion.parent_assertion.IsNull())
            return {Error::BAD_ASSERTION, {}};
        const auto assertion_hash{arbitrum::detail::AssertionHash(assertion)};
        const auto& expected{pins::Registry()};
        if (expected.size() != 17 || groups[0].size() != MAX_ACCOUNTS || groups[1].size() != 1)
            return {Error::BAD_DEPLOYMENT, {}};
        std::set<EthAddress> addresses;
        for (size_t i{0}; i < expected.size(); ++i) {
            const auto& pin{expected[i]}; const auto& witness{groups[i / MAX_ACCOUNTS][i % MAX_ACCOUNTS]};
            const bool rollup{pin.address == pins::Rollup()};
            if (witness.address != pin.address || !addresses.insert(witness.address).second ||
                witness.storage.size() != pin.storage.size() + (rollup ? 2U : 0U))
                return {Error::BAD_STORAGE, {}};
            std::set<StorageWord> slots;
            for (const auto& item : witness.storage) if (!slots.insert(item.slot).second) return {Error::BAD_STORAGE, {}};
            std::string error;
            const auto account{VerifyAccountProof(anchor.Header().state_root, pin.address, witness.nodes, error)};
            if (!account || account->code_hash != pin.code_hash) return {Error::BAD_ACCOUNT, {}};
            for (size_t j{0}; j < pin.storage.size(); ++j) {
                if (witness.storage[j].slot != pin.storage[j].slot) return {Error::BAD_STORAGE, {}};
                const auto word{VerifyStorageValue(account->storage_root, witness.storage[j].slot, witness.storage[j].nodes, error)};
                if (!word || *word != pin.storage[j].value) return {Error::BAD_STORAGE, {}};
            }
            if (rollup) {
                const auto offset{pin.storage.size()};
                const auto& latest{witness.storage[offset]}; const auto& node{witness.storage[offset+1]};
                if (latest.slot != SlotWord(pins::LATEST_CONFIRMED_SLOT) || node.slot != detail::AssertionSlot(assertion_hash))
                    return {Error::BAD_STORAGE, {}};
                const auto latest_hash{VerifyStorageValue(account->storage_root, latest.slot, latest.nodes, error)};
                const auto node_word{VerifyStorageValue(account->storage_root, node.slot, node.nodes, error)};
                if (!latest_hash || !std::equal(latest_hash->begin(), latest_hash->end(), assertion_hash.begin()) ||
                    !node_word || !detail::ConfirmedNodeWord(*node_word, anchor.Header().block_number)) return {Error::BAD_ASSERTION, {}};
            }
        }
        const auto header{arbitrum::detail::NitroHeader(settled_header)};
        if (!header || header->hash != assertion.block_hash || header->timestamp > anchor.Header().timestamp)
            return {Error::BAD_HEADER, {}};
        return {Error::OK, SettledHead{ethereum, anchor, *header, assertion_hash}};
    }

    static Reply<Ancestor> BeginAncestor(const gl::LightClient& ethereum, uint64_t slot, const SettledHead& head)
    {
        if (!head.IsCurrent(ethereum, slot)) return {Error::STALE_ETHEREUM, {}};
        return {Error::OK, Ancestor{head, head.Header()}};
    }
    static Reply<Ancestor> WalkBack(const gl::LightClient& ethereum, uint64_t slot, const Ancestor& current,
                                   const std::vector<Bytes>& page)
    {
        if (!current.IsCurrent(ethereum, slot)) return {Error::STALE_ETHEREUM, {}};
        if (page.empty() || page.size() > MAX_HEADERS) return {Error::LIMIT, {}};
        auto previous{current.Header()};
        for (const auto& raw : page) {
            const auto header{arbitrum::detail::NitroHeader(raw)};
            if (!header || !previous.number || header->hash != previous.parent || header->number != previous.number-1 ||
                header->timestamp > previous.timestamp) return {Error::BAD_ANCESTRY, {}};
            previous = *header;
        }
        return {Error::OK, Ancestor{current.Settlement(), previous}};
    }
    static Reply<ConfirmedRange> VerifyRange(const gl::LightClient& ethereum, uint64_t slot, const Ancestor& endpoint,
                                           const RangeRequest& request, const Proof& proof)
    {
        if (!endpoint.IsCurrent(ethereum, slot)) return {Error::STALE_ETHEREUM, {}};
        if (request.to_height != endpoint.Header().number || request.to_hash != endpoint.Header().hash)
            return {Error::BAD_ANCESTRY, {}};
        const auto checked{history::CheckRange(request, proof)};
        if (!checked.evidence) return {checked.error == arbitrum::Error::LIMIT ? Error::LIMIT : Error::BAD_RANGE, {}};
        return {Error::OK, ConfirmedRange{endpoint, *checked.evidence}};
    }
};

/** Public availability request description only, never an allowlist input. */
using WitnessRequests = std::array<std::vector<std::pair<EthAddress, std::vector<StorageWord>>>, 2>;
inline WitnessRequests RequiredWitnessesByHash(const uint256& assertion_hash)
{
    WitnessRequests out;
    const auto& registry{pins::Registry()};
    for (size_t i{0}; i < registry.size(); ++i) {
        const auto& pin{registry[i]};
        std::vector<StorageWord> slots;
        for (const auto& storage : pin.storage) slots.push_back(storage.slot);
        if (pin.address == pins::Rollup()) {
            slots.push_back(SlotWord(pins::LATEST_CONFIRMED_SLOT));
            slots.push_back(detail::AssertionSlot(assertion_hash));
        }
        out[i / MAX_ACCOUNTS].emplace_back(pin.address, std::move(slots));
    }
    return out;
}
inline WitnessRequests RequiredWitnesses(const AssertionPreimage& assertion)
{
    return RequiredWitnessesByHash(arbitrum::detail::AssertionHash(assertion));
}

} // namespace bridge::arbitrum::sepolia
#endif
