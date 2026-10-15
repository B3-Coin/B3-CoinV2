# B3 and FlowMesh redesign plan

Date: 2026-10-15. Status: **PROPOSED FOR REVIEW**. Planning baseline:
`flowmeshV2-dev` at `74bd4e5e7e5f13c885be57f32e760927b94ef382`.

Build a purpose-designed trading system around useful Bitcoin foundations,
preserving B3 history and existing obligations. Replace unsafe coupling in
reviewable stages rather than restarting the project or only changing its UI.
The target remains a 200 ms median client-verified trading result, with real
durability and cryptography, first on isolated regtest.

This plan authorizes no implementation, activation height, live migration,
contract deployment, VPS change, public release or push. A completed stage
needs its own frozen revision, evidence and separate review before progression.

## 1 Requirements and current evidence

### Agreed product direction

- V2 includes both spot and futures. Spot is the first implementation slice,
  not the entire release.
- Genuine ordered base and quote AssetIds; one shared spot balance per account
  and exact AssetId across markets; atomic cross-market reservations.
- Deposits credit spot first. Only explicit authorized transfers move funds
  between spot and futures. Futures cannot automatically debit spot.
- Trading fees and corresponding rewards use approved stable AssetIds. Native
  B3 network fees are a different charge.
- Safe agreement, leader failure, conflict, view-change, membership and restart
  recovery; B3 PoS V2 improvements remain required.
- Ordinary Qt trading with validator mode off, with a clear exchange-style UI.
- Preserve the recorded 30-60-minute external bridge deposit/withdrawal
  objective as a route-specific target, conditional on the approved finality,
  relay, custody and availability assumptions. It is separate from trade latency.
- Preserve existing chain history, funds, reservations, pending claims and
  signing obligations. A new protocol cannot invalidate previously usable
  signatures merely by ignoring their journals.
- Regtest qualification precedes any mainnet activation decision. A hard fork
  is allowed in design, not scheduled by this plan.

### What the baseline establishes

The running tester product is a V1-based native spot path with later client,
networking and recovery improvements. Its asset/B3 representation is not a
generic V2 shared ledger. The current Futures tab is informational. The
accounting, PBFT-style agreement and SQLite recovery models are useful tested
references with declared synthetic boundaries; they are not deployed V2.
An uncommitted native spot prototype remains separate, provisional work.

The observed timeout path is B3 validation holding `cs_main` during finality
processing, while FlowMesh holds its market mutex waiting for that chain lock;
HTTPS readers then wait for the market mutex. The finality tracker marks itself
dirty on a disconnect and can rebuild from the modern-era boundary. A retained
regtest journal excerpt shows a 97-second tip-update gap around a shallow reorg;
it does not measure the exact duration of each function or the replay span.

This implicates custom B3/FlowMesh state maintenance and shared-lock coupling,
not Bitcoin's signature or serialization primitives. Adding threads without
changing those dependencies is insufficient. Narrow repairs at the baseline
do not resolve the complete wait chain. Two reproduced native timing-test
failures also remain a qualification gap, not a clean suite pass.

## 2 Components to keep and boundaries to rebuild

Reuse means retaining a component's tested behavior where suitable, not
declaring the new integration audited. Pin dependency versions, licenses,
build options and security updates in each reviewed milestone.

