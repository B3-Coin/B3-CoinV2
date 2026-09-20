# B3 PoS V2 + FlowMesh V2 — integrated Stage 0 specification

Date: 2026-09-20. Baseline: `0b930e303e4c2c6bc28beb3bf656488b636ad49d`.
Status: **Stage 0 R1 corrected specification candidate; PROPOSED, not
ratified, implemented or activation-ready**. Original review freeze
`f327bd2cc334282eec17a09c1d15d04f68b7f4c5` remains unchanged in Git and on
`design/flowmesh-v2-stage0`; R1 is a separate design revision. The original
read-only audit remains unchanged; its nine findings are tracked in the
[R1 disposition and review package](v2-stage0-r1-review.md).

This document plus the following explicit normative proposal appendices form
ONE integrated candidate. They refine the corresponding sections below;
they are not implemented alternatives or permission to choose rules at runtime:

- [Accounting rules, worked vectors, bounded decisions and proposed tests](v2-stage0-r1-accounting.md).
- [PBFT-style consensus, pure validation and authority/recovery rules](v2-stage0-r1-consensus.md).
- [Read-only bridge verification and exact outstanding evidence](v2-stage0-r1-bridge.md).
- [Preservation/cutover routing and remaining futures gates](v2-stage0-r1-transition.md).

All newly selected candidate rules remain **PROPOSED** until the owner approves
their bounded decision set. Conflicts between the integrated text/appendices
are review failures, not implementation discretion. Exact production codecs,
bounds and any still-unapproved economics remain gated. No mainnet height is
selected; the first accounting model has NOT begun.

## 0. Authority, scope and terminology

**R** denotes an owner requirement. **P** denotes a proposed design choice,
not silently approved by this document. **F** denotes a source-verified V1
fact. **G** denotes a blocking implementation/release qualification gate.
“Must” in a proposed profile means required *if that profile is approved*.
It does not grant deployment authority.

The owner's 2026-09-20 workflow instruction supersedes the old Claude-only
implementation and blanket no-hard-fork restrictions for V2 design. Codex
may implement later approved stages; each frozen stage needs a separate
read-only audit of source and adversarial cases before progression. No live
height, contract replacement, deployment, funds movement, forced rearming,
journal reset, quorum reduction or production migration is authorized.

R: one coordinated program includes B3 PoS V2 and FlowMesh V2 **spot and
futures**. Spot-first is build order, never a spot-only V2 release claim.
Preserve V1 history and the deployed Ethereum bridge. Reuse the independent
FN network and working ordinary-client path. Qt trading must work with
`-enableflowmeshvalidator=0`; do not add a second conflicting engine flag.

There are four distinct authorities: B3 block producers, B3 finality
signers, FlowMesh FN seats, and Ethereum's already-deployed verifier.
Connected peers, armed keys, admission, socket writes, quorum certificates,
local durable application and B3 settlement are not interchangeable evidence.

## 1. Recovered baseline and delta

See [recovered handoff](v2-handoff-20260920.md) and the existing
[V1 qualification inventory](flowmesh-v1-integrated-qualification.md).
`flowmeshV2-dev` equals the published V1 baseline; no V2 implementation was
found on it. The eight integrated V1 commits and their evidence are retained.
This stage adds documents only and does not repeat the qualification campaign.

| Component | F: current V1 | R/P: V2 boundary |
|---|---|---|
| Market | One colored base / native B3 quote; vault per market | R: arbitrary supported exact base/quote AssetIds; B3 may be base |
| Ledger | Exact AssetIds, but separate ledger and nonce per market | R: shared spot balances, atomic cross-market reservation |
| Futures | Action numbers 6/7 reserved and rejected | R: real margin/position lifecycle and explicit transfers |
| Fees | 100 ppm seller-paid native B3; 80% seats / 20% treasury | R: configured quote asset; 50 ppm each side for spot, 80/20 allocation |
| Agreement | V1 final locks plus opt-in fresh-market preagreement | R: complete versioned commitment/view-change/restart rules |
| Base chain | V1 deterministic stake eligibility and snapshot finality | P: retain producer economics, add complete checkpoint agreement |
| Client | Market-scoped HTTPS proofs/outbox and reverse-view Qt | R: shared balances, real pair direction, futures and versioned proof routing |
| Ethereum | Immutable V1 digest, lineage, proof and threshold verifier | R: unchanged; adapter compatibility is a gate, not an assumption |

V1 curve-auction matching remains the starting engine. This design does not
silently switch spot markets to price-time matching or reinterpret old
market IDs. Futures instrument matching requires its own approved profile.

## 2. Proposed integrated architecture (P1)

Use **one ordered FlowMesh V2 execution domain** per network/deployment,
containing all configured spot markets and separate futures subledgers.
One committee and one certified sequence order every state transition that
can consume a shared balance. This avoids independent per-market copies of
the same spendable funds. Parallel *read/verification* is allowed; committed
mutation is one deterministic atomic transition. Sharding or asynchronous
cross-ledger commits are outside this initial profile.

The committed state includes:

```
FlowMeshV2State
  domain, protocol/config version, sequence, parent commit, B3 finalized anchor
  committee/epoch and pending authorized handover
  spot[(account, AssetId)] -> available, reserved
  reservations[(account, orderId)] -> market, AssetId, remaining backing
  spot books[MarketId] -> standing curves/orders and executed quantities
  futures[subaccount, InstrumentId] -> collateral, positions, order margin
  account sequence/high-water and exact action outcomes
  deposit provenance/consumed-outpoint set
  pending withdrawals + chain settlement import/nullifier state
  fees, historical seat rewards, treasury liabilities, rounding remainders
  oracle/funding/risk state, backstop liabilities if explicitly approved
  migration provenance and old-claim fences
```

State roots commit to each component under distinct versioned tags, including
configuration and authority. No mutable public ticker or endpoint response
can determine identity, decimals, authority or asset eligibility. A V2
snapshot must prove the same full state a replay reconstructs. Whole-state
snapshot size remains client policy, not permission to omit liabilities.

P1 keeps physically segregated V1 vaults servicing V1 liabilities and uses a
new V2 custody domain for V2 balances. “Shared balance” does not authorize
spending a V1 market vault. V2 introduces versioned on-chain custody rules;
ordinary B3 nodes validate them even when their FlowMesh engine is disabled.

## 3. Identities, amounts and instruction ordering

R: `AssetId` is exact 32 bytes; native B3 retains the existing zero AssetId.
Do not apply V1's “base must be non-null” check to a V2 B3-base market.
Reject base == quote. Two tokens called USD remain two distinct assets.
Asset decimals come from authenticated issuance/adapter rules; labels are
display data. Support eligibility is an explicit versioned rule, not symbol
matching or a promise that any token is stable/backed.

P: `MarketIdV2` commits to network/domain, schema version, base, quote,
market kind, lot/tick/execution-config identity and custody domain. Futures
instrument IDs additionally commit to contract, collateral and risk/oracle
configuration. Changing these creates a new identity or an explicitly
versioned transition; it never edits historical signed meaning in place.

P: integer atomic-unit amounts with checked widened intermediates; encode
signed PnL separately from unsigned token balances. No floating point in
consensus, no wrapping/saturation of balances, no cross-AssetId rounding.
Reject aggregate overflow rather than truncate when merging old balances.
Wire widths and maximum amounts are a G1 vector-freeze item; changing the
V1 `MAX_MONEY` use requires an explicit bound, not accidental widening.

