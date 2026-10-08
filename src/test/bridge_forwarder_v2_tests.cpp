// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// Bridge V2 forwarders, B3 side: the F_d and F_x salts and CREATE2 accounts
// (modern/bridge_forwarder.h; contracts spec section 7.2), the burn record's
// exit block bytes (node changes section 7.2 item 5), the OI-2 deposit fee
// cap, the light-client horizon policy (node changes section 12.3), the OI-6
// reserved-address roles, and the EIP-191 refund-EOA proof (bridge/eip191.h;
// contracts spec section 7.3).
//
// Vector provenance. Every hex value below equals the shared C++/Foundry
// vectors file contracts/test/vectors/bridge_v2_vectors.json, section
// "forwarder_v2", written by contracts/test/vectors/gen_v2_vectors.sh and
// read by contracts/test/v2/ForwarderVectorsV2.t.sol. This test hard-codes
// the values rather than reading that file, so a regenerated file must be
// copied here again. Hex is in EVM byte order: the 32 bytes of a hash are its
// raw uint256 bytes. Factories, vaults, recipients, most EOAs and the
// init-code hash are stand-ins; the refund-EOA signatures are anvil dev
// accounts 0 and 1 (public test keys of the "test ... junk" mnemonic),
// produced offline by `cast wallet sign --mnemonic-index <0|1>` (cast 1.7.1).

#include <bridge/eip191.h>
#include <consensus/bridge_params.h>
#include <key.h>
#include <modern/bridge_forwarder.h>
#include <modern/chain_domain.h>
#include <modern/withdrawal_tree.h>
#include <pubkey.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr uint64_t U64_MAX{std::numeric_limits<uint64_t>::max()};

//! A hash whose 32 raw bytes are the given EVM-order hex (no byte reversal).
uint256 Evm(const std::string_view hex)
{
    const auto bytes{ParseHex(hex)};
    BOOST_REQUIRE_EQUAL(bytes.size(), 32U);
    uint256 out;
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return out;
}

std::string EvmHex(const uint256& h) { return HexStr(std::span<const unsigned char>(h.begin(), 32)); }

modern::ForwarderWord Word(const std::string_view hex)
{
    const auto bytes{ParseHex(hex)};
    BOOST_REQUIRE_EQUAL(bytes.size(), 32U);
    modern::ForwarderWord out{};
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return out;
}

Consensus::BridgeEthAddress Address(const std::string_view hex)
{
    const auto bytes{ParseHex(hex)};
    BOOST_REQUIRE_EQUAL(bytes.size(), 20U);
    Consensus::BridgeEthAddress out{};
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return out;
}

Consensus::BridgeEthAddress FillAddress(const unsigned char b)
{
    Consensus::BridgeEthAddress out{};
    out.fill(b);
    return out;
}

uint256 Fill(const unsigned char b)
{
    uint256 out;
    std::fill(out.begin(), out.end(), b);
    return out;
}

uint256 KeccakOf(const std::string_view text)
{
    return modern::WithdrawalKeccak(std::span<const unsigned char>{
        reinterpret_cast<const unsigned char*>(text.data()), text.size()});
}

// ------------------------------------------------- forwarder_v2 vectors

//! forwarder_v2.init_code_hash: keccak256("B3-VECTORS/forwarder-init-code").
constexpr std::string_view INIT_CODE_HASH{"6278af86459e0481b5e9f1f02566368936967234f7930022c721a1b3ae94bf57"};
//! forwarder_v2.pinned_dst_eid (Arbitrum).
constexpr uint32_t PINNED_DST_EID{30110};

struct DepositCase {
    std::string_view name;
    std::string_view factory;
    std::string_view vault;
    std::string_view b3_recipient;
    std::string_view refund_eoa;
    uint64_t fee_cap;
    uint64_t not_after;
    std::string_view salt_preimage;
    std::string_view salt;
    std::string_view account;
};

