# Snapshot-aware finality signing

## Problem and scope

A confirmed BLS binding rotation changes the latest binding before the new
key becomes effective in an epoch snapshot. Resolving only the latest binding
can therefore leave staking running with a key that cannot sign current
checkpoints. A loaded key or `armed: true` alone does not prove signing progress.

This fix is generic to validators with retained or wallet-derived snapshot
keys. It is not a new mainnet recovery exception and does not change proposer
selection, epoch/quorum rules, stake weights, checkpoint digests, or consensus
serialization.

## Key selection and safety

- Each snapshot retains its validated binding sequence as internal metadata.
  It is reconstructed from blocks on startup and is excluded from snapshot
  hashes and serialized consensus commitments.
- On staking startup, the unlocked wallet resolves the exact public keys for
  its current, previous, and prepared next snapshots, plus the latest
  non-revoked binding. Keys are deduplicated; the node accepts at most four.
- Each checkpoint is signed with the key in that checkpoint's epoch snapshot,
  not whichever binding is newest. Loaded rotation keys can cross the prepared
  handover without a restart.
- Every key shares the same validator-identity signing journal and monotone
  watermark. Existing missing/corrupt-journal and fork-ancestry checks run
  before signing; the journal commit still precedes signature publication.
- Missing the current snapshot key is reported explicitly. Available older
  keys may still sign eligible previous-epoch checkpoints. A missing key never
  authorizes substitution, rewriting the journal, or voting on another fork.

Derived historical keys can be reconstructed from the original wallet's
validator secret and the recorded sequence, then checked against the snapshot
public key. An independently imported old secret that was overwritten cannot
be recreated this way. The wallet must fail closed in that case.

Keys are collected at staking startup. A later binding change may require a
staking restart to load a key that was not available at startup.

## Operator checks

`getfinalityinfo.signing.snapshot_keys` lists only public snapshot metadata.
For an unlocked wallet, each entry also reports `key_available` and, on failure,
`key_error`. The current member must have its current snapshot key available
before the GUI's normal Start Staking action proceeds.

After an authorized activation, confirm that `last_signed_height` advances and
that peers accept signatures for the same checkpoint. Local signing progress
does not by itself prove network quorum or an included finality certificate.

Never delete, replace with an older copy, or manufacture the `finality_signer`
journal to address a key-selection error. Do not rebind just to make the GUI
show a newer key.

## Incident rollout constraint

This change can restore a missing quorum participant. On the affected mainnet,
that could enable a certified handover and advance the epoch, closing an
existing incident-pinned recovery path for another offline signer.

At preparation time, the owner of `5e6268…91142` remains unavailable. Activation
of index 4 (`3e9a1f…74b18`) is on hold pending coordination; preparing or testing
the fix does not authorize activating it. Other validators can independently
change network state, so recheck the epoch and recovery preconditions before
deployment. Passing a nominal height alone is not proof of a certified handover.

This local change is not the already published `v1.1.3-finality-recovery1`
binary. Any new distributed artifact must identify the new source commit and
have its own version/checksums; do not replace the published binary silently.

## Local validation (2026-09-06)

The following checks ran on the wallet-fix patch before its separation from
the recovery checkout; they are not a new full qualification of the standalone
branch.

- Native macOS daemon, GUI, core-test and GUI-test targets compiled.
- Nine selected core cases passed, with 595 assertions: snapshot metadata and
  golden consensus commitments; wallet old/new key resolution and missing
  imported-key refusal; journal persistence/fail-closed behavior; key rotation
  through epoch handover and restart; recoverable missing-key status; and the
  production staking loop's retained-key startup.
- The isolated `stakingRequiresCurrentSnapshotKey` GUI case passed. The full
  GUI suite was not run.
- A broader existing recovery-regression selection was canceled during its
  synthetic legacy mining setup; it is not counted as passed. No full core
  suite was run.
- No new Windows artifact was built, no release was published, and no mainnet
  wallet was activated for this change.

## Separate wallet branch (2026-09-07)

`fix/wallet-finality-snapshot-keys` is based directly on normal `master`
(`bcd340858167432db294d79474108a23c06f8445`). It excludes the changes in recovery
prerelease commit `1777969feaecb4e9ab0009f6755336c73a088270`.

The code patch transferred unchanged. The normal base's `TryPinnedRecovery`
and `EnsurePersistentSafety` implementations are unchanged, and the finality
signer translation unit compiled successfully after separation. No long test
suite was rerun. The existing recovery branch and release tag are untouched.
