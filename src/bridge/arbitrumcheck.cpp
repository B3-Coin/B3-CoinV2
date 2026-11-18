// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Bounded offline history checker. The separately supplied L2 checkpoint is
// NOT authenticated by this tool. No networking, signing, state journal,
// operator handoff, finality claim, or production activation is implemented.
#include <bridge/arbitrum_history.h>
#include <univalue.h>
#include <util/strencodings.h>

#include <array>
#include <charconv>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

#ifdef WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace arb = bridge::arbitrum;
namespace hist = arb::history;
namespace codec = arb::proof_codec;
namespace {
template <typename T> std::string Hex(const T& value) { return "0x" + HexStr(value); }
uint64_t Number(const std::string& text)
{
    uint64_t value{0};
    const auto result{std::from_chars(text.data(), text.data() + text.size(), value)};
    if (text.empty() || (text.size() > 1 && text[0] == '0') || result.ec != std::errc{} ||
        result.ptr != text.data() + text.size()) throw std::runtime_error("INVALID_RANGE_HEIGHT");
    return value;
}
arb::Bytes Bytes(const std::string& text, size_t size)
{
    if (text.size() != 2 + size * 2 || text.substr(0, 2) != "0x" ||
        text.find_first_not_of("0123456789abcdef", 2) != std::string::npos)
        throw std::runtime_error("CANONICAL_LOWERCASE_HASH_REQUIRED");
    const auto bytes{TryParseHex<unsigned char>(text.substr(2))};
    if (!bytes || bytes->size() != size) throw std::runtime_error("INVALID_HASH_LENGTH");
    return *bytes;
}
std::string Decimal(const bridge::StorageWord& word)
{
    std::string out{"0"};
    for (const auto byte : word) {
        unsigned carry{byte};
        for (auto i = out.rbegin(); i != out.rend(); ++i) {
            const auto value{static_cast<unsigned>(*i - '0') * 256 + carry};
            *i = '0' + value % 10;
            carry = value / 10;
        }
        while (carry) { out.insert(out.begin(), '0' + carry % 10); carry /= 10; }
    }
    return out;
}
const char* Name(arb::Error error)
{
    switch (error) {
    case arb::Error::OK: return "OK";
    case arb::Error::LIMIT: return "LIMIT";
    case arb::Error::BAD_POLICY: return "BAD_POLICY";
    case arb::Error::BAD_ACCOUNT: return "BAD_ACCOUNT";
    case arb::Error::BAD_STORAGE: return "BAD_STORAGE";
    case arb::Error::BAD_ASSERTION: return "BAD_ASSERTION";
    case arb::Error::BAD_HEADER: return "BAD_HEADER";
    case arb::Error::BAD_ANCESTRY: return "BAD_ANCESTRY";
    case arb::Error::BAD_RECEIPTS: return "BAD_RECEIPTS";
    case arb::Error::BAD_TRANSACTIONS: return "BAD_TRANSACTIONS";
    case arb::Error::STALE_ETHEREUM: return "STALE_ETHEREUM";
    case arb::Error::UNREVIEWED_DEPLOYMENT: return "UNREVIEWED_DEPLOYMENT";
    }
    return "UNKNOWN";
}
UniValue Base(const std::string& result, bool matches)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("format", "b3-arbitrum-history-check/1");
    out.pushKV("result", result);
    out.pushKV("commitments_match", matches);
    out.pushKV("mainnet_enabled", false);
    out.pushKV("arbitrum_authority", false);
    out.pushKV("execution_verified", false);
    out.pushKV("rollup_settlement_verified", false);
    out.pushKV("external_anchor_required", true);
    out.pushKV("checkpoint_trust", "separately-supplied-L2-checkpoint-not-authenticated-here");
    return out;
}
UniValue Accounts(const std::vector<hist::AccountValue>& values)
{
    UniValue out{UniValue::VARR};
    for (const auto& value : values) {
        UniValue account{UniValue::VOBJ}, slots{UniValue::VARR};
        account.pushKV("address", Hex(value.address));
        account.pushKV("nonce", std::to_string(value.account.nonce));
        account.pushKV("balance", Decimal(value.account.balance));
        account.pushKV("code_hash", Hex(value.account.code_hash));
        account.pushKV("storage_root", Hex(value.account.storage_root));
        for (const auto& storage : value.storage) {
            UniValue slot{UniValue::VOBJ};
            slot.pushKV("slot", Hex(storage.slot)); slot.pushKV("value", Hex(storage.value));
            slots.push_back(std::move(slot));
        }
        account.pushKV("storage", std::move(slots)); out.push_back(std::move(account));
    }
    return out;
}
}

