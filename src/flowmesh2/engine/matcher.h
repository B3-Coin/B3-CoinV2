// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_ENGINE_MATCHER_H
#define B3_FLOWMESH2_ENGINE_MATCHER_H

#include <flowmesh2/core/action.h>
#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/market.h>
#include <flowmesh2/core/outcome.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/engine/book.h>
#include <flowmesh2/ledger/ledger.h>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

/** The A2 sweep of one market's same-slot takers (slice R6; ARCHITECTURE.md
 * section 11; D4, D5, O-5, O-17).
 *
 * PROVISIONAL REGTEST (D4 matching A2_PRORATA, D5 seller fee, D6 caps).
 * Engine phase 5 (R7) reserves every taker in canonical order, then calls
 * SweepA2 once per market; phase 6 rests GTC remainders or releases them.
 *
 * Matching rule (A2), for the BUY takers against the asks and then for the
 * SELL takers against the bids:
 * 1. Take the best level of the other side. The eligible takers are those
 *    with lots left, fewer than max_fills_per_order fills, and a limit that
 *    reaches the level. If there are none (or no level), this side is done.
 * 2. Allocate the level's lots S over the eligible takers' remaining lots
 *    d_i (sum D): if D <= S every taker gets d_i; otherwise taker i gets
 *    floor(S * d_i / D), and the S - sum(floor) leftover lots go one each to
 *    the largest remainders (S * d_i mod D), ties to the lower canonical
 *    rank (AllocateProRata).
 * 3. Pairing (M3-ENG-14, PROVISIONAL): the eligible takers, in ascending
 *    canonical rank, each consume their allocation from the level's makers
 *    in FIFO order. Each (taker, maker) piece is one fill.
 * 4. Repeat; a taker that reaches its fill cap leaves its unconsumed
 *    allocation at the level, and the next round re-allocates it.
 * Every fill executes at the maker's price. Same-slot takers never match
 * each other here: opposite GTC remainders that still cross are the
 * residual cross (engine/residual.h).
 *
 * Settlement of one fill of `lots` at maker price p (D5, PROVISIONAL):
 *   base  = BaseAtoms(market, lots), quote = QuoteAtoms(market, lots, p);
 *   fee   = SellerFeeIncrement(seller.cum_quote, seller.cum_quote + quote)
 *           (core/fee_math.h: ceiling on the seller order's cumulative quote);
 *   ledger.PayFromReserved(seller -> buyer, base asset, base, 0);
 *   ledger.PayFromReserved(buyer -> seller, quote asset, quote, fee): the
 *   fee waits in fees_held until phase 8 (O-31).
 * Both orders add quote to cum_quote and one fill; the seller order adds the
 * fee to fee_charged; the taker's reserved_left drops by what it paid. A
 * maker that runs out of lots leaves the book and its owner's open-order
 * count drops by one. A self-trade (maker and taker of one account) executes
 * as any other fill (O-17, PROVISIONAL, M3-ENG-13): it conserves, and the
 * fee is charged once, to the selling order.
 *
 * Caps (D6, PROVISIONAL; M3-ENG-04 completes them in R7):
 * - per order: a taker makes at most profile.max_fills_per_order fills; the
 *   cap does not apply to the maker side;
 * - per slot: every fill takes one unit of the caller's SlotFillBudget, which
 *   R7 starts at profile.max_fills_per_slot and passes to the sweeps of
 *   every market of the slot (ascending MarketId; inside a market the BUY
 *   takers sweep before the SELL takers). At zero, matching stops.
 *
 * Every taker leaves the sweep with a TakerStop computed from the final
 * book: FILLED, else NO_CROSS if the other side's best no longer reaches its
 * limit, else ORDER_FILL_CAP if it made max_fills_per_order fills, else
 * SLOT_FILL_CAP. R7 maps them to outcomes (an IOC remainder is released; a
 * GTC remainder rests unless it would cross, which only a cap can leave).
 *
 * Errors. Preconditions are checked before anything changes, and a refusal
 * (INVALID_INPUT unless stated) leaves book, ledger, takers and budget
 * unchanged:
 * - Validate(profile) fails (its error), or the matching profile is not
 *   A2_PRORATA;
 * - a taker has an unknown side, a time in force other than GTC or IOC
 *   (POST_ONLY never sweeps), limit or lots of 0, fills, cum_quote or
 *   fee_charged other than 0, or a stop other than PENDING;
 * - two takers share an OrderId or a rank, or a taker's OrderId rests in
 *   the book;
 * - a taker's owner has no account, or is a protocol account;
 * - a taker's reserved_left is below RequiredReservation at its limit;
 * - an owner of a maker in this book has no account, or fewer open orders
 *   than its makers here;
 * - for some (owner, asset), the takers' reserved_left plus this book's
 *   makers' reserved_left exceed the ledger's reserved balance.
 * After the preconditions hold, an error (from the ledger or the book)
 * means a broken invariant elsewhere (for example reservations of other
 * markets that the ledger no longer covers, or more listed assets than
 * MAX_LEAF_ASSETS, O-34). Book, ledger, takers and budget are then
 * unspecified and the caller (R7) discards its candidate.
 *
 * Determinism. Pure integer arithmetic (128-bit where needed), ordered
 * containers, iteration by canonical rank; the order of `takers` in the span
 * does not matter. No clock, I/O, randomness or floating point.
 */
