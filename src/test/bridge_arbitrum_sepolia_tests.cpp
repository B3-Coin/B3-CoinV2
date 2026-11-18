// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license.

#include <bridge/arbitrum_sepolia_availability.h>
#include <test/util/bridge_eth_trie.h>
#include <test/util/gloas_fixture.h>
#include <boost/test/unit_test.hpp>

#include <functional>
#include <memory>
#include <type_traits>

using namespace bridge;
namespace arb = bridge::arbitrum;
namespace sep = arb::sepolia;
namespace gl = peer::gloas;
namespace gf = gloas_fixture;

BOOST_AUTO_TEST_SUITE(bridge_arbitrum_sepolia_tests)
namespace {
using Bytes = arb::Bytes;
using bridge_test::EthTrie;
using bridge_test::EthStorageTrie;
EthAddress Address(unsigned char byte) { EthAddress out{}; out.fill(byte); return out; }
StorageWord Word(const uint256& hash) { StorageWord out; std::copy(hash.begin(), hash.end(), out.begin()); return out; }
Bytes Raw(const uint256& hash) { return {hash.begin(), hash.end()}; }
Bytes Field(const Bytes& header, size_t index, const Bytes& replacement)
{
    const auto top{RlpDecode(header)};
    const auto fields{RlpChildren(*top)};
    std::vector<Bytes> out;
    for (size_t i{0}; i < fields->size(); ++i) {
        const auto encoded{RlpEncodedSpan((*fields)[i], header)};
        out.push_back(i == index ? replacement : Bytes{encoded.begin(), encoded.end()});
    }
    return RlpEncodeList(out);
}
template <typename T> uint256 Root(const std::vector<T>& values)
{
    EthTrie trie;
    for (const auto& item : values) trie.Insert(RlpEncodeUint64(item.index), item.encoded);
    return trie.Root();
}
Bytes Header(uint64_t number, const uint256& parent, const uint256& state,
             const uint256& transactions, const uint256& receipts, uint64_t gas)
{
    return RlpEncodeList({RlpEncodeBytes(Raw(parent)), RlpEncodeBytes(Raw(gf::Tag(0x11))),
        RlpEncodeBytes(Address(0x22)), RlpEncodeBytes(Raw(state)),
        RlpEncodeBytes(Raw(transactions)), RlpEncodeBytes(Raw(receipts)),
        RlpEncodeBytes(Bytes(256, 0)), RlpEncodeUint64(0), RlpEncodeUint64(number),
        RlpEncodeUint64(1'000'000), RlpEncodeUint64(gas), RlpEncodeUint64(1000 + number),
        RlpEncodeBytes({}), RlpEncodeBytes(Raw(gf::Tag(0x77))),
        RlpEncodeBytes(Bytes(8, 0)), RlpEncodeUint64(1)});
}
Bytes Receipt(bool deposit)
{
    const auto topic{arb::history::Topic(deposit ? "Deposit(uint64,bytes32,bytes32,uint256,address)" : "Withdrawn(bytes32,address,uint256)")};
    std::vector<Bytes> topics{RlpEncodeBytes(Raw(topic))};
    Bytes data;
    if (deposit) {
        topics.push_back(RlpEncodeBytes(SlotWord(1)));
        topics.push_back(RlpEncodeBytes(Raw(gf::Tag(0x41))));
        topics.push_back(RlpEncodeBytes(Raw(gf::Tag(0x42))));
        data = Bytes(64, 0); data[31] = 100; std::fill(data.begin() + 44, data.end(), 0x43);
    } else {
        topics.push_back(RlpEncodeBytes(Raw(gf::Tag(0x51))));
        Bytes address(32, 0); std::fill(address.begin() + 12, address.end(), 0x52);
        topics.push_back(RlpEncodeBytes(address)); data = Bytes(32, 0); data.back() = 90;
    }
    const auto log{RlpEncodeList({RlpEncodeBytes(Address(0x33)), RlpEncodeList(topics), RlpEncodeBytes(data)})};
    return RlpEncodeList({RlpEncodeUint64(1), RlpEncodeUint64(21),
        RlpEncodeBytes(Bytes(256, 0)), RlpEncodeList({log})});
}

struct Rig {
    using MutatePins = std::function<void(std::vector<sep::pins::AccountPin>&)>;
    std::vector<arb::BlockWitness> blocks; // ascending; index0 is block1
    sep::HeadProof proof;
    uint256 previous{gf::Tag(0x70)};
    EthTrie l2_state;
    EthStorageTrie token_storage;
    std::vector<arb::AccountWitness> l2_witnesses;
    std::unique_ptr<gf::Fixture> fixture;
    std::unique_ptr<gl::LightClient> ethereum;
    std::optional<gl::FinalizedExecution> anchor;

