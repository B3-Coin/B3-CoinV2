// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** fm2-sim: the deterministic simulator CLI of the rebirth outline (slice
 * R14; ARCHITECTURE.md section 14).
 *
 *   fm2-sim [--scenario <name>|all] [--seed <n> | --seeds <a>-<b>]
 *
 * Runs every selected walking-skeleton variant (walking_skeleton,
 * walking_skeleton_storage_fault, walking_skeleton_authority_fault; default
 * all) for every seed (default 1-32), each seed twice (P6), and prints one
 * JSON document (sorted keys; no times, paths or pointers) on stdout.
 *
 * Exit status: 0 iff every check of the required manifest ran and passed
 * and every run passed its own manifest; 1 otherwise; 2 for a usage error.
 *
 * REGTEST and TEST values only: no network, datadir or real key is touched.
 * Not a latency benchmark (PERF-6).
 */

#include <flowmesh2/sim/scenario.h>
#include <key.h>
#include <util/translation.h>

#include <charconv>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// This headless executable uses untranslated messages.
const TranslateFn G_TRANSLATION_FUN{nullptr};

namespace {

std::optional<uint64_t> ParseU64(std::string_view text)
{
    uint64_t value{0};
    const auto [end, error]{std::from_chars(text.data(), text.data() + text.size(), value)};
    if (error != std::errc{} || end != text.data() + text.size() || text.empty()) return std::nullopt;
    return value;
}

int Usage(std::string_view problem)
{
    std::cerr << "fm2-sim: " << problem << "\n"
              << "usage: fm2-sim [--scenario <walking_skeleton|walking_skeleton_storage_fault|walking_skeleton_authority_fault|all>]"
              << " [--seed <n> | --seeds <a>-<b>]\n";
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    using namespace flowmesh2::sim;
    std::vector<Variant> variants{AllVariants().begin(), AllVariants().end()};
    uint64_t seed_from{1};
    uint64_t seed_to{32};
    for (int i{1}; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--help" || arg == "-h") {
            Usage("help");
            return 0;
        }
        if (i + 1 >= argc) return Usage("missing value for " + std::string{arg});
        const std::string_view value{argv[++i]};
        if (arg == "--scenario") {
            if (value == "all") {
                variants.assign(AllVariants().begin(), AllVariants().end());
            } else if (const std::optional<Variant> variant{VariantFromName(value)}) {
                variants = {*variant};
            } else {
                return Usage("unknown scenario " + std::string{value});
            }
        } else if (arg == "--seed") {
            const std::optional<uint64_t> seed{ParseU64(value)};
            if (!seed) return Usage("bad seed " + std::string{value});
            seed_from = seed_to = *seed;
        } else if (arg == "--seeds") {
            const size_t dash{value.find('-')};
            if (dash == std::string_view::npos) return Usage("bad seed range " + std::string{value});
            const std::optional<uint64_t> from{ParseU64(value.substr(0, dash))};
            const std::optional<uint64_t> to{ParseU64(value.substr(dash + 1))};
            if (!from || !to || *from > *to) return Usage("bad seed range " + std::string{value});
            seed_from = *from;
            seed_to = *to;
        } else {
            return Usage("unknown option " + std::string{arg});
        }
    }

    // The clients sign with CKey (TEST keys only).
    ECC_Context ecc_context;
    const SuiteReport report{RunSuite(variants, seed_from, seed_to)};
    std::cout << SuiteJson(report);
    std::cout.flush();
    return report.ok ? 0 : 1;
}
