# B3 economics v1 — consolidated owner rulings

This is the economic source of truth for the transition release. Values marked
RULED may be implemented; OPEN values require a later explicit decision.

## Supply and denomination — RULED

- Legacy B3 UTXO value carries across H one-for-one. There is no premine or
  replacement issuance.
- The human-facing unit is B3 and one displayed B3 equals `1e9` base units.
- Native B3 and non-native colored assets are separate conservation domains.
- FN is a global non-native whole-unit asset with lifetime cap 5,000.

## Modern PoS emission — RULED 2026-08-26

At the measured seal supply `S_H`:

```
R0 = floor(S_H_base_units * 1% / 525,600)
reward(h) = R0 >> floor((h - M) / 525,600)
```

- H = 810,000 and M = 811,001.
- Corridor blocks 810,001..811,000 have zero subsidy and may claim fees only.
- Modern reward split is 90% producer / 10% treasury.
- Halving interval is 525,600 blocks, approximately one year at the 60-second
  target.
- Total scheduled lifetime emission approaches 2% of S_H.

## Treasury — RULED

The single pinned treasury address is:

```
SNyANHiUkuqPSfbeKHDXzVD86LC2ZUUjLX
```

Its mainnet script is:

```
76a91412602418ffc74640e37f1a73d0cdc255d2a07c3588ac
```

The single-key risk is accepted by the simplicity ruling. Release operators
must protect the key with strong offline or hardware-backed custody. Rotation
requires a reviewed release.

## Simple-v1 colored assets — RULED 2026-08-22/09-01

- One genesis transaction creates exactly the full declared fixed supply.
- No later mint authority or mint button exists.
- Asset identity is deterministic, chain-bound, and commits to the immutable
  genesis rules.
- Issuance fee is a flat **1,000 B3** paid to the pinned treasury script in a
  coinbase-independent output of the issuing transaction.
- The issuer also pays the ordinary native B3 network fee.
- Colored-asset outputs do not need native B3 attached; later transfers pay
  network fees using separate native B3 inputs.
- Asset issuance activates only at the post-M height pinned in the transition
  release (A2).

## Historical FN Genesis — RULED 2026-09-01

- The full canonical historical-rights manifest, count R, and Merkle root are
  measured and independently reproduced during the seal pause.
- Block 810,001 coinbase creates one amount-1 FN output per manifest row,
  directly to the exact historical legacy P2PKH owner commitment.
- Historical issuance has no holder claim, proof, deadline, disintegration
  payment, colored-asset issuance fee, or separate network fee.
- The outputs use ordinary 30-block coinbase maturity and no additional FN
  transfer lock.

Historical B3 destruction already occurred in 2017. FN Genesis recognizes the
result; it does not destroy or recreate native B3 at height 810,001.

## Modern FN creation — RULED 2026-08-28/09-01

Final modern capacity is:

```
5,000 - R
```

It is not a separately fixed 1,500. The prior through-height-807,709 run found
at least 3,500 historical rights, so the final capacity is expected to be no
more than 1,500.

Each modern PoD transaction creates exactly one FN and removes D from native B3
through the accounting gap:

```
native_input - native_output = D + ordinary_network_fee
```

D is never an output, treasury payment, or producer fee. Price depends on the
number M of modern FN already created:

| M before creation | Required destruction |
|---:|---:|
| 0..499 | 15,000 B3 |
| 500..999 | 30,000 B3 |
| 1,000 and above while `R + M < 5,000` | 60,000 B3 |

Retiring or extinguishing FN never reopens a lifetime slot. Historical FN does
not advance the modern price counter. Permissionless modern creation is
fail-closed before the separately pinned post-M height A1.

## FN transfers — RULED 2026-09-01

- Owner-script authorization is required over the complete spending
  transaction.
- Whole FN units are conserved across ordinary transfer.
- FN is indivisible in the decimal sense; whole units may be combined or split.
- Native B3 pays the ordinary transaction fee.
- Historical proof data and PoDIds never travel with transferred units.

## FlowMesh economics — RULED FOR THE TRANSITION RELEASE

FlowMesh v1 spot trading activates at A3 in the transition release. The fee is
100 ppm (0.01%) of matched native-B3 notional, charged once and split 80% equally
across every active FN seat and 20% to the same treasury address. Any indivisible
seat remainder is assigned in canonical SeatId order.

After every ordinary slot, the engine requests the deterministic maximal partial
treasury flush: the lesser of accrued treasury available and anchored native
withdrawal capacity remaining after existing pending native withdrawals, when
positive. It never waits for the full balance, and zero capacity never blocks
trading.

## Superseded economic notes

The following remain history only and must not be implemented:

- 10 B3 colored-asset issuance fee;
- burning the colored-asset issuance fee;
- 3% initial annual emission or four-year halvings;
- corridor subsidy;
- a hard-coded 1,500 modern-FN capacity independent of final R;
- a 4,000-byte proof fee or carrier gate for historical FN; and
- charging historical FN holders at Genesis.

## OPEN for later scope

- FN/FlowMesh reward amount and ownership cutoff, if rewards are adopted.
- Final allocation of the 80% FlowMesh fee share.
- Bridge mint/withdraw fee policy.