    explicit Rig(MutatePins mutate = {}, unsigned char node_status = 2, uint64_t created_at = 100)
    {
        token_storage.Set(SlotWord(2), SlotWord(100));
        l2_state.InsertSecure(Address(0x66), bridge_test::EncodeAccount(1, {}, token_storage.Root(), gf::Tag(0x67)));
        l2_witnesses = {{Address(0x66), l2_state.ProofSecure(Address(0x66)),
            {{SlotWord(2), token_storage.Proof(SlotWord(2))}, {SlotWord(3), token_storage.Proof(SlotWord(3))}}}};
        auto parent{previous};
        for (uint64_t n{1}; n <= 300; ++n) {
            arb::BlockWitness block;
            if (n <= 2) {
                block.receipts = {{0, Receipt(n == 1)}};
                block.transactions = {{0, RlpEncodeList({RlpEncodeUint64(n), RlpEncodeBytes(Address(0x44))})}};
            }
            block.header = Header(n, parent, l2_state.Root(), Root(block.transactions), Root(block.receipts), n <= 2 ? 21 : 0);
            parent = arb::detail::Hash(block.header); blocks.push_back(std::move(block));
        }
        proof.header = blocks.back().header;
        proof.assertion = {gf::Tag(1), parent, gf::Tag(2), 3, 4, 1, gf::Tag(5), gf::Tag(6)};
        auto pins{sep::pins::Registry()};
        if (mutate) mutate(pins);
        std::vector<EthStorageTrie> storage(pins.size()); EthTrie l1_state;
        const auto assertion{arb::detail::AssertionHash(proof.assertion)};
        for (size_t i{0}; i < pins.size(); ++i) {
            for (const auto& slot : pins[i].storage) storage[i].Set(slot.slot, slot.value);
            if (pins[i].address == sep::pins::Rollup()) {
                storage[i].Set(SlotWord(sep::pins::LATEST_CONFIRMED_SLOT), Word(assertion));
                StorageWord node{}; node[6] = node_status;
                for (size_t j{0}; j < 8; ++j) node[15-j] = static_cast<unsigned char>(created_at >> (8*j));
                storage[i].Set(sep::detail::AssertionSlot(assertion), node);
            }
            l1_state.InsertSecure(pins[i].address, bridge_test::EncodeAccount(1, {}, storage[i].Root(), pins[i].code_hash));
        }
        const auto requests{sep::RequiredWitnesses(proof.assertion)};
        for (size_t group{0}, i{0}; group < requests.size(); ++group) {
            for (const auto& [address, slots] : requests[group]) {
                arb::AccountWitness account{address, l1_state.ProofSecure(address), {}};
                for (const auto& slot : slots) account.storage.push_back({slot, storage[i].Proof(slot)});
                proof.accounts[group].push_back(std::move(account)); ++i;
            }
        }
        fixture = std::make_unique<gf::Fixture>(l1_state.Root(), 20'000'000);
        ethereum = std::make_unique<gl::LightClient>(fixture->policy); fixture->Bootstrap(*ethereum);
        anchor = ethereum->Latest(fixture->slot, fixture->execution_header);
        if (!anchor) throw std::runtime_error("test native Gloas capability unavailable");
    }
    uint64_t Slot() const { return fixture->slot; }
    sep::Reply<sep::SettledHead> Verify() const
    { return sep::Verifier::AuthenticateHead(*ethereum, *anchor, Slot(), proof); }
    std::vector<Bytes> Page(uint64_t current, uint64_t destination) const
    {
        std::vector<Bytes> out;
        while (current > destination) { --current; out.push_back(blocks[current-1].header); }
        return out;
    }
    arb::Proof Range(uint64_t from, uint64_t to) const
    {
        arb::Proof out;
        for (auto n{to}; n > from; --n) out.blocks.push_back(blocks[n-1]);
        out.confirmed_accounts = l2_witnesses; out.historical_accounts = l2_witnesses;
        return out;
    }
};
void Refused(const Rig& rig, sep::Error expected)
{
    const auto reply{rig.Verify()};
    BOOST_CHECK_MESSAGE(reply.error == expected, sep::Name(reply.error));
    BOOST_CHECK(!reply.value);
}
void SetPin(std::vector<sep::pins::AccountPin>& pins, size_t account, const StorageWord& slot, const StorageWord& value)
{
    auto found{std::find_if(pins[account].storage.begin(), pins[account].storage.end(), [&](const auto& pin) { return pin.slot == slot; })};
    if (found == pins[account].storage.end()) throw std::runtime_error("missing compiled test pin");
    found->value = value;
}
} // namespace

