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
#include <node/utxo_rows.h>
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
#include <fstream>
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
                "  -max-mismatches=<n>    mismatch diagnostics to print (default: 20)\n"
                "\n"
                "Three-way invariant (doc/design/b3-utxo-equivalence.md):\n"
                "  -portrows=<file>       write the live chainstate (U_port) as canonical rows\n"
                "  -replayrows=<file>     write the replay reconstruction (U_replay) as canonical rows\n"
                "  -masterrows=<file>     read the legacy master client's exported rows (U_master)\n"
                "                         and verify U_master == U_port == U_replay; with this\n"
                "                         option, exit 0 only when all three sets agree\n"
                "\n"
                "FN Proof-of-Disintegration report (doc/design/b3-fn-pod.md):\n"
                "  -podreport             derive every qualifying historical PoD during the\n"
                "                         replay pass and print the report. The QUALIFYING\n"
                "                         COUNT (R vs the 1,000 cap) is the pre-activation\n"
                "                         gate; the payload figures are a SUPERSEDED type-1\n"
                "                         diagnostic, NOT the type-2 issuance capacity gate\n"
                "                         (real type-2 proof sizes = future measurement)\n");
}

struct ToolArgs {
    fs::path datadir;
    int height{-1};
    uint256 hash{};
    fs::path workdir;
    size_t max_mismatches{20};
    fs::path portrows;
    fs::path replayrows;
    fs::path masterrows;
    bool podreport{false};
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
        } else if (const auto v{eat("-portrows=")}) {
            args.portrows = fs::PathFromString(*v);
        } else if (const auto v{eat("-replayrows=")}) {
            args.replayrows = fs::PathFromString(*v);
        } else if (arg == "-podreport") {
            args.podreport = true;
        } else if (const auto v{eat("-masterrows=")}) {
            args.masterrows = fs::PathFromString(*v);
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

//! One side of a row-level difference, printed as a full canonical row so
//! the exact divergent field is visible.
std::string DescribeRowSide(const COutPoint& outpoint, const std::optional<Coin>& coin)
{
    if (!coin) return "absent";
    return node::UtxoRowLine({outpoint, *coin});
}

//! Write one UTXO set as a canonical row file.
bool WriteRowsFile(const fs::path& path, std::vector<node::UtxoEntry> entries,
                   const uint256& tip_hash, const int tip_height)
{
    std::ofstream out{path.std_path(), std::ios::binary | std::ios::trunc};
    std::string error;
    if (!out || !node::WriteUtxoRows(out, {tip_hash, tip_height, std::move(entries)}, error)) {
        tfm::format(std::cerr, "error: cannot write rows to %s%s%s\n", fs::PathToString(path),
                    error.empty() ? "" : ": ", error);
        return false;
    }
    tfm::format(std::cout, "rows written:       %s\n", fs::PathToString(path));
    return true;
}

//! Print one pairwise comparison of the three-way check; returns equality.
bool ReportPair(const std::string& label_a, const std::string& label_b,
                node::UtxoComparison cmp, const size_t max_mismatches)
{
    tfm::format(std::cout, "%s vs %s:  %s (%d vs %d rows)\n", label_a, label_b,
                cmp.Equal() ? "EQUAL" : "NOT EQUAL", cmp.count_a, cmp.count_b);
    const size_t shown{std::min(max_mismatches, cmp.mismatches.size())};
    if (!cmp.mismatches.empty()) {
        tfm::format(std::cout, "  differing rows: %d (showing %d)\n", cmp.mismatches.size(), shown);
    }
    for (size_t i{0}; i < shown; ++i) {
        const auto& m{cmp.mismatches[i]};
        tfm::format(std::cout, "  %s\n    %s: %s\n    %s: %s\n", m.outpoint.ToString(),
                    label_a, DescribeRowSide(m.outpoint, m.in_a),
                    label_b, DescribeRowSide(m.outpoint, m.in_b));
    }
    return cmp.Equal();
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
        const auto read_block{[&](const int height) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) -> std::optional<CBlock> {
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
             .max_mismatch_sample = args->max_mismatches,
             .derive_pod_report = args->podreport})};

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
        tfm::format(std::cout, "result:             %s\n",
                    result.ok ? "EQUAL (U_port == U_replay)" : "NOT EQUAL");

        // ---- Historical PoD report (doc/design/b3-fn-pod.md §8.4);
        // payload portion superseded/non-authoritative.
        if (result.pod_report) {
            const node::PodCapacityReport& pod{*result.pod_report};
            tfm::format(std::cout, "PoD qualifying:     %d\n", pod.total_qualifying);
            tfm::format(std::cout, "PoD claimable:      %d\n", pod.claimable);
            for (const auto& [reason, count] : pod.by_reason) {
                tfm::format(std::cout, "PoD reason %d:       %d (%s)\n", reason, count,
                            reason == 0 ? "SUPPORTED" : "UNSUPPORTED_FUNDING_SCRIPT");
            }
            tfm::format(std::cout, "PoD max scripts:    %d (largest eligible claim)\n",
                        pod.max_distinct_funding_scripts);
            tfm::format(std::cout, "PoD max action:     %d bytes (SUPERSEDED: worst-case payload of the ABANDONED type-1 FnClaimActionV1)\n",
                        pod.max_action_payload);
            tfm::format(std::cout, "PoD within 4000:    %d (superseded type-1 arithmetic)\n",
                        pod.within_native_bound);
            tfm::format(std::cout, "PoD exceeding 4000: %d (superseded type-1 arithmetic)\n",
                        pod.exceeding_native_bound);
            tfm::format(std::cout, "PoD native fit:     %s\n",
                        pod.fits_native_action
                            ? "yes (SUPERSEDED type-1 verdict; NOT the type-2 issuance capacity gate)"
                            : "NO (superseded type-1 verdict)");
            tfm::format(std::cout,
                        "NOTE: the payload figures above measure the ABANDONED funding-signature\n"
                        "claim encoding and are NON-AUTHORITATIVE for activation. Real encoded\n"
                        "LegacyFnIssuanceActionV1 (type-2) proof sizes over actual history are\n"
                        "UNMEASURED future work; FN activation remains blocked until that\n"
                        "measurement exists or a reviewed versioned carrier is selected.\n");
            for (const node::PodRecord& record : result.pod_records) {
                tfm::format(std::cout,
                            "  pod %s h=%d gap=%d tier=%d scripts=%d claimable=%s markers=%d\n",
                            record.pod_id.ToString(), record.height, record.disintegrated,
                            record.tier, record.funding_scripts.size(),
                            record.claimable ? "yes" : "no", record.marker_vouts.size());
            }
        }

        // ---- Canonical row export and the three-way invariant
        // U_master == U_port == U_replay (doc/design/b3-utxo-equivalence.md).
        // Only meaningful once the two-way pipeline itself ran to completion;
        // row export deliberately proceeds on a row MISMATCH (result.ok
        // false), which is exactly when the rows are needed for diagnosis.
        if (!result.errors.empty()) {
            if (!args->portrows.empty() || !args->replayrows.empty() || !args->masterrows.empty()) {
                tfm::format(std::cerr,
                            "error: skipping row export/comparison: the verification pipeline "
                            "did not complete\n");
            }
            return 1;
        }

        if (!args->portrows.empty() &&
            !WriteRowsFile(args->portrows, node::EnumerateUtxos(live), args->hash, args->height)) {
            return 2;
        }
        if (!args->replayrows.empty() &&
            !WriteRowsFile(args->replayrows, node::EnumerateUtxos(scratch), args->hash, args->height)) {
            return 2;
        }

        if (!args->masterrows.empty()) {
            std::ifstream in{args->masterrows.std_path(), std::ios::binary};
            if (!in) {
                tfm::format(std::cerr, "error: cannot open %s\n", fs::PathToString(args->masterrows));
                return 2;
            }
            node::UtxoRowsFile master;
            std::string error;
            if (!node::ReadUtxoRows(in, master, error)) {
                tfm::format(std::cerr, "error: %s: %s\n", fs::PathToString(args->masterrows), error);
                return 2;
            }
            if (master.tip_hash != args->hash || master.tip_height != args->height) {
                tfm::format(std::cerr,
                            "error: master rows were captured at %s (height %d), not the "
                            "requested block %s (height %d)\n",
                            master.tip_hash.ToString(), master.tip_height,
                            args->hash.ToString(), args->height);
                return 2;
            }

            const std::vector<node::UtxoEntry> port_entries{node::EnumerateUtxos(live)};
            const std::vector<node::UtxoEntry> replay_entries{node::EnumerateUtxos(scratch)};
            tfm::format(std::cout, "master rows:        %d\n", master.entries.size());
            tfm::format(std::cout, "master commitment:  %s\n",
                        node::UtxoSetCommitment(master.entries).ToString());
            const bool master_port{ReportPair("master", "port",
                node::CompareUtxoSets(master.entries, port_entries), args->max_mismatches)};
            const bool master_replay{ReportPair("master", "replay",
                node::CompareUtxoSets(master.entries, replay_entries), args->max_mismatches)};
            const bool three_way{master_port && master_replay && result.ok};
            tfm::format(std::cout, "three-way result:   %s\n",
                        three_way ? "EQUAL (U_master == U_port == U_replay)" : "NOT EQUAL");
            return three_way ? 0 : 1;
        }

        return result.ok ? 0 : 1;
    } catch (const std::exception& e) {
        tfm::format(std::cerr, "error: %s\n", e.what());
        return 2;
    }
}
