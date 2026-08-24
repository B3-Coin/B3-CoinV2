// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// ETH -> B3 deposit leg, end to end on ONE real mainnet block: the
// receipts fixture (src/test/data/eth_receipts_proof_fixture.h) was
// captured at exactly the execution block whose receipts_root the sync-
// committee light client PROVES in bridge_eth_light_client_tests
// (update U2's finalized header). Chain under test here:
//   proven receipts_root -> MPT inclusion -> receipt decode -> log extraction
// plus strict decode/extraction rules over synthetic vault receipts.

#include <bridge/deposit.h>
#include <bridge/mpt.h>
#include <bridge/rlp.h>
#include <test/data/eth_lc_fixture.h>
#include <test/data/eth_receipts_proof_fixture.h>
#include <uint256.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

using namespace bridge;

BOOST_AUTO_TEST_SUITE(bridge_deposit_tests)

namespace {
std::vector<unsigned char> Hex(const std::string& h)
{
    auto v{TryParseHex<unsigned char>(h)};
    BOOST_REQUIRE(v.has_value());
    return *v;
}
uint256 Hex32(const std::string& h)
{
    const auto v{Hex(h)};
    BOOST_REQUIRE_EQUAL(v.size(), 32U);
    return uint256{std::span<const unsigned char>{v}};
}
} // namespace

BOOST_AUTO_TEST_CASE(mainnet_end_to_end)
{
    // The receipts fixture block IS the light-client-proven finalized block.
    BOOST_REQUIRE_EQUAL(eth_receipts_fixture::BLOCK_NUMBER,
                        eth_lc_fixture::U2_FINALIZED_EXEC.block_number);
    BOOST_REQUIRE_EQUAL(eth_receipts_fixture::RECEIPTS_ROOT_HEX,
                        eth_lc_fixture::U2_FINALIZED_EXEC.receipts_root);
    const uint256 proven_root{Hex32(eth_lc_fixture::U2_FINALIZED_EXEC.receipts_root)};

    const uint256 transfer_topic{Hex32("ddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef")};
    bool saw_transfer{false};
    for (const auto& c : eth_receipts_fixture::CASES) {
        std::vector<std::vector<unsigned char>> proof;
        for (const auto& n : c.proof_hex) proof.push_back(Hex(n));
        const auto value{VerifyMptProof(proven_root, Hex(c.key_hex), proof)};
        BOOST_REQUIRE_MESSAGE(value, "MPT inclusion failed at index " << c.index);
        const auto receipt{DecodeReceipt(*value)};
        BOOST_REQUIRE_MESSAGE(receipt, "receipt decode failed at index " << c.index);
        BOOST_CHECK(receipt->status); // fixture picks successful receipts
        for (const auto& log : receipt->logs) {
            if (!log.topics.empty() && log.topics[0] == transfer_topic) saw_transfer = true;
        }
    }
    // At least one real ERC-20 Transfer decoded through the full chain.
    BOOST_CHECK(saw_transfer);
}