| Area | Keep and adapt | Rebuild or specify for V2 |
| --- | --- | --- |
| Chain history and wallet | Historical block and transaction decoding, UTXO validation, wallet ownership, backups, addresses and existing keys | Versioned modern-state transition and authoritative cutover; no new genesis or duplicated funds |
| Cryptography and codecs | Existing secp256k1, hash and bounded serialization utilities; existing blst integration as a separately reviewed dependency | Exact V2 domains, canonical encodings, proof and certificate rules; never reinterpret V1 signatures |
| Transport | Useful Bitcoin peer/socket infrastructure, the independent FN network, CA/hostname/pin verification and verified TLS connection reuse | Bounded priority lanes, request outcomes, recovery traffic and explicit admission backpressure; no automatic economic replay or TLS trust bypass |
| Storage | Tested LevelDB/SQLite and filesystem primitives where their semantics fit; exact-action outbox and retained failure evidence | Versioned journal/snapshot layout, bounded recovery, durable commit/application and rollback fencing; library choice alone supplies none of these guarantees |
| Trading | Accounting invariants, integer arithmetic, conservation checkers and useful matching utilities | Generic asset pairs, shared balances, reservations, native order lifecycle, stable-asset fees and futures state |
| Consensus and finality | Existing models, test vectors, pin enforcement and counterexamples | Complete approved BFT state machine, producer/finality authority, membership and quorum-loss behavior; bounded derived-state recovery |
| Qt | Wallet integration, working chart/book components, exact amount formatting, receipt protections and regression tests | Small controller and typed view models; independent command/read work; genuine pair semantics, funding lifecycle and stable readiness |
| Bridge | Proof-validation and contract/vector work that passes its own review | Explicit V2 authority, route identities, mint/burn accounting, finality and idempotent relay interfaces; no hidden signer-minting fallback |

Use the existing C++20 foundation initially. A language change, new database or
new network library needs evidence of a particular benefit; none is a shortcut
to safe finality or 200 ms trading.

## 3 Proposed execution architecture

### Ownership and concurrency

Split the modern system into modules with typed messages and explicit ownership.
Use bounded queues and independent workers; add process isolation where the
failure boundary justifies it. Do not require a new microservice fleet to prove
the first working slice.

| Module | Responsibility | Isolation rule |
| --- | --- | --- |
| B3 chain and finality | Chain validation, producer eligibility, finality, authenticated custody facts and membership transitions | Exact recent undo and bounded work; no history-sized rebuild on every shallow reorg |
| Admission and verification | Bound/authenticate original actions and queue accepted work | Reserved capacity for orders, cancels and status; overload is explicit and bounded |
| Trading state owner | Apply one deterministic ordered batch to shared balances, reservations, orders and segregated futures cash | One authoritative writer initially; parallel workers cannot independently spend the same account balance |
| Agreement | Propose and certify exact batches and results under the approved BFT protocol | Retain locks and recovery evidence; leader replacement never erases an issued obligation |
| Durable store | Persist signing evidence, exact signatures, decisions and application records | A write may run on a storage worker, but signing/publication/finality waits for the required durable completion |
| Read service | Serve immutable certified snapshots, book depth and bounded verified updates | Reads do not take the trading writer's lock or wait behind chain replay; stale display is labelled, not fresh spending authority |
| Client and signer | Capture user intent, sign locally, retain originals and independently verify results | Keys never go to a trading endpoint; reconnect does not create or sign a replacement action |
| Bridge manager | Acquire proofs, submit authorized relay work and reconcile exact transaction identities | A bridge outage does not consume chart/order workers or create mint/payout authority |

Do not hold a shared trading/read lock or cross-module lock while waiting on
network, chain access, another module's disk completion or a callback. Retain
exclusive signer-store ownership and the database transaction locks required
for atomic durability. An asynchronous pending write remains an ordered,
version-checked transition: releasing a shared lock cannot permit another writer
to reorder it, publish it early or spend its funds. Account-level economic
ordering remains deterministic while parsing, verification and display run in
parallel.
Read traffic also needs memory/CPU/I/O budgets: a separate thread alone does not
prevent resource starvation. Never create an unbounded thread or queue per user.
Visual updates may coalesce to the newest verified head, but economic outcomes,
original requests and indispensable signing/recovery evidence are never dropped
to make the screen or benchmark faster.

### Authority must be specified before wiring

