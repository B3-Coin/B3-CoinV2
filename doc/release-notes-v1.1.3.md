# B3 Hive v1.1.3 — Validator Coordination and Stability

B3 Hive v1.1.3 is a compatibility-preserving stability release for the live
Modern-PoS network. It does not change block validity, stake eligibility,
chain-work selection, activation heights, bridge rules, or wallet data.
Validators should upgrade together to receive the full fork-reduction benefit.

## Fewer honest same-height blocks

- Eligible validators now derive the same deterministic, stake-weighted
  preferred-proposer order from the frozen validator set and current chain
  state.
- The preferred validator sends first and backups use separate short windows.
  If a window is missed, the validator waits for the next existing recovery
  round instead of producing late.
- This is voluntary production coordination only. Blocks from v1.1.2, older
  producers, or non-preferred validators remain valid under the unchanged
  consensus rules.
- Validator machines should keep their system clocks synchronized (normally
  with automatic network time). The short backup windows reduce honest races
  only when validators agree closely on the current time.

## Safer recovery and reorganization handling

- Modern child-before-parent blocks are retained in a strictly bounded cache
  while the node requests the missing header path from their peer.
- Once a parent arrives through P2P, cached descendants are processed through
  the normal block-validation path. Cache count, byte, per-peer, and expiry
  limits remain enforced.
- Altered hash-external PoS signatures or Modern Payload Area records can no
  longer poison the shared hash of an authentic block. New bodies are checked
  against their committed data before storage, and duplicate bodies can never
  replace the already-validated body stored on disk during branch activation.
- Peer attribution and cleanup now use the permanent wire-codec identity, so
  a wrong-era block cannot leave stale source records in memory.

## Qt stability and signed updates

- The startup crash caused by an early macOS palette-change event has been
  fixed.
- Official Qt packages now contain a pinned HTTPS update channel and a
  threshold set of release public keys. Missing or incomplete production
  updater configuration fails the release build instead of silently shipping
  a disabled updater.
- The generation-1 updater authority is 2-of-3, with public key IDs
  `a28cd03b`, `425c5eda`, and `d106e770`. The complete public configuration is
  recorded in the tagged source; private keys are never sent to GitHub.
- Linux `.tar.gz`, macOS `.zip`, and Windows `.exe` packages are matched by the
  updater using their real release formats.
- The updater verifies the threshold-signed manifest, approved HTTPS hosts,
  version, size, and SHA-256 before retaining a download. Installation remains
  manual in v1.1.3; the application does not replace itself automatically.

## Upgrade

Back up the wallet, shut down the old B3 Hive application completely, and then
replace it with the v1.1.3 package for your platform. The existing data
directory and wallet remain compatible; no rescan or reindex is expected.

The exact source revision was qualified locally with focused Modern-PoS,
finality, orphan-recovery, updater, and Qt regression suites. The extended
GitHub test job is deliberately skipped for this time-critical packaging run;
all platform builds, contract checks, architecture checks, checksums, and
release gates remain mandatory.

All published packages are unsigned at the operating-system layer. Verify the
published SHA-256 checksums before use.
