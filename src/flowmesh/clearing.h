// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_CLEARING_H
#define B3COIN_FLOWMESH_CLEARING_H

#include <consensus/amount.h>
#include <flowmesh/ledger.h>
#include <hash.h>
#include <modern/policy.h>
#include <uint256.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace flowmesh {

/**
 * FlowMesh persistent demand curves and deterministic batch clearing for
 * one market (base asset priced in quote asset). Everything is exact
 * integer arithmetic on price ticks and quantity lots — no floating point
 * anywhere — and fills settle internally against the ledger with no
 * per-fill UTXO spend.
 *
 * A demand curve is bounded piecewise-linear and monotone: at most K
 * breakpoints (price, quantity), strictly ascending in price, with
 * quantity non-increasing for a BID (buy less as price rises) and
 * non-decreasing for an ASK. Outside its range a curve clamps flat.
 * Curves are PERSISTENT: they stand across slots until cancelled or fully
 * filled, and each is backed by a ledger reservation made at submission.
 *
 * A slot clears once: the uniform clearing price is the candidate price
 * level maximizing traded volume min(demand, supply); ties break to
 * minimum imbalance, then lowest price. The long side is rationed by
 * largest-remainder allocation in account order, so allocation and
 * remainder are fully deterministic.
 */
class ClearingEngine
{
public:
    enum class Side { BID, ASK };

    struct Breakpoint {
        CAmount price{0}; // ticks
        CAmount qty{0};   // lots
    };

    struct ClearingResult {
        bool cleared{false};
        CAmount price{0};
        CAmount volume{0};
        CAmount imbalance{0}; // |demand - supply| at the clearing price (tie-break)
        std::map<AccountId, CAmount> bid_fill; // base lots bought
        std::map<AccountId, CAmount> ask_fill; // base lots sold
    };

    ClearingEngine(const AssetId& base, const AssetId& quote, Ledger& ledger, size_t max_k = 8)
        : m_base{base}, m_quote{quote}, m_ledger{ledger}, m_max_k{max_k} {}

    //! Validate a curve independently of the ledger (bounds + monotonicity).
    //! A BID must terminate at zero quantity so its worst-case spend — and
    //! therefore its reservation — is finite regardless of how high the
    //! uniform price clears.
    bool CurveIsValid(Side side, const std::vector<Breakpoint>& points) const
    {
        if (points.empty() || points.size() > m_max_k) return false;
        for (size_t i{0}; i < points.size(); ++i) {
            if (points[i].qty < 0 || points[i].qty > MAX_MONEY) return false;
            if (points[i].price < 0 || points[i].price > MAX_MONEY) return false;
            if (i > 0) {
                if (!(points[i - 1].price < points[i].price)) return false; // strictly ascending
                const bool ok{side == Side::BID ? points[i].qty <= points[i - 1].qty
                                                : points[i].qty >= points[i - 1].qty};
                if (!ok) return false;
            }
        }
        if (side == Side::BID && points.back().qty != 0) return false;
        // Zero demand means NO curve, never a free persistent book entry:
        // an all-zero curve would add candidate prices, state-root bytes
        // and per-slot scan work at zero reservation cost. A bid must
        // open with positive quantity (and still terminate at zero); an
        // ask must reach positive quantity.
        if (side == Side::BID && points.front().qty <= 0) return false;
        if (side == Side::ASK && points.back().qty <= 0) return false;
        return true;
    }

    //! Evaluate a monotone piecewise-linear curve at a price (public for
    //! tooling and differential tests). Clamps flat outside its range.
    static CAmount EvaluateCurve(const std::vector<Breakpoint>& points, const CAmount price)
    {
        return EvalCurve(points, price);
    }

