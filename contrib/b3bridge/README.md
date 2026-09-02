# Ethereum to B3 bridge relayer

This directory contains the proof-capture, proof-verification, and relayer
tools for the Ethereum deposit leg. They do not make an unconfigured bridge
safe or active. The decentralized bridge is intended for the current transition
release and may be deployed before Modern PoS, but it remains deliberately
fail-closed until a later B3 build pins the complete audited deployment tuple
and every remaining bridge parameter, review, and operating gate is complete
and matching. Deposits stay disabled until verifier initialization, inbound
height B, and a fresh valid certificate prove qualified current and successor
sets. Irreversible burns use a separate B3 height W. If B is enabled while W is
unset, this relayer can mint proven deposits but users cannot yet return bUSD
to Ethereum; disclose that custodial waiting period. Do not send real funds
without understanding which leg is active.

## Trust and prerequisites

The relayer is permissionless, but its inputs are security-critical. Before
running it, obtain the trusted Ethereum checkpoint root from the signed B3
release material. Pass that exact value with `--trusted-root`; never discover
or replace it from the same Ethereum endpoint being verified. The scan begins
at the consensus-pinned `getbridgeinfo.origin_deployment_block`.
`--start-block` is only an optional operator cross-check and must equal that
pin; it can never move the cursor forward and skip a deposit.

At startup `getbridgeinfo` must agree with the checkpoint, Ethereum chain id,
deployment block, verifier/vault addresses and runtime code hashes, token,
registry/asset ids, decimals, genesis root, fork schedule,
Electra epoch, committee threshold, and fork/lag horizons. The beacon endpoint
supplies evidence, not policy; the emitter config is built from B3's pins.
In particular, `getbridgeinfo.vault_runtime_code_hash` is the exact
`BridgeAssetParams::vault_runtime_code_hash` registry pin; it must match both
the final manifest and the live Ethereum `extcodehash` byte-for-byte.
Before binding or advancing durable state, the automatic relayer checks the
vault, verifier, and direct-token runtime hashes at the pinned deployment block
and at `latest` through the primary and every witness execution provider. A
missing historical state, empty runtime, provider disagreement, or hash mismatch
halts the relayer.
For decentralized withdrawals, the reported minimum and maximum bridge
validator counts and minimum total bridge weight must also match the reviewed
deployment manifest exactly.

The B3 side requires:

- a fully synced, unpruned B3 node with the bridge configuration active;
- a loaded, unlocked **dedicated relayer wallet** named by `--wallet`, funded
  only with the native B3 needed for carrier and mint transaction fees plus a
  small operating buffer; do not use a custody, staking, treasury, or personal
  wallet for this role; and
- B3 RPC authentication supplied either by `--b3-cookie` or by both
  `--b3-rpc-user` and `--b3-rpc-password`.

The automatic relayer is a POSIX Python 3 program (it uses an advisory file
lock) and is packaged for the Linux operator archives. It has no third-party
Python dependencies; `sqlite3` must be available in the Python standard
library. Keep its state and work directories on local durable storage with
permissions restricted to the operator account.

Live operation also requires at least two independent Ethereum execution RPC
providers. Pass the primary with `--ethereum-rpc` and at least one witness with
`--ethereum-rpc-secondary`. Every URL must have a distinct hostname/effective
port origin. Different API keys, credentials, or URL paths on the same origin
do not count as independent providers and are rejected. Distinct origins are a
minimum mechanical check; operators must still choose providers backed by
independent infrastructure. A dry run may use one provider.

Keep RPC endpoints private or TLS-protected. Prefer cookie authentication on a
local host, restrict the state database and cookie to the operator account,
and do not put passwords in shell history or service logs.

## Authenticated deposit discovery

The durable scan cursor never trusts `eth_getLogs` as evidence that a page has
no deposits. The relayer starts from the execution block hash proven by the
Ethereum beacon light client, walks the exact parent-hash chain, and verifies
each execution header's RLP hash. It then uses the authenticated `logsBloom`
only for definite negatives: a bloom miss proves that block cannot contain the
pinned vault, `Deposit` topic, and pinned token combination.

