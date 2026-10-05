// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Ethereum account and storage proofs (bridge/state_proof.h), the EIP-1186
// layer under bridge V2 kind 9 state observations.
//
// Three independent sources of vectors:
//  - real eth_getProof responses captured from a local anvil 1.7.1
//    (127.0.0.1, no network; setup below) and cross-checked by
//    test_framework.b3bridge.verify_account_storage_proof before capture;
//  - the test-only trie builder in test/util/bridge_eth_trie.h, which
//    reproduces the classic do/dog/doge/horse root and whose proofs are also
//    checked by the independent inclusion verifier in bridge/mpt.h;
//  - hand-built nodes, one per malformation the verifier must refuse.

#include <bridge/mpt.h>
#include <bridge/rlp.h>
#include <bridge/state_proof.h>
#include <crypto/keccak256.h>
#include <test/util/bridge_eth_trie.h>
#include <uint256.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace bridge;
using bridge_test::EncodeAccount;
using bridge_test::EthStorageTrie;
using bridge_test::EthTrie;

BOOST_AUTO_TEST_SUITE(bridge_state_proof_tests)

namespace {

using Bytes = std::vector<unsigned char>;
using Nodes = std::vector<Bytes>;

Bytes FromHexStr(const std::string& h)
{
    auto v{TryParseHex<unsigned char>(h)};
    BOOST_REQUIRE(v.has_value());
    return *v;
}

//! Ethereum hashes are kept in raw byte order (byte 0 first).
uint256 Raw(const std::string& hex)
{
    const Bytes b{FromHexStr(hex)};
    BOOST_REQUIRE_EQUAL(b.size(), 32U);
    return uint256{b};
}

EthAddress Address(const std::string& hex)
{
    const Bytes b{FromHexStr(hex)};
    BOOST_REQUIRE_EQUAL(b.size(), 20U);
    EthAddress out;
    std::copy(b.begin(), b.end(), out.begin());
    return out;
}

StorageWord WordOf(const std::string& hex)
{
    const Bytes b{FromHexStr(hex)};
    BOOST_REQUIRE_EQUAL(b.size(), 32U);
    StorageWord out;
    std::copy(b.begin(), b.end(), out.begin());
    return out;
}

Nodes HexNodes(const std::vector<std::string>& hex)
{
    Nodes out;
    for (const auto& h : hex) out.push_back(FromHexStr(h));
    return out;
}

uint256 Keccak(std::span<const unsigned char> data)
{
    uint256 out;
    Keccak256().Write(data).Finalize(out);
    return out;
}

std::span<const unsigned char> Span(const uint256& h) { return {h.begin(), 32}; }

Bytes Enc(std::span<const unsigned char> b) { return RlpEncodeBytes(b); }
Bytes EmptyItem() { return RlpEncodeBytes(std::span<const unsigned char>{}); }
Bytes HashRef(const Bytes& node)
{
    const uint256 h{Keccak(node)};
    return Enc(Span(h));
}

Bytes Nibbles(const uint256& key) { return EthTrie::ToNibbles(Span(key)); }
Bytes Slice(const Bytes& v, size_t from, size_t to) { return {v.begin() + from, v.begin() + to}; }

Bytes Leaf(const Bytes& path, std::span<const unsigned char> value)
{
    return RlpEncodeList({Enc(EthTrie::HexPrefix(path, true)), Enc(value)});
}
Bytes Extension(const Bytes& path, const Bytes& child_ref)
{
    return RlpEncodeList({Enc(EthTrie::HexPrefix(path, false)), child_ref});
}
Bytes Branch(const std::array<Bytes, 16>& children, const Bytes& value_item)
{
    std::vector<Bytes> items;
    for (const Bytes& c : children) items.push_back(c.empty() ? EmptyItem() : c);
    items.push_back(value_item);
    return RlpEncodeList(items);
}

//! A 32-byte key: `first` then `fill` bytes, optionally ending in `last`.
uint256 KeyOf(unsigned char first, unsigned char fill, std::optional<unsigned char> last = std::nullopt)
{
    Bytes b(32, fill);
    b[0] = first;
    if (last) b[31] = *last;
    return uint256{b};
}

Bytes Filled(size_t n, unsigned char v) { return Bytes(n, v); }

void CheckRefused(const uint256& root, const uint256& key, const Nodes& nodes,
                  const std::string& expected)
{
    std::string error;
    const auto result{VerifySecureMptProof(root, key, nodes, error)};
    BOOST_CHECK_MESSAGE(!result, "accepted, expected refusal: " + expected);
    BOOST_CHECK_EQUAL(error, expected);
}

SecureMptLookup CheckAccepted(const uint256& root, const uint256& key, const Nodes& nodes)
{
    std::string error;
    const auto result{VerifySecureMptProof(root, key, nodes, error)};
    BOOST_REQUIRE_MESSAGE(result, error);
    return *result;
}

// ---------------------------------------------------------------------------
// anvil 1.7.1 eth_getProof fixture (contrib-free capture, local only).
// anvil --host 127.0.0.1 --chain-id 31337 --timestamp 1700000000, then
//   anvil_setCode(VAULT, 0x600160005260206000f3)
//   anvil_setStorageAt(VAULT, 0, 5); (VAULT, 1, 1000000); (VAULT, 3, 0x1234);
//   (VAULT, 7, 2^256-1); anvil_setNonce(VAULT, 1); anvil_setNonce(FUNDED, 42)
//   evm_mine; eth_getProof at block 1 for VAULT slots [0,1,2,3,7], FUNDED, MISSING.
// ---------------------------------------------------------------------------
const std::string ANVIL_STATE_ROOT{"8c9affb3733989f1fd5d0cf58a032f2d6dcdcad2165c47beb54ad1ae40527e67"};
const std::string ANVIL_VAULT{"5fbdb2315678afecb367f032d93f642f64180aa3"};
const std::string ANVIL_FUNDED{"f39fd6e51aad88f6f4ce6ab8827279cfffb92266"};
const std::string ANVIL_MISSING{"1111111111111111111111111111111111111111"};
const std::string ANVIL_VAULT_STORAGE_ROOT{"c41e5fc91f851d80fc6dc0068352743e96bb61c80efaf4ac215afa006382e3e5"};
const std::string ANVIL_VAULT_CODE_HASH{"c1d5b4ce3e2a6227293fccce2904121c8647bbe16c1216340b851bf12d12560e"};
const std::string ANVIL_STATE_ROOT_NODE{
    "f90151a0b91a8b7a7e9d3eab90afd81da3725030742f663c6ed8c26657bf00d842a9f4aaa01689b2a5203afd9ea0a0ca"
    "3765e4a538c7176e53eac1f8307a344ffc3c6176558080a0279db21cf3495a2f4a349a213186c9477faa818d405eecc9"
    "b4fde90c78824c0ba07e56f2f326dc0a6bb22a0fdc58af094cd465ab4a27c9aaaef9413be1da26cb57a005e90dd4ac1d"
    "18cf3692bacdb8b2f1cd971a8e081bf3cdbd359124e5b12caabb80a04b29efa44ecf50c19b34950cf1d0f05e00568bcc"
    "873120fbea9a4e8439de0962a0d0a1bfe5b45d2d863a794f016450a4caca04f3b599e8d1652afca8b752935fd880a0bf"
    "9b09e442e044778b354abbadb5ec049d7f5e8b585c3966d476c4fbc9a181d28080a059a232f2218cecd985daeac86aff"
    "264561bf7e1a9acbe01053d88e883c882f5fa0e5c557a0ce3894afeb44c37f3d24247f67dc76a174d8cacc360c1210ee"
    "f60a7680"};
const std::string ANVIL_VAULT_BRANCH{
    "f85180808080a0ba2193798e9899a3d880cd01c3a6deb8718708617d62114cbf8e1f5c917f78d180808080a074ae0767"
    "a40fc6fff780050f46a50f6b39ca4edb7faa9669108157a1cd96f40980808080808080"};
const std::string ANVIL_VAULT_LEAF{
    "f869a020e659e60b21cc961f64ad47f20523c1d329d4bbda245ef3940a76dc89d0911bb846f8440180a0c41e5fc91f85"
    "1d80fc6dc0068352743e96bb61c80efaf4ac215afa006382e3e5a0c1d5b4ce3e2a6227293fccce2904121c8647bbe16c"
    "1216340b851bf12d12560e"};
const std::string ANVIL_FUNDED_BRANCH{
    "f8518080808080a0aabfb1441169c3379f428df147ba34658049e31ab75bca31dcea5ea3513408a7808080a064cbb815"
    "f19cdbec34685a90cf93390a1247c2c8f933b24faf31d4afdf0f487380808080808080"};
const std::string ANVIL_FUNDED_LEAF{
    "f873a020707d0e6171f728f7473c24cc0432a9b07eaaf1efed6a137a4a8c12c79552d9b850f84e2a8a021e19e0c9bab2"
    "400000a056e81f171bcc55a6ff8345e692c0f86e5b48e01b996cadc001622fb5e363b421a0c5d2460186f7233c927e7d"
    "b2dcc703c0e500b653ca82273b7bfad8045d85a470"};
const std::string ANVIL_STORAGE_ROOT_NODE{
    "f8918080a09ae7808229da169d14e5bc2a4429fd27b491c217358e63b4d2546738a7e9e26580808080808080a07532bd"
    "0dea31b59e91c77665fa3e608c21cbb2579e2fb55e923294bef0f6e1a2a0d4abfd3b8c53829c6f93b9ee3670d3ddd0f8"
    "4953234cd601e37df89387775989a0908b02e5ad3f81879065681a2528e1bfb368122d3f746dfd1c78c9eb95a849b280"
    "808080"};
const std::string ANVIL_SLOT0_LEAF{"e2a0390decd9548b62a8d60345a988386fc84ba6bc95484008f6362f93160ef3e56305"};
const std::string ANVIL_SLOT1_LEAF{"e6a0310e2d527612073b26eecdfd717e6a320cf44b4afac2b0732d9fcbe2b7fa0cf684830f4240"};
const std::string ANVIL_SLOT3_LEAF{"e5a032575a0e9e593c00f959f8c92f12db2869c3395a3b0502d05e2516446f71f85b83821234"};
const std::string ANVIL_SLOT7_LEAF{
    "f843a0366cc928b5edb82af9bd49922954155ab7b0942694bea4ce44661d9a8736c688a1a0ffffffffffffffffffffff"
    "ffffffffffffffffffffffffffffffffffffffffff"};

//! A small raw-key trie whose root is a branch with hashed leaves under
//! nibbles 1 and 2.
struct TwoLeafTrie {
    EthTrie trie;
    uint256 key_a{KeyOf(0x10, 0x11)};
    uint256 key_b{KeyOf(0x20, 0x22)};
    Bytes value_a{Filled(32, 0xaa)};
    Bytes value_b{Filled(32, 0xbb)};
    TwoLeafTrie()
    {
        trie.Insert(Span(key_a), value_a);
        trie.Insert(Span(key_b), value_b);
    }
};

//! Two raw keys sharing 63 nibbles: the root is an extension over an inline
//! branch whose two leaves are inline too, so every proof is one node.
struct InlineTrie {
    EthTrie trie;
    uint256 key_1{KeyOf(0xab, 0x00, 0x01)};
    uint256 key_2{KeyOf(0xab, 0x00, 0x02)};
    InlineTrie()
    {
        trie.Insert(Span(key_1), Bytes{0x05});
        trie.Insert(Span(key_2), Bytes{0x06});
    }
};

} // namespace

