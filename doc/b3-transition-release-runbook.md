# B3 Transition Release Runbook

The ordered procedure for turning the sealed block-810,000 history into the
transition release. The release pins X, R0, and the complete FN rights
manifest/count/root; requires FN Genesis in the first corridor coinbase; and
carries modern FN PoD, simple-v1 colored assets, and FlowMesh v1 fail-closed
until their later post-M heights A1, A2, and A3.

Expect an honest 2–4 week seal pause for final measurement, pinning, independent
review, packaging, and real-history shadow-fork rehearsal. Do not describe it
as a days-long automatic update.

Implementation state (2026-09-01): the transition branch contains the FN
manifest/genesis, B3A1 authorization, modern-FN PoD, simple-v1 asset, A1/A2
gating, FlowMesh A2 preparation/A3 trading gates, dedicated authenticated
microblock transport, persistent checkpoint/vault state, serialization, index,
miner, mempool, wallet-signing, RPC, and four-node release-test paths. The seal
and owner pinning are now complete:

```
X                     = 2413ba59476afb9a01b971c350b2c5a51494b37925055be42dde774f30d865c6
S_H (base units)      = 1042617596101695152
R0 (base units)       = 19836712254
FN rights count       = 3592
FN rights root        = e8f282a7dcaa9a8fbcfcc5c22ba4f456e5b50968fcf899aaacdaca65bef898ec
FN artifact SHA-256   = c80470eec785600f33fa2e69c520ff331c2b354ebf6e0a9bf8cae7d1eb5f9dca
A1 / A2 / A3          = 812000 / 813000 / 815000
```

The old client, port client, and fresh replay produced equal final-H UTXO
sets, and two independent manifest runs produced byte-identical artifacts.
Bridge-backed bUSD remains separately fail-closed pending its security pins.

## 0. Preconditions before the seal

- The release workflow is green for Linux x86_64, Windows x86_64, Windows x86,
  macOS arm64, and macOS x86_64.
- A fully synced B3 Hive node and the independent legacy-client snapshot needed
  by the three-way equivalence protocol are ready to freeze exactly at H.
- The canonical FN manifest codec, row predicate, Merkle construction, report
  command, and test vectors are implemented and dry-run on a copied datadir
  through a recent tip. Do not design the format after the seal measurement.
- The isolated shadow-fork harness uses different network magic and ports,
  localhost-only peers, overridable test H, and trivial corridor difficulty. It
  must be physically unable to contact mainnet.
- Update installation remains manual unless the tagged source contains reviewed
  manifest URLs and threshold public keys and every package has tested installer
  support. Private release keys remain offline.

## 1. Read the seal at block 810,000

Freeze the port node exactly at H; do not measure from an advanced tip:

```
b3coin-cli getblockcount        -> 810000
b3coin-cli getblockhash 810000  -> X
b3coin-cli gettxoutsetinfo      -> bestblock = X, total_amount = S_H
```

X must match `getblockhash 810000` from at least one independently run old
client before it is pinned.

### Mandatory final-H equivalence gate

Follow [b3-utxo-equivalence.md](design/b3-utxo-equivalence.md) at
`T = (810,000, X)` and preserve `master-H.rows`, `port-H.rows`, and
`replay-H.rows`, their SHA-256 checksums, row counts, commitments, commands,
logs, and direct process exit statuses. The required result is:

```
U_master(H, X) == U_port(H, X) == U_replay(H, X)
```

Any mismatch in a row, commitment, height, X, or exit status stops the release.

## 2. Measure the sealed constants during the pause

Compute R0 with integer arithmetic only:

```
R0 = floor(S_H_base_units * 1% / 525,600)
   = floor(S_H_base_units / 52,560,000)
```

Build the final historical FN manifest according to
[b3-legacy-fn-issuance-proposal.md](design/b3-legacy-fn-issuance-proposal.md).
On each independent, clean `(H, X)` view, run the equivalence verifier with
the manifest export enabled (the output target must not already exist):