namespace {

std::vector<unsigned char> EncodeLog(const EthAddress& addr, const std::vector<uint256>& topics,
                                     std::span<const unsigned char> data)
{
    std::vector<std::vector<unsigned char>> ts;
    for (const auto& t : topics) ts.push_back(RlpEncodeBytes({t.begin(), 32}));
    return RlpEncodeList({RlpEncodeBytes(addr), RlpEncodeList(ts), RlpEncodeBytes(data)});
}

std::vector<unsigned char> EncodeReceipt(uint8_t type, bool status, uint64_t gas,
                                         const std::vector<std::vector<unsigned char>>& logs)
{
    const std::vector<unsigned char> bloom(256, 0x00);
    auto body{RlpEncodeList({RlpEncodeUint64(status ? 1 : 0), RlpEncodeUint64(gas),
                             RlpEncodeBytes(bloom), RlpEncodeList(logs)})};
    if (type == 0) return body;
    std::vector<unsigned char> out{type};
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

const EthAddress VAULT{{0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00, 0x11, 0x22, 0x33,
                        0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd}};

std::vector<unsigned char> VaultDepositLog(uint64_t id, const EthAddress& token,
                                           unsigned char amount_lsb, unsigned char recip_tag)
{
    uint256 t1{}, t2{};
    for (int i = 0; i < 8; ++i) *(t1.begin() + 24 + i) = static_cast<unsigned char>(id >> (8 * (7 - i)));
    std::copy(token.begin(), token.end(), t2.begin() + 12);
    std::vector<unsigned char> data(64, 0x00);
    data[31] = amount_lsb;
    data[32] = recip_tag; // recipient bytes32, arbitrary content
    return EncodeLog(VAULT, {DepositTopic(), t1, t2}, data);
}

} // namespace

BOOST_AUTO_TEST_CASE(vault_deposit_extraction)
{
    const EthAddress token{{0xda, 0xc1, 0x7f, 0x95, 0x8d, 0x2e, 0xe5, 0x23, 0xa2, 0x20,
                            0x62, 0x06, 0x99, 0x45, 0x97, 0xc1, 0x3d, 0x83, 0x1e, 0xc7}};
    EthAddress other_addr{VAULT};
    other_addr[0] ^= 0x01;

    // A foreign log plus one well-formed vault deposit.
    const auto foreign{EncodeLog(other_addr, {DepositTopic()}, std::vector<unsigned char>(64, 0))};
    const auto good{VaultDepositLog(7, token, 0x2a, 0x99)};
    const auto enc{EncodeReceipt(2, true, 100'000, {foreign, good})};

    const auto receipt{DecodeReceipt(enc)};
    BOOST_REQUIRE(receipt);
    BOOST_CHECK_EQUAL(receipt->type, 2);
    BOOST_CHECK(receipt->status);
    BOOST_REQUIRE_EQUAL(receipt->logs.size(), 2U);

    const auto deposits{ExtractDeposits(*receipt, VAULT)};
    BOOST_REQUIRE_EQUAL(deposits.size(), 1U);
    BOOST_CHECK_EQUAL(deposits[0].deposit_id, 7U);
    BOOST_CHECK(deposits[0].token == token);
    BOOST_CHECK_EQUAL(deposits[0].amount[31], 0x2a);
    BOOST_CHECK_EQUAL(deposits[0].b3_recipient[0], 0x99);

    // Same receipt, queried for a different vault: the foreign log carries
    // the topic but not the 3-topic Deposit shape, so nothing extracts.
    BOOST_CHECK(ExtractDeposits(*receipt, other_addr).empty());
    EthAddress third{VAULT};
    third[19] ^= 0x01;
    BOOST_CHECK(ExtractDeposits(*receipt, third).empty());

    // Failed receipt: no deposits ever.
    const auto failed{DecodeReceipt(EncodeReceipt(2, false, 100'000, {good}))};
    BOOST_REQUIRE(failed);
    BOOST_CHECK(ExtractDeposits(*failed, VAULT).empty());
}

BOOST_AUTO_TEST_CASE(malformed_pseudo_deposits_skipped)
{
    // Bad id padding (non-zero byte in the first 24).
    {
        uint256 t1{uint256::ONE}, t2{};
        std::vector<unsigned char> data(64, 0);
        const auto log{EncodeLog(VAULT, {DepositTopic(), t1, t2}, data)};
        const auto r{DecodeReceipt(EncodeReceipt(2, true, 1, {log}))};
        BOOST_REQUIRE(r);
        BOOST_CHECK(ExtractDeposits(*r, VAULT).empty());
    }
    // Wrong data size (63 bytes).
    {
        uint256 t1{}, t2{};
        std::vector<unsigned char> data(63, 0);
        const auto log{EncodeLog(VAULT, {DepositTopic(), t1, t2}, data)};
        const auto r{DecodeReceipt(EncodeReceipt(2, true, 1, {log}))};
        BOOST_REQUIRE(r);
        BOOST_CHECK(ExtractDeposits(*r, VAULT).empty());
    }
    // Wrong topic count (2 instead of 3).
    {
        uint256 t1{};
        const auto log{EncodeLog(VAULT, {DepositTopic(), t1}, std::vector<unsigned char>(64, 0))};
        const auto r{DecodeReceipt(EncodeReceipt(2, true, 1, {log}))};
        BOOST_REQUIRE(r);
        BOOST_CHECK(ExtractDeposits(*r, VAULT).empty());
    }
}

BOOST_AUTO_TEST_CASE(receipt_decode_strictness)
{
    const auto good{EncodeReceipt(0, true, 42, {})};
    BOOST_CHECK(DecodeReceipt(good));

    // Pre-Byzantium state-root form: 32-byte first field.
    {
        const std::vector<unsigned char> bloom(256, 0);
        const std::vector<unsigned char> root(32, 0xab);
        const auto enc{RlpEncodeList({RlpEncodeBytes(root), RlpEncodeUint64(1),
                                      RlpEncodeBytes(bloom), RlpEncodeList({})})};
        BOOST_CHECK(!DecodeReceipt(enc));
    }
    // Status byte other than 0x01.
    {
        const std::vector<unsigned char> bloom(256, 0);
        const auto enc{RlpEncodeList({RlpEncodeUint64(2), RlpEncodeUint64(1),
                                      RlpEncodeBytes(bloom), RlpEncodeList({})})};
        BOOST_CHECK(!DecodeReceipt(enc));
    }
    // Wrong bloom size.
    {
        const std::vector<unsigned char> bloom(255, 0);
        const auto enc{RlpEncodeList({RlpEncodeUint64(1), RlpEncodeUint64(1),
                                      RlpEncodeBytes(bloom), RlpEncodeList({})})};
        BOOST_CHECK(!DecodeReceipt(enc));
    }
    // Unknown envelope type 0x05 is not a valid RLP list start either.
    {
        auto bad{good};
        bad.insert(bad.begin(), 0x05);
        BOOST_CHECK(!DecodeReceipt(bad));
    }
    // Empty.
    BOOST_CHECK(!DecodeReceipt({}));
}

BOOST_AUTO_TEST_SUITE_END()
