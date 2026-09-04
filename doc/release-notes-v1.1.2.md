# B3 Hive v1.1.2 — Peer Recovery and Bridge Safety

This urgent follow-up keeps the v1.1.1 consensus rules and improves recovery,
operator clarity, and bridge safety. The exact release source was qualified
locally before tagging; the extended GitHub test job is deliberately skipped
for this time-critical packaging run. Platform builds, contract checks,
checksums, and packaging gates remain mandatory.

## Network recovery

- Modern wallets no longer remain dependent on automatic historical peers
  after the transition boundary. Obsolete automatic outbound peers are
  disconnected without banning them, while manual and inbound connections
  remain available.
- When a post-transition wallet has too few usable outbound peers, it performs
  a one-time recovery using modern B3 seed peers. This also helps installations
  reported stuck at block 810,000.
- Historical peer service flags are normalized so old peers cannot appear to
  provide modern FlowMesh capabilities they do not support.

## Staking clarity

- Wallet and RPC status now distinguish the amount placed into a stake from
  the validator's frozen current-epoch voting weight. A displayed weight of
  333 is the current validator-set snapshot, not an indication that a larger
  stake transaction was reduced.
- Wallets without a local validator key now report the network's real total
  validator-set weight instead of incorrectly showing zero.
- Newly active stakes continue to enter a future certified validator-set
  rotation; an existing Ethereum/B3 set fingerprint is never changed midway
  through an epoch.

## Bridge safety and usability

- The withdrawal console path now converts its arguments correctly.
- Ethereum light-client recovery may fill a missing same-period next sync
  committee from a fully verified stale update without regressing the finalized
  header.
- Bridge-relayer dry runs stop after planning unapplied light-client updates,
  avoiding an invalid deposit scan against an anchor that has not been retained.

No wallet secrets or local operator files are included in this release.
