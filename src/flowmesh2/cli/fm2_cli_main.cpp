// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/cli/https_client.h>
#include <flowmesh2/cli/outbox.h>
#include <flowmesh2/cli/reply_json.h>
#include <flowmesh2/cli/signer.h>
#include <flowmesh2/cli/trust.h>
#include <flowmesh2/core/action.h>
#include <flowmesh2/core/market.h>
#include <flowmesh2/netio/api_json.h>
#include <flowmesh2/netio/genesis_doc.h>
#include <flowmesh2/read/views.h>
#include <key.h>
#include <univalue.h>
#include <util/fs.h>
#include <util/translation.h>

#include <arpa/inet.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

/** fm2-cli: the FlowMesh V2 client (stage 2 step 6, M5-CLI-01; ARCHITECTURE.md
 * O-106). It signs every action ONCE with the client's BIP340 key, makes the
 * exact bytes durable in its outbox BEFORE they leave (DUR-4), posts them to
 * EVERY seat of the genesis document over pinned TLS, and believes nothing a
 * seat answers until b3verify accepts it against the genesis trust inputs
 * (cli/trust.h). Output is one JSON object per command on stdout; errors are
 * one JSON object on stderr. Exit codes: 0 done; 1 an error (a refused
 * reply, nothing verified, a journal fault); 2 the action ended EXPIRED
 * (certified) or FAILED (every answering seat reports a terminal pool
 * status; not certified) under --wait; 3 --wait ran out, or `stream` got no
 * event within --limit-ms; 64 a usage error.
 *
 *   fm2-cli --genesis G --key K --outbox DIR --pins P [--seat i]... [--timeout-ms 5000] <command> [options]
 *
 *   keygen                                    a fresh key into --key (refuses an existing file)
 *   account                                   the account of --key
 *   markets                                   the genesis assets and markets
 *   status                                    every seat's health
 *   state [--account HEX]                     the verified leaf of an account (default: --key's)
 *   place --market B3/rUSD --side buy|sell --price TICKS --lots N [--tif gtc|ioc|post-only] [--max-delay 64] [--nonce N] [--label L] [--wait]
 *   cancel --order HEX [...]                  cancel one order (the PLACE's action id)
 *   cancel-all [...]                          cancel every resting order of the account
 *   transfer --to HEX --asset NAME --amount ATOMS [...]
 *   resend --label L [--wait]                 post the exact stored bytes again (never re-signs)
 *   action --label L                          the verified status of an outbox action on every seat
 *   outbox                                    every outbox entry and what became of it
 *   stream --channel slots|account|book|trades [--account HEX] [--market B3/rUSD] [--from epoch:seq:head] [--count 5] [--limit-ms 10000] [--seat i]
 *
 * --pins names a JSON object {"<seat>": "<SPKI SHA-256 hex>", ...} (the
 * daemon writes each seat's into its datadir, `api.fingerprint`; the pins
 * reach the client out of band, O-106 (8)). Nonces: max(the highest verified
 * leaf's used nonces, the outbox's) + 1, and --nonce may not name a nonce the
 * outbox has signed; ref_slot: the highest verified head minus REF_SLOT_LAG,
 * never below the lowest verified head among the seats that answered;
 * max_delay 64 slots. Everything printed under "verified" was checked with
 * b3verify: an UNKNOWN account is a seat's claim (v0 has no absence proof)
 * and is printed as not verified. REGTEST only (the genesis document is).
 */
// Core's clientversion.cpp (bitcoin_common) refers to this; every executable
// defines it, as bitcoind and bitcoin-cli do. Nothing here is translated.
const TranslateFn G_TRANSLATION_FUN{nullptr};

