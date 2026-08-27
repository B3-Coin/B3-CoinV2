# B3 Hive v1 — mainnet QA smoke (the production-path test)

The legacy-era wallet scope is SEND + RECEIVE. No synthetic harness
exercises the true production path (real chain, real mempool, old peers,
real signatures); this ten-minute live check does. Run it with TINY
amounts before distributing v1 to operators.

## Setup
1. Launch v1 B3 Hive on a synced mainnet datadir (default datadir picks
   up the legacy `B3-CoinV2` directory; or use an already-synced copy).
2. Wallet with a small real balance: migrate the legacy wallet.dat
   (`migratewallet`) or import a key. Verify the expected balance shows
   (denomination check: amounts read in B3, 1 B3 = 1,000 legacy).

## The smoke
3. RECEIVE: Request a new address in the GUI. MUST start with 'S'
   (legacy P2PKH). Confirm the address-type selector offers nothing else.
4. NEGATIVE: `b3coin-cli getnewaddress "" bech32` MUST fail with the
   witness-unavailable error. Same for `-addresstype=bech32` at startup.
5. SEND (self-spend): send a tiny amount (e.g. 0.001 B3) to your own new
   address. It must broadcast without error.
6. CONFIRM: the transaction must appear in the mempool of an OLD client
   or on the public explorer, and confirm in a legacy block. This single
   step proves format, signature (nTime preimage), relay to old peers
   and legacy consensus acceptance end to end.
7. ROUND-TRIP: `gettransaction <txid>` -> take the hex ->
   `decoderawtransaction <hex>` must show the same txid.
8. BALANCE: after 1 confirmation the wallet balance reflects the send
   (amount + fee), in B3 units.

## Pass criteria
Every step behaves as stated. Any deviation is a release stopper --
report the exact step and output.

Executed record:
- [ ] date/operator/txid: ____________________
