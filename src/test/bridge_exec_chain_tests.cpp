// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Execution-header ancestry proofs (bridge/exec_chain.h): synthetic
// parent-hash chains built with the strict RLP encoder, plus every way a
// chain can be forged or malformed. The live mainnet path is exercised by
// contrib/b3bridge/eth_live_test.py --tx (83-header chain verified
// 2026-08-24 against block 25824112).

#include <bridge/exec_chain.h>
#include <bridge/rlp.h>
#include <crypto/keccak256.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

using namespace bridge;

BOOST_AUTO_TEST_SUITE(bridge_exec_chain_tests)

namespace {

//! Build a minimal post-London execution header (16 fields) with the given
//! parent hash, number and receipts root.
std::vector<unsigned char> MakeHeader(const uint256& parent, uint64_t number,
                                      const uint256& receipts_root)
{
    std::vector<std::vector<unsigned char>> f;
    f.push_back(RlpEncodeBytes({parent.begin(), 32}));                    // parentHash
    const std::vector<unsigned char> zero32(32, 0x11);
    const std::vector<unsigned char> addr(20, 0x22);
    f.push_back(RlpEncodeBytes(zero32));                                   // sha3Uncles
    f.push_back(RlpEncodeBytes(addr));                                     // miner
    f.push_back(RlpEncodeBytes(zero32));                                   // stateRoot
    f.push_back(RlpEncodeBytes(zero32));                                   // transactionsRoot
    f.push_back(RlpEncodeBytes({receipts_root.begin(), 32}));              // receiptsRoot
    f.push_back(RlpEncodeBytes(std::vector<unsigned char>(256, 0x00)));    // logsBloom
    f.push_back(RlpEncodeUint64(0));                                       // difficulty
    f.push_back(RlpEncodeUint64(number));                                  // number
    f.push_back(RlpEncodeUint64(30'000'000));                              // gasLimit
    f.push_back(RlpEncodeUint64(1'000'000));                               // gasUsed
    f.push_back(RlpEncodeUint64(1'700'000'000 + number));                  // timestamp
    f.push_back(RlpEncodeBytes({}));                                       // extraData
    f.push_back(RlpEncodeBytes(zero32));                                   // mixHash
    f.push_back(RlpEncodeBytes(std::vector<unsigned char>(8, 0x00)));      // nonce
    f.push_back(RlpEncodeUint64(7));                                       // baseFeePerGas
    return RlpEncodeList(f);
}

uint256 KeccakOf(std::span<const unsigned char> d)
{
    uint256 out;
    Keccak256().Write(d).Finalize(out);
    return out;
}

uint256 RootFor(uint64_t n)
{
    uint256 r;
    *r.begin() = static_cast<unsigned char>(n & 0xff);
    *(r.begin() + 1) = 0xaa;
    return r;
}

//! Chain of headers for blocks [lo, hi], returned NEWEST FIRST, plus the
//! tip (block hi) hash.
std::pair<std::vector<std::vector<unsigned char>>, uint256> MakeChain(uint64_t lo, uint64_t hi)
{
    std::vector<std::vector<unsigned char>> oldest_first;
    uint256 parent{}; // genesis-of-test parent
    for (uint64_t n = lo; n <= hi; ++n) {
        oldest_first.push_back(MakeHeader(parent, n, RootFor(n)));
        parent = KeccakOf(oldest_first.back());
    }
    std::vector<std::vector<unsigned char>> newest_first{oldest_first.rbegin(), oldest_first.rend()};
    return {newest_first, parent};
}

} // namespace

BOOST_AUTO_TEST_CASE(valid_walks)
{
    const auto [chain, tip]{MakeChain(100, 110)};

    // Full walk to the oldest block.
    const auto a{VerifyExecAncestry(tip, 100, chain)};
    BOOST_REQUIRE(a);
    BOOST_CHECK_EQUAL(a->block_number, 100U);
    BOOST_CHECK(a->receipts_root == RootFor(100));

    // Single-header "chain": the proven block itself.
    const std::vector<std::vector<unsigned char>> self{chain.front()};
    const auto b{VerifyExecAncestry(tip, 110, self)};
    BOOST_REQUIRE(b);
    BOOST_CHECK(b->receipts_root == RootFor(110));

    // Partial walk: chain trimmed to end exactly at the target.
    std::vector<std::vector<unsigned char>> part{chain.begin(), chain.begin() + 4}; // 110..107
    const auto c{VerifyExecAncestry(tip, 107, part)};
    BOOST_REQUIRE(c);
    BOOST_CHECK_EQUAL(c->block_number, 107U);
}

BOOST_AUTO_TEST_CASE(forgeries_rejected)
{
    const auto [chain, tip]{MakeChain(100, 110)};

    { // Wrong tip hash.
        uint256 bad{tip};
        *bad.begin() ^= 0x01;
        BOOST_CHECK(!VerifyExecAncestry(bad, 100, chain));
    }
    { // A mutated byte in a middle header breaks the keccak link.
        auto m{chain};
        m[5][40] ^= 0x01;
        BOOST_CHECK(!VerifyExecAncestry(tip, 100, m));
    }
    { // A deleted middle header breaks the parent linkage.
        auto m{chain};
        m.erase(m.begin() + 5);
        BOOST_CHECK(!VerifyExecAncestry(tip, 100, m));
    }
    { // Chain that ends before the target.
        std::vector<std::vector<unsigned char>> part{chain.begin(), chain.begin() + 4};
        BOOST_CHECK(!VerifyExecAncestry(tip, 100, part));
    }
    { // Trailing junk after the target header.
        BOOST_CHECK(!VerifyExecAncestry(tip, 107, chain)); // target mid-chain, chain continues past it
    }
    { // Target newer than the tip is unreachable.
        BOOST_CHECK(!VerifyExecAncestry(tip, 111, chain));
    }
    { // Empty chain.
        BOOST_CHECK(!VerifyExecAncestry(tip, 110, {}));
    }
    { // Non-header garbage.
        const std::vector<std::vector<unsigned char>> junk{{0x83, 0x64, 0x6f, 0x67}};
        BOOST_CHECK(!VerifyExecAncestry(KeccakOf(junk[0]), 110, junk));
    }
}

BOOST_AUTO_TEST_SUITE_END()
