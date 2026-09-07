# Finality recovery in B3 Hive 1.1.4

Recovery is available but disabled by default. Installing this wallet alone
does not recover an orphaned signer. This is an explicit operator-approved
exception, not an automatic reorg of the signer journal or a quorum proof.

## Commands

Run these in the wallet's Debug Console, or with `b3coin-cli` selecting the
correct wallet. Use `help <command>` for the built-in argument descriptions.

| Command | Purpose |
| --- | --- |
| `getfinalityrecoveryinfo` | Read the selected wallet's recovery observation and pending approval. Does not unlock, sign, or modify its journal. |
| `getfinalityinfo` | Inspect binding, epoch snapshot keys, finality and signing progress. |
| `getfinalitystatus` | Inspect locally verified votes and quorum progress. |
| `stopstaking` | Stop this node's staking loop before changing an approval. |
| `setfinalityrecovery <manifest-object> <accepted-anchor-hash>` | Approve one exact incident for the selected unlocked wallet. Configures memory only; does not recover or sign immediately. |
| `clearfinalityrecovery` | Clear pending approval while staking is stopped. Does not undo a completed recovery or reset the journal. |
| `startstaking` | Start the normal signer, which rechecks any configured recovery before applying it. |

The manifest is a JSON object, not a filename for the RPC. It has exactly ten
fields; use independently verified public data, never private keys:

| Field | Required value |
| --- | --- |
| `version` | JSON integer `1` |
| `chain_domain` | Exact 64-hex modern network domain |
| `validator_key` | This wallet's public 64-hex validator key, as displayed by the wallet |
| `incident_height` | Original signed checkpoint height, a nonnegative integer |
| `incident_block_hash` | Exact original signed block hash, 64 hex characters |
| `incident_epoch` | Original signed epoch, a nonnegative integer |
| `incident_signing_set_hash` | Original signing-set hash, 64 hex characters |
| `incident_successor_set_hash` | Original successor-set hash, 64 hex characters |
| `anchor_height` | Independently agreed, later scheduled checkpoint in the same incident epoch |
| `anchor_block_hash` | Exact agreed anchor block hash, 64 hex characters |

The second RPC argument must repeat the exact approved anchor hash. Unknown,
duplicate or missing fields, wrong types, null hashes, wrong wallet/network,
and a non-advancing anchor are rejected. The maximum manifest size is 16 KiB.
This document deliberately provides no preapproved mainnet manifest.

## Operator sequence

1. Read `getfinalityrecoveryinfo`, `getfinalityinfo` and `getstakinginfo`.
   Record the exact public incident. A missing local vote is not proof that
   the validator has an orphaned lock; unavailable observation is not a safety
   approval. Do not delete or replace the `finality_signer` directory.
2. Independently agree on the chain and scheduled recovery anchor. Review
   external bridge state and the possibility of retained old certificates.
   Twenty blocks of depth do not prove irreversibility.
3. Stop staking, unlock the selected wallet through its usual secure workflow,
   and call `setfinalityrecovery` with the approved public object and hash.
   The response confirms **configuration only**, not completed recovery.
4. Start staking normally. The signer requires an intact journal matching the
   exact validator, old vote, old lock, epoch and sets. The old checkpoint must
   be absent from the active chain; the anchor must match that chain and be at
   least 20 blocks deep (or the consensus depth if greater). Its incident epoch
   must be current or immediately previous, with its exact sets still retained.
   The anchor must belong to that same incident epoch; lineage must not be broken.
5. Verify that the lock moved forward and a later `last_signed_height`
   advances. New votes must be strictly above the anchor. Check peer acceptance
   and network finality separately; a local success is not network quorum.
   Clear the pending configuration with staking stopped before starting a
   subsequent session. Preserve the updated journal and relock spending if
   the chosen unlock workflow left it unlocked.

Approval is memory-only and is not a journal rewrite. Invalid requests preserve
the existing approval. The running signer consumes its approval when applied;
reusing the old incident must fail once its journal no longer matches. If the
approved anchor later becomes orphaned, signing stops again. There is no
recovery for missing/deleted/corrupt journals or unavailable historical keys.
Do not run two signers for the same validator identity.

## Startup alternative

An operator may instead supply both startup options exactly once:

```text
-finalityrecoveryfile=/absolute/path/to/approved-public-incident.json
-acceptfinalityrecovery=<exact-approved-anchor-block-hash>
```

The file contains the same public manifest. Relative paths use the network
data directory. It is read once, with no automatic download or live reload.
Remove both options after successful recovery before another staking session
or using an older wallet. RPC clearing does not edit startup configuration.

## Compatibility and risk

Normal V1 digests, wire messages, certificates, quorum, journal serialization
and the 12-block consensus depth are unchanged. Updated mainnet signers wait
20 blocks before creating new votes; valid older-wallet votes at consensus
depth remain accepted. The original compiled recovery retains its hardened
checkpoint requirement; operator recovery adds no new block-validity rule.

The original signed vote is retained when the lock advances. Copies of old
signatures elsewhere remain valid under V1 rules: this exception does not
revoke them or prove that an old conflicting certificate cannot exist.
Recovery is a coordinated trust decision and does not guarantee that an
existing Ethereum verifier will accept new anchors or reopen deposits.

The epoch-snapshot key-rotation fix is also included. It selects the BLS key
required by the checkpoint's snapshot, not simply the newest binding. Derived
historical keys can be reconstructed from the original validator secret;
overwritten imported secrets cannot. See
[snapshot-aware signing](finality-snapshot-key-selection.md).
