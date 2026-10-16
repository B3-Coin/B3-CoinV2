// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_CORE_FACTS_H
#define B3_FLOWMESH2_CORE_FACTS_H

#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/authority.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/result.h>
#include <serialize.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <ios>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

/** Chain facts consumed by FlowMesh slots (slice R4; plan sections 3.3, 3.5).
 *
 * A fact is something the B3 chain finalized that FlowMesh must apply exactly
 * once, at its position `cursor` in the fact stream (core/authority.h). The
 * chain adapter (R10) produces them; a slot consumes the half-open cursor
 * range [facts_from, facts_to) in phase 1 of the engine (R7).
 *
 * Canonical encoding: u8 FactKind, then the kind's fields. FactId(fact) =
 * Tagged(FACT, EncodeFact(fact)), which equals core/body.h FactLeaf of the
 * bytes a slot body carries, so facts_root = MerkleRoot of the FactIds.
 *
 * Real in the outline: DepositFact, 117 bytes:
 *   u8 kind (1) | cursor (u32 height, u32 index) | outpoint (txid, u32 vout)
 *   | account | asset | i64 amount
 * The amount must be in [0, MAX_ATOMS] on the wire; whether zero is credited
 * is the ledger's rule (R5). The ledger credits each outpoint once.
 *
 * Stubs, whose fields and codecs belong to later tasks (every EncodeFact,
 * FactId and DecodeFact of these kinds returns NotImplemented with the ID):
 * - SeatChangeFact: seat registration, PoP, maturity, endpoints (M7-SEAT-01);
 * - ListingFact: asset and market listing (M7-GOV-01);
 * - CheckpointAcceptedFact: B3 accepted a CheckpointV2 (MB1-B3D-02);
 * - PayoutConfirmedFact: a bridge withdrawal paid out (M8-BRG-02);
 * - ExitNullifierFact: a forced-exit nullifier (M8-EXIT-01).
 * Each stub struct carries only its cursor until its task defines it.
 *
 * DecodeFact refuses: input above MAX_FACT_BYTES (OVERSIZE, core/body.h),
 * empty input or an unknown kind (DECODE), a malformed deposit (DECODE, or
 * OVERSIZE when longer than a deposit).
 */
namespace flowmesh2 {

//! Fact kinds. APPEND-ONLY registry; zero is never a value.
enum class FactKind : uint8_t {
    DEPOSIT = 1,
    SEAT_CHANGE = 2,
    LISTING = 3,
    CHECKPOINT_ACCEPTED = 4,
    PAYOUT_CONFIRMED = 5,
    EXIT_NULLIFIER = 6,
};

bool IsKnown(FactKind kind);
std::string_view ToString(FactKind kind);

struct DepositFact {
    FactCursor cursor;
    Outpoint outpoint;
    AccountId account;
    AssetId asset;
    Atoms amount{0};

    //! Without the kind byte.
    static constexpr size_t BODY_BYTES{8 + 36 + 32 + 32 + 8};
    friend bool operator==(const DepositFact&, const DepositFact&) = default;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << cursor << outpoint << account << asset << amount;
    }
    template <typename Stream>
    void Unserialize(Stream& s);
};

struct SeatChangeFact {
    FactCursor cursor;
    friend bool operator==(const SeatChangeFact&, const SeatChangeFact&) = default;
};
struct ListingFact {
    FactCursor cursor;
    friend bool operator==(const ListingFact&, const ListingFact&) = default;
};
struct CheckpointAcceptedFact {
    FactCursor cursor;
    friend bool operator==(const CheckpointAcceptedFact&, const CheckpointAcceptedFact&) = default;
};
struct PayoutConfirmedFact {
    FactCursor cursor;
    friend bool operator==(const PayoutConfirmedFact&, const PayoutConfirmedFact&) = default;
};
struct ExitNullifierFact {
    FactCursor cursor;
    friend bool operator==(const ExitNullifierFact&, const ExitNullifierFact&) = default;
};

using Fact = std::variant<DepositFact, SeatChangeFact, ListingFact, CheckpointAcceptedFact, PayoutConfirmedFact,
                          ExitNullifierFact>;

FactKind KindOf(const Fact& fact);
FactCursor CursorOf(const Fact& fact);

//! Canonical bytes: u8 kind || fields. Stub kinds return NotImplemented(ID).
Result<std::vector<unsigned char>> EncodeFact(const Fact& fact);
Result<std::vector<unsigned char>> EncodeFact(const DepositFact& fact);
Result<std::vector<unsigned char>> EncodeFact(const SeatChangeFact& fact);
Result<std::vector<unsigned char>> EncodeFact(const ListingFact& fact);
Result<std::vector<unsigned char>> EncodeFact(const CheckpointAcceptedFact& fact);
Result<std::vector<unsigned char>> EncodeFact(const PayoutConfirmedFact& fact);
Result<std::vector<unsigned char>> EncodeFact(const ExitNullifierFact& fact);

//! Tagged(FACT, EncodeFact(fact)); EncodeFact's error otherwise.
Result<uint256> FactId(const Fact& fact);

//! Exact decode of canonical fact bytes (see file comment).
Result<Fact> DecodeFact(std::span<const unsigned char> bytes);

template <typename Stream>
void DepositFact::Unserialize(Stream& s)
{
    s >> cursor >> outpoint >> account >> asset >> amount;
    if (!InAtomRange(amount)) throw std::ios_base::failure("deposit amount outside [0, MAX_ATOMS]");
}

} // namespace flowmesh2

#endif // B3_FLOWMESH2_CORE_FACTS_H
