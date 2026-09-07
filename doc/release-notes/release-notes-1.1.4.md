# B3 Hive 1.1.4 — release preparation

This is the working `release/1.1.4` branch, not an approved or published
release. Builds identify themselves as `1.1.4-dev`. Remove the development
suffix and mark the build as a release only when the release is approved.

## Scope

- Finality message delivery: bounded retention of premature messages, followed
  by the unchanged signature checks when the receiving node reaches depth;
  bounded retransmission of exact verified messages retained in its pool,
  including delivery to newly connected peers.
- `getfinalitystatus` gains `verified_checkpoints`: per-checkpoint verified
  signer indices, signer count, signed stake, both quorum thresholds, and an
  explicit `quorum_reached` flag. These are local observations, not proof that
  validators are online or that a certificate has been included in a block.
- Retains the normal wallet fix selecting finality keys from the applicable
  epoch snapshots. The separate `finality-recovery1` patch is not added to this
  branch; existing mainline consensus rules remain unchanged.
- Send-screen recipient buttons receive dark backgrounds and readable icons
  in normal, hovered, pressed, focused, and disabled states. The address field
  says `Enter a B3 address`, without a Bitcoin example or suggested recipient.
- The Python Ethereum-to-B3 relayer recognizes an active light-client update
  awaiting B3 finality as a wait condition before making external proof/RPC
  requests. Finalized-store consistency and proof validation remain required.
- The Stake screen explicitly offers the staking-only equivalent for encrypted
  wallets: briefly unlock, load dedicated validator/finality keys, start staking,
  and lock spending again. This also relocks wallets that were already fully
  unlocked before the action. The normal spending unlock workflow is unchanged.

## Safety and operator expectations

No change to checkpoint depth, quorum thresholds, validator selection, signed
digests, certificate validation, or the P2P message format. The modern protocol
identity remains 80010. There is no signer-journal reset, recovery-anchor
change, or new signing operation in message retransmission.

Early messages are unverified until ordinary validation accepts them; they
must not count toward quorum. Expired, finalized, or changed-branch messages
must not be retransmitted as current votes. A matching checkpoint hash alone
is not a substitute for verifying the entire signed object.

The early-message queue expires entries after five minutes and caps storage
globally and per source. Retries use the existing verification budgets.
Periodic replay sends at most two messages per peer per batch, at least two
seconds apart, with longer spacing for large peer counts. This targets at most
64 replay messages per second across the node in steady state, not a strict
instantaneous burst cap. Initial acceptance still uses the normal immediate
relay path. Large validator pools take longer to replay in full.

Messages previously discarded without retaining their signature bytes cannot
be reconstructed from diagnostic logs. The verified pool and transport buffers
are in memory: restarting a node does not recover discarded messages or its
former pool. Other peers may still have the exact messages, and subsequent
checkpoints can generate new eligible votes. Replay improves delivery, but
cannot supply unavailable signers or guarantee network-wide quorum.

Keep the same wallet, finality signer journal, and relayer state database when
upgrading. Do not run two signers for one validator identity. ETH gas does not
repair a stalled B3 finality quorum. No live-wallet replacement, transaction
broadcast, release tag, or release publication is part of this source change.

The staking-only workflow uses the real wallet lock after its brief normal
unlock; it is not a separate partial-unlock permission mode. Dedicated staking
keys remain in the node's memory while staking runs. New spending signatures
and private-key access require another wallet unlock after relocking. An
already-signed transaction can still be broadcast, and an authorized concurrent
RPC caller is not restricted during the brief normal unlock. Unencrypted wallets
cannot offer passphrase-protected spending and are labeled accordingly. Stop
staking or close the node to stop the retained signing keys from being used.

## Qualification

Local macOS arm64 qualification (2026-09-07):

- Built `test_bitcoin`, `test_b3_sendcoinsentry-qt`, and `b3coin-qt` successfully.
- The focused headless Send-screen test passed: three test rows plus setup and
  cleanup, checking actual button surfaces, icon modes, focus, placeholder,
  and retained address validators. Both macOS and Windows `PlatformStyle`
  branches were exercised on local Qt 6.11.1; this is not a native Windows test.
- All 55 offline Python relayer tests passed, including waiting for finality,
  canonical-chain conflicts, races, malformed metadata, and quantity bounds.
- Six focused finality transport/pool cases passed: queue limits, expiry,
  poisoned variants, retry fairness, paced replay, send backpressure, early
  arrival followed by depth, dropped delivery, reconnects, exact-byte replay,
  verified signer observations, and stale/fork/finalized suppression. These
  use synthetic chains and in-process peers, not the live node. A mock send
  queue issue was corrected and only the affected network case was rerun.
- Built the staking-only Qt preview separately from the running executable.
  Its isolated headless test passed 19 entries (17 cases/data rows plus setup
  and cleanup) in 0.96 seconds. Checks cover real encrypted-wallet relocking,
  ordinary message-signing refusal after relock, cancellation, wrong password,
  failures before and after scoped-unlock construction, unchanged general
  unlock behavior, unencrypted/watch-only handling, and nine presentation rows.
  It uses an in-memory regtest wallet with no mining or networking; it does not
  claim end-to-end live staking coverage or a native Windows run.
- The existing focused `ismine_tests/exact_script_signability` core test passed
  all 12 assertions. No long staking/mining suite was repeated for this Qt-only
  change, and the live wallet was neither replaced nor restarted.

No full integration/soak suite, cross-platform release packaging, or live
network deployment is claimed by these local checks.

Fee-estimation policy, consensus changes, deleted signer-journal recovery, and
automatic B3-to-Ethereum transaction submission are outside this batch.