BOOST_AUTO_TEST_CASE(private_authority_and_exact_separate_testnet_identity)
{
    static_assert(!std::is_default_constructible_v<sep::SettledHead>);
    static_assert(!std::is_default_constructible_v<sep::Ancestor>);
    static_assert(!std::is_default_constructible_v<sep::ConfirmedRange>);
    static_assert(!std::is_constructible_v<sep::SettledHead, gl::FinalizedExecution, arb::Header>);
    static_assert(!std::is_constructible_v<sep::ConfirmedRange, arb::history::Evidence>);
    Rig rig;
    const auto result{rig.Verify()}; BOOST_REQUIRE(result.value);
    BOOST_CHECK_EQUAL(result.value->ChainId(), 421614U);
    BOOST_CHECK(!result.value->MainnetEnabled());
    BOOST_CHECK_EQUAL(result.value->Header().number, 300U);
    BOOST_CHECK(result.value->DeploymentDigest() == sep::pins::Digest());
    BOOST_CHECK(result.value->IsCurrent(*rig.ethereum, rig.Slot()));
    BOOST_CHECK_EQUAL(rig.proof.accounts[0].size(), 16U);
    BOOST_CHECK_EQUAL(rig.proof.accounts[1].size(), 1U);
}

BOOST_AUTO_TEST_CASE(authenticated_ancestry_pages_reach_older_complete_history)
{
    Rig rig; const auto settled{rig.Verify()}; BOOST_REQUIRE(settled.value);
    const auto start{sep::Verifier::BeginAncestor(*rig.ethereum, rig.Slot(), *settled.value)}; BOOST_REQUIRE(start.value);
    const auto first{sep::Verifier::WalkBack(*rig.ethereum, rig.Slot(), *start.value, rig.Page(300, 172))}; BOOST_REQUIRE(first.value);
    const auto second{sep::Verifier::WalkBack(*rig.ethereum, rig.Slot(), *first.value, rig.Page(172, 44))}; BOOST_REQUIRE(second.value);
    const auto third{sep::Verifier::WalkBack(*rig.ethereum, rig.Slot(), *second.value, rig.Page(44, 2))}; BOOST_REQUIRE(third.value);
    const arb::RangeRequest request{0, 2, rig.previous, arb::detail::Hash(rig.blocks[1].header)};
    const auto result{sep::Verifier::VerifyRange(*rig.ethereum, rig.Slot(), *third.value, request, rig.Range(0, 2))};
    BOOST_REQUIRE(result.value); BOOST_CHECK(result.error == sep::Error::OK);
    BOOST_REQUIRE_EQUAL(result.value->History().headers.size(), 2U);
    BOOST_CHECK_EQUAL(result.value->History().headers.front().number, 1U);
    BOOST_REQUIRE_EQUAL(result.value->History().receipts.size(), 2U);
    BOOST_CHECK_EQUAL(result.value->History().receipts.front().block_number, 1U);
    BOOST_REQUIRE_EQUAL(result.value->History().oldest_accounts.size(), 1U);
    BOOST_CHECK(result.value->History().oldest_accounts.front().storage[1].value == StorageWord{});
    const auto events{arb::history::ExtractEvents(result.value->History(), {Address(0x33)})};
    BOOST_REQUIRE(events); BOOST_REQUIRE_EQUAL(events->size(), 2U);
    BOOST_CHECK(events->front().kind == arb::history::EventKind::DEPOSIT);
    BOOST_CHECK(events->back().kind == arb::history::EventKind::WITHDRAWN);
    BOOST_CHECK(result.value->IsCurrent(*rig.ethereum, rig.Slot()));
    BOOST_CHECK(!result.value->MainnetEnabled());
}