BOOST_AUTO_TEST_CASE(constants_and_builder_known_roots)
{
    const Bytes empty_rlp{0x80};
    BOOST_CHECK(EMPTY_TRIE_ROOT == Keccak(empty_rlp));
    BOOST_CHECK(EMPTY_TRIE_ROOT == Raw("56e81f171bcc55a6ff8345e692c0f86e5b48e01b996cadc001622fb5e363b421"));
    BOOST_CHECK(EMPTY_CODE_HASH == Keccak(Bytes{}));
    BOOST_CHECK(EMPTY_CODE_HASH == Raw("c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470"));
    BOOST_CHECK(EthTrie{}.Root() == EMPTY_TRIE_ROOT);

    // The classic Ethereum trie vector, with a value in a branch node ("do").
    EthTrie trie;
    const std::vector<std::pair<std::string, std::string>> pairs{
        {"do", "verb"}, {"dog", "puppy"}, {"doge", "coin"}, {"horse", "stallion"}};
    auto bytes = [](const std::string& s) { return Bytes(s.begin(), s.end()); };
    for (const auto& [k, v] : pairs) trie.Insert(bytes(k), bytes(v));
    BOOST_CHECK(trie.Root() == Raw("5991bb8c6514148a29db676a14ac506cd2cd5775ace63c30a4fe457715e9ac84"));
    // The builder's proofs satisfy the independent inclusion verifier.
    for (const auto& [k, v] : pairs) {
        const auto value{VerifyMptProof(trie.Root(), bytes(k), trie.Proof(bytes(k)))};
        BOOST_REQUIRE(value);
        BOOST_CHECK(*value == bytes(v));
    }
}

