// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH2_READ_SLOT_H
#define B3COIN_FLOWMESH2_READ_SLOT_H

#include <sync.h>
#include <uint256.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace flowmesh2 {

/** In-memory scope, NOT a selected V2 wire/signing/certificate codec. */
struct ReadScope {
    uint256 domain, configuration, validator_set, anchor;
    uint64_t epoch{0}, authority_revision{0};
    friend bool operator==(const ReadScope&, const ReadScope&) = default;
};

/** A claimed identity, not proof that its value is valid or durable. */
struct CommitIdentity {
    uint256 id, parent, state_root;
    uint64_t sequence{0};
    friend bool operator==(const CommitIdentity&, const CommitIdentity&) = default;
};

/**
 * Deeply owned opaque bytes and their claimed scope/head. No verified,
 * ready, fresh, spendable or final label is supplied by this primitive.
 * The future durable/verifying adapter must establish those separately.
 */
struct ReadEnvelope {
    ReadScope scope;
    CommitIdentity commit;
    std::vector<unsigned char> payload;
};

struct PublishedEnvelope {
    uint64_t publication{0}; // Local publication counter, NOT consensus sequence.
    ReadEnvelope envelope;
};

class ReadSlot;

/**
 * A move-only reader permit. No shared_ptr escapes: at most max_readers
 * leases can retain historical views. References die with the lease.
 */
class ReadLease {
    friend class ReadSlot;
    struct Permits {
        explicit Permits(size_t limit) : maximum{limit} {}
        const size_t maximum;
        std::atomic<size_t> held{0};
    };
    std::shared_ptr<Permits> m_permits;
    std::shared_ptr<const PublishedEnvelope> m_view;
    ReadLease(std::shared_ptr<Permits> permits, std::shared_ptr<const PublishedEnvelope> view);
    void Release() noexcept;

public:
    ReadLease(const ReadLease&) = delete;
    ReadLease& operator=(const ReadLease&) = delete;
    ReadLease(ReadLease&& other) noexcept;
    ReadLease& operator=(ReadLease&& other) noexcept;
    ~ReadLease();
    const PublishedEnvelope& View() const;
};

enum class ReadPublishResult { PUBLISHED, OVERSIZE, EXHAUSTED };

/**
 * Memory publication only, NOT a durability barrier or authority source.
 * Readers never acquire the publishing mutex or a chain/market lock.
 * atomic<shared_ptr> is not promised to be formally lock-free.
 * One bounded current view plus <=max_readers leased old views; callers
 * must bound publisher concurrency and their own input allocations.
 * Quiesce operations before slot destruction; existing leases may outlive it.
 */
class ReadSlot {
    const size_t m_max_payload_bytes;
    std::shared_ptr<ReadLease::Permits> m_permits;
    std::atomic<std::shared_ptr<const PublishedEnvelope>> m_view;
    mutable Mutex m_publish_mutex;
    uint64_t m_publication GUARDED_BY(m_publish_mutex){0};

public:
    ReadSlot(size_t max_readers, size_t max_payload_bytes);
    ReadPublishResult Publish(const ReadEnvelope& envelope) LOCKS_EXCLUDED(m_publish_mutex);
    std::optional<ReadLease> Acquire() const;
    size_t Readers() const noexcept;
};

} // namespace flowmesh2

#endif // B3COIN_FLOWMESH2_READ_SLOT_H