BOOST_AUTO_TEST_CASE(compiled_config_refuses_valid_but_different_authenticated_storage)
{
    const std::vector<std::pair<size_t, StorageWord>> changed{
        {0, SlotWord(51)}, {0, SlotWord(101)}, {0, SlotWord(104)}, {0, SlotWord(105)},
        {0, SlotWord(123)}, {0, SlotWord(114)}, {5, SlotWord(8)}, {15, SlotWord(0)}, {16, SlotWord(0)}};
    for (const auto& [account, slot] : changed) {
        Rig rig{[&](auto& pins) { SetPin(pins, account, slot, SlotWord(999)); }};
        Refused(rig, sep::Error::BAD_STORAGE);
    }
    // Explicitly test a nonzero AnyTrust fast confirmer packed at offset1.
    Rig fast{[](auto& pins) { auto value{SlotWord(0)}; value[30] = 1; SetPin(pins, 0, SlotWord(123), value); }};
    Refused(fast, sep::Error::BAD_STORAGE);
    Rig logic{[](auto& pins) { pins[1].code_hash = gf::Tag(0x91); }};
    Refused(logic, sep::Error::BAD_ACCOUNT);
    Rig governance{[](auto& pins) { pins[16].code_hash = gf::Tag(0x92); }};
    Refused(governance, sep::Error::BAD_ACCOUNT);
}

BOOST_AUTO_TEST_CASE(settled_assertion_requires_confirmed_status_and_valid_creation)
{
    for (const auto status : {0, 1, 3}) { Rig rig{{}, static_cast<unsigned char>(status)}; Refused(rig, sep::Error::BAD_ASSERTION); }
    for (const uint64_t created : {uint64_t{0}, uint64_t{20'000'001}}) { Rig rig{{}, 2, created}; Refused(rig, sep::Error::BAD_ASSERTION); }
    Rig rig; rig.proof.assertion.inbox_position++; Refused(rig, sep::Error::BAD_STORAGE);
    Rig bad_machine; bad_machine.proof.assertion.machine_status = 2; Refused(bad_machine, sep::Error::BAD_ASSERTION);
}

BOOST_AUTO_TEST_CASE(witness_groups_cannot_omit_duplicate_reorder_or_expand)
{
    Rig rig; const auto good{rig.proof};
    rig.proof.accounts[1].clear(); Refused(rig, sep::Error::BAD_DEPLOYMENT);
    rig.proof = good; rig.proof.accounts[1].push_back(good.accounts[1].front()); Refused(rig, sep::Error::BAD_DEPLOYMENT);
    rig.proof = good; std::swap(rig.proof.accounts[0][0], rig.proof.accounts[0][1]); Refused(rig, sep::Error::BAD_STORAGE);
    rig.proof = good; rig.proof.accounts[1][0] = good.accounts[0][0]; Refused(rig, sep::Error::BAD_STORAGE);
    rig.proof = good; rig.proof.accounts[0][0].storage[1] = rig.proof.accounts[0][0].storage[0]; Refused(rig, sep::Error::BAD_STORAGE);
    rig.proof = good; rig.proof.accounts[0].push_back(good.accounts[0].front()); Refused(rig, sep::Error::LIMIT);
    rig.proof = good; rig.proof.accounts[0][0].nodes[0].resize(8193); Refused(rig, sep::Error::LIMIT);
    rig.proof = good; rig.proof.accounts[0][0].nodes[0].back() ^= 1; Refused(rig, sep::Error::BAD_ACCOUNT);
    rig.proof = good; rig.proof.header = Field(rig.proof.header, 8, RlpEncodeUint64(301)); Refused(rig, sep::Error::BAD_HEADER);
}

