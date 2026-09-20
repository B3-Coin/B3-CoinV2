# Stage 0 R1 — corrected specification review package

Status: **PROPOSED; RETURN FOR INDEPENDENT REVIEW. No implementation approval.**

## R0. Identity, preservation and scope

Original specification freeze: `f327bd2cc334282eec17a09c1d15d04f68b7f4c5`,
branch `design/flowmesh-v2-stage0`, document `doc/design/b3-flowmesh-v2-stage0.md`.
Original document SHA256:
`257a49ab34f056e02beb837f193e3f4c91676b08c016cdbb7ffc6dd3ff6fe814`.
That commit, branch tip and document blob are unchanged. The original
independent read-only audit is retained unchanged in the owner's preceding
review record. This file is its **new disposition**, not an edited or
purported verbatim copy of the audit.

R1 branch: `design/flowmesh-v2-stage0-r1`. Its exact frozen commit and patch
are reported at handoff, avoiding a self-referential commit hash in these
documents. Review the complete diff against the original freeze, not against
the much older V1 source baseline. No V1 implementation file is changed.

The single integrated candidate comprises:

1. [Main specification](b3-flowmesh-v2-stage0.md).
2. [Accounting semantics, decision sheet and worked vectors](v2-stage0-r1-accounting.md).
3. [PBFT-style candidate and authority/recovery rules](v2-stage0-r1-consensus.md).
4. [Read-only bridge verification and its evidence limits](v2-stage0-r1-bridge.md).
5. [Preservation, version routing and futures gates](v2-stage0-r1-transition.md).

