// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3COIN_BRIDGE_ARBITRUM_HISTORY_H
#define B3COIN_BRIDGE_ARBITRUM_HISTORY_H

#include <bridge/arbitrum_proof_codec.h>

#include <map>
#include <set>
#include <string_view>

/** Offline commitment checks against a SEPARATELY SUPPLIED L2 checkpoint.
 * These results do not authenticate that checkpoint, execute Nitro, prove
 * finality/rollup settlement, or authorize source facts. In particular this
 * module cannot construct arbitrum::ConfirmedRange. A dishonest checkpoint
 * can commit to a completely fabricated but internally consistent history.
 */
namespace bridge::arbitrum::history {

struct StorageValue { StorageWord slot{}, value{}; };
struct AccountValue {
    EthAddress address{};
    EthAccount account;
    std::vector<StorageValue> storage;
};
struct Evidence {
    RangeRequest request;
    std::vector<Header> headers; // oldest first
    std::vector<LocatedReceipt> receipts; // block number, then transaction index
    std::vector<AccountValue> newest_accounts, oldest_accounts;
};
struct Reply { Error error; std::optional<Evidence> evidence; };

inline bool NeutralAssertion(const AssertionPreimage& a)
{
    return a.parent_assertion.IsNull() && a.block_hash.IsNull() && a.send_root.IsNull() &&
        a.inbox_position == 0 && a.position_in_message == 0 && a.machine_status == 1 &&
        a.end_history_root.IsNull() && a.inbox_accumulator.IsNull();
}

inline std::optional<std::vector<AccountValue>> CheckAccounts(
    const uint256& state_root, const std::vector<AccountWitness>& witnesses)
{
    std::set<EthAddress> addresses;
    std::vector<AccountValue> values;
    for (const auto& witness : witnesses) {
        if (!detail::Nonzero(witness.address) || !addresses.insert(witness.address).second) return std::nullopt;
        std::string error;
        const auto account{VerifyAccountProof(state_root, witness.address, witness.nodes, error)};
        if (!account) return std::nullopt;
        AccountValue value{witness.address, *account, {}};
        std::set<StorageWord> slots;
        for (const auto& storage : witness.storage) {
            if (!slots.insert(storage.slot).second) return std::nullopt;
            const auto word{VerifyStorageValue(account->storage_root, storage.slot, storage.nodes, error)};
            if (!word) return std::nullopt;
            value.storage.push_back({storage.slot, *word});
        }
        values.push_back(std::move(value));
    }
    return values;
}

inline Reply CheckRange(const RangeRequest& request, const Proof& proof)
{
    // The codec applies total byte, element and allocation budgets even for
    // callers constructing Proof directly rather than using the wire decoder.
    if (proof_codec::EncodeProofV1(proof).result != proof_codec::Result::OK) return {Error::LIMIT, {}};
    if (!NeutralAssertion(proof.assertion) || !proof.l1_accounts.empty()) return {Error::BAD_ASSERTION, {}};
    if (request.previous_height >= request.to_height || request.previous_hash.IsNull() || request.to_hash.IsNull() ||
        request.to_height - request.previous_height != proof.blocks.size()) return {Error::BAD_ANCESTRY, {}};
    Evidence out;
    out.request = request;
    uint256 want{request.to_hash};
    std::optional<Header> newer;
    for (const auto& block : proof.blocks) {
        const auto header{detail::NitroHeader(block.header)};
        if (!header) return {Error::BAD_HEADER, {}};
        if (header->hash != want || (!newer && header->number != request.to_height) ||
            (newer && (newer->number == 0 || header->number != newer->number - 1 ||
                       header->timestamp > newer->timestamp))) return {Error::BAD_ANCESTRY, {}};
        const auto receipts_root{detail::ReceiptRoot(block.receipts)};
        if (!receipts_root || *receipts_root != header->receipts_root) return {Error::BAD_RECEIPTS, {}};
        if (block.transactions.size() != block.receipts.size()) return {Error::BAD_TRANSACTIONS, {}};
        const auto transactions_root{detail::TransactionRoot(block.transactions)};
        if (!transactions_root || *transactions_root != header->transactions_root) return {Error::BAD_TRANSACTIONS, {}};
        uint64_t gas{0};
        for (const auto& item : block.receipts) {
            auto receipt{DecodeReceipt(item.encoded, RECEIPT_MAX_TYPE_V2)};
            if (!receipt || receipt->cumulative_gas < gas || receipt->cumulative_gas > header->gas_used ||
                (!receipt->status && !receipt->logs.empty())) return {Error::BAD_RECEIPTS, {}};
            gas = receipt->cumulative_gas;
            out.receipts.push_back({header->number, item.index,
                detail::Hash(block.transactions[item.index].encoded), std::move(*receipt)});
        }
        if (gas != header->gas_used) return {Error::BAD_RECEIPTS, {}};
        out.headers.push_back(*header);
        newer = header;
        want = header->parent;
    }
    if (!newer || newer->number != request.previous_height + 1 || want != request.previous_hash)
        return {Error::BAD_ANCESTRY, {}};
    const auto newest_accounts{CheckAccounts(out.headers.front().state_root, proof.confirmed_accounts)};
    const auto oldest_accounts{CheckAccounts(out.headers.back().state_root, proof.historical_accounts)};
    if (!newest_accounts || !oldest_accounts) return {Error::BAD_ACCOUNT, {}};
    out.newest_accounts = *newest_accounts;
    out.oldest_accounts = *oldest_accounts;
    std::reverse(out.headers.begin(), out.headers.end());
    std::sort(out.receipts.begin(), out.receipts.end(), [](const auto& a, const auto& b) {
        return std::pair{a.block_number, a.transaction_index} < std::pair{b.block_number, b.transaction_index};
    });
    return {Error::OK, std::move(out)};
}

enum class EventKind { DEPOSIT, WITHDRAWN };
struct Event {
    EventKind kind;
    EthAddress vault{}, address{}; // sender for deposit; recipient for payout
    uint64_t block_number{0}, transaction_index{0}, log_index{0}, deposit_id{0};
    uint256 block_hash{}, transaction_hash{}, account{}, asset{}, withdrawal_id{};
    StorageWord amount{};
};
inline uint256 Topic(std::string_view text)
{
    return detail::Hash({reinterpret_cast<const unsigned char*>(text.data()), text.size()});
}
inline bool PaddedAddress(std::span<const unsigned char> word, EthAddress& address)
{
    if (word.size() != 32 || detail::Nonzero(word.first(12))) return false;
    std::copy(word.begin() + 12, word.end(), address.begin());
    return detail::Nonzero(address);
}

/** Candidate events ONLY, not native ledger facts or a source authority.
 * The complete range must be checked before calling. Global log indices count
 * every log, including unrelated/system logs, and reset at each block.
 */
inline std::optional<std::vector<Event>> ExtractEvents(const Evidence& evidence,
                                                      const std::vector<EthAddress>& vaults)
{
    if (vaults.size() > MAX_ACCOUNTS) return std::nullopt;
    std::set<EthAddress> watched;
    for (const auto& vault : vaults) {
        if (!detail::Nonzero(vault) || !watched.insert(vault).second) return std::nullopt;
    }
    const auto deposit_topic{Topic("Deposit(uint64,bytes32,bytes32,uint256,address)")};
    const auto paid_topic{Topic("Withdrawn(bytes32,address,uint256)")};
    std::map<uint64_t, uint256> hashes;
    for (const auto& header : evidence.headers) hashes.emplace(header.number, header.hash);
    std::vector<Event> events;
    uint64_t block{0}, index{0};
    bool first{true};
    for (const auto& located : evidence.receipts) {
        if (first || block != located.block_number) { first = false; block = located.block_number; index = 0; }
        const auto found{hashes.find(block)};
        if (found == hashes.end()) return std::nullopt;
        for (const auto& log : located.receipt.logs) {
            const uint64_t log_index{index++};
            if (!watched.contains(log.address) || log.topics.empty()) continue;
            const bool deposit{log.topics[0] == deposit_topic}, paid{log.topics[0] == paid_topic};
            if (!deposit && !paid) continue;
            if (!located.receipt.status || events.size() >= 4096) return std::nullopt;
            Event event;
            event.kind = deposit ? EventKind::DEPOSIT : EventKind::WITHDRAWN;
            event.vault = log.address;
            event.block_number = block; event.block_hash = found->second;
            event.transaction_index = located.transaction_index;
            event.transaction_hash = located.transaction_hash; event.log_index = log_index;
            if (deposit) {
                if (log.topics.size() != 4 || log.data.size() != 64 ||
                    detail::Nonzero(detail::Raw(log.topics[1]).first(24)) ||
                    log.topics[2].IsNull() || log.topics[3].IsNull() ||
                    !PaddedAddress(std::span{log.data}.subspan(32), event.address)) return std::nullopt;
                for (const auto byte : detail::Raw(log.topics[1]).subspan(24)) event.deposit_id = (event.deposit_id << 8) | byte;
                event.account = log.topics[2]; event.asset = log.topics[3];
                std::copy_n(log.data.begin(), 32, event.amount.begin());
            } else {
                if (log.topics.size() != 3 || log.data.size() != 32 || log.topics[1].IsNull() ||
                    !PaddedAddress(detail::Raw(log.topics[2]), event.address)) return std::nullopt;
                event.withdrawal_id = log.topics[1];
                std::copy_n(log.data.begin(), 32, event.amount.begin());
            }
            if (!detail::Nonzero(event.amount)) return std::nullopt;
            events.push_back(std::move(event));
        }
    }
    return events;
}

} // namespace bridge::arbitrum::history
#endif // B3COIN_BRIDGE_ARBITRUM_HISTORY_H
