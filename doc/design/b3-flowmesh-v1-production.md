# B3 FlowMesh v1 production wiring

Status: production design for the v1.1 release. This document records the
owner ruling of 2026-08-31 and is the implementation contract for this branch.
Where an older document differs, this document wins.

## 1. Shipped scope and non-goals

FlowMesh v1 ships in the transition release as a B3-dependent but
B3-fail-independent spot DEX:

- every market is either one activated simple-v1 asset or the one explicitly
  registered bridge-backed bUSD asset against native B3; B3 is always quote;
- one active FN seat has one vote and one turn in the proposer schedule;
- every active seat is in the committee, ordered canonically;
- a certificate needs `floor(2*k/3) + 1` seats and `k >= 4`;
- dedicated FlowMesh messages and priority queues run over existing B3 P2P
  connections; v1 opens no separate port or network;
- the trade fee is 100 ppm (0.01%) of matched B3 notional, split 80% to
  all seats and 20% to the B3 treasury;
- FlowMesh activates at the post-M height named
  `flowmesh_activation_height` (the release name for A3).

Futures, leverage, native CDP/oracle bUSD, generic bridges, asset/asset
markets, governance, slashing, and a generic policy VM are not v1. The
production exception is the exact Ethereum-mainnet USDT vault registration
that may mint bridge-backed bUSD 1:1 only after its independent bridge gates
pass. The bounded type-10 bootstrap/update/mint/backfill/managed-withdrawal
carrier, exact OWNER mint, exact bUSD BURN request, nullifier/caps,
undo/reindex replay, and mempool/miner/asset integration are implemented.
Every type-10 record has one exact zero-value policy-9 `BRIDGE_RECORD`
metadata output, so standard `SIGHASH_ALL` binds its canonical bytes through
ordinary outputs without `OP_RETURN` or a custom sighash. Bridge state is
rebuilt in memory from activation and configured bridge nodes refuse pruning
because no durable sidecar exists. The origin-enforced USDT adapter and
operator release/consumption service are not implemented. The reserved
`SPOT_TO_FUTURES` and `FUTURES_TO_SPOT` action numbers remain rejected.

FlowMesh may pause or permanently safe-halt without stopping block download,
mempool admission, mining, staking, finality, or validation of ordinary B3
transactions. B3 calls FlowMesh verification only for an optional FlowMesh
checkpoint or a transaction spending a DEX vault output. There is no required
FlowMesh object per B3 block.

## 2. Production implementation state

The transition branch contains the activation-gated production path:

- deterministic spot clearing, BIP340 user-action authorization, exact fee
  allocation, custody accounting, and bounded action admission;
- BLS FN-seat proposals, attestations, certificates, anchored epoch handoff,
  and the permanent per-sequence signing lock;
- A2 seat binding and vault-history preparation, A3 checkpoint/vault effects,
  nullifiers, typed MPA proofs, and deterministic withdrawal publication;
- `FlowMeshProductionStore` atomic persistence, authenticated replay, restart
  recovery, and corruption fail-close;
- dedicated bounded FlowMesh messages over the existing B3 P2P connections,
  plus the production runtime, service, wallet, and RPC surfaces; and
- B3A1 simple-asset, FN, and DEX_VAULT-v2 outputs with MPA types 7/8/9.

The earlier BIP340 static-committee spike, synthetic key, single-seat quorum,
and `-b3flowmeshdev` path are historical development scaffolding, not release
behavior. Mainnet remains fail-closed until the release pins, rehearsals, and
independent bridge gates in §14 are complete.

## 3. Activation and naming

Use semantic consensus fields, not overloaded ordinal names:

```
fn_pod_activation_height         // A1: post-genesis modern FN PoD
asset_activation_height          // A2: assets + FN-v2 seats + vault preparation
flowmesh_activation_height       // A3: trading, checkpoints, and vault effects
busd_bridge.activation_height    // separate bridge gate, >= A3; unset fails closed
busd_bridge.withdrawal_mode      // MANAGED_V1 only when explicitly pinned
```

Both bridge fields participate in the complete fail-closed bridge parameter
envelope. Reaching the height is insufficient unless every required identity,
light-client, fork, cap, adapter, authority, runtime, and rules pin is present.

The former document convention `A2 = FlowMesh, A3 = bridge` is superseded.
Bridge-gating comments and checks must name the explicit bridge activation
field; A3 means FlowMesh and does not implicitly activate bridge minting or
withdrawals.