BOOST_AUTO_TEST_CASE(anvil_eip1186_vectors)
{
    const uint256 state_root{Raw(ANVIL_STATE_ROOT)};
    const Nodes vault_proof{HexNodes({ANVIL_STATE_ROOT_NODE, ANVIL_VAULT_BRANCH, ANVIL_VAULT_LEAF})};
    std::string error;

    const auto vault{VerifyAccountProof(state_root, Address(ANVIL_VAULT), vault_proof, error)};
    BOOST_REQUIRE_MESSAGE(vault, error);
    BOOST_CHECK_EQUAL(vault->nonce, 1U);
    BOOST_CHECK(vault->balance == StorageWord{});
    BOOST_CHECK(vault->storage_root == Raw(ANVIL_VAULT_STORAGE_ROOT));
    BOOST_CHECK(vault->code_hash == Raw(ANVIL_VAULT_CODE_HASH));
    BOOST_CHECK(vault->code_hash == Keccak(FromHexStr("600160005260206000f3")));

    const Bytes storage_root_node{FromHexStr(ANVIL_STORAGE_ROOT_NODE)};
    const std::vector<std::pair<uint64_t, std::string>> present{
        {0, ANVIL_SLOT0_LEAF}, {1, ANVIL_SLOT1_LEAF}, {3, ANVIL_SLOT3_LEAF}, {7, ANVIL_SLOT7_LEAF}};
    const std::vector<StorageWord> expected{
        SlotWord(5), SlotWord(1'000'000), SlotWord(0x1234), WordOf(std::string(64, 'f'))};
    for (size_t i = 0; i < present.size(); ++i) {
        const Nodes proof{storage_root_node, FromHexStr(present[i].second)};
        const auto word{VerifyStorageValue(vault->storage_root, SlotWord(present[i].first), proof, error)};
        BOOST_REQUIRE_MESSAGE(word, error);
        BOOST_CHECK(*word == expected[i]);
        // The same proof never shows another slot holding this value: it is
        // refused, or (when the other key diverges in the same leaf) zero.
        const auto other{VerifyStorageValue(vault->storage_root, SlotWord(present[i].first + 100), proof, error)};
        BOOST_CHECK(!other || *other == StorageWord{});
    }
    // Slot 2 is unset: an empty child of the storage root proves it is zero.
    const auto zero{VerifyStorageValue(vault->storage_root, SlotWord(2), Nodes{storage_root_node}, error)};
    BOOST_REQUIRE_MESSAGE(zero, error);
    BOOST_CHECK(*zero == StorageWord{});

    const Nodes funded_proof{HexNodes({ANVIL_STATE_ROOT_NODE, ANVIL_FUNDED_BRANCH, ANVIL_FUNDED_LEAF})};
    const auto funded{VerifyAccountProof(state_root, Address(ANVIL_FUNDED), funded_proof, error)};
    BOOST_REQUIRE_MESSAGE(funded, error);
    BOOST_CHECK_EQUAL(funded->nonce, 42U);
    BOOST_CHECK(funded->balance == WordOf("00000000000000000000000000000000000000000000021e19e0c9bab2400000"));
    BOOST_CHECK(funded->storage_root == EMPTY_TRIE_ROOT);
    BOOST_CHECK(funded->code_hash == EMPTY_CODE_HASH);

    // An absent account: the second-level branch has an empty child.
    const Nodes missing_proof{HexNodes({ANVIL_STATE_ROOT_NODE, ANVIL_FUNDED_BRANCH})};
    const auto lookup{VerifySecureMptProof(state_root, AccountTrieKey(Address(ANVIL_MISSING)), missing_proof, error)};
    BOOST_REQUIRE_MESSAGE(lookup, error);
    BOOST_CHECK(!lookup->present);
    error.clear();
    BOOST_CHECK(!VerifyAccountProof(state_root, Address(ANVIL_MISSING), missing_proof, error));
    BOOST_CHECK_EQUAL(error, "account is absent");

    // Every single-bit flip in every node of a real proof is refused.
    for (size_t n = 0; n < vault_proof.size(); ++n) {
        for (size_t i = 0; i < vault_proof[n].size(); ++i) {
            Nodes mutated{vault_proof};
            mutated[n][i] ^= 0x01;
            BOOST_CHECK(!VerifyAccountProof(state_root, Address(ANVIL_VAULT), mutated, error));
        }
    }
}

