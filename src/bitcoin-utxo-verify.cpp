// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * b3coin-utxo-verify: standalone, operator-facing U == U' verification.
 *
 * Compares the UTXO set of a fully-validated legacy chainstate, stopped at a
 * chosen final legacy block (H, X), against a fresh trusted-replay
 * reconstruction of the same chain from genesis through that exact block, and
 * reports canonical commitments, counts, and bounded per-outpoint mismatch
 * diagnostics. Exit status: 0 only if the two sets are identical and every
 * verification passed; 1 on any difference or verification failure; 2 on
 * usage or environment errors.
 *
 * The tool is read-only by intent: it never issues a database write to the
 * node's chainstate or block index, refuses to run unless both databases
 * already exist, and stages the replay reconstruction in a disposable work
 * directory of its own. Run it against a cleanly-stopped node (e.g. one
 * synced with -stopatheight=H); LevelDB itself may touch housekeeping files
 * (LOCK/LOG) on open, which is inherent to opening any LevelDB store.
 *
 * This lives entirely outside consensus and outside node startup.
 */

#include <chainparams.h>
#include <coins.h>
#include <consensus/params.h>
#include <dbwrapper.h>
#include <kernel/cs_main.h>
#include <legacy/codec.h>
#include <node/blockstorage.h>
#include <node/utxo_equivalence_check.h>
#include <primitives/block.h>
#include <streams.h>
#include <sync.h>
#include <tinyformat.h>
#include <txdb.h>
#include <uint256.h>
#include <util/fs.h>
#include <util/hasher.h>
#include <util/obfuscation.h>
#include <util/signalinterrupt.h>
#include <util/strencodings.h>

#include <array>
#include <cstdio>
#include <iostream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

void Usage()
{
    tfm::format(std::cerr,
                "Usage: b3coin-utxo-verify -datadir=<dir> -height=<H> -hash=<X> [options]\n"
                "\n"
                "Verify that a trusted replay of the legacy chain from genesis through the\n"
                "block <X> at height <H> reconstructs exactly the UTXO set of the node's\n"
                "fully-validated chainstate at that same block.\n"
                "\n"
                "The node must be STOPPED, and must have been stopped with its tip exactly\n"
                "at <H> (for example by syncing with -stopatheight=<H>).\n"
                "\n"
                "Options:\n"
                "  -datadir=<dir>         node data directory (with chainstate/ and blocks/)\n"
                "  -height=<H>            the final legacy height under verification\n"
                "  -hash=<X>              the exact block hash at height <H>\n"
                "  -workdir=<dir>         scratch directory for the replay reconstruction\n"
                "                         (default: <datadir>/utxo-verify.tmp; created fresh)\n"
                "  -max-mismatches=<n>    mismatch diagnostics to print (default: 20)\n");
}

struct ToolArgs {
    fs::path datadir;
    int height{-1};
    uint256 hash{};
    fs::path workdir;
    size_t max_mismatches{20};
};

std::optional<ToolArgs> ParseArgs(const int argc, char* argv[])
{
    ToolArgs args;
    for (int i{1}; i < argc; ++i) {
        const std::string arg{argv[i]};
        const auto eat{[&arg](const std::string& name) -> std::optional<std::string> {
            if (arg.rfind(name, 0) == 0) return arg.substr(name.size());
            return std::nullopt;
        }};
        if (const auto v{eat("-datadir=")}) {
            args.datadir = fs::PathFromString(*v);
        } else if (const auto v{eat("-height=")}) {
            if (const auto h{ToIntegral<int>(*v)}) args.height = *h;
        } else if (const auto v{eat("-hash=")}) {
            if (const auto h{uint256::FromHex(*v)}) args.hash = *h;
        } else if (const auto v{eat("-workdir=")}) {
            args.workdir = fs::PathFromString(*v);
        } else if (const auto v{eat("-max-mismatches=")}) {
            if (const auto n{ToIntegral<size_t>(*v)}) args.max_mismatches = *n;
        } else {
            tfm::format(std::cerr, "Unknown argument: %s\n", arg);
            return std::nullopt;
        }
    }
    if (args.datadir.empty() || args.height < 0 || args.hash.IsNull()) return std::nullopt;
    if (args.workdir.empty()) args.workdir = args.datadir / "utxo-verify.tmp";
    return args;
}

//! Describe one side of a mismatch for the report.
std::string DescribeSide(const std::optional<Coin>& coin)
{
    if (!coin) return "absent";
    return strprintf("value=%d height=%u coinbase=%d coinstake=%d ntime=%u",
                     coin->out.nValue, unsigned{coin->nHeight}, coin->fCoinBase ? 1 : 0,
                     coin->fCoinStake ? 1 : 0, coin->nTime);
}

} // namespace

