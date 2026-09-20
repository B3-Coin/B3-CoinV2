# Stage 0 R1 — bounded shared-spot accounting proposal

Status: **PROPOSED; owner approval required before an isolated model begins.**
This is a documentation-only correction to §§3–6 and the first-model gate of
the frozen integrated draft `f327bd2cc334282eec17a09c1d15d04f68b7f4c5`.
It neither implements these rules nor authorizes mainnet economic parameters.
Where this companion supplies a more precise rule, that rule is the R1
candidate; the frozen draft remains historical evidence, not edited history.

Owner requirements retained: one shared spot ledger per account and exact
AssetId; real base/quote markets; atomic cross-market reservations; spot
fees of 50 ppm per side in the configured quote asset, nominal 80% FN / 20%
treasury distribution; explicit futures transfers and no implicit spot use;
futures required for full V2 release; ordinary Qt without a local validator.
The rounding, ordering, order replacement and recipient-allocation profiles
below remain proposals, even where the fee rates themselves are requirements.

## A1. Model boundary and exact identities

The first model takes an explicitly ordered synthetic execution input and
authenticated-fact stand-ins. It does **not** establish the BFT authority,
B3-finality proof, migration authority, bridge compatibility or production
custody-spend selection behind those inputs. Test fixtures identify those
stand-ins explicitly; they are never accepted as production proofs.

All AssetIds and AccountIds are exact fixed-length byte strings; ordering
means unsigned lexicographic order of canonical bytes, not ticker order or
hex-display reversal. Native B3 uses its exact zero AssetId. Equal labels
do not make assets interchangeable. Reject equal base/quote AssetIds.

An immutable market configuration binds domain/version, base and quote
AssetIds, authenticated decimals, positive integer `baseAtomsPerLot` and
`quoteAtomsPerTick`, curve limits and fee-schedule identity. Prices are
nonnegative integer ticks; quantities are nonnegative integer lots:

```
baseAtoms = lots * baseAtomsPerLot
quoteNotional = lots * priceTicks * quoteAtomsPerTick
```

Configuration changes cannot edit an old order's signed units. Changing
these fields creates a new market/configuration identity; no reverse-label
conversion is involved. Preserve the V1 uniform-price curve-auction price,
volume, imbalance and deterministic rationing rules as the starting matching
profile. Model every configured spot market, in ascending MarketId order,
once in the clearing phase; standing curves are not omitted merely because
no new instruction arrived for their market.

Model arithmetic uses exact integers plus explicit manifest bounds on
balances, lots, prices, cumulative notionals, orders and batch counts.
Fixtures must supply those bounds by name, with checked preflight rejection;
no wrap, saturation or float conversion. They are **test parameters**, not
implicit production wire widths or mainnet limits. A later codec freeze
must prove that every legal product and aggregate fits its chosen widths.

## A2. OrderId lifetime and replacement meaning

P-A2: retain at most one open order for `(account, market, side)`, matching
the baseline curve book's scope. Opening an occupied slot is rejected;
replacement is a distinct authorized instruction, not a second opening.

`OrderId = H("B3/FLOWMESH/ORDER/V2", domain, openingActionId)`, with the final
length-delimited codec a later vector-freeze obligation. The model uses
the corresponding typed tuple, not ambiguous string concatenation.
The ID is immutable and never reused. Record:

```
OrderId, account, MarketId, side, immutable fee/config identity
revision, current revision curve, revisionFilledLots
lifetimeFilledLots, lifetimeQuoteNotional T, lifetimeChargedFees C
revisionInitialNotionalBound B0, revisionActualNotionalSpent X
exact owned reservation and terminal status
```

A replacement names `OrderId` and `expectedRevision` and supplies a curve
whose quantities mean **additional remaining lots from this replacement**,
not total lifetime quantity. On success, increment revision, set
`revisionFilledLots=0`, install the new curve/bound, and preserve all lifetime
fill, notional and fee counters. Thus a ten-lot order that already filled
four and is replaced with six remaining lots authorizes at most ten lifetime
lots; replacing it with ten remaining lots authorizes at most fourteen.
Review UI must show already filled and newly authorized remaining amounts.

