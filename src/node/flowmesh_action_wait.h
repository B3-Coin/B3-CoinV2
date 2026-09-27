// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_NODE_FLOWMESH_ACTION_WAIT_H
#define BITCOIN_NODE_FLOWMESH_ACTION_WAIT_H

#include <compat/compat.h>
#include <crypto/hex_base.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace node {

/** Admission of bounded server-side waits on the public 'action' read.
 *
 * This is a thread-capacity policy, NOT a request budget: every request is
 * still charged against the API's request budget before it is decoded. A slot
 * is taken without blocking; a caller that gets none receives the immediate,
 * unchanged reply. At most `capacity` slots exist at once and at most
 * MAX_PER_PEER per client key (PeerKey), so the per-key map never holds more
 * than `capacity` entries.
 *
 * Close() refuses new slots and interrupts every existing one through its
 * epoch, under the mutex TryAcquire uses: a handler either holds a slot from
 * before Close (and observes Interrupted) or is refused. Open() readmits;
 * slots from before a Close stay interrupted. A Slot must not outlive its
 * FlowMeshActionWaitSlots. */
class FlowMeshActionWaitSlots {
public:
    static constexpr size_t MAX_PER_PEER{2};

    class Slot {
    public:
        Slot(Slot&& other) noexcept
            : m_owner{std::exchange(other.m_owner, nullptr)}, m_key{std::move(other.m_key)}, m_epoch{other.m_epoch} {}
        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;
        Slot& operator=(Slot&&) = delete;
        ~Slot()
        {
            if (m_owner) m_owner->Release(m_key);
        }

    private:
        friend class FlowMeshActionWaitSlots;
        Slot(FlowMeshActionWaitSlots& owner, std::string key, uint64_t epoch)
            : m_owner{&owner}, m_key{std::move(key)}, m_epoch{epoch} {}
        FlowMeshActionWaitSlots* m_owner;
        std::string m_key;
        uint64_t m_epoch;
    };

    explicit FlowMeshActionWaitSlots(size_t capacity) : m_capacity{capacity} {}
    FlowMeshActionWaitSlots(const FlowMeshActionWaitSlots&) = delete;
    FlowMeshActionWaitSlots& operator=(const FlowMeshActionWaitSlots&) = delete;

    //! Never blocks. Refused while closed, full, or at the peer's limit.
    std::optional<Slot> TryAcquire(const std::string& remote_address)
    {
        auto key{PeerKey(remote_address)};
        std::lock_guard lock{m_mutex};
        if (!m_open || m_active >= m_capacity) return std::nullopt;
        const auto it{m_per_peer.find(key)};
        if (it != m_per_peer.end() && it->second >= MAX_PER_PEER) return std::nullopt;
        // A new key is added only below capacity, so keys <= active <= capacity.
        ++m_per_peer[key];
        ++m_active;
        return Slot{*this, std::move(key), m_epoch.load(std::memory_order_acquire)};
    }

    //! Lock-free: safe as a wait predicate evaluated under another mutex.
    bool Interrupted(const Slot& slot) const
    {
        return m_epoch.load(std::memory_order_acquire) != slot.m_epoch;
    }

    void Close()
    {
        std::lock_guard lock{m_mutex};
        m_open = false;
        m_epoch.fetch_add(1, std::memory_order_acq_rel);
    }

    void Open()
    {
        std::lock_guard lock{m_mutex};
        m_open = true;
    }

    size_t Capacity() const { return m_capacity; }
    size_t Active() const
    {
        std::lock_guard lock{m_mutex};
        return m_active;
    }
    size_t Peers() const
    {
        std::lock_guard lock{m_mutex};
        return m_per_peer.size();
    }

    /** Client key for the per-peer limit: an IPv4 address as given, an
     * IPv4-mapped IPv6 address as its IPv4 address, and any other IPv6
     * address by its /64, since one host commonly holds a whole /64. Other
     * text (never produced by the server) is its own key. */
    static std::string PeerKey(const std::string& address)
    {
        in6_addr v6{};
        if (address.find(':') == std::string::npos || inet_pton(AF_INET6, address.c_str(), &v6) != 1) return address;
        std::array<unsigned char, 16> bytes{};
        std::copy_n(reinterpret_cast<const unsigned char*>(&v6), bytes.size(), bytes.begin());
        static constexpr std::array<unsigned char, 12> MAPPED{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (std::equal(MAPPED.begin(), MAPPED.end(), bytes.begin())) {
            return std::to_string(bytes[12]) + "." + std::to_string(bytes[13]) + "." +
                   std::to_string(bytes[14]) + "." + std::to_string(bytes[15]);
        }
        return "ipv6/64:" + HexStr(std::span{bytes}.first(8));
    }

private:
    void Release(const std::string& key)
    {
        std::lock_guard lock{m_mutex};
        const auto it{m_per_peer.find(key)};
        if (it != m_per_peer.end() && --it->second == 0) m_per_peer.erase(it);
        --m_active;
    }

    mutable std::mutex m_mutex;
    const size_t m_capacity;
    size_t m_active{0};
    std::map<std::string, size_t> m_per_peer;
    bool m_open{true};
    std::atomic<uint64_t> m_epoch{0};
};

} // namespace node

#endif // BITCOIN_NODE_FLOWMESH_ACTION_WAIT_H
