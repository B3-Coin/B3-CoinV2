# B3 UTXO equivalence: the three-way invariant

Status: **protocol locked, real-data run pending.** Nothing in consensus references
any of this; it is the migration acceptance test of contract §12.

## Three states, not two

For a boundary candidate `T = (H, X)` there are three independently produced legacy
UTXO states:

| State | Producer | What it exercises |
|---|---|---|
| `U_master(T)` | the **actual legacy client** (`master` branch) fully validating 0..H with its own code and its own database | the reference semantics |
| `U_port(T)` | this tree's **live legacy validation** (Core-31.1 port of kernel/rewards/difficulty/scripts) | the ported rule set |
| `U_replay(T)` | this tree's **trusted replay engine** (`legacy::TrustedReplay`) | the mechanical reconstruction |

The `b3coin-utxo-verify` command's original comparison — the live chainstate of a
synced port node against a replay of the same block files — establishes only
`U_port(T) == U_replay(T)`. That is the port-vs-replay half. It says nothing about
whether the port matches the historical client. **The migration invariant is
three-way:**

    U_master(T) == U_port(T) == U_replay(T)

for every unspent outpoint: value, script bytes, transaction identity, and the
metadata replay must preserve. **The boundary must not be pinned (H/X compiled into
mainnet params) and the pinned-mode replay path must not activate until this
three-way comparison passes on real B3 history.** All comparisons are over
canonical logical rows — never over raw database serialization, which differs by
construction between the 0.8-era `CTxIndex` model and the per-outpoint coins model.

## The canonical logical UTXO

A logical UTXO row exists for output `n` of transaction `txid` iff:

1. the transaction is in the main chain at or below `H` and output `n` is unspent
   at `T`;
2. the output is **not** a historical marker output: `nValue == 0` with an empty
   `scriptPubKey` (this includes the first output of every coinstake);
3. the `scriptPubKey` is **not** unspendable: it does not begin with `OP_RETURN`
   (0x6a) and is not larger than 10000 bytes;
4. the transaction is not the genesis coinbase (the historical client never
   indexed it; the port and replay never create its coin);
5. where the chain overwrote a txid (pre-BIP30-style duplicate), only the latest
   instance's outputs exist — all three producers already share this semantic.

Each row carries exactly: `txid`, `n`, raw amount (integer units), creation
height, coinbase flag, coinstake flag, the transaction's `nTime`, and the
transaction's in-block offset `nTxOffset` (bytes from the start of the serialized
block to the start of the transaction; in the legacy client this is
`nTxPos - nBlockPos`).

## Canonical row file format (`b3-utxo-rows/v1`)

Text, LF line endings, lowercase hex, single spaces, no padding. Byte-identical
across producers, so `diff`/`comm` work directly on two row files.

    b3-utxo-rows/v1
    tip_hash=<64-hex block hash of the tip the set was captured at>
    tip_height=<decimal>
    <txid-hex>:<n> <value> <height> <cb> <cs> <ntime> <ntxoffset> <script-hex>
    ...
    count=<decimal number of rows>

- `txid-hex` is the usual reversed-byte (display) hex; `cb`/`cs` are `0`/`1`.
- `script-hex` is the exact script bytes; an **empty script is written as `-`**
  (possible only for a spendable `value > 0` output with an empty script).
- **Ordering:** rows are sorted ascending by the outpoint's *raw serialized txid
  bytes* (memcmp order — NOT the display-hex string order, which reverses bytes),
  then by `n`. This equals the port's `COutPoint` ordering, and equals LevelDB's
  bytewise iteration order over the legacy client's `("tx", txid)` keys.
- Readers must reject: a missing/foreign format tag, unsorted or duplicate
  outpoints, malformed fields, and a `count` that disagrees with the rows read.

The diagnostic set commitment stays `node::UtxoSetCommitment` (domain
`b3/utxo-commitment/v1` over sorted outpoint+Coin serializations). Only the new
tree computes it — for all three states, from rows or views uniformly — so the
0.8-era codebase never reimplements the Coin fold. Mismatch diagnosis is by rows.

## Producing each state at the same `T`

