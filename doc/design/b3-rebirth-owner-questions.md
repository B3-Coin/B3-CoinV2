# Owner questions from the gap-filling run (2026-10-25 to 2026-10-26)

**Status: questions for the project owner; nothing here is ruled.** Each
question blocks part of one ledger task, and the rest of that task was
done. No answer was chosen on the owner's behalf. Paths under
`~/B3-evidence/` are evidence bundles on the build host, each with a
verified manifest; they are not in the repository.

## M4-STO-01: journal recovery (BLOCKED-ON-RULING for three parts)

The task text says "single-segment recovery is real; torn tail handled".
The code does not handle a torn tail: a reopen refuses it, and the file
must be investigated by hand (`file_journal.h`, `record.h`). The plan, the
ledger and the code leave three things open.

1. **A torn tail or a CRC break.** A crash during a barrier write can
   leave the last record partly written, so its CRC fails. That looks the
   same as damage to the last acknowledged record. Plan section 3.7 and
   DUR-2 say a CRC break retires the key. The ledger row asks for torn-tail
   recovery. On the signer journal, dropping a damaged acknowledged vote
   could let the seat sign again differently. Which rule applies?
   - (a) Never truncate. Any CRC failure is a refusal on both journals, as
     today.
   - (b) Truncate an unreadable tail on the commit journal only. On the
     signer journal it retires the key.
   - (c) Truncate on both, but only when everything after the last valid
     record is zero or unreadable, and nothing valid follows.
2. **What retiring a key does.** No document says how retirement is
   recorded or enforced. Today an unreadable signer journal already stops
   signing, because the journal cannot be opened. Should retirement leave
   a durable marker that survives a repaired or restored file? If so,
   where, since the signer directory is the damaged one? And how does the
   seat come back with a new key: a new epoch roster, which is outside
   `src/flowmesh2`?
3. **Unique instance ids per role.** Two journals of one role with one
   instance id cannot be told apart, even with the new segment headers.
   The regtest operator takes the ids from its config. Should ids be
   random at creation and recorded in the store identity, checked against
   a per-role registry, or derived from the seat and epoch?

Done without a ruling: the index rebuild at every barrier is tested with
file journals and rollover. The signer log's equivocation index and the
commit log's decision index after a reopen equal the ones before. DECISION
replay was already real.

## Latency (task 1.4)

These are in `~/B3-evidence/latency/report.md`:

1. May a guard result be reused for the same parent and observation within
   one slot, or must every frame run the guards afresh?
2. Should the 200 ms target hold on a shared, loaded host, or only on the
   reference machine?
3. Should the gate also measure a long run, for example 1000 fills? Latency
   grows from about 238 ms to 390 ms over 1000 fills.

## M4-STO-03 snapshots and restart (task 2.4)

Done without a ruling, all marked PROVISIONAL in ARCHITECTURE.md O-95:
the snapshot is the commitment leaves, the store is LevelDB, the mark is
written after the snapshot, and replay below a snapshot base checks the
headers as a chain.

1. **Which mark a restart uses.** A restart re-executes every slot the
   pool and the read index keep, so it starts from the newest mark at
   least that window (1,024 slots) below the head. A newer snapshot is
   not used yet. The other choice is to persist the pool's INCLUDED
   records, so the newest snapshot is always the base. Which do you want?
2. **A test-only action window.** The simulator has a TEST-only setting
   that shortens max_delay_slots and outcome_index_slots to 16. Without
   it, a restart test needs more than 1,024 slots. DevRegtestProfile and
   its pin are unchanged. Is this acceptable, or should the tests run the
   full window?
3. **A stale test name.** The case
   `replay_tests/replay_of_snapshot_and_view_records_is_a_stub` no longer
   tests a stub. Renaming it removes an INVENTORY line, which needs your
   sign-off. May it be renamed?
4. **The TODO row's wording.** The row said a restarted pool answers an
   exact resend "A5/A6 instead of DUPLICATE". In the test the answer is
   NONCE_USED, which is A9, because the nonce window has not passed. The
   row now records what the test shows.
5. **The regtest operator** still restarts by re-executing from genesis.
   Should it move to snapshots now, or with M4-STO-07?

## Host load (all tasks)

