// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

// b3-bridge-regtest-certify: REGTEST ONLY bridge V2 certificate tool
// (regtest plan section 3.2; node changes rev 11 sections 7-8) over the
// public deterministic test keys of bridge/regtest_certify.h. It emits the
// K_OF_N 3-of-4 bootstrap attestation for B3FinalityVerifierV2.initialize,
// bound to the verifier address, the EVM chain id and START_SET_HASH (open
// issue OI-12, owner ruling R9), and digest-V2 certificates from labelled
// test committees for the forged and conflicting-certificate drills. It
// refuses every chain domain but the -b3modernregtest one (the B3 mainnet
// domain by name) and every EVM chain id but 31337, and never reads a
// secret key. No network access. NOT part of the node; test tooling only
// (option B3_BRIDGE_TOOLS).
//
// Commands (JSON on stdout; any failure prints "error: ..." on stderr and
// exits 1):
//   bootstrap-set
//       The bootstrap committee and the DeployV2.s.sol K_OF_N env values.
//   keys --label L --count N [--with-secrets]
//       Test keys 0..N-1 of label L (public keys and PoPs; the secrets, in
//       importfinalitykey's format, only with --with-secrets).
//   bootstrap <input.json>
//       {"chain_domain", "evm_chain_id", "verifier", "start_set_hash",
//        "fb_encoded" (185-byte FinalizedBlockV2), optional "signers"}
//       -> the bootstrap digest, header, bitmap and BlsDigestProver proof.
//   certify <input.json>
//       {"chain_domain", "fb_encoded", "label", "weights", optional
//        "signers", optional "expected_set_hash"}
//       -> a digest-V2 certificate: B3 carrier payload, prover proof and
//          the kind 13 FINALITY_CONFLICT record (finality_conflict_record,
//          submitbridgecarrier's input; node changes section 8.5).
// Hashes and addresses are 0x-prefixed raw (EVM byte order) hex.

#include <bridge/bootstrap_v2.h>
#include <bridge/regtest_certify.h>
#include <uint256.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <util/string.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace bridge;
using namespace bridge::regtest_certify;

namespace {

constexpr std::string_view USAGE{
    "usage: b3-bridge-regtest-certify bootstrap-set\n"
    "       b3-bridge-regtest-certify keys --label L --count N [--with-secrets]\n"
    "       b3-bridge-regtest-certify bootstrap <input.json>\n"
    "       b3-bridge-regtest-certify certify <input.json>"};

UniValue LoadJson(const std::string& path)
{
    std::ifstream file{path};
    if (!file) throw std::runtime_error("cannot open " + path);
    std::ostringstream text;
    text << file.rdbuf();
    UniValue value;
    if (!value.read(text.str()) || !value.isObject()) {
        throw std::runtime_error("expected one JSON object in " + path);
    }
    return value;
}

//! Inputs are strict, so a misspelled optional field can never be ignored.
void CheckKeys(const UniValue& object, std::initializer_list<std::string_view> allowed)
{
    for (const std::string& key : object.getKeys()) {
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            throw std::runtime_error("unknown input key " + key);
        }
    }
}

const UniValue& Field(const UniValue& object, const std::string& name)
{
    const UniValue& value{object.find_value(name)};
    if (value.isNull()) throw std::runtime_error("missing field: " + name);
    return value;
}

std::vector<unsigned char> Data(const UniValue& object, const std::string& name)
{
    const UniValue& value{Field(object, name)};
    if (!value.isStr()) throw std::runtime_error(name + " must be 0x-prefixed hex");
    const std::string_view text{value.get_str()};
    if (!text.starts_with("0x")) throw std::runtime_error(name + " must be 0x-prefixed hex");
    const auto bytes{TryParseHex<unsigned char>(text.substr(2))};
    if (!bytes) throw std::runtime_error(name + " must be 0x-prefixed hex");
    return *bytes;
}

uint256 HashField(const UniValue& object, const std::string& name)
{
    const auto bytes{Data(object, name)};
    if (bytes.size() != 32) throw std::runtime_error(name + " must be 32 bytes");
    return uint256{std::span<const unsigned char>{bytes}};
}