Replace cannot change account, market, side, units, fee schedule or OrderId.
It preflights the new bound and applies one reservation delta atomically;
failure retains the exact old curve, revision, counters and reservation.
No other market can observe a transient release. A stale expected revision
is a deterministic state rejection. Zero remaining quantity is a cancel,
not a zero-demand curve that changes auction candidate prices.

Cancellation also names OrderId and expected revision. It releases only
that order's actual residual reservation and closes the ID. Exhaustion
closes it automatically after releasing its residual cushion. Terminal
records retain lifetime counters and outcomes; no replacement reopens them.
A later distinct opening has a new OrderId and new fee accumulator. This
fresh-order dust effect is explicit in A5, not disguised as a replacement.

## A3. Persistent-curve reservation and safe residual handling

P-A3 reuses the **conservative persistent-order staircase bound**, not the
maximum possible spend in one auction. For a BUY curve with ascending
integer prices `p[0] < ... < p[k]`, nonincreasing integer quantities
`q[0] >= ... >= q[k] = 0`, and `q[0] > 0`:

```
B0 = quoteAtomsPerTick * sum(i=0..k-1, (q[i]-q[i+1]) * (p[i+1]-1))
```

Strict ascending nonnegative prices guarantee `p[i+1] >= 1`. Every lot in
cumulative positions `(q[i+1], q[i]]` can fill only below `p[i+1]`; the sum
bounds **any sequence** of fills. It is exact for a limit curve
`[(P,Q),(P+1,0)]`, but deliberately conservative for general curves.
Interpolated quantities retain V1's integer floor; no float interpolation.

For this first profile, retain the simple V1-style conservative residual:

```
N = B0 - X                    # remaining notional budget in this revision
feeBound(T,N) = ceil((T+N)*50/1_000_000) - C
BUY owned reservation R = N + feeBound(T,N)
```

At initialization `X=0`; `T,C` are zero for a new order and preserved for
a replacement. A fill of `l` lots at `p` has `x=l*p*quoteAtomsPerTick` and
`f=floor((T+x)*50/1_000_000)-C`. Preflight `x <= N`, valid residual curve
quantity and the exact owned ledger reservation. Then atomically:

```
X' = X+x; N' = N-x; T' = T+x; C' = C+f
R' = R-x-f
revisionFilledLots' = revisionFilledLots+l
lifetimeFilledLots' = lifetimeFilledLots+l
```

Because `T'+N'=T+N`, recomputation gives exactly the same `R'`. Thus the
bound cannot acquire unbacked atoms after a fill. It is intentionally
conservative: **no speculative partial-fill release** uses a tighter,
unproven one-auction or prefix-difference estimate. Cancel/exhaustion releases
all `R'`; replacement recomputes a new staircase bound and applies its exact
delta against available funds. This precisely defines residual behavior,
not a claim of optimal capital efficiency. A tighter future bound needs
a separately proved amendment and vectors.

For SELL, initial owned reservation is `maxCurveLots*baseAtomsPerLot`.
Each fill consumes its exact base amount; no quote fee is pre-reserved.
The seller's fee is deducted atomically from that fill's gross quote
proceeds. With the proposed rate below one whole unit per unit, the
cumulative fee increment cannot exceed an integer fill's gross proceeds.
Fee preflight still checks this; failed settlement invalidates the candidate,
not a partially applied user balance. Both sides' lifetime fee counters
survive replacement.