For every bloom-positive block, the relayer downloads the complete receipt
array, checks its length and block/transaction positions, reconstructs the
full Ethereum receipt trie, and requires its root to equal the authenticated
header's `receiptsRoot`. Only then does it enumerate deposits from those
receipts. An endpoint may therefore make the relayer halt by withholding data,
but it cannot move the cursor past an omitted deposit: an incomplete or altered
receipt set does not reproduce the committed root.

Every accepted event must be a successful, exact vault `Deposit` with the
consensus-pinned origin token. The verifier rejects a different token even if
the other event fields look valid. The 32-byte B3 recipient must also be the
canonical `RECIPIENT_V1` encoding and its 20-byte P2PKH hash must be nonzero;
the all-zero, practically unspendable recipient is rejected before broadcast.

Build the payload verifier/emitter explicitly; it is test/operator tooling and
is not part of the node by default:

```sh
cmake -B build -DB3_BRIDGE_TOOLS=ON
cmake --build build --target \
  b3-bridge-ethcheck b3-bridge-bootstrap-proof
```

The normal and fully static Linux release archives already contain both
binaries under `bin/` and the relayer, helpers, tests, and this guide under
`contrib/b3bridge/`. From an unpacked archive, use
`--payload-tool ./bin/b3-bridge-ethcheck`. Operators can run the packaged
offline recovery suite before configuring credentials:

```sh
python3 contrib/b3bridge/test_b3_bridge_relayer.py
```

The operator examples below use the unpacked-archive path `./bin/`. In a
source checkout built with the commands above, substitute `build/bin/`.

## Build the one-time Set_0 initialization

The same build option provides `b3-bridge-bootstrap-proof`. It is an offline
builder, not a deployer: it accepts four public bootstrap identities and three
independent `signbridgebootstrap` result files, verifies them, and emits the
exact `B3FinalityVerifier.initialize` calldata.

Before contract deployment, **all four bootstrap operators** must unlock their
own wallet and export its already-confirmed FINALITY_KEY identity:

```sh
./bin/b3coin-cli -rpcwallet=VALIDATOR_WALLET exportbridgebootstrapidentity \
  > operator-identity.json
```

Each operator retains and publishes the returned `validator_key`, `bls_pubkey`,
`proof_of_possession`, `binding_seq`, `binding_height`,
`binding_bip340_sig`, and `chain_domain`. These are public proofs; the command
does not export either private key. All four packages must carry the identical
chain domain. Preserve the original files so the deployment manifest can be
reproduced and audited later.

After the four identities are frozen into the Ethereum deployment, those four
operators must **not** call `bindfinalitykey` again or revoke that binding until
the one-time Ethereum `initialize` transaction succeeds. A repeated confirmed
bind is an intentional key rotation; it makes that operator's old immutable
manifest row ineligible to sign the handoff. If any rotation already happened,
re-export all affected current identities and rebuild/review the deployment
pins before deploying the contracts.

The public manifest is:

```json
{
  "chain_domain": "0x...32 bytes...",
  "modern_start_height": 811001,
  "committee": [
    {
      "validator_key": "...32 bytes...",
      "bls_pubkey": "...48 bytes...",
      "proof_of_possession": "...96-byte BLS proof of possession...",
      "binding_seq": 0,
      "binding_height": 810123,
      "binding_bip340_sig": "...64-byte BIP340 binding proof..."
    }
  ]
}
```

There must be exactly four committee rows, built without editing the exported
identity fields. Proof of possession and binding evidence are mandatory for
every row. The builder verifies each BIP340 binding proof against the manifest
chain domain, validator key, BLS key, and sequence. A compact row may omit the
export's repeated `chain_domain` as shown above; if it is retained, the builder
also requires it to equal the authoritative top-level `chain_domain`. It sorts
rows by raw `validator_key`, assigns equal weight one, and therefore
deterministically constructs the pinned synthetic 3-of-4 header.