const DepositCase DEPOSIT_CASES[]{
    {"launch_default", "c28c473e2372beff1aea4815f8e834cd432ce26b", "f501296ff4a14d49fd013ed3b2bbcbe58835ac20",
     "000000000000000000000001c715244f7680c260cb1c281abb084f83e4f80619", "f39fd6e51aad88f6f4ce6ab8827279cfffb92266",
     15000000ULL, 1794000000ULL,
     "cc2b88b5788e6a3d2f51c52fcb908e55a5386859799df6e4878912fdb1b01d2a"
     "000000000000000000000000f501296ff4a14d49fd013ed3b2bbcbe58835ac20"
     "000000000000000000000001c715244f7680c260cb1c281abb084f83e4f80619"
     "000000000000000000000000f39fd6e51aad88f6f4ce6ab8827279cfffb92266"
     "0000000000000000000000000000000000000000000000000000000000e4e1c0"
     "000000000000000000000000000000000000000000000000000000006aee4480",
     "a00ff3a3bc3fab361627ddbf9c79432bba8e17343461d76c97a77b9ffe48557f", "e0003afdc9d2314620c6ca0c332fc684db971e10"},
    {"u64_max_fee_cap", "c28c473e2372beff1aea4815f8e834cd432ce26b", "f501296ff4a14d49fd013ed3b2bbcbe58835ac20",
     "000000000000000000000001f6e92f31603af95b13935011c487dfb0cc586d3b", "2346ee195ebe1e77a97e34cf8203325d43861b07",
     U64_MAX, 1ULL,
     "cc2b88b5788e6a3d2f51c52fcb908e55a5386859799df6e4878912fdb1b01d2a"
     "000000000000000000000000f501296ff4a14d49fd013ed3b2bbcbe58835ac20"
     "000000000000000000000001f6e92f31603af95b13935011c487dfb0cc586d3b"
     "0000000000000000000000002346ee195ebe1e77a97e34cf8203325d43861b07"
     "000000000000000000000000000000000000000000000000ffffffffffffffff"
     "0000000000000000000000000000000000000000000000000000000000000001",
     "1f61b798c865a5acfe399f39021969732f687209855256975dbd094947e29f29", "926002b611b2f51cfd92d7dba6cd71a646432454"},
    {"successor_zero_fee_cap", "ce5df53b7cd69e70fe7fc79ac5bfcb3d660d9472", "80837dc936abb39a4fb447a067499c088860c336",
     "0000000000000000000000011098a898cea7532efbc986de32fff315660013fe", "42ce6d213350f9ddc34c77ecad375d9225821781",
     0ULL, U64_MAX,
     "cc2b88b5788e6a3d2f51c52fcb908e55a5386859799df6e4878912fdb1b01d2a"
     "00000000000000000000000080837dc936abb39a4fb447a067499c088860c336"
     "0000000000000000000000011098a898cea7532efbc986de32fff315660013fe"
     "00000000000000000000000042ce6d213350f9ddc34c77ecad375d9225821781"
     "0000000000000000000000000000000000000000000000000000000000000000"
     "000000000000000000000000000000000000000000000000ffffffffffffffff",
     "6618ecab3fba5571cfac1ead40a87c10913da3fe2f51e5a0ba1eabf1852e81f3", "d89fa4675a3dafc8dc699e213f53b35bf2d3941b"},
};

struct ExitCase {
    std::string_view name;
    std::string_view factory;
    uint32_t dst_eid;
    std::string_view to;
    uint16_t max_bridge_fee_bps;
    uint64_t fee_cap;
    std::string_view refund_eoa;
    bool is_valid;
    std::string_view salt_preimage;
    std::string_view salt;
    std::string_view account;
};

const ExitCase EXIT_CASES[]{
    {"arbitrum_default", "ffaa7bb40fa5d2ef12e5f9e78a3817155e15610c", 30110,
     "000000000000000000000000944db6d25977dbda8cbb4694b85a9e568d62dbfd", 0, 3000000ULL,
     "f39fd6e51aad88f6f4ce6ab8827279cfffb92266", true,
     "0fdc29b9af6b10d38ef4781cb890c8a6d762464db73cc2f5800457cdf07b4c87"
     "000000000000000000000000000000000000000000000000000000000000759e"
     "000000000000000000000000944db6d25977dbda8cbb4694b85a9e568d62dbfd"
     "0000000000000000000000000000000000000000000000000000000000000000"
     "00000000000000000000000000000000000000000000000000000000002dc6c0"
     "000000000000000000000000f39fd6e51aad88f6f4ce6ab8827279cfffb92266",
     "071ac7ffa96bacd92528bfd633ded7ef85daca529f36b4502b69af45bb331090", "1b7a56fb16009b1c9d27edc96452e1ac2241f118"},
    {"arbitrum_max_bps_u64_fee_cap", "ffaa7bb40fa5d2ef12e5f9e78a3817155e15610c", 30110,
     "000000000000000000000000502190d499de05e0f75db1bf02a34b42a608b0b1", 50, U64_MAX,
     "d51b1155903e20ac5f9d666951c317dfed630f41", true,
     "0fdc29b9af6b10d38ef4781cb890c8a6d762464db73cc2f5800457cdf07b4c87"
     "000000000000000000000000000000000000000000000000000000000000759e"
     "000000000000000000000000502190d499de05e0f75db1bf02a34b42a608b0b1"
     "0000000000000000000000000000000000000000000000000000000000000032"
     "000000000000000000000000000000000000000000000000ffffffffffffffff"
     "000000000000000000000000d51b1155903e20ac5f9d666951c317dfed630f41",
     "0c0d12cd8a15a9ccf3b66c41d4ce661e86d3773bff04cb3adb0229bd8327685c", "b2ca5b929a0de10381784fa9f849b0f66ad4cd98"},
    {"ethereum_eid_zero_fee_cap", "3960797ff693c2cdff0248b779ea7010d0859b02", 30101,
     "000000000000000000000000fcdb59f9cc095bc1c5809fa648f91bde103347ad", 25, 0ULL,
     "e0dc39242a2b335954701a70534232bf7b0ac46e", false,
     "0fdc29b9af6b10d38ef4781cb890c8a6d762464db73cc2f5800457cdf07b4c87"
     "0000000000000000000000000000000000000000000000000000000000007595"
     "000000000000000000000000fcdb59f9cc095bc1c5809fa648f91bde103347ad"
     "0000000000000000000000000000000000000000000000000000000000000019"
     "0000000000000000000000000000000000000000000000000000000000000000"
     "000000000000000000000000e0dc39242a2b335954701a70534232bf7b0ac46e",
     "9394f5b2118c8fe787fcb3171cdbdadb197fad4d18d3ff70bbae4ec5b7e84f16", "379d29ea09d786e4c5ca010780212357cedb0345"},
};