The proposed boundary is a fast certified trading log consuming explicitly
authenticated B3 facts, with a versioned B3 checkpoint/custody adapter. Ordinary
tip growth must not automatically invalidate a still-valid finalized anchor or
force every chart read through the latest tip. The exact finality, freshness,
membership and anchor-update rules must establish when that remains valid.

Decision R-D1 must compare this anchored design with a unified modern B3/trading
commit pipeline before native consensus integration. The choice determines who
can finalize trades and authorize withdrawals; it cannot be decided incidentally
by a cache, timeout or first-arriving callback. Producer selection and checkpoint
authority must both be addressed: keeping current producer election is not, by
itself, completion of PoS V2.

Chain reads distinguish **available facts**, **temporarily busy**, and
**invalid/unavailable evidence**. Busy retains exact work and defers safely; it
must not be interpreted as an orphaned anchor, missing deposit, permanent signing
conflict or permission to use stale authority. A cached `ready=true` or online
peer count cannot authorize signing. If authority becomes uncertain, affected
writes pause explicitly while read/status service stays responsive.

### One commitment path and durable recovery

Choose and fully specify one BFT protocol rather than stacking independent
approval rounds without a defined purpose. The existing PBFT-style model is a
reference; a pipelined HotStuff-family alternative is a candidate, not already
selected or implemented. Stable coordinator tenure and replacement policy are
also proposals until covered by the chosen state machine and tests.

Specify proposal/vote meaning, domain, epoch/view/sequence, parent, prior/new
state roots, original ActionIds, outcome/effect commitments, safe voting and
locking predicates, commit rule, NEW_VIEW evidence, membership/bootstrap,
restart, hidden certificates and quorum loss. No timeout may lower quorum or
clear signing history.

Required ordering: durable intent/evidence before signing; durable exact signed
object before publication; durable decision before reporting finality. Record
the accounting application and completion so restart applies a committed batch
exactly once. Publish a new certified read snapshot only after the corresponding
durable commit/application boundary. A storage error produces an explicit safe
state, not an invented successful receipt. Known restored-old stores fence
signing; coherent old backups need an external trusted freshness mechanism.

## 4 Trading and funding semantics

Use exact `AssetAmount{AssetId, atoms}` and a versioned market descriptor binding
base/quote AssetIds, decimals, lots, ticks, fee identity and protocol version.
For `B3/rUSD`, quantity is B3, price is rUSD per B3 and total is rUSD throughout
the ticket, book, chart and receipt. Tickers and inverted labels never redefine
an existing market or signed instruction.

Shared spot reservations, withdrawals and spot-to-futures transfers compete
atomically for the same available asset. Fill, fee debit, reservation update,
recipient credit and outcome record are one deterministic transition. Replacing
or cancelling an order releases only that order's exact unused backing. Exact
retries return the retained outcome; they do not consume funds twice.

Matching decision R-D3 must choose whether V2 retains the curve auction or
adopts a conventional price-time order book. A price-time book with independently
identified orders and a bounded IOC market-order mode is the proposed usability
direction, not an approved replacement of auction rules. Reuse conservation
tests across either choice; do not assume their fill-price results are identical.
Market orders need explicit slippage/spend bounds and atomic cancellation of any
unfilled remainder. Until that exists, show Limit or honestly labelled
marketable-limit orders, not a fictitious Market button.

Stable-quoted pairs are the first integration profile. Non-stable-quoted pairs
remain part of V2 scope but require an approved stable-fee valuation and failure
rule before that path ships. Fee floors, fractional dust, FN reward ownership,
rounding/remainders and rates are explicit manifests, not copied silently from
synthetic tests. Funds collected in one AssetId cannot pay another AssetId by
renaming it.

Funding has a first-class lifecycle:

- Deposit: reviewed, broadcast, confirming, eligible, certified credit, spot
  available. Native B3 deposited into custody must cease being wallet-spendable;
  pending and certified credit must not both count as spendable funds.