Before deployment, derive the three contract pins directly from that manifest:

```sh
./bin/b3-bridge-bootstrap-proof bootstrap-manifest.json \
  > bootstrap-deployment-pins.json
```

Copy `deployment_env.BOOTSTRAP_AGGREGATE_PUBKEY`,
`deployment_env.BOOTSTRAP_MEMBERS_ROOT`, and
`deployment_env.EXPECTED_BOOTSTRAP_SET_HASH` exactly into the deployment
environment. Never calculate or hand-edit these values separately. Publish the
manifest and this output so another operator can reproduce them before funds
are accepted.

This offline check validates all four BLS proofs of possession and all four
BIP340 binding signatures. It cannot query B3 and therefore deliberately
reports `manifest_binding_chain_inclusion_verified: false`. Before deployment,
independently confirm that every identity's binding transaction is confirmed
on the active B3 chain at the stated height and that the exported binding is
still current. The later `signbridgebootstrap` command repeats the stronger
wallet-side current-binding check against the M-1 snapshot.

Later, once every node can reproduce the exact M-1 Set_0 snapshot and B3
finality has pinned a descendant of M-1, at least three of those same four
operators run:

```sh
./bin/b3coin-cli -rpcwallet=VALIDATOR_WALLET signbridgebootstrap \
  "BLS_PUBKEY_FROM_MANIFEST" BINDING_SEQ > signer-a.json
```

No snapshot or digest is supplied by the caller. Each wallet derives it from
its active B3 chain. The three selected result files must therefore sign the
identical M-1 block, Set_0 header/hash, finalized-block bytes, digest, and chain
domain before the offline builder will aggregate them.

Save three unmodified wallet RPC results and run:

```sh
./bin/b3-bridge-bootstrap-proof bootstrap-manifest.json \
  signer-a.json signer-b.json signer-c.json > initialize.json
```

The three packages must be from distinct manifest members. Their confirmed
binding heights must be no later than M-1 and each binding sequence must match
its manifest identity. Output includes the same canonical bootstrap header/hash
produced before deployment, signer bitmap, compressed and EIP-2537 points, the
ordered absent-member path,
the prover ABI bytes, and full initialize calldata. Publishing the manifest,
all four identity exports, the three signature packages, and the builder output
lets independent operators reproduce the initialization before it is sent.

Bitmap indices use that same canonical validator-key order and are LSB-first
inside each byte: bit `1` means that member signed and bit `0` means it did
not. For four members, `0x0b` is binary `1011`, so members 0, 1, and 3 signed
while member 2 is the ordered absent witness. The bitmap identifies the
participants; the aggregate BLS signature is what proves they signed.

## Resume from the B3-finalized light-client store

After B3 has connected an Ethereum light-client state and B3 finality covers
the block that last changed it, `getbridgelightclientstore` exports the exact
store: the finalized beacon and execution header with execution branch, all
512 current sync-committee keys, the optional next committee, its period, the
B3 connection block, and the B3 checkpoint that finalized that connection.
The RPC fails rather than exporting a tip-only or reorgable connection.

The automatic relayer saves that RPC result as `store.json` in its proof work
directory. When this file is present, `b3-bridge-ethcheck` validates its
version, committee shape, period, execution branch, B3 connection metadata,
and finality ordering, then processes only updates after that exact store. It
does not emit another bootstrap carrier. This avoids depending on beacon APIs
to preserve every historical intermediate update forever. Before B3 has a
finalized store, the original release-pinned checkpoint/bootstrap path remains
the only accepted starting point.

