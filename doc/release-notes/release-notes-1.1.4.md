# B3 Hive 1.1.4 — scope and qualification record

The owner approved preparing v1.1.4 on 2026-09-08, combining the normal
wallet/GUI work and explicit operator-trusted recovery. Version branding is
now 1.1.4. Publication is separate from source preparation and requires the
release workflow's platform, checksum and contract gates. The historical
qualification entries below describe the batches on which they actually ran.

## Scope

- Mainnet staking delays new finality signatures until checkpoints are at
  least 20 blocks deep. This is a signer-side policy, not a change to the
  existing 12-block consensus minimum. Test-network schedules remain scaled
  to their consensus parameters. Existing signing watermarks and locks are
  preserved; an upgrade may wait for the next unsigned checkpoint to mature.
- Finality message delivery: bounded retention of premature messages, followed
  by the unchanged signature checks when the receiving node reaches depth;
  bounded retransmission of exact verified messages retained in its pool,
  including delivery to newly connected peers.
- Retains signatures for nearby checkpoints whose block has not arrived yet
  (at most 64 blocks ahead, only locally retained signing epochs). Exact-byte
  duplicates can retain up to three source peers without extending expiry,
  so the first source disconnecting need not discard another source's copy.
  Global and per-source storage caps and per-coordinate retry limits remain.
- Finality transport uses monotonic timers. Separate wire-admission and BLS
  budgets keep direct duplicate arrivals bounded without charging them as new
  BLS verifications. Budget-deferred candidates can retry within the bounded
  queue; immediate forwarding respects paused send queues.
- `getfinalityinfo.signing.recovery` exposes the matching running signer's
  read-only recovery observation, including its observation tip, preserved
  vote, ancestry lock, and the checks required of a normal recovery
  certificate. It neither opens/recreates a journal nor authorizes recovery.
- `getfinalitystatus` gains `verified_checkpoints`: per-checkpoint verified
  signer indices, signer count, signed stake, both quorum thresholds, and an
  explicit `quorum_reached` flag. These are local observations, not proof that
  validators are online or that a certificate has been included in a block.
- Retains the normal wallet fix selecting finality keys from the applicable
  epoch snapshots, including available older and prepared-next keys for
  rotation. Missing overwritten imported keys cannot be reconstructed.
- Adds default-off, exact-incident operator recovery and the
  `getfinalityrecoveryinfo`, `setfinalityrecovery`, `clearfinalityrecovery`
  RPCs. Approval is explicit and memory-only. The intact journal and active
  chain are rechecked before advancing the ancestry lock; the old vote is
  retained. No new hardened mainnet checkpoint or automatic fork unlock.
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

No change to consensus checkpoint depth, quorum thresholds, validator selection, signed
digests, certificate validation, or the P2P message format. The modern protocol
identity remains 80010. There is no signer-journal reset, recovery-anchor
change, or new signing operation in message retransmission.

Early messages are unverified until ordinary validation accepts them; they
must not count toward quorum. Expired, finalized, or changed-branch messages
must not be retransmitted as current votes. A matching checkpoint hash alone
is not a substitute for verifying the entire signed object.

The early-message queue expires entries after five minutes and caps storage
globally and per source. Retries conservatively reserve the existing
verification budget; an entry verified through another delivery in the
meantime may still reserve one retry token before being discarded as a
duplicate. Full unverified queues can still refuse new candidates; this
does not claim delivery guarantees under arbitrary hostile traffic.
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

Delivery improvements alone do not release an orphaned signer lock. A
separate operator-approved recovery is now available; see the
[recovery RPC guide](../finality-recovery-rpc.md). It remains a coordinated
trust exception, not evidence that an old conflicting certificate cannot
exist. No incident or anchor is preapproved by this release. The historical
[814,191 incident assessment](../finality-quorum-deadlock-814191.md) is not a
current bridge-state guarantee or an authorization to recover a validator.

The 20-block mainnet signing policy reduces exposure to shallow forks; it
does not make checkpoints irreversible and does not repair existing orphaned
signer locks. Older wallets may still sign at 12 blocks. A separate offline
common-ancestor recovery prototype is deliberately not connected to staking,
RPC, the signer store, or the network. It models the proposed fallback and
its residual conflicting-certificate risk; it is not an operator recovery
command. See [depth-based recovery development notes](../finality-depth-recovery-development.md).

Keep the same wallet, finality signer journal, and relayer state database when
upgrading. Do not run two signers for one validator identity. ETH gas does not
repair a stalled B3 finality quorum. Release preparation does not activate
recovery on any live wallet or authorize bridge transactions.

