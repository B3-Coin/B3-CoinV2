// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_TEST_FLOWMESH_FASTPATH_PROBE_FIXTURE_H
#define B3COIN_TEST_FLOWMESH_FASTPATH_PROBE_FIXTURE_H

#include <flowmesh/production_engine.h>
#include <key.h>

#include <memory>
#include <optional>
#include <span>
#include <vector>

// TEST ONLY. All identities, keys, custody and anchors are synthetic. This
// fixture supplies real authenticated production curve-auction execution, NOT
// a consensus protocol, a custody proof, durable storage or a bridge proof.
// The executable must create one process-lifetime ECC_Context before use.
namespace fastprobe {

using Bytes = std::vector<unsigned char>;
inline constexpr uint64_t MAX_REQUESTS{128};
inline constexpr CAmount TRADE_PRICE{100'000};
inline constexpr CAmount TRADE_QUANTITY{1};
inline constexpr CAmount MAKER_QUANTITY{1'000};

struct Fixture {
    Fixture();

    uint256 domain;
    modern::AssetId base;
    flowmesh::MarketId market;
    flowmesh::VaultId vault;
    uint256 treasury;
    CKey buyer_key;
    CKey seller_key;
    flowmesh::AccountId buyer;
    flowmesh::AccountId seller;
    flowmesh::ActiveFnBlsSeatSet seats;
    // Canonical SeatId/bitmap order, NOT independently sorted public keys.
    std::vector<bls::SecretKey> seat_keys;
    flowmesh::AnchorRef anchor;

    uint256 ConfigId() const;
};

// Client-only signing: does not construct an Engine or execute maker setup.
Bytes MakeRequest(const Fixture& fixture, uint64_t buyer_sequence);

struct AccountDelta {
    flowmesh::AccountId account;
    modern::AssetId asset;
    CAmount available_before{0};
    CAmount available_after{0};
    CAmount reserved_before{0};
    CAmount reserved_after{0};
    friend bool operator==(const AccountDelta&, const AccountDelta&) = default;
    SERIALIZE_METHODS(AccountDelta, obj)
    {
        READWRITE(obj.account, obj.asset, obj.available_before,
                  obj.available_after, obj.reserved_before, obj.reserved_after);
    }
};

struct Receipt {
    uint16_t version{1};
    uint256 domain;
    flowmesh::MarketId market;
    uint256 config_id;
    uint256 action_id;
    uint256 entry_hash;
    uint256 result_root;
    uint256 previous_state_root;
    uint256 state_root;
    uint64_t entry_sequence{0};
    uint64_t action_sequence{0};
    CAmount price{0};
    CAmount quantity{0};
    CAmount quote_notional{0};
    CAmount fee_total{0};
    CAmount treasury_fee{0};
    CAmount seat_fee{0};
    std::vector<AccountDelta> deltas;

    template <typename Stream> void Serialize(Stream& s) const
    {
        s << version << domain << market << config_id << action_id
          << entry_hash << result_root << previous_state_root << state_root
          << entry_sequence << action_sequence << price << quantity
          << quote_notional << fee_total << treasury_fee << seat_fee << deltas;
    }
    template <typename Stream> void Unserialize(Stream& s)
    {
        s >> version >> domain >> market >> config_id >> action_id
          >> entry_hash >> result_root >> previous_state_root >> state_root
          >> entry_sequence >> action_sequence >> price >> quantity
          >> quote_notional >> fee_total >> treasury_fee >> seat_fee;
        const auto count{ReadCompactSize(s)};
        if (count != 9) throw std::ios_base::failure("probe receipt delta count");
        deltas.resize(count);
        for (auto& delta : deltas) s >> delta;
    }
};

struct Execution {
    Bytes request_bytes;
    Bytes entry_bytes;
    Bytes receipt_bytes;
    uint256 entry_hash;
    uint256 result_root;
    uint256 state_root;
    uint256 parent_hash;
    uint256 previous_state_root;
    uint64_t entry_sequence;
    uint64_t next_effect_start;
    flowmesh::FlowMeshState next_state;
    Receipt receipt;
};

// Decode, authenticate and bind the exact request/entry, then check the known
// fixture's expected fill and full account-delta arithmetic. Does not re-execute
// the auction. The caller MUST verify a decision certificate binding BOTH the
// entry and exact receipt bytes: an entry-only certificate is not a proof of an
// otherwise uncommitted receipt summary. No freshness/ancestry claim is made.
Receipt ValidateReceipt(const Fixture& fixture,
                        std::span<const unsigned char> request_bytes,
                        std::span<const unsigned char> entry_bytes,
                        std::span<const unsigned char> receipt_bytes);

class Engine {
public:
    explicit Engine(const Fixture& fixture);
    ~Engine();
    Engine(Engine&&) noexcept;
    Engine& operator=(Engine&&) noexcept;
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    Bytes MakeRequest(uint64_t buyer_sequence) const;
    // Leader builds; follower independently authenticates and executes a
    // supplied entry. Neither path mutates this engine or performs disk I/O.
    Execution Execute(std::span<const unsigned char> request_bytes,
                      std::optional<std::span<const unsigned char>> entry_bytes = std::nullopt) const;
    // Move a verified execution into memory after the caller's durable decision
    // boundary. No second clearing. Refuses stale/out-of-order state replacement.
    void Apply(Execution&& execution);
    const flowmesh::FlowMeshState& State() const;
    uint64_t NextEntrySequence() const;
    uint256 Head() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fastprobe

#endif // B3COIN_TEST_FLOWMESH_FASTPATH_PROBE_FIXTURE_H
