// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_NET_SENTRY_H
#define B3_FLOWMESH2_NET_SENTRY_H

#include <flowmesh2/core/result.h>
#include <flowmesh2/net/seat_mesh.h>

#include <cstddef>
#include <span>

/** Sentry fan-out (slice R13: interface and stub; plan section 3.1;
 * research-network-api section 5; PLAN-10, SEC-5).
 *
 * Target (M4-NET-03): a sentry is the public tier in front of the seats.
 * It runs the cheap admission checks before crypto, verifies the BIP340
 * signature once, then Fanout(frame) relays the exact client bytes to
 * EVERY seat on the ACTION lane (dedup by ActionId at each seat), so no
 * single seat can censor by silence. It serves verified views, streams and
 * proofs but is never a source of truth (SEC-5); seats never serve the
 * public API (research N-R11). A BUSY lane to one seat does not stop the
 * others; the report says how many seats queued the frame.
 *
 * Stub: Fanout returns NotImplemented(M4-NET-03).
 */
namespace flowmesh2::net {

//! What one Fanout did (target shape).
struct FanoutReport {
    size_t queued{0}; //!< seats whose ACTION lane queued the frame
    size_t busy{0};   //!< seats whose ACTION lane was full (nothing queued there)
};

class SentryFanout
{
public:
    //! Fan out through `mesh`, which must outlive this object.
    explicit SentryFanout(SeatMesh& mesh) : m_mesh{mesh} {}

    //! Stub M4-NET-03.
    Result<FanoutReport> Fanout(std::span<const unsigned char> frame);

    SeatMesh& Mesh() const { return m_mesh; }

private:
    SeatMesh& m_mesh;
};

} // namespace flowmesh2::net

#endif // B3_FLOWMESH2_NET_SENTRY_H
