// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bridge/arbitrum_history.h>
#include <test/util/bridge_eth_trie.h>
#include <boost/test/unit_test.hpp>
#include <type_traits>

namespace arb = bridge::arbitrum;
namespace hist = arb::history;
using namespace bridge;

BOOST_AUTO_TEST_SUITE(bridge_arbitrum_history_tests)
namespace {
uint256 Hash(unsigned char tag) { uint256 value; value.begin()[0] = tag; return value; }
EthAddress Address(unsigned char tag) { EthAddress value{}; value[0] = tag; return value; }
arb::Bytes Receipt(uint64_t gas, const std::vector<EthLog>& logs = {}, bool success = true)
{
    std::vector<arb::Bytes> encoded_logs;
    for (const auto& log : logs) {
        std::vector<arb::Bytes> topics;
        for (const auto& topic : log.topics) topics.push_back(RlpEncodeBytes(arb::detail::Raw(topic)));
        encoded_logs.push_back(RlpEncodeList({RlpEncodeBytes(log.address), RlpEncodeList(topics), RlpEncodeBytes(log.data)}));
    }
    auto encoded{RlpEncodeList({RlpEncodeUint64(success ? 1 : 0), RlpEncodeUint64(gas),
        RlpEncodeBytes(arb::Bytes(256)), RlpEncodeList(encoded_logs)})};
    encoded.insert(encoded.begin(), 0x64); // actual Nitro system-envelope range
    return encoded;
}
arb::BlockWitness Block(uint64_t number, uint256 parent, std::vector<arb::IndexedReceipt> receipts = {},
                        const uint256& state_root = EMPTY_TRIE_ROOT)
{
    arb::BlockWitness out;
    out.receipts = std::move(receipts);
    for (size_t i{0}; i < out.receipts.size(); ++i) out.transactions.push_back({i, {0x64, 0xc0, static_cast<unsigned char>(i)}});
    const auto transactions{arb::detail::TransactionRoot(out.transactions)}, root{arb::detail::ReceiptRoot(out.receipts)};
    BOOST_REQUIRE(transactions); BOOST_REQUIRE(root);
    const auto gas{out.receipts.empty() ? 0 : DecodeReceipt(out.receipts.back().encoded, RECEIPT_MAX_TYPE_V2)->cumulative_gas};
    out.header = RlpEncodeList({RlpEncodeBytes(arb::detail::Raw(parent)), RlpEncodeBytes(arb::detail::Raw(Hash(1))),
        RlpEncodeBytes(Address(2)), RlpEncodeBytes(arb::detail::Raw(state_root)),
        RlpEncodeBytes(arb::detail::Raw(*transactions)), RlpEncodeBytes(arb::detail::Raw(*root)),
        RlpEncodeBytes(arb::Bytes(256)), RlpEncodeUint64(0), RlpEncodeUint64(number), RlpEncodeUint64(10000000),
        RlpEncodeUint64(gas), RlpEncodeUint64(number * 10), RlpEncodeBytes({}),
        RlpEncodeBytes(arb::detail::Raw(Hash(3))), RlpEncodeBytes(arb::Bytes(8)), RlpEncodeUint64(1)});
    return out;
}
struct Sample {
    arb::Proof proof;
    arb::RangeRequest request;
    explicit Sample(std::vector<arb::IndexedReceipt> receipts = {})
    {
        proof.blocks.push_back(Block(10, Hash(7), std::move(receipts)));
        request = {9, 10, Hash(7), arb::detail::Hash(proof.blocks[0].header)};
    }
};
struct StateSample {
    arb::Proof proof;
    arb::RangeRequest request;
    EthAddress account{Address(9)};
    StorageWord present_slot{SlotWord(2)}, absent_slot{SlotWord(3)};
    uint256 old_code{Hash(21)}, new_code{Hash(22)};
    StateSample()
    {
        bridge_test::EthStorageTrie old_storage, new_storage;
        old_storage.Set(present_slot, SlotWord(123));
        new_storage.Set(present_slot, SlotWord(456));
        // An additional storage branch makes both membership and absence
        // proofs walk an actual secure trie rather than a fabricated word.
        old_storage.Set(SlotWord(7), SlotWord(777));
        new_storage.Set(SlotWord(7), SlotWord(888));
        bridge_test::EthTrie old_state, new_state;
        old_state.InsertSecure(account, bridge_test::EncodeAccount(41, SlotWord(100), old_storage.Root(), old_code));
        new_state.InsertSecure(account, bridge_test::EncodeAccount(42, SlotWord(200), new_storage.Root(), new_code));
        const auto other{Address(10)};
        old_state.InsertSecure(other, bridge_test::EncodeAccount(1, SlotWord(1), EMPTY_TRIE_ROOT, Hash(23)));
        new_state.InsertSecure(other, bridge_test::EncodeAccount(2, SlotWord(2), EMPTY_TRIE_ROOT, Hash(24)));
        proof.historical_accounts.push_back({account, old_state.ProofSecure(account),
            {{present_slot, old_storage.Proof(present_slot)}, {absent_slot, old_storage.Proof(absent_slot)}}});
        proof.confirmed_accounts.push_back({account, new_state.ProofSecure(account),
            {{present_slot, new_storage.Proof(present_slot)}, {absent_slot, new_storage.Proof(absent_slot)}}});
        auto older{Block(10, Hash(7), {}, old_state.Root())};
        auto newer{Block(11, arb::detail::Hash(older.header), {}, new_state.Root())};
        request = {9, 11, Hash(7), arb::detail::Hash(newer.header)};
        proof.blocks.push_back(std::move(newer)); proof.blocks.push_back(std::move(older));
    }
};
EthLog Deposit(const EthAddress& vault)
{
    auto deposit_id{SlotWord(17)};
    EthLog log{vault, {hist::Topic("Deposit(uint64,bytes32,bytes32,uint256,address)"),
        uint256{deposit_id}, Hash(4), Hash(5)}, {}};
    const auto amount{SlotWord(123456789012345ULL)};
    log.data.insert(log.data.end(), amount.begin(), amount.end());
    log.data.resize(64);
    const auto sender{Address(6)};
    std::copy(sender.begin(), sender.end(), log.data.begin() + 44);
    return log;
}
}

