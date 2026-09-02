# Migrating from the legacy B3-CoinV2 wallet to B3 Hive

B3 Hive 1.1.0 uses the SAME default data directory as the legacy client
(`~/Library/Application Support/B3-CoinV2`, `%APPDATA%\B3-CoinV2`,
`~/.B3-CoinV2`), so the normal path is: close the old wallet, open
B3 Hive, and migrate in place.

## Healthy wallet (the normal case)

1. Close the legacy client completely.
2. **Back up `wallet.dat`** (two copies, one off-machine).
3. Open B3 Hive; it finds the legacy data directory automatically. If startup
   asks for the transition block-index upgrade, close it and perform the full
   `-reindex` described below. That rebuild reuses the existing raw block files,
   then the node downloads only the blocks it is still missing.
4. Migrate the old wallet:  `b3coin-cli migratewallet wallet.dat`
   (or the equivalent Hive menu action). This converts the legacy
   Berkeley-DB wallet into a modern descriptor wallet. Encrypted wallets
   prompt for the passphrase.
5. Verify balances after sync completes, then make a FRESH backup of the
   migrated wallet.

For the transition release, a data directory whose block index was written by
the legacy client or an incompatible pre-transition build must use full
`-reindex`:

    b3coind -reindex

or start the GUI as `b3coin-qt -reindex`. Do not use
`-reindex-chainstate`; the block index itself must be rebuilt. Existing raw
block files are reused, but the rebuild can take time. A current transition
beta data directory does not need this merely because the wallet is being
migrated; use it when startup reports the B3 block-index incompatibility.

Historical FN Coins were created in block 810,001. After importing a legacy
dump or key, let the wallet finish its rescan. If the expected FN Coin is still
missing after the node is fully synced, run this once in that wallet:

    b3coin-cli rescanblockchain 810001

The FN Coin remains in the wallet; during ordinary coinbase maturity the GUI
shows it as confirmed and immature rather than spendable.

## Wallet misbehaving in the OLD client first?

Symptoms and the right tool — these fix DIFFERENT problems:

| Symptom | Fix (old client) |
|---|---|
| Wrong balance, "lost" coins, stuck stake — wallet opens fine | RPC `checkwallet`, then `repairwallet` (fixes spent-state mismatches) |
| Transactions missing after key import | `-rescan` |
| "wallet.dat corrupted" / `-salvagewallet` FAILED | see the recovery ladder below |

## Corruption recovery ladder (salvage failed)

Work ONLY on copies; every attempt can worsen the original.

1. **Copy `wallet.dat` (and the `database/` folder) somewhere safe.**
2. **Fresh-datadir trick:** place a copy of `wallet.dat` in a new, empty
   data directory and open it with the old client — once WITH the copied
   `database/` folder beside it, once WITHOUT. Stale BDB environments
   masquerade as corruption surprisingly often.
3. **Try B3 Hive's `migratewallet` on the corrupted copy.** Hive reads
   old wallets through an independent Berkeley reader that needs no BDB
   environment or log replay — it frequently opens files the old
   client's salvage cannot.
4. `db_dump -r wallet.dat > dump.txt && db_load new.dat < dump.txt`
   (Berkeley DB 4.8 utilities; `-R` for the aggressive variant).
5. Raw key carving as the last resort: private keys sit in recognizable
   byte patterns and can be extracted from badly damaged files, then
   imported into Hive. Ask the maintainers before running third-party
   carving tools against key material.

After ANY successful recovery: sweep all funds to a freshly created
wallet and retire the damaged file permanently.
