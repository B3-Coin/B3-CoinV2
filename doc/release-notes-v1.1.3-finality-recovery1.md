# B3 Hive v1.1.3-finality-recovery1

This is a narrowly scoped, one-time finality-signer recovery prerelease. It is
not a replacement release for ordinary B3 nodes. Its numeric client version,
P2P protocol, finality digest, signature message and journal format remain
compatible with official v1.1.3.

## Exact incidents covered

The build contains separate fail-closed rules for these intact signer
journals:

| Validator | Recorded orphan vote | Canonical block at that height | Recovery anchor |
| --- | --- | --- | --- |
| `5e62687180477d750f480d24bb02f952c0c807f44fd67128d1fd7a1d09d91142` | `812151` / `bc6807d5d543c6dde7baf50fc08146409e824194c306e8ca9af9329319f5dade` | `812151` / `759a6144c929fadca9473a3ddf31210c0e73171f577c88cbc9ca706630ebc0c6` | `812401` / `6cc78147e8ad80348e81ea5d6b00c7723188edafec8d544d55e1bac4b90ea22a` |
| `caa592dda8d13dd45e3596402b0577a67d31c1c27cffdf56b20ddfa648b58e2f` | `812961` / `1b5bceaec722edb63a50a9b392a79f5a53a2003fed163fe848880d8a09632659` | `812961` / `b7cce34e37b6e615d949f657c00ba181a73a41c82f1df73395815649c190b83b` | `813401` / `1490ab26fca2e91490ae9e3b94208e9fa65d126b4cd75b97abab1097440f54eb` |

Official v1.1.3's existing exact recovery for the older 811631 incident is
retained. For a wallet that matches none of these incidents, signer behaviour
and its journal are unchanged; every recovery node still enforces the newly
hardened canonical anchor blocks.

For a listed validator, the build preserves the recorded orphan vote and
moves only its ancestry lock to that validator's compiled canonical anchor.
It applies only when the chain domain, validator key, last vote, existing
lock, epoch, signing set, successor set, active-chain anchor and normal
12-block signing depth all match. Any mismatch leaves the journal untouched
and finality signing disabled. The signer cannot vote again at or below its
anchor.

This build does **not** recreate a deleted, missing or corrupt signer journal.
The prior votes of such a signer cannot be proven from an absent file. Restore
that validator's exact original `finality_signer` directory from a known-good
backup, or handle it through a separately reviewed and explicitly approved
missing-state recovery. Never copy another validator's signer directory.

### Required recovery order

Recover `5e6268...91142` **before** starting the CAA recovery. The `5e6268`
journal is locked in epoch 0 and can be checked only while epoch 0 remains the
previous validator-set snapshot. If CAA supplies the missing epoch-1 quorum
first, the included certificate can authorize epoch 2 at or after height
813881; epoch 0 then leaves the `{current, previous}` safety window and the
`5e6268` recovery correctly refuses to run.

Before starting `5e6268`, require `getfinalityinfo.epoch` to report epoch `1`,
`handover_certified: false`, and `lineage_broken: false`. Keep CAA stopped
until `5e6268` is armed and its durable `last_signed_height` has advanced above
812401. If handover is already certified, lineage is broken, or the network
already reports epoch 2, stop: do not modify or delete the `5e6268` journal
and do not try to force this package to recover it.

## Operator procedure

1. Keep bridge deposits and the Ethereum finality relay closed. Before using
   this build, the release coordinators must independently confirm that the
   retained orphan signatures cannot meet both the old-set stake quorum and
   signer-headcount threshold, that no conflicting certificate was included
   on B3, and that Ethereum's permissionless finality verifier and vault have
   not accepted a certificate from either discarded branch. There is no
   owner pause key that makes this check optional.
2. If both listed validators need recovery, recover `5e6268...91142` first.
   Do not start the CAA recovery until the `5e6268` checks in step 9 pass.
3. Confirm that the validator key and the complete fork-refusal error exactly
   match one row above. Healthy validators must stay on official v1.1.3.
4. Stop B3 Hive completely. Confirm that no other wallet or daemon using the
   same validator key is running on any machine.
5. Make a timestamped external backup of the wallet and the complete existing
   `finality_signer` directory. Record the original journal's SHA-256. Do not
   delete, generate, replace or edit the live journal.
6. Download the reviewed **portable Win64 ZIP**, not the installer. Verify the
   archive against `SHA256SUMS.win64` and the extracted `b3coin-qt.exe`
   against the separately published `SHA256SUMS.win64-inner`. Extract and run
   it from a separate folder; never overwrite the official installation in
   Program Files.
7. Check `b3coin-qt.exe -version` or Help -> About contains
   `1.1.3-finality-recovery1`. `getnetworkinfo` intentionally continues to
   report numeric version `10103` and protocol version `80010`.
8. Start the recovery executable with the same data directory the official
   wallet uses, including the same custom `-datadir` argument if configured.
   It must open the original signer journal in place.
9. Verify the canonical incident-height hash and recovery-anchor hash from the
   applicable row with `getblockhash`. Then verify:

   ```text
   getstakinginfo  -> staking.finality_signing: true
                      staking.last_error is absent
                      staking.last_signed_height > the applicable anchor
   getfinalityinfo -> signing.armed: true
                      signing.last_signed_height > the applicable anchor
   ```

10. Keep every recovery wallet running. When both listed incidents are being
    recovered, require both durable journals to be above their respective
    anchors. Do not stop either wallet until a correct-chain certificate is
    included, global `getfinalityinfo.finalized.height` is strictly above
    `813401`, and its hash equals `getblockhash <finalized.height>`. A relayed
    signature by itself is not enough because the signature pool is
    memory-only.
11. Exit the recovery wallet cleanly. This return is manual because the
    updater sees both builds as numeric version 1.1.3. Afterwards launch the
    untouched official v1.1.3 executable with the identical arguments,
    including any custom `-datadir`; retain the advanced live journal. Confirm
    About no longer contains `finality-recovery1`, and confirm signing remains
    armed with the advanced `last_signed_height`. Archive or remove both the
    recovery ZIP and its extracted executable folder so neither is launched
    accidentally.
12. Never restore the pre-recovery journal after the validator may have sent a
    new signature. The backup is forensic evidence only, not rollback state.

Existing official v1.1.3 peers accept signatures from this build. Do not add
this artifact to the stable update manifest, and do not mark its GitHub
release as latest. Native finality advancing is not by itself permission to
reopen deposits or Ethereum relay: keep both closed until the Ethereum state
has been reconciled independently and bridge-readiness is explicitly approved.
