# Legacy FN issuance — canonical manifest and corridor genesis

Status: **OWNER-RATIFIED DESIGN (2026-09-01)**. The filename is retained for
links, but this is no longer a proposal. It supersedes the per-holder claim and
proof-carrying issuance models formerly recorded here. Their wire identifiers
and history are preserved in §8.

## 1. Final model

Every qualifying legacy FN disintegration through `H = 810,000` creates one
historical FN right. During the seal pause, the transition release pins the
complete canonical rights manifest, row count `R`, and Merkle root. The
coinbase of block **810,001** creates one amount-1 FN output for every manifest
row, directly to that right's exact historical legacy P2PKH script commitment.

There are:

- no holder claims or holder-submitted historical issuance transactions;
- no claim signatures, deadlines, private queues, or duplicate-claim state;
- no holder-carried historical transaction or Merkle proof;
- no issuance fee for historical FN Genesis; and
- no miner or administrator choice over the FN recipients or ordering.

The root commits the fully published manifest. It is not enough to pin only a
root and later choose a list that matches it; the exact manifest bytes, count,
and root are all release inputs.

## 2. Historical-right predicate and owner

A legacy transaction creates one right exactly when all of the following are
true on the X-anchored prefix through H:

1. it is neither coinbase nor coinstake;
2. its verified input/output gap is at least
   `legacy::GetFNCollateral(pod_height)`; and
3. it contains an output of exactly `1 * legacy COIN` whose script is the
   byte-exact 25-byte legacy P2PKH form.

`PoDId` is the legacy transaction id. If several outputs satisfy item 3, the
lowest-index matching output is the historical owner designation. A burn with
no matching designation creates no FN right; there is no marker-address
heuristic, funding-input heuristic, operator list, or fallback address.

Input values and transaction positions come from verified sealed history and
replay/undo data. Current spent status, wallet state, labels, and optional
indexes never change the result.

## 3. Canonical rights manifest v1

The consensus manifest contains one minimal row per right:

```
uint256 pod_id                  # raw canonical uint256 bytes
bytes20 recipient_key_hash      # exact HASH160 from the P2PKH designation
```

Rows are strictly increasing by the 32 raw internal/serialized bytes of
`pod_id`, not human-facing reversed txid text. Duplicate PoDIds are invalid.
The 25-byte P2PKH owner script is reconstructed canonically from
`recipient_key_hash`. No aggregation is permitted: two PoDIds with the same
key hash remain two rows and become two amount-1 coinbase outputs.

The published audit report also records height, tier, disintegration amount,
transaction position, and designation-output index so independent reviewers can
diagnose derivation differences. Those audit fields are recomputed facts, not
additional consensus-manifest fields.

All manifest commitment integers are unsigned big-endian. Hash fields use raw
internal/serialization bytes. Define:

```
context = ModernChainDomain[32]
          || fn_genesis_height:u32_be       # 810001
          || manifest_version:u16_be        # 1
          || row_count:u32_be               # R

canonical_manifest_bytes = context
                           || row_0_pod_id[32] || row_0_key_hash[20]
                           || ...
                           || row_R-1_pod_id[32] || row_R-1_key_hash[20]

leaf_i = TaggedHash("B3/FN/GENESIS/LEAF/V1",
                    context || row_index:u32_be
                    || pod_id[32] || recipient_key_hash[20])

parent = TaggedHash("B3/FN/GENESIS/NODE/V1", left[32] || right[32])

FN_RIGHTS_ROOT = TaggedHash("B3/FN/GENESIS/ROOT/V1",
                            context || tree_root[32])
```

At an odd tree level the final hash is duplicated. Binding row index and count
removes duplicate-tail ambiguity. Separate leaf/node/root tags prevent a hash
from one level being reused at another. The manifest must be non-empty and R
must not exceed 5,000. The transition release pins the full row vector and root;
R is the vector's exact size and is additionally printed in the seal packet.
The implementation must pin root vectors for one, two, three, and larger row
sets before producing the real artifacts.

## 4. Seal-pause reproduction gate

The final artifacts are produced only after H and X are known. Before the
transition release is tagged:

1. freeze independent archival views exactly at `(H, X)`;
2. complete the mandatory three-way legacy UTXO equivalence check;
3. run the manifest builder from at least two clean data views;
4. require byte-identical manifests, equal R, and equal rights roots;
5. publish the manifest, file checksum, count, root, commands, chain identity,
   logs, and direct process exit statuses;