int main(int argc, char** argv)
{
    try {
        if (argc == 2 && std::string{argv[1]} == "--help") {
            std::cout << "Offline Nitro history commitment checker; NOT source authority.\n"
                "Read canonical B3ARPR01 binary Proof from stdin. Required arguments:\n"
                "  --previous-height DEC --previous-hash 0xHASH --to-height DEC --to-hash 0xHASH\n"
                "Optional repeated --vault 0xADDRESS decodes candidate Rebirth custody events.\n"
                "The supplied checkpoint requires independent authentication. Mainnet disabled.\n";
            return 0;
        }
        if (argc < 9 || (argc - 1) % 2) throw std::runtime_error("EXACT_RANGE_ARGUMENTS_REQUIRED");
        std::map<std::string, std::string> args;
        std::vector<bridge::EthAddress> vaults;
        for (int i{1}; i < argc; i += 2) {
            const std::string key{argv[i]}, value{argv[i + 1]};
            if (key == "--vault") {
                if (vaults.size() >= arb::MAX_ACCOUNTS) throw std::runtime_error("VAULT_LIMIT");
                const auto bytes{Bytes(value, 20)};
                bridge::EthAddress address{};
                std::copy(bytes.begin(), bytes.end(), address.begin()); vaults.push_back(address);
            } else if ((key != "--previous-height" && key != "--previous-hash" &&
                        key != "--to-height" && key != "--to-hash") || !args.emplace(key, value).second)
                throw std::runtime_error("UNKNOWN_OR_DUPLICATE_ARGUMENT");
        }
        if (args.size() != 4) throw std::runtime_error("EXACT_RANGE_ARGUMENTS_REQUIRED");
        arb::RangeRequest request{Number(args.at("--previous-height")), Number(args.at("--to-height")),
            uint256{Bytes(args.at("--previous-hash"), 32)}, uint256{Bytes(args.at("--to-hash"), 32)}};
#ifdef WIN32
        if (_setmode(_fileno(stdin), _O_BINARY) == -1) throw std::runtime_error("BINARY_STDIN_REQUIRED");
#endif
        arb::Bytes wire;
        std::array<char, 8192> buffer;
        while (std::cin) {
            std::cin.read(buffer.data(), buffer.size());
            const auto count{static_cast<size_t>(std::cin.gcount())};
            if (count > arb::MAX_PROOF_BYTES - wire.size()) throw std::runtime_error("PROOF_LIMIT");
            wire.insert(wire.end(), buffer.begin(), buffer.begin() + count);
        }
        if (!std::cin.eof()) throw std::runtime_error("PROOF_READ_FAILED");
        auto decoded{codec::DecodeProofV1(wire)};
        if (!decoded.proof) {
            std::cout << Base(codec::Name(decoded.result), false).write() << '\n'; return 2;
        }
        const auto checked{hist::CheckRange(request, *decoded.proof)};
        if (!checked.evidence) {
            std::cout << Base(Name(checked.error), false).write() << '\n'; return 2;
        }
        const auto& evidence{*checked.evidence};
        const auto events{hist::ExtractEvents(evidence, vaults)};
        if (!events) { std::cout << Base("BAD_CANDIDATE_EVENT", false).write() << '\n'; return 2; }
        auto out{Base("OK", true)};
        out.pushKV("proof_digest", Hex(decoded.digest));
        out.pushKV("previous_height", std::to_string(request.previous_height));
        out.pushKV("previous_hash", Hex(request.previous_hash));
        out.pushKV("to_height", std::to_string(request.to_height)); out.pushKV("to_hash", Hex(request.to_hash));
        out.pushKV("header_count", evidence.headers.size()); out.pushKV("receipt_count", evidence.receipts.size());
        out.pushKV("newest_accounts", Accounts(evidence.newest_accounts));
        out.pushKV("oldest_accounts", Accounts(evidence.oldest_accounts));
        UniValue records{UniValue::VARR};
        for (const auto& event : *events) {
            UniValue record{UniValue::VOBJ};
            record.pushKV("kind", event.kind == hist::EventKind::DEPOSIT ? "deposit" : "paid");
            record.pushKV("vault", Hex(event.vault));
            record.pushKV("block_number", std::to_string(event.block_number)); record.pushKV("block_hash", Hex(event.block_hash));
            record.pushKV("transaction_index", std::to_string(event.transaction_index));
            record.pushKV("transaction_hash", Hex(event.transaction_hash)); record.pushKV("log_index", std::to_string(event.log_index));
            record.pushKV("amount_raw", Decimal(event.amount));
            if (event.kind == hist::EventKind::DEPOSIT) {
                record.pushKV("deposit_id", std::to_string(event.deposit_id)); record.pushKV("account", Hex(event.account));
                record.pushKV("asset", Hex(event.asset)); record.pushKV("sender", Hex(event.address));
            } else { record.pushKV("withdrawal_id", Hex(event.withdrawal_id)); record.pushKV("recipient", Hex(event.address)); }
            records.push_back(std::move(record));
        }
        out.pushKV("candidate_events", std::move(records));
        const auto text{out.write()};
        if (text.size() > 8 * 1024 * 1024) throw std::runtime_error("REPORT_LIMIT");
        std::cout << text << '\n';
        return std::cout.good() ? 0 : 1;
    } catch (const std::exception&) {
        // Never echo argv, raw proof, paths or uncontrolled error strings.
        std::cout << Base("INVALID_INPUT_OR_LIMIT", false).write() << '\n';
        return 2;
    }
}