namespace {

using namespace flowmesh2;
using namespace flowmesh2::cli;

struct Options {
    std::string genesis;
    std::string key;
    std::string outbox;
    std::string pins;
    std::vector<uint32_t> seats;
    uint32_t timeout_ms{5'000};
    bool wait{false};
    uint32_t wait_ms{60'000};
    std::string command;
    std::map<std::string, std::string> values;
    std::set<std::string> flags;

    bool Has(const std::string& name) const { return values.contains(name) || flags.contains(name); }
    std::string Value(const std::string& name, const std::string& fallback = {}) const
    {
        const auto it{values.find(name)};
        return it == values.end() ? fallback : it->second;
    }
};

const std::set<std::string> VALUE_OPTIONS{"--genesis", "--key", "--outbox", "--pins", "--seat", "--timeout-ms", "--wait-ms",
                                          "--account", "--market", "--side", "--price", "--lots", "--tif", "--max-delay", "--nonce",
                                          "--label", "--order", "--to", "--asset", "--amount", "--channel", "--from", "--count", "--limit-ms"};
const std::set<std::string> FLAG_OPTIONS{"--wait", "--help", "-h"};

//! ref_slot is this many slots below the highest verified head (never below the lowest), so one lagging seat cannot expire every action.
constexpr uint64_t REF_SLOT_LAG{4};
constexpr int EXIT_USAGE{64};

// Static details of the program's own refusals (ErrorCode in parentheses).
constexpr std::string_view CLI_GENESIS_REQUIRED{"--genesis is required"};                                  //!< (INVALID_INPUT)
constexpr std::string_view CLI_GENESIS_READ{"cannot read the genesis document"};                           //!< (INVALID_INPUT)
constexpr std::string_view CLI_PINS_REQUIRED{"--pins is required for a command that talks to the seats"};  //!< (INVALID_INPUT)
constexpr std::string_view CLI_PINS_READ{"cannot read the pins file"};                                     //!< (INVALID_INPUT)
constexpr std::string_view CLI_PINS_SHAPE{"the pins file is not a JSON object of seat index to fingerprint"}; //!< (INVALID_INPUT)
constexpr std::string_view CLI_PINS_MISSING{"the pins file lacks a selected seat's fingerprint"};          //!< (INVALID_INPUT)
constexpr std::string_view CLI_NO_SEAT{"no seat selected"};                                                //!< (INVALID_INPUT)
constexpr std::string_view CLI_API_ADDRESS{"a seat's API address is not an IP literal (name resolution has no deadline)"}; //!< (INVALID_INPUT)
constexpr std::string_view CLI_KEY_REQUIRED{"--key is required"};                                          //!< (INVALID_INPUT)
constexpr std::string_view CLI_OUTBOX_REQUIRED{"--outbox is required"};                                    //!< (INVALID_INPUT)
constexpr std::string_view CLI_MARKET_SHAPE{"--market is BASE/QUOTE"};                                     //!< (INVALID_INPUT)
constexpr std::string_view CLI_NONCE_SIGNED{"--nonce names a nonce this outbox has signed already"};       //!< (INVALID_INPUT)
constexpr std::string_view CLI_NO_VERIFIED_HEAD{"no seat answered with a verified head"};                  //!< (NEED_DATA)
constexpr std::string_view CLI_WAIT_FAILED{"every answering seat reports a terminal pool status (nonce used or too low); not certified"};

std::optional<uint64_t> ParseNumber(std::string_view text)
{
    uint64_t value{0};
    const auto [end, error]{std::from_chars(text.data(), text.data() + text.size(), value)};
    if (error != std::errc{} || end != text.data() + text.size() || text.empty()) return std::nullopt;
    return value;
}

int Usage(std::string_view problem)
{
    std::cerr << "fm2-cli: " << problem << "\n"
              << "usage: fm2-cli --genesis <genesis.json> --key <file> --outbox <dir> --pins <pins.json> [--seat <i>]... [--timeout-ms <ms>]\n"
              << "               keygen | account | markets | status | state [--account HEX]\n"
              << "             | place --market BASE/QUOTE --side buy|sell --price TICKS --lots N [--tif gtc|ioc|post-only] [--max-delay N] [--nonce N] [--label L] [--wait] [--wait-ms N]\n"
              << "             | cancel --order HEX | cancel-all | transfer --to HEX --asset NAME --amount ATOMS\n"
              << "             | resend --label L [--wait] | action --label L | outbox\n"
              << "             | stream --channel slots|account|book|trades [--account HEX] [--market BASE/QUOTE] [--from CURSOR] [--count N] [--limit-ms N] [--seat i]\n";
    return EXIT_USAGE;
}

int Fail(std::string_view code, std::string_view reason)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("error", UniValue{std::string{code}});
    out.pushKV("reason", UniValue{std::string{reason}});
    std::cerr << out.write() << "\n";
    return 1;
}

int Fail(const Error& error) { return Fail(ToString(error.code), error.detail); }

void Print(const UniValue& value) { std::cout << value.write() << "\n"; }

UniValue Hex(const uint256& hash) { return UniValue{hash.GetHex()}; }
UniValue Hex(std::span<const unsigned char> bytes) { return UniValue{netio::ToHex(bytes)}; }
UniValue U64(uint64_t v) { return UniValue{v}; }
UniValue I64(int64_t v) { return UniValue{v}; }
UniValue Str(std::string_view s) { return UniValue{std::string{s}}; }

std::optional<std::string> ReadFile(const std::string& path)
{
    std::ifstream in{path};
    if (!in) return std::nullopt;
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

struct Endpoint {
    uint32_t seat{0};
    std::string address;
    uint16_t port{0};
    std::string pin;
};

//! The genesis document, the trust it gives and the endpoints to talk to.
struct Context {
    netio::GenesisDoc genesis;
    std::optional<Trust> trust;
    std::vector<Endpoint> endpoints;
    uint32_t timeout_ms{5'000};

    HttpsClient Client(const Endpoint& endpoint) const { return HttpsClient{endpoint.address, endpoint.port, endpoint.pin, timeout_ms}; }

    std::string AssetName(const AssetId& asset) const
    {
        for (const auto& [name, id] : genesis.assets) {
            if (id == asset) return name;
        }
        return asset.id.GetHex();
    }
};

Result<Context> LoadContext(const Options& opt, bool network)
{
    Context ctx;
    ctx.timeout_ms = opt.timeout_ms;
    if (opt.genesis.empty()) return Error{ErrorCode::INVALID_INPUT, CLI_GENESIS_REQUIRED};
    const std::optional<std::string> text{ReadFile(opt.genesis)};
    if (!text.has_value()) return Error{ErrorCode::INVALID_INPUT, CLI_GENESIS_READ};
    Result<netio::GenesisDoc> doc{netio::GenesisDoc::Parse(*text)};
    if (!doc.has_value()) return doc.error();
    ctx.genesis = std::move(*doc);
    Result<Trust> trust{Trust::FromGenesis(ctx.genesis)};
    if (!trust.has_value()) return trust.error();
    ctx.trust = std::move(*trust);
    if (!network) return Ok(std::move(ctx));
    if (opt.pins.empty()) return Error{ErrorCode::INVALID_INPUT, CLI_PINS_REQUIRED};
    const std::optional<std::string> pins_text{ReadFile(opt.pins)};
    if (!pins_text.has_value()) return Error{ErrorCode::INVALID_INPUT, CLI_PINS_READ};
    Result<UniValue> pins{ParseJson(*pins_text)};
    if (!pins.has_value() || !pins->isObject()) return Error{ErrorCode::INVALID_INPUT, CLI_PINS_SHAPE};
    for (const netio::GenesisSeat& seat : ctx.genesis.seats) {
        if (!opt.seats.empty() && std::find(opt.seats.begin(), opt.seats.end(), seat.index) == opt.seats.end()) continue;
        const std::string key{std::to_string(seat.index)};
        if (!pins->exists(key) || !(*pins)[key].isStr()) return Error{ErrorCode::INVALID_INPUT, CLI_PINS_MISSING};
        const std::string pin{(*pins)[key].get_str()};
        if (!HttpsClient::PinWellFormed(pin)) return Error{ErrorCode::INVALID_INPUT, HTTPS_PIN_SHAPE};
        in_addr v4{};
        in6_addr v6{};
        if (inet_pton(AF_INET, seat.api_address.c_str(), &v4) != 1 && inet_pton(AF_INET6, seat.api_address.c_str(), &v6) != 1) {
            return Error{ErrorCode::INVALID_INPUT, CLI_API_ADDRESS};
        }
        ctx.endpoints.push_back(Endpoint{seat.index, seat.api_address, seat.api_port, pin});
    }
    if (ctx.endpoints.empty()) return Error{ErrorCode::INVALID_INPUT, CLI_NO_SEAT};
    return Ok(std::move(ctx));
}

Result<ClientKey> LoadKey(const Options& opt)
{
    if (opt.key.empty()) return Error{ErrorCode::INVALID_INPUT, CLI_KEY_REQUIRED};
    return ClientKey::Load(fs::PathFromString(opt.key));
}

Result<std::unique_ptr<Outbox>> LoadOutbox(const Options& opt, const Context& ctx)
{
    if (opt.outbox.empty()) return Error{ErrorCode::INVALID_INPUT, CLI_OUTBOX_REQUIRED};
    return Outbox::Open(fs::PathFromString(opt.outbox), ctx.trust->Profile());
}

//! One seat's verified view of an account.
struct SeatState {
    uint32_t seat{0};
    std::optional<StateReply> reply;
    std::optional<AccountLeafV0> leaf; //!< verified, when FOUND
    uint64_t height{0};                //!< verified head height
    std::string error;                 //!< why there is no verified reply
};

std::vector<SeatState> QueryState(const Context& ctx, const AccountId& account)
{
    std::vector<SeatState> out;
    for (const Endpoint& endpoint : ctx.endpoints) {
        SeatState state;
        state.seat = endpoint.seat;
        HttpsClient client{ctx.Client(endpoint)};
        const Result<HttpResponse> response{client.Request("GET", "/v2/state/" + account.id.GetHex())};
        if (!response.has_value()) {
            state.error = std::string{ToString(response.error().code)} + ": " + std::string{response.error().detail};
            out.push_back(std::move(state));
            continue;
        }
        if (response->status != 200) {
            state.error = "HTTP " + std::to_string(response->status) + ": " + response->body;
            out.push_back(std::move(state));
            continue;
        }
        const Result<UniValue> json{ParseJson(response->body)};
        Result<StateReply> parsed{json.has_value() ? ParseStateReply(*json, ctx.trust->Profile()) : Result<StateReply>{json.error()}};
        if (!parsed.has_value()) {
            state.error = std::string{ToString(parsed.error().code)} + ": " + std::string{parsed.error().detail};
            out.push_back(std::move(state));
            continue;
        }
        if (const Result<void> head{ctx.trust->VerifyHead(parsed->head)}; !head.has_value()) {
            state.error = std::string{"unverified head: "} + std::string{head.error().detail};
            out.push_back(std::move(state));
            continue;
        }
        state.height = parsed->head.envelope.header.height;
        if (parsed->found && parsed->witness.has_value()) {
            Result<AccountLeafV0> leaf{ctx.trust->VerifyAccount(parsed->head, *parsed->witness, account)};
            if (!leaf.has_value()) {
                state.error = std::string{"unverified leaf: "} + std::string{leaf.error().detail};
                out.push_back(std::move(state));
                continue;
            }
            state.leaf = std::move(*leaf);
        }
        state.reply = std::move(*parsed);
        out.push_back(std::move(state));
    }
    return out;
}

//! The verified state with the highest head, if any seat gave one.
const SeatState* Best(const std::vector<SeatState>& states)
{
    const SeatState* best{nullptr};
    for (const SeatState& state : states) {
        if (state.reply.has_value() && (best == nullptr || state.height > best->height)) best = &state;
    }
    return best;
}

UniValue LeafJson(const Context& ctx, const AccountLeafV0& leaf)
{
    UniValue out{UniValue::VOBJ};
    UniValue balances{UniValue::VARR};
    for (const AssetBalance& balance : leaf.balances) {
        UniValue b{UniValue::VOBJ};
        b.pushKV("asset", Str(ctx.AssetName(balance.asset)));
        b.pushKV("asset_id", Hex(balance.asset.id));
        b.pushKV("available", I64(balance.available));
        b.pushKV("reserved", I64(balance.reserved));
        balances.push_back(b);
    }
    out.pushKV("balances", balances);
    UniValue nonces{UniValue::VARR};
    for (const uint64_t nonce : leaf.used_nonces) nonces.push_back(U64(nonce));
    out.pushKV("used_nonces", nonces);
    out.pushKV("open_orders", U64(leaf.open_orders));
    return out;
}

UniValue OutcomeJson(const Context& ctx, const OutcomeV2& outcome)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("code", Str(ToString(outcome.code)));
    out.pushKV("nonce", U64(outcome.nonce));
    out.pushKV("remaining_lots", U64(outcome.remaining_lots));
    UniValue fills{UniValue::VARR};
    for (const Fill& fill : outcome.fills) {
        UniValue f{UniValue::VOBJ};
        f.pushKV("maker", Hex(fill.maker.id));
        f.pushKV("taker", Hex(fill.taker.id));
        f.pushKV("taker_side", Str(fill.taker_side == Side::BUY ? "buy" : "sell"));
        f.pushKV("price_ticks", U64(fill.price_ticks));
        f.pushKV("lots", U64(fill.lots));
        f.pushKV("quote_atoms", I64(fill.quote_atoms));
        f.pushKV("seller_fee", I64(fill.seller_fee));
        fills.push_back(f);
    }
    out.pushKV("fills", fills);
    UniValue deltas{UniValue::VARR};
    for (const BalanceDelta& delta : outcome.deltas) {
        UniValue d{UniValue::VOBJ};
        d.pushKV("account", Hex(delta.account.id));
        d.pushKV("asset", Str(ctx.AssetName(delta.asset)));
        d.pushKV("available_delta", I64(delta.available_delta));
        d.pushKV("reserved_delta", I64(delta.reserved_delta));
        deltas.push_back(d);
    }
    out.pushKV("deltas", deltas);
    return out;
}

//! POST the exact bytes to every endpoint; what each answered.
UniValue PostAll(const Context& ctx, std::span<const unsigned char> bytes, size_t& admitted_or_duplicate)
{
    UniValue out{UniValue::VARR};
    admitted_or_duplicate = 0;
    for (const Endpoint& endpoint : ctx.endpoints) {
        UniValue one{UniValue::VOBJ};
        one.pushKV("seat", U64(endpoint.seat));
        HttpsClient client{ctx.Client(endpoint)};
        const Result<HttpResponse> response{client.Request("POST", "/v2/actions", bytes)};
        if (!response.has_value()) {
            one.pushKV("error", Str(ToString(response.error().code)));
            one.pushKV("reason", Str(response.error().detail));
            out.push_back(one);
            continue;
        }
        one.pushKV("http", U64(static_cast<uint64_t>(response->status)));
        const Result<UniValue> json{ParseJson(response->body)};
        if (json.has_value() && json->isObject()) {
            const Result<AdmissionReplyJson> reply{ParseAdmissionReply(*json, ctx.trust->Profile())};
            if (reply.has_value()) {
                one.pushKV("code", Str(reply->code));
                if (!reply->reason.empty()) one.pushKV("reason", Str(reply->reason));
                if (reply->head_height.has_value()) one.pushKV("head_height", U64(*reply->head_height));
                if (reply->code == "ADMITTED" || reply->code == "DUPLICATE") ++admitted_or_duplicate;
            } else if (json->exists("error")) {
                one.pushKV("error", (*json)["error"]);
                if (json->exists("reason")) one.pushKV("reason", (*json)["reason"]);
            } else {
                one.pushKV("body", Str(response->body));
            }
        } else {
            one.pushKV("body", Str(response->body));
        }
        out.push_back(one);
    }
    return out;
}

//! Poll every seat for the action's status until a certified end, a timeout or a terminal report.
int Wait(const Context& ctx, Outbox& outbox, const std::string& label, const OutboxEntry& entry, UniValue& out, uint32_t wait_ms)
{
    const ActionId id{entry.action.Id()};
    const auto deadline{std::chrono::steady_clock::now() + std::chrono::milliseconds{wait_ms}};
    std::string last_status;
    std::string last_reason;
    uint64_t last_seat{0};
    uint64_t unverified_claims{0}; // INCLUDED or EXPIRED answers b3verify refused: never acted on, counted for the operator
    for (;;) {
        size_t answering{0};
        size_t terminal{0};
        uint64_t terminal_height{0};
        for (const Endpoint& endpoint : ctx.endpoints) {
            HttpsClient client{ctx.Client(endpoint)};
            const Result<HttpResponse> response{client.Request("POST", "/v2/actions/status", entry.bytes)};
            if (!response.has_value() || response->status != 200) continue;
            const Result<UniValue> json{ParseJson(response->body)};
            if (!json.has_value()) continue;
            const Result<StatusReply> reply{ParseStatusReply(*json, ctx.trust->Profile())};
            if (!reply.has_value()) continue;
            ++answering;
            last_status = reply->status;
            last_reason = reply->reason.empty() ? reply->pending_status : reply->reason;
            last_seat = endpoint.seat;
            if (reply->status == "UNKNOWN" && reply->reason == net::API_UNKNOWN_TERMINAL_UNPROVEN) {
                ++terminal;
                if (reply->head.has_value()) terminal_height = std::max(terminal_height, reply->head->envelope.header.height);
            }
            if (reply->status == "INCLUDED" && reply->included.has_value()) {
                const Result<OutcomeV2> outcome{ctx.trust->VerifyIncluded(*reply->included, id)};
                if (!outcome.has_value()) {
                    ++unverified_claims; // an unverifiable claim is no answer
                    continue;
                }
                UniValue result{UniValue::VOBJ};
                result.pushKV("status", Str("INCLUDED"));
                result.pushKV("seat", U64(endpoint.seat));
                result.pushKV("height", U64(reply->included->ref.height));
                result.pushKV("index", U64(reply->included->ref.index));
                result.pushKV("header_hash", Hex(reply->included->envelope.header.Hash()));
                result.pushKV("outcome", OutcomeJson(ctx, *outcome));
                result.pushKV("verified", UniValue{true});
                out.pushKV("result", result);
                const OutboxResult recorded{OutboxOutcome::INCLUDED, reply->included->ref.height, reply->included->ref.index, std::string{}};
                if (const Result<void> ok{outbox.Record(label, recorded)}; !ok.has_value()) return Fail(ok.error());
                Print(out);
                return 0;
            }
            if (reply->status == "EXPIRED" && reply->head.has_value() && reply->expiry.has_value()) {
                if (const Result<void> expired{ctx.trust->VerifyExpired(entry.action, *reply->head, *reply->expiry)}; !expired.has_value()) {
                    ++unverified_claims;
                    continue;
                }
                UniValue result{UniValue::VOBJ};
                result.pushKV("status", Str("EXPIRED"));
                result.pushKV("seat", U64(endpoint.seat));
                result.pushKV("height", U64(reply->head->envelope.header.height));
                result.pushKV("verified", UniValue{true});
                out.pushKV("result", result);
                const OutboxResult recorded{OutboxOutcome::EXPIRED, reply->head->envelope.header.height, 0, std::string{}};
                if (const Result<void> ok{outbox.Record(label, recorded)}; !ok.has_value()) return Fail(ok.error());
                Print(out);
                return 2;
            }
        }
        if (answering == ctx.genesis.seats.size() && terminal == answering) {
            // EVERY seat of the genesis (not only the ones --seat selected: one seat's word is never decisive) answers and says
            // its pool dropped the action for a nonce reason. Nothing certifies that (the lease proves nothing,
            // net/client_api.h), so it is printed as FAILED and NOT recorded: the outbox keeps only certified ends, and a
            // later `resend --wait` polls again (review 2026-11-16, defect 9). A seat that is silent or merely behind keeps
            // the client polling instead.
            UniValue result{UniValue::VOBJ};
            result.pushKV("status", Str("FAILED"));
            result.pushKV("reason", Str(CLI_WAIT_FAILED));
            result.pushKV("seats_answering", U64(answering));
            result.pushKV("height", U64(terminal_height));
            result.pushKV("verified", UniValue{false});
            result.pushKV("recorded", UniValue{false});
            out.pushKV("result", result);
            Print(out);
            return 2;
        }
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
    }
    UniValue result{UniValue::VOBJ};
    result.pushKV("status", Str("TIMEOUT"));
    result.pushKV("last_status", Str(last_status));
    result.pushKV("last_reason", Str(last_reason));
    result.pushKV("last_seat", U64(last_seat));
    result.pushKV("unverified_claims", U64(unverified_claims));
    out.pushKV("result", result);
    Print(out);
    return 3;
}

Result<MarketDescriptor> MarketOf(const Context& ctx, const std::string& text)
{
    const size_t slash{text.find('/')};
    if (slash == std::string::npos) return Error{ErrorCode::INVALID_INPUT, CLI_MARKET_SHAPE};
    return ctx.genesis.Market(text.substr(0, slash), text.substr(slash + 1));
}

//! Sign, store, post and optionally wait: the common tail of place, cancel, cancel-all and transfer.
int Submit(const Options& opt, const Context& ctx, const ClientKey& key, Outbox& outbox, ActionKind kind, std::vector<unsigned char> payload,
           UniValue extra)
{
    const std::vector<SeatState> states{QueryState(ctx, key.Account())};
    const SeatState* best{Best(states)};
    if (best == nullptr) {
        std::string why;
        for (const SeatState& s : states) why += " seat " + std::to_string(s.seat) + ": " + s.error + ";";
        return Fail("NEED_DATA", std::string{CLI_NO_VERIFIED_HEAD} + ":" + why);
    }
    // ref_slot: a little below the best verified head, never below the lowest, so one lagging seat cannot expire the action
    // and one slow seat still admits it (REF_SLOT_AHEAD).
    uint64_t lowest{best->height};
    for (const SeatState& s : states) {
        if (s.reply.has_value() && s.height < lowest) lowest = s.height;
    }
    const uint64_t ref_slot{std::max(lowest, best->height > REF_SLOT_LAG ? best->height - REF_SLOT_LAG : 0)};
    // The nonce: the highest VERIFIED leaf among the answering seats (an UNKNOWN account is a claim), then the outbox's floor.
    const SeatState* leaf_from{nullptr};
    for (const SeatState& s : states) {
        if (s.leaf.has_value() && (leaf_from == nullptr || s.height > leaf_from->height)) leaf_from = &s;
    }
    uint64_t nonce{0};
    std::string nonce_source{"outbox"};
    if (leaf_from != nullptr) {
        for (const uint64_t used : leaf_from->leaf->used_nonces) nonce = std::max(nonce, used);
        nonce_source = "leaf";
    }
    const std::optional<uint64_t> signed_before{outbox.HighestNonce(key.Account())};
    if (signed_before.has_value() && *signed_before >= nonce) {
        if (leaf_from == nullptr || *signed_before > nonce) nonce_source = "outbox";
        nonce = *signed_before;
    }
    nonce += 1;
    if (opt.Has("--nonce")) {
        const std::optional<uint64_t> chosen{ParseNumber(opt.Value("--nonce"))};
        if (!chosen.has_value()) return Usage("bad --nonce");
        // A nonce once signed is never signed again (cli/outbox.h), whatever became of the action.
        if (signed_before.has_value() && *chosen <= *signed_before) return Fail("INVALID_INPUT", CLI_NONCE_SIGNED);
        nonce = *chosen;
        nonce_source = "option";
    }
    uint64_t max_delay{64};
    if (opt.Has("--max-delay")) {
        const std::optional<uint64_t> chosen{ParseNumber(opt.Value("--max-delay"))};
        if (!chosen.has_value() || *chosen == 0 || *chosen > ctx.trust->Profile().max_delay_slots) return Usage("bad --max-delay");
        max_delay = *chosen;
    }
    ActionV2 action;
    action.domain = ctx.trust->Domain();
    action.nonce = nonce;
    action.ref_slot = ref_slot;
    action.max_delay = static_cast<uint16_t>(max_delay);
    action.kind = kind;
    action.payload = std::move(payload);
    const Result<std::vector<unsigned char>> bytes{key.SignAction(action)};
    if (!bytes.has_value()) return Fail(bytes.error());
    const Result<ActionV2> signed_action{ActionV2::Decode(*bytes, ctx.trust->Profile())};
    if (!signed_action.has_value()) return Fail(signed_action.error());
    const std::string label{opt.Value("--label", std::string{ToString(kind)} + "-" + std::to_string(nonce))};
    // Durable before the first byte leaves (DUR-4).
    if (const Result<void> stored{outbox.Add(label, *bytes)}; !stored.has_value()) return Fail(stored.error());
    UniValue out{std::move(extra)};
    out.pushKV("command", Str(opt.command));
    out.pushKV("label", Str(label));
    out.pushKV("account", Hex(key.Account().id));
    out.pushKV("action_id", Hex(signed_action->Id().id));
    out.pushKV("nonce", U64(nonce));
    out.pushKV("nonce_source", Str(nonce_source));
    out.pushKV("ref_slot", U64(ref_slot));
    out.pushKV("max_delay", U64(max_delay));
    out.pushKV("bytes", Hex(*bytes));
    size_t accepted{0};
    out.pushKV("admission", PostAll(ctx, *bytes, accepted));
    out.pushKV("accepted_by", U64(accepted));
    if (!opt.wait) {
        Print(out);
        return accepted > 0 ? 0 : 1;
    }
    const Result<OutboxEntry> entry{outbox.Find(label)};
    if (!entry.has_value()) return Fail(entry.error());
    return Wait(ctx, outbox, label, *entry, out, opt.wait_ms);
}

int CmdKeygen(const Options& opt)
{
    if (opt.key.empty()) return Usage("--key is required");
    const Result<ClientKey> key{ClientKey::Generate(fs::PathFromString(opt.key))};
    if (!key.has_value()) return Fail(key.error());
    UniValue out{UniValue::VOBJ};
    out.pushKV("command", Str("keygen"));
    out.pushKV("key_file", Str(opt.key));
    out.pushKV("account", Hex(key->Account().id));
    out.pushKV("pubkey", Hex(std::span<const unsigned char>{key->Key().begin(), key->Key().end()}));
    Print(out);
    return 0;
}

int CmdAccount(const Options& opt)
{
    const Result<ClientKey> key{LoadKey(opt)};
    if (!key.has_value()) return Fail(key.error());
    UniValue out{UniValue::VOBJ};
    out.pushKV("command", Str("account"));
    out.pushKV("account", Hex(key->Account().id));
    out.pushKV("pubkey", Hex(std::span<const unsigned char>{key->Key().begin(), key->Key().end()}));
    Print(out);
    return 0;
}

int CmdMarkets(const Options& opt)
{
    const Result<Context> ctx{LoadContext(opt, /*network=*/false)};
    if (!ctx.has_value()) return Fail(ctx.error());
    UniValue out{UniValue::VOBJ};
    out.pushKV("command", Str("markets"));
    out.pushKV("domain", Hex(ctx->trust->Domain()));
    out.pushKV("seat_set_root", Hex(ctx->trust->Seats().Root()));
    UniValue assets{UniValue::VOBJ};
    for (const auto& [name, id] : ctx->genesis.assets) assets.pushKV(name, Hex(id.id));
    out.pushKV("assets", assets);
    UniValue markets{UniValue::VARR};
    for (const netio::GenesisMarket& market : ctx->genesis.markets) {
        UniValue m{UniValue::VOBJ};
        m.pushKV("market", Str(market.base + "/" + market.quote));
        m.pushKV("base_atoms_per_lot", U64(market.base_atoms_per_lot));
        m.pushKV("quote_atoms_per_tick", U64(market.quote_atoms_per_tick));
        const Result<MarketDescriptor> descriptor{ctx->genesis.Market(market.base, market.quote)};
        if (descriptor.has_value()) m.pushKV("market_id", Hex(descriptor->Id().id));
        markets.push_back(m);
    }
    out.pushKV("markets", markets);
    Print(out);
    return 0;
}

int CmdStatus(const Options& opt)
{
    const Result<Context> ctx{LoadContext(opt, /*network=*/true)};
    if (!ctx.has_value()) return Fail(ctx.error());
    UniValue out{UniValue::VOBJ};
    out.pushKV("command", Str("status"));
    UniValue seats{UniValue::VARR};
    size_t answering{0};
    for (const Endpoint& endpoint : ctx->endpoints) {
        UniValue one{UniValue::VOBJ};
        one.pushKV("seat", U64(endpoint.seat));
        one.pushKV("api", Str(endpoint.address + ":" + std::to_string(endpoint.port)));
        HttpsClient client{ctx->Client(endpoint)};
        const Result<HttpResponse> response{client.Request("GET", "/v2/health")};
        if (!response.has_value()) {
            one.pushKV("error", Str(ToString(response.error().code)));
            one.pushKV("reason", Str(response.error().detail));
        } else {
            const Result<UniValue> json{ParseJson(response->body)};
            const Result<HealthReplyJson> health{json.has_value() ? ParseHealthReply(*json) : Result<HealthReplyJson>{json.error()}};
            one.pushKV("http", U64(static_cast<uint64_t>(response->status)));
            if (health.has_value()) {
                ++answering;
                one.pushKV("fenced", UniValue{health->fenced});
                if (health->published_height.has_value()) one.pushKV("published_height", U64(*health->published_height));
                one.pushKV("pool_live", U64(health->pool_live));
            } else {
                one.pushKV("body", Str(response->body));
            }
        }
        seats.push_back(one);
    }
    out.pushKV("seats", seats);
    out.pushKV("answering", U64(answering));
    Print(out);
    return answering > 0 ? 0 : 1;
}

int CmdState(const Options& opt)
{
    const Result<Context> ctx{LoadContext(opt, /*network=*/true)};
    if (!ctx.has_value()) return Fail(ctx.error());
    AccountId account;
    if (opt.Has("--account")) {
        const std::optional<uint256> id{uint256::FromHex(opt.Value("--account"))};
        if (!id.has_value()) return Usage("--account is 64 hex characters");
        account.id = *id;
    } else {
        const Result<ClientKey> key{LoadKey(opt)};
        if (!key.has_value()) return Fail(key.error());
        account = key->Account();
    }
    const std::vector<SeatState> states{QueryState(*ctx, account)};
    const SeatState* best{Best(states)};
    UniValue out{UniValue::VOBJ};
    out.pushKV("command", Str("state"));
    out.pushKV("account", Hex(account.id));
    UniValue seats{UniValue::VARR};
    for (const SeatState& state : states) {
        UniValue one{UniValue::VOBJ};
        one.pushKV("seat", U64(state.seat));
        if (state.reply.has_value()) {
            one.pushKV("height", U64(state.height));
            one.pushKV("status", Str(state.leaf.has_value() ? "FOUND" : "UNKNOWN"));
        } else {
            one.pushKV("error", Str(state.error));
        }
        seats.push_back(one);
    }
    out.pushKV("seats", seats);
    if (best == nullptr) {
        Print(out);
        return Fail("NEED_DATA", "no seat answered with a verified head");
    }
    // The leaf comes from the highest verified leaf, which may be another seat's than the highest verified head's.
    const SeatState* leaf_from{nullptr};
    for (const SeatState& state : states) {
        if (state.leaf.has_value() && (leaf_from == nullptr || state.height > leaf_from->height)) leaf_from = &state;
    }
    out.pushKV("seat", U64(best->seat));
    out.pushKV("height", U64(best->height));
    out.pushKV("header_hash", Hex(best->reply->head.envelope.header.Hash()));
    out.pushKV("head_verified", UniValue{true});
    if (leaf_from != nullptr) {
        out.pushKV("status", Str("FOUND"));
        out.pushKV("leaf_seat", U64(leaf_from->seat));
        out.pushKV("leaf_height", U64(leaf_from->height));
        out.pushKV("leaf", LeafJson(*ctx, *leaf_from->leaf));
        out.pushKV("verified", UniValue{true});
    } else {
        // UNKNOWN is the seats' claim: v0 has no absence proof (read/read_service.h).
        out.pushKV("status", Str("UNKNOWN"));
        out.pushKV("verified", UniValue{false});
        out.pushKV("reason", Str("no account leaf: an absent account is a seat's claim, not a proof"));
    }
    Print(out);
    return 0;
}

int CmdPlace(const Options& opt, const Context& ctx, const ClientKey& key, Outbox& outbox)
{
    const Result<MarketDescriptor> market{MarketOf(ctx, opt.Value("--market"))};
    if (!market.has_value()) return Fail(market.error());
    const std::string side_text{opt.Value("--side")};
    const Side side{side_text == "buy" ? Side::BUY : Side::SELL};
    if (side_text != "buy" && side_text != "sell") return Usage("--side is buy or sell");
    const std::string tif_text{opt.Value("--tif", "gtc")};
    TimeInForce tif{TimeInForce::GTC};
    if (tif_text == "ioc") {
        tif = TimeInForce::IOC;
    } else if (tif_text == "post-only") {
        tif = TimeInForce::POST_ONLY;
    } else if (tif_text != "gtc") {
        return Usage("--tif is gtc, ioc or post-only");
    }
    const std::optional<uint64_t> price{ParseNumber(opt.Value("--price"))};
    const std::optional<uint64_t> lots{ParseNumber(opt.Value("--lots"))};
    if (!price.has_value() || !lots.has_value() || *price == 0 || *lots == 0) return Usage("--price and --lots are positive integers");
    const Result<std::vector<unsigned char>> payload{EncodePayload(PlacePayload{market->Id(), side, tif, *price, *lots})};
    if (!payload.has_value()) return Fail(payload.error());
    UniValue extra{UniValue::VOBJ};
    extra.pushKV("market", Str(opt.Value("--market")));
    extra.pushKV("market_id", Hex(market->Id().id));
    extra.pushKV("side", Str(side_text));
    extra.pushKV("tif", Str(tif_text));
    extra.pushKV("price_ticks", U64(*price));
    extra.pushKV("lots", U64(*lots));
    return Submit(opt, ctx, key, outbox, ActionKind::PLACE, *payload, std::move(extra));
}

int CmdCancel(const Options& opt, const Context& ctx, const ClientKey& key, Outbox& outbox)
{
    const std::optional<uint256> order{uint256::FromHex(opt.Value("--order"))};
    if (!order.has_value()) return Usage("--order is the PLACE's action id, 64 hex characters");
    const Result<std::vector<unsigned char>> payload{EncodePayload(CancelPayload{OrderId{*order}})};
    if (!payload.has_value()) return Fail(payload.error());
    UniValue extra{UniValue::VOBJ};
    extra.pushKV("order", Hex(*order));
    return Submit(opt, ctx, key, outbox, ActionKind::CANCEL, *payload, std::move(extra));
}

int CmdCancelAll(const Options& opt, const Context& ctx, const ClientKey& key, Outbox& outbox)
{
    const Result<std::vector<unsigned char>> payload{EncodePayload(CancelAllPayload{})};
    if (!payload.has_value()) return Fail(payload.error());
    return Submit(opt, ctx, key, outbox, ActionKind::CANCEL_ALL, *payload, UniValue{UniValue::VOBJ});
}

int CmdTransfer(const Options& opt, const Context& ctx, const ClientKey& key, Outbox& outbox)
{
    const std::optional<uint256> to{uint256::FromHex(opt.Value("--to"))};
    if (!to.has_value()) return Usage("--to is an account id, 64 hex characters");
    const std::optional<AssetId> asset{ctx.genesis.Asset(opt.Value("--asset"))};
    if (!asset.has_value()) return Usage("--asset names a genesis asset");
    const std::optional<uint64_t> amount{ParseNumber(opt.Value("--amount"))};
    if (!amount.has_value() || *amount == 0 || *amount > static_cast<uint64_t>(MAX_ATOMS)) return Usage("--amount is a positive number of atoms");
    const Result<std::vector<unsigned char>> payload{EncodePayload(TransferPayload{AccountId{*to}, *asset, static_cast<Atoms>(*amount)})};
    if (!payload.has_value()) return Fail(payload.error());
    UniValue extra{UniValue::VOBJ};
    extra.pushKV("to", Hex(*to));
    extra.pushKV("asset", Str(opt.Value("--asset")));
    extra.pushKV("amount", U64(*amount));
    return Submit(opt, ctx, key, outbox, ActionKind::TRANSFER, *payload, std::move(extra));
}

int CmdResend(const Options& opt, const Context& ctx, Outbox& outbox)
{
    const std::string label{opt.Value("--label")};
    const Result<OutboxEntry> entry{outbox.Find(label)};
    if (!entry.has_value()) return Fail(entry.error());
    UniValue out{UniValue::VOBJ};
    out.pushKV("command", Str("resend"));
    out.pushKV("label", Str(label));
    out.pushKV("action_id", Hex(entry->action.Id().id));
    out.pushKV("nonce", U64(entry->action.nonce));
    size_t accepted{0};
    out.pushKV("admission", PostAll(ctx, entry->bytes, accepted));
    out.pushKV("accepted_by", U64(accepted));
    // A recorded FAILED (an earlier build wrote one) is not certified and does not end --wait: the client polls again, and a
    // certified result may follow it in the outbox (review workflow of 2026-11-18, 08a).
    const bool certified{entry->result.has_value() && entry->result->outcome != OutboxOutcome::FAILED};
    if (!opt.wait || certified) {
        if (entry->result.has_value()) out.pushKV("recorded", Str(ToString(entry->result->outcome)));
        Print(out);
        return accepted > 0 ? 0 : 1;
    }
    return Wait(ctx, outbox, label, *entry, out, opt.wait_ms);
}

int CmdAction(const Options& opt, const Context& ctx, Outbox& outbox)
{
    const std::string label{opt.Value("--label")};
    const Result<OutboxEntry> entry{outbox.Find(label)};
    if (!entry.has_value()) return Fail(entry.error());
    const ActionId id{entry->action.Id()};
    UniValue out{UniValue::VOBJ};
    out.pushKV("command", Str("action"));
    out.pushKV("label", Str(label));
    out.pushKV("action_id", Hex(id.id));
    if (entry->result.has_value()) {
        UniValue recorded{UniValue::VOBJ};
        recorded.pushKV("outcome", Str(ToString(entry->result->outcome)));
        recorded.pushKV("height", U64(entry->result->height));
        recorded.pushKV("index", U64(entry->result->index));
        if (!entry->result->detail.empty()) recorded.pushKV("detail", Str(entry->result->detail));
        out.pushKV("recorded", recorded);
    }
    UniValue seats{UniValue::VARR};
    for (const Endpoint& endpoint : ctx.endpoints) {
        UniValue one{UniValue::VOBJ};
        one.pushKV("seat", U64(endpoint.seat));
        HttpsClient client{ctx.Client(endpoint)};
        const Result<HttpResponse> response{client.Request("POST", "/v2/actions/status", entry->bytes)};
        if (!response.has_value()) {
            one.pushKV("error", Str(ToString(response.error().code)));
            seats.push_back(one);
            continue;
        }
        one.pushKV("http", U64(static_cast<uint64_t>(response->status)));
        const Result<UniValue> json{ParseJson(response->body)};
        const Result<StatusReply> reply{json.has_value() ? ParseStatusReply(*json, ctx.trust->Profile()) : Result<StatusReply>{json.error()}};
        if (!reply.has_value()) {
            one.pushKV("body", Str(response->body));
            seats.push_back(one);
            continue;
        }
        one.pushKV("status", Str(reply->status));
        if (!reply->reason.empty()) one.pushKV("reason", Str(reply->reason));
        if (!reply->pending_status.empty()) one.pushKV("pool_status", Str(reply->pending_status));
        if (reply->status == "INCLUDED" && reply->included.has_value()) {
            const Result<OutcomeV2> outcome{ctx.trust->VerifyIncluded(*reply->included, id)};
            one.pushKV("verified", UniValue{outcome.has_value()});
            if (outcome.has_value()) {
                one.pushKV("height", U64(reply->included->ref.height));
                one.pushKV("outcome", OutcomeJson(ctx, *outcome));
            } else {
                one.pushKV("reason", Str(outcome.error().detail));
            }
        }
        if (reply->status == "EXPIRED" && reply->head.has_value() && reply->expiry.has_value()) {
            const Result<void> expired{ctx.trust->VerifyExpired(entry->action, *reply->head, *reply->expiry)};
            one.pushKV("verified", UniValue{expired.has_value()});
            if (!expired.has_value()) one.pushKV("reason", Str(expired.error().detail));
        }
        seats.push_back(one);
    }
    out.pushKV("seats", seats);
    Print(out);
    return 0;
}

int CmdOutbox(const Options& opt, const Context& ctx)
{
    const Result<std::unique_ptr<Outbox>> outbox{LoadOutbox(opt, ctx)};
    if (!outbox.has_value()) return Fail(outbox.error());
    UniValue out{UniValue::VOBJ};
    out.pushKV("command", Str("outbox"));
    out.pushKV("recovered_records", U64((*outbox)->Recovery().records));
    out.pushKV("truncated_bytes", U64((*outbox)->Recovery().truncated_bytes));
    UniValue entries{UniValue::VARR};
    for (const OutboxEntry& entry : (*outbox)->Entries()) {
        UniValue one{UniValue::VOBJ};
        one.pushKV("label", Str(entry.label));
        one.pushKV("action_id", Hex(entry.action.Id().id));
        one.pushKV("kind", Str(ToString(entry.action.kind)));
        one.pushKV("account", Hex(entry.action.Account().id));
        one.pushKV("nonce", U64(entry.action.nonce));
        one.pushKV("ref_slot", U64(entry.action.ref_slot));
        one.pushKV("max_delay", U64(entry.action.max_delay));
        if (entry.result.has_value()) {
            UniValue result{UniValue::VOBJ};
            result.pushKV("outcome", Str(ToString(entry.result->outcome)));
            result.pushKV("height", U64(entry.result->height));
            result.pushKV("index", U64(entry.result->index));
            if (!entry.result->detail.empty()) result.pushKV("detail", Str(entry.result->detail));
            one.pushKV("result", result);
        }
        entries.push_back(one);
    }
    out.pushKV("entries", entries);
    Print(out);
    return 0;
}

int CmdStream(const Options& opt, const Context& ctx)
{
    const std::string channel{opt.Value("--channel")};
    if (channel != "slots" && channel != "account" && channel != "book" && channel != "trades") return Usage("--channel is slots, account, book or trades");
    std::string path{"/v2/stream?channel=" + channel};
    if (opt.Has("--account")) {
        const std::optional<uint256> id{uint256::FromHex(opt.Value("--account"))};
        if (!id.has_value()) return Usage("--account is 64 hex characters");
        path += "&account=" + id->GetHex();
    }
    if (opt.Has("--market")) {
        const Result<MarketDescriptor> market{MarketOf(ctx, opt.Value("--market"))};
        if (!market.has_value()) return Fail(market.error());
        path += "&market=" + market->Id().id.GetHex();
    }
    if (opt.Has("--from")) {
        const std::optional<net::StreamCursor> from{netio::CursorFromText(opt.Value("--from"))};
        if (!from.has_value()) return Usage("--from is epoch:seq:head");
        path += "&from=" + netio::CursorText(*from);
    }
    const std::optional<uint64_t> count{ParseNumber(opt.Value("--count", "5"))};
    const std::optional<uint64_t> limit{ParseNumber(opt.Value("--limit-ms", "10000"))};
    if (!count.has_value() || !limit.has_value() || *limit > 3'600'000) return Usage("bad --count or --limit-ms");
    const Endpoint& endpoint{ctx.endpoints.front()};
    HttpsClient client{ctx.Client(endpoint)};
    uint64_t seen{0};
    const Result<HttpResponse> streamed{client.Stream(path, [&](const SseEvent& event) {
        if (event.event == "heartbeat") return true;
        UniValue out{UniValue::VOBJ};
        out.pushKV("seat", U64(endpoint.seat));
        out.pushKV("event", Str(event.event));
        out.pushKV("id", Str(event.id));
        const Result<UniValue> data{ParseJson(event.data)};
        if (data.has_value()) {
            out.pushKV("data", *data);
            if (data->isObject() && data->exists("envelope") && (*data)["envelope"].isStr()) {
                // "verified" covers the certified envelope AND the convenience fields beside it: a genuine old envelope with
                // fabricated height, header_hash or state_root is not verified.
                const Result<std::vector<unsigned char>> bytes{ParseBytes((*data)["envelope"])};
                const Result<read::CertifiedEnvelope> envelope{bytes.has_value() ? read::CertifiedEnvelope::Decode(*bytes)
                                                                                 : Result<read::CertifiedEnvelope>{bytes.error()}};
                bool verified{envelope.has_value() && ctx.trust->VerifyEnvelope(*envelope).has_value()};
                if (verified) {
                    const SlotHeaderV2& header{envelope->header};
                    const Result<uint64_t> height{data->exists("height") ? ParseU64((*data)["height"]) : Result<uint64_t>{Ok(header.height)}};
                    const Result<uint256> hash{data->exists("header_hash") ? ParseHash((*data)["header_hash"]) : Result<uint256>{Ok(header.Hash())}};
                    const Result<uint256> root{data->exists("state_root") ? ParseHash((*data)["state_root"]) : Result<uint256>{Ok(header.state_root)}};
                    verified = height.has_value() && *height == header.height && hash.has_value() && *hash == header.Hash() && root.has_value() &&
                               *root == header.state_root;
                    if (verified) {
                        out.pushKV("height", U64(header.height));
                        out.pushKV("header_hash", Hex(header.Hash()));
                        out.pushKV("state_root", Hex(header.state_root));
                    }
                }
                out.pushKV("verified", UniValue{verified});
            }
        } else {
            out.pushKV("raw", Str(event.data));
        }
        Print(out);
        return ++seen < *count;
    }, static_cast<uint32_t>(*limit))};
    if (!streamed.has_value()) return Fail(streamed.error());
    if (streamed->status != 200) return Fail("HTTP " + std::to_string(streamed->status), streamed->body);
    return seen > 0 ? 0 : 3;
}

} // namespace

int main(int argc, char** argv)
{
    Options opt;
    for (int i{1}; i < argc; ++i) {
        const std::string arg{argv[i]};
        if (FLAG_OPTIONS.contains(arg)) {
            if (arg == "--help" || arg == "-h") return Usage("help");
            opt.flags.insert(arg);
            if (arg == "--wait") opt.wait = true;
            continue;
        }
        if (VALUE_OPTIONS.contains(arg)) {
            if (i + 1 >= argc) return Usage("missing value for " + arg);
            const std::string value{argv[++i]};
            if (arg == "--genesis") {
                opt.genesis = value;
            } else if (arg == "--key") {
                opt.key = value;
            } else if (arg == "--outbox") {
                opt.outbox = value;
            } else if (arg == "--pins") {
                opt.pins = value;
            } else if (arg == "--seat") {
                const std::optional<uint64_t> seat{ParseNumber(value)};
                if (!seat.has_value() || *seat > 63) return Usage("bad --seat");
                opt.seats.push_back(static_cast<uint32_t>(*seat));
            } else if (arg == "--timeout-ms") {
                const std::optional<uint64_t> ms{ParseNumber(value)};
                if (!ms.has_value() || *ms == 0 || *ms > 600'000) return Usage("bad --timeout-ms");
                opt.timeout_ms = static_cast<uint32_t>(*ms);
            } else if (arg == "--wait-ms") {
                const std::optional<uint64_t> ms{ParseNumber(value)};
                if (!ms.has_value() || *ms == 0 || *ms > 3'600'000) return Usage("bad --wait-ms");
                opt.wait_ms = static_cast<uint32_t>(*ms);
            } else {
                opt.values[arg] = value;
            }
            continue;
        }
        if (arg.starts_with("-")) return Usage("unknown option " + arg);
        if (!opt.command.empty()) return Usage("one command at a time");
        opt.command = arg;
    }
    if (opt.command.empty()) return Usage("a command is required");
    const ECC_Context ecc;
    if (opt.command == "keygen") return CmdKeygen(opt);
    if (opt.command == "account") return CmdAccount(opt);
    if (opt.command == "markets") return CmdMarkets(opt);
    if (opt.command == "status") return CmdStatus(opt);
    if (opt.command == "state") return CmdState(opt);
    const bool network{opt.command != "outbox"};
    const Result<Context> ctx{LoadContext(opt, network)};
    if (!ctx.has_value()) return Fail(ctx.error());
    if (opt.command == "outbox") return CmdOutbox(opt, *ctx);
    if (opt.command == "stream") return CmdStream(opt, *ctx);
    const Result<std::unique_ptr<Outbox>> outbox{LoadOutbox(opt, *ctx)};
    if (!outbox.has_value()) return Fail(outbox.error());
    if (opt.command == "resend") return CmdResend(opt, *ctx, **outbox);
    if (opt.command == "action") return CmdAction(opt, *ctx, **outbox);
    const Result<ClientKey> key{LoadKey(opt)};
    if (!key.has_value()) return Fail(key.error());
    if (opt.command == "place") return CmdPlace(opt, *ctx, *key, **outbox);
    if (opt.command == "cancel") return CmdCancel(opt, *ctx, *key, **outbox);
    if (opt.command == "cancel-all") return CmdCancelAll(opt, *ctx, *key, **outbox);
    if (opt.command == "transfer") return CmdTransfer(opt, *ctx, *key, **outbox);
    return Usage("unknown command " + opt.command);
}