The TSan run of the unit suite comes close to CTest's 120 s limit when
the trading bots load the host. One CI replay timed out and passed when
run again alone. Should the limit be raised for the TSan job, or should
the suite be split?

## M4-STO-07 durable view state (task 2.5): BLOCKED-ON-RULING

The task asks to replace the regtest PBFT's private control journal with the
general VIEW_STATE, NEW_VIEW_ACCEPTED and PREPARED_QC records. No document
fixes how. PROTOCOL.md says view entry and the exact VIEW_CHANGE report intent
commit atomically. The only target shape of VIEW_STATE holds an epoch and a
view, so it cannot carry the report. The control journal from 12b501fe already
meets the rule with CONTROL_INTENT, and the four-seat round trip passed on it.

Done without a ruling: two restart tests in `pbft_view_change_tests`. Seats
restart from their journals alone, before and during a view change, and still
carry their prepared QC, so the new leader selects the prepared body. A planted
bug that forgets the PREPARED_QC records on restart makes both fail. One test
decides the other body, and the other fences with PBFT_REPORT_REPLAY_HIGHEST.
The task's failing-first ("a restart in view change loses the prepared QC")
does not fail on the current code. It would fail only once the control journal
is removed.

1. **Which design should the general records follow?**
   - (a) Keep the control journal as the durable view state, and retire the
     VIEW_STATE target shape and the three stubs.
   - (b) Move the control records into the one signer journal, so a seat has
     one SIGNER journal. VIEW_STATE would then be the atomic view entry with
     its report intent, which changes its target shape.
   - (c) Something else you have in mind.
2. **Is the on-disk change acceptable?** Options (b) and (c) change the
   regtest operator's journal layout, so the round trip must be qualified
   again.
3. **What should AppendPreparedQc become?** The commit log already writes
   PREPARED_QC through RecordVerifiedPreparedQc, which trusts the caller's
   verification. Should the stub be removed, or should it verify the QC
   itself?

## Sanitizer suite time against the CTest limit (measured 2026-10-25)

On a quiet host (load about 4 on 32 CPUs), the whole `flowmesh2_unit` suite
takes about 103 s under ASan+UBSan and about 110 s under TSan. CTest's limit
is 120 s. Both jobs timed out once each today while the host was loaded and
passed when rerun alone. Each new task adds cases, so the margin keeps
shrinking. Options: raise the sanitizer jobs' CTest timeout, split
`flowmesh2_unit` into several CTests by suite, or run the sanitizer suites
with Boost's parallel runner. All three touch `src/flowmesh2/CMakeLists.txt`
or the CI workflow, so none is done.

Update during M2-LED-10 (load about 16): the whole ASan binary alone took
112.8 s at HEAD and 112.6 s with the change. The second limit is the protected
`check_inventory.py`, which runs the whole binary again with its own 120 s
`RUN_TIMEOUT_S`. CTest's limit alone can therefore not fix it. My local replay
ran six CTests in parallel and timed out at 120 s. GitHub runs them one at a
time, so the replay now runs serially as well. The slowest cases under ASan:
`random_ops_keep_conservation_and_match_an_independent_tally` 36 s,
`split_exhaustive_0_to_10000` 10 s, `w3_every_sequence_up_to_7_matches_naive_model`
7.5 s, `allocate_pro_rata_exhaustive_small_domain` 5.4 s.

Update during M2-LED-02 (load about 5): the serial TSan replay failed once.
`flowmesh2_unit` took 119.97 s and `check_inventory.py`'s second whole run
passed 120 s (`TimeoutExpired`). The new SMT suites cost about 2.3 s under
TSan, so I halved their sizes. The rerun passed, but with no real margin:

| job | `flowmesh2_unit` | limit |
|---|---|---|
| ASan+UBSan | 109.8 s | 120 s |
| TSan | 118.4 s | 120 s |

The slowest TSan cases are
`random_ops_keep_conservation_and_match_an_independent_tally` (31.3 s,
640,000 ledger operations), `split_exhaustive_0_to_10000` (13.8 s) and
`allocate_pro_rata_exhaustive_small_domain` (6.5 s). Per-case lists are in
`~/B3-evidence/M2/M2-LED-02/notes/`. I did not shrink those property tests,
because that would weaken a guard. The next task that adds tests will likely
fail TSan on a GitHub runner. This needs your decision: raise both limits,
split the binary, or name the slow cases to run outside the sanitizer jobs.

