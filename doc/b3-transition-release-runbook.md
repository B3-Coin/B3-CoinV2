# B3 Transition Release Runbook (the X-pin release)

The complete, ordered procedure for turning the sealed values of block
810,000 into the public transition release. Everything here is prepared
in advance; on seal day only the two MEASURED VALUES change.

## 0. Preconditions (before the seal)

- `finalized` pushed and the release workflow green on all 3 platforms.
- A fully synced B3 Hive node running against mainnet.
- Updater signing keys generated (`b3hive-sign genkey`, 3 keys, offline)
  and the manifest URL chosen — the transition release ships with the
  updater ENABLED so it is the last manual install.

## 1. Read the seal (at block 810,000)

On the synced node (any old client must agree — cross-check at least one):

    b3coin-cli getblockhash 810000          -> X (the seal)
    b3coin-cli gettxoutsetinfo               -> total_amount = S_H (in B3)

Cross-verification is the whole point: X printed by OUR node must match
`getblockhash 810000` from independent old clients before it is pinned.

## 2. Compute the two derived numbers

    R0 = floor(S_H_base_units * 1% / 525,600)
       = floor(S_H_base_units / 52,560,000)     # S_H in base units (1 B3 = 1e9)

Treasury script (ruled 2026-08-26, address SNyANHiUkuqPSfbeKHDXzVD86LC2ZUUjLX):

    76a91412602418ffc74640e37f1a73d0cdc255d2a07c3588ac

## 3. Pin into CMainParams (src/kernel/chainparams.cpp)

    consensus.legacy_final_hash = uint256{"<X>"};
    consensus.modern_pos = Consensus::ModernPosParams{};        // ratified block:
    //   block_interval_seconds = 60, round_seconds = 30, f0 = 1/1   (2026-08-21)
    //   sentinel_bits = 0x207fffff, max_future_seconds = 120        (ratify at pin)
    //   reward = <R0>, halving_interval = 525'600                   (OD-2, 2026-08-26)
    //   treasury_percent = 10, treasury_script = <hex above>        (OD-2, 2026-08-26)
    //   reorg_horizon = 1440; finality block per ruling M7           (2026-08-21/23)

min_stake_amount (333 B3) and transition_pow_bits (0x1f008000) are
already pinned and need no change.

## 4. Tests that MUST change in the same commit

- The guard test asserting "real chainparams never configure Modern PoS"
  (modern_pos_tests / finality_activation_tests pause-shape assertions):
  flip to assert the FULL pinned configuration, value by value,
  including X itself.
- Add an assertion pinning R0's exact value so no rebuild can drift it.

## 5. Ship

1. Commit on the working branch; merge to `finalized`.
2. Full local build + unit suites (minimum: modern_pos_tests,
   finality_activation_tests, fn_claim_tests, stake suites, era tests).
3. Tag `v1.1.0` (transition release), push finalized + tag, refresh
   `release/*` branch; CI builds all 3 platforms.
4. Attach binaries + SHA-256 to the GitHub release. Release notes MUST
   print X prominently with the one-line self-verification instruction:
   `getblockhash 810000` on any old client.
5. Publish + sign the update manifest so future releases auto-notify.
6. Announce Phase 2 complete / Phase 3 live (Discord + YouTube pinned
   comments), with the seal value in the message.

## 6. After adoption

The corridor opens with the first mined block on X. Stakers deposit
during the corridor (doc/b3-staking-guide.md). At 811,001 the modern
era begins on the ratified parameters. The through-H FN report is run
against the sealed chain to fix final R (doc/design/b3-fn-pod.md).
