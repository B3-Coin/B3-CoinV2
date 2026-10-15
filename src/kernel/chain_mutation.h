// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_KERNEL_CHAIN_MUTATION_H
#define BITCOIN_KERNEL_CHAIN_MUTATION_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace kernel {

enum class ChainMutationKind : uint8_t {
    CONNECT_TIP, DISCONNECT_TIP, LOAD_TIP, INITIALIZE, ADD, REMOVE, RESET,
    TARGET_CHANGE, VALIDATION_MODE, DESTROY, COINS_VIEW,
    COUNT,
};

/** Local publication bookkeeping ONLY. CURRENT means this process-local
 * generation ticket still matches, not that any chain facts, certificate,
 * signature, balance or spending permission have been verified. */
enum class ChainPublicationCheck : uint8_t {
    CURRENT, SUPERSEDED, FENCED, EXHAUSTED, STOPPED, UNSUPPORTED,
};

class ChainMutationFence;
class ChainMutationScope;

/** Owns its issuer so destruction/reallocation cannot turn an old ticket into
 * a ticket for a different observer at the same address. Never serialized. */
class ChainPublicationTicket {
    friend class ChainMutationFence;
    std::shared_ptr<const ChainMutationFence> m_issuer;
    uint64_t m_generation;
    ChainPublicationTicket(std::shared_ptr<const ChainMutationFence> issuer, uint64_t generation) noexcept
        : m_issuer{std::move(issuer)}, m_generation{generation} {}
public:
    uint64_t Generation() const noexcept { return m_generation; }
};

/** Optional final observer, owned by ChainstateManager options. Its mutation
 * path uses only lock-free scalar atomics: no virtual/user callback, mutex,
 * allocation, logging, disk, waits, or release of published snapshots.
 *
 * Default-null options install nothing. Platforms without always-lock-free
 * uint64 atomics explicitly refuse this optional facility; the normal kernel
 * remains supported and no atomic fallback lock is used by these methods.
 *
 * New instances start fenced. A native mutation scope atomically increments
 * generation AND active depth before live state may change. Capture and
 * completion refuse while a scope is active, including recursive callbacks
 * on the validation thread. Ending a scope does not reopen the fence.
 * A future SINGLE publication owner may capture a ticket
 * while it copies facts under the real chain lock, install an immutable bundle
 * still labelled unavailable, then CompletePublication for the exact ticket.
 * A stale completion can never reopen the fence. Readers must check their
 * bundle's ticket again; completion is not a lease or signing authorization.
 * Manager destruction terminally stops this issuer; retaining it does not
 * lease or permit accessing the destroyed manager. Generations and nested
 * active depth saturate fail-closed rather than wrapping.
 * This component publishes NO facts and does not replace any chain query.
 */
class ChainMutationFence final {
    friend struct ChainMutationFenceTestAccess;
    friend class ChainMutationScope;
    static constexpr unsigned GENERATION_BITS{48};
    static constexpr uint64_t ACTIVE_UNIT{uint64_t{1} << GENERATION_BITS};
    static constexpr uint64_t STOPPED_BIT{uint64_t{1} << 61};
    static constexpr uint64_t FENCED_BIT{uint64_t{1} << 62};
    static constexpr uint64_t EXHAUSTED_BIT{uint64_t{1} << 63};
    static constexpr uint64_t ACTIVE_MASK{(STOPPED_BIT - 1) & ~(ACTIVE_UNIT - 1)};
    std::atomic<uint64_t> m_word{FENCED_BIT};
    std::array<std::atomic<uint64_t>, static_cast<size_t>(ChainMutationKind::COUNT)> m_calls{};

    static ChainPublicationCheck Classify(uint64_t word, uint64_t generation) noexcept
    {
        if (word & STOPPED_BIT) return ChainPublicationCheck::STOPPED;
        if (word & EXHAUSTED_BIT) return ChainPublicationCheck::EXHAUSTED;
        if ((word & MAX_GENERATION) != generation) return ChainPublicationCheck::SUPERSEDED;
        return word & (FENCED_BIT | ACTIVE_MASK) ? ChainPublicationCheck::FENCED : ChainPublicationCheck::CURRENT;
    }
    bool BeginMutation(ChainMutationKind kind) noexcept
    {
        if constexpr (SUPPORTED) {
            // Diagnostics only, never used to accept a ticket. Counter wrap
            // cannot resurrect a generation; the generation itself saturates.
            const auto index{static_cast<size_t>(kind)};
            if (index < m_calls.size()) m_calls[index].fetch_add(1, std::memory_order_relaxed);
            if (kind == ChainMutationKind::DESTROY) {
                m_word.fetch_or(STOPPED_BIT | FENCED_BIT, std::memory_order_acq_rel);
                return false; // terminal; cannot capture again after owner teardown
            }
            auto old{m_word.load(std::memory_order_relaxed)};
            while (!(old & (EXHAUSTED_BIT | STOPPED_BIT))) {
                const auto generation{old & MAX_GENERATION};
                const bool exhausted{generation == MAX_GENERATION || (old & ACTIVE_MASK) == ACTIVE_MASK};
                const uint64_t next{exhausted
                    ? old | FENCED_BIT | EXHAUSTED_BIT
                    : (generation + 1) | ((old & ACTIVE_MASK) + ACTIVE_UNIT) | FENCED_BIT};
                if (m_word.compare_exchange_strong(old, next, std::memory_order_acq_rel,
                                                  std::memory_order_relaxed)) return !exhausted;
            }
        }
        return false;
    }

