// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Execution-header ancestry proofs (bridge/exec_chain.h): synthetic
// parent-hash chains built with the strict RLP encoder, plus every way a
// chain can be forged or malformed. The live mainnet path is exercised by
// contrib/b3bridge/eth_live_test.py --tx (83-header chain verified
// 2026-08-24 against block 25824112).

#include <bridge/exec_chain.h>
#include <bridge/relayer_plan.h>
#include <bridge/rlp.h>
#include <crypto/keccak256.h>
#include <crypto/sha256.h>
#include <test/data/eth_lc_fixture.h>
#include <test/data/eth_receipts_proof_fixture.h>
#include <uint256.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <optional>

using namespace bridge;

BOOST_AUTO_TEST_SUITE(bridge_exec_chain_tests)

namespace {

//! Build a minimal post-London execution header (16 fields) with the given
//! parent hash, number and receipts root. `state_item` replaces the encoded
//! stateRoot field (default: 32 bytes of 0x11).
std::vector<unsigned char> MakeHeader(const uint256& parent, uint64_t number,
                                      const uint256& receipts_root,
                                      const size_t extra_data_size = 0,
                                      const std::optional<std::vector<unsigned char>>& state_item = std::nullopt)
{
    std::vector<std::vector<unsigned char>> f;
    f.push_back(RlpEncodeBytes({parent.begin(), 32}));                    // parentHash
    const std::vector<unsigned char> zero32(32, 0x11);
    const std::vector<unsigned char> addr(20, 0x22);
    f.push_back(RlpEncodeBytes(zero32));                                   // sha3Uncles
    f.push_back(RlpEncodeBytes(addr));                                     // miner
    f.push_back(state_item ? *state_item : RlpEncodeBytes(zero32));        // stateRoot
    f.push_back(RlpEncodeBytes(zero32));                                   // transactionsRoot
    f.push_back(RlpEncodeBytes({receipts_root.begin(), 32}));              // receiptsRoot
    f.push_back(RlpEncodeBytes(std::vector<unsigned char>(256, 0x00)));    // logsBloom
    f.push_back(RlpEncodeUint64(0));                                       // difficulty
    f.push_back(RlpEncodeUint64(number));                                  // number
    f.push_back(RlpEncodeUint64(30'000'000));                              // gasLimit
    f.push_back(RlpEncodeUint64(1'000'000));                               // gasUsed
    f.push_back(RlpEncodeUint64(1'700'000'000 + number));                  // timestamp
    f.push_back(RlpEncodeBytes(std::vector<unsigned char>(extra_data_size,
                                                          0x33)));         // extraData
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
std::pair<std::vector<std::vector<unsigned char>>, uint256> MakeChain(
    uint64_t lo, uint64_t hi, const size_t extra_data_size = 0)
{
    std::vector<std::vector<unsigned char>> oldest_first;
    uint256 parent{}; // genesis-of-test parent
    for (uint64_t n = lo; n <= hi; ++n) {
        oldest_first.push_back(
            MakeHeader(parent, n, RootFor(n), extra_data_size));
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

BOOST_AUTO_TEST_CASE(state_root_is_recorded)
{
    const auto [chain, tip]{MakeChain(100, 110)};
    const auto target{VerifyExecAncestry(tip, 100, chain)};
    BOOST_REQUIRE(target);
    const std::vector<unsigned char> expected(32, 0x11);
    BOOST_CHECK(target->state_root == uint256{expected});

    // V1 parity: a header whose field [3] is not a 32-byte string still
    // links, but records no state root to prove anything against.
    const std::vector<std::vector<unsigned char>> odd_items{
        RlpEncodeBytes(std::span<const unsigned char>{}),
        RlpEncodeBytes(std::vector<unsigned char>(31, 0x11)),
        RlpEncodeBytes(std::vector<unsigned char>(33, 0x11)),
        RlpEncodeList({RlpEncodeBytes(std::vector<unsigned char>(32, 0x11))}),
    };
    for (const auto& item : odd_items) {
        const auto header{MakeHeader(uint256{}, 7, RootFor(7), 0, item)};
        uint256 parent;
        const auto parsed{exec_detail::ParseHeader(header, parent)};
        BOOST_REQUIRE(parsed);
        BOOST_CHECK(parsed->state_root.IsNull());
        BOOST_CHECK(parsed->receipts_root == RootFor(7));
        BOOST_CHECK(VerifyExecAncestry(KeccakOf(header), 7, {header}));
    }
}

BOOST_AUTO_TEST_CASE(state_root_from_mainnet_execution_fields)
{
    // Ethereum mainnet block 25811248 (the receipts-fixture block), from the
    // captured light-client fixture. Its execution payload header fields are
    // verbatim mainnet data, SSZ-proven against the beacon block in
    // bridge_eth_light_client_tests. The full 21-field Prague/Fulu RLP header
    // is rebuilt from them, with parentBeaconBlockRoot = the beacon block's
    // parent_root. The fixture did not capture requestsHash (EIP-7685; it is
    // not in the payload header), so the empty-requests value stands in for
    // it and the rebuilt header's keccak is not the mainnet block hash. Every
    // field ParseHeader reads is mainnet data.
    using namespace eth_lc_fixture;
    const ExecHeaderHex& e{U2_FINALIZED_EXEC};
    const BeaconHeaderHex& b{U2_FINALIZED_BEACON};
    auto hex = [](const std::string& h) {
        auto v{TryParseHex<unsigned char>(h)};
        BOOST_REQUIRE(v.has_value());
        return *v;
    };
    auto raw = [&](const std::string& h) {
        const auto v{hex(h)};
        BOOST_REQUIRE_EQUAL(v.size(), 32U);
        return uint256{v};
    };
    const auto base_fee_le{hex(e.base_fee_le)};
    uint64_t base_fee{0};
    for (size_t i = 0; i < base_fee_le.size(); ++i) {
        if (i >= 8) {
            BOOST_REQUIRE_EQUAL(base_fee_le[i], 0);
            continue;
        }
        base_fee |= static_cast<uint64_t>(base_fee_le[i]) << (8 * i);
    }
    unsigned char empty_requests[CSHA256::OUTPUT_SIZE];
    CSHA256().Finalize(empty_requests);
    const std::vector<std::vector<unsigned char>> fields{
        RlpEncodeBytes(hex(e.parent_hash)),
        RlpEncodeBytes(hex("1dcc4de8dec75d7aab85b567b6ccd41ad312451b948a7413f0a142fd40d49347")), // ommersHash
        RlpEncodeBytes(hex(e.fee_recipient)),
        RlpEncodeBytes(hex(e.state_root)),
        RlpEncodeBytes(hex(e.transactions_root)),
        RlpEncodeBytes(hex(e.receipts_root)),
        RlpEncodeBytes(hex(e.logs_bloom)),
        RlpEncodeUint64(0), // difficulty
        RlpEncodeUint64(e.block_number),
        RlpEncodeUint64(e.gas_limit),
        RlpEncodeUint64(e.gas_used),
        RlpEncodeUint64(e.timestamp),
        RlpEncodeBytes(hex(e.extra_data)),
        RlpEncodeBytes(hex(e.prev_randao)), // mixHash
        RlpEncodeBytes(std::vector<unsigned char>(8, 0x00)), // nonce
        RlpEncodeUint64(base_fee),
        RlpEncodeBytes(hex(e.withdrawals_root)),
        RlpEncodeUint64(e.blob_gas_used),
        RlpEncodeUint64(e.excess_blob_gas),
        RlpEncodeBytes(hex(b.parent_root)), // parentBeaconBlockRoot
        RlpEncodeBytes(std::span<const unsigned char>{empty_requests}), // requestsHash stand-in
    };
    const auto header{RlpEncodeList(fields)};
    uint256 parent;
    const auto parsed{exec_detail::ParseHeader(header, parent)};
    BOOST_REQUIRE(parsed);
    BOOST_CHECK_EQUAL(parsed->block_number, eth_receipts_fixture::BLOCK_NUMBER);
    BOOST_CHECK(parent == raw(e.parent_hash));
    BOOST_CHECK(parsed->state_root == raw(e.state_root));
    BOOST_CHECK(parsed->state_root ==
                raw("60bfc2e5324a94442dc0af36abc9901d1850c7434548bbcc6c59b7d1b275b01a"));
    BOOST_CHECK(parsed->receipts_root == raw(eth_receipts_fixture::RECEIPTS_ROOT_HEX));
    BOOST_CHECK(parsed->state_root != parsed->receipts_root);
    BOOST_CHECK(parsed->block_hash == KeccakOf(header));
}

BOOST_AUTO_TEST_CASE(relayer_plan_chunks_with_one_header_overlap)
{
    const auto [chain, tip]{MakeChain(100, 164)}; // 65 headers
    const auto plan{PlanBridgeExecutionAncestryV1(tip, 100, chain)};
    BOOST_REQUIRE(plan);
    BOOST_REQUIRE_EQUAL(plan->backfills.size(), 2U);

    uint256 source{tip};
    for (const auto& step : plan->backfills) {
        BOOST_CHECK(step.record.finalized_anchor_hash == source);
        BOOST_CHECK_LE(step.record.ancestry_headers.size(),
                       MAX_BRIDGE_EXECUTION_ANCESTRY_HEADERS);
        const auto target{VerifyExecAncestry(
            source, step.record.target_block_number,
            step.record.ancestry_headers)};
        BOOST_REQUIRE(target);
        BOOST_CHECK(target->block_hash == step.target_block_hash);
        const auto encoded{EncodeBridgeRecordV1(
            {BridgeRecordKindV1::EXECUTION_BACKFILL, step.record})};
        BOOST_REQUIRE(encoded);
        source = step.target_block_hash;
    }
    BOOST_CHECK(plan->mint_anchor_hash == source);
    BOOST_REQUIRE_EQUAL(plan->mint_ancestry_headers.size(), 3U);
    BOOST_REQUIRE(VerifyExecAncestry(plan->mint_anchor_hash, 100,
                                    plan->mint_ancestry_headers));

    // Boundary header 133 is both the first backfill's target and the next
    // backfill's source preimage. Header 102 repeats at the mint boundary.
    BOOST_CHECK(plan->backfills[0].record.ancestry_headers.back() ==
                plan->backfills[1].record.ancestry_headers.front());
    BOOST_CHECK(plan->backfills[1].record.ancestry_headers.back() ==
                plan->mint_ancestry_headers.front());

    // A bootstrap/update may already have retained the exact header where a
    // naive 32-header split would land. Begin at the oldest retained anchor;
    // submitting the otherwise-planned 164 -> 133 backfill would be a no-op.
    const uint256 retained_133{KeccakOf(chain[31])};
    const auto with_retained{PlanBridgeExecutionAncestryV1(
        tip, 100, chain, std::set<uint256>{retained_133})};
    BOOST_REQUIRE(with_retained);
    BOOST_REQUIRE_EQUAL(with_retained->backfills.size(), 1U);
    BOOST_CHECK(with_retained->backfills.front().record.finalized_anchor_hash ==
                retained_133);

    const uint256 retained_102{KeccakOf(chain[62])};
    const auto near_target{PlanBridgeExecutionAncestryV1(
        tip, 100, chain,
        std::set<uint256>{retained_133, retained_102})};
    BOOST_REQUIRE(near_target);
    BOOST_CHECK(near_target->backfills.empty());
    BOOST_CHECK(near_target->mint_anchor_hash == retained_102);
}

BOOST_AUTO_TEST_CASE(relayer_plan_honors_byte_limit_and_rejects_bad_input)
{
    const auto [wide, tip]{MakeChain(200, 239, 400)};
    const auto plan{PlanBridgeExecutionAncestryV1(tip, 200, wide)};
    BOOST_REQUIRE(plan);
    BOOST_REQUIRE(!plan->backfills.empty());
    for (const auto& step : plan->backfills) {
        size_t bytes{0};
        for (const auto& header : step.record.ancestry_headers) {
            bytes += header.size();
        }
        BOOST_CHECK_LE(bytes, MAX_BRIDGE_EXECUTION_ANCESTRY_BYTES);
        BOOST_CHECK_LT(step.record.ancestry_headers.size(),
                       MAX_BRIDGE_EXECUTION_ANCESTRY_HEADERS);
    }

    auto oversized{wide};
    oversized.front().resize(MAX_BRIDGE_RLP_ITEM_SIZE + 1);
    BOOST_CHECK(!PlanBridgeExecutionAncestryV1(tip, 200, oversized));

    const std::vector<std::vector<unsigned char>> too_deep(
        MAX_BRIDGE_CUMULATIVE_BACKFILL_BLOCKS + 2, wide.front());
    BOOST_CHECK(!PlanBridgeExecutionAncestryV1(tip, 200, too_deep));

    uint256 wrong{tip};
    *wrong.begin() ^= 1;
    BOOST_CHECK(!PlanBridgeExecutionAncestryV1(wrong, 200, wide));
}

BOOST_AUTO_TEST_CASE(retained_source_is_exact_and_cumulatively_bounded)
{
    const auto [chain, tip]{MakeChain(100, 110)};
    BridgeRetainedExecutionSourceV1 source{
        110, tip, RootFor(110), 115};
    BOOST_CHECK(VerifyRetainedExecutionSourceV1(source, 100, chain));

    auto bad{source};
    bad.receipts_root = RootFor(109);
    BOOST_CHECK(!VerifyRetainedExecutionSourceV1(bad, 100, chain));

    bad = source;
    bad.block_number = 109;
    BOOST_CHECK(!VerifyRetainedExecutionSourceV1(bad, 100, chain));

    bad = source;
    bad.source_finalized_execution_block =
        100 + MAX_BRIDGE_CUMULATIVE_BACKFILL_BLOCKS + 1;
    BOOST_CHECK(!VerifyRetainedExecutionSourceV1(bad, 100, chain));
}

BOOST_AUTO_TEST_SUITE_END()
