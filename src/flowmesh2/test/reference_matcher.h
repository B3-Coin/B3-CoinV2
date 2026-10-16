// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_TEST_REFERENCE_MATCHER_H
#define B3_FLOWMESH2_TEST_REFERENCE_MATCHER_H

#include <flowmesh2/core/action.h>
#include <flowmesh2/core/ids.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

/** A naive O(n^2) reference matcher for the A2 sweep (TEST ONLY, slice R6).
 *
 * Written from the rule text in ARCHITECTURE.md section 11 and the file
 * comment of engine/matcher.h, sharing nothing with engine/ or ledger/
 * except the id types and the Side enum. Every question is answered by
 * scanning plain vectors: the best price, the eligible takers in rank order
 * (repeated minimum selection), the level's supply, the FIFO maker (the
 * smallest arrival at that price), and the pro-rata leftover (a taker gets an
 * extra lot iff fewer than `leftover` takers are ahead of it: a larger
 * remainder, or an equal one and a lower rank). The seller fee is
 * ceil(cum * ppm / 1e6) differences in plain 64-bit arithmetic, so inputs
 * must keep every product below 2^64 (the seeded tests use small numbers;
 * Overflow() throws if they do not).
 *
 * It does not settle balances; the tests derive the expected ledger from its
 * fills.
 */
