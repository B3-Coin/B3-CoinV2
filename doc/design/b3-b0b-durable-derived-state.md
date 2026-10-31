# B0b: durable derived state

**Status: DESIGN for owner review; no code.** Written 2026-10-31 against
`r0-rebirth` at 070cc37b, the B0a commit. It changes no block validity,
height, rule or parameter. B0 was authorized on 2026-10-29
(`doc/design/b3-height-c-fast-track.md` §7, ask 1). B0b adds files that a
node trusts at startup, so their format and trust rules are up for review
before any code.

## 1. What is left of B0

The plan defines B0 (`doc/design/b3-flowmesh-greenfield-plan-20261016.md:372`):

> **Month 1 fix (B0):** bounded, durable per-block undo for derived state
> (finality, stake, seats, vault, bridge), proven equal to full replay. This
> removes the 97 s replay under `cs_main` (CONS-3b). It is an implementation
> fix, not a consensus change.

B0a (070cc37b) gave the stake and finality trackers bounded per-block undo.
The other trackers already undid a block in place. Two things are left:

- **Durable.** Nothing survives a restart. Every start rebuilds each tracker
  by reading and re-verifying every block from its activation height.
- **Bounded.** Four trackers keep one history entry per block since
  activation and never trim it.

| Tracker | Replays from (mainnet) | Re-verifies per block | Undo today | History kept |
|---|---|---|---|---|
| Finality, with its private stake registry and binding index | H + 1 = 810,001 | certificates, the H1 schedule, each binding's BIP340 signature and PoP | last 1,440 blocks (B0a) | bounded |
| Chainstate binding index, read by the mempool, `ConnectBlock`, the miner and status | 810,001 | each binding's BIP340 signature and PoP | exact | one entry per block that carries a binding |
| Bridge | 811,001 | Ethereum sync-committee BLS, SSZ branches, receipt and state proofs | last 288 blocks (`bridge_state.h:403`) | a withdrawal root per block; anchors, nullifiers and withdrawals per event |
| FN seats | A2 = 813,000 | the PoP of each seat output | exact | one delta per block (`fn_seat_index.cpp:222`) |
| FlowMesh vaults | A2 | nothing cryptographic | exact | one delta per block (`flowmesh_vault_index.cpp:301`) |
| FlowMesh checkpoints | A3 = 815,000 | per checkpoint, the PoP of every seat in its set (up to 5,000) and the BLS certificate | exact | one delta per block (`flowmesh_checkpoint_index.cpp:1229`) |

The chainstate's own `StakeTracker` is left out: production never syncs it
(fast-track doc §4.6).

**Cost today.** The B0 measurement of 2026-10-25
(`~/B3-evidence/B0/report.md` on the build host) puts each replay at 15 to
19 µs per block. On mainnet's 46,533 modern blocks of that day, a restart
paid about 3.6 s, and it grows by about 42 s per year of blocks:

| Tracker | Replay |
|---|---:|
| finality | 0.88 s |
| chainstate bindings | 0.76 s |
| vaults | 0.68 s |
| seats | 0.65 s |
| checkpoints | 0.60 s |

These are floors:

- the fixture's blocks were nearly empty, with no FN seats or checkpoints
  to verify;
- it had 4 validators, while mainnet's epoch-10 set has 47;
- it had no bridge.

The checkpoint replay is the one most likely to be far above its floor:
every checkpoint record re-verifies the PoP of every seat in its set
(`src/flowmesh/bls_certificate.cpp:160`), and an epoch handoff does it for a
second set (`flowmesh_checkpoint_index.cpp:1006`). Step 0 (§9) replaces the
floors with a real node's numbers before the later steps are ordered.

## 2. Goals and non-goals

Goals:

1. A start reads only the blocks after each tracker's last persisted
   record.
2. A loaded tracker equals a rebuild. Any doubt means a rebuild: the
   records are a cache, and the rebuild stays the reference.
3. A reorganization right after a start is undone in place, as B0a does
   while running.
4. A crash leaves the previous record or the new one, never a mix.
5. No record survives a change of build or of consensus parameters.
6. Later, bounded memory for the trackers whose history grows per block
   (§7).

Non-goals:

- **Pruning.** It stays refused (`src/init.cpp:1094-1098`,
  `src/node/blockstorage.cpp:788-801`). B0b removes the reason those
  comments give only once every tracker is durable.
