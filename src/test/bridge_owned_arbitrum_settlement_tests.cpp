// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license.
#include <bridge/owned_arbitrum_settlement.h>
#include <boost/test/unit_test.hpp>
#include <type_traits>

namespace own=bridge::arbitrum::owned;
BOOST_AUTO_TEST_SUITE(bridge_owned_arbitrum_settlement_tests)
BOOST_AUTO_TEST_CASE(no_default_authority_or_public_chain_alias)
{
    static_assert(!std::is_default_constructible_v<own::Evidence>);
    static_assert(!std::is_constructible_v<bridge::arbitrum::sepolia::SettledHead,own::Evidence>);
    BOOST_CHECK_EQUAL(own::L1_CHAIN_ID,1337U);BOOST_CHECK_EQUAL(own::L2_CHAIN_ID,412346U);
    BOOST_CHECK(!own::ValidPolicy({}));
    BOOST_CHECK(!own::Verifier::Verify({},{}).evidence);
    own::Input in;in.regtest=true;BOOST_CHECK(!own::Verifier::Verify({},in).evidence);
}
BOOST_AUTO_TEST_CASE(only_reviewed_packed_delay_counters_may_evolve)
{
    bridge::StorageWord base{},changed{};
    for(size_t byte=0;byte<32;++byte){changed={};changed[byte]=1;
        BOOST_CHECK_EQUAL(own::EqualMasked(13,bridge::SlotWord(12),base,changed),byte<8||byte>=24);
        BOOST_CHECK_EQUAL(own::EqualMasked(13,bridge::SlotWord(13),base,changed),byte>=16&&byte<24);
        BOOST_CHECK(!own::EqualMasked(13,bridge::SlotWord(10),base,changed));
        BOOST_CHECK(!own::EqualMasked(0,bridge::SlotWord(123),base,changed));}
}
BOOST_AUTO_TEST_CASE(assertion_mapping_derivation_is_shared_but_not_network_authority)
{
    const uint256 hash{std::vector<unsigned char>(32,0x42)};
    BOOST_CHECK(own::AssertionSlot(hash)==bridge::arbitrum::sepolia::detail::AssertionSlot(hash));
    bridge::StorageWord slot{};slot.back()=0xff;const auto next{own::Next(slot)};
    BOOST_CHECK_EQUAL(next[30],1);BOOST_CHECK_EQUAL(next[31],0);
}
BOOST_AUTO_TEST_SUITE_END()