EthAddress AddressField(const UniValue& object, const std::string& name)
{
    const auto bytes{Data(object, name)};
    if (bytes.size() != 20) throw std::runtime_error(name + " must be 20 bytes");
    EthAddress out;
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return out;
}

uint64_t Number(const UniValue& value, const std::string& name)
{
    if (!value.isNum()) throw std::runtime_error(name + " must be a non-negative integer");
    try {
        return value.getInt<uint64_t>();
    } catch (const std::exception&) {
        throw std::runtime_error(name + " must be a non-negative integer");
    }
}

std::vector<uint64_t> Numbers(const UniValue& object, const std::string& name)
{
    const UniValue& value{Field(object, name)};
    if (!value.isArray()) throw std::runtime_error(name + " must be an array");
    std::vector<uint64_t> out;
    out.reserve(value.size());
    for (size_t i{0}; i < value.size(); ++i) out.push_back(Number(value[i], name + "[]"));
    return out;
}

std::optional<std::vector<uint32_t>> Signers(const UniValue& object)
{
    if (object.find_value("signers").isNull()) return std::nullopt;
    std::vector<uint32_t> out;
    for (const uint64_t index : Numbers(object, "signers")) {
        if (index > std::numeric_limits<uint32_t>::max()) {
            throw std::runtime_error("signer index " + util::ToString(index) + " is out of range");
        }
        out.push_back(static_cast<uint32_t>(index));
    }
    return out;
}

modern::FinalizedBlockV2 FinalizedBlock(const UniValue& object)
{
    const auto bytes{Data(object, "fb_encoded")};
    if (bytes.size() != modern::FinalizedBlockV2::SIZE) throw std::runtime_error("fb_encoded must be 185 bytes");
    const auto fb{modern::FinalizedBlockV2::Decode(bytes)};
    if (!fb) throw std::runtime_error("fb_encoded has unknown bridge-flag bits");
    return *fb;
}

UniValue SetHeaderJson(const modern::ValidatorSetHeader& header, const uint256& set_hash)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("epoch", header.epoch);
    out.pushKV("ruleset_version", header.ruleset_version);
    out.pushKV("validator_count", header.validator_count);
    out.pushKV("total_weight", header.total_weight);
    out.pushKV("quorum_weight", header.quorum_weight);
    out.pushKV("aggregate_pubkey", EvmHex(header.aggregate_pubkey));
    out.pushKV("members_root", EvmHex(header.members_root));
    out.pushKV("set_hash", EvmHex(set_hash));
    out.pushKV("encoded_header", EvmHex(header.Encode()));
    return out;
}

UniValue FinalizedBlockJson(const modern::FinalizedBlockV2& fb)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("lineage_id", EvmHex(fb.lineage_id));
    out.pushKV("height", fb.height);
    out.pushKV("block_hash_evm", EvmHex(fb.block_hash));
    out.pushKV("withdrawal_root", EvmHex(fb.withdrawal_root));
    out.pushKV("validator_set_hash", EvmHex(fb.validator_set_hash));
    out.pushKV("epoch", fb.epoch);
    out.pushKV("checkpoint_index", fb.checkpoint_index);
    out.pushKV("checkpoint_history_root", EvmHex(fb.checkpoint_history_root));
    out.pushKV("bridge_flags", fb.bridge_flags);
    out.pushKV("encoded", EvmHex(fb.Encode()));
    return out;
}

UniValue IndexArray(const std::vector<uint32_t>& indices)
{
    UniValue out{UniValue::VARR};
    for (const uint32_t index : indices) out.push_back(index);
    return out;
}

