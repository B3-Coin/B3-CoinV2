// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** fm2-trace: run the real engine on a batch of slots and print what it
 * produced, for the independent checker of M3-ENG-05
 * (contrib/flowmesh2/checker). PROVISIONAL REGTEST profile only.
 *
 *   fm2-trace BATCH.json          prints fm2-trace-result/1 JSON on stdout
 *   fm2-trace --export-skeleton   runs the walking skeleton (seed 1) and prints
 *                                 its committed slots as a batch (O-87)
 *
 * The batch (fm2-trace-batch/1) names the genesis (domain, authority, anchor,
 * fact cursor, markets) and a list of slots, each with its anchor, fact
 * range, fact bytes, original action bytes and the certificate hash its head
 * carries. Every slot is built on the last head through state::BuildCandidate,
 * the path the state owner uses; a refused slot leaves the head unchanged.
 * Hashes are hex of the stored bytes (hashlib order), never GetHex. Unsigned
 * 64-bit values are decimal strings.
 */

#include <flowmesh2/core/params.h>
#include <flowmesh2/engine/engine.h>
#include <flowmesh2/sim/scenario.h>
#include <flowmesh2/state/candidate.h>
#include <key.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <util/translation.h>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace flowmesh2;

const TranslateFn G_TRANSLATION_FUN{nullptr};

