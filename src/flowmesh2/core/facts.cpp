// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/core/facts.h>

#include <flowmesh2/core/body.h>
#include <flowmesh2/core/codec.h>
#include <flowmesh2/core/hashes.h>
#include <streams.h>

#include <type_traits>
#include <utility>

namespace flowmesh2 {

bool IsKnown(FactKind kind)
{
    switch (kind) {
    case FactKind::DEPOSIT:
    case FactKind::SEAT_CHANGE:
    case FactKind::LISTING:
    case FactKind::CHECKPOINT_ACCEPTED:
    case FactKind::PAYOUT_CONFIRMED:
    case FactKind::EXIT_NULLIFIER:
    case FactKind::SOURCE_RANGE:
        return true;
    }
    return false;
}

std::string_view ToString(FactKind kind)
{
    switch (kind) {
    case FactKind::DEPOSIT: return "DEPOSIT";
    case FactKind::SEAT_CHANGE: return "SEAT_CHANGE";
    case FactKind::LISTING: return "LISTING";
    case FactKind::CHECKPOINT_ACCEPTED: return "CHECKPOINT_ACCEPTED";
    case FactKind::PAYOUT_CONFIRMED: return "PAYOUT_CONFIRMED";
    case FactKind::EXIT_NULLIFIER: return "EXIT_NULLIFIER";
    case FactKind::SOURCE_RANGE: return "SOURCE_RANGE";
    }
    return "UNKNOWN";
}

FactKind KindOf(const Fact& fact)
{
    return std::visit(
        [](const auto& f) {
            using T = std::decay_t<decltype(f)>;
            if constexpr (std::is_same_v<T, DepositFact>) {
                return FactKind::DEPOSIT;
            } else if constexpr (std::is_same_v<T, SeatChangeFact>) {
                return FactKind::SEAT_CHANGE;
            } else if constexpr (std::is_same_v<T, ListingFact>) {
                return FactKind::LISTING;
            } else if constexpr (std::is_same_v<T, CheckpointAcceptedFact>) {
                return FactKind::CHECKPOINT_ACCEPTED;
            } else if constexpr (std::is_same_v<T, PayoutConfirmedFact>) {
                return FactKind::PAYOUT_CONFIRMED;
            } else if constexpr (std::is_same_v<T, ExitNullifierFact>) {
                return FactKind::EXIT_NULLIFIER;
            } else {
                static_assert(std::is_same_v<T, SourceRangeFact>, "every Fact alternative has a kind");
                return FactKind::SOURCE_RANGE;
            }
        },
        fact);
}

FactCursor CursorOf(const Fact& fact)
{
    return std::visit([](const auto& f) { return f.cursor; }, fact);
}

Result<std::vector<unsigned char>> EncodeFact(const Fact& fact)
{
    return std::visit([](const auto& f) { return EncodeFact(f); }, fact);
}

Result<std::vector<unsigned char>> EncodeFact(const DepositFact& fact)
{
    if (!InAtomRange(fact.amount)) return Error{ErrorCode::INVALID_INPUT, "deposit amount outside [0, MAX_ATOMS]"};
    std::vector<unsigned char> out;
    VectorWriter writer{out, 0};
    writer << static_cast<uint8_t>(FactKind::DEPOSIT) << fact;
    return Ok(std::move(out));
}

Result<std::vector<unsigned char>> EncodeFact(const SeatChangeFact&)
{
    return NotImplemented("M7-SEAT-01"); // TODO(rebirth:M7-SEAT-01) seat registration fact fields and codec
}

Result<std::vector<unsigned char>> EncodeFact(const ListingFact&)
{
    return NotImplemented("M7-GOV-01"); // TODO(rebirth:M7-GOV-01) listing fact fields and codec
}

Result<std::vector<unsigned char>> EncodeFact(const CheckpointAcceptedFact&)
{
    return NotImplemented("MB1-B3D-02"); // TODO(rebirth:MB1-B3D-02) checkpoint acceptance fact fields and codec
}

bool PayoutConfirmedFact::IsCanonical() const
{
    return !withdrawal.id.IsNull() && !account.id.IsNull() && !asset.IsNative() && amount > 0 && InAtomRange(amount) &&
           std::any_of(recipient.begin(), recipient.end(), [](unsigned char b) { return b != 0; }) &&
           !payout_event.txid.IsNull() && !domain.IsNull();
}

Result<std::vector<unsigned char>> EncodeFact(const PayoutConfirmedFact& fact)
{
    if (!fact.IsCanonical()) return Error{ErrorCode::INVALID_INPUT, "noncanonical payout confirmation"};
    std::vector<unsigned char> out;
    VectorWriter writer{out, 0};
    writer << static_cast<uint8_t>(FactKind::PAYOUT_CONFIRMED) << fact;
    return Ok(std::move(out));
}

Result<std::vector<unsigned char>> EncodeFact(const ExitNullifierFact&)
{
    return NotImplemented("M8-EXIT-01"); // TODO(rebirth:M8-EXIT-01) forced-exit nullifier fact fields and codec
}

bool SourceRangeFact::IsCanonical() const
{
    return !domain.IsNull() && chain_id != 0 && !previous_hash.IsNull() && !to_hash.IsNull() && confirmations != 0 &&
           to_height >= previous_height && (to_height != previous_height || to_hash == previous_hash);
}

Result<std::vector<unsigned char>> EncodeFact(const SourceRangeFact& fact)
{
    if (!fact.IsCanonical()) return Error{ErrorCode::INVALID_INPUT, "noncanonical source range commitment"};
    std::vector<unsigned char> out;
    VectorWriter writer{out, 0};
    writer << static_cast<uint8_t>(FactKind::SOURCE_RANGE) << fact;
    return Ok(std::move(out));
}

Result<uint256> FactId(const Fact& fact)
{
    const auto bytes{EncodeFact(fact)};
    if (!bytes.has_value()) return bytes.error();
    return Ok(Tagged(HashTag::FACT, *bytes));
}

Result<Fact> DecodeFact(std::span<const unsigned char> bytes)
{
    if (bytes.size() > MAX_FACT_BYTES) return Error{ErrorCode::OVERSIZE, "fact longer than MAX_FACT_BYTES"};
    if (bytes.empty()) return Error{ErrorCode::DECODE, "empty fact"};
    const auto body{bytes.subspan(1)};
    switch (static_cast<FactKind>(bytes[0])) {
    case FactKind::DEPOSIT: {
        auto deposit{DecodeExact<DepositFact>(body, DepositFact::BODY_BYTES)};
        if (!deposit.has_value()) return deposit.error();
        return Ok(Fact{std::move(*deposit)});
    }
    case FactKind::SEAT_CHANGE:
        return NotImplemented("M7-SEAT-01"); // TODO(rebirth:M7-SEAT-01)
    case FactKind::LISTING:
        return NotImplemented("M7-GOV-01"); // TODO(rebirth:M7-GOV-01)
    case FactKind::CHECKPOINT_ACCEPTED:
        return NotImplemented("MB1-B3D-02"); // TODO(rebirth:MB1-B3D-02)
    case FactKind::PAYOUT_CONFIRMED: {
        auto payout{DecodeExact<PayoutConfirmedFact>(body, PayoutConfirmedFact::BODY_BYTES)};
        if (!payout.has_value()) return payout.error();
        return Ok(Fact{std::move(*payout)});
    }
    case FactKind::EXIT_NULLIFIER:
        return NotImplemented("M8-EXIT-01"); // TODO(rebirth:M8-EXIT-01)
    case FactKind::SOURCE_RANGE: {
        auto source{DecodeExact<SourceRangeFact>(body, SourceRangeFact::BODY_BYTES)};
        if (!source.has_value()) return source.error();
        return Ok(Fact{std::move(*source)});
    }
    }
    return Error{ErrorCode::DECODE, "unknown fact kind"};
}

} // namespace flowmesh2