Historical FN Genesis and ordinary FN-v1 ownership/transfer are already active
from H+1. They are not delayed to A1. A1 activates permissionless modern FN
PoD. A2 activates simple-v1 assets and lets FN holders pre-bind FN-v2 seats.
A3 activates the complete usable FlowMesh product: trading, deposits,
checkpoints, sweeps, and withdrawals. Preserve these predicates and ordering:

```
FnRulesActive(height, params)        // configured FN Genesis; height >= H+1
FnPodRulesActive(height, params)     // A1
AssetRulesActive(height, params)     // A2, with A1 <= A2
FlowMeshSeatBindingRulesActive(height, params) // A2, but only with valid A3 runway
FlowMeshRulesActive(height, params)  // full service at A3
```

Mainnet parameters must also satisfy:

```
M < fn_pod_activation_height <= asset_activation_height
asset_activation_height + FLOWMESH_ANCHOR_DEPTH <= flowmesh_activation_height
```

A node option may disable the local FlowMesh worker or relay, but never change
transaction or block validity. Before their gates, every B3A1/MPA claim is
rejected rather than ignored. FN-v2/type-7 seat binding and DEX_VAULT-v2
output creation/history activate at A2 only when the complete A3 schedule and
30-block runway are configured. MPA types 8/9 and vault spends/effects remain
invalid until A3.

For each market, the unique epoch-zero anchor is the earliest canonical block
at or after `market.created_height` whose post-block active FN-v2 set contains
at least four seats. Sequence zero may be certified only at or after A3 and
once that exact anchor is at least `FLOWMESH_ANCHOR_DEPTH` (30) blocks deep.
If no such deep anchor exists, FlowMesh reports
`PAUSED_INSUFFICIENT_SEATS`; B3 continues normally. User deposit RPCs refuse
while paused, although consensus does not pretend a keyless vault is
withdrawable without a committee.

The exact absolute mainnet A3 height is deliberately not guessed here. It is a
release pin and must be set in chain parameters before the release candidate.

## 4. B3A1 carrier and UTXO rules

Preserve the implemented non-`OP_RETURN` B3A1 codec exactly. Spendable
non-native assets and the A2 DEX_VAULT extension use:

```
minimal-push(
    "B3A1" || asset_id[32] || amount_u64_be
    || policy_type_u16_be || policy_version_u16_be
    || policy_params[0..80]
)
OP_DROP
spend_suffix
```

The payload is 48 fixed header bytes followed by the remaining push bytes as
policy parameters. It has no carrier version, no CompactSize field, and no
serialized policy commitment. The payload must decode exactly, use a minimal
push, have at most 80 parameter bytes, and pass contextual policy validation.
A script claiming the B3A1 magic but violating this grammar is invalid, never
an ordinary script. A legacy coin whose bytes coincidentally claim B3A1 remains
ordinary native B3 according to its creation-height namespace.

Ordinary native B3 remains an ordinary `CTxOut` and is never wrapped merely to
use B3A1. Existing non-native OWNER/FN/BURN carriers have `nValue == 0` and a
positive encoded asset amount. From A2, DEX_VAULT-v2 extends the same codec:
a non-native vault output keeps `nValue == 0`; a native-B3 vault output has
`asset_id == NativeAsset()`, `nValue == encoded amount`, and is otherwise
rejected outside that exact policy/gate. OWNER and FN derive
`policy_commitment = SHA256(the exact owner suffix)`; the commitment is not on
the wire. BURN uses exact `OP_FALSE` and the null commitment. DEX_VAULT uses
exact `OP_FALSE`; its canonical v2 parameter bytes begin with the 32-byte
VaultId, which becomes the parsed policy commitment. This is the one keyless
policy that carries its identity in policy parameters because it has no owner
suffix from which an identity could be derived.

UTXO classification is by the successfully decoded policy, not merely by the
suffix and not by `CScript::IsUnspendable()`:

- OWNER, FN, and DEX_VAULT enter the UTXO set;
- BURN never enters the UTXO set and is counted as an explicit burn;
- existing B3 metadata cells remain excluded under their own grammar;
- a DEX_VAULT output must never be mistaken for BURN merely because both end
  in `OP_FALSE`.

This distinction must be identical in ConnectBlock, DisconnectBlock,
`AddCoins`, reindex, assume-UTXO/snapshot handling, mempool input views, and
wallet coin discovery.

## 5. FN v2 seat encoding and lifecycle

FN v1 remains an ordinary, unbound FN holding and has empty parameters. It is
not a committee seat. Production seats use append-only `FN policy v2`:

```
asset              = FnAssetId(chain_domain)
amount             = exactly 1
policy_type        = FN (5)
policy_version     = 2
policy_commitment  = SHA256(owner spend suffix)
policy_params      = bls_pubkey[48]
```

