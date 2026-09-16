// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <flowmesh/client_evidence.h>

#include <streams.h>

#include <algorithm>
#include <limits>

namespace flowmesh {
namespace {

class BoundedStateWriter {
public:
    std::vector<unsigned char> bytes;
    void write(std::span<const std::byte> input)
    {
        if (input.empty()) return;
        if (input.size() > CLIENT_STATE_MAX_BYTES - bytes.size()) {
            throw std::ios_base::failure("FlowMesh client state exceeds 8 MiB");
        }
        const auto* begin{reinterpret_cast<const unsigned char*>(input.data())};
        bytes.insert(bytes.end(), begin, begin + input.size());
    }
    template <typename T> BoundedStateWriter& operator<<(const T& value)
    {
        ::Serialize(*this, value);
        return *this;
    }
};

int StatusPriority(const ClientEventKind kind)
{
    switch (kind) {
    case ClientEventKind::CERTIFIED_INCLUDED: return 3;
    case ClientEventKind::POOL_ADMITTED: return 2;
    case ClientEventKind::POOL_REFUSED: return 1;
    default: return 0;
    }
}

} // namespace

bool CheckClientEvidencePins(const ClientEvidencePins& pins)
{
    const auto market{ComputeFlowMeshMarketId(pins.domain, pins.base_asset)};
    const auto vault{market ? ComputeFlowMeshVaultId(pins.domain, *market) : std::nullopt};
    return market && vault && *market == pins.market_id && *vault == pins.vault_id &&
           pins.execution_config_id == ComputeExecutionConfigId(
               *vault, pins.base_asset, modern::NativeAsset(), FLOWMESH_V1_MAX_CURVE_POINTS);
}

std::optional<std::vector<unsigned char>> EncodeClientState(
    const FlowMeshState& state, std::string& error)
{
    try {
        BoundedStateWriter writer;
        writer << state;
        return std::move(writer.bytes);
    } catch (const std::exception& e) {
        error = e.what();
        return std::nullopt;
    }
}

std::optional<VerifiedClientState> VerifyClientStateEvidence(
    const ClientEvidencePins& pins, const ClientStateEvidence& evidence,
    const ActiveFnBlsSeatSet& seats,
    const std::function<bool(const AnchorRef&)>& canonical_anchor,
    std::string& error)
{
    if (!CheckClientEvidencePins(pins)) {
        error = "FlowMesh client market/domain/configuration pins are invalid";
        return std::nullopt;
    }
    if (evidence.state_bytes.empty() || evidence.state_bytes.size() > CLIENT_STATE_MAX_BYTES ||
        evidence.certified_payload.empty() ||
        evidence.certified_payload.size() > FLOWMESH_V1_MAX_MICROBLOCK_BYTES +
            FLOWMESH_CERTIFIED_PAYLOAD_PREFIX_SIZE + FLOWMESH_BLS_CERTIFICATE_MAX_SIZE) {
        error = "FlowMesh client evidence is missing or exceeds its bound";
        return std::nullopt;
    }
    if (seats.market_id != pins.market_id ||
        CheckActiveFnBlsSeatSet(pins.domain, seats) != BlsSeatSetCheck::OK ||
        seats.anchor_height > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
        error = "FlowMesh client anchored seat authority is invalid";
        return std::nullopt;
    }
    const auto certified{DecodeProductionCertifiedPayload(evidence.certified_payload, seats.Size())};
    if (!certified || certified->entry.domain != pins.domain ||
        certified->entry.market_id != pins.market_id ||
        certified->entry.epoch != seats.epoch || certified->entry.seat_set_hash != seats.set_hash) {
        error = "FlowMesh client certified entry does not match pinned context";
        return std::nullopt;
    }
    const AnchorRef seat_anchor{static_cast<int32_t>(seats.anchor_height), seats.anchor_hash};
    if (!canonical_anchor || !canonical_anchor(seat_anchor) ||
        !canonical_anchor(certified->entry.anchor) ||
        certified->entry.anchor.height < seat_anchor.height) {
        error = "FlowMesh client evidence anchor is not acceptable";
        return std::nullopt;
    }
    if (CheckProductionEntryCertificate(certified->entry, seats, certified->certificate) !=
        BlsCertificateCheck::OK) {
        error = "FlowMesh client quorum certificate is invalid";
        return std::nullopt;
    }
    try {
        FlowMeshState state{pins.vault_id, pins.base_asset, modern::NativeAsset(),
                            FLOWMESH_V1_MAX_CURVE_POINTS};
        SpanReader reader{evidence.state_bytes};
        reader >> state;
        if (!reader.empty() || state.Root() != certified->entry.state_root) {
            error = "FlowMesh client state does not match the certified root";
            return std::nullopt;
        }
        return VerifiedClientState{*certified, std::move(state), seats.Size()};
    } catch (const std::exception&) {
        error = "FlowMesh client state is malformed or has inconsistent accounting";
        return std::nullopt;
    }
}

std::optional<MarketData> ClientMarketData(
    const ClientEvidencePins& pins, const VerifiedClientState& verified,
    const std::optional<AccountId>& account, const MarketDataQuery& query,
    std::string& error)
{
    const auto& entry{verified.certified.entry};
    if (!CheckClientEvidencePins(pins) || verified.state.ConfigId() != pins.execution_config_id ||
        entry.domain != pins.domain || entry.market_id != pins.market_id ||
        entry.sequence == std::numeric_limits<uint64_t>::max() ||
        query.limit == 0 || query.limit > MARKET_DATA_MAX_HISTORY ||
        query.curve_limit == 0 || query.curve_limit > MARKET_DATA_MAX_CURVES ||
        (query.curve_cursor && !query.expected_head) ||
        (query.known_head && (query.curve_cursor || query.before_sequence)) ||
        (query.expected_head && *query.expected_head != entry.GetHash())) {
        error = "FlowMesh client snapshot/query context is invalid";
        return std::nullopt;
    }
    MarketData out;
    out.domain = pins.domain;
    out.market_id = pins.market_id;
    out.base_asset_id = pins.base_asset;
    out.execution_config_id = pins.execution_config_id;
    out.snapshot.certified = true;
    out.snapshot.next_microblock_sequence = entry.sequence + 1;
    out.snapshot.last_microblock_hash = entry.GetHash();
    out.snapshot.state_root = entry.state_root;
    out.snapshot.epoch = entry.epoch;
    out.snapshot.anchor_height = entry.anchor.height;
    out.snapshot.anchor_hash = entry.anchor.hash;
    out.snapshot.active_seats = verified.active_seats;
    out.snapshot.quorum_required = FlowMeshBlsThreshold(verified.active_seats);
    out.unchanged = query.known_head && *query.known_head == entry.GetHash();
    if (out.unchanged) return out;
    out.liquidity = verified.state.ReadCurves(query.curve_limit, query.curve_cursor);
    if (account) {
        const auto& ledger{verified.state.LedgerView()};
        out.account = MarketAccountData{*account, verified.state.NextSequence(*account),
            ledger.Available(*account, pins.base_asset), ledger.Reserved(*account, pins.base_asset),
            ledger.Available(*account, modern::NativeAsset()), ledger.Reserved(*account, modern::NativeAsset()),
            verified.state.AccountCurves(*account)};
    }
    return out;
}

const char* ClientEventKindName(const ClientEventKind kind)
{
    switch (kind) {
    case ClientEventKind::QUEUE_ADMITTED: return "queue_admitted";
    case ClientEventKind::POOL_ADMITTED: return "pool_admitted";
    case ClientEventKind::POOL_REFUSED: return "pool_refused";
    case ClientEventKind::CERTIFIED_INCLUDED: return "certified_included";
    case ClientEventKind::CERTIFIED_HEAD: return "certified_head";
    }
    return "unknown";
}

ClientEventLog::ClientEventLog(const uint256& instance_id) : m_instance_id{instance_id} {}

void ClientEventLog::Reset(const uint256& instance_id)
{
    std::lock_guard lock{m_mutex};
    m_instance_id = instance_id;
    m_last_event_id = 0;
    m_events.clear();
}

void ClientEventLog::Append(ClientEvent event)
{
    std::lock_guard lock{m_mutex};
    // Practically unreachable; fail closed rather than wrap a resumable cursor.
    if (m_last_event_id == std::numeric_limits<uint64_t>::max()) return;
    event.event_id = ++m_last_event_id;
    event.reason.resize(std::min<size_t>(event.reason.size(), 160));
    m_events.push_back(std::move(event));
    if (m_events.size() > CLIENT_EVENT_CAPACITY) m_events.pop_front();
}

ClientEventCursor ClientEventLog::Cursor() const
{
    std::lock_guard lock{m_mutex};
    return {m_instance_id, m_last_event_id};
}

ClientEventPage ClientEventLog::Read(const std::optional<ClientEventCursor>& after,
                                    const std::optional<MarketId>& market,
                                    const std::optional<AccountId>& account,
                                    const size_t limit) const
{
    std::lock_guard lock{m_mutex};
    ClientEventPage out;
    out.latest_event_id = m_last_event_id;
    out.oldest_event_id = m_events.empty() ? 0 : m_events.front().event_id;
    out.cursor = {m_instance_id, after ? after->event_id : 0};
    if (limit == 0 || limit > CLIENT_EVENT_PAGE_MAX ||
        (after && (after->instance_id != m_instance_id || after->event_id > m_last_event_id ||
                   (!m_events.empty() && after->event_id < m_events.front().event_id - 1)))) {
        out.gap = true;
        out.cursor.event_id = m_last_event_id;
        return out;
    }
    for (const auto& event : m_events) {
        if (event.event_id <= out.cursor.event_id) continue;
        if (out.events.size() == limit) { out.more = true; break; }
        out.cursor.event_id = event.event_id;
        if (market && event.market_id != *market) continue;
        if (account && event.kind != ClientEventKind::CERTIFIED_HEAD && event.account_id != *account) continue;
        out.events.push_back(event);
    }
    if (!out.more) out.cursor.event_id = m_last_event_id;
    return out;
}

std::optional<ClientEvent> ClientEventLog::ActionStatus(const MarketId& market,
                                                      const uint256& action_id) const
{
    std::lock_guard lock{m_mutex};
    std::optional<ClientEvent> out;
    if (action_id.IsNull()) return out;
    for (auto it{m_events.rbegin()}; it != m_events.rend(); ++it) {
        if (it->market_id != market || it->action_id != action_id) continue;
        if (!out || StatusPriority(it->kind) > StatusPriority(out->kind)) out = *it;
        if (out->kind == ClientEventKind::CERTIFIED_INCLUDED) break;
    }
    return out;
}

} // namespace flowmesh
