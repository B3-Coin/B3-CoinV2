// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/netio/api_json.h>

#include <flowmesh2/admission/pool.h>
#include <flowmesh2/core/codec.h>
#include <flowmesh2/core/codes.h>
#include <flowmesh2/net/stream_types.h>
#include <flowmesh2/read/views.h>

#include <charconv>
#include <cstdint>

namespace flowmesh2::netio {

namespace {

constexpr char HEX[]{"0123456789abcdef"};

UniValue Hex(std::span<const unsigned char> bytes) { return UniValue{ToHex(bytes)}; }
UniValue Hex(const uint256& hash) { return UniValue{hash.GetHex()}; }
UniValue U64(uint64_t v) { return UniValue{v}; }
UniValue I64(int64_t v) { return UniValue{v}; }

UniValue ProofJson(const MerkleProof& proof)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("index", U64(proof.index));
    out.pushKV("size", U64(proof.size));
    UniValue path{UniValue::VARR};
    for (const uint256& node : proof.path) path.push_back(Hex(node));
    out.pushKV("path", path);
    out.pushKV("encoded", Hex(EncodeBytes(proof)));
    return out;
}

UniValue LeafJson(const AccountLeafV0& leaf)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("account", Hex(leaf.account.id));
    UniValue balances{UniValue::VARR};
    for (const AssetBalance& balance : leaf.balances) {
        UniValue b{UniValue::VOBJ};
        b.pushKV("asset", Hex(balance.asset.id));
        b.pushKV("available", I64(balance.available));
        b.pushKV("reserved", I64(balance.reserved));
        balances.push_back(b);
    }
    out.pushKV("balances", balances);
    UniValue nonces{UniValue::VARR};
    for (const uint64_t nonce : leaf.used_nonces) nonces.push_back(U64(nonce));
    out.pushKV("used_nonces", nonces);
    out.pushKV("open_orders", U64(leaf.open_orders));
    out.pushKV("encoded", Hex(leaf.Encode()));
    return out;
}

UniValue WitnessJson(const read::AccountWitness& witness)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("leaf", LeafJson(witness.leaf));
    out.pushKV("roots", Hex(EncodeBytes(witness.roots)));
    out.pushKV("proof", ProofJson(witness.proof));
    return out;
}

UniValue IncludedRefJson(const read::IncludedRef& ref)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("height", U64(ref.height));
    out.pushKV("index", U64(ref.index));
    return out;
}

UniValue FillJson(const Fill& fill)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("maker", Hex(fill.maker.id));
    out.pushKV("taker", Hex(fill.taker.id));
    out.pushKV("market", Hex(fill.market.id));
    out.pushKV("taker_side", UniValue{fill.taker_side == Side::BUY ? "buy" : "sell"});
    out.pushKV("price_ticks", U64(fill.price_ticks));
    out.pushKV("lots", U64(fill.lots));
    out.pushKV("quote_atoms", I64(fill.quote_atoms));
    out.pushKV("seller_fee", I64(fill.seller_fee));
    return out;
}

UniValue DeltaJson(const BalanceDelta& delta)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("account", Hex(delta.account.id));
    out.pushKV("asset", Hex(delta.asset.id));
    out.pushKV("available_delta", I64(delta.available_delta));
    out.pushKV("reserved_delta", I64(delta.reserved_delta));
    return out;
}

UniValue OutcomeJson(const OutcomeV2& outcome)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("source", UniValue{std::string{ToString(outcome.source)}});
    out.pushKV("subject", Hex(outcome.subject));
    out.pushKV("account", Hex(outcome.account.id));
    out.pushKV("nonce", U64(outcome.nonce));
    out.pushKV("code", UniValue{std::string{ToString(outcome.code)}});
    out.pushKV("remaining_lots", U64(outcome.remaining_lots));
    UniValue fills{UniValue::VARR};
    for (const Fill& fill : outcome.fills) fills.push_back(FillJson(fill));
    out.pushKV("fills", fills);
    UniValue deltas{UniValue::VARR};
    for (const BalanceDelta& delta : outcome.deltas) deltas.push_back(DeltaJson(delta));
    out.pushKV("deltas", deltas);
    out.pushKV("encoded", Hex(outcome.Encode()));
    return out;
}

UniValue IncludedOutcomeJson(const read::IncludedOutcome& included)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("ref", IncludedRefJson(included.ref));
    out.pushKV("envelope", Hex(included.envelope.Encode()));
    out.pushKV("header_hash", Hex(included.envelope.header.Hash()));
    out.pushKV("outcome_index", U64(included.outcome.index));
    out.pushKV("outcome", OutcomeJson(included.outcome.outcome));
    out.pushKV("proof", ProofJson(included.outcome.proof));
    return out;
}