There is no separate x-only operator key. The owner script controls the FN
coin; the BLS private key controls only FlowMesh proposal, attestation, and
seat-reward signing. Spending the output terminates that seat. A transfer may
create a new FN-v2 output with a new key, reuse the same key atomically, or
create FN v1 and thereby end the seat. FN conservation remains exact and
issued-ever remains capped at `MAX_FN_EVER_ISSUED == 5000`; extinguishment
does not reopen issuance capacity. Every historical PoD still authorizes
exactly one FN unit and the final through-H rights count must fit the cap.

MPA numbers are frozen and append-only. Type 6 is already the modern FN PoD
record and must not be reused:

```
7  FLOWMESH_SEAT_BINDING v1
8  FLOWMESH_CHECKPOINT   v1
9  FLOWMESH_VAULT_PROOF  v1
```

A transaction creating an FN-v2 output must contain exactly one matching
type-7 record. The 48-byte BLS key is already in the referenced output's
policy parameters and is not duplicated:

```
vout_index_u32_be || bls_proof_of_possession[96]
```

The payload is exactly 100 bytes. The referenced output's key must decode canonically,
must pass the repository's BLS proof-of-possession verifier, and may not be
held by another live FN-v2 output. Connect applies spends before creations so
an atomic rotation may reuse a key. The live-key reverse index and all seat
changes have block undo and reindex tests. Type 7 has the same declared
verification cost class as existing BLS PoP binding evidence.

For an anchored output `O`:

```
SeatId(O) = TaggedHash("B3/FLOWMESH/SEAT/V1",
                       ModernChainDomain || O.outpoint)
member    = (SeatId, outpoint, bls_pubkey)
```

The active set at anchor `R` is every unspent, PoP-verified FN-v2 output at
`R`, sorted by `SeatId` and then outpoint. The signer bitmap uses this exact
order. `FnSeatIndex` must retain per-block deltas/undo for at least the anchor
window and cache immutable snapshots by anchor hash. Duplicate BLS keys are a
consensus error at creation, not deduplicated at snapshot time.

Operationally, a spend ends signing power when a 30-deep anchor first includes
the spend. A local owner should stop signing as soon as the spend confirms;
the anchor delay is the maximum protocol recognition delay, not permission to
double-operate.

## 6. Markets and execution domain

The smallest safe reuse of the current one-book engine is one independent
FlowMesh log per approved base-asset/B3 market:

```
MarketId = TaggedHash("B3/FLOWMESH/MARKET/V1",
                      ModernChainDomain || base_asset || NativeAsset())
VaultId  = TaggedHash("B3/FLOWMESH/VAULT/V1",
                      ModernChainDomain || MarketId)
```

The base must resolve at the microblock anchor either to an activated
simple-v1 genesis-fixed asset or to the exact production bUSD `AssetId` bound
to Ethereum chain id 1, vault
`0x143F207e23e6aebD7E974be90ac6D434f4c7BFb6`, canonical USDT
`0xdAC17F958D2ee523a2206206994597C13D831ec7`, and 6-decimal exact
conversion. The quote must be native B3. FN, BURN, a second native asset,
unknown modes, asset/asset pairs, ticker-selected assets, and every unregistered
bridge asset are rejected. A B3 balance deposited to one market cannot be
spent in another market.

Every active seat serves every live market. The first A2-or-later colored
`USER_DEPOSIT` for a VaultId establishes the market and its
`market.created_height`; off-chain packets alone cannot create one. Trading
starts only at or after A3 when the market's unique epoch-zero anchor is 30
blocks deep. Nodes instantiate its state/store lazily. Creation rate is
therefore bounded by B3 block space, fees, asset issuance, and UTXO growth.
Empty markets may be archived only after custody, balances, orders, pending
effects, and checkpoint obligations are all zero; certified history needed
for proofs is never silently deleted.

Production fixes `max_curve_points = 8`, `MAX_MICROBLOCK_ACTIONS = 1024`, and
`MAX_MICROBLOCK_BYTES = 2 MiB`; both count and byte limits are checked before
allocation or cryptography.

## 7. Epochs, proposer schedule, and certificates

An epoch binds one market to one anchored seat set. Its commitment includes
the market, epoch number, anchor, ordered members, `k`, and threshold. The
threshold is exactly `floor(2*k/3)+1`; no configuration flag may weaken it.

The proposer for `(sequence, round)` is:

```
members[(sequence + round) mod k]
```

using the SeatId order above. Proposal envelopes and attestations are BLS
signed by the selected seat; user actions remain BIP340. The proposal round
stays outside microblock identity, so a later proposer can re-propose the same
locked candidate. Durable write-ahead locks remain permanent for a sequence.
There is no v1 unlock/view-change rule: a split lock can halt FlowMesh, but it
cannot fork B3.