- Withdrawal: reserved, certified, settlement pending, payout broadcast,
  confirmed. Keep the backing reserved until the applicable payout/refund rule
  settles it. FlowMesh finality is not a B3 or external-chain payout.
- Spot/futures: an explicit signed amount, exact asset, account/subaccount and
  configuration. Futures-to-spot requires approved withdrawable collateral;
  unknown risk refuses transfer-out. PnL is not additional custody backing.

## 5 Qt trading workspace

Keep Qt Widgets for the first redesign. Existing native components, packaging
and tests already use it; a QML replacement is optional after measuring a real
benefit. It is not a prerequisite for correct pair semantics or responsive
trading. Keep the dependency version pinned to the qualified build, with any
upgrade reviewed separately.

Extract the active large trading panel into a session/controller, a pure
request/state reducer, command worker, independent read-update worker and typed
models for markets, balances, orders, fills, funding and future positions.
Widgets render models and emit intents. Networking, proof verification, wallet
unlock/signing and large state copies do not run on the GUI thread. Apply model
updates on the owning GUI thread through queued delivery, as required by
[Qt's model threading rules](https://doc.qt.io/qt-6/qabstractitemmodel.html#thread-safety).

The existing preview backend is not the active trading backend. Extract from
the working panel and retain its wallet/generation/head attribution, exact
formatting and saved-action protections. The node/client remains the durable
outbox owner; do not create a second UI signing journal. A separate daemon/Qt
process boundary can isolate crashes, but needs authenticated local IPC and
cross-platform lifecycle tests before adoption; do not expose admin RPC publicly.

### Screen layout

```text
Network and account     Spot | Futures     Connection and freshness
Market B3/rUSD          Price and statistics          Shared balances
Chart                  Full order book              Buy / Sell ticket
                       Recent trades                 Limit / Market when supported
Open orders | Fills | Balances | Funding | Requests
```

Show prices consistently as quote per base, quantities as base and totals/volume
as quote. Use time-based candles with explicit time provenance; never invent
times for old records or turn operator-reported candles into verified prices.
Preserve pan/zoom, keyboard focus, the typed draft and selection across updates.
Endpoint setup and raw checkpoint tools belong in Connection/Advanced, not the
ordinary order ticket.

Example: **Buy 10 B3 at a limit of 2 rUSD/B3** means a maximum principal of
20 rUSD, plus the disclosed fee bound. Partial fill and actual fees appear
separately. Available, reserved, pending and futures collateral are distinct;
one shared rUSD balance appears consistently in every market.

A single connection indicator distinguishes Ready, Reconnecting, Validation
busy and Safety paused. Opening/editing a draft or review can stay usable while
refreshing; final submission checks current validated authority, funds and the
captured review context. An unrelated chart failure does not erase an order
receipt or repeatedly toggle the entire workspace. An uncertain economic
request still blocks conflicting new instructions where necessary.

Request states distinguish signed/saved, possibly sent, admitted, certified
inclusion, verified applied/open, partially filled, filled, cancelled and
verified rejection. A timeout means **outcome unknown**, not failed. Status
reads can recover automatically; economic resends remain explicit and exact
byte only unless a separately approved retry policy specifies otherwise.

Futures gets a real positions/margin/funding workspace after its risk model is
qualified. Show collateral, equity, withdrawable amount, oracle health and
liquidation state separately. An informational tab does not satisfy V2 futures.

## 6 Performance contract

Retain historical [M1](../flowmesh-latency-definition.md) unchanged: original
order RPC invocation to client-verified inclusion. It excludes a complete fill
outcome and Qt rendering. Do not rename old measurements to stronger results.

The proposed redesign profile adds:

| Metric | Start and finish | Proposed gate |
| --- | --- | --- |
| R1 order result | Before the original signing preflight through a durable client receipt proving that action's applied/open result | Co-located p50 <= 200 ms, p95 <= 600 ms under the frozen nominal load |
| R2 matched fill | Same start through durable client verification of the actual fill, exact fee and balance deltas; crossing maker liquidity preexists | Same local target, measured separately from resting orders |
| R3 visible result | Client-verified result to corresponding Qt model/view installation or paint | Provisional p95 <= 30 ms; preserve existing UI harness distinctions |
| Recovery | Fault injected to safe convergence/catch-up or explicit quorum-loss state | Bound per declared fault/network/storage profile; not a 200 ms claim |
| WAN | Same R1/R2 boundaries on independently hosted regtest validators/client | Measure separately; topology/RTT/load gate requires approval |

Applying the retained 200/600 ms inclusion target to the stronger R1/R2 result
boundaries is a proposed acceptance contract. No 300 ms or 700 ms WAN guarantee
follows from a local measurement. External deposits, B3 settlement and external
withdrawals have separate finality clocks, not the 200 ms trading target.
External bridge transfers retain the 30-60-minute objective under the approved
route's declared source finality, relay/capacity and payout-funding conditions.
Expired lineage, missing quorum or invalid proofs require safe pauses, not an
unsafe fast payout. Report each route and outage separately; anvil/mocked
finality cannot establish real external-chain elapsed time.

Freeze a versioned profile before timing: exact commits/binary hashes,
dependency/build flags, CPU/RAM/storage/OS, full durability mode, committee and
fault assumptions, topology/RTT, book depth, markets/accounts, fees, offered
load, warmup policy, seeds and deadlines. Include the engine-off client and
real cryptography/durable writes. Four separate processes on one host are a
local concurrency test, not independent failure domains. Qualify a declared
ordinary-PC profile as well as the current powerful Mac; select its exact
hardware and load before accepting performance, not after seeing results.
A candidate reference profile is eight logical CPU cores, 16 GiB RAM and an
SSD, with four operator processes and an engine-off client on one host. This is
a proposed test requirement, not a claim about an available machine or its
speed. Independently hosted VPS and WAN profiles are frozen separately.

Start with one actual matched fill and an eight-request burst; then at least
1,000 measured samples for each identically configured headline workload.
Separate cold/warm sessions, healthy/fault, acceptance/fill, serial/open-loop
load and local/WAN distributions. Predetermine rate ramps, stop bounds and
book-depth arms. Record offered/admitted/completed/rejected/timed-out counts,
queue growth, throughput, p50/p95/p99/max and actual process exits. A sustainable
load needs bounded queues and correct completion, not fast surviving requests.
Retain the original clock across exact retries and all failures in the request
denominator; deliberate invalid requests do not improve the successful-fill
percentile. Use nonblocking tracing after setup, with a matching uninstrumented
control; do not add overlapping stage durations or combine unrelated medians.
The nominal-load gate fails on unexplained refusals, lost/duplicate outcomes or
requests still unknown/timed out at the profile deadline; a fast successful
subset cannot pass it. Deliberate overload and injected-fault campaigns report
their expected backpressure/pauses separately, with bounded recovery when the
declared quorum, delivery and storage conditions return.

The isolated native ~79 ms result is feasibility evidence for its fixed-leader
synthetic profile only, not today's Qt/full-node performance or a V2 release.
[Hyperliquid's published median](https://hyperliquid.gitbook.io/hyperliquid-docs/hypercore/overview)
is also explicitly co-located and measured send-to-committed-response; it is
not automatically the same boundary as the stronger receipt above.

## 7 Staged implementation and review gates

Stage numbers below are this redesign roadmap, not replacements for frozen
historical milestone identities. Continue on the single `flowmeshV2-dev`
branch; preserve all old commits, model branches and unrelated work. Make
small local commits during an approved stage. Publish only an approved completed
milestone, with no PR unless the owner changes that workflow.

| Stage | Deliverable | Required exit evidence |
| --- | --- | --- |
| D0 integrated contract | Reconcile existing specifications and later owner rulings; select authority topology and exact protocol/units/interfaces; freeze B3 checkpoint/custody and external verifier acceptance requirements before dependent signing/commit rules; freeze profiles and unresolved choices | Separate specification and compatibility review; no native protocol integration from unresolved rules |
| D1 boundaries and UI prototype | Typed busy/invalid states, dependency/lock ownership rules, bounded worker/read interfaces; offline Qt controller/models with existing chart/book components | Deterministic blocked-worker tests, no false freshness or lost originals; prototype visibly synthetic and unfunded |
| D2 chain and ledger foundations | Bounded exact finality/stake/binding/key recovery; native shared spot balances, generic pairs, reservations, actions and accounting | Compare shallow reorg/restart results against full replay; conservation, overflow, duplicate and cross-market race tests. No signer journal rollback |
| D3 agreement and durable store | Approved BFT, actual crypto, disk-backed intent/signature/commit/application and native recovery | Separate-process faults, hidden/late certificates, membership and protected-evidence admission; independent checker over every issued signature |
| D4 first complete regtest slice | Generated test assets, real B3 custody facts, four operators, one engine-off Qt wallet, one stable-quoted market and preloaded test liquidity | Deposit -> spot credit -> real order -> actual matched fill -> cancel -> genuinely authorized versioned B3 custody withdrawal/payout, not a synthetic bridge authority; keep data/orders across restart. One-request/burst timing with blocks advancing |
| D5 usable spot and performance | Complete Qt spot workflow, multiple markets, shared balances, selected matching/order types, stable-fee rewards, verified incremental updates | Full headline samples/load ramps/deep books with refresh traffic; Windows/macOS correctness and UI responsiveness; held chain/market locks cannot consume every status/HTTPS slot |
| D6 futures | Approved contracts, margin/oracle/funding/liquidation/backstop, segregated balances and positions UI | Native conservation/liability, insolvency, stale oracle and transfer-out refusal tests; no automatic spot consumption |
| D7 bridge and cutover | Implement/qualify the D0-reviewed V2 custody/verifier/relay interfaces, original obligations and authoritative V1/V2 transition | Local anvil+B3 round trips and migration rehearsals, no duplicate credit/payout, retained signature/claim history, pinned compatibility evidence and a route-specific 30-60-minute latency validation plan with external assumptions |
| D8 distributed qualification | Independently hosted regtest validators/testers, signed/checksummed platform artifacts and soak/fault campaigns | Proposed minimum 72-hour soak plus all safety/recovery gates; actual CI for the exact candidate; external review. Testnet bridge trials require separate approval |
| D9 mainnet decision | Final security review, operator/client rollout, capacity/custody reconciliation and fork rehearsal | Only then approve a specific activation height/hash/ruleset and deployment. No live value from regtest results alone |

D1's offline UI work can run alongside specification refinement. D2/D3 cannot
wire consensus, asset authorities or economic rules that D0 has not resolved.
Early D4/D5 spot experiments are labelled incomplete V2; a full V2 release
requires D6 futures and the required D7 bridge/cutover work too. No mainnet
activation is selected to satisfy a delivery date or a benchmark.

## 8 Regtest qualification matrix

Create new disposable local test namespaces with generated keys/coins and
explicit network/profile identities. Keep the existing tester datadirs, orders
and signing journals unchanged. Separate mainnet/regtest directories, ports,
peers, TLS trust, key stores and packaging. Default-off experimental rules must
refuse mainnet/testnet use, not depend on a user noticing a badge.

Regtest uses production code paths for cryptography, signatures, proof checks,
durability, custody, staking and recovery. Cheap funding and shorter test epochs
are disclosed test conveniences; also exercise production-like epoch/age/limit
settings. Do not use instant confirmations, reduced quorum or disabled fsync to
qualify the trading headline. Use four- and seven-member declared fault profiles
where applicable; these are test rosters, not selected mainnet economics.

| Campaign | Required failures and invariants |
| --- | --- |
| Accounting | Two markets/withdrawal/spot-futures transfer competing for funds; partial fill/replacement/cancel; exact retries and conflicting same-sequence instructions; dust/overflow; ownership as well as aggregate conservation |
| Funding | B3 wallet spendable debit, pending credit, final spot credit, asset precision/identity, repeated deposits and consumed receipts; payout/refund exactly once |
| Agreement | Leader loss, withholding/equivocation, hidden/late certificates, partitions, changing membership, slow/returning validators and sparse final requests; no forced unlock or quorum reduction |
| Storage | Kill before/after every durability/signature/publication/application boundary; truncated/corrupt/unavailable/failed flush; duplicate store ownership and known rollback fencing. Process kills are not physical power-loss qualification |
| Chain | Continuous tip growth, shallow/deep reorgs, replaced anchors, epoch boundaries, long outage restart, fresh sync/reindex and stale callbacks; B3 and FlowMesh authority agree. Work counters expose historical replay rather than only timing it |
| Resource pressure | Unsolicited DATA/OFFER/header traffic, delayed timers, bounded scan/retry/catch-up work, slow disk, blocked chain locks and display floods; retain indispensable evidence, refuse disposable work without permanent signing halt |
| Client and Qt | Timeout before/after admission, exact retained ActionId, reconnect and stream gaps, old/equal-height conflicting heads, wallet/market switch and quit during reads; no duplicate execution, unsafe auto-resend or fresh label on stale evidence |
| Futures | Explicit transfers, insufficient withdrawable collateral, loss allocation, bankrupt counterparty, oracle staleness/manipulation, liquidation/backstop exhaustion and funding gaps |
| Bridge | Route/chain/contract/AssetId binding, finalized receipt proofs, fork-range/freshness, double mint/burn/payout, conflicting certificates, relayer crash/conflict, sponsor exhaustion and already-paid receipt retries |
| Cutover | Authoritative initial state/sets on competing preactivation branches; preserve balances, orders, reservations, deposits, pending withdrawals, consumed receipts, rewards, original client instructions and signer history; never spend in both versions |

Use anvil and deterministic beacon/receipt fixtures for local bridge proof
coverage; keep mock-authority tests separate from actual verifier tests. Local
EVM success does not prove deployed bytecode/configuration equivalence, real
USDT/USDT0 behavior or external-chain finality. Those need their own pinned
evidence and, where authorized, testnet qualification before production.

Windows and macOS must run the ordinary engine-off Qt path, not just compile.
Test practical 1280x800 layout, 100/150/200 percent scaling, keyboard/accessibility,
long asset names, wallet switching, metadata and reconnect states. An unresolved
wallet/Qt crash remains a tester-release blocker. CI success is not an
independent security review.

## 9 Choices that need ratification

| ID | Choice and proposed direction | Blocks |
| --- | --- | --- |
| R-D1 | B3/FlowMesh authority topology, full BFT variant, producer election/fork choice, membership, signing domains and recovery. Prefer explicit finalized-input boundaries and one well-defined commitment path; compare anchored trading with unified finality before selecting | D2/D3 protocol integration and activation |
| R-D2 | Finality cache/undo bounds, store schema, durability barriers, snapshot authority and external anti-rollback/fencing mechanism. Exact recent undo with safe rebuild/refusal fallback; no journal deletion | Native signing/storage qualification |
| R-D3 | Matching priority, independent order slots/IDs, replacement semantics and market/IOC slippage policy. Conventional price-time book is the proposed usability direction; current auction remains the baseline until approved | Native matching and corresponding live Qt ticket |
| R-D4 | Stable fee AssetIds, non-stable-quote valuation, rates, rounding/dust and FN/treasury recipient allocation. Keep historical ownership and explicit manifests; do not elevate model assumptions to defaults | Fee/reward production paths |
| R-D5 | Futures contract/collateral model, margin/leverage, oracle, funding, liquidation, loss allocation/backstop and withdrawable-cash rule | D6 futures; full V2 release |
| R-D6 | Authoritative bootstrap/cutover, old obligations and bridge lineage/expiry compatibility. Preserve each old claim once and admit new state only through the approved transition | D7 migration and all activation |
| R-D7 | Exact reference PC/VPS hardware, nominal load/depth, stronger R1/R2 result gate, UI targets, WAN/RTT target and soak profile | Performance/release acceptance, not offline functional development |

The [later bridge owner-ruling record](b3-bridge-v2-owner-rulings-2026-10-03.md)
permits new V2 contracts and describes one Ethereum-USDT-backed bUSD, Arbitrum
routing through USDT0/LayerZero, no LP and no signer-attested minting at launch.
It also keeps native-B3 network fees with sponsorship, distinct from stable
trading fees. Reconcile older Stage0 unchanged-verifier text against those
decisions; do not silently add a direct Arbitrum vault or substitute an LP.
Bridge route trust/finality/cost, issuer freeze risks and external compatibility
remain explicit security properties. Ethereum light-client transport replacement
stays deferred; supported-fork range, freshness and proof validation do not.
Rehearse a pre-activation abort separately from post-activation forward repair.
Rolling a binary back after V2 has created obligations must not resurrect V1
spending authority or erase new finalized history.

## 10 First bounded deliverable

Complete D0 before protocol implementation: one reconciled architecture contract,
typed interface/ownership map, the exact consensus selection comparison,
versioned test/latency profiles, B3 checkpoint/custody and external verifier
acceptance mapping, and an owner decision sheet for R-D1 through R-D7. D7 is
not the first compatibility check: D0 must expose any incompatible digest,
lineage, threshold, commitment or proof before dependent signing code begins.
Use the existing accounting/agreement/storage models and current incidents
as acceptance tests, not a reason to erase prior work. An offline D1 Qt prototype
can make the proposed trading experience tangible without live funds or keys.

Freeze that deliverable for separate review. No current tester-network reset,
VPS update, mainnet operation or unapproved economic rule follows from planning.

## 11 References

- [Original integrated V2 specification](b3-flowmesh-v2-stage0.md),
  [R1 accounting](v2-stage0-r1-accounting.md),
  [R1 consensus](v2-stage0-r1-consensus.md) and
  [transition preservation](v2-stage0-r1-transition.md). These remain historical
  references; newer choices require identifiable successors.
- [Accounting model milestone](../../test/flowmesh_v2_model/MILESTONE1.md),
  [agreement protocol](../../test/flowmesh_v2_agreement/PROTOCOL.md) and
  [disk recovery scope](../../test/flowmesh_v2_storage/README.md).
- [Native probe limits](../../test/flowmesh_v2_fastpath_native/PROFILE.md) and
  [full-node integration gates](../../test/flowmesh_v2_fastpath_native/INTEGRATION_GATE.md).
- Current coupling: [runtime](../../src/node/flowmesh_runtime.cpp),
  [service](../../src/node/flowmesh_service.cpp),
  [finality tracker](../../src/node/finality_tracker.cpp) and
  [validation](../../src/validation.cpp).
- Active Qt: [trading panel](../../src/qt/b3flowmeshtradingpanel.cpp),
  [order book](../../src/qt/b3flowmeshorderbook.cpp),
  [chart](../../src/qt/b3flowmeshchart.cpp) and
  [current market representation](../../src/qt/b3flowmeshtrading.h).
- [Latency definitions and existing UI targets](../flowmesh-latency-definition.md).
  [Qt queued cross-thread delivery](https://doc.qt.io/qt-6/threads-qobject.html#signals-and-slots-across-threads)
  supports the proposed GUI-thread ownership boundary. QML is not a promised
  speedup; [QQuickWidget has additional rendering costs](https://doc.qt.io/qt-6/qquickwidget.html#performance-considerations).
