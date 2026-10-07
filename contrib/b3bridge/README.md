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
python3 -m unittest discover -s contrib/b3bridge -p 'test_*.py' -v
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

Reorg reconciliation now invalidates affected connection observations without
discarding any original transaction ID, signed bytes or reserved network fee.
It preserves verified payload dependency order and does not reactivate unused
superseded plans. A wallet conflict, missing-input error or mempool conflict
is not proof that a prepared transaction can never settle. The relayer reports
`job_waiting_conflict_resolution` and reconciles its exact original effect on
later cycles; it does not automatically prepare a replacement. If the original
can be sent again, the retry uses its exact recorded bytes. A permanently spent
input may require separately reviewed operator recovery; do not edit/reset the
database to obtain a new transaction. Inconsistent pending state or missing
transaction/fee evidence fails before preparation or broadcast. These checks
cannot reconstruct history already erased by older software or authenticate a
coherently rewritten database.

Already verified pending deposits/backfills and prepared sync work are drained
before OPTIONAL fresh beacon proof capture. Their pinned bridge identity,
provider runtime-code checks, unchanged finalized B3 light-client snapshot,
transaction/effect reconciliation, B3 finality and fee checks remain mandatory.
This does not accept an unverified proof, extend Ethereum fork support, or
permit a stale mint that B3 consensus rejects. Pristine optional sync work and
new deposit discovery still require independently verified fresh planning.
Dry-run mode never enters the live pending-plan submission path.

The source repair is covered by `test_relayer_recovery_preservation.py` plus
the existing relay, scheduling, cache and coalescing suites. It repairs relay
bookkeeping/liveness only: it does not release orphaned validator locks,
establish a safe conflicting-certificate recovery, renew an immutable expired
Ethereum verifier, or demonstrate deployed bridge operation. No consensus,
quorum, signer-journal or contract change is included.

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
on `cycle_failed`, repeated `job_waiting_finality` or
`job_waiting_conflict_resolution`, and a cursor that stops
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

## Regtest synthetic beacon

**REGTEST ONLY.** These pieces drive the unchanged relayer and the real B3
light client end to end on one machine: a local `anvil` (chain id 31337, no
`--fork-url`), the V1 contracts plus the regtest mocks in
`contracts/regtest/`, a synthetic sync committee over anvil's real execution
blocks, and a `-b3modernregtest -b3flowmeshtest` regtest node started with
`-b3bridgepins`. The committee secrets are derived from a public seed, so
nothing they sign means anything outside regtest. The regtest plan is in
`doc/design/b3-bridge-v2-regtest-plan.md`. The functional tests that use
them are `feature_b3_bridge_rt0_toolchain.py`,
`feature_b3_bridge_rt2_light_client.py`,
`feature_b3_bridge_rt3_v1_route_a.py --deposit-only` and, for bridge V2,
`feature_b3_bridge_rt4a_round_trip.py` and `feature_b3_bridge_rt4_kofn_gate.py`;
their harness lives in `test/functional/test_framework/{anvil,b3bridge}.py`.
The regtest-only refusals of the Python pieces are unit-tested offline by
`contrib/b3bridge/test_regtest_mock_beacon.py`,
`contrib/b3bridge/test_b3_bridge_observe_v2.py`,
`contrib/b3bridge/test_b3_bridge_deposit_guard_v2.py` (run `python3 -m unittest`
here) and by `TestFrameworkB3BridgeGuards` in
`test/functional/test_framework/b3bridge.py` (run by
`feature_framework_unit_tests.py`).

### `b3-bridge-regtest-beacon`

You build this tool with `-DB3_BRIDGE_TOOLS=ON`, like `b3-bridge-ethcheck`.
It reads one request file and prints JSON to standard output:

```sh
./bin/b3-bridge-regtest-beacon request.json > outputs.json
```

```json
{"version": 1,
 "genesis_validators_root": "0x<32 bytes>", "fork_version": "0xb3000001",
 "electra_epoch": 0, "slot_offset": S0, "base_block": B0,
 "headers": [<eth_getBlockByNumber results for B0, B0+1, ...>],
 "outputs": [{"kind": "bootstrap"},
             {"kind": "update", "finalized": N}]}
```

- Block N has slot S0 + N. The bootstrap is the header of B0, which is the
  pinned checkpoint.
