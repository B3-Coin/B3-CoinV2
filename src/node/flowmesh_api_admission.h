// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_FLOWMESH_API_ADMISSION_H
#define B3COIN_NODE_FLOWMESH_API_ADMISSION_H

#include <node/flowmesh_action_wait.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace node {

//! The exact 429 body of a public trading request refused before admission.
inline constexpr std::string_view FLOWMESH_API_BUDGET_EXHAUSTED_BODY{
    R"({"ok":false,"error":"Public trading request budget exhausted before admission"})"};

enum class FlowMeshApiRequestClass : uint8_t { READ = 0, TRADE_WRITE = 1 };

/** Pre-parse, O(18): a request is TRADE_WRITE only when its body starts
 * with the exact compact prefix every in-tree client writes. Anything else
 * (whitespace, reordered keys) is charged as READ; the totals and the order
 * sub-budget still bind it, so a non-compact submit gains nothing. */
inline FlowMeshApiRequestClass ClassifyFlowMeshApiRequest(const std::string_view body)
{
    static constexpr std::string_view SUBMIT_PREFIX{R"({"method":"submit")"};
    return body.starts_with(SUBMIT_PREFIX) ? FlowMeshApiRequestClass::TRADE_WRITE : FlowMeshApiRequestClass::READ;
}

/** Requests per one-second window. */
struct FlowMeshApiAdmissionLimits {
    unsigned global_total{128}, global_write{64}, global_read{112};
    unsigned peer_total{32}, peer_write{16}, peer_read{28};
    unsigned global_order{48}, peer_order{12};
    size_t identities{1024};
};

/**
 * Public trading API admission budgets: fixed one-second windows, globally
 * and per client identity (IPv4 as given, IPv4-mapped IPv6 as IPv4, other
 * IPv6 by /64). Every limit is checked before anything is charged, so a
 * refused request costs nothing: one flooding identity can never spend the
 * global budget of everyone else.
 *
 * Every bound is at most the previous one (128/s global and 32/s per
 * client, both totals unchanged); each class sits below its total, leaving
 * the other class a floor (writes keep >= 16/s global and >= 4/s per
 * identity, reads >= 64/s and >= 16/s). The order sub-budget caps order
 * and deposit submissions inside the write class, so cancels and
 * withdrawals keep room when orders flood.
 */
class FlowMeshApiAdmission {
public:
    using Limits = FlowMeshApiAdmissionLimits;
    enum class Result : uint8_t { ADMITTED, PEER, GLOBAL, IDENTITIES };
    using Clock = std::chrono::steady_clock;

    FlowMeshApiAdmission() = default;
    explicit FlowMeshApiAdmission(const Limits& limits) : m_limits{limits} {}

    //! Charged before parsing: one request of `request_class`.
    Result Admit(const std::string& remote_address, const FlowMeshApiRequestClass request_class,
                 const Clock::time_point now)
    {
        std::lock_guard lock{m_mutex};
        Counts* peer{nullptr};
        if (const auto result{Find(remote_address, now, peer)}; result != Result::ADMITTED) return result;
        if (!Room(m_global, request_class, m_limits.global_total, m_limits.global_write, m_limits.global_read)) {
            return Result::GLOBAL;
        }
        if (!Room(*peer, request_class, m_limits.peer_total, m_limits.peer_write, m_limits.peer_read)) {
            return Result::PEER;
        }
        Charge(m_global, request_class);
        Charge(*peer, request_class);
        return Result::ADMITTED;
    }

    //! A post-parse extra token of the same class and total (a request that
    //! asked for more work). False, and nothing charged, when either is spent.
    bool ChargeExtra(const std::string& remote_address, const FlowMeshApiRequestClass request_class,
                     const Clock::time_point now)
    {
        return Admit(remote_address, request_class, now) == Result::ADMITTED;
    }

    //! The order sub-budget, inside the write class: false, and nothing
    //! charged, when the identity's or the global order budget is spent.
    bool ChargeOrder(const std::string& remote_address, const Clock::time_point now)
    {
        std::lock_guard lock{m_mutex};
        Counts* peer{nullptr};
        if (Find(remote_address, now, peer) != Result::ADMITTED) return false;
        if (m_global.order >= m_limits.global_order || peer->order >= m_limits.peer_order) return false;
        ++m_global.order;
        ++peer->order;
        return true;
    }

    const Limits& GetLimits() const { return m_limits; }

private:
    struct Counts {
        Clock::time_point window{};
        unsigned total{0}, write{0}, read{0}, order{0};
    };

    static void Roll(Counts& counts, const Clock::time_point now)
    {
        if (now - counts.window >= std::chrono::seconds{1}) counts = Counts{now};
    }

    static bool Room(const Counts& counts, const FlowMeshApiRequestClass request_class, const unsigned total,
                     const unsigned write, const unsigned read)
    {
        if (counts.total >= total) return false;
        return request_class == FlowMeshApiRequestClass::TRADE_WRITE ? counts.write < write : counts.read < read;
    }

    static void Charge(Counts& counts, const FlowMeshApiRequestClass request_class)
    {
        ++counts.total;
        ++(request_class == FlowMeshApiRequestClass::TRADE_WRITE ? counts.write : counts.read);
    }

    //! Caller holds m_mutex. Rolls both windows and finds (or makes room
    //! for) the identity; refuses only when the identity map is full.
    Result Find(const std::string& remote_address, const Clock::time_point now, Counts*& peer)
    {
        Roll(m_global, now);
        auto key{FlowMeshActionWaitSlots::PeerKey(remote_address)};
        auto it{m_peers.find(key)};
        if (it == m_peers.end()) {
            if (m_peers.size() >= m_limits.identities) {
                std::erase_if(m_peers, [&](const auto& pair) { return now - pair.second.window > std::chrono::minutes{1}; });
                if (m_peers.size() >= m_limits.identities) return Result::IDENTITIES;
            }
            it = m_peers.emplace(std::move(key), Counts{now}).first;
        }
        Roll(it->second, now);
        peer = &it->second;
        return Result::ADMITTED;
    }

    const Limits m_limits{};
    std::mutex m_mutex;
    Counts m_global;
    std::map<std::string, Counts> m_peers;
};

} // namespace node

#endif // B3COIN_NODE_FLOWMESH_API_ADMISSION_H
