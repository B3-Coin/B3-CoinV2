# Wallet safety follow-up: scope and qualification

This source checkpoint preserves the bounded wallet/migration/STAKE fixes.
It is not a public-release certification or a community-wallet recovery claim.
No holder wallet, live validator or signing journal was used in qualification.

## Included changes

- Reject duplicate live BDB records before publishing a parsed wallet snapshot.
  Historical transaction encoding and nTime are unchanged.
- Build and verify a migration replacement before publishing it. Preserve
  unique verified backups and retained originals; handle errors/exceptions;
  refuse foreign destination/sidecar files; clean only operation-owned paths.
  Defer auxiliary autoload until success and default the primary confirmation
  to Cancel. The separate Restore-and-Migrate confirmation is unchanged.
- Expose STAKE outputs only through explicit coin-control consent. Preserve
  selected outpoint identity if it becomes unavailable; do not silently select
  ordinary funds instead. Revalidate owner authority, trust, locks, maturity,
  and chain/mempool availability. Ordinary automatic selection excludes STAKE.
- Fix stale Send-dialog OptionsModel callbacks and duplicate fee choices.
  Capture the prepared change position for exact review.
- Correct the diagnostic mempool duplicate-view height from INT_MAX to the
  existing MEMPOOL_HEIGHT sentinel, preserving Coin's existing height limits.
- Add a read-only reconciliation tool which distinguishes current UTXO
  observations from complete wallet history, reports incomplete observations
  explicitly, passes public descriptors through stdin and creates private
  reports. It does not repair balances, migrate, sign, or abort another scan.
- Preserve isolated splash timing/lifecycle tests without changing production
  splash behavior.

No matching, fee, quorum, committee, finality, signing-history, bridge or
settlement rules are changed. The unrelated local FlowMesh independent-network
and ordinary-client development deltas are not imported into this checkpoint.

## Retained candidate evidence

The predecessor and follow-up candidates were tested against their documented
composed source baseline. Those binaries are not relabelled as binaries built
from this clean publication branch.

| Check | Retained result |
|---|---|
| Migration creation failure before fix | Generated daemon abort; original replaced by a zero-byte active file; backup survived. |
| Same failure after fix | Safe refusal; original, backup and unrelated sentinel intact; daemon exit 0. |
| Final migration unit suite | 9 cases, 370 assertions, exit 0. Covers staged creation/begin/population/commit/verification and coordinator failures. |
| Process interruption | Three boundaries, followed by six successful same-wallet daemon opens. Flat-file rename-gap recovery was explicit offline restoration, not automatic. |
| STAKE/Send callback before fixes | Selection and trust failures; stale-model callback SIGSEGV; duplicated fee choices. |
| Final automated Qt suite | 12 passes including setup/cleanup, exit 0; not twelve attended interactions. |
| Focused core wallet / mempool regression | Debug lock-order tests pass; diagnostic sentinel test passes. |
| Reconciliation | 24 unit methods and isolated functional owner/watch/mempool/reopen fixture pass. |
| Native encrypted owner | Explicit selection, relock, passphrase unlock, exact review, Cancel, separate authorized test spend, normal confirmation, ordinary change, same-wallet reopen. |
| Actual native application Quit | Two child exits 0 with current-session Shutdown done; no forced cleanup or remaining child. |

Native cancellation did not record, admit or broadcast a spend. The existing
review path had already prepared/signed the candidate transaction and reserved
a change key. Therefore Cancel is not a promise of no preparation/signing or
byte-identical wallet state. The separately confirmed test spent the exact
selected stake; change was ordinary and not automatically restaked. Original
wallet transaction identities and signed bytes were checked after reopen.

## Clean publication integration

The wallet scope was extracted onto GitHub base
`59057cb6c85d32129e1e38c3dd5b67e7c8b36e02`, not copied wholesale from a dirty
development tree. A separate source audit found 34 source/test/documentation
files: 27 byte-identical to the final candidate and seven shared files with
only wallet-related hunks retained. The seven omit unrelated remote-client,
asset-metadata and guarded client-launcher dependencies. This note is additional.

The reconciliation fixture alone needed one publication-base adaptation:
remove the not-yet-present `enableflowmeshvalidator` argument and explicitly
assert that plain regtest's existing service is disabled, not running and not
armed, before and after restart. No production flag or activation rule changed.

Newly executed on this clean publication source (macOS arm64, AppleClang 17,
Debug with lock-order and failed-assumption checks):

| Targeted check | Result |
|---|---|
| Normal daemon, CLI and Qt build | All three targets built successfully. |
| Core and narrow Qt test targets | Built successfully after serial generation of IPC headers. |
| `walletdb_tests` | 9 cases / 370 assertions; exit 0. The opt-in interruption probe is inactive in this ordinary invocation; subprocess evidence above is retained, not rerun. |
| `wallet_tests/b3_validator_key_and_stake_outputs` | 144 assertions; exit 0. |
| `mempool_tests/MempoolCheckUsesRepresentableHeight` | 9 assertions; exit 0. |
| STAKE coin-control Qt | 12 passes, including setup/cleanup; exit 0. |
| Splash Qt | 19 passes, including data rows and setup/cleanup; exit 0. |
| Reconciliation units | 24 methods; exit 0. |
| Reconciliation functional | Pass; generated wallet reopened; service dormancy verified; normal test-node shutdown. |

The first combined build failed in unchanged IPC schema generation: two
parallel generation commands targeted `common.capnp` and a generator reported
`parent is not a directory`. Serial generation completed, then the remaining
targets built. This build-tool observation is retained separately; no IPC or
networking source was changed to work around it.

These Qt suites use the minimal platform. The earlier actual macOS UI session
is retained candidate evidence, not a newly executed native screen pass on
the publication binaries. No broad settlement/networking campaign was rerun.

## Recovery and remaining limits

- A verified main-file copy alone does not prove the latest state of a historic
  BDB environment. Shut the legacy client down cleanly and preserve the full
  wallet plus its database/log environment. Historical unflushed-log replay and
  migration of genuine old/encrypted wallets remain unqualified here.
- Process-interruption checks are not power-loss tests. Existing directory
  synchronization is best-effort and a no-op on Windows. Hardware failure,
  filesystem crash ordering, cross-filesystem publication and independent
  concurrent-process races remain unqualified.
- Native watch-wallet selection/switching and hardware-signer interaction
  remain pending. Automated authority checks do not substitute for those UI
  checks. The zero-confirmation trust unit fixture deliberately uses unchecked
  pool membership; it is not proof of STAKE creation admission.
- The original accessibilitySelectedChildren crash remains OPEN. It is
  distinct from the corrected Send callback and earlier warning-dialog crashes.
  No recurrence during ordinary checks does not establish a repair.
- Community block-sync stalls remain a separate investigation requiring the
  affected release and node evidence. These patches do not establish their cause.
- Native migration-worker exception dialogs, clean platform packaging/signing,
  Windows qualification and public-release approval remain separate.

Only source, tests and documentation belong in this checkpoint. Generated
wallets, keys, outboxes, raw local reports, screenshots and test datadirs are
excluded. No tags, releases or deployments are implied by the branch push.
