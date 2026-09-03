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


## Campaign 2026-08-22 — REAL HISTORY, ALL THREE GATES: **PASSED**

Data source: live-network sync (owner-authorized 2026-08-21; peer
176.31.13.198 plus explorer-listed nodes). The port client synced and fully
validated the real chain to height 807,709 (live tip at capture time); the
compiled legacy client (`claude/b3-master-utxo-export` @ 9336752, arm64
rebuild) validated the identical history offline from a de-XOR bootstrap of
the port's block files (magic-verified b3 2e 1e e6), freezing at each
capture height via -exportstopatheight. All exports and comparisons offline.

| T | X_T (old client == explorer, cross-checked) | rows | three-way |
|---|---|---|---|
| 95,350 | 095f1cb3cf1f1300ad99f891c2c0bb13cc374d9215781ad988e82cc0086a8e45 | 86,374 | **EQUAL**, commitment 20594665fbd77086add00595ada5c76d22b964130b7a3c2f66dd1f1c6ca321f7 |
| 110,000 | 2f5eece12025b19f2229b11d8dc06a017264bb7560af9073b5d19c0ff9e3f9c7 | 115,835 | **EQUAL**, commitment 9cf042db50ddcf369dfc1980f0f911e7bc952400bf63bb9fb923ae049c3a11eb |
| 797,000 | 05a34afe1651642a893dc581b7957564e5d7e9a8856aee6dc7de71dbbc28c741 | 1,235,918 | **EQUAL**, commitment 7402ee2abb7071da75ba574678d2565972a7eebca19e558c4376d85683de62ee |

Mutation-negative (per runbook, once per campaign, on the real T1 rows),
exit codes captured UNPIPED: a single value edit (+1 base unit) → NOT
EQUAL naming COutPoint(cf21b7b640, 1), **exit 1**; a naively dropped row →
"count line says 86374 rows but 86373 were read", **exit 2**. The
comparator provably distinguishes and its exit contract holds.

Artifacts preserved (owner directive: never delete): full port datadir at
807,709 (~/development/ON/b3-mainnet-sync-v2), frozen old-client datadirs at
each T (b3-capture-T{1,2,3}-*), all row files and the 637 MB de-XOR
bootstrap (b3-eqgate/) — the bootstrap alone suffices to rebuild any node
offline.

**Consequence: the T3 H/X equivalence gate of
[b3-utxo-equivalence.md](b3-utxo-equivalence.md) is SATISFIED.** The owner
subsequently pinned H = 810,000 with X blank (pause-fail-closed). Remaining
for the X-pin release: capture and verify once more AT H, then pin the
observed X.

**FN RELEASE GATE FINDING (same run, port-T3 state, equivalence-gated
-podreport): R = 3,500 qualifying historical PoDs, all 3,500 claimable
(supported funding scripts + canonical 1-COIN P2PKH markers; tiers 25M/20M/15M
all represented). R exceeds the ratified MAX_FN_EVER_ISSUED = 1,000.** Per
the recorded gate rule (commit 0714074): do not pin H/X in an FN-activating
release, never truncate rights — the cap-vs-R conflict RETURNS TO THE OWNER.
One sequencing note: the cap only binds an FN-activating release; H/X
pinning for the base chain is not blocked if FN activation ships behind a
later height, but the cap must be re-ruled before ANY FN issuance activates.

**SUPERSEDED/RESOLVED IN PART (owner rulings 2026-08-22 and 2026-08-28):**
the lifetime cap is now 5,000 and the modern price table is pinned. This
run's `R = 3,500` was measured at height 807,709, so it is a reservation
floor, not the final through-H count. The mandatory report at H fixes final
R before FN activation. All final historical rights are reserved; reachable
modern capacity is at most 1,500 and may be smaller. If final `R > 5,000`,
FN activation returns to the owner and no historical right is truncated.

**LATEST SUPERSESSION (owner ruling 2026-09-01):** FN no longer activates
behind a later historical-claim height. Block 810,001 coinbase is mandatory FN
Genesis, so the final through-H manifest/count/root reproduction is a gate for
the X-pin transition release itself and must finish during the seal pause before
tagging. The 3,500 result remains a floor; final modern capacity is exactly
`5,000 - R`. No holder claim or proof-carrier measurement remains.

## Final seal 2026-09-01 — H=810,000: **PASSED**

The cleanly stopped legacy client, current port chainstate, and an independent
trusted replay all resolved height 810,000 to
`2413ba59476afb9a01b971c350b2c5a51494b37925055be42dde774f30d865c6`.
Each state contained 1,241,032 UTXOs and produced commitment
`5e7011beee57f403be78ce66c983218e1ee5c934d60dc4b43503d2d6a70760c7`.
All three canonical row files were byte-identical with SHA-256
`f1746c935ce4445841b179633d35387c0cbf81ede139ea8f4f6b6e2b88b55f89`;
the verifier exited 0 with `U_master == U_port == U_replay`.

The sealed spendable supply was 1,042,617,596.101695152 B3 in modern display
units, or 1,042,617,596,101,695,152 base units. Independent manifest producers
then derived the same 3,592 eligible historical FN rights (3,596 qualifying,
four ignored), modern capacity 1,408, chain domain
`6a48d15d8da05571e0e7afe5d49bfae0ca7cd71305297f04461603e92a2651a6`,
and rights root
`e8f282a7dcaa9a8fbcfcc5c22ba4f456e5b50968fcf899aaacdaca65bef898ec`.
Their 186,875-byte canonical artifacts were byte-identical with SHA-256
`c80470eec785600f33fa2e69c520ff331c2b354ebf6e0a9bf8cae7d1eb5f9dca`.
This closes the final-H X and FN-manifest gates without truncating any right.

Release-candidate database reconstruction also passed. A fresh
`-reindex-chainstate` replay reached H/X with 1,241,032 UTXOs and shut down
cleanly. A separate full `-reindex` imported all five raw block files
(119,938 + 118,284 + 205,696 + 214,487 + 151,596 = 810,001 blocks), then
connected and validated the chain through H = 810,000 at the exact X above,
again with 1,241,032 UTXOs and a clean shutdown. The full reindex exposed and
closed a legacy raw-block import codec bug; an in-process regression now loads
a framed historical block through `LoadExternalBlockFile` and verifies its
legacy transaction encoding.

Incident log (for the record): the port node crashed once mid-sync on a
relayed unconfirmed legacy transaction (CalculateLockPointsAtTip assertion)
— worked around with -blocksonly during this campaign and subsequently
fixed by commit `e2c2d36`; the 2016
client's seed infrastructure and the b3nodes.net DNS seeds are dead, so
peers must come from the explorer list or operator-supplied addnodes.
