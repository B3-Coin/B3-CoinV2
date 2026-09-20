# Isolated model test record — profile 1

Status: **IMPLEMENTED FOR INDEPENDENT AUDIT, NOT PRODUCTION QUALIFIED**.

Frozen reference: `2b32645e26f4ba41de8aa746bd04e84192fb3972`.
Branch: `model/flowmesh-v2-accounting-test1`.
Profile: `flowmesh-v2-isolated-accounting-test/1`.
Environment used for this record: Python 3.14.6, standard library only.

Executed from the model repository root:

```sh
python3 -m unittest discover -s test/flowmesh_v2_model -p 'test_*.py' -v
```

Recorded pre-freeze result: **88 tests, 0 failures, 0 errors**, 4.539 seconds,
exit status 0. This duration is suite execution time, not a trade-latency,
certification or network benchmark. The exact committed-tree repeat is recorded
in the stage handoff outside this commit, avoiding a self-referential commit ID.

## Coverage actually executed

| Test file | Tests | Scope |
| --- | ---: | --- |
| `test_model.py` | 50 | Shared reservations, partial fills/replacement/cancel, fees/claims, transfers, capacity, duplicate/replay/equivocation, rollback, V1 separation and generated replay |
| `test_curves.py` | 17 | Interpolation/auction/rationing reference, persistent bound, canonical order and explicit intermediate ceilings |
| `test_validation.py` | 9 | Corrupted snapshot/deposit/claim/high-water/V1-link rejection |
| `test_decimal_fees.py` | 6 | Native zero AssetId, 9/6 and 8/0 decimals, exact stable fees, order/batch dust and distinct quote assets |
| `test_boundaries.py` | 6 | Stable-quote boundary, five-phase timing, cancel/withdraw sequence, limit exhaustion and atomic arithmetic refusal |

The generated test executes 80 mixed-action batches from seed `0xF10A2026`
on two model instances with different action arrival orders and periodic
restore. It compares both reports and canonical state after every batch.
This is reproducible bounded coverage, not exhaustive state-machine proof.

Persistent-bound enumeration covers 215 small monotone BUY curves, two atom
scales and all reachable nonzero partial-fill/spend transitions within those
small domains (more than 1,000 transitions). This does not prove every possible
production-width curve mathematically or test the production C++ integration.

## Invariants exercised

- Each exact AssetId's synthetic custody equals available spot plus owned
  reservations, backed futures cash, pending payouts and protocol balances.
  Completed payouts reduce custody exactly once. Equal symbols do not combine.
- Reservations are exclusive. Cross-market contention and order/withdrawal/
  futures-transfer contention resolve in the disclosed account-sequence order.
- Immutable OrderId, expected revision and lifetime notional/fee history survive
  partial fills and replacement; unsuccessful replacement leaves the order's
  money and counters intact. Close releases exactly its remaining reservation.
- Deposits credit spot once. Explicit transfers are atomic; a repeated ActionId
  cannot move cash again. Risk absence/undefined/denied status fails closed.
- Completed action A returns its original outcome even beside conflicting B.
  New equivocation executes neither action; gaps do not consume a sequence.
- Aggregate stable fees remain in their exact asset. Historical test seat and
  treasury claims cannot spend each other's funds or reuse consumed receipts.
- Interruption after actions or one fill rejects the whole staged candidate.
  Snapshot restore retains original semantic bytes, outcomes and high-water.
- V1 fixture custody/claims do not automatically become V2 credit. A synthetic
  settled payout and distinct new deposit are linked and consumed once.

## Counterexamples retained, not economically hidden

1. **Persistent curve:** BUY `[(0,4),(4,0)]` has a one-auction maximum notional
   of 4. Sequential fills at 3, 2, 1 and 0 spend 6. A reservation of 4 is
   unsafe; the conservative staircase bound is 12. Cancel/replacement releases
   the actual unused reserve, not a guessed one-auction amount.
2. **Order dust:** three fresh 9,999-atom orders collect zero from that side.
   One order filled in three parts accumulates 29,997 notional and collects
   one atom. The model does not silently introduce a minimum or carry across
   fresh OrderIds. This needs an explicit economic decision before production.
3. **Batch dust:** two markets collecting 4 atoms each in the same quote asset
   and batch give treasury `floor(8/5)=1`. Separate batches give `0+0` because
   there is no carry. Separate AssetIds also remain separate groups. Nominal
   80/20 is not an exact fractional split at indivisible atom precision.
4. **Recipient remainder:** the same 8-atom fee group leaves 7 FN atoms; two
   historical seats receive 4 and 3 by canonical SeatId. Repeated remainders
   may systematically favor the first seat. This remains a test assumption.
5. **Contention:** with 100,000 quote atoms, the first market's 60,003 reserve
   leaves 39,997. A second request for 60,003 fails without borrowing the
   first reserve. Private input order does not choose the winner.

The development inspection also added missing restoration checks for deposit,
claim and V1 receipt identities. The corruption tests passed on their first
recorded execution after those checks; there is no retained failing-before run
and no claim that these are production wallet repairs.

## Deliberate limits and next gate

This model uses synthetic `authorized`/`verified` inputs, explicit capacities
and a synthetic risk adapter. It neither verifies signatures/proofs nor decides
data availability, BFT votes, custody authority, oracle/margin/PNL, liquidation,
backstop funding, futures contracts or live migration. Futures cash is backed
cash, not a fabricated settled profit. A whole V2 release still requires spot
AND futures, PoS V2 improvements, safe recovery and ordinary validator-off Qt.

No node imports, node/Qt build changes, live wallets, contracts, signing keys,
network I/O, activation height or deployment. Snapshot consistency is not an
authenticated history or crash/power-loss recovery claim. No 200 ms claim.
Non-stable quote markets remain unimplemented until their stable-fee conversion
is specified; genuine asset/asset trading remains locked product scope.

Ethereum P2P replacement is DEFERRED without weakening existing verification,
fork-range, freshness or unchanged-contract compatibility. Bootstrap/cutover,
lineage-expiry and BridgeAssetId/RegistryId corrections stay separate open
authority/specification gates. Retaining a V1 producer election is not PoS V2.

Stop here for an independent read-only audit of this exact model, profile and
adversarial tests. Any later correction gets a new identifiable commit. Do not
advance to node/BFT/bridge/migration/futures integration from these passes.
