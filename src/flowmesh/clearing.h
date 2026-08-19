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
#include <cstdint>
#include <ios>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace flowmesh {

//! Snapshot decode bound for the persistent book, enforced before
//! elements are read.
inline constexpr uint64_t BOOK_SNAPSHOT_MAX_CURVES{uint64_t{1} << 22};

/**
 * FlowMesh persistent demand curves and deterministic batch clearing for
 * one market (base asset priced in quote asset). Everything is exact
 * integer arithmetic on price ticks and quantity lots — no floating point
 * anywhere — and fills settle internally against the ledger with no
 * per-fill UTXO spend.
 *
 * OWNERSHIP: the engine stores NO ledger binding. Every operation that
 * moves value takes the authoritative Ledger as an explicit parameter,
 * so an engine can never be attached to (or drift onto) a different
 * ledger than its owner's — FlowMeshState passes its own ledger and the
 * pairing is correct by construction.
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
 *
 * FAILURE MODEL: no consensus-relevant invariant depends on assert.
 * ClearSlot returns nullopt on any internal accounting inconsistency —
 * an explicit fatal error the caller must treat as "commit no candidate
 * state" (candidate execution runs on a discardable copy, so a fatal
 * slot can never leave a half-settled committed state).
 */
class ClearingEngine
{
public:
    enum class Side { BID, ASK };

    struct Breakpoint {
        CAmount price{0}; // ticks
        CAmount qty{0};   // lots

        SERIALIZE_METHODS(Breakpoint, obj) { READWRITE(obj.price, obj.qty); }
    };

    struct ClearingResult {
        bool cleared{false};
        CAmount price{0};
        CAmount volume{0};
        CAmount imbalance{0}; // |demand - supply| at the clearing price (tie-break)
        std::map<AccountId, CAmount> bid_fill; // base lots bought
        std::map<AccountId, CAmount> ask_fill; // base lots sold
    };

    ClearingEngine(const AssetId& base, const AssetId& quote, size_t max_k = 8)
        : m_base{base}, m_quote{quote}, m_max_k{max_k}
    {
    }

