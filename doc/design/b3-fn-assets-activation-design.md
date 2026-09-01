# FN Coin + Colored Assets — Activation Design

Status: **OWNER-RATIFIED (through 2026-09-01)**. This is the governing activation
record for FN Coin and simple-v1 colored assets in the transition release. It
supersedes the historical claim/proof designs and any earlier schedule that put
FN Genesis after the modern-era soak.

Implementation state (2026-09-01): production consensus, mempool, miner,
serialization, index, and wallet-signing paths implement this design behind
fail-closed parameters. `getassetstate` reports the next-block activation and
branch-local modern-FN counter; `getwalletassets`, `issueasset`, `sendasset`,
`burnasset`, and `createfncoin` provide the transition-release wallet surface.
The seal-derived X, R0, manifest/count/root, and exact schedule are now pinned:
A1 = 812,000, A2 = 813,000, and A3 = 815,000. Final release review and the
real-history shadow-fork rehearsal remain release gates.

## 1. Release plan (at most two planned feature releases)

1. **Transition release.** During the seal pause it pins X, the measured
   `S_H` and derived `R0`, and the full canonical FN rights manifest, count,
   and Merkle root. It carries complete FN, simple-v1 colored-asset, and
   FlowMesh-v1 spot code.
   FN Genesis is mandatory in corridor block 810,001 and transfers use ordinary
   coinbase maturity. Permissionless modern FN PoD and asset issuance remain
   fail-closed until their separately pinned post-M heights A1 and A2. A2 also
   enables FlowMesh seat/vault preparation; A3 enables working spot trading
   after the preparation runway. The release ships only after the real-history
   shadow-fork rehearsal. The expected pause is 2–4 weeks and public messaging
   must say so plainly.
2. **FlowMesh expansion release.** Ships later, after a dedicated testnet on
   which real FN holders operate seats and produce the honest speed benchmark.
   It expands FlowMesh; it is not the first activation of spot trading.

Emergency security or correctness releases are not prohibited by this product
plan. Working FlowMesh spot trading ships behind A2/A3 gates in the transition
release.

## 2. Colored assets — simple v1

- One genesis transaction mints the entire fixed `max_supply`.
- No later mint or mint authority exists in v1.
- `AssetIdV1` is chain-bound and commits to the immutable genesis rules.
- The only v1 mode is `GENESIS_FIXED`; other mode values remain reserved.
- Issuance requires a flat **1,000 B3** payment to the pinned treasury script
  in a coinbase-independent output of the issuing transaction.
- Transfers conserve the colored asset; their ordinary network fee is paid in
  native B3.
- An explicit burn is a canonical B3A1 modern output with `PolicyType::BURN`.
  Asset and FN carriers never use `OP_RETURN`; policy alone gives the burn its
  semantics, and destroyed units never reopen issuance capacity.
- Issuance and non-FN colored outputs fail closed before the exact asset
  activation height A2 = 813,000. A1 = 812,000 and A2 are later than M, with
  A1 no later than A2.

## 3. FN ownership and transfers

Moving FN requires ordinary satisfaction of the owner script carried by the
spent FN output, under the normal transaction-signature rules. The successor
output carries the recipient's owner script. Multi-key scripts are permitted
when their normal script rules are satisfied.

FN has zero decimals. Transfers conserve whole FN units. The historical
genesis event and valid modern PoD transactions are the only creation paths.
An explicit extinguishment, if permitted, uses the same B3A1 `BURN` policy and
never reopens lifetime capacity.

There is no separate FN transfer-height lock. Because the historical outputs
are actual outputs of the 810,001 coinbase, the existing 30-block coinbase
maturity rule is their only initial delay. After maturity, the ordinary owner
signature rule applies.

## 4. Pinned historical-rights artifacts

At the seal, run the deterministic through-H report over the exact `(H, X)`
prefix. The historical-right predicate is:

- non-coinbase and non-coinstake;
- verified input/output gap at least `legacy::GetFNCollateral(height)`; and
- a 1-old-COIN byte-exact legacy P2PKH designation output, with the
  lowest-index matching output selected if several exist.