BOOST_AUTO_TEST_CASE(builder_account_and_storage_inclusion)
{
    // A storage trie with the slots a vault observation reads, a mapping
    // entry, a full 32-byte word and enough other slots for deep branches.
    const EthAddress vault{Address(ANVIL_VAULT)};
    EthStorageTrie storage;
    std::vector<std::pair<StorageWord, StorageWord>> slots{
        {SlotWord(0), SlotWord(7)},
        {SlotWord(1), SlotWord(123'456'789'000)},
        {SlotWord(3), SlotWord(55)},
        {MappingSlot(AddressWord(vault), 2), SlotWord(1)},
        {SlotWord(9), WordOf("8000000000000000000000000000000000000000000000000000000000000001")},
    };
    for (uint64_t i = 100; i < 160; ++i) slots.emplace_back(SlotWord(i), SlotWord(i * 0x1000193 + 1));
    for (const auto& [slot, word] : slots) storage.Set(slot, word);
    const uint256 storage_root{storage.Root()};

    std::string error;
    for (const auto& [slot, word] : slots) {
        const auto proof{storage.Proof(slot)};
        const auto got{VerifyStorageValue(storage_root, slot, proof, error)};
        BOOST_REQUIRE_MESSAGE(got, error);
        BOOST_CHECK(*got == word);
        // Cross-check with the independent inclusion-only verifier.
        const uint256 key{StorageTrieKey(slot)};
        const auto raw{VerifyMptProof(storage_root, Span(key), proof)};
        BOOST_REQUIRE(raw);
        BOOST_CHECK(*raw == bridge_test::EncodeStorageValue(word));
    }
    for (uint64_t i = 1000; i < 1040; ++i) {
        const auto got{VerifyStorageValue(storage_root, SlotWord(i), storage.Proof(SlotWord(i)), error)};
        BOOST_REQUIRE_MESSAGE(got, error);
        BOOST_CHECK(*got == StorageWord{});
    }
    // Clearing a slot to zero removes it; its proof then shows absence.
    storage.Set(SlotWord(3), StorageWord{});
    const auto cleared{VerifyStorageValue(storage.Root(), SlotWord(3), storage.Proof(SlotWord(3)), error)};
    BOOST_REQUIRE_MESSAGE(cleared, error);
    BOOST_CHECK(*cleared == StorageWord{});

    // A state trie holding the vault and forty other accounts.
    EthTrie state;
    const uint256 code_hash{Keccak(FromHexStr("6001"))};
    StorageWord balance{SlotWord(0)};
    balance[0] = 0x01; // a full 32-byte balance
    state.InsertSecure(vault, EncodeAccount(3, balance, storage_root, code_hash));
    for (unsigned char i = 1; i <= 40; ++i) {
        EthAddress other{};
        other.fill(i);
        state.InsertSecure(other, EncodeAccount(i, SlotWord(i * 1000), EMPTY_TRIE_ROOT, EMPTY_CODE_HASH));
    }
    const auto account{VerifyAccountProof(state.Root(), vault, state.ProofSecure(vault), error)};
    BOOST_REQUIRE_MESSAGE(account, error);
    BOOST_CHECK_EQUAL(account->nonce, 3U);
    BOOST_CHECK(account->balance == balance);
    BOOST_CHECK(account->storage_root == storage_root);
    BOOST_CHECK(account->code_hash == code_hash);
    EthAddress absent{};
    absent.fill(0xee);
    error.clear();
    BOOST_CHECK(!VerifyAccountProof(state.Root(), absent, state.ProofSecure(absent), error));
    BOOST_CHECK_EQUAL(error, "account is absent");
}

BOOST_AUTO_TEST_CASE(inline_nodes)
{
    InlineTrie t;
    const Nodes proof{t.trie.Proof(Span(t.key_1))};
    // Extension root -> inline branch -> inline leaves: one listed node.
    BOOST_REQUIRE_EQUAL(proof.size(), 1U);
    BOOST_CHECK_LT(proof[0].size(), 64U);
    const auto one{CheckAccepted(t.trie.Root(), t.key_1, proof)};
    BOOST_CHECK(one.present);
    BOOST_CHECK(one.value == Bytes{0x05});
    const auto two{CheckAccepted(t.trie.Root(), t.key_2, t.trie.Proof(Span(t.key_2)))};
    BOOST_CHECK(two.present);
    BOOST_CHECK(two.value == Bytes{0x06});
    // The same inline branch has an empty child for nibble 3.
    const uint256 key_3{KeyOf(0xab, 0x00, 0x03)};
    BOOST_CHECK(!CheckAccepted(t.trie.Root(), key_3, t.trie.Proof(Span(key_3))).present);
    const auto independent{VerifyMptProof(t.trie.Root(), Span(t.key_1), proof)};
    BOOST_REQUIRE(independent);
    BOOST_CHECK(*independent == Bytes{0x05});
}

BOOST_AUTO_TEST_CASE(exclusion_forms)
{
    std::string error;
    // 1. Empty trie, no nodes.
    {
        const auto r{CheckAccepted(EMPTY_TRIE_ROOT, KeyOf(0x12, 0x34), {})};
        BOOST_CHECK(!r.present);
        const auto word{VerifyStorageValue(EMPTY_TRIE_ROOT, SlotWord(0), {}, error)};
        BOOST_REQUIRE_MESSAGE(word, error);
        BOOST_CHECK(*word == StorageWord{});
    }
    TwoLeafTrie t;
    const uint256 root{t.trie.Root()};
    // 2. Empty branch child (nibble 3 under the root branch).
    {
        const uint256 key{KeyOf(0x30, 0x33)};
        const Nodes proof{t.trie.Proof(Span(key))};
        BOOST_REQUIRE_EQUAL(proof.size(), 1U);
        BOOST_CHECK(!CheckAccepted(root, key, proof).present);
        Nodes extra{proof};
        extra.push_back(t.trie.Proof(Span(t.key_a)).back());
        CheckRefused(root, key, extra, "state proof proves absence before its last node");
    }
    // 3. Diverging leaf (nibble 1, then a different path).
    {
        const uint256 key{KeyOf(0x1f, 0x11)};
        const Nodes proof{t.trie.Proof(Span(key))};
        BOOST_REQUIRE_EQUAL(proof.size(), 2U);
        BOOST_CHECK(proof == t.trie.Proof(Span(t.key_a)));
        BOOST_CHECK(!CheckAccepted(root, key, proof).present);
        Nodes extra{proof};
        extra.push_back(proof.back());
        CheckRefused(root, key, extra, "state proof proves absence before its last node");
    }
    // 4. Diverging extension (the inline trie's root extension).
    {
        InlineTrie it;
        const uint256 key{KeyOf(0xac, 0x00, 0x01)};
        const Nodes proof{it.trie.Proof(Span(key))};
        BOOST_REQUIRE_EQUAL(proof.size(), 1U);
        BOOST_CHECK(!CheckAccepted(it.trie.Root(), key, proof).present);
        // A diverging extension that is not the last node.
        const uint256 deep{KeyOf(0x55, 0x66)};
        const Bytes nib{Nibbles(deep)};
        Bytes other_path{Slice(nib, 0, 3)};
        other_path[2] ^= 0x01;
        const Bytes branch_child{Leaf(Slice(nib, 4, 64), Filled(32, 0x01))};
        std::array<Bytes, 16> children;
        children[0] = HashRef(branch_child);
        children[1] = HashRef(branch_child);
        const Bytes branch{Branch(children, EmptyItem())};
        const Bytes ext{Extension(other_path, HashRef(branch))};
        BOOST_CHECK(!CheckAccepted(Keccak(ext), deep, {ext}).present);
        CheckRefused(Keccak(ext), deep, {ext, branch}, "state proof proves absence before its last node");
    }
    // 5. A diverging leaf with a full 64-nibble path at the root.
    {
        const uint256 key{KeyOf(0x01, 0x02)};
        const Bytes leaf{Leaf(Nibbles(KeyOf(0x01, 0x03)), Filled(4, 0x09))};
        BOOST_CHECK(!CheckAccepted(Keccak(leaf), key, {leaf}).present);
        const Bytes same{Leaf(Nibbles(key), Filled(4, 0x09))};
        const auto r{CheckAccepted(Keccak(same), key, {same})};
        BOOST_CHECK(r.present);
        BOOST_CHECK(r.value == Filled(4, 0x09));
    }
}

BOOST_AUTO_TEST_CASE(malformed_walks_refused)
{
    TwoLeafTrie t;
    const uint256 root{t.trie.Root()};
    const Nodes good{t.trie.Proof(Span(t.key_a))};
    BOOST_REQUIRE_EQUAL(good.size(), 2U);
    BOOST_CHECK(CheckAccepted(root, t.key_a, good).value == t.value_a);

    // Wrong root.
    uint256 wrong{root};
    *wrong.begin() ^= 0x01;
    CheckRefused(wrong, t.key_a, good, "state proof root node does not match the root");
    // Extra node after a present leaf.
    Nodes extra{good};
    extra.push_back(good.back());
    CheckRefused(root, t.key_a, extra, "state proof has unused nodes");
    // Missing node.
    CheckRefused(root, t.key_a, {good[0]}, "state proof is missing a node");
    // Hash-mismatched node.
    Nodes mismatched{good};
    mismatched[1].back() ^= 0x01;
    CheckRefused(root, t.key_a, mismatched, "state proof node does not match its reference");
    // Empty proof against a non-empty root; nodes against the empty root.
    CheckRefused(root, t.key_a, {}, "state proof is empty for a non-empty root");
    CheckRefused(EMPTY_TRIE_ROOT, t.key_a, good, "state proof has nodes for the empty root");
    CheckRefused(EMPTY_TRIE_ROOT, t.key_a, {Bytes{0x80}}, "state proof has nodes for the empty root");

    const uint256 key{KeyOf(0x33, 0x44)};
    const Bytes nib{Nibbles(key)};
    const Bytes hash_child{Enc(Filled(32, 0x77))};

    // Branch value set.
    {
        std::array<Bytes, 16> children;
        children[1] = hash_child;
        const Bytes branch{Branch(children, Enc(Bytes{'x', 'y'}))};
        CheckRefused(Keccak(branch), key, {branch}, "state proof branch carries a value");
    }
    // Key exhausted at a branch: a 63-nibble extension, a branch for the
    // last nibble, then another branch with no nibble left.
    {
        std::array<Bytes, 16> inner_children;
        inner_children[0] = hash_child;
        const Bytes inner{Branch(inner_children, EmptyItem())};
        BOOST_REQUIRE_GE(inner.size(), 32U);
        std::array<Bytes, 16> outer_children;
        outer_children[nib[63]] = HashRef(inner);
        const Bytes outer{Branch(outer_children, EmptyItem())};
        const Bytes ext{Extension(Slice(nib, 0, 63), HashRef(outer))};
        CheckRefused(Keccak(ext), key, {ext, outer, inner}, "state proof key ends inside the trie");
        // An extension that consumes the whole key, matching or not: it is
        // malformed, never a proof of absence.
        const Bytes whole{Extension(nib, HashRef(outer))};
        CheckRefused(Keccak(whole), key, {whole, outer}, "state proof key ends inside the trie");
        Bytes other_nib{nib};
        other_nib[10] ^= 0x01;
        const Bytes whole_other{Extension(other_nib, HashRef(outer))};
        CheckRefused(Keccak(whole_other), key, {whole_other}, "state proof key ends inside the trie");
    }
    // Leaf path length mismatch (63 nibbles left, 64 to go).
    {
        const Bytes leaf{Leaf(Slice(nib, 1, 64), Filled(8, 0x01))};
        CheckRefused(Keccak(leaf), key, {leaf}, "state proof leaf path length does not match the key");
        // Under a branch: 63 nibbles remain, the leaf carries 62.
        const Bytes short_leaf{Leaf(Slice(nib, 2, 64), Filled(32, 0x01))};
        std::array<Bytes, 16> children;
        children[nib[0]] = HashRef(short_leaf);
        children[(nib[0] + 1) % 16] = hash_child;
        const Bytes branch{Branch(children, EmptyItem())};
        CheckRefused(Keccak(branch), key, {branch, short_leaf},
                     "state proof leaf path length does not match the key");
    }
    // Extension with an empty path.
    {
        const Bytes ext{RlpEncodeList({Enc(Bytes{0x00}), hash_child})};
        CheckRefused(Keccak(ext), key, {ext}, "state proof extension has an empty path");
    }
    // Extension whose child is a leaf.
    {
        const Bytes leaf{Leaf(Slice(nib, 1, 64), Filled(32, 0x01))};
        const Bytes ext{Extension(Slice(nib, 0, 1), HashRef(leaf))};
        CheckRefused(Keccak(ext), key, {ext, leaf}, "state proof extension does not lead to a branch");
    }
    // A hash-referenced node shorter than 32 bytes (it must be inline).
    {
        std::array<Bytes, 16> children;
        children[nib[63]] = Leaf({}, Bytes{0x05});
        children[(nib[63] + 1) % 16] = Leaf({}, Bytes{0x06});
        const Bytes small_branch{Branch(children, EmptyItem())};
        BOOST_REQUIRE_LT(small_branch.size(), 32U);
        const Bytes ext{Extension(Slice(nib, 0, 63), HashRef(small_branch))};
        CheckRefused(Keccak(ext), key, {ext, small_branch}, "state proof hashed node is shorter than 32 bytes");
        // The canonical form embeds it and verifies.
        const Bytes canonical{Extension(Slice(nib, 0, 63), small_branch)};
        const auto r{CheckAccepted(Keccak(canonical), key, {canonical})};
        BOOST_CHECK(r.present);
        BOOST_CHECK(r.value == Bytes{0x05});
    }
    // Malformed child references: a 31-byte string, an inline node of 32+
    // bytes, a list as an extension child that is too long.
    {
        std::array<Bytes, 16> children;
        children[5] = Enc(Filled(31, 0x01));
        const Bytes branch{Branch(children, EmptyItem())};
        CheckRefused(Keccak(branch), key, {branch}, "state proof child reference is malformed");

        std::array<Bytes, 16> fat;
        fat[nib[0]] = Leaf(Slice(nib, 1, 64), Filled(4, 0x01)); // >= 32 bytes, inline
        BOOST_REQUIRE_GE(fat[nib[0]].size(), 32U);
        const Bytes fat_branch{Branch(fat, EmptyItem())};
        CheckRefused(Keccak(fat_branch), key, {fat_branch}, "state proof child reference is malformed");

        const Bytes ext{RlpEncodeList({Enc(EthTrie::HexPrefix(Slice(nib, 0, 2), false)), Enc(Filled(20, 0x01))})};
        CheckRefused(Keccak(ext), key, {ext}, "state proof child reference is malformed");
    }
    // Non-canonical RLP, a 3-item node, a string node, bad hex prefixes and
    // leaf values that are lists or empty.
    {
        InlineTrie it;
        const Bytes canonical{it.trie.Proof(Span(it.key_1)).front()};
        BOOST_REQUIRE_EQUAL(canonical[0], 0xf7); // 55-byte payload, short form
        Bytes long_form{0xf8, 0x37};
        long_form.insert(long_form.end(), canonical.begin() + 1, canonical.end());
        CheckRefused(Keccak(long_form), it.key_1, {long_form}, "state proof node is not canonical RLP");

        const Bytes three{RlpEncodeList({Enc(Bytes{0x20}), Enc(Bytes{0x01}), Enc(Bytes{0x02})})};
        CheckRefused(Keccak(three), key, {three}, "state proof node has an invalid shape");
        const Bytes string_node{Enc(Filled(40, 0x01))};
        CheckRefused(Keccak(string_node), key, {string_node}, "state proof node has an invalid shape");

        const Bytes bad_flag{RlpEncodeList({Enc(Bytes{0x40}), Enc(Bytes{0x01})})};
        CheckRefused(Keccak(bad_flag), key, {bad_flag}, "state proof node has an invalid path");
        const Bytes even_nibble{RlpEncodeList({Enc(Bytes{0x21}), Enc(Bytes{0x01})})};
        CheckRefused(Keccak(even_nibble), key, {even_nibble}, "state proof node has an invalid path");
        const Bytes list_path{RlpEncodeList({RlpEncodeList({}), Enc(Bytes{0x01})})};
        CheckRefused(Keccak(list_path), key, {list_path}, "state proof node has an invalid path");

        const Bytes list_value{RlpEncodeList({Enc(EthTrie::HexPrefix(nib, true)), RlpEncodeList({})})};
        CheckRefused(Keccak(list_value), key, {list_value}, "state proof leaf value is malformed");
        const Bytes empty_value{RlpEncodeList({Enc(EthTrie::HexPrefix(nib, true)), EmptyItem()})};
        CheckRefused(Keccak(empty_value), key, {empty_value}, "state proof leaf value is malformed");
    }
}

BOOST_AUTO_TEST_CASE(size_limits)
{
    BOOST_CHECK_EQUAL(STATE_PROOF_LIMITS.max_nodes, 64U);
    BOOST_CHECK_EQUAL(STATE_PROOF_LIMITS.max_node_size, 1024U);
    BOOST_CHECK_EQUAL(STATE_PROOF_LIMITS.max_total_bytes, 11264U);

    const uint256 key{KeyOf(0x42, 0x24)};
    const Bytes nib{Nibbles(key)};
    // A root leaf of exactly 1024 bytes passes; 1025 bytes is refused.
    {
        const Bytes at_limit{Leaf(nib, Filled(984, 0x01))};
        BOOST_REQUIRE_EQUAL(at_limit.size(), 1024U);
        BOOST_CHECK(CheckAccepted(Keccak(at_limit), key, {at_limit}).present);
        const Bytes over{Leaf(nib, Filled(985, 0x01))};
        BOOST_REQUIRE_EQUAL(over.size(), 1025U);
        CheckRefused(Keccak(over), key, {over}, "state proof node size is out of range");
        CheckRefused(Keccak(at_limit), key, {at_limit, Bytes{}}, "state proof node size is out of range");
    }
    TwoLeafTrie t;
    const uint256 root{t.trie.Root()};
    const Nodes good{t.trie.Proof(Span(t.key_a))};
    // 64 nodes reach the walk (and fail as unused); 65 never do.
    {
        Nodes padded{good};
        while (padded.size() < 64) padded.push_back(good.back());
        CheckRefused(root, t.key_a, padded, "state proof has unused nodes");
        padded.push_back(good.back());
        CheckRefused(root, t.key_a, padded, "state proof has too many nodes");
    }
    // Exactly 11264 bytes reach the walk; one more byte is refused.
    {
        Nodes padded{good};
        size_t total{good[0].size() + good[1].size()};
        while (total + 1024 <= STATE_PROOF_LIMITS.max_total_bytes) {
            padded.push_back(Filled(1024, 0x01));
            total += 1024;
        }
        padded.push_back(Filled(STATE_PROOF_LIMITS.max_total_bytes - total, 0x01));
        CheckRefused(root, t.key_a, padded, "state proof has unused nodes");
        padded.back().push_back(0x01);
        CheckRefused(root, t.key_a, padded, "state proof exceeds the byte limit");
    }
}

BOOST_AUTO_TEST_CASE(storage_values_refused)
{
    // Leaf values a storage trie never holds, each beside valid slots.
    const std::vector<std::pair<std::string, Bytes>> bad{
        {"zero byte", Bytes{0x00}},
        {"leading zero", RlpEncodeBytes(Bytes{0x00, 0x01})},
        {"33 bytes", RlpEncodeBytes(Filled(33, 0x01))},
        {"list", RlpEncodeList({})},
        {"non-canonical", Bytes{0x81, 0x05}},
        {"trailing byte", Bytes{0x05, 0x05}},
    };
    const StorageWord slot{SlotWord(4)};
    for (const auto& [name, value] : bad) {
        EthStorageTrie storage;
        storage.Set(SlotWord(0), SlotWord(1));
        storage.Set(SlotWord(1), SlotWord(2));
        const uint256 hashed{StorageTrieKey(slot)};
        storage.Raw().Insert(Span(hashed), value);
        std::string error;
        BOOST_CHECK_MESSAGE(!VerifyStorageValue(storage.Root(), slot, storage.Proof(slot), error), name);
        BOOST_CHECK_EQUAL(error, "storage value is malformed");
        // The neighbours still verify.
        const auto ok{VerifyStorageValue(storage.Root(), SlotWord(1), storage.Proof(SlotWord(1)), error)};
        BOOST_REQUIRE_MESSAGE(ok, error);
        BOOST_CHECK(*ok == SlotWord(2));
    }
    // Boundaries that are valid: one byte, 32 bytes with a non-zero first byte.
    EthStorageTrie storage;
    // A stored zero (the RLP of zero, 0x80): Ethereum clients delete zero
    // slots, so node changes section 8.2 calls it malformed and the default
    // (StoredZero::REFUSE) refuses it. anvil keeps slots a transaction only
    // read as such leaves (seen in RT4 for the token's slot 10 and
    // isBlackListed[vault] after a deposit), so the regtest origin chain
    // passes StoredZero::ACCEPT. The root commits to the leaf, so it then
    // reads as the zero word.
    {
        EthStorageTrie touched;
        touched.Set(SlotWord(0), SlotWord(1));
        touched.Raw().Insert(Span(StorageTrieKey(slot)), Bytes{0x80});
        std::string zero_error;
        BOOST_CHECK(!VerifyStorageValue(touched.Root(), slot, touched.Proof(slot), zero_error));
        BOOST_CHECK_EQUAL(zero_error, "storage value is a stored zero, which only anvil keeps");
        BOOST_CHECK(!VerifyStorageValue(touched.Root(), slot, touched.Proof(slot), zero_error,
                                        StoredZero::REFUSE));
        const auto zero{VerifyStorageValue(touched.Root(), slot, touched.Proof(slot), zero_error,
                                           StoredZero::ACCEPT)};
        BOOST_REQUIRE_MESSAGE(zero, zero_error);
        BOOST_CHECK(*zero == StorageWord{});
        // A different root still refuses the same proof.
        BOOST_CHECK(!VerifyStorageValue(storage.Root(), slot, touched.Proof(slot), zero_error,
                                        StoredZero::ACCEPT));
    }
    storage.Set(slot, SlotWord(1));
    std::string error;
    BOOST_CHECK(VerifyStorageValue(storage.Root(), slot, storage.Proof(slot), error));
    storage.Set(slot, WordOf(std::string(2, '0') + std::string(62, '1')));
    const auto shorter{VerifyStorageValue(storage.Root(), slot, storage.Proof(slot), error)};
    BOOST_REQUIRE_MESSAGE(shorter, error);
    BOOST_CHECK((*shorter)[0] == 0x00 && (*shorter)[1] == 0x11);
}

BOOST_AUTO_TEST_CASE(anvil_stored_zero_vector)
{
    // A real anvil 1.7.1 eth_getProof (chain 31337, local only): a contract
    // whose runtime is SLOAD(0) RETURN, called once, so slot 0 was read and
    // never written. anvil reports storageHash 0xb9d1...720e (not the empty
    // trie root) and proves slot 0 with one leaf holding 0x80, the stored
    // zero Ethereum clients never keep.
    const uint256 storage_root{Raw("b9d1a401293d84978870a16d07fb2687c2fe446f94302b9b89d1fdfcc17f720e")};
    const Nodes proof{HexNodes({"e4a120290decd9548b62a8d60345a988386fc84ba6bc95484008f6362f93160ef3e5638180"})};
    std::string error;
    BOOST_CHECK(!VerifyStorageValue(storage_root, SlotWord(0), proof, error));
    BOOST_CHECK_EQUAL(error, "storage value is a stored zero, which only anvil keeps");
    const auto zero{VerifyStorageValue(storage_root, SlotWord(0), proof, error, StoredZero::ACCEPT)};
    BOOST_REQUIRE_MESSAGE(zero, error);
    BOOST_CHECK(*zero == StorageWord{});
    // The proof names slot 0 only: slot 1 diverges at the leaf (absent).
    const auto other{VerifyStorageValue(storage_root, SlotWord(1), proof, error, StoredZero::ACCEPT)};
    BOOST_REQUIRE_MESSAGE(other, error);
    BOOST_CHECK(*other == StorageWord{});
}

BOOST_AUTO_TEST_CASE(account_values_refused)
{
    const EthAddress address{Address(ANVIL_VAULT)};
    const Bytes root32{Enc(Span(EMPTY_TRIE_ROOT))};
    const Bytes code32{Enc(Span(EMPTY_CODE_HASH))};
    const Bytes nonce{RlpEncodeUint64(1)};
    const Bytes balance{RlpEncodeUint64(1000)};
    const std::vector<std::pair<std::string, Bytes>> bad{
        {"3 items", RlpEncodeList({nonce, balance, root32})},
        {"5 items", RlpEncodeList({nonce, balance, root32, code32, nonce})},
        {"31-byte storage root", RlpEncodeList({nonce, balance, Enc(Filled(31, 0x01)), code32})},
        {"33-byte code hash", RlpEncodeList({nonce, balance, root32, Enc(Filled(33, 0x01))})},
        {"list storage root", RlpEncodeList({nonce, balance, RlpEncodeList({}), code32})},
        {"padded nonce", RlpEncodeList({RlpEncodeBytes(Bytes{0x00, 0x01}), balance, root32, code32})},
        {"zero-byte nonce", RlpEncodeList({Bytes{0x00}, balance, root32, code32})},
        {"9-byte nonce", RlpEncodeList({RlpEncodeBytes(Filled(9, 0x01)), balance, root32, code32})},
        {"33-byte balance", RlpEncodeList({nonce, RlpEncodeBytes(Filled(33, 0x01)), root32, code32})},
        {"padded balance", RlpEncodeList({nonce, RlpEncodeBytes(Bytes{0x00, 0x01}), root32, code32})},
        {"list nonce", RlpEncodeList({RlpEncodeList({}), balance, root32, code32})},
        {"string account", RlpEncodeBytes(Filled(40, 0x01))},
    };
    for (const auto& [name, value] : bad) {
        EthTrie state;
        state.InsertSecure(address, value);
        EthAddress other{};
        other.fill(0x42);
        state.InsertSecure(other, EncodeAccount(0, SlotWord(1), EMPTY_TRIE_ROOT, EMPTY_CODE_HASH));
        std::string error;
        BOOST_CHECK_MESSAGE(!VerifyAccountProof(state.Root(), address, state.ProofSecure(address), error), name);
        BOOST_CHECK_EQUAL(error, "account value is malformed");
    }
    // Boundaries that are valid: an 8-byte nonce and a 32-byte balance.
    StorageWord full{};
    full.fill(0xff);
    EthTrie state;
    state.InsertSecure(address, EncodeAccount(UINT64_MAX, full, EMPTY_TRIE_ROOT, EMPTY_CODE_HASH));
    std::string error;
    const auto account{VerifyAccountProof(state.Root(), address, state.ProofSecure(address), error)};
    BOOST_REQUIRE_MESSAGE(account, error);
    BOOST_CHECK_EQUAL(account->nonce, UINT64_MAX);
    BOOST_CHECK(account->balance == full);
}

BOOST_AUTO_TEST_CASE(slot_helpers)
{
    BOOST_CHECK(SlotWord(0) == StorageWord{});
    BOOST_CHECK(SlotWord(0x0102030405060708) ==
                WordOf("0000000000000000000000000000000000000000000000000102030405060708"));
    const EthAddress vault{Address(ANVIL_VAULT)};
    BOOST_CHECK(AddressWord(vault) ==
                WordOf("0000000000000000000000005fbdb2315678afecb367f032d93f642f64180aa3"));
    // Vectors computed with `cast index address <vault> <slot>`.
    BOOST_CHECK(MappingSlot(AddressWord(vault), 2) ==
                WordOf("4ace221c3c265e8444dd224598300347d682bc5b143f1c7eaacaff6645692070"));
    BOOST_CHECK(MappingSlot(AddressWord(vault), 6) ==
                WordOf("cf5ad832f9ea9114b8af5f3aa1e68b6212e43f857e1939607b2573abeb2a43ef"));
    // The storage-trie key of slot 0 is keccak(0^32), the key in ANVIL_SLOT0_LEAF.
    BOOST_CHECK(StorageTrieKey(SlotWord(0)) ==
                Raw("290decd9548b62a8d60345a988386fc84ba6bc95484008f6362f93160ef3e563"));

    // WordField reads little-endian bit offsets out of a big-endian word.
    const StorageWord w{WordOf("00000000ffeeddccbbaa99887766554433221100f0e1d2c3b4a5968778695a4b")};
    BOOST_CHECK_EQUAL(WordField(w, 0, 64), 0xb4a5968778695a4bULL);
    BOOST_CHECK_EQUAL(WordField(w, 0, 8), 0x4bU);
    BOOST_CHECK_EQUAL(WordField(w, 8, 8), 0x5aU);
    BOOST_CHECK_EQUAL(WordField(w, 0, 40), 0x8778695a4bULL);
    BOOST_CHECK_EQUAL(WordField(w, 4, 8), 0xa4U);
    BOOST_CHECK_EQUAL(WordField(w, 64, 64), 0x33221100f0e1d2c3ULL);
    BOOST_CHECK_EQUAL(WordField(w, 128, 64), 0xbbaa998877665544ULL);
    BOOST_CHECK_EQUAL(WordField(w, 192, 64), 0x00000000ffeeddccULL);
    BOOST_CHECK_EQUAL(WordField(w, 224, 32), 0U);
    BOOST_CHECK_EQUAL(WordField(w, 255, 1), 0U);
    BOOST_CHECK_EQUAL(WordField(WordOf(std::string(64, 'f')), 255, 1), 1U);
}

BOOST_AUTO_TEST_SUITE_END()
