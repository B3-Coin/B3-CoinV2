# FN Coin and Proof of Disintegration

Status: **OWNER-RATIFIED DESIGN (2026-09-01)**. This is the governing FN
lifecycle and economics record. Historical issuance is defined in
[b3-legacy-fn-issuance-proposal.md](b3-legacy-fn-issuance-proposal.md), and
release timing in
[b3-fn-assets-activation-design.md](b3-fn-assets-activation-design.md).

## 1. FN identity

FN is one global, chain-scoped modern asset:

- `FnAssetId = TaggedHash("B3/FN/ASSET/V1", ModernChainDomain)`;
- decimals = 0;
- lifetime issuance cap = 5,000 units;
- every output contains a positive whole-unit amount;
- FN is not native B3 and carries no required native B3 amount; and
- FN transaction fees are paid separately in native B3.

FN is fungible after issuance. A historical PoDId establishes why one row
exists in the pinned genesis manifest, but is not part of the FN asset id or a
successor output. Whole units may move, combine, or split under asset
conservation; fractional FN cannot exist.

## 2. Two creation paths only

Exactly two consensus paths create FN:

1. **Historical FN Genesis.** The coinbase of block 810,001 creates one
   amount-1 output for every row of the pinned legacy-rights manifest.
2. **Modern PoD.** A user transaction permanently destroys the required native
   B3 and creates exactly one amount-1 FN output to the creator's selected
   owner script.

Generic colored-asset genesis cannot create `FnAssetId`. There is no authority
mint, holder claim, historical proof transaction, reward mint, or replacement
mint. Unknown creation forms are invalid.

## 3. Historical FN Genesis

At height 810,001:

- the transition release has already pinned the complete canonical manifest,
  count R, and rights root;
- the corridor coinbase contains one separate amount-1 FN output per row in
  exact manifest order;
- each initial owner commitment is the SHA-256 of the row's exact legacy P2PKH
  script;
- repeated owner scripts are not aggregated; and
- the block producer cannot omit, reorder, or redirect any FN output.

There is no holder transaction, claim, proof, signature, deadline, or fee for
historical creation. The outputs are real coinbase outputs and use only the
ordinary 30-block coinbase maturity rule. No separate FN transfer lock exists.

Historical owners already paid by disintegrating B3 in the legacy era. They do
not pay the modern PoD cost or the colored-asset issuance fee.

## 4. Modern FN creation

Modern creation preserves Proof of Disintegration as a distinct consensus
operation. It is not generic `BURN`, generic asset issuance, or a payment to the
block producer. It fails closed before the separately pinned post-M height A1;
historical FN transfers after coinbase maturity do not activate modern creation.

For one recognized modern FN PoD transaction:

```
I = sum of native-B3 inputs
O = sum of native-B3 outputs
D = RequiredDisintegration(modern_created_before_this_transaction)

require I >= O
gap = I - O
require gap >= D
ordinary_fee = gap - D
```

All evaluation uses overflow-safe integer base-unit arithmetic. D is removed
from spendable B3 through the accounting gap; it is never serialized as an
output, added to UTXO state, paid to treasury, or claimable by the producer.
Only `ordinary_fee` is a transaction fee.

A valid modern PoD transaction:

- has exactly one modern-FN creation declaration;
- creates exactly one amount-1 `FnAssetId` output to the creator's selected
  owner script;
- increments the modern-created-ever counter exactly once;
- cannot create any other FN surplus; and
- is invalid when lifetime capacity is exhausted.

The declaration is MPA type 6, version 1. Its payload is exactly eight bytes:
`modern_created_before_u32_be || fn_output_index_u32_be`. The named output is
an amount-1 FN owner output. Binding the branch-local slot makes the price
unambiguous and ensures a transaction prepared for an earlier slot cannot be
silently reinterpreted after another creation confirms. Types 1 and 2 remain
retired historical formats and never validate as substitutes.

Several creation transactions in one block use canonical transaction order.
The counter advances after each successful creation before the next one is
evaluated.

## 5. Lifetime cap and counters

Let:

```
R = final historical manifest row count pinned by the transition release
M = modern FN units ever created on the active chain
C = 5,000
```

Consensus maintains:

```
R + M <= C
modern_capacity_remaining = C - R - M
```

Each block index entry caches M for its own ancestry. A versioned per-block
sidecar persists that value without changing the existing block-index record
format, so restart, competing branches, reorganization, pruning, and
`-reindex-chainstate` retain deterministic state. Missing state before A1 means
zero; missing state at or after A1 fails closed. B3 AssumeUTXO bases at or
after A1 remain disabled until trusted snapshot metadata commits this counter.