6. investigate any mismatch row by row; never resolve it by choosing a favored
   run; and
7. stop the release if `R > 5,000` or any independent result differs.

The transition source pins the full manifest bytes, R, and root. Altering any
of them after tagging is a consensus change. The former instruction to run the
final FN report after release adoption is superseded; this is a pre-tag gate
during the 2–4 week seal pause.

## 5. FN Genesis coinbase at 810,001

The first transition-PoW corridor block is mandatory FN Genesis. Its coinbase
must satisfy:

```
asset_outputs(coinbase) = the R expected FN outputs in manifest order
```

Here `asset_outputs` preserves coinbase output order while ignoring ordinary
native fee-payout outputs. For manifest row `i`, expected FN output `i` is:

```
asset              = FnAssetId(ModernChainDomain)
amount             = 1
policy_type        = FN
policy_version     = 1
policy_commitment  = SHA256(recipient_script)
policy_params      = empty
```

The output carries zero native B3. The block producer may choose normal
coinbase nonce material, legal native fee outputs, and their placement, but
cannot omit, insert, reorder relative to another FN output, aggregate, or
redirect an FN output. No non-FN asset output is allowed in a coinbase. A
mismatch makes block 810,001 invalid, and no later coinbase can repeat FN
Genesis.

These are ordinary outputs of the real coinbase transaction, so each outpoint
is its coinbase txid plus its actual vout index. They carry the existing
coinbase flag and therefore the existing 30-block coinbase maturity rule. There
is no additional transfer lock: an FN genesis output first becomes spendable
when ordinary coinbase maturity permits it.

Connect/disconnect, reindex, replay, and wallet rescan use normal coinbase
semantics. A holder importing the old P2PKH key later discovers the FN in block
810,001 without claiming or paying an issuance fee.

## 6. Authorization and movement

The initial commitment is `SHA256(exact_legacy_p2pkh_script)`. Spending reveals
that script and proves control under its normal key rules with a signature over
the complete modern transaction. Merely providing non-empty proof bytes is
never sufficient.

Successor FN outputs commit to their recipient owner scripts. FN transfers use
ordinary whole-unit asset conservation and pay transaction fees from separate
native B3 inputs. PoDId and historical evidence are not copied into successor
outputs.

## 7. Supply interaction

FN Genesis issues all historical units at once, so the historical issued and
live counts both become R at block 810,001. The lifetime cap is 5,000 and modern
capacity is exactly:

```
modern_capacity = 5,000 - R
```

Modern creation is specified in [b3-fn-pod.md](b3-fn-pod.md). A retired or
extinguished FN never reopens a lifetime slot.

## 8. Superseded designs and reserved identifiers

This history is non-normative for activation but remains explicit so old bytes
cannot be reinterpreted:

1. **Integrated PodDB and funding-key claim, action type 1.** Nodes derived
   claimant state and holders supplied fresh signatures. Superseded. Type 1
   version 1 remains reserved and must always fail semantic FN validation.
2. **Virtual per-right claim outpoints.** Proposed as anchors for later claims
   and rejected. The actual 810,001 coinbase is different: it creates final
   owned FN outputs immediately and has no claim phase.
3. **Archival-builder/stateless-proof issuance, action type 2.** A builder would
   broadcast one issuance transaction per right containing legacy transaction
   bytes and Merkle paths. Superseded on 2026-08-31. Type 2 version 1 remains
   reserved and must always fail semantic FN validation.

The 4,000-byte historical proof-carrier fit gate is therefore dead. A generic
action-size bound may remain for unrelated actions but has no bearing on FN
Genesis.

## 9. Mandatory tests

The transition release may not be tagged until tests demonstrate:

- canonical manifest encoding, strict decoding, ordering, duplicate rejection,
  count/root agreement, mutation sensitivity, and final-H reproduction;
- exact block-810,001 FN output count, order, shape, and commitments;
- one output per row even when owners repeat;
- rejection for one missing, inserted, reordered, aggregated, or redirected
  genesis output and rejection of genesis in every other block;
- ordinary 30-block coinbase maturity with no extra transfer-height lock;
- coinbase connect/disconnect, restart, reindex, replay, and wallet rescan;
- valid and invalid historical-owner signatures bound to the complete modern
  spend;
- FN conservation and native-B3 fee separation;
- lifetime cap and modern capacity `5,000 - R`; and
- permanent semantic rejection of retired type-1 and type-2 actions.
