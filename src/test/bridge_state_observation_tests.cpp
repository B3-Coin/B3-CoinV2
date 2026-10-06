// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

// Bridge V2 kind 9 ETH_STATE_OBSERVATION (node changes section 8.2): the
// record codec in bridge/proof.h and the account/storage decoding in
// bridge/state_observation.h.
//
// Vector provenance. The slot numbers, bit offsets, example words and
// mapping keys below are copied from the shared vector file
// contracts/test/vectors/bridge_v2_vectors.json, section "storage_layout_v2"
// (written by contracts/test/vectors/gen_v2_vectors.sh, which asserts the
// mapping keys against `cast index address`). Foundry's
// StorageLayoutV2.t.sol checks the same JSON against the deployed Verifier
// V2, Vault V2 and MockTetherToken, so a regenerated file must be copied
// here again. Proofs come from the test-only trie builder in
// test/util/bridge_eth_trie.h, whose walk is independent of
// bridge/state_proof.h's verifier.

#include <bridge/deposit.h>
#include <bridge/proof.h>
#include <bridge/state_observation.h>
#include <bridge/state_proof.h>
#include <consensus/amount.h>
#include <consensus/bridge_params.h>
#include <crypto/keccak256.h>
#include <test/util/bridge_eth_trie.h>
#include <test/util/bridge_observation_world.h>
#include <uint256.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

using namespace bridge;
using bridge_test::EncodeAccount;
using bridge_test::EthStorageTrie;
using bridge_test::EthTrie;

BOOST_AUTO_TEST_SUITE(bridge_state_observation_tests)

