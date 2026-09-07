// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

// Standalone, offline parser tests. Only public synthetic files are created;
// no wallet, journal, network, blockchain fixture or chainparams is loaded.
#include <node/finality_recovery_options.h>

#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>

namespace {
unsigned checks{0};
void Check(const bool passed, const char* message)
{
    ++checks;
    if (!passed) throw std::runtime_error{message};
}

std::string Hex(const char digit) { return std::string(64, digit); }
UniValue Manifest()
{
    UniValue value{UniValue::VOBJ};
    value.pushKV("version", 1);
    value.pushKV("chain_domain", Hex('1'));
    value.pushKV("validator_key", Hex('2'));
    value.pushKV("incident_height", 100);
    value.pushKV("incident_block_hash", Hex('3'));
    value.pushKV("incident_epoch", 2);
    value.pushKV("incident_signing_set_hash", Hex('4'));
    value.pushKV("incident_successor_set_hash", Hex('5'));
    value.pushKV("anchor_height", 120);
    value.pushKV("anchor_block_hash", Hex('a'));
    return value;
}

void Reject(const std::string& json, const std::string& ack = Hex('a'))
{
    Consensus::FinalitySignerRecovery output;
    output.anchor_height = 567;
    output.chain_domain = *uint256::FromHex(Hex('f'));
    std::string error;
    Check(!node::ParseFinalityRecoveryManifest(json, ack, output, error), "invalid manifest accepted");
    Check(!error.empty(), "failure lacks explanation");
    Check(output.anchor_height == 567 && output.chain_domain.GetHex() == Hex('f'), "failed parse modified output");
}

void ParseTests()
{
    const auto valid{Manifest()};
    Consensus::FinalitySignerRecovery output;
    std::string error;
    Check(node::ParseFinalityRecoveryManifest(valid.write(), Hex('A'), output, error), "valid manifest rejected");
    Check(output.Valid() && output.validator_key.has_value(), "parsed plan invalid or untargeted");
    Check(output.incident_height == 100 && output.anchor_height == 120 && output.incident_epoch == 2, "integer fields differ");
    Check(std::all_of(output.validator_key->begin(), output.validator_key->end(), [](auto byte) { return byte == 0x22; }), "validator byte order differs");
    Check(output.chain_domain.GetHex() == Hex('1') && output.anchor_block_hash.GetHex() == Hex('a'), "hash byte order differs");
    const std::string ordered_hex{"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"};
    auto ordered{valid};
    ordered.pushKV("validator_key", ordered_hex);
    ordered.pushKV("chain_domain", ordered_hex);
    ordered.pushKV("anchor_block_hash", ordered_hex);
    Check(node::ParseFinalityRecoveryManifest(ordered.write(), ordered_hex, output, error), "nonuniform key/hash manifest rejected");
    for (size_t i{0}; i < output.validator_key->size(); ++i) {
        Check((*output.validator_key)[i] == i, "validator identity byte order reversed");
        Check(output.chain_domain.data()[31 - i] == i, "RPC hash representation not reversed internally");
    }
    Check(output.chain_domain.GetHex() == ordered_hex && output.anchor_block_hash.GetHex() == ordered_hex, "nonuniform hash roundtrip differs");
    for (const auto& field : valid.getKeys()) {
        auto duplicate{valid};
        duplicate.pushKVEnd(field, valid[field]);
        Reject(duplicate.write());
        UniValue missing{UniValue::VOBJ};
        for (const auto& other : valid.getKeys()) if (field != other) missing.pushKVEnd(other, valid[other]);
        Reject(missing.write());
        auto wrong_type{valid};
        wrong_type.pushKV(field, UniValue{UniValue::VARR});
        Reject(wrong_type.write());
    }
    auto unknown{valid};
    unknown.pushKV("trust_me", true);
    Reject(unknown.write());
    Reject("{}");
    Reject("[]");
    Reject("null");
    Reject("");
    Reject(valid.write() + "{}");
    Reject(valid.write() + std::string(1, '\0'));
    Reject(std::string(node::MAX_FINALITY_RECOVERY_MANIFEST_BYTES + 1, ' '));
    for (const std::string field : {"version", "incident_height", "incident_epoch", "anchor_height"}) {
        for (const std::string raw : {"-1", "1.0", "1e0", "-0", "18446744073709551616"}) {
            auto modified{valid};
            UniValue number;
            number.setNumStr(raw);
            modified.pushKV(field, number);
            Reject(modified.write());
        }
        for (const UniValue& value : {UniValue{"1"}, UniValue{true}, UniValue{}}) {
            auto modified{valid};
            modified.pushKV(field, value);
            Reject(modified.write());
        }
    }
    for (const std::string field : {"version", "incident_height", "anchor_height"}) {
        auto modified{valid};
        modified.pushKV(field, uint64_t{2147483648});
        Reject(modified.write());
    }
    for (const int version : {0, 2}) {
        auto modified{valid};
        modified.pushKV("version", version);
        Reject(modified.write());
    }
    for (const std::string field : {"chain_domain", "validator_key", "incident_block_hash", "incident_signing_set_hash", "incident_successor_set_hash", "anchor_block_hash"}) {
        for (const std::string& value : {Hex('0'), Hex('g'), Hex('1').substr(1), "0x" + Hex('1'), Hex('1') + " ", " " + Hex('1')}) {
            auto modified{valid};
            modified.pushKV(field, value);
            Reject(modified.write());
        }
    }
    for (const int anchor : {0, 99, 100}) {
        auto modified{valid};
        modified.pushKV("anchor_height", anchor);
        Reject(modified.write());
    }
    auto equal_hash{valid};
    equal_hash.pushKV("anchor_block_hash", Hex('3'));
    Reject(equal_hash.write(), Hex('3'));
    for (const std::string& ack : {std::string{}, Hex('b'), Hex('0'), Hex('g'), "0x" + Hex('a')}) Reject(valid.write(), ack);
    auto boundaries{valid};
    boundaries.pushKV("incident_height", 2147483646);
    boundaries.pushKV("anchor_height", 2147483647);
    boundaries.pushKV("incident_epoch", std::numeric_limits<uint64_t>::max());
    Check(node::ParseFinalityRecoveryManifest(boundaries.write(), Hex('a'), output, error), "valid maximum integers rejected");
    Check(output.incident_epoch == std::numeric_limits<uint64_t>::max(), "epoch truncated");
    const auto padded{valid.write() + std::string(node::MAX_FINALITY_RECOVERY_MANIFEST_BYTES - valid.write().size(), ' ')};
    Check(node::ParseFinalityRecoveryManifest(padded, Hex('a'), output, error), "exact size limit rejected");
}

struct TemporaryDirectory {
    fs::path path;
    TemporaryDirectory()
    {
        std::random_device random;
        for (int i{0}; i < 16; ++i) {
            const auto candidate{fs::path{fs::temp_directory_path()} / fs::u8path(
                "b3-recovery-options-test-" + std::to_string(random()) + "-" + std::to_string(random()))};
            if (fs::create_directory(candidate)) { path = candidate; return; }
        }
        throw std::runtime_error{"cannot create isolated test directory"};
    }
    ~TemporaryDirectory()
    {
        // Only the exact newly-created random test directory; no user path.
        std::error_code ec;
        if (!path.empty()) fs::remove_all(path, ec);
    }
    void Write(const std::string& bytes) const
    {
        std::ofstream file{(path / "public-test.json").std_path(), std::ios::binary | std::ios::trunc};
        file.write(bytes.data(), bytes.size());
        Check(static_cast<bool>(file), "cannot write synthetic fixture");
    }
};

void LoadTests()
{
    TemporaryDirectory directory;
    const uint256 domain{*uint256::FromHex(Hex('1'))};
    std::optional<Consensus::FinalitySignerRecovery> output;
    std::string error;
    auto load = [&](std::vector<std::string> files, std::vector<std::string> anchors, const uint256& expected) {
        return node::LoadFinalityRecoveryOptions(files, anchors, directory.path, expected, output, error);
    };
    Check(load({}, {}, uint256{}) && !output, "default must be off without configured domain or file");
    Check(!load({"public-test.json"}, {}, domain), "missing acknowledgement accepted");
    Check(!load({}, {Hex('a')}, domain), "missing file option accepted");
    Check(!load({""}, {Hex('a')}, domain), "empty path accepted");
    Check(!load({"public-test.json", "public-test.json"}, {Hex('a')}, domain), "repeated file option accepted");
    Check(!load({"public-test.json"}, {Hex('a'), Hex('a')}, domain), "repeated acknowledgement accepted");
    Check(!load({"public-test.json"}, {Hex('a')}, domain), "missing file accepted");
    Check(!load({"."}, {Hex('a')}, domain), "directory accepted");
    directory.Write(Manifest().write());
    Check(load({"public-test.json"}, {Hex('a')}, domain) && output, "valid relative file rejected");
    Check(load({(directory.path / "public-test.json").utf8string()}, {Hex('a')}, domain), "valid absolute file rejected");
    Check(!load({"public-test.json"}, {Hex('a')}, uint256{}), "unconfigured domain accepted");
    const uint256 wrong_domain{*uint256::FromHex(Hex('f'))};
    Check(!load({"public-test.json"}, {Hex('a')}, wrong_domain), "wrong chain domain accepted");
    Check(!load({"public-test.json"}, {Hex('b')}, domain), "unacknowledged anchor accepted");
    Check(output && output->anchor_height == 120 && output->chain_domain == domain, "failed load replaced accepted output");
    for (const std::string& bytes : {std::string{}, std::string{"{}"}, std::string(node::MAX_FINALITY_RECOVERY_MANIFEST_BYTES + 1, ' ')}) {
        directory.Write(bytes);
        Check(!load({"public-test.json"}, {Hex('a')}, domain), "invalid file accepted");
        Check(output && output->anchor_height == 120, "invalid file changed output");
    }
    // Replacing the file does not change the already copied startup plan.
    Check(output && output->anchor_height == 120, "loaded plan unexpectedly follows file changes");
    Check(load({}, {}, domain) && !output, "ordinary startup did not clear optional plan");
}
} // namespace

int main(int argc, char**)
{
    if (argc != 1) return 2; // No real data paths or live inputs accepted.
    try {
        ParseTests();
        LoadTests();
        std::cout << "PASS: " << checks << " finality recovery option checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
