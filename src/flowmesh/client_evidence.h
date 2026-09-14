// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_CLIENT_EVIDENCE_H
#define B3COIN_FLOWMESH_CLIENT_EVIDENCE_H

#include <flowmesh/market_data.h>
#include <flowmesh/production_wire.h>

#include <deque>
#include <functional>
#include <mutex>

namespace flowmesh {

// Public-client resource policy, never a consensus/state codec limit.
inline constexpr size_t CLIENT_STATE_MAX_BYTES{8 * 1024 * 1024};
inline constexpr size_t CLIENT_EVENT_CAPACITY{2048};
inline constexpr size_t CLIENT_EVENT_PAGE_MAX{256};

struct ClientEvidencePins {
    uint256 domain;
    MarketId market_id;
    AssetId base_asset;
    VaultId vault_id;
    uint256 execution_config_id;
};

bool CheckClientEvidencePins(const ClientEvidencePins& pins);

struct ClientEventCursor {
    uint256 instance_id;
    uint64_t event_id{0};
};

struct ClientStateEvidence {
    std::vector<unsigned char> certified_payload;
    std::vector<unsigned char> state_bytes;
    // Local delivery position only, not covered by the certificate.
    ClientEventCursor cursor;
};

struct VerifiedClientState {
    ProductionCertifiedEnvelope certified;
    FlowMeshState state;
    size_t active_seats{0};
};

std::optional<std::vector<unsigned char>> EncodeClientState(
    const FlowMeshState& state, std::string& error);

/** Verify the existing quorum certificate and whole-state commitment only.
 * The caller supplies locally established epoch authority and anchor policy.
 * This does NOT establish latest-head freshness, ancestry, B3 settlement or
 * a fresh independently executed result. No executor or signing is invoked. */
std::optional<VerifiedClientState> VerifyClientStateEvidence(
    const ClientEvidencePins& pins, const ClientStateEvidence& evidence,
    const ActiveFnBlsSeatSet& anchored_seats,
    const std::function<bool(const AnchorRef&)>& canonical_anchor,
    std::string& error);

/** Account/book values come exclusively from the authenticated state. Local
 * running/paused status and endpoint-reported history are not authenticated. */
std::optional<MarketData> ClientMarketData(
    const ClientEvidencePins& pins, const VerifiedClientState& verified,
    const std::optional<AccountId>& account, const MarketDataQuery& query,
    std::string& error);

enum class ClientEventKind : uint8_t {
    QUEUE_ADMITTED,
    POOL_ADMITTED,
    POOL_REFUSED,
    CERTIFIED_INCLUDED,
    CERTIFIED_HEAD,
};
const char* ClientEventKindName(ClientEventKind kind);

struct ClientEvent {
    uint64_t event_id{0};
    MarketId market_id;
    ClientEventKind kind{ClientEventKind::QUEUE_ADMITTED};
    uint256 action_id;
    AccountId account_id;
    uint64_t account_sequence{0};
    uint8_t action_type{0};
    // SHA256d of the exact signed action payload, absent for credential-free
    // certified bodies. This is transport correlation, NOT a new ActionId.
    uint256 signed_action_hash;
    uint64_t microblock_sequence{0};
    uint256 microblock_hash;
    std::string reason;
};

struct ClientEventPage {
    ClientEventCursor cursor;
    uint64_t oldest_event_id{0};
    uint64_t latest_event_id{0};
    bool gap{false};
    bool more{false};
    std::vector<ClientEvent> events;
};

/** Memory-only, globally bounded per runtime. No callbacks or network work.
 * An absent action status means unknown/expired, never a definite rejection.
 * Pool refusal is an observation of one attempt, not an execution outcome. */
class ClientEventLog {
public:
    explicit ClientEventLog(const uint256& instance_id);
    void Reset(const uint256& instance_id);
    void Append(ClientEvent event);
    ClientEventCursor Cursor() const;
    ClientEventPage Read(const std::optional<ClientEventCursor>& after,
                         const std::optional<MarketId>& market,
                         const std::optional<AccountId>& account,
                         size_t limit = CLIENT_EVENT_PAGE_MAX) const;
    std::optional<ClientEvent> ActionStatus(const MarketId& market,
                                          const uint256& action_id) const;

private:
    mutable std::mutex m_mutex;
    uint256 m_instance_id;
    uint64_t m_last_event_id{0};
    std::deque<ClientEvent> m_events;
};

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_CLIENT_EVIDENCE_H