    /**
     * Submit or replace an account's curve on a side. The worst-case
     * reservation it requires (quote for a bid, base for an ask) must be
     * backed by the account's available ledger balance, or the submission
     * is rejected and nothing changes. A replacement never releases the
     * old reservation wholesale: it adjusts the ledger by exactly the
     * DELTA between the old remaining reservation and the new requirement
     * (reserving more, or releasing the difference).
     */
    bool SubmitCurve(const AccountId& account, Side side, const std::vector<Breakpoint>& points)
    {
        if (!CurveIsValid(side, points)) return false;
        const AssetId& reserve_asset{side == Side::BID ? m_quote : m_base};
        const std::optional<CAmount> need{WorstCaseReservation(side, points)};
        if (!need) return false;

        // ATOMIC by reservation-delta accounting: look up without
        // inserting, adjust the ledger by exactly the difference, and
        // only touch the book after the ledger operation succeeded. A
        // failed first submission or failed replacement leaves balances,
        // reservations, the curve, effective quantities and the state
        // root completely unchanged — no ghost curve is ever created.
        const auto it{m_curves.find({side, account})};
        const CAmount old_reserved{it == m_curves.end() ? 0 : it->second.reserved};
        if (*need > old_reserved) {
            if (!m_ledger.Reserve(account, reserve_asset, *need - old_reserved)) return false;
        } else if (*need < old_reserved) {
            // The recorded amount tracks consumption exactly (Consume),
            // so this release matches what the ledger holds for this
            // curve; a failure would mean internal inconsistency — fail
            // closed, changing nothing.
            if (!m_ledger.Release(account, reserve_asset, old_reserved - *need)) return false;
        }
        Curve& slot{it == m_curves.end() ? m_curves[{side, account}] : it->second};
        slot.points = points;
        slot.filled = 0;
        slot.reserved = *need;
        return true;
    }

    //! Cancel a standing curve and release its remaining reservation.
    //! If the release fails — an impossible state under exact
    //! consumption tracking — the curve is NOT erased and false is
    //! returned: an accounting inconsistency must stay visible, never be
    //! papered over by discarding the only record of the reservation.
    bool CancelCurve(const AccountId& account, Side side)
    {
        const auto it{m_curves.find({side, account})};
        if (it == m_curves.end()) return false;
        const AssetId& reserve_asset{side == Side::BID ? m_quote : m_base};
        if (it->second.reserved > 0 &&
            !m_ledger.Release(account, reserve_asset, it->second.reserved)) {
            return false;
        }
        m_curves.erase(it);
        return true;
    }

    //! Effective (remaining) demand of a curve at price p: its monotone
    //! piecewise-linear value minus what it has already been filled.
    CAmount EffectiveQty(Side side, const AccountId& account, CAmount price) const
    {
        const auto it{m_curves.find({side, account})};
        if (it == m_curves.end()) return 0;
        return std::max<CAmount>(0, EvalCurve(it->second.points, price) - it->second.filled);
    }

    /**
     * Clear the current slot: compute the uniform price and volume,
     * allocate deterministically, settle fills internally, deduct filled
     * quantities from the persistent curves (dropping exhausted ones), and
     * advance the ledger slot.
     */
    ClearingResult ClearSlot()
    {
        ClearingResult result;
        const std::vector<CAmount> candidates{CandidatePrices()};

        for (const CAmount p : candidates) {
            const CAmount demand{SideTotal(Side::BID, p)};
            const CAmount supply{SideTotal(Side::ASK, p)};
            const CAmount volume{std::min(demand, supply)};
            if (volume <= 0) continue;
            const CAmount imbalance{demand > supply ? demand - supply : supply - demand};
            if (!result.cleared || volume > result.volume ||
                (volume == result.volume && imbalance < result.imbalance)) {
                result.cleared = true;
                result.price = p;
                result.volume = volume;
                result.imbalance = imbalance;
            }
        }

        if (result.cleared) {
            const CAmount p{result.price};
            result.bid_fill = Allocate(Side::BID, p, result.volume);
            result.ask_fill = Allocate(Side::ASK, p, result.volume);
            SettleAndConsume(result);
        }
        m_ledger.AdvanceSlot();
        return result;
    }

