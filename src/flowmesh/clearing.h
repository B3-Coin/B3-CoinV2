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
#include <util/int128.h>

#include <algorithm>
#include <cstdint>
#include <ios>
#include <stdexcept>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace flowmesh {

class FlowMeshState;

//! Snapshot decode bound for the persistent book, enforced before
//! elements are read.
inline constexpr uint64_t BOOK_SNAPSHOT_MAX_CURVES{uint64_t{1} << 22};

//! HARD protocol safety ceiling on curve breakpoints. The configured
//! per-market bound (m_max_k) may only tighten this, never exceed it —
//! a bogus configuration cannot re-open the pre-allocation bound.
inline constexpr size_t HARD_MAX_CURVE_POINTS{64};

/**
 * FlowMesh persistent demand curves and deterministic batch clearing for
 * one market (base asset priced in quote asset). Everything is exact
 * integer arithmetic on price ticks and quantity lots — no floating point
 * anywhere — and fills settle internally against the ledger with no
 * per-fill UTXO spend.
 *
 * OWNERSHIP: the engine stores NO ledger binding, and its value-moving
 * operations (submit/cancel/clear) are PRIVATE with FlowMeshState as
 * their only caller — the sole way to reach them is through the owning
 * state, which always pairs its own book with its own ledger. No public
 * API accepts an arbitrary Ledger&, so book A + ledger B cannot be
 * combined even accidentally.
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
        if (m_max_k == 0 || m_max_k > HARD_MAX_CURVE_POINTS) {
            throw std::invalid_argument("flowmesh curve bound outside the hard protocol cap");
        }
        // Reservation accounting assumes one asset per side: a BID
        // reserves quote, an ASK reserves base. base == quote would
        // merge the two pools (fails closed today, but the assumption
        // must hold by construction, not by luck).
        if (m_base == m_quote) {
            throw std::invalid_argument("flowmesh market base and quote must differ");
        }
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

private:
    friend class FlowMeshState;

    /**
     * Submit or replace an account's curve on a side, reserving against
     * `ledger` (the owning FlowMeshState's ledger — FlowMeshState is
     * the only caller). The worst-case
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

public:
    //! Effective (remaining) demand of a curve at price p.
    CAmount EffectiveQty(Side side, const AccountId& account, CAmount price) const
    {
        const auto it{m_curves.find({side, account})};
        if (it == m_curves.end()) return 0;
        return std::max<CAmount>(0, EvalCurve(it->second.points, price) - it->second.filled);
    }

private:
    /**
     * Clear the current slot against `ledger` (FlowMeshState is the
     * only caller): compute the uniform price
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

        // EXACT widened aggregation: aggregate demand can legitimately
        // exceed MAX_MONEY (many bids), so totals are computed in 128
        // bits with NO saturation — saturation must never change
        // selection or allocation semantics. The traded volume itself
        // is bounded by the reserved supply side (<= per-asset custody
        // <= MAX_MONEY); a violation of that bound is a fatal internal
        // inconsistency, not a clamp.
        util::Signed128 best_imbalance{0};
        for (const CAmount p : candidates) {
            const util::Signed128 demand{SideTotal128(Side::BID, p)};
            const util::Signed128 supply{SideTotal128(Side::ASK, p)};
            const util::Signed128 volume{demand < supply ? demand : supply};
            if (volume <= 0) continue;
            const util::Signed128 imbalance{
                demand > supply ? demand - supply : supply - demand};
            if (!result.cleared || volume > static_cast<util::Signed128>(result.volume) ||
                (volume == static_cast<util::Signed128>(result.volume) &&
                 imbalance < best_imbalance)) {
                if (volume > static_cast<util::Signed128>(MAX_MONEY)) return std::nullopt; // fatal
                result.cleared = true;
                result.price = p;
                result.volume = static_cast<CAmount>(volume);
                result.imbalance = imbalance > static_cast<util::Signed128>(MAX_MONEY)
                                       ? MAX_MONEY
                                       : static_cast<CAmount>(imbalance); // tie-break only
                best_imbalance = imbalance;
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

public:
    /**
     * Post-decode accounting reconciliation (snapshots): every restored
     * curve must be semantically reachable — fill within the curve's
     * quantity range, reservation within the worst-case bound — and the
     * ledger's reservations must match the book EXACTLY: for each
     * account, Reserved(quote) equals its bid curve's recorded
     * reservation, Reserved(base) equals its ask curve's, and no
     * reservation exists without a backing curve (reservations have no
     * other source in this system). Throws on any violation: a decoded
     * snapshot cannot construct impossible reservation state.
     */
    void CheckDecodedAccounting(const Ledger& ledger) const
    {
        std::map<std::pair<AccountId, AssetId>, CAmount> expected;
        for (const auto& [key, curve] : m_curves) {
            const bool bid{key.first == Side::BID};
            const CAmount max_qty{bid ? curve.points.front().qty : curve.points.back().qty};
            if (curve.filled < 0 || curve.filled > max_qty) {
                throw std::ios_base::failure("flowmesh book snapshot fill state impossible");
            }
            // An EXHAUSTED curve never survives in live state (Consume
            // erases it): a retained one is unreachable accounting.
            if (EvalCurve(curve.points, curve.points.front().price) - curve.filled <= 0 &&
                EvalCurve(curve.points, curve.points.back().price) - curve.filled <= 0) {
                throw std::ios_base::failure("flowmesh book snapshot retains an exhausted curve");
            }
            if (bid) {
                // BID residual: REACHABLE range only. Live submission
                // reserves EXACTLY the staircase bound; each fill of
                // one lot consumes between 0 (a zero-price clear) and
                // that lot's per-lot price bound. Hence:
                //   filled == 0  =>  reserved == worst (exact), and
                //   otherwise     worst - MaxPrefixSpend(filled)
                //                   <= reserved <= worst.
                // Anything outside that band cannot arise through
                // execution and may fail settlement later.
                const std::optional<CAmount> worst{WorstCaseReservation(key.first, curve.points)};
                const std::optional<CAmount> max_spend{
                    BidMaxPrefixSpend(curve.points, curve.filled)};
                if (!worst || !max_spend || curve.reserved < 0 || curve.reserved > *worst ||
                    curve.reserved < *worst - *max_spend) {
                    throw std::ios_base::failure(
                        "flowmesh book snapshot reservation state impossible");
                }
            } else {
                // ASK residual is EXACT in live state: reserved base ==
                // remaining deliverable == max qty - filled. An ask
                // "needing 10 but reserving 5" is unreachable.
                if (curve.reserved != max_qty - curve.filled) {
                    throw std::ios_base::failure(
                        "flowmesh book snapshot ask reservation is not the exact residual");
                }
            }
            // base != quote is enforced at construction, so each
            // (account, asset) key aggregates exactly one curve's
            // reservation — no summation overflow is possible.
            expected[{key.second, bid ? m_quote : m_base}] += curve.reserved;
        }
        bool ok{true};
        ledger.ForEachBalance([&](const AccountId& account, const AssetId& asset,
                                  const Ledger::Balance& balance) {
            if (balance.reserved == 0) return;
            const auto it{expected.find({account, asset})};
            if (it == expected.end() || it->second != balance.reserved) ok = false;
        });
        for (const auto& [key, reserved] : expected) {
            if (ledger.Reserved(key.first, key.second) != reserved) ok = false;
        }
        if (!ok) {
            throw std::ios_base::failure(
                "flowmesh book reservations do not reconcile with the ledger");
        }
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
            s << static_cast<uint8_t>(key.first) << key.second;
            WriteCompactSize(s, curve.points.size());
            for (const Breakpoint& bp : curve.points) s << bp;
            s << curve.filled << curve.reserved;
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
            s >> side >> account;
            if (side > static_cast<uint8_t>(Side::ASK)) {
                throw std::ios_base::failure("flowmesh book snapshot has an invalid side");
            }
            // Curve points: count validated against THIS market's bound
            // BEFORE any allocation or element read.
            Curve curve;
            const uint64_t points{ReadCompactSize(s)};
            if (points > m_max_k || points > HARD_MAX_CURVE_POINTS) {
                throw std::ios_base::failure("flowmesh book snapshot curve too large");
            }
            curve.points.reserve(points);
            for (uint64_t j{0}; j < points; ++j) {
                Breakpoint bp;
                s >> bp;
                curve.points.push_back(bp);
            }
            s >> curve.filled >> curve.reserved;
            const std::pair<Side, AccountId> key{static_cast<Side>(side), account};
            if (!curves.empty() && !(std::prev(curves.end())->first < key)) {
                throw std::ios_base::failure("flowmesh book snapshot keys not canonical");
            }
            // Semantic curve validity on decode: count/monotonicity/
            // range via the same rule live submission enforces, plus
            // sane fill/reservation accounting fields.
            if (!CurveIsValid(key.first, curve.points)) {
                throw std::ios_base::failure("flowmesh book snapshot curve invalid");
            }
            if (curve.filled < 0 || curve.filled > MAX_MONEY || curve.reserved < 0 ||
                curve.reserved > MAX_MONEY) {
                throw std::ios_base::failure("flowmesh book snapshot curve accounting invalid");
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
        // No generic serializer on purpose: the book codec decodes
        // points with a pre-allocation bound against m_max_k.
    };

    static CAmount FloorDiv128(const util::Signed128 a, const util::Signed128 b) // b > 0
    {
        util::Signed128 q{a / b};
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
        const util::Unsigned128 product{static_cast<util::Unsigned128>(qty) *
                                        static_cast<util::Unsigned128>(price)};
        if (product > static_cast<util::Unsigned128>(MAX_MONEY)) return std::nullopt;
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
                const util::Signed128 num{
                    (static_cast<util::Signed128>(b.qty) -
                     static_cast<util::Signed128>(a.qty)) *
                    (static_cast<util::Signed128>(price) -
                     static_cast<util::Signed128>(a.price))};
                const util::Signed128 den{static_cast<util::Signed128>(b.price) -
                                          static_cast<util::Signed128>(a.price)};
                return a.qty + FloorDiv128(num, den);
            }
        }
        return points.back().qty;
    }

    /**
     * Maximum quote a BID can have spent after `filled` lots: the
     * filled lots are always the SMALLEST cumulative positions (fills
     * reduce effective quantity uniformly at every price), and the lot
     * at cumulative position λ in (q_{i+1}, q_i] can never fill above
     * integer price p_{i+1}-1 — so the maximum spend is the staircase
     * prefix over the first `filled` lots, consumed from the
     * highest-price segments downward.
     */
    static std::optional<CAmount> BidMaxPrefixSpend(const std::vector<Breakpoint>& points,
                                                    const CAmount filled)
    {
        if (filled < 0) return std::nullopt;
        util::Signed128 spend{0};
        CAmount remaining{filled};
        for (size_t i{points.size()}; i-- > 1 && remaining > 0;) {
            const CAmount width{points[i - 1].qty - points[i].qty};
            const CAmount take{std::min(remaining, width)};
            spend += static_cast<util::Signed128>(take) * (points[i].price - 1);
            remaining -= take;
        }
        if (remaining > 0) return std::nullopt; // filled exceeds the curve: impossible
        if (spend > static_cast<util::Signed128>(MAX_MONEY)) return MAX_MONEY;
        return static_cast<CAmount>(spend);
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
        //     Σ (q_i − q_{i+1}) · (price_{i+1} − 1).
        // HONEST CHARACTERIZATION: this is a SAFE UPPER BOUND (every
        // per-lot price is individually maximal), proven EXACT for the
        // degenerate single-segment limit order {(P,Q),(P+1,0)} — which
        // reserves exactly Q·P — but CONSERVATIVE in general: no proof
        // is claimed that every lot can reach its per-lot maximum
        // simultaneously across multi-segment curves, so multi-segment
        // bids may reserve somewhat more than any realizable spend.
        // That conservatism is deliberate and economically acceptable
        // (funds are merely reserved, released exactly on cancel or
        // exhaustion); the settlement invariants only require an upper
        // bound. Strictly ascending prices with p_0 >= 0 guarantee
        // price_{i+1} >= 1, so the subtraction is safe.
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

    //! Exact widened side total: never saturates (curve count and
    //! per-curve quantities are bounded, so the 128-bit sum cannot
    //! overflow).
    util::Signed128 SideTotal128(Side side, const CAmount price) const
    {
        util::Signed128 total{0};
        for (const auto& [key, curve] : m_curves) {
            if (key.first != side) continue;
            const CAmount q{std::max<CAmount>(0, EvalCurve(curve.points, price) - curve.filled)};
            total += static_cast<util::Signed128>(q);
        }
        return total;
    }

    //! Rationing: the short side fills fully; the long side is cut to
    //! `volume` by largest-remainder allocation in account order.
    std::map<AccountId, CAmount> Allocate(Side side, const CAmount price, const CAmount volume)
    {
        std::map<AccountId, CAmount> desired;
        util::Unsigned128 total{0}; // exact: an intermediate sum may exceed MAX_MONEY
        for (const auto& [key, curve] : m_curves) {
            if (key.first != side) continue;
            const CAmount q{std::max<CAmount>(0, EvalCurve(curve.points, price) - curve.filled)};
            if (q > 0) {
                desired[key.second] = q;
                total += static_cast<util::Unsigned128>(q);
            }
        }
        std::map<AccountId, CAmount> alloc;
        if (total <= static_cast<util::Unsigned128>(volume)) { // short side: fully filled
            return desired;
        }
        // Long side: largest-remainder rationing in account order, with
        // every product/total widened — an aggregate overflow can never
        // hand back an un-rationed map.
        struct Rem { AccountId account; util::Unsigned128 rem; };
        std::vector<Rem> remainders;
        CAmount handed{0};
        for (const auto& [account, q] : desired) {
            const util::Unsigned128 num{static_cast<util::Unsigned128>(q) *
                                        static_cast<util::Unsigned128>(volume)};
            const CAmount base{static_cast<CAmount>(num / total)};
            alloc[account] = base;
            handed += base;
            remainders.push_back({account, num % total});
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
        // ---- Allocation invariant, before ANY mutation: both advertised
        // fill maps must sum exactly to the clearing volume. Any
        // violation is a fatal execution failure — never settle a
        // mis-rationed allocation.
        {
            util::Unsigned128 bid_sum{0}, ask_sum{0};
            for (const auto& [account, fill] : result.bid_fill) {
                if (fill < 0) return false;
                bid_sum += static_cast<util::Unsigned128>(fill);
            }
            for (const auto& [account, fill] : result.ask_fill) {
                if (fill < 0) return false;
                ask_sum += static_cast<util::Unsigned128>(fill);
            }
            if (bid_sum != static_cast<util::Unsigned128>(result.volume) ||
                ask_sum != static_cast<util::Unsigned128>(result.volume)) {
                return false;
            }
        }

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
