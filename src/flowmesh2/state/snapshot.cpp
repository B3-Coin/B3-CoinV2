// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/state/snapshot.h>

#include <flowmesh2/core/account_leaf.h>
#include <flowmesh2/core/codec.h>
#include <flowmesh2/core/market.h>
#include <flowmesh2/core/state_root.h>
#include <flowmesh2/engine/book.h>
#include <flowmesh2/engine/engine.h>
#include <flowmesh2/ledger/ledger.h>
#include <serialize.h>
#include <streams.h>

#include <algorithm>
#include <ios>
#include <utility>

namespace flowmesh2::state {

namespace {

//! The fewest bytes one encoded account leaf takes, with its length prefix:
//! prefix, account, two empty counts and open_orders.
constexpr size_t MIN_ACCOUNT_BYTES{1 + 32 + 1 + 1 + 4};
constexpr size_t MARKET_BYTES{32 + 32 + 8 + 8};
constexpr size_t OUTPOINT_BYTES{32 + 4};
constexpr size_t TOTALS_BYTES{32 + AssetTotals::ENCODED_BYTES};

//! A count of items of at least `item_bytes` each that the rest of the input can hold.
uint64_t ReadCount(SpanReader& reader, size_t item_bytes)
{
    const uint64_t count{ReadCompactSize(reader, /*range_check=*/false)};
    if (count > reader.size() / item_bytes) throw CapExceeded{"snapshot count exceeds the bytes that remain"};
    return count;
}

//! Exactly `size` bytes of `reader`.
std::vector<unsigned char> ReadFixed(SpanReader& reader, size_t size)
{
    std::vector<unsigned char> bytes(size);
    reader.read(MakeWritableByteSpan(bytes));
    return bytes;
}

template <class T>
T Unwrap(Result<T> result)
{
    if (!result.has_value()) throw std::ios_base::failure("snapshot leaf does not decode");
    return std::move(result).value();
}

//! The parts of a snapshot, decoded but not yet a state.
struct Parts {
    uint256 seat_set_root;
    std::vector<MarketDescriptor> markets;
    std::vector<AccountLeafV0> accounts;
    std::vector<Outpoint> nullifiers;
    std::vector<std::pair<AssetId, AssetTotals>> totals;
    std::vector<WithdrawalRecord> withdrawals;
    std::vector<RestingOrderLeafV0> orders;
};

} // namespace

Result<std::vector<unsigned char>> EncodeSnapshot(const CoreState& state)
{
    const ledger::LedgerState& ledger{state.engine.ledger};
    const ProfileParams& profile{state.Profile()};
    std::vector<unsigned char> out;
    VectorWriter writer{out, 0};
    writer.write(MakeByteSpan(SNAPSHOT_MAGIC));
    writer << SNAPSHOT_FORMAT_VERSION << ProfileHash(profile) << state.seat_set_root;
    const std::vector<MarketDescriptor> markets{state.engine.Markets()};
    WriteCompactSize(writer, markets.size());
    for (const MarketDescriptor& market : markets) writer << market;
    WriteCompactSize(writer, ledger.Accounts().size());
    for (const auto& [account, account_state] : ledger.Accounts()) {
        const Result<AccountLeafV0> leaf{ledger.Leaf(account)};
        if (!leaf.has_value()) return leaf.error();
        writer << leaf->Encode();
    }
    WriteCompactSize(writer, ledger.Nullifiers().size());
    for (const Outpoint& outpoint : ledger.Nullifiers()) writer << outpoint;
    WriteCompactSize(writer, ledger.Totals().size());
    for (const auto& [asset, totals] : ledger.Totals()) writer << asset << totals;
    WriteCompactSize(writer, ledger.Withdrawals().size());
    for (const auto& [id, record] : ledger.Withdrawals()) {
        if (!record.IsCanonical()) return Error{ErrorCode::INVARIANT, "noncanonical withdrawal leaf"};
        const std::vector<unsigned char> bytes{record.Encode()};
        writer.write(MakeByteSpan(bytes));
    }
    const std::vector<RestingOrderLeafV0> orders{state.engine.OrderLeaves()};
    WriteCompactSize(writer, orders.size());
    for (const RestingOrderLeafV0& order : orders) {
        if (!order.IsCanonical()) return Error{ErrorCode::INVARIANT, "noncanonical order leaf"};
        const std::vector<unsigned char> bytes{order.Encode()};
        writer.write(MakeByteSpan(bytes));
    }
    return Ok(std::move(out));
}

Result<std::shared_ptr<const CoreState>> DecodeSnapshot(std::span<const unsigned char> bytes, const ProfileParams& profile)
{
    const uint256 profile_hash{ProfileHash(profile)};
    bool other_profile{false};
    Result<Parts> decoded{DecodeExactWith(bytes, MAX_SNAPSHOT_BYTES, [&](SpanReader& reader) {
        Parts parts;
        std::array<unsigned char, SNAPSHOT_MAGIC.size()> magic{};
        reader.read(MakeWritableByteSpan(magic));
        uint32_t version{0};
        uint256 hash;
        reader >> version >> hash >> parts.seat_set_root;
        if (magic != SNAPSHOT_MAGIC || version != SNAPSHOT_FORMAT_VERSION) throw std::ios_base::failure("snapshot format");
        if (hash != profile_hash) {
            other_profile = true;
            throw std::ios_base::failure("snapshot profile");
        }
        for (uint64_t n{ReadCount(reader, MARKET_BYTES)}, i{0}; i < n; ++i) {
            MarketDescriptor market;
            reader >> market;
            parts.markets.push_back(market);
        }
        const size_t max_leaf{AccountLeafV0::MaxEncodedSize(profile)};
        for (uint64_t n{ReadCount(reader, MIN_ACCOUNT_BYTES)}, i{0}; i < n; ++i) {
            std::vector<unsigned char> leaf;
            ReadCappedVector(reader, leaf, max_leaf);
            parts.accounts.push_back(Unwrap(AccountLeafV0::Decode(leaf, profile)));
        }
        for (uint64_t n{ReadCount(reader, OUTPOINT_BYTES)}, i{0}; i < n; ++i) {
            Outpoint outpoint;
            reader >> outpoint;
            parts.nullifiers.push_back(outpoint);
        }
        for (uint64_t n{ReadCount(reader, TOTALS_BYTES)}, i{0}; i < n; ++i) {
            AssetId asset;
            AssetTotals totals;
            reader >> asset >> totals;
            parts.totals.emplace_back(asset, totals);
        }
        for (uint64_t n{ReadCount(reader, WithdrawalRecord::ENCODED_BYTES)}, i{0}; i < n; ++i) {
            parts.withdrawals.push_back(Unwrap(WithdrawalRecord::Decode(ReadFixed(reader, WithdrawalRecord::ENCODED_BYTES))));
        }
        for (uint64_t n{ReadCount(reader, RestingOrderLeafV0::ENCODED_BYTES)}, i{0}; i < n; ++i) {
            parts.orders.push_back(Unwrap(RestingOrderLeafV0::Decode(ReadFixed(reader, RestingOrderLeafV0::ENCODED_BYTES))));
        }
        return parts;
    })};
    if (!decoded.has_value()) {
        if (other_profile) return Error{ErrorCode::VERIFY_FAILED, SNAPSHOT_PROFILE};
        if (decoded.error().code == ErrorCode::DECODE) return Error{ErrorCode::DECODE, SNAPSHOT_FORMAT};
        return decoded.error();
    }
    Parts& parts{*decoded};
    for (size_t i{1}; i < parts.markets.size(); ++i) {
        if (!(parts.markets[i - 1].Id() < parts.markets[i].Id())) return Error{ErrorCode::INVALID_INPUT, SNAPSHOT_ORDER};
    }
    Result<ledger::LedgerState> ledger{
        ledger::LedgerState::FromLeaves(profile, parts.accounts, parts.nullifiers, parts.totals, parts.withdrawals)};
    if (!ledger.has_value()) return ledger.error();
    Result<engine::BookSet> books{engine::BookSet::Create(parts.markets)};
    if (!books.has_value()) return books.error();
    for (size_t i{0}; i < parts.orders.size(); ++i) {
        const RestingOrderLeafV0& leaf{parts.orders[i]};
        // Ascending MarketId, then each book's own order (Rest refuses a FIFO break).
        if (i > 0 && leaf.market < parts.orders[i - 1].market) return Error{ErrorCode::INVALID_INPUT, SNAPSHOT_ORDER};
        engine::OrderBook* book{books->FindMutable(leaf.market)};
        if (!book) return Error{ErrorCode::INVALID_INPUT, SNAPSHOT_ORDER};
        const Result<void> rested{book->Rest(engine::RestingOrder{leaf.order, leaf.owner, leaf.side, leaf.price_ticks, leaf.remaining_lots,
                                                                  leaf.arrival_rank, leaf.reserved_left, leaf.cum_quote, leaf.fee_charged,
                                                                  leaf.fills})};
        if (!rested.has_value()) return rested.error();
    }
    auto state{std::make_shared<CoreState>(CoreState{engine::EngineState{std::move(ledger).value(), std::move(books).value()}, parts.seat_set_root})};
    // A snapshot rests orders in book order; anything else is not the state that was written.
    if (state->engine.OrderLeaves() != parts.orders) return Error{ErrorCode::INVALID_INPUT, SNAPSHOT_ORDER};
    if (const Result<void> invariants{state->engine.CheckInvariants()}; !invariants.has_value()) return invariants.error();
    return Ok(std::shared_ptr<const CoreState>{std::move(state)});
}

Result<HeadView> SnapshotHead(std::span<const unsigned char> bytes, const ProfileParams& profile, const SlotHeaderV2& header,
                              const uint256& cert_hash)
{
    Result<std::shared_ptr<const CoreState>> state{DecodeSnapshot(bytes, profile)};
    if (!state.has_value()) return state.error();
    const uint8_t version{ExpectedCommitmentVersion(profile)};
    if (header.profile_hash != ProfileHash(profile) || header.seat_set_root != (*state)->seat_set_root ||
        header.commitment_version != version) {
        return Error{ErrorCode::VERIFY_FAILED, SNAPSHOT_HEADER};
    }
    const Result<StateRootsV0> roots{(*state)->engine.Commit()};
    if (!roots.has_value()) return roots.error();
    if (roots->version != version || roots->Root() != header.state_root) return Error{ErrorCode::VERIFY_FAILED, SNAPSHOT_HEADER};
    return HeadView::Make(header, cert_hash, std::move(state).value());
}

} // namespace flowmesh2::state
