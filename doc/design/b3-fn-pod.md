# Proof of Disintegration — the B3 Fundamental Node creation mechanism

**Status: DESIGN DIRECTION (2026-08-16).** Historical mechanism authoritative
(traced from `master`); modern direction locked at the level below, with the
economics OPEN. This document supersedes any earlier description of FN
creation as a burn output. No modern FN economics, governance, rewards or
FlowMesh coupling are implemented; Modern PoS and the transition corridor
are unchanged by this design.

## 1. The historical mechanism — authoritative

Legacy B3 creates a Fundamental Node through **implicit destruction via the
transaction accounting gap** — Proof of Disintegration (PoD):

    total inputs  >  ordinary outputs

    ordinary_fee = total_inputs − ordinary_outputs − FN_collateral

Traced facts (`master`):

- The collateral schedule (`fn-activity.h GetFNCollateral`): 25,000,000 B3
  through height 85,000; 20,000,000 through 105,000; 15,000,000 after;
  testnet 15 B3 past height 60.
- The disintegrated amount **has no output**. It is not a fee: both the
  mempool path (`AcceptableFundamentalTxn`, `main.cpp:740`) and
  `ConnectBlock` (`main.cpp:1643`) compute
  `fee = in − collateral − out`, so the block producer cannot claim it and
  it permanently leaves the spendable supply. The only on-chain evidence is
  the gap itself.
- The wallet flow sends collateral **+ 1 B3**; the 1-B3 output is the FN's
  registration marker. `CFundamentalnode` identity is a `CTxIn vin` bound
  to operator pubkeys via **network-layer broadcasts** (fn-manager /
  fn-activity), masternode-style.

**Never describe historical FN creation as an explicit burn output.**
Historical bytes remain historical; replay reproduces the state effect
mechanically (the gap simply never re-enters the UTXO set), and the ported
fee rule (`legacy::GetLegacyTransactionFee`) plus the per-index
`m_legacy_fn_integrated` aggregate preserve the accounting during live
validation.

### What chain data alone cannot recover

The operator/pubkey registration lived in P2P messages, not blocks. From
chain data alone one can recover: every disintegration transaction, its gap
amount, its height, and its outputs (including the customary 1-B3 marker,
and therefore *whoever can spend that marker output*). One can NOT recover:
the historical operator pubkeys, service addresses, ping/activity history,
or any network-layer binding. Claim derivation (OD-5) must therefore be
defined over the recoverable facts — the natural candidate being
"beneficiary = controller of the disintegration transaction's marker
output" — and never over network-layer state.

## 2. Modern FN creation preserves PoD

Modern B3 keeps Proof of Disintegration as the FN creation mechanism — it
is NOT replaced by a generic BURN output. The economic signature is
preserved: **B3 is permanently sacrificed, and an FN right is created.**

    Modern FN creation transaction

    Inputs:   B3 being disintegrated
    Outputs:  ordinary change / payment outputs
              one explicit FN ownership (FN Coin) output
    Gap:      exactly the required PoD amount (plus the ordinary fee)

    invariant:  inputs − outputs − ordinary_fee = PoD amount

The PoD amount remains permanently excluded from B3 supply; the miner can
never claim it; a modern FN creation transaction is explicitly recognizable
by consensus (no ambiguity between fee and destruction).

### PoD is not BURN

    BURN  → generic, visible destruction of an asset (asset engine)
    PoD   → B3-specific economic transformation:
            B3 permanently sacrificed AND an FN right / FN Coin created

Both exist; they are never merged. Generic asset destruction uses the BURN
policy; FN creation uses PoD.

## 3. Modernized ownership: on-chain, no P2P registration

The historical weakness is fixed, not the mechanism: a modern FN creation
transaction explicitly creates an **on-chain FN ownership object** (the FN
Coin output) canonically identifying the FN creation identity, the owner
authority, policy/version, and any future consensus-required FN state. No
separate network-layer registration establishes ownership.

## 4. FN Coin is a separate asset/state from B3

If 25,000 B3 (example numbers) is disintegrated and 1 FN Coin is created:

    B3 supply      −= 25,000 (permanently)
    FN Coin supply += 1      (by FN rules, independently accounted)

The FN Coin never "recreates" the destroyed B3; destroyed B3 never returns
to circulating supply. Two ledgers, one event.

## 5. One PoD event, at most one FN

    PoDId = the modern FN creation transaction identity
            (or its designated creation outpoint — exact identifier OPEN)

    one valid PoD event  → at most one FN creation
    same PoD reused      → INVALID / no second FN

Replay, restart, reindex and branch handling must never double-create FN
state.

## 6. Lineage

    LEGACY FN                          MODERN FN
    =========                          =========
    historical PoD                     modern PoD
    implicit input/output gap          implicit input/output gap
    historical collateral schedule     modern PoD amount (OPEN)
    historical marker + P2P binding    explicit on-chain FN ownership
    aggregate-only chain accounting    FN Coin / FN policy object
                                       deterministic replay protection

Related, distinct, same economic signature.

## 7. Testing

The evolution test (`b3_evolution_tests`) exercises the AUTHENTIC
historical mechanism in its legacy phase: a real disintegration transaction
(gap = collateral + fee) inside a legacy PoS block, with the collateral
recognized exactly once (`m_legacy_fn_integrated`), the destroyed amount
excluded from the claimable fee (a coinstake claiming it is refused),
spendable supply reduced by exactly the collateral, an
insufficient-gap/fake-marker transaction creating no FN state, and the same
facts reproduced after restart, chainstate reindex, and on a
trusted-replay-mode synced node from raw block + undo data alone. The
regtest fixture uses a small collateral via
`Consensus::Params::legacy_fn_collateral_test_override`; **the mainnet
historical schedule is untouched and must never change.**

Future modern-FN tests (not yet implementable): valid PoD → exactly one FN;
insufficient PoD → reject; same PoD reused → reject; FN ownership transfer
deterministic; restart/reindex → identical FN state; B3 supply permanently
reduced; FN Coin supply independently accounted.

## 8. Decision status

**LOCKED / DESIGN DIRECTION:** historical FN creation = PoD; PoD is
implicit destruction through the accounting gap; PoD value is never a miner
fee; PoD permanently reduces B3 spendable supply; modern FN creation
preserves PoD rather than generic BURN; modern FN ownership is explicit and
on-chain; FN Coin is separate from B3 supply; one PoD event creates at most
one FN; historical and modern mechanisms share lineage but not encoding;
mainnet historical collateral rules unchanged.

**OPEN:** modern FN PoD amount; FN Coin issuance rate; dynamic vs fixed
pricing; excess-gap treatment (extra PoD / fee / invalid); ordinary-fee
calculation for FN creation transactions; FN Coin quantity per PoD; final
FN ownership policy serialization; FN Coin transfer rules and lifecycle;
FN reward economics (OD-4); claim derivation details for historical FNs
(OD-5, over recoverable facts only). Implementation must not close these
silently.