The production microblock core adds `market_id`, `epoch`, `seat_set_hash`, and
an entry kind (`EXECUTION` or `EPOCH_HANDOFF`). Anchors are monotonic,
canonical, and at least 30 blocks deep. A validator refuses to sign if a newer
30-deep anchor has a different active seat set from the epoch. A handoff has
no user actions and commits the next anchor and next seat-set hash.

A production certificate is:

```
epoch_u64 || sequence_u64 || microblock_hash[32]
|| signer_bitmap[ceil(k/8)] || aggregate_bls_signature[96]
```

Bits are LSB-first, the width is exact, high bits are zero, and set bits must
number at least the threshold. The digest is a tagged hash over chain domain,
market, epoch, seat-set hash, sequence, and microblock hash. Verification does
cheap framing, epoch, anchor, membership, bitmap, and threshold checks before
one `FastAggregateVerify`. PoP-verified decoded member keys and aggregate-key
work are cached by `(anchor_hash, seat_set_hash)`; repeated certificate
digests and signer bitmaps are cached with bounded LRU storage. Type-8 records
have a declared cost of 6,000 units, limiting them to two per transaction and
20 per block under the existing 12,000/120,000 budgets.

When the 30-deep active set changes, the outgoing committee certifies one
`EPOCH_HANDOFF` to the exact new anchored set. The handoff is posted as a
type-8 checkpoint and that publication must itself become 30 blocks deep
before the new committee signs an execution entry. Until then the outgoing
marker remains authoritative. A shallow reorg can therefore remove and
republish the byte-identical handoff without rolling back any incoming entry.
This gives B3 a single monotonic committee chain and prevents a stale committee
from authorizing later withdrawals. If the old threshold cannot hand off, or
the new set has fewer than four seats, FlowMesh pauses fail-closed. It never
invents members or falls back to a smaller threshold.

A reorg that removes any committed FlowMesh anchor causes a FlowMesh
`ANCHOR_INVALIDATED` halt. B3 reorg handling independently undoes its seat,
checkpoint, vault-effect, and nullifier indexes. FlowMesh never auto-rolls
back a certified log across such an event; recovery requires a separately
versioned, owner-approved procedure.

## 8. Fee accounting

Every cleared slot computes total matched quote notional in widened integer
arithmetic and then:

```
fee_total    = floor(matched_b3_notional * 100 / 1,000,000)
treasury_fee = floor(fee_total * 20 / 100)
seat_fee     = fee_total - treasury_fee
```

The fee is withheld once from sellers' aggregate B3 proceeds; buyers pay the
uniform-price notional, so the protocol does not accidentally charge 0.01%
per side. The total seller fee is allocated across seller fills by largest
remainder with AccountId as the final tie-break. No maker/taker distinction or
rebate exists in v1.

`seat_fee` is divided equally among every seat in the epoch: quotient to each,
then one extra base unit to the first remainder seats in SeatId order.
Rewards accrue to an internal account derived from `(market, epoch, SeatId,
bls_pubkey)`. A new `CLAIM_SEAT_REWARD` action is authorized by that BLS key,
so rewards remain claimable after the FN output is spent. Treasury fees accrue
to a reserved account whose only allowed withdrawal destination is the pinned
B3 treasury commitment. After every ordinary execution slot, the engine
creates the deterministic maximal partial treasury withdrawal

```
min(accrued treasury available,
    anchored native withdrawal capacity - existing pending native withdrawals)
```

when that value is positive. It does not wait for the full treasury balance;
zero capacity creates no request and never blocks trading. Fees are liabilities
inside the same B3 vault and are covered by the existing custody invariant.

## 9. Deposits, checkpoints, and withdrawals

### 9.1 Vault outputs

DEX_VAULT remains keyless and uses policy v2. The owner-ratified wire
parameters begin with the exact VaultId; this is an extension of policy
parsing, not a new carrier:

```
USER_DEPOSIT:     vault_id[32] || kind=1 || shard_u16_le || flowmesh_account_id[32]
VAULT_POOL_CHANGE vault_id[32] || kind=2 || shard_u16_le
```

