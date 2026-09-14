# Legacy B3 wallet migration: preservation and recovery

The failure-safety changes described here are a separately reviewed candidate,
not authorization to migrate a holder wallet or deploy an unreleased build.
Generated-fixture tests do not demonstrate recovery of a community wallet.

## Preserve before attempting migration

B3 Hive can use the same default data directory as the legacy client:
`~/Library/Application Support/B3-CoinV2`, `%APPDATA%\\B3-CoinV2`, or
`~/.B3-CoinV2`. Confirm the actual executable, wallet name and datadir; do not
assume a different application icon means it uses different data.

1. Shut down the legacy client cleanly and confirm no process still owns its
   wallet/datadir. Do not launch two versions against the same directory.
2. Preserve an offline copy of the **complete legacy wallet environment**:
   `wallet.dat`, the corresponding `database/` directory and any accompanying
   files that exist after clean shutdown. Preserve file paths and permissions.
   Keep a second protected copy off-machine. Do not publish these copies.
3. Keep the original untouched. Initial migration/recovery attempts belong in
   an explicitly isolated copy using an approved build. Verify exclusive
   ownership of that copied directory before starting it.
4. A wallet that did not shut down cleanly may require its historical Berkeley
   database environment and logs. The migration reader does **not** replay
   those logs. A readable `wallet.dat`, reset LSN or matching displayed balance
   does not prove that no later committed records exist in the logs.

Do not delete lockfiles, discard `database/`, reset signing journals, run blind
salvage/reindex, rewrite transaction timestamps or sweep funds merely to make
migration appear successful. Preserve the exact error and investigate it.

## What the candidate does

The migration RPC/menu action uses the wallet's exact configured name. An
encrypted legacy wallet requires its passphrase through a trusted local UI;
never share the passphrase, wallet file, seeds or keys in a bug report.

Before replacement, the candidate creates a new exclusive backup named:

`<wallet-prefix>_<timestamp>_<unique-id>.legacy.bak`

The successful RPC returns its exact `backup_path`; handled preparation errors
also identify it where available. The backup lives in the configured wallet
directory, not necessarily beside the original if an explicit wallet path is
used. The backup is file-flushed and byte-compared with the original. It is
never overwritten or consumed by automatic rollback.

The replacement is built in a unique `.migration-<unique-id>` directory beside
the original database. Raw records are committed and verified, existing
descriptor conversion is completed, and SQLite is closed/reopened for
verification before publication. Existing SQLite sidecars are refused rather
than deleted, including symlinks. The old transaction encoding and `nTime`
are not changed by the storage copy.

Directory wallets publish a completed `wallet.dat`. Historical flat-file
wallets must become directories: their original is first retained at the
returned backup path plus `.original.bak`, before the staged directory is
promoted. Both preserved copies must be kept until the migration is reviewed.

Auxiliary watch-only/solvable wallets are registered for automatic startup only
after the complete migration succeeds. Handled failures close/unload created
wallets before exact, non-recursive cleanup. If a wallet cannot be unloaded,
its files are left intact and the error identifies the preserved backup.

## After a failure or interruption

Do not repeatedly start migration, delete staging files or overwrite a wallet
path to bypass an error. First record the error, stop the affected installation
cleanly if it is running, and preserve the original, backup, retained original,
staging directory and any auxiliary wallet files.

The supported recovery decision depends on the actual boundary:

- **Before publication:** the legacy original is still at its original path.
  Resolve the specific refusal before an operator-approved retry on a copy.
- **After successful publication:** a complete SQLite replacement may exist
  even if the process stopped before returning an RPC response. Inspect/load
  that same wallet; do not treat a missing response as permission to overwrite
  it or blindly repeat the conversion.
- **During flat-file promotion:** the original name may temporarily be absent.
  The unique retained original and verified backup must be preserved. With no
  process owning the directory, an operator can verify the backup and restore
  it **exclusively to the absent original path**, then retry migration. Never
  overwrite an occupied path. This is explicit recovery, not automatic startup
  recovery, and must be qualified on isolated copies first.
- **Cleanup/restoration incomplete:** keep all remaining files. Do not infer
  that an RPC error means nothing was published; use the exact reported state
  and verify the original/backup before choosing a recovery path.

After success, verify transaction identities and relevant history, encryption
and ownership, addresses, and spendable/locked/immature/STAKE classifications.
A UTXO comparison or matching totals alone is not complete recovery evidence.
Make a fresh protected backup of each resulting descriptor wallet.

## Limits and separate incidents

File flush failures are checked, but the existing directory-flush helper is
best-effort and is a no-op on Windows. Process-interruption tests are not proof
of power-loss-safe directory persistence. Historical-log replay, genuine old
client wallet variants, filesystem/hardware failures and concurrent access
remain separate qualification requirements.

Block synchronization failures, including a stall at height 819599, are not
diagnosed by wallet migration tests. Finality signing readiness is also a
separate concern. This guide does not authorize reindexing, chain invalidation,
anchor changes, journal resets or network-configuration changes for a sync
incident.