namespace {

[[noreturn]] void Fail(const std::string& what) { throw std::runtime_error(what); }

std::vector<unsigned char> Bytes(const UniValue& v, const char* what)
{
    if (!v.isStr() || (!v.get_str().empty() && !IsHex(v.get_str()))) Fail(std::string{"bad hex in "} + what);
    return ParseHex(v.get_str());
}

uint256 Hash(const UniValue& v, const char* what)
{
    const std::vector<unsigned char> bytes{Bytes(v, what)};
    if (bytes.size() != 32) Fail(std::string{"not 32 bytes: "} + what);
    return uint256{std::span<const unsigned char, 32>{bytes.data(), 32}};
}

uint64_t U64(const UniValue& v, const char* what)
{
    if (!v.isStr()) Fail(std::string{"u64 must be a decimal string: "} + what);
    const std::optional<uint64_t> n{ToIntegral<uint64_t>(v.get_str())};
    if (!n) Fail(std::string{"bad u64: "} + what);
    return *n;
}

uint32_t U32(const UniValue& v, const char* what)
{
    const uint64_t n{U64(v, what)};
    if (n > UINT32_MAX) Fail(std::string{"u32 out of range: "} + what);
    return static_cast<uint32_t>(n);
}

Anchor AnchorOf(const UniValue& v) { return Anchor{U32(v["height"], "anchor height"), Hash(v["hash"], "anchor hash")}; }

FactCursor CursorOf(const UniValue& v)
{
    if (!v.isArray() || v.size() != 2) Fail("a cursor is [height, index]");
    return FactCursor{U32(v[0], "cursor height"), U32(v[1], "cursor index")};
}

std::vector<std::vector<unsigned char>> ByteList(const UniValue& v, const char* what)
{
    if (!v.isArray()) Fail(std::string{"not an array: "} + what);
    std::vector<std::vector<unsigned char>> out;
    for (size_t i{0}; i < v.size(); ++i) out.push_back(Bytes(v[i], what));
    return out;
}

std::string Hex(const uint256& h) { return HexStr(h); }

template <class T>
T Must(Result<T> r, const char* what)
{
    if (!r.has_value()) Fail(std::string{what} + ": " + std::string{ToString(r.error().code)} + " " + std::string{r.error().detail});
    return std::move(r).value();
}

UniValue Roots(const state::CoreState& s)
{
    const StateRootsV0 roots{Must(s.engine.Commit(), "commit")};
    UniValue out{UniValue::VOBJ};
    out.pushKV("accounts", Hex(roots.accounts));
    out.pushKV("orders", Hex(roots.orders));
    out.pushKV("markets", Hex(roots.markets));
    out.pushKV("nullifiers", Hex(roots.nullifiers));
    out.pushKV("totals", Hex(roots.totals));
    out.pushKV("state", Hex(roots.Root()));
    return out;
}

UniValue OrderLeaves(const state::CoreState& s)
{
    UniValue out{UniValue::VARR};
    for (const RestingOrderLeafV0& leaf : s.engine.OrderLeaves()) out.push_back(HexStr(leaf.Encode()));
    return out;
}

UniValue AccountLeaves(const state::CoreState& s)
{
    UniValue out{UniValue::VARR};
    for (const auto& [account, unused] : s.engine.ledger.Accounts()) {
        out.push_back(HexStr(Must(s.engine.ledger.Leaf(account), "account leaf").Encode()));
    }
    return out;
}

UniValue Run(const UniValue& batch)
{
    if (batch["format"].get_str() != "fm2-trace-batch/1") Fail("unknown batch format");
    const ProfileParams profile{Must(DevRegtestProfile(ChainType::REGTEST), "profile")};
    const engine::Engine engine{Must(engine::Engine::Create(profile), "engine")};

    std::vector<MarketDescriptor> markets;
    const UniValue& ms{batch["markets"]};
    for (size_t i{0}; i < ms.size(); ++i) {
        markets.push_back(MarketDescriptor{AssetId{Hash(ms[i]["base"], "base")}, AssetId{Hash(ms[i]["quote"], "quote")},
                                           U64(ms[i]["lot"], "lot"), U64(ms[i]["tick"], "tick")});
    }
    const AuthorityVersion authority{U64(batch["authority_epoch"], "authority_epoch"), Hash(batch["seat_set_root"], "seat_set_root"),
                                     ProfileHash(profile)};
    state::HeadView head{Must(state::GenesisHead(profile, markets, Hash(batch["domain"], "domain"), authority,
                                                 AnchorOf(batch["genesis_anchor"]), CursorOf(batch["facts_start"])),
                              "genesis")};

    UniValue result{UniValue::VOBJ};
    result.pushKV("format", "fm2-trace-result/1");
    UniValue genesis{UniValue::VOBJ};
    genesis.pushKV("header", HexStr(head.Header().Encode()));
    genesis.pushKV("hash", Hex(head.HeaderHash()));
    genesis.pushKV("roots", Roots(head.State()));
    result.pushKV("genesis", std::move(genesis));

    UniValue slots{UniValue::VARR};
    const UniValue& ss{batch["slots"]};
    for (size_t i{0}; i < ss.size(); ++i) {
        const UniValue& s{ss[i]};
        state::SlotInputs inputs;
        inputs.authority = authority;
        inputs.anchor = AnchorOf(s["anchor"]);
        inputs.facts_from = CursorOf(s["facts_from"]);
        inputs.facts_to = CursorOf(s["facts_to"]);
        inputs.body.facts = ByteList(s["facts"], "facts");
        inputs.body.originals = ByteList(s["originals"], "originals");

        UniValue out{UniValue::VOBJ};
        Result<state::Candidate> candidate{state::BuildCandidate(engine, head, inputs)};
        if (!candidate.has_value()) {
            out.pushKV("ok", false);
            out.pushKV("error", std::string{ToString(candidate.error().code)});
            out.pushKV("detail", std::string{candidate.error().detail});
            slots.push_back(std::move(out));
            continue;
        }
        out.pushKV("ok", true);
        out.pushKV("header", HexStr(candidate->Header().Encode()));
        out.pushKV("hash", Hex(candidate->Header().Hash()));
        UniValue originals{UniValue::VARR};
        for (const auto& original : candidate->Body().originals) originals.push_back(HexStr(original));
        out.pushKV("originals", std::move(originals));
        UniValue outcomes{UniValue::VARR};
        for (const OutcomeV2& outcome : candidate->Outcomes()) outcomes.push_back(HexStr(outcome.Encode()));
        out.pushKV("outcomes", std::move(outcomes));
        out.pushKV("roots", Roots(*candidate->Next()));
        out.pushKV("orders", OrderLeaves(*candidate->Next()));
        out.pushKV("accounts", AccountLeaves(*candidate->Next()));
        slots.push_back(std::move(out));
        head = Must(state::HeadView::Make(candidate->Header(), Hash(s["cert_hash"], "cert_hash"), candidate->Next()), "head");
    }
    result.pushKV("slots", std::move(slots));
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: fm2-trace BATCH.json | fm2-trace --export-skeleton\n";
        return 2;
    }
    try {
        if (std::string_view{argv[1]} == "--export-skeleton") {
            ECC_Context ecc_context; // the skeleton's clients sign (as in fm2-sim)
            const sim::RunReport report{sim::RunWalkingSkeleton(1, sim::Variant::WALKING_SKELETON)};
            if (!report.Passed()) Fail("the walking skeleton did not pass: " + report.harness_error);
            std::cout << report.replay_batch << '\n';
            return 0;
        }
        std::ifstream in{argv[1], std::ios::binary};
        if (!in) Fail(std::string{"cannot read "} + argv[1]);
        const std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        UniValue batch;
        if (!batch.read(text)) Fail("batch is not JSON");
        std::cout << Run(batch).write(1) << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "fm2-trace: " << e.what() << '\n';
        return 2;
    }
}
