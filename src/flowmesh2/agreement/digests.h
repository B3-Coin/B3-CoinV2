// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_AGREEMENT_DIGESTS_H
#define B3_FLOWMESH2_AGREEMENT_DIGESTS_H

#include <flowmesh2/agreement/messages.h>
#include <uint256.h>

#include <cstdint>
#include <optional>
#include <span>

/** The digests the agreement signs and seeds from (M4-AGR-02, M4-AGR-03,
 * M4-AGR-06; O-96).
 *
 * A seat's BLS key signs exactly these 32-byte digests, each under its own
 * domain separation, so no statement can be mistaken for another:
 * - COMMIT vote: SlotHeaderV2::CommitDigest() (core/slot.h; tag COMMIT).
 * - PREPARE vote: PrepareDigest(domain, epoch, slot, view, header_hash) =
 *   Tagged(PREPARE, domain ‖ u64 epoch ‖ u64 slot ‖ u64 view ‖ header_hash).
 *   The view makes a preparation a fact of its view (PreparedCert); domain,
 *   epoch and slot are bound explicitly, not only through the header hash,
 *   because a verifier of a carried certificate (a VIEW_CHANGE report, a
 *   NEW_VIEW, evidence) holds the hash, not the header: a genuine
 *   certificate of another slot must not verify as this slot's preparation
 *   (review of 2026-11-16, defect 1).
 * - Statements under the tag AGREEMENT, as Tagged(AGREEMENT, u8 kind ‖ ...):
 *   VIEW_CHANGE (1): domain ‖ u64 epoch ‖ u64 slot ‖ u64 new_view ‖ u32 seat
 *     ‖ u8 has_prepared ‖ [u64 prepared view ‖ prepared certificate hash];
 *   NEW_VIEW (2): domain ‖ u64 epoch ‖ u64 slot ‖ u64 view ‖ u32 seat
 *     ‖ selected ‖ CompactSize(n) ‖ n report digests (ViewChangeDigest of
 *     each report, in message order);
 *   HIGH_WATER (3): domain ‖ u32 subject ‖ u64 epoch ‖ u64 slot ‖ u64 view;
 *   LEADER_SEED (4): seat_set_root ‖ u64 epoch ‖ u64 term, the seed of the
 *     leader schedule (not signed).
 * The messages carry no domain field; both sides compute the digest with
 * the instance's domain, so a statement signed for another instance never
 * verifies. Deterministic: no clock, I/O or randomness.
 */
namespace flowmesh2::agreement {

//! Kinds under the AGREEMENT tag. APPEND-ONLY; zero is never a value.
enum class StatementKind : uint8_t {
    VIEW_CHANGE = 1,
    NEW_VIEW = 2,
    HIGH_WATER = 3,
    LEADER_SEED = 4,
};

//! Tagged(PREPARE, domain ‖ u64 epoch ‖ u64 slot ‖ u64 view ‖ header_hash): what a PREPARE vote signs.
uint256 PrepareDigest(const uint256& domain, uint64_t epoch, uint64_t slot, uint64_t view, const uint256& header_hash);
//! What a VIEW_CHANGE report signs (see the file comment).
uint256 ViewChangeDigest(const uint256& domain, uint64_t epoch, uint64_t slot, uint64_t new_view, uint32_t seat,
                         const std::optional<PreparedCert>& prepared);
//! ViewChangeDigest of a decoded report under `domain`.
uint256 ViewChangeDigest(const uint256& domain, const ViewChange& report);
//! What a NEW_VIEW signs: its fields and the digests of its reports.
uint256 NewViewDigest(const uint256& domain, uint64_t epoch, uint64_t slot, uint64_t view, uint32_t seat, const uint256& selected,
                      std::span<const uint256> report_digests);
//! NewViewDigest of a decoded message under `domain`.
uint256 NewViewDigest(const uint256& domain, const NewView& message);
//! What a high-water statement about `subject` signs (M4-AGR-06).
uint256 HighWaterDigest(const uint256& domain, uint32_t subject, uint64_t epoch, uint64_t slot, uint64_t view);
//! The leader schedule's seed for a term (M4-AGR-03).
uint256 LeaderSeed(const uint256& seat_set_root, uint64_t epoch, uint64_t term);

} // namespace flowmesh2::agreement

#endif // B3_FLOWMESH2_AGREEMENT_DIGESTS_H