```
./build/bin/b3coin-utxo-verify \
  -datadir=<clean-stopped-datadir-a> \
  -workdir=<fresh-scratch-a> \
  -height=810000 -hash=<X> -podreport \
  -masterrows=<seal-packet>/master-H.rows \
  -fnmanifest=<seal-packet>/fn-genesis-a.bin
```

Repeat as producer B with separate datadir, scratch, log, and output paths.
Capture the command's direct exit status. Exit 0 is required. The tool refuses
to export unless replay/equivalence and manifest construction succeed, refuses
to overwrite an existing artifact, and prints:

```
FN Genesis eligible: R
FN Genesis version:  1
FN Genesis height:   810001
FN chain domain:     <CHAIN_DOMAIN>
FN rights root:      <FN_RIGHTS_ROOT>
three-way result:   EQUAL (U_master == U_port == U_replay)
FN manifest file:    <path>
FN manifest bytes:   <91 + 52 * R, printed as a decimal integer>
FN manifest SHA256:  <64 lowercase hex>
```

The checksum is ordinary SHA-256 in standard byte order and must equal an
external `sha256sum` (or `shasum -a 256`) of the file. The canonical binary
layout is:

```
ASCII "b3-fn-genesis/v1\n" (17 bytes)
chain_domain                 (32 raw serialization bytes)
fn_genesis_height            (u32 big-endian)
manifest_version             (u16 big-endian)
row_count                    (u32 big-endian)
rights_root                  (32 raw serialization bytes)
for each row in raw pod_id order:
    pod_id                   (32 raw serialization bytes)
    recipient_key_hash       (20 bytes)
```

The exporter recomputes the consensus rights root from this exact context and
row sequence and stops on any mismatch. Compare the two independent artifacts
directly, preserve both direct exit statuses, and require:

```
cmp -s <seal-packet>/fn-genesis-a.bin <seal-packet>/fn-genesis-b.bin
sha256sum <seal-packet>/fn-genesis-a.bin <seal-packet>/fn-genesis-b.bin
```

On a host without `sha256sum`, use `shasum -a 256`. The required result is:

```
manifest_a bytes == manifest_b bytes
count_a = count_b = R
root_a  = root_b  = FN_RIGHTS_ROOT
R <= 5,000
```

The transition release pins all three FN artifacts: the complete manifest
bytes, R, and the root. A root alone is insufficient. Publish the manifest,
file checksum, count, root, commands, logs, chain identity, and exit statuses in
the seal packet.

Treasury script for `SNyANHiUkuqPSfbeKHDXzVD86LC2ZUUjLX`:

```
76a91412602418ffc74640e37f1a73d0cdc255d2a07c3588ac
```

### Seal packet checklist

- [x] Every capture source reports H = 810,000 and identical 64-hex X.
- [x] `gettxoutsetinfo` reports `bestblock = X`.
- [x] S_H, its exact base-unit conversion, and R0 are recorded without floating
      point.
- [x] Final-H artifacts prove three-way UTXO equivalence with direct exit 0.
- [x] Independent FN manifest runs are byte-identical and reproduce R and root.
- [x] The canonical manifest, checksum, R, and root are embedded and pinned.
- [x] Every manifest row satisfies the historical predicate and exact P2PKH
      recipient rule; no rows are aggregated.
- [x] R is at most 5,000.
- [x] Every other mainnet consensus field, including exact post-M heights A1
      (modern FN PoD), A2 (simple-v1 assets plus FlowMesh preparation), and A3
      (FlowMesh trading/settlement), has an owner ruling. A3 is at least 30
      blocks after A2. Missing values stop the tag.