Update after `bd539e35` (2026-10-25): the margin is back without raising a
limit or shrinking a test. Four exhaustive loops now use `FM2_REQUIRE`
(`test/util/require.h`), which checks the same condition but skips Boost's
per-pass bookkeeping; four planted bugs show the checks still stop their
cases. Serial replay: ASan+UBSan `flowmesh2_unit` 92.8 s, TSan 90.9 s
(was 117.7 s). The question stands for the longer term: the slowest TSan
case is now `random_ops_keep_conservation_and_match_an_independent_tally`
(about 31 s), whose comparison after every operation is quadratic in the
ledger size. I left it alone because a cheaper comparison could miss a
copy-on-write bug.

## M0-FUZ-01 fuzzing (2026-10-25)

- **No fuzz job in CI.** `bin/fm2_fuzz` builds with `BUILD_FUZZ_BINARY=ON`,
  and the evidence was made by hand on WSL with Clang 18. The CI workflow is
  owner-protected, so no job was added. Should CI build `fm2_fuzz` under Clang
  and replay the kept corpora, or run each target for a short time?
- **A cap bug the round trip cannot see.** A decoder cap one byte too large
  passes the round-trip oracle when the encoder ignores the cap too. The
  certificate, envelope and account-leaf targets now also assert their
  documented bounds. A planted certificate bitmap cap of 129 bytes was still
  not found in 60 s, from an empty corpus or from valid seeds, because it needs
  a 258-byte input with one exact length byte. Caps that depend on a profile
  (action payload, outcome fills and deltas, body items, snapshot leaves) are
  not bound-checked by the targets.
- **Not fuzzed.** The segment header check is internal to
  `storage/file_journal.cpp`, and `ShortIdCodec` is a stub (M4-NET-02).

## TCP mesh under adversarial input (task 2.7, 2026-10-25)

No accepted replay was found: a replayed AUTH is refused as ciphertext, on a
new connection and re-encrypted in a fresh session. Two properties of the
current design are reported, not changed:

- **Pending slots can be held by strangers.** Any host that can reach the
  listen port may hold all `max_pending_connections` slots without a key,
  renewing each one at its handshake deadline. A seat that dials meanwhile is
  refused PENDING_FULL and retries every `reconnect_ms`. Endpoints are pinned
  per seat, so the listener could accept only from pinned addresses, or keep
  a slot per seat. Which do you want, if either?
- **The listener signs before the dialler proves its key.** It sends its own
  session signature right after the HELLO. The signature covers a fresh
  session ID, so it cannot be replayed. Still, every stranger's connection
  costs the listener one BIP340 signature. Signing after verifying the
  dialler's AUTH would remove that cost but changes the handshake order.
- **A replay's reason is not fixed.** BIP324 encrypts the length separately,
  so a replayed packet is refused at its noise length (FRAME_OVERSIZE or
  AUTH_LENGTH) or, rarely, by the AEAD (DECRYPT_FAILED). The tests accept
  either.
- **M4-ADM-01 and M4-ADM-02 are in the task title.** M4-ADM-02 is already
  BLOCKED-ON-RULING (O-73, O-77). For M4-ADM-01, see its own entry.

## M4-ADM-01 batch verification: where the workers live (BLOCKED-ON-RULING)

The row says "`CCheckQueue` batch BIP340 plus `CuckooCache`" in
`core/action` `BatchVerifier::VerifyBatch`, with `Admission::AdmitBatch` in
`admission/`. `core/` is a deterministic directory. Guard rule 2 bans
`std::thread` and `<thread>` there, and CCheckQueue owns worker threads. The
secp256k1 in the tree has no batch Schnorr API, so "batch" can only mean
parallel single verifications. Options:

- (a) `BatchVerifier` in `core/` stays single-threaded, adding only the
  verified-signature cache. `admission/` owns a CCheckQueue and splits a
  batch across it, calling `VerifiedAction::Verify` per action.