void PushSigned(UniValue& out, const SignedDigest& signed_digest)
{
    out.pushKV("signers", IndexArray(signed_digest.signers));
    std::vector<uint32_t> absent;
    for (const BootstrapAbsentWitness& witness : signed_digest.absent) absent.push_back(witness.index);
    out.pushKV("absent_indices", IndexArray(absent));
    out.pushKV("signer_bitmap", EvmHex(signed_digest.bitmap));
    out.pushKV("signed_weight", signed_digest.signed_weight);
    out.pushKV("aggregate_signature", EvmHex(signed_digest.aggregate_signature.Compressed()));
    out.pushKV("proof", EvmHex(signed_digest.proof));
}

UniValue MemberJson(const TestCommittee& committee, const uint32_t index)
{
    const TestKey& key{committee.keys[index]};
    UniValue out{UniValue::VOBJ};
    out.pushKV("index", index);
    out.pushKV("pubkey", EvmHex(key.pubkey.Compressed()));
    out.pushKV("weight", committee.weights[index]);
    out.pushKV("pop", EvmHex(key.pop.Compressed()));
    out.pushKV("leaf", EvmHex(committee.leaves[index]));
    return out;
}

UniValue BootstrapSet()
{
    const TestCommittee committee{BootstrapCommittee()};
    UniValue env{UniValue::VOBJ};
    env.pushKV("B3V2_BOOTSTRAP_MODE", 0); // K_OF_N
    env.pushKV("B3V2_BOOTSTRAP_AGGREGATE_PUBKEY", EvmHex(committee.header.aggregate_pubkey));
    env.pushKV("B3V2_BOOTSTRAP_MEMBERS_ROOT", EvmHex(committee.header.members_root));
    env.pushKV("B3V2_BOOTSTRAP_SET_HASH", EvmHex(committee.set_hash));
    UniValue members{UniValue::VARR};
    for (uint32_t i{0}; i < committee.Size(); ++i) members.push_back(MemberJson(committee, i));
    UniValue out{UniValue::VOBJ};
    out.pushKV("label", committee.label);
    out.pushKV("env", std::move(env));
    out.pushKV("signing_set", SetHeaderJson(committee.header, committee.set_hash));
    out.pushKV("members", std::move(members));
    out.pushKV("test_keys_only", true);
    return out;
}

UniValue Keys(const std::vector<std::string>& args)
{
    std::optional<std::string> label;
    std::optional<uint32_t> count;
    bool with_secrets{false};
    for (size_t i{0}; i < args.size(); ++i) {
        if (args[i] == "--with-secrets") {
            with_secrets = true;
        } else if ((args[i] == "--label" || args[i] == "--count") && i + 1 < args.size()) {
            if (args[i] == "--label") {
                label = args[++i];
            } else {
                count = ToIntegral<uint32_t>(args[++i]);
                if (!count) throw std::runtime_error("--count must be an integer");
            }
        } else {
            throw std::runtime_error("unknown or incomplete keys argument " + args[i]);
        }
    }
    if (!label || !count) throw std::runtime_error("keys needs --label and --count");
    CheckLabel(*label);
    if (*count == 0 || *count > modern::MAX_FINALITY_SET) {
        throw std::runtime_error("--count must be in 1.." + util::ToString(modern::MAX_FINALITY_SET));
    }
    UniValue keys{UniValue::VARR};
    for (uint32_t i{0}; i < *count; ++i) {
        const TestKey key{DeriveTestKey(*label, i)};
        UniValue entry{UniValue::VOBJ};
        entry.pushKV("index", i);
        entry.pushKV("pubkey", EvmHex(key.pubkey.Compressed()));
        entry.pushKV("pop", EvmHex(key.pop.Compressed()));
        // importfinalitykey's format: 32 big-endian bytes, plain hex.
        if (with_secrets) entry.pushKV("blssecret", HexStr(key.secret.Bytes()));
        keys.push_back(std::move(entry));
    }
    UniValue out{UniValue::VOBJ};
    out.pushKV("label", *label);
    out.pushKV("keys", std::move(keys));
    out.pushKV("test_keys_only", true);
    return out;
}