The exact wire sizes are therefore 67 bytes for `USER_DEPOSIT` and 35 bytes
for `VAULT_POOL_CHANGE`, both below B3A1's frozen 80-byte parameter bound.
The parsed `ModernOutput` exposes `policy_commitment = VaultId` and the
remaining `{kind, shard, optional account}` as the semantic policy parameters.
There are 256 shards. A user deposit shard is
`low16(TaggedHash("B3/FLOWMESH/SHARD/V1", VaultId || account)) mod 256`;
any other shard is invalid. Asset and amount come only from the decoded B3A1
output. A deposit action names only its outpoint. `ChainDepositVerifier`
answers against a block-indexed vault history as of the microblock anchor,
not the caller's current UTXO view, and checks creation at or after A2,
canonical ancestry, market, kind, shard, account, amount, and unspent-at-anchor
state. The FlowMesh consumed-deposit set prevents double credit.

### 9.2 Deposit sweep

A `USER_DEPOSIT` output may not fund a withdrawal. Without this rule an
uncredited deposit could be consumed for somebody else's receipt and lose its
beneficiary when the remainder becomes pool change.

After the deposit action is certified, its typed deposit-acceptance effect is
included in a checkpoint. A permissionless B3 transaction uses a type-9
`DEPOSIT_SWEEP` proof to spend that exact USER_DEPOSIT output and recreate the
same asset and amount as `VAULT_POOL_CHANGE` under the same VaultId and shard.
No owner payout is allowed and the transaction fee comes from separate native
B3 inputs. Only pool-change outputs may fund withdrawals.

### 9.3 Checkpoint and effect commitment

Type 8 carries a bounded `CheckpointCore` plus the certificate:

```
version, domain, market_id, epoch, sequence, microblock_hash,
previous_checkpoint_id, anchor, seat_set_hash,
state_root, effect_start, effect_count, effect_root,
[next_epoch, next_anchor, next_seat_set_hash for handoff],
signer_bitmap, aggregate_signature
```

`CheckpointId` is a tagged hash of the core. Checkpoints are permissionless
MPA records paid for by ordinary transaction fees and committed by the
existing coinbase MPA root; no new metadata cell is needed. ConnectBlock
verifies activation, the previous checkpoint head, increasing sequence,
anchor ancestry/depth, the appropriate anchored seat snapshot, exact quorum,
and BLS signature, then updates a per-market checkpoint index with undo.
Conflicting or stale heads are invalid.

Effects are append-only typed leaves. One checkpoint covers at most 4,096
consecutive effects, using a canonical padded binary Merkle tree and carrying
the exact start/count. When 4,096 uncheckpointed effects exist, further
deposit and withdrawal actions pause until a checkpoint connects; trading can
continue. This keeps a type-9 inclusion branch at no more than 12 hashes.

### 9.4 Withdrawal authorization

A withdrawal receipt commits at least:

```
receipt_id, market_id, epoch, sequence, account, asset, amount,
destination_owner_commitment, vault_id, deterministic_change_shard
```

The signed user action fixes the destination. A type-9 `WITHDRAWAL` proof
contains `CheckpointId`, the complete canonical receipt leaf, leaf index, and
Merkle branch. Each v1 withdrawal transaction proves exactly one receipt;
multiple vault inputs may be used, all referencing the same proof id. Consensus
checks that the checkpoint is connected, the leaf is included, the receipt is
not in the nullifier set, all vault inputs are POOL_CHANGE for the same market,
the exact asset/amount is paid to OWNER outputs with the committed destination,
and every remainder output returns to the same VaultId as POOL_CHANGE. At most
64 vault inputs and one change output per asset are allowed; its shard is the
receipt's committed deterministic shard. Vault inputs equal receipt payouts
plus vault change for every asset exactly.

The withdrawal's miner fee must be supplied by separate native-B3 owner inputs;
vault value can never leak into fees. Connect records the receipt nullifier and
Disconnect removes it. Anyone may construct or relay a sweep/withdrawal because
the B3 DEX vault has no custodian key, but no relayer can change destination,
amount, asset, vault, or change policy. This statement is limited to B3 vault
custody; the Ethereum managed-v1 reserve vault has a separate immutable
withdrawal authority.

At each entry anchor, a withdrawal request is admitted only when the existing
pending obligations for that market/asset plus the new amount do not exceed
the sum of the largest 64 live `VAULT_POOL_CHANGE` UTXOs. If that exact chain
capacity is unavailable, admission fails closed. Payout construction orders
those live UTXOs by amount descending and then outpoint ascending, and takes
the shortest prefix that covers the receipt, never more than 64 inputs.

The publisher is deliberately sequential: publish one withdrawal, wait for it
to confirm, refresh the live vault index/capacity, then rebuild and publish the
next withdrawal. It never prepares a stale batch whose transactions compete
for the same keyless pool inputs.

Type 9 is bounded to 4 KiB and assigned a fixed 500-unit verification cost.
All length/count/Merkle-shape/nullifier checks precede hashes or signature work.