P: represent prices as an integer number of quote-atom ticks per configured
base lot. `notional = lots * priceTicks * quoteAtomsPerTick`, checked for
overflow; base transfer is `lots * baseAtomsPerLot`. This avoids hidden
decimal division during settlement. Lot and tick grids must be signed
configuration and reviewed for every pair. UI quantity is base units; price
is quote units per base, with exact conversion/error bounds on review.

R: BUY spends quote and receives base; SELL spends base and receives quote.
Thus B3/cUSD BUY receives B3 and pays exact configured cUSD. Do not relabel
the old cUSD/B3 market. Differing 9/6/8/0-decimal cases need vectors.

P: one V2 user sequence per account across both spot and explicit futures
actions, with domain-separated semantic ActionId and authentication evidence
outside semantic identity. A valid authorized expected-sequence instruction
consumes its sequence even on deterministic state rejection, matching the
V1 anti-replay principle. Structural/authentication failures do not.
Duplicates return the original outcome; equal sequence/different content is
equivocation, never a replacement. Admission alone does not allocate credit.
The accounting appendix specifies the proposed event phases, action ordering,
duplicate/rejection behavior and bounded model configuration. External
oracle/deposit/settlement evidence is committed, not read nondeterministically
during execution. Parallel outstanding client instructions must respect the
shared nonce; no automatic re-signing around a gap. Production resource bounds
and byte codecs still require G1 approval.

## 4. Spot execution and cross-market reservations

R: all markets consume the same authoritative `(account, AssetId)` available
balance. The state transition validates the entire action, stages all
debits/credits/reservations and commits once. Failure leaves economic state
unchanged except the explicitly defined consumed action sequence/outcome.

For a BUY, reserve maximum quote notional **plus buyer quote fee bound**.
For a SELL, reserve base only; deduct seller trading fee from quote proceeds.
Store reservation ownership by order ID and market; a generic aggregate
reserved balance alone cannot safely release a different order's funds.
An order replacement atomically releases/reuses its own reservation and
acquires its new bound; it cannot temporarily expose spendable funds to
another market. Cross-market composite actions, if later added, are all-or-
nothing—not two independent market submissions.

Partial fills debit only executed base, executed quote and the actual quote
fees. R1 preserves lifetime execution/fee accumulators across same-order
replacement, while replacement curves express NEW REMAINING quantities and
start a new revision-local fill counter. This is a proposed V2 rule, not a
reinterpretation of V1 replacement. Reuse the conservative persistent-curve
staircase bound with the exact residual recurrence in the accounting appendix;
do not replace it with the maximum notional of a single auction. Return only
proven excess reserve under that recurrence. Cancel releases the precise
remaining reserve after any earlier committed fills. A delayed cancel can
legitimately lose the race to a fill; UI closure is never cancellation.
Reject over-release, wrong-order release, replayed cancel and overflow.

P: retain uniform-price curve-auction selection and deterministic volume,
imbalance, price tie-breaking. Generalize asset units/configuration without
silently changing the auction. All configured spot markets clear in a canonical
order in a microblock; matching never consumes unreserved shared funds.
Settlement of a trade is atomic across both counterparties and fee accounts.

Per asset, the accounting equation at a certified state is:

```
recognized backed custody
 = user spot available + spot reserved + futures backed collateral
 + protocol/insurance/treasury/reward balances not already in those ledgers
 + pending withdrawal liabilities
```

Do not double-count a fee or insurance account also represented in spot.
Unswept deposits, chain-settled but not yet imported withdrawals and pending
migration custody have explicit reconciliation buckets. A spendable pool
UTXO total is not automatically the internal recognized-custody total.

## 5. Quote-asset fees and payouts

R: spot fee rate 50 ppm per side, 100 ppm total nominal; 80% FN / 20%
treasury. All trading-fee debits and fee payouts are in **that market's exact quote
AssetId**. B3 transaction/network fees remain B3. No implicit asset swap,
generic USD fee account or extra B3 trading charge.

P (approval D6): use cumulative **per-order** fee accounting. For either
side, charge the difference between `floor(cumulativeQuoteNotional * 50 /
1,000,000)` after and before a fill. Preserve this accumulator through partial
fills and same-order replacement. A new order has a new accumulator; splitting
orders can affect dust and must be disclosed/tested before approval.
R1 groups collected fees across all markets in ONE global microblock by
exact quote AssetId, then allocates treasury `floor(assetBatchTotalFees * 20 /
100)`; the remainder is the FN pool. The accounting appendix defines historical
recipient authority and deterministic seat remainders. This grouping and dust
policy are proposed economics requiring approval, not existing mainnet rules.
Reserve BUY remaining notional plus
`ceil((alreadyFilledNotional + maximumRemainingNotional) * 50 / 1,000,000)
 - alreadyChargedFees`. Check this bound against actual reserve before any
fill/replacement commits; recomputation cannot create backing. Do not round
display units first or charge a minimum fee absent approval.
For fill notional x, require `x + newMaximumRemainingNotional <=
oldMaximumRemainingNotional`. A replacement increasing the bound must reserve
the difference from available funds atomically or fail; it cannot borrow a
different market's reservation. Include those preconditions in G1 vectors.
When an order is finished or cancelled, release every unused fee-reserve atom;
a fractional cumulative fee remainder is not an unpaid token liability.
Rate changes, if later supported, need a new signed fee schedule/accumulator
boundary; they cannot retroactively reprice already filled notional.

Do **not** use batch-largest-remainder fee assignment with only a per-order
percentage ceiling: repeated partial fills can repeatedly assign the rounding
atom to one order and exceed that reserve. Two ten-lot BUY orders at 10,000
quote atoms/lot, each filled one lot in ten successive batches, expose this:
one order can pay ten rounding atoms despite an initial five-atom fee reserve.
This rejected draft alternative is retained as a mandatory negative vector.

This is deliberately labelled a proposal: per-order floors can differ from
V1's single seller-paid 100 ppm batch floor on dust trades. “100 ppm total” is nominal
and cannot imply an exact fractional atom debit. Compare aggregate-side vs
cumulative-order rounding before ratification; freeze zero/dust/split-order
vectors and disclose any splitting incentive. Existing V1 outcomes are never
recomputed using the V2 rule.

Accrual and claims key by protocol domain, historical epoch/seat authority
and exact AssetId; record market attribution separately. Ownership of a
current FN cannot steal an earlier BLS key's rewards. Keep historical claim
authorization or approve a separately signed transfer. Treasury payouts
likewise carry exact assets and destinations. Reward/treasury withdrawals
use ordinary custody authorization/nullifiers, not privileged minting.

Futures trading fees must also use their configured quote asset. Whether
the same 50/50 ppm and 80/20 schedule applies to futures, and whether any
explicit fee funds a backstop, is D7/D9—not an assumed change in rewards.

## 6. Futures is release-required, not reserved names

R: separate futures state; no futures order, loss, funding debit,
liquidation, fee or margin call may consume spot available/reserved funds.
No “auto top-up” default or implicit collateral conversion.
B3 deposits credit spot by default. B3 withdrawal requests debit spot
available only. Futures collateral must first pass an explicit successful
`FUTURES_TO_SPOT`; a direct chain withdrawal cannot bypass margin checks.