- [ ] If bUSD is to activate in this release, every bridge readiness pin and
      implementation gate is independently reviewed and complete. The tree now
      has a bounded type-10 bootstrap/update/mint/backfill/managed-withdrawal
      carrier, exact `BRIDGE_BACKED` OWNER mint and bUSD BURN transitions,
      nullifier/cap accounting, undo, reindex replay, and mempool/miner/asset
      wiring. Every type-10 transaction has exactly one zero-value policy-9
      `BRIDGE_RECORD` metadata cell committing the
      `B3/BRIDGE/RECORD/V1` tagged hash of the canonical frame; tests reject
      missing, duplicate, mismatched, and orphan cells. Managed-withdrawal
      consensus requires exact ECDSA `SIGHASH_ALL` or Schnorr
      `SIGHASH_DEFAULT`/`SIGHASH_ALL` on every input, rejecting `NONE`,
      `SINGLE`, and `ANYONECANPAY`; this covers the binding without
      `OP_RETURN` or a custom sighash. Its bridge index is rebuilt in memory
      from activation and has
      no durable sidecar, so a configured bridge must continue to refuse
      pruning and any snapshot that skips bridge history.
      The managed-v1 vault is exactly
      `0x143F207e23e6aebD7E974be90ac6D434f4c7BFb6`; otherwise bUSD remains
      explicitly fail-closed at tag time.
- [ ] Managed-v1 operating rules commit to one workflow: a confirmed B3 burn
      request binds canonical bUSD, exact raw amount, Ethereum recipient and a
      unique request id; after the pinned finality depth the operator releases
      registry-derived USDT exactly once, durably consumes the id, and
      reconciles reserve against supply. No confirmed burn means no release.
- [ ] The operator-side Ethereum release service and durable request-consumption
      database implement that workflow, including crash-safe retry/recovery.
      The B3 node records the canonical burn request but does not call the vault
      or record an Ethereum release.
- [ ] The USDT adapter commitment has a reviewed preimage and consensus-enforced
      applicability/upgrade rule; a nonzero parameter alone is not enforcement.
- [ ] Reproducible evidence byte-matches the deployed vault runtime to the
      reviewed source with exact compiler/settings/constructor immutables, and
      the contract plus bridge verification stack have passed the required
      independent audits/fuzz gates.

## 3. Pin the transition release

In mainnet consensus parameters, confirm the already-fixed height and pin the
seal-derived or owner-selected values:

```
legacy_final_hash       = 2413ba59476afb9a01b971c350b2c5a51494b37925055be42dde774f30d865c6
modern_pos.reward       = 19836712254 base units
fn_genesis_manifest_version = 1
fn_genesis_manifest     = embedded canonical artifact (count = 3592)
fn_genesis_rights_root  = e8f282a7dcaa9a8fbcfcc5c22ba4f456e5b50968fcf899aaacdaca65bef898ec
hard_fork_height        = 810001 (also the fixed FN Genesis height)
fn_pod_activation_height = 812000 (A1)
asset_activation_height  = 813000 (A2)
flowmesh_activation_height = 815000 (A3; 2,000-block preparation runway)
asset_issuance_fee      = 1,000 B3
```

The FN manifest sequence is mandatory in the coinbase of block 810,001. FN
transfers have no separate height lock; ordinary 30-block coinbase maturity is
the only initial delay. Modern FN PoD creation remains invalid before A1,
asset issuance and FlowMesh preparation remain invalid before A2, and full
FlowMesh processing remains invalid before A3.

After pinning, `getassetstate` must report the schedule as configured, the exact
FN asset id and historical count, the selected A1/A2/A3 heights, and the
correct next-slot disintegration tier. Preserve its output in the release
packet.
`getwalletassets` must find historical FN outputs after rescan/import, while
`issueasset`, `sendasset`, `burnasset`, and `createfncoin` must construct, fund,
sign, and optionally broadcast their respective actions without treating asset
units as native B3.

Construct the entire Modern PoS block explicitly; never rely on its
fees-only/empty-treasury defaults:

```
block_interval_seconds = 60
round_seconds          = 30
f0_num                  = 1
f0_den                  = 1
sentinel_bits           = 0x207fffff
max_future_seconds      = 120
reward                 = R0
halving_interval       = 525,600
treasury_percent       = 10
treasury_script        = script above
reorg_horizon          = 1,440
finality_epoch_blocks  = 1,440
checkpoint_interval    = 10
checkpoint_depth       = 12
max_epoch_extension    = 10,080
min_finality_set       = 2
```

Immediately assert `modern_pos.Valid()` and add value-by-value mainnet tests
for every field above. A default-constructed or partially initialized block is
a release failure even when it happens to pass structural validation.

