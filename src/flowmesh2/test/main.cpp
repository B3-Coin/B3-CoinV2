// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** Boost.Test runner for the FlowMesh V2 rebirth outline (test_flowmesh2).
 *
 * Single-header runner, as src/test/main.cpp: only this file includes
 * boost/test/included/unit_test.hpp; every other test file includes
 * boost/test/unit_test.hpp. Every case is listed in test/INVENTORY.txt and the
 * CTest flowmesh2_inventory fails if a listed case is missing or did not run.
 *
 * Global fixture (slice R4): one ECC_Context for the whole run, so tests can
 * sign with CKey (test/util/keys.h). Verification needs no fixture: it uses
 * libsecp256k1's static context.
 */
#define BOOST_TEST_MODULE FlowMesh2 Rebirth Outline Tests

#include <boost/test/included/unit_test.hpp>

#include <key.h>

namespace {
struct Fm2EccFixture {
    ECC_Context ecc_context;
};
} // namespace

BOOST_TEST_GLOBAL_FIXTURE(Fm2EccFixture);