The consensus manifest records one row per right as raw PoDId plus the exact
20-byte key hash from its P2PKH designation, strictly sorted by raw PoDId bytes.
The published audit report additionally carries height, tier, transaction
position, disintegration amount, and designation-output index for diagnosis.
The transition release pins:

- the complete manifest bytes;
- the exact row count `R`; and
- the Merkle root over the canonical rows.

The chain-domain/height/version/count-bound leaf, node, and root construction is
normative in
[b3-legacy-fn-issuance-proposal.md](b3-legacy-fn-issuance-proposal.md) §3.

Independent clean runs must reproduce all three before the release is tagged.
The old 4,000-byte proof fit measurement is not an activation gate because no
holder carries a historical proof.

## 5. Historical FN Genesis — block 810,001

Block **810,001**, the first transition-PoW corridor block, is FN Genesis. Its
single coinbase creates every historical FN unit:

```
one manifest row -> one coinbase FN output
asset            -> global FnAssetId
amount           -> 1
owner script      -> exact historical P2PKH script
```

Reading only the coinbase's asset outputs must yield the exact FN sequence in
manifest order. Ordinary native fee-payout outputs may coexist. A miner may
choose coinbase nonce material and native fee destinations, but cannot omit,
insert another asset, reorder the FN sequence, aggregate, or redirect an FN
output. A block with a wrong sequence is invalid.

The ordinary coinbase txid plus each output's actual vout index gives every FN
output a normal outpoint. Standard coinbase connect/disconnect, maturity,
reindex, and wallet-rescan behavior applies. There is no synthetic claimant
state.

Historical FN Genesis has no holder transaction, claim, signature, proof,
deadline, or issuance fee. A dormant holder can import the historical wallet
later and discover the already-created FN output by rescanning block 810,001.

## 6. Modern FN creation

At and after the separately pinned post-M height **A1**, a modern PoD
transaction destroys native B3 via the accounting gap and creates exactly one
FN unit to the creator's owner script. Before A1, modern PoD creation fails
closed even though historical FN transfers are already possible after
coinbase maturity. The disintegration is never an output, miner fee, or
treasury payment. The ordinary fee is the native input/output gap remaining
after subtracting the required destruction.

Modern capacity is `5,000 - R`, where R is the pinned historical count. Prices
are 15,000 / 30,000 / 60,000 B3 for successive 500-modern-unit tiers; the
lifetime cap can make a later tier partly or wholly unreachable.

The canonical creation declaration is MPA type 6/version 1 with the exact
payload `modern_created_before_u32_be || fn_output_index_u32_be`. The count is
branch-local and advances in transaction order. Consensus subtracts the tier
destruction from the native input/output gap before calculating producer fees,
so the coinbase can claim only the remaining ordinary fee.

## 7. Shadow-fork rehearsal

Before tagging, copy the synced real-chain datadir and run 2–3 isolated clients
with different network magic and ports, localhost-only, an overridable test H,
and trivial corridor difficulty. Rehearse in chronological order:

```
seal and independent manifest reproduction
  -> mandatory FN Genesis coinbase at H+1
  -> rejection of a mutated genesis coinbase
  -> FN spend rejection before coinbase maturity
  -> FN transfer at maturity
  -> corridor completion and Modern PoS
  -> A1 modern FN PoD activation
  -> A2 colored-asset activation, 1,000 B3 treasury fee, and FlowMesh prep
  -> A3 FlowMesh spot trading after the required anchor depth
```

The rehearsal uses real history and is a transition-release gate.

## 8. Retired paths

Creation-action type 1 (`FnClaimActionV1`) and type 2
(`LegacyFnIssuanceActionV1`) retain their assigned identifiers but are dead and
semantically rejected. Their frozen decoders may remain only to guarantee that
old bytes never gain a new meaning. Neither activates at any height. The
funding-key claim, archival-builder broadcast, per-holder proof, claim deadline,
and `issued[pod_id]` models are superseded.