P (D7): first instrument profile is **linear, quote-collateralized perpetuals
with isolated position/subaccount risk**, not inverse, dated delivery or
portfolio cross-margin. This is a proposed product choice, not an owner
ruling. The contract manifest must freeze underlying reference, lot/tick,
multiplier, quote/collateral AssetId, oracle/risk configuration, limits,
margin tiers, funding rule and settlement arithmetic. Multi-collateral and
cross-margin are deferred unless explicitly chosen instead.

### Explicit transfers

`SPOT_TO_FUTURES(account, subaccount, AssetId, amount)` atomically debits only
spot available and credits accepted futures collateral. `FUTURES_TO_SPOT`
atomically debits **withdrawable** futures collateral and credits spot only
after positions, open-order worst-case exposure, funding, fees, pending
liquidations and maintenance/initial margin buffers have been checked using
the committed risk/oracle version. Unknown/stale valuation cannot authorize
a risk-increasing transfer out. Rejected transfers modify neither ledger;
record only their deterministic nonce/outcome. Transfers have one ActionId,
not a debit request followed by an independently retryable credit request.

### Required futures state machine

1. Register an approved instrument/risk/oracle manifest; no arbitrary client
   chooses the oracle, collateral or leverage limit.
2. Admit a signed futures order only after reserving its worst-case margin
   and fees *inside futures*. Reduce-only orders cannot increase exposure.
3. Match under the approved instrument auction rules; atomically update both
   positions, entry/cost accounting, collateral and fees. Preserve exact lots
   and price evidence and prevent negative transferable balances.
4. Funding and mark updates are deterministic committed events, with bounded
   time steps and catch-up behavior. No node-local wall-clock accrual.
5. Evaluate margin after fills, funding, oracle changes and transfers. Freeze
   risk-increasing actions when data is invalid/stale; specify separately
   whether safe cancels/reduce-only actions are permitted.
6. Liquidate through an explicitly specified auction/takeover rule. Record
   filled reduction, realized PnL, fees, remaining position and any deficit.
   Do not magically mark a bankrupt position settled by deleting it.
7. Apply the approved funded backstop/waterfall or enter a precise deficit
   halt. No surprise haircut/ADL, shared-spot seizure, token mint or transfer
   from an unrelated asset. Publish which claims cannot currently withdraw.
8. Closed positions and settlements retain their original action, oracle,
   funding and liquidation evidence; explicit transfer out uses the margin-
   safety rule in “Explicit transfers,” never the spot withdrawal path directly.

For linear contracts, signed PnL is derived from position quantity,
multiplier and mark/entry difference in exact rational atomic units, with
one approved rounding direction and residual-account rule. Opposite position
PnL and funding transfers must balance; unrealized gains are not new backed
custody. Specify when variation margin becomes withdrawable and how unpaid
losses constrain winner claims. Keep contractual PnL claims separate from
cash collateral so a hidden deficit cannot satisfy the custody equation.

**D8 oracle:** proposed quorum-authenticated multi-source index plus bounded
mark methodology; signer quorum authenticates publication, not economic
truth. Required fields include feed/instrument ID, observation time, round,
price units, source policy, expiry and version. Reject replay, future/stale
data and divergent-unit inputs. Define quorum independence, fallback,
outlier bounds, update frequency and signer rotation before implementation.
No source/vendor or oracle committee is silently selected here.

**D9 liquidation/backstop:** proposed explicitly funded isolated insurance
reserve, then a declared deficit halt if insufficient. ADL/socialized losses
require a separate owner choice and precisely ordered haircut rule. Insurance
funding source, liquidator incentive, insolvency allocation and withdrawal
priority cannot be left to an operator at runtime. These are release blockers.

G: futures qualification must include margin exhaustion, stale/manipulated
oracle, funding gaps, partial liquidation, bankrupt counterparty, insufficient
backstop, recovery, restart and conservation. A model with only profitable
fills does not qualify a futures product.

## 7. Complete commitment/recovery profile for review (P2)

P2 recommends a **single-sequence PBFT-style prepared/commit protocol**,
building on inspected V1 preagreement proof mechanics rather than inventing
a PoW winner priority or interpreting an individual signature as commitment.
Selection versus a Tendermint-style protocol remains D1. These are different
protocols, not interchangeable labels. The V1 implementation is evidence of
available components, not a proof of the proposed V2 system.

### Authority and domains

Each instance binds protocol version, chain/domain, subsystem (B3 finality
or FlowMesh), epoch, committee commitment, sequence, parent committed digest
and config hash. Candidate digest binds complete ordered body, B3 finalized
anchor and all result/effect roots. Every signed message additionally binds
phase, view and signer identity. PROPOSE, PREPARE, COMMIT, VIEW_CHANGE and
NEW_VIEW domains are distinct; none is an Ethereum/V1 finality attestation.
Signature encoding, aggregation/PoP policy and bounded proof codec are G1.

P: FN equal-seat `Q=floor(2N/3)+1`, tolerate at most `f=floor((N-1)/3)`
Byzantine/unavailable seats for the stated progress claim; nine seats still
need seven. Safety needs honest quorum intersection; progress needs enough
available votes and eventual timely delivery. Do not lower a threshold to
fit currently online peers. Multiple keys on one VPS are correlated faults,
not independent resilience. Base finality/bridge additionally requires its
existing weight threshold; assume faulty weight **and** faulty headcount
are below one third for that profile.

### Normal case

The explicit transition guards, context/authority identifiers and pure
candidate-validation predicates are in the consensus appendix. In particular,
"valid candidate" does not mean acceptable to a node's current private tip.

1. One deterministic scheduled proposer per `(epoch, sequence, view)` sends
   complete candidate and admissibility evidence. P: anchored seat order
   rotation for FN; no persistent-leader optimization in first implementation.
2. A replica first checks its durable current view, the scheduled authenticated
   proposer and absence of a different local decision. For a nonzero view it
   must have durably accepted the matching complete NEW_VIEW and its selected
   value; a timeout or larger view number is insufficient. It verifies parent,
   configuration, complete data, user auth,
   custody/oracle evidence and deterministic execution on a scratch state.
   It persists proposal acceptance and a unique PREPARE intent before signing;
   persists exact signed bytes before publication. At most one candidate per
   phase/view/sequence/key may be signed.
3. Q distinct valid PREPAREs for the same context/view/candidate form a
   PreparedQC. COMMIT also requires the matching durably accepted proposal,
   current-view authorization and NEW_VIEW where applicable. Persist the
   complete proof before COMMIT intent/signature.
   Prepared is **not** decided and must not create an externally usable final
   attestation or a final client success response.
4. Q valid COMMITs for that prepared value form CommitQC. For view > 0,
   include its valid NEW_VIEW justification. Verify the full proof and body;
   durably record the decision, state/effects and commit/application marker
   under an atomic or replay-safe transactional protocol.
5. Publish the final V2 commit certificate/client proof only after the durable
   boundary. An aggregator need not be the leader: any replica with the
   complete proof can disseminate it. Receipt status distinguishes QC formed,
   locally durable and durably applied on other replicas.
6. The next sequence extends that committed parent. No rollback of a decided
   sequence for a nicer order set, newer oracle or later-arriving proposal.

### View change and incomplete delivery