    const AssetId& BaseAsset() const { return m_base; }
    const AssetId& QuoteAsset() const { return m_quote; }

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
     * Submit or replace an account's curve on a side, reserving against
     * `ledger` (the owner's authoritative ledger). The worst-case
     * reservation (quote for a bid, base for an ask) must be backed by
     * the account's available balance, or the submission is rejected and
     * nothing changes. A replacement adjusts the ledger by exactly the
     * DELTA between the old remaining reservation and the new
     * requirement.
     */
    bool SubmitCurve(Ledger& ledger, const AccountId& account, Side side,
                     const std::vector<Breakpoint>& points)
    {
        if (!CurveIsValid(side, points)) return false;
        const AssetId& reserve_asset{side == Side::BID ? m_quote : m_base};
        const std::optional<CAmount> need{WorstCaseReservation(side, points)};
        if (!need) return false;

        // ATOMIC by reservation-delta accounting: look up without
        // inserting, adjust the ledger by exactly the difference, and
        // only touch the book after the ledger operation succeeded.
        const auto it{m_curves.find({side, account})};
        const CAmount old_reserved{it == m_curves.end() ? 0 : it->second.reserved};
        if (*need > old_reserved) {
            if (!ledger.Reserve(account, reserve_asset, *need - old_reserved)) return false;
        } else if (*need < old_reserved) {
            if (!ledger.Release(account, reserve_asset, old_reserved - *need)) return false;
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
    //! returned: an accounting inconsistency must stay visible.
    bool CancelCurve(Ledger& ledger, const AccountId& account, Side side)
    {
        const auto it{m_curves.find({side, account})};
        if (it == m_curves.end()) return false;
        const AssetId& reserve_asset{side == Side::BID ? m_quote : m_base};
        if (it->second.reserved > 0 &&
            !ledger.Release(account, reserve_asset, it->second.reserved)) {
            return false;
        }
        m_curves.erase(it);
        return true;
    }

    //! Effective (remaining) demand of a curve at price p.
    CAmount EffectiveQty(Side side, const AccountId& account, CAmount price) const
    {
        const auto it{m_curves.find({side, account})};
        if (it == m_curves.end()) return 0;
        return std::max<CAmount>(0, EvalCurve(it->second.points, price) - it->second.filled);
    }

    /**
     * Clear the current slot against `ledger`: compute the uniform price
     * and volume, allocate deterministically, settle fills internally,
     * deduct filled quantities from the persistent curves (dropping
     * exhausted ones), and advance the ledger slot.
     *
     * Returns nullopt on FATAL internal inconsistency (a settlement
     * plan or ledger move that exact accounting says cannot fail,
     * failing anyway). Every such condition is preflighted BEFORE the
     * first ledger mutation; the residual mid-settlement checks cannot
     * trigger unless the preflight itself is wrong, and the caller must
     * discard the candidate state either way.
     */
    std::optional<ClearingResult> ClearSlot(Ledger& ledger)
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
            if (!SettleAndConsume(ledger, result)) return std::nullopt;
        }
        ledger.AdvanceSlot();
        return result;
    }

    //! Deterministic root binding the whole persistent book to the given
    //! ledger's state and slot. CANONICALLY FRAMED end to end (identical
    //! preimage to the pre-refactor form).
    uint256 StateRoot(const Ledger& ledger) const
    {
        HashWriter h;
        h << std::string{"b3/flowmesh/clearing/v2"} << m_base << m_quote
          << static_cast<uint64_t>(m_max_k) << ledger.Slot();
        h << static_cast<uint64_t>(m_curves.size());
        for (const auto& [key, curve] : m_curves) {
            h << static_cast<uint8_t>(key.first) << key.second << curve.filled << curve.reserved;
            h << static_cast<uint64_t>(curve.points.size());
            for (const Breakpoint& bp : curve.points) h << bp.price << bp.qty;
        }
        h << ledger.StateRoot();
        return h.GetHash();
    }

    /**
     * Canonical whole-book serialization (snapshots). The market
     * configuration is verified, the curve count is bounded before
     * elements are read, and keys must be strictly ascending (one byte
     * representation per state).
     */
    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << m_base << m_quote << static_cast<uint64_t>(m_max_k);
        WriteCompactSize(s, m_curves.size());
        for (const auto& [key, curve] : m_curves) {
            s << static_cast<uint8_t>(key.first) << key.second << curve;
        }
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        AssetId base, quote;
        uint64_t max_k;
        s >> base >> quote >> max_k;
        if (base != m_base || quote != m_quote || max_k != m_max_k) {
            throw std::ios_base::failure("flowmesh book snapshot is for a different market");
        }
        const uint64_t n{ReadCompactSize(s)};
        if (n > BOOK_SNAPSHOT_MAX_CURVES) {
            throw std::ios_base::failure("flowmesh book snapshot too large");
        }
        std::map<std::pair<Side, AccountId>, Curve> curves;
        for (uint64_t i{0}; i < n; ++i) {
            uint8_t side;
            AccountId account;
            Curve curve;
            s >> side >> account >> curve;
            if (side > static_cast<uint8_t>(Side::ASK)) {
                throw std::ios_base::failure("flowmesh book snapshot has an invalid side");
            }
            const std::pair<Side, AccountId> key{static_cast<Side>(side), account};
            if (!curves.empty() && !(std::prev(curves.end())->first < key)) {
                throw std::ios_base::failure("flowmesh book snapshot keys not canonical");
            }
            curves.emplace_hint(curves.end(), key, std::move(curve));
        }
        m_curves = std::move(curves);
    }

private:
    struct Curve {
        std::vector<Breakpoint> points;
        CAmount filled{0};
        CAmount reserved{0};

        SERIALIZE_METHODS(Curve, obj) { READWRITE(obj.points, obj.filled, obj.reserved); }
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
        // deterministic zero result; a non-ascending segment degrades to
        // the left quantity instead of dividing by zero. Every
        // subtraction is performed AFTER widening each operand to 128
        // bits. Never undefined behavior.
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
        // SEQUENCE. Prices are integer ticks and every fill happens at a
        // candidate price (some curve's breakpoint — an integer), so a
        // lot at cumulative quantity λ in (q_{i+1}, q_i] can only fill at
        // an integer price p with demand(p) >= λ > q_{i+1} = demand at
        // p_{i+1}; demand is non-increasing, hence p <= p_{i+1} - 1.
        // Total spend is therefore bounded by the staircase sum
        //     Σ (q_i − q_{i+1}) · (price_{i+1} − 1)
        // — the EXACT integer-price worst case. (The earlier
        // right-edge-price bound over-reserved by one tick per lot: a
        // degenerate BUY {(P,Q),(P+1,0)} reserved Q·(P+1) although no
        // fill above P is possible.) Strictly ascending prices with
        // p_0 >= 0 guarantee price_{i+1} >= 1, so the subtraction is
        // safe.
        CAmount worst{0};
        for (size_t i{0}; i + 1 < points.size(); ++i) {
            const std::optional<CAmount> spend{
                Quote(points[i].qty - points[i + 1].qty, points[i + 1].price - 1)};
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
                if (q > MAX_MONEY - total) return desired; // unreachable; bounded by custody
                total += q;
            }
        }
        std::map<AccountId, CAmount> alloc;
        if (total <= volume) { // short side: everyone fully filled
            return desired;
        }
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