int main(int argc, char* argv[])
{
    const auto args{ParseArgs(argc, argv)};
    if (!args) {
        Usage();
        return 2;
    }

    const fs::path chainstate_dir{args->datadir / "chainstate"};
    const fs::path blocks_dir{args->datadir / "blocks"};
    const fs::path index_dir{blocks_dir / "index"};
    // Refuse to run against anything that is not an existing database: opening
    // a missing LevelDB path would create one, and this tool must never write
    // into the node's data directory.
    if (!fs::exists(chainstate_dir / "CURRENT") || !fs::exists(index_dir / "CURRENT")) {
        tfm::format(std::cerr,
                    "error: %s does not look like a synced node datadir "
                    "(missing chainstate/ or blocks/index/ database)\n",
                    fs::PathToString(args->datadir));
        return 2;
    }

    const auto chainparams{CChainParams::Main()};
    const Consensus::Params& consensus{chainparams->GetConsensus()};

    try {
        LOCK(::cs_main);

        // ---- Block index: load read-only into a local map and walk the
        // X-anchored chain back to genesis.
        kernel::BlockTreeDB block_index_db{DBParams{.path = index_dir, .cache_bytes = size_t{8} << 20}};
        std::unordered_map<uint256, CBlockIndex, BlockHasher> index;
        const auto insert{[&index](const uint256& hash) -> CBlockIndex* {
            if (hash.IsNull()) return nullptr;
            const auto [it, inserted]{index.try_emplace(hash)};
            if (inserted) it->second.phashBlock = &it->first;
            return &it->second;
        }};
        const util::SignalInterrupt interrupt;
        if (!block_index_db.LoadBlockIndexGuts(consensus, insert, interrupt)) {
            tfm::format(std::cerr, "error: failed to load the block index\n");
            return 2;
        }

        const auto tip_it{index.find(args->hash)};
        if (tip_it == index.end()) {
            tfm::format(std::cerr, "error: block %s is not in this node's block index\n",
                        args->hash.ToString());
            return 1;
        }
        const CBlockIndex* tip{&tip_it->second};
        if (tip->nHeight != args->height) {
            tfm::format(std::cerr, "error: block %s is at height %d, not the expected height %d\n",
                        args->hash.ToString(), tip->nHeight, args->height);
            return 1;
        }

        std::vector<const CBlockIndex*> chain(static_cast<size_t>(args->height) + 1, nullptr);
        for (const CBlockIndex* p{tip}; p != nullptr; p = p->pprev) {
            if (p->nHeight < 0 || p->nHeight > args->height) break;
            chain[static_cast<size_t>(p->nHeight)] = p;
        }
        for (int h{0}; h <= args->height; ++h) {
            const CBlockIndex* p{chain[static_cast<size_t>(h)]};
            if (!p) {
                tfm::format(std::cerr, "error: broken index linkage below %s at height %d\n",
                            args->hash.ToString(), h);
                return 1;
            }
            if (!(p->nStatus & BLOCK_HAVE_DATA)) {
                tfm::format(std::cerr,
                            "error: no block data at height %d (pruned datadir?); a full "
                            "unpruned datadir is required\n", h);
                return 1;
            }
        }
        if (chain[0]->GetBlockHash() != consensus.hashGenesisBlock) {
            tfm::format(std::cerr, "error: the chain under %s does not descend from the B3 genesis\n",
                        args->hash.ToString());
            return 1;
        }

        // ---- Raw block reader: the stored xor key (never created here), the
        // recorded file positions, and a re-check that the bytes hash to the
        // indexed identity.
        std::array<std::byte, Obfuscation::KEY_SIZE> key_bytes{};
        const fs::path xor_path{blocks_dir / "xor.dat"};
        if (fs::exists(xor_path)) {
            AutoFile key_file{fsbridge::fopen(xor_path, "rb")};
            key_file >> key_bytes;
        }
        const Obfuscation obfuscation{key_bytes};
        const auto read_block{[&](const int height) -> std::optional<CBlock> {
            AssertLockHeld(::cs_main);
            const CBlockIndex* pindex{chain[static_cast<size_t>(height)]};
            AutoFile file{fsbridge::fopen(blocks_dir / fs::u8path(strprintf("blk%05u.dat", pindex->nFile)), "rb")};
            if (file.IsNull()) return std::nullopt;
            file.SetObfuscation(obfuscation);
            file.seek(pindex->nDataPos, SEEK_SET);
            CBlock block;
            try {
                file >> legacy::TX_LEGACY(block);
            } catch (const std::exception&) {
                return std::nullopt;
            }
            if (block.GetMarkerHash(consensus) != pindex->GetBlockHash()) return std::nullopt;
            return block;
        }};

        // ---- The two views: the node's chainstate (opened to read; no write
        // is ever issued) and a fresh scratch database for the reconstruction.
        CCoinsViewDB live{DBParams{.path = chainstate_dir, .cache_bytes = size_t{64} << 20},
                          CoinsViewOptions{}};
        fs::create_directories(args->workdir);
        CCoinsViewDB scratch{DBParams{.path = args->workdir / "replay-scratch",
                                      .cache_bytes = size_t{64} << 20,
                                      .wipe_data = true},
                             CoinsViewOptions{}};

        const node::ReplayEquivalenceResult result{node::VerifyReplayEquivalence(
            consensus, live, read_block, scratch,
            {.final_height = args->height, .final_hash = args->hash,
             .max_mismatch_sample = args->max_mismatches})};

        // ---- Report.
        for (const std::string& error : result.errors) {
            tfm::format(std::cerr, "error: %s\n", error);
        }
        tfm::format(std::cout, "blocks replayed:    %d\n", result.blocks_replayed);
        tfm::format(std::cout, "live UTXOs:         %d\n", result.live_count);
        tfm::format(std::cout, "replay UTXOs:       %d\n", result.replay_count);
        tfm::format(std::cout, "live commitment:    %s\n", result.live_commitment.ToString());
        tfm::format(std::cout, "replay commitment:  %s\n", result.replay_commitment.ToString());
        if (result.mismatch_total > 0) {
            tfm::format(std::cout, "mismatches:         %d (showing %d)\n", result.mismatch_total,
                        result.mismatch_sample.size());
            for (const auto& m : result.mismatch_sample) {
                tfm::format(std::cout, "  %s\n    live:   %s\n    replay: %s\n",
                            m.outpoint.ToString(), DescribeSide(m.in_a), DescribeSide(m.in_b));
            }
        }
        tfm::format(std::cout, "result:             %s\n", result.ok ? "EQUAL (U == U')" : "NOT EQUAL");
        return result.ok ? 0 : 1;
    } catch (const std::exception& e) {
        tfm::format(std::cerr, "error: %s\n", e.what());
        return 2;
    }
}
