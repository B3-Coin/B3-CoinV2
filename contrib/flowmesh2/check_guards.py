#!/usr/bin/env python3
# Copyright (c) 2026 The B3 developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Mechanical guards of the FlowMesh V2 rebirth outline (src/flowmesh2/).

Run with `python3 -I -B` (stdlib only). CTests: flowmesh2_guards runs this
script on the real tree and build; flowmesh2_guards_selftest
(test_check_guards.py) proves that every rule fires on a planted fixture from
guard_fixtures/ and that a clean fixture passes.

OWNER-PROTECTED gate file (TODO.md rule 5, plan section 10.5): the lists and
pins below may be extended; removing or weakening an entry needs an audit and
the owner's sign-off.

Modes (give at least one):

  --source           rules 1, 2, 4 (CMake text), 5, 6 and 7 over --root
  --nm BUILD_DIR     rules 3 and 4 (link closure) over BUILD_DIR/fm2_libs.manifest,
                     which src/flowmesh2/CMakeLists.txt writes with file(GENERATE)

"Non-test fm2 code" is every C++ file under src/flowmesh2/ except test/ and
the two C1 test files. Comments and string literals are removed before the
token rules run, so documentation may name a banned construct.

Rule 1, forbidden includes (non-test fm2 code): validation.h,
  kernel/cs_main.h, net_processing.h, net.h, node/, txmempool.h, index/,
  interfaces/, wallet/, qt/, rpc/, init.h, chainparams.h and the V1 flowmesh/.
  Addition: verify/ includes no flowmesh2/{ledger,engine,state,storage}/ header.
