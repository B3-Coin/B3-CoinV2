// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_ENGINE_BOOK_H
#define B3_FLOWMESH2_ENGINE_BOOK_H

#include <flowmesh2/core/action.h>
#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/market.h>
#include <flowmesh2/core/result.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <vector>

/** The order book of one spot market, and the set of all books (slice R6;
 * ARCHITECTURE.md section 11, plan section 3.5).
 *
 * PROVISIONAL REGTEST skeleton. Each side is a std::map from price (ticks)
 * to a FIFO std::deque of resting orders, plus a std::map from OrderId to
 * the order's side and price, used only for lookup. Task M3-ENG-01 replaces
 * it with the plan's fixed-capacity book (2^17-tick band array, two-level
 * bitmap, Fenwick tree, FIFO pool, open-addressing OrderId table that is
 * never iterated, BOOK_FULL). The interface below is the contract that
 * replacement keeps; the skeleton has no capacity limit and no price band.
 * engine/band_book.h holds that replacement, tested against this book; the
 * engine keeps this one until the band, the capacity and the refusal codes
 * are ruled (BLOCKED-ON-RULING).
 *
 * Invariants. Every mutation keeps all of them or changes nothing:
 * 1. Every resting order has a known side, price_ticks >= 1,
 *    remaining_lots >= 1 and an OrderId unique in the book.
 * 2. Full reservation: reserved_left == RequiredReservation(market, side,
 *    price_ticks, remaining_lots).amount exactly. A BUY holds quote atoms at
 *    its own price, a SELL holds base atoms. Fills at the maker's price keep
 *    this exact (FillBestFront).
 * 3. The book is never crossed: best bid < best ask whenever both exist.
 *    Rest refuses an order that would execute against the other side.
 * 4. Price-time priority: inside a level, orders queue in arrival order and
 *    their arrival_rank is strictly increasing, so the book is a function of
 *    its set of orders (R7 assigns arrival_rank strictly increasing over the
 *    life of the market; the book refuses a rank that is not above the
 *    level's last).
 * 5. No empty level is stored, and the lookup index names exactly the
 *    resting orders.
 *
 * Ownership, locks, determinism. OrderBook and BookSet are plain value types
 * with no lock: the single writer (state/StateOwner, R9) copies them into a
 * candidate and publishes them immutably. Every container is ordered; there
 * is no clock, I/O, randomness or floating point. The book does not touch
 * the ledger: the matcher (engine/matcher.h) settles balances.
 */
namespace flowmesh2::engine {

//! The other side.
constexpr Side Opposite(Side side) { return side == Side::BUY ? Side::SELL : Side::BUY; }

/** True iff an order of `side` limited at `limit_ticks` executes against a
 * resting order of the OTHER side at `level_ticks`: a BUY reaches asks at or
 * below its limit, a SELL reaches bids at or above it.
 */
constexpr bool Reaches(Side side, uint64_t limit_ticks, uint64_t level_ticks)
{
    return side == Side::BUY ? level_ticks <= limit_ticks : level_ticks >= limit_ticks;
}

//! What an order holds reserved, in one asset.
struct Reservation {
    AssetId asset;
    Atoms amount{0};
    friend bool operator==(const Reservation&, const Reservation&) = default;
};

/** The exact reservation of `lots` at `price_ticks` on `side`:
 * BUY  -> {market.quote, QuoteAtoms(market, lots, price_ticks)};
 * SELL -> {market.base,  BaseAtoms(market, lots)}.
 * INVALID_INPUT for an invalid market or an unknown side;
 * ARITHMETIC_OVERFLOW above MAX_ATOMS.
 */
Result<Reservation> RequiredReservation(const MarketDescriptor& market, Side side, uint64_t price_ticks, uint64_t lots);

/** One resting (maker) order. The engine (R7) creates it from a PLACE whose
 * GTC remainder rests or from a POST_ONLY placement; the matcher updates it
 * when it fills.
 */
struct RestingOrder {
    OrderId id;               //!< the PLACE's ActionId
    AccountId owner;          //!< the signer of the PLACE
    Side side{Side::BUY};
    uint64_t price_ticks{0};
    uint64_t remaining_lots{0};
    uint64_t arrival_rank{0}; //!< FIFO key inside the level; strictly increasing per level
    Atoms reserved_left{0};   //!< exactly the reservation of remaining_lots at price_ticks
    Atoms cum_quote{0};       //!< quote atoms matched so far (the seller's fee base, D5)
    Atoms fee_charged{0};     //!< seller fees charged so far (0 for a BUY)
    uint64_t fills{0};        //!< fills this order took part in, as taker or maker

    friend bool operator==(const RestingOrder&, const RestingOrder&) = default;
};

//! Where a resting order is: its side and its price level.
struct Locator {
    Side side{Side::BUY};
    uint64_t price_ticks{0};
    friend bool operator==(const Locator&, const Locator&) = default;
};

//! The result of FillBestFront: the maker after the fill, and whether it left the book.
struct FrontFill {
    RestingOrder order; //!< state after the fill (remaining_lots == 0 when closed)
    bool closed{false};
};

// TODO(rebirth:M3-ENG-01) fixed-capacity band book with bitmap, Fenwick tree, FIFO pool and a never-iterated OrderId table.
class OrderBook
{
public:
    //! One price level: resting orders in arrival (FIFO) order.
    using Level = std::deque<RestingOrder>;
    //! Price (ticks) -> level, ascending price.
    using Ladder = std::map<uint64_t, Level>;