UniValue Bootstrap(const UniValue& input)
{
    CheckKeys(input, {"chain_domain", "evm_chain_id", "verifier", "start_set_hash", "fb_encoded", "signers"});
    // Refuse a non-regtest domain or chain before reading anything else.
    const uint256 chain_domain{HashField(input, "chain_domain")};
    CheckRegtestDomain(chain_domain);
    const uint64_t evm_chain_id{Number(Field(input, "evm_chain_id"), "evm_chain_id")};
    CheckRegtestEvmChainId(evm_chain_id);
    const EthAddress verifier{AddressField(input, "verifier")};
    const uint256 start_set_hash{HashField(input, "start_set_hash")};
    const modern::FinalizedBlockV2 fb{FinalizedBlock(input)};
    const KofNAttestation a{
        BuildKofNAttestation(chain_domain, evm_chain_id, verifier, start_set_hash, fb, Signers(input))};

    UniValue out{UniValue::VOBJ};
    out.pushKV("mode", "K_OF_N");
    out.pushKV("chain_domain", EvmHex(chain_domain));
    out.pushKV("evm_chain_id", evm_chain_id);
    out.pushKV("verifier", EvmHex(verifier));
    out.pushKV("start_set_hash", EvmHex(start_set_hash));
    out.pushKV("fb", FinalizedBlockJson(fb));
    out.pushKV("message", EvmHex(a.message));
    out.pushKV("digest", EvmHex(a.digest));
    out.pushKV("signing_set", SetHeaderJson(a.committee.header, a.committee.set_hash));
    PushSigned(out, a.signed_digest);
    out.pushKV("test_keys_only", true);
    return out;
}

UniValue Certify(const UniValue& input)
{
    CheckKeys(input, {"chain_domain", "fb_encoded", "label", "weights", "signers", "expected_set_hash"});
    const uint256 chain_domain{HashField(input, "chain_domain")};
    CheckRegtestDomain(chain_domain);
    const modern::FinalizedBlockV2 fb{FinalizedBlock(input)};
    const UniValue& label{Field(input, "label")};
    if (!label.isStr()) throw std::runtime_error("label must be a string");
    std::optional<uint256> expected_set_hash;
    if (!input.find_value("expected_set_hash").isNull()) expected_set_hash = HashField(input, "expected_set_hash");
    const TestCertificate c{BuildTestCertificate(chain_domain, fb, label.get_str(), Numbers(input, "weights"),
                                                 Signers(input), expected_set_hash)};

    UniValue out{UniValue::VOBJ};
    out.pushKV("label", c.committee.label);
    out.pushKV("chain_domain", EvmHex(chain_domain));
    out.pushKV("fb", FinalizedBlockJson(fb));
    out.pushKV("digest_v2", EvmHex(c.digest));
    out.pushKV("cert_id", EvmHex(c.cert_id));
    out.pushKV("signing_set", SetHeaderJson(c.committee.header, c.committee.set_hash));
    out.pushKV("expected_set_hash_checked", expected_set_hash.has_value());
    PushSigned(out, c.signed_digest);
    out.pushKV("payload", EvmHex(c.payload));
    out.pushKV("finality_conflict_record", EvmHex(FinalityConflictRecord(c)));
    out.pushKV("test_keys_only", true);
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const std::vector<std::string> args(argv + std::min(argc, 1), argv + argc);
        if (args.empty()) throw std::runtime_error(std::string{USAGE});
        const std::string& command{args[0]};
        UniValue result;
        if (command == "bootstrap-set" && args.size() == 1) {
            result = BootstrapSet();
        } else if (command == "keys") {
            result = Keys({args.begin() + 1, args.end()});
        } else if (command == "bootstrap" && args.size() == 2) {
            result = Bootstrap(LoadJson(args[1]));
        } else if (command == "certify" && args.size() == 2) {
            result = Certify(LoadJson(args[1]));
        } else {
            throw std::runtime_error(std::string{USAGE});
        }
        std::fputs((result.write() + "\n").c_str(), stdout);
        return 0;
    } catch (const std::exception& e) {
        std::fputs(("error: " + std::string{e.what()} + "\n").c_str(), stderr);
        return 1;
    }
}