The store snapshot and historical deposit ancestry are separate checks. For a
scan cursor older than the current light-client head, the relayer calls
`getbridgeanchorforblock CURSOR` and selects the nearest execution anchor that
is already on B3's active chain and covered by B3 finality. Both independent
Ethereum execution providers must reproduce that anchor's exact block number,
hash, receipts root, timestamp, and canonical RLP header. The relayer then
walks the authenticated parent-hash chain only from that historical anchor to
the cursor. It re-queries the B3 anchor immediately before persisting a proof
or advancing the cursor, so a concurrent B3 reorganization causes a retry.

This recovery path never substitutes the historical anchor for `store.json`.
The C++ verifier still starts the Ethereum light client from the exact exported
store and verifies new sync-committee updates independently; the retained
anchor only supplies the already-B3-finalized source for the older execution
ancestry. If B3 has no finalized retained anchor that can reach the cursor
within the cumulative 20,000-block proof window, the relayer halts. Each later
cycle requests a new anchor for its current cursor, allowing a fresh database
or the one-time preview replay to progress through the finalized retained
history without trusting a local historical-anchor cache.

## Verify before broadcasting

Start with a one-pass dry run:

```sh
python3 contrib/b3bridge/b3_bridge_relayer.py \
  --ethereum-rpc https://example.invalid \
  --ethereum-rpc-secondary https://independent.example.invalid \
  --beacon-url https://example.invalid \
  --b3-rpc-url http://127.0.0.1:5467 \
  --b3-cookie /secure/path/.cookie \
  --wallet RELAYER_WALLET \
  --payload-tool ./bin/b3-bridge-ethcheck \
  --trusted-root 0xRELEASE_PINNED_CHECKPOINT \
  --start-block DEPLOYMENT_START_BLOCK \
  --state /secure/path/b3-bridge-relayer.sqlite3 \
  --work-root /secure/path/b3-bridge-relayer.work \
  --workdir-retention 8 \
  --dry-run --one-shot
```

`--dry-run` performs no B3 broadcast and deliberately does not advance the
durable Ethereum scan cursor. It may retain fully verified candidate jobs, but
live mode always rescans that range using authenticated headers and complete
receipt roots. `--one-shot` performs one scan and then exits; omit it only
after the dry-run output, checkpoint, start height, wallet, fee limits, and
recovery database have been reviewed. If `--state` is omitted, the database is
`b3-bridge-relayer.sqlite3` in the current directory. If `--work-root` is
omitted, it is `<state path>.work`.

Live mode additionally requires both fee controls; it refuses to start without
them:

```sh
python3 contrib/b3bridge/b3_bridge_relayer.py \
  ...same reviewed endpoints, wallet, pins, state and work-root... \
  --max-fee-b3 0.01 \
  --daily-fee-budget-b3 0.50
```

The values above are syntax examples, not recommended limits. Choose limits
for the current B3 fee market and the deliberately small balance in the
dedicated relayer wallet. `--max-fee-b3` caps each prepared transaction.
`--daily-fee-budget-b3` caps the sum of fees reserved in the SQLite database
for one UTC calendar day; the daily limit must be at least the per-transaction
limit. The raw transaction and its fee reservation are persisted before
broadcast, so a restart cannot bypass either budget.

For diagnosis or independent integration, the payload tool consumes a work
directory containing `config.json`, `updates.json`, optional
`finality_update.json`, and optional `receipt_proof.json`, plus exactly one
starting state: the release-checkpoint `bootstrap.json`, or the node-exported
`store.json` described above:

```sh
./bin/b3-bridge-ethcheck WORKDIR --emit-payloads
```

On success its standard output is one JSON plan. `updates` and `backfills` are
always present; `mint` is present when `receipt_proof.json` is provided. The
checkpoint path also includes a canonical `bootstrap` carrier. The finalized
store path instead includes read-only `store` reconciliation metadata and no
bootstrap payload. Carrier and mint entries contain canonical type-10 v1
payloads without an outer Modern Payload Area frame. Diagnostic text must not
be mixed into the JSON output. The relayer submits any bootstrap, each update,
and each execution backfill with `submitbridgecarrier`, and submits the mint
last with `claimbridgedeposit`.

