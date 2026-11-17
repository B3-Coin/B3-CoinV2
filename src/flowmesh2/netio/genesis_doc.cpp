// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/netio/genesis_doc.h>

#include <charconv>

#include <crypto/bls.h>
#include <flowmesh2/netio/api_json.h>
#include <univalue.h>
#include <util/chaintype.h>

#include <algorithm>

namespace flowmesh2::netio {

namespace {

template <size_t N>
bool HexInto(const UniValue& value, std::array<unsigned char, N>& out)
{
    if (!value.isStr()) return false;
    const std::optional<std::vector<unsigned char>> bytes{FromHex(value.get_str())};
    if (!bytes.has_value() || bytes->size() != N) return false;
    std::copy(bytes->begin(), bytes->end(), out.begin());
    return true;
}

bool HashInto(const UniValue& value, uint256& out)
{
    if (!value.isStr()) return false;
    const std::optional<uint256> hash{uint256::FromHex(value.get_str())};
    if (!hash.has_value()) return false;
    out = *hash;
    return true;
}

//! An unsigned integer written as a JSON number: digits only (no sign, fraction or exponent), in range. UniValue's getInt
//! throws on anything else, which crashed the tools on a malformed document (review 2026-11-16, R3).
bool U64Into(const UniValue& value, uint64_t& out)
{
    if (!value.isNum()) return false;
    const std::string& text{value.getValStr()};
    if (text.empty() || text.size() > 20) return false;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
    }
    uint64_t parsed{0};
    const auto [end, error]{std::from_chars(text.data(), text.data() + text.size(), parsed)};
    if (error != std::errc{} || end != text.data() + text.size()) return false;
    out = parsed;
    return true;
}

Error Decode(std::string_view detail) { return Error{ErrorCode::DECODE, detail}; }

} // namespace

std::string GenesisDoc::Json() const
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("format", UniValue{std::string{GENESIS_FORMAT}});
    UniValue net{UniValue::VOBJ};
    net.pushKV("chain", UniValue{ChainTypeToString(network.chain)});
    net.pushKV("magic", UniValue{ToHex(network.magic)});
    net.pushKV("domain", UniValue{network.modern_chain_domain.GetHex()});
    out.pushKV("network", net);
    out.pushKV("mesh_magic", UniValue{ToHex(std::span<const unsigned char>{mesh_magic.data(), mesh_magic.size()})});
    UniValue base_v{UniValue::VOBJ};
    base_v.pushKV("height", UniValue{static_cast<uint64_t>(base.height)});
    base_v.pushKV("hash", UniValue{base.hash.GetHex()});
    out.pushKV("base", base_v);
    UniValue start{UniValue::VOBJ};
    start.pushKV("height", UniValue{static_cast<uint64_t>(facts_start.height)});
    start.pushKV("index", UniValue{static_cast<uint64_t>(facts_start.index)});
    out.pushKV("facts_start", start);
    UniValue seats_v{UniValue::VARR};
    for (const GenesisSeat& seat : seats) {
        UniValue s{UniValue::VOBJ};
        s.pushKV("index", UniValue{static_cast<uint64_t>(seat.index)});
        s.pushKV("bls_pubkey", UniValue{ToHex(seat.bls_pubkey)});
        s.pushKV("pop", UniValue{ToHex(seat.pop)});
        UniValue mesh{UniValue::VOBJ};
        mesh.pushKV("address", UniValue{seat.mesh_address});
        mesh.pushKV("port", UniValue{static_cast<uint64_t>(seat.mesh_port)});
        s.pushKV("mesh", mesh);
        UniValue api{UniValue::VOBJ};
        api.pushKV("address", UniValue{seat.api_address});
        api.pushKV("port", UniValue{static_cast<uint64_t>(seat.api_port)});
        s.pushKV("api", api);
        UniValue record{UniValue::VOBJ};
        record.pushKV("transport_key", UniValue{ToHex(seat.endpoint.transport_key)});
        record.pushKV("valid_from_epoch", UniValue{seat.endpoint.valid_from_epoch});
        record.pushKV("valid_until_epoch", UniValue{seat.endpoint.valid_until_epoch});
        record.pushKV("counter", UniValue{seat.endpoint.counter});
        record.pushKV("signature", UniValue{ToHex(seat.endpoint.signature)});
        s.pushKV("endpoint_record", record);
        seats_v.push_back(s);
    }
    out.pushKV("seats", seats_v);
    UniValue assets_v{UniValue::VOBJ};
    for (const auto& [name, id] : assets) assets_v.pushKV(name, UniValue{id.id.GetHex()});
    out.pushKV("assets", assets_v);
    UniValue markets_v{UniValue::VARR};
    for (const GenesisMarket& market : markets) {
        UniValue m{UniValue::VOBJ};
        m.pushKV("base", UniValue{market.base});
        m.pushKV("quote", UniValue{market.quote});
        m.pushKV("base_atoms_per_lot", UniValue{market.base_atoms_per_lot});
        m.pushKV("quote_atoms_per_tick", UniValue{market.quote_atoms_per_tick});
        markets_v.push_back(m);
    }
    out.pushKV("markets", markets_v);
    return out.write(2) + "\n";
}