- **`U_master(T)`** — branch `claude/b3-master-utxo-export` (on the legacy
  codebase): run the old client offline (`-connect=0 -listen=0 -dnsseed=0`) on a
  datadir synced exactly to `T` — deterministically reachable by importing a
  bootstrap file truncated after block `H` into a fresh datadir — then run once
  with `-exportutxo=<file> -exportutxoat=<X-hex>`. The exporter walks the client's
  own LevelDB `("tx", …)` index with the client's own deserialization, refuses to
  run unless `hashBestChain == X` at height `H`, maps every entry to its
  main-chain block by `(nFile, nBlockPos)`, hard-fails on any stale or
  inconsistent entry, applies the membership rules above, streams rows in index
  order (already canonical), and exits before any network start.
- **`U_port(T)`** — a port node synced with `-stopatheight=H` (verified best block
  `== X`); `b3coin-utxo-verify -datadir=… -height=H -hash=X -portrows=<file>`.
- **`U_replay(T)`** — the same invocation's replay side; `-replayrows=<file>`.

`b3coin-utxo-verify -masterrows=<file>` ingests a master row file and reports the
full three-way verdict: one commitment per state, pairwise equality, and bounded
per-row differences for every unequal pair. Exit 0 only when all three agree.

## The exact membership predicate of `U_master` (audited)

The exporter emits a row for output `n` of the transaction with id `txid` **iff
all** of the following hold, where every datum comes from the legacy client's
own structures:

1. **Indexed:** a `("tx", txid)` record exists in the client's LevelDB
   transaction index. Audited lifecycle: `ConnectBlock` queues
   `CTxIndex(posThisTx, vout.size())` for *every* transaction of a connected
   block (coinbase and coinstake included) plus the spent-markers of its
   inputs, and all writes commit inside the `SetBestChain`/`Reorganize`
   `TxnBegin`/`TxnCommit` batch — the index and `hashBestChain` move
   atomically; `DisconnectInputs` erases the record and un-marks its inputs'
   spent-markers on disconnect; the mempool path (`fBlock=false`) never writes
   the database; `AddTxIndex` is uncalled; the genesis coinbase is never
   indexed (`SetBestChain` short-circuits genesis before `ConnectBlock`).
2. **Unspent by the client's own spent-state:** `vSpent[n].IsNull()` in that
   record. The exporter aborts if `vSpent.size()` disagrees with the
   deserialized transaction's `vout.size()`.
3. **On the canonical chain ending at `T`:** the record's
   `(pos.nFile, pos.nBlockPos)` matches a block on the best chain walked back
   from `pindexBest`, whose tip the exporter requires to be exactly `X_T`
   (else it refuses to run). An entry with an unspent output whose block is
   *not* on that chain aborts the export. (Records whose outputs are all
   spent are skipped before this check: the client legitimately retains
   fully-spent duplicates from reorganized-away chains — the documented
   `EraseTxIndex` caveat — and they contribute no rows.)
4. **Master's own deserialization:** amount, script bytes and `nTime` come
   from `CTransaction::ReadFromDisk(pos)` through the client's own codec; the
   exporter aborts unless the re-read transaction hashes to the indexed
   `txid`. Height comes from the client's own block index; the in-block
   offset is `pos.nTxPos - pos.nBlockPos`.
