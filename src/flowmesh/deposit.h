// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_DEPOSIT_H
#define B3COIN_FLOWMESH_DEPOSIT_H

#include <consensus/amount.h>
#include <flowmesh/ledger.h>
#include <flowmesh/settlement.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace flowmesh {

/**
 * The B3 position a FlowMesh state builds relative to: a block hash at a
 * height on the base chain that the anchoring rules accept as final
 * enough. WHAT is acceptable — the finality depth, and behavior across a
 * base-chain reorg — is an OWNER DECISION (OD-6) enforced by whoever
 * supplies/validates anchors; this struct only names the position.
 */
struct AnchorRef {
    int32_t height{-1};
    uint256 hash;

    SERIALIZE_METHODS(AnchorRef, obj) { READWRITE(obj.height, obj.hash); }

    bool IsNull() const { return height < 0 && hash.IsNull(); }
    friend bool operator==(const AnchorRef& a, const AnchorRef& b)
    {
        return a.height == b.height && a.hash == b.hash;
    }
};

//! The canonical facts of one recognized B3 DEX_VAULT deposit, as
//! established from the chain — never from any caller-supplied claim.
struct DepositInfo {
    AssetId asset;
    CAmount amount{0};
    AccountId account;
};

/**
 * One bounded chain-settlement step. `anchor` is the furthest canonical
 * block boundary that can be retired by one production entry and `count` is
 * the exact number of market withdrawals in that interval.
 */
struct WithdrawalSettlementPlan {
    AnchorRef anchor;
    size_t count{0};
};

/**
 * Custody-side verifier for deposits. Implementations answer from
 * canonical B3 chain data as of `anchor`: the outpoint must exist, be a
 * DEX_VAULT deposit output for this domain's vault, and satisfy the
 * approved finality rule at that anchor. The asset, amount and
 * beneficiary account in the result come from the CHAIN (the deposit
 * output and its account binding — the binding rule is an OWNER
 * DECISION); a FlowMesh action only ever names the outpoint.
 *
 * CONTRACT: deterministic — for one (outpoint, anchor) every honest node
 * with the anchored block available must return the same result.
 */
class DepositVerifier
{
public:
    virtual ~DepositVerifier() = default;
    virtual std::optional<DepositInfo> GetDeposit(const COutPoint& outpoint,
                                                  const AnchorRef& anchor) const = 0;

    /**
     * Spendable withdrawal capacity for `asset` at this exact B3 anchor:
     * the sum of the largest 64 live pool-change outputs. A null value means
     * the chain fact is unavailable and withdrawal admission must fail
     * closed. Implementations used by tests must opt into a capacity
     * explicitly; the safe default never invents liquidity.
     */
    virtual std::optional<CAmount> GetWithdrawalCapacity(
        const AssetId& asset, const AnchorRef& anchor) const
    {
        return std::nullopt;
    }

    /**
     * Exact, market-bound type-9 withdrawals connected after the previous
     * production anchor and through the proposed anchor, sorted strictly by
     * receipt id. A null result means the chain fact is unavailable and must
     * fail production closed; an empty vector is a proved empty interval.
     */
    virtual std::optional<std::vector<WithdrawalSettlementFactV1>>
    GetWithdrawalSettlements(
        const std::optional<AnchorRef>& after_exclusive,
        const AnchorRef& through_inclusive) const = 0;

    /**
     * Select a deterministic, bounded settlement anchor no later than
     * `through_inclusive`. Production implementations override this with a
     * count-only history scan so an offline backlog cannot force allocation
     * of every pending receipt. The default keeps lightweight test verifiers
     * source-compatible while enforcing the same per-entry bound.
     */
    virtual std::optional<WithdrawalSettlementPlan>
    PlanWithdrawalSettlements(
        const std::optional<AnchorRef>& after_exclusive,
        const AnchorRef& through_inclusive) const
    {
        const auto settlements{
            GetWithdrawalSettlements(after_exclusive, through_inclusive)};
        if (!settlements ||
            settlements->size() >
                FLOWMESH_MAX_WITHDRAWAL_SETTLEMENTS_PER_ENTRY) {
            return std::nullopt;
        }
        return WithdrawalSettlementPlan{through_inclusive,
                                        settlements->size()};
    }
};

/**
 * Anchor acceptability, isolated: WHAT counts as an acceptable B3
 * position (finality depth, reorg handling) is an OWNER DECISION (OD-6);
 * validators consult this interface and node-side implementations read
 * real chain state. `Current()` is the anchor a proposer should use now.
 *
 * CONTRACT: Acceptable must be deterministic for nodes sharing the same
 * B3 chain view; disagreement across honest nodes is bounded by the
 * chosen finality depth — exactly the risk parameter the owner sets.
 */
class AnchorPolicy
{
public:
    virtual ~AnchorPolicy() = default;
    //! Acceptable for a NEW proposal (canonical + buried to the
    //! approved finality depth).
    virtual bool Acceptable(const AnchorRef& anchor) const = 0;
    //! Still on the canonical B3 chain at all (depth-free): the test a
    //! COMMITTED anchor must keep passing. An anchor relied on by
    //! certified FlowMesh history that stops being canonical must halt
    //! unsafe FlowMesh progression — how deep-reorged history is then
    //! treated is an OWNER DECISION; halting is the fail-safe floor.
    //! A null anchor references no B3 state and trivially passes.
    virtual bool StillCanonical(const AnchorRef& anchor) const = 0;
    //! The anchor a proposer should use now.
    virtual AnchorRef Current() const = 0;
};

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_DEPOSIT_H
