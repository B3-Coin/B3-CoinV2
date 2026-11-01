# B0b: durable derived state

**Status: DESIGN for owner review; step 1 built, off by default.**
Written 2026-10-31 against `r0-rebirth` at 070cc37b, the B0a commit. It
changes no block validity, height, rule or parameter. B0 was authorized on
2026-10-29 (`doc/design/b3-height-c-fast-track.md` §7, ask 1). B0b adds
files that a node trusts at startup, so their format and trust rules are up
for review.

Step 1a (§9), the framework and the chainstate binding index, landed on
2026-11-01 in 4f3219ea, b5cae181 and 9be63a38. It follows the
recommended answers to §11's questions 2 to 4, and stays behind
`-persistderivedstate` until question 6 is answered. Step 1b, the
finality group, landed the same day in 50ce7565, 71e351d5, 2818a2a0,
ec5d5eef and f8f45427. It follows P1 for question 1, chosen on
2026-11-01; P1 needs no ruling (§5). A review of step 1 the same day
added four fixes with their tests (77070016, 0647a33e, cfc47691 and
54c6d329), a test of the mainnet shape (57f46241), a faster test setup
(7a7c816f) and the tests under mainnet's bridge rules (§10). §13 lists
where both steps and the review differ from the first draft of this
design. Line references are to 9be63a38 unless they name another commit.

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
5. No record survives a change of build, or of a consensus parameter its
   tracker reads.
6. Later, bounded memory for the trackers whose history grows per block
   (§7).

Non-goals:

- **Pruning.** It stays refused (`src/init.cpp:1096-1100`,
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
- the build id, `SourceBuildId()` (§4.1);
- `uint256` parameters digest (§4.2);
- `uint256` hash and `i32` height of the block the state is synced to;
- the payload (§4.3);
- SHA256d of everything before it.

### 4.1 Bound to the build

`SourceBuildId()` (`src/clientversion.cpp`) names the exact sources: the
tag of a clean tagged checkout, otherwise `v<version>-<commit>`, or
`v<version>-g<hash>` for a `git archive`. A build with uncommitted changes
to tracked files, or with no git information, has no id and persists
nothing. A record written by another build is never loaded.

- **The cost:** the first start after an upgrade rebuilds once, as every
  start does today.
- **The gain:** a derivation change can never meet a stale record. No
  "rules version" has to be bumped by hand in every commit that touches a
  tracker.

The first draft used `FormatFullVersion()`. It gives one string to:

- every untagged build of a release version;
- every build of a commit with local edits, whatever the edits;
- every build without git information.

`SourceBuildId()` tells all of these apart, or gives no id.
`-checkderivedstate` (§6) still defaults on in regtest, as
`-checkblockindex` and `-checkmempool` do (`src/init.cpp:711`, `:715`).

### 4.2 Bound to the parameters

Within one build, regtest and testnet parameters can change between starts,
for example `-b3h1height` or an assumed height C. Each tracker's digest is a
SHA256 over exactly the consensus parameters its derivation reads, each
optional one with its presence. Within one build that set is fixed, so a
parameter outside it cannot change the tracker's state; across builds §4.1
refuses the record anyway.

- **Id 2** reads four: `legacy_b3coin`, `hard_fork_height`, the genesis
  hash and `legacy_final_hash`
  (`FinalityBindingTracker::ParamsDigest`). Its tests change each field
  alone and require a different digest, tell an unset field from a zero
  one, and change two unread fields and require the same digest.
- **Id 1**, the finality group, reads many more
  (`FinalityTracker::ParamsDigest`):
  - id 2's four, through id 2's digest, and the corridor
    (`transition_pow_length`);
  - all of `ModernPosParams`, `reorg_horizon` included;
  - both fields of `h1`;
  - whether `fresh_lineage` and `bridge_v2` are set: under either,
    nothing is written (§4.3);
  - the A1, A2 and A3 heights and the bridge withdrawal activation;
  - every field of `busd_bridge`, its pins included.

  The heights and the bridge fields are in whether the derivation reads
  them today or not: a field too many costs one rebuild after a change,
  a field missing would load a stale state. Its test changes each of 76
  fields alone, tells unset from zero for 16 optional ones, and changes
  two unread fields and requires the same digest.

A derivation that starts to read another parameter must add it to its
digest. The digest sits next to the derivation, and its comment names the
functions it must stay in step with.

### 4.3 Payloads

**The finality group (id 1):**

- **Set table.** Every distinct `ValidatorSetSnapshot` that the state, the
  undo history or the producer schedule refers to, stored once each.
  - Fields: its epoch, then per member `validator_key`, `bls_pubkey`,
    weight and `binding_seq`. `binding_seq` is not consensus data, but
    wallets resolve keys with it (`validator_set.h`).
  - On load each set is rebuilt from its members, with keys that passed §5.
    The recomputed set hash must equal the stored one.
  - `ValidatorSetSnapshot::FromMembers` (71e351d5), a factory beside
    `Build`, does this.
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
    carries a binding (`finality_binding_index.cpp:310`).
  - Every key's PoP (§5).
  - The owner-of-key map is not stored. It is derived on load and must come
    out one to one.
- **Not stored.** The tip-snapshot memo, the rebuild counter and the
  fresh-key tracker.
- **As built** (`FinalityTracker::Encode`), the order is: the private
  binding index, every key's PoP, the private stake registry, the set
  table, the state, then the undo history.
  - Each set is stored with its epoch and set hash. The states name sets
    by their place in the table.
  - The PoPs are a table of their own, by key. The binding index keeps
    each binding's PoP since 50ce7565.

When the fresh lineage (fresh keys included) or bridge V2 is configured,
both regtest-only today, the finality group writes no record (§9,
step 5).

**The chainstate binding index (id 2):** the records by validator key,
then the undo trail in apply order (`FinalityBindingIndex::Encode`).

- No PoPs. Its readers check keys against it: mempool policy, the binding
  checks of `ConnectBlock` and the status call. None admits a key into an
  aggregate, so §5 does not apply.
- The owner-of-key map is not stored. The decoder derives it, and refuses
  a key held twice or an undo trail that does not lead to the records.
- `ConnectBlock` reads this index for consensus. A loaded index that was
  wrong would make the node reject a valid block or accept an invalid
  one, as a wrong coins database would. That is why a record is used only
  after the checks of §6, and why question 3 of §11 matters.

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
  bytes from disk" (`src/validation.cpp:4114-4116`).

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
  - Cost at load: one PoP check per key the binding index holds, current
    or in its trail (`HeldKeys`). The trail is never trimmed, so that is
    every unrevoked key bound since activation, not only the keys of the
    sets: at least the 47 of mainnet's epoch-10 set, and one more for each
    later binding to a new key. It grows with the bindings on the chain,
    not with its blocks.
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
   - every recomputed set hash equals the stored one, and every set is
     one Build could make: no weight above `MAX_MEMBER_WEIGHT`, which
     keeps the total from wrapping, and no BLS key twice;
   - the owner-of-key map is one to one;
   - the undo records name exactly the active chain's blocks ending at the
     record's block;
   - no undo history is longer than the horizon;
   - each state, the tip's and each prior one, keeps what the rules keep
     at its block: empty below M, one epoch start per epoch from M, each
     at least E above the one before, the sets of the epochs around it, a
     finalized checkpoint on this chain, and the H1 producer schedule
     exactly where H1 is active;
   - a prior state has exactly the tip state's epoch starts that are at
     or below its block's parent. The decoder checks both start rules on
     the tip's list before any prior state copies from it, so a record
     cannot make the loader copy a long list once per undo record, and it
     counts the prior states' starts in one pass;
5. for id 1, the finalized checkpoint is at or below the finality pin,
   and a pin exists if a checkpoint does. A record never raises the pin,
   which no restart lowers. A record this node wrote passes, except one
   written by the flush that `RefreshFinalityAnchor` makes before it
   raises the pin, when the node stops in between; that one costs a
   rebuild, which pins the checkpoint again.

For id 2, item 4 is the decoder's own checks (§4.3), and its undo trail
names heights from H+1 to the record's block, rising.

The loader creates the tracker and installs it as synced to that block.
A tracker still behind the tip when the first block connects would
rebuild: `BlockConnected` follows only a tracker in step with the parent
block. So each tracker reaches the tip through the forward path of
`Sync` (`finality_tracker.cpp:566-568`,
`finality_binding_index.cpp:343-345`) before any block connects:

- **id 2** in the loader, at once;
- **id 1** in the first `FindMostWorkChain`, whose
  `RefreshFinalityAnchor` syncs the bridge and then the finality group.
  `ConnectBlock` also syncs it to the parent block. Leaving this to them
  keeps the bridge in step first (see Order).

If any check fails, the loader logs one line naming the tracker and the
reason, and that tracker rebuilds as it does today. The finality pin keeps
its own fail-closed file; records fail open to the slow path, and never
raise the pin (item 5).

**What a record does not save.** A level-4 `VerifyDB` reconnects blocks
below the tip, and reconnecting a block that carries a binding rebuilds the
index. That happens with `-checklevel=4` (the default is 3), and in the
one-time post-H validation-schema pass (`src/node/chainstate.cpp:380-415`).
That pass runs only on a coins database without a current marker, which is
once per database for a node that keeps running marker-writing releases.

**Order.** `RefreshFinalityAnchor` already syncs the bridge before the
finality tracker (`validation.cpp:2729`). That stays, so a loaded finality
group extends against a bridge that has caught up, whether that bridge was
loaded or rebuilt.

**Write.** The write happens in `FlushStateToDisk`, after the coins are
written, and only for a fully validated chainstate whose coins are at the
chain's tip, the flushed block. `ConnectTip` and `DisconnectTip` flush
while the two differ (the coins already at the new block, or already at
the parent of the block being disconnected), so their flushes write no
record. A tracker writes its record when it is synced to the flushed
block and any of these holds:

- it has no record;
- its record's block is not an ancestor of the flushed block;
- its record is at least K = 144 blocks old;
- the flush is forced: `FORCE_FLUSH` or `FORCE_SYNC`. That is the shutdown
  flush (`init.cpp:390`, `:417`) and a few RPC and cache-resize flushes.
  The pin's `FORCE_FINALITY` flushes are not forced here; they follow K.

All records of one flush, ids 1 and 2 today, go into one synchronous
batch. A tracker that is not synced keeps its old record, which stays
valid while its block is on the active chain. A flush never syncs a
tracker to have something to write.

**Why K.** Flushes can be frequent:

- `RaiseFinalityAnchorDurably` forces a flush each time the pin rises
  (`validation.cpp:2756-2767`). With finality working, that can be every
  checkpoint, about every 10 blocks.
- Periodic flushes come every 50 to 70 minutes (`validation.cpp:130-131`).

K keeps records from being rewritten at every one of them. An unclean stop
therefore replays at most K blocks, plus the blocks since the last flush.

**Crash during a write.** The batch lands whole or not at all. The coins
database may name a different block than the records; nothing requires them
to agree (§3). The records are written synchronously and the coins are
not, so after a power loss the records can name a block above the coins
on disk. That block is not on the active chain at the next start, so
the records are passed over (item 2). The sidecar is to get the same
`-dbcrashratio` hook as the coins database; step 1a does not have it
yet.

**`-reindex` and `-reindex-chainstate`** wipe `derivedstate/`, so a reindex
always re-derives.

**Switches:**

- `-persistderivedstate=0` turns off both load and write. Today's
  behaviour stays one flag away. Step 1a makes 0 the default until
  question 6 is answered.
- `-checkderivedstate` rebuilds every loaded tracker and requires equality.
  - On a mismatch it logs an error and erases the record. Step 1a then
    leaves the tracker to rebuild when first needed, rather than keeping
    the check's rebuild: a mismatch is a bug, not a path to optimize.
  - Id 1 is rebuilt at its record's block, the bridge synced first, as
    `RefreshFinalityAnchor` orders them. A bridge that cannot sync, or a
    rebuild that fails, leaves the record unused, never erased. On
    mainnet the order matters: its bridge rules, in verifier mode from
    its modern start, make finality certificates read the bridge's
    withdrawal roots (`FinalityWithdrawalRoot`). §10 tests this sync and
    the forward sync's under those rules.
  - Default on in regtest (§4.1) and off elsewhere. It is for CI and for the
    owner's shadow-chain runs.
  - It is an audit, not a setting to pair with question 6. A start with it
    pays a full rebuild, more than a start without persistence.
- Without the check, a record that passes its own checks is trusted, as
  the coins database is (§4.3). Its checksum is SHA256d, not keyed: it
  catches a damaged record, not a forged one, and the datadir is trusted
  as it is for the coins. For id 1 the trust covers the producer
  schedule, which decides which blocks are valid, and the finalized
  checkpoint, which decides which certificates may follow. A checkpoint
  above the pin is refused (item 5), so a record cannot raise the pin; one
  at or below it is trusted. Question 3 is about this trust.

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
   rebuild. It is about ten lines, with no consensus effect. The log of a
   restart of Mohamed's own node replaces the floors of §1 with real
   numbers. Not done yet.
1. **Framework, finality group and chainstate binding index**, in two
   parts:
   - **1a, done on 2026-11-01:** the sidecar, the record, the build and
     parameter binding, load and write, both switches, and tracker id 2.
     Off by default.
   - **1b, done on 2026-11-01:** the finality group, tracker id 1, with
     P1's PoP retention. The binding index keeps each PoP, a validator
     set is restored from its members, the stake registry and the
     finality group encode and decode, and the group is loaded and
     written beside id 2. Off by default.
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

**Step 1a has** unit cases for id 2 in `finality_key_binding_tests`, each
failed first by mutants:

- **Round trip and hostile bytes**, covered deterministically rather than
  by fuzzing:
  - every truncation and every flipped bit of a record;
  - malformed index encodings.

  There is no fuzz target yet: the build host does not build the fuzz
  binary, and a new target needs CMake.
- **Restart**, on a disk-backed `BindingFixture`. It covers the three
  checks above for id 2, and an older record that is caught up. It also
  covers the rejections that apply to id 2:
  - another build, or other parameters;
  - a flipped byte;
  - a block off the active chain, or a wrong height;
  - a malformed payload;
  - a wrong state, under `-checkderivedstate`.
- **What is persisted**: the write rule, and nothing without the switch,
  without a build id, or from a tracker that is not in step.

**Step 1b has** these cases for id 1, each failed first by mutants.
Step 1a's and 1b's mutants include the six that the list of mutants
above asks for.

- **Parts.** `from_members_restores_built_set` in `validator_set_tests`,
  and `b0b_stake_registry_codec` in `finality_h1_tests`.
- **Parameters** (`b0b_finality_group_params_digest`), as §4.2 says.
- **Round trip and hostile bytes** (`b0b_finality_group_codec`), on a
  chain with a certificate, a rotation, H1 with an escape, a binding, a
  rebinding and stake changes:
  - the encoding decodes to a tracker equal to the live one and to a
    rebuild, and encodes to the same bytes;
  - decoded four blocks back, it catches up forward without a rebuild;
    it undoes in place through the escape; after a two-block
    reorganization its history is a suffix of a rebuild's;
  - every truncation and a byte more are refused, and so is each broken
    invariant of §6, two PoPs swapped and a set hash its members do not
    give among them.
- **Restart**, on a disk-backed fixture (`b0b_finality_group_restart`,
  `..._older_record_and_write_rule`, `..._unusable_records_are_rebuilt`):
  - the three checks above, for id 1;
  - one shutdown writing both records at the tip, in one batch;
  - an older record brought to the tip without a rebuild, and the write
    rule;
  - the rejections that apply to id 1, H1's escape rounds changed and
    two PoPs swapped among them;
  - under `-checkderivedstate`, a wrong state that passes the record's
    own checks is refused; without it, that state is installed. The
    test pins that trust down.

**The step 1 review added** these cases, each failed first by mutants:

- `b0b_finality_group_record_never_raises_the_pin`: a record whose
  checkpoint is above the pin (the block below the record's), valid in
  every other respect, is passed over, and so is a record with a
  checkpoint when the pin file is gone; a record at the pin is
  installed.
- `b0b_finality_group_epoch_starts_follow_the_chain` and
  `b0b_finality_group_decode_bounds_the_epoch_starts`: a rotation one
  block early and a stale prior state are refused, and so is a record
  whose tip state lists four million epoch starts that each of its undo
  records claims, without copying them (peak memory, against the decoder
  before the fix).
- `from_members_restores_built_set` in `validator_set_tests` also
  refuses a set that holds a key twice, a weight above the bound, and
  weights whose total wraps.
- `b0b_derived_state_written_at_coins_flushes`: the write rule at each
  flush of a running node, and no record while the coins are below the
  tip.
- `b0b_finality_group_mainnet_shape`: a record with no H1, V1 past two
  rotations and a broken lineage, under the bridge rules in verifier
  mode: the shape of mainnet today.
- `b0b_finality_group_under_verifier_bridge`: under those bridge rules,
  no tracker checks a certificate without the bridge index, and a zero
  root is refused. The check rebuild syncs the bridge first. An older
  record is brought over a certificate to the tip by the first
  `FindMostWorkChain`, the bridge first, without a rebuild; the loader
  does not sync it, as nothing has synced the bridge by then.

The crash functional test waits for the `-dbcrashratio` hook and a daemon
build.

## 11. Questions for the owner

1. **Keys on load (§5).** P1, every PoP re-verified at load (recommended)?
   Or P2, a ruling that the node's own record is validated-chain
   provenance? Step 1b builds P1, which needs no ruling. P2 later would
   swap the load call and drop the stored PoPs; records are bound to the
   build, so nothing would need migrating.
2. **Where (§3).** A dedicated LevelDB, as the bridge comment asks
   (recommended)? Or the coins database's batch, or a flat file?
3. **Trust.** A cache that falls back to a rebuild on any doubt
   (recommended)? Or fail closed, like the pin? Since the review a record
   cannot raise the pin (§6, item 5). Three middle grounds are open,
   cheapest first: read the block that carries the finalized checkpoint's
   certificate and check the certificate there; run the check rebuild on
   the first record each build loads; or check every loaded record by a
   rebuild in the background after the start, and stop on a mismatch.
4. **Upgrades (§4.1).** Records bound to the exact build, so one rebuild
   per upgrade (recommended)? Or a rules version that survives upgrades?
5. **Scope and order (§9).** Should steps 2 and 3 wait for step 0's
   numbers? Who reviews bridge persistence, and when?
6. **Default.** On for every network, once the tests and a shadow-chain run
   with `-checkderivedstate` pass?

Step 1a follows the recommended answers to questions 2, 3 and 4, and keeps
the default off for question 6. Step 1b adds P1 for question 1. Adopting
another answer would cost about this much:

- **2, another store:** the store class and its tests, about 100 lines;
- **3, fail closed:** each refusal at load becomes a startup error;
- **4, a rules version:** a hand-kept number in place of the build id,
  bumped by every change to a derivation or its encoding;
- **6:** one default.

Question 1 does not arise for id 2 (§4.3).

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
- **The legacy prefix cost every test process minutes.** The first
  legacy-prefix case of a process searched the scrypt nonces of the
  32-block prefix, about four minutes of CPU. The test cache now starts
  from a checked table of them (7a7c816f).

## 13. Changes from the first draft (e0560888)

Step 1a changed these points:

- §4.1: the build id is `SourceBuildId()`, not `FormatFullVersion()`. A
  build with local edits persists nothing.
- §4: the checksum is SHA256d.
- §4.2: one digest per tracker, over exactly the parameters it reads.
- §4.3: id 2 stores no PoPs, because it feeds no aggregate.
- §6:
  - the loader syncs a tracker to the tip at once;
  - a level-4 `VerifyDB` still rebuilds;
  - forced flushes are `FORCE_FLUSH` and `FORCE_SYNC`;
  - `-persistderivedstate` defaults off;
  - a check mismatch erases the record and leaves the tracker to rebuild.

Step 1b changed these points:

- §4.2: id 1's digest is listed as built, with the superset it keeps.
- §4.3: the PoPs are a table of their own, and the states name sets by
  their place in the set table. Bridge V2 also suppresses the record.
- §6:
  - the loader leaves id 1 at its block, and the first
    `FindMostWorkChain` brings it to the tip, after the bridge;
  - the check rebuild of id 1 syncs the bridge first too;
  - step 1a's load messages now name the tracker. Those for a missing,
    unreadable or unusable record end "rebuilt from blocks when first
    needed"; the others end "will be rebuilt", except the pin's (item 5).

The step 1 review changed these points:

- §5: the PoP cost at load grows with the never-trimmed trail.
- §6:
  - item 4 holds the epoch starts to the chain (0647a33e) and the sets
    to what Build makes (cfc47691), and item 5 keeps a record from
    raising the pin (77070016);
  - no record is written while the coins and the chain's tip differ, and
    a power loss can leave records above the coins (54c6d329);
  - the trust paragraph names the unkeyed checksum.
- §10 lists the review's cases; §11 lists three middle grounds for
  question 3; §12 notes the legacy prefix's cost to every test process.

Not built yet: the `-dbcrashratio` hook, the crash functional test and the
fuzz targets (§10).