UniValue PendingRefJson(const admission::PendingRef& ref)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("seq", U64(ref.seq));
    out.pushKV("snapshot_height", U64(ref.snapshot_height));
    return out;
}

UniValue LevelsJson(const std::vector<net::BookLevel>& levels)
{
    UniValue out{UniValue::VARR};
    for (const net::BookLevel& level : levels) {
        UniValue l{UniValue::VOBJ};
        l.pushKV("price_ticks", U64(level.price_ticks));
        l.pushKV("lots", U64(level.lots));
        l.pushKV("orders", U64(level.orders));
        out.push_back(l);
    }
    return out;
}

} // namespace

std::string ToHex(std::span<const unsigned char> bytes)
{
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const unsigned char b : bytes) {
        out.push_back(HEX[b >> 4]);
        out.push_back(HEX[b & 0x0f]);
    }
    return out;
}

std::optional<std::vector<unsigned char>> FromHex(std::string_view hex)
{
    if (hex.size() % 2 != 0) return std::nullopt;
    std::vector<unsigned char> out;
    out.reserve(hex.size() / 2);
    const auto nibble{[](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }};
    for (size_t i{0}; i < hex.size(); i += 2) {
        const int hi{nibble(hex[i])};
        const int lo{nibble(hex[i + 1])};
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<unsigned char>((hi << 4) | lo));
    }
    return out;
}

UniValue ErrorJson(std::string_view code, std::string_view reason)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("error", UniValue{std::string{code}});
    out.pushKV("reason", UniValue{std::string{reason}});
    return out;
}

UniValue ErrorJson(const Error& error) { return ErrorJson(ToString(error.code), error.detail); }

UniValue ReplyHeadJson(const read::ReplyHead& head)
{
    UniValue out{UniValue::VOBJ};
    UniValue scope{UniValue::VOBJ};
    scope.pushKV("domain", Hex(head.scope.domain));
    scope.pushKV("configuration", Hex(head.scope.configuration));
    scope.pushKV("validator_set", Hex(head.scope.validator_set));
    scope.pushKV("anchor", Hex(head.scope.anchor));
    scope.pushKV("epoch", U64(head.scope.epoch));
    scope.pushKV("authority_revision", U64(head.scope.authority_revision));
    out.pushKV("scope", scope);
    UniValue commit{UniValue::VOBJ};
    commit.pushKV("id", Hex(head.commit.id));
    commit.pushKV("parent", Hex(head.commit.parent));
    commit.pushKV("state_root", Hex(head.commit.state_root));
    commit.pushKV("sequence", U64(head.commit.sequence));
    out.pushKV("commit", commit);
    out.pushKV("publication", U64(head.publication));
    out.pushKV("height", U64(head.envelope.header.height));
    out.pushKV("header_hash", Hex(head.envelope.header.Hash()));
    out.pushKV("envelope", Hex(head.envelope.Encode()));
    out.pushKV("label", UniValue{std::string{read::ToString(head.label)}});
    return out;
}

UniValue AdmissionReplyJson(const admission::AdmissionReply& reply)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("code", UniValue{std::string{ToString(reply.code)}});
    if (!reply.reason.empty()) out.pushKV("reason", UniValue{std::string{reply.reason}});
    if (reply.id.has_value()) out.pushKV("action_id", Hex(reply.id->id));
    if (reply.pending.has_value()) out.pushKV("pending", PendingRefJson(*reply.pending));
    if (reply.included.has_value()) out.pushKV("included", IncludedRefJson(*reply.included));
    if (reply.outcome.has_value()) out.pushKV("outcome", IncludedOutcomeJson(*reply.outcome));
    if (reply.head_height.has_value()) out.pushKV("head_height", U64(*reply.head_height));
    return out;
}

UniValue ActionStatusJson(const net::ActionStatusReply& reply)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("status", UniValue{std::string{net::ToString(reply.status)}});
    if (!reply.reason.empty()) out.pushKV("reason", UniValue{std::string{reply.reason}});
    if (reply.head.has_value()) out.pushKV("head", ReplyHeadJson(*reply.head));
    if (reply.included.has_value()) out.pushKV("included", IncludedOutcomeJson(*reply.included));
    if (reply.expiry.has_value()) out.pushKV("expiry", WitnessJson(*reply.expiry));
    if (reply.pending.has_value()) {
        UniValue pending{UniValue::VOBJ};
        pending.pushKV("status", UniValue{std::string{admission::ToString(reply.pending->status)}});
        if (reply.pending->pending.has_value()) pending.pushKV("pending", PendingRefJson(*reply.pending->pending));
        if (reply.pending->included.has_value()) pending.pushKV("included", IncludedRefJson(*reply.pending->included));
        pending.pushKV("height", U64(reply.pending->height));
        out.pushKV("pending", pending);
    }
    return out;
}

