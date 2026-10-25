// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_ENGINE_BAND_BOOK_H
#define B3_FLOWMESH2_ENGINE_BAND_BOOK_H

#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/market.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/engine/book.h>
#include <util/int128.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

/** The fixed-capacity order book of one spot market (task M3-ENG-01, plan
 * section 3.5).
 *
 * Per market side:
 * - a level array over a band of BAND_TICKS = 2^17 consecutive prices;
 * - a two-level bitmap (2,048 level words under 32 summary words) that finds
 *   the best price in at most 33 word reads;
 * - a Fenwick tree of lots per level, for depth.
 * Per book:
 * - one pool of order nodes, each level an intrusive FIFO list through it;
 * - an OrderId table with open addressing (linear probing, backward-shift
 *   deletion, at most half full) that is never iterated.
 *
 * Same contract as OrderBook (engine/book.h): the same invariants, the same
 * reads, and the same mutations with the same verdicts. Rest adds two
 * refusals, checked after every refusal of OrderBook::Rest, so an order the
 * skeleton refuses gets the skeleton's error:
 * - a price outside [band_low_ticks, band_low_ticks + BAND_TICKS - 1]:
 *   INVALID_INPUT, detail BAND_REFUSED;
 * - a book that already holds order_capacity orders: OVERSIZE, detail
 *   BOOK_FULL.
 *
 * NOT WIRED. The engine still builds OrderBook. Where the band sits
 * (band_low_ticks), how many orders a book holds (order_capacity), and which
 * certified code each refusal maps to are D6 engine values that no document
 * pins, so the swap is BLOCKED-ON-RULING (TODO.md M3-ENG-01).
 *
 * Determinism. No float, clock, I/O, randomness, pointer order or unordered
 * container. Pool positions depend on the order of mutations, but no read
 * exposes them: every read, Orders() and operator== depend only on the set
 * of resting orders. The table hashes the first 8 bytes of the OrderId, so
 * its probe lengths are public; OrderIds can be ground to collide (an
 * ActionId does not cover the signature), which costs probe time, never a
 * different result.
 *
 * Copying. A book owns about 6 MiB of level arrays and Fenwick trees however
 * few orders rest (2 x 2^17 x (8 + 16) bytes), plus 4 bytes of table and one
 * node per order of capacity used. The single writer copies the engine state
 * into each candidate (M2-LED-10), so the copy cost is part of the ruling.
 */
namespace flowmesh2::engine {

//! Prices one side's level array covers (plan section 3.5: 131,072).
inline constexpr uint64_t BAND_TICKS{uint64_t{1} << 17};
//! Largest order_capacity: node indices are u32 and the table is twice the capacity.
inline constexpr uint32_t MAX_BOOK_CAPACITY{uint32_t{1} << 24};

inline constexpr std::string_view BAND_REFUSED{"price outside the book's band"};
inline constexpr std::string_view BOOK_FULL{"book full"};

/** The unpinned D6 values of one book (BLOCKED-ON-RULING, M3-ENG-01). */
struct BookLimits {
    uint64_t band_low_ticks{1}; //!< lowest price the book holds; the band ends BAND_TICKS - 1 above
    uint32_t order_capacity{0}; //!< resting orders over both sides, in [1, MAX_BOOK_CAPACITY]
    friend bool operator==(const BookLimits&, const BookLimits&) = default;
};

class BandBook
{
public:
    /** An empty book of `market`. INVALID_INPUT for a market ValidateMarket
     * refuses, band_low_ticks 0, a band that ends above 2^64-1, or an
     * order_capacity outside [1, MAX_BOOK_CAPACITY].
     */
    static Result<BandBook> Create(const MarketDescriptor& market, const BookLimits& limits);

    const MarketDescriptor& Market() const { return m_market; }
    const MarketId& Id() const { return m_id; }
    const BookLimits& Limits() const { return m_limits; }
    //! True iff `price_ticks` lies inside the band.
    bool InBand(uint64_t price_ticks) const;

    // ---- Reads (as OrderBook) ------------------------------------------------

    std::optional<uint64_t> BestBid() const;
    std::optional<uint64_t> BestAsk() const;
    std::optional<uint64_t> Best(Side side) const;
    bool WouldCross(Side side, uint64_t price_ticks) const;
    //! Valid until the next mutation.
    const RestingOrder* Find(const OrderId& id) const;
    //! Valid until the next mutation.
    const RestingOrder* BestFront(Side side) const;
    //! 0 outside the band or without a level; ARITHMETIC_OVERFLOW above 2^64-1.
    Result<uint64_t> LevelLots(Side side, uint64_t price_ticks) const;