BOOST_AUTO_TEST_CASE(empty_complete_range_has_no_authority_or_events)
{
    Sample sample;
    const auto result{hist::CheckRange(sample.request, sample.proof)};
    BOOST_REQUIRE(result.evidence);
    BOOST_CHECK(result.error == arb::Error::OK);
    BOOST_CHECK(result.evidence->headers.size() == 1);
    BOOST_CHECK(result.evidence->receipts.empty());
    BOOST_REQUIRE(hist::ExtractEvents(*result.evidence, {}));
    static_assert(!std::is_convertible_v<hist::Evidence, arb::ConfirmedRange>);
}
BOOST_AUTO_TEST_CASE(complete_receipt_and_transaction_roots_are_required)
{
    Sample sample{{{0, Receipt(100)}}};
    BOOST_REQUIRE(hist::CheckRange(sample.request, sample.proof).evidence);
    auto changed{sample.proof}; changed.blocks[0].receipts[0].encoded.back() ^= 1;
    BOOST_CHECK(!hist::CheckRange(sample.request, changed).evidence);
    changed = sample.proof; changed.blocks[0].transactions[0].encoded.back() ^= 1;
    BOOST_CHECK(hist::CheckRange(sample.request, changed).error == arb::Error::BAD_TRANSACTIONS);
    changed = sample.proof; changed.blocks[0].transactions.clear();
    BOOST_CHECK(!hist::CheckRange(sample.request, changed).evidence);
    changed = sample.proof; changed.blocks[0].receipts.clear();
    BOOST_CHECK(!hist::CheckRange(sample.request, changed).evidence);
}
BOOST_AUTO_TEST_CASE(caller_range_cannot_be_replaced_by_proof_metadata)
{
    Sample sample;
    auto request{sample.request}; request.to_hash = Hash(8);
    BOOST_CHECK(hist::CheckRange(request, sample.proof).error == arb::Error::BAD_ANCESTRY);
    request = sample.request; request.previous_hash = Hash(8);
    BOOST_CHECK(hist::CheckRange(request, sample.proof).error == arb::Error::BAD_ANCESTRY);
    request = sample.request; request.previous_height = 8;
    BOOST_CHECK(hist::CheckRange(request, sample.proof).error == arb::Error::BAD_ANCESTRY);
    sample.proof.assertion.block_hash = sample.request.to_hash;
    BOOST_CHECK(hist::CheckRange(sample.request, sample.proof).error == arb::Error::BAD_ASSERTION);
}
BOOST_AUTO_TEST_CASE(contiguous_headers_are_required_and_output_is_oldest_first)
{
    Sample sample;
    const auto newer{Block(11, sample.request.to_hash)};
    sample.proof.blocks.insert(sample.proof.blocks.begin(), newer);
    sample.request.to_height = 11; sample.request.to_hash = arb::detail::Hash(newer.header);
    const auto valid{hist::CheckRange(sample.request, sample.proof)};
    BOOST_REQUIRE(valid.evidence);
    BOOST_CHECK_EQUAL(valid.evidence->headers[0].number, 10U);
    BOOST_CHECK_EQUAL(valid.evidence->headers[1].number, 11U);
    std::swap(sample.proof.blocks[0], sample.proof.blocks[1]);
    BOOST_CHECK(!hist::CheckRange(sample.request, sample.proof).evidence);
}
BOOST_AUTO_TEST_CASE(unknown_header_extensions_are_not_assumed_compatible)
{
    Sample sample;
    auto top{RlpDecode(sample.proof.blocks[0].header)};
    BOOST_REQUIRE(top);
    const auto fields{RlpChildren(*top)};
    BOOST_REQUIRE(fields);
    std::vector<arb::Bytes> next;
    for (const auto& field : *fields) next.push_back(RlpEncodeBytes(field.payload));
    next.push_back(RlpEncodeBytes({}));
    sample.proof.blocks[0].header = RlpEncodeList(next);
    sample.request.to_hash = arb::detail::Hash(sample.proof.blocks[0].header);
    BOOST_CHECK(hist::CheckRange(sample.request, sample.proof).error == arb::Error::BAD_HEADER);
}
BOOST_AUTO_TEST_CASE(cumulative_gas_and_failed_logs_must_be_consistent)
{
    Sample sample{{{0, Receipt(100)}, {1, Receipt(50)}}};
    BOOST_CHECK(hist::CheckRange(sample.request, sample.proof).error == arb::Error::BAD_RECEIPTS);
    Sample failed{{{0, Receipt(100, {Deposit(Address(9))}, false)}}};
    BOOST_CHECK(hist::CheckRange(failed.request, failed.proof).error == arb::Error::BAD_RECEIPTS);
}
BOOST_AUTO_TEST_CASE(global_log_index_counts_unrelated_and_system_logs)
{
    const auto vault{Address(9)};
    const auto unrelated{EthLog{Address(8), {}, {0x10}}};
    Sample sample{{{0, Receipt(100, {unrelated, unrelated})}, {1, Receipt(200, {unrelated, Deposit(vault)})}}};
    const auto checked{hist::CheckRange(sample.request, sample.proof)};
    BOOST_REQUIRE(checked.evidence);
    const auto events{hist::ExtractEvents(*checked.evidence, {vault})};
    BOOST_REQUIRE(events); BOOST_REQUIRE_EQUAL(events->size(), 1U);
    const auto& event{events->front()};
    BOOST_CHECK_EQUAL(event.log_index, 3U); BOOST_CHECK_EQUAL(event.transaction_index, 1U);
    BOOST_CHECK_EQUAL(event.deposit_id, 17U); BOOST_CHECK(event.amount == SlotWord(123456789012345ULL));
    BOOST_CHECK(event.address == Address(6)); BOOST_CHECK(event.account == Hash(4));
    BOOST_CHECK(event.transaction_hash == arb::detail::Hash(sample.proof.blocks[0].transactions[1].encoded));
}
BOOST_AUTO_TEST_CASE(watched_vaults_and_exact_event_abi_are_required)
{
    const auto vault{Address(9)};
    auto log{Deposit(vault)}; log.data[32] = 1; // nonzero address padding
    Sample sample{{{0, Receipt(100, {log})}}};
    const auto checked{hist::CheckRange(sample.request, sample.proof)};
    BOOST_REQUIRE(checked.evidence);
    BOOST_CHECK(!hist::ExtractEvents(*checked.evidence, {vault}));
    const auto other{hist::ExtractEvents(*checked.evidence, {Address(8)})};
    BOOST_REQUIRE(other); BOOST_CHECK(other->empty());
    BOOST_CHECK(!hist::ExtractEvents(*checked.evidence, {vault, vault}));
    BOOST_CHECK(!hist::ExtractEvents(*checked.evidence, {EthAddress{}}));
}
BOOST_AUTO_TEST_CASE(payout_event_preserves_exact_uint256_amount_and_recipient)
{
    const auto vault{Address(9)}, recipient{Address(8)};
    StorageWord address_word{}; std::copy(recipient.begin(), recipient.end(), address_word.begin() + 12);
    arb::Bytes amount(32, 0xff);
    EthLog paid{vault, {hist::Topic("Withdrawn(bytes32,address,uint256)"), Hash(6), uint256{address_word}}, amount};
    Sample sample{{{0, Receipt(100, {paid})}}};
    const auto checked{hist::CheckRange(sample.request, sample.proof)};
    BOOST_REQUIRE(checked.evidence);
    const auto events{hist::ExtractEvents(*checked.evidence, {vault})};
    BOOST_REQUIRE(events); BOOST_REQUIRE_EQUAL(events->size(), 1U);
    BOOST_CHECK(events->front().kind == hist::EventKind::WITHDRAWN);
    BOOST_CHECK(events->front().address == recipient); BOOST_CHECK(events->front().withdrawal_id == Hash(6));
    BOOST_CHECK(std::all_of(events->front().amount.begin(), events->front().amount.end(), [](auto b) { return b == 0xff; }));
}
BOOST_AUTO_TEST_CASE(unproved_accounts_and_l1_assertions_cannot_be_smuggled_in)
{
    Sample sample;
    sample.proof.confirmed_accounts.push_back({Address(1), {{0xc0}}, {}});
    BOOST_CHECK(hist::CheckRange(sample.request, sample.proof).error == arb::Error::BAD_ACCOUNT);
    sample.proof.confirmed_accounts.clear();
    sample.proof.l1_accounts.push_back({Address(1), {{0xc0}}, {}});
    BOOST_CHECK(hist::CheckRange(sample.request, sample.proof).error == arb::Error::BAD_ASSERTION);
}
BOOST_AUTO_TEST_CASE(distinct_newest_and_oldest_account_and_storage_proofs_are_verified)
{
    StateSample sample;
    const auto checked{hist::CheckRange(sample.request, sample.proof)};
    BOOST_REQUIRE(checked.evidence);
    BOOST_REQUIRE_EQUAL(checked.evidence->newest_accounts.size(), 1U);
    BOOST_REQUIRE_EQUAL(checked.evidence->oldest_accounts.size(), 1U);
    const auto& newest{checked.evidence->newest_accounts.front()};
    const auto& oldest{checked.evidence->oldest_accounts.front()};
    BOOST_CHECK(newest.address == sample.account); BOOST_CHECK(oldest.address == sample.account);
    BOOST_CHECK_EQUAL(newest.account.nonce, 42U); BOOST_CHECK_EQUAL(oldest.account.nonce, 41U);
    BOOST_CHECK(newest.account.balance == SlotWord(200)); BOOST_CHECK(oldest.account.balance == SlotWord(100));
    BOOST_CHECK(newest.account.code_hash == sample.new_code); BOOST_CHECK(oldest.account.code_hash == sample.old_code);
    BOOST_CHECK(newest.account.storage_root != oldest.account.storage_root);
    BOOST_REQUIRE_EQUAL(newest.storage.size(), 2U); BOOST_REQUIRE_EQUAL(oldest.storage.size(), 2U);
    BOOST_CHECK(newest.storage[0].slot == sample.present_slot); BOOST_CHECK(newest.storage[0].value == SlotWord(456));
    BOOST_CHECK(oldest.storage[0].slot == sample.present_slot); BOOST_CHECK(oldest.storage[0].value == SlotWord(123));
    BOOST_CHECK(newest.storage[1].slot == sample.absent_slot); BOOST_CHECK(newest.storage[1].value == StorageWord{});
    BOOST_CHECK(oldest.storage[1].slot == sample.absent_slot); BOOST_CHECK(oldest.storage[1].value == StorageWord{});
}
BOOST_AUTO_TEST_CASE(account_and_storage_proofs_cannot_borrow_the_other_header_root)
{
    StateSample sample;
    auto changed{sample.proof};
    std::swap(changed.confirmed_accounts, changed.historical_accounts);
    BOOST_CHECK(hist::CheckRange(sample.request, changed).error == arb::Error::BAD_ACCOUNT);
    changed = sample.proof;
    changed.confirmed_accounts[0].nodes = sample.proof.historical_accounts[0].nodes;
    BOOST_CHECK(hist::CheckRange(sample.request, changed).error == arb::Error::BAD_ACCOUNT);
    changed = sample.proof;
    changed.historical_accounts[0].nodes = sample.proof.confirmed_accounts[0].nodes;
    BOOST_CHECK(hist::CheckRange(sample.request, changed).error == arb::Error::BAD_ACCOUNT);
    changed = sample.proof;
    changed.confirmed_accounts[0].storage[0].nodes = sample.proof.historical_accounts[0].storage[0].nodes;
    BOOST_CHECK(hist::CheckRange(sample.request, changed).error == arb::Error::BAD_ACCOUNT);
    changed = sample.proof;
    changed.historical_accounts[0].nodes[0].back() ^= 1;
    BOOST_CHECK(hist::CheckRange(sample.request, changed).error == arb::Error::BAD_ACCOUNT);
}
BOOST_AUTO_TEST_CASE(duplicate_and_missing_account_or_storage_proofs_refuse)
{
    StateSample sample;
    auto changed{sample.proof};
    changed.confirmed_accounts.push_back(changed.confirmed_accounts.front());
    BOOST_CHECK(hist::CheckRange(sample.request, changed).error == arb::Error::BAD_ACCOUNT);
    changed = sample.proof;
    changed.historical_accounts[0].storage.push_back(changed.historical_accounts[0].storage.front());
    BOOST_CHECK(hist::CheckRange(sample.request, changed).error == arb::Error::BAD_ACCOUNT);
    changed = sample.proof; changed.confirmed_accounts[0].nodes.clear();
    BOOST_CHECK(hist::CheckRange(sample.request, changed).error == arb::Error::BAD_ACCOUNT);
    changed = sample.proof; changed.historical_accounts[0].storage[0].nodes.clear();
    BOOST_CHECK(hist::CheckRange(sample.request, changed).error == arb::Error::BAD_ACCOUNT);
}
BOOST_AUTO_TEST_SUITE_END()
