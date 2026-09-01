# B3 Hive v1.1.0-beta.2 — Transition Beta

The beta.2 binaries were rebuilt in place with two transition-blocker fixes.
Block assembly now skips a competing stale `FINALITY_KEY` transaction instead
of failing `generatetoaddress` with `finality-key-bad-seq` (or
`finality-key-bls-key-in-use`). Legacy dump recovery into a blank descriptor
wallet now also creates fresh active receiving/change descriptor chains, so
the recovered wallet can fund `bindfinalitykey` and other modern transactions
without first running `migratewallet`. Operators who downloaded an earlier
beta.2 build should replace it and verify it against the newly published
checksums.

This prerelease keeps the consensus and sealed-history commitments from
v1.1.0-beta.1: legacy height 810,000 and its hash, the deterministic 3,592-row
historical FN Genesis manifest, the temporary proof-of-work corridor, Modern
PoS, colored assets, and the FlowMesh activation schedule.

Beta.2 is the operator-UX follow-up to beta.1. Its release gate is an honest Qt
surface for the validator workflow: public validator and BLS status, BLS-key
binding, native-B3 stake creation and status, explicit start/stop controls for
Modern-PoS staking, and explicitly paced corridor-block production.

These controls do not introduce a miner that starts with the application.
Corridor production is an explicit opt-in using one CPU thread, bounded
`generatetoaddress` attempts, and deliberate pacing. It stops automatically
when Modern PoS begins. Starting Modern-PoS staking is also explicit; after it
starts, the node keeps the required signing keys in memory and continues
staking if the wallet is re-locked, until the operator selects stop or shuts
down the application. Stopping clears those in-memory copies. Merely launching
B3 Hive never starts either path.

Block 810,001 must contain the deterministic FN Genesis coinbase. Public BLS
validator bindings and native-B3 stakes are ordinary on-chain corridor data;
they are not hardcoded into this beta. Operators may prepare BLS keys in advance
and include valid binding and stake transactions from block 810,001 onward.

For the first live corridor block, use the built-in `generatetoaddress` mining
path rehearsed by this release. Generic external `getblocktemplate` miners are
not qualified for the corridor unless they preserve every required FN Genesis
coinbase output and use the corridor's scrypt eligibility hash.

An operator synced exactly to block 810,000 can unlock a descriptor wallet and
inspect finality state before binding a BLS key. Binding needs fee funds but no
stake. Its transaction may wait in the modern next-block mempool and be mined in
block 810,001. Share only the public `validator_key`, public `bls_pubkey`, and
transaction id. Create a fresh full-wallet backup after key creation and never
share private validator or BLS key material.

Beta.2 retains the beta.1 package set: regular Qt packages for supported desktop
platforms and the fully static x86-64 Linux operator package containing
`b3coind`, `b3coin-cli`, `b3coin-tx`, `b3coin-util`, and `b3coin-wallet` without
Qt. Automated Windows publication remains x86-64 only; Win32 remains deferred
pending a separate architecture and runtime review.

This beta uses the same numeric client and protocol version as v1.1.0 while its
package and displayed build version carry the explicit `-beta.2` suffix. It is
a GitHub prerelease, is not the latest stable release, and must not replace the
mandatory shadow-fork rehearsal or final release verification described in
`doc/b3-transition-release-runbook.md`.

Back up `wallet.dat`, shut down the old client cleanly, and verify every download
against the published SHA-256 checksums before testing.

Follow the complete
[validator setup and console fallback guide](https://github.com/B3-Coin/B3-CoinV2/blob/v1.1.0-beta.2/doc/b3-staking-guide.md)
before creating a live binding or stake.