- **Snapshot chainstates (assumeutxo).** They never write or load records.
- **The fresh lineage and fresh keys.** They are regtest knobs today. §9
  step 5 adds them before C uses them anywhere else.
- **Derivation.** Nothing changes what any tracker derives.

## 3. Shape

**A. A deep snapshot, then replay.** This is the outline in the fast-track
doc (§4.3): persist the state one horizon below the tip and replay forward
on start.

- Every start then replays at least a horizon, 1,440 blocks on mainnet.
- The writer needs each tracker rolled back a horizon.
- Its one advantage, refilling the undo history, disappears once the undo
  history itself is persisted.

Rejected.

**B. One record per tracker at the tip, with its undo history.**
Recommended for the finality group and the binding index.

- At a write, each synced tracker stores its whole state as one record
  naming the block it is synced to. The state includes the bounded undo
  history of B0a.
- A start loads the record when its block is on the active chain, then
  replays only the blocks after it.

**C. Per-block history keys.** For the bridge, seats, vaults and
checkpoints (§7).

- Their history reaches back to activation, and consensus queries read it.
- A whole-state record would therefore grow with the chain, and be
  rewritten at every write.
- They need one key per block, written incrementally. This is the shape of
  Core's `coinstatsindex`.

**Where: a dedicated LevelDB, `derivedstate/`, next to `chainstate/`.** The
bridge code already sets the requirement
(`src/node/bridge_state.cpp:3928-3931`):

> production may replace this full replay with a dedicated LevelDB sidecar
> only when config digest, tip marker, state and per-block undo are
> committed in one synchronous batch. Never load a partial or mismatched
> cache.

A record carries all four, and every write is one synchronous batch. The
alternatives are worse:

- **The coins database's final batch**, next to `DB_BEST_BLOCK`, as the B3
  validation marker does (`src/txdb.cpp:221`). It changes Core's
  `CCoinsViewDB::BatchWrite`, and C's per-block keys would sit among the
  coins.
- **A flat file** like `finality_pin.dat`. It works for B and not for C.

Atomicity with the coins tip is not needed. The derived state at a block is
a pure function of the chain up to that block. So a record that names its
block is valid for as long as that block is on the active chain, whatever
the coins database holds.

## 4. The record

**Key:** `('r', tracker id)`.

- 1: the finality group.
- 2: the chainstate binding index.
- 3 to 6: reserved for the bridge, seats, vaults and checkpoints.

**Value:**

- `u8` format version;
- the build string, `FormatFullVersion()` (§4.1);
- `uint256` parameters digest (§4.2);
- `uint256` hash and `i32` height of the block the state is synced to;
- the payload (§4.3);
- SHA256 of everything before it.

### 4.1 Bound to the build

`FormatFullVersion()` is `v<version>` for a release and
`v<version>-<commit>` for a development build
(`src/clientversion.cpp:40-62`). A record written by another build is never
loaded.

- **The cost:** the first start after an upgrade rebuilds once, as every
  start does today.
- **The gain:** a derivation change can never meet a stale record. No
  "rules version" has to be bumped by hand in every commit that touches a
  tracker.

One gap: uncommitted local edits keep their commit's string. To cover it,
`-checkderivedstate` (§6) defaults on in regtest, as `-checkblockindex` and
`-checkmempool` do (`src/init.cpp:711`, `:713`).

### 4.2 Bound to the parameters

Within one build, regtest and testnet parameters can change between starts,
for example `-b3h1height` or an assumed height C. The digest is a SHA256
over every consensus parameter a tracker reads:

- the genesis hash and the message start;
- `hard_fork_height`, `legacy_final_hash` and the corridor;
- all of `ModernPosParams`, `reorg_horizon` included;
- `h1`, `fresh_lineage` and the fresh-key height;
- the A1, A2 and A3 heights;
- `busd_bridge`, the bridge withdrawal activation and their pins.

No such digest exists in the tree yet. One test per field changes that field
alone and requires a different digest. A field added to the parameters later
still has to be added by hand; the per-field tests keep the list
reviewable.

### 4.3 Payloads

**The finality group (id 1):**

- **Set table.** Every distinct `ValidatorSetSnapshot` that the state, the
  undo history or the producer schedule refers to, stored once each.
  - Fields: its epoch, then per member `validator_key`, `bls_pubkey`,
    weight and `binding_seq`. `binding_seq` is not consensus data, but
    wallets resolve keys with it (`validator_set.h`).
  - On load each set is rebuilt from its members, with keys that passed §5.
    The recomputed set hash must equal the stored one.
  - The class has no public constructor, so this needs a new factory beside
    `Build`.