    /** Lots resting on `side` at prices at least as good as `price_ticks`
     * for that side's owner: asks at or below it, bids at or above it (the
     * Fenwick tree; O(log BAND_TICKS)). ARITHMETIC_OVERFLOW above 2^64-1.
     */
    Result<uint64_t> DepthThrough(Side side, uint64_t price_ticks) const;

    //! Non-empty levels of `side`.
    size_t LevelCount(Side side) const;
    //! The orders at `price_ticks` on `side`, FIFO order; empty without a level.
    std::vector<RestingOrder> LevelOrders(Side side, uint64_t price_ticks) const;
    size_t Size() const { return m_size; }
    bool Empty() const { return m_size == 0; }
    //! Bids from the best price down, then asks from the best price up, each level FIFO.
    std::vector<RestingOrder> Orders() const;

    // ---- Mutations (as OrderBook, plus the two refusals above) ---------------

    Result<void> Rest(RestingOrder order);
    Result<RestingOrder> Remove(const OrderId& id);
    Result<FrontFill> FillBestFront(Side side, uint64_t lots, Atoms quote, Atoms fee);

    /** INVARIANT if an OrderBook invariant fails or the structures disagree:
     * a level bit without a list or the reverse, a summary bit, a list link,
     * a level's lots against the Fenwick tree, the level count, the table
     * against the orders, or the pool's free list. O(BAND_TICKS + capacity).
     */
    Result<void> CheckInvariants() const;

    //! Same market, limits and Orders(); pool layout is not compared.
    friend bool operator==(const BandBook& a, const BandBook& b);

private:
    static constexpr uint32_t NONE{UINT32_MAX};
    static constexpr size_t WORDS{BAND_TICKS / 64};
    static constexpr size_t SUMMARY_WORDS{WORDS / 64};

    struct Node {
        RestingOrder order;
        uint32_t prev{NONE};
        uint32_t next{NONE}; //!< also the free list
    };
    struct Level {
        uint32_t head{NONE};
        uint32_t tail{NONE};
    };
    struct SideBook {
        std::vector<Level> levels;              //!< BAND_TICKS
        std::vector<uint64_t> words;            //!< WORDS: bit i of word w = level 64w + i non-empty
        std::vector<uint64_t> summary;          //!< SUMMARY_WORDS: bit i of word s = words[64s + i] != 0
        std::vector<util::Unsigned128> fenwick; //!< BAND_TICKS + 1, 1-based, lots per level
        size_t level_count{0};
    };

    BandBook(MarketDescriptor market, MarketId id, BookLimits limits);

    const SideBook& SideOf(Side side) const { return side == Side::BUY ? m_bids : m_asks; }
    SideBook& MutableSide(Side side) { return side == Side::BUY ? m_bids : m_asks; }
    size_t IndexOf(uint64_t price_ticks) const { return static_cast<size_t>(price_ticks - m_limits.band_low_ticks); }
    uint64_t PriceOf(size_t index) const { return m_limits.band_low_ticks + index; }

    //! Lowest or highest non-empty level index of `side`; nullopt if empty.
    std::optional<size_t> BestIndex(Side side) const;
    static void SetBit(SideBook& s, size_t index);
    static void ClearBit(SideBook& s, size_t index);
    static void FenwickAdd(SideBook& s, size_t index, util::Unsigned128 lots);
    static void FenwickSub(SideBook& s, size_t index, util::Unsigned128 lots);
    //! Sum of lots over levels [0, index] (index + 1 levels).
    static util::Unsigned128 Prefix(const SideBook& s, size_t index);
    static util::Unsigned128 PointLots(const SideBook& s, size_t index);

    // The OrderId table (never iterated).
    size_t Home(const OrderId& id) const;
    //! Table slot holding `id`, or nullopt.
    std::optional<size_t> Slot(const OrderId& id) const;
    void TableInsert(uint32_t node);
    void TableErase(size_t slot);

    //! Unlink `node` from its level, clear its bit and lots, drop it from the table, free it.
    void Unlink(uint32_t node, util::Unsigned128 lots_to_remove);

    MarketDescriptor m_market;
    MarketId m_id;
    BookLimits m_limits;
    SideBook m_bids;
    SideBook m_asks;
    std::vector<Node> m_pool; //!< grows to at most order_capacity nodes
    uint32_t m_free{NONE};
    size_t m_size{0};
    std::vector<uint32_t> m_table; //!< node index or NONE; size a power of two >= 2 x capacity
};

} // namespace flowmesh2::engine

#endif // B3_FLOWMESH2_ENGINE_BAND_BOOK_H
