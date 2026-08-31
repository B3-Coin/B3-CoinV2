// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bridge/admission.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <cstdint>
#include <vector>

using namespace bridge;

BOOST_AUTO_TEST_SUITE(bridge_admission_tests)

namespace {

std::array<unsigned char, 32> Uint256Amount(uint64_t value)
{
    std::array<unsigned char, 32> out{};
    for (int i{31}; i >= 24; --i) {
        out[i] = static_cast<unsigned char>(value & 0xff);
        value >>= 8;
    }
    return out;
}

EthAddress Address(const unsigned char first)
{
    EthAddress out{};
    for (size_t i{0}; i < out.size(); ++i) {
        out[i] = static_cast<unsigned char>(first + i);
    }
    return out;
}

RecipientV1 Recipient(const unsigned char first)
{
    RecipientV1 out;
    for (size_t i{0}; i < out.pubkey_hash.size(); ++i) {
        out.pubkey_hash[i] = static_cast<unsigned char>(first + i);
    }
    return out;
}

BridgeAssetRegistryEntry ActiveRegistry()
{
    BridgeAssetRegistryEntry entry;
    entry.origin_chain_id = 1;
    entry.vault_address = Address(0x10);
    entry.token_address = Address(0x40);
    entry.b3_asset_id = uint256::ONE;
    entry.origin_decimals = 6;
    entry.asset_decimals = 6;
    entry.implementation_or_adapter = uint256::ONE;
    entry.adapter_version = 1;
    entry.approval_first_height = 100;
    entry.approval_last_height = 200;
    entry.state = BridgeRegistryState::ACTIVE;
    return entry;
}

ProvenBridgeDeposit MatchingDeposit(const BridgeAssetRegistryEntry& registry)
{
    ProvenBridgeDeposit deposit;
    deposit.origin_chain_id = registry.origin_chain_id;
    deposit.vault_address = registry.vault_address;
    deposit.event.deposit_id = 17;
    deposit.event.token = registry.token_address;
    deposit.event.amount = Uint256Amount(1'250'000); // 1.25 at six decimals
    deposit.event.b3_recipient = EncodeRecipientV1(Recipient(0x80));
    return deposit;
}

} // namespace

BOOST_AUTO_TEST_CASE(recipient_v1_is_exact_and_fail_closed)
{
    const RecipientV1 recipient{Recipient(0x20)};
    const auto encoded{EncodeRecipientV1(recipient)};
    BOOST_CHECK(std::all_of(encoded.begin(), encoded.begin() + RECIPIENT_V1_PADDING_SIZE,
                            [](const unsigned char byte) { return byte == 0; }));
    BOOST_CHECK_EQUAL(encoded[RECIPIENT_V1_PADDING_SIZE], RECIPIENT_V1_P2PKH);

    const auto decoded{DecodeRecipientV1(encoded)};
    BOOST_REQUIRE(decoded);
    BOOST_CHECK(*decoded == recipient);
    BOOST_CHECK(RecipientV1Script(*decoded) ==
                (CScript{} << OP_DUP << OP_HASH160
                           << std::vector<unsigned char>{recipient.pubkey_hash.begin(),
                                                         recipient.pubkey_hash.end()}
                           << OP_EQUALVERIFY << OP_CHECKSIG));

    auto bad_padding{encoded};
    bad_padding[0] = 1;
    BOOST_CHECK(!DecodeRecipientV1(bad_padding));
    auto bad_version{encoded};
    bad_version[RECIPIENT_V1_PADDING_SIZE] = RECIPIENT_V1_P2PKH + 1;
    BOOST_CHECK(!DecodeRecipientV1(bad_version));
    BOOST_CHECK(!DecodeRecipientV1(std::array<unsigned char, 32>{}));
}

