# Closed-test source composition

This review branch preserves the finalized wallet-safety commits through
`372adcbd8e7836b37791c8da1adc076292748bb2`. It is not production-release approval.

The independent FMN2 implementation is transplanted from reviewed local commit
`4700e85c15cef202c57754efa09fd3966a342322`, whose parent is the shared
`59057cb6c85d32129e1e38c3dd5b67e7c8b36e02`. Its functional source is unchanged;
developer-identifying documentation paths are removed before publication.
That original local commit is not made reachable merely to publish private
evidence links. FMN2 remains authenticated plaintext TCP, not QUIC or encryption.

The following client integration uses the preserved whole-file composition,
not clean HEAD alone. It includes metadata/RPC prerequisites, the ordinary
HTTPS client, Qt lifecycle repair, exact-action receipts, oversized-request
cleanup, reverse-B3 presentation and demonstrated redundant-refresh repair.
The later historical inclusion lookup, honest historical-certification wording
and deferred status-read changes are applied as separate dependency-ordered
commits. Original component deltas and raw generated-fixture evidence remain
private; they are not copied into this repository.

The pre-wallet client freeze contained 3,597 files and had canonical source
manifest SHA-256 `5808648ff87a487e1e2442c7f6fca714d1054add5c8ae9e5b46ddbe8b5fe2803`.
The reviewed functional wallet/client composition contains 3,603 files and has
canonical source manifest SHA-256
`87d25160206d998a97463f3c452942bbe6c2ae45b4f11fe3b0aa99e39fdcba16`.
These are source identities, not binary hashes or a claim of fresh testing.
Integration documentation sanitization, the published wallet qualification
note, the explicit validator-disabled reconciliation test and the successor
packaging layer are separately identified differences from that composition.

Seven shared wallet/client files preserve both completed implementations;
whole-file client snapshots must not remove the already-published wallet
safety changes. Historical combined patches are alternatives to their
component patches, not additional layers to apply twice.

The candidate preserves existing market identities, canonical colored-asset/B3
semantics, V1 B3-denominated trading fees, quorum, membership, signature formats,
signing history and B3 settlement validation. It does not activate stable fees,
inactive-validator changes, hosting commissions, new PoW/BFT or bridge changes.

The original macOS accessibilitySelectedChildren crash remains unresolved.
An absent recurrence is not a repair. Native request selection and watch-wallet
switching retain their separately reported qualification limits. Process
interruption migration tests are not power-loss or Windows durability proof.
The integrated candidate requires its own clean-checkout build and bounded
qualification; retained earlier passes are not reported as newly executed.
