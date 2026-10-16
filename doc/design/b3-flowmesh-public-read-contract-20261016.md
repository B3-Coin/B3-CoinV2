# Next bounded stage: public read isolation

Source baseline: `0d119032046da1e1dca557583cb691fd70299ee6`.
Status: **proposed contract for review; not implemented or qualified**.
No new consensus, signing authority, economic rule or successful-response schema
is selected here. This is not a substitute for the wider chain-facts/mutation
lease stage.

## Evidence and scope

The retained test-first service counterexample uses the same initialized
service, a genuinely certified/durably appended state, and a held real
`cs_main`. The runtime snapshot answers; the service snapshot wrapper does not
answer inside the one-second held-lock window. It then drains unchanged after
release. Its child exit 201 is intended red evidence, not qualification; see
the [overnight report](b3-flowmesh-redesign-overnight-20261016.md).

Removing that wrapper's chain check alone is insufficient:

- `FlowMeshService::{MarketStatus,MarketData,ClientSnapshotView}` overlay
  chain activation/reconciliation after reading the runtime view.
- `InstallMarket` holds the service registry mutex while acquiring `cs_main`.
  Its existing health publication can precede metadata initialization and is
  not an initialized public-read registry.
- `TradingApi::MarketResponse(MarketData)` in
  [flowmesh_client.cpp](../../src/node/flowmesh_client.cpp) calls `Enabled()` and
  `PendingCheckpointStatus()`; the latter can reach chain/store/seat state.
- `RemoteBackend::RefreshRead` marks successful replies fresh. A successful
  historical reply with an advisory UNKNOWN/paused string is not a no-freshness
  contract.
- Public `ClientCertifiedEntry` currently permits the runtime's historical
  store fallback. The existing `allow_history=false` mode is the narrow
  memory-only option, not proof that an older action never executed.

## Proposed compatibility-preserving first slice

1. Publish only fully initialized read handles and exact market metadata in
   a separate leaf-owned immutable registry. Installation, removal, Stop and
   reader retirement must have a reviewed lifetime proof: any callable runtime
   and all dependencies it may use outlive the reader. Existing early health
   IDs do not grant this property. No raw chain pointer crosses this boundary.
2. A successful state-bearing reply takes one coherent durably committed
   runtime head. Book, account and recent evidence follow that captured view
   and coherent prefix. Preserve the existing conservative cursor rule: an
   older cursor may redeliver hints but must not skip unseen state; the cursor
   itself is not certified as exactly that head. Health/catalog and explicit
   unavailable/uncertified status must remain possible without a certified head.
   Do not join fields from independently acquired newer snapshots or advisory
   generations.
3. Never wait on `cs_main`, the market writer, a blocking disk fallback or an
   installation-owned mutex while constructing this reply. Removing just one
   of these dependencies does not satisfy the contract. This is an ownership/
   dependency requirement, not a claim of lock-free allocation or scheduling.
4. If required current advisory facts cannot be obtained safely, refuse the
   reply through the **existing failure envelope**. A busy observation is not
   inactive, reconciled, no-pending-checkpoint, expired or nonexecution. Do not
   serialize UNKNOWN as false in the existing boolean fields. Legacy clients
   must take their existing failed-read path without a new freshness stamp.
5. Public recent-certificate lookup uses bounded retained evidence. An older
   missing retained record returns an explicit transient history-busy/unavailable
   result, preserving the original ActionId, inclusion/no-resubmit obligations
   and journal. Internal operator historical lookup remains separate.
6. Do not hold an API slot waiting for a blocked prerequisite. Keep admission,
   retries, work and memory bounded using the existing budgets; do not increase
   capacity to hide blocking. No polling or unbounded chain work is moved into
   a supposedly cheap publisher.

This first slice may promptly refuse a read during chain uncertainty. It does
**not** promise uninterrupted fresh trading or authorize signing on stale data.
Successful historical display, or a current-authority observation attached to
it, needs a separate reviewed consumer/version contract before implementation.
The default-null mutation-fence prototype cannot supply those production facts.

## Required falsification tests

- Hold real `cs_main` for several seconds on the same initialized certified
  service. Actual HTTPS snapshot/book/status/health paths must answer or refuse
  within a disclosed test bound; release/drain must preserve exact head/state.
  The test bound is not silently a production deadline.
- Block first-market installation and race read, Stop and replacement. No
  partial metadata, use-after-free, wrong market or installation-lock convoy.
- Exercise the real remote consumer: bounded failure does not renew freshness,
  lose a verified inclusion, change a signed original or trigger resubmission;
  normal recovery succeeds without a replacement signed action or economic
  resubmission. Subsequent ordinary reads remain allowed/necessary; this contract
  does not select new automatic polling.
- Race a new commit/reorg with response construction. No mixed-head reply or
  authority renewal from cached display; required mutation checks stay binding.
- Request an older nonretained certificate and saturate ordinary read lanes.
  No disk/market fallback, unbounded durable write, false EXPIRED or swallowed
  no-resubmit obligation; protected health admission remains bounded.

Retain failing-before traces and run the strict native profile plus affected
Qt tests on the frozen successor. Separately audit source and adversarial
cases. Do not describe functional passes as 200 ms, arbitrary-load availability,
Linux/TSan, bridge compatibility or production release qualification.