`min_stake_amount = 333 B3`, corridor bits `0x1f008000`, H, M, and the
corridor length are already pinned. Never infer a missing value during release
construction.

## 4. Implemented release surface and required verification

The branch contains the production wiring below. Before tagging, review it and
require passing tests for:

- exact manifest parsing, count/root recomputation, and source-byte pinning;
- mandatory block-810,001 coinbase FN outputs in manifest order;
- rejection of omitted, inserted, reordered, aggregated, or redirected FN
  genesis outputs and rejection of genesis at any other height;
- ordinary coinbase maturity with no additional FN transfer lock;
- actual owner-script signature authorization over the complete modern spend;
- FN conservation, modern capacity `5,000 - R`, and 15k/30k/60k modern PoD
  tiers with destroyed B3 excluded from producer fees and fail-closed before A1;
- permanent semantic rejection of retired FN action types 1 and 2;
- simple-v1 fixed genesis, no re-mint path, deterministic chain-bound AssetId,
  1,000 B3 treasury fee, and fail-closed before A2;
- canonical B3A1 `PolicyType::BURN` outputs, with no asset/FN `OP_RETURN`
  carrier and no reopening of issuance capacity;
- activation, mempool, block-template, connect/disconnect, reorg, restart,
  reindex, replay, wallet rescan, and late legacy-key import behavior; and
- `getassetstate`, `getwalletassets`, `issueasset`, `sendasset`, `burnasset`,
  and `createfncoin`, including non-broadcast construction and native
  fee/change accounting; and
- A2 seat binding and vault preparation; at or after A3, sequence-zero
  bootstrap from each market's unique earliest canonical block at or after
  `market.created_height` whose post-block FN set first has at least four
  seats, once that anchor is 30 blocks deep; dedicated authenticated
  microblock relay, exact spot clearing/fee allocation, typed checkpoint and
  vault-proof publication, custody conservation, seat handoff, shallow reorg
  recovery, restart convergence, and fail-safe market pause;
- withdrawal admission that keeps pending obligations within the deterministic
  largest-64 live pool-UTXO capacity, payout input selection by amount
  descending then outpoint, and the sequential publisher rule: publish one,
  confirm it, refresh capacity, rebuild, then publish the next;
- after every ordinary slot, a deterministic maximal partial treasury flush of
  `min(accrued treasury available, anchored native capacity minus existing
  pending native withdrawals)` when positive; zero capacity never blocks
  trading;
- the four-node FlowMesh release test from A2 through deposits, trading,
  treasury/user withdrawals, mixed-boundary settlement, and restart with
  identical state roots and balances; and
- all existing boundary, replay, corridor, staking, Modern PoS, and finality
  suites.

The old 4,000-byte historical proof-size measurement is not a release gate.
Its claim/proof builders and verifiers have been removed. Only the numeric
type/version registry entries 1 and 2 remain permanently inactive and rejected,
so previously assigned bytes can never acquire a new meaning.

Confirm `CLIENT_VERSION_MAJOR/MINOR/BUILD` remains `1/1/0`. For the operator
beta, `CLIENT_VERSION_PRERELEASE` must be `beta.1` and
`doc/release-notes-v1.1.0-beta.1.md` must be present. Before the final release,
clear `CLIENT_VERSION_PRERELEASE` and confirm
`doc/release-notes-v1.1.0.md` remains present and accurate. Flip the guard tests from
the fail-closed pre-pin shape to exact value-by-value assertions for X, R0,
manifest checksum/count/root, FN genesis height, A1, A2, A3, fee, and every
Modern PoS parameter.

## 5. Mandatory shadow-fork rehearsal

Using the isolated harness and a copied real-history datadir, run 2–3 clients
through:

1. the test seal and independent manifest reproduction;
2. first-corridor-block mining with the exact FN Genesis coinbase;
3. rejection of mutated genesis coinbases;
4. rejection of FN spends before coinbase maturity and acceptance at maturity;
5. modern PoD rejection before A1 and price/cap/fee accounting at A1;
6. before block 811,000, prove that its exact post-block snapshot contains at
   least two mature native-B3 stakes, each with a valid validator-authorized
   BLS binding and nonzero finality weight; do not approach M without this
   preflight, because there is no safe late bootstrap;
