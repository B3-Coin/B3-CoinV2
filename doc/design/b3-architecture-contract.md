# B3Coin Core Architecture Contract

**Status: AUTHORITATIVE AND LOCKED.** This document supersedes all earlier design notes
where they conflict. Do not reinterpret these decisions. If an implementation problem
appears to require changing something here, **stop and report the contradiction** rather
than choosing a new protocol.

Items marked *(open)* are deliberately unresolved and are tracked in
[b3-open-decisions.md](b3-open-decisions.md).

---

## 0. The core invariant

> B3 has one immutable historical ledger through block **X** at height **H**;
> beginning with **H+1**, the same ledger evolves under a new consensus and
> state-transition language **without rewriting any historical identity**.

---

## 1. Chain identity

1. B3 remains one continuous blockchain.
2. The original B3 genesis block is permanent and must remain byte-for-byte and
   hash-for-hash unchanged.
3. There is no new genesis.
4. There is no balance migration to another chain.
5. There is no consensus-pinned UTXO snapshot.
6. Historical transaction IDs, outpoints, balances, scripts, staking rewards and block
   history remain part of the same B3 chain.

## 2. The transition anchor is immutable

Two consensus constants:

    LEGACY_FINAL_HEIGHT = H      // last legacy block
    LEGACY_FINAL_HASH   = X      // exact hash of block H
    hard_fork_height    = H + 1  // first modern block

Therefore:

    height <= H     => LEGACY era
    height >= H + 1 => MODERN era

A node must **never** enter the modern era merely because it reached height H. It must have

    height == H  AND  block_hash(H) == X

Only that exact historical B3 prefix may transition. A competing chain that reaches
height H with a different hash is **permanently incapable** of becoming modern B3.

**H and X are mainnet consensus constants, not runtime overrides.** No RPC or
configuration option may change them on mainnet (no `-finallegacyheight=`,
`-finallegacyhash=`). Regtest and testnet may expose override facilities; mainnet may not.

Existing mainnet H and X are not configured until a later explicitly approved task.