    /**
     * Settle fills as internal ledger moves (no UTXO), then deduct the
     * filled volume from the persistent curves and drop exhausted ones.
     *
     * NO SILENT MASKING, NO ASSERT: every quote computation, per-curve
     * reservation sufficiency AND ledger source reservation is
     * PREFLIGHTED before the first ledger mutation; any preflight
     * failure returns false with NOTHING mutated. The per-move results
     * are still checked — a mid-settlement failure (impossible when the
     * preflight is correct) also returns false, and the caller discards
     * the candidate state, so no partially settled state can ever be
     * committed.
     */
    [[nodiscard]] bool SettleAndConsume(Ledger& ledger, const ClearingResult& result)
    {
        // ---- Preflight, before any mutation.
        for (const auto& [account, fill] : result.bid_fill) {
            if (fill <= 0) continue;
            const std::optional<CAmount> quote{Quote(fill, result.price)};
            if (!quote) return false;
            const auto it{m_curves.find({Side::BID, account})};
            if (it == m_curves.end()) return false;
            if (*quote > it->second.reserved) return false; // staircase invariant
            if (*quote > ledger.Reserved(account, m_quote)) return false;
        }
        for (const auto& [account, fill] : result.ask_fill) {
            if (fill <= 0) continue;
            const auto it{m_curves.find({Side::ASK, account})};
            if (it == m_curves.end()) return false;
            if (fill > it->second.reserved) return false;
            if (fill > ledger.Reserved(account, m_base)) return false;
        }

        // Two-pointer pairwise matching in account order: deterministic
        // and fully balanced (bid volume == ask volume == result.volume).
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
            if (!quote) return false; // preflighted; fatal if reached
            // Buyer pays quote to seller; seller delivers base to buyer.
            // A ZERO quote (possible only at a zero uniform price) is an
            // explicit successful no-op on the quote side. The paid leg
            // is checked BEFORE the delivery leg executes.
            if (*quote > 0) {
                if (!ledger.MoveReservedToAvailable(bids[bi].first, asks[ai].first, m_quote,
                                                    *quote)) {
                    return false; // fatal: preflight said this cannot happen
                }
            }
            if (!ledger.MoveReservedToAvailable(asks[ai].first, bids[bi].first, m_base, fill)) {
                return false; // fatal
            }
            b_rem -= fill;
            a_rem -= fill;
        }

        // Deduct fills from the persistent curves; exhausted curves
        // release their remaining reservation and are removed.
        if (!Consume(ledger, Side::BID, result.bid_fill, m_quote, result.price)) return false;
        if (!Consume(ledger, Side::ASK, result.ask_fill, m_base, result.price)) return false;
        return true;
    }

    [[nodiscard]] bool Consume(Ledger& ledger, Side side,
                               const std::map<AccountId, CAmount>& fills,
                               const AssetId& reserve_asset, const CAmount price)
    {
        for (const auto& [account, fill] : fills) {
            if (fill <= 0) continue;
            const auto it{m_curves.find({side, account})};
            if (it == m_curves.end()) continue;
            it->second.filled += fill;
            CAmount consumed{fill};
            if (side == Side::BID) {
                const std::optional<CAmount> quote{Quote(fill, price)};
                if (!quote) return false; // fatal inconsistency
                consumed = *quote;
            }
            if (consumed > it->second.reserved) return false; // fatal inconsistency
            it->second.reserved -= consumed;
            if (EvalCurve(it->second.points, it->second.points.back().price) -
                        it->second.filled <= 0 &&
                EvalCurve(it->second.points, it->second.points.front().price) -
                        it->second.filled <= 0) {
                if (it->second.reserved > 0 &&
                    !ledger.Release(account, reserve_asset, it->second.reserved)) {
                    return false; // fatal: exact tracking makes this infallible
                }
                m_curves.erase(it);
            }
        }
        return true;
    }

    // Not const so the engine is assignable (candidate execution copies
    // and replaces whole states).
    AssetId m_base;
    AssetId m_quote;
    size_t m_max_k;
    std::map<std::pair<Side, AccountId>, Curve> m_curves;
};

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_CLEARING_H