UniValue AccountReplyJson(const read::AccountReply& reply)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("head", ReplyHeadJson(reply.head));
    out.pushKV("status", UniValue{reply.status == read::LookupStatus::FOUND ? "FOUND" : "UNKNOWN"});
    if (!reply.reason.empty()) out.pushKV("reason", UniValue{std::string{reply.reason}});
    if (reply.witness.has_value()) out.pushKV("witness", WitnessJson(*reply.witness));
    return out;
}

UniValue HealthJson(const net::HealthReply& reply)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("fenced", UniValue{reply.fenced});
    if (reply.published_height.has_value()) out.pushKV("published_height", U64(*reply.published_height));
    out.pushKV("pool_height", U64(reply.pool_height));
    out.pushKV("pool_live", U64(reply.pool_live));
    UniValue lane{UniValue::VOBJ};
    lane.pushKV("messages", U64(reply.action_lane.messages));
    lane.pushKV("payload_bytes", U64(reply.action_lane.payload_bytes));
    lane.pushKV("closed", UniValue{reply.action_lane.closed});
    out.pushKV("action_lane", lane);
    return out;
}

std::string CursorText(const net::StreamCursor& cursor)
{
    return std::to_string(cursor.stream_epoch) + ":" + std::to_string(cursor.seq) + ":" + std::to_string(cursor.head_slot);
}

std::optional<net::StreamCursor> CursorFromText(std::string_view text)
{
    net::StreamCursor cursor;
    uint64_t* fields[]{&cursor.stream_epoch, &cursor.seq, &cursor.head_slot};
    size_t field{0};
    size_t start{0};
    for (size_t i{0}; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == ':') {
            if (field >= 3) return std::nullopt;
            const std::string_view part{text.substr(start, i - start)};
            if (part.empty()) return std::nullopt;
            uint64_t value{0};
            const auto [ptr, ec]{std::from_chars(part.data(), part.data() + part.size(), value)};
            if (ec != std::errc{} || ptr != part.data() + part.size()) return std::nullopt;
            *fields[field++] = value;
            start = i + 1;
        }
    }
    if (field != 3) return std::nullopt;
    return cursor;
}

UniValue StreamEventJson(const net::StreamEvent& event)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("cursor", UniValue{CursorText(event.cursor)});
    out.pushKV("kind", UniValue{std::string{net::ToString(event.kind)}});
    out.pushKV("head_slot", U64(event.cursor.head_slot));
    std::visit(
        [&](const auto& body) {
            using T = std::decay_t<decltype(body)>;
            if constexpr (std::is_same_v<T, net::SlotEvent>) {
                out.pushKV("channel", UniValue{"slots"});
                out.pushKV("height", U64(body.height));
                out.pushKV("header_hash", Hex(body.header_hash));
                out.pushKV("state_root", Hex(body.state_root));
                out.pushKV("action_count", U64(body.action_count));
                out.pushKV("outcome_count", U64(body.outcome_count));
                out.pushKV("envelope", Hex(body.envelope));
            } else if constexpr (std::is_same_v<T, net::AccountEvent>) {
                out.pushKV("channel", UniValue{"account"});
                out.pushKV("account", Hex(body.account.id));
                if (body.leaf.has_value()) out.pushKV("leaf", LeafJson(*body.leaf));
                UniValue deltas{UniValue::VARR};
                for (const BalanceDelta& delta : body.deltas) deltas.push_back(DeltaJson(delta));
                out.pushKV("deltas", deltas);
            } else if constexpr (std::is_same_v<T, net::BookEvent>) {
                out.pushKV("channel", UniValue{"book"});
                out.pushKV("market", Hex(body.market.id));
                out.pushKV("bids", LevelsJson(body.bids));
                out.pushKV("asks", LevelsJson(body.asks));
            } else {
                out.pushKV("channel", UniValue{"trades"});
                out.pushKV("market", Hex(body.market.id));
                UniValue fills{UniValue::VARR};
                for (const Fill& fill : body.fills) fills.push_back(FillJson(fill));
                out.pushKV("fills", fills);
            }
        },
        event.body);
    return out;
}

} // namespace flowmesh2::netio
