// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
// Small standalone regression: no node, wallet, real keys, network or signing.
#include <node/flowmesh_keyfile.h>

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
void Require(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error{message};
}
constexpr std::array<unsigned char, 32> DATA{0x42, 0x33, 0x46, 0x4d, 0x00, 0x0a, 0x1a, 0xff};

void ReadSame(const std::filesystem::path& path)
{
    FILE* file{node::detail::OpenFlowMeshKeyFile(path, false)};
    Require(file != nullptr, "read existing identity failed");
    std::array<unsigned char, DATA.size()> bytes{};
    const bool same{std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size() &&
                    bytes == DATA && std::fgetc(file) == EOF && !std::ferror(file)};
    const bool closed{std::fclose(file) == 0};
    Require(same && closed, "existing bytes changed or file was extended");
}
void CheckPath(const std::filesystem::path& path)
{
    FILE* file{node::detail::OpenFlowMeshKeyFile(path, true)};
    Require(file != nullptr, "fresh exclusive creation failed");
    const bool written{std::fwrite(DATA.data(), 1, DATA.size(), file) == DATA.size()};
    const bool closed{std::fclose(file) == 0};
    Require(written && closed, "test write/close failed");
    ReadSame(path);
    FILE* duplicate{node::detail::OpenFlowMeshKeyFile(path, true)};
    const int duplicate_errno{errno};
    if (duplicate) std::fclose(duplicate);
    Require(!duplicate && duplicate_errno == EEXIST, "exclusive creation overwrote an existing file");
    ReadSame(path);
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2 || argc > 3) return 2;
    try {
        const std::string_view root_text{argv[1]};
        const std::filesystem::path root{std::u8string{root_text.begin(), root_text.end()}};
        const std::string_view mode{argc == 3 ? argv[2] : ""};
        if (mode == "--reopen") {
            ReadSame(root / "operator.key");
            std::cout << "PASS separate-process reopen retains exact bytes\n";
            return 0;
        }
        Require(!std::filesystem::exists(root), "use a fresh isolated test directory");
        Require(std::filesystem::create_directories(root), "test directory creation failed");
        if (mode == "--legacy") {
            // The exact former source operation, using the same MSVCRT as
            // the distributed MinGW binary. Failure is evidence, not a pass.
            const auto path{root / "operator.key"};
            const auto utf8{path.u8string()};
            const std::string filename{utf8.begin(), utf8.end()};
            FILE* file{std::fopen(filename.c_str(), "wbx")};
            if (!file) {
                std::cout << "LEGACY first_create_failed errno=" << errno << '\n';
                return 11;
            }
            std::fclose(file);
            std::cout << "LEGACY first_create_succeeded (failure not reproduced)\n";
            return 0;
        }
        Require(mode.empty(), "unknown test mode");
        CheckPath(root / "operator.key");
        std::cout << "PASS first creation, exact binary read, existing-file refusal\n";
        const auto unicode_dir{root / std::filesystem::path{u8"identity_\u20bf_\u4e2d"}};
        Require(std::filesystem::create_directory(unicode_dir), "Unicode test directory creation failed");
        CheckPath(unicode_dir / "operator.key");
        std::cout << "PASS helper Unicode path creation/read (not whole-datadir qualification)\n";
        const auto empty{root / "partial.key"};
        FILE* partial{node::detail::OpenFlowMeshKeyFile(empty, true)};
        Require(partial != nullptr, "partial-file fixture creation failed");
        Require(std::fclose(partial) == 0, "partial-file fixture close failed");
        FILE* replacement{node::detail::OpenFlowMeshKeyFile(empty, true)};
        if (replacement) std::fclose(replacement);
        Require(!replacement && std::filesystem::file_size(empty) == 0, "partial file silently replaced");
        FILE* directory{node::detail::OpenFlowMeshKeyFile(unicode_dir, true)};
        if (directory) std::fclose(directory);
        Require(!directory && std::filesystem::is_directory(unicode_dir), "directory replaced");
        FILE* missing{node::detail::OpenFlowMeshKeyFile(root / "missing-parent" / "operator.key", true)};
        if (missing) std::fclose(missing);
        Require(!missing, "missing-parent error was hidden");
        std::cout << "PASS partial-file/directory preservation and missing-parent refusal\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << " errno=" << errno << '\n';
        return 1;
    }
}