Local progress timeout enters a higher view; persist the monotonic view and
stop creating old-view votes. It may retransmit exact old bytes and accept
valid old proofs; timeout does not revoke a signature. A bounded suspicion
trigger may solicit reports but is not a new-view or unlock proof.
P: `f+1` distinct authenticated VIEW_CHANGE reports for the same higher target
may make an honest lagging replica join that view; one peer's integer cannot.
The target must be within the validated instance and checked view bounds.
Both this trigger and local timeout still require Q reports before NEW_VIEW
acceptance. Deduplicate reports, bound future-view storage, and never reset a
progress timer for duplicate or unauthenticated traffic.

Each VIEW_CHANGE signs the target view, committed parent/sequence, highest
known complete PreparedQC and any known decision. Reporting no preparation
after a local durable prepared/commit record is forbidden. The new proposer
collects Q distinct reports for the same target/context. It must re-propose
the candidate of the highest prepared view in that quorum; with none it may
choose any fully valid candidate. Equal-highest different candidates are a
safety violation, not a tie broken by hash. If a valid decision is learned,
finish/replay that decision rather than choose a replacement.
Every included prepared proof must name a view strictly below the new target
and the identical instance/authority/parent. Each vote is counted once by
anchored identity; a proof cannot combine phases, views, candidates or epochs.
A decision verifies the matching PREPARE and COMMIT quorums, candidate body,
and nonzero-view justification. Missing witnesses are a data-recovery state,
not a valid proof with a weaker quorum.

Followers verify all reports, signatures, quorum, proof linkage, chosen
value and candidate availability before accepting NEW_VIEW. A newer view
number alone authorizes nothing. With no local decision, a complete valid
NEW_VIEW is authority to supersede local preparation or an individual
COMMIT vote; do not require unanimity or indefinitely veto it because one
local PreparedQC is absent from that quorum's reports. Retain/forward that
evidence and report the highest known proof in one's own next VIEW_CHANGE.
Never supersede a complete CommitQC with a different candidate. Under the
fault bound, a conflicting valid NEW_VIEW and complete decision cannot
coexist: the committers/report quorum intersect in honest members who carry
preparation evidence. If both verify, halt and retain the safety-breach
evidence, not choose a winner.

This relies on the full report set and monotonic persist-before-sign behavior,
not a bare leader assertion. Independently model-check hidden commit quorums,
preparation known to only one honest replica, omitted local proof and delayed
old-view decisions before porting it. Writing these rules is not a proof of
their implementation or liveness.

Receiving COMMIT before candidate/PreparedQC retains bounded recovery work
and requests the exact missing objects. Cannot vote until data validates;
cannot silently discard critical proofs during reconciliation. Maintain
per-peer critical/action/bulk bounds and relevance-aware exact-byte retries.
Socket writes are not peer acknowledgements or durable replication.

No fixed 128-view exhaustion that erases safety history. P: monotonic wide
views with checked overflow, safe failure on actual exhaustion, and bounded
in-memory windows backed by durable required proofs. Compact only obsolete
traffic after a durable decision/stable snapshot; never forget current
prepared/committed evidence to recover memory. Timeouts back off under
partial synchrony; measure normal delivery before choosing values. No timeout
can guarantee progress when quorum or required durable data is absent.

### Restart, catch-up and reconciliation

Journal contains instance/authority, monotonic view, accepted proposal,
highest prepared proof, every local sign intent/exact signed object, commit
decision, state transition/application marker and checkpoint/receipt cursors.
Use one signer owner per key/domain. Process exclusivity and deployment
fencing must prevent a second runtime, not just hope copies sign identically.

On restart verify journal/version/authority and replay committed state;
unfinished intent can finish only its exact object and only while still
authorized by the durable current view. An unsigned abandoned-view intent
must not create a new signature; already-signed exact bytes may be relayed.
Missing/corrupt safety
records fail closed with explicit reason. A structurally valid old filesystem
snapshot is NOT thereby proven fresh: the consensus appendix defines the
stable-storage fault assumption, freshness/recovery gate and signing refusal
after detected or suspected rollback. No claim of automatic rollback detection
is made from checksums, process exclusivity or persist-before-sign alone.
Unknown network outcome triggers
identity reconciliation/exact-byte retry, never a new signature for another
candidate. Reopening the wallet cannot clear no-resubmit records.

Catch-up verifies parent/committee transitions, certificates, effects and
deterministic state. Snapshot and log pruning retain signing high-water and
replay fences. Readiness reports separately: synchronized, eligible, armed,
observed valid signature, peer acceptance and durable replication.

P: FlowMesh irreversible decisions depend only on **B3 finalized anchors**.
New unfinalized deposits/withdrawal settlements wait for the committed B3
anchor to include them; already-backed internal trades need not wait for
each new B3 block. Reorg above that anchor invalidates only speculative input
evidence, not certified balances. Conflicting final B3 proofs or removal of
a committed anchor are a safety breach requiring a halt and separate incident
analysis. Never “unlock because this fork cannot gather fresh votes.” Anchor
advancement, complete ordered event import and handover activation must follow
the pure predicates in the consensus appendix; a newer local tip is not an
anchor-update command.

### Committee transitions and inactive seats

P: anchored permissionless registration and objective readiness diagnostics;
epoch membership changes only through authenticated on-chain state and a
terminal decision by the outgoing committee linking the exact successor and
state. First successor decision extends that parent with the new domain and
set proof. Late old-epoch messages cannot spend new-epoch balances.
Unilateral reachability estimates do not remove seats, decay weight or
authorize newcomers. Rewards may not be redesigned under a readiness check.

Automatic removal, decay, slashing and penalties are D4 proposals requiring
new objective evidence, censorship/rejoin rules and economics. Baseline
recommendation: preserve thresholds and no automatic removal until modeled.
An outgoing committee lacking quorum cannot safely authorize its own
replacement merely because a coordinator declares it offline.

## 8. B3 PoS V2 profile and fork-choice boundary (P3)

P3-R1 proposes retaining V1 block-eligibility arithmetic/economics while
separating checkpoint authority from the producer snapshot tracker, with
full BFT checkpoint coordination and versioned validation/commit enforcement.
The exact proposed separation and changed handover signal are in C7 of the
consensus appendix. This is an explicit D3 choice, not an assertion that all
producer/finality timing rules remain identical. A new VRF, lower-difficulty PoW,
cooldown, slashing or staking reward schedule is **not selected**.
If the owner wants replacement producer election, choose D3 before its
implementation; this profile is a concrete default proposal, not a hidden
promise of a complete new election algorithm.

Preserve existing STAKE ownership/maturity and exact V1 arithmetic, snapshot
member/weight construction, BIP340 producer identity, 60-second spacing with
30-second recovery rounds, reward cap/halving/treasury and no automatic
restaking. Seed/first V2 block derives from the verified V1 parent state;
do not invent a new genesis or re-decode old transactions. Existing B3
transaction fees remain B3. Authenticated snapshots, not live UTXO spending
alone or online-peer estimates, determine the two in-force authorities.
C7 explicitly defines producer handover by included checkpoint height and
checkpoint snapshot selection by a terminal committed checkpoint, instead
of implicitly reusing V1's shared inclusion-dependent epoch tracker.

