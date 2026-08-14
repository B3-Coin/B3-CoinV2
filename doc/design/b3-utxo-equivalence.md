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

## What each pairwise failure means

- `U_port != U_replay`, `U_master == U_port`: replay engine defect.
- `U_master != U_port`, `U_port == U_replay`: ported rule set diverges from the
  historical client (the exact fidelity failure this framework exists to catch);
  locate the earliest affected height by bisecting `T`.
- `U_master != U_replay` only: impossible if the other two hold; check the row
  files themselves.
