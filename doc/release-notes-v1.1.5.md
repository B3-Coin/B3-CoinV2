# B3 Hive v1.1.5 — development draft

Status: `1.1.5-dev`. No v1.1.5 tag, published release, activation height or
production wallet build is implied by these notes.

## Included in the development branches

- Asset display metadata and selected-wallet Asset Send/Receive improvements
  already committed on master after v1.1.4.
- Ethereum-to-B3 relayer queue scheduling reconciles finalized effects before
  selecting a freshly verified sync sequence. Superseded unprepared plans
  remain in the audit history; in-flight transactions and deposit dependencies
  are preserved.
- Once sufficiently caught up, the relayer can scan deposits against its
  exact B3-finalized Ethereum store before adding an optional tip refresh.
  Existing prepared deposit plans are not starved by continuous refreshes.
- Authenticated Ethereum history scanning uses bounded read-only batches and
  a bounded persistent header cache. The configured production default is
  three headers per batch, with an independent hard cap of sixteen.
  Cached headers are rehashed and parent-linked from the currently verified
  anchor. Partial header walks never advance the deposit scan cursor.
- Focused regressions cover queue ordering and preservation, finalized-store
  scheduling, malformed RPC batches, cache corruption, interrupted scanning
  and restart behavior.

These changes do not lower finality quorum, remove validators, replace the
Ethereum verifier, change the transaction/certificate format, reset a signer
journal, or increase a relayer's configured fee limits. Preserving the existing
relayer database between runs remains mandatory.

## Not implemented or approved by this preparation

- Automatic removal of inactive validators. The future-epoch proposal still
  requires an agreed, consensus-verifiable activity definition, observation
  window, re-entry policy, activation/migration plan and safety review.
- A current-set or already-committed-successor override. Ordinary handover
  still requires the existing authority's quorum. A smaller set is not an
  emergency shortcut around a missing current quorum.
- The separate, unfinished recovery/RPC/Qt prototype in the development
  workspace. It is not included in these commits and requires its own
  durability review, compilation and targeted tests before inclusion.

## Qualification and release gates

The relayer changes passed 108 focused offline tests in the master checkout
(55 existing, 14 coalescing, 10 scheduling and 29 header/cache cases).
Read-only live checks also verified sixteen Ethereum headers against a
B3-finalized anchor, replayed them from cache without additional network
requests, and checked the frozen-store path using the C++ proof utility.

These checks do not qualify a new wallet binary, an inactivity consensus rule,
or a completed Ethereum-to-B3 deposit. No full wallet suite, cross-platform
build, public bridge launch or v1.1.5 release is claimed here.

See [the preparation plan](release-v1.1.5-plan.md) for branch synchronization
and the remaining validator-liveness design decisions.
