# B3 Hive v1.1.3 — Validator Coordination and Stability

B3 Hive v1.1.3 is a targeted stability and recovery release for the live
Modern-PoS network. It changes block validity only by hardening the agreed
block at height 811,641 as a checkpoint; stake eligibility, normal chain-work
selection, activation heights, bridge rules, and wallet data are unchanged.
Validators should upgrade together to receive the full fork-reduction benefit
and resume finality on the checkpointed chain.

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

## Finality signer recovery (revised v1.1.3 build)

- Validators that signed the later-discarded checkpoint at height 811,631
  (block `86297c10…26be0`) hold a durable ancestry lock on that block. The
  protocol's only unlock proof is a newer quorum certificate on the active
  chain, and that certificate cannot form without the locked weight, so
  finality deadlocked at checkpoint 811,591 while every node agrees on the
  same active chain.
- This build hardens the agreed current-chain block at height 811,641
  (`5dbb0e58…3b1f75`) as a real modern checkpoint. Updated nodes reject a
  competing history at that height during normal synchronization and reindex.
  It also carries a compiled-in, one-time signer recovery for that exact
  incident. A signer journal that holds
  precisely the 811,631 vote, and no newer vote, under epoch 0 with the
  recorded validator sets, moves only its ancestry lock to the agreed
  checkpoint at height 811,641 (verify with
  `getblockhash 811641`) once that block has the normal 12-block
  finality-signing depth (from tip height 811,653). The recorded 811,631
  vote is retained; journals are never deleted or recreated; the next
  signature is strictly above both heights (811,651 or later). Any
  other chain, height, hash, epoch, validator set, journal state or newer
  signing record leaves the journal untouched and the signer fails closed
  exactly as before. While the recovery is pending or inapplicable the
  staking status names the reason after the usual fork-refusal message.
- The 811,591 certificate already certified the epoch-0 handover, so nodes
  rotated into epoch 1 at height 812,441 on schedule. The pin resolves the
  incident's validator sets through the normal current-or-previous epoch
  window, so it still applies after that rotation, and recovered validators
  then sign the remaining epoch-0 checkpoints and the epoch-1 checkpoints in
  order. It also still applies to a validator that upgrades after the first
  new certificate has been included; that validator's next vote is then the
  first checkpoint above both the anchor and the newly finalized checkpoint.
- The recovery window closes when epoch 2 starts: the first height at or
  above 813,881 once an epoch-1 certificate has been included (epoch 0 then
  leaves the current-or-previous window), or, if no epoch-1 certificate is
  ever included, when the finality lineage breaks at height 823,961. Every
  locked validator must therefore run this build and be recovered before
  height 813,881.
  A validator that misses the window stays locked, exactly as any validator
  that misses a whole epoch of votes does under the existing journal rule.
- There is no operator unlock, configuration option, RPC or timeout. A
  journal that has been recovered, or that has signed anything newer, can
  never match the pin again, and the pin stops matching every journal once
  epoch 2 starts or the lineage breaks. Normal operation then continues.
- The production Ethereum vault currently holds zero USDT and records zero
  locked USDT, so the temporary exposure of the previously relayed
  certificate carries no reserve risk. After B3 finality resumes, a newer
  current-chain certificate is relayed to Ethereum immediately.
- Do not deposit into, or otherwise interact directly with, the production
  vault until the Ethereum verifier reports a current-chain certificate
  strictly above height 811,631. A saved certificate from the discarded
  branch could otherwise be submitted before the replacement certificate.
- The advertised B3 modern protocol version changes from `80009` to `80010`
  so that recovered peers can be identified on the network. This is a wire
  identity only: the Core feature version stays `70016`, every modern B3
  version above `80008` negotiates the same mode, and the application version
  remains 1.1.3.

## Upgrade

Back up the wallet, shut down the old B3 Hive application completely, and then
replace it with the v1.1.3 package for your platform. The existing data
directory and wallet remain compatible. A node already on the checkpointed
chain needs no rescan or reindex. If startup detects another block at height
811,641, it stops safely and tells the operator to rebuild the chainstate.

Affected validators must preserve the `finality_signer` directory, unlock the
wallet, and run `startstaking` normally after installing the build. Confirm
that `getblockhash 811641` returns
`5dbb0e582be41444933d43c9dda576f15a2922a870c3fb9d1c47b84b473b1f75`
and that `getstakinginfo` reports `finality_signing: true`. Do not delete or
manually edit the signer journal; the one-time recovery matches and updates it
in place.

Nodes already running the first v1.1.3 build must install the revised build
manually: the secure updater only offers strictly newer versions, so it
reports the revised same-version package as not newer. The published stable
update manifest still describes the first build; the release operator must
re-publish a higher-sequence manifest for the revised packages so that older
nodes are not offered the pre-recovery build.

The exact source revision was qualified locally with focused Modern-PoS,
finality, orphan-recovery, updater, and Qt regression suites. The extended
GitHub test job is deliberately skipped for this time-critical packaging run;
all platform builds, contract checks, architecture checks, checksums, and
release gates remain mandatory.

All published packages are unsigned at the operating-system layer. Verify the
published SHA-256 checksums before use.