P3 adds a separate B3-finality instance of P2, not the FN committee. It agrees
on exact checkpoint block hash, cumulative bridge withdrawal root and
successor header before generating existing-contract finality signatures.
All candidates must descend from the last committed/finalized checkpoint,
meet checkpoint schedule/depth/branch-validation rules and the in-force set.
Proposal validation includes required branch data, not a requirement that
every replica already selected the same provisional tip. Missing branch data
means catch-up, not an arbitrary different checkpoint vote.

P: B3 agreement sequence is a monotonic checkpoint-decision ordinal, not
local tip height. The candidate names its actual schedule-valid height;
all replicas extend the same prior committed checkpoint. The first instance
is authorized by the fixed-outgoing-authority B0 proof in C8; it preserves
the actual inherited V1 finalized state and all precommitted successors.
Never create independent instances for competing hashes at one checkpoint.
The previous committed decision/outgoing-set handover authorizes the one
next-instance committee, exact snapshot boundary/header and activation rule
under C7, including the exact checkpoint epoch-floor and terminal predicate.
Candidate-local projections of certificate inclusion height must not create
different authorities for the same sequence. This replaces that V1 ambiguity
at the explicit V2 boundary while retaining member/weight construction and
the external precommitted successor lineage. Genesis/cutover vectors must
cover already-committed V1 successor headers, not compute replacements.

P: transport B3-finality coordination through bounded versioned B3 peer
messages authenticated by finality keys; stakers need not own an FN or enable
FlowMesh validation. FlowMesh continues over the independent FN network.
This reuses existing transports, not a second FlowMesh enable flag.

At activation, V2 nodes validate a new domain-separated B3 CommitQC and its
body before updating their durable finalized pin; use the committed checkpoint
as the common lower bound for fork choice. Within its unfinalized descendants,
retain the specified V1 height/round/hash ordering unless D3 changes it.
A valid committed pin cannot be undone by a higher uncommitted tip, a local
reindex/import exception or peer majority. Recovery/reindex reconstructs the
same commitments. Comparator fields are immutable before candidate insertion.

Track **separate** high-water records for BFT checkpoint commitment, legacy
attestation signing, legacy certificate inclusion/export and Ethereum
acceptance. Persisting the BFT pin must not make V1's strictly-increasing-height
test reject the subsequent export of that *same decision*. Permit its exact
legacy certificate to complete the pipeline; reject different same-height
objects. Rebuild/pruning must preserve both records and outstanding exports.
The adapter cannot replace a decided checkpoint if legacy export is blocked.

The on-chain finality carrier must bind the CommitQC (or an exact verified
commitment with data available) and the compatible legacy certificate; new
versioned payload rules must be explicit. The preliminary QC cannot be
hash-committed inside the very candidate whose hash it signs: carry proof of
an earlier checkpoint in a later block, or use a separately validated proof
object. Preserve this acyclic dependency in codec fixtures.

R/G: a durable BFT decision precedes releasing each externally valid V1
finality attestation. This prevents *new* premature conflicting external
votes when assumptions hold. It does not withdraw already-issued V1 votes,
repair an expired immutable Ethereum lineage, or ensure a currently offline
committee becomes available. Base block production may continue while
finality/bridge is stalled; label those states separately.

## 9. Existing Ethereum bridge — unchanged acceptance envelope

Source inspected at the baseline, not merely presumed from BLS byte size:
`B3FinalityVerifier.sol`, `BlsCertificateProver.sol`, `B3StakerBridge.sol`,
`IB3FinalityProver.sol`, node finality/withdrawal codecs and calldata builder.
See §15 for source anchors. These are exact source rules and retained
deployment pins; **independently reproduced source/compiler-to-runtime
equivalence remains unverified** (§9.4). R1 adds bounded public Ethereum
runtime/getter evidence in its bridge appendix, not a live B3-node audit.

### 9.1 Frozen verifier interface

`FinalizedBlock` is 112 bytes:
`height:u64BE || blockHash:32 || withdrawalRoot:32 || successorSetHash:32 ||
signingEpoch:u64BE`. Hash fields are raw bytes, not reversed display hex.
Final digest is SHA256 of `tagHash || tagHash || CHAIN_DOMAIN || block`, where
`tagHash=SHA256("B3/FINALITY/V1")`. The domain is the existing modern chain
domain; no V2 domain substitution in this external message.

BLS is G1 public keys/G2 signatures, PoP ciphersuite DST
`BLS_SIG_BLS12381G2_XMD:SHA-256_SSWU_RO_POP_`; compressed node sizes 48/96.
Ethereum witnesses use EIP-2537 coordinates, not direct blst byte order.

`SetHeader` is 110 packed bytes:
`epoch:u64BE || ruleset:u16BE || count:u32BE || totalWeight:u64BE ||
quorumWeight:u64BE || aggregateKey:48 || membersRoot:32`, hashed with Keccak.
Ruleset must remain 1; count 1..8192; nonzero weight/root; quorum weight
`floor(2W/3)+1`. Signer **headcount also** requires `floor(2N/3)+1`.
The ordered member tree has depth 13, zero padding to 8192 leaves and leaf
`Keccak(index:u32BE || key:48 || weight:u64BE)`; no sorted-pair replacement.

Proof is ABI encoding of bitmap, uncompressed signature G2 (256 bytes),
aggregate key G1 (128 bytes) and ordered absent-member rows. Bitmap is exactly
ceil(N/8) bytes, LSB first, unused high bits zero. Every absent member requires
its index, compressed/uncompressed key, weight and exact 13-sibling proof.
The verifier subtracts absent weights/keys from committed totals and checks
non-infinity aggregate/pairing. Do not replace this with a generic V2 QC ABI.

### 9.2 Lineage, time, root and release constraints

One-time bootstrap is 3-of-4 before its immutable deadline, at M−1 with
epoch zero, zero withdrawal root and the committed Set0. It is not a recovery
entry point. Later certificates are only current epoch or the already-
committed next epoch, with strictly increasing height and exact successor
header. First certificate fixes a successor; same-epoch certificates cannot
change it. No skipped epochs or retrospective committee replacement.

Retained deployment pins: M=B=811001; minimum epoch time 86400 seconds;
maximum lineage lag 2592000 seconds. Rotation cannot occur sooner than one
day; same-epoch updates do not reset the rotation age. Certificate acceptance
allows age exactly 2592000 seconds and rejects a larger age; no signing
timestamp bypass. Epoch window is `GENESIS_TIME + e*86400 <= now <=
GENESIS_TIME + (e+1)*2592000`. GENESIS_TIME is set once at initialization,
not a constructor immutable.

Bridge-qualified set: 4..64 members and weight >=900. A qualifying **signing
set** can advance the bridge release root at or above B. Deposits additionally
require a qualifying current and known successor set, a qualifying recent
certificate (<=86400 seconds), epoch validity and **more than** seven days
of remaining lineage lifetime: rotation age <1987200 seconds. Do not confuse
this with release readiness: an already
accepted qualified root remains usable for authorized releases even if
new deposits become unavailable or lineage expires.

Withdrawal root is zero before B and nonzero at or above B (including canonical
empty tree). It is a cumulative ordered depth-32 Keccak tree, with consecutive
zero-based IDs. Leaf is 128 bytes:
`id:u64BE || originChain:u64BE || asset:32 || token:20 || recipient:20 ||
amount:u256BE || b3Height:u64BE`. Release checks fixed asset/token/domain,
ID < 2^32, unreleased ID, nonzero amount/recipient, height >= B, authorized
finalized height, exact 32-sibling proof against latest bridge root and locked
reserve. Keep the spent-ID ledger and cumulative roots across V2.