    //! Deterministic root binding the whole persistent book to the ledger
    //! state and slot. CANONICALLY FRAMED end to end: the curve count and
    //! each curve's breakpoint count precede the variable-length book
    //! data (clearing v2 domain), and the embedded ledger root frames its
    //! own three collections the same way (ledger v2 domain), so no two
    //! distinct states can flatten to one preimage — a requirement before
    //! this commitment can ever become consensus-facing. Each domain tag
    //! was bumped when its preimage format changed.
    uint256 StateRoot() const
    {
        HashWriter h;
        h << std::string{"b3/flowmesh/clearing/v2"} << m_base << m_quote
          << static_cast<uint64_t>(m_max_k) << m_ledger.Slot();
        h << static_cast<uint64_t>(m_curves.size());
        for (const auto& [key, curve] : m_curves) {
            h << static_cast<uint8_t>(key.first) << key.second << curve.filled << curve.reserved;
            h << static_cast<uint64_t>(curve.points.size());
            for (const Breakpoint& bp : curve.points) h << bp.price << bp.qty;
        }
        h << m_ledger.StateRoot();
        return h.GetHash();
    }

private:
    struct Curve {
        std::vector<Breakpoint> points;
        CAmount filled{0};
        CAmount reserved{0};
    };

    static CAmount FloorDiv128(const __int128 a, const __int128 b) // b > 0
    {
        __int128 q{a / b};
        if (a % b != 0 && a < 0) --q;
        // Callers only pass interpolants whose result lies between two
        // validated quantities, so the cast is in range.
        return static_cast<CAmount>(q);
    }

    //! quote = qty * price, exact and capped: std::nullopt on overflow of
    //! the native monetary range.
    static std::optional<CAmount> Quote(const CAmount qty, const CAmount price)
    {
        if (qty < 0 || price < 0) return std::nullopt;
        const unsigned __int128 product{static_cast<unsigned __int128>(qty) *
                                        static_cast<unsigned __int128>(price)};
        if (product > static_cast<unsigned __int128>(MAX_MONEY)) return std::nullopt;
        return static_cast<CAmount>(product);
    }

    static CAmount EvalCurve(const std::vector<Breakpoint>& points, const CAmount price)
    {
        // GENUINELY TOTAL, even on adversarial garbage (the public
        // EvaluateCurve entry accepts arbitrary vectors): an empty curve
        // demands nothing; any point outside MoneyRange yields the
        // deterministic zero result (stored, validated curves can never
        // contain one); a non-ascending segment degrades to the left
        // quantity instead of dividing by zero. Every subtraction is
        // performed AFTER widening each operand to 128 bits, so no
        // intermediate — including INT64_MIN/INT64_MAX inputs — can
        // overflow a signed 64-bit value. Never undefined behavior.
        if (points.empty()) return 0;
        for (const Breakpoint& bp : points) {
            if (bp.price < 0 || bp.price > MAX_MONEY || bp.qty < 0 || bp.qty > MAX_MONEY) {
                return 0;
            }
        }
        if (price <= points.front().price) return points.front().qty;
        if (price >= points.back().price) return points.back().qty;
        for (size_t i{1}; i < points.size(); ++i) {
            if (price < points[i].price) {
                const Breakpoint& a{points[i - 1]};
                const Breakpoint& b{points[i]};
                if (b.price <= a.price) return a.qty; // garbage guard (see above)
                // Floor of the exact linear interpolant keeps
                // monotonicity; exact in 128-bit throughout.
                const __int128 num{(static_cast<__int128>(b.qty) - static_cast<__int128>(a.qty)) *
                                   (static_cast<__int128>(price) - static_cast<__int128>(a.price))};
                const __int128 den{static_cast<__int128>(b.price) -
                                   static_cast<__int128>(a.price)};
                return a.qty + FloorDiv128(num, den);
            }
        }
        return points.back().qty;
    }

    std::optional<CAmount> WorstCaseReservation(Side side,
                                                const std::vector<Breakpoint>& points) const
    {
        if (side == Side::ASK) {
            // Non-decreasing: max quantity (base reserved) is at the last
            // breakpoint; base custody is price-independent.
            return points.back().qty;
        }
        // Bid terminates at zero, so demand is confined to [p0, p_last].
        // The curve is PERSISTENT: fills can accumulate across many slots
        // at many prices, so the bound must cover every possible fill
        // SEQUENCE, not just one slot. Any lot filled at price p sits at
        // a cumulative quantity where the (descending) demand still
        // reaches it, i.e. p is at most the right-edge price of the
        // quantity segment holding that lot — so total spend is bounded
        // by the staircase sum over segments:
        //     Σ (q_i − q_{i+1}) · price_{i+1}
        // (a single-slot max-rectangle bound is NOT sufficient: adversarial
        // descending-price fill sequences across slots exceed it).
        CAmount worst{0};
        for (size_t i{0}; i + 1 < points.size(); ++i) {
            const std::optional<CAmount> spend{
                Quote(points[i].qty - points[i + 1].qty, points[i + 1].price)};
            if (!spend) return std::nullopt;
            if (*spend > MAX_MONEY - worst) return std::nullopt;
            worst += *spend;
        }
        return worst;
    }

