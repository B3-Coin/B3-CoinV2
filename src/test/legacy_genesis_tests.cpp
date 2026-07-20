// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <legacy/primitives.h>

#include <streams.h>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(legacy_genesis_tests)

BOOST_AUTO_TEST_CASE(matches_existing_b3coin_chain)
{
    const legacy::Block genesis{legacy::CreateGenesisBlock()};

    BOOST_CHECK_EQUAL(genesis.bits, 0x1e0fffffU);
    BOOST_CHECK_EQUAL(genesis.merkle_root.GetHex(),
        "4243fd570d4cb2e2930767f5bf18b2f65f1b7c4e16a392552d1efadeec00753d");
    BOOST_CHECK_EQUAL(genesis.GetHash().GetHex(),
        "4b0d7f133c5267d715d4d8992635a5490d1edd6b7072cce3f8fe116aba983b6a");

    DataStream encoded_header;
    encoded_header << static_cast<const legacy::BlockHeader&>(genesis);
    BOOST_CHECK_EQUAL(encoded_header.size(), 80U);
}

BOOST_AUTO_TEST_SUITE_END()