Bridge AssetId commits domain, Ethereum chain, existing vault/token, adapter
and decimals. Its origin-chain preimage uses u64 **little endian**, unlike
the withdrawal leaf. Exact six-decimal USDT pins cannot be relabelled by a
V2 market or changed in a generic asset migration.

### 9.3 Proposed compatible node-side adapter

1. Authenticate an in-force legacy-compatible set and full branch/root data.
   Before PREPARE, validate schedule, root, exact successor and the local
   signer's outstanding V1 ancestry/vote obligations. Activation readiness
   must account for a quorum actually capable of legacy export, not just
   connected upgraded nodes. A node unable to export must report that state,
   not bypass its journal to cast the proposed bridge-compatible vote.
2. Complete P2 B3 checkpoint agreement and persist its decision/pin.
3. Recheck that exact decision's compatibility before legacy export. If a
   fault or missing obligation prevents export, retain a precise committed-
   but-export-blocked state. Do not reopen the decision or manufacture a
   replacement checkpoint. Separate the high-water records described in §8.
4. Persist attestation intent/history and emit only the **unchanged** V1
   digest signature; never encode the V2 phase/view in that external object.
5. Relay the existing proof ABI with both original thresholds and sequential
   lineage. Ethereum sees the legacy proof, not the BFT coordination proof.

Ethereum cannot itself enforce step 2. Security also requires all counted
honest upgraded signers to enforce that gate and compatible committee
handover; mixed legacy early-signers must count against the fault budget.
FlowMesh FN certificates must not be substituted for B3 finality authority.

| Proposed change | Unchanged contract outcome / compatible alternative |
|---|---|
| New external digest/DST, round fields or ruleset | Rejected; keep legacy export and separate internal V2 domains |
| Smaller weight/headcount threshold | Rejected; both deployed thresholds remain |
| More than 64 bridge members | No new bridge-qualified root; either retain compatible set or separately approve a bounded, lineage-authorized bridge committee |
| Fast base-chain epochs | Cannot force fast Ethereum rotation. P3-R1 retains the inherited checkpoint interval parameter but explicitly changes checkpoint epoch boundaries; external export still obeys existing Ethereum timing/lineage and may wait or fail closed. Independent export epochs are not selected |
| Uncommitted replacement/skip epoch | Rejected; old lineage must authenticate successor |
| Expired deployed lineage or conflicting old final signatures | New internal BFT alone cannot repair it; preserve evidence and report incompatible/blocked state |
| New FlowMesh internal balances/fees | No direct Ethereum change if B3 cumulative burn/root/asset/release semantics remain identical |

A separately bounded bridge committee changes trust/selection even when
contract-accepted; it is D5, not an automatic shim. The recommended first
profile keeps existing bridge lineage/economics and does not assume such a
committee has already been authorized.

### 9.4 Deployment evidence gate

Public manifest: `contracts/deployments/ethereum-mainnet-v1.1.1.json`.

| Component | Retained address | Retained runtime hash |
|---|---|---|
| Vault | 0x077839b12cebfbF163acAEAC3A59A015D100c64b | 0xdb267712887568bffd394e46538bddba01da11cefc38e32b2428c00911237f8d |
| Verifier | 0xE72B3Fe73F0d42A6e964D33E7BB1cc2EA7a3F690 | 0xafdba8befb1aacc832bff4e08dcd92e6645a012ea8a8088b0f2811d916022902 |
| Prover | 0x8e612aE4D475d25940E2A2FC907F21b6813eedA7 | 0x77d2aea2d2a6842fae8b29e64a146622e2f45e772a6c351640ffe8362211a959 |