**Transition-corridor reconciliation (2026-08-16,
[b3-during-fork-transition.md](b3-during-fork-transition.md)):** the fork is
a staged process. `H = FINAL_LEGACY_POS_HEIGHT` and `X = hash(H)` are exactly
this contract's boundary: everything stated about `H`/`H+1` — the immutable
anchor, the finality boundary, the reorg prohibition, the absolute
format/codec switch at `H+1`, trusted replay ending at `H` — applies
unchanged. What the corridor refines is the **block-production consensus**
inside the modern format: heights `H+1 … H+1000` are the temporary-PoW
corridor (modern blocks, modern transactions, Policy Outputs, block
production by B3's historically existing PoW primitive), during which real
STAKE Policy Outputs are created and matured; `M = H+1001` is the first
modern-PoS block, validated against the registry derived deterministically
at the end of `H+1000`. "Modern era" in this contract therefore spans two
production phases (TRANSITION_POW, then MODERN_POS) over one unchanged
modern block/transaction format; where this contract says modern PoS begins
at `H+1`, read: the modern *format* begins at `H+1`, modern *PoS* begins at
`M = H+1001`. After `H`, legacy PoS never resumes under any circumstances.

## 3. H is a finality boundary

Stronger than an ordinary checkpoint. Once modern activation exists:

    reorg crossing H         -> forbidden
    disconnect H             -> forbidden
    replace H                -> forbidden
    alternate pre-H history  -> forbidden

Modern chain selection applies **only to descendants of X**.

    Genesis ───────────── X(H)
                           │
                           └── modern H+1 ── modern ...

    (no alternate parent for H is ever admissible)

A refused cross-boundary disconnect is a **rejection of the candidate chain**, not a
local failure: it must not abort or shut down the node.

## 4. Legacy nodes after H

Legacy software may keep generating `H -> legacy H+1 -> legacy H+2`. That is simply a
**dead fork**. Modern nodes reject legacy-form blocks at connected heights `>= H+1`.
There is **no** stake-weight comparison, cumulative-work comparison, or chain-selection
competition between that fork and modern B3. After X is locked, old consensus has no
authority over modern B3.

## 5. Grace period

The upgrade grace period is **operational and exists before H**. Consensus remains legacy
until H; at H+1 the switch is **absolute**. The grace lives in wallet/relay tooling and
user communication, never in consensus.

After the switch, compatibility may continue for: legacy addresses, legacy private keys,
legacy RPCs, legacy wallet import, historical block serving, legacy transaction decoding,
and legacy-script UTXO spending — **but not for producing legacy blocks**.

There is **no** consensus fallback such as "if transition fails, accept legacy blocks for
another N blocks." If software is wrong, the remedy is coordinated repair before
activation, not hidden consensus fallback.

## 6. Block codec selection

The reserved marker is

    0x28000000    // BIP9 top bits | reserved bit 27

The marker determines, **before the parent height is known**: block codec, transaction
codec used inside the block, presence/absence of the trailing legacy block signature, and
the block-header hash domain.

Required pipeline:

    network bytes
        -> read minimal fixed header/version
        -> identify block family (marker)
        -> legacy decoder OR modern decoder
        -> calculate hash in the correct domain
        -> locate parent
        -> calculate connected height
        -> enforce era legality

Never `unknown parent -> assume height zero -> legacy decode`.
Never `height guess -> choose codec`.

The marker states what the bytes **claim** to be; connected height decides whether that
claim is **legal**:

    LEGACY codec at height <= H      allowed
    MODERN codec at height <= H      reject
    MODERN codec at height >= H+1    allowed
    LEGACY codec at height >= H+1    reject

## 7. Hash domains remain permanently separated

Legacy block hashes remain exactly the historical B3 block hashes. Modern blocks use the
**existing modern hash construction** — a tagged modern block hash must **not** be
introduced merely for domain-separation aesthetics; modern block hashing changes only if
separately and explicitly decided.

Do not reinterpret legacy bytes through modern serialization. Do not produce a
"normalized" historical block hash.

    legacy txid           = historical txid, forever
    modern transition id  = modern definition

No ID collision may be created through cross-codec interpretation.

Where **new** hashing is introduced (assets, policies, receipts, and similar new
constructions), it must be explicitly domain-separated, e.g. conceptually
`H("B3/ASSET" || ...)`, `H("B3/POLICY" || ...)`, `H("B3/RECEIPT" || ...)`, rather than
reusing an undifferentiated construction. This requirement applies to new constructions
only — **not** to block identity or existing transaction identity.

## 8. Standalone transactions

Transactions outside a block have no enclosing block marker. Their codec is selected
explicitly by **trusted context**:

- historical legacy importer: LEGACY
- legacy peer before activation: LEGACY where explicitly supported
- modern wallet / RPC / mempool / P2P relay: MODERN

Do **not** use a process-global `legacy_b3coin` Boolean as the transaction codec selector.
Do **not** add a transaction-family marker byte: block-context codec selection plus
trusted per-connection context for standalone transactions is the locked mechanism.

Connection context determines which wire interpretation is expected. Transaction bytes
determine permanent consensus identity. These are separate concerns.

## 9. No process-global era switch

No `bool fModernMode;` controlling serialization — that creates races and context
confusion. Era/context is passed explicitly (serialization context, block codec, tx codec,
validation era) or represented in strongly typed objects. Legacy and modern objects should
be hard to accidentally mix.

## 10. Legacy era

The legacy chain must retain: historical transaction-level `nTime`; historical transaction
IDs; historical block encoding; historical trailing PoS block signatures; historical B3
block hashing; compatibility with protocol-version 80008 peers; existing balances, scripts
and outpoints; and existing staking rewards through H.

Before the fork is finalized, live nodes continue validating and staking under existing
legacy rules.

## 11. Trusted legacy replay

**X authenticates the legacy prefix; replay reconstructs state.** Replay does not re-prove
that legacy consensus was correct.

A fresh node:

    1. Obtain blocks 0…H.
    2. Parse using the historical codec.
    3. Verify block byte/hash identity.
    4. Verify prev-hash linkage.
    5. Verify required historical checkpoint anchors.
    6. Verify Merkle commitments.
    7. Apply transactions deterministically to replay state.
    8. Reach H.
    9. Require hash(H) == X.
    10. Seal the resulting legacy UTXO state.
    11. Continue modern validation from H+1.

Replay **skips**: historical PoW, PoS kernel, stake modifier, difficulty, reward
entitlement, historical timestamp consensus, and historical signature/script consensus
validation — unless a given piece is *mechanically necessary to reconstruct state* rather
than to judge consensus.

Replay still rejects malformed or internally inconsistent data: broken linkage, wrong
checkpoints, invalid Merkle roots, missing prevouts, duplicate spends, arithmetic overflow.

Replay must be deterministic and crash-safe: a restart produces identical state, and
recovery never guesses.

## 12. Replay must reproduce historical quirks

Do **not** "clean up" legacy history. Strange-but-valid historical semantics are
reproduced exactly.

Mandatory migration test — legacy full validation vs trusted replay at H:

    CanonicalSerialize(U) == CanonicalSerialize(U')     // and ideally hash(U) == hash(U')

where `U` is the UTXO state from the legacy (`master`) client fully validating the chain,
and `U'` is the state from modern trusted replay. Requires a deterministic UTXO commitment
tool built for testing. **No mainnet activation until these match exactly** for every
unspent outpoint: value, script bytes, transaction identity, and metadata needed later.

## 13. No consensus snapshot migration

A locally generated cache may speed startup, but the protocol does not become a snapshot
chain. A node must always be capable of deriving state from `Genesis -> ... -> X`. A
distributed precomputed replay state may later be offered as an optimization, but it must
be authenticated against independently reconstructable state and must not replace the
historical chain.

## 14. Legacy UTXOs retain original identity

A historical output

    txid = abc..., vout = 2, value = 500 B3, scriptPubKey = legacy-script

remains `OutPoint(abc..., 2)` after H. No conversion into a new asset transaction, no new
txid, no remapping table, no artificial genesis allocation, no balance migration. The
original historical UTXO is consumed directly by a modern transaction.

## 15. Legacy-spend adapter

Modern validation bridges the two state models:

    Prevout
     ├─ modern::ModernOutput  -> modern policy transition validation
     └─ legacy CTxOut         -> LEGACY_LOCK validation

A historical B3 output is interpreted as native B3 under a legacy locking policy:

    asset     = B3_NATIVE
    amount    = legacy nValue
    policy    = LEGACY_LOCK
    lock_data = original scriptPubKey
    identity  = original outpoint

This is a **validation view**, not a mutation of the stored historical output.

## 16. Legacy script spends after H

**Freeze legacy script interpreter semantics at H.** Future Bitcoin Core script changes
must not retroactively alter what historical B3 coins mean. Modern code calls that frozen
interpreter when consuming a legacy output. Modern outputs do not inherit arbitrary legacy
script semantics unless the policy explicitly requests `LEGACY_LOCK`.

## 17. Modern era

Starting at H+1: clean Bitcoin Core 31.1-style transaction serialization; no
transaction-level `nTime`; no trailing legacy block signature; the post-fork B3 block codec
marker; full modern validation; modern B3 PoS *(open — see §28)*; modern networking
capabilities; and permanent rejection of reorganizations that disconnect H or anything
below it.

The modern block header remains Bitcoin-style. It is **not** to be redesigned merely
because modern consensus is proof-of-stake.

## 18. Pre-H UTXOs after the fork

A modern transaction may spend a pre-H UTXO. The prevout remains the old legacy txid +
output index, and the old locking script remains enforceable (under the frozen legacy
ruleset, §16) when that output is eventually spent. Historical blocks are **not**
revalidated merely because an old output is spent after H.

## 19. Modern native B3

Native B3 has a canonical reserved asset identifier (`AssetRef 0` / all-zero `AssetId`),
reserved forever. B3 is **not** issued through the generic coloured-asset issuance engine.
Its supply continues directly from the historical B3 ledger plus whatever modern B3
monetary policy is eventually locked *(open)*.

## 20. Asset registry

Modern outputs stay compact:

    ModernOutput {
        AssetRef asset;
        Amount amount;
        PolicyType policy_type;
        PolicyVersion policy_version;
        Commitment commitment;
    }

`AssetRef` resolves through consensus state to asset id, display metadata, decimals,
issuance policy, supply where relevant, and bridge identity where relevant.

**Symbol/ticker is never asset identity.** `USDC` text is UI; `AssetId` is consensus.

## 21. AssetId creation

Deterministic. For a native B3-issued asset, conceptually

    AssetId = H("B3/ASSET" || issuance_outpoint || issuance_policy_commitment)

For externally bridged assets the identity must also encode origin domain
(`origin_chain`, `origin_contract`, `bridge_instance`) so that Ethereum USDC, Arbitrum
USDC and a native B3 synthetic USDC can never collapse into the same asset.

## 22. Never trust ticker equality

FlowMesh trades by `AssetId`, never symbol. UI may render `USDC.e`, `USDC`, `USDT`;
consensus operates strictly on immutable identifiers.

## 23. Policy outputs are state cells

The modern UTXO layer is the custody/state commitment layer. Policy types include at least:

    OWNER, LEGACY_LOCK, DEX_VAULT, STAKE, BRIDGE, ASSET_ISSUER,
    FN / FN_LICENSE (if eventually required), EXPERIMENTAL

Unknown consensus policy types are **invalid** unless an explicit extension mechanism is
created. Silently ignoring unknown policy semantics is forbidden.

**Existing serialized policy enum numbers must never be renumbered.** Current assignments
(`LEGACY_LOCK = 0`, `OWNER = 1`, `BURN = 2`, `DEX_VAULT = 3`) are consensus-stable as-is;
additional types take new numbers.

## 24. Policy versions

Each policy type carries its own explicit version (`OWNER v1`, `DEX_VAULT v1`, `STAKE v1`,
…). An unknown version is **invalid** unless that policy specifies forward-compatible
semantics. This prevents a new client from giving old nodes a different interpretation of
the same output.

## 25. Commitment semantics

The 32-byte commitment is not an arbitrary opaque hash. Each policy defines a canonical
preimage encoding, e.g. conceptually

    commitment = H("B3/POLICY/DEX_VAULT/V1" || CanonicalEncode(vault_state))

Canonical encoding means exactly one byte representation per logical state: no JSON, no
platform-dependent integers, no unordered containers, no native struct serialization.

## 26. TransitionProof architecture

    old state + operation + new state + authorization  ->  TransitionProof

Witness-style separation is preferred:

    Transition ID  excludes large proof/authentication material
    Witness/proof ID may commit to the complete serialized transaction

This gives malleability-resistant state identity while letting proof systems evolve.
Proofs prove that the current state exists and the transition is authorized — **never the
full ancestry**.

## 27. Authorization binds the complete transition

Any signature/authorization must commit to at least: chain/domain, policy type, policy
version, operation, input state commitment, new state commitment, asset, amount where
applicable, and anti-replay data. Otherwise cross-policy or cross-asset replay becomes
possible.

## 28. Modern PoS — *(open)*

Modern PoS is **UNRESOLVED at the protocol-detail level** and must not be implemented
until its consensus specification is supplied. It currently fails closed. See
[b3-open-decisions.md](b3-open-decisions.md).

## 29. FlowMesh stays account-model

DEX fills are **never** UTXO spends. One order is **never** one UTXO. The boundary:

    UTXO layer
        ↓ deposit
    DEX_VAULT
        ↓
    FlowMesh internal ledger
        ├─ available   ├─ reserved  ├─ margin
        ├─ positions   ├─ orders/demand curves  └─ PnL
        ↓ withdrawal receipt
    DEX_VAULT
        ↓ UTXO layer

Trades happen entirely inside deterministic FlowMesh state. Do not add DEX-specific fields
to generic transaction inputs/outputs. Do not introduce a generic smart-contract VM.

## 30. Deposit semantics

A deposit has an unambiguous identity (`DepositId` from outpoint or modern transition
identity + index). FlowMesh credits a deposit **once**, after the required B3 finality rule
is satisfied. Ingestion must be idempotent: replaying the same deposit event produces no
second credit.

## 31. Withdrawals use receipts

    WithdrawalReceipt { account; asset_id; amount; destination; nonce; epoch; shard; }

The DEX vault transition proves: the receipt is valid, finalized, belongs to this
vault/shard, has not previously been consumed, the output pays the exact
destination/amount, and the remainder returns to the correct vault.

## 32. Vault must remain keyless

There must be **no private key** capable of arbitrarily withdrawing the DEX custody pool —
no multisig treasury. Instead:

    consensus-valid finalized FlowMesh receipt
        -> permissionless transaction construction
        -> DEX_VAULT policy validation
        -> withdrawal

The committee certifies state; it does not possess custody keys. Anyone may relay a valid
withdrawal; nobody may redirect it.

## 33. Forced change

A vault input of 1,000,000 USDC with a 500 USDC receipt must enforce
`500 -> destination` and `999,500 -> valid vault successor`. The transaction creator
cannot redirect the remainder. This is enforced **inside consensus**.

## 34. Receipt replay protection

Consensus-tracked one-time consumption with an immutable identifier, conceptually

    ReceiptId = H("B3/FLOWMESH/WITHDRAWAL" || shard || epoch || account
                  || nonce || asset || amount || destination)

Consumed receipts can never be reused.

## 35. Sharded vaults

Custody is sharded deterministically (by asset, vault shard, epoch rotation) so no single
vault UTXO becomes the DEX's serialization bottleneck. The mapping must be **deterministic**,
never opportunistically selected by the withdrawing user.

## 36. FlowMesh accounting uses integers

No `double`, `float`, or `long double` anywhere in consensus. Integer fixed-point only.
Each market defines consensus precision: price ticks, quantity lots, fee units, funding
units, margin precision. Overflow handling is explicit, using wide intermediate arithmetic
where necessary.

## 37. Determinism rules are global

Consensus code must never depend on: unordered map/set iteration order, wall clock,
`random_device`, floating point, locale-sensitive parsing, thread scheduling order,
pointer identity, filesystem order, or non-canonical serialization. Any set that affects an
output must have an explicit canonical ordering.

## 38. FlowMesh action order

Within an epoch there is exactly one deterministic ordering rule: per account, nonce
strictly increases; across accounts, a certified canonical-set ordering plus deterministic
tie-breakers. Every validator must execute the same action set in the same order wherever
ordering matters.

## 39. Microblocks are not separate blockchain histories

    B3 consensus/finality
        ├── FlowMesh epoch 100
        ├── FlowMesh epoch 101
        └── ...

Microblocks provide rapid execution/certification; periodic B3 commitments anchor the
state. Cadence is tunable later.

## 40. Latency target is not a consensus specification

Do not hard-code a latency figure as a validity rule. Consensus defines epoch identity,
cutoff logic, certification requirements, state transition, and timeouts/view changes.
Implementations may optimize toward low latency without making physical network latency
part of transaction validity.

## 41. Encryption remains optional until justified

Encrypted order flow stays outside the minimum viable consensus transition. Build first:
canonical action set, deterministic batch clearing, account reservations, microblock
certification, withdrawal receipts. Threshold cryptography must not become a dependency of
H+1 unless required for core safety.

## 42. Stablecoin-denominated FlowMesh fees

Markets may charge fees in the quote/collateral stable asset (B3/USDC → USDC; BTC/USDT →
USDT). "USDC" means the explicitly approved B3 `AssetId`, never an arbitrary asset sharing
that ticker.

## 43. Fee assets need a governed registry

A consensus-recognized `AcceptedFeeAssets` set with bridge/issuer identity pinned
(initially e.g. `USDC_ETH_BRIDGED`, `USDT_ETH_BRIDGED`). Otherwise someone issues
`FakeUSDC` and pays system fees with it. Adding/removing fee assets follows the modern
governance/consensus-upgrade mechanism *(open)*.

## 44. Base-chain fees stay native

    B3 blockchain transaction fee -> native B3
    FlowMesh trading fee          -> approved trading/settlement asset

Core consensus liveness must not depend on an external stablecoin issuer. This is a safety
boundary.

## 45. Stablecoin bridge risk stays explicit

A bridged USDC asset is a different security domain from B3; its solvency depends on the
origin chain, origin token, bridge verification, finality assumptions, and issuer
freeze/blacklist policy. It must not be described as protocol-native dollars without
qualification.

## 46. Independent PoW-issued coloured assets

A coloured asset may define PoW-based issuance, but that PoW **does not participate in B3
chain selection**:

    PoW proof -> ASSET_ISSUER policy -> valid new XYZ supply

never `PoW asset mining -> changes B3 consensus weight`. The issuance policy verifies its
own puzzle and supply rules; B3 validators simply validate that state transition.

## 47. Bound custom issuance complexity

No arbitrary unmetered VM code in issuance policies. First implementation uses typed/native
policy modules: `FIXED_SUPPLY`, `MINT_AUTHORITY`, `BRIDGE_BACKED`, `POW_ISSUED`,
`ALGORITHMIC`. General smart contracts are a separate future problem.

## 48. FN: three separate concepts

    FN recognition   |   FN license/right   |   FN economic bond

A historical FundamentalNode entitlement does **not** automatically imply perpetual modern
operational power. Legacy owners may receive a modern claim/recognition asset derived from
historical proof-of-integration state, but modern participation still requires modern
activation/bond/performance conditions.

## 49. FN claims come from a deterministic snapshot

Because there is no balance migration, FN recognition is generated from historical
on-chain facts. At X, construct a deterministic claim set (legacy FN identity, historical
proof-of-integration burn/outpoint, eligible beneficiary, claim amount). Claims are then
exercised permissionlessly. **No manual distribution. No administrator CSV.** The claim
root/state must be reproducible from legacy history.

## 50. FN claim is not a new genesis

    Genesis balance migration                          -> NO
    Consensus-recognized post-H claim from pre-H facts -> YES

A modern transaction creates the FN asset only when the historical entitlement is
proven/claimed. This preserves the one-chain architecture.

## 51. FN supply economics remain unlocked — *(open)*

Do **not** implement the old "every 25 FN → price doubles" scheme; its cartel/oligopoly
failure mode is identified. Keep the policy interface available and leave the issuance
curve outside consensus until economics is finalized. Current conceptual direction:
license scarcity + bond + performance-based revenue + B3 burn for new entry — rather than
forcing monetary deflation through token issuance.

## 52. Validators and FlowMesh FNs are separate roles

Modern PoS validators secure B3; FNs operate FlowMesh infrastructure/committee duties. The
roles are not automatically merged, and validators do not automatically receive a share of
FlowMesh trading fees — that would create unnecessary MEV coupling. Validator rewards
belong to B3 consensus economics; FlowMesh rewards to FlowMesh service economics.

## 53. Implementation order

    legacy replay + H boundary
        -> modern block validation
        -> modern PoS
        -> modern transaction/output model
        -> policy engine
        -> asset registry
        -> FlowMesh deposits/withdrawals
        -> FlowMesh execution
        -> bridge
        -> advanced issuance

**Do not wire FlowMesh, FN economics, bridge logic, experimental issuance or advanced
asset policies into consensus until the legacy→modern transition and modern PoS can
independently produce and validate a clean H+1 chain.**

## 54. H+1 is intentionally boring

The first modern block requires only the minimum capable modern consensus: modern block
codec, modern chain identity, modern PoS, native B3, legacy UTXO spending, basic modern
outputs, basic fees. Large subsystems activate later behind explicit activation
heights/version gates:

    H+1  modern consensus
    A1   typed assets
    A2   FlowMesh
    A3   bridge

This substantially reduces migration risk.

## 55. Consensus activation is deterministic

No runtime configuration (`-enableflowmesh`, `-modernassets=1`) may change block validity
on mainnet. Activation comes from a fixed height, a versioned deployment, or another
explicitly defined deterministic mechanism. Local configuration may affect relay, mining
and UI only.

## 56. Modern networking negotiates capability

Peers advertise what they understand (conceptually `NODE_B3_LEGACY_HISTORY`,
`NODE_B3_MODERN`, `NODE_B3_FLOWMESH`, `NODE_B3_ASSET`). Capabilities **never** override
consensus: a peer declaring modern support does not make malformed modern data valid.

## 57. Historical block serving stays useful

Modern nodes should retain the ability to serve the complete historical chain, so a new
node can obtain `Genesis ... H ... modern tip` from the modern network rather than relying
forever on a handful of archival legacy nodes. Pruning may exist; archival mode should
support both eras.

## 58. Anti-DoS precedes mainnet activation

Every externally supplied object needs bounded serialized size, vector counts, proof size,
script size, policy params, asset registrations, FlowMesh actions, receipts, bridge proofs,
CPU verification cost, memory allocation, and orphan retention. Perform cheap structural
rejection **before** expensive cryptography.

**Constraint:** anti-DoS changes must **not** impose proof-of-work validity on historical
B3 PoS blocks. Legacy PoS blocks legitimately carry no PoW; hardening must scope its gates
accordingly rather than requiring work where the historical protocol required none.

## 59. Unknown-parent handling must be bounded

Because codec selection happens before parent discovery, attackers can send arbitrary
marker-valid blocks with unknown parents. Therefore: decode only enough to establish
identity, apply strict size limits, use a bounded orphan cache, and apply rate
limiting/peer scoring. Never permit unlimited fully parsed unknown-parent blocks.

Sync must be **bounded and progress-safe**. (A specific ownership policy such as
"outbound-only sync" is *not* locked; any design meeting the bounded/progress-safe
requirement is acceptable.)

## 60. Cross-era reorg tests are mandatory

Deterministic unit/functional tests for at least:

    legacy H-1 -> legacy H              VALID
    legacy H-1 -> modern H              INVALID
    legacy H   -> legacy H+1            INVALID
    legacy H   -> modern H+1            VALID
    modern H+1 -> legacy H+2            INVALID
    alternate hash at H                 INVALID transition
    reorg from H+10 back through H      INVALID
    disconnect H                        forbidden
    unknown-parent legacy block
    unknown-parent modern block
    wrong marker / right height
    right marker / wrong height

## 61. Mainnet activation requires the state-equivalence test

See §12. Before choosing the real H/X, run replay against the actual current B3 chain and
require exact equality with legacy `master` state at H. No real activation until they match.

## 62. H is chosen only after software deployment

    1. Finish consensus code.   2. Audit.   3. Run legacy-state equivalence.
    4. Run transition testnet/regtest.      5. Release binaries.
    6. Give operators an upgrade period.    7. Observe adoption.
    8. Freeze candidate H.      9. Obtain block X at H.
    10. Produce final release containing H/X.   11. Activate.

**Preferred operational model:** choose a legacy block already sufficiently buried, declare
it H, record its known hash X, ship the final modern release, and begin the modern era per
a separately agreed activation mechanism. H/X should be **known before** the binary that
enforces the transition is activated. This coordination must not be improvised late.

## 63. Modern chain domain

B3V2 is not another blockchain. Modern signature/domain separation may include the B3
mainnet identity, the legacy genesis, and the transition anchor X — conceptually

    ModernChainDomain = H("B3/MODERN/CHAIN" || legacy_genesis_hash || X)

This is **not** a new genesis; it is a cryptographic domain identifier for anti-replay
separation while preserving continuity. (Per §7 this does not change block hashing.)

## 64. Testnet/regtest need independent anchors

Never reuse mainnet H/X semantics blindly. Each network gets its own genesis, message
start, ports, transition height and transition hash. Regtest must allow arbitrarily small H
so the whole transition can be tested in seconds.

## 65. Wallet migration

An old wallet holding B3 keys requires **no coin migration**. The upgraded wallet loads
existing keys, recognizes legacy UTXOs, creates modern-era spending transactions, and
supports modern addresses/policies. The private key controlling an old P2PKH/P2SH output
remains the authority unless the historical script says otherwise.

## 66. Backup compatibility

Wallet backups from before H remain recoverable: restoring an old `wallet.dat` after H must
allow rescanning historical chain, finding old outputs, and spending them through modern
transaction machinery (given the required keys).

## 67. RPC and indexers

RPC must make era explicit where ambiguity matters (e.g. an `"era": "legacy" | "modern"`
field on block/transaction introspection), and expose immutable `asset_id` alongside
display metadata for modern assets.

Indexers and explorers must **never** rewrite historical txids. Old links remain valid —
one of the strongest advantages of this architecture.

## 68. Genesis safety

No task may: regenerate genesis; change genesis transaction bytes, `nTime`, nonce, bits,
Merkle root or hash; apply the modern marker to genesis; or reinterpret historical blocks
using the modern codec.

**If a task appears to require any of these, stop and report instead.**
