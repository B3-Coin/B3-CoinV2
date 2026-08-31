# FN Coin + Colored Assets — Activation Design (owner rulings 2026-09-01)

The agreed design for shipping colored assets and FN Coin in the
transition release, recorded from the owner session of 2026-08-31/09-01.
Where this document conflicts with earlier design notes (notably the
4,000-byte proof-carrying issuance), THIS design supersedes them by
owner ruling; the conflict register should be updated when the older
documents are reconciled.

## 1. Release plan (owner ruling: at most two releases)

1. **Transition release** — pins THREE constants measured from the
   sealed chain in the same pause window: X (boundary hash), S_H
   (supply → R0), and the **FN rights root** (§4). Carries the full
   colored-asset and FN Coin code with **activation heights placed
   weeks after M**, so the modern era soaks before any feature fires.
   Ships only after the shadow-fork rehearsal (§7). The pause runs
   ~2–4 weeks; community messaging owns that honestly.
2. **FlowMesh release** — later, after a dedicated FlowMesh testnet
   with real FN holders as seats.

## 2. Colored assets (simple-v1, activation by height)

- As ruled 2026-08-22: one genesis transaction mints the entire fixed
  `max_supply`; no later mint exists by construction; AssetIdV1 is
  chain-bound and unforgeable; `issuance_mode` byte reserved for
  future modes (GENESIS_FIXED only in v1).
- **Issuance fee: flat 1,000 B3** (owner ruling 2026-09-01) paid to the
  treasury script as a coinbase-independent output inside the issuing
  transaction. Anti-spam economics: fake-token floods fund the
  treasury; serious issuers pay once.
- Remaining build: promote the test-only activation flag to a real
  height parameter; wire issuance/conservation validation; fee rule;
  tests.

## 3. FN Coin transfers (closes pre-activation gate (b))

**Owner ruling: moving an FN Coin requires a signature from the key of
the address committed on it — checked exactly the way an ordinary
spend from that address is checked — and the commitment is rewritten
to the recipient's address.**

- The signature lives in the spending transaction (its authorization
  area), permanently recorded on chain; it commits to the exact
  transaction, so it cannot be replayed or redirected.
- "Address" is a script: multi-signature FN ownership works for free.
- No funding-key signatures, no claim ceremonies, no transfer
  approvals, no lock-ins. FN Coins move like money because they are
  money (an indivisible kind: decimals 0, whole units only).
- Conservation: FN units in == FN units out on every transfer; only
  the genesis event (§5) and modern PoD transactions (§6) create, only
  extinguishment destroys, and a dead FN never reopens a slot.

## 4. The pinned rights root (kills the 4,000-byte proof design)

- At the seal, run the deterministic through-H `-podreport` scan
  (node/fn_pod.cpp) over the sealed chain. It applies the verbatim
  2017 rules — non-coinbase, non-coinstake, gap >= tier
  (legacy::GetFNCollateral), 1-coin P2PKH marker present — and yields,
  per qualifying disintegration: pod_id (txid), height, tier, and the
  marker output's address (the historical owner).
- Build a Merkle tree over the canonical serialization of that list;
  **pin its root in the transition release** alongside X, exactly the
  assumeutxo/X-pin trust pattern. Anyone re-runs the report over the
  sealed chain and checks the root.
- This SUPERSEDES the proof-carrying issuance design (the embedded
  historical transaction + funding transactions, ~4,000 bytes) and
  closes pre-activation gate (a) (carrier fit) by making it moot.
- Dry-run rehearsal before the seal: run the report against a copy of
  the synced datadir through the current tip to validate the pipeline;
  the final run at the seal takes minutes. NOTE: legacy disintegration
  remains possible until block 810,000 at the 15M-old-B3 tier; only
  the through-H run is final.

## 5. Historical issuance: FN GENESIS in the corridor (owner design,
## refined 2026-09-01: "use the PoW corridor's empty coinbase space")

**Block 810,001 — the corridor's first block — is FN Genesis. Its
coinbase creates all historical FN Coins; no issuance transactions
exist.**

- Corridor blocks are PoW with a plain, subsidy-free coinbase and
  near-zero traffic — ample room. The 810,001 coinbase MUST carry,
  in addition to its fees-only rule, the FN genesis outputs derived
  byte-for-byte from the pinned rights list:
  `3,500 × [FN output: 1 FN, owner commitment = legacy address #n]`
  (~300 KB; one block).
- Any miner produces the identical required set or the block is
  invalid. Every node validates equality against the pinned list.
- **No transfer lock (owner ruling 2026-09-01: "why increase
  complicacy").** FN Coins are spendable as soon as they mature — and
  because they sit in a coinbase, the standard ~30-block coinbase
  maturity applies automatically: a natural breather enforced by the
  oldest rules in the tree, with zero new lock code. The transfer rule
  (§3) is simply active from the transition release onward; its safety
  budget is exhaustive tests plus the §7 rehearsal, not a padlock.
- Holder experience: nothing to do, ever. Migrate the old wallet
  whenever; the FN Coin is already at the address that earned it in
  2017. No claims, no deadlines; dormant holders keep their rights
  perpetually.
- This mirrors the colored-asset model: genesis mints the historical
  supply in one event; transactions create the modern remainder.
- Supersedes both the archival-builder broadcast model and the
  batched issuance-transaction plan.

## 6. Modern creation: PoD transactions on the pinned curve

Example at slot #1 (price 15,000 B3), creator holds a 20,000 B3 UTXO:

    input:   20,000 B3           (ordinary UTXO, normal signature)
    outputs: [0] FN output       (1 FN, owner = creator's address)
             [1] change 4,999.999 B3
    action:  FN creation declaration

    gap = 15,000.001
      15,000.000  destroyed (RequiredDisintegration(counter));
                  never a fee, never producer-claimable, no burn
                  address — the 2017 accounting-gap semantics verbatim
           0.001  ordinary network fee to the producer

Checks: price from the pinned curve (15k/30k/60k per 500-slot tier),
exactly +1 FN, lifetime counter within cap 5,000 (3,500 reserved),
destroyed amount excluded from block fee accounting.

## 7. Shadow-fork rehearsal (owner-prescribed test method)

Copy the synced real-chain datadir; run 2–3 clients under a dev flag
that swaps network magic + port (physically cannot reach production),
localhost-only, with overridable H just above the copied tip and
trivial difficulty. Rehearse on real history: seal → corridor mining →
modern era → asset activation at its height → FN Genesis in the
810,001 coinbase → FN transfers after maturity. Mandatory before the transition
release ships.

## 8. Open items (small, non-blocking)

- Height spacing: assets activation ~2 weeks after M (owner blesses
  the number when the code is ready); FN Genesis fixed at 810,001,
  transfers live at coinbase maturity (no separate height).
- 810,001 block-size handling: one coinbase vs deterministic spread
  over the first corridor blocks.
- Canonical serialization of the rights-list entries (fixed before the
  dry run so the root is stable).
- Community messaging update: the pause runs weeks, not days.