## 10. P2P and denial-of-service limits

Advertise `NODE_B3_FLOWMESH` and negotiate market subscriptions after the B3
version handshake. FlowMesh v1 uses dedicated messages and priority queues over
the existing B3 P2P connections; it opens no separate listener, port, overlay,
or network. Saturation or failure of those queues cannot delay B3 block
production or ordinary B3 validation. Commands are append-only and at most 12
characters:

```
fmhello    supported version, live markets, epoch/head summaries
fmaction   one user/deposit action
fmprop     one proposal envelope
fmattest   one BLS seat attestation
fmcert     one certified entry or handoff
fmget      bounded catch-up request
fmentries  bounded catch-up response
```

All messages carry version, MarketId, epoch, and sequence before variable data.
Unknown versions are rejected. Production limits are:

- action: 4 KiB; action pool: 65,536 actions/16 MiB per market and 256
  actions/1 MiB attributable to one peer;
- proposal: 1,024 actions and 2 MiB serialized;
- certificate: at most a 625-byte bitmap plus fixed fields/signature;
- at most eight executed candidates for one `(market, epoch, sequence)`;
- catch-up: at most 64 entries and 4 MiB per response, with no unsolicited
  response and one outstanding request per peer/market;
- FlowMesh receive queues: 2 MiB per peer and 64 MiB global; overflow drops
  FlowMesh work first and never blocks the B3 validation queue.

Inbound and outbound FlowMesh scheduling use separate bounded priority queues,
highest to lowest: certificates/attestations, proposals, user actions, then
catch-up. Bulky proposals or catch-up responses cannot head-of-line block a
vote or certificate. Existing B3 block/transaction download, validation, and
production use independent queues and never wait for any FlowMesh queue.

Use token buckets per peer and per market: a 256-action/1-MiB burst refilling
at 64 actions/256 KiB per second, and a 32-message committee burst refilling
at eight per second. Apply framing, epoch/head window, scheduled proposer,
seat index, duplicate, bitmap, and byte checks before BLS or execution.
Cache one attestation per `(epoch, sequence, hash, seat_index)` and disconnect
for repeated oversized/non-canonical frames. `MeshNode` remains single-threaded
per market; P2P threads only enqueue validated bounded objects. No FlowMesh
handler holds `cs_main` during cryptography or deterministic execution.

## 11. Runtime, persistence, and RPC

`FlowMeshProductionStore` format v3 is per market and replaces the static quorum marker
with domain, MarketId, current epoch/anchor/seat-set hash, next sequence, last
microblock, state root, and last B3 checkpoint. Every entry records the epoch
whose certificate verified it. Lock keys are `(epoch, sequence)`. Before a
real store reports a successful lock, it atomically persists the permanent
`(epoch, sequence) -> candidate_hash` mapping, the exact validated candidate,
and its authenticated action evidence. Commit atomically appends the entry and
marker and erases only the bounded pending candidate/evidence; the permanent
lock remains. Restart re-authenticates and re-executes only that candidate, and
corruption fails closed. Replay rebuilds and re-verifies every epoch
snapshot/certificate. There is no v2-to-v3 signing migration because FlowMesh
was never active; an old dev store fails closed.

The production runtime is owned by `NodeContext`, starts only after chainstate,
seat, vault, and checkpoint indexes are loaded, and stops before those
dependencies. One worker multiplexes lazily created per-market `MeshNode`
instances. A validator may own multiple FN seats and produces one independent
signature per local seat. Private BLS keys live encrypted in the wallet/key
provider. Generation and binding RPCs never return raw private keys;
`importflowmeshkey` is the explicit import path that accepts a caller-supplied
secret and stores it without returning it.

Read/status RPCs implemented in `src/wallet/rpc/flowmesh.cpp`:

```
listflowmeshmarkets
getflowmeshbalance market_id
listflowmeshvaultoperations [market_id]
```

Wallet construction and write RPCs implemented in
`src/wallet/rpc/assets.cpp` and `src/wallet/rpc/flowmesh.cpp`:

```
importflowmeshkey blssecret
bindflowmeshseat [address] [options]       // spends to FN v2, creates or reuses a BLS key+PoP
flowmeshdeposit base_asset_id deposit_asset amount [options]
submitflowmeshdeposit market_id txid vout
startflowmeshvalidator
stopflowmeshvalidator
submitflowmeshorder market_id bid|ask price quantity [sequence]
cancelflowmeshorder market_id bid|ask [sequence]
requestflowmeshwithdrawal market_id asset amount destination [sequence]
createflowmeshcheckpoint market_id
createflowmeshvaulttx effect_id [destination] // sweep or withdrawal
```

