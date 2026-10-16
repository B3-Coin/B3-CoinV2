// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** Boost.Test runner for the FlowMesh V2 rebirth outline (test_flowmesh2).
 *
 * Single-header runner, as src/test/main.cpp: only this file includes
 * boost/test/included/unit_test.hpp; every other test file includes
 * boost/test/unit_test.hpp. No global fixture yet (an ECC fixture arrives with
 * the first signing code). Every case is listed in test/INVENTORY.txt and the
 * CTest flowmesh2_inventory fails if a listed case is missing or did not run.
 */
#define BOOST_TEST_MODULE FlowMesh2 Rebirth Outline Tests

#include <boost/test/included/unit_test.hpp>