namespace flowmesh2::test::reference {

enum class Stop { FILLED, NO_CROSS, ORDER_FILL_CAP, SLOT_FILL_CAP };

struct Maker {
    OrderId id;
    AccountId owner;
    Side side{Side::BUY};
    uint64_t price{0};
    uint64_t lots{0};
    uint64_t arrival{0};
    uint64_t cum_quote{0};
    uint64_t fee_charged{0};
    uint64_t fills{0};
};

struct Taker {
    OrderId id;
    AccountId owner;
    Side side{Side::BUY};
    uint64_t limit{0};
    uint64_t lots{0};
    uint64_t rank{0};
    uint64_t cum_quote{0};
    uint64_t fee_charged{0};
    uint64_t fills{0};
    uint64_t paid{0}; //!< what the taker spent from its reservation (quote for a BUY, base for a SELL)
    Stop stop{Stop::FILLED};
};

struct Fill {
    OrderId maker;
    OrderId taker;
    AccountId maker_owner;
    AccountId taker_owner;
    Side taker_side{Side::BUY};
    uint64_t price{0};
    uint64_t lots{0};
    uint64_t base{0};
    uint64_t quote{0};
    uint64_t fee{0};
    uint64_t seller_before{0};
};

struct Rules {
    uint64_t base_per_lot{1};
    uint64_t quote_per_tick{1};
    uint64_t fee_ppm{0};
    uint64_t fill_cap{0};
    uint64_t budget{0};
};

struct Run {
    std::vector<Fill> fills;
    std::vector<Maker> makers; //!< every maker, closed ones with lots == 0, in input order
    std::vector<Taker> takers; //!< in input order
    std::vector<OrderId> closed;
    uint64_t fees{0};
    uint64_t budget_left{0};
    uint64_t prorata_rounds{0}; //!< rounds whose eligible demand exceeded the level (coverage only)
    uint64_t leftover_rounds{0}; //!< of those, rounds that handed out leftover lots (coverage only)
};

inline uint64_t Mul(uint64_t a, uint64_t b)
{
    if (a != 0 && b > UINT64_MAX / a) throw std::overflow_error("reference matcher: product above 2^64-1");
    return a * b;
}

inline uint64_t CeilFee(uint64_t cum, uint64_t ppm) { return (Mul(cum, ppm) + 999'999) / 1'000'000; }

inline bool Reach(Side side, uint64_t limit, uint64_t price) { return side == Side::BUY ? price <= limit : price >= limit; }

inline Run Match(std::vector<Maker> makers, std::vector<Taker> takers, const Rules& rules)
{
    Run run;
    run.budget_left = rules.budget;
    bool budget_out{false};
    for (const Side side : {Side::BUY, Side::SELL}) {
        const Side other{side == Side::BUY ? Side::SELL : Side::BUY};
        while (!budget_out && run.budget_left > 0) {
            bool any{false};
            uint64_t best{0};
            for (const Maker& m : makers) {
                if (m.side != other || m.lots == 0) continue;
                if (!any || (other == Side::SELL ? m.price < best : m.price > best)) best = m.price;
                any = true;
            }
            if (!any) break;

            // Eligible takers in ascending rank, by repeated minimum selection.
            std::vector<size_t> eligible;
            std::vector<bool> picked(takers.size(), false);
            while (true) {
                size_t pick{takers.size()};
                for (size_t i{0}; i < takers.size(); ++i) {
                    const Taker& t{takers[i]};
                    if (picked[i] || t.side != side || t.lots == 0 || t.fills >= rules.fill_cap || !Reach(side, t.limit, best)) continue;
                    if (pick == takers.size() || t.rank < takers[pick].rank) pick = i;
                }
                if (pick == takers.size()) break;
                picked[pick] = true;
                eligible.push_back(pick);
            }
            if (eligible.empty()) break;

            uint64_t supply{0};
            for (const Maker& m : makers) {
                if (m.side == other && m.price == best) supply += m.lots;
            }
            uint64_t demand{0};
            for (const size_t i : eligible) demand += takers[i].lots;
            std::vector<uint64_t> share(eligible.size());
            if (demand <= supply) {
                for (size_t k{0}; k < eligible.size(); ++k) share[k] = takers[eligible[k]].lots;
            } else {
                std::vector<uint64_t> rem(eligible.size());
                uint64_t given{0};
                for (size_t k{0}; k < eligible.size(); ++k) {
                    share[k] = Mul(supply, takers[eligible[k]].lots) / demand;
                    rem[k] = Mul(supply, takers[eligible[k]].lots) % demand;
                    given += share[k];
                }
                const uint64_t leftover{supply - given};
                run.prorata_rounds += 1;
                if (leftover > 0) run.leftover_rounds += 1;
                std::vector<uint64_t> extra(eligible.size(), 0);
                for (size_t k{0}; k < eligible.size(); ++k) {
                    uint64_t ahead{0};
                    for (size_t j{0}; j < eligible.size(); ++j) {
                        const bool larger{rem[j] > rem[k]};
                        const bool tie_lower_rank{rem[j] == rem[k] && takers[eligible[j]].rank < takers[eligible[k]].rank};
                        if (larger || tie_lower_rank) ++ahead;
                    }
                    if (ahead < leftover) extra[k] = 1;
                }
                for (size_t k{0}; k < eligible.size(); ++k) share[k] += extra[k];
            }

            for (size_t k{0}; k < eligible.size() && !budget_out; ++k) {
                Taker& t{takers[eligible[k]]};
                uint64_t need{share[k]};
                while (need > 0 && t.fills < rules.fill_cap) {
                    if (run.budget_left == 0) {
                        budget_out = true;
                        break;
                    }
                    // FIFO: the oldest maker with lots at the best price.
                    size_t mk{makers.size()};
                    for (size_t j{0}; j < makers.size(); ++j) {
                        const Maker& m{makers[j]};
                        if (m.side != other || m.price != best || m.lots == 0) continue;
                        if (mk == makers.size() || m.arrival < makers[mk].arrival) mk = j;
                    }
                    if (mk == makers.size()) throw std::logic_error("reference matcher: allocation above the level");
                    Maker& m{makers[mk]};
                    const uint64_t lots{need < m.lots ? need : m.lots};
                    const uint64_t base{Mul(lots, rules.base_per_lot)};
                    const uint64_t quote{Mul(Mul(lots, m.price), rules.quote_per_tick)};
                    const bool taker_sells{side == Side::SELL};
                    const uint64_t before{taker_sells ? t.cum_quote : m.cum_quote};
                    const uint64_t fee{CeilFee(before + quote, rules.fee_ppm) - CeilFee(before, rules.fee_ppm)};
                    run.fills.push_back(Fill{m.id, t.id, m.owner, t.owner, side, m.price, lots, base, quote, fee, before});
                    m.lots -= lots;
                    m.cum_quote += quote;
                    m.fills += 1;
                    if (!taker_sells) m.fee_charged += fee;
                    if (m.lots == 0) run.closed.push_back(m.id);
                    t.lots -= lots;
                    t.cum_quote += quote;
                    t.fills += 1;
                    t.paid += taker_sells ? base : quote;
                    if (taker_sells) t.fee_charged += fee;
                    run.fees += fee;
                    run.budget_left -= 1;
                    need -= lots;
                }
            }
        }
    }

    for (Taker& t : takers) {
        const Side other{t.side == Side::BUY ? Side::SELL : Side::BUY};
        bool crosses{false};
        for (const Maker& m : makers) {
            if (m.side == other && m.lots > 0 && Reach(t.side, t.limit, m.price)) crosses = true;
        }
        if (t.lots == 0) {
            t.stop = Stop::FILLED;
        } else if (!crosses) {
            t.stop = Stop::NO_CROSS;
        } else if (t.fills >= rules.fill_cap) {
            t.stop = Stop::ORDER_FILL_CAP;
        } else {
            t.stop = Stop::SLOT_FILL_CAP;
        }
    }
    run.makers = std::move(makers);
    run.takers = std::move(takers);
    return run;
}

} // namespace flowmesh2::test::reference

#endif // B3_FLOWMESH2_TEST_REFERENCE_MATCHER_H