Market/status results report activation state, anchor, epoch, seat
count/threshold, local sync/halt state, and whether the state is available.
Vault-operation results expose only connected, unconsumed effects that can be
published. No RPC may call a request “redeemable” before its checkpoint and
inclusion proof are connected. Wallet balances distinguish chain wallet,
uncredited deposit, FlowMesh available/reserved, pending withdrawal, and
redeemable withdrawal; they are never summed into one misleading balance.

## 12. File-level implementation map

This is the current transition-release implementation map. Activation gates
keep later paths unreachable until their earlier chain/index dependencies
exist.

1. **Activation and carrier.** `src/consensus/{params,era,flowmesh_params}.h`
   defines the ordered A1/A2/A3 schedule and runway checks.
   `src/modern/asset_output.h` keeps the frozen B3A1 byte codec and parses only
   the gated DEX_VAULT-v2 shapes. `src/validation.cpp`, `src/coins.{h,cpp}`,
   `src/node/flowmesh_vault_index.{h,cpp}`, mempool, reindex, and wallet coin
   views carry vault UTXO classification. Simple-v1 conservation/issuance/BURN
   and DEX_VAULT creation/history begin at A2; type-9 vault spends and effects
   begin at A3.

2. **FN seats.** `src/modern/flowmesh_seat.h`,
   `src/modern/{policy,fn,mpa,payload_cost}.h`, and
   `src/flowmesh/seat_id.h` define FN v2 and frozen MPA type 7.
   `src/node/fn_seat_index.{h,cpp}` enforces live-key uniqueness, per-block
   undo, anchored snapshots, and reindex. Legacy genesis and modern PoD
   issuance produce exactly one FN unit while preserving the final historical
   reservation and 5,000 issued-ever cap.

3. **BLS engine and fees.** `src/flowmesh/bls_certificate.{h,cpp}`,
   `production_engine.{h,cpp}`, `production_wire.{h,cpp}`, and
   `production_commitment.h` implement SeatId-ordered BLS proposal,
   attestation, aggregate-certificate, and epoch-handoff paths.
   `fee_allocation.h`, `batch.h`, `clearing.h`, `ledger.h`, and `state.h`
   implement exact 100-ppm fee allocation, seat/treasury accounts, typed
   effects, and reward accounting. BIP340 remains the ordinary-user action
   credential.

4. **Persistence and chain authorization.**
   `src/node/flowmesh_production_store.{h,cpp}` implements the v3 durable
   store. The chain-authorized records and indexes are
   `src/modern/flowmesh_checkpoint.h`, `src/modern/flowmesh_vault_proof.h`,
   `src/node/flowmesh_checkpoint_index.{h,cpp}`, and
   `src/node/flowmesh_vault_index.{h,cpp}`. MPA types 8/9, their costs and
   mempool policy, checkpoint/sweep/withdrawal connect/disconnect, nullifiers,
   reindex, and mining are wired into the production path.

5. **Production runtime and transport.**
   `src/node/flowmesh_runtime.{h,cpp}` and
   `src/node/flowmesh_service.{h,cpp}` implement the `NodeContext`-owned
   lifecycle without a synthetic enable flag or signing key.
   `src/flowmesh/p2p.{h,cpp}`, `src/protocol.{h,cpp}`, and
   `src/net_processing.{h,cpp}` provide bounded dispatch, relay, and catch-up;
   `src/init.cpp` and the build manifests own startup/shutdown integration.

6. **Wallet and RPC.** `src/wallet/rpc/assets.cpp`,
   `src/wallet/rpc/asset_queries.cpp`, and `src/wallet/rpc/flowmesh.{h,cpp}`
   implement the command surface listed in §11. The wallet database stores
   encrypted BLS key records; backup/restore, asset-aware coin selection, and
   the explicit balance states above are wired through `src/wallet/`.
   GUI exposure is optional for v1.1; RPC correctness is not.

7. **Documentation and release pins.** The governing documents are reconciled
   across
   `b3-architecture-contract.md`, `b3-flowmesh-dex-decisions.md`, the FN/asset
   activation design, transition runbook, release notes, and RPC help. The
   absolute A3 height remains a release pin required before producing release
   artifacts.

## 13. Required tests and release gates

Unit/consensus tests must cover:

- frozen B3A1 big-endian header and implicit parameter length; no serialized
  commitment; ordinary native outputs versus the A2 native-vault exception;
  colored zero `nValue`; malformed claims; OWNER/FN/VAULT UTXO retention; BURN and metadata
  exclusion; Connect/Disconnect/reindex equivalence;
