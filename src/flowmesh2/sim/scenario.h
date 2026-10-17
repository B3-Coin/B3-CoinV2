// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_SIM_SCENARIO_H
#define B3_FLOWMESH2_SIM_SCENARIO_H

#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/** The walking skeleton (slice R14; ARCHITECTURE.md section 14 is the
 * normative script and acceptance).
 *
 * RunWalkingSkeleton(seed, variant) builds a fresh one-node SimCluster, a
 * Scheduler(seed) and the clients alice, bob and carol, runs the variant's
 * script and returns a RunReport. The seed permutes only independent
 * submissions inside a step (different accounts) and their jitter; every
 * dependent submission is a later step that waits for the earlier reply,
 * and SealSlot runs only between steps, after RunUntilIdle. So the slot
 * contents, header hashes and final root do not depend on the seed, while
 * the admission order (and so the trace digest) may.
 *
 * Checks. Every check of 14.4 and 14.5 has the ID it has there (P1..P8,
 * N1..N13). A run records, per check it executes, whether every assertion
 * of that check held and the first failure. ManifestOf(variant) lists the
 * checks a run of that variant must execute; a manifest check that never
 * ran counts as failed (RunReport::Passed). P6 is a property of several
 * runs, so RunSuite evaluates it (SuiteChecks): for every variant and seed
 * the same seed run twice gives the same trace digest, every seed gives the
 * same header hashes and final state root, and with at least 8 seeds the
 * walking_skeleton variant shows at least two distinct trace digests (so a
 * seed that permutes nothing is caught). The union of the three manifests
 * and SuiteChecks() is exactly {P1..P8, N1..N13} (tested).
 *
 * A harness error (a component refusing where the script needs it to
 * succeed) stops the script; the report keeps the checks run so far and
 * names the error, and Passed() is false.
 *
 * SuiteJson renders a SuiteReport as JSON with sorted keys and no times,
 * paths or pointers: one (variants, seeds) input gives one byte string.
 */
namespace flowmesh2::sim {

//! The three walking-skeleton variants. APPEND-ONLY; zero is never a value.
enum class Variant : uint8_t {
    WALKING_SKELETON = 1,
    STORAGE_FAULT = 2,   //!< walking_skeleton_storage_fault (N10)
    AUTHORITY_FAULT = 3, //!< walking_skeleton_authority_fault (N11)
};

//! The CLI name: walking_skeleton, walking_skeleton_storage_fault, walking_skeleton_authority_fault.
std::string_view ToString(Variant variant);
std::optional<Variant> VariantFromName(std::string_view name);
std::span<const Variant> AllVariants();

//! P1..P8, N1..N13, in that order.
std::span<const std::string_view> AllChecks();
//! The checks one run of `variant` must execute and pass.
std::vector<std::string_view> ManifestOf(Variant variant);
//! The checks RunSuite evaluates over several runs: P6.
std::span<const std::string_view> SuiteChecks();

struct CheckResult {
    bool ran{false};
    bool passed{false}; //!< ran and every assertion held
    std::string detail; //!< the first failed assertion; empty when passed
    friend bool operator==(const CheckResult&, const CheckResult&) = default;
};
using CheckMap = std::map<std::string, CheckResult, std::less<>>;

struct RunReport {
    Variant variant{Variant::WALKING_SKELETON};
    uint64_t seed{0};
    CheckMap checks;                         //!< every manifest check (ran or not) and any other that ran
    std::string harness_error;               //!< non-empty if the script stopped early
    std::vector<uint256> headers;            //!< header hash of each committed slot, by height
    uint256 final_state_root;                //!< state root of the last committed head
    std::vector<std::string> canonical_order; //!< per committed slot: its originals' labels in body order, "," separated
    uint256 trace_digest;
    size_t trace_events{0};

    //! No harness error and every manifest check ran and passed.
    bool Passed() const;
};

RunReport RunWalkingSkeleton(uint64_t seed, Variant variant);

struct SuiteReport {
    std::vector<Variant> variants;
    uint64_t seed_from{0};
    uint64_t seed_to{0};
    std::vector<RunReport> runs; //!< one per (variant, seed), variants in input order, seeds ascending
    CheckMap suite;              //!< SuiteChecks()
    //! Every check of the required set (the variants' manifests and SuiteChecks): "pass", "fail" or "not_run".
    std::map<std::string, std::string, std::less<>> manifest;
    std::map<std::string, size_t, std::less<>> distinct_trace_digests; //!< per variant name
    bool ok{false};
};

/** P6 over the runs of one variant (ARCHITECTURE.md 14.4): first[i] and
 * second[i] are two runs of one seed. It fails unless the two runs of each
 * seed have the same trace digest, headers and final root, every seed has
 * the headers and final root of first[0], first and second have the same
 * size, and, when `need_distinct` and first holds at least 8 runs, at least
 * two of first's trace digests differ. Ran is true unless first is empty.
 */
CheckResult EvaluateP6(std::span<const RunReport> first, std::span<const RunReport> second, bool need_distinct);

/** Every variant of `variants` for every seed in [seed_from, seed_to], each
 * run twice (P6, EvaluateP6; need_distinct for walking_skeleton). Empty
 * variants or seed_from > seed_to give a report that is not ok.
 */
SuiteReport RunSuite(std::span<const Variant> variants, uint64_t seed_from, uint64_t seed_to);

//! Sorted-key JSON of `report`, ending in a newline.
std::string SuiteJson(const SuiteReport& report);

} // namespace flowmesh2::sim

#endif // B3_FLOWMESH2_SIM_SCENARIO_H