Result<GenesisDoc> GenesisDoc::Parse(std::string_view json)
{
    UniValue root;
    if (!root.read(std::string{json}) || !root.isObject()) return Decode(GENESIS_JSON);
    if (!root["format"].isStr() || root["format"].get_str() != GENESIS_FORMAT) return Decode(GENESIS_FORMAT_UNKNOWN);
    GenesisDoc doc;
    const UniValue& net{root["network"]};
    if (!net.isObject() || !net["chain"].isStr()) return Decode(GENESIS_JSON);
    const std::optional<ChainType> chain{ChainTypeFromString(net["chain"].get_str())};
    if (!chain.has_value() || *chain != ChainType::REGTEST) return Error{ErrorCode::UNSUPPORTED_NETWORK, GENESIS_NOT_REGTEST};
    doc.network.chain = *chain;
    if (!HexInto(net["magic"], doc.network.magic) || !HashInto(net["domain"], doc.network.modern_chain_domain)) return Decode(GENESIS_HEX);
    std::array<unsigned char, 4> mesh_magic{};
    if (!HexInto(root["mesh_magic"], mesh_magic)) return Decode(GENESIS_HEX);
    std::copy(mesh_magic.begin(), mesh_magic.end(), doc.mesh_magic.begin());
    const UniValue& base_v{root["base"]};
    uint64_t height{0};
    if (!base_v.isObject() || !U64Into(base_v["height"], height) || height > UINT32_MAX || !HashInto(base_v["hash"], doc.base.hash)) {
        return Decode(GENESIS_JSON);
    }
    doc.base.height = static_cast<uint32_t>(height);
    const UniValue& start{root["facts_start"]};
    uint64_t start_height{0}, start_index{0};
    if (!start.isObject() || !U64Into(start["height"], start_height) || !U64Into(start["index"], start_index) || start_height > UINT32_MAX ||
        start_index > UINT32_MAX) {
        return Decode(GENESIS_JSON);
    }
    doc.facts_start = FactCursor{static_cast<uint32_t>(start_height), static_cast<uint32_t>(start_index)};
    const UniValue& seats_v{root["seats"]};
    if (!seats_v.isArray() || seats_v.empty()) return Decode(GENESIS_SEATS);
    for (size_t i{0}; i < seats_v.size(); ++i) {
        const UniValue& s{seats_v[i]};
        GenesisSeat seat;
        uint64_t index{0}, mesh_port{0}, api_port{0};
        if (!s.isObject() || !U64Into(s["index"], index) || index != i) return Decode(GENESIS_SEATS);
        seat.index = static_cast<uint32_t>(index);
        if (!HexInto(s["bls_pubkey"], seat.bls_pubkey) || !HexInto(s["pop"], seat.pop)) return Decode(GENESIS_HEX);
        const UniValue& mesh{s["mesh"]};
        const UniValue& api{s["api"]};
        if (!mesh.isObject() || !mesh["address"].isStr() || !U64Into(mesh["port"], mesh_port) || mesh_port == 0 || mesh_port > 65535 ||
            !api.isObject() || !api["address"].isStr() || !U64Into(api["port"], api_port) || api_port > 65535) {
            return Decode(GENESIS_JSON);
        }
        seat.mesh_address = mesh["address"].get_str();
        seat.mesh_port = static_cast<uint16_t>(mesh_port);
        seat.api_address = api["address"].get_str();
        seat.api_port = static_cast<uint16_t>(api_port);
        const UniValue& record{s["endpoint_record"]};
        if (!record.isObject() || !HexInto(record["transport_key"], seat.endpoint.transport_key) || !HexInto(record["signature"], seat.endpoint.signature) ||
            !U64Into(record["valid_from_epoch"], seat.endpoint.valid_from_epoch) || !U64Into(record["valid_until_epoch"], seat.endpoint.valid_until_epoch) ||
            !U64Into(record["counter"], seat.endpoint.counter)) {
            return Decode(GENESIS_JSON);
        }
        seat.endpoint.seat = seat.index;
        seat.endpoint.addresses = {seat.mesh_address};
        seat.endpoint.port = seat.mesh_port;
        doc.seats.push_back(std::move(seat));
    }
    const UniValue& assets_v{root["assets"]};
    if (!assets_v.isObject()) return Decode(GENESIS_JSON);
    for (const std::string& name : assets_v.getKeys()) {
        uint256 id;
        if (!HashInto(assets_v[name], id)) return Decode(GENESIS_HEX);
        doc.assets.emplace(name, AssetId{id});
    }
    const UniValue& markets_v{root["markets"]};
    if (!markets_v.isArray()) return Decode(GENESIS_JSON);
    for (size_t i{0}; i < markets_v.size(); ++i) {
        const UniValue& m{markets_v[i]};
        GenesisMarket market;
        if (!m.isObject() || !m["base"].isStr() || !m["quote"].isStr() || !U64Into(m["base_atoms_per_lot"], market.base_atoms_per_lot) ||
            !U64Into(m["quote_atoms_per_tick"], market.quote_atoms_per_tick)) {
            return Decode(GENESIS_JSON);
        }
        market.base = m["base"].get_str();
        market.quote = m["quote"].get_str();
        if (!doc.assets.contains(market.base) || !doc.assets.contains(market.quote)) return Decode(GENESIS_ASSET_UNKNOWN);
        doc.markets.push_back(std::move(market));
    }
    return Ok(std::move(doc));
}