BOOST_AUTO_TEST_CASE(ancestry_pages_and_ranges_refuse_gaps_reordering_and_wrong_endpoints)
{
    Rig rig; const auto head{rig.Verify()}; BOOST_REQUIRE(head.value);
    const auto start{sep::Verifier::BeginAncestor(*rig.ethereum, rig.Slot(), *head.value)}; BOOST_REQUIRE(start.value);
    auto bad{rig.Page(300, 172)}; bad.erase(bad.begin()+1);
    BOOST_CHECK(sep::Verifier::WalkBack(*rig.ethereum, rig.Slot(), *start.value, bad).error == sep::Error::BAD_ANCESTRY);
    bad = rig.Page(300, 172); std::swap(bad[0], bad[1]);
    BOOST_CHECK(sep::Verifier::WalkBack(*rig.ethereum, rig.Slot(), *start.value, bad).error == sep::Error::BAD_ANCESTRY);
    BOOST_CHECK(sep::Verifier::WalkBack(*rig.ethereum, rig.Slot(), *start.value, {}).error == sep::Error::LIMIT);
    BOOST_CHECK(sep::Verifier::WalkBack(*rig.ethereum, rig.Slot(), *start.value, rig.Page(300, 171)).error == sep::Error::LIMIT);
    const arb::RangeRequest wrong{0, 2, rig.previous, arb::detail::Hash(rig.blocks[1].header)};
    BOOST_CHECK(sep::Verifier::VerifyRange(*rig.ethereum, rig.Slot(), *start.value, wrong, rig.Range(0, 2)).error == sep::Error::BAD_ANCESTRY);
    const arb::RangeRequest request{298, 300, arb::detail::Hash(rig.blocks[297].header), head.value->Header().hash};
    auto proof{rig.Range(298, 300)}; proof.blocks.pop_back();
    BOOST_CHECK(sep::Verifier::VerifyRange(*rig.ethereum, rig.Slot(), *start.value, request, proof).error == sep::Error::BAD_RANGE);
    proof = rig.Range(298, 300); proof.l1_accounts = rig.proof.accounts[0];
    BOOST_CHECK(sep::Verifier::VerifyRange(*rig.ethereum, rig.Slot(), *start.value, request, proof).error == sep::Error::BAD_RANGE);
}

BOOST_AUTO_TEST_CASE(stale_and_different_native_owners_cannot_reuse_capabilities)
{
    Rig rig; const auto head{rig.Verify()}; BOOST_REQUIRE(head.value);
    const auto endpoint{sep::Verifier::BeginAncestor(*rig.ethereum, rig.Slot(), *head.value)}; BOOST_REQUIRE(endpoint.value);
    const auto late{rig.Slot()+rig.fixture->policy.max_sync_lag_slots+1};
    BOOST_CHECK(sep::Verifier::AuthenticateHead(*rig.ethereum, *rig.anchor, late, rig.proof).error == sep::Error::STALE_ETHEREUM);
    BOOST_CHECK(sep::Verifier::WalkBack(*rig.ethereum, late, *endpoint.value, rig.Page(300, 299)).error == sep::Error::STALE_ETHEREUM);
    gl::LightClient other{rig.fixture->policy};
    BOOST_REQUIRE(other.EndReplay());
    BOOST_REQUIRE(other.Submit(rig.fixture->Record(), rig.Slot(), [](auto) { return true; }).result == peer::Result::OK);
    BOOST_CHECK(sep::Verifier::BeginAncestor(other, rig.Slot(), *head.value).error == sep::Error::STALE_ETHEREUM);
}

