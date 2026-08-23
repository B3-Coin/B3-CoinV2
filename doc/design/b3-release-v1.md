# B3 Hive — Release v1 checklist (the hard-fork client)

**Product name: B3 Hive** (owner, LOCKED 2026-08-22). FlowMesh remains the
name of the DEX engine inside it. Release v1 = the hard-fork client: legacy
continuity + temporary-PoW corridor + Modern PoS V1, with FN, colored
assets and FlowMesh shipped activation-inert behind later heights.

## Owner inputs (one line each; the release is cut when all are filled)

| # | Item | Status |
|---|---|---|
| 1 | H (final legacy height); corridor; M | **RULED 2026-08-23: H = 820,000; corridor = 820,001..821,000; M = 821,001.** NOT YET PINNED in mainnet chainparams — see the pin gates below. |
| 2 | X distribution | **RULED 2026-08-23: PAUSE, fail closed.** With H set and X unset the node accepts through H and refuses H+1; the mandatory follow-up release pins X and resumes the corridor. Nodes with blank X must never enter the corridor (consensus-enforced, see `legacy-boundary-unpinned`). |
| 3 | Seed nodes | **RULED 2026-08-23:** `176.31.13.198` is one fixed seed (added). Final release REQUIRES at least two additional independently hosted fixed seeds and a DNS seed under our control — both still **pending**. Do not hardcode explorer peers without operator approval. |
| 4 | Corridor difficulty constant (`transition_pow_bits`) | **RULED 2026-08-23: canonical compact `0x1f008000`** (the same target, 2^239, that `0x20000080` encodes non-canonically; 2^17 expected hashes per block). NOT YET PINNED — see gates. A canonical round-trip test pins the form; non-canonical configured bits fail closed. |
| 5 | Validator UX for v1 | **RULED 2026-08-23: yes** — wallet-held validator key, `createstake`, staking status, start/stop staking, PENDING/ACTIVE visibility, automatic staking loop. Nothing more advanced in V1. |
| 6 | Name / platforms | name **B3 Hive — LOCKED**; platforms: macOS arm64 here, Linux x86_64 needs a box/CI — **pending** |

## Pin / publish gates (owner, 2026-08-23)

**Do not pin or publish final activation parameters** (H/X, corridor
bits, M) until ALL of:

1. the live-sync legacy-mempool assertion bug is fixed;
2. the T3 and the final-H equivalence captures pass;
3. seed infrastructure is operational (the seeds in item 3 above);
4. release binaries are reproducible and audited.

Until then the ruled values live in the documents and tests only;
mainnet `hard_fork_height`, `legacy_final_hash` and `transition_pow_bits`
stay unset and the chain stays in live legacy operation.

## Corridor pacing — VERIFIED COMPRESSIBLE, then RULED (2026-08-23): 60 s spacing, 120 s future bound

Owner instruction: "verify corridor pacing cannot be compressed
unexpectedly by large hashpower; do not assume fixed difficulty implies
fixed elapsed time." Verified against the code: a corridor header is
bound only by the median-time-past rule and the stock 2-hour future
window; there is no minimum spacing. With a fixed 2^17-hash target a
scrypt farm can therefore produce the whole 1,000-block corridor in
minutes of wall-clock, carrying timestamps that advance 1 s per block
(a test pins this behaviour). Stake that needs 20 blocks to mature, and
operators who need hours to create STAKE outputs, would get neither. The
owner ruled the same day: corridor blocks must be at least 60 s apart and
at most 120 s ahead of the clock (`transition_pow_min_spacing` /
`transition_pow_max_future`, consensus, implemented and tested) — so the
corridor takes ≥ ~16.6 h of real time no matter the hashpower. Details:
[b3-during-fork-transition.md](b3-during-fork-transition.md) §6.1.

## Engineering plan (starts on inputs; "rest we keep improving")

- Day 1: fail-closed H/X semantics (done 2026-08-23); canonical corridor
  bits check + test (done); seed 176.31.13.198 (done); regtest/testnet
  override flags + first functional tests of the transition;
  `getblocktemplate` corridor support; STAKE-output standardness carve-in
  (relay) (done with the validator UX).
- Day 2: validator UX — DONE 2026-08-23 as ruled: wallet-held validator
  key (a non-active single-key `pk()` descriptor; public key recorded as
  `b3validatorpubkey`), `createstake <amount>` (STAKE carrier with a P2PKH
  owner of this wallet), `getstakinginfo` (validator key, node loop state,
  every owned STAKE output as UNCONFIRMED / PENDING / ACTIVE with
  `active_at_height`, active/total weight), `startstaking` / `stopstaking`
  (the node's automatic loop behind `interfaces::Chain`; fees to a fresh
  wallet address), STAKE outputs recognized as their bare owner script for
  standardness/ownership/signing (the relay carve-in) and never auto-selected
  for ordinary spends (unstake = explicit spend via `inputs`). Still owed:
  disk-format detection and reindex message; version strings ("B3 Hive");
  operator runbook for H → corridor → M; regtest override flags so the
  staking RPCs get functional tests (today they are unit-tested in-process:
  `modern_pos_tests/staking_loop_produces_blocks`,
  `wallet_tests/b3_validator_key_and_stake_outputs`).
- Day 3: full gate, packaging, release notes, tag. The mainnet pin of
  H/X/corridor bits happens ONLY after the pin gates above; the X-pin
  follow-up release when block H is buried (pause).

## Release-binary note (gate 4)

- FIXED 2026-08-23: `b3coin-tx`, `b3coin-util` and `b3coin-wallet` did not
  link because `chainparams.cpp` (`bitcoin_common`) referenced
  `legacy::MainnetCheckpoints()` compiled into `bitcoin_node`; the
  checkpoint table now lives in `legacy/checkpoints.cpp` inside
  `bitcoin_common`. All of `b3coind`, `b3coin-cli`, `b3coin-wallet`,
  `b3coin-tx`, `b3coin-util` and `test_bitcoin` build. Reproducibility and
  audit of the binaries remain the gate.

## Already true (no action)

Equivalence gate PASSED on real history (95,350 / 110,000 / 797,000); all
corridor and Modern-PoS numbers ratified; FN cap 5,000; colored-asset v1;
DEX-vault rulings structural; live-sync crash fixed. Full chain preserved
at height 807,709 with an offline bootstrap.