    std::vector<CAmount> CandidatePrices() const
    {
        std::set<CAmount> prices;
        for (const auto& [key, curve] : m_curves) {
            for (const Breakpoint& bp : curve.points) prices.insert(bp.price);
        }
        return {prices.begin(), prices.end()};
    }

    CAmount SideTotal(Side side, const CAmount price) const
    {
        CAmount total{0};
        for (const auto& [key, curve] : m_curves) {
            if (key.first != side) continue;
            const CAmount q{std::max<CAmount>(0, EvalCurve(curve.points, price) - curve.filled)};
            if (q > MAX_MONEY - total) return MAX_MONEY; // saturate; volume is bounded anyway
            total += q;
        }
        return total;
    }

    //! Rationing: the short side fills fully; the long side is cut to
    //! `volume` by largest-remainder allocation in account order.
    std::map<AccountId, CAmount> Allocate(Side side, const CAmount price, const CAmount volume)
    {
        std::map<AccountId, CAmount> desired;
        CAmount total{0};
        for (const auto& [key, curve] : m_curves) {
            if (key.first != side) continue;
            const CAmount q{std::max<CAmount>(0, EvalCurve(curve.points, price) - curve.filled)};
            if (q > 0) {
                desired[key.second] = q;
                total += q;
            }
        }
        std::map<AccountId, CAmount> alloc;
        if (total <= volume) { // short side: everyone fully filled
            return desired;
        }
        // Long side: floor(q * volume / total), then hand out the remainder
        // one lot at a time, largest fractional remainder first (account
        // ascending on ties).
        struct Rem { AccountId account; unsigned __int128 rem; };
        std::vector<Rem> remainders;
        CAmount handed{0};
        for (const auto& [account, q] : desired) {
            const unsigned __int128 num{static_cast<unsigned __int128>(q) *
                                        static_cast<unsigned __int128>(volume)};
            const CAmount base{static_cast<CAmount>(num / static_cast<unsigned __int128>(total))};
            alloc[account] = base;
            handed += base;
            remainders.push_back({account, num % static_cast<unsigned __int128>(total)});
        }
        CAmount leftover{volume - handed};
        std::stable_sort(remainders.begin(), remainders.end(),
                         [](const Rem& a, const Rem& b) { return a.rem > b.rem; });
        for (size_t i{0}; leftover > 0 && i < remainders.size(); ++i, --leftover) {
            ++alloc[remainders[i].account];
        }
        return alloc;
    }

