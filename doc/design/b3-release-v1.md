# B3 Hive — Release v1 checklist (the hard-fork client)

**Product name: B3 Hive** (owner, LOCKED 2026-08-22). FlowMesh remains the
name of the DEX engine inside it. Release v1 = the hard-fork client: legacy
continuity + temporary-PoW corridor + Modern PoS V1, with FN, colored
assets and FlowMesh shipped activation-inert behind later heights.

## Owner inputs (one line each; the release is cut when all are filled)

| # | Item | Status |
|---|---|---|
| 1 | H (final legacy height); M = H + 1001 | proposed 815,000 — **pending** |
| 2 | X distribution: pause (ship H, X blank; pin X when block H is buried) vs precommit | recommended pause — **pending** |
| 3 | Seed nodes (IPs the owner/team keeps up; DNS seed if any) | **pending** |
| 4 | Corridor difficulty constant (`transition_pow_bits`) | proposed `0x20000080` (2^17 hashes/block; measured 7,466 H/s single core) — **pending** |
| 5 | Validator UX for v1: wallet-held validator key + node staking loop + STAKE RPCs | recommended yes — **pending** |
| 6 | Name / platforms | name **B3 Hive — LOCKED**; platforms: macOS arm64 here, Linux x86_64 needs a box/CI — **pending** |

## Engineering plan (starts on inputs; "rest we keep improving")

- Day 1: release chainparams (H, corridor bits, fees-only reward, min
  stake, seeds, X blank); regtest/testnet override flags + first functional
  tests of the transition; `getblocktemplate` corridor support; STAKE-output
  standardness carve-in (relay).
- Day 2: staking loop + RPCs + wallet STAKE creation; disk-format detection
  and reindex message; version strings ("B3 Hive"); operator runbook for
  H → corridor → M.
- Day 3: full gate, packaging, release notes, tag. X-pin follow-up
  scheduled for when block H is buried (pause).

## Already true (no action)

Equivalence gate PASSED on real history (95,350 / 110,000 / 797,000); all
corridor and Modern-PoS numbers ratified; FN cap 5,000; colored-asset v1;
DEX-vault rulings structural; live-sync crash fixed. Full chain preserved
at height 807,709 with an offline bootstrap.
