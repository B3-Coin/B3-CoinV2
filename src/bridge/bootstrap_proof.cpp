// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

// b3-bridge-bootstrap-proof: deterministic offline builder for the one-time
// B3FinalityVerifier deployment and initialize transaction. Before M it turns
// a four-member public manifest into deterministic deployment pins. At M it
// additionally consumes exactly three signbridgebootstrap JSON packages and
// emits initialize calldata. No private key, Ethereum RPC, B3 RPC, or
// deployment authority is used by this tool.

#include <bridge/bootstrap_proof.h>

#include <pubkey.h>
#include <univalue.h>
#include <util/strencodings.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

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

const UniValue& Field(const UniValue& object, const char* name)
{
    const UniValue& value{object.find_value(name)};
    if (value.isNull()) throw std::runtime_error(std::string{"missing field: "} + name);
    return value;
}

std::vector<unsigned char> HexField(const UniValue& object, const char* name,
                                    const size_t expected)
{
    const UniValue& value{Field(object, name)};
    if (!value.isStr()) throw std::runtime_error(std::string{name} + " must be hex text");
    std::string text{value.get_str()};
    if (text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0) text.erase(0, 2);
    if (!IsHex(text)) throw std::runtime_error(std::string{name} + " is not canonical hex");
    const auto bytes{ParseHex(text)};
    if (bytes.size() != expected) {
        throw std::runtime_error(std::string{name} + " must be " +
                                 std::to_string(expected) + " bytes");
    }
    return bytes;
}

template <size_t N>
std::array<unsigned char, N> FixedHex(const UniValue& object, const char* name)
{
    const auto bytes{HexField(object, name, N)};
    std::array<unsigned char, N> out{};
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return out;
}

uint256 HashField(const UniValue& object, const char* name)
{
    const auto bytes{FixedHex<32>(object, name)};
    uint256 out;
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return out;
}

uint64_t NumberField(const UniValue& object, const char* name)
{
    const UniValue& value{Field(object, name)};
    if (!value.isNum()) throw std::runtime_error(std::string{name} + " must be a number");
    return value.getInt<uint64_t>();
}

bls::PublicKey PublicKeyField(const UniValue& object, const char* name)
{
    const auto bytes{FixedHex<bls::PUBKEY_SIZE>(object, name)};
    const auto key{bls::PublicKey::Decode(bytes)};
    if (!key) throw std::runtime_error(std::string{name} + " is not a canonical BLS public key");
    return *key;
}

bls::Signature SignatureField(const UniValue& object, const char* name)
{
    const auto bytes{FixedHex<bls::SIGNATURE_SIZE>(object, name)};
    const auto signature{bls::Signature::Decode(bytes)};
    if (!signature) throw std::runtime_error(std::string{name} + " is not a canonical BLS signature");
    return *signature;
}

bls::Signature SignatureFieldEither(const UniValue& object,
                                    const char* preferred,
                                    const char* compatibility)
{
    return object.find_value(preferred).isNull()
               ? SignatureField(object, compatibility)
               : SignatureField(object, preferred);
}

std::string EthHex(std::span<const unsigned char> bytes)
{
    return "0x" + HexStr(bytes);
}

std::string EthHex(const uint256& value)
{
    return EthHex(std::span<const unsigned char>{value.begin(), 32});
}

bridge::BootstrapSignaturePackage ParsePackage(const UniValue& object)
{
    const uint64_t raw_binding_seq{NumberField(object, "binding_seq")};
    if (raw_binding_seq > UINT32_MAX) {
        throw std::runtime_error("binding_seq exceeds uint32");
    }
    bridge::BootstrapSignaturePackage package{
        FixedHex<32>(object, "validator_key"),
        PublicKeyField(object, "bls_pubkey"),
        {}, {}, HashField(object, "digest"),
        SignatureField(object, "signature"),
        static_cast<uint32_t>(raw_binding_seq),
        NumberField(object, "binding_height"),
        HashField(object, "chain_domain")};

    const auto finalized_bytes{
        HexField(object, "finalized_block", modern::FinalizedBlock::SIZE)};
    const auto snapshot{modern::FinalizedBlock::Decode(finalized_bytes)};
    if (!snapshot) throw std::runtime_error("cannot decode finalized_block");
    package.snapshot = *snapshot;

    const auto header_bytes{
        HexField(object, "set0_header", modern::ValidatorSetHeader::SIZE)};
    const auto set0{modern::ValidatorSetHeader::Decode(header_bytes)};
    if (!set0) throw std::runtime_error("cannot decode set0_header");
    package.set0 = *set0;

    if (NumberField(object, "snapshot_height") != package.snapshot.height ||
        HashField(object, "snapshot_block_hash") != package.snapshot.block_hash ||
        HashField(object, "set0_hash") != modern::ValidatorSetHash(package.set0)) {
        throw std::runtime_error(
            "signbridgebootstrap display fields disagree with its encoded objects");
    }
    return package;
}

