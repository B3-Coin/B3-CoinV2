// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** The driver of bin/fm2_fuzz (M0-FUZ-01): the registry behind the
 * repository's FUZZ_TARGET macro (test/fuzz/fuzz.h), for flowmesh2 targets
 * only. It does not link the node's test_fuzz library, which pulls in
 * bitcoin_node and with it the V1 code an fm2 executable must not define
 * (check_guards.py rule 3).
 *
 * FUZZ=<target> selects the target. With libFuzzer (SANITIZERS=fuzzer,...
 * under Clang) the libFuzzer flags apply. Without it, the arguments are
 * input files or directories whose files are each run once, or stdin when
 * there are none: a corpus replay, for GCC builds.
 */

#include <test/fuzz/fuzz.h>
#include <util/translation.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <vector>

//! No translations: the fm2 libraries link bitcoin_util, which needs this hook.
const TranslateFn G_TRANSLATION_FUN{nullptr};

namespace {
struct Registered {
    TypeTestOneInput test;
    FuzzTargetOptions options;
};
std::map<std::string_view, Registered>& Targets()
{
    static std::map<std::string_view, Registered> targets;
    return targets;
}
const TypeTestOneInput* g_selected{nullptr};

void Select()
{
    const char* name{std::getenv("FUZZ")};
    const auto it{name ? Targets().find(name) : Targets().end()};
    if (it == Targets().end()) {
        std::fprintf(stderr, "set FUZZ to one of:\n");
        for (const auto& [target, registered] : Targets()) {
            if (!registered.options.hidden) std::fprintf(stderr, "%.*s\n", int(target.size()), target.data());
        }
        std::exit(EXIT_FAILURE);
    }
    it->second.options.init();
    g_selected = &it->second.test;
}

#ifdef FM2_FUZZ_PROVIDE_MAIN
void RunFile(const std::filesystem::path& path)
{
    std::ifstream file{path, std::ios::binary};
    const std::vector<uint8_t> input{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    (*g_selected)(FuzzBufferType{input});
}
#endif
} // namespace

void FuzzFrameworkRegisterTarget(std::string_view name, TypeTestOneInput target, FuzzTargetOptions opts)
{
    if (!Targets().try_emplace(name, Registered{std::move(target), std::move(opts)}).second) {
        std::fprintf(stderr, "fuzz target %.*s registered twice\n", int(name.size()), name.data());
        std::abort();
    }
}

extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/)
{
    Select();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    (*g_selected)(FuzzBufferType{data, size});
    return 0;
}

#ifdef FM2_FUZZ_PROVIDE_MAIN
int main(int argc, char** argv)
{
    Select();
    if (argc < 2) {
        const std::vector<uint8_t> input{std::istreambuf_iterator<char>{std::cin.rdbuf()}, std::istreambuf_iterator<char>{}};
        (*g_selected)(FuzzBufferType{input});
        return 0;
    }
    for (int i{1}; i < argc; ++i) {
        const std::filesystem::path path{argv[i]};
        if (std::filesystem::is_directory(path)) {
            for (const auto& entry : std::filesystem::directory_iterator{path}) {
                if (entry.is_regular_file()) RunFile(entry.path());
            }
        } else {
            RunFile(path);
        }
    }
    return 0;
}
#endif
