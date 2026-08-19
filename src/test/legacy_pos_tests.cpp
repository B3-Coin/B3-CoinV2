// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <legacy/consensus.h>
#include <legacy/pos.h>

#include <chain.h>
#include <key.h>
#include <pubkey.h>
#include <script/script.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <array>

BOOST_AUTO_TEST_SUITE(legacy_pos_tests)

BOOST_AUTO_TEST_CASE(kernel_serialization_matches_old_chain)
{
    const legacy::pos::KernelInput input{
        .stake_modifier = 0x0102030405060708,
        .source_block_time = 1481667355,
        .source_transaction_offset = 1234,
        .source_transaction_time = 1481667355,
        .source_output_index = 2,
        .stake_time = 1481674555,
    };

    BOOST_CHECK_EQUAL(legacy::pos::ComputeKernelHash(input).GetHex(),
        "bdad6b08bea71027b572844c9bb43eba5854cd4f9c241e9d17f99cc6bad0d0ba");
}

BOOST_AUTO_TEST_CASE(validates_the_first_historical_pos_kernel)
{
    // Block 136 on the B3Coin main chain. This is the first historical PoS
    // block encountered during initial sync and anchors the real kernel data.
    const legacy::pos::KernelInput input{
        .stake_modifier = 0xbee43f6d062b61cd,
        .source_block_time = 1'482'055'707,
        .source_transaction_offset = 174,
        .source_transaction_time = 1'482'055'237,
        .source_output_index = 1,
        .stake_time = 1'482'088'579,
    };

    const auto result{legacy::pos::EvaluateKernel(input, 0x1e0fffff,
        23'095 * legacy::pos::Params::COIN)};
    BOOST_REQUIRE(result);
    BOOST_CHECK_EQUAL(result->proof_hash.GetHex(),
        "019634c6378f4747330deb6735f0009f92e282140f22237b8b13b233f96e36e5");
    BOOST_CHECK_EQUAL(result->coin_day_weight, 7'950U);
    BOOST_CHECK(result->IsValid());
}

BOOST_AUTO_TEST_CASE(enforces_old_chain_age_and_timestamp_rules)
{
    legacy::pos::KernelInput input{
        .stake_modifier = 1,
        .source_block_time = 1'000,
        .source_transaction_offset = 80,
        .source_transaction_time = 1'000,
        .source_output_index = 0,
        .stake_time = 1'000 + legacy::pos::Params::MIN_AGE,
    };

    BOOST_CHECK(!legacy::pos::EvaluateKernel(input, 0x1e0fffff, 10'000'000));
    ++input.stake_time;
    BOOST_CHECK(!legacy::pos::EvaluateKernel(input, 0x1e0fffff, 10'000'000));

    input.stake_time += 24 * 60 * 60;
    const auto result{legacy::pos::EvaluateKernel(input, 0x1e0fffff, 10'000'000)};
    BOOST_REQUIRE(result);
    BOOST_CHECK_EQUAL(result->coin_day_weight, 10U);

    BOOST_CHECK(legacy::pos::CheckCoinStakeTimestamp(10, 10));
    BOOST_CHECK(!legacy::pos::CheckCoinStakeTimestamp(10, 11));
}

BOOST_AUTO_TEST_CASE(stake_modifier_uses_historical_numeric_hash_order)
{
    // The old B3Coin uint256 type compared hashes as numeric little-endian
    // values, unlike Core's bytewise uint256 ordering. These values make the
    // first selection differ if the latter is accidentally used.
    std::array<CBlockIndex, 4> indexes{};
    const std::array<uint256, 4> block_hashes{uint256{1}, uint256{2}, uint256{3}, uint256{4}};
    constexpr std::array<uint32_t, 4> block_times{0, 4'400, 4'500, 36'000};

    for (size_t i{0}; i < indexes.size(); ++i) {
        CBlockIndex& index{indexes[i]};
        index.phashBlock = &block_hashes[i];
        index.nHeight = static_cast<int>(i);
        index.nTime = block_times[i];
        index.pprev = i == 0 ? nullptr : &indexes[i - 1];
    }
    indexes[0].m_legacy_stake_modifier_generated = true;
    indexes[1].m_legacy_hash_proof = uint256{};
    indexes[2].m_legacy_hash_proof = uint256{3};
    indexes[3].m_legacy_hash_proof = uint256{};

    uint64_t stake_modifier{0};
    bool generated{false};
    BOOST_REQUIRE(legacy::ComputeNextStakeModifier(&indexes.back(), stake_modifier, generated));
    BOOST_CHECK(generated);
    BOOST_CHECK_EQUAL(stake_modifier, 2U);
}

BOOST_AUTO_TEST_CASE(preserves_historical_proof_of_integration_fee_accounting)
{
    // The historical client's height-tiered Fundamental Node collateral
    // (fn-activity.h GetFNCollateral): 25M B3 through 85000, 20M through
    // 105000, 15M afterwards. Destroyed by proof of integration: an
    // input/output shortfall, never an output to a burn address.
    const std::pair<int, CAmount> tiers[]{
        {1, 25'000'000 * COIN},
        {85'000, 25'000'000 * COIN},
        {85'001, 20'000'000 * COIN},
        {105'000, 20'000'000 * COIN},
        {105'001, 15'000'000 * COIN},
        {1'000'000, 15'000'000 * COIN},
    };

    const Consensus::Params params{}; // no test override: the historical schedule
    const CAmount output{10 * COIN};
    for (const auto& [height, collateral] : tiers) {
        BOOST_CHECK_EQUAL(legacy::GetFNCollateral(height, params), collateral);
        // Exactly the collateral destroyed: the whole shortfall is a burn,
        // none of it counts as fees.
        BOOST_CHECK_EQUAL(
            legacy::GetLegacyTransactionFee(output + collateral, output, /*is_coinstake=*/false, height, params),
            0);
        // Excess above the collateral counts as an ordinary fee.
        BOOST_CHECK_EQUAL(
            legacy::GetLegacyTransactionFee(output + collateral + COIN, output, /*is_coinstake=*/false, height, params),
            COIN);
        // Below the collateral nothing is treated as a burn.
        BOOST_CHECK_EQUAL(
            legacy::GetLegacyTransactionFee(output + collateral - COIN, output, /*is_coinstake=*/false, height, params),
            collateral - COIN);
    }

    BOOST_CHECK_EQUAL(
        legacy::GetLegacyTransactionFee(output + COIN, output, /*is_coinstake=*/true, /*height=*/1, params),
        0);
}

BOOST_AUTO_TEST_CASE(historical_reward_rule_exceptions_are_sourced)
{
    // Repair window: the final client skips the reward-cap check strictly
    // inside (77446, 77506).
    BOOST_CHECK(!legacy::IsRepairWindowHeight(77'446));
    BOOST_CHECK(legacy::IsRepairWindowHeight(77'447));
    BOOST_CHECK(legacy::IsRepairWindowHeight(77'505));
    BOOST_CHECK(!legacy::IsRepairWindowHeight(77'506));
    BOOST_CHECK(!legacy::IsRepairWindowHeight(107'488)); // the superblock is NOT a cap bypass

    // Superblock payment bound: main.h SUPERBLOCKPAYMENT = 75656908 * 1e9.
    BOOST_CHECK_EQUAL(legacy::LEGACY_SUPERBLOCK_PAYMENT, CAmount{75'656'908'000'000'000});

    // Superblock payee: the P2PKH script of the pinned key's hash. The hash160
    // of the historical vSuperBlockPubKey is pinned here as a golden vector.
    const auto pubkey{ParseHex("0432160bdb95ec14c30a3c76ed742403a34d3b57841f49caec6971eee735bcc68d35d35936c66719910b32c51db72621191437d23659785fe20ee7268e7d340522")};
    const CScript payee{legacy::SuperblockPayeeScript(pubkey)};
    const CScript expected{CScript() << OP_DUP << OP_HASH160
                                     << ParseHex("1c49f78e1a406c64996da1bc5fda3b371bd33706")
                                     << OP_EQUALVERIFY << OP_CHECKSIG};
    BOOST_CHECK(payee == expected);

    // Restriction activation boundary.
    BOOST_CHECK_EQUAL(legacy::LEGACY_RESTRICTED_STAKE_HEIGHT, 78'000);

    // The restricted key id is the base58 payload of
    // ShJsVNBQMa2M7cfCVPzRMt8nVZxHitBp7v (version byte 63).
    BOOST_CHECK_EQUAL(HexStr(legacy::RestrictedStakeKeyId()), "db8ca2a4493aaed6b7d2f30acb4467b823e0b0a5");

    // Old-client destination semantics: P2PKH of the key id matches...
    const CScript restricted_p2pkh{CScript() << OP_DUP << OP_HASH160
                                             << ToByteVector(legacy::RestrictedStakeKeyId())
                                             << OP_EQUALVERIFY << OP_CHECKSIG};
    BOOST_CHECK(legacy::StakeDestinationIsRestricted(restricted_p2pkh));
    // ...an unrelated P2PKH does not...
    const CScript other_p2pkh{CScript() << OP_DUP << OP_HASH160
                                        << ParseHex("1c49f78e1a406c64996da1bc5fda3b371bd33706")
                                        << OP_EQUALVERIFY << OP_CHECKSIG};
    BOOST_CHECK(!legacy::StakeDestinationIsRestricted(other_p2pkh));
    // ...and pay-to-pubkey folds to the key's hash exactly as the old Solver
    // did: the (known) superblock key matches its own id through the P2PK arm,
    // and does not match a different id.
    const CScript p2pk{CScript() << pubkey << OP_CHECKSIG};
    uint160 superblock_id;
    const auto superblock_id_bytes{ParseHex("1c49f78e1a406c64996da1bc5fda3b371bd33706")};
    std::copy(superblock_id_bytes.begin(), superblock_id_bytes.end(), superblock_id.begin());
    BOOST_CHECK(legacy::StakeDestinationMatches(p2pk, superblock_id));
    BOOST_CHECK(!legacy::StakeDestinationMatches(p2pk, legacy::RestrictedStakeKeyId()));
    // Non-standard scripts never match.
    BOOST_CHECK(!legacy::StakeDestinationMatches(CScript() << OP_TRUE, legacy::RestrictedStakeKeyId()));
}

BOOST_AUTO_TEST_SUITE_END()