- (b) Move `BatchVerifier` to `admission/` (not deterministic), where it may
  own the CCheckQueue. `VerifiedAction`'s friend declaration moves with it.
- (c) A guard exception for one file in `core/`. This weakens a guard, so it
  is listed only for completeness.

Also open: whether the CuckooCache is per process or per `Admission`, its
size, and whether a cache hit counts in `AdmissionStats::sig_verifications`.
Nothing was built. The row is marked BLOCKED-ON-RULING.

## M3-ENG-01: the band book is built, but four D6 values block the swap (2026-10-25)

`engine/band_book.h` now has the plan §3.5 book: a 2^17-tick level array,
the two-level bitmap, a Fenwick tree, one FIFO node pool and an OrderId
table that is never iterated. It matches the skeleton `OrderBook` on 10^5
random operations. The engine still builds the skeleton, because wiring
the new book makes certified outcomes depend on values no document pins.
D6 is on the do-not-touch list.

1. **Where does the band sit?** The book holds 131,072 consecutive prices
   per market. The plan does not say which. Options:
   - a fixed origin per market at listing;
   - centred on the epoch reference price, whose source is itself blocked
     (M3-ENG-03);
   - re-anchored when the book is empty.
2. **How many orders fit in a book?** It is one capacity for both sides
   today. The plan says "per market side" for the queues but gives no
   number.
3. **Which certified code does each refusal get?** A price outside the
   band could be `REJ_PRICE_BAND` or `REJ_BOOK_FULL`. A full book is
   presumably `REJ_BOOK_FULL`. Neither is produced today.
4. **Is about 6 MiB per book acceptable?** A book holds that much however
   few orders rest. Every candidate copies the whole engine state until
   M2-LED-10 lands, so one copy costs about 1 to 3 ms per market on this
   host. A skeleton book holding 1,000 orders copies in 0.04 ms.

Measured on one pinned core, 200,000 operations, latencies in ns:

| Depth, levels per side | Rest p99, before / after | Remove p99, before / after | FillBestFront p99, before / after |
|---|---|---|---|
| 1,000, 100 | 571 / 471 | 441 / 240 | 360 / 220 |
| 10,000, 1,000 | 1,122 / 561 | 1,072 / 601 | 571 / 231 |
| 10,000, 20 | 831 / 511 | 2,595 / 631 | 521 / 231 |
| 50,000, 5,000 | 3,356 / 1,042 | 3,257 / 1,212 | 1,433 / 451 |

Also reported: an ActionId does not cover the signature, so OrderIds can
be ground to share a table slot. Each colliding id costs every lookup in
the cluster one probe. It is bounded by the capacity and never changes a
result.

## M2-LED-10: copy-on-write ledger and phase 9 by changes (2026-10-25)

Landed for review, not a ruling request, but three points need your eye.

1. **Phase 9 is now inductive.** The engine no longer re-checks every
   account at every slot boundary. `EngineState::Verify` checks the accounts
   that changed since the last state that passed the check, plus every
   resting-order owner then and now, and the books in full. Genesis and
   snapshot loads still run the full `CheckInvariants`. Tests compare the
   two verdicts (code and detail) on doctored states, including a change
   made before a failed check. Is a by-changes phase 9 acceptable as the
   guard, or must the full check stay (for example in debug builds, or every
   N slots)?
2. **The books are still O(orders) per slot.** Every candidate copies,
   hashes and checks the whole order book. The `TODO(rebirth:M2-LED-10)` tag
   stays in `engine/engine.cpp` and the row stays `outline`. The band book of
   M3-ENG-01 is about 6 MiB fixed per book, so its copy cost decides how the
   books become copy-on-write. This waits on the D6 swap questions above.
3. **A v0 account insert still rehashes the leaves after it.** Commitment
   v0 is a tree over sorted leaves, so a new account at position p costs
   about n − p leaf and node hashes. Updates cost O(log n). True O(changed)
   for inserts arrives with the sparse Merkle tree of M2-LED-02 (task 2.10).

## M2-LED-02: sparse Merkle commitment v1 is built, the switch is BLOCKED-ON-RULING (2026-10-25)