struct RefundMessageCase {
    std::string_view name;
    std::string_view chain_domain;
    std::string_view refund_eoa;
    std::string_view message;
    std::string_view eip191_digest;
    std::string_view signature;
};

const RefundMessageCase REFUND_MESSAGE_CASES[]{
    {"regtest", "1a3405424a805d6b95600c5b7598af38b1f8f3ba25bdb2636500c76ad3f43879",
     "f39fd6e51aad88f6f4ce6ab8827279cfffb92266",
     "B3 bridge V2 refund EOA\n"
     "chain domain: 0x1a3405424a805d6b95600c5b7598af38b1f8f3ba25bdb2636500c76ad3f43879\n"
     "refund EOA: 0xf39fd6e51aad88f6f4ce6ab8827279cfffb92266",
     "1028445d867b38ad8f5cd49033551ba8db6f985be984cefb38e36cc57dadda59",
     "a3000ca3798b9de067a8f029e2ff2420a16a790042a9f67d7c166d583782316c"
     "2f08e9f568da67c99585920b84935688a44357bf0cce08a77a17f1de9e332c751b"},
    {"mainnet", "a651262ae9031646047f290513d77ccae0fa9bd4e5afe7e07155a08d5dd1486a",
     "70997970c51812dc3a010c7d01b50e0d17dc79c8",
     "B3 bridge V2 refund EOA\n"
     "chain domain: 0xa651262ae9031646047f290513d77ccae0fa9bd4e5afe7e07155a08d5dd1486a\n"
     "refund EOA: 0x70997970c51812dc3a010c7d01b50e0d17dc79c8",
     "e22a982f59ce436654b7a0b3769bcf5683149bbc3ed65639d22f4c86c74dfebb",
     "8f7e5401ac3a4170c62f5c573f4bca418d25da232d07d00ea87b5ac6c67e98e2"
     "1b61fd792cc0bda0ed6ab2dbddf3270be53f3876ca7ea97cadfbfc8cd97d1c8f1c"},
};

struct BurnExitBlockCase {
    size_t exit_case;
    std::string_view recipient;
    std::string_view encoded;
};

const BurnExitBlockCase BURN_EXIT_BLOCK_CASES[]{
    {0, "1b7a56fb16009b1c9d27edc96452e1ac2241f118",
     "01" "0000759e" "000000000000000000000000944db6d25977dbda8cbb4694b85a9e568d62dbfd" "0000"
     "00000000002dc6c0" "f39fd6e51aad88f6f4ce6ab8827279cfffb92266"},
    {1, "b2ca5b929a0de10381784fa9f849b0f66ad4cd98",
     "01" "0000759e" "000000000000000000000000502190d499de05e0f75db1bf02a34b42a608b0b1" "0032"
     "ffffffffffffffff" "d51b1155903e20ac5f9d666951c317dfed630f41"},
    {2, "379d29ea09d786e4c5ca010780212357cedb0345",
     "01" "00007595" "000000000000000000000000fcdb59f9cc095bc1c5809fa648f91bde103347ad" "0019"
     "0000000000000000" "e0dc39242a2b335954701a70534232bf7b0ac46e"},
};

struct FeeCapCase {
    uint64_t fee_per_tx;
    uint64_t fee_cap;
};

const FeeCapCase FEE_CAP_CASES[]{
    {1ULL, 4ULL},
    {1000000ULL, 4000000ULL},
    {2500000ULL, 10000000ULL},
    // The uint64 boundary: the largest fee_per_tx whose x 4 fits.
    {4611686018427387903ULL, 18446744073709551612ULL},
};

modern::DepositForwarderParamsV2 DepositParams(const DepositCase& c)
{
    return modern::DepositForwarderParamsV2{
        .vault = Address(c.vault),
        .b3_recipient = Word(c.b3_recipient),
        .refund_eoa = Address(c.refund_eoa),
        .fee_cap = c.fee_cap,
        .not_after = c.not_after,
    };
}

modern::ExitForwarderParamsV2 ExitParams(const ExitCase& c)
{
    return modern::ExitForwarderParamsV2{
        .dst_eid = c.dst_eid,
        .to = Word(c.to),
        .max_bridge_fee_bps = c.max_bridge_fee_bps,
        .fee_cap = c.fee_cap,
        .refund_eoa = Address(c.refund_eoa),
    };
}

//! A valid F_x parameter set under PINNED_DST_EID.
modern::ExitForwarderParamsV2 ValidExitParams() { return ExitParams(EXIT_CASES[0]); }

//! r || s || v (v = 27 + recid) of `key` over the EIP-191 digest of `message`.
std::vector<unsigned char> EthSign(const CKey& key, const std::string_view message)
{
    std::vector<unsigned char> compact;
    BOOST_REQUIRE(key.SignCompact(bridge::Eip191Digest(message), compact));
    BOOST_REQUIRE_EQUAL(compact.size(), CPubKey::COMPACT_SIGNATURE_SIZE);
    const unsigned char recid{static_cast<unsigned char>((compact[0] - 27) & 3)};
    std::vector<unsigned char> out(compact.begin() + 1, compact.end());
    out.push_back(static_cast<unsigned char>(27 + recid));
    return out;
}

