// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef B3_FLOWMESH2_AGREEMENT_CONTROL_OUTBOUND_H
#define B3_FLOWMESH2_AGREEMENT_CONTROL_OUTBOUND_H
#include <flowmesh2/storage/durable.h>
namespace flowmesh2::agreement {
class OutboundControl
{
public:
    OutboundControl() = delete;
    static OutboundControl From(const DurableSignedControl& record)
    {
        return OutboundControl{record.Binding().key, record.Digest(), record.Binding().signature};
    }
    const storage::ControlKey& Key() const { return m_key; }
    const uint256& Digest() const { return m_digest; }
    const storage::VoteSignature& Signature() const { return m_signature; }
private:
    OutboundControl(storage::ControlKey key, uint256 digest, storage::VoteSignature signature)
        : m_key{std::move(key)}, m_digest{std::move(digest)}, m_signature{std::move(signature)} {}
    storage::ControlKey m_key;
    uint256 m_digest;
    storage::VoteSignature m_signature;
};
}
#endif