What landed is dormant and labelled PROVISIONAL REGTEST (ARCHITECTURE O-96):
compact sparse Merkle trees with typed proofs (`core/smt.h`), the six v1
roots and a typed account proof that can show an account absent
(`core/state_root_v1.h`), the ledger trees with O(changed) updates
(`ledger/state_commitment_v1.h`), and a b3verify check that accepts only a
header with `commitment_version` 1. No header carries version 1 yet, so
nothing a client sees changes. Four questions block the switch:

1. **The version byte is overloaded.** `commitment_version` today means
   0 (five sorted roots), 2 (adds withdrawals) and 3 (view-bound COMMIT
   digest). 1 was reserved for the SMT. Version 3 changes the certificate's
   view format, not the state tree. Does v1 keep that meaning of "SMT, no
   view binding", or does the SMT need a new value that also binds the
   COMMIT view (a 4)? The header codec, `ExpectedCommitmentVersion` and the
   certificate transport all key on this byte.
2. **Which profiles switch, and when.** A profile's version follows two of
   its flags (`withdrawals_enabled`, `view_bound_commits`). A switch needs a
   new profile flag, which changes the profile encoding, every profile hash,
   the protected `PROFILE_DEX1_REGTEST.json` and every state root vector.
3. **The component split.** Plan section 3.7 lists separate trees for
   balances, reservations, orders, positions, markets, nonce windows, seat
   accruals, withdrawals and fact nullifiers, plus a Merkle-sum tree per
   asset. v1 as built keeps v0's six components and v0's leaf values (one
   account leaf holds balances, reservations, nonces and open orders). That
   was the smallest change that gives absence proofs. Is the plan's split
   required for v1, or a later version?
4. **The proof size target.** The plan says a balance proof is about
   768 B. A member proof over n hashed paths here is about
   3 + 32 (log2 n + 1.3) + 65 bytes plus the account leaf. Measured
   (`~/B3-evidence/M2/M2-LED-02/perf/smt.txt`), SMT path only:

   | leaves | mean | median | p99 | max |
   |---|---|---|---|---|
   | 2^20 | 720 B | 710 B | 806 B | 871 B |
   | 2^21 | 753 B | 742 B | 838 B | 903 B |

   The typed account proof adds 34 bytes plus the account leaf. So the
   SMT path alone meets 768 B on average at 2^21 accounts, and the whole
   typed reply does not. Is 768 B a target for the SMT path alone, or for
   the whole typed reply with the leaf? Is it a mean or a bound?

The stub moved to `engine::EngineState::CommitV1`, which stays
NotImplemented until these are answered.

## M4-NET-02: short IDs are built; the key derivation is provisional (2026-10-25)

`net/short_id` is real now: `Encode`, `Decode`, and `Resolve` plus `Fill`
for the bodies a seat must fetch. Not blocked, but two choices are labelled
PROVISIONAL REGTEST (ARCHITECTURE O-97) and need your confirmation:

1. **Key derivation.** BIP152 takes k0 and k1 from one SHA-256 of the block
   header and the nonce. fm2 uses `Tagged(SHORT_ID_KEY, header_hash || u64 LE
   nonce)` (new hash tag 33), so a short-ID key can never equal another fm2
   digest. Keep this, or use plain BIP152?
2. **Collisions inside one proposal.** A short ID that occurs twice in one
   proposal, or matches two held actions, is never guessed. The position
   stays open and needs the full 32-byte ID. A collision costs a round trip,
   never a wrong body. The body is still checked against `actions_root`
   before any vote.

Not built, because they belong to M4-AGR-02 and M4-AGR-04: the fetch
messages, their lane, and whether the inclusion lists carry short IDs.
Whether the vote signature covers inclusion IDs is already open as O-57.

## Hive Trade page on the V2 regtest API: read-only built, signing BLOCKED-ON-RULING (2026-10-25)

Task 3.3 asked for the Trade page against the V2 HTTPS API, read-only first,
then signing through the approval window. The read-only card is built
(`hive/src-tauri/src/v2api.rs`, `hive/web/src/v2market.ts`). Three findings:

1. **The V2 API has no book, clearing or trade route.** It serves status,
   totals, account and withdrawal proofs, the certified-state stream and
   signed submission. The task says to point the Trade page's market view at
   it; the book, clearings and trades cannot come from it, so the card shows
   status, markets, totals and balances, and says the rest is not served.
   Is a book/trades route planned for the V2 API, or is the Trade page
   expected to show them from somewhere else?
