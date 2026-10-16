#!/usr/bin/env python3
# Copyright (c) 2026 The B3 developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Self-test of check_guards.py: every rule fires on a planted fixture.

Run with `python3 -I -B` (stdlib only). CTest: flowmesh2_guards_selftest.

The harness builds a clean fake repository from guard_fixtures/clean/ plus
copies of the real C1 sources, the real src/flowmesh2/CMakeLists.txt and the
real core/result.h, core/codes.h and core/codes.cpp, and checks that the
guard passes it. Then, for every scenario, it copies the clean tree, plants
exactly one violation and requires that the guard exits 1 with a finding for
the scenario's rule (`r<N>_...`) and for no other rule. Scenarios come from
three sources:

- the overlays in guard_fixtures/planted/;
- mutations written here (C1 pins, TODO.md states, CMake links);
- one generated scenario per entry of this file's own copies of the rule 1
  and rule 2 lists, so dropping an entry from the guard fails this test.

Rule 3 and the build half of rule 4 use real objects: guard_fixtures/nm/
nm_fixture.cpp.fixture is compiled with --cxx once per FM2_NM_* case,
archived with --ar into a fake build tree, and described by a generated
fm2_libs.manifest that check_guards.py --nm reads with --nm-tool.

Exit status 0 only if every scenario behaved as required.
"""

import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GUARD = os.path.join(HERE, "check_guards.py")
FIXTURES = os.path.join(HERE, "guard_fixtures")
FM2 = os.path.join("src", "flowmesh2")
C1_FILES = ("mailbox.h", "mailbox.cpp", "read_slot.h", "read_slot.cpp", "foundation_tests.cpp", "read_slot_tests.cpp")
REAL_FILES = ("CMakeLists.txt", "core/result.h", "core/codes.h", "core/codes.cpp")
DETERMINISTIC_DIRS = ("core", "ledger", "engine", "state", "agreement", "verify")
TIMEOUT_S = 120

# Rule 1: one planted include per forbidden entry (exact names and prefixes).
FORBIDDEN_INCLUDES = (
    "validation.h", "kernel/cs_main.h", "net_processing.h", "net.h", "node/context.h",
    "txmempool.h", "index/txindex.h", "interfaces/chain.h", "wallet/wallet.h", "qt/guiutil.h",
    "rpc/server.h", "init.h", "chainparams.h", "flowmesh/ledger.h",
)
# Rule 2, all non-test code: (label, snippet).
BANNED_ALL = (
    ("std_mutex", "std::mutex g_m;"),
    ("std_recursive_mutex", "std::recursive_mutex g_m;"),
    ("RecursiveMutex", "RecursiveMutex g_m;"),
    ("GlobalMutex", "GlobalMutex g_m;"),
    ("std_shared_mutex", "std::shared_mutex g_m;"),
    ("std_lock_guard", "void F(Mutex& m) { std::lock_guard<Mutex> guard{m}; }"),
    ("std_scoped_lock", "void F(Mutex& m) { std::scoped_lock guard{m}; }"),
    ("std_unique_lock_std_mutex", "void F(std::unique_lock< std::mutex >& guard);"),
    ("std_condition_variable_any", "std::condition_variable_any g_cv;"),
    ("std_jthread", "std::jthread g_worker;"),
    ("std_atomic_shared_ptr", "std::atomic<std::shared_ptr<const int>> g_head;"),
    ("return_braces", "Result<int> F() { return {}; }"),
    ("std_timed_mutex", "std::timed_mutex g_m;"),
    ("using_namespace_std", "using namespace std;"),
)
# Rule 2, deterministic directories only: (label, snippet).
BANNED_DETERMINISTIC = (
    ("float", "float g_x;"),
    ("double", "double g_x;"),
    ("std_unordered_map", "std::unordered_map<int, int> g_map;"),
    ("std_hash", "size_t F(int x) { return std::hash<int>{}(x); }"),
    ("fopen", "void* F() { return fopen(\"x\", \"r\"); }"),
    ("fs", "fs::path g_path;"),
    ("std_condition_variable", "std::condition_variable g_cv;"),
    ("GetTime", "long F() { return GetTime(); }"),
    ("GetTimeMillis", "long F() { return GetTimeMillis(); }"),
    ("rand", "int F() { return rand(); }"),
    ("random_device", "std::random_device g_rd;"),
    ("FastRandomContext", "FastRandomContext g_rng;"),
    ("GetRand", "uint256 F() { return GetRandHash(); }"),
    ("Log", "void F() { LogPrintf(\"x\\n\"); }"),
    ("LogInfo", "void F() { LogInfo(\"x\\n\"); }"),
    ("float_literal", "constexpr auto kHalf{0.5};"),
    ("exponent_literal", "constexpr auto kBig{1e9};"),
    ("std_chrono", "auto F() { return std::chrono::seconds{1}; }"),
    ("std_thread", "std::thread g_worker;"),
)
DETERMINISTIC_INCLUDES = (
    "chrono", "random", "thread", "fstream", "cstdio", "stdio.h", "ctime", "time.h", "cmath",
    "math.h", "unordered_map", "unordered_set", "future", "condition_variable", "util/time.h",
    "random.h", "logging.h", "util/fs.h", "util/fs_helpers.h",
)

# Rule 3 and rule 4 (build): fake build trees.
NM_LIBRARIES = {
    # target: [(object path below <target>.dir, FM2_NM case)]
    "flowmesh2_foundations": [("mailbox.cpp.o", "C1_OK")],
    "fm2_types": [("core/types.cpp.o", "TYPES_OK")],
    "fm2_core": [("ledger/ledger.cpp.o", "CORE_LEDGER_OK"), ("net/lanes.cpp.o", "CORE_NET_OK")],
    "fm2_verify": [("verify/b3verify.cpp.o", "VERIFY_OK")],
}
NM_EXECUTABLES = {"test_flowmesh2": "TEST_OK", "test_flowmesh2_foundations": "C1_TEST_OK", "fm2_sim_cli": "SIM_OK"}
NM_CLOSURES = {
    "flowmesh2_foundations": ["flowmesh2_foundations", "core_interface", "bitcoin_util"],
    "fm2_types": ["fm2_types", "core_interface", "fm2_warnings", "bitcoin_util", "bitcoin_crypto"],
    "fm2_core": ["fm2_core", "fm2_types", "flowmesh2_foundations", "bitcoin_util", "crc32c"],
    "fm2_verify": ["fm2_verify", "fm2_types", "bitcoin_crypto"],
    "test_flowmesh2": ["test_flowmesh2", "fm2_core", "fm2_verify", "bitcoin_common", "Boost::headers"],
    "fm2_sim_cli": ["fm2_sim_cli", "fm2_core", "bitcoin_common"],
    "fm2_warnings": ["fm2_warnings"],
}
NM_SCENARIOS = {
    # name: {"add": {target: [(obj, case)]}, "exe": {target: case}, "closure": {target: [extra]},
    #        "drop_target": name, "drop_closure": name, "missing_file": name, "truncate": bool}
    "r3_cs_main_in_types": {"add": {"fm2_types": [("core/lock.cpp.o", "CS_MAIN")]}},
    "r3_chainstatemanager_in_verify": {"add": {"fm2_verify": [("verify/height.cpp.o", "CHAINSTATEMANAGER")]}},
    "r3_chainstate_in_core": {"add": {"fm2_core": [("chain/ibd.cpp.o", "CHAINSTATE")]}},
    "r3_peermanager_in_core": {"add": {"fm2_core": [("net/ping.cpp.o", "PEERMANAGER")]}},
    "r3_cconnman_in_types": {"add": {"fm2_types": [("core/peers.cpp.o", "CCONNMAN")]}},
    "r3_wallet_in_core": {"add": {"fm2_core": [("admission/balance.cpp.o", "WALLET")]}},
    "r3_v1_reference_in_verify": {"add": {"fm2_verify": [("verify/legacy.cpp.o", "V1_REFERENCE")]}},
    "r3_cs_main_in_c1_library": {"add": {"flowmesh2_foundations": [("lock.cpp.o", "CS_MAIN")]}},
    "r3_dev_certifier_in_core": {"add": {"fm2_core": [("agreement/dev.cpp.o", "DEV_CERTIFIER")]}},
    "r3_fake_chain_in_verify": {"add": {"fm2_verify": [("verify/fake.cpp.o", "FAKE_CHAIN")]}},
    "r3_dev_budget_in_core": {"add": {"fm2_core": [("admission/budget.cpp.o", "DEV_BUDGET")]}},
    "r3_verify_references_ledger_code": {"add": {"fm2_verify": [("verify/bad.cpp.o", "VERIFY_USES_LEDGER")]}},
    "r3_ledger_namespace_in_verify": {"add": {"fm2_verify": [("verify/ns.cpp.o", "LEDGER_NAMESPACE")]}},
    "r3_test_binary_defines_v1": {"exe": {"test_flowmesh2": "V1_DEFINITION"}},
    "r3_sim_binary_defines_v1": {"exe": {"fm2_sim_cli": "V1_DEFINITION"}},
    "r3_built_library_missing": {"missing_file": "fm2_core"},
    "r3_manifest_truncated": {"truncate": True},
    "r3_required_target_missing": {"drop_target": "test_flowmesh2"},
    "r4_types_closure_has_bitcoin_common": {"closure": {"fm2_types": ["bitcoin_common"]}},
    "r4_core_closure_has_bitcoin_node": {"closure": {"fm2_core": ["bitcoin_node"]}},
    "r4_verify_closure_has_wallet": {"closure": {"fm2_verify": ["bitcoin_wallet"]}},
    "r4_core_closure_has_qt": {"closure": {"fm2_core": ["bitcoinqt"]}},
    "r4_verify_closure_has_fm2_core": {"closure": {"fm2_verify": ["fm2_dev", "fm2_core"]}},
    "r4_closure_line_missing": {"drop_closure": "fm2_verify"},
}


class Harness:
    def __init__(self, args):
        self.args = args
        self.root = os.path.abspath(args.root)
        self.work = os.path.abspath(args.workdir)
        self.failures = []
        self.ran = 0
        self.objects = {}

    # -- trees -------------------------------------------------------------

    @staticmethod
    def materialize(source, dest):
        for dirpath, _dirnames, filenames in os.walk(source):
            for name in sorted(filenames):
                if not name.endswith(".fixture"):
                    continue
                rel = os.path.relpath(os.path.join(dirpath, name), source)[: -len(".fixture")]
                target = os.path.join(dest, rel)
                os.makedirs(os.path.dirname(target), exist_ok=True)
                shutil.copyfile(os.path.join(dirpath, name), target)

    def clean_tree(self, name):
        dest = os.path.join(self.work, "trees", name)
        shutil.rmtree(dest, ignore_errors=True)
        self.materialize(os.path.join(FIXTURES, "clean"), dest)
        for rel in C1_FILES + REAL_FILES:
            target = os.path.join(dest, FM2, rel)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            shutil.copyfile(os.path.join(self.root, FM2, rel), target)
        return dest

    @staticmethod
    def write(tree, rel, text, mode="w"):
        path = os.path.join(tree, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, mode, encoding="utf-8") as f:
            f.write(text)

    @staticmethod
    def replace_in(tree, rel, old, new):
        path = os.path.join(tree, rel)
        with open(path, encoding="utf-8") as f:
            text = f.read()
        if old not in text:
            raise RuntimeError(f"mutation: {old!r} not in {rel}")
        with open(path, "w", encoding="utf-8") as f:
            f.write(text.replace(old, new, 1))

    # -- running the guard ---------------------------------------------------

    def guard(self, tree, mode_args):
        cmd = [sys.executable, "-I", "-B", GUARD, "--root", tree] + mode_args
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=TIMEOUT_S)
        return proc.returncode, proc.stdout + proc.stderr

    def expect_clean(self, name, tree, mode_args):
        self.ran += 1
        rc, out = self.guard(tree, mode_args)
        ok = rc == 0 and "check_guards: OK" in out
        print(f"{'ok  ' if ok else 'FAIL'} clean {name}: exit {rc}")
        if not ok:
            self.failures.append(f"clean fixture {name} did not pass:\n{out}")

    def expect_rule(self, name, tree, mode_args):
        self.ran += 1
        rule = int(name.split("_", 1)[0][1:])
        rc, out = self.guard(tree, mode_args)
        fired = sorted({int(line.split("]", 1)[0].split()[-1]) for line in out.splitlines() if line.startswith("FAIL [rule ")})
        ok = rc == 1 and fired == [rule]
        print(f"{'ok  ' if ok else 'FAIL'} planted {name}: exit {rc}, rules fired {fired}")
        if not ok:
            self.failures.append(f"planted {name}: expected exit 1 and exactly rule {rule}, got exit {rc} and rules {fired}:\n{out}")

    # -- source scenarios ----------------------------------------------------

    def source_scenarios(self):
        clean = self.clean_tree("clean")
        self.expect_clean("source", clean, ["--source"])

        planted_root = os.path.join(FIXTURES, "planted")
        for name in sorted(os.listdir(planted_root)):
            tree = self.clean_tree(name)
            self.materialize(os.path.join(planted_root, name), tree)
            self.expect_rule(name, tree, ["--source"])

        cmake = os.path.join(FM2, "CMakeLists.txt")
        todo = os.path.join(FM2, "TODO.md")
        row_08 = ("| M9-FIX-08 | M9 | outline | STUB | R1 | Outline stub with no tag and no case | `core/` "
                  "| fixture | M9/M9-FIX-08/ | — |\n")
        mutations = {
            "r4_types_links_bitcoin_common": lambda t: self.write(
                t, cmake, "\ntarget_link_libraries(fm2_types PRIVATE bitcoin_common)\n", "a"),
            "r4_verify_links_core_transitively": lambda t: self.write(
                t, cmake, "\nadd_library(fm2_verify STATIC verify/b3verify.cpp)\n"
                          "target_link_libraries(fm2_verify PRIVATE fm2_helper)\n"
                          "target_link_libraries(fm2_helper PUBLIC fm2_core)\n", "a"),
            "r4_add_subdirectory": lambda t: self.write(t, cmake, "\nadd_subdirectory(extra)\n", "a"),
            "r5_outline_stub_without_case": lambda t: os.remove(os.path.join(t, FM2, "test", "fixture_tests.cpp")),
            "r5_outline_stub_without_tag": lambda t: self.write(t, todo, row_08, "a"),
            "r5_tagged_todo_row_with_landed_slice": lambda t: self.replace_in(
                t, todo, "| M9-FIX-03 | M9 | outline |", "| M9-FIX-03 | M9 | todo |"),
            "r6_c1_source_byte_changed": lambda t: self.replace_in(t, os.path.join(FM2, "mailbox.cpp"), "#include", "#include "),
            "r6_c1_source_missing": lambda t: os.remove(os.path.join(t, FM2, "read_slot.h")),
            "r6_c1_cmake_block_changed": lambda t: self.replace_in(t, cmake, "TIMEOUT 60", "TIMEOUT 61"),
            "r6_c1_target_modified_later": lambda t: self.write(
                t, cmake, "\ntarget_compile_definitions(flowmesh2_foundations PRIVATE FM2_PLANTED=1)\n", "a"),
            "r6_c1_test_property_modified_later": lambda t: self.write(
                t, cmake, "\nset_property(TEST flowmesh2_foundations PROPERTY TIMEOUT 1)\n", "a"),
        }
        for name, mutate in sorted(mutations.items()):
            tree = self.clean_tree(name)
            mutate(tree)
            self.expect_rule(name, tree, ["--source"])

        for index, include in enumerate(FORBIDDEN_INCLUDES):
            name = "r1_include_" + include.replace("/", "_").replace(".", "_")
            tree = self.clean_tree(name)
            self.write(tree, os.path.join(FM2, "chain", "planted.cpp"), f"#include <{include}>\n")
            self.expect_rule(name, tree, ["--source"])
        for label, snippet in BANNED_ALL:
            name = "r2_all_" + label
            tree = self.clean_tree(name)
            self.write(tree, os.path.join(FM2, "net", "planted.cpp"), f"namespace flowmesh2 {{\n{snippet}\n}}\n")
            self.expect_rule(name, tree, ["--source"])
        for index, (label, snippet) in enumerate(BANNED_DETERMINISTIC):
            directory = DETERMINISTIC_DIRS[index % len(DETERMINISTIC_DIRS)]
            name = f"r2_{directory}_{label}"
            tree = self.clean_tree(name)
            self.write(tree, os.path.join(FM2, directory, "planted.cpp"), f"namespace flowmesh2 {{\n{snippet}\n}}\n")
            self.expect_rule(name, tree, ["--source"])
        for index, include in enumerate(DETERMINISTIC_INCLUDES):
            directory = DETERMINISTIC_DIRS[index % len(DETERMINISTIC_DIRS)]
            name = f"r2_{directory}_include_" + include.replace("/", "_").replace(".", "_")
            tree = self.clean_tree(name)
            self.write(tree, os.path.join(FM2, directory, "planted.cpp"), f"#include <{include}>\n")
            self.expect_rule(name, tree, ["--source"])

        # Deterministic-only constructs are allowed outside the deterministic
        # directories: all of them together in net/ must still pass.
        tree = self.clean_tree("clean_nondeterministic_dir")
        body = "\n".join(snippet for _label, snippet in BANNED_DETERMINISTIC)
        includes = "".join(f"#include <{inc}>\n" for inc in DETERMINISTIC_INCLUDES)
        self.write(tree, os.path.join(FM2, "net", "allowed.cpp"), f"{includes}namespace flowmesh2 {{\n{body}\n}}\n")
        self.expect_clean("deterministic-only constructs in net/", tree, ["--source"])

    # -- nm scenarios --------------------------------------------------------

    def compile_case(self, case):
        if case in self.objects:
            return self.objects[case]
        src_dir = os.path.join(self.work, "nm_src")
        os.makedirs(src_dir, exist_ok=True)
        source = os.path.join(src_dir, "nm_fixture.cpp")
        shutil.copyfile(os.path.join(FIXTURES, "nm", "nm_fixture.cpp.fixture"), source)
        obj = os.path.join(src_dir, f"{case}.o")
        cmd = [self.args.cxx, "-std=c++20", "-O0", f"-DFM2_NM_{case}", "-c", source, "-o", obj]
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=TIMEOUT_S)
        if proc.returncode != 0:
            raise RuntimeError(f"compiling nm fixture case {case} failed:\n{' '.join(cmd)}\n{proc.stdout}{proc.stderr}")
        self.objects[case] = obj
        return obj

    def nm_build(self, name, scenario):
        build = os.path.join(self.work, "builds", name)
        shutil.rmtree(build, ignore_errors=True)
        lines = ["# fixture manifest", "format\tfm2-libs-manifest/1"]
        libraries = {t: list(objs) for t, objs in NM_LIBRARIES.items()}
        for target, extra in scenario.get("add", {}).items():
            libraries[target] = libraries[target] + extra
        executables = dict(NM_EXECUTABLES)
        executables.update(scenario.get("exe", {}))
        closures = {t: list(c) for t, c in NM_CLOSURES.items()}
        for target, extra in scenario.get("closure", {}).items():
            closures[target] = closures[target] + extra
        for target, objs in sorted(libraries.items()):
            obj_paths = []
            for rel, case in objs:
                dest = os.path.join(build, "src", "flowmesh2", "CMakeFiles", f"{target}.dir", rel)
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                shutil.copyfile(self.compile_case(case), dest)
                obj_paths.append(dest)
            archive = os.path.join(build, "lib", f"lib{target}.a")
            os.makedirs(os.path.dirname(archive), exist_ok=True)
            proc = subprocess.run([self.args.ar, "rcs", archive] + obj_paths, capture_output=True, text=True, timeout=TIMEOUT_S)
            if proc.returncode != 0:
                raise RuntimeError(f"{self.args.ar} failed for {archive}: {proc.stderr}")
            if scenario.get("missing_file") == target:
                os.remove(archive)
            if scenario.get("drop_target") != target:
                lines.append(f"target\t{target}\tSTATIC_LIBRARY\t1\t{archive}")
                lines.append("objects\t" + "\t".join([target] + obj_paths))
        for target, case in sorted(executables.items()):
            exe = os.path.join(build, "bin", target)
            os.makedirs(os.path.dirname(exe), exist_ok=True)
            shutil.copyfile(self.compile_case(case), exe)
            if scenario.get("drop_target") != target:
                lines.append(f"target\t{target}\tEXECUTABLE\t1\t{exe}")
        lines.append("target\tfm2_warnings\tINTERFACE_LIBRARY\t1\t-")
        lines.append("target\tfm2_cf_fixture\tOBJECT_LIBRARY\t0\t-")
        for target, closure in sorted(closures.items()):
            if scenario.get("drop_closure") != target and scenario.get("drop_target") != target:
                lines.append("closure\t" + "\t".join([target] + closure))
        if not scenario.get("truncate"):
            lines.append("end")
        with open(os.path.join(build, "fm2_libs.manifest"), "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        return build

    def nm_scenarios(self):
        tree = self.clean_tree("nm_root")
        mode = ["--nm-tool", self.args.nm_tool, "--nm"]
        self.expect_clean("nm", tree, mode + [self.nm_build("clean", {})])
        for name, scenario in sorted(NM_SCENARIOS.items()):
            self.expect_rule(name, tree, mode + [self.nm_build(name, scenario)])
        # Both modes at once on a clean tree and build.
        self.expect_clean("source+nm", tree, ["--source"] + mode + [self.nm_build("clean_both", {})])

    def usage_scenarios(self):
        self.ran += 1
        tree = self.clean_tree("usage")
        rc, out = self.guard(tree, [])
        ok = rc == 2
        print(f"{'ok  ' if ok else 'FAIL'} usage: no mode gives exit {rc}")
        if not ok:
            self.failures.append(f"no mode should exit 2, got {rc}:\n{out}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", required=True, help="repository root (source of the real C1 files)")
    parser.add_argument("--workdir", required=True)
    parser.add_argument("--cxx", required=True, help="C++ compiler for the nm fixtures (CMAKE_CXX_COMPILER)")
    parser.add_argument("--ar", required=True, help="archiver (CMAKE_AR)")
    parser.add_argument("--nm-tool", required=True, help="nm (CMAKE_NM)")
    args = parser.parse_args()
    os.makedirs(args.workdir, exist_ok=True)
    harness = Harness(args)
    try:
        harness.usage_scenarios()
        harness.source_scenarios()
        harness.nm_scenarios()
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as e:
        print(f"HARNESS ERROR: {e}")
        return 1
    print(f"scenarios run: {harness.ran}; failures: {len(harness.failures)}")
    for failure in harness.failures:
        print(f"SELFTEST FAILURE: {failure}")
    if harness.failures:
        return 1
    print("OK: every guard rule fires on its planted fixture and the clean fixtures pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
