// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/mailbox.h>

#include <stdexcept>
#include <type_traits>
#include <utility>

namespace flowmesh2 {

static_assert(std::is_nothrow_move_assignable_v<MailboxMessage>);
static_assert(std::is_nothrow_move_constructible_v<MailboxMessage>);

BoundedMailbox::BoundedMailbox(const MailboxLimits limits) : m_limits{limits}
{
    if (limits.max_messages == 0 || limits.max_payload_bytes == 0) {
        throw std::invalid_argument{"Mailbox count and payload-byte limits must be nonzero"};
    }
}

MailboxPushResult BoundedMailbox::TryPush(MailboxMessage&& message)
{
    // Allocate only an empty metadata node outside the queue lock. The
    // caller's large payload stays untouched until admission is atomic.
    std::list<MailboxMessage> staged;
    staged.emplace_back();
    {
        LOCK(m_mutex);
        if (m_closed) return MailboxPushResult::CLOSED;
        const size_t bytes{message.payload.size()};
        if (bytes > m_limits.max_payload_bytes) return MailboxPushResult::OVERSIZE;
        // Subtraction avoids overflow even for a SIZE_MAX byte limit.
        if (m_queue.size() >= m_limits.max_messages || bytes > m_limits.max_payload_bytes - m_payload_bytes) {
            return MailboxPushResult::FULL;
        }
        staged.front() = std::move(message);
        m_queue.splice(m_queue.end(), staged);
        m_payload_bytes += bytes;
    }
    // Any rejected empty staging node is destroyed after the lock unwinds.
    return MailboxPushResult::ACCEPTED;
}

std::optional<MailboxMessage> BoundedMailbox::TryPop()
{
    std::list<MailboxMessage> staged;
    {
        LOCK(m_mutex);
        if (m_queue.empty()) return std::nullopt;
        m_payload_bytes -= m_queue.front().payload.size();
        staged.splice(staged.end(), m_queue, m_queue.begin());
    }
    // Move/return and release the list node outside the queue lock.
    return std::optional<MailboxMessage>{std::move(staged.front())};
}

void BoundedMailbox::Close()
{
    LOCK(m_mutex);
    m_closed = true;
}

MailboxStats BoundedMailbox::Stats() const
{
    LOCK(m_mutex);
    return {m_queue.size(), m_payload_bytes, m_closed};
}

} // namespace flowmesh2