    void EndMutation() noexcept
    {
        if constexpr (SUPPORTED) {
            auto old{m_word.load(std::memory_order_relaxed)};
            for (;;) {
                // Depth underflow is a programming error, never permission to
                // reopen. Generation and exhaustion bits are never decremented.
                const auto next{(old & ACTIVE_MASK) ? old - ACTIVE_UNIT
                    : old | FENCED_BIT | EXHAUSTED_BIT};
                if (m_word.compare_exchange_strong(old, next, std::memory_order_acq_rel,
                                                  std::memory_order_relaxed)) return;
            }
        }
    }
public:
    static constexpr uint64_t MAX_GENERATION{ACTIVE_UNIT - 1};
    static constexpr uint64_t MAX_ACTIVE_DEPTH{ACTIVE_MASK / ACTIVE_UNIT};
    static constexpr bool SUPPORTED{std::atomic<uint64_t>::is_always_lock_free};
    ChainMutationFence() = default;
    ChainMutationFence(const ChainMutationFence&) = delete;
    ChainMutationFence& operator=(const ChainMutationFence&) = delete;

    static std::optional<ChainPublicationTicket> Capture(
        const std::shared_ptr<const ChainMutationFence>& issuer) noexcept
    {
        if constexpr (SUPPORTED) {
            if (!issuer) return std::nullopt;
            const auto word{issuer->m_word.load(std::memory_order_acquire)};
            if (word & (EXHAUSTED_BIT | STOPPED_BIT | ACTIVE_MASK)) return std::nullopt;
            return ChainPublicationTicket{issuer, word & MAX_GENERATION};
        }
        return std::nullopt;
    }

    ChainPublicationCheck Check(const ChainPublicationTicket& ticket) const noexcept
    {
        if constexpr (SUPPORTED) {
            if (ticket.m_issuer.get() != this) return ChainPublicationCheck::SUPERSEDED;
            return Classify(m_word.load(std::memory_order_acquire), ticket.m_generation);
        }
        return ChainPublicationCheck::UNSUPPORTED;
    }

    ChainPublicationCheck CompletePublication(const ChainPublicationTicket& ticket) noexcept
    {
        if constexpr (SUPPORTED) {
            if (ticket.m_issuer.get() != this) return ChainPublicationCheck::SUPERSEDED;
            uint64_t expected{ticket.m_generation | FENCED_BIT};
            if (m_word.compare_exchange_strong(expected, ticket.m_generation,
                                              std::memory_order_acq_rel, std::memory_order_acquire)) {
                return ChainPublicationCheck::CURRENT;
            }
            return Classify(expected, ticket.m_generation);
        }
        return ChainPublicationCheck::UNSUPPORTED;
    }

    //! Per-site call counters only, not a coherent chain snapshot or authority.
    uint64_t MutationCalls(ChainMutationKind kind) const noexcept
    {
        if constexpr (SUPPORTED) {
            const auto index{static_cast<size_t>(kind)};
            return index < m_calls.size() ? m_calls[index].load(std::memory_order_relaxed) : 0;
        }
        return 0;
    }
};

/** Lexical native scope. Borrows the immutable options-owned observer: that
 * owner must outlive the call, exactly as a mutex must outlive its lock guard.
 * Noncopyable/nonmovable, no shared-owner decrement/destruction under cs_main.
 * Null observers do nothing. End leaves the generation fenced even on error
 * or exception; only a separately verified publication may complete it. */
class ChainMutationScope final {
    ChainMutationFence* const m_observer;
    const bool m_registered;
public:
    ChainMutationScope(const std::shared_ptr<ChainMutationFence>& observer, ChainMutationKind kind) noexcept
        : m_observer{observer.get()}, m_registered{m_observer && m_observer->BeginMutation(kind)} {}
    ChainMutationScope(std::shared_ptr<ChainMutationFence>&&, ChainMutationKind) = delete;
    ~ChainMutationScope() { if (m_registered) m_observer->EndMutation(); }
    ChainMutationScope(const ChainMutationScope&) = delete;
    ChainMutationScope& operator=(const ChainMutationScope&) = delete;
};

} // namespace kernel
#endif // BITCOIN_KERNEL_CHAIN_MUTATION_H