2. **Signing needs a ruling.** `/v1/submit` takes a BIP340-signed V2 action
   frame. B3 Hive has no secp256k1 crate, and the rules forbid adding one;
   `b3coind` has no RPC that signs V2 frames. Which is wanted: a crate in
   Hive (a dependency-policy decision), or a node or operator RPC that signs
   with a wallet key behind the approval window?
3. **The API certificate was a CA certificate.** `create_local_tls` made it
   with `openssl req -x509`, which marks it `CA:TRUE`. rustls (used by Hive
   through ureq) refuses a CA certificate presented by a server
   (`CaUsedAsEndEntity`). The certificate is now issued with
   `basicConstraints=critical,CA:FALSE`; the Python client and both HTTPS
   test files still pass. This changes a regtest fixture only.

Also seen while reading the operator: `Info()` in
`src/flowmesh2/dev/regtest_operator.cpp` still reports `seats: 4`,
`quorum: 3` and computes `next_leader` with `% 4`. The operator builds only
four-seat networks today, so this is correct for it, but it is one more place
the seat-count groundwork (M4-AGR-13) will need to reach.

## Sanitizer time, final figures (2026-10-25, serial replay)

bd539e35 cut four exhaustive loops, which bought back some margin. Then
2.12 and M4-STO-09 added cases:

| revision | ASan+UBSan unit | TSan unit | ASan inventory | TSan inventory |
|---|---|---|---|---|
| bd539e35 | 92.8 s | 90.9 s | | |
| efce764d (2.12) | 106.7 s | 103.9 s | 206.2 s | 204.7 s |
| 0cd8021f (M4-STO-09) | 102.5 s | 99.7 s | 202.5 s | 200.3 s |

The limits are 120 s for `flowmesh2_unit` and 300 s for
`flowmesh2_inventory`. The unit suite uses about 85 % of its limit on this
host, and a slower GitHub runner may not fit. The decision in the section
above is still open.

## B0: derived-state undo cost, measured; nothing implemented (2026-10-25)

Task 3.2 says to implement only after you have seen the numbers. The full
report is `~/B3-evidence/B0/report.md` on the build host, with the raw
CSV and a verified run manifest.

- **Today a one-block reorg replays the whole modern era.** The stake and
  finality trackers go dirty on any disconnect and replay from H+1. The
  binding, seat, vault and checkpoint trackers undo one block in place,
  and replay only after a restart or a gap.
- **About 18 µs per modern block** end to end on a 4,024-block regtest
  chain, linear in length. At mainnet's roughly 46,500 modern blocks
  that is about 0.8 s per one-block reorg, growing with every block. A
  full replay of all six trackers would take about 4.3 s.
- **These are floors.** The fixture's blocks are almost empty, it has
  4 validators against mainnet's 47, and the bridge index was not
  measured.

Question: should the stake and finality trackers get per-block undo
records, as the other four have? If so, should that wait for V2? A design
(B0a for reorgs, B0b for restarts) is in
`doc/design/b3-height-c-fast-track.md` §4. Row B0-B3D-01 is OWNER-GATED,
so nothing is implemented.

## H1 escape race: checked; the escape gives no liveness against one adversarial member in force (2026-10-26)

Dependency document section 5.2 leaves the race between an escape block and
a late in-force block on the same parent to the section 13 model check.
Only the H-H1 part of section 13 concerns H1, and I checked that part: three
new `escape_race_*` cases in `finality_h1_tests` on the real code, and a
bounded exhaustive model (`test/b3_h1_escape/escape_race_model.py`).
Evidence: `~/B3-evidence/h1-escape-race/`.

- **Determinism holds.** After every branch switch the tracker equals one
  rebuilt from scratch; both arrival orders end on the same tip.
- **The escape always loses at equal height.** All modern blocks carry the
  same work, the tie goes to the branch whose first block is earlier, an
  escape block is always later than an in-force block on the same parent,
  and nothing refuses a block for being published late. One member in force
  that answers each escape with a block one round earlier holds the chain
  at one block per 24 hours until rotation lets new stake in: E to 2E
  blocks, so four to eight years on mainnet.