    //! An empty book of `market`; INVALID_INPUT if ValidateMarket refuses it.
    static Result<OrderBook> Create(const MarketDescriptor& market);

    const MarketDescriptor& Market() const { return m_market; }
    const MarketId& Id() const { return m_id; }

    // ---- Reads ---------------------------------------------------------------

    //! Highest bid price; nullopt if there is no bid.
    std::optional<uint64_t> BestBid() const;
    //! Lowest ask price; nullopt if there is no ask.
    std::optional<uint64_t> BestAsk() const;
    //! BestBid() for BUY, BestAsk() for SELL.
    std::optional<uint64_t> Best(Side side) const;
    //! True iff an order of `side` at `price_ticks` would execute against the other side's best.
    bool WouldCross(Side side, uint64_t price_ticks) const;

    //! The resting order `id`; nullptr if it is not in the book.
    const RestingOrder* Find(const OrderId& id) const;
    //! The oldest order of the best level of `side`; nullptr if that side is empty.
    const RestingOrder* BestFront(Side side) const;
    //! Sum of remaining lots at `price_ticks` on `side` (0 without a level); ARITHMETIC_OVERFLOW above 2^64-1.
    Result<uint64_t> LevelLots(Side side, uint64_t price_ticks) const;

    //! Bids (side BUY) or asks (side SELL), ascending price.
    const Ladder& LadderOf(Side side) const { return side == Side::BUY ? m_bids : m_asks; }
    //! Number of resting orders.
    size_t Size() const { return m_index.size(); }
    bool Empty() const { return m_index.empty(); }
    //! Every resting order: bids from the best (highest) price down, then asks
    //! from the best (lowest) price up, each level in FIFO order.
    std::vector<RestingOrder> Orders() const;

    // ---- Mutations (each one keeps every invariant or changes nothing) -----

    /** Rest `order` at the back of its level. INVALID_INPUT, with no change,
     * for: an unknown side; price_ticks or remaining_lots of 0; an OrderId
     * already resting; reserved_left that is not the exact reservation;
     * cum_quote or fee_charged outside [0, MAX_ATOMS], fee_charged >
     * cum_quote, or a BUY with a fee (only the seller pays, D5); an order
     * that would cross the other side (WouldCross); an
     * arrival_rank not above the level's last order. Errors of
     * RequiredReservation propagate.
     */
    Result<void> Rest(RestingOrder order);

    //! Take `id` out of the book (cancel); INVALID_INPUT and no change if it is not resting.
    Result<RestingOrder> Remove(const OrderId& id);

    /** Record one fill of `lots` against BestFront(side), at that order's own
     * price: remaining_lots -= lots, reserved_left -= the exact reservation
     * of `lots`, cum_quote += quote, fee_charged += fee, fills += 1. When
     * remaining_lots reaches 0 the order leaves the book (closed). Refusals,
     * all with no change: the side is empty, lots is 0 or above
     * remaining_lots, `quote` is not QuoteAtoms(market, lots, the order's
     * price) (execution is always at the maker price), `fee` is outside
     * [0, quote], or a BUY order is given a fee (the seller pays, D5)
     * (INVALID_INPUT); a sum outside its range (ARITHMETIC_OVERFLOW). The
     * fill count of a resting order has no cap: the per-order fill cap
     * applies to the taker side of a sweep (engine/matcher.h).
     */
    Result<FrontFill> FillBestFront(Side side, uint64_t lots, Atoms quote, Atoms fee);

    /** INVARIANT if any invariant in the file comment does not hold (the
     * mutations above maintain them; tests and the engine's phase 9 check).
     */
    Result<void> CheckInvariants() const;

    friend bool operator==(const OrderBook&, const OrderBook&) = default;

private:
    OrderBook(MarketDescriptor market, MarketId id) : m_market{market}, m_id{id} {}

    Ladder& MutableLadder(Side side) { return side == Side::BUY ? m_bids : m_asks; }

    MarketDescriptor m_market;
    MarketId m_id;
    Ladder m_bids;
    Ladder m_asks;
    std::map<OrderId, Locator> m_index; //!< lookup only (M3-ENG-01: never iterated)
};

/** One book per market. Markets are fixed when the set is created (D6: tick
 * and lot fixed at genesis; listings are M7-GOV-01).
 */
class BookSet
{
public:
    /** One empty book per market. INVALID_INPUT for an invalid market or two
     * descriptors with the same MarketId.
     */
    static Result<BookSet> Create(std::span<const MarketDescriptor> markets);

    //! The book of `market`; nullptr for an unknown market.
    const OrderBook* Find(const MarketId& market) const;
    OrderBook* FindMutable(const MarketId& market);

    //! The book in which `order` rests; nullptr if no book holds it.
    const OrderBook* FindOrder(const OrderId& order) const;
    OrderBook* FindOrderMutable(const OrderId& order);

    //! Every book, ascending MarketId.
    const std::map<MarketId, OrderBook>& Books() const { return m_books; }
    size_t MarketCount() const { return m_books.size(); }
    //! Resting orders over all books.
    size_t OrderCount() const;

    //! INVARIANT if a book breaks its invariants or one OrderId rests in two books.
    Result<void> CheckInvariants() const;

    friend bool operator==(const BookSet&, const BookSet&) = default;

private:
    BookSet() = default;

    std::map<MarketId, OrderBook> m_books;
};

} // namespace flowmesh2::engine

#endif // B3_FLOWMESH2_ENGINE_BOOK_H