For a historical deposit, `receipt_proof.json` also contains the complete
`source_anchor` returned by `getbridgeanchorforblock`. The emitter requires the
first execution header to match its block number, hash, and receipts root and
emits normalized source metadata for the relayer to compare back to the RPC
result. Supplying this JSON does not itself make an anchor trusted: the
resulting carrier or mint remains consensus-valid only if its source hash is
already retained by B3.

## Withdrawal readiness

This section describes implemented tooling, not an enabled mainnet workflow.
Withdrawals and B3 burns remain disabled by the unset outbound height W until
canonicality and liveness safety for the complete round trip is solved,
audited, rehearsed, and explicitly enabled in a later B3 build.

If that later gate is ever closed, a decentralized withdrawal burns bUSD on B3
before the corresponding release can be claimed on Ethereum. B3 accepts a
`BRIDGE_BURN` only when its
already-determined projected finality state has both a current and next
validator set, neither set is outside the pinned minimum/maximum validator
count, both sets meet the pinned minimum total weight, and finality lineage is
not broken. The check is applied by the wallet, mempool, miner, and block
validation. It uses the parent state projected to the candidate height, so the
withdrawal root created by that same candidate block cannot make itself ready.
The mainnet minimum count is four in each set. A two- or three-member Set0 can
start and finalize B3 and initialize the Ethereum verifier, while bridge
authorization remains closed until both the current and successor canonical
sets reach that minimum and the weight and freshness checks pass.

This consensus check proves that B3 has validator sets capable of satisfying
the immutable Ethereum verifier bounds. It cannot observe Ethereum wall-clock
freshness. Wallets, relayers, and operator monitoring must additionally require
the Ethereum contract's live readiness view before asking a user to burn.

## Permissionless outbound relay

This read-only surface may be exercised for review and rehearsal while burns
and releases remain disabled. No validator or bridge private key is needed to
build an outbound proof. A
fully validating, unpruned B3 node exposes two read-only RPCs:

```text
getbridgefinalityproof <certificate_block_hash_or_height>
getbridgewithdrawalproof <confirmed_txid> <burn_vout> [certificate_block_hash_or_height]
```

`getbridgefinalityproof` reconstructs the validator state at the selected
active-chain block, re-verifies its certificate, builds every ordered absent-
member witness, and returns exact `B3FinalityVerifier.submitCertificate`
calldata. Relay certificates in B3 epoch/height order; calldata for a later
certificate does not let Ethereum skip a missing earlier handover.

`bridgewithdraw` returns a stable transaction id and `burn_vout`, but it does
not claim to know a withdrawal id or leaf before confirmation: block ordering
and a reorganization can change both. After the burn confirms, pass those two
stable values to `getbridgewithdrawalproof`. The RPC resolves the consensus id
only from the active chain; an unconfirmed or reorganized burn fails closed.
It uses the latest certificate by default, or a named older active-chain
certificate, and recomputes the complete cumulative withdrawal prefix and its
ordered depth-32 path. It returns exact `B3StakerBridge.release` calldata. A
selected certificate must include the withdrawal and must have a signing set
that meets the deployment's bridge count and weight floors. Ethereum verifies
the path only against the verifier's single current bridge root, so normally
relay the latest certificate first and build the withdrawal proof from that
same certificate. An older certificate is useful only while its root is still
the verifier's current root.

Both calls fail closed for a side-chain block, missing/pruned block data,
unavailable historical state, a mismatched active-chain connection, a bad
leaf/root/set/signature, or incomplete production pins. They never read,
accept, or store an Ethereum private key. Anyone may submit the returned
calldata to the pinned contract, but that Ethereum account pays the gas.

Byte order is explicit in the JSON. `asset_id` is B3's normal wallet/display
form; `asset_id_evm` is the raw 32-byte order committed by Solidity and the
withdrawal leaf. Do not substitute one for the other.