- **State.** The scalars, the four set references, `epoch_starts`,
  `finalized` and the producer schedule (`finality_tracker.h:36-65`,
  `:175-208`). The fresh lineage must be absent.
- **Undo history.** At most a horizon of records, each a block hash and the
  prior `State`.
  - Each prior `epoch_starts` is stored as a length: a prior state belongs
    to an ancestor, so its list is a prefix of the next one.
  - The writer checks the prefix property, and writes no record if it
    fails.
- **Private stake registry.** The entries in outpoint order, then the undo
  records: a block hash, then the operations in apply order.
- **Private binding index.**
  - The records by validator key.
  - The whole undo trail, which gains an entry only for a block that
    carries a binding (`finality_binding_index.cpp:201`).
  - Every key's PoP (§5).
  - The owner-of-key map is not stored. It is derived on load and must come
    out one to one.
- **Not stored.** The tip-snapshot memo, the rebuild counter and the
  fresh-key tracker.

When the fresh lineage or fresh keys are configured, the finality group
writes no record (§9, step 5).

**The chainstate binding index (id 2):** the same binding-index payload.

## 5. Where a loaded BLS key comes from

`src/crypto/bls.h:126-140` admits a key into an aggregate by two paths
only:

> - FromPoP(pk, pop): verifies the PoP; the ONLY path for any key that
>   arrives from a transaction, a block being validated, a P2P message, an
>   RPC/wallet input, or any other external source.
> - TrustedFromValidatedChain(pk) — INVARIANT (owner, 2026-08-23):
>   permitted ONLY when reconstructing consensus state from blocks that
>   already passed full validation [...]

Today every aggregate key has passed a PoP check in the running process:

- the binding replay re-verifies each PoP (`modern/finality_key.h:156-157`);
- the seat replay "rechecks the type-7 PoP rather than trusting carrier
  bytes from disk" (`src/validation.cpp:4009-4011`).

A record is not network input. Nor is it "blocks that already passed full
validation". There are two ways to handle its keys:

- **P1, recommended: re-verify on load.** Persist each key's PoP and run
  `FromPoP` on every distinct key at load. Needs no ruling.
  - The binding transition already carries a 96-byte PoP
    (`finality_key.h:28`). The index keeps it per key instead of dropping
    it.
  - FN seats already work this way: they keep their PoP and re-verify it at
    use (`bls_certificate.cpp:155-160`).
  - Every key a retained set refers to is in the binding index, either
    current or in the undo trail, which is never trimmed.
  - Cost at load: one PoP check per distinct key, 47 on mainnet's epoch-10
    set. It is bounded by the size of the sets, not by the chain.
- **P2: an owner ruling.** A record this node wrote counts as
  validated-chain provenance, so `TrustedFromValidatedChain` may wrap its
  keys. Simpler and cheaper, but it widens an owner invariant.

## 6. Start, write, crash

**Load.** This happens right after `LoadChainTip`, before `VerifyDB` and
the first `FindMostWorkChain`, so that no lazy `Sync` rebuilds first. A
record is used only when all of these hold:

1. the format, the build, the parameters digest and the checksum match;
2. its block is in the block index and on the active chain, at or below
   the tip;
3. the payload decodes completely, with no bytes left over;
4. its invariants hold:
   - every key passes `FromPoP`;
   - every recomputed set hash equals the stored one;
   - the owner-of-key map is one to one;
   - the undo records name exactly the active chain's blocks ending at the
     record's block;
   - no undo history is longer than the horizon.

The loader creates the tracker and installs it as synced to that block. The
existing forward path of `Sync` then replays only the blocks after it
(`finality_tracker.cpp:566-568`, `finality_binding_index.cpp:213-216`).

If any check fails, the loader logs one line naming the tracker and the
reason, and that tracker rebuilds as it does today. The finality pin keeps
its own fail-closed file; records fail open to the slow path.

**Order.** `RefreshFinalityAnchor` already syncs the bridge before the
finality tracker (`validation.cpp:2728`). That stays, so a loaded finality
group extends against a bridge that has caught up, whether that bridge was
loaded or rebuilt.

**Write.** The write happens in `FlushStateToDisk`, after the coins are
written, and only for a fully validated chainstate. A tracker writes its
record when it is synced to the flushed block and any of these holds:

