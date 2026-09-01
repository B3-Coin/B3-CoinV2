# B3 Hive v1.1.0-beta.1 — Transition Beta

This prerelease packages the pinned B3 transition rules for final operator
rehearsal before the stable v1.1.0 release. It pins legacy height 810,000 and
its hash, the deterministic 3,592-row historical FN Genesis manifest, the
temporary PoW corridor, Modern PoS, colored assets, and FlowMesh activation
schedule.

Beta.1 repairs the Linux release link boundary caught by the beta build: the
offline `b3coin-wallet` utility no longer pulls full-node bridge symbols into
its executable. The bridge validation behavior and all consensus rules are
unchanged. Beta.1 also uses the project's portable wide-integer type for
FlowMesh vault accounting, preserving the same checks on the genuine 32-bit
Windows build.

The release now also includes a fully static x86-64 Linux operator package
built against musl. It contains `b3coind`, `b3coin-cli`, `b3coin-tx`,
`b3coin-util`, and `b3coin-wallet`; it intentionally does not contain the Qt
GUI. The regular Linux package continues to include the Qt wallet.

Block 810,001 must contain the deterministic FN Genesis coinbase. Public BLS
validator bindings and native-B3 stakes are ordinary on-chain corridor data;
they are not hardcoded into this beta. Operators may prepare their BLS keys in
advance and include valid binding transactions from block 810,001 onward.

For the first live corridor block, use the built-in `generatetoaddress` mining
path rehearsed by this release. Generic external `getblocktemplate` miners are
not qualified for the corridor unless they explicitly preserve every required
FN Genesis coinbase output and use the corridor's scrypt eligibility hash.

An operator synced exactly to block 810,000 can unlock a descriptor wallet and
run `getfinalityinfo`, then `bindfinalitykey`. Binding needs fee funds but no
stake. The transaction may wait in the modern next-block mempool and be mined
in block 810,001. Share only its public `validator_key`, public `bls_pubkey`,
and transaction id. Create a fresh full-wallet backup after key creation and
never share private BLS material.

This beta uses the same numeric client and protocol version as v1.1.0 while its
package and displayed build version carry the explicit `-beta.1` suffix. It is
a GitHub prerelease, is not the latest stable release, and must not replace the
mandatory shadow-fork rehearsal or final release verification described in
`doc/b3-transition-release-runbook.md`.

Back up `wallet.dat`, shut down the old client cleanly, and verify every
download against the published SHA-256 checksums before testing.
