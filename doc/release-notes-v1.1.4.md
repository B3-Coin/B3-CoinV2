# B3 Hive v1.1.4

This update improves finality signature delivery, validator key rotation,
wallet diagnostics and the desktop interface. It also adds explicit,
operator-approved recovery for an intact orphaned signer journal.

## Included

- Mainnet signers wait until checkpoints are 20 blocks deep. This reduces
  shallow-fork exposure; it does not make forks impossible.
- Bounded retention and replay of finality messages, including nearby blocks
  not yet received, verification-budget retries and newly connected peers.
- Snapshot-aware finality keys: a key rotation no longer makes the wallet use
  only the newest binding when the current epoch still requires the old key.
- `getfinalitystatus` reports verified checkpoint votes, counts, signed stake
  and both quorum thresholds. These describe this node's observations.
- New RPCs: `getfinalityrecoveryinfo`, `setfinalityrecovery` and
  `clearfinalityrecovery`. Recovery is **off by default** and requires approval
  of one exact public incident and anchor. Configuration is not recovery.
- Readable dark-theme recipient and console buttons, corrected B3 address
  placeholder, dashboard/navigation fixes and staking-only unlock workflow.
- The top-right wallet selector is a visible dropdown that switches the Send,
  Assets, Stake and console wallet context. It lists loaded wallets; open
  another through File > Open Wallet if only one is loaded.
- Wallet unlock failures restore the previous spending lock state; fixes a
  staking-page wallet-lifetime crash during shutdown or wallet unload.
- Ethereum-to-B3 relayer treats a confirmed light-client update awaiting B3
  finality as a wait condition, avoiding unnecessary external proof requests.

## Upgrade and recovery

Back up and retain your wallet, `finality_signer` journal and relayer database.
Shut down the old wallet cleanly before starting the new one. Never run two
signers for the same validator identity or restore an older signer journal.

Ordinary staking and epoch-key selection do not need a recovery manifest.
Normal qualifying-certificate recovery remains available. For explicitly
coordinated recovery when that normal path is unavailable, follow
[the recovery RPC guide](https://github.com/B3-Coin/B3-CoinV2/blob/v1.1.4/doc/finality-recovery-rpc.md)
and the built-in RPC help.
There is no generic force-unlock and no repair of a deleted or corrupt journal.
Previously emitted signatures are not revoked by recovery. An approved
20-deep anchor is an operator trust decision, not proof that no conflicting
certificate exists. No mainnet incident or anchor is preapproved in this release.

Older wallets remain compatible with the existing V1 consensus and messages.
The quorum rules, checkpoint schedule, certificate format, journal format and
protocol version 80010 are unchanged. Incoming votes retain the existing
consensus depth; the additional signing delay is local policy. Do not pass
the new recovery options to older software.

Available older snapshot keys are retained for epoch handover. An overwritten
imported private key cannot be recreated by this update. Delivery improvements
cannot create missing votes or guarantee quorum, and wallet recovery alone
does not guarantee that a stale Ethereum bridge verifier can resume.

Release artifacts are unsigned platform builds with SHA-256 checksums. macOS
packages require macOS 15 or newer. Qualification details are recorded in
[the qualification notes](https://github.com/B3-Coin/B3-CoinV2/blob/v1.1.4/doc/release-notes/release-notes-1.1.4.md); long integration/soak suites are not
claimed as part of the focused release qualification.
