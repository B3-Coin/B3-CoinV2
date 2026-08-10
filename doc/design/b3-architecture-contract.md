# B3Coin Core Architecture Contract

These decisions are locked. Do not reinterpret them.

## Chain identity

1. B3 remains one continuous blockchain.
2. The original B3 genesis block is permanent and must remain
   byte-for-byte and hash-for-hash unchanged.
3. There is no new genesis.
4. There is no balance migration to another chain.
5. There is no consensus-pinned UTXO snapshot.
6. Historical transaction IDs, outpoints, balances, scripts, staking
   rewards and block history remain part of the same B3 chain.

## Two eras

Define:

    H = final legacy block height
    X = exact final legacy block hash
    hard_fork_height = H + 1

Therefore:

    height <= H     => LEGACY era
    height >= H + 1 => MODERN era

Existing mainnet H and X are not configured until a later explicitly
approved task.

## Legacy era

The legacy chain must retain:

- historical transaction-level nTime;
- historical transaction IDs;
- historical block encoding;
- historical trailing PoS block signatures;
- historical B3 block hashing;
- compatibility with protocol-version 80008 peers;
- existing balances, scripts and outpoints;
- existing staking rewards through H.

Before the fork is finalized, live nodes may continue validating and
staking under the existing legacy rules.

After H and X are permanently fixed, a fresh node may reconstruct
legacy history through trusted replay:

- download every legacy block;
- parse safely;
- verify previous-hash linkage;
- verify checkpoint hashes;
- verify transaction Merkle roots;
- mechanically remove spent UTXOs and create new UTXOs;
- preserve values, scripts, outpoints, heights and maturity-relevant
  classifications.

Trusted historical replay skips:

- historical script/signature validation;
- historical Proof of Work;
- historical PoS kernel validation;
- stake-modifier validation;
- historical reward validation;
- historical difficulty validation;
- historical timestamp/MTP validation;
- legacy chainwork competition.

The node still rejects malformed or internally inconsistent downloaded
data, such as broken linkage, wrong checkpoints, invalid Merkle roots,
missing prevouts, duplicate spends and arithmetic overflow.

## Modern era

Starting at H+1:

- use clean Bitcoin Core 31.1-style transaction serialization;
- no transaction-level nTime;
- no trailing legacy block signature;
- use the post-fork B3 block codec marker;
- use full modern validation;
- use modern B3 PoS;
- use modern networking capabilities;
- permanently reject reorganizations that disconnect H or anything
  below H.

## Block format marker

The existing reserved marker is:

    0x28000000

The marker determines, before parent height is known:

- block codec;
- transaction codec used inside the block;
- presence or absence of trailing block signature;
- block-header hash domain/algorithm.

Connected height determines whether that codec is legal:

    LEGACY codec at height <= H      allowed
    MODERN codec at height <= H      reject
    MODERN codec at height >= H+1    allowed
    LEGACY codec at height >= H+1    reject

Never use an assumed height of zero to determine the hash identity of
an unknown-parent block.

## Standalone transactions

Transactions outside a block have no enclosing block marker.

Their codec must be selected explicitly by trusted context:

- historical legacy importer: LEGACY;
- legacy peer before activation: LEGACY where explicitly supported;
- modern wallet: MODERN;
- modern RPC: MODERN;
- modern mempool: MODERN;
- modern P2P relay: MODERN.

Do not use a process-global legacy_b3coin Boolean as the transaction
codec selector.

## Pre-H UTXOs after the fork

A modern transaction may spend a pre-H UTXO.

The prevout remains:

    old legacy txid + output index

The old locking script remains enforceable when that output is
eventually spent.

Historical blocks themselves are not revalidated merely because an
old output is spent after H.

## Future asset and DEX architecture

Modern B3 must later support:

- coloured assets;
- typed Policy Outputs;
- typed segregated TransitionProofs;
- DEX vault custody;
- staking policies;
- bridge policies;
- optional Proof-of-Work asset issuance.

Do not implement these unless the current commit explicitly requests
them.

The future generic model is conceptually:

    ModernInput {
        OutPoint prevout;
        Sequence sequence;
        ProofRef proof;
    }

    ModernOutput {
        AssetId asset;
        Amount amount;
        PolicyType policy_type;
        PolicyVersion policy_version;
        PolicyCommitment commitment;
    }

The prevout identifies where the coin came from.

The TransitionProof establishes why the proposed state transition is
authorized.

## DEX architecture

DEX fills are never UTXO spends.

The custody boundary is:

    UTXO deposit
        -> FlowMesh internal balances and trading
        -> UTXO withdrawal

FlowMesh will maintain:

- internal per-asset balances;
- reservations;
- positions;
- persistent demand curves;
- deterministic batch clearing;
- settlement;
- withdrawal receipts.

A future DEX_VAULT output:

- has no controlling private key;
- may be spent only by a finalized withdrawal receipt;
- permits anyone to relay the exact authorized withdrawal;
- fixes asset, amount, destination and receipt ID;
- supports partial withdrawals;
- forces all remaining value back into approved vault outputs;
- enforces exact conservation;
- consumes each receipt once;
- supports sharded vault outputs for parallel withdrawals.

Do not implement fills as UTXO spends.
Do not create one UTXO per order.
Do not add DEX-specific fields directly to generic transaction inputs.
Do not introduce a generic smart-contract VM.

## Genesis safety

No task may:

- regenerate genesis;
- change genesis transaction bytes;
- change genesis nTime;
- change genesis nonce;
- change genesis bits;
- change genesis Merkle root;
- change genesis hash;
- apply the modern marker to genesis;
- reinterpret historical blocks using the modern codec.

If a task appears to require any of these, stop and report instead.
