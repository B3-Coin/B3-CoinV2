// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_AGREEMENT_OUTBOUND_H
#define B3_FLOWMESH2_AGREEMENT_OUTBOUND_H

#include <flowmesh2/storage/durable.h>
#include <flowmesh2/storage/tokens.h>
#include <uint256.h>

#include <cstdint>

/** A vote that may leave the process (slice R8; ARCHITECTURE.md 7.2, B-VOTE).
 *
 * OutboundVote::From(const DurableSignedVote&) is the only way to build one,
 * and a DurableSignedVote exists only after the SIGNED_VOTE record passed its
 * barrier. So a vote can be sent, or put into a certificate, only once it is
 * durable in the signer journal (section 7.3, B-VOTE). The vote carries the
 * VoteKey, the digest and the signature of that record; the transport, the
 * seat index and the wire codec are R10's agreement messages.
 *
 * Header-only and deterministic.
 */
namespace flowmesh2::agreement {

class OutboundVote
{
public:
    OutboundVote() = delete;

    //! The only constructor path: from a durable signed vote.
    static OutboundVote From(const DurableSignedVote& vote)
    {
        return OutboundVote{vote.Binding().key, vote.Binding().digest, vote.Binding().signature};
    }

    const storage::VoteKey& Key() const { return m_key; }
    const uint256& Digest() const { return m_digest; }
    const storage::VoteSignature& Signature() const { return m_signature; }

    friend bool operator==(const OutboundVote&, const OutboundVote&) = default;

private:
    OutboundVote(const storage::VoteKey& key, const uint256& digest, const storage::VoteSignature& signature)
        : m_key{key}, m_digest{digest}, m_signature{signature}
    {
    }

    storage::VoteKey m_key;
    uint256 m_digest;
    storage::VoteSignature m_signature;
};

} // namespace flowmesh2::agreement

#endif // B3_FLOWMESH2_AGREEMENT_OUTBOUND_H
