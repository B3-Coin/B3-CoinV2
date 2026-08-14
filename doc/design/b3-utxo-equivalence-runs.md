# B3 three-way equivalence — verification run log

Evidence log for real-data runs of the protocol in
[b3-utxo-equivalence.md](b3-utxo-equivalence.md). The activation gate requires
passes at T1=95350, T2=110000 and a recent well-buried T3.

## Run 2026-08-14 — T=0 (real genesis; pipeline baseline)

The only real B3 chain state present on the build machine is height 0, so this
run establishes the full pipeline on genuine chain identity at T=0. All three
producers ran for real: `master.rows` from the compiled legacy client
(`claude/b3-master-utxo-export` @ 9336752, offline, tip pinned to X), `port.rows`
and `replay.rows` from `b3coin-utxo-verify` against a current-format port
datadir at the same block.

- `T` = 0, `X_T` = `4b0d7f133c5267d715d4d8992635a5490d1edd6b7072cce3f8fe116aba983b6a`
  (checkpoint 0 in both codebases)
- Counts: master 0, port 0, replay 0 (the genesis coinbase is excluded by the
  membership predicate on all three sides)
- Commitment, identical for all three:
  `80c340e756be02fe427ce912959cbceed67982ee897c095769318cf9111b5da3`
- Pairwise: master==port EQUAL, master==replay EQUAL, port==replay EQUAL;
  tool exit 0 (three-way EQUAL)
- Row files byte-identical across producers; common SHA256
  `820d07026ae6445186953f27f945e63d400b94412921913a6e5d1d7b654e5135`

Mutation tests on the real `master.rows`:

- Consistent mutation (fabricated row added, count kept consistent):
  `master vs port: NOT EQUAL (1 vs 0 rows)` naming exactly
  `…00aa:0` with both canonical rows printed; three-way NOT EQUAL; exit 1. ✔
- Count-inconsistent file (row added, count untouched): deterministic refusal
  `count line says 0 rows but 1 were read`; exit 2 (never silently compared). ✔

Incidental finding: a datadir written by an earlier experimental build
(`~/Library/Application Support/B3Coin`, 2026-08-06) fails index load under the
current branch (older `CDiskBlockIndex` layout — expected); current-format
datadirs load fine. Captures must use datadirs written by the current branch.

## T1/T2/T3 — BLOCKED: no legacy chain history on this machine

Every local datadir holds genesis only. Producing T1=95350 / T2=110000 / T3
requires real history, and the standing safety rule forbids this environment
from connecting to production B3 nodes. Unblocking options (operator decision):

1. Provide chain data locally: an old-client datadir (`blk0001.dat` +
   `txleveldb`) or a bootstrap/blk-file set covering at least T3; the staged
   `-exportstopatheight` + `-loadblock` captures then run fully offline per the
   runbook.
2. Explicitly authorize a network sync on a designated machine; the runbook's
   capture procedure applies unchanged.

The gate stands: no H/X pin and no startup replay wiring until T1/T2/T3 pass.