Current V1 keeps actual residual backing until cancellation/exhaustion and
resets its replacement-local fill count. R1 makes that distinction explicit
without resetting V2 lifetime quantities or fee history. Sources:
[replacement](https://github.com/B3-Coin/B3-CoinV2/blob/0b930e303e4c2c6bc28beb3bf656488b636ad49d/src/flowmesh/clearing.h#L220),
[persistent bound](https://github.com/B3-Coin/B3-CoinV2/blob/0b930e303e4c2c6bc28beb3bf656488b636ad49d/src/flowmesh/clearing.h#L596),
[residual consumption](https://github.com/B3-Coin/B3-CoinV2/blob/0b930e303e4c2c6bc28beb3bf656488b636ad49d/src/flowmesh/clearing.h#L850).

## A4. Event ordering, account sequences and duplicate handling

P-A4: every candidate has one frozen parent/configuration/committee context.
External evidence and action lists are committed in the candidate; execution
never queries a node's current tip, clock, mempool or arrival order. An
epoch/configuration transition is a separate boundary, not a mid-batch fee
recipient or authorization switch. Apply these phases in order:

1. Validate the proposed anchor/import range and all included external facts
   against the consensus companion's pure predicates. Import recognized
   withdrawal settlements in `(B3 height, transaction position, effect
   position, receiptId)` order. Import any approved risk observations as
   committed external events; the spot-only model has no enabled economic
   risk profile and cannot manufacture oracle authority.
2. Credit eligible deposits in `(B3 height, transaction position, output
   index)` order. The output's authenticated custody version/domain fixes
   its account, AssetId and amount; a user cannot supply alternative credit
   fields. Sweep/pool consolidation is never another deposit credit.
3. Execute user instructions in ascending `(AccountId, sequence)` order.
   **Transfers, opens, replacements, cancels and withdrawal requests share
   this one stream; no action type jumps ahead of another signed sequence.**
   Thus a cancel followed by withdrawal can use released reserve; an open
   followed by withdrawal cannot use its reserved funds. A withdrawal in
   this phase cannot spend proceeds from this batch's later clearing.
4. Clear every configured spot market in ascending MarketId order using its
   now-current standing curves; preflight and atomically settle fills,
   reservations and fee debits. Newly received base/quote cannot retroactively
   fund an instruction already rejected in phase 3. A subsequent batch's
   authorized instruction may spend it.
5. Allocate collected spot fees by A5, finalize all outcomes/component roots,
   and require every conservation/reservation invariant before accepting
   the candidate. No market-level partial commit is allowed if a later
   market or invariant fails.

First validate canonical shape, domain, authority and signature. Invalid
ones are refused without account sequence consumption; if an invalid one
is embedded as an executable candidate instruction, reject that candidate,
not partially execute it. After authentication, collapse identical semantic
ActionIds (including equivalent valid authentication wrappers).

For one account/sequence, distinct authenticated semantic actions in the
same candidate are an equivocation group: execute none, consume no sequence,
and record the group refusal. An unauthenticated conflicting object cannot
veto an authenticated one. This is a proposed precise version of V1's
[grouping and sequence treatment](https://github.com/B3-Coin/B3-CoinV2/blob/0b930e303e4c2c6bc28beb3bf656488b636ad49d/src/flowmesh/batch.h#L444).

For each remaining instruction:

- If its ActionId already has a consumed-sequence outcome, return that exact
  outcome with no economic or sequence mutation, even when it was a rejection.
- A previously consumed sequence naming different content is rejected as
  conflicting/replayed content, never as a replacement.
- A future sequence with a gap is `BAD_SEQUENCE`, not consumed or certified
  as a terminal economic outcome. The identical instruction may be considered
  after the gap resolves under the existing retry rules; never auto-re-sign.
- Exactly the expected sequence consumes one sequence when its authorized
  transition succeeds **or** deterministically fails on state (insufficient
  funds, stale revision, closed order, unavailable risk profile, capacity).
  State rejection changes only the sequence and immutable outcome record.
- No wrap at the sequence bound: an account at the manifest limit cannot
  execute another instruction. A resource/codec bound is not bypassed by
  arithmetic saturation.

A deposit's replay identity is `(sourceChainDomain, custodyVersion,
custodyDomain, originalOutpoint)`, separate from user sequence. Identical
facts after credit return the original credit record. Contradictory facts
for one identity fail evidence validation; do not choose first arrival.
A payout import similarly keys its certified receipt/nullifier and must
match asset, amount, destination and custody authority exactly. Duplicate
import is idempotent; inconsistent settlement evidence rejects the candidate.
Explicit transfers have one semantic ActionId and one atomic transition;
there are no independently replayable debit and credit legs.

## A5. Proposed fee grouping, historical authority and remainder allocation

P-A5: each order, each side charges `floor(T*50/1_000_000)` cumulatively.
The incremental fill fee is the difference of cumulative floors, as A3.
No minimum fee; no fractional atom becomes a collectible liability at close.
Do not assign batch-largest-remainder rounding atoms to individual orders:
that can exceed a repeatedly partially filled order's reserved fee bound.

After all markets clear, group spot fee atoms by the **exact quote AssetId**
and frozen `(execution domain, committee epoch, fee schedule, recipient
authority context)` of this batch. There is one such context per batch.
Different markets sharing that quote/context aggregate **before** treasury
rounding; market attribution is retained as audit data, not another payout.
Never group different AssetIds, different epochs or future instrument fees.

For each group with integer total `F`:

```
treasury = floor(F*20/100)
fnPool = F-treasury
seatBase = floor(fnPool/N)
seatExtra = fnPool mod N
```

P-A5 chooses equal shares among **all seats in the authenticated historical
committee for that batch**, not online peers or only certificate signers.
Sort unique SeatIds by canonical bytes; the first `seatExtra` receive one
extra atom. Invalid/empty recipient authority makes the candidate invalid;
there is no fallback to the proposer. Committee size/quorum is supplied by
the consensus profile and is not changed by this allocation rule.
No fractional treasury carry across batches is proposed here. This creates
batch-size dust effects and small systematic seat-order remainder effects;
those are disclosed economic choices requiring explicit approval, not exact
per-batch fractional 80/20 payouts.

Reward balances key `(domain, epoch, recipient-context-hash, SeatId,
historical BLS public key, AssetId)`. Historical reward authorization stays
with that committed BLS key under the proposed V1-compatible claim-authority
principle; owning a later rebound FN does not redirect old claims. A claim
is separately authenticated against that historical context and carries its
own exact AssetId and anti-replay identity. It cannot mint a reward or erase
a pending payout. A future transfer of reward authority needs a separately
approved signed rule. Treasury balances bind the frozen treasury owner
commitment, AssetId and fee context; current operator configuration cannot
redirect them. See baseline [recipient identity](https://github.com/B3-Coin/B3-CoinV2/blob/0b930e303e4c2c6bc28beb3bf656488b636ad49d/src/flowmesh/fee_allocation.h#L35)
and [seat remainder ordering](https://github.com/B3-Coin/B3-CoinV2/blob/0b930e303e4c2c6bc28beb3bf656488b636ad49d/src/flowmesh/fee_allocation.h#L313).

The model may represent these as protocol ledger balances with explicit
authorization predicates; they are not simultaneously ordinary spot balances
and additional liabilities. Payout request moves a reward/treasury balance
into a pending custody claim, and settlement retires that claim once.
Network transaction fees remain B3 and are outside this spot trading fee.
The first model does not construct payout transactions or assume a free
network fee: it takes a separate external B3 fee-funding fact where modeled.
No token or recipient amount is silently reduced to pay a B3 fee.

Authorized protocol payout requests occupy phase 3's same canonical signed
stream. Their typed, domain-separated reward/treasury AccountId has its own
sequence and the historical authorization predicate just defined; it is
not a user's ordinary account with an interchangeable credential. A claim
cannot spend fees collected later in this batch's phase 5. This isolates
claim authority without introducing an arrival-ordered privileged payout
phase or an independently replayable withdrawal leg.

## A6. Conservation, withdrawal capacity and V1 separation

For each exact AssetId `a`, maintain recognized custody `C[a]` at the
committed import cursor:

```
C[a] = imported new deposits[a] - imported settled payouts[a]
C[a] = sum(spotAvailable[a]) + sum(orderOwnedReservations[a])
     + sum(backedFuturesCash[a]) + sum(protocolBalances[a])
     + sum(pendingWithdrawalLiabilities[a])
```

The initial fixture state must prove the same equality against its declared
synthetic funding provenance. Reservation aggregates equal the sum of their
owned entries. No account/asset field can go negative; contractual PnL and
unfunded claims are separate signed records, not negative spot or minted cash.
Trade settlement moves base from seller reserve to buyer available, quote
notional from buyer reserve to seller available less seller fee, and both
quote fees to protocol accounts. No cross-AssetId conversion satisfies this
equation. Fees/rewards create ownership transfers, not new custody.

A withdrawal requests only available spot (or an authorized protocol payout
balance), moving it into a pending liability with exact destination and one
receipt identity. It is not yet a B3-authorized payout. The isolated model
also checks `existingPending[a] + requested <= anchoredCapacity[a]` against
an explicitly supplied **synthetic external-capacity fact**. A later smaller
capacity blocks new requests; it never cancels existing liabilities.
Actual V2 bounded-input selection/fragmentation proofs remain a custody
integration gate, not qualified by this capacity stub. V1 precedent:
[pending admission](https://github.com/B3-Coin/B3-CoinV2/blob/0b930e303e4c2c6bc28beb3bf656488b636ad49d/src/flowmesh/ledger.h#L241)
and [anchored capacity](https://github.com/B3-Coin/B3-CoinV2/blob/0b930e303e4c2c6bc28beb3bf656488b636ad49d/src/node/flowmesh_vault_index.cpp#L402).

Settlement import decreases both `C[a]` and that exact pending liability.
Broadcast-unknown or an absent response does neither. At a physical chain
tip later than the import cursor, deposits not yet imported and payouts
already settled but not yet imported are separate reconciliation buckets.
The model cannot count those as additional available balance or silently
equate recognized custody with the current pool UTXO total.

V1 ledgers, vaults and pending claims remain separate preserved namespaces.
No balances snapshot, old reservation release on a local copy, or V2 startup
credits V2. Under the proposed migration profile, an actual authorized V1
payout settles once, then a separately authorized V2 deposit credits once.
The first model tests these accounting fences, not the validity or authority
of a live cutoff/migration proof. Old split-locked funds remain liabilities
of their old rules and do not fund this model's new balances.

## A7. Explicit futures interface; undefined risk means rejection

Futures remains mandatory for the complete V2 release, not implemented by
this first model. The proposed transfer envelope binds domain/version,
account sequence, AccountId, explicit futures subaccount, exact collateral
AssetId, positive amount, action kind and expected configuration identity.
An approved authorization registry must bind the subaccount to that account;
an arbitrary destination identifier is not authority. The model's generated
fixtures supply that binding explicitly and permit no delegation by default.

- `SPOT_TO_FUTURES`: require authorized source/subaccount, enabled configured
  collateral and risk profile, available spot amount and permitted transfer.
  Atomically debit spot available and credit **backed cash** in that futures
  subaccount; do not touch any spot reservation.
- `FUTURES_TO_SPOT`: require the same authority plus a deterministic result
  from the committed risk profile, covering positions, open-order exposure,
  margin, fees, funding, liquidation/deficit state and oracle validity.
  Transfer at most its explicitly computed withdrawable backed cash.
- A deterministically committed `NOT_DEFINED`/unapproved profile,
  unsupported collateral, committed `UNKNOWN` risk result, stale valuation,
  insufficient cash or deficit blocking withdrawal gives a typed state
  rejection and the sequence treatment in A4. There is no default `PASS`,
  zero-margin assumption or automatic oracle.
- A replica that lacks required candidate body, parent state or proof
  witnesses must instead `DEFER` and obtain the missing data/catch up. Its
  local absence is not a committed risk result and cannot authorize a
  certified rejection or account-sequence consumption. Only deterministic
  execution over the complete authenticated input can establish the preceding
  state-rejection conditions.

The model may test mechanics with a **TEST-ONLY synthetic risk adapter**
that has fixed, named PASS/DENY/UNKNOWN fixtures. Passing such a fixture
does not select leverage, validate collateral safety or authorize any real
futures transfer. Include negative tests in which positions, funding, fees
and losses attempt to call a spot debit: all fail absent an explicit signed
transfer. No general futures callback receives unrestricted spot-ledger
mutation authority.

Still unapproved: perpetual/dated/inverse contract, isolated/cross margin,
multiplier, margin tiers/leverage, realized versus unrealized cash accounting,
funding interval/rate/rounding, oracle/mark sources and freshness, futures fee
schedule, liquidation execution/incentives, insurance funding, deficit/ADL
priority and exact withdrawability predicate. These block futures work and
full V2 release; the first model must not fill them with market conventions.

## A8. Worked adversarial vectors (all amounts are test atomic units)

These are specified expected results, **not newly executed implementation
tests**. Unit multipliers are one unless stated; no mainnet defaults are
selected by these numbers.

| Case | Input and exact expected result |
|---|---|
| Two markets share one quote | Available 100,000. Open A needs 60,000 notional + 3 fee; reserve 60,003, leave 39,997. Open B needing the same fails, consumes its expected sequence, and cannot borrow A's reserve. Reverse signed sequence chooses B first; private arrival order cannot. |
| Order versus withdrawal | With the same funding, open 60,003 first prevents a 60,000 withdrawal. Withdrawal 60,000 first leaves 40,000 and prevents the open. Pending plus available/reserved remains exactly 100,000; no duplicate spend. |
| Deposit and same-batch order | Imported eligible deposit 100 precedes phase-3 order requiring 60; it succeeds, available 40. A deposit absent from certified evidence cannot fund the order. A later clearing receipt likewise cannot retroactively fund phase 3. |
| Partial limit fill then cancel | Ten lots at 10,000 reserve 100,005. First one-lot fill has fee 0: `T=10,000,C=0,N=90,000,R=90,005`. Second has fee 1: `T=20,000,C=1,N=80,000,R=80,004`. Cancel releases 80,004. If started with 100,005 total, available ends 80,004; buyer paid 20,001. Seller got 19,999 and protocol fees 2 across those fills. |
| Replacement quantity | After four of ten lots fill, replacement with six means six additional, maximum lifetime ten. Replacement with ten means ten additional, maximum fourteen. Lifetime T/C do not reset; stale revision or insufficient delta leaves the original six-lot residual order and all its counters unchanged. |
| Persistent curve beats one-auction maximum | BUY `[(0,100),(100,0)]`, `q(p)=100-p`. One-auction maximum is 2,500. Fill 25 at 75 (1,875), then 25 at 50 (1,250): total 3,125, so 2,500 is unsafe. Staircase bound is `100*99=9,900`; initial fee cushion 1. Residual R is 8,026 then 6,776 with C=0. Cancel releases 6,776, exactly `9,901-3,125`. It is intentionally not aggressively tightened. |
| Replacement preserves fee dust | Fill 10,000 notional: cumulative fee 0. Replace same OrderId, then fill another 10,000: fee increment 1. Resetting the accumulator would incorrectly charge 0 on the second fill. A fresh OrderId after cancel really does start at zero under this proposed dust policy. |
| Split-order incentive | Ten partial 10,000 fills of one order charge total 5. Ten distinct one-fill orders of 10,000 each charge total 0. Disclose this before A5 approval; do not silently add a minimum fee. |
| Rejected batch-largest-remainder alternative | Two orders each reserve fee 5 for total notional 100,000. Assigning the same order one rounding atom in each of ten 10,000 partial-fill batches charges it 10 and violates its bound. Cumulative-order floors charge it 5 instead. |
| Fee grouping | Same quote/context markets collect 4 and 1 atoms in one batch: F=5, treasury=1, FN=4. Per-market rounding would instead give treasury=0 and FN=5 and is not this proposal. If these are different AssetIds, they remain separate groups; no conversion to one USD bucket. |
| Seat remainders | Group F=13: treasury=2, FN=11. Four historical seats in byte order get 3,3,3,2. All recipients and totals are retained after FN rebinding. No payout depends on who appears online. |
| Different decimals | Base has 9 decimals, quote 6. `baseAtomsPerLot=100,000,000` (0.1 base), `quoteAtomsPerTick=1,000` (0.001 quote/lot), price 250, two lots: base 0.2 and quote 0.5, i.e. 2.5 quote/base. Notional 500,000 quote atoms; each fee 25 quote atoms. BUY reserves 500,025; SELL reserves 200,000,000 base atoms and receives 499,975 quote atoms. No display-unit rounding changes any debit. Repeat with 8/0 decimals. |
| Duplicate deposits/transfers | Original outpoint credits 100 once. Repeated identical proof and later sweep leave custody/available at 100. Explicit transfer 40 leaves spot 60/futures 40; the same ActionId repeated leaves 60/40. A new sequence is not a retry of the old transfer. |
| Unknown withdrawal | Available 40 becomes pending 40 on request. Lost submit/payout response cannot restore available. Matching finalized settlement consumes pending and custody once; duplicate import does nothing. Wrong destination or amount fails evidence validation. |
| Futures shortfall | Counterparties fund 100 cash each. Contractual PnL +150/-150 does not create 250 backed cash for the winner by flooring loser cash at zero. Preserve the 50 deficit/unpaid claim separately; absent approved allocation/funding, transfer-out fails. The loser's separate spot funds are not available to the risk engine. |
| V1 namespace fence | V1 custody 100 consists of available 40, reserved 30, pending payout 30. V2 receives no 100- or 40-atom snapshot credit. Only an independently proven new V2 deposit after its actual funding movement credits V2. Old pending 30 remains authoritative despite uncertainty. |

## A9. Compact accounting approval sheet

Approval here would authorize **only a separate isolated model stage**, not
production code, consensus, a deployed bridge assumption or the full release.

| ID | Proposed bounded choice to approve or amend | Consequence |
|---|---|---|
| A-D1 | One global exact-AssetId ledger and the five-phase A4 order; all configured markets clear by MarketId after signed actions | Deterministic contention; no spending same-batch clearing proceeds in earlier instructions. |
| A-D2 | One open curve per account/market/side; immutable OrderId; replacement quantities are additional remaining lots with expected revision | No ambiguous total-versus-residual quantity; lifetime fee/fill history survives replacement. |
| A-D3 | A3 staircase bound plus conservative initial-bound-minus-actual-spend residual, release on cancel/exhaustion or successful replacement delta | Safe persistent fills; potentially excess locked capital rather than unproved early release. |
| A-D4 | Cumulative per-order fee floors; per-batch exact-quote/context grouping; treasury floor/no carry; equal historical seats with byte-order dust | Explicit split-order/batch dust incentives and historical authority; not a claim of exact fractional 80/20. |
| A-D5 | A4 sequence/duplicate/equivocation rules, including state-rejection consumption and nonterminal sequence-gap refusal | Exact-action recovery without replacement instructions or duplicate credits. |
| A-D6 | Segregated V1 namespace and settled-payout/new-deposit migration accounting; capacity/risk/authority fixtures visibly synthetic | Accounting tests only, no live migration, actual withdrawal proof or BFT authority inferred. |
| A-D7 | Explicit futures transfer envelope and fail-closed risk interface; only labelled synthetic adapter in this stage | Preserves required futures boundary without silently choosing economics or claiming futures complete. |

No real fee recipients, activation height, asset registry, margin/oracle
parameters or production amount limits are selected by this sheet. Unapproved
rows block the corresponding model behavior; approval must identify the
reviewed document revision and any amendments.

## A10. Proposed model acceptance tests

1. Execute every A8 vector and boundary variants (zero, one atom, maximum,
   overflow, same ticker/different AssetId, B3 as base, 9/6/8/0 decimals).
2. After every action, clearing and import, assert A6 conservation per asset,
   exact reservation ownership, nonnegativity and no duplicate liability.
   Randomize input arrival; canonicalized identical committed inputs must
   replay to identical state and outcome roots.
3. Enumerate small valid BUY curves and multi-batch legal fill paths. Prove
   actual prefix spend never exceeds B0 and A3 recurrence never underbacks;
   include the unsafe one-auction-bound implementation as a failing control.
4. Compete orders across markets with cancels, replacements, transfers and
   withdrawals; failures consume only the permitted sequence/outcome and
   never release another order's reserve. Interrupted staged transitions
   leave either the old or the whole new model state, not one transfer leg.
5. Test fee telescoping across fills/replacements, fresh-order dust,
   grouping/remainder ordering, empty/changed recipient context, historical
   key rebinding, wrong-asset rewards and double-counted protocol balances.
6. Test exact and conflicting duplicate ActionIds, equivalent authentication
   wrappers, invalid-signature equivocation attempts, sequence gaps, stale
   revisions, sequence limits and already-certified no-resubmit records.
7. Test duplicate/out-of-order import, insufficient/fragmented synthetic
   capacity, capacity shrink with old pending claims, payout-unknown,
   incorrect settlement facts and V1/V2 duplicate-credit fences. Label
   capacity selection and migration authority as unqualified integration.
8. Test permitted synthetic spot/futures transfers, every DENY/UNKNOWN
   condition, unsupported subaccount/asset and attempted loss/funding/fee
   spot debits. PnL claims must not increase C or backed cash. No test adapter
   is a default economic profile.
9. Serialize/reopen the same generated model state and replay suffixes;
   compare all balances, reservations, counters, outcomes, deposit identities,
   pending/consumed receipts and historical reward authority. This qualifies
   model persistence only, not power-loss-safe production signing journals.

A future passing model report must name its exact commit, approved profile,
synthetic parameters, failing controls and remaining limits. It must not be
presented as BFT, bridge, live migration, actual futures or full-V2 validation.