- simple asset genesis cap/conservation/burn and unauthorized remint; historical
  and modern FN PoD nullifiers, final rights count, 5,000 issued-ever cap,
  extinguishment, FN-v1 non-seat behavior, FN-v2 amount one, PoP, duplicate-key
  rejection, atomic rotation, and undo;
- A2-1/A2 vault preparation, pre-A3 trade/spend rejection, A3 trading, delayed
  four-seat readiness, the unique per-market epoch-zero anchor, and failure of
  every runtime flag to change validity;
- thresholds and exact bitmaps for k=4, 5, 6, 4,096, and 5,000; wrong domain,
  market, epoch, set, anchor, bitmap, signature, proposal, and duplicate seat;
- deterministic round robin and durable no-double-sign across restart;
  exact locked-candidate/evidence recovery, seat spend/rebind handoff,
  30-deep handoff-publication activation, shallow handoff republication,
  fewer-than-four pause, missed-handoff safe halt, conflicting certificate
  halt, and deep-anchor-reorg halt;
- fee overflow/rounding vectors, 100-ppm total (not per side), 80/20 split,
  seller largest-remainder allocation, and equal all-seat remainder order;
- deposit as-of-anchor lookup, one-time credit, wrong market/account/shard,
  the attack that tries to spend USER_DEPOSIT directly, exact sweep, receipt
  redirection, wrong checkpoint/branch, duplicate/nullified receipt, vault fee
  leakage, forced pool change, top-64 pending-obligation admission, amount-desc
  then outpoint payout selection, sequential publish-confirm-refresh/rebuild,
  disconnect/reconnect, and an offline backlog above 4,096 withdrawal effects
  checkpointed deterministically at B3 block boundaries while base-chain
  publication remains sequential;
- every P2P count/byte/rate bound, allocation-before-bound fuzzing, stale epoch,
  fake proposer/seat, malformed BLS, unsolicited catch-up, queue saturation,
  and restart/catch-up determinism.

Functional gates require at least a four-seat multi-node regtest executing:

1. simple asset issue and FN-v2 seat binding;
2. B3 and asset deposits, certification, checkpoint, and deposit sweep;
3. a matched trade with exact fee split;
4. user, seat-reward, and treasury withdrawals through B3;
5. a spent/rebound seat and on-chain epoch handoff;
6. validator restart and a lagging node's bounded catch-up;
7. an induced FlowMesh split-lock, store failure, and P2P flood while B3 keeps
   producing and validating ordinary blocks.

Before mainnet, run the existing H/X state-equivalence and corridor suites,
full unit/functional/fuzz suites under sanitizers, a 5,000-seat certificate and
rotation load test, wallet backup/restore tests for BLS keys, clean reindex from
genesis, and a release-candidate soak spanning at least one real seat rotation.

## 14. Hard release blockers

There is no unresolved cryptographic design decision: FlowMesh seat
certification is BLS aggregate plus a canonical bitmap and PoP-bound FN-v2
keys. The following are hard blockers to tagging v1.1:

1. the absolute mainnet A1/A2/A3 heights are not yet pinned;
2. the sealed X/R0/FN-manifest constants and final shadow-fork rehearsal are
   still release gates;
3. the four-seat end-to-end release rehearsal, reorg rollback checks, and
   focused custody/funds-safety suites must all be green on the final tree;
4. bridge-backed bUSD needs independent review of the implemented type-10
   mint/burn state machine and replay/undo path; an origin-enforced USDT
   adapter; the production Ethereum checkpoint/fork schedule, separate
   activation, approval range and per-block/per-epoch caps; reproducible
   runtime evidence and audits; and every authority/rules/X-dependent pin.
   The current state is rebuilt from activation and pruning stays refused until
   an atomic durable sidecar exists. Managed rules require the implemented
   finalized B3 burn request binding canonical bUSD, exact raw amount,
   Ethereum recipient and unique request id; the still-unimplemented operator
   service must wait finality, release exactly once, durably consume the id,
   and reconcile reserves against supply. No burn means no release; and
5. every stale document/comment that says FlowMesh ships later or native-CDP
   bUSD is current must be superseded.

Until the relevant gates pass, FlowMesh and bridge minting remain independently
fail-closed on production networks. A green FlowMesh rehearsal never activates
an incompletely pinned bridge.

A later decentralized verifier cannot replace the managed authority in this
vault. A new vault changes the current vault-bound `AssetId`, so migration must
explicitly retire the old registry, stop presenting deposits, handle/refund
late deposits to the still-callable old vault, burn/swap/reissue old bUSD, move
reserves without creating a second mint claim, and pin the new identity and
contracts.