uint256 GenesisDoc::Domain() const { return DomainHash(network, FM_VERSION); }

Result<SeatSet> GenesisDoc::SeatSetOf() const
{
    std::vector<SeatRegistration> registrations;
    for (const GenesisSeat& seat : seats) {
        const std::optional<bls::PublicKey> key{bls::PublicKey::Decode(seat.bls_pubkey)};
        const std::optional<bls::Signature> pop{bls::Signature::Decode(seat.pop)};
        if (!key.has_value() || !pop.has_value()) return Error{ErrorCode::DECODE, GENESIS_HEX};
        registrations.push_back(SeatRegistration{*key, *pop});
    }
    return SeatSet::FromPoP(registrations);
}

AuthorityVersion GenesisDoc::Authority(const ProfileParams& profile, const SeatSet& seat_set) const
{
    return AuthorityVersion{0, seat_set.Root(), ProfileHash(profile)};
}

std::optional<AssetId> GenesisDoc::Asset(std::string_view name) const
{
    const auto it{assets.find(std::string{name})};
    if (it == assets.end()) return std::nullopt;
    return it->second;
}

Result<std::vector<MarketDescriptor>> GenesisDoc::Descriptors() const
{
    std::vector<MarketDescriptor> out;
    for (const GenesisMarket& market : markets) {
        const std::optional<AssetId> base_id{Asset(market.base)};
        const std::optional<AssetId> quote_id{Asset(market.quote)};
        if (!base_id.has_value() || !quote_id.has_value()) return Error{ErrorCode::DECODE, GENESIS_ASSET_UNKNOWN};
        out.push_back(MarketDescriptor{*base_id, *quote_id, market.base_atoms_per_lot, market.quote_atoms_per_tick});
    }
    return Ok(std::move(out));
}

Result<MarketDescriptor> GenesisDoc::Market(std::string_view base, std::string_view quote) const
{
    for (const GenesisMarket& market : markets) {
        if (market.base == base && market.quote == quote) {
            const std::optional<AssetId> base_id{Asset(market.base)};
            const std::optional<AssetId> quote_id{Asset(market.quote)};
            if (!base_id.has_value() || !quote_id.has_value()) return Error{ErrorCode::DECODE, GENESIS_ASSET_UNKNOWN};
            return Ok(MarketDescriptor{*base_id, *quote_id, market.base_atoms_per_lot, market.quote_atoms_per_tick});
        }
    }
    return Error{ErrorCode::INVALID_INPUT, GENESIS_ASSET_UNKNOWN};
}

Result<state::HeadView> GenesisDoc::GenesisHead(const ProfileParams& profile, const SeatSet& seat_set) const
{
    Result<std::vector<MarketDescriptor>> descriptors{Descriptors()};
    if (!descriptors.has_value()) return descriptors.error();
    return state::GenesisHead(profile, *descriptors, Domain(), Authority(profile, seat_set), base, facts_start);
}

std::vector<net::SeatEndpointRecord> GenesisDoc::Endpoints() const
{
    std::vector<net::SeatEndpointRecord> out;
    for (const GenesisSeat& seat : seats) out.push_back(seat.endpoint);
    return out;
}

} // namespace flowmesh2::netio