//! A complete launch envelope with a successor and both forwarder pins, every
//! role on a distinct address.
Consensus::BridgeV2Params ReservedRolesParams()
{
    Consensus::BridgeV2Params params;
    params.vault.vault_address = FillAddress(0xa1);
    params.vault.token_address = FillAddress(0xa2);
    params.verifier.address = FillAddress(0xa3);
    Consensus::BridgeV2SuccessorEntry successor;
    successor.entry.vault_address = FillAddress(0xa4);
    successor.entry.token_address = params.vault.token_address;
    successor.predecessor = params.vault.vault_address;
    params.successor = successor;
    params.deposit_forwarder = Consensus::BridgeV2DepositForwarderPins{
        .factory = FillAddress(0xa5), .init_code_hash = Fill(0x01)};
    params.exit_forwarder = Consensus::BridgeV2ExitForwarderPins{
        .factory = FillAddress(0xa6), .init_code_hash = Fill(0x02), .dst_eid = PINNED_DST_EID};
    return params;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(bridge_forwarder_v2_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(forwarder_tags_and_sizes)
{
    BOOST_CHECK_EQUAL(EvmHex(modern::DepositForwarderTagV2()),
                      "cc2b88b5788e6a3d2f51c52fcb908e55a5386859799df6e4878912fdb1b01d2a");
    BOOST_CHECK_EQUAL(EvmHex(modern::ExitForwarderTagV2()),
                      "0fdc29b9af6b10d38ef4781cb890c8a6d762464db73cc2f5800457cdf07b4c87");
    BOOST_CHECK(modern::DepositForwarderTagV2() == KeccakOf("B3/FWD/DEPOSIT/V2"));
    BOOST_CHECK(modern::ExitForwarderTagV2() == KeccakOf("B3/FWD/EXIT/OFT/V2"));
    BOOST_CHECK(Evm(INIT_CODE_HASH) == KeccakOf("B3-VECTORS/forwarder-init-code"));
    BOOST_CHECK_EQUAL(modern::FORWARDER_SALT_PREIMAGE_SIZE, 192U);
    BOOST_CHECK_EQUAL(modern::FORWARDER_CREATE2_PREIMAGE_SIZE, 85U);
    BOOST_CHECK_EQUAL(modern::EXIT_FORWARDER_BLOCK_SIZE, 67U);
    BOOST_CHECK_EQUAL(modern::FORWARDER_MAX_BRIDGE_FEE_BPS, 50);
}

BOOST_AUTO_TEST_CASE(deposit_forwarder_vectors)
{
    for (const DepositCase& c : DEPOSIT_CASES) {
        BOOST_TEST_CONTEXT(c.name)
        {
            const auto p{DepositParams(c)};
            const auto preimage{modern::DepositForwarderSaltPreimageV2(p)};
            BOOST_CHECK_EQUAL(HexStr(preimage), c.salt_preimage);
            BOOST_CHECK_EQUAL(EvmHex(modern::DepositForwarderSaltV2(p)), c.salt);
            const Consensus::BridgeV2DepositForwarderPins pins{
                .factory = Address(c.factory), .init_code_hash = Evm(INIT_CODE_HASH)};
            const auto account{modern::DepositForwarderAddressV2(pins, p)};
            BOOST_CHECK_EQUAL(HexStr(account), c.account);
            BOOST_CHECK(account == modern::Create2AddressV2(pins.factory, Evm(c.salt), pins.init_code_hash));
            // OI-6: no derivation yields the vault or the factory.
            BOOST_CHECK(account != p.vault);
            BOOST_CHECK(account != pins.factory);
            // The account commits to every field.
            for (int field = 0; field < 5; ++field) {
                auto q{p};
                switch (field) {
                case 0: q.vault[19] ^= 1; break;
                case 1: q.b3_recipient[31] ^= 1; break;
                case 2: q.refund_eoa[0] ^= 1; break;
                case 3: q.fee_cap ^= 1; break;
                case 4: q.not_after ^= 1; break;
                }
                BOOST_CHECK(modern::DepositForwarderAddressV2(pins, q) != account);
            }
            auto other_pins{pins};
            other_pins.init_code_hash = Fill(0x5a);
            BOOST_CHECK(modern::DepositForwarderAddressV2(other_pins, p) != account);
            other_pins = pins;
            other_pins.factory[0] ^= 1;
            BOOST_CHECK(modern::DepositForwarderAddressV2(other_pins, p) != account);
        }
    }
}

BOOST_AUTO_TEST_CASE(exit_forwarder_vectors)
{
    for (const ExitCase& c : EXIT_CASES) {
        BOOST_TEST_CONTEXT(c.name)
        {
            const auto p{ExitParams(c)};
            BOOST_CHECK_EQUAL(HexStr(modern::ExitForwarderSaltPreimageV2(p)), c.salt_preimage);
            BOOST_CHECK_EQUAL(EvmHex(modern::ExitForwarderSaltV2(p)), c.salt);
            const Consensus::BridgeV2ExitForwarderPins pins{
                .factory = Address(c.factory), .init_code_hash = Evm(INIT_CODE_HASH), .dst_eid = PINNED_DST_EID};
            BOOST_CHECK_EQUAL(HexStr(modern::ExitForwarderAddressV2(pins, p)), c.account);
            BOOST_CHECK_EQUAL(modern::ExitForwarderParamsValidV2(p, PINNED_DST_EID), c.is_valid);
            // The Ethereum-eid case is valid under its own pin.
            BOOST_CHECK(modern::ExitForwarderParamsValidV2(p, c.dst_eid));
        }
    }
}

BOOST_AUTO_TEST_CASE(exit_forwarder_block_vectors)
{
    for (const BurnExitBlockCase& c : BURN_EXIT_BLOCK_CASES) {
        BOOST_TEST_CONTEXT("exit_case " << c.exit_case)
        {
            const ExitCase& exit{EXIT_CASES[c.exit_case]};
            const auto p{ExitParams(exit)};
            const auto encoded{modern::EncodeExitForwarderBlockV2(p)};
            BOOST_CHECK_EQUAL(HexStr(encoded), c.encoded);
            const auto decoded{modern::DecodeExitForwarderBlockV2(encoded)};
            BOOST_REQUIRE(decoded);
            BOOST_CHECK(*decoded == p);
            // The burn recipient of the case is its F_x account.
            BOOST_CHECK_EQUAL(c.recipient, exit.account);
        }
    }

    const auto encoded{modern::EncodeExitForwarderBlockV2(ValidExitParams())};
    std::vector<unsigned char> bytes(encoded.begin(), encoded.end());
    // Exact length only.
    BOOST_CHECK(!modern::DecodeExitForwarderBlockV2(std::span{bytes}.first(66)));
    bytes.push_back(0);
    BOOST_CHECK(!modern::DecodeExitForwarderBlockV2(bytes));
    bytes.pop_back();
    BOOST_CHECK(!modern::DecodeExitForwarderBlockV2(std::span<const unsigned char>{}));
    // Version 1 only.
    for (const unsigned char version : {0x00, 0x02, 0xff}) {
        bytes[0] = version;
        BOOST_CHECK(!modern::DecodeExitForwarderBlockV2(bytes));
    }
    bytes[0] = modern::EXIT_FORWARDER_BLOCK_VERSION_V1;
    BOOST_CHECK(modern::DecodeExitForwarderBlockV2(bytes));
}

BOOST_AUTO_TEST_CASE(exit_forwarder_params_refusals)
{
    const auto valid{ValidExitParams()};
    BOOST_CHECK(modern::ExitForwarderParamsValidV2(valid, PINNED_DST_EID));

    // No pin, or a pin p does not name.
    BOOST_CHECK(!modern::ExitForwarderParamsValidV2(valid, 0));
    BOOST_CHECK(!modern::ExitForwarderParamsValidV2(valid, 30101));
    auto p{valid};
    p.dst_eid = 0;
    BOOST_CHECK(!modern::ExitForwarderParamsValidV2(p, 0));
    p.dst_eid = 30101;
    BOOST_CHECK(!modern::ExitForwarderParamsValidV2(p, PINNED_DST_EID));

    // `to` must be an address word: each upper byte set, or a zero address.
    for (size_t i = 0; i < 12; ++i) {
        p = valid;
        p.to[i] = 0x01;
        BOOST_CHECK(!modern::ExitForwarderParamsValidV2(p, PINNED_DST_EID));
    }
    p = valid;
    p.to.fill(0);
    BOOST_CHECK(!modern::ExitForwarderParamsValidV2(p, PINNED_DST_EID));
    p.to[31] = 0x01; // address 0x...01 is a non-zero address word
    BOOST_CHECK(modern::ExitForwarderParamsValidV2(p, PINNED_DST_EID));

    // Bridge fee bound: 50 bps passes, 51 does not.
    p = valid;
    p.max_bridge_fee_bps = 50;
    BOOST_CHECK(modern::ExitForwarderParamsValidV2(p, PINNED_DST_EID));
    p.max_bridge_fee_bps = 51;
    BOOST_CHECK(!modern::ExitForwarderParamsValidV2(p, PINNED_DST_EID));
    p.max_bridge_fee_bps = std::numeric_limits<uint16_t>::max();
    BOOST_CHECK(!modern::ExitForwarderParamsValidV2(p, PINNED_DST_EID));

    // A zero refund EOA.
    p = valid;
    p.refund_eoa = Consensus::BridgeEthAddress{};
    BOOST_CHECK(!modern::ExitForwarderParamsValidV2(p, PINNED_DST_EID));

    // Any fee cap, zero and uint64 max included.
    p = valid;
    p.fee_cap = 0;
    BOOST_CHECK(modern::ExitForwarderParamsValidV2(p, PINNED_DST_EID));
    p.fee_cap = U64_MAX;
    BOOST_CHECK(modern::ExitForwarderParamsValidV2(p, PINNED_DST_EID));

    // Address-word helper round trip.
    const auto address{FillAddress(0x42)};
    const auto word{modern::ForwarderAddressWord(address)};
    BOOST_CHECK(modern::ForwarderAddressWordValid(word));
    BOOST_CHECK(std::all_of(word.begin(), word.begin() + 12, [](unsigned char b) { return b == 0; }));
    BOOST_CHECK(std::equal(address.begin(), address.end(), word.begin() + 12));
}

BOOST_AUTO_TEST_CASE(deposit_forwarder_params_refusals)
{
    const auto vault{Address(DEPOSIT_CASES[0].vault)};
    const auto token{FillAddress(0x77)};
    const auto factory{Address(DEPOSIT_CASES[0].factory)};
    const auto valid{DepositParams(DEPOSIT_CASES[0])};
    BOOST_CHECK(modern::DepositForwarderParamsValidV2(valid, vault, token, factory));

    // Another vault.
    BOOST_CHECK(!modern::DepositForwarderParamsValidV2(valid, FillAddress(0x22), token, factory));

    // RECIPIENT_V1: padding, version and a non-zero key hash.
    for (size_t i = 0; i < 11; ++i) {
        auto p{valid};
        p.b3_recipient[i] = 0x01;
        BOOST_CHECK(!modern::DepositForwarderParamsValidV2(p, vault, token, factory));
    }
    for (const unsigned char version : {0x00, 0x02, 0xff}) {
        auto p{valid};
        p.b3_recipient[11] = version;
        BOOST_CHECK(!modern::DepositForwarderParamsValidV2(p, vault, token, factory));
    }
    auto p{valid};
    std::fill(p.b3_recipient.begin() + 12, p.b3_recipient.end(), 0);
    BOOST_CHECK(!modern::DepositForwarderParamsValidV2(p, vault, token, factory));

    // Refund EOA: non-null and none of the vault, the token or the factory.
    for (const auto& refund : {Consensus::BridgeEthAddress{}, vault, token, factory}) {
        p = valid;
        p.refund_eoa = refund;
        BOOST_CHECK(!modern::DepositForwarderParamsValidV2(p, vault, token, factory));
    }

    // Fee cap and expiry are free.
    p = valid;
    p.fee_cap = 0;
    p.not_after = 0;
    BOOST_CHECK(modern::DepositForwarderParamsValidV2(p, vault, token, factory));
}

BOOST_AUTO_TEST_CASE(deposit_forwarder_fee_cap)
{
    // OI-2: one delivery of at most MAX_DEPOSIT_RAW per forwarder, so the
    // lifetime cap is fee_per_tx x 4 (deploy, two flushes, returnToOwner).
    BOOST_CHECK_EQUAL(modern::DEPOSIT_FORWARDER_FEE_CAP_TXS, 4U);
    for (const FeeCapCase& c : FEE_CAP_CASES) {
        BOOST_TEST_CONTEXT("fee_per_tx " << c.fee_per_tx)
        {
            BOOST_CHECK_EQUAL(modern::DepositForwarderFeeCapV2(c.fee_per_tx).value_or(0), c.fee_cap);
        }
    }
    // A zero fee, and one unit per transaction past the uint64 boundary.
    BOOST_CHECK(!modern::DepositForwarderFeeCapV2(0));
    BOOST_CHECK(!modern::DepositForwarderFeeCapV2(U64_MAX / 4 + 1));
    BOOST_CHECK(!modern::DepositForwarderFeeCapV2(U64_MAX));
}

BOOST_AUTO_TEST_CASE(light_client_horizon_and_not_after)
{
    constexpr int64_t DAY{24 * 60 * 60};
    constexpr int64_t I64_MAX{std::numeric_limits<int64_t>::max()};
    // The horizon starts at the first slot after the valid-through epoch:
    // epoch 9 ends with slot 319, so 320 - 100 = 220 slots after the head.
    BOOST_CHECK_EQUAL(modern::BridgeV2LightClientHorizonTime(9, 100, 1'000'000), 1'000'000 + 220 * 12);
    BOOST_CHECK_EQUAL(modern::BridgeV2LightClientHorizonTime(9, 319, 1'000'000), 1'000'000 + 12);
    // A head at or beyond it (never admitted by the light client) is the horizon.
    BOOST_CHECK_EQUAL(modern::BridgeV2LightClientHorizonTime(9, 320, 1'000'000), 1'000'000);
    BOOST_CHECK_EQUAL(modern::BridgeV2LightClientHorizonTime(9, 5'000, 1'000'000), 1'000'000);
    // Saturation instead of overflow.
    BOOST_CHECK_EQUAL(modern::BridgeV2LightClientHorizonTime(U64_MAX, 0, 0), I64_MAX);
    BOOST_CHECK_EQUAL(modern::BridgeV2LightClientHorizonTime(U64_MAX / 32 - 1, 0, 0), I64_MAX);
    BOOST_CHECK_EQUAL(modern::BridgeV2LightClientHorizonTime(0, 0, U64_MAX), I64_MAX);
    BOOST_CHECK_EQUAL(modern::BridgeV2LightClientHorizonTime(0, 0, I64_MAX - 384), I64_MAX);
    BOOST_CHECK_EQUAL(modern::BridgeV2LightClientHorizonTime(0, 0, I64_MAX - 385), I64_MAX - 1);

    // Without a horizon: now + 30 days.
    constexpr int64_t now{1'800'000'000};
    BOOST_CHECK_EQUAL(modern::DepositForwarderLatestNotAfterV2(now, std::nullopt).value_or(0), now + 30 * DAY);
    // A far horizon leaves 30 days; a nearer one caps at horizon - 24 h.
    BOOST_CHECK_EQUAL(modern::DepositForwarderLatestNotAfterV2(now, now + 31 * DAY).value_or(0), now + 30 * DAY);
    BOOST_CHECK_EQUAL(modern::DepositForwarderLatestNotAfterV2(now, now + 20 * DAY).value_or(0), now + 19 * DAY);
    BOOST_CHECK_EQUAL(modern::DepositForwarderLatestNotAfterV2(now, I64_MAX).value_or(0), now + 30 * DAY);
    // Exactly 7 days out is still served; closer, past or unset clocks are refused.
    BOOST_CHECK_EQUAL(modern::DepositForwarderLatestNotAfterV2(now, now + 7 * DAY).value_or(0), now + 6 * DAY);
    BOOST_CHECK(!modern::DepositForwarderLatestNotAfterV2(now, now + 7 * DAY - 1));
    BOOST_CHECK(!modern::DepositForwarderLatestNotAfterV2(now, now));
    BOOST_CHECK(!modern::DepositForwarderLatestNotAfterV2(now, 0));
    BOOST_CHECK(!modern::DepositForwarderLatestNotAfterV2(0, std::nullopt));
    BOOST_CHECK(!modern::DepositForwarderLatestNotAfterV2(-1, std::nullopt));
    BOOST_CHECK(!modern::DepositForwarderLatestNotAfterV2(I64_MAX - 30 * DAY + 1, std::nullopt));
}

BOOST_AUTO_TEST_CASE(reserved_eth_address_roles)
{
    const Consensus::BridgeV2Params params{ReservedRolesParams()};
    const std::pair<Consensus::BridgeEthAddress, std::string_view> roles[]{
        {params.vault.vault_address, "the bridge V2 launch vault"},
        {params.successor->entry.vault_address, "the bridge V2 successor vault"},
        {params.vault.token_address, "the bridge V2 origin token"},
        {params.verifier.address, "the bridge V2 verifier"},
        {params.deposit_forwarder->factory, "the bridge V2 deposit forwarder factory"},
        {params.exit_forwarder->factory, "the bridge V2 exit forwarder factory"},
    };
    for (const auto& [address, reason] : roles) {
        const auto got{modern::BridgeV2ReservedEthAddressReason(params, address)};
        BOOST_REQUIRE(got);
        BOOST_CHECK_EQUAL(*got, reason);
    }

    // Anything else, the null address included, holds no role.
    BOOST_CHECK(!modern::BridgeV2ReservedEthAddressReason(params, FillAddress(0xb0)));
    BOOST_CHECK(!modern::BridgeV2ReservedEthAddressReason(params, Consensus::BridgeEthAddress{}));
    BOOST_CHECK(!modern::BridgeV2ReservedEthAddressReason(params, Address(EXIT_CASES[0].account)));

    // Unset successor and forwarder pins name nothing.
    Consensus::BridgeV2Params launch_only{params};
    launch_only.successor.reset();
    launch_only.deposit_forwarder.reset();
    launch_only.exit_forwarder.reset();
    BOOST_CHECK(!modern::BridgeV2ReservedEthAddressReason(launch_only, params.successor->entry.vault_address));
    BOOST_CHECK(!modern::BridgeV2ReservedEthAddressReason(launch_only, params.deposit_forwarder->factory));
    BOOST_CHECK(!modern::BridgeV2ReservedEthAddressReason(launch_only, params.exit_forwarder->factory));
    BOOST_CHECK(modern::BridgeV2ReservedEthAddressReason(launch_only, params.vault.vault_address));
    BOOST_CHECK(modern::BridgeV2ReservedEthAddressReason(launch_only, params.vault.token_address));
    BOOST_CHECK(modern::BridgeV2ReservedEthAddressReason(launch_only, params.verifier.address));
}

BOOST_AUTO_TEST_CASE(forwarder_pins_valid)
{
    const Consensus::BridgeV2Params params{ReservedRolesParams()};
    BOOST_CHECK(params.deposit_forwarder->Valid());
    BOOST_CHECK(params.exit_forwarder->Valid());
    BOOST_CHECK(!Consensus::BridgeV2DepositForwarderPins{}.Valid());
    BOOST_CHECK(!Consensus::BridgeV2ExitForwarderPins{}.Valid());

    auto deposit{*params.deposit_forwarder};
    deposit.factory = {};
    BOOST_CHECK(!deposit.Valid());
    deposit = *params.deposit_forwarder;
    deposit.init_code_hash.SetNull();
    BOOST_CHECK(!deposit.Valid());

    auto exit{*params.exit_forwarder};
    exit.factory = {};
    BOOST_CHECK(!exit.Valid());
    exit = *params.exit_forwarder;
    exit.init_code_hash.SetNull();
    BOOST_CHECK(!exit.Valid());
    exit = *params.exit_forwarder;
    exit.dst_eid = 0;
    BOOST_CHECK(!exit.Valid());
}

BOOST_AUTO_TEST_CASE(refund_eoa_message_vectors)
{
    for (const RefundMessageCase& c : REFUND_MESSAGE_CASES) {
        BOOST_TEST_CONTEXT(c.name)
        {
            const std::string message{bridge::RefundEoaMessageV2(Evm(c.chain_domain), Address(c.refund_eoa))};
            BOOST_CHECK_EQUAL(message, c.message);
            BOOST_CHECK_EQUAL(EvmHex(bridge::Eip191Digest(message)), c.eip191_digest);
            const auto signature{ParseHex(c.signature)};
            BOOST_REQUIRE_EQUAL(signature.size(), bridge::EIP191_SIGNATURE_SIZE);
            const auto signer{bridge::RecoverEip191SignerV2(message, signature)};
            BOOST_REQUIRE(signer);
            BOOST_CHECK_EQUAL(HexStr(*signer), c.refund_eoa);

            // The raw recovery id form (v - 27) recovers the same signer.
            auto raw_v{signature};
            raw_v[64] -= 27;
            BOOST_CHECK(bridge::RecoverEip191SignerV2(message, raw_v) == signer);
        }
    }

    // The anvil dev account 0 signature over the regtest message recovers
    // 0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266.
    const RefundMessageCase& regtest{REFUND_MESSAGE_CASES[0]};
    BOOST_CHECK(bridge::RecoverEip191SignerV2(regtest.message, ParseHex(regtest.signature)) ==
                Address("f39fd6e51aad88f6f4ce6ab8827279cfffb92266"));

    // The regtest domain is ModernChainDomain(genesis, X = genesis) of a
    // -b3modernregtest node (genesis 10d0f5bb...929a, display order), written
    // as its raw bytes, so the message never shows the reversed display hex.
    const auto genesis{uint256::FromHex("10d0f5bb6fde880011fd56ea35dddbf9b32da2a7825f5579ac587a7904d6929a")};
    BOOST_REQUIRE(genesis);
    const auto domain{modern::ModernChainDomain(*genesis, *genesis)};
    BOOST_REQUIRE(domain);
    BOOST_CHECK_EQUAL(EvmHex(*domain), regtest.chain_domain);

    // The digest prefixes the decimal byte length of the message.
    BOOST_CHECK_EQUAL(EvmHex(bridge::Eip191Digest("")),
                      EvmHex(KeccakOf(std::string_view{"\x19" "Ethereum Signed Message:\n0", 27})));
    BOOST_CHECK_EQUAL(EvmHex(bridge::Eip191Digest("hello")), EvmHex(KeccakOf("\x19" "Ethereum Signed Message:\n5hello")));
}

BOOST_AUTO_TEST_CASE(refund_eoa_signature_round_trip_and_refusals)
{
    const uint256 domain{Evm(REFUND_MESSAGE_CASES[0].chain_domain)};
    for (const bool compressed : {true, false}) {
        BOOST_TEST_CONTEXT("compressed " << compressed)
        {
            const CKey key{GenerateRandomKey(compressed)};
            const auto address{bridge::EthAddressFromPubKey(key.GetPubKey())};
            BOOST_REQUIRE(address);
            const std::string message{bridge::RefundEoaMessageV2(domain, *address)};
            const auto signature{EthSign(key, message)};
            BOOST_REQUIRE_EQUAL(signature.size(), bridge::EIP191_SIGNATURE_SIZE);
            BOOST_CHECK(bridge::RecoverEip191SignerV2(message, signature) == address);

            // A wrong signer: another key's signature recovers another address.
            const CKey other{GenerateRandomKey()};
            const auto other_signature{EthSign(other, message)};
            const auto other_signer{bridge::RecoverEip191SignerV2(message, other_signature)};
            BOOST_REQUIRE(other_signer);
            BOOST_CHECK(*other_signer != *address);
            BOOST_CHECK(other_signer == bridge::EthAddressFromPubKey(other.GetPubKey()));

            // A flipped v recovers a different key, or none.
            auto flipped{signature};
            flipped[64] = flipped[64] == 27 ? 28 : 27;
            BOOST_CHECK(bridge::RecoverEip191SignerV2(message, flipped) != address);

            // Another message (a different chain domain or EOA) does not
            // recover the signer.
            const std::string other_domain_message{bridge::RefundEoaMessageV2(Fill(0x07), *address)};
            BOOST_CHECK(bridge::RecoverEip191SignerV2(other_domain_message, signature) != address);

            // A tampered r or s.
            auto tampered{signature};
            tampered[10] ^= 0x01;
            BOOST_CHECK(bridge::RecoverEip191SignerV2(message, tampered) != address);

            // Lengths other than 65 bytes.
            BOOST_CHECK(!bridge::RecoverEip191SignerV2(message, std::span{signature}.first(64)));
            auto longer{signature};
            longer.push_back(0);
            BOOST_CHECK(!bridge::RecoverEip191SignerV2(message, longer));
            BOOST_CHECK(!bridge::RecoverEip191SignerV2(message, std::span<const unsigned char>{}));

            // v outside {0, 1, 27, 28}.
            for (const unsigned char v : {2, 3, 26, 29, 30, 31, 35, 36, 0xff}) {
                auto bad_v{signature};
                bad_v[64] = v;
                BOOST_CHECK(!bridge::RecoverEip191SignerV2(message, bad_v));
            }
        }
    }

    // r = 0 (or s = 0) recovers nothing.
    std::vector<unsigned char> zero(bridge::EIP191_SIGNATURE_SIZE, 0);
    zero[64] = 27;
    BOOST_CHECK(!bridge::RecoverEip191SignerV2("B3", zero));
    // r, s at or above the group order are refused.
    std::vector<unsigned char> overflow(bridge::EIP191_SIGNATURE_SIZE, 0xff);
    overflow[64] = 27;
    BOOST_CHECK(!bridge::RecoverEip191SignerV2("B3", overflow));

    // EthAddressFromPubKey: anvil dev account 0's public key.
    const auto anvil0{bridge::EthAddressFromPubKey(CPubKey{ParseHex(
        "048318535b54105d4a7aae60c08fc45f9687181b4fdfc625bd1a753fa7397fed75"
        "3547f11ca8696646f2f3acb08e31016afac23e630c5d11f59f61fef57b0d2aa5")})};
    BOOST_REQUIRE(anvil0);
    BOOST_CHECK_EQUAL(HexStr(*anvil0), "f39fd6e51aad88f6f4ce6ab8827279cfffb92266");
    BOOST_CHECK(!bridge::EthAddressFromPubKey(CPubKey{}));
}

BOOST_AUTO_TEST_SUITE_END()