- it has no record;
- its record's block is not an ancestor of the flushed block;
- its record is at least K = 144 blocks old;
- the flush is the shutdown flush (`init.cpp:390`, `:417`).

All records of one flush go into one synchronous batch. A tracker that is
not synced keeps its old record, which stays valid while its block is on
the active chain.

**Why K.** Flushes can be frequent:

- `RaiseFinalityAnchorDurably` forces a flush each time the pin rises
  (`validation.cpp:2755-2766`). With finality working, that can be every
  checkpoint, about every 10 blocks.
- Periodic flushes come every 50 to 70 minutes (`validation.cpp:129-130`).

K keeps records from being rewritten at every one of them. An unclean stop
therefore replays at most K blocks, plus the blocks since the last flush.

**Crash during a write.** The batch lands whole or not at all. The coins
database may name a different block than the records; nothing requires them
to agree (§3). The sidecar gets the same `-dbcrashratio` hook as the coins
database.

**`-reindex` and `-reindex-chainstate`** wipe `derivedstate/`, so a reindex
always re-derives.

**Switches:**

- `-persistderivedstate=0` turns off both load and write. Today's
  behaviour stays one flag away.
- `-checkderivedstate` rebuilds every loaded tracker and requires equality.
  - On a mismatch it logs an error, keeps the rebuilt state and erases the
    record.
  - Default on in regtest (§4.1) and off elsewhere. It is for CI and for the
    owner's shadow-chain runs.

## 7. The trackers with history back to activation

The bridge, seat, vault and checkpoint indexes answer queries about any
past height, and consensus reads some of these answers:

- **`BridgeStateIndex::WithdrawalRootAtHeight`** (`bridge_state.cpp:2727`)
  reaches back to activation. The finality tracker reads it
  (`finality_tracker.cpp:335`, `:413`), and so do finality signatures
  (`finality_signature.cpp:74`).
- **`FnSeatIndex::AnchoredSnapshot`** (`fn_seat_index.cpp:327`) takes an
  anchor at least 30 blocks deep (`FLOWMESH_ANCHOR_DEPTH`), with no maximum
  depth. Checkpoint validation reads it.
- **`FlowMeshVaultIndex::MarketAt`** and the other `...At` queries
  (`flowmesh_vault_index.cpp:44`, `:347`, `:376`, `:470`, `:484`) reach
  back to A2.
- **`FlowMeshCheckpointIndex::WithdrawalSettlementsBetween`**
  (`flowmesh_checkpoint_index.cpp:405`) accepts any interval inside the
  retained history.

Cutting these histories in memory would change which heights the queries
can answer, which is a consensus question, unless the older entries move to
disk. Hence two phases.

**2a, durable.**

- Each block's delta, the struct the in-memory history already holds, is
  stored under `('h', tracker id, height)`.
- Each write stores the heights since the previous write, and erases the
  heights above a reorganization's fork.
- On load the deltas are read back into memory. The in-memory model is
  unchanged.
- A start then reads small records instead of blocks, and verifies no
  proofs.

**2b, bounded.**

- Memory keeps the last horizon of deltas, and older queries read from
  disk.
- This changes how consensus-reachable queries read their data, not what
  they answer.
- It is the larger change, and is reviewed separately.

Both need serializers for types that have none today, among them
`BridgeBlockDelta`, `FnSeatBlockDelta` and the light-client store. Only some
leaf types, such as `FlowMeshCheckpointCoreV1`, can encode and decode.

The bridge needs more:

- the independent review that CLAUDE.md rule 5 requires for bridge
  persistence;
- a stable code base: the owner's bridge work is still changing it;
- an answer to the §5 question for its light-client store. The store holds
  Ethereum sync-committee keys, which the light-client protocol checks; no
  PoP does. §5 does not cover them.

The V1 FlowMesh code is reference only for V2
(`src/flowmesh2/ARCHITECTURE.md:77-78`). It still runs on mainnet above A3,
and the plan ships B0 to the V1 track
(`b3-flowmesh-greenfield-plan-20261016.md:956`, `:969`).

## 8. Size

The finality group on mainnet today stays well under a megabyte:

- each set has 47 members, at 92 bytes each before PoPs, and the window
  holds a handful of distinct sets;
- each stake entry is about 80 bytes;
- the undo history holds up to 1,440 prior states, each only scalars and
  set references.

At the frozen header's limit of 8,192 members, a distinct set is about
0.75 MB, plus 96 bytes per key for its PoP. The worst case is a few
megabytes, written at most every K blocks and at shutdown.