namespace {

using Bytes = std::vector<unsigned char>;
using Nodes = std::vector<Bytes>;

Bytes FromHex(const std::string_view hex)
{
    auto bytes{TryParseHex<unsigned char>(hex)};
    BOOST_REQUIRE(bytes.has_value());
    return *bytes;
}

StorageWord WordOf(const std::string_view hex)
{
    const Bytes bytes{FromHex(hex.substr(0, 2) == "0x" ? hex.substr(2) : hex)};
    BOOST_REQUIRE_EQUAL(bytes.size(), 32U);
    StorageWord out;
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return out;
}

EthAddress AddressOf(const std::string_view hex)
{
    const Bytes bytes{FromHex(hex.substr(0, 2) == "0x" ? hex.substr(2) : hex)};
    BOOST_REQUIRE_EQUAL(bytes.size(), 20U);
    EthAddress out;
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return out;
}

EthAddress FillAddress(const unsigned char b)
{
    EthAddress out;
    out.fill(b);
    return out;
}

uint256 Fill(const unsigned char b)
{
    uint256 out;
    std::fill(out.begin(), out.end(), b);
    return out;
}

StorageWord FillWord(const unsigned char b)
{
    StorageWord out;
    out.fill(b);
    return out;
}

uint256 KeccakOf(const std::string_view text)
{
    uint256 out;
    Keccak256()
        .Write(std::span<const unsigned char>{reinterpret_cast<const unsigned char*>(text.data()), text.size()})
        .Finalize(out);
    return out;
}

//! `word` with bit `bit` (0 = least significant) set.
StorageWord WithBit(StorageWord word, const unsigned bit)
{
    word[31 - bit / 8] |= static_cast<unsigned char>(1U << (bit % 8));
    return word;
}

//! A word holding `value` in `bits` bits at `offset` (value must fit).
StorageWord Packed(std::initializer_list<std::tuple<unsigned, unsigned, uint64_t>> fields)
{
    StorageWord out{};
    for (const auto& [offset, bits, value] : fields) {
        for (unsigned i{0}; i < bits; ++i) {
            if ((value >> i) & 1) out = WithBit(out, offset + i);
        }
    }
    return out;
}

// ------------------------------------------------- storage_layout_v2 vectors

const std::string_view STAND_IN_VAULT{"2222222222222222222222222222222222222222"};
const std::string_view BALANCES_KEY{"74b48303b668c996c09b610daf2f53f83448dd15b9ac57e9e8fb1ff2f745c71d"};
const std::string_view BLACKLIST_KEY{"0a3e3a135c9eb9a4f916a39edbe9cf899cba91b2a6ec56f43a0ae8ee4b3185de"};

struct LivenessExample {
    std::string_view word;
    uint64_t current_epoch;
    int64_t last_rotation_time;
    int64_t last_certificate_time;
    uint64_t latest_bridge_height;
    uint8_t status;
    bool deposits_closed;
};
const std::array<LivenessExample, 2> LIVENESS_EXAMPLES{{
    {"000000000001000000000000014600684f24dc00684f238f0000000000000003", 3, 1'750'016'911, 1'750'017'244, 326, 1, false},
    {"000000000103ffffffffffffffffffffffffffffffffffffffffffffffffffff", std::numeric_limits<uint64_t>::max(),
     1'099'511'627'775, 1'099'511'627'775, std::numeric_limits<uint64_t>::max(), 3, true},
}};

struct LatestExample {
    std::string_view word;
    uint64_t height;
    uint64_t index;
    int64_t init_time;
};
const std::array<LatestExample, 2> LATEST_EXAMPLES{{
    {"000000000000000000000000684ee1ef00000000000000030000000000000114", 276, 3, 1'750'000'111},
    {"0000000000000000000000ffffffffffffffffffffffffffffffffffffffffff", std::numeric_limits<uint64_t>::max(),
     std::numeric_limits<uint64_t>::max(), 1'099'511'627'775},
}};

struct TokenWordExample {
    std::string_view word;
    std::string_view address;
    bool flag;
};
const std::array<TokenWordExample, 2> TOKEN_SLOT0_EXAMPLES{{
    {"0000000000000000000000006666666666666666666666666666666666666666", "6666666666666666666666666666666666666666", false},
    {"0000000000000000000000016666666666666666666666666666666666666666", "6666666666666666666666666666666666666666", true},
}};
const std::array<TokenWordExample, 2> TOKEN_SLOT10_EXAMPLES{{
    {"0000000000000000000000000000000000000000000000000000000000000000", "0000000000000000000000000000000000000000", false},
    {"0000000000000000000000017777777777777777777777777777777777777777", "7777777777777777777777777777777777777777", true},
}};

// ---------------------------------------------------------- a small world

/**
 * An Ethereum state with the three observed contracts (plus filler accounts
 * so the state trie has branches), from which Observe() builds kind 9
 * records exactly as a relayer would from eth_getProof: the plan's accounts
 * and slots, in order. Zero words are never stored, so they are proven by
 * exclusion.
 */
class World
{
public:
    ObservationPins pins;
    std::map<EthAddress, EthStorageTrie> storage;
    std::map<EthAddress, uint256> code;
    std::map<EthAddress, bool> present;

    World()
    {
        pins.origin_chain_id = OBSERVATION_UNPINNED_CODE_CHAIN_ID;
        pins.verifier = FillAddress(0x44);
        pins.vault = AddressOf(STAND_IN_VAULT);
        pins.token = AddressOf("dac17f958d2ee523a2206206994597c13d831ec7");
        pins.token_code_hash = KeccakOf("token runtime");
        code[pins.verifier] = KeccakOf("verifier runtime");
        code[pins.vault] = KeccakOf("vault runtime");
        code[pins.token] = pins.token_code_hash;
        for (const EthAddress& a : {pins.verifier, pins.vault, pins.token}) present[a] = true;

        using namespace observation_layout;
        Set(pins.verifier, VERIFIER_SLOT_LIVENESS, WordOf(LIVENESS_EXAMPLES[0].word));
        Set(pins.verifier, VERIFIER_SLOT_CURRENT_SET_HASH, FillWord(0xa1));
        Set(pins.verifier, VERIFIER_SLOT_NEXT_SET_HASH, FillWord(0xa2));
        Set(pins.verifier, 3, FillWord(0xa3)); // SetSummary: proved by nobody
        Set(pins.verifier, VERIFIER_SLOT_LATEST, WordOf(LATEST_EXAMPLES[0].word));
        Set(pins.verifier, 5, FillWord(0xa5));
        Set(pins.verifier, VERIFIER_SLOT_LATEST_CERT_ID, FillWord(0xa6));

        Set(pins.vault, VAULT_SLOT_NEXT_DEPOSIT_ID, SlotWord(2));
        Set(pins.vault, VAULT_SLOT_LOCKED, SlotWord(1'040'000'000));
        // absorbedTotal stays zero: an exclusion proof.
        for (uint64_t id{0}; id < 12; ++id) SetWord(pins.vault, MappingSlot(SlotWord(id), 2), SlotWord(1));

        Set(pins.token, TOKEN_SLOT_OWNER_PAUSED, WordOf(TOKEN_SLOT0_EXAMPLES[0].word));
        SetWord(pins.token, BalanceSlot(), SlotWord(1'040'000'000));
        for (unsigned char holder{1}; holder <= 30; ++holder) {
            SetWord(pins.token, MappingSlot(AddressWord(FillAddress(holder)), TOKEN_SLOT_BALANCES),
                    SlotWord(holder * 1'000'000ULL));
        }
        // deprecated (slot 10) and isBlackListed[vault] stay zero.
    }

    StorageWord BalanceSlot() const
    {
        return MappingSlot(AddressWord(pins.vault), observation_layout::TOKEN_SLOT_BALANCES);
    }
    StorageWord BlacklistSlot() const
    {
        return MappingSlot(AddressWord(pins.vault), observation_layout::TOKEN_SLOT_IS_BLACK_LISTED);
    }

    void Set(const EthAddress& account, const uint64_t slot, const StorageWord& word)
    {
        SetWord(account, SlotWord(slot), word);
    }
    void SetWord(const EthAddress& account, const StorageWord& slot, const StorageWord& word)
    {
        storage[account].Set(slot, word);
    }

    EthTrie State() const
    {
        EthTrie state;
        for (const auto& [address, is_present] : present) {
            if (!is_present) continue;
            const auto it{storage.find(address)};
            const uint256 storage_root{it == storage.end() ? EMPTY_TRIE_ROOT : it->second.Root()};
            state.InsertSecure(address, EncodeAccount(1, SlotWord(0), storage_root, code.at(address)));
        }
        for (unsigned char i{1}; i <= 24; ++i) {
            state.InsertSecure(FillAddress(static_cast<unsigned char>(0x80 + i)),
                               EncodeAccount(i, SlotWord(i * 1'000ULL), EMPTY_TRIE_ROOT, EMPTY_CODE_HASH));
        }
        return state;
    }

    uint256 StateRoot() const { return State().Root(); }

    BridgeEthStateObservationV2 Observe(const uint8_t subject, const uint8_t verdict = 0) const
    {
        const EthTrie state{State()};
        BridgeEthStateObservationV2 record;
        record.subject = subject;
        record.anchor_hash = Fill(0x3c);
        record.verdict = verdict;
        for (const ObservationAccountRequest& request : ObservationRequests(subject, pins)) {
            BridgeEthAccountProofV2 account;
            account.account_nodes = state.ProofSecure(request.address);
            const auto it{storage.find(request.address)};
            for (const StorageWord& slot : request.slots) {
                account.storage_nodes.push_back(it == storage.end() ? Nodes{} : it->second.Proof(slot));
            }
            record.accounts.push_back(std::move(account));
        }
        return record;
    }

    std::optional<ObservedStateV2> Verify(const uint8_t subject, std::string& error) const
    {
        return VerifyEthStateObservationV2(Observe(subject), StateRoot(), pins, error);
    }
};

template <typename T>
T VerifiedAs(const World& world, const uint8_t subject)
{
    std::string error;
    const auto state{world.Verify(subject, error)};
    BOOST_REQUIRE_MESSAGE(state, error);
    BOOST_REQUIRE(std::holds_alternative<T>(*state));
    return std::get<T>(*state);
}

void CheckRefused(const World& world, const uint8_t subject, const std::string& expected)
{
    std::string error;
    const auto state{world.Verify(subject, error)};
    BOOST_CHECK_MESSAGE(!state, "accepted, expected: " + expected);
    BOOST_CHECK_EQUAL(error, expected);
}

BridgeRecordV1 Kind9(const BridgeEthStateObservationV2& value)
{
    return BridgeRecordV1{BridgeRecordKindV1::ETH_STATE_OBSERVATION, value};
}

std::optional<Bytes> EncodeV2(const BridgeEthStateObservationV2& value)
{
    return EncodeBridgeRecordV1(Kind9(value), BRIDGE_MPT_LIMITS_V2);
}

//! A small record with arbitrary node bytes (no proof meaning).
BridgeEthStateObservationV2 Skeleton(const uint8_t subject)
{
    BridgeEthStateObservationV2 out;
    out.subject = subject;
    out.anchor_hash = Fill(0x3c);
    for (const uint8_t slots : EthObservationShapeV2(subject)) {
        BridgeEthAccountProofV2 account;
        account.account_nodes = {Bytes{0xc1, 0x80}, Bytes(40, 0x11)};
        for (uint8_t i{0}; i < slots; ++i) account.storage_nodes.push_back({Bytes{0xc2, static_cast<unsigned char>(i)}});
        out.accounts.push_back(std::move(account));
    }
    return out;
}

/** Raw kind 9 wire writer with every count explicit, for malformed inputs. */
class RawWire
{
public:
    RawWire(const uint8_t subject, const uint256& anchor, const uint8_t verdict, const uint8_t account_count)
    {
        U8(9);
        U8(0);
        U8(subject);
        m_out.insert(m_out.end(), anchor.begin(), anchor.end());
        U8(verdict);
        U8(account_count);
    }
    RawWire& U8(const uint8_t v)
    {
        m_out.push_back(v);
        return *this;
    }
    //! A node list: u8 count, then u16 length and bytes per node.
    RawWire& List(const Nodes& nodes, std::optional<uint8_t> count = std::nullopt)
    {
        U8(count.value_or(static_cast<uint8_t>(nodes.size())));
        for (const Bytes& node : nodes) {
            U8(static_cast<uint8_t>(node.size() >> 8));
            U8(static_cast<uint8_t>(node.size()));
            m_out.insert(m_out.end(), node.begin(), node.end());
        }
        return *this;
    }
    //! A node length prefix without the bytes (for zero-length nodes).
    RawWire& Length(const uint16_t size)
    {
        U8(static_cast<uint8_t>(size >> 8));
        return U8(static_cast<uint8_t>(size));
    }
    const Bytes& Get() const { return m_out; }

private:
    Bytes m_out;
};

//! TOKEN_FLAGS in raw form: one account with `slot_count` storage lists.
RawWire TokenWire(const uint8_t slot_count = 3, const uint8_t verdict = 0)
{
    RawWire wire{ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS, Fill(0x3c), verdict, 1};
    wire.List({Bytes{0xc1, 0x80}});
    wire.U8(slot_count);
    for (uint8_t i{0}; i < slot_count; ++i) wire.List({Bytes{0xc2, i}});
    return wire;
}

bool DecodesV2(const Bytes& bytes) { return DecodeBridgeRecordV1(bytes, BRIDGE_MPT_LIMITS_V2).has_value(); }

} // namespace

// ------------------------------------------------------------------ codec

BOOST_AUTO_TEST_CASE(codec_round_trip_and_wire_layout)
{
    const World world;
    for (const uint8_t subject : {ETH_OBSERVATION_SUBJECT_VERIFIER, ETH_OBSERVATION_SUBJECT_RESERVE,
                                  ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS}) {
        BOOST_TEST_CONTEXT("subject " << int{subject})
        {
            const BridgeEthStateObservationV2 value{world.Observe(subject)};
            const auto encoded{EncodeV2(value)};
            BOOST_REQUIRE(encoded);
            BOOST_CHECK_LE(encoded->size(), MAX_BRIDGE_RECORD_SIZE);
            const auto decoded{DecodeBridgeRecordV1(*encoded, BRIDGE_MPT_LIMITS_V2)};
            BOOST_REQUIRE(decoded);
            BOOST_CHECK(decoded->kind == BridgeRecordKindV1::ETH_STATE_OBSERVATION);
            BOOST_CHECK(*decoded == Kind9(value));

            const auto mpa{MakeBridgeMpaRecord(Kind9(value), BRIDGE_MPT_LIMITS_V2)};
            BOOST_REQUIRE(mpa);
            BOOST_CHECK_EQUAL(mpa->payload_type, BRIDGE_MPA_TYPE);
            BOOST_CHECK_EQUAL(mpa->payload_version, BRIDGE_MPA_VERSION_V1);
            BOOST_CHECK(DecodeBridgeMpaRecordV1(*mpa, BRIDGE_MPT_LIMITS_V2) == decoded);
        }
    }
    // Every verdict bit combination round-trips on VERIFIER_E.
    for (const uint8_t verdict : {0, 1, 2, 3}) {
        const auto encoded{EncodeV2(world.Observe(ETH_OBSERVATION_SUBJECT_VERIFIER, verdict))};
        BOOST_REQUIRE(encoded);
        const auto decoded{DecodeBridgeRecordV1(*encoded, BRIDGE_MPT_LIMITS_V2)};
        BOOST_REQUIRE(decoded);
        BOOST_CHECK_EQUAL(std::get<BridgeEthStateObservationV2>(decoded->payload).verdict, verdict);
    }

    // The exact wire bytes: kind 9, reserved 0, subject, raw anchor, verdict,
    // account count, then per account the account nodes, the slot count and
    // one node list per slot (u8 counts, u16 big-endian lengths).
    BridgeEthStateObservationV2 tiny;
    tiny.subject = ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS;
    for (size_t i{0}; i < 32; ++i) tiny.anchor_hash.begin()[i] = static_cast<unsigned char>(i + 1);
    tiny.accounts.push_back({{Bytes{0xc0}}, {Nodes{}, Nodes{Bytes{0xaa, 0xbb}}, Nodes{}}});
    const auto tiny_encoded{EncodeV2(tiny)};
    BOOST_REQUIRE(tiny_encoded);
    BOOST_CHECK_EQUAL(HexStr(*tiny_encoded),
                      "0900"
                      "03"
                      "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"
                      "00"
                      "01"
                      "01" "0001" "c0"
                      "03"
                      "00"
                      "01" "0002" "aabb"
                      "00");
    BOOST_CHECK(DecodeBridgeRecordV1(*tiny_encoded, BRIDGE_MPT_LIMITS_V2) == Kind9(tiny));

    // An empty storage list (a slot of an account without storage) is valid.
    BridgeEthStateObservationV2 empty_storage{Skeleton(ETH_OBSERVATION_SUBJECT_RESERVE)};
    for (auto& account : empty_storage.accounts) {
        for (auto& slot : account.storage_nodes) slot.clear();
    }
    const auto empty_encoded{EncodeV2(empty_storage)};
    BOOST_REQUIRE(empty_encoded);
    BOOST_CHECK(DecodeBridgeRecordV1(*empty_encoded, BRIDGE_MPT_LIMITS_V2) == Kind9(empty_storage));
}

BOOST_AUTO_TEST_CASE(kind_9_only_under_the_v2_record_set)
{
    // Below V (and on every network without bridge_v2) consensus decodes
    // with the V1 set, in which kind 9 does not exist in either direction.
    const BridgeEthStateObservationV2 value{Skeleton(ETH_OBSERVATION_SUBJECT_VERIFIER)};
    BOOST_CHECK(!EncodeBridgeRecordV1(Kind9(value)));
    BOOST_CHECK(!EncodeBridgeRecordV1(Kind9(value), BRIDGE_MPT_LIMITS_V1));
    BOOST_CHECK(!MakeBridgeMpaRecord(Kind9(value)));
    const auto encoded{EncodeV2(value)};
    BOOST_REQUIRE(encoded);
    BOOST_CHECK(!DecodeBridgeRecordV1(*encoded));
    BOOST_CHECK(!DecodeBridgeRecordV1(*encoded, BRIDGE_MPT_LIMITS_V1));
    BOOST_CHECK(!DecodeBridgeProofV1(*encoded));
    const auto mpa{MakeBridgeMpaRecord(Kind9(value), BRIDGE_MPT_LIMITS_V2)};
    BOOST_REQUIRE(mpa);
    BOOST_CHECK(!DecodeBridgeMpaRecordV1(*mpa));
    BOOST_CHECK(DecodeBridgeMpaRecordV1(*mpa, BRIDGE_MPT_LIMITS_V2));
    BOOST_CHECK(!BRIDGE_MPT_LIMITS_V1.v2_kinds);
    BOOST_CHECK(BRIDGE_MPT_LIMITS_V2.v2_kinds);

    // The kind byte and the payload alternative must agree.
    BOOST_CHECK(!EncodeBridgeRecordV1(BridgeRecordV1{BridgeRecordKindV1::ETH_STATE_OBSERVATION, BridgeBurnV1{}},
                                      BRIDGE_MPT_LIMITS_V2));
    BOOST_CHECK(!EncodeBridgeRecordV1(BridgeRecordV1{BridgeRecordKindV1::BRIDGE_BURN, value}, BRIDGE_MPT_LIMITS_V2));
}

BOOST_AUTO_TEST_CASE(codec_refusals)
{
    using V = BridgeEthStateObservationV2;
    const auto refused_encode = [](const V& value, const std::string& what) {
        BOOST_CHECK_MESSAGE(!EncodeV2(value), "encoded: " + what);
    };

    // Subject, anchor and verdict.
    for (const uint8_t subject : {0, 4, 9, 13, 255}) {
        V v{Skeleton(ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS)};
        v.subject = subject;
        refused_encode(v, "subject " + std::to_string(subject));
    }
    {
        V v{Skeleton(ETH_OBSERVATION_SUBJECT_VERIFIER)};
        v.anchor_hash.SetNull();
        refused_encode(v, "null anchor");
    }
    for (const uint8_t verdict : {0x04, 0x08, 0x80, 0xff, 0x07}) {
        V v{Skeleton(ETH_OBSERVATION_SUBJECT_VERIFIER)};
        v.verdict = verdict;
        refused_encode(v, "unknown verdict bits " + std::to_string(verdict));
    }
    for (const uint8_t subject : {ETH_OBSERVATION_SUBJECT_RESERVE, ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS}) {
        for (const uint8_t verdict : {ETH_OBSERVATION_VERDICT_SETS_MISMATCH, ETH_OBSERVATION_VERDICT_CERT_CONFLICT}) {
            V v{Skeleton(subject)};
            v.verdict = verdict;
            refused_encode(v, "verdict on subject " + std::to_string(subject));
        }
    }

    // Account and slot counts follow the subject's shape.
    {
        V none{Skeleton(ETH_OBSERVATION_SUBJECT_VERIFIER)};
        none.accounts.clear();
        refused_encode(none, "no accounts");
        V extra{Skeleton(ETH_OBSERVATION_SUBJECT_VERIFIER)};
        extra.accounts.push_back(extra.accounts[0]);
        refused_encode(extra, "two verifier accounts");
        V short_reserve{Skeleton(ETH_OBSERVATION_SUBJECT_RESERVE)};
        short_reserve.accounts.pop_back();
        refused_encode(short_reserve, "reserve without the token account");
        V swapped{Skeleton(ETH_OBSERVATION_SUBJECT_RESERVE)};
        std::swap(swapped.accounts[0], swapped.accounts[1]);
        refused_encode(swapped, "reserve accounts swapped (slot counts 1, 3)");
    }
    for (const size_t slots : {size_t{0}, size_t{4}, size_t{6}}) {
        V v{Skeleton(ETH_OBSERVATION_SUBJECT_VERIFIER)};
        v.accounts[0].storage_nodes.resize(slots);
        refused_encode(v, "verifier with " + std::to_string(slots) + " slots");
    }
    {
        V v{Skeleton(ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS)};
        v.accounts[0].storage_nodes.pop_back();
        refused_encode(v, "token flags with two slots");
    }

    // Node list limits: 1-64 account nodes, 0-64 storage nodes, each 1-1,024
    // bytes and at most 11,264 bytes per list.
    {
        V v{Skeleton(ETH_OBSERVATION_SUBJECT_VERIFIER)};
        v.accounts[0].account_nodes.clear();
        refused_encode(v, "no account nodes");
    }
    for (const bool storage : {false, true}) {
        const auto list = [&](V& v) -> Nodes& {
            return storage ? v.accounts[0].storage_nodes[2] : v.accounts[0].account_nodes;
        };
        V empty_node{Skeleton(ETH_OBSERVATION_SUBJECT_VERIFIER)};
        list(empty_node).push_back(Bytes{});
        refused_encode(empty_node, "zero-length node");
        V big{Skeleton(ETH_OBSERVATION_SUBJECT_VERIFIER)};
        list(big) = {Bytes(1'025, 0x01)};
        refused_encode(big, "1,025-byte node");
        V at_limit{Skeleton(ETH_OBSERVATION_SUBJECT_VERIFIER)};
        list(at_limit) = {Bytes(1'024, 0x01)};
        BOOST_CHECK(EncodeV2(at_limit));
        V many{Skeleton(ETH_OBSERVATION_SUBJECT_VERIFIER)};
        list(many) = Nodes(65, Bytes{0x01});
        refused_encode(many, "65 nodes");
        V sixty_four{Skeleton(ETH_OBSERVATION_SUBJECT_VERIFIER)};
        list(sixty_four) = Nodes(64, Bytes{0x01});
        BOOST_CHECK(EncodeV2(sixty_four));
        V heavy{Skeleton(ETH_OBSERVATION_SUBJECT_VERIFIER)};
        list(heavy) = Nodes(11, Bytes(1'024, 0x01));
        BOOST_CHECK(EncodeV2(heavy)); // exactly 11,264 bytes
        list(heavy).push_back(Bytes{0x01});
        refused_encode(heavy, "11,265 bytes in one list");
    }

    // The 32 KiB record cap bounds the sum of otherwise valid lists.
    V oversize{Skeleton(ETH_OBSERVATION_SUBJECT_VERIFIER)};
    for (auto& slot : oversize.accounts[0].storage_nodes) slot = Nodes(11, Bytes(1'000, 0x02));
    refused_encode(oversize, "record over 32 KiB");
    V fits{Skeleton(ETH_OBSERVATION_SUBJECT_VERIFIER)};
    fits.accounts[0].storage_nodes[0] = Nodes(11, Bytes(1'000, 0x02));
    fits.accounts[0].storage_nodes[1] = Nodes(11, Bytes(1'000, 0x02));
    const auto fits_encoded{EncodeV2(fits)};
    BOOST_REQUIRE(fits_encoded);
    BOOST_CHECK(DecodesV2(*fits_encoded));

    // Decoding: the hand-written wire is valid, each single malformation is not.
    BOOST_CHECK(DecodesV2(TokenWire().Get()));
    {
        const Bytes good{TokenWire().Get()};
        Bytes truncated{good};
        truncated.pop_back();
        BOOST_CHECK(!DecodesV2(truncated));
        Bytes trailing{good};
        trailing.push_back(0x00);
        BOOST_CHECK(!DecodesV2(trailing));
        Bytes reserved{good};
        reserved[1] = 0x01;
        BOOST_CHECK(!DecodesV2(reserved));
        for (const uint8_t subject : {0, 4, 255}) {
            Bytes wrong{good};
            wrong[2] = subject;
            BOOST_CHECK(!DecodesV2(wrong));
        }
        Bytes subject_one{good};
        subject_one[2] = ETH_OBSERVATION_SUBJECT_VERIFIER; // shape [5], body has 3 slots
        BOOST_CHECK(!DecodesV2(subject_one));
        Bytes null_anchor{good};
        std::fill(null_anchor.begin() + 3, null_anchor.begin() + 35, 0);
        BOOST_CHECK(!DecodesV2(null_anchor));
        for (const uint8_t count : {0, 2, 255}) {
            Bytes wrong{good};
            wrong[36] = count;
            BOOST_CHECK(!DecodesV2(wrong));
        }
    }
    BOOST_CHECK(!DecodesV2(TokenWire(3, ETH_OBSERVATION_VERDICT_SETS_MISMATCH).Get()));
    BOOST_CHECK(!DecodesV2(TokenWire(3, ETH_OBSERVATION_VERDICT_CERT_CONFLICT).Get()));
    for (const uint8_t slots : {0, 2, 4}) BOOST_CHECK(!DecodesV2(TokenWire(slots).Get()));
    {
        RawWire verifier{ETH_OBSERVATION_SUBJECT_VERIFIER, Fill(0x3c), 0x04, 1};
        verifier.List({Bytes{0xc1, 0x80}}).U8(5);
        for (int i{0}; i < 5; ++i) verifier.List({});
        BOOST_CHECK(!DecodesV2(verifier.Get())); // unknown verdict bit
        RawWire ok{ETH_OBSERVATION_SUBJECT_VERIFIER, Fill(0x3c), 0x03, 1};
        ok.List({Bytes{0xc1, 0x80}}).U8(5);
        for (int i{0}; i < 5; ++i) ok.List({});
        BOOST_CHECK(DecodesV2(ok.Get()));
    }
    {
        RawWire no_account_nodes{ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS, Fill(0x3c), 0, 1};
        no_account_nodes.List({}).U8(3).List({}).List({}).List({});
        BOOST_CHECK(!DecodesV2(no_account_nodes.Get()));
        RawWire zero_length{ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS, Fill(0x3c), 0, 1};
        zero_length.List({Bytes{0xc1, 0x80}}).U8(3).U8(1).Length(0).List({}).List({});
        BOOST_CHECK(!DecodesV2(zero_length.Get()));
        RawWire long_node{ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS, Fill(0x3c), 0, 1};
        long_node.List({Bytes(1'025, 0x01)}).U8(3).List({}).List({}).List({});
        BOOST_CHECK(!DecodesV2(long_node.Get()));
        RawWire many_nodes{ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS, Fill(0x3c), 0, 1};
        many_nodes.List({Bytes{0xc1, 0x80}}).U8(3).List(Nodes(65, Bytes{0x01})).List({}).List({});
        BOOST_CHECK(!DecodesV2(many_nodes.Get()));
        RawWire heavy{ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS, Fill(0x3c), 0, 1};
        Nodes twelve(11, Bytes(1'024, 0x01));
        twelve.push_back(Bytes{0x01});
        heavy.List({Bytes{0xc1, 0x80}}).U8(3).List(twelve).List({}).List({});
        BOOST_CHECK(!DecodesV2(heavy.Get()));
        RawWire lying_count{ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS, Fill(0x3c), 0, 1};
        lying_count.List({Bytes{0xc1, 0x80}}, 2).U8(3).List({}).List({}).List({});
        BOOST_CHECK(!DecodesV2(lying_count.Get()));
    }
    {
        // The same oversized body, written by hand, decodes nowhere.
        RawWire big{ETH_OBSERVATION_SUBJECT_VERIFIER, Fill(0x3c), 0, 1};
        big.List({Bytes{0xc1, 0x80}}).U8(5);
        for (int i{0}; i < 5; ++i) big.List(Nodes(11, Bytes(1'000, 0x02)));
        BOOST_CHECK_GT(big.Get().size(), MAX_BRIDGE_RECORD_SIZE);
        BOOST_CHECK(!DecodesV2(big.Get()));
    }
}

// ----------------------------------------------------- layout and the plan

BOOST_AUTO_TEST_CASE(plan_and_layout_match_shared_vectors)
{
    using namespace observation_layout;
    // Mapping keys: keccak(AddressWord(vault) || SlotWord(slot)).
    const StorageWord vault_key{AddressWord(AddressOf(STAND_IN_VAULT))};
    BOOST_CHECK_EQUAL(HexStr(MappingSlot(vault_key, TOKEN_SLOT_BALANCES)), BALANCES_KEY);
    BOOST_CHECK_EQUAL(HexStr(MappingSlot(vault_key, TOKEN_SLOT_IS_BLACK_LISTED)), BLACKLIST_KEY);

    // Slot numbers and bit offsets (storage_layout_v2.*.slots and *_fields).
    BOOST_CHECK_EQUAL(VERIFIER_SLOT_LIVENESS, 0U);
    BOOST_CHECK_EQUAL(VERIFIER_SLOT_CURRENT_SET_HASH, 1U);
    BOOST_CHECK_EQUAL(VERIFIER_SLOT_NEXT_SET_HASH, 2U);
    BOOST_CHECK_EQUAL(VERIFIER_SLOT_LATEST, 4U);
    BOOST_CHECK_EQUAL(VERIFIER_SLOT_LATEST_CERT_ID, 6U);
    const std::vector<std::pair<StorageFieldV2, std::pair<unsigned, unsigned>>> fields{
        {LIVENESS_CURRENT_EPOCH, {0, 64}},       {LIVENESS_LAST_ROTATION_TIME, {64, 40}},
        {LIVENESS_LAST_CERTIFICATE_TIME, {104, 40}}, {LIVENESS_LATEST_BRIDGE_HEIGHT, {144, 64}},
        {LIVENESS_STATUS, {208, 8}},             {LIVENESS_DEPOSITS_CLOSED, {216, 8}},
        {LATEST_HEIGHT, {0, 64}},                {LATEST_INDEX, {64, 64}},
        {LATEST_INIT_TIME, {128, 40}},           {TOKEN_OWNER, {0, 160}},
        {TOKEN_PAUSED, {160, 8}},                {TOKEN_UPGRADED_ADDRESS, {0, 160}},
        {TOKEN_DEPRECATED, {160, 8}},
    };
    for (const auto& [field, expected] : fields) {
        BOOST_CHECK_EQUAL(field.offset, expected.first);
        BOOST_CHECK_EQUAL(field.bits, expected.second);
    }
    BOOST_CHECK_EQUAL(LIVENESS_UNUSED_FROM_BIT, 224U);
    BOOST_CHECK_EQUAL(LATEST_UNUSED_FROM_BIT, 168U);
    BOOST_CHECK_EQUAL(VAULT_SLOT_NEXT_DEPOSIT_ID, 0U);
    BOOST_CHECK_EQUAL(VAULT_SLOT_LOCKED, 1U);
    BOOST_CHECK_EQUAL(VAULT_SLOT_ABSORBED_TOTAL, 3U);
    BOOST_CHECK_EQUAL(VAULT_NEXT_DEPOSIT_ID_BITS, 64U);
    BOOST_CHECK_EQUAL(TOKEN_SLOT_OWNER_PAUSED, 0U);
    BOOST_CHECK_EQUAL(TOKEN_SLOT_BALANCES, 2U);
    BOOST_CHECK_EQUAL(TOKEN_SLOT_IS_BLACK_LISTED, 6U);
    BOOST_CHECK_EQUAL(TOKEN_SLOT_UPGRADED_DEPRECATED, 10U);
    BOOST_CHECK_EQUAL(TOKEN_FLAG_WORD_UNUSED_FROM_BIT, 168U);

    // The eth_getProof plan, in record order, agrees with the codec's shape.
    ObservationPins pins;
    pins.verifier = FillAddress(0x44);
    pins.vault = AddressOf(STAND_IN_VAULT);
    pins.token = FillAddress(0x55);
    const auto verifier{ObservationRequests(ETH_OBSERVATION_SUBJECT_VERIFIER, pins)};
    BOOST_REQUIRE_EQUAL(verifier.size(), 1U);
    BOOST_CHECK(verifier[0].role == ObservationAccountRole::VERIFIER);
    BOOST_CHECK(verifier[0].address == pins.verifier);
    BOOST_CHECK(verifier[0].slots ==
                (std::vector<StorageWord>{SlotWord(0), SlotWord(1), SlotWord(2), SlotWord(4), SlotWord(6)}));
    const auto reserve{ObservationRequests(ETH_OBSERVATION_SUBJECT_RESERVE, pins)};
    BOOST_REQUIRE_EQUAL(reserve.size(), 2U);
    BOOST_CHECK(reserve[0].role == ObservationAccountRole::VAULT);
    BOOST_CHECK(reserve[0].address == pins.vault);
    BOOST_CHECK(reserve[0].slots == (std::vector<StorageWord>{SlotWord(0), SlotWord(1), SlotWord(3)}));
    BOOST_CHECK(reserve[1].role == ObservationAccountRole::TOKEN);
    BOOST_CHECK(reserve[1].address == pins.token);
    BOOST_CHECK(reserve[1].slots == (std::vector<StorageWord>{WordOf(BALANCES_KEY)}));
    const auto token{ObservationRequests(ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS, pins)};
    BOOST_REQUIRE_EQUAL(token.size(), 1U);
    BOOST_CHECK(token[0].role == ObservationAccountRole::TOKEN);
    BOOST_CHECK(token[0].address == pins.token);
    BOOST_CHECK(token[0].slots == (std::vector<StorageWord>{SlotWord(0), SlotWord(10), WordOf(BLACKLIST_KEY)}));
    for (const uint8_t subject : {ETH_OBSERVATION_SUBJECT_VERIFIER, ETH_OBSERVATION_SUBJECT_RESERVE,
                                  ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS}) {
        const auto plan{ObservationRequests(subject, pins)};
        const auto shape{EthObservationShapeV2(subject)};
        BOOST_REQUIRE_EQUAL(plan.size(), shape.size());
        for (size_t i{0}; i < plan.size(); ++i) BOOST_CHECK_EQUAL(plan[i].slots.size(), shape[i]);
    }
    for (const uint8_t subject : {0, 4, 255}) {
        BOOST_CHECK(ObservationRequests(subject, pins).empty());
        BOOST_CHECK(EthObservationShapeV2(subject).empty());
    }

    // Packed example words decode to the vector's field values.
    std::string error;
    for (const LivenessExample& liveness : LIVENESS_EXAMPLES) {
        for (const LatestExample& latest : LATEST_EXAMPLES) {
            const std::array<StorageWord, 5> words{WordOf(liveness.word), FillWord(0xa1), StorageWord{},
                                                   WordOf(latest.word), FillWord(0xa6)};
            const auto state{DecodeVerifierStateV2(words, Fill(0x0c), error)};
            BOOST_REQUIRE_MESSAGE(state, error);
            BOOST_CHECK_EQUAL(state->current_epoch, liveness.current_epoch);
            BOOST_CHECK_EQUAL(state->last_rotation_time, liveness.last_rotation_time);
            BOOST_CHECK_EQUAL(state->last_certificate_time, liveness.last_certificate_time);
            BOOST_CHECK_EQUAL(state->latest_bridge_height, liveness.latest_bridge_height);
            BOOST_CHECK_EQUAL(state->status, liveness.status);
            BOOST_CHECK_EQUAL(state->deposits_closed, liveness.deposits_closed);
            BOOST_CHECK(state->current_set_hash == Fill(0xa1));
            BOOST_CHECK(state->next_set_hash.IsNull());
            BOOST_CHECK_EQUAL(state->latest_height, latest.height);
            BOOST_CHECK_EQUAL(state->latest_index, latest.index);
            BOOST_CHECK_EQUAL(state->latest_init_time, latest.init_time);
            BOOST_CHECK(state->latest_cert_id == Fill(0xa6));
            BOOST_CHECK(state->verifier_code_hash == Fill(0x0c));
        }
    }
    // Vault examples: slot 0 = 2 and 2^64 - 1, slot 1 = 1,040,000,000,
    // slot 3 = 60,000,000.
    for (const auto& [word, next] : std::vector<std::pair<std::string_view, uint64_t>>{
             {"0000000000000000000000000000000000000000000000000000000000000002", 2},
             {"000000000000000000000000000000000000000000000000ffffffffffffffff",
              std::numeric_limits<uint64_t>::max()}}) {
        const std::array<StorageWord, 3> vault{
            WordOf(word), WordOf("000000000000000000000000000000000000000000000000000000003dfd2400"),
            WordOf("0000000000000000000000000000000000000000000000000000000003938700")};
        const auto state{DecodeReserveStateV2(vault, SlotWord(77), Fill(0x0b), Fill(0x0e), error)};
        BOOST_REQUIRE_MESSAGE(state, error);
        BOOST_CHECK_EQUAL(state->next_deposit_id, next);
        BOOST_CHECK_EQUAL(state->locked, 1'040'000'000);
        BOOST_CHECK_EQUAL(state->absorbed_total, 60'000'000);
        BOOST_CHECK_EQUAL(state->balance, 77);
        BOOST_CHECK(state->vault_code_hash == Fill(0x0b));
        BOOST_CHECK(state->token_code_hash == Fill(0x0e));
    }
    // Tether slot 0 and slot 10 examples.
    for (const TokenWordExample& slot0 : TOKEN_SLOT0_EXAMPLES) {
        for (const TokenWordExample& slot10 : TOKEN_SLOT10_EXAMPLES) {
            const std::array<StorageWord, 3> words{WordOf(slot0.word), WordOf(slot10.word), SlotWord(1)};
            const auto flags{DecodeTokenFlagsV2(words, Fill(0x0e), error)};
            BOOST_REQUIRE_MESSAGE(flags, error);
            BOOST_CHECK(flags->owner == AddressOf(slot0.address));
            BOOST_CHECK_EQUAL(flags->paused, slot0.flag);
            BOOST_CHECK(flags->upgraded_address == AddressOf(slot10.address));
            BOOST_CHECK_EQUAL(flags->deprecated, slot10.flag);
            BOOST_CHECK(flags->vault_blacklisted);
        }
    }
}

// -------------------------------------------------- proofs against a root

BOOST_AUTO_TEST_CASE(verify_inclusion_and_exclusion)
{
    World world;
    using namespace observation_layout;

    const auto verifier{VerifiedAs<VerifierStateV2>(world, ETH_OBSERVATION_SUBJECT_VERIFIER)};
    BOOST_CHECK_EQUAL(verifier.current_epoch, 3U);
    BOOST_CHECK_EQUAL(verifier.last_rotation_time, 1'750'016'911);
    BOOST_CHECK_EQUAL(verifier.last_certificate_time, 1'750'017'244);
    BOOST_CHECK_EQUAL(verifier.latest_bridge_height, 326U);
    BOOST_CHECK_EQUAL(verifier.status, VERIFIER_STATUS_LIVE);
    BOOST_CHECK(!verifier.deposits_closed);
    BOOST_CHECK(verifier.current_set_hash == Fill(0xa1));
    BOOST_CHECK(verifier.next_set_hash == Fill(0xa2));
    BOOST_CHECK_EQUAL(verifier.latest_height, 276U);
    BOOST_CHECK_EQUAL(verifier.latest_index, 3U);
    BOOST_CHECK_EQUAL(verifier.latest_init_time, 1'750'000'111);
    BOOST_CHECK(verifier.latest_cert_id == Fill(0xa6));
    BOOST_CHECK(verifier.verifier_code_hash == KeccakOf("verifier runtime"));

    // absorbedTotal is zero, so slot 3 is proven by exclusion.
    const auto reserve{VerifiedAs<ReserveStateV2>(world, ETH_OBSERVATION_SUBJECT_RESERVE)};
    BOOST_CHECK_EQUAL(reserve.next_deposit_id, 2U);
    BOOST_CHECK_EQUAL(reserve.locked, 1'040'000'000);
    BOOST_CHECK_EQUAL(reserve.absorbed_total, 0);
    BOOST_CHECK_EQUAL(reserve.balance, 1'040'000'000);
    BOOST_CHECK(reserve.vault_code_hash == KeccakOf("vault runtime"));
    BOOST_CHECK(reserve.token_code_hash == world.pins.token_code_hash);

    // deprecated and isBlackListed[vault] are zero: exclusion proofs.
    const auto token{VerifiedAs<TokenFlagsV2>(world, ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS)};
    BOOST_CHECK(token.owner == FillAddress(0x66));
    BOOST_CHECK(!token.paused);
    BOOST_CHECK(token.upgraded_address == EthAddress{});
    BOOST_CHECK(!token.deprecated);
    BOOST_CHECK(!token.vault_blacklisted);
    BOOST_CHECK(token.token_code_hash == world.pins.token_code_hash);

    // Setting the flags turns the exclusions into inclusions.
    world.Set(world.pins.token, TOKEN_SLOT_OWNER_PAUSED, WordOf(TOKEN_SLOT0_EXAMPLES[1].word));
    world.Set(world.pins.token, TOKEN_SLOT_UPGRADED_DEPRECATED, WordOf(TOKEN_SLOT10_EXAMPLES[1].word));
    world.SetWord(world.pins.token, world.BlacklistSlot(), SlotWord(1));
    world.Set(world.pins.vault, VAULT_SLOT_ABSORBED_TOTAL, SlotWord(60'000'000));
    const auto flagged{VerifiedAs<TokenFlagsV2>(world, ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS)};
    BOOST_CHECK(flagged.paused);
    BOOST_CHECK(flagged.deprecated);
    BOOST_CHECK(flagged.upgraded_address == FillAddress(0x77));
    BOOST_CHECK(flagged.vault_blacklisted);
    BOOST_CHECK_EQUAL(VerifiedAs<ReserveStateV2>(world, ETH_OBSERVATION_SUBJECT_RESERVE).absorbed_total, 60'000'000);

    // Clearing a word back to zero removes it again.
    world.Set(world.pins.verifier, VERIFIER_SLOT_NEXT_SET_HASH, StorageWord{});
    BOOST_CHECK(VerifiedAs<VerifierStateV2>(world, ETH_OBSERVATION_SUBJECT_VERIFIER).next_set_hash.IsNull());

    // An account without storage proves every slot with zero storage nodes.
    World bare;
    bare.storage.erase(bare.pins.token);
    const auto bare_record{bare.Observe(ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS)};
    for (const Nodes& nodes : bare_record.accounts[0].storage_nodes) BOOST_CHECK(nodes.empty());
    const auto bare_flags{VerifiedAs<TokenFlagsV2>(bare, ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS)};
    BOOST_CHECK(bare_flags.owner == EthAddress{});
    BOOST_CHECK(!bare_flags.paused && !bare_flags.deprecated && !bare_flags.vault_blacklisted);
    const auto bare_reserve{VerifiedAs<ReserveStateV2>(bare, ETH_OBSERVATION_SUBJECT_RESERVE)};
    BOOST_CHECK_EQUAL(bare_reserve.balance, 0);
}

BOOST_AUTO_TEST_CASE(proof_refusals)
{
    const World world;
    const uint256 root{world.StateRoot()};
    std::string error;

    // Another state root.
    BOOST_CHECK(!VerifyEthStateObservationV2(world.Observe(ETH_OBSERVATION_SUBJECT_VERIFIER), Fill(0x99),
                                             world.pins, error));
    BOOST_CHECK_EQUAL(error, "observation verifier account proof: state proof root node does not match the root");
    BOOST_CHECK(!VerifyEthStateObservationV2(world.Observe(ETH_OBSERVATION_SUBJECT_VERIFIER), uint256{},
                                             world.pins, error));
    BOOST_CHECK_EQUAL(error, "observation anchor has no state root");

    // A missing contract.
    World missing;
    missing.present[missing.pins.vault] = false;
    CheckRefused(missing, ETH_OBSERVATION_SUBJECT_RESERVE, "observation vault account proof: account is absent");

    // Storage proofs must be for the slot the plan names.
    {
        auto record{world.Observe(ETH_OBSERVATION_SUBJECT_VERIFIER)};
        std::swap(record.accounts[0].storage_nodes[1], record.accounts[0].storage_nodes[4]);
        BOOST_CHECK(!VerifyEthStateObservationV2(record, root, world.pins, error));
        BOOST_CHECK_EQUAL(error.rfind("observation verifier storage proof 1: ", 0), 0U);
    }
    {
        // A proof of an absent slot cannot stand in for a present one.
        auto record{world.Observe(ETH_OBSERVATION_SUBJECT_RESERVE)};
        record.accounts[0].storage_nodes[1] = record.accounts[0].storage_nodes[2]; // locked <- absorbedTotal
        BOOST_CHECK(!VerifyEthStateObservationV2(record, root, world.pins, error));
        BOOST_CHECK_EQUAL(error.rfind("observation vault storage proof 1: ", 0), 0U);
    }
    {
        // An unused trailing node is refused rather than ignored.
        auto record{world.Observe(ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS)};
        record.accounts[0].storage_nodes[0].push_back(Bytes(40, 0x01));
        BOOST_CHECK(!VerifyEthStateObservationV2(record, root, world.pins, error));
        BOOST_CHECK_EQUAL(error, "observation token storage proof 0: state proof has unused nodes");
    }
    {
        // The account proof of another contract.
        auto record{world.Observe(ETH_OBSERVATION_SUBJECT_RESERVE)};
        std::swap(record.accounts[0].account_nodes, record.accounts[1].account_nodes);
        BOOST_CHECK(!VerifyEthStateObservationV2(record, root, world.pins, error));
        BOOST_CHECK_EQUAL(error.rfind("observation vault account proof: ", 0), 0U);
    }

    // Shapes are checked again here for records built without the codec.
    {
        auto record{world.Observe(ETH_OBSERVATION_SUBJECT_RESERVE)};
        record.accounts.pop_back();
        BOOST_CHECK(!VerifyEthStateObservationV2(record, root, world.pins, error));
        BOOST_CHECK_EQUAL(error, "observation account count does not match its subject");
        auto slots{world.Observe(ETH_OBSERVATION_SUBJECT_VERIFIER)};
        slots.accounts[0].storage_nodes.pop_back();
        BOOST_CHECK(!VerifyEthStateObservationV2(slots, root, world.pins, error));
        BOOST_CHECK_EQUAL(error, "observation verifier slot count does not match its subject");
        auto unknown{world.Observe(ETH_OBSERVATION_SUBJECT_VERIFIER)};
        unknown.subject = 4;
        BOOST_CHECK(!VerifyEthStateObservationV2(unknown, root, world.pins, error));
        BOOST_CHECK_EQUAL(error, "observation subject is unknown");
    }
}

BOOST_AUTO_TEST_CASE(code_hash_rules)
{
    // Pinned hashes must match exactly.
    World pinned;
    pinned.pins.verifier_code_hash = KeccakOf("verifier runtime");
    pinned.pins.vault_code_hash = KeccakOf("vault runtime");
    for (const uint64_t chain : {uint64_t{1}, uint64_t{11'155'111}, OBSERVATION_UNPINNED_CODE_CHAIN_ID}) {
        pinned.pins.origin_chain_id = chain;
        VerifiedAs<VerifierStateV2>(pinned, ETH_OBSERVATION_SUBJECT_VERIFIER);
        VerifiedAs<ReserveStateV2>(pinned, ETH_OBSERVATION_SUBJECT_RESERVE);
        VerifiedAs<TokenFlagsV2>(pinned, ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS);
    }
    {
        World w{pinned};
        w.pins.verifier_code_hash = KeccakOf("other verifier");
        CheckRefused(w, ETH_OBSERVATION_SUBJECT_VERIFIER, "observation verifier code hash does not match its pin");
        w.pins.vault_code_hash = KeccakOf("other vault");
        CheckRefused(w, ETH_OBSERVATION_SUBJECT_RESERVE, "observation vault code hash does not match its pin");
    }
    {
        // A redeployed (empty) pinned contract fails its pin.
        World w{pinned};
        w.code[w.pins.verifier] = EMPTY_CODE_HASH;
        CheckRefused(w, ETH_OBSERVATION_SUBJECT_VERIFIER, "observation verifier code hash does not match its pin");
    }

    // The token is always pinned, in both subjects that read it.
    for (const uint8_t subject : {ETH_OBSERVATION_SUBJECT_RESERVE, ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS}) {
        World w;
        w.code[w.pins.token] = KeccakOf("upgraded token");
        CheckRefused(w, subject, "observation token code hash does not match its pin");
        World unpinned;
        unpinned.pins.token_code_hash.SetNull();
        CheckRefused(unpinned, subject, "observation token code hash does not match its pin");
    }

    // OI-3: an unpinned verifier or vault is accepted only on 31337 and only
    // with code.
    VerifiedAs<VerifierStateV2>(World{}, ETH_OBSERVATION_SUBJECT_VERIFIER);
    VerifiedAs<ReserveStateV2>(World{}, ETH_OBSERVATION_SUBJECT_RESERVE);
    for (const uint64_t chain : {uint64_t{1}, uint64_t{11'155'111}, uint64_t{31'338}, uint64_t{0}}) {
        World w;
        w.pins.origin_chain_id = chain;
        CheckRefused(w, ETH_OBSERVATION_SUBJECT_VERIFIER,
                     "observation verifier code hash is unpinned off the regtest chain");
        CheckRefused(w, ETH_OBSERVATION_SUBJECT_RESERVE, "observation vault code hash is unpinned off the regtest chain");
        // TOKEN_FLAGS reads only the always-pinned token.
        VerifiedAs<TokenFlagsV2>(w, ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS);
    }
    {
        World w;
        w.code[w.pins.verifier] = EMPTY_CODE_HASH;
        CheckRefused(w, ETH_OBSERVATION_SUBJECT_VERIFIER, "observation verifier has no code");
        w.code[w.pins.vault] = EMPTY_CODE_HASH;
        CheckRefused(w, ETH_OBSERVATION_SUBJECT_RESERVE, "observation vault has no code");
    }

    // Pins from a bridge V2 envelope.
    Consensus::BridgeV2Params params;
    params.vault.origin_chain_id = 31'337;
    params.vault.vault_address = FillAddress(0x22);
    params.vault.token_address = FillAddress(0x55);
    params.vault.token_runtime_code_hash = Fill(0x0e);
    params.verifier.address = FillAddress(0x44);
    ObservationPins from{ObservationPinsFromParams(params)};
    BOOST_CHECK_EQUAL(from.origin_chain_id, 31'337U);
    BOOST_CHECK(from.vault == FillAddress(0x22));
    BOOST_CHECK(from.token == FillAddress(0x55));
    BOOST_CHECK(from.verifier == FillAddress(0x44));
    BOOST_CHECK(!from.vault_code_hash && !from.verifier_code_hash);
    BOOST_CHECK(from.token_code_hash == Fill(0x0e));
    params.vault.vault_runtime_code_hash = Fill(0x0b);
    params.verifier.runtime_code_hash = Fill(0x0c);
    from = ObservationPinsFromParams(params);
    BOOST_CHECK(from.vault_code_hash == Fill(0x0b));
    BOOST_CHECK(from.verifier_code_hash == Fill(0x0c));
}

BOOST_AUTO_TEST_CASE(stored_zero_only_on_the_regtest_chain)
{
    // A token slot an anvil transaction only read: a present leaf holding
    // 0x80. Node changes section 8.2 calls a present zero malformed (real
    // Ethereum deletes zero slots); only the regtest origin chain, anvil's
    // 31337, reads it as zero (bridge::StoredZero).
    BOOST_CHECK_EQUAL(OBSERVATION_STORED_ZERO_CHAIN_ID, 31'337U);
    World w;
    const StorageWord deprecated_slot{SlotWord(observation_layout::TOKEN_SLOT_UPGRADED_DEPRECATED)};
    const uint256 hashed{StorageTrieKey(deprecated_slot)};
    w.storage[w.pins.token].Raw().Insert(std::span<const unsigned char>{hashed.begin(), 32}, Bytes{0x80});
    BOOST_REQUIRE_EQUAL(w.pins.origin_chain_id, OBSERVATION_STORED_ZERO_CHAIN_ID);
    const TokenFlagsV2 flags{VerifiedAs<TokenFlagsV2>(w, ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS)};
    BOOST_CHECK(!flags.deprecated);
    BOOST_CHECK(flags.upgraded_address == EthAddress{});
    for (const uint64_t chain : {uint64_t{1}, uint64_t{11'155'111}, uint64_t{31'338}}) {
        World other{w};
        other.pins.origin_chain_id = chain;
        CheckRefused(other, ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS,
                     "observation token storage proof 1: storage value is a stored zero, which only anvil keeps");
    }
}

BOOST_AUTO_TEST_CASE(strict_layout_refusals)
{
    using namespace observation_layout;
    const auto refused = [](const std::function<void(World&)>& mutate, const uint8_t subject,
                            const std::string& expected) {
        World w;
        mutate(w);
        CheckRefused(w, subject, expected);
    };
    const StorageWord liveness{WordOf(LIVENESS_EXAMPLES[0].word)};
    const StorageWord latest{WordOf(LATEST_EXAMPLES[0].word)};

    // Liveness: bits 224-255 zero, status <= 3, depositsClosed boolean.
    for (const unsigned bit : {224U, 231U, 240U, 255U}) {
        refused([&](World& w) { w.Set(w.pins.verifier, VERIFIER_SLOT_LIVENESS, WithBit(liveness, bit)); },
                ETH_OBSERVATION_SUBJECT_VERIFIER, "verifier liveness word has unused bits set");
    }
    for (const uint64_t status : {4U, 5U, 255U}) {
        refused([&](World& w) {
            StorageWord word{liveness};
            word[31 - 208 / 8] = static_cast<unsigned char>(status);
            w.Set(w.pins.verifier, VERIFIER_SLOT_LIVENESS, word);
        }, ETH_OBSERVATION_SUBJECT_VERIFIER, "verifier status is out of range");
    }
    for (const uint64_t closed : {2U, 255U}) {
        refused([&](World& w) {
            StorageWord word{liveness};
            word[31 - 216 / 8] = static_cast<unsigned char>(closed);
            w.Set(w.pins.verifier, VERIFIER_SLOT_LIVENESS, word);
        }, ETH_OBSERVATION_SUBJECT_VERIFIER, "verifier depositsClosed is not a boolean");
    }
    // Every status value 0-3 and both depositsClosed values decode.
    for (const uint64_t status : {0U, 1U, 2U, 3U}) {
        World w;
        w.Set(w.pins.verifier, VERIFIER_SLOT_LIVENESS,
              Packed({{208, 8, status}, {216, 8, 1}, {0, 64, 9}}));
        const auto state{VerifiedAs<VerifierStateV2>(w, ETH_OBSERVATION_SUBJECT_VERIFIER)};
        BOOST_CHECK_EQUAL(state.status, status);
        BOOST_CHECK(state.deposits_closed);
        BOOST_CHECK_EQUAL(state.current_epoch, 9U);
    }
    // Latest: bits 168-255 zero.
    for (const unsigned bit : {168U, 200U, 255U}) {
        refused([&](World& w) { w.Set(w.pins.verifier, VERIFIER_SLOT_LATEST, WithBit(latest, bit)); },
                ETH_OBSERVATION_SUBJECT_VERIFIER, "verifier latest word has unused bits set");
    }
    // Set hashes and the certificate id are whole words: any value decodes.
    {
        World w;
        w.Set(w.pins.verifier, VERIFIER_SLOT_LATEST_CERT_ID, FillWord(0xff));
        BOOST_CHECK(VerifiedAs<VerifierStateV2>(w, ETH_OBSERVATION_SUBJECT_VERIFIER).latest_cert_id == Fill(0xff));
    }

    // Vault: nextDepositId bits 64-255 zero; amounts at most INT64_MAX.
    for (const unsigned bit : {64U, 128U, 255U}) {
        refused([&](World& w) { w.Set(w.pins.vault, VAULT_SLOT_NEXT_DEPOSIT_ID, WithBit(SlotWord(2), bit)); },
                ETH_OBSERVATION_SUBJECT_RESERVE, "vault nextDepositId word has unused bits set");
    }
    const StorageWord int64_max{SlotWord(static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))};
    const StorageWord int64_max_plus_one{SlotWord(static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1)};
    {
        World w;
        w.Set(w.pins.vault, VAULT_SLOT_LOCKED, int64_max);
        w.Set(w.pins.vault, VAULT_SLOT_ABSORBED_TOTAL, int64_max);
        w.SetWord(w.pins.token, w.BalanceSlot(), int64_max);
        const auto state{VerifiedAs<ReserveStateV2>(w, ETH_OBSERVATION_SUBJECT_RESERVE)};
        BOOST_CHECK_EQUAL(state.locked, std::numeric_limits<CAmount>::max());
        BOOST_CHECK_EQUAL(state.absorbed_total, std::numeric_limits<CAmount>::max());
        BOOST_CHECK_EQUAL(state.balance, std::numeric_limits<CAmount>::max());
    }
    for (const StorageWord& too_big : {int64_max_plus_one, WithBit(StorageWord{}, 64), WithBit(StorageWord{}, 255),
                                       FillWord(0xff)}) {
        refused([&](World& w) { w.Set(w.pins.vault, VAULT_SLOT_LOCKED, too_big); },
                ETH_OBSERVATION_SUBJECT_RESERVE, "reserve observation amount exceeds the int64 range");
        refused([&](World& w) { w.Set(w.pins.vault, VAULT_SLOT_ABSORBED_TOTAL, too_big); },
                ETH_OBSERVATION_SUBJECT_RESERVE, "reserve observation amount exceeds the int64 range");
        refused([&](World& w) { w.SetWord(w.pins.token, w.BalanceSlot(), too_big); },
                ETH_OBSERVATION_SUBJECT_RESERVE, "reserve observation amount exceeds the int64 range");
    }

    // Tether slots 0 and 10: bits 168-255 zero, the flag byte boolean; the
    // blacklist word is 0 or 1.
    const StorageWord slot0{WordOf(TOKEN_SLOT0_EXAMPLES[0].word)};
    for (const unsigned bit : {168U, 200U, 255U}) {
        refused([&](World& w) { w.Set(w.pins.token, TOKEN_SLOT_OWNER_PAUSED, WithBit(slot0, bit)); },
                ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS, "token slot 0 has unused bits set");
        refused([&](World& w) { w.Set(w.pins.token, TOKEN_SLOT_UPGRADED_DEPRECATED, WithBit(StorageWord{}, bit)); },
                ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS, "token slot 10 has unused bits set");
    }
    for (const unsigned bit : {161U, 167U}) {
        refused([&](World& w) { w.Set(w.pins.token, TOKEN_SLOT_OWNER_PAUSED, WithBit(slot0, bit)); },
                ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS, "token flag is not a boolean");
        refused([&](World& w) { w.Set(w.pins.token, TOKEN_SLOT_UPGRADED_DEPRECATED, WithBit(StorageWord{}, bit)); },
                ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS, "token flag is not a boolean");
    }
    for (const StorageWord& word : {SlotWord(2), SlotWord(255), WithBit(SlotWord(1), 255), FillWord(0x01)}) {
        refused([&](World& w) { w.SetWord(w.pins.token, w.BlacklistSlot(), word); },
                ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS, "token flag is not a boolean");
    }

    // The decode helpers refuse wrong word counts.
    std::string error;
    const std::array<StorageWord, 4> four{};
    BOOST_CHECK(!DecodeVerifierStateV2(four, uint256{}, error));
    BOOST_CHECK_EQUAL(error, "verifier observation needs five storage words");
    BOOST_CHECK(!DecodeReserveStateV2(std::span{four}.first(2), StorageWord{}, uint256{}, uint256{}, error));
    BOOST_CHECK_EQUAL(error, "reserve observation needs three vault storage words");
    BOOST_CHECK(!DecodeTokenFlagsV2(four, uint256{}, error));
    BOOST_CHECK_EQUAL(error, "token flags observation needs three storage words");
}

// ------------------------------------------------ B-23 successor subjects

BOOST_AUTO_TEST_CASE(successor_subjects_plan_and_verify)
{
    using Consensus::BridgeV2EntryRole;
    using namespace observation_layout;

    // Numbering: base subject | (vault entry index << 4). Exactly five
    // subjects exist, the verdict stays VERIFIER_E's, and 4-0x11 and
    // 0x14-0xff stay malformed.
    BOOST_CHECK_EQUAL(int{ETH_OBSERVATION_SUBJECT_RESERVE_SUCCESSOR}, 0x12);
    BOOST_CHECK_EQUAL(int{ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS_SUCCESSOR}, 0x13);
    for (unsigned value{0}; value <= 255; ++value) {
        const auto subject{static_cast<uint8_t>(value)};
        const bool launch_subject{value >= 1 && value <= 3};
        const bool successor_subject{value == 0x12 || value == 0x13};
        BOOST_TEST_CONTEXT("subject " << value)
        {
            BOOST_CHECK_EQUAL(int{EthObservationBaseSubject(subject)},
                              launch_subject ? int(value) : successor_subject ? int(value & 0x0f) : 0);
            BOOST_CHECK(EthObservationEntryRole(subject) ==
                        (successor_subject ? BridgeV2EntryRole::SUCCESSOR : BridgeV2EntryRole::LAUNCH));
            BOOST_CHECK_EQUAL(EthObservationShapeV2(subject).empty(), !launch_subject && !successor_subject);
            BOOST_CHECK_EQUAL(proof_detail::EthObservationHeaderValid(subject, Fill(0x3c), 0),
                              launch_subject || successor_subject);
            BOOST_CHECK_EQUAL(proof_detail::EthObservationHeaderValid(subject, Fill(0x3c),
                                                                      ETH_OBSERVATION_VERDICT_SETS_MISMATCH),
                              value == ETH_OBSERVATION_SUBJECT_VERIFIER);
        }
    }
    BOOST_CHECK(std::ranges::equal(EthObservationShapeV2(ETH_OBSERVATION_SUBJECT_RESERVE_SUCCESSOR),
                                   std::vector<uint8_t>{3, 1}));
    BOOST_CHECK(std::ranges::equal(EthObservationShapeV2(ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS_SUCCESSOR),
                                   std::vector<uint8_t>{3}));

    // An envelope with a successor: v' is the shared vectors' stand-in vault
    // 0x2222..22, so its mapping keys are the vector's; v is 0x2121..21.
    Consensus::BridgeV2Params params;
    params.vault.origin_chain_id = OBSERVATION_UNPINNED_CODE_CHAIN_ID;
    params.vault.vault_address = FillAddress(0x21);
    params.vault.token_address = AddressOf("dac17f958d2ee523a2206206994597c13d831ec7");
    params.vault.token_runtime_code_hash = KeccakOf("token runtime");
    params.verifier.address = FillAddress(0x44);

    // Without a successor the SUCCESSOR pins name no vault and no token.
    const ObservationPins unpinned{ObservationPinsFromParams(params, BridgeV2EntryRole::SUCCESSOR)};
    BOOST_CHECK(unpinned.entry == BridgeV2EntryRole::SUCCESSOR);
    BOOST_CHECK(unpinned.vault == EthAddress{});
    BOOST_CHECK(unpinned.token == EthAddress{});
    BOOST_CHECK(unpinned.verifier == params.verifier.address);

    Consensus::BridgeV2SuccessorEntry successor;
    successor.entry = params.vault;
    successor.entry.vault_address = AddressOf(STAND_IN_VAULT);
    successor.predecessor = params.vault.vault_address;
    successor.asset_id = Fill(0x11);
    successor.migration_height = 400;
    params.successor = successor;

    const ObservationPins launch{ObservationPinsFromParams(params)};
    const ObservationPins next{ObservationPinsFromParams(params, BridgeV2EntryRole::SUCCESSOR)};
    {
        const ObservationPins explicit_launch{ObservationPinsFromParams(params, BridgeV2EntryRole::LAUNCH)};
        for (const ObservationPins* p : {&launch, &explicit_launch}) {
            BOOST_CHECK(p->entry == BridgeV2EntryRole::LAUNCH);
            BOOST_CHECK(p->vault == params.vault.vault_address);
            BOOST_CHECK(p->vault_code_hash == params.vault.vault_runtime_code_hash);
        }
        BOOST_CHECK(next.entry == BridgeV2EntryRole::SUCCESSOR);
        BOOST_CHECK(next.vault == AddressOf(STAND_IN_VAULT));
        BOOST_CHECK(!next.vault_code_hash);
        for (const ObservationPins* p : {&launch, &explicit_launch, &next}) {
            BOOST_CHECK_EQUAL(p->origin_chain_id, OBSERVATION_UNPINNED_CODE_CHAIN_ID);
            BOOST_CHECK(p->verifier == params.verifier.address);
            BOOST_CHECK(!p->verifier_code_hash);
            BOOST_CHECK(p->token == params.vault.token_address);
            BOOST_CHECK(p->token_code_hash == params.vault.token_runtime_code_hash);
        }
        // v''s own code-hash pin, when set, is the one its pins carry.
        Consensus::BridgeV2Params pinned{params};
        pinned.successor->entry.vault_runtime_code_hash = Fill(0x0d);
        pinned.vault.vault_runtime_code_hash = Fill(0x0b);
        BOOST_CHECK(ObservationPinsFromParams(pinned, BridgeV2EntryRole::SUCCESSOR).vault_code_hash == Fill(0x0d));
        BOOST_CHECK(ObservationPinsFromParams(pinned).vault_code_hash == Fill(0x0b));
    }

    // The plans: 0x12 reads v' slots 0, 1, 3 and balances[v']; 0x13 reads the
    // token's slots 0, 10 and isBlackListed[v'].
    const auto reserve_plan{ObservationRequests(ETH_OBSERVATION_SUBJECT_RESERVE_SUCCESSOR, next)};
    BOOST_REQUIRE_EQUAL(reserve_plan.size(), 2U);
    BOOST_CHECK(reserve_plan[0].role == ObservationAccountRole::VAULT);
    BOOST_CHECK(reserve_plan[0].address == AddressOf(STAND_IN_VAULT));
    BOOST_CHECK(reserve_plan[0].slots == (std::vector<StorageWord>{SlotWord(0), SlotWord(1), SlotWord(3)}));
    BOOST_CHECK(reserve_plan[1].role == ObservationAccountRole::TOKEN);
    BOOST_CHECK(reserve_plan[1].address == params.vault.token_address);
    BOOST_CHECK(reserve_plan[1].slots == (std::vector<StorageWord>{WordOf(BALANCES_KEY)}));
    const auto flags_plan{ObservationRequests(ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS_SUCCESSOR, next)};
    BOOST_REQUIRE_EQUAL(flags_plan.size(), 1U);
    BOOST_CHECK(flags_plan[0].role == ObservationAccountRole::TOKEN);
    BOOST_CHECK(flags_plan[0].address == params.vault.token_address);
    BOOST_CHECK(flags_plan[0].slots == (std::vector<StorageWord>{SlotWord(0), SlotWord(10), WordOf(BLACKLIST_KEY)}));
    // The launch plans read v with v's keys.
    const StorageWord v_key{AddressWord(params.vault.vault_address)};
    const auto launch_reserve_plan{ObservationRequests(ETH_OBSERVATION_SUBJECT_RESERVE, launch)};
    BOOST_REQUIRE_EQUAL(launch_reserve_plan.size(), 2U);
    BOOST_CHECK(launch_reserve_plan[0].address == params.vault.vault_address);
    BOOST_CHECK(launch_reserve_plan[1].slots == (std::vector<StorageWord>{MappingSlot(v_key, TOKEN_SLOT_BALANCES)}));
    BOOST_CHECK(launch_reserve_plan[1].slots[0] != WordOf(BALANCES_KEY));
    const auto launch_flags_plan{ObservationRequests(ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS, launch)};
    BOOST_REQUIRE_EQUAL(launch_flags_plan.size(), 1U);
    BOOST_CHECK(launch_flags_plan[0].slots[2] == MappingSlot(v_key, TOKEN_SLOT_IS_BLACK_LISTED));
    for (const uint8_t subject : {ETH_OBSERVATION_SUBJECT_RESERVE_SUCCESSOR,
                                  ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS_SUCCESSOR}) {
        const auto plan{ObservationRequests(subject, next)};
        const auto shape{EthObservationShapeV2(subject)};
        BOOST_REQUIRE_EQUAL(plan.size(), shape.size());
        for (size_t i{0}; i < plan.size(); ++i) BOOST_CHECK_EQUAL(plan[i].slots.size(), shape[i]);
    }
    // Pins of the other vault entry plan nothing, nor does an unknown subject.
    for (const uint8_t subject : {ETH_OBSERVATION_SUBJECT_VERIFIER, ETH_OBSERVATION_SUBJECT_RESERVE,
                                  ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS}) {
        BOOST_CHECK(ObservationRequests(subject, next).empty());
    }
    for (const uint8_t subject : {ETH_OBSERVATION_SUBJECT_RESERVE_SUCCESSOR,
                                  ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS_SUCCESSOR}) {
        BOOST_CHECK(ObservationRequests(subject, launch).empty());
    }
    for (const uint8_t subject : {0x10, 0x11, 0x14, 0x21, 0x22, 0x23}) {
        BOOST_CHECK(ObservationRequests(subject, launch).empty());
        BOOST_CHECK(ObservationRequests(subject, next).empty());
    }

    // One Ethereum state holding both vaults: v holds three deposits; v'
    // has absorbed a release from v and is blacklisted.
    bridge_test::ObservationWorld world{launch};
    world.AddSuccessorVault(next.vault, next.vault_code_hash);
    world.SetReserve(3, 1'000'000, 0, 1'000'000);
    world.SetSuccessorReserve(0, 0, 999'000, 999'000);
    world.SetTokenFlags(false, false, false);
    world.SetSuccessorBlacklisted(true);
    const uint256 root{world.StateRoot()};
    const uint256 anchor{Fill(0x3c)};
    std::string error;
    const auto verify = [&](const BridgeEthStateObservationV2& record, const ObservationPins& pins) {
        return VerifyEthStateObservationV2(record, root, pins, error);
    };

    const BridgeEthStateObservationV2 reserve_next{world.Observe(ETH_OBSERVATION_SUBJECT_RESERVE_SUCCESSOR, anchor)};
    const BridgeEthStateObservationV2 flags_next{world.Observe(ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS_SUCCESSOR, anchor)};
    const BridgeEthStateObservationV2 reserve_launch{world.Observe(ETH_OBSERVATION_SUBJECT_RESERVE, anchor)};
    const BridgeEthStateObservationV2 flags_launch{world.Observe(ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS, anchor)};
    const BridgeEthStateObservationV2 verifier_record{world.Observe(ETH_OBSERVATION_SUBJECT_VERIFIER, anchor)};
    {
        const auto state{verify(reserve_next, next)};
        BOOST_REQUIRE_MESSAGE(state, error);
        BOOST_REQUIRE(std::holds_alternative<ReserveStateV2>(*state));
        const ReserveStateV2& reserve{std::get<ReserveStateV2>(*state)};
        BOOST_CHECK_EQUAL(reserve.next_deposit_id, 0U);
        BOOST_CHECK_EQUAL(reserve.locked, 0);
        BOOST_CHECK_EQUAL(reserve.absorbed_total, 999'000);
        BOOST_CHECK_EQUAL(reserve.balance, 999'000);
        BOOST_CHECK(reserve.vault_code_hash == world.code.at(next.vault));
        BOOST_CHECK(reserve.vault_code_hash != world.code.at(launch.vault));
        BOOST_CHECK(reserve.token_code_hash == params.vault.token_runtime_code_hash);
    }
    {
        const auto state{verify(reserve_launch, launch)};
        BOOST_REQUIRE_MESSAGE(state, error);
        const ReserveStateV2& reserve{std::get<ReserveStateV2>(*state)};
        BOOST_CHECK_EQUAL(reserve.next_deposit_id, 3U);
        BOOST_CHECK_EQUAL(reserve.locked, 1'000'000);
        BOOST_CHECK_EQUAL(reserve.absorbed_total, 0);
        BOOST_CHECK_EQUAL(reserve.balance, 1'000'000);
        BOOST_CHECK(reserve.vault_code_hash == world.code.at(launch.vault));
    }
    TokenFlagsV2 launch_flags;
    {
        const auto next_state{verify(flags_next, next)};
        BOOST_REQUIRE_MESSAGE(next_state, error);
        BOOST_REQUIRE(std::holds_alternative<TokenFlagsV2>(*next_state));
        const auto launch_state{verify(flags_launch, launch)};
        BOOST_REQUIRE_MESSAGE(launch_state, error);
        const TokenFlagsV2& next_flags{std::get<TokenFlagsV2>(*next_state)};
        launch_flags = std::get<TokenFlagsV2>(*launch_state);
        BOOST_CHECK(next_flags.vault_blacklisted);
        BOOST_CHECK(!launch_flags.vault_blacklisted);
        // Slots 0 and 10 are the token's own, shared by both entries.
        BOOST_CHECK(next_flags.owner == FillAddress(0x66));
        BOOST_CHECK(next_flags.owner == launch_flags.owner);
        BOOST_CHECK_EQUAL(next_flags.paused, launch_flags.paused);
        BOOST_CHECK_EQUAL(next_flags.deprecated, launch_flags.deprecated);
        BOOST_CHECK(next_flags.token_code_hash == launch_flags.token_code_hash);
    }
    BOOST_CHECK_MESSAGE(verify(verifier_record, launch), error);

    // The successor records travel as kind 9 with their own subject byte,
    // only under the V2 record set.
    for (const BridgeEthStateObservationV2* record : {&reserve_next, &flags_next}) {
        const auto encoded{EncodeV2(*record)};
        BOOST_REQUIRE(encoded);
        BOOST_CHECK_EQUAL((*encoded)[0], 9);
        BOOST_CHECK_EQUAL((*encoded)[2], record->subject);
        BOOST_CHECK(DecodeBridgeRecordV1(*encoded, BRIDGE_MPT_LIMITS_V2) == Kind9(*record));
        BOOST_CHECK(!DecodeBridgeRecordV1(*encoded, BRIDGE_MPT_LIMITS_V1));
        BOOST_CHECK(!EncodeBridgeRecordV1(Kind9(*record), BRIDGE_MPT_LIMITS_V1));
    }

    // Pins of the wrong vault entry fail before any proof is read, in both
    // directions; VERIFIER_E is read with the launch pins only.
    for (const auto& [record, pins] : std::vector<std::pair<const BridgeEthStateObservationV2*, const ObservationPins*>>{
             {&reserve_next, &launch}, {&flags_next, &launch}, {&reserve_launch, &next},
             {&flags_launch, &next}, {&verifier_record, &next}}) {
        BOOST_CHECK(!verify(*record, *pins));
        BOOST_CHECK_EQUAL(error, "observation pins belong to another vault entry");
    }
    // A record relabelled to the other entry's subject does not verify with
    // that entry's pins: its vault account proof is of the other vault.
    {
        BridgeEthStateObservationV2 relabelled{reserve_next};
        relabelled.subject = ETH_OBSERVATION_SUBJECT_RESERVE;
        BOOST_CHECK(!verify(relabelled, launch));
        BOOST_CHECK_EQUAL(error.rfind("observation vault account proof: ", 0), 0U);
        relabelled = reserve_launch;
        relabelled.subject = ETH_OBSERVATION_SUBJECT_RESERVE_SUCCESSOR;
        BOOST_CHECK(!verify(relabelled, next));
        BOOST_CHECK_EQUAL(error.rfind("observation vault account proof: ", 0), 0U);
        // TOKEN_FLAGS shares the token account, so a relabelled record can
        // at most prove the slots its pins name: v''s blacklisting never
        // reads as v's. In this fixture the isBlackListed[v'] proof is no
        // proof of isBlackListed[v] (v's key leaves the proven path for a
        // subtree the proof omits), so the third storage proof is refused.
        // A proof that did happen to exclude isBlackListed[v] could only
        // read v's own value (false here, flags_launch above), as any
        // accepted Merkle-Patricia proof reads the true value of its key.
        BridgeEthStateObservationV2 flags_relabelled{flags_next};
        flags_relabelled.subject = ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS;
        BOOST_CHECK(!verify(flags_relabelled, launch));
        BOOST_CHECK_EQUAL(error, "observation token storage proof 2: state proof node does not match its reference");
        BOOST_CHECK(!launch_flags.vault_blacklisted);
    }

    // Unknown subjects are refused with either pins and by the codec.
    for (const uint8_t subject : {0x11, 0x14, 0x22}) {
        BOOST_TEST_CONTEXT("subject " << int{subject})
        {
            BridgeEthStateObservationV2 unknown{reserve_launch};
            unknown.subject = subject;
            for (const ObservationPins* pins : {&launch, &next}) {
                BOOST_CHECK(!verify(unknown, *pins));
                BOOST_CHECK_EQUAL(error, "observation subject is unknown");
            }
            BOOST_CHECK(!EncodeV2(unknown));
            const auto encoded{EncodeV2(Skeleton(ETH_OBSERVATION_SUBJECT_RESERVE))};
            BOOST_REQUIRE(encoded);
            Bytes patched{*encoded};
            patched[2] = subject;
            BOOST_CHECK(!DecodesV2(patched));
        }
    }

    // The wire: a RESERVE_E body under subject 0x12 decodes as 0x12, a
    // TOKEN_FLAGS body under 0x13 as 0x13; each other shape is malformed.
    {
        const auto encoded{EncodeV2(Skeleton(ETH_OBSERVATION_SUBJECT_RESERVE))};
        BOOST_REQUIRE(encoded);
        Bytes as_successor{*encoded};
        as_successor[2] = ETH_OBSERVATION_SUBJECT_RESERVE_SUCCESSOR;
        const auto decoded{DecodeBridgeRecordV1(as_successor, BRIDGE_MPT_LIMITS_V2)};
        BOOST_REQUIRE(decoded);
        BridgeEthStateObservationV2 expected{Skeleton(ETH_OBSERVATION_SUBJECT_RESERVE)};
        expected.subject = ETH_OBSERVATION_SUBJECT_RESERVE_SUCCESSOR;
        BOOST_CHECK(*decoded == Kind9(expected));
        BOOST_CHECK(EncodeV2(expected) == as_successor);
        Bytes wrong_shape{*encoded};
        wrong_shape[2] = ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS_SUCCESSOR;
        BOOST_CHECK(!DecodesV2(wrong_shape));

        Bytes flags_wire{TokenWire().Get()};
        flags_wire[2] = ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS_SUCCESSOR;
        BOOST_CHECK(DecodesV2(flags_wire));
        flags_wire[2] = ETH_OBSERVATION_SUBJECT_RESERVE_SUCCESSOR;
        BOOST_CHECK(!DecodesV2(flags_wire));
    }

    // A verdict on a successor subject is refused in both directions.
    for (const uint8_t subject : {ETH_OBSERVATION_SUBJECT_RESERVE_SUCCESSOR,
                                  ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS_SUCCESSOR}) {
        BridgeEthStateObservationV2 skeleton{Skeleton(subject)};
        BOOST_REQUIRE(EncodeV2(skeleton));
        for (const uint8_t verdict : {ETH_OBSERVATION_VERDICT_SETS_MISMATCH, ETH_OBSERVATION_VERDICT_CERT_CONFLICT,
                                      ETH_OBSERVATION_VERDICT_MASK}) {
            BridgeEthStateObservationV2 with_verdict{skeleton};
            with_verdict.verdict = verdict;
            BOOST_CHECK(!EncodeV2(with_verdict));
            Bytes patched{*EncodeV2(skeleton)};
            patched[35] = verdict; // kind, reserved, subject, 32-byte anchor
            BOOST_CHECK(!DecodesV2(patched));
        }
    }

    // v''s code-hash rules are v's (OI-3): a pin must match, an unpinned v'
    // needs code and the regtest chain.
    {
        bridge_test::ObservationWorld pinned_world{launch};
        pinned_world.AddSuccessorVault(next.vault, KeccakOf("successor runtime"));
        const BridgeEthStateObservationV2 record{
            pinned_world.Observe(ETH_OBSERVATION_SUBJECT_RESERVE_SUCCESSOR, anchor)};
        const uint256 pinned_root{pinned_world.StateRoot()};
        ObservationPins pins{pinned_world.PinsFor(BridgeV2EntryRole::SUCCESSOR)};
        BOOST_CHECK_MESSAGE(VerifyEthStateObservationV2(record, pinned_root, pins, error), error);
        pins.vault_code_hash = KeccakOf("other successor");
        BOOST_CHECK(!VerifyEthStateObservationV2(record, pinned_root, pins, error));
        BOOST_CHECK_EQUAL(error, "observation vault code hash does not match its pin");
        pins.vault_code_hash.reset();
        BOOST_CHECK_MESSAGE(VerifyEthStateObservationV2(record, pinned_root, pins, error), error);
        pins.origin_chain_id = 1;
        BOOST_CHECK(!VerifyEthStateObservationV2(record, pinned_root, pins, error));
        BOOST_CHECK_EQUAL(error, "observation vault code hash is unpinned off the regtest chain");
    }
    {
        bridge_test::ObservationWorld empty_world{launch};
        empty_world.AddSuccessorVault(next.vault);
        empty_world.code[next.vault] = EMPTY_CODE_HASH;
        BOOST_CHECK(!VerifyEthStateObservationV2(
            empty_world.Observe(ETH_OBSERVATION_SUBJECT_RESERVE_SUCCESSOR, anchor), empty_world.StateRoot(),
            empty_world.PinsFor(BridgeV2EntryRole::SUCCESSOR), error));
        BOOST_CHECK_EQUAL(error, "observation vault has no code");
    }
    // Pins of an envelope without a successor read nothing, not even the
    // token-only 0x13 (which would otherwise read isBlackListed[0]).
    for (const BridgeEthStateObservationV2* record : {&reserve_next, &flags_next}) {
        BOOST_CHECK(!verify(*record, unpinned));
        BOOST_CHECK_EQUAL(error, "observation pins name no vault entry");
    }
}

BOOST_AUTO_TEST_SUITE_END()