The staking-only workflow uses the real wallet lock after its brief normal
unlock; it is not a separate partial-unlock permission mode. Dedicated staking
keys remain in the node's memory while staking runs. New spending signatures
and private-key access require another wallet unlock after relocking. An
already-signed transaction can still be broadcast, and an authorized concurrent
RPC caller is not restricted during the brief normal unlock. Unencrypted wallets
cannot offer passphrase-protected spending and are labeled accordingly. Stop
staking or close the node to stop the retained signing keys from being used.

## Historical batch qualification

### Combined 1.1.4 candidate (2026-09-08)

- Rebuilt native macOS arm64 Qt, daemon and CLI with the combined recovery,
  wallet RPC, transport, security and UI source. Live Qt remains separate.
- Passed 63 selected core cases / 25,832 assertions: recovery RPCs, exact
  journal preflight and stopped-staking controls; both wallet snapshot-key
  rotation cases; unlock failure rollback; transport queue/source retention;
  real pool verification budgets; read-only signer observations; signing-depth
  policy; journal persistence; unchanged compiled mainnet pin; schedule and
  certificate checks; and the offline recovery-risk counterexample.
- Added normal CMake targets for the production signer, option parser and
  durable-store synthetic harnesses. All three passed in 2.34 seconds:
  10 signer groups / 643 checks, 460 parser checks, 160 store checks.
- All 55 offline Python relayer tests passed. No external transactions sent.
- Earlier in this candidate, the isolated staking-unlock/lifetime suite passed
  23 entries and Send-form tests passed 5. The subsequently added actual
  topbar-selector case needs its own completed run, recorded below.
- Qualification fixed two test compilation errors (filesystem copy options
  and a missing dialog parent), and registered the parser's UniValue build
  dependency. These were not runtime wallet failures.
- Final combined GUI check: all four focused CTest targets passed in 3.72
  seconds, including real keyboard-based two-wallet switching, wallet removal,
  shutdown model lifetimes, spending relock, screen navigation, settings
  actions, Send controls and icon rendering. The full Qt test executable also
  compiled, but its full integration run was not repeated.
- Testing under both native-style test backends found and fixed double-dimmed
  disabled icons: source artwork now receives the disabled tint once. All 15
  theme-rendering rows passed under both `minimal` and `offscreen`. This remains
  macOS-hosted testing, not a native Windows execution claim.
- New GUI fixtures were corrected to use a processed genesis watermark and
  a registered icon resource. Assertions were retained; production failures
  were not bypassed. The top-right selector now owns its actual visible
  control directly, rather than inheriting hidden-toolbar visibility.

The full long legacy-prefix/integration/soak suites are intentionally not
repeated. Platform release builds, contract checks and packaged checksum
verification remain required in GitHub Actions. Local synthetic tests do not
prove main-chain agreement, revoke old votes, or qualify a real recovery
anchor. Windows compilation and mixed-old/new binary networking are not
claimed by the macOS checks.

Additional local signing-delay work (2026-09-08, still unreleased):

- Rebuilt `test_bitcoin` with the 20-block mainnet policy; 30 focused cases
  passed (440 assertions, 1.32 seconds wall time). This includes actual signer,
  real-BLS, temporary-journal, schedule and certificate compatibility checks.
- The offline recovery counterexample confirms that common-ancestor fallback
  can produce conflicting valid certificates even with 20-block depth. Passing
  that test demonstrates the residual risk, not safety of automatic recovery.
- No production generic recovery, live wallet changes, Qt rebuild, network
  rollout, release, tag or push. Full chain derivation, Ethereum execution
  and Windows compilation were not tested by these isolated cases.

Additional local transport/diagnostic work (2026-09-07, still unreleased):

- Rebuilt `test_bitcoin`, including the changed production node/wallet code.
- Passed 13 isolated cases (24,857 assertions): the seven new transport
  cases, three recovery-formatting cases, and three existing queue/cursor
  cases. These do not mine a legacy prefix or touch the running wallet.
- A broader 17-case selection was stopped while rebuilding the synthetic
  legacy prefix used by the chain-based fixtures. It is not counted as a
  completed pass. The changed pool-budget and direct signer-snapshot
  regressions, plus the network integration case, still need completed
  validation before this additional batch is considered release-ready.
- No live wallet replacement/restart, mainnet recovery exception, release,
  or deployment was performed for this additional work.

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
