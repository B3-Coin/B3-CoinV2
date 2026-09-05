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

## Wallets that stop at block 810,000 (revised v1.1.3 build)

Block 810,000 is the sealed final legacy block. A node that holds it is
already treated as a modern node: it advertises the modern protocol, drops
automatic connections to historical 80008 peers after the handshake, and can
only continue through modern peers using header-first synchronization.
Several distinct problems made wallets stop exactly there.

- **Header replies across the boundary were rejected (fixed).** A node that
  reached 810,000 asked its first modern peer for headers starting one block
  below its tip, so the honest reply began with block 810,000 itself followed
  by block 810,001. The receiver compared block identities with the modern
  SHA256d hash, but block 810,000 is a legacy block whose identity is its
  scrypt hash, so the reply looked discontinuous, the peer was discouraged
  for the rest of the process lifetime, and the same happened with every
  modern peer. Header
  continuity now uses each header's real identity, and a node parked at the
  boundary asks for headers from 810,000 itself. This affected every
  v1.1.x build for nodes that arrived at 810,000 after the transition; nodes
  that were already online when block 810,001 appeared were not affected.
- **No modern peer found.** v1.1.0 and v1.1.1 have no recovery path at all:
  their address database holds only historical peers, which can never
  satisfy the post-boundary service requirement. v1.1.2 introduced a one-shot
  rescue that seeds three modern addresses after 60 seconds, but it is
  skipped while two outbound peers are connected even when those peers are
  themselves parked at 810,000, and it is never re-armed. This build adds a
  forced rescue: on the ten-minute stale-tip check, a node past the boundary
  whose connected peers have announced nothing above its tip re-arms the
  rescue regardless of how many peers it has, and the connection manager
  then contacts the three recovery seeds directly instead of leaving them to
  random selection among thousands of dead legacy addresses.
- **Builds older than v1.1.0 stop by design.** v1.0.0, v1.0.0rev and
  v1.0.0rev2 pin the final legacy height but deliberately not its hash, so
  they refuse every block above 810,000 until replaced. The historical
  B3-CoinV2 client is served only through 810,000 by upgraded peers.

What to check on a stuck wallet:

1. `getnetworkinfo`: the `subversion` names the build. Anything before
   1.1.3 must be replaced by a manual download of this release; the in-app
   updater on older builds is not configured and cannot deliver it. A
   subversion of 1.1.3 alone is not proof: the first v1.1.3 build has the
   header bug too and reports the same version. Verify the binary's SHA-256
   against the revised release, or start with `-debug=net` and look for
   `initial getheaders (810000)`; the first build asks from 809999.
2. `getblockhash 810000` must be
   `2413ba59476afb9a01b971c350b2c5a51494b37925055be42dde774f30d865c6`. A
   different hash means an older build followed a minority legacy fork at
   the boundary. Install this release and start it normally: if the node
   already knows the sealed block it unwinds the wrong branch by itself
   (debug.log: `lies off the finalized legacy boundary anchor; unwinding`);
   if startup reports `Error initializing block database` it offers
   `-reindex` or `-reindex-chainstate`, and `-reindex-chainstate` is
   sufficient unless the node is pruned.
3. `getpeerinfo`: a healthy node has at least one outbound peer with
   `version` 80009 or higher (70016 from a v1.1.0 peer) whose
   `synced_headers` is above 810,000. Historical peers report 80008.
4. `debug.log`: `Disconnecting legacy-protocol outbound peer after the sealed
   boundary` and `Adding B3 modern recovery seeds` are written by default.
   The header bug is only visible with `-debug=net` (or after
   `b3coin-cli logging '["net"]'`): `Misbehaving: peer=N: non-continuous
   headers sequence` right after `initial getheaders (809999)`. Without net
   logging its signature is modern peers that disappear seconds after
   `New outbound-full-relay` while `getpeerinfo` never shows one with
   `synced_headers` above 810,000.
5. Remove any `connect=` line from `b3coin.conf`; it bypasses peer discovery
   entirely.

Manual recovery on any v1.1.x build, while waiting for the fixed build:

    b3coin-cli addnode 38.191.246.166:5647 onetry
    b3coin-cli addnode 46.151.140.5:5647 onetry
    b3coin-cli addnode 77.74.83.147:5647 onetry

Manual connections are exempt from the post-boundary service filter and are
never disconnected or discouraged for misbehaviour. On builds with the header
bug the manual peer's first headers reply is still dropped (debug.log with
net logging: `not punishing manually connected peer`), and sync resumes when
that peer announces its next block; on this build the reply is accepted at
once. Do not reindex unless startup explicitly reports an incompatible block
index.

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