BOOST_AUTO_TEST_CASE(raw_unit_conversion_is_exact_and_bounded)
{
    BOOST_CHECK_EQUAL(*ConvertRawUnitsExact(Uint256Amount(1'000'000), 6, 6), 1'000'000);
    BOOST_CHECK_EQUAL(*ConvertRawUnitsExact(Uint256Amount(1'000'000), 6, 9), 1'000'000'000);
    BOOST_CHECK_EQUAL(*ConvertRawUnitsExact(Uint256Amount(1'230'000'000'000ULL), 12, 6),
                      1'230'000);

    BOOST_CHECK(!ConvertRawUnitsExact(Uint256Amount(1'230'000'000'001ULL), 12, 6));
    BOOST_CHECK(!ConvertRawUnitsExact(Uint256Amount(0), 6, 6));
    BOOST_CHECK(!ConvertRawUnitsExact(Uint256Amount(1), 19, 6));
    BOOST_CHECK(!ConvertRawUnitsExact(Uint256Amount(static_cast<uint64_t>(MAX_MONEY)), 6, 7));

    auto too_large{Uint256Amount(1)};
    too_large[0] = 1;
    BOOST_CHECK(!ConvertRawUnitsExact(too_large, 6, 6));
}

BOOST_AUTO_TEST_CASE(active_full_tuple_authorizes_only_the_exact_deposit)
{
    const BridgeAssetRegistryEntry registry{ActiveRegistry()};
    const ProvenBridgeDeposit deposit{MatchingDeposit(registry)};
    BridgeMintAuthorization mint;
    BOOST_CHECK(AdmitProvenDeposit(registry, deposit, 150, mint) == BridgeAdmissionResult::OK);
    BOOST_CHECK(mint.asset == registry.b3_asset_id);
    BOOST_CHECK_EQUAL(mint.amount, 1'250'000);
    BOOST_CHECK(mint.recipient_script == RecipientV1Script(Recipient(0x80)));
    BOOST_CHECK(mint.nullifier ==
                (BridgeDepositKey{registry.origin_chain_id, registry.vault_address, 17}));

    auto changed{deposit};
    changed.origin_chain_id = 2;
    BOOST_CHECK(AdmitProvenDeposit(registry, changed, 150, mint) ==
                BridgeAdmissionResult::ORIGIN_MISMATCH);
    changed = deposit;
    changed.vault_address[0] ^= 1;
    BOOST_CHECK(AdmitProvenDeposit(registry, changed, 150, mint) ==
                BridgeAdmissionResult::VAULT_MISMATCH);
    changed = deposit;
    changed.event.token[0] ^= 1;
    BOOST_CHECK(AdmitProvenDeposit(registry, changed, 150, mint) ==
                BridgeAdmissionResult::TOKEN_MISMATCH);
    changed = deposit;
    changed.event.b3_recipient = {};
    BOOST_CHECK(AdmitProvenDeposit(registry, changed, 150, mint) ==
                BridgeAdmissionResult::RECIPIENT_INVALID);
    changed = deposit;
    changed.event.amount = {};
    BOOST_CHECK(AdmitProvenDeposit(registry, changed, 150, mint) ==
                BridgeAdmissionResult::AMOUNT_INVALID);
}

BOOST_AUTO_TEST_CASE(proposed_expired_or_incomplete_registry_never_mints)
{
    const ProvenBridgeDeposit deposit{MatchingDeposit(ActiveRegistry())};
    BridgeMintAuthorization mint;

    auto registry{ActiveRegistry()};
    registry.state = BridgeRegistryState::PROPOSED;
    BOOST_CHECK(AdmitProvenDeposit(registry, deposit, 150, mint) ==
                BridgeAdmissionResult::REGISTRY_INACTIVE);

    registry = ActiveRegistry();
    BOOST_CHECK(AdmitProvenDeposit(registry, deposit, 99, mint) ==
                BridgeAdmissionResult::REGISTRY_INACTIVE);
    BOOST_CHECK(AdmitProvenDeposit(registry, deposit, 201, mint) ==
                BridgeAdmissionResult::REGISTRY_INACTIVE);

    registry = ActiveRegistry();
    registry.implementation_or_adapter = {};
    BOOST_CHECK(AdmitProvenDeposit(registry, deposit, 150, mint) ==
                BridgeAdmissionResult::REGISTRY_INACTIVE);
    registry = ActiveRegistry();
    registry.vault_address = {};
    BOOST_CHECK(AdmitProvenDeposit(registry, deposit, 150, mint) ==
                BridgeAdmissionResult::REGISTRY_INACTIVE);
    registry = ActiveRegistry();
    registry.adapter_version = 0;
    BOOST_CHECK(AdmitProvenDeposit(registry, deposit, 150, mint) ==
                BridgeAdmissionResult::REGISTRY_INACTIVE);
}

BOOST_AUTO_TEST_CASE(nullifier_apply_replay_and_undo_are_atomic)
{
    const auto registry{ActiveRegistry()};
    const BridgeDepositKey first{registry.origin_chain_id, registry.vault_address, 1};
    const BridgeDepositKey second{registry.origin_chain_id, registry.vault_address, 2};
    EthAddress another_vault{registry.vault_address};
    another_vault[0] ^= 1;
    const BridgeDepositKey same_id_other_vault{registry.origin_chain_id, another_vault, 1};

    BridgeNullifierSet set;
    BridgeNullifierUndo undo;
    const std::array batch{first, second, same_id_other_vault};
    BOOST_REQUIRE(set.ApplyBlock(batch, undo));
    BOOST_CHECK_EQUAL(set.Size(), 3U);
    BOOST_CHECK(set.Contains(first));
    BOOST_CHECK(set.Contains(same_id_other_vault));

    BridgeNullifierUndo rejected;
    BOOST_CHECK(!set.ApplyBlock(std::span<const BridgeDepositKey>{batch}.first(1), rejected));
    BOOST_CHECK(rejected.inserted.empty());
    BOOST_CHECK_EQUAL(set.Size(), 3U);

    const std::array duplicate{BridgeDepositKey{1, Address(0x70), 9},
                               BridgeDepositKey{1, Address(0x70), 9}};
    BOOST_CHECK(!set.ApplyBlock(duplicate, rejected));
    BOOST_CHECK_EQUAL(set.Size(), 3U);

    BridgeNullifierUndo bad_undo{{BridgeDepositKey{1, Address(0x60), 99}}};
    BOOST_CHECK(!set.UndoBlock(bad_undo));
    BOOST_CHECK_EQUAL(set.Size(), 3U);

    BOOST_REQUIRE(set.UndoBlock(undo));
    BOOST_CHECK_EQUAL(set.Size(), 0U);
    BOOST_CHECK(!set.UndoBlock(undo));
}

BOOST_AUTO_TEST_SUITE_END()
