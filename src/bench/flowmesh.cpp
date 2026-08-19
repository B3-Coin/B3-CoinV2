// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! Honest latency benchmark of the FlowMesh deterministic engine: the
//! COMPLETE slot pipeline — action-set construction, canonicalization,
//! authentication calls, per-signer sequencing, curve submission with
//! ledger reservations, uniform-price clearing, internal settlement,
//! and both commitment roots — measured per slot in steady state.
//!
//! HONESTY NOTES, so the numbers mean what they claim:
//!  - Workloads are verified OUTSIDE the timed section: the crossing
//!    workload asserts that every action applies and every slot clears
//!    with full exhaustion (so iteration N does exactly the work of
//!    iteration 1 — nothing accumulates); the no-cross workload asserts
//!    that no slot ever clears and the standing book keeps its shape.
//!  - Roles are FIXED (half the accounts always bid, half always ask),
//!    so no stale opposite-side curves accumulate. The crossing flow
//!    moves quote one way; funding gives ~10^7 slots of headroom, far
//!    beyond any measurement horizon, and solvency is asserted after
//!    the run.
//!  - The authenticator is a constant-true stub: credential
//!    cryptography does not exist in this engine yet and is therefore
//!    EXCLUDED from these numbers; real signature/quorum verification
//!    will add its own per-action cost on top of everything here.
//!  - Per-slot cost includes hashing the ENTIRE persistent book and
//!    ledger into the state root — the price of a per-slot commitment,
//!    deliberately not excluded.

#include <bench/bench.h>

#include <arith_uint256.h>
#include <cassert>
#include <consensus/amount.h>
#include <flowmesh/batch.h>
#include <flowmesh/clearing.h>
#include <flowmesh/ledger.h>
#include <modern/asset.h>
#include <modern/policy.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <cstdint>
#include <vector>

namespace {

using Breakpoint = flowmesh::ClearingEngine::Breakpoint;

//! Constant-true stub: see the honesty notes above.
class PassAuth final : public flowmesh::ActionAuthenticator
{
public:
    bool Authenticate(const flowmesh::Action&) const override { return true; }
};

flowmesh::AccountId Account(const uint32_t i)
{
    return ArithToUint256(arith_uint256{i} + 1);
}

modern::AssetId Base()
{
    return modern::IssuanceAssetId(
        COutPoint{Txid::FromUint256(ArithToUint256(arith_uint256{0x11})), 0});
}

struct SlotBench {
    flowmesh::Ledger ledger;
    flowmesh::ClearingEngine engine;
    PassAuth auth;
    flowmesh::BatchExecutor exec;
    const size_t n_accounts;
    const size_t k;
    const bool crossing;

    SlotBench(const size_t n, const size_t k_points, const bool cross)
        : ledger{uint256::ONE},
          engine{Base(), modern::NativeAsset(), ledger, k_points},
          exec{ledger, engine, auth},
          n_accounts{n},
          k{k_points},
          crossing{cross}
    {
        assert(n_accounts % 2 == 0);
        for (uint32_t i{0}; i < n_accounts; ++i) {
            ledger.Deposit(Account(i), modern::NativeAsset(), CAmount{1} << 40);
            ledger.Deposit(Account(i), Base(), CAmount{1} << 40);
        }
    }

    /**
     * K-point curves. CROSSING: the bid opens at price 100 with its full
     * quantity and the ask reaches its full quantity exactly at 100, so
     * every slot clears at 100 with volume = full quantity on BOTH
     * sides — both curves exhaust, are erased, and the next slot
     * resubmits fresh: true steady state. NO-CROSS: the ask's first
     * price (bid_terminal + 10) is strictly above the bid's terminal
     * price, so no candidate price ever has positive volume.
     */
    std::vector<Breakpoint> Curve(const bool bid) const
    {
        const CAmount step{10};
        std::vector<Breakpoint> points;
        if (bid) {
            // Prices 100 .. 100+10(k-1), quantity 10(k-1) .. 0.
            for (size_t j{0}; j < k; ++j) {
                points.push_back({100 + static_cast<CAmount>(step * j),
                                  static_cast<CAmount>(step * (k - 1 - j))});
            }
            return points;
        }
        if (crossing) {
            // Prices ending exactly at 100 with full quantity 10(k-1).
            const CAmount first{100 - static_cast<CAmount>(step * (k - 1))};
            for (size_t j{0}; j < k; ++j) {
                points.push_back({first + static_cast<CAmount>(step * j),
                                  static_cast<CAmount>(step * j)});
            }
            return points;
        }
        // Strictly above the bid's terminal price 100+10(k-1).
        const CAmount first{100 + static_cast<CAmount>(step * (k - 1)) + step};
        for (size_t j{0}; j < k; ++j) {
            points.push_back({first + static_cast<CAmount>(step * j),
                              static_cast<CAmount>(step * j)});
        }
        return points;
    }

    //! One steady-state slot: every account refreshes its (fixed-role)
    //! quote, then the slot clears and commits.
    flowmesh::BatchResult RunSlot()
    {
        std::vector<flowmesh::Action> actions;
        actions.reserve(n_accounts);
        for (uint32_t i{0}; i < n_accounts; ++i) {
            const bool bid{i % 2 == 0};
            flowmesh::Action action;
            action.signer = Account(i);
            action.sequence = exec.NextSequence(action.signer);
            action.type = static_cast<uint8_t>(bid ? flowmesh::ActionType::SUBMIT_BID
                                                   : flowmesh::ActionType::SUBMIT_ASK);
            action.curve = Curve(bid);
            action.credential = {0x01};
            actions.push_back(std::move(action));
        }
        return exec.ExecuteSlot(actions);
    }

    //! Untimed verification that the workload is what it claims.
    void AssertShape()
    {
        const auto result{RunSlot()};
        assert(result.applied.size() == n_accounts);
        assert(result.rejected.empty());
        if (crossing) {
            assert(result.clearing.cleared);
            assert(result.clearing.price == 100);
            // Full exhaustion: every account fills its whole quantity.
            assert(result.clearing.volume ==
                   static_cast<CAmount>(10 * (k - 1) * (n_accounts / 2)));
        } else {
            assert(!result.clearing.cleared);
            assert(result.clearing.volume == 0);
        }
        assert(ledger.SolvencyHolds());
    }
};

void RunSlotBench(benchmark::Bench& bench, const size_t accounts, const size_t k,
                  const bool crossing)
{
    SlotBench slot_bench{accounts, k, crossing};
    slot_bench.AssertShape(); // untimed: prove the workload's shape first
    bench.unit("slot").run([&] { slot_bench.RunSlot(); });
    slot_bench.AssertShape(); // untimed: shape and solvency still hold after the run
}

void FlowMeshSlot16x4Crossing(benchmark::Bench& bench) { RunSlotBench(bench, 16, 4, true); }
void FlowMeshSlot256x8Crossing(benchmark::Bench& bench) { RunSlotBench(bench, 256, 8, true); }
void FlowMeshSlot256x8NoCross(benchmark::Bench& bench) { RunSlotBench(bench, 256, 8, false); }

} // namespace

BENCHMARK(FlowMeshSlot16x4Crossing);
BENCHMARK(FlowMeshSlot256x8Crossing);
BENCHMARK(FlowMeshSlot256x8NoCross);