Rule 2, banned constructs. In all non-test fm2 code: std::mutex,
  std::recursive_mutex, RecursiveMutex, GlobalMutex, std::shared_mutex,
  std::lock_guard, std::scoped_lock, std::unique_lock<std::mutex>,
  std::condition_variable_any, std::jthread, std::atomic<std::shared_ptr and
  `return {};`. Also in core/, ledger/, engine/, state/, agreement/ and
  verify/: float, double, std::unordered_*, std::hash, <chrono>, <random>,
  <thread>, <fstream>, <cstdio>, fopen, fs::, std::condition_variable,
  GetTime*, rand(, random_device, FastRandomContext, GetRand*, Log*.
  Additions are marked "(addition)" in the tables below.
Rule 3, nm over every target in fm2_libs.manifest: fm2_types, fm2_verify,
  fm2_core (and the C1 library) have no cs_main, ChainstateManager,
  Chainstate::, PeerManager, CConnman, wallet:: or V1 flowmesh:: symbol;
  fm2_core and fm2_verify define no Dev*, FakeChainAdapter or
  UnlimitedDevBudget; fm2_verify has no flowmesh2::{ledger,engine,state,
  storage}:: symbol and no undefined reference that an fm2_core object built
  from ledger/, engine/, state/ or storage/ defines; no fm2 executable
  (test_flowmesh2, fm2-sim, ...) defines a V1 flowmesh:: symbol. Every built
  library and executable in the manifest must exist, so none is skipped.
Rule 4, link isolation: no CMakeLists.txt or *.cmake outside src/flowmesh2/
  names an fm2_* or flowmesh2_* target; src/flowmesh2/ has one CMakeLists.txt
  and no add_subdirectory; the link closure of fm2_types, fm2_verify and
  fm2_core excludes bitcoin_common, bitcoin_node, bitcoin_wallet and
  bitcoinqt, and that of fm2_verify excludes fm2_core (source text and the
  CMake-computed closure in the manifest).
Rule 5, TODO consistency: every NotImplemented("X")/Unsupported("X") site in
  non-test C++ (and every not_implemented("X") in contrib/flowmesh2 Python)
  has the tag TODO(rebirth:<ID>) with the same ID within 3 lines; every refusal marker
  (REJ_RESIDUAL_CROSS_UNSUPPORTED, UNSUPPORTED_KIND) outside the registry
  files has a tag within 3 lines; every tag names a TODO.md row; a tagged row
  is `outline`, or `todo` while none of its slices has landed; a STUB or
  REAL-REPLACE row with a landed slice is not `todo`; every `outline` STUB has
  a tag and a stub_inventory case (C++ FM2_STUB_CASE or a Python
  test_stub_<ID> method); every `outline` REAL-REPLACE has a tag.
Rule 6, C1 freeze: the six C1 sources have the SHA-256 values recorded at
  1f05fc74, the C1 block of src/flowmesh2/CMakeLists.txt is byte-identical,
  and no later CMake command modifies a C1 target.
Rule 7, signing confinement: bls::SecretKey and .Sign( (addition: ->Sign()
  appear only in storage/seat_signer.cpp, dev/, test/ and sim/.

Exit status: 0 every selected rule passed; 1 a rule failed; 2 usage error.
Each finding is printed as `FAIL [rule N] <where>: <message>`.
"""

import argparse
import bisect
import hashlib
import importlib.util
import io
import os
import posixpath
import re
import subprocess
import sys
import tokenize

FM2 = "src/flowmesh2"
CONTRIB = "contrib/flowmesh2"
WORKFLOW = ".github/workflows/flowmesh2.yml"
CODE_SUFFIXES = (".h", ".hpp", ".hh", ".cpp", ".cc", ".cxx", ".ipp", ".inc", ".tpp")
TAG_SCAN_SUFFIXES = CODE_SUFFIXES + (".py", ".cmake", ".txt", ".yml", ".yaml", ".sh")
C1_TEST_FILES = ("foundation_tests.cpp", "read_slot_tests.cpp")
DETERMINISTIC_DIRS = ("core", "ledger", "engine", "state", "agreement", "verify")
SIGNING_ALLOWED_FILES = ("storage/seat_signer.cpp",)
SIGNING_ALLOWED_DIRS = ("dev", "test", "sim")
REGISTRY_FILES = ("core/codes.h", "core/codes.cpp")
STUB_DEFINITION_FILE = "core/result.h"
NEIGHBOURHOOD = 3

# ---------------------------------------------------------------------------
# Rule 6 pins (owner-protected). Values recorded at 1f05fc74; see
# src/flowmesh2/ARCHITECTURE.md section 13.4.
C1_SOURCES = {
    "mailbox.h": "aa3b31a8ec018e420876911e0388bd4926112982bdfa25bef983787240799c56",
    "mailbox.cpp": "a8d9986adb2dc1ebc4e3721900cf761eee47ea41cb378c07f6817d72e2d5a536",
    "read_slot.h": "fc3e90b50cd3a87caed2eca12f4f3e5a074dd6eab87bfbe00bcf7d3c5397eafd",
    "read_slot.cpp": "702ddde4927299e04cfe743c37106954738527f8b05ce21aa5fe1c5a425ea892",
    "foundation_tests.cpp": "84bd3dfb8f49cb42d1f54506e05cfadd1ef468243ea11363edfdd1294b93104f",
    "read_slot_tests.cpp": "d53769cee07fdb209ab85284953da0f22904c774b1f409e23f82fbc414e66bfb",
}
# src/flowmesh2/CMakeLists.txt at 1f05fc74 (1,155 bytes) is the C1 block.
C1_CMAKE_BLOCK_LENGTH = 1155
C1_CMAKE_BLOCK_SHA256 = "06b755f1b1921869d3d5a293dca640ce6d788d59fd04ff6a9d686796171a115c"
C1_TARGETS = ("flowmesh2_foundations", "test_flowmesh2_foundations")

# ---------------------------------------------------------------------------
# Rule 1 lists.
FORBIDDEN_INCLUDE_EXACT = (
    "validation.h", "kernel/cs_main.h", "net_processing.h", "net.h",
    "txmempool.h", "init.h", "chainparams.h",
)
FORBIDDEN_INCLUDE_PREFIX = (
    "node/", "index/", "interfaces/", "wallet/", "qt/", "rpc/", "flowmesh/",
)
# (addition) b3verify never compiles application code.
VERIFY_FORBIDDEN_INCLUDE_PREFIX = (
    "flowmesh2/ledger/", "flowmesh2/engine/", "flowmesh2/state/", "flowmesh2/storage/",
)

# Rule 2 tables: (label, regex). Matched on comment- and string-free text.
BANNED_ALL = (
    ("std::mutex", r"\bstd::mutex\b"),
    ("std::recursive_mutex", r"\bstd::recursive_mutex\b"),
    ("RecursiveMutex", r"\bRecursiveMutex\b"),
    ("GlobalMutex", r"\bGlobalMutex\b"),
    ("std::shared_mutex", r"\bstd::shared_mutex\b"),
    ("std::lock_guard", r"\bstd::lock_guard\b"),
    ("std::scoped_lock", r"\bstd::scoped_lock\b"),
    ("std::unique_lock<std::mutex>", r"\bstd::unique_lock\s*<\s*std::mutex\s*>"),
    ("std::condition_variable_any", r"\bstd::condition_variable_any\b"),
    ("std::jthread", r"\bstd::jthread\b"),
    ("std::atomic<std::shared_ptr", r"\bstd::atomic\s*<\s*std::shared_ptr\b"),
    ("return {};", r"\breturn\s*\{\s*\}\s*;"),
    ("std::*timed_mutex (addition)", r"\bstd::(?:recursive_timed|shared_timed|timed)_mutex\b"),
    ("using namespace std (addition)", r"\busing\s+namespace\s+std\b"),
)
FLOAT_LITERAL = (r"(?<![\w.])(?:\d[\d']*\.[\d']*(?:[eE][+-]?\d+)?|\.\d[\d']*(?:[eE][+-]?\d+)?"
                 r"|\d[\d']*[eE][+-]?\d+)[fFlL]?(?![\w.])")
BANNED_DETERMINISTIC = (
    ("float", r"\bfloat\b"),
    ("double", r"\bdouble\b"),
    ("std::unordered_", r"\bstd::unordered_\w*"),
    ("std::hash", r"\bstd::hash\b"),
    ("fopen", r"\bfopen\b"),
    ("fs::", r"\bfs::"),
    ("std::condition_variable", r"\bstd::condition_variable\b"),
    ("GetTime", r"\bGetTime(?:[A-Z]\w*)?\b"),
    ("rand(", r"\brand\s*\("),
    ("random_device", r"\brandom_device\b"),
    ("FastRandomContext", r"\bFastRandomContext\b"),
    ("GetRand", r"\bGetRand\w*"),
    ("Log*", r"\bLog[A-Z]\w*"),
    ("floating-point literal (addition)", FLOAT_LITERAL),
    ("std::chrono (addition)", r"\bstd::chrono\b"),
    ("std::thread (addition)", r"\bstd::(?:this_)?thread\b"),
)
BANNED_DETERMINISTIC_INCLUDES = (
    "chrono", "random", "thread", "fstream", "cstdio",
    # additions
    "stdio.h", "ctime", "time.h", "cmath", "math.h", "unordered_map", "unordered_set",
    "future", "condition_variable", "util/time.h", "random.h", "logging.h",
    "util/fs.h", "util/fs_helpers.h",
)

# Rule 3 tables (matched on demangled names).
_NOT_ID = r"(?<![A-Za-z0-9_])"
NODE_SYMBOLS = (
    ("cs_main", re.compile(_NOT_ID + r"_?cs_main(?![A-Za-z0-9_])")),
    ("ChainstateManager", re.compile(_NOT_ID + r"ChainstateManager(?![A-Za-z0-9_])")),
    ("Chainstate::", re.compile(_NOT_ID + r"Chainstate::")),
    ("PeerManager", re.compile(_NOT_ID + r"PeerManager")),
    ("CConnman", re.compile(_NOT_ID + r"CConnman(?![A-Za-z0-9_])")),
    ("wallet::", re.compile(_NOT_ID + r"wallet::")),
    ("V1 flowmesh::", re.compile(_NOT_ID + r"flowmesh::")),
)
V1_SYMBOL = re.compile(_NOT_ID + r"flowmesh::")
DEV_SYMBOL = re.compile(_NOT_ID + r"(?:Dev[A-Z0-9_]\w*|FakeChainAdapter|UnlimitedDevBudget)(?![A-Za-z0-9_])"
                        r"|" + _NOT_ID + r"flowmesh2::dev::")
APPLICATION_NAMESPACE = re.compile(_NOT_ID + r"flowmesh2::(?:ledger|engine|state|storage)::")
APPLICATION_DIRS = ("ledger", "engine", "state", "storage")
NODE_CLEAN_TARGETS = ("fm2_types", "fm2_verify", "fm2_core", "flowmesh2_foundations")
DEV_CLEAN_TARGETS = ("fm2_core", "fm2_verify")
REQUIRED_MANIFEST_TARGETS = ("fm2_types", "test_flowmesh2", "flowmesh2_foundations")
ALWAYS_INSPECTED = ("test_flowmesh2", "fm2_sim_cli")
BUILT_TYPES = ("STATIC_LIBRARY", "SHARED_LIBRARY", "MODULE_LIBRARY", "EXECUTABLE")
MANIFEST_FORMAT = "fm2-libs-manifest/1"

# Rule 4 tables.
LINK_GUARDED = ("fm2_types", "fm2_verify", "fm2_core")
LINK_FORBIDDEN = ("bitcoin_common", "bitcoin_node", "bitcoin_wallet", "bitcoinqt")
LINK_FORBIDDEN_EXTRA = {"fm2_verify": ("fm2_core",)}
OUTSIDE_TARGET_NAME = re.compile(r"(?<![A-Za-z0-9_])(?:lib)?((?:fm2|flowmesh2)_[A-Za-z0-9_]+)")
CMAKE_KEYWORDS = {"PUBLIC", "PRIVATE", "INTERFACE", "LINK_PUBLIC", "LINK_PRIVATE", "LINK_INTERFACE_LIBRARIES"}

# Rule 5 tables.
STUB_CALL = re.compile(r"\b(NotImplemented|Unsupported)\s*\(")
REFUSAL_MARKERS = re.compile(r"\b(REJ_RESIDUAL_CROSS_UNSUPPORTED|UNSUPPORTED_KIND)\b")
TAG = re.compile(r"TODO\(rebirth([^)\n]*)\)")
CPP_STUB_CASE = re.compile(r"\bFM2_STUB_CASE\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*\"([^\"\n]*)\"\s*\)")
PY_STUB_CASE = re.compile(r"^\s*def\s+test_stub_([A-Za-z0-9_]+)\s*\(", re.M)

# Rule 7 table.
SIGNING = (
    ("bls::SecretKey", re.compile(r"\bbls::SecretKey\b")),
    (".Sign(", re.compile(r"\.\s*Sign\s*\(")),
    ("->Sign( (addition)", re.compile(r"->\s*Sign\s*\(")),
)

RULE_NAMES = {
    1: "forbidden includes",
    2: "banned constructs",
    3: "nm symbol checks",
    4: "link isolation",
    5: "TODO consistency",
    6: "C1 freeze",
    7: "signing confinement",
}


def tag_text(task_id):
    """The source tag for task_id, spelled so this file holds no literal tag."""
    return "TODO(" + "rebirth:" + task_id + ")"


class Findings:
    def __init__(self):
        self.items = []
        self.notes = []

    def fail(self, rule, where, message):
        self.items.append((rule, where, message))

    def note(self, message):
        self.notes.append(message)


# ---------------------------------------------------------------------------
# Source helpers.

def load_check_todo():
    """Import the sibling check_todo.py by path (works under python3 -I)."""
    here = os.path.dirname(os.path.abspath(__file__))
    spec = importlib.util.spec_from_file_location("fm2_check_todo", os.path.join(here, "check_todo.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def read_text(path):
    with open(path, encoding="utf-8", errors="surrogateescape") as f:
        return f.read()


def read_bytes(path):
    with open(path, "rb") as f:
        return f.read()


def sha256_hex(data):
    return hashlib.sha256(data).hexdigest()


def line_index(text):
    starts = [0]
    for m in re.finditer("\n", text):
        starts.append(m.end())
    return starts


def line_of(starts, offset):
    return bisect.bisect_right(starts, offset)


def _token_before(text, i):
    j = i
    while j > 0 and (text[j - 1].isalnum() or text[j - 1] in "_'."):
        j -= 1
    return text[j:i]


def strip_cpp(text, keep_strings=False):
    """Blank C++ comments (and string/char literal contents unless keep_strings).

    Offsets and newlines are preserved, so line numbers stay valid. Handles
    escapes, raw strings (R"d(...)d" with u8/u/U/L prefixes) and digit
    separators (1'000)."""
    out = list(text)
    n = len(text)

    def blank(a, b):
        for k in range(a, min(b, n)):
            if out[k] != "\n":
                out[k] = " "

    i = 0
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            j = i
            while j < n and text[j] != "\n":
                if text[j] == "\\" and j + 1 < n and text[j + 1] == "\n":
                    j += 2
                    continue
                j += 1
            blank(i, j)
            i = j
            continue
        if c == "/" and nxt == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            blank(i, j)
            i = j
            continue
        if c == '"':
            ident = re.search(r"[A-Za-z0-9_]*$", text[max(0, i - 4):i]).group(0)
            if ident in ("R", "u8R", "uR", "UR", "LR"):
                m = re.compile(r'"([^()\\ \t\n]{0,16})\(').match(text, i)
                if m:
                    close = ")" + m.group(1) + '"'
                    end = text.find(close, m.end())
                    end = n if end < 0 else end + len(close)
                    if not keep_strings:
                        blank(i + 1, end - 1)
                    i = end
                    continue
            j = i + 1
            while j < n and text[j] not in '"\n':
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            if not keep_strings:
                blank(i + 1, j - 1)
            i = j
            continue
        if c == "'":
            token = _token_before(text, i)
            if token and token[0].isdigit():
                i += 1  # digit separator
                continue
            j = i + 1
            while j < n and text[j] not in "'\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            if not keep_strings:
                blank(i + 1, j - 1)
            i = j
            continue
        i += 1
    return "".join(out)


INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*([<"])([^>"\n]+)[>"]', re.M)


def includes_of(rel, text_no_comments):
    """Yield (line, spelled path, [candidate paths relative to src/]).

    A quoted include may resolve next to the including file or on the include
    path, so both readings are candidates; a directive is forbidden if any
    candidate is."""
    starts = line_index(text_no_comments)
    here = posixpath.dirname(rel)
    for m in INCLUDE.finditer(text_no_comments):
        target = m.group(2).strip()
        candidates = [posixpath.normpath(target)]
        if m.group(1) == '"' and not target.startswith("/"):
            beside = posixpath.normpath(posixpath.join(here, target))
            if beside.startswith("src/"):
                candidates.append(beside[len("src/"):])
        yield line_of(starts, m.start()), target, candidates


def walk_files(root, base):
    """Sorted relative POSIX paths of the files under root/base."""
    top = os.path.join(root, base)
    found = []
    for dirpath, dirnames, filenames in os.walk(top):
        dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
        for name in filenames:
            path = os.path.join(dirpath, name)
            found.append(os.path.relpath(path, root).replace(os.sep, "/"))
    return sorted(found)


def fm2_code_files(root):
    return [p for p in walk_files(root, FM2) if p.endswith(CODE_SUFFIXES)]


def fm2_rel(path):
    return path[len(FM2) + 1:]


def is_test_code(path):
    rel = fm2_rel(path)
    return rel.startswith("test/") or rel in C1_TEST_FILES


def top_dir(path):
    rel = fm2_rel(path)
    return rel.split("/", 1)[0] if "/" in rel else ""


# ---------------------------------------------------------------------------
# Rules 1, 2 and 7 (one pass over the C++ files).

def check_code_rules(root, findings, counts):
    for path in fm2_code_files(root):
        text = read_text(os.path.join(root, path))
        no_comments = strip_cpp(text, keep_strings=True)
        code = strip_cpp(text, keep_strings=False)
        starts = line_index(code)
        rel = fm2_rel(path)
        test = is_test_code(path)
        deterministic = top_dir(path) in DETERMINISTIC_DIRS
        if not test:
            counts[1] += 1
            counts[2] += 1
            for line, spelled, candidates in includes_of(path, no_comments):
                if any(c in FORBIDDEN_INCLUDE_EXACT or c.startswith(FORBIDDEN_INCLUDE_PREFIX) for c in candidates):
                    findings.fail(1, f"{path}:{line}", f"forbidden include {spelled}")
                if top_dir(path) == "verify" and any(c.startswith(VERIFY_FORBIDDEN_INCLUDE_PREFIX) for c in candidates):
                    findings.fail(1, f"{path}:{line}", f"verify/ includes application header {spelled} (addition)")
                if deterministic and any(c in BANNED_DETERMINISTIC_INCLUDES for c in candidates):
                    findings.fail(2, f"{path}:{line}", f"include {spelled} in deterministic directory {top_dir(path)}/")
            tables = [(label, regex, "") for label, regex in BANNED_ALL]
            if deterministic:
                tables += [(label, regex, f" in deterministic directory {top_dir(path)}/") for label, regex in BANNED_DETERMINISTIC]
            for label, regex, suffix in tables:
                for m in re.finditer(regex, code):
                    findings.fail(2, f"{path}:{line_of(starts, m.start())}", f"banned construct {label}{suffix}")
        allowed = rel in SIGNING_ALLOWED_FILES or rel.split("/", 1)[0] in SIGNING_ALLOWED_DIRS
        counts[7] += 1
        if not allowed:
            for label, regex in SIGNING:
                for m in regex.finditer(code):
                    findings.fail(7, f"{path}:{line_of(starts, m.start())}",
                                  f"{label} outside storage/seat_signer.cpp, dev/, test/ and sim/")


# ---------------------------------------------------------------------------
# Rule 4 (source).

def strip_cmake_comments(text):
    out = []
    for line in text.splitlines(keepends=True):
        in_quote = False
        cut = len(line)
        for k, ch in enumerate(line):
            if ch == '"' and (k == 0 or line[k - 1] != "\\"):
                in_quote = not in_quote
            elif ch == "#" and not in_quote:
                cut = k
                break
        out.append(line[:cut] + ("\n" if line.endswith("\n") and cut < len(line) else ""))
    return "".join(out)


def cmake_files_outside(root):
    found = []
    for dirpath, dirnames, filenames in os.walk(root):
        rel_dir = os.path.relpath(dirpath, root).replace(os.sep, "/")
        keep = []
        for d in sorted(dirnames):
            rel = d if rel_dir == "." else f"{rel_dir}/{d}"
            if d.startswith(".") or d == "node_modules":
                continue
            if rel_dir == "." and "build" in d and d not in ("build-aux", "build_msvc"):
                continue
            if rel in (FM2, "depends/work", "depends/built", "depends/sources", "depends/SDKs"):
                continue
            if os.path.exists(os.path.join(dirpath, d, "CMakeCache.txt")):
                continue
            keep.append(d)
        dirnames[:] = keep
        for name in sorted(filenames):
            if name == "CMakeLists.txt" or name.endswith(".cmake"):
                found.append(os.path.relpath(os.path.join(dirpath, name), root).replace(os.sep, "/"))
    return found


def cmake_link_map(text):
    """{target: [dependency names]} from target_link_libraries() calls."""
    links = {}
    for m in re.finditer(r"\btarget_link_libraries\s*\(([^)]*)\)", text):
        tokens = m.group(1).split()
        if not tokens:
            continue
        target = tokens[0]
        names = []
        for token in tokens[1:]:
            if token in CMAKE_KEYWORDS:
                continue
            names.extend(re.findall(r"[A-Za-z0-9_.+-]+(?:::[A-Za-z0-9_.+-]+)*", token))
        links.setdefault(target, []).extend(names)
    return links


def closure_of(links, target):
    seen = []
    queue = [target]
    while queue:
        current = queue.pop(0)
        if current in seen:
            continue
        seen.append(current)
        queue.extend(links.get(current, []))
    return seen


def check_closure(findings, where, target, closure):
    forbidden = LINK_FORBIDDEN + LINK_FORBIDDEN_EXTRA.get(target, ())
    for name in closure:
        if name != target and name in forbidden:
            findings.fail(4, where, f"{target} links {name} (forbidden in its link closure)")


def check_link_source(root, findings, counts):
    for path in cmake_files_outside(root):
        counts[4] += 1
        text = strip_cmake_comments(read_text(os.path.join(root, path)))
        starts = line_index(text)
        for m in OUTSIDE_TARGET_NAME.finditer(text):
            findings.fail(4, f"{path}:{line_of(starts, m.start())}",
                          f"names fm2 target {m.group(1)} outside {FM2}/")
    fm2_cmake = [p for p in walk_files(root, FM2) if p.endswith("CMakeLists.txt") or p.endswith(".cmake")]
    for path in fm2_cmake:
        if path != f"{FM2}/CMakeLists.txt":
            findings.fail(4, path, "src/flowmesh2/ must have exactly one CMake file (fm2_libs.manifest lists one directory)")
    main = os.path.join(root, FM2, "CMakeLists.txt")
    if not os.path.exists(main):
        findings.fail(4, f"{FM2}/CMakeLists.txt", "missing")
        return
    text = strip_cmake_comments(read_text(main))
    starts = line_index(text)
    for m in re.finditer(r"\badd_subdirectory\s*\(", text):
        findings.fail(4, f"{FM2}/CMakeLists.txt:{line_of(starts, m.start())}", "add_subdirectory is not allowed in src/flowmesh2/")
    links = cmake_link_map(text)
    for target in LINK_GUARDED:
        if target in links or re.search(r"\badd_library\s*\(\s*" + target + r"\b", text):
            check_closure(findings, f"{FM2}/CMakeLists.txt", target, closure_of(links, target))


# ---------------------------------------------------------------------------
# Rule 5.

def ledger_rows(root, findings):
    module = load_check_todo()
    path = os.path.join(root, FM2, "TODO.md")
    if not os.path.exists(path):
        findings.fail(5, f"{FM2}/TODO.md", "missing")
        return module, {}
    rows, problems = module.parse(read_text(path).splitlines())
    for problem in problems:
        findings.fail(5, f"{FM2}/TODO.md", problem)
    by_id = {}
    for _number, cells in rows:
        by_id.setdefault(cells[0], {"status": cells[2], "skeleton": cells[3], "slices": cells[4]})
    return module, by_id


def landed_slices(rows):
    return {i for i, row in rows.items() if i.startswith("R") and row["status"] in ("outline", "done")}


def slices_of(row):
    return [s.strip() for s in row["slices"].split(",") if s.strip().startswith("R")]


def tag_scan_files(root):
    files = [p for p in walk_files(root, FM2) if p.endswith(TAG_SCAN_SUFFIXES)]
    files += [p for p in walk_files(root, CONTRIB)
              if p.endswith(TAG_SCAN_SUFFIXES) and not p.startswith(f"{CONTRIB}/guard_fixtures/")]
    if os.path.exists(os.path.join(root, WORKFLOW)):
        files.append(WORKFLOW)
    return files


def python_stub_sites(text):
    """(line, argument string literal or None) for each not_implemented( call."""
    sites = []
    try:
        tokens = list(tokenize.generate_tokens(io.StringIO(text).readline))
    except (tokenize.TokenError, SyntaxError):
        return None
    for k, tok in enumerate(tokens):
        if tok.type == tokenize.NAME and tok.string == "not_implemented":
            if k > 0 and tokens[k - 1].type == tokenize.NAME and tokens[k - 1].string == "def":
                continue
            if k + 1 < len(tokens) and tokens[k + 1].string == "(":
                arg = tokens[k + 2] if k + 2 < len(tokens) else None
                literal = None
                if arg is not None and arg.type == tokenize.STRING and k + 3 < len(tokens) and tokens[k + 3].string == ")":
                    literal = re.sub(r"^[rRbBuU]*['\"]|['\"]$", "", arg.string)
                sites.append((tok.start[0], literal))
    return sites


def check_todo_consistency(root, findings, counts):
    module, rows = ledger_rows(root, findings)
    id_re = module.ID_RE
    landed = landed_slices(rows)
    tags = {}  # id -> [where]
    tag_lines = {}  # path -> {line: [ids]}

    def row_state_problem(task_id, what):
        row = rows.get(task_id)
        if row is None:
            return f"{what} names {task_id}, which has no row in TODO.md"
        if row["skeleton"] == "NONE":
            return f"{what} names {task_id}, a NONE row (new work carries no tag)"
        if row["status"] == "done":
            return f"{what} names {task_id}, whose row is done (remove the {what} with the task)"
        if row["status"] == "todo" and any(s in landed for s in slices_of(row)):
            return f"{what} names {task_id}, whose row is still todo although slice(s) {row['slices']} landed; move it to outline"
        return None

    for path in tag_scan_files(root):
        text = read_text(os.path.join(root, path))
        starts = line_index(text)
        for m in TAG.finditer(text):
            line = line_of(starts, m.start())
            inner = m.group(1)
            if re.fullmatch(r":<[^>]*>", inner):
                continue  # documentation placeholder such as TODO(rebirth:<ID>)
            counts[5] += 1
            task_id = inner[1:] if inner.startswith(":") else None
            if task_id is None or not id_re.match(task_id):
                findings.fail(5, f"{path}:{line}", f"malformed tag {m.group(0)!r}")
                continue
            tags.setdefault(task_id, []).append(f"{path}:{line}")
            tag_lines.setdefault(path, {}).setdefault(line, []).append(task_id)
            problem = row_state_problem(task_id, "tag")
            if problem:
                findings.fail(5, f"{path}:{line}", problem)

    def tags_near(path, line):
        near = []
        for k in range(line - NEIGHBOURHOOD, line + NEIGHBOURHOOD + 1):
            near.extend(tag_lines.get(path, {}).get(k, []))
        return near

    for path in fm2_code_files(root):
        if is_test_code(path):
            continue
        rel = fm2_rel(path)
        text = read_text(os.path.join(root, path))
        no_comments = strip_cpp(text, keep_strings=True)
        code = strip_cpp(text, keep_strings=False)
        starts = line_index(code)
        for m in STUB_CALL.finditer(code):
            if rel == STUB_DEFINITION_FILE and re.search(r"\bError\s+$", code[max(0, m.start() - 16):m.start()]):
                continue  # the declarations of NotImplemented/Unsupported
            line = line_of(starts, m.start())
            counts[5] += 1
            arg = re.match(r'\s*"([^"\n]*)"\s*\)', no_comments[m.end():])
            if not arg:
                findings.fail(5, f"{path}:{line}", f"{m.group(1)}(...) must take a task ID string literal")
                continue
            task_id = arg.group(1)
            if not id_re.match(task_id):
                findings.fail(5, f"{path}:{line}", f"{m.group(1)}(\"{task_id}\") is not a task ID")
            elif task_id not in tags_near(path, line):
                findings.fail(5, f"{path}:{line}", f"{m.group(1)}(\"{task_id}\") has no {tag_text(task_id)} within {NEIGHBOURHOOD} lines")
        if rel in REGISTRY_FILES:
            continue
        for m in REFUSAL_MARKERS.finditer(code):
            line = line_of(starts, m.start())
            counts[5] += 1
            if not tags_near(path, line):
                findings.fail(5, f"{path}:{line}", f"refusal marker {m.group(1)} has no TODO(rebirth:<ID>) tag within {NEIGHBOURHOOD} lines")

    for path in walk_files(root, CONTRIB):
        name = posixpath.basename(path)
        if not path.endswith(".py") or name.startswith("test_") or path.startswith(f"{CONTRIB}/guard_fixtures/"):
            continue
        sites = python_stub_sites(read_text(os.path.join(root, path)))
        if sites is None:
            findings.fail(5, path, "cannot tokenize Python source")
            continue
        for line, literal in sites:
            counts[5] += 1
            if literal is None or not id_re.match(literal):
                findings.fail(5, f"{path}:{line}", "not_implemented(...) must take a task ID string literal")
            elif literal not in tags_near(path, line):
                findings.fail(5, f"{path}:{line}", f"not_implemented(\"{literal}\") has no {tag_text(literal)} within {NEIGHBOURHOOD} lines")

    cases = {}  # id -> [where]
    for path in walk_files(root, f"{FM2}/test"):
        if not path.endswith(CODE_SUFFIXES):
            continue
        no_comments = strip_cpp(read_text(os.path.join(root, path)), keep_strings=True)
        starts = line_index(no_comments)
        for m in CPP_STUB_CASE.finditer(no_comments):
            where = f"{path}:{line_of(starts, m.start())}"
            ident, task_id = m.group(1), m.group(2)
            if ident != task_id.replace("-", "_"):
                findings.fail(5, where, f"FM2_STUB_CASE name {ident} should be {task_id.replace('-', '_')}")
            cases.setdefault(task_id, []).append(where)
    for path in walk_files(root, CONTRIB):
        name = posixpath.basename(path)
        if not (name.startswith("test_") and name.endswith(".py")) or path.startswith(f"{CONTRIB}/guard_fixtures/"):
            continue
        text = read_text(os.path.join(root, path))
        starts = line_index(text)
        for m in PY_STUB_CASE.finditer(text):
            cases.setdefault(m.group(1).replace("_", "-"), []).append(f"{path}:{line_of(starts, m.start())}")
    for task_id, wheres in sorted(cases.items()):
        counts[5] += 1
        row = rows.get(task_id)
        if row is not None and row["skeleton"] != "STUB":
            findings.fail(5, wheres[0], f"stub_inventory case names {task_id}, whose row is {row['skeleton']}, not STUB")
            continue
        problem = row_state_problem(task_id, "stub_inventory case")
        if problem:
            findings.fail(5, wheres[0], problem)

    for task_id, row in sorted(rows.items()):
        if row["skeleton"] not in ("STUB", "REAL-REPLACE"):
            continue
        if row["status"] == "todo" and any(s in landed for s in slices_of(row)):
            findings.fail(5, f"{FM2}/TODO.md ({task_id})", f"{row['skeleton']} row is todo although slice(s) {row['slices']} landed")
        if row["status"] != "outline":
            continue
        if not tags.get(task_id):
            findings.fail(5, f"{FM2}/TODO.md ({task_id})", f"outline {row['skeleton']} row has no {tag_text(task_id)} tag in the tree")
        if row["skeleton"] == "STUB" and not cases.get(task_id):
            findings.fail(5, f"{FM2}/TODO.md ({task_id})", "outline STUB row has no stub_inventory case")


# ---------------------------------------------------------------------------
# Rule 6.

def check_c1_freeze(root, findings, counts):
    for name, expected in sorted(C1_SOURCES.items()):
        counts[6] += 1
        path = os.path.join(root, FM2, name)
        if not os.path.exists(path):
            findings.fail(6, f"{FM2}/{name}", "C1 source is missing")
            continue
        actual = sha256_hex(read_bytes(path))
        if actual != expected:
            findings.fail(6, f"{FM2}/{name}", f"SHA-256 {actual} differs from the pinned {expected}")
    counts[6] += 1
    path = os.path.join(root, FM2, "CMakeLists.txt")
    if not os.path.exists(path):
        findings.fail(6, f"{FM2}/CMakeLists.txt", "missing")
        return
    data = read_bytes(path)
    block = data[:C1_CMAKE_BLOCK_LENGTH]
    if len(block) != C1_CMAKE_BLOCK_LENGTH or sha256_hex(block) != C1_CMAKE_BLOCK_SHA256:
        findings.fail(6, f"{FM2}/CMakeLists.txt", f"the first {C1_CMAKE_BLOCK_LENGTH} bytes (the C1 block) differ from 1f05fc74")
        return
    rest = strip_cmake_comments(data[C1_CMAKE_BLOCK_LENGTH:].decode("utf-8", errors="replace"))
    starts = line_index(rest)
    first_line = data[:C1_CMAKE_BLOCK_LENGTH].count(b"\n")
    c1 = "|".join(C1_TARGETS)
    for m in re.finditer(r"\b(target_\w+|set_target_properties|set_tests_properties)\s*\(\s*(" + c1 + r")\b", rest):
        findings.fail(6, f"{FM2}/CMakeLists.txt:{first_line + line_of(starts, m.start())}",
                      f"{m.group(1)} modifies the frozen C1 target {m.group(2)} after the C1 block")
    for m in re.finditer(r"\bset_property\s*\(\s*(?:TARGET|TEST)\b([^)]*)\)", rest):
        if re.search(r"\b(?:" + c1 + r")\b", m.group(1).split("PROPERTY", 1)[0]):
            findings.fail(6, f"{FM2}/CMakeLists.txt:{first_line + line_of(starts, m.start())}",
                          "set_property modifies a frozen C1 target after the C1 block")


# ---------------------------------------------------------------------------
# Rules 3 and 4 (build): fm2_libs.manifest and nm.

class GuardError(Exception):
    pass


def read_manifest(path, findings):
    targets, objects, closures = {}, {}, {}
    if not os.path.exists(path):
        findings.fail(3, path, "fm2_libs.manifest is missing (configure with -DBUILD_FLOWMESH2=ON)")
        return None
    lines = read_text(path).splitlines()
    body = [line for line in lines if line and not line.startswith("#")]
    if not body or body[0] != f"format\t{MANIFEST_FORMAT}":
        findings.fail(3, path, f"first line must be 'format\\t{MANIFEST_FORMAT}'")
        return None
    if body[-1] != "end":
        findings.fail(3, path, "manifest is truncated (no final 'end' line)")
        return None
    for line in body[1:-1]:
        fields = line.split("\t")
        kind = fields[0]
        if kind == "target" and len(fields) == 5:
            targets[fields[1]] = {"type": fields[2], "in_all": fields[3] == "1", "file": fields[4]}
        elif kind == "objects" and len(fields) >= 2:
            objects[fields[1]] = [f for f in fields[2:] if f]
        elif kind == "closure" and len(fields) >= 2:
            closures[fields[1]] = [f for f in fields[2:] if f]
        else:
            findings.fail(3, path, f"malformed manifest line {line!r}")
    return targets, objects, closures


SYMBOL_DEFINED = re.compile(r"^([0-9A-Fa-f]+)\s+(\S)\s+(.*)$")
SYMBOL_UNDEFINED = re.compile(r"^\s+(\S)\s+(.*)$")


def nm_symbols(nm_tool, path):
    """[(member, defined, letter, demangled name)] for an object, archive or binary."""
    try:
        proc = subprocess.run([nm_tool, "-C", path], capture_output=True, text=True, errors="replace", timeout=600)
    except (OSError, subprocess.TimeoutExpired) as e:
        raise GuardError(f"cannot run {nm_tool} on {path}: {e}")
    if proc.returncode != 0:
        raise GuardError(f"{nm_tool} -C {path} exited {proc.returncode}: {proc.stderr.strip()[:400]}")
    symbols = []
    member = os.path.basename(path)
    for line in proc.stdout.splitlines():
        if not line.strip():
            continue
        m = SYMBOL_DEFINED.match(line)
        if m:
            symbols.append((member, m.group(2) not in "Uwv", m.group(2), m.group(3)))
            continue
        m = SYMBOL_UNDEFINED.match(line)
        if m:
            symbols.append((member, False, m.group(1), m.group(2)))
            continue
        if line.endswith(":"):
            member = line[:-1].strip()
    return symbols


def application_definitions(nm_tool, objects):
    """Externally defined flowmesh2 names of fm2_core objects from ledger/, engine/, state/, storage/."""
    defined = set()
    for obj in objects:
        norm = obj.replace("\\", "/")
        if not re.search(r"/fm2_core\.dir/(?:" + "|".join(APPLICATION_DIRS) + r")/", norm):
            continue
        for _member, is_defined, letter, name in nm_symbols(nm_tool, obj):
            if is_defined and letter.isupper() and "flowmesh2::" in name and "(anonymous namespace)" not in name:
                defined.add(name)
    return defined


def check_nm(build_dir, config, nm_tool, findings, counts):
    candidates = []
    if config:
        candidates.append(os.path.join(build_dir, f"fm2_libs-{config}.manifest"))
    candidates.append(os.path.join(build_dir, "fm2_libs.manifest"))
    path = next((c for c in candidates if os.path.exists(c)), candidates[-1])
    manifest = read_manifest(path, findings)
    if manifest is None:
        return
    targets, objects, closures = manifest
    for required in REQUIRED_MANIFEST_TARGETS:
        if required not in targets:
            findings.fail(3, path, f"manifest does not list {required}")
    symbols = {}
    for name, info in sorted(targets.items()):
        if info["type"] not in BUILT_TYPES:
            continue
        exists = info["file"] not in ("", "-") and os.path.exists(info["file"])
        if not exists:
            if info["in_all"] or name in NODE_CLEAN_TARGETS or name in ALWAYS_INSPECTED:
                findings.fail(3, name, f"listed in the manifest but not built: {info['file']} (build the fm2_all target)")
            else:
                findings.note(f"rule 3: {name} is EXCLUDE_FROM_ALL and not built; not inspected")
            continue
        counts[3] += 1
        try:
            symbols[name] = nm_symbols(nm_tool, info["file"])
        except GuardError as e:
            findings.fail(3, name, str(e))
    for name in NODE_CLEAN_TARGETS:
        for member, _defined, _letter, sym in symbols.get(name, []):
            for label, regex in NODE_SYMBOLS:
                if regex.search(sym):
                    findings.fail(3, f"{name}({member})", f"node/wallet/V1 symbol {label}: {sym}")
    for name in DEV_CLEAN_TARGETS:
        for member, defined, _letter, sym in symbols.get(name, []):
            if defined and DEV_SYMBOL.search(sym):
                findings.fail(3, f"{name}({member})", f"defines a dev-only symbol: {sym}")
    if "fm2_verify" in symbols:
        app = set()
        if "fm2_core" in objects:
            try:
                app = application_definitions(nm_tool, objects["fm2_core"])
            except GuardError as e:
                findings.fail(3, "fm2_core", str(e))
        for member, defined, _letter, sym in symbols["fm2_verify"]:
            if APPLICATION_NAMESPACE.search(sym):
                findings.fail(3, f"fm2_verify({member})", f"application (ledger/engine/state/storage) symbol: {sym}")
            elif not defined and sym in app:
                findings.fail(3, f"fm2_verify({member})", f"references fm2_core application code: {sym}")
    for name, info in sorted(targets.items()):
        if info["type"] != "EXECUTABLE":
            continue
        for member, defined, _letter, sym in symbols.get(name, []):
            if defined and V1_SYMBOL.search(sym):
                findings.fail(3, f"{name}({member})", f"executable defines a V1 flowmesh:: symbol: {sym}")
    for name in ("fm2_verify", "fm2_core"):
        if name not in targets:
            findings.note(f"rule 3: {name} does not exist yet; its checks run once a slice adds it")
    # Rule 4: the CMake-computed link closure.
    for name in LINK_GUARDED:
        if name not in targets:
            continue
        counts[4] += 1
        if name not in closures:
            findings.fail(4, path, f"manifest has no closure line for {name}")
            continue
        check_closure(findings, f"{path} (closure {name})", name, closures[name])


# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    default_root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    parser.add_argument("--root", default=default_root, help="repository root (default: inferred)")
    parser.add_argument("--source", action="store_true", help="run rules 1, 2, 4, 5, 6 and 7 on the source tree")
    parser.add_argument("--nm", metavar="BUILD_DIR", help="run rules 3 and 4 on BUILD_DIR/fm2_libs.manifest")
    parser.add_argument("--config", help="multi-config build: read fm2_libs-<CONFIG>.manifest")
    parser.add_argument("--nm-tool", default="nm", help="nm executable (CTest passes CMAKE_NM)")
    args = parser.parse_args()
    if not args.source and not args.nm:
        parser.error("give --source, --nm BUILD_DIR or both")
    root = os.path.abspath(args.root)
    if not os.path.isdir(os.path.join(root, FM2)):
        parser.error(f"{root} has no {FM2}/")

    findings = Findings()
    counts = {rule: 0 for rule in RULE_NAMES}
    ran = set()
    if args.source:
        check_code_rules(root, findings, counts)
        check_link_source(root, findings, counts)
        check_todo_consistency(root, findings, counts)
        check_c1_freeze(root, findings, counts)
        ran |= {1, 2, 4, 5, 6, 7}
    if args.nm:
        check_nm(os.path.abspath(args.nm), args.config, args.nm_tool, findings, counts)
        ran |= {3, 4}

    for rule, where, message in sorted(findings.items, key=lambda item: (item[0], item[1])):
        print(f"FAIL [rule {rule}] {where}: {message}")
    for note in findings.notes:
        print(f"note: {note}")
    failed_rules = {rule for rule, _where, _message in findings.items}
    for rule in sorted(ran):
        status = "FAIL" if rule in failed_rules else "PASS"
        print(f"rule {rule} ({RULE_NAMES[rule]}): {status} ({counts[rule]} item(s) checked)")
    if findings.items:
        print(f"check_guards: {len(findings.items)} finding(s)")
        return 1
    print("check_guards: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
