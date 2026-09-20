# Stage 0 R1 — preservation, routing and remaining futures gates

Status: **PROPOSED; owner approval required. Documentation only.** This appendix
is part of the integrated Stage 0 R1 candidate, not migration authorization.
The original freeze is `f327bd2cc334282eec17a09c1d15d04f68b7f4c5`.

## T1. Authority is not a local snapshot

Use the explicit bootstrap/cutover authority and verification predicate in
[`v2-stage0-r1-consensus.md`](v2-stage0-r1-consensus.md). A proposed preservation
manifest is inert until the specified outgoing authority certifies its exact
hash, historical anchor/prefix, initial V2 context and activation conditions.
The new FN committee cannot authorize itself. `H` is a symbolic, **UNSET**
future activation parameter, not a height selected by this document.

The manifest records, for each V1 market, its exact configuration/custody
domain, authenticated certified prefix, on-chain checkpoint/effect cursor,
and the evidence needed to reproduce that prefix's liabilities. A certified
head is evidence of that prefix, not proof that no later certificate or
unknown signed instruction exists. Later valid V1 certificates and withdrawals
retain their original authority; the manifest neither revokes nor replaces them.

Preservation counts and roots must be independently reproducible against the
authenticated prefix, including available/reserved balances, filled quantities,
fee/reward rights, credited deposit outpoints, pending withdrawals, consumed
receipts and settlement-import cursors. Unknown or conflicting evidence is
recorded explicitly and cannot be silently replaced with zero. A required
unauthenticated manifest component prevents bootstrap acceptance under the
consensus appendix; it does not authorize copying a local balance estimate.

Client outboxes and signing journals remain private local durable records, not
contents published in this manifest. Publish only necessary public commitments
and verifiable protocol evidence. Retain local exact bytes, ActionIds, nonces,
unknown outcomes, no-resubmit records and signing history under V1 domains.

## T2. Proposed ingress and version routing

This is a new **D10/T2 choice**, not an assertion about existing V1 behavior.
The activation predicate must bind `H` and these routing rules before any
implementation or production selection. Version classification is by exact
transaction/output grammar, domain/configuration and inclusion height; never
by ticker, selected endpoint, wallet version, transaction creation time or
the client's local clock.

| Evidence/action | Proposed routing |
|---|---|
| V1 custody deposit confirmed below H | Its sole ingress authority remains V1, even if credit is requested after H or was absent from the preservation prefix. |
| Attempt to create a new V1 custody deposit at or above H | Reject that new deposit under the approved boundary rule; an earlier mempool/broadcast time is not an exception. Ordinary valid outputs are not classified as custody deposits by address labels. |
| V2 custody deposit below H or before the authorized V2 bootstrap is effective | Reject; a fresh binary or local initial state is insufficient authority. |
| Valid V2 custody deposit after both gates | Credit V2 spot only after its normal finalized-anchor/evidence requirements, once per original ingress identity. |
| Existing V1 order, reservation, certified effect or pending claim | Keep original V1 validation and custody authority; no silent cancel, conversion or duplication. |
| Old withdrawal broadcast before H, included after H | Validate its original receipt, exact destination/amount, custody and one-time consumption under its applicable historical rules. |
| Unconfirmed/reorged V1 deposit near H | Reconcile the original transaction and canonical inclusion. No confirmed deposit means no credit. If its later inclusion is forbidden, preserve the rejection/unknown record; never create a replacement automatically. |

The hard-fork grammar must distinguish **new V1 user deposits** from valid
V1 settlement change, pool maintenance/sweeps and existing custody payouts.
Forbidding the former must not accidentally prohibit the latter. Exact byte
dispatch and valid/invalid boundary blocks remain a later codec/integration
gate; this table supplies the proposed semantic rule, not executable consensus.

V1 action validity is not rewritten by calling its service a "claim service."
If continuing V1 rules allow further fills or other actions, those outcomes
remain V1 outcomes. Updated clients should route new ordinary trading to V2
after authorized activation and expose V1 claim/recovery separately. Preventing
all new V1 off-chain trading would need an additional explicit rule; UI routing
alone does not cryptographically disable it. No such rule is smuggled in here.

## T3. No snapshot-based balance migration

Recommended D10 remains: valid V1 withdrawal settles to its original owner
destination, followed by a separately owner-authorized V2 deposit. These are
two independently tracked operations, not one automatic transfer and not an
implicit authorization to change destination or sign a new instruction.

Per version and exact AssetId, track independently evidenced physical custody,
recognized liabilities and explicit reconciliation differences. A later V1
certificate is never replayed as a V2 deposit. Original deposit outpoints and
withdrawal nullifiers remain valid in their original custody domains; V2's
new deposit is recognized only against new, separately locked custody.

Worked preservation vector, illustrative atoms:

1. V1 custody is 100: available 40, reserved 30, pending withdrawal 30.
   V2 spendable credit is **zero**. Neither the 100 total nor the 40 available
   is an authorized V2 opening balance.
2. The original withdrawal settles 30 to its fixed destination. Physical V1
   custody becomes 70. Until V1 imports the settlement, its 30-atom discrepancy
   is an explicit settled-not-imported bucket, not reusable custody.
3. Only a separately authorized, confirmed/finalized V2 deposit of those 30
   can back V2 credit of 30. The original V1 withdrawal can never consume again.
   Old reserved 30 and available 40 remain entirely in V1.
4. If settlement is unknown, none of steps 2 or 3 may be inferred from a
   timeout. If a V1 market is split-locked, record blocked rights; do not
   manufacture a settlement or V2 allocation.

Direct in-protocol V1-to-V2 migration remains excluded. It would require a
new audited atomic authority/custody/nullifier rule and separate approval.

## T4. Required preservation/boundary tests (not executed here)

- Competing preactivation branches; identical old finalized checkpoint but
  different inclusion-dependent snapshots; only the specified bootstrap proof
  can authorize V2. Restart must reconstruct the same context and routing.
- Old confirmed but uncredited deposits arriving after H; deposits exactly
  on each side of H; mempool-before/inclusion-after; canonical reorg and duplicate
  sweep evidence; no credit in both versions.
- Late old certificates beyond the preserved prefix; still-valid withdrawal
  proofs; uncertain broadcasts; duplicate claim consumption; no snapshot erasure.
- V1 pool change and settlement inputs remain distinguishable from prohibited
  new user deposits. Engine-off and enabled nodes validate identical blocks.
- Concurrent pending claims and physically settled/import-lagged custody;
  different exact AssetIds with identical tickers; all reward beneficiaries
  retain historical authority, not the current holder's assumed identity.

## F1. Futures is still a required release component

The shared-spot model is an implementation-order boundary, not a spot-only
release definition. The following choices remain **UNAPPROVED**:

| Decision | Still required before futures implementation |
|---|---|
| Contract/profile | Linear perpetual proposal versus alternatives; quantity/multiplier, collateral and quote AssetIds; position netting and isolated-account scope. |
| Margin | Initial/maintenance tiers, leverage limits, open-order exposure, rounding, maximum amounts and exact withdrawable-collateral predicate. |
| Oracle/mark | Sources, authenticated authority and independence, aggregation, freshness, price bounds, rotation and outage rules. |
| Funding | Formula, scheduling, bounded catch-up, debit/credit rounding and unpaid funding treatment. |
| Fees | Quote-asset-only requirement remains; actual futures rates/distribution and any backstop levy require explicit approval. |
| Liquidation | Eligibility, ordering, auction/takeover mechanics, partial reduction, incentives and settlement accounting. |
| Backstop/deficit | Actual fund source and reserve authority, priority of losses/claims, halt scope and recovery; no presumed ADL, haircut or implicit spot recourse. |

Unrealized or unpaid derivative PnL is not backing. Example: two counterparties
deposit 100 collateral each, then contractual PnL is +150/-150. Paying the
winner 250 while flooring the loser at zero creates 50 unfunded atoms unless
an explicitly authorized funded source supplies them. A loser's separate
50 spot atoms are not that source. Record the deficit and constrain withdrawal
under approved rules; do not invent those rules during model implementation.

The first model may test only the explicit authorized transfer coordinator,
with a clearly synthetic collateral policy and evidence interface. Undefined
instrument, risk or oracle checks return NOT_DEFINED and leave economic state
unchanged. There is no bypass that converts PnL into withdrawable spot funds.
No futures order, funding, fee, loss or liquidation handler receives authority
to debit spot. The exact model scope is in the accounting appendix.

## Source provenance

References concern the inspected published V1 baseline, not V2 implementation:

- [V1 ledger and pending withdrawals](https://github.com/B3-Coin/B3-CoinV2/blob/0b930e303e4c2c6bc28beb3bf656488b636ad49d/src/flowmesh/ledger.h#L241).
- [V1 deposit replay tracking](https://github.com/B3-Coin/B3-CoinV2/blob/0b930e303e4c2c6bc28beb3bf656488b636ad49d/src/flowmesh/state.h#L253).
- [V1 settlement import](https://github.com/B3-Coin/B3-CoinV2/blob/0b930e303e4c2c6bc28beb3bf656488b636ad49d/src/flowmesh/batch.h#L368).
- [V1 checkpoint validation](https://github.com/B3-Coin/B3-CoinV2/blob/0b930e303e4c2c6bc28beb3bf656488b636ad49d/src/modern/flowmesh_checkpoint.h#L485).

Public source links are commit-pinned. This R1 design package is local until
separately published; no claim is made that its revision is already on GitHub.
