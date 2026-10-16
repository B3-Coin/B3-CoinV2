// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_MAILBOX_H
#define B3_FLOWMESH2_MAILBOX_H

#include <sync.h>
#include <uint256.h>

#include <cstddef>
#include <list>
#include <optional>
#include <vector>

namespace flowmesh2 {

//! Opaque input only: admission does not authenticate or certify this object.
struct MailboxMessage {
    uint256 request_id;
    std::vector<unsigned char> payload;
};

struct MailboxLimits {
    size_t max_messages;
    size_t max_payload_bytes;
};

struct MailboxStats {
    size_t messages;
    size_t payload_bytes;
    bool closed;
};

enum class MailboxPushResult { ACCEPTED, CLOSED, OVERSIZE, FULL };

/** One bounded FIFO. Limits count opaque payload.size() bytes, not total heap
 * use, RequestId bytes or surrounding serialization. There is no duplicate,
 * signature, consensus, execution or durability guarantee.
 *
 * No operation waits for messages/capacity or invokes outside code under the
 * mutex. Mutex acquisition can contend. Callers must own a pushed message
 * exclusively during TryPush and quiesce all callers before destruction.
 */
class BoundedMailbox
{
private:
    const MailboxLimits m_limits;
    mutable Mutex m_mutex;
    std::list<MailboxMessage> m_queue GUARDED_BY(m_mutex);
    size_t m_payload_bytes GUARDED_BY(m_mutex){0};
    bool m_closed GUARDED_BY(m_mutex){false};

public:
    //! Either zero limit is invalid. Construction does not reserve capacity.
    explicit BoundedMailbox(MailboxLimits limits);
    BoundedMailbox(const BoundedMailbox&) = delete;
    BoundedMailbox& operator=(const BoundedMailbox&) = delete;

    /** CLOSED takes precedence over OVERSIZE, then FULL. A rejection leaves
     * message untouched; only ACCEPTED moves it. A metadata-node allocation
     * before locking may throw without accepting or altering the input.
     */
    MailboxPushResult TryPush(MailboxMessage&& message) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Empty returns nullopt, including after Close. Queued data remains FIFO.
    std::optional<MailboxMessage> TryPop() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Idempotently refuse new inputs; do not discard queued messages.
    void Close() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! A coherent observation of one queue, not an admission reservation.
    MailboxStats Stats() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
};

} // namespace flowmesh2

#endif // B3_FLOWMESH2_MAILBOX_H