Phase 1 does not change memory.

## 9. Plan

0. **Measure.** Log the tracker, its block count and its time at each
   rebuild. It is about ten lines, with no consensus effect. Mohamed reads
   the log of his own node after a restart, which replaces the floors of §1
   with real numbers.
1. **Framework, finality group and chainstate binding index.** This covers
   the sidecar, the record, the build and parameter binding, P1's PoP
   retention, load and write, both switches, and tracker ids 1 and 2.
2. **Seats, vaults and checkpoints, phase 2a**, if step 0 shows that their
   restart cost matters. A cheaper first fix may be enough for checkpoints.
   Their replay re-verifies every seat's PoP for every checkpoint, while a
   cache of verified seat sets exists that only the service uses
   (`flowmesh_service.cpp:378`).
3. **The bridge, phase 2a**, after the independent review and the owner's
   bridge work.
4. **Phase 2b**, bounded memory.
5. **The fresh lineage and fresh keys**, before C turns them on outside
   regtest.

Each step lands with failing-first tests and its own evidence.

## 10. Tests, failing first (steps 0 and 1)

**Round trip.** Each payload decodes to an equal tracker, undo history
included. This is covered by unit cases, and by a fuzz target that drives
random block sequences into a tracker, then encodes, decodes and compares.

**Hostile bytes.** A fuzz target feeds arbitrary records to the loader. It
must never crash, and never install a record that fails a check.

**Restart.** On `FinalityChainDiskFixture`
(`src/test/util/finality_fixture.h:425`), build a chain that contains:

- a certificate;
- a rotation;
- H1's activation and an escape;
- bindings;
- stake changes.

Then flush, and restart the chainstate. Three checks:

- the trackers load without a rebuild: `Rebuilds()` stays 0;
- each equals a tracker rebuilt from scratch;
- a one-block reorganization right after the restart is undone in place.

**Rejection.** Each of these records is refused, and a correct rebuild
follows:

- one from another build;
- one written before a parameter changed (`-b3h1height`);
- one with a flipped byte;
- one with a truncated value;
- one whose block is off the active chain;
- one holding a key whose PoP fails.

**Crashes.** A regtest functional test after `feature_dbcrash.py` runs with
`-dbcrashratio` over a modern chain with certificates. After every crash and
restart:

- `-checkderivedstate` passes;
- the finality status equals that of the same node started with
  `-persistderivedstate=0`.

**Mutants.** Each must fail at least one case above:

- the writer drops a field (`escapes`, `binding_seq`);
- the loader skips the parameters digest, or the build check;
- the loader skips the PoP check;
- the loader accepts a block off the active chain;
- the writer writes a tracker that is not synced to the flushed block;
- an undo record keeps a cut-short `epoch_starts`.

## 11. Questions for the owner

1. **Keys on load (§5).** P1, every PoP re-verified at load (recommended)?
   Or P2, a ruling that the node's own record is validated-chain
   provenance?
2. **Where (§3).** A dedicated LevelDB, as the bridge comment asks
   (recommended)? Or the coins database's batch, or a flat file?
3. **Trust.** A cache that falls back to a rebuild on any doubt
   (recommended)? Or fail closed, like the pin?
4. **Upgrades (§4.1).** Records bound to the exact build, so one rebuild
   per upgrade (recommended)? Or a rules version that survives upgrades?
5. **Scope and order (§9).** Should steps 2 and 3 wait for step 0's
   numbers? Who reviews bridge persistence, and when?
6. **Default.** On for every network, once the tests and a shadow-chain run
   with `-checkderivedstate` pass?

## 12. Found on the way

- **The bridge's undo is shorter than the horizon.** It keeps 288 blocks
  (`bridge_state.h:403`) against a horizon of 1,440. A reorganization of
  289 to 1,440 blocks still rebuilds the bridge from 811,001. B0b does not
  change that.
- **A doc comment is wrong.** `FlowMeshVaultTracker` is documented as an
  "A3 replay/undo driver" (`flowmesh_vault_index.h:167`), but it replays
  from A2 (`flowmesh_vault_index.cpp:580`).
- **A memo has no eviction.** `FnSeatIndex::m_snapshots`
  (`fn_seat_index.h:144`) grows with every distinct anchor queried.
- **The chainstate `StakeTracker` is still unsynced in production**
  (fast-track doc §4.6).