BOOST_AUTO_TEST_CASE(actual_bls_finality_advance_revokes_all_prior_source_capabilities)
{
    Rig rig; const auto head{rig.Verify()}; BOOST_REQUIRE(head.value);
    const auto endpoint{sep::Verifier::BeginAncestor(*rig.ethereum, rig.Slot(), *head.value)}; BOOST_REQUIRE(endpoint.value);
    const arb::RangeRequest request{298, 300, arb::detail::Hash(rig.blocks[297].header), head.value->Header().hash};
    const auto range{sep::Verifier::VerifyRange(*rig.ethereum, rig.Slot(), *endpoint.value, request, rig.Range(298, 300))};
    BOOST_REQUIRE(range.value);
    const auto raw{gf::ExecutionHeader(rig.anchor->Header().state_root, rig.Slot()+10, 20'000'001)};
    gl::Update update;
    update.finalized = gf::Header(rig.Slot()+10, gf::Keccak(raw), gf::Tag(0x51));
    update.has_next = true; update.next = gf::Committee();
    gf::Tree state;
    state.leaves[gl::FINALITY_GINDEX] = update.finalized.beacon.HashTreeRoot();
    state.leaves[gl::NEXT_GINDEX] = update.next.HashTreeRoot();
    update.finality_branch = state.Branch(gl::FINALITY_GINDEX);
    update.next_branch = state.Branch(gl::NEXT_GINDEX);
    update.attested = gf::Header(rig.Slot()+20, gf::Tag(0x61), state.Node(1));
    update.sync.bits.fill(0xff);
    const auto domain{ssz::SyncCommitteeDomain(gl::FORK_VERSION, uint256{std::span{gl::GENESIS_VALIDATORS_ROOT}})};
    update.sync.signature = gf::Sign(ssz::SigningRoot(update.attested.beacon.HashTreeRoot(), domain));
    update.signature_slot = rig.Slot()+21;
    const auto record{gl::EncodeRecord(gl::Record{update})}; BOOST_REQUIRE(record);
    BOOST_REQUIRE(rig.ethereum->Submit(*record, update.signature_slot, rig.fixture->Sink()).result == peer::Result::OK);
    BOOST_CHECK(!head.value->IsCurrent(*rig.ethereum, update.signature_slot));
    BOOST_CHECK(!endpoint.value->IsCurrent(*rig.ethereum, update.signature_slot));
    BOOST_CHECK(!range.value->IsCurrent(*rig.ethereum, update.signature_slot));
    BOOST_CHECK(sep::Verifier::AuthenticateHead(*rig.ethereum, *rig.anchor, update.signature_slot, rig.proof).error == sep::Error::STALE_ETHEREUM);
    const auto fresh{rig.ethereum->Latest(update.signature_slot, raw)}; BOOST_REQUIRE(fresh);
    BOOST_CHECK(sep::Verifier::AuthenticateHead(*rig.ethereum, *fresh, update.signature_slot, rig.proof).value);
}

BOOST_AUTO_TEST_CASE(aggregate_proof_limits_span_both_bounded_groups)
{
    Rig bytes;
    auto& account{bytes.proof.accounts[0][0]}; account.storage.resize(64);
    for (auto& slot : account.storage) slot.nodes.assign(64, Bytes(8192, 0x80));
    Refused(bytes, sep::Error::LIMIT);
    Rig nodes;
    for (auto& group : nodes.proof.accounts) {
        for (auto& item : group) {
            item.storage.resize(64);
            for (auto& slot : item.storage) slot.nodes.assign(64, Bytes{0x80});
        }
    }
    Refused(nodes, sep::Error::LIMIT);
}