- An update finalizes N. Block N+1 is the attested header, so its execution
  header must be in `headers`. The signature slot is the attested slot + 1,
  and all 512 members sign.
- The next committee is included when the attested and signature slots fall
  in the same period.
- Member i of the committee for period p has the BLS secret key
  `FromIKM(SHA256("B3-REGTEST-SYNC-COMMITTEE-V1" ‖ LE64(p) ‖ LE32(i)))`.
- Optional update fields build negative cases: `participants` (1-512),
  `next_committee`, `signature_slot`, `slot_shift`,
  `sign_genesis_validators_root`, `sign_fork_version` and
  `finalized_proposer_index`.

Each output carries the following fields:

- `data`: the Beacon API object;
- `payload_hex`: the `BOOTSTRAP`/`UPDATE` bridge record you pass to
  `submitbridgecarrier`;
- for a bootstrap: the checkpoint root, slot, block, block hash and period;
- for an update: the finalized root, slot, block and block hash, the
  attested and signature slots, the period, the participant count and
  `has_next_committee`.

The same headers always give byte-identical output. If the request is
malformed or refused, the tool exits 1 and writes `FAIL: ...` to standard
error.

### `regtest/mock_beacon.py`

This is a standard-library Beacon API server over the tool:

```sh
python3 contrib/b3bridge/regtest/mock_beacon.py --anvil http://127.0.0.1:8545 \
  --tool ./bin/b3-bridge-regtest-beacon --pins /abs/path/pins.json --port 5052
```

It checks that the pin file's checkpoint root equals the beacon header root
the tool derives for the checkpoint block on this anvil. It then follows the
anvil head and finalizes head-1, moved back to an earlier block when needed so
that the attested and signature slots stay in the finalized block's period.
It serves these endpoints under `/eth/v1/`:

- `beacon/genesis`;
- `config/fork_schedule`;
- `beacon/light_client/bootstrap/<root>`;
- `beacon/light_client/updates` (one best update per period, each with the
  next committee);
- `beacon/light_client/finality_update`;
- `beacon/headers/head`.

Tests import `SyntheticBeacon` and call `finalize_through(n)`,
`set_participants(period, k)` and `serve(port)` directly. Point the relayer at
it with `--beacon-url http://127.0.0.1:5052`. On regtest, `--ethereum-rpc
http://127.0.0.1:P` and `--ethereum-rpc-secondary http://localhost:P` are the
same anvil. They pass only the mechanical distinct-origin check and are not
independent providers.

### `b3_bridge_observe_v2.py` (bridge V2 kind 9 observer)

A one-shot, standard-library observer for the bridge V2 burn gate (node
changes rev 11 section 8). It reads `getbridgev2info.observation_plan`,
calls `eth_getProof(address, slots, anchor.number)` for each account of each
requested subject, in plan order, builds each record with
`buildbridgeobservationv2` and submits it with the wallet's
`submitbridgecarrier`:

```sh
python3 contrib/b3bridge/b3_bridge_observe_v2.py --ethereum-rpc http://127.0.0.1:8545 \
  --b3-rpc-url http://127.0.0.1:P --b3-cookie <datadir>/regtest/.cookie \
  --wallet relayer [--subjects 1,2,3] [--dry-run]
```