All R historical units are issued in block 810,001; there is no unclaimed
historical balance. Modern capacity is therefore exactly `5,000 - R`, not a
separate hard-coded 1,500. The prior height-807,709 run found a floor of 3,500,
so final modern capacity is expected to be at most 1,500. If final R exceeds
5,000, the transition release stops for a new owner decision.

An explicit FN extinguishment may reduce live supply, but never decreases R or
M and never reopens a creation slot. Ordinary transfer cannot silently erase
FN value.

## 6. Modern disintegration price

The price is a nondecreasing function of M, the number of modern units already
created before the candidate transaction:

| M before creation | Modern slot | Required destruction |
|---:|---:|---:|
| 0..499 | 1..500 | 15,000 B3 |
| 500..999 | 501..1,000 | 30,000 B3 |
| 1,000 and above, while `R + M < 5,000` | remaining reachable slots | 60,000 B3 |

The final R can make a tier partly or wholly unreachable. The cap check always
runs independently of price lookup. Destroying extra B3 creates no extra FN;
value beyond D is ordinary fee unless returned as native B3 change.

## 7. Ownership and transfer

Every FN spend requires valid authorization from the spent output's committed
owner script over the complete modern transaction. A merely non-empty proof is
not authorization.

For a historical genesis output, the owner reveals the exact committed legacy
P2PKH script and a public key whose hash matches it, then supplies a valid
signature. A successor commits to the recipient's supported owner script.
Normal multi-key scripts work under their ordinary authorization rules.

FN transfer follows the colored-asset conservation model:

- FN input units equal FN output units unless an explicit extinguishment rule
  is used;
- amounts are whole numbers;
- whole units may be combined or split;
- PoDId and historical evidence do not travel with units; and
- ordinary network fees come from separate native B3 value.

Wallets must exclude FN from native-B3 coin selection. They must discover
historical outputs when an old key is imported and block 810,001 is rescanned.

## 8. Activation and release ordering

```
seal H/X
  -> reproduce and pin X, R0, and FN manifest/count/root
  -> shadow-fork rehearsal during the seal pause
  -> transition release
  -> 810,001 corridor coinbase: FN Genesis
  -> FN transfer once ordinary coinbase maturity is reached
  -> Modern PoS at M = 811,001
  -> soak period
  -> A1: permissionless modern FN PoD creation activates
  -> A2: simple-v1 colored assets plus FlowMesh seat/vault preparation activate
  -> A3: working FlowMesh spot trading activates after the preparation runway
  -> later FlowMesh expansion testnet/release
```

FlowMesh v1 spot trading is part of the transition release. FN ownership alone
is not an operating seat: the holder must create an FN-v2 seat binding at or
after A2. Trading begins at A3, and each market waits until its unique earliest
eligible epoch-zero anchor is 30 blocks deep.

## 9. Retired mechanisms

These remain design history only:

- production PodDB plus funding-key holder claims;
- virtual per-right claim anchors;
- action type 1 (`FnClaimActionV1`);
- action type 2 (`LegacyFnIssuanceActionV1`);
- holder-carried funding transactions and Merkle paths;
- the 4,000-byte historical proof-carrier gate;
- `issued[pod_id]` claim state and perpetual unclaimed reservations; and
- same-PoDId successor or per-unit provenance rules.

Types 1 and 2 keep their assigned identifiers and frozen historical decoders so
old bytes never gain a new meaning. All semantic FN validation rejects them at
every height.

## 10. Mandatory consensus tests

Before the transition release tag, tests cover:

- pre-810,001 FN creation rejection and exact genesis-height behavior;
- complete manifest/count/root consistency and mutation rejection;
- exact coinbase output set, order, count, values, and commitments;
- duplicate owners producing separate outputs;
- ordinary coinbase maturity and absence of another transfer lock;
- genesis disconnect/reconnect, restart, reindex, replay, and wallet rescan;
- historical-P2PKH and multi-key owner authorization over the full transaction;
- whole-unit conservation, combine/split, and native-fee separation;
- exact modern capacity `5,000 - R` and no reopening after extinguishment;
- 499/500 and 999/1,000 price boundaries and several creations in one block;
- destruction-versus-fee accounting and overflow rejection;
- generic asset issuance unable to create `FnAssetId`; and
- permanent rejection of retired action types 1 and 2.
