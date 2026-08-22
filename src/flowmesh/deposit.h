// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_DEPOSIT_H
#define B3COIN_FLOWMESH_DEPOSIT_H

#include <consensus/amount.h>
#include <flowmesh/ledger.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <optional>

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