UniValue HeaderJson(const modern::ValidatorSetHeader& header)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("epoch", header.epoch);
    out.pushKV("ruleset_version", header.ruleset_version);
    out.pushKV("validator_count", header.validator_count);
    out.pushKV("total_weight", header.total_weight);
    out.pushKV("quorum_weight", header.quorum_weight);
    out.pushKV("aggregate_pubkey", EthHex(header.aggregate_pubkey));
    out.pushKV("members_root", EthHex(header.members_root));
    out.pushKV("encoded", EthHex(header.Encode()));
    out.pushKV("hash", EthHex(modern::ValidatorSetHash(header)));
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2 && argc != 5) {
        std::fprintf(
            stderr,
            "usage: b3-bridge-bootstrap-proof <manifest.json> "
            "[<signature-1.json> <signature-2.json> <signature-3.json>]\n");
        return 2;
    }
    try {
        const UniValue manifest{LoadJson(argv[1])};
        const uint256 chain_domain{HashField(manifest, "chain_domain")};
        const uint64_t modern_start_height{
            NumberField(manifest, "modern_start_height")};
        const UniValue& committee{Field(manifest, "committee")};
        if (!committee.isArray() || committee.size() != 4) {
            throw std::runtime_error("committee must be an array of exactly four members");
        }

        std::vector<bridge::BootstrapIdentity> identities;
        identities.reserve(4);
        size_t binding_evidence_verified{0};
        for (size_t i{0}; i < committee.size(); ++i) {
            const UniValue& row{committee[i]};
            if (!row.isObject()) throw std::runtime_error("committee member is not an object");
            const UniValue& row_chain_domain{row.find_value("chain_domain")};
            if (!row_chain_domain.isNull() &&
                HashField(row, "chain_domain") != chain_domain) {
                throw std::runtime_error(
                    "committee member " + std::to_string(i) +
                    " chain_domain does not match manifest");
            }
            bridge::BootstrapIdentity identity{
                FixedHex<32>(row, "validator_key"),
                PublicKeyField(row, "bls_pubkey"),
                SignatureFieldEither(row, "proof_of_possession", "pop"),
                0,
                0};
            const XOnlyPubKey validator{identity.validator_key};
            if (!validator.IsFullyValid()) {
                throw std::runtime_error("committee validator_key is not a valid x-only key");
            }

            const UniValue& nested_binding{row.find_value("binding")};
            const bool flat_binding{
                !row.find_value("binding_seq").isNull() ||
                !row.find_value("binding_bip340_sig").isNull()};
            if (nested_binding.isNull() && !flat_binding) {
                throw std::runtime_error(
                    "every committee member requires FINALITY_KEY binding evidence");
            }
            if (!nested_binding.isNull() && !nested_binding.isObject()) {
                throw std::runtime_error("binding must be an object");
            }
            const UniValue& binding{
                nested_binding.isNull() ? row : nested_binding};
            const uint64_t raw_seq{NumberField(
                binding, nested_binding.isNull() ? "binding_seq" : "seq")};
            if (raw_seq > UINT32_MAX) throw std::runtime_error("binding seq exceeds uint32");
            identity.binding_seq = static_cast<uint32_t>(raw_seq);
            identity.binding_height = NumberField(
                binding, nested_binding.isNull() ? "binding_height" : "height");
            const auto signature{FixedHex<modern::BIP340_SIG_SIZE>(
                binding, nested_binding.isNull() ? "binding_bip340_sig"
                                                 : "bip340_sig")};
            const uint256 digest{modern::FinalityBindDigest(
                chain_domain, identity.validator_key,
                identity.bls_pubkey.Compressed(),
                static_cast<uint32_t>(raw_seq))};
            if (!validator.VerifySchnorr(digest, signature)) {
                throw std::runtime_error("invalid FINALITY_KEY binding signature");
            }
            const UniValue& txid{binding.find_value("txid")};
            if (!txid.isNull()) (void)HexField(binding, "txid", 32);
            ++binding_evidence_verified;
            identities.push_back(std::move(identity));
        }

        std::string error;
        UniValue result{UniValue::VOBJ};
        result.pushKV("chain_domain", EthHex(chain_domain));
        result.pushKV("modern_start_height", modern_start_height);
        result.pushKV("manifest_pop_verified", 4);
        result.pushKV("manifest_binding_signatures_verified",
                      static_cast<uint64_t>(binding_evidence_verified));
        result.pushKV("manifest_binding_chain_inclusion_verified", false);

        if (argc == 2) {
            const auto deployment_set{bridge::BuildBootstrapSet(
                chain_domain, identities, modern_start_height, error)};
            if (!deployment_set) throw std::runtime_error(error);

            result.pushKV("format", "b3-bridge-bootstrap-set-v1");
            result.pushKV("bootstrap_set",
                          HeaderJson(deployment_set->bootstrap_set));
            UniValue deployment_env{UniValue::VOBJ};
            deployment_env.pushKV(
                "BOOTSTRAP_AGGREGATE_PUBKEY",
                EthHex(deployment_set->bootstrap_set.aggregate_pubkey));
            deployment_env.pushKV(
                "BOOTSTRAP_MEMBERS_ROOT",
                EthHex(deployment_set->bootstrap_set.members_root));
            deployment_env.pushKV(
                "EXPECTED_BOOTSTRAP_SET_HASH",
                EthHex(deployment_set->bootstrap_set_hash));
            result.pushKV("deployment_env", std::move(deployment_env));
            std::puts(result.write(2).c_str());
            return 0;
        }

        std::vector<bridge::BootstrapSignaturePackage> packages;
        for (int i{2}; i < argc; ++i) {
            packages.push_back(ParsePackage(LoadJson(argv[i])));
        }
        const auto artifacts{bridge::BuildBootstrapProof(
            chain_domain, identities, packages, modern_start_height, error)};
        if (!artifacts) throw std::runtime_error(error);

        result.pushKV("format", "b3-bridge-bootstrap-proof-v1");
        result.pushKV("bootstrap_set", HeaderJson(artifacts->bootstrap_set));
        result.pushKV("set0", HeaderJson(artifacts->set0));

        UniValue snapshot{UniValue::VOBJ};
        snapshot.pushKV("height", artifacts->snapshot.height);
        snapshot.pushKV("block_hash", EthHex(artifacts->snapshot.block_hash));
        snapshot.pushKV("withdrawal_root", EthHex(artifacts->snapshot.withdrawal_root));
        snapshot.pushKV("validator_set_hash", EthHex(artifacts->snapshot.validator_set_hash));
        snapshot.pushKV("epoch", artifacts->snapshot.epoch);
        snapshot.pushKV("encoded", EthHex(artifacts->snapshot.Encode()));
        result.pushKV("snapshot", std::move(snapshot));

        UniValue proof{UniValue::VOBJ};
        proof.pushKV("signer_bitmap", EthHex(artifacts->signer_bitmap));
        proof.pushKV("aggregate_signature_compressed",
                     EthHex(artifacts->aggregate_signature.Compressed()));
        proof.pushKV("signature_g2_eip2537",
                     EthHex(artifacts->aggregate_signature.Eip2537Uncompressed()));
        proof.pushKV("aggregate_pubkey_compressed",
                     EthHex(artifacts->aggregate_pubkey.Compressed()));
        proof.pushKV("aggregate_pubkey_g1_eip2537",
                     EthHex(artifacts->aggregate_pubkey.Eip2537Uncompressed()));
        proof.pushKV("abi", EthHex(artifacts->proof_abi));

        UniValue absent{UniValue::VOBJ};
        absent.pushKV("index", artifacts->absent.index);
        absent.pushKV("weight", artifacts->absent.weight);
        absent.pushKV("pubkey", EthHex(artifacts->absent.compressed_pubkey));
        absent.pushKV("uncompressed_pubkey",
                      EthHex(artifacts->absent.uncompressed_pubkey));
        UniValue siblings{UniValue::VARR};
        for (const uint256& sibling : artifacts->absent.siblings) {
            siblings.push_back(EthHex(sibling));
        }
        absent.pushKV("siblings", std::move(siblings));
        proof.pushKV("absent", std::move(absent));
        result.pushKV("proof", std::move(proof));
        result.pushKV("initialize_calldata",
                      EthHex(artifacts->initialize_calldata));

        std::puts(result.write(2).c_str());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "b3-bridge-bootstrap-proof: %s\n", e.what());
        return 1;
    }
}
