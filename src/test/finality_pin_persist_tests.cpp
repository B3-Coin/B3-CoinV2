// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// Commit 14A of the Modern PoS V1 finality plan: the persisted finality pin.
// Once (height, hash) is final it survives restart, -reindex-chainstate and
// the removal of its certificate carrier by an allowed reorganization; the
// file is monotone and atomic; a lower chain-derived finalized height never
// overwrites a higher persisted pin.

#include <chain.h>
#include <chainparams.h>
#include <node/blockstorage.h>
#include <node/finality_pin.h>
#include <node/finality_tracker.h>
#include <test/util/finality_fixture.h>
#include <test/util/setup_common.h>
#include <util/fs.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <cstdio>
#include <vector>

using b3test::FinalityChainDiskFixture;

namespace {

std::optional<std::pair<int, uint256>> Anchor(const node::NodeContext& node)
{
    LOCK(cs_main);
    return node.chainman->m_blockman.FinalityAnchor();
}

} // namespace

BOOST_AUTO_TEST_SUITE(finality_pin_persist_tests)

BOOST_FIXTURE_TEST_CASE(pin_file_is_monotone_atomic_and_network_bound, BasicTestingSetup)
{
    const fs::path path{m_args.GetDataDirBase() / "finality_pin.dat"};
    const MessageStartChars magic{Params().MessageStart()};
    BOOST_CHECK(!node::ReadFinalityPin(path, magic).has_value()); // absent
    const node::FinalityPin p10{10, uint256::ONE};
    BOOST_REQUIRE(node::WriteFinalityPin(path, magic, p10));
    BOOST_CHECK(*node::ReadFinalityPin(path, magic) == p10);
    // Lower: refused (still true, nothing written); equal: no-op; higher: written.
    uint256 h5{}; h5.begin()[0] = 5;
    BOOST_CHECK(node::WriteFinalityPin(path, magic, {5, h5}));
    BOOST_CHECK(*node::ReadFinalityPin(path, magic) == p10);
    uint256 h10b{}; h10b.begin()[0] = 0xB;
    BOOST_CHECK(node::WriteFinalityPin(path, magic, {10, h10b}));
    BOOST_CHECK(*node::ReadFinalityPin(path, magic) == p10);
    uint256 h12{}; h12.begin()[0] = 12;
    BOOST_CHECK(node::WriteFinalityPin(path, magic, {12, h12}));
    BOOST_CHECK((*node::ReadFinalityPin(path, magic) == node::FinalityPin{12, h12}));
    // Invalid pins are never written.
    BOOST_CHECK(!node::WriteFinalityPin(path, magic, {-1, h12}));
    BOOST_CHECK(!node::WriteFinalityPin(path, magic, {13, uint256{}}));
    // Another network's magic: ignored (and a write for it does not see ours).
    MessageStartChars other{magic};
    other[0] ^= 0xFF;
    BOOST_CHECK(!node::ReadFinalityPin(path, other).has_value());
    // Corruption: flip a byte of the stored hash -> checksum mismatch. FAIL
    // CLOSED: the tri-state read reports INVALID with a reason, and no write
    // ever silently replaces an invalid file.
    {
        FILE* f{fsbridge::fopen(path, "r+b")};
        BOOST_REQUIRE(f != nullptr);
        BOOST_REQUIRE(std::fseek(f, 4 + 1 + 4 + 3, SEEK_SET) == 0);
        const unsigned char c{0x7f};
        BOOST_REQUIRE(std::fwrite(&c, 1, 1, f) == 1);
        std::fclose(f);
    }
    {
        node::FinalityPin out;
        std::string error;
        BOOST_CHECK(node::ReadFinalityPinFile(path, magic, out, error) == node::FinalityPinFileStatus::INVALID);
        BOOST_CHECK(!error.empty());
    }
    BOOST_CHECK(!node::ReadFinalityPin(path, magic).has_value());
    BOOST_CHECK(!node::WriteFinalityPin(path, magic, {12, h12}));
    BOOST_CHECK(!node::WriteFinalityPin(path, magic, {100, h12}));
    // Only a deliberate operator removal reopens the path.
    fs::remove(path);
    {
        node::FinalityPin out;
        std::string error;
        BOOST_CHECK(node::ReadFinalityPinFile(path, magic, out, error) == node::FinalityPinFileStatus::ABSENT);
    }
    BOOST_CHECK(node::WriteFinalityPin(path, magic, {12, h12}));
    BOOST_CHECK((*node::ReadFinalityPin(path, magic) == node::FinalityPin{12, h12}));
}