BOOST_AUTO_TEST_CASE(untrusted_creation_log_recovers_only_hash_bound_canonical_preimage)
{
    Rig rig; const auto& a{rig.proof.assertion}; const auto hash{arb::detail::AssertionHash(a)};
    EthLog log;
    log.address = sep::pins::Rollup();
    log.topics = {sep::pins::Hash("901c3aee23cf4478825462caaab375c606ab83516060388344f0650340753630"), hash, a.parent_assertion};
    log.data.resize(25*32);
    const auto put = [&](size_t index, std::span<const unsigned char> value) { std::copy(value.begin(), value.end(), log.data.begin()+index*32); };
    put(13, Raw(a.block_hash)); put(14, Raw(a.send_root)); put(15, SlotWord(a.inbox_position));
    put(16, SlotWord(a.position_in_message)); put(17, SlotWord(a.machine_status));
    put(18, Raw(a.end_history_root)); put(19, Raw(a.inbox_accumulator));
    const auto parsed{sep::DecodeAssertionCreated(log, hash)}; BOOST_REQUIRE(parsed);
    BOOST_CHECK(arb::detail::AssertionHash(*parsed) == hash);
    const auto requests{sep::RequiredWitnessesByHash(hash)};
    BOOST_REQUIRE_EQUAL(requests[0].front().second.size(), 33U);
    BOOST_CHECK(requests[0].front().second.back() == sep::detail::AssertionSlot(hash));
    for (unsigned int mutation{0}; mutation < 9; ++mutation) {
        auto bad{log};
        if (mutation == 0) bad.address = Address(0x99);
        if (mutation == 1) bad.topics[0] = gf::Tag(0x99);
        if (mutation == 2) bad.topics[1] = gf::Tag(0x99);
        if (mutation == 3) bad.topics[2] = gf::Tag(0x99);
        if (mutation == 4) bad.data.pop_back();
        if (mutation == 5) bad.data[15*32] = 1; // noncanonical uint64 padding
        if (mutation == 6) bad.data[4*32] = 1; // noncanonical address padding
        if (mutation == 7) bad.data[17*32+31] = 2; // non-FINISHED state
        if (mutation == 8) bad.data[13*32] ^= 1; // valid layout, wrong preimage
        BOOST_CHECK(!sep::DecodeAssertionCreated(bad, hash));
    }
}

BOOST_AUTO_TEST_CASE(native_latest_query_selects_exact_creation_block_without_settlement_authority)
{
    static_assert(!std::is_default_constructible_v<sep::CurrentAssertionHash>);
    static_assert(!std::is_constructible_v<sep::SettledHead, sep::CurrentAssertionHash>);
    Rig rig;
    const auto query{sep::QueryLatestConfirmed(*rig.ethereum, *rig.anchor, rig.Slot(), rig.proof.accounts[0][0])};
    BOOST_REQUIRE(query.value);
    BOOST_CHECK(query.value->Hash() == arb::detail::AssertionHash(rig.proof.assertion));
    BOOST_CHECK_EQUAL(query.value->CreatedBlock(), 100U);
    BOOST_CHECK(query.value->IsCurrent(*rig.ethereum, rig.Slot()));
    BOOST_CHECK(!query.value->MainnetEnabled());
    const auto late{rig.Slot()+rig.fixture->policy.max_sync_lag_slots+1};
    BOOST_CHECK(!query.value->IsCurrent(*rig.ethereum, late));
    BOOST_CHECK(sep::QueryLatestConfirmed(*rig.ethereum, *rig.anchor, late, rig.proof.accounts[0][0]).error == sep::Error::STALE_ETHEREUM);
    auto wrong{rig.proof.accounts[0][0]}; wrong.storage.back().slot = SlotWord(117);
    BOOST_CHECK(!sep::QueryLatestConfirmed(*rig.ethereum, *rig.anchor, rig.Slot(), wrong).value);
    wrong = rig.proof.accounts[0][0]; wrong.nodes[0].back() ^= 1;
    BOOST_CHECK(sep::QueryLatestConfirmed(*rig.ethereum, *rig.anchor, rig.Slot(), wrong).error == sep::Error::BAD_ACCOUNT);
    Rig changed{[](auto& pins) { SetPin(pins, 0, SlotWord(123), SlotWord(1)); }};
    BOOST_CHECK(sep::QueryLatestConfirmed(*changed.ethereum, *changed.anchor, changed.Slot(), changed.proof.accounts[0][0]).error == sep::Error::BAD_STORAGE);
    Rig pending{{}, 1};
    BOOST_CHECK(sep::QueryLatestConfirmed(*pending.ethereum, *pending.anchor, pending.Slot(), pending.proof.accounts[0][0]).error == sep::Error::BAD_ASSERTION);
}

BOOST_AUTO_TEST_SUITE_END()