7. the corridor-to-Modern-PoS boundary;
8. simple-v1 asset rejection before A2, then activation and the 1,000 B3
   treasury fee at A2;
9. FlowMesh seat/deposit preparation at A2, rejection of trading before A3,
   then sequence-zero bootstrap from the unique earliest eligible market
   anchor once 30-deep, spot trade, checkpoint, vault sweeps and handoff;
10. top-64 withdrawal admission, amount-descending/outpoint payout selection,
   sequential publish-confirm-refresh/rebuild, and maximal partial treasury
   flush without blocking trading at zero capacity;
11. exact custody/supply/state-root agreement across all nodes after restart;
   and
12. restart, reindex, disconnect/reconnect, and wallet rescan.

Preserve configs, commands, logs, block hashes, artifact hashes, and test exit
statuses. Any unexplained divergence stops the release.

## 6. Tag and ship

1. Commit and review the measured pins and production wiring.
2. Complete the full local build, mandatory suites, and shadow-fork gate.
3. To publish the operator beta, tag `v1.1.0-beta.1`, push the release branch
   and tag, and let CI build all five package variants, including the static
   headless Linux operator package and Windows x86-64. Win32 is deferred from
   this automated release; any later upload must be built from the exact tag,
   independently architecture/runtime checked, and checksummed. CI must mark
   this release as a prerelease and must not make it the latest stable release.
4. After every release gate passes, clear `CLIENT_VERSION_PRERELEASE`, rebuild
   and reverify the packages, then tag `v1.1.0`. Push the final tag and let CI
   publish it as the latest stable release.
5. Attach binaries and SHA-256 checksums to each release. Publish X prominently
   with the old-client verification command `getblockhash 810000`.
6. Publish the full FN manifest, checksum, R, and rights root beside the release
   so anyone can independently reproduce them.
7. Publish and sign an update manifest only if §0's updater prerequisites are
   actually met. Otherwise give manual download links and say installation is
   manual.
8. Announce the honest pause duration, required upgrade, FN Genesis in block
   810,001, 30-block maturity, and the later A1/A2/A3 feature heights.

## 7. After adoption

The first valid block on X is corridor block 810,001 and must contain FN
Genesis. Native-B3 STAKE outputs and validator-authorized public BLS bindings
may be included in that same block and throughout corridor blocks
810,001..811,000. Set0 is the exact snapshot after block 811,000 and must have
at least two qualified members; otherwise Modern PoS cannot start. Modern PoS
begins at M = 811,001. Modern FN PoD creation activates at A1 = 812,000.
Simple-v1 colored assets and FlowMesh seat/vault preparation activate at
A2 = 813,000. Full FlowMesh spot trading and settlement activate at
A3 = 815,000, after a 2,000-block preparation runway. Each market nevertheless
waits for its own unique earliest canonical block at or after
`market.created_height` whose post-block FN set has at least four seats, and
sequence zero starts only once that block is 30-deep. No post-adoption FN
rights scan or list selection exists; those were completed and pinned before
the tag.

Do not announce bUSD as active merely because its canonical Ethereum-USDT
identity and managed-v1 vault facts are present. Bridge minting stays
fail-closed even though the consensus carrier, mint/burn checks, and replay
state are implemented. The USDT adapter still must be enforced; the production
checkpoint, fork schedule, caps, activation, rules commitment, X-dependent
parameters, authority/runtime evidence, audits, and operator withdrawal service
must all pass the bridge readiness gate.

Do not describe a future verifier as an in-place upgrade. The managed vault is
immutable and remains callable after retirement. Under the current identity
formula a new vault has a new `AssetId`; migration therefore needs a pinned old
registry cutoff, finality drain window, late-deposit refund/handling process,
burn/swap/reissue of old bUSD, reserve movement without double minting, and all
new vault/verifier/identity pins.