- **A withheld in-force branch replaces honest blocks** once it reaches
  equal height, up to the 1,440-block horizon. The eligibility digest does
  not cover block contents, so equal-time ties can be ground.

Question: is this acceptable for the interim release, which assumes the
in-force set is entirely offline when the escape is needed? The V2 design
adopts the same schedule and escape for its producers (V2-D3), so the
question also applies to height C.

The three candidate rules are compared in the model
(`escape_race_model.py --compare-fixes`; evidence
`~/B3-evidence/h1-escape-race/fix-compare/`). None is implemented, and each
is a consensus change:
- counting the escape over the epoch restores near-normal chain growth
  under the slow-producer attack, but does not stop a withheld branch;
- refusing late blocks cuts withheld-branch reorgs to one block, but leaves
  the stall and depends on each node's clock;
- letting an escape block outrank a non-escape sibling changes little,
  because the adversary makes its own escape sibling and the grindable hash
  decides.
The summary and a recommendation for prototyping are in
`doc/design/b3-height-c-fast-track.md` §5.

## -coinstatsindex aborts the node on B3 chains (2026-10-26)

`index/coinstatsindex.cpp` is unchanged from Bitcoin Core. It takes each
block's new money from Bitcoin's GetBlockSubsidy (50 coins, halving). B3
mints differently in every era: legacy proof-of-work and coinstake rewards
(block 1 alone pays 260,000 coins), the corridor reward, and the modern
subsidy. When a block mints more than Bitcoin's subsidy, the running
"unclaimed rewards" goes negative, wraps as an unsigned number and fails an
assert, which aborts the node. Reproduced on modern regtest at block 1:
coinbase 2,000,000,000 base units against an assumed 50,000,000. Mainnet
should abort at block 1 the same way (by the code; not run). Evidence:
`~/B3-evidence/coinstatsindex/`. The option is off by default, and neither
Qt nor Hive sets it.

Question: refuse `-coinstatsindex` on B3 chains with a clear message, or
teach the index B3's issuance per era? The second changes what
`gettxoutsetinfo` reports as subsidy.

## P3 against reorganizations: modelled; the pin and the horizon are not ordered (2026-10-26)

`test/b3_p3_reorg/p3_reorg_model.py` checks Stage 0's P3 voting, and V1's
ancestry lock beside it, against arbitrary reorganizations at one
checkpoint height. Evidence: `~/B3-evidence/p3-reorg/`. Details are in
`doc/design/b3-height-c-fast-track.md` §9.

- **V1 reproduces both incidents.** Two orphan locks of four (811,631), or
  one orphan lock plus one crash (814,191), and no certificate ever forms on
  the winning branch.
- **P3 as written holds.** Agreement and progress after stabilization hold
  in every reachable state of the bound, with one Byzantine member in any
  leader position or one crash. A member locks only on an observed quorum,
  and a view change carries that lock or frees it.
- **This depends on validity not reading the voter's tip.** If a member
  votes only for a candidate on its own tip, a prepared quorum on a block B3
  then abandons becomes V1's orphan lock again: every new view must
  re-propose it, and the members on the winning branch refuse. Progress then
  stops with all four members honest. Stage 0 already forbids the tip check
  (`v2-stage0-r1-consensus.md:118-140`); the model shows that rule is
  load-bearing.
- **P3 may decide the branch fork choice left**, so B3 must reorganize back
  to the decision. Stage 0 intends this (`b3-flowmesh-v2-stage0.md:576-581`).
  P3 gives no bound on how late a node learns a decision. b3d refuses a
  header 1,440 blocks below the tip (`src/validation.cpp:7042`) before it
  checks the finality pin (`:7050`). A node that learns a decision when its
  branch forked that far below its tip would refuse the headers leading to
  it. If it also applies the pin, it follows neither branch. This has the same
  shape as the late-upgrader case (`b3-bridge-v2-finality-dependency.md:230`).

Question: for P3, should a valid CommitQC outrank the reorganization horizon
for the headers that lead to it, or should the horizon stay first and a
node that far behind need manual recovery? Stage 0 does not say.
