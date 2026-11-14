// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/core/facts.h>
#include <flowmesh2/dev/dev_chain_file.h>
#include <flowmesh2/netio/genesis_doc.h>
#include <util/fs.h>

#include <charconv>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

/** fm2-devchain: the regtest faucet and chain inspector (stage 2 step 5,
 * M5-NODE-01). DEV ONLY (fm2_dev): it appends scripted blocks to the shared
 * dev chain file every fm2d of the cluster tails (dev/dev_chain_file.h).
 *
 *   fm2-devchain --file <devchain.log> --genesis <genesis.json> deposit <account-hex> <asset-name> <amount> [...]
 *   fm2-devchain --file <devchain.log> list
 *
 * `deposit` appends one block at the next height holding one DepositFact per
 * triple (cursor (height, i), outpoint DevChainOutpoint(height, i)); `list`
 * prints every block. The deposit is a regtest convenience, not a chain
 * fact anyone verified: stage 3 replaces this file with the b3d adapter.
 */
namespace {

using namespace flowmesh2;

int Usage(std::string_view problem)
{
    std::cerr << "fm2-devchain: " << problem << "\n"
              << "usage: fm2-devchain --file <devchain.log> --genesis <genesis.json> deposit <account-hex> <asset> <amount> [...]\n"
              << "       fm2-devchain --file <devchain.log> list\n";
    return 2;
}

std::optional<int64_t> ParseI64(std::string_view text)
{
    int64_t value{0};
    const auto [end, error]{std::from_chars(text.data(), text.data() + text.size(), value)};
    if (error != std::errc{} || end != text.data() + text.size() || text.empty()) return std::nullopt;
    return value;
}

} // namespace

int main(int argc, char** argv)
{
    std::string file;
    std::string genesis_path;
    std::vector<std::string> rest;
    for (int i{1}; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--help" || arg == "-h") return Usage("help");
        if (arg == "--file" || arg == "--genesis") {
            if (i + 1 >= argc) return Usage("missing value for " + std::string{arg});
            (arg == "--file" ? file : genesis_path) = argv[++i];
        } else {
            rest.emplace_back(arg);
        }
    }
    if (file.empty() || rest.empty()) return Usage("--file and a command are required");
    const fs::path path{fs::PathFromString(file)};
    if (rest[0] == "list") {
        const Result<std::vector<dev::DevChainBlock>> blocks{dev::ReadDevChain(path)};
        if (!blocks.has_value()) {
            std::cerr << "fm2-devchain: " << blocks.error().detail << "\n";
            return 1;
        }
        for (const dev::DevChainBlock& block : *blocks) {
            std::cout << block.height << " " << block.hash.GetHex() << " facts=" << block.facts.size() << "\n";
        }
        return 0;
    }
    if (rest[0] != "deposit") return Usage("unknown command " + rest[0]);
    if (genesis_path.empty()) return Usage("--genesis is required for deposit");
    if ((rest.size() - 1) % 3 != 0 || rest.size() == 1) return Usage("deposit takes <account-hex> <asset> <amount> triples");
    std::ifstream in{genesis_path};
    if (!in) return Usage("cannot read " + genesis_path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    const Result<netio::GenesisDoc> doc{netio::GenesisDoc::Parse(buffer.str())};
    if (!doc.has_value()) {
        std::cerr << "fm2-devchain: genesis: " << doc.error().detail << "\n";
        return 1;
    }
    const Result<std::vector<dev::DevChainBlock>> existing{dev::ReadDevChain(path)};
    if (!existing.has_value()) {
        std::cerr << "fm2-devchain: " << existing.error().detail << "\n";
        return 1;
    }
    const uint32_t height{existing->empty() ? doc->base.height + 1 : existing->back().height + 1};
    dev::DevChainBlock block;
    block.height = height;
    block.hash = dev::DevChainBlockHash(height);
    for (size_t i{1}; i + 2 < rest.size(); i += 3) {
        const std::optional<uint256> account{uint256::FromHex(rest[i])};
        const std::optional<AssetId> asset{doc->Asset(rest[i + 1])};
        const std::optional<int64_t> amount{ParseI64(rest[i + 2])};
        if (!account.has_value()) return Usage("bad account hex " + rest[i]);
        if (!asset.has_value()) return Usage("unknown asset " + rest[i + 1]);
        if (!amount.has_value() || *amount <= 0) return Usage("bad amount " + rest[i + 2]);
        DepositFact fact;
        fact.cursor = FactCursor{height, static_cast<uint32_t>(block.facts.size())};
        fact.outpoint = dev::DevChainOutpoint(height, static_cast<uint32_t>(block.facts.size()));
        fact.account = AccountId{*account};
        fact.asset = *asset;
        fact.amount = *amount;
        block.facts.push_back(Fact{fact});
    }
    if (const Result<void> appended{dev::AppendDevChainBlock(path, block)}; !appended.has_value()) {
        std::cerr << "fm2-devchain: " << appended.error().detail << "\n";
        return 1;
    }
    std::cout << "fm2-devchain: appended block " << height << " with " << block.facts.size() << " deposit(s)\n";
    return 0;
}