    //! Settle fills as internal ledger moves (no UTXO), then deduct the
    //! filled volume from the persistent curves and drop exhausted ones.
    //!
    //! NO SILENT MASKING, NO PARTIAL SETTLEMENT: every quote computation,
    //! every per-curve reservation sufficiency AND every ledger source
    //! reservation is PREFLIGHTED before the first ledger mutation, so
    //! the moves below cannot fail; each leg's result is still checked,
    //! and a first leg is checked before the second leg executes. A
    //! violated invariant fails VISIBLY via assert — that is a fail-fast
    //! abort, not transactional recovery: nothing is rolled back, which
    //! is exactly why every failure condition is checked before any
    //! state is touched.
    void SettleAndConsume(const ClearingResult& result)
    {
        // ---- Preflight, before any mutation. Per side and account this
        // covers the account's ENTIRE settled volume: the pairwise chunks
        // below sum to exactly `fill` base lots and (at a uniform price)
        // exactly `fill * price` quote, so per-account totals bound every
        // individual chunk move.
        for (const auto& [account, fill] : result.bid_fill) {
            if (fill <= 0) continue;
            const std::optional<CAmount> quote{Quote(fill, result.price)};
            assert(quote.has_value()); // bounded by the buyer's validated reservation
            const auto it{m_curves.find({Side::BID, account})};
            assert(it != m_curves.end());
            assert(*quote <= it->second.reserved); // staircase invariant
            assert(*quote <= m_ledger.Reserved(account, m_quote)); // ledger backs the whole leg
        }
        for (const auto& [account, fill] : result.ask_fill) {
            if (fill <= 0) continue;
            const auto it{m_curves.find({Side::ASK, account})};
            assert(it != m_curves.end());
            assert(fill <= it->second.reserved);
            assert(fill <= m_ledger.Reserved(account, m_base)); // ledger backs the whole leg
        }

        // Two-pointer pairwise matching in account order: deterministic and
        // fully balanced (bid volume == ask volume == result.volume).
        std::vector<std::pair<AccountId, CAmount>> bids{result.bid_fill.begin(),
                                                        result.bid_fill.end()};
        std::vector<std::pair<AccountId, CAmount>> asks{result.ask_fill.begin(),
                                                        result.ask_fill.end()};
        size_t bi{0}, ai{0};
        while (bi < bids.size() && ai < asks.size()) {
            CAmount& b_rem{bids[bi].second};
            CAmount& a_rem{asks[ai].second};
            if (b_rem == 0) { ++bi; continue; }
            if (a_rem == 0) { ++ai; continue; }
            const CAmount fill{std::min(b_rem, a_rem)};
            const std::optional<CAmount> quote{Quote(fill, result.price)};
            assert(quote.has_value()); // preflighted above per account
            // Buyer pays quote to seller; seller delivers base to buyer.
            // A ZERO quote (possible only when the uniform price is zero:
            // fill is always positive here) is an explicit successful
            // no-op — zero value changes hands on the quote side, and the
            // ledger by design rejects zero-amount moves, so the leg must
            // be skipped rather than attempted. The paid leg is checked
            // BEFORE the delivery leg executes: a failed first leg never
            // lets the second leg run.
            if (*quote > 0) {
                const bool paid{m_ledger.MoveReservedToAvailable(bids[bi].first, asks[ai].first,
                                                                 m_quote, *quote)};
                assert(paid); // preflighted; fail fast before touching the base leg
            }
            const bool delivered{m_ledger.MoveReservedToAvailable(asks[ai].first, bids[bi].first,
                                                                  m_base, fill)};
            assert(delivered); // preflighted; a failure here is a real bug
            b_rem -= fill;
            a_rem -= fill;
        }

        // Deduct fills from the persistent curves; exhausted curves release
        // their remaining reservation and are removed.
        Consume(Side::BID, result.bid_fill, m_quote, result.price);
        Consume(Side::ASK, result.ask_fill, m_base, result.price);
    }

    void Consume(Side side, const std::map<AccountId, CAmount>& fills,
                 const AssetId& reserve_asset, const CAmount price)
    {
        for (const auto& [account, fill] : fills) {
            if (fill <= 0) continue;
            const auto it{m_curves.find({side, account})};
            if (it == m_curves.end()) continue;
            it->second.filled += fill;
            // Settlement just consumed part of this curve's ledger
            // reservation (base lots for an ask; quote = fill × price for
            // a bid). Subtract it EXACTLY — preflighted in
            // SettleAndConsume, so neither the quote computation nor the
            // subtraction can fail; a clamp or fabricated zero here would
            // only hide a real accounting bug.
            CAmount consumed{fill};
            if (side == Side::BID) {
                const std::optional<CAmount> quote{Quote(fill, price)};
                assert(quote.has_value());
                consumed = *quote;
            }
            assert(consumed <= it->second.reserved);
            it->second.reserved -= consumed;
            if (EvalCurve(it->second.points, it->second.points.back().price) -
                        it->second.filled <= 0 &&
                EvalCurve(it->second.points, it->second.points.front().price) -
                        it->second.filled <= 0) {
                if (it->second.reserved > 0) {
                    const bool released{
                        m_ledger.Release(account, reserve_asset, it->second.reserved)};
                    assert(released); // exact tracking makes this infallible
                }
                m_curves.erase(it);
            }
        }
    }

    const AssetId m_base;
    const AssetId m_quote;
    Ledger& m_ledger;
    const size_t m_max_k;
    std::map<std::pair<Side, AccountId>, Curve> m_curves;
};

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_CLEARING_H