BOOST_FIXTURE_TEST_CASE(pin_survives_carrier_reorg_restart_and_reindex, FinalityChainDiskFixture)
{
    PrepareFinalityChain();
    const int M{m_M};
    const fs::path pin_path{m_node.chainman->m_blockman.FinalityPinPath()};
    const MessageStartChars magic{Params().MessageStart()};
    BOOST_CHECK(!fs::exists(pin_path));

    // 1. Finalize M+5 (carrier M+8) then M+10 (carrier M+13); the file is
    //    written synchronously at each raise (crash-safe by construction).
    ProduceTo(M + 7, m_vk_a);
    const auto set0{*FinalityState().current};
    const uint256 next_hash{FinalityState().next->SetHash()};
    Produce(m_vk_a, {MakeCertificate({M + 5, 0, next_hash}, set0)});
    BOOST_REQUIRE(fs::exists(pin_path));
    BOOST_CHECK((*node::ReadFinalityPin(pin_path, magic) == node::FinalityPin{M + 5, ChainHashAt(M + 5)}));
    ProduceTo(M + 12, m_vk_a);
    Produce(m_vk_a, {MakeCertificate({M + 10, 0, next_hash}, set0)});
    ProduceTo(M + 15, m_vk_a);
    const uint256 pin_hash{ChainHashAt(M + 10)};
    BOOST_CHECK((*node::ReadFinalityPin(pin_path, magic) == node::FinalityPin{M + 10, pin_hash}));

    // 2. Reorg away the carrier (M+13) with a branch forking AT the pin (M+10):
    //    allowed; the derived finalized state disappears, the file does not move.
    {
        const CBlockIndex* parent{IndexAt(M + 10)};
        uint256 seed{SeedFor(parent)};
        for (int h{M + 11}; h <= M + 16; ++h) {
            auto [side, digest] = BuildPosBlockOnSeed(parent, seed, m_vk_a, {}, {}, /*extra=*/100 + h);
            if (side.GetBlockTime() > GetTime()) SetMockTime(side.GetBlockTime());
            BOOST_REQUIRE(Submit(side));
            parent = WITH_LOCK(cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(side.GetHash()));
            seed = digest;
        }
    }
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, M + 16);
    // The chain-derived finalized height fell back to M+5 (carrier M+8 is
    // still on the chain); the pin stays at M+10.
    BOOST_CHECK_EQUAL(FinalityState().finalized->height, M + 5);
    BOOST_CHECK_EQUAL(Anchor(m_node)->first, M + 10);
    BOOST_CHECK_EQUAL(ChainHashAt(M + 10).GetHex(), pin_hash.GetHex());
    BOOST_CHECK((*node::ReadFinalityPin(pin_path, magic) == node::FinalityPin{M + 10, pin_hash}));
    const int height_before{Tip()->nHeight};

    const auto restart{[&](const bool flush, const bool reindex_chainstate) {
        if (flush) {
            LOCK(cs_main);
            m_node.chainman->ActiveChainstate().ForceFlushStateToDisk();
        }
        m_node.chainman.reset();
        if (reindex_chainstate) m_args.ForceSetArg("-reindex-chainstate", "1");
        m_make_chainman();
        LoadVerifyActivateChainstate();
        if (reindex_chainstate) m_args.ForceSetArg("-reindex-chainstate", "0");
    }};
    const auto below_pin_refused{[&] {
        const CBlockIndex* parent{IndexAt(M + 9)};
        const auto [side, digest] = BuildPosBlockOnSeed(parent, SeedFor(parent), m_vk_a, {}, {}, /*extra=*/500 + Tip()->nHeight);
        BOOST_CHECK(!Submit(side));
        BOOST_CHECK(WITH_LOCK(cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(side.GetHash())) == nullptr);
    }};

    // 3./4./5. Restart: the pin is loaded from the file BEFORE any chain
    //    state is derived (the chain no longer carries the certificate), and a
    //    below-pin fork is refused.
    restart(/*flush=*/true, /*reindex_chainstate=*/false);
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, height_before);
    BOOST_REQUIRE(Anchor(m_node).has_value());
    BOOST_CHECK_EQUAL(Anchor(m_node)->first, M + 10);
    BOOST_CHECK_EQUAL(Anchor(m_node)->second.GetHex(), pin_hash.GetHex());
    BOOST_CHECK_EQUAL(FinalityState().finalized->height, M + 5); // lower derived state never lowers the pin
    BOOST_CHECK_EQUAL(Anchor(m_node)->first, M + 10);
    below_pin_refused();
    BOOST_CHECK_EQUAL(Tip()->nHeight, height_before);

    // Reindex-chainstate: same.
    restart(/*flush=*/true, /*reindex_chainstate=*/true);
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, height_before);
    BOOST_CHECK_EQUAL(Anchor(m_node)->first, M + 10);
    below_pin_refused();

    // 6. Re-deriving after reindex kept the lower derived height (M+5) and
    //    the higher pin (M+10); the file is unchanged.
    BOOST_CHECK_EQUAL(FinalityState().finalized->height, M + 5);
    BOOST_CHECK((*node::ReadFinalityPin(pin_path, magic) == node::FinalityPin{M + 10, pin_hash}));
    // 7. Monotonic advancement: certifying M+15 raises pin and file.
    ProduceTo(M + 18, m_vk_a);
    Produce(m_vk_a, {MakeCertificate({M + 15, 0, next_hash}, set0)});
    BOOST_CHECK_EQUAL(Anchor(m_node)->first, M + 15);
    BOOST_CHECK((*node::ReadFinalityPin(pin_path, magic) == node::FinalityPin{M + 15, ChainHashAt(M + 15)}));
    // Crash-style reload (no explicit flush): the file was written
    // synchronously at the raise, so the loaded pin is M+15 regardless of
    // what the databases had flushed.
    restart(/*flush=*/false, /*reindex_chainstate=*/false);
    BOOST_REQUIRE(Anchor(m_node).has_value());
    BOOST_CHECK_EQUAL(Anchor(m_node)->first, M + 15);

    // FAIL CLOSED at startup: a corrupt pin file refuses node construction
    // instead of silently running without the protection it recorded.
    std::vector<unsigned char> good_bytes;
    {
        FILE* f{fsbridge::fopen(pin_path, "rb")};
        BOOST_REQUIRE(f != nullptr);
        unsigned char buf[4096];
        const size_t n{std::fread(buf, 1, sizeof(buf), f)};
        std::fclose(f);
        good_bytes.assign(buf, buf + n);
    }
    {
        LOCK(cs_main);
        m_node.chainman->ActiveChainstate().ForceFlushStateToDisk();
    }
    m_node.chainman.reset();
    {
        FILE* f{fsbridge::fopen(pin_path, "r+b")};
        BOOST_REQUIRE(f != nullptr);
        BOOST_REQUIRE(std::fseek(f, 4 + 1 + 4 + 3, SEEK_SET) == 0);
        const unsigned char c{0x7f};
        BOOST_REQUIRE(std::fwrite(&c, 1, 1, f) == 1);
        std::fclose(f);
    }
    BOOST_CHECK_THROW(m_make_chainman(), std::runtime_error);
    m_node.chainman.reset();
    // Operator restores the file from backup: startup succeeds and the pin holds.
    {
        FILE* f{fsbridge::fopen(pin_path, "wb")};
        BOOST_REQUIRE(f != nullptr);
        BOOST_REQUIRE(std::fwrite(good_bytes.data(), 1, good_bytes.size(), f) == good_bytes.size());
        std::fclose(f);
    }
    m_make_chainman();
    LoadVerifyActivateChainstate();
    BOOST_REQUIRE(Anchor(m_node).has_value());
    BOOST_CHECK_EQUAL(Anchor(m_node)->first, M + 15);
}

BOOST_AUTO_TEST_SUITE_END()