5. **Not excluded by the shared logical rules:** not a marker output
   (`nValue == 0` with an empty script — every coinstake's first output), not
   an unspendable script (leading `OP_RETURN` or larger than 10000 bytes),
   not at height 0.
6. **Duplicate-txid overwrites:** the index holds one record per txid, the
   later instance having overwritten the earlier (`UpdateTxIndex`), matching
   the port's one-coin-per-outpoint overwrite semantics by construction.

Rows stream out in LevelDB's bytewise key order — which *is* the canonical
raw-txid-byte order — and the exporter verifies the ordering as it writes
instead of assuming it.

## Verified exporter build (arm64 macOS reference recipe)

The export branch compiles and links unmodified legacy sources; commit
`makefile.osx-arm64` documents the only build-system changes. Staged
dependencies (source-built, no system installs; SHA256 as published):
OpenSSL 1.0.2u (`ecd0c6…9d16`; `no-asm no-shared`, `-arch` switched to
arm64), Berkeley DB 4.8.30.NC (`12edc0…64ef`; the standard
`__atomic_compare_exchange`/`atomic_init` renames applied *to the
dependency*), boost 1.63 (`fe34a4…088b`; the five static libs compiled
directly with `-std=c++11`), and the tree's vendored LevelDB. Then:

    make -f makefile.osx-arm64 STATIC=1 DEPSDIR=<staging>/local b3coind

Verified on a scratch datadir with `-connect=0 -listen=0 -dnsseed=0`: the
export mode refuses a tip that is not the requested capture block (fresh
datadir at genesis vs. a foreign hash), and succeeds at the genuine B3
genesis `4b0d7f13…83b6a` (matching the port's pinned genesis and checkpoint
0), emitting a well-formed `b3-utxo-rows/v1` file with `count=0` — the
genesis coinbase is excluded by predicate.

## The three real-history capture heights

Run the full three-way comparison at **three** real heights before any H/X
decision — each `T` with its exact real block hash `X_T`:

| | `T` | `X_T` source | What it exercises |
|---|---|---|---|
| 1 | **95350** (the highest hardened checkpoint) | pinned identically in both codebases' checkpoint maps: `095f1cb3cf1f1300ad99f891c2c0bb13cc374d9215781ad988e82cc0086a8e45` | plain-era history under a hash both implementations already assert |
| 2 | **110000** | old client `getblockhash 110000`, cross-checked against an independent explorer | the entire unusual-reward region: the 77447–77505 repair window, the restricted-stake rule from 78001, the 107488 superblock (checkpoint 78961 is a pinned-hash intermediate if a closer anchor is wanted) |
| 3 | **tip − 10000, rounded down to a thousand** (recent, well buried) | old client + explorer cross-check | the full modern-era state size and every rule change up to the present |

Capture procedure (staged, forward-only — one sync pass):

1. Old client (export branch), isolated once synced history is available
   locally; sync with `-exportstopatheight=95350`; the node freezes and shuts
   down at exactly that height. Copy the datadir. Run
   `b3coind -exportutxo=master-95350.rows -exportutxoat=<X_95350>`
   (with `-connect=0 -listen=0 -dnsseed=0`).
2. Restart on the same datadir with `-exportstopatheight=110000`; repeat.
   Then again for `T3`. Keep every per-height datadir copy — bisection reuses
   them.
3. Port node per height: fresh sync with `-stopatheight=T` (verify best block
   `== X_T`), stop cleanly, then
   `b3coin-utxo-verify -datadir=… -height=T -hash=X_T
    -portrows=port-T.rows -replayrows=replay-T.rows
    -masterrows=master-T.rows`.
4. Required for each `T`: equal row counts, equal commitments, and
   `diff master-T.rows port-T.rows` / `diff master-T.rows replay-T.rows`
   empty (byte-identical canonical rows). The tool exits 0 only on full
   three-way agreement.

**On inequality:** the tool prints the differing outpoints in canonical order
— the first one is the entry point. Bisect by height: re-run the capture at
the midpoint between the highest agreeing `T` and the lowest disagreeing `T`
(forward-staged datadir copies make each probe a resume, not a resync) until
the first divergent height is isolated; then diagnose that block's
transactions against the pairwise-failure table below.

**Mutation (negative) test — run once per campaign on a real `master.rows`:**

- *Consistent mutation:* copy the file, change one row's value field (keep
  the row count intact), re-run with `-masterrows=<mutated>`. Required
  result: `master vs port: NOT EQUAL` naming exactly that outpoint with both
  canonical rows printed, and exit status 1.
- *Naive deletion:* delete one row without adjusting `count=`. Required
  result: deterministic rejection `count line says N rows but N-1 were read`,
  exit status 2 — a malformed reference file is refused outright, never
  silently compared.

## What each pairwise failure means

- `U_port != U_replay`, `U_master == U_port`: replay engine defect.
- `U_master != U_port`, `U_port == U_replay`: ported rule set diverges from the
  historical client (the exact fidelity failure this framework exists to catch);
  locate the earliest affected height by bisecting `T`.
- `U_master != U_replay` only: impossible if the other two hold; check the row
  files themselves.
