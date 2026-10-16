// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <flowmesh2/read_slot.h>

#include <limits>
#include <stdexcept>
#include <utility>

namespace flowmesh2 {

ReadLease::ReadLease(std::shared_ptr<Permits> permits, std::shared_ptr<const PublishedEnvelope> view)
    : m_permits{std::move(permits)}, m_view{std::move(view)} {}

void ReadLease::Release() noexcept
{
    // Destroy the bounded view before making its permit reusable.
    m_view.reset();
    if (m_permits) {
        m_permits->held.fetch_sub(1, std::memory_order_release);
        m_permits.reset();
    }
}

ReadLease::ReadLease(ReadLease&& other) noexcept
    : m_permits{std::move(other.m_permits)}, m_view{std::move(other.m_view)} {}

ReadLease& ReadLease::operator=(ReadLease&& other) noexcept
{
    if (this != &other) {
        Release();
        m_permits = std::move(other.m_permits);
        m_view = std::move(other.m_view);
    }
    return *this;
}

ReadLease::~ReadLease() { Release(); }

const PublishedEnvelope& ReadLease::View() const
{
    if (!m_view) throw std::logic_error{"Moved-from FlowMesh read lease"};
    return *m_view;
}

ReadSlot::ReadSlot(size_t max_readers, size_t max_payload_bytes)
    : m_max_payload_bytes{max_payload_bytes}, m_permits{std::make_shared<ReadLease::Permits>(max_readers)}
{
    if (max_readers == 0 || max_payload_bytes == 0) throw std::invalid_argument{"Zero FlowMesh read-slot limit"};
}

ReadPublishResult ReadSlot::Publish(const ReadEnvelope& envelope)
{
    if (envelope.payload.size() > m_max_payload_bytes) return ReadPublishResult::OVERSIZE;
    // Allocate before the publishing mutex. This API is fed by one future
    // state owner; it does not spawn work or bound arbitrary caller threads.
    // Copy actual bytes, not the caller's possibly oversized spare capacity.
    // Every field owns its values; subsequent caller mutation cannot change it.
    auto candidate{std::make_shared<PublishedEnvelope>(PublishedEnvelope{0, envelope})};
    std::shared_ptr<const PublishedEnvelope> retired;
    {
        LOCK(m_publish_mutex);
        if (m_publication == std::numeric_limits<uint64_t>::max()) return ReadPublishResult::EXHAUSTED;
        candidate->publication = ++m_publication;
        retired = std::atomic_exchange_explicit(&m_view,
            std::shared_ptr<const PublishedEnvelope>{std::move(candidate)}, std::memory_order_acq_rel);
    }
    retired.reset(); // Retire the old bounded payload outside the publishing lock.
    return ReadPublishResult::PUBLISHED;
}

std::optional<ReadLease> ReadSlot::Acquire() const
{
    auto held{m_permits->held.load(std::memory_order_relaxed)};
    do {
        if (held >= m_permits->maximum) return std::nullopt;
    } while (!m_permits->held.compare_exchange_weak(held, held + 1, std::memory_order_acquire, std::memory_order_relaxed));
    auto view{std::atomic_load_explicit(&m_view, std::memory_order_acquire)};
    if (!view) {
        m_permits->held.fetch_sub(1, std::memory_order_release);
        return std::nullopt;
    }
    return ReadLease{m_permits, std::move(view)};
}

size_t ReadSlot::Readers() const noexcept { return m_permits->held.load(std::memory_order_relaxed); }

} // namespace flowmesh2