Source evidence links use the inspected published commit
[`0b930e303e4c2c6bc28beb3bf656488b636ad49d`](https://github.com/B3-Coin/B3-CoinV2/tree/0b930e303e4c2c6bc28beb3bf656488b636ad49d),
or the exact release commits listed in the bridge appendix. No private
absolute source paths or signer/wallet material are required by this package.
Relative document links resolve within the frozen review revision. R1 is
prepared locally; this task does not claim it has been pushed to GitHub.
Publishing this branch later requires the appropriate authorization; a
GitHub-only reviewer needs that actual published revision, not a local URL.

## R1. Nine-finding disposition

"Rule supplied" means a deterministic **proposed specification rule**, not
approval, implementation, proof or regression qualification. A missing economic
choice is not a demonstrated exploit. Futures choices and deployed evidence
stay open rather than being marked resolved by additional prose.

| Audit finding | Missing deterministic rule | Genuine unapproved choice | Missing deployed/external evidence | R1 clarification | Gate blocked | Status after R1 |
|---|---|---|---|---|---|---|
| 1. PoS candidate validity | Branch-independent height/depth/root/authority validation, distinct from local readiness | P3 profile; retention of 20-block local export wait | No live readiness inferred; BR-EXT below still applies to actual export | C2 specifies pure ValidValue, checkpoint grid/skips, 12-depth supplied branch witness and local export-depth/old-lock gate | BFT application model, PoS adapter | Proposed predicate supplied; owner ratification, codecs and adversarial proof pending |
| 2. First authority/epoch boundary | Who authorizes B0, historical prefix, first context and subsequent snapshot/activation | D3 separation of producer/checkpoint authority, D10 transition; no mainnet H chosen | Actual outgoing authority viability and bridge lineage are not established by local source | C7–C8 define outgoing-authority transition QC, F/P/B0, committed checkpoint snapshot, producer separation; T1–T3 preserve prefixes and old claims | Bootstrap, historical compatibility, activation | Concrete proposal supplied; outgoing-quorum/fault assumptions, D3/D10 and tests remain gates |
| 3. P2 voting transitions | Current-view/scheduled-proposer/accepted NEW_VIEW/proposal/QC guards; import/handover transitions | D1 PBFT-style profile, FN cadence and bounded configuration | None needed for isolated protocol model; deployed committee evidence later | C1–C4/C6–C7 define guarded transitions and value identity independent of QC subset; no omitted-local-QC veto | Consensus-model implementation/qualification | Rules supplied for review; not a PBFT safety/liveness proof or selected protocol |
| 4. Valid stale journal | Integrity alone cannot prove freshness or exclusive ownership | Accepted stable-storage/rollback fault boundary; production fencing/anti-rollback profile | No production nonrollbackable witness or fencing service is verified | C5 separates ordinary crash durability from coherent rollback; detected/suspected restoration blocks signing | Restart/rollback, signer deployment/transfer qualification | Limitation made explicit; arbitrary rollback recovery NOT resolved or claimed |
| 5. Replacement/reservation | Immutable OrderId, residual quantity meaning, lifetime counters and persistent bound/residual recurrence | A-D2/A-D3/A-D4 | None for isolated arithmetic model | A2–A3 reuse conservative V1 staircase, reserve B0 minus actual revision spend, retain fee history; A8 has counterexample vectors | First accounting model | Deterministic proposal supplied; bounded owner approval and tests required |
| 6. Fees/event order | Deposit/import/user/clearing order; grouping, recipient context and integer remainders | A-D1/A-D4/A-D5 dust and ordering | Historical authority fixture is synthetic until integration | A4–A6 define phases, signed account sequence, per-AssetId global batch grouping and historical recipients | Model reproducibility/payout accounting | Deterministic proposal supplied; economics remain PROPOSED |
| 7. Futures cash/loss safety | Exact risk, funded PnL, withdrawal and deficit predicates | D7/D8/D9 contract/margin/funding/oracle/liquidation/backstop | No selected feeds/backstop or economic qualification | A7 and F1 define authorized atomic transfer interface and NOT_DEFINED fail-closed scope; retain shortfall vector | Futures implementation and full V2 release | OPEN intentionally; only bounded transfer mechanics can be modeled after approval |
| 8. Inbound bridge horizon | Exact unsupported update/mint behavior and a historical-safe pin/registry transition | Any later node-side pin/registry extension requires explicit approval | BR-EXT: current B3 setup/head plus remaining source/runtime and operational evidence | Bridge appendix verifies configurations, slot-minus-one distinction, timestamp freshness, nullifiers and public Ethereum observations; no pin edit | Two-way bridge qualification/activation | Source boundary verified; no universal timestamp outage asserted; transition not implemented and deployed B3 evidence remains open |
| 9. Catch-up versus export readiness | Sequential epoch evidence advancement without fake local signatures or old-lock loss | D5 evidence-driven signer-store extension; no arbitrary unlock | Own retained history/freshness and available compatible quorum are installation-specific | C9 specifies separate evidence cursor and guarded new export; broken lineage/expiry/orphan split remain separate blocked states | Adapter recovery qualification | Recovery candidate supplied; no claim existing V1 supports it or all old locks can recover |

The audit established no new conflicting-decision exploit under the intended
P2 reading. R1 does not convert its completeness concerns into claims of an
exploited production defect. In particular, a valid NEW_VIEW may override a
local preparation omitted from its report quorum, but never a conflicting
complete decision; demanding every local QC would add an unjustified veto.

## R2. Short owner-decision sheet

### Decision now: only the first isolated accounting model

Approve/amend the exact seven rows **A-D1 through A-D7** in
[accounting §A9](v2-stage0-r1-accounting.md#a9-compact-accounting-approval-sheet)
at the final R1 revision. They settle global event ordering; residual-quantity
replacement; conservative curve backing; fee grouping/dust/historical payouts;
sequence/idempotence; segregated V1 accounting; and fail-closed futures-transfer
mechanics using visibly synthetic fixtures.

This does not approve real fee recipients, production limits, a live oracle,
futures risk, bootstrap authority, migration, BFT implementation or deployment.
The proposed tests are [A10](v2-stage0-r1-accounting.md#a10-proposed-model-acceptance-tests).
No model was implemented or started during this revision.

### Decisions later, still explicit

| Decision | Proposed direction / unresolved choice |
|---|---|
| D1 | Ratify the specified PBFT-style prepared/commit profile only after separate safety/liveness review; no automatic HotStuff substitution. Approve deployment freshness assumptions separately. |
| D3 | P3-R1 separates checkpoint authority from producer snapshots. Eligibility/reward arithmetic stays V1, but the handover linkage/snapshot boundaries change as explicitly specified. Exact preservation of one shared epoch tracker is NOT simultaneously promised. |
| D4 | No automatic inactivity removal, weight decay, slashing or lower quorum selected. |
| D5 | Preserve unchanged Ethereum verifier envelope/lineage; review evidence-driven returning-signer logic. Outgoing transition authority must remain within the approved fault bound and possess a usable quorum at activation. |
| D7–D9 | Select actual futures contract/margin/funding/fee/oracle/liquidation/backstop and loss priorities. A synthetic transfer adapter supplies none of these. |
| D10 | Ratify B0 authority and T2 routing, V1 claim continuation and settled-withdrawal/new-deposit transition. Old split-locked funds are not newly spendable. No H is selected here. |

## R3. Bridge evidence boundary

Use the bridge appendix as the single detailed evidence record. It separates:

- inspected code and exact released Git revisions;
- retained deployment manifest from public pinned-block runtime/getter reads;
- runtime-to-manifest hash equality from reproducible source/compiler/immutable
  equivalence;
- first unsupported header slot from signature-slot validation and B3
  execution-timestamp mint freshness;
- Ethereum contract deposit/release predicates from actual B3 mint readiness;
- a proposed future historical-preserving extension from currently supported
  node behavior.

**BR-EXT:** the exact remaining evidence is listed once in
[the bridge appendix](v2-stage0-r1-bridge.md#br-ext-exact-remaining-evidence-and-qualification-gate).
No missing datum is replaced with a claim of a live outage or live safety.
Public getters alone cannot prove the availability of the necessary private
signing history or that an old conflicting certificate was never issued.

## R4. Review and handoff limits

The worked vectors are specified expected outcomes, not a newly implemented
model's pass report. Authoring checks may verify arithmetic, Markdown links,
publication-sensitive strings, source-only references and documentation scope;
they do not qualify BFT, production power-loss recovery, futures, migration,
WAN latency or 200 ms execution. The read-only bridge evidence is separately
dated and bounded in its appendix.

Original frozen specification and audit remain authoritative records of the
earlier stage. This revision does not erase their findings. The review should
challenge R1's newly explicit proposals, including the capital-efficiency
tradeoff, fee dust incentives, producer/checkpoint separation, bootstrap trust,
unknown old signatures and rollback fault boundary, rather than assume more
detail equals approval.

Stop after delivering this package. No source implementation, live wallet or
signer action, pin change, real activation height, migration, contract write,
merge, deployment or publication follows from this document.
