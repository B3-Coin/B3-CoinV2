# B3 Transition Release Runbook (the X-pin release)

The complete, ordered procedure for turning the sealed values of block
810,000 into the public transition release. Complete every operational
precondition in advance; on seal day only the two MEASURED VALUES change.

## 0. Preconditions (before the seal)

- `finalized` pushed and the release workflow green for all five package
  variants: Linux x86_64, Windows x86_64, Windows x86, macOS arm64 and
  macOS x86_64.
- A fully synced B3 Hive node and the independent legacy-client snapshot
  required by the three-way equivalence protocol, ready to freeze at H.
- Update installation remains manual. Do not call the updater enabled unless
  the tagged source contains a reviewed compile-time manifest URL and
  threshold public keys, and each shipped format has working, tested installer
  support. Keep private signing keys offline; do not use placeholder keys or
  URLs.

## 1. Read the seal (at block 810,000)

On the port node frozen exactly at H (do not measure from an advanced tip):

    b3coin-cli getblockcount                   -> 810000
    b3coin-cli getblockhash 810000             -> X (the seal)
    b3coin-cli gettxoutsetinfo                 -> bestblock = X, total_amount = S_H

Cross-verification is the whole point: X printed by the port node must match
`getblockhash 810000` from at least one independent old client before it is
pinned.

### Mandatory final-H equivalence gate

Before any mainnet pin, follow `doc/design/b3-utxo-equivalence.md` at
T = (810,000, X) and capture `master-H.rows`, `port-H.rows` and
`replay-H.rows`. Preserve the row files, SHA-256 hashes, row counts,
commitments, command logs and the comparator's direct (unpiped) exit status.
The mandatory result is exit 0 and byte-identical canonical rows:

    U_master(H, X) == U_port(H, X) == U_replay(H, X)

Any row, commitment, height or hash mismatch stops the pin and release tag.

## 2. Compute the two derived numbers

    R0 = floor(S_H_base_units * 1% / 525,600)
       = floor(S_H_base_units / 52,560,000)     # S_H in base units (1 B3 = 1e9)

Treasury script (ruled 2026-08-26, address SNyANHiUkuqPSfbeKHDXzVD86LC2ZUUjLX):

    76a91412602418ffc74640e37f1a73d0cdc255d2a07c3588ac

Seal packet checklist — every item is required before editing chainparams:

- [ ] Every capture source reports H = 810,000 and the same 64-hex X; the raw
      `gettxoutsetinfo` output reports `bestblock = X`.
- [ ] S_H, its exact integer base-unit conversion and the R0 calculation are
      recorded without floating-point arithmetic.
- [ ] The preserved final-H artifacts prove
      `U_master == U_port == U_replay`, including exit 0 and artifact hashes.
- [ ] Every other Modern PoS field has a recorded owner ruling; a missing,
      provisional or inferred activation value stops the release.

## 3. Pin into CMainParams (src/kernel/chainparams.cpp)

    consensus.legacy_final_hash = uint256{"<X>"};
    consensus.modern_pos = Consensus::ModernPosParams{};        // ratified block:
    //   block_interval_seconds = 60, round_seconds = 30, f0 = 1/1   (2026-08-21)
    //   sentinel_bits = 0x207fffff, max_future_seconds = 120        (must be ratified before pin)
    //   reward = <R0>, halving_interval = 525'600                   (OD-2, 2026-08-26)
    //   treasury_percent = 10, treasury_script = <hex above>        (OD-2, 2026-08-26)
    //   reorg_horizon = 1440; finality block per ruling M7           (2026-08-21/23)

Use only the measured X/R0 from the seal packet and recorded owner-ratified
values. Do not infer a missing value at seal time.

min_stake_amount (333 B3) and transition_pow_bits (0x1f008000) are
already pinned and need no change.

## 4. Code, tests and release notes that MUST change in the same commit

- Bump `CLIENT_VERSION_MINOR` in `CMakeLists.txt` so the source version is
  1.1.0, and add `doc/release-notes-v1.1.0.md`. Do not tag until the workflow's
  source-version check and release-notes path both match `v1.1.0`.

- The guard test asserting "real chainparams never configure Modern PoS"
  (modern_pos_tests / finality_activation_tests pause-shape assertions):
  flip to assert the FULL pinned configuration, value by value,
  including X itself.
- Add an assertion pinning R0's exact value so no rebuild can drift it.
- The release notes must say installation is manual unless the updater
  precondition in section 0 was actually completed and tested.

## 5. Ship

1. Commit on the working branch; merge to `finalized`.
2. Full local build + unit suites (minimum: modern_pos_tests,
   finality_activation_tests, fn_claim_tests, stake suites, era tests).
3. Tag `v1.1.0` (transition release), push finalized + tag, refresh
   `release/*` branch; CI packages all five variants listed in section 0.
4. Attach binaries + SHA-256 to the GitHub release. Release notes MUST
   print X prominently with the one-line self-verification instruction:
   `getblockhash 810000` on any old client.
5. Only if the updater precondition in section 0 is satisfied, publish and
   sign the update manifest. Otherwise publish manual download links and
   checksums and state plainly that update installation remains manual.
6. Announce that the X-pin transition release is live and the corridor has
   opened (Discord + YouTube pinned comments), with the seal value in the
   message.

## 6. After adoption

The corridor opens with the first mined block on X. Stakers deposit
during the corridor (doc/b3-staking-guide.md). At 811,001 the modern
era begins on the ratified parameters. The through-H FN report is run
against the sealed chain to fix final R (doc/design/b3-fn-pod.md).