## Confirmation order

Bridge state advances only when B3 confirms a carrier. Process exactly one
dependent step at a time:

1. when starting from the release checkpoint, submit the bootstrap and wait
   for its B3 confirmation; when starting from `store.json`, reconcile that
   read-only snapshot and submit no bootstrap;
2. submit updates in emitted order, confirming each before the next;
3. submit backfills in emitted order, confirming each newly retained execution
   anchor before using it; and
4. submit the mint only after its named anchor is confirmed.

Do not parallelize, reorder, or blindly rebroadcast these steps. One ancestry
record contains at most 32 headers, including both its source and target, so it
advances at most 31 Ethereum blocks. Older deposits therefore require serial
backfill carriers.

The complete backfill plan is capped at 20,000 Ethereum execution headers.
The light-client bootstrap and every relevant update are subject to the same
20,000-block adjacent execution-height bound. The relayer checks this before
funding a carrier and asks for missing intermediate period updates rather than
submitting a consensus-invalid jump.
Run at least one relayer continuously enough to keep a consensus-retained
anchor within that window. If every retained anchor falls farther behind, the
relayer stops rather than constructing an unbounded proof; changing RPC
providers or deleting its database does not repair that protocol-level gap.
The exact stopped cursor can be checked manually with
`getbridgeanchorforblock`; `found: false` means an operator must first establish
a reachable anchor through valid intermediate light-client/backfill records,
not override the cursor.

`--b3-confirmations` is an additional minimum (default 1), not the finality
rule. A job becomes a permanent dependency only when its B3 block is at or
below `getfinalitystatus.finalized.height`.

## Recovery and fail-closed operation

The SQLite state file is the relayer's crash-recovery record. Reuse the same
file after restart, keep it on durable storage, back it up with the operating
configuration, and never delete or edit it to force progress. The relayer must
record the phase, payload identity, B3 transaction id, and confirmation before
advancing. After a timeout or crash near broadcast, reconcile the recorded
transaction and `getbridgeproofstatus` with the B3 node before retrying; an
unknown response is not proof that no transaction was sent. A per-database
process lock prevents competing local daemon instances.

On the first run with authenticated receipt-root scanning, a database created
by the preview relayer replays once from the immutable deployment height. This
is intentional: the old `eth_getLogs` cursor is not accepted as proof that no
deposit was omitted. Existing deposit and payload identities deduplicate during
the replay, and subsequent restarts keep the new authenticated cursor.

Generated receipt proofs and ancestry files are diagnostic artifacts; the
canonical queued payloads and their fee reservations live in SQLite.
`--workdir-retention N` keeps the newest N `deposit-*` proof directories
(default 8, maximum 1000) and removes older ones from the managed work root;
`0` retains none. The `sync` directory and SQLite database are not part of this
pruning. For an incident, copy the relevant proof directory and logs outside
the managed work root before the retention limit removes them. Increasing the
limit consumes disk, so monitor both the work-root filesystem and database.

Standard error is newline-delimited JSON suitable for an operator log. Alert
on `cycle_failed`, repeated `job_waiting_finality`, and a cursor that stops
moving. In daemon mode a failed cycle is retried after `--poll-seconds`; in
`--one-shot` mode a failed cycle exits nonzero for monitoring and CI.

Stop without broadcasting or advancing state on any checkpoint mismatch,
unknown Ethereum fork, provider-origin duplication, non-final or failed
receipt, header/receipt-root/proof mismatch, incomplete receipt array, wrong
origin token, noncanonical or zero B3 recipient, malformed payload plan, B3 RPC
disagreement, inactive or stale bridge state, fee-limit exhaustion, rejected
carrier, duplicate deposit, mint-cap failure, database error, or ambiguous
broadcast result. Changing RPC providers, restarting, or using `--one-shot`
must not bypass these checks. Preserve the retained work directory, SQLite
database, emitted plan, transaction ids, and both nodes' logs for recovery and
incident review.