namespace flowmesh2::engine {

/** Why a taker stopped matching. APPEND-ONLY registry; zero is never a value. */
enum class TakerStop : uint8_t {
    PENDING = 1,        //!< not swept yet: the only value SweepA2 accepts
    FILLED = 2,         //!< no lots left
    NO_CROSS = 3,       //!< lots left; the other side is empty or its best does not reach the limit
    ORDER_FILL_CAP = 4, //!< lots left, the book still crosses, max_fills_per_order fills made
    SLOT_FILL_CAP = 5,  //!< lots left, the book still crosses, the slot's fill budget is exhausted
};
bool IsKnown(TakerStop stop);
//! Registry name; "UNKNOWN" for an unregistered value.
std::string_view ToString(TakerStop stop);

/** One same-slot taker: a PLACE with time in force GTC or IOC, already
 * reserved by the engine (phase 5). SweepA2 updates it in place.
 */
struct Taker {
    OrderId id;                     //!< the PLACE's ActionId
    AccountId owner;                //!< its signer
    Side side{Side::BUY};
    TimeInForce tif{TimeInForce::GTC};
    uint64_t limit_ticks{0};
    uint64_t remaining_lots{0};
    uint64_t rank{0};               //!< canonical rank in the slot (R7 CanonicalOrder, O-4); unique per sweep
    Atoms reserved_left{0};         //!< reserved for this order and not yet spent
    Atoms cum_quote{0};             //!< quote atoms matched so far
    Atoms fee_charged{0};           //!< seller fees charged so far (0 for a BUY)
    uint64_t fills{0};              //!< fills made so far
    TakerStop stop{TakerStop::PENDING};

    friend bool operator==(const Taker&, const Taker&) = default;
};

//! The slot's remaining fill allowance (D6: fills per slot).
struct SlotFillBudget {
    uint64_t remaining{0};

    //! A fresh budget of profile.max_fills_per_slot fills.
    static SlotFillBudget ForSlot(const ProfileParams& profile) { return SlotFillBudget{profile.max_fills_per_slot}; }
    friend bool operator==(const SlotFillBudget&, const SlotFillBudget&) = default;
};

//! One executed fill: the certified Fill plus what R7 needs for outcome deltas.
struct ExecutedFill {
    Fill fill;            //!< maker, taker, market, taker_side, price (maker's), lots, quote, seller fee and seller_quote_before
    AccountId maker_owner;
    AccountId taker_owner;
    Atoms base_atoms{0};  //!< BaseAtoms(market, lots)
    friend bool operator==(const ExecutedFill&, const ExecutedFill&) = default;
};

struct SweepResult {
    std::vector<ExecutedFill> fills;   //!< in execution order
    std::vector<OrderId> closed_makers; //!< makers that left the book, in the order they closed
    Atoms fees{0};                      //!< seller fees withheld into fees_held, in the market's quote asset
    friend bool operator==(const SweepResult&, const SweepResult&) = default;
};

/** Pro-rata allocation of `supply` lots over `demands` (step 2 above):
 * largest remainder, ties by the lower rank. The result is aligned with
 * `demands`; each entry is at most its demand, and they sum to
 * min(supply, sum(demands)). INVALID_INPUT for sizes that differ, a zero
 * demand or a repeated rank; ARITHMETIC_OVERFLOW if the demands sum above
 * 2^64-1.
 */
// TODO(rebirth:M3-ENG-14) ratify the pro-rata remainder rule and the taker-to-maker pairing.
Result<std::vector<uint64_t>> AllocateProRata(uint64_t supply, std::span<const uint64_t> demands,
                                              std::span<const uint64_t> ranks);

//! Sweep `takers` (one market, any order in the span) against `book`; see the file comment.
Result<SweepResult> SweepA2(OrderBook& book, ledger::LedgerState& ledger, std::span<Taker> takers,
                            const ProfileParams& profile, SlotFillBudget& budget);

/** The epoch reference price of a market for the spot price band (D6:
 * +/- price_band_percent). Its source is BLOCKED-ON-RULING (M3-ENG-03).
 */
struct ReferencePrice {
    MarketId market;
    uint64_t price_ticks{0};
    friend bool operator==(const ReferencePrice&, const ReferencePrice&) = default;
};

/** True iff `price_ticks` lies inside the band around `reference`; R7 answers
 * REJ_PRICE_BAND otherwise. The profile keeps the band disabled
 * (price_band_enabled = false, Validate refuses true), so the engine never
 * calls this in the outline.
 */
// TODO(rebirth:M3-ENG-03) the +/- price band against the epoch reference price.
Result<bool> PriceBandCheck(const MarketDescriptor& market, uint64_t price_ticks, const ReferencePrice& reference);

} // namespace flowmesh2::engine

#endif // B3_FLOWMESH2_ENGINE_MATCHER_H