- Subjects: 1 VERIFIER_E (Verifier V2 liveness, set hashes, latest
  certificate), 2 RESERVE_E (Vault V2 `nextDepositId`, `locked`,
  `absorbedTotal` and the token's balance of the vault), 3 TOKEN_FLAGS
  (paused, deprecated, vault blacklisted). From a B-23 successor's migration
  height (node changes section 7.6) the plan also lists 18
  RESERVE_E_SUCCESSOR and 19 TOKEN_FLAGS_SUCCESSOR, the same reads of the
  successor vault. Without `--subjects` the observer submits every subject
  the plan lists, so 18 and 19 join 1-3 from the migration height without a
  flag (from then on the burn gate reads 1, 18 and 19, and a sweep's gate 1-3);
  `--subjects` narrows a round. A RESERVE_E_SUCCESSOR credits a vault sweep
  only once the successor's `absorb()` has counted it.
- The plan's anchor is the newest retained V2 execution anchor with a state
  root, so relay a light-client update for a recent block first; a record
  must name an anchor newer than its subject's latest observation.
- It refuses every endpoint but plain http on 127.0.0.1 or localhost (bridge
  V2 is configured only by the regtest pin file), a B3 chain other than
  regtest, an Ethereum chain id other than the plan's `origin_chain_id`, an
  anchor whose block hash differs on the Ethereum endpoint, a subject whose
  anchor is not newer, and proofs that do not answer for the plan's account
  and keys. Every subject is built before anything is submitted.
- It prints one JSON object (anchor, decoded values, verdicts, payloads and
  txids); a refusal prints `error: ...` and exits 1. The B3 wallet signs; no
  key is handled here.

### `b3_bridge_deposit_guard_v2.py` (bridge V2 deposit and flush guard)

Once B3 has closed bridge V2 burns for good (node changes rev 11 section
8.5: a sticky kind 13, E5 or E8 conflict, a broken fresh lineage, or a latest
VERIFIER_E showing the verifier halted, cancelled, expired or
`depositsClosed`), it still mints every proven Vault V2 deposit, but that
bUSD V2 can never be redeemed, so a relayer runs this one-shot,
standard-library guard (`python3 contrib/b3bridge/b3_bridge_deposit_guard_v2.py
--b3-rpc-url http://127.0.0.1:P --b3-cookie <datadir>/regtest/.cookie`, or
`deposit_guard(node)` from a deposit or flush path) before it deposits or
flushes. It refuses every endpoint but plain http on 127.0.0.1 or localhost,
a B3 chain other than regtest, and a node whose `getbridgev2info` is not
configured and active; then `getbridgev2info.deposits.status` OPEN prints
`{"deposits":"OPEN","next_height":n}` and exits 0, while CLOSED_FOR_GOOD
prints `error: bridge V2 deposits are closed for good: <reason>; this
relayer refuses deposits and flushes` and UNAVAILABLE (or anything else)
fails closed, both with exit 1. OPEN only means B3 has not closed deposits,
not that Verifier V2 `depositsOpen()` is true; the wallet-side counterpart
is `getbridgedepositrecipient`.

### Pin file (`-b3bridgepins=<absolute path>`)

`write_regtest_bridge_pins()` in the functional-test harness writes this file
from the live anvil deployment. The node reads at most 64 KiB and parses
strict JSON, so an unknown, duplicate or missing key is an error:

```json
{"format": "b3-regtest-bridge-pins/1",
 "v1_decentralized": {
  "origin_chain_id": 31337, "vault": "0x..", "token": "0x..",
  "origin_deployment_block": 3, "vault_runtime_code_hash": "0x..",
  "token_runtime_code_hash": "0x..", "max_deposit_raw": 1000000000,
  "max_per_block": 1000000000, "max_per_epoch": 10000000000,
  "light_client": {
   "trusted_checkpoint_root": "0x..", "trusted_checkpoint_slot": 20480,
   "checkpoint_execution_block": 3, "genesis_validators_root": "0x..",
   "fork_schedule": [{"activation_epoch": 0, "fork_version": "0xb3000001"}],
   "fork_schedule_valid_through_epoch": 1664, "electra_epoch": 0,
   "min_sync_committee_participants": 342, "max_sync_lag_slots": 8192},
  "verifier": "0x..", "verifier_runtime_code_hash": "0x..",
  "bootstrap_validator_set_hash": "0x..", "min_bridge_validators": 4,
  "max_bridge_validators": 64, "min_bridge_total_weight": 4,
  "max_epoch_lag": 86400}}
```

- Hex values are 0x-prefixed, in raw Ethereum byte order, as `getbridgeinfo`
  prints them.
- `max_per_block`, `max_per_epoch`, `min_sync_committee_participants` and
  `max_sync_lag_slots` are optional. Their defaults are 1e9, 1e10, 342 and
  8192.
- `electra_epoch` is required and must be `0`: the synthetic beacon builds
  Electra objects from genesis, so any other value could never bootstrap.
- The node fills the withdrawal-rules version and commitment from the V1
  constants.
- `token_runtime_code_hash` becomes the direct-token
  `implementation_or_adapter` pin.
- The node refuses the file unless all of these hold:
  - `origin_chain_id` is exactly 31337;
  - vault, token and verifier are non-zero and pairwise distinct;
  - deployment block ≤ checkpoint block ≤ deployment block + 20,000;
  - 0 < `max_deposit_raw` ≤ block cap ≤ epoch cap ≤ MAX_MONEY;
  - the genesis validators root is not Ethereum mainnet's;
  - every fork version starts with `0xb3`;
  - `electra_epoch` is 0;
  - the light-client and withdrawal pins are internally valid.
- With the file, bUSD runs in `decentralized-verifier-v1` mode. Activation
  and withdrawal heights stay at A3 (163).
- `getbridgeinfo` reports the pinned tuple.

### Regtest-only guards

- **Mainnet and test networks.** `-b3bridgepins` on main, testnet, testnet4
  or signet fails with `-b3bridgepins is regtest-only`. Without the option,
  every network's parameters are unchanged, including the default
  `-b3flowmeshtest` synthetic bridge. One deliberate, fail-closed difference
  is visible there: before this option existed, a `b3bridgepins=` line in a
  main or test-network `b3coin.conf` was ignored with an "unknown
  configuration value" warning; it now stops startup with the error above.
  (The other `-b3*` regtest knobs are still ignored there.)
- **Regtest without the test switches.** On regtest the option requires both
  `-b3modernregtest` and `-b3flowmeshtest`.
- **Changing the pin file.** A `-b3flowmeshtest` datadir records the pin
  file's SHA256 (or `none`) in `<datadir>/regtest/b3bridgepins.sha256`. A
  missing record counts as `none`, because a datadir older than the record
  was always judged without a pin file.
  - Once the block index holds any entry (block or header) at or above bUSD
    activation, a different pin file is an init error, even when the tip is
    still below activation: `-b3bridgepins differs from the pin file recorded
    for this datadir, which already stores blocks at or above bridge
    activation; start from a fresh -datadir`. A stored block may already
    have been judged, or rejected and flagged invalid, under the recorded
    file, and a later restart would not judge it again.
  - `-reindex` and `-reindex-chainstate` refuse a changed file at any height,
    before anything is wiped (`... -reindex and -reindex-chainstate cannot
    apply a changed pin file; start from a fresh -datadir`).
  - The record cannot tell a deleted record from a datadir that predates
    it: a datadir whose record was deleted is treated as judged without a
    pin file. Do not delete it; start from a fresh `-datadir` instead.
- **Tool and mock beacon.**
  - Both refuse the Ethereum mainnet genesis validators root, any fork version
    outside `0xb3`, and any `electra_epoch` other than 0.
  - The mock accepts only `http://127.0.0.1` or `http://localhost` endpoints,
    binds 127.0.0.1, and ignores HTTP proxies.
- **Mock contracts.** The mocks in `contracts/regtest/` are never imported by
  `contracts/src/` or `contracts/script/`.
- **Forge runs.** The harness runs `forge --offline` with `--out`,
  `--cache-path` and broadcast records under the build directory. The one
  exception is RT0's run of the production `Deploy.s.sol`: a forge script
  may write only where `contracts/foundry.toml`'s `fs_permissions` allow,
  so its candidate manifest goes to
  `contracts/deployments/local-rt0-<pid>.json` (gitignored) and is deleted
  afterwards. A killed run can leave that file behind.

### Deviations from the regtest plan

These are reported here rather than silently built in (solve-and-report):

- **Pin activation (plan section 3.3).** The plan says each added or changed
  pin takes effect above the tip, with activation heights in the pin file.
  The V1 pin file has no heights: every pin applies from bUSD activation
  (A3 = 163, unchanged from the default `-b3flowmeshtest` bridge), and a
  changed file is refused as described above. Height-scheduled re-pins
  (V entry, re-pins) are left to the V2 pin-file sections.
- **One clock (plan section 3.5).** The plan asks for one monotonic clock
  driving anvil and B3 mock time. The harness runs the corridor under mock
  time, then returns B3 to the wall clock and stakes continuously, as
  `feature_b3_finality_soak.py` does, because the four-validator BLS
  finality fixture is only proven live in real time. anvil mines on demand
  with wall-clock timestamps. B3 heights at each step therefore vary between
  runs (`bridge_evidence.json` records them), and RT2's relayer loop
  tolerates repeated tip or finality races (up to 29 identical failed cycles
  within 600 s). Clock jumps use mock time explicitly: RT0's one-day jump,
  RT2's freshness boundary and RT4's stale-observation drill, which runs
  after RT4a's release and reindex steps and then produces blocks under mock
  time, because a block staked 5,000 s ahead keeps the wall clock behind the
  chain for the rest of the test.
