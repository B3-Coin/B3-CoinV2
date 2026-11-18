// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bridge/arbitrum_source.h>
#include <test/util/bridge_eth_trie.h>
#include <test/data/eth_lc_fixture.h>
#include <util/strencodings.h>
#include <boost/test/unit_test.hpp>

#include <array>
#include <type_traits>

using namespace bridge;
namespace arb = bridge::arbitrum;
namespace fix = arb::fixture;
namespace fx = eth_lc_fixture;

BOOST_AUTO_TEST_SUITE(bridge_arbitrum_source_tests)
namespace {
using Bytes = arb::Bytes;
using bridge_test::EthTrie;
using bridge_test::EthStorageTrie;

std::vector<unsigned char> Hex(const std::string& h)
{
    auto v{TryParseHex<unsigned char>(h)};
    BOOST_REQUIRE_MESSAGE(v.has_value(), "bad hex in fixture");
    return *v;
}

uint256 Hex32(const std::string& h)
{
    const auto v{Hex(h)};
    BOOST_REQUIRE_EQUAL(v.size(), 32U);
    return uint256{std::span<const unsigned char>{v}};
}

template <size_t N>
std::array<unsigned char, N> HexArr(const std::string& h)
{
    const auto v{Hex(h)};
    BOOST_REQUIRE_EQUAL(v.size(), N);
    std::array<unsigned char, N> out;
    std::copy(v.begin(), v.end(), out.begin());
    return out;
}

std::vector<uint256> Branch(const std::vector<std::string>& b)
{
    std::vector<uint256> out;
    out.reserve(b.size());
    for (const auto& r : b) out.push_back(Hex32(r));
    return out;
}

ssz::BeaconBlockHeader Beacon(const fx::BeaconHeaderHex& h)
{
    return {h.slot, h.proposer_index, Hex32(h.parent_root), Hex32(h.state_root), Hex32(h.body_root)};
}

ssz::ExecutionPayloadHeader Exec(const fx::ExecHeaderHex& e)
{
    ssz::ExecutionPayloadHeader out;
    out.parent_hash = Hex32(e.parent_hash);
    out.fee_recipient = HexArr<20>(e.fee_recipient);
    out.state_root = Hex32(e.state_root);
    out.receipts_root = Hex32(e.receipts_root);
    out.logs_bloom = HexArr<256>(e.logs_bloom);
    out.prev_randao = Hex32(e.prev_randao);
    out.block_number = e.block_number;
    out.gas_limit = e.gas_limit;
    out.gas_used = e.gas_used;
    out.timestamp = e.timestamp;
    out.extra_data = Hex(e.extra_data);
    out.base_fee_per_gas = Hex32(e.base_fee_le);
    out.block_hash = Hex32(e.block_hash);
    out.transactions_root = Hex32(e.transactions_root);
    out.withdrawals_root = Hex32(e.withdrawals_root);
    out.blob_gas_used = e.blob_gas_used;
    out.excess_blob_gas = e.excess_blob_gas;
    return out;
}

ssz::SyncCommittee Committee(const std::string& pubkeys_hex, const std::string& agg_hex)
{
    const auto raw{Hex(pubkeys_hex)};
    BOOST_REQUIRE_EQUAL(raw.size(), ssz::SYNC_COMMITTEE_SIZE * 48);
    ssz::SyncCommittee c;
    c.pubkeys.resize(ssz::SYNC_COMMITTEE_SIZE);
    for (size_t i = 0; i < ssz::SYNC_COMMITTEE_SIZE; ++i) {
        std::copy(raw.begin() + i * 48, raw.begin() + (i + 1) * 48, c.pubkeys[i].begin());
    }
    c.aggregate_pubkey = HexArr<48>(agg_hex);
    return c;
}

LightClientConfig Config()
{
    LightClientConfig cfg;
    cfg.genesis_validators_root = Hex32(fx::GENESIS_VALIDATORS_ROOT);
    for (const auto& f : fx::FORKS) {
        ForkVersion fv;
        fv.epoch = f.epoch;
        fv.version = HexArr<4>(f.version);
        cfg.forks.push_back(fv);
        if (fv.version[0] == 0x05) cfg.electra_epoch = fv.epoch; // Electra moved the gindices
    }
    BOOST_REQUIRE(cfg.electra_epoch != UINT64_MAX);
    return cfg;
}

LightClientUpdate Update1()
{
    LightClientUpdate u;
    u.attested = {Beacon(fx::U1_ATTESTED_BEACON), Exec(fx::U1_ATTESTED_EXEC), Branch(fx::U1_ATTESTED_EXEC_BRANCH)};
    u.finalized = {Beacon(fx::U1_FINALIZED_BEACON), Exec(fx::U1_FINALIZED_EXEC), Branch(fx::U1_FINALIZED_EXEC_BRANCH)};
    u.finality_branch = Branch(fx::U1_FINALITY_BRANCH);
    u.has_next = true;
    u.next_committee = Committee(fx::U1_NEXT_PUBKEYS, fx::U1_NEXT_AGG);
    u.next_branch = Branch(fx::U1_NEXT_BRANCH);
    u.sync_aggregate.bits = HexArr<64>(fx::U1_SYNC_BITS);
    u.sync_aggregate.signature = HexArr<96>(fx::U1_SYNC_SIG);
    u.signature_slot = fx::U1_SIGNATURE_SLOT;
    return u;
}

peer::Policy NativePolicy()
{
    peer::Policy policy;
    policy.config = Config();
    policy.trusted_root = Hex32(fx::BOOTSTRAP_ROOT);
    policy.trusted_slot = fx::U1_FINALIZED_BEACON.slot;
    policy.fork_schedule_valid_through_epoch = 479'999;
    policy.max_sync_lag_slots = SLOTS_PER_PERIOD;
    policy.max_recovery_lag_slots = SLOTS_PER_PERIOD;
    BOOST_REQUIRE(policy.Valid());
    return policy;
}

std::vector<unsigned char> native_bootstrap()
{
    const BridgeBootstrapV1 bootstrap{
        {Beacon(fx::U1_FINALIZED_BEACON), Exec(fx::U1_FINALIZED_EXEC), Branch(fx::U1_FINALIZED_EXEC_BRANCH)},
        Committee(fx::BOOTSTRAP_COMMITTEE_PUBKEYS, fx::BOOTSTRAP_COMMITTEE_AGG),
        Branch(fx::BOOTSTRAP_COMMITTEE_BRANCH)};
    auto record{EncodeBridgeRecordV1({BridgeRecordKindV1::BOOTSTRAP, bootstrap})};
    BOOST_REQUIRE(record);
    return *record;
}



uint256 Tag(unsigned char byte) { return uint256{Bytes(32, byte)}; }
EthAddress Address(unsigned char byte) { EthAddress out{}; out.fill(byte); return out; }
StorageWord Word(unsigned char byte) { StorageWord out{}; out.fill(byte); return out; }
Bytes Raw(const uint256& h) { return {h.begin(), h.end()}; }
Bytes Receipt(uint64_t gas, unsigned char type = 0, bool success = true)
{
    const Bytes log{RlpEncodeList({RlpEncodeBytes(Address(0x33)),
                                   RlpEncodeList({RlpEncodeBytes(Raw(Tag(0x44)))}),
                                   RlpEncodeBytes(Bytes{0x55})})};
    auto out{RlpEncodeList({RlpEncodeUint64(success ? 1 : 0), RlpEncodeUint64(gas),
                            RlpEncodeBytes(Bytes(256, 0)),
                            RlpEncodeList(success ? std::vector<Bytes>{log} : std::vector<Bytes>{})})};
    if (type) out.insert(out.begin(), type);
    return out;
}
uint256 IndependentRoot(const std::vector<arb::IndexedReceipt>& receipts)
{
    EthTrie trie;
    for (const auto& r : receipts) trie.Insert(RlpEncodeUint64(r.index), r.encoded);
    return trie.Root();
}
Bytes Transaction(uint64_t nonce, bool typed = false)
{
    auto out{RlpEncodeList({RlpEncodeUint64(nonce), RlpEncodeBytes(Address(0x22)),
                            RlpEncodeBytes(Bytes{0xab, 0xcd})})};
    if (typed) out.insert(out.begin(), 0x64);
    return out;
}
uint256 IndependentTxRoot(const std::vector<arb::IndexedTransaction>& transactions)
{
    EthTrie trie;
    for (const auto& tx : transactions) trie.Insert(RlpEncodeUint64(tx.index), tx.encoded);
    return trie.Root();
}
Bytes Header(uint64_t number, const uint256& parent, const uint256& state,
             const uint256& receipts, uint64_t gas)
{
    return RlpEncodeList({RlpEncodeBytes(Raw(parent)), RlpEncodeBytes(Raw(Tag(0x11))),
                          RlpEncodeBytes(Address(0x22)), RlpEncodeBytes(Raw(state)),
                          RlpEncodeBytes(Raw(IndependentTxRoot({}))), RlpEncodeBytes(Raw(receipts)),
                          RlpEncodeBytes(Bytes(256, 0)), RlpEncodeUint64(0),
                          RlpEncodeUint64(number), RlpEncodeUint64(1'000'000),
                          RlpEncodeUint64(gas), RlpEncodeUint64(1'000 + number),
                          RlpEncodeBytes({}), RlpEncodeBytes(Raw(Tag(0x77))),
                          RlpEncodeBytes(Bytes(8, 0)), RlpEncodeUint64(1)});
}
Bytes Field(const Bytes& header, size_t index, const Bytes& replacement)
{
    const auto top{RlpDecode(header)};
    const auto fields{RlpChildren(*top)};
    std::vector<Bytes> out;
    for (size_t i{0}; i < fields->size(); ++i) {
        const auto enc{RlpEncodedSpan((*fields)[i], header)};
        out.push_back(i == index ? replacement : Bytes{enc.begin(), enc.end()});
    }
    return RlpEncodeList(out);
}
struct Rig {
    fix::Policy policy;
    arb::Proof proof;
    arb::RangeRequest request;
    uint256 l1_root;
    EthStorageTrie rollup_storage, token_storage;
    EthTrie l1_state, l2_state;
    Rig()
    {
        policy.rollup = Address(0x10);
        policy.latest_confirmed_slot = SlotWord(12);
        policy.l1_accounts.push_back({policy.rollup, Tag(0x20), {{SlotWord(11), Word(0x30)}}});
        policy.historical_accounts.push_back({Address(0x40), Tag(0x50), {{SlotWord(2), Word(0x60)}, {SlotWord(3), {}}}});
        policy.confirmed_accounts = policy.historical_accounts;
        token_storage.Set(SlotWord(2), Word(0x60));
        l2_state.InsertSecure(Address(0x40), bridge_test::EncodeAccount(1, {}, token_storage.Root(), Tag(0x50)));
        proof.historical_accounts.push_back({Address(0x40), l2_state.ProofSecure(Address(0x40)),
                                      {{SlotWord(2), token_storage.Proof(SlotWord(2))},
                                       {SlotWord(3), token_storage.Proof(SlotWord(3))}}});
        proof.confirmed_accounts = proof.historical_accounts;
        arb::BlockWitness oldest;
        oldest.header = Header(8, Tag(0x70), l2_state.Root(), IndependentRoot({}), 0);
        arb::BlockWitness newest;
        newest.receipts = {{0, Receipt(21)}, {1, Receipt(42, 0x64, false)}};
        newest.transactions = {{0, Transaction(1)}, {1, Transaction(2, true)}};
        newest.header = Header(9, arb::detail::Hash(oldest.header), l2_state.Root(), IndependentRoot(newest.receipts), 42);
        newest.header = Field(newest.header, 4, RlpEncodeBytes(Raw(IndependentTxRoot(newest.transactions))));
        proof.blocks = {newest, oldest};
        proof.assertion.parent_assertion = Tag(1);
        proof.assertion.block_hash = arb::detail::Hash(newest.header);
        request = {7, 9, Tag(0x70), proof.assertion.block_hash};
        proof.assertion.send_root = Tag(2);
        proof.assertion.inbox_position = 3;
        proof.assertion.position_in_message = 4;
        proof.assertion.end_history_root = Tag(5);
        proof.assertion.inbox_accumulator = Tag(6);
        RefreshL1();
    }
    void RefreshL1()
    {
        rollup_storage.Set(SlotWord(11), Word(0x30));
        const auto assertion{arb::detail::AssertionHash(proof.assertion)};
        StorageWord value{};
        std::copy(assertion.begin(), assertion.end(), value.begin());
        rollup_storage.Set(policy.latest_confirmed_slot, value);
        l1_state.InsertSecure(policy.rollup, bridge_test::EncodeAccount(1, {}, rollup_storage.Root(), Tag(0x20)));
        l1_root = l1_state.Root();
        proof.l1_accounts = {{policy.rollup, l1_state.ProofSecure(policy.rollup),
                               {{SlotWord(11), rollup_storage.Proof(SlotWord(11))},
                                {policy.latest_confirmed_slot, rollup_storage.Proof(policy.latest_confirmed_slot)}}}};
    }
    void RebindNewest()
    {
        proof.assertion.block_hash = arb::detail::Hash(proof.blocks[0].header);
        request.to_hash = proof.assertion.block_hash;
        RefreshL1();
    }
    fix::Reply Verify() const { return fix::Verify(l1_root, policy, request, proof); }
};
void Refused(const Rig& rig, arb::Error error)
{
    const auto reply{rig.Verify()};
    BOOST_CHECK(reply.error == error);
    BOOST_CHECK(!reply.evidence);
}
} // namespace

BOOST_AUTO_TEST_CASE(valid_fixture_proves_complete_receipts_state_and_absent_storage)
{
    Rig rig;
    const auto result{rig.Verify()};
    BOOST_REQUIRE(result.error == arb::Error::OK);
    BOOST_REQUIRE(result.evidence);
    BOOST_CHECK(!result.evidence->MainnetEnabled());
    BOOST_CHECK_EQUAL(result.evidence->confirmed_head.number, 9U);
    BOOST_CHECK_EQUAL(result.evidence->oldest_header.number, 8U);
    BOOST_REQUIRE_EQUAL(result.evidence->receipts.size(), 2U);
    BOOST_CHECK_EQUAL(result.evidence->receipts[0].transaction_index, 0U);
    BOOST_CHECK_EQUAL(result.evidence->receipts[1].receipt.type, 0x64);
    BOOST_CHECK(!result.evidence->receipts[1].receipt.status);
    BOOST_CHECK(result.evidence->receipts[0].transaction_hash == arb::detail::Hash(Transaction(1)));
    BOOST_CHECK(result.evidence->receipts[1].transaction_hash == arb::detail::Hash(Transaction(2, true)));
}

BOOST_AUTO_TEST_CASE(receipt_trie_matches_independent_builder_across_index_boundaries)
{
    // Independently derived with py-trie HexaryTrie 3.1.0 + rlp 5.0.0,
    // eth-hash/PyCryptodome, not this implementation or the C++ test trie.
    // See work derivation/provenance; these constants have no runtime deps.
    const std::array<std::pair<size_t, const char*>, 7> vectors{{
        {0, "56e81f171bcc55a6ff8345e692c0f86e5b48e01b996cadc001622fb5e363b421"},
        {1, "39fa29e713819f796d451181c8bbc457d1e6001a31f948ff5ec944ec3ab1b704"},
        {2, "d2b4fd7abcf844cd37da361e7f8bb331d6a83a8a0d7ad26529af2cf00190e3a0"},
        {17, "ccc0df39a397d5fdcdf77a69eedd43ead735e311ccc2b041859acb29a1632319"},
        {128, "fa33de01a0bdb7335bbb0918ebc5e85ab358a847e7f54b5da5e870b7594c0ad7"},
        {129, "67685aadf7a21786566878b843af4c6e7968c5c700ade8b3694086fb1363232d"},
        {257, "f5018c18d7e683f8302a201624c66cc17b3a9f7b5f674207f4baba5efc7be135"},
    }};
    for (const auto& [count, expected] : vectors) {
        std::vector<arb::IndexedReceipt> receipts;
        for (size_t i{0}; i < count; ++i) receipts.push_back({i, Receipt(i + 1, i % 2 ? 0x64 : 0)});
        const auto result{arb::detail::ReceiptRoot(receipts)};
        BOOST_REQUIRE(result);
        BOOST_CHECK(*result == IndependentRoot(receipts));
        BOOST_CHECK(*result == Hex32(expected));
    }
    const auto typed{arb::detail::ReceiptRoot({{0, Receipt(1, 0x64)}})};
    BOOST_REQUIRE(typed);
    BOOST_CHECK(*typed == Hex32("2846d7ade10c1ca4e0a123d635086e4ecbe821a82a9ecaa110e3113d82549c09"));
}

BOOST_AUTO_TEST_CASE(missing_extra_reordered_or_modified_receipts_are_refused)
{
    for (unsigned int mutation{0}; mutation < 4; ++mutation) {
        Rig rig;
        auto& receipts{rig.proof.blocks[0].receipts};
        if (mutation == 0) receipts.pop_back();
        if (mutation == 1) receipts.push_back({2, Receipt(43)});
        if (mutation == 2) std::swap(receipts[0], receipts[1]);
        if (mutation == 3) receipts[0].encoded = Receipt(22);
        Refused(rig, arb::Error::BAD_RECEIPTS);
    }
}

BOOST_AUTO_TEST_CASE(opaque_transaction_trie_matches_independent_fixed_vectors)
{
    const std::array<std::pair<size_t, const char*>, 6> vectors{{
        {0, "56e81f171bcc55a6ff8345e692c0f86e5b48e01b996cadc001622fb5e363b421"},
        {1, "ffe918c2c5f8080ecb87895ccb8f1cf483e8c4940f7abba4f9f253efb6697a39"},
        {2, "f58e05710f928043b16e7d67931f2558cfc20368205825855d4e093a266970d6"},
        {128, "65e6551ab74dd3d5bed89fe4fc8e6b6d08b5626ea85e64a921cf38db437c126f"},
        {129, "f51309c6575329a86f2faf42ddf4c23b7d2bdd144959007a5b328cc271bed6b3"},
        {257, "742a4b6335d1fddfe37572a219d0bc27f07bdee55d1639031d760d6b81fd09c2"},
    }};
    for (const auto& [count, expected] : vectors) {
        std::vector<arb::IndexedTransaction> transactions;
        for (size_t i{0}; i < count; ++i) transactions.push_back({i, Transaction(i + 1, i % 2 != 0)});
        const auto root{arb::detail::TransactionRoot(transactions)};
        BOOST_REQUIRE(root);
        BOOST_CHECK(*root == IndependentTxRoot(transactions));
        BOOST_CHECK(*root == Hex32(expected));
    }
    BOOST_CHECK(arb::detail::Hash(Transaction(1)) ==
        Hex32("ce9f69ce66c2729cc198eae99b972985badb957ee3583433363faed136db335b"));
}

BOOST_AUTO_TEST_CASE(missing_swapped_mutated_or_extra_transaction_body_is_refused)
{
    for (unsigned int mutation{0}; mutation < 5; ++mutation) {
        Rig rig;
        auto& transactions{rig.proof.blocks[0].transactions};
        if (mutation == 0) transactions.pop_back();
        if (mutation == 1) std::swap(transactions[0], transactions[1]);
        if (mutation == 2) transactions[0].encoded.back() ^= 1;
        if (mutation == 3) transactions.push_back({2, Transaction(3)});
        if (mutation == 4) transactions[0].index = 1;
        Refused(rig, arb::Error::BAD_TRANSACTIONS);
    }
}

BOOST_AUTO_TEST_CASE(typed_envelope_body_and_zero_type_are_strict)
{
    for (const Bytes& bad : {Bytes{0x00}, Bytes{0x64, 0x80}, Bytes{0x80}, Bytes{0xff}}) {
        BOOST_CHECK(!arb::detail::ReceiptRoot({{0, bad}}));
    }
    for (unsigned char type : {0x01, 0x04, 0x64, 0x7f}) {
        BOOST_CHECK(arb::detail::ReceiptRoot({{0, Receipt(1, type)}}));
    }
}

BOOST_AUTO_TEST_CASE(empty_block_requires_authentic_empty_receipt_root_and_zero_gas)
{
    Rig rig;
    rig.proof.blocks[0].receipts.clear();
    Refused(rig, arb::Error::BAD_RECEIPTS);
    rig.proof.blocks[0].transactions.clear();
    rig.proof.blocks[0].header = Header(9, arb::detail::Hash(rig.proof.blocks[1].header),
                                      rig.l2_state.Root(), IndependentRoot({}), 0);
    rig.RebindNewest();
    BOOST_CHECK(rig.Verify().error == arb::Error::OK);
}

BOOST_AUTO_TEST_CASE(omitted_changed_or_spliced_header_cannot_match_assertion)
{
    for (unsigned int mutation{0}; mutation < 4; ++mutation) {
        Rig rig;
        if (mutation == 0) rig.proof.blocks.erase(rig.proof.blocks.begin());
        if (mutation == 1) rig.proof.blocks[1].header = Field(rig.proof.blocks[1].header, 8, RlpEncodeUint64(7));
        if (mutation == 2) std::swap(rig.proof.blocks[0], rig.proof.blocks[1]);
        if (mutation == 3) rig.proof.blocks.pop_back();
        Refused(rig, arb::Error::BAD_ANCESTRY);
    }
}

BOOST_AUTO_TEST_CASE(valid_hash_link_with_wrong_height_or_time_is_refused)
{
    for (unsigned int mutation{0}; mutation < 2; ++mutation) {
        Rig rig;
        auto& oldest{rig.proof.blocks[1].header};
        oldest = Field(oldest, mutation == 0 ? 8 : 11, RlpEncodeUint64(mutation == 0 ? 7 : 2000));
        rig.proof.blocks[0].header = Field(rig.proof.blocks[0].header, 0, RlpEncodeBytes(Raw(arb::detail::Hash(oldest))));
        rig.RebindNewest();
        Refused(rig, arb::Error::BAD_ANCESTRY);
    }
}

BOOST_AUTO_TEST_CASE(strict_header_schema_rejects_extension_bad_width_and_nonminimal_integer)
{
    Rig rig;
    const auto good{rig.proof.blocks[0].header};
    const auto top{RlpDecode(good)};
    const auto fields{RlpChildren(*top)};
    std::vector<Bytes> extended;
    for (const auto& f : *fields) {
        const auto enc{RlpEncodedSpan(f, good)};
        extended.emplace_back(enc.begin(), enc.end());
    }
    extended.push_back(RlpEncodeBytes(Raw(Tag(1))));
    BOOST_CHECK(!arb::detail::NitroHeader(RlpEncodeList(extended)));
    BOOST_CHECK(!arb::detail::NitroHeader(Field(good, 3, RlpEncodeBytes(Bytes(31, 1)))));
    BOOST_CHECK(!arb::detail::NitroHeader(Field(good, 8, RlpEncodeBytes(Bytes{0, 9}))));
    BOOST_CHECK(!arb::detail::NitroHeader(Field(good, 10, RlpEncodeUint64(1'000'001))));
    BOOST_CHECK(!arb::detail::NitroHeader(Field(good, 12, RlpEncodeBytes(Bytes(33, 1)))));
}

BOOST_AUTO_TEST_CASE(assertion_state_machine_and_preimage_fields_cannot_be_substituted)
{
    arb::AssertionPreimage independent;
    independent.parent_assertion = Tag(1); independent.block_hash = Tag(2);
    independent.send_root = Tag(3); independent.inbox_position = 4;
    independent.position_in_message = 5; independent.end_history_root = Tag(6);
    independent.inbox_accumulator = Tag(7);
    // Six static ABI words, independently hashed using PyCryptodome Keccak.
    BOOST_CHECK(arb::detail::AssertionHash(independent) ==
        Hex32("b974303d259169f79b24906e00f973f9d43c189b39fce967706352f0286cc018"));
    for (unsigned int mutation{0}; mutation < 8; ++mutation) {
        Rig rig;
        auto& a{rig.proof.assertion};
        if (mutation == 0) a.machine_status = 0;
        if (mutation == 1) a.machine_status = 2;
        if (mutation == 2) a.block_hash = {};
        if (mutation == 3) a.parent_assertion = {};
        if (mutation == 4) a.inbox_position++;
        if (mutation == 5) a.send_root = Tag(7);
        if (mutation == 6) a.end_history_root = Tag(8);
        if (mutation == 7) a.inbox_accumulator = Tag(9);
        Refused(rig, mutation < 4 ? arb::Error::BAD_ASSERTION : arb::Error::BAD_STORAGE);
    }
}

BOOST_AUTO_TEST_CASE(rollup_proxy_governance_pin_or_finalized_root_change_is_refused)
{
    for (unsigned int mutation{0}; mutation < 5; ++mutation) {
        Rig rig;
        if (mutation == 0) rig.policy.l1_accounts[0].code_hash = Tag(0x21);
        if (mutation == 1) rig.policy.l1_accounts[0].storage[0].value = Word(0x31);
        if (mutation == 2) rig.policy.latest_confirmed_slot = SlotWord(13);
        if (mutation == 3) rig.proof.l1_accounts[0].nodes[0].back() ^= 1;
        if (mutation == 4) rig.l1_root = Tag(0x99);
        Refused(rig, mutation == 1 || mutation == 2 ? arb::Error::BAD_STORAGE : arb::Error::BAD_ACCOUNT);
    }
}

BOOST_AUTO_TEST_CASE(unused_duplicate_or_reordered_witnesses_are_refused)
{
    Rig rig;
    rig.proof.l1_accounts.push_back(rig.proof.l1_accounts[0]);
    Refused(rig, arb::Error::BAD_POLICY);
    Rig duplicate;
    duplicate.policy.l1_accounts[0].storage.push_back(duplicate.policy.l1_accounts[0].storage[0]);
    duplicate.proof.l1_accounts[0].storage.insert(duplicate.proof.l1_accounts[0].storage.begin(), duplicate.proof.l1_accounts[0].storage[0]);
    Refused(duplicate, arb::Error::BAD_POLICY);
    Rig reordered;
    std::swap(reordered.proof.l1_accounts[0].storage[0], reordered.proof.l1_accounts[0].storage[1]);
    Refused(reordered, arb::Error::BAD_STORAGE);
}

BOOST_AUTO_TEST_CASE(l2_account_code_storage_and_absence_are_root_bound)
{
    Rig rig;
    rig.policy.historical_accounts[0].code_hash = Tag(0x51);
    Refused(rig, arb::Error::BAD_ACCOUNT);
    Rig storage;
    storage.policy.historical_accounts[0].storage[0].value = Word(0x61);
    Refused(storage, arb::Error::BAD_STORAGE);
    Rig absent;
    absent.policy.historical_accounts[0].storage[1].value = Word(1);
    Refused(absent, arb::Error::BAD_STORAGE);
    // An older state's valid witness must not satisfy fresh confirmed-head pins.
    Rig current;
    current.proof.blocks[0].header = Field(current.proof.blocks[0].header, 3, RlpEncodeBytes(Raw(Tag(0x99))));
    current.RebindNewest();
    Refused(current, arb::Error::BAD_ACCOUNT);
    Rig omitted;
    omitted.proof.confirmed_accounts.clear();
    Refused(omitted, arb::Error::BAD_POLICY);
}

BOOST_AUTO_TEST_CASE(cumulative_gas_must_match_complete_header_and_not_go_backward)
{
    for (unsigned int mutation{0}; mutation < 2; ++mutation) {
        Rig rig;
        rig.proof.blocks[0].receipts[1].encoded = Receipt(mutation ? 20 : 41, 0x64);
        rig.proof.blocks[0].header = Field(rig.proof.blocks[0].header, 5,
            RlpEncodeBytes(Raw(IndependentRoot(rig.proof.blocks[0].receipts))));
        rig.RebindNewest();
        Refused(rig, arb::Error::BAD_RECEIPTS);
    }
}

BOOST_AUTO_TEST_CASE(bounds_refuse_without_partial_evidence_or_production_capability)
{
    static_assert(!std::is_default_constructible_v<arb::ConfirmedRange>);
    static_assert(!std::is_constructible_v<arb::ConfirmedRange, fix::Evidence>);
    Rig rig;
    rig.proof.blocks.resize(arb::MAX_HEADERS + 1);
    Refused(rig, arb::Error::LIMIT);
    Rig node;
    node.proof.l1_accounts[0].nodes[0].resize(8193);
    Refused(node, arb::Error::LIMIT);
    BOOST_CHECK(!arb::detail::ReceiptRoot(std::vector<arb::IndexedReceipt>(arb::MAX_RECEIPTS + 1)));
    Rig transaction;
    transaction.proof.blocks[0].transactions[0].encoded.resize(arb::MAX_TRANSACTION_BYTES + 1);
    Refused(transaction, arb::Error::LIMIT);
    for (unsigned int mutation{0}; mutation < 4; ++mutation) {
        Rig cursor;
        if (mutation == 0) cursor.request.previous_hash = Tag(0x71);
        if (mutation == 1) cursor.request.previous_height = 8;
        if (mutation == 2) cursor.request.to_height = 10;
        if (mutation == 3) cursor.request.to_hash = Tag(0x72);
        Refused(cursor, arb::Error::BAD_ANCESTRY);
    }
}


BOOST_AUTO_TEST_CASE(real_native_ethereum_anchor_cannot_authorize_unreviewed_arbitrum)
{
    auto policy{NativePolicy()};
    peer::LightClient ethereum{policy};
    BOOST_REQUIRE(ethereum.EndReplay());
    std::vector<Bytes> journal;
    auto append = [&](std::span<const unsigned char> bytes) {
        journal.emplace_back(bytes.begin(), bytes.end());
        return true;
    };
    BOOST_REQUIRE(ethereum.Submit(native_bootstrap(), fx::U1_SIGNATURE_SLOT, append).result == peer::Result::OK);
    const auto anchor{ethereum.Latest(fx::U1_SIGNATURE_SLOT)};
    BOOST_REQUIRE(anchor);
    Rig fixture;
    BOOST_REQUIRE(fixture.Verify().error == arb::Error::OK);
    const auto result{arb::Verifier::VerifyConfirmedRange(ethereum, *anchor, fx::U1_SIGNATURE_SLOT,
        arb::Deployment::ARBITRUM_ONE_BOLD, fixture.request, fixture.proof)};
    BOOST_CHECK(result.error == arb::Error::UNREVIEWED_DEPLOYMENT);
    BOOST_CHECK(!result.range);
    BOOST_CHECK(arb::Verifier::VerifyConfirmedRange(ethereum, *anchor,
        policy.trusted_slot + policy.max_sync_lag_slots + 1,
        arb::Deployment::ARBITRUM_ONE_BOLD, fixture.request, fixture.proof).error == arb::Error::STALE_ETHEREUM);
}

BOOST_AUTO_TEST_CASE(old_native_capability_is_refused_after_real_bls_update)
{
    peer::LightClient ethereum{NativePolicy()};
    BOOST_REQUIRE(ethereum.EndReplay());
    auto append = [](std::span<const unsigned char>) { return true; };
    BOOST_REQUIRE(ethereum.Submit(native_bootstrap(), fx::U1_SIGNATURE_SLOT, append).result == peer::Result::OK);
    const auto old{ethereum.Latest(fx::U1_SIGNATURE_SLOT)};
    BOOST_REQUIRE(old);
    const auto update{EncodeBridgeRecordV1({BridgeRecordKindV1::UPDATE, BridgeUpdateV1{Update1()}})};
    BOOST_REQUIRE(update);
    BOOST_REQUIRE(ethereum.Submit(*update, fx::U1_SIGNATURE_SLOT, append).result == peer::Result::OK);
    BOOST_CHECK(arb::Verifier::VerifyConfirmedRange(ethereum, *old, fx::U1_SIGNATURE_SLOT,
        arb::Deployment::ARBITRUM_ONE_BOLD, {}, {}).error == arb::Error::STALE_ETHEREUM);
    const auto latest{ethereum.Latest(fx::U1_SIGNATURE_SLOT)};
    BOOST_REQUIRE(latest);
    BOOST_CHECK(arb::Verifier::VerifyConfirmedRange(ethereum, *latest, fx::U1_SIGNATURE_SLOT,
        arb::Deployment::ARBITRUM_ONE_BOLD, {}, {}).error == arb::Error::UNREVIEWED_DEPLOYMENT);
}

BOOST_AUTO_TEST_SUITE_END()