The manifest's build-provenance hash is
`0x4892851a63adab398e2496437956986e1c9ee25a3563c8fd226288a7180fdedd`.
Historical relayer read evidence corroborates addresses, but explicitly is
not a current readiness certificate. R1's bridge appendix separately records
public state at finalized Ethereum block 26018233 and two-provider checks:
runtime hashes match the retained manifest; depositViable is false and
releaseReady true at that block. This is not full source/runtime equivalence
or current B3 inbound readiness. The earlier bounded attempt to read the
[verifier](https://etherscan.io/address/0xE72B3Fe73F0d42A6e964D33E7BB1cc2EA7a3F690#code)
and [prover](https://etherscan.io/address/0x8e612aE4D475d25940E2A2FC907F21b6813eedA7#code)
source pages on 2026-09-20 returned tool access failures;
that does not prove verified source is absent.

G: before asserting full deployed parity, reproduce source/compiler/optimizer/
constructor/immutable mapping and close the remaining BR-EXT evidence in the
R1 bridge appendix. Its runtime hash and recognized-state observations narrow,
but do not erase, that gate. No wallet or signer is needed for public getters.
If bytecode/source mapping or lineage availability fails, report it; do not
choose a new vault.

### 9.5 Independent inbound light-client gate (R1 verification)

The [bridge appendix](v2-stage0-r1-bridge.md) verifies the inspected release
configurations, header/signature-slot boundaries, execution-timestamp mint
freshness and preservation rules. Ethereum contract lineage expiry and B3's
pinned Ethereum-fork horizon are separate mechanisms. The recorded
`2026-10-04T20:00:23Z` is not a proven wall-clock stop of every bridge operation.
An Ethereum deposit-readiness response does not prove the B3 inbound mint path
is available; a client warning cannot change the unchanged contract's predicate.
No pins or live configuration were changed during this verification.

## 10. V1 preservation and migration (P4, owner decision D10)

R: historical V1 validation, market IDs, exact signed instructions, deposited
balances and pending rights survive. V2 is not “start at zero and forget.”
Use separate domains, stores and explicit version dispatch, with downgrade
refusal after V2 state is written; never in-place reinterpret a V1 signature.

P4 recommends **parallel V1 claim service plus opt-in, proof-backed transfer
to V2**, not bulk forced migration. Legacy orders remain their original
orders until explicitly cancelled/executed under valid V1 rules. A V1
withdrawal pays its fixed destination; after settlement the owner can make
a separately authorized V2 deposit. A direct migration optimization would
require a reviewed one-time custody/liability transfer transaction and old
claim nullifiers; it is not included merely by publishing a new balance root.

| Transition inventory | Required treatment |
|---|---|
| Spot available and reserved, all accounts/assets/markets | Preserve exact integer values/provenance; aggregate only exact IDs after valid release/transfer; check sum overflow |
| Standing and partially filled curves | Preserve executed quantity and residual backing; no full-quantity recreation or implicit cancellation |
| Created/shallow/queued/credited/checkpointed/unswept deposits | One credit per original outpoint, with explicit old or new ingress authority; no second credit when sweeping |
| Withdrawal requested/certified/authorized/broadcast-unknown | Retain amount, destination, asset, receipt/proof and original settlement authority; no recredit while old authorization can spend |
| Chain-settled but not imported; consumed claims | Preserve chain nullifiers and import cursor; reconcile before changing liability totals |
| Fees, historical rewards, treasury and pending payouts | Keep original AssetId, rounding result, historical claim key and accrued/pending status |
| Client nonce/outbox/unknown/certified/rejected records | V1 exact-action recovery remains V1; V2 has explicit new namespace, no automatic replacement/re-sign |
| Validator prepared/final locks, candidates, decisions | Preserve as V1 evidence; new key/domain is not evidence that old obligations disappeared |
| Vault UTXOs, shards, effect ranges and checkpoint lineage | Every unit assigned to exactly one liability domain; retain old proof/nullifier paths |
| Ethereum lineage, cumulative roots and released IDs | Unchanged; no domain reset/bootstrap replay or rewriting accepted history |

V1 permanently split final locks may prevent old claims progressing. New
V2 design does **not** establish permission to confiscate, duplicate or
arbitrarily snapshot those balances. A hard-fork recovery allocation, if ever
proposed, requires full competing-certificate/claim inventory, public rules
and separate owner/security approval. Until then affected V1 assets remain
accounted, visibly blocked, and excluded from V2 spendable credit.

R1 proposes the exact semantic cutover routing table in the transition appendix:
old deposits confirmed below symbolic H remain V1 ingress even when credited
later; new V1 user-custody deposits at/above H are rejected; V2 deposits require
both H and effective authorized bootstrap. Preserve V1 settlement change and
old payouts rather than mistaking them for new user deposits. This rule is a
pending D10 approval, not activated behavior. Never silently send to whichever
backend is reachable.
Keep V1 observation/claim APIs available with honest stalled/retired status.
The manifest of preserved liabilities and its independently reproduced
root/counts is a release gate even when no automatic migration is selected.

## 11. B3 custody/checkpoint/withdrawal upgrade

P: add new versioned rules rather than overwriting type-8/type-9 semantics.
V2 custody identifies shared execution domain + AssetId, not one old market.
Checkpoint commits protocol/config, committee transition, global sequence,
parent/state component roots, cumulative effect ranges, B3 finalized anchor
and complete V2 commit proof. Pending withdrawals commit exact asset,
amount, owner destination, custody domain and replay identity.

Full B3 validators independently validate allowed custody input/output kinds,
per-asset conservation, effect continuity, certificate authority/threshold,
anchored membership, withdrawal proof and one-time consumption. No local
FlowMesh engine required; no trust in an HTTPS success response.
Retain pool-input/weight bounds and deterministic change; generalizing the
asset set cannot make an unbounded sweep or mix B3 fees with token amounts.
Withdrawal admission also requires deterministic per-AssetId anchored payout
capacity/selection accounting for existing pending liabilities, unswept
deposits, pool fragmentation and settlement imports. Recognized custody alone
does not prove spendable bounded-input capacity. Freeze that policy at G1
and test fragmented pools and concurrently pending claims. Chain withdrawal
debits spot available only, never futures collateral directly.

B3 includes checkpoint first, confirms it under the required rules, then
authorizes payout. Client distinguishes internal certification from payout.
Futures-to-spot is internal accounting, not automatically a B3 withdrawal.
Block-validation parity fixtures must compare enabled/disabled nodes on
constructed valid/invalid V1, V2 and boundary blocks—not only mempool checks.

## 12. Client, networking and operator upgrade

Reuse HTTPS connection/failover and independent FN critical/action/bulk
transport. V2 capability/version negotiation must reject incompatible votes
without accepting a peer's claimed readiness as membership. FMN2 remains
authenticated **plaintext TCP**; encryption/QUIC is separate work, never
implied by reusing HTTPS for traders. No new discovery trust authority is
silently introduced.

Ordinary clients discover exact market/instrument configs and shared asset
balances without starting a signer. Review shows true BUY/SELL, base quantity,
quote price, worst-case reservation, fee asset/rate, futures risk account
where applicable and exact receiving/withdrawal destination. Portfolio
screens separate available, reserved, pending claim and futures collateral.
No generic USD label replacing the actual quote asset.

Versioned API/proofs bind deployment, state sequence, committee/config and
account. Preserve exact signed bytes, ActionId, shared sequence, uncertainty
and certified/no-resubmit markers through wallet switching/shutdown/reopen.
Selecting an endpoint or closing Qt cannot submit, cancel or replace an
instruction. Read-only reconnect remains read-only. V1 receipts retain the
V1 verifier and namespace; a V2 endpoint cannot certify them by relabeling.

Operator tooling must show connection, synchronization, committee membership,
eligibility, arming and observed accepted signature separately. Preflight
tests diagnose missing listen/connect/config/data; they cannot unilaterally
exclude a voter. Single-owner key transfer preserves journals and proves the
old signer stopped before any authorized new signing runtime starts.

## 13. Activation, test isolation and latency gates

R: planned coordinated **B3 hard fork**, no selected mainnet height. Version
dispatch is inactive/unset in production until a later explicit activation
approval. Define preactivation, activation and postactivation behavior for
block validation, finality, custody, clients and operators. First V2 state
requires the consensus appendix's explicit bootstrap authority and binds
authenticated V1 preservation PREFIXES, the authoritative B3 checkpoint and
manifest. A prefix is not proof that no later V1 certificate exists. Preserve
later valid old claims under their original rules, and never turn a balances
snapshot into V2 spendable credit. Unknown versions fail closed. No historical
nTime/transaction/signature rewriting.

Test first in deterministic unit/model fixtures, then separate generated
regtest nodes. A shadow fork is isolated by network magic, ports, genesis/
test-domain rules, peers, data directories and disabled production routes;
changing only the peer list is insufficient. Import only authorized public
history/state, never production wallets/journals/keys; generated keys and
valueless test assets. Replay old history with its original validation;
new simulated signatures are test-domain-specific.
Generated regtest may have its own genesis. A public-history shadow fork
preserves the original genesis and historical domains through its fork point,
then uses an explicit isolated test-upgrade domain for future signatures.
Never reinterpret imported signatures under a changed genesis/domain.
Replacing production authority with generated keys is a declared test-only
fork fixture, not a claim those keys control historical mainnet stakes.

Before coordinated rollout require independent audits, code/byte vectors,
historical replay, disk-failure/restart/power-loss tests, custody conservation,
mixed-version refusal, old claims preserved, ordinary-client block parity,
operator readiness, signed package identity and rollback/downgrade plan.
Reverting software must not roll back a committed ledger or signing history.

R: target approximately **200 ms median**, usable healthy path 200–600 ms.
This is a performance requirement to test, not a guarantee or achieved result.
Measure original client submission → client-verified durable certification;
separately first admission → certification; each replica's durable application;
and lagging-node catch-up/signing readiness. Do not call submission-to-last-
replica time “extra replication delay.” Retain retries/refusals in timing.

Use four separate processes, then independent machines and approved US/EU
WAN locations. Separate healthy advancing-B3 runs from partition/rejoin,
leader failure, slow disk/peer, held bulk, stale oracle, burst and multi-market
workloads. Report p50/p95/p99, offered/achieved load, sample counts, CPU/disk/
RTT, missing timestamps, backlog and timeouts. 200 ms on localhost does not
establish WAN latency; three protocol phases, durable writes and geography
remain costs. No async durability, lower quorum or hidden pre-admission wait
to meet the target. Profile before pipelining or execution parallelism.

## 14. Decision sheet and bounded progression

These are the genuine owner choices. Routine safe research and documentation
do not require additional decisions. All recommended entries remain **PENDING**.

| ID | Choice | Recommended profile / meaningful alternative |
|---|---|---|
| D1 | BFT protocol and fault model | P2 complete single-slot PBFT-style commit/view-change first; alternative explicitly specified Tendermint-style protocol. No reduced quorum |
| D2 | Shared execution/reservation boundary | P1 one global ordered domain with separated spot/futures accounting; alternative sharded atomic protocol adds substantial design work |
| D3 | PoS V2 scope | P3-R1 preserves producer eligibility/reward arithmetic but separates checkpoint/producer authority and explicitly changes handover linkage; approve that boundary or require a separately specified unified-authority alternative. New VRF/election remains unselected |
| D4 | Inactive validators and staking changes | Objective diagnostics + epoch-authorized changes initially; no automatic decay/slashing/cooldown/reward change without a separate approved rule |
| D5 | Unchanged bridge authority | Preserve existing compatible lineage/set rules; a bounded separate bridge committee is possible only with explicit trust/selection/handover approval and available old lineage |
| D6 | Units, fee rounding and dust | Exact lot/tick amounts, cumulative per-order fee floors and provable BUY reserve bound; approve splitting/dust/replacement vectors before freezing |
| D7 | Futures product/margin/fees | Linear quote-collateralized perpetuals, isolated risk first; decide leverage tiers, funding, futures rates and matching profile; alternatives dated/inverse/cross-margin not assumed |
| D8 | Oracle/mark policy | Authenticated multi-source index with explicit freshness/mark bounds; select sources, quorum independence, rotation and outage behavior |
| D9 | Liquidation and backstop | Funded isolated reserve then explicit deficit halt; decide funding/incentives/loss priority. ADL/haircuts require explicit approval |
| D10 | V1-to-V2 transition | Parallel V1 claim service, opt-in settled transfer; no forced balance/order migration. Direct atomic migration requires a separate fully specified rule |

G1 after choices: freeze exact wire fields/tags/widths, proof limits, ordering,
amount/risk/timeout/committee bounds and positive/negative vectors. Pending
choices are not runtime-selectable defaults. Future activation date/height,
deployed-bytecode verification and geographic test infrastructure are later
gates, not requests to deploy during Stage 0.

**Stop now for specification review.** Proposed first bounded implementation
stage after approval: an isolated executable shared-spot reference model and
adversarial invariant fixtures, with no node/RPC/activation changes. Cover
exact-AssetId shared balances, competing cross-market reserves, partial fills,
cancel/replacement atomicity, quote-fee bounds and V1 migration accounting.
Preserve a separate futures ledger interface and test that no implicit or
unauthorized spot debit can occur through it; explicit authorized transfers
are the sole exception. This stage does not claim futures or BFT implementation.
Prerequisites: approval of the compact accounting decision sheet in the R1
accounting appendix, including its bounded D2/D6/D10 choices and exact vectors.
That approval does not select D1/D3, production bootstrap, futures economics
or deployed bridge compatibility. Undefined futures risk predicates fail closed.

Then individually approved/audited stages: (a) complete BFT model/codec and
restart adversarial proof; (b) PoS checkpoint adapter + isolated historical/
Ethereum compatibility fixtures; (c) shared-spot execution/custody integration;
(d) ordinary Qt/client proof path; (e) full approved futures/risk/oracle/
liquidation implementation; (f) transition/replay/claim preservation and WAN
qualification. A stage publishes exact local commit, tests including failures,
invariant evidence and limits. Do not automatically progress this roadmap.

Required adversarial review cases include hidden old commit signatures,
selectively delivered PreparedQC/CommitQC, dishonest view reports, omitted
highest proof, missing body, old-view delayed decision, disjoint/overlapping
partitions, permanent missing quorum, crash at every sign/apply boundary,
rollback/corrupt journal, conflicting finalized anchors, cross-market double
reserve, duplicate deposit/migration, pending withdrawal double credit,
stale oracle, insolvency and unbacked rewards. A passing happy path is not
stage approval. A missing proof segment must be reported, not filled in by
implementation convenience.

## 15. Source and research map

All repository anchors below are at the commit-pinned published baseline
[`0b930e303e4c2c6bc28beb3bf656488b636ad49d`](https://github.com/B3-Coin/B3-CoinV2/tree/0b930e303e4c2c6bc28beb3bf656488b636ad49d);
the R1 appendices provide direct file/line links for corrected rules. Historical prose
conflicts are recorded in the handoff. These targeted checks extend the
existing inventory; they are not a new full security audit.

| Boundary | Baseline source anchors |
|---|---|
| Market/custody identity, shared-ledger gap | `src/flowmesh/market.h:27`, `state.h:35`, `ledger.h:469` |
| Futures separation/reserved actions | `src/flowmesh/ledger.h:45`, `batch.h:52`, `batch.h:175` |
| Rounding/fees/rewards | `src/flowmesh/clearing.h:522`, `clearing.h:668`, `fee_allocation.h:180`, `fee_allocation.h:313` |
| User auth/replay/claim identity | `src/flowmesh/auth.h:43`, `state.h:162`, `batch.h:484`, `production_engine.cpp:386` |
| Custody/checkpoint/nullifiers | `src/node/flowmesh_vault_index.cpp:402`, `src/modern/flowmesh_checkpoint.h:62`, `flowmesh_checkpoint.h:485`, `src/node/flowmesh_checkpoint_index.h:45` |
| Persistent locks/preagreement | `src/node/flowmesh_production_store.h:87`, `src/flowmesh/production_engine.h:333`, `src/node/flowmesh_agreement.cpp:250`, `flowmesh_agreement.cpp:584`, `src/flowmesh/preagreement.cpp` |
| Ordinary client | `src/node/flowmesh_client.h:37`, `src/interfaces/chain.h:248`, `src/node/flowmesh_https.cpp:41` |
| Actual PoS / epoch set / forks | `src/modern/pos_v1.h:96`, `src/validation.cpp:2541`, `src/node/validator_set.cpp:14`, `src/node/blockstorage.cpp:290` |
| B3 finality / recovery / economics | `src/node/finality_tracker.cpp:72`, `src/modern/finality_certificate.h:103`, `src/node/finality_signing_policy.h:15`, `src/node/finality_signer_store.cpp:425`, `src/kernel/chainparams.cpp:277` |
| Bridge digest/lineage | `contracts/src/B3FinalityVerifier.sol:194`, `src/modern/finality_types.h:71`, `src/modern/chain_domain.h:15` |
| Bridge thresholds/proofs | `contracts/src/IB3FinalityProver.sol:13`, `contracts/src/BlsCertificateProver.sol:42`, `src/bridge/ethereum_calldata.h:119` |
| Bridge root/release/asset | `contracts/src/B3FinalityVerifier.sol:253`, `contracts/src/B3StakerBridge.sol:117`, `contracts/src/B3BridgeAssetId.sol:16`, `src/modern/withdrawal_tree.h:25` |

Research context, not selected dependency: the distinction between prepare
and commit and evidence-carrying view changes is grounded in
[Castro/Liskov, PBFT §4](https://www.usenix.org/legacy/publications/library/proceedings/osdi99/full_papers/castro/castro_html/node4.html).
A distinct candidate with explicit lock/round rules is
[Buchman/Kwon/Milosevic, Tendermint v3](https://arxiv.org/abs/1807.04938v3).
Neither paper supplies the B3 application validity, custody migration, bridge
adapter or crash-safe implementation proof automatically. Our proposed profile
and safety gates above must be audited on their own exact specification.
