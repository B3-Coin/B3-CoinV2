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

- The release workflow is green for Linux x86_64 (Qt and static headless),
  Windows x86_64, macOS arm64, and macOS x86_64. Automated Win32 publication
  is deferred for this release.
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
      has a bounded type-10 bootstrap/update/mint/backfill/decentralized-burn
      carrier, exact `BRIDGE_BACKED` OWNER mint and bUSD BURN transitions,
      nullifier/cap accounting, undo, reindex replay, and mempool/miner/asset
      wiring. Every type-10 transaction has exactly one zero-value policy-9
      `BRIDGE_RECORD` metadata cell committing the
      `B3/BRIDGE/RECORD/V1` tagged hash of the canonical frame; tests reject
      missing, duplicate, mismatched, and orphan cells. Withdrawal consensus
      requires exact ECDSA `SIGHASH_ALL` or Schnorr
      `SIGHASH_DEFAULT`/`SIGHASH_ALL` on every input, rejecting `NONE`,
      `SINGLE`, and `ANYONECANPAY`; this covers the binding without
      `OP_RETURN` or a custom sighash. Its bridge index is rebuilt in memory
      from activation and has
      no durable sidecar, so a configured bridge must continue to refuse
      pruning and any snapshot that skips bridge history.
      The historical managed-v1 vault is not a valid production pin.
- [ ] The immutable decentralized stack is deployed in the order prover,
      verifier, then keyless vault. The final mined manifest proves all three
      runtime hashes and constructor pins, the new vault's first-code block,
      canonical USDT, deposit cap, bootstrap set hash, and exact derived
      AssetId. Recheck that the historical managed vault has zero liabilities
      and that no bUSD was activated against it; otherwise stop for migration.
- [ ] A later reviewed B3 build pins the complete audited deployment tuple:
      Ethereum chain ID, verifier/vault addresses and runtime code hashes,
      vault deployment block, canonical USDT address, and derived B3 AssetId.
      Missing or mismatching fields keep the bridge fail-closed.
      The manifest's `vault_runtime_code_hash` maps exactly to
      `BridgeAssetParams::vault_runtime_code_hash`; `getbridgeinfo` must expose
      the same value and it must match the live Ethereum `extcodehash`.
- [ ] The four public bootstrap identities reproduce one synthetic equal-weight
      header, and at least three sign the exact M-1/Set0 handoff. The one-time
      initialization calldata reproduces independently and lands before the
      immutable deadline. No bootstrap private key enters a deployment file.
- [ ] The normal outbound workflow is rehearsed: a finalized B3 certificate's
      bitmap/aggregate/absent-member paths reproduce `submitCertificate`
      calldata; `getbridgewithdrawalproof <confirmed_txid> <burn_vout>
      [certificate_block]` resolves only the confirmed active-chain burn and
      its cumulative depth-32 path into `release` calldata. The wallet never
      exposes a provisional withdrawal id/leaf before confirmation. Duplicate
      release is rejected. The permissionless Ethereum submitter pays gas and
      holds its own key outside the B3 node.
- [ ] The USDT adapter commitment has a reviewed preimage and consensus-enforced
      applicability/upgrade rule; a nonzero parameter alone is not enforcement.
- [ ] Reproducible evidence byte-matches the deployed vault runtime to the
      reviewed source with exact compiler/settings/constructor immutables, and
      the contract plus bridge verification stack have passed the required
      independent audits/fuzz gates.
- [ ] Deposits remain disabled until initialization and a fresh valid
      certificate prove qualified current and successor sets. If inbound B is
      enabled before outbound W, publish the custodial-waiting warning: bUSD
      can be minted but cannot yet be burned for Ethereum release. W remains
      unset unless canonicality and liveness safety for the complete round trip
      is solved, independently reviewed, rehearsed, and explicitly enabled.

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
busd_bridge.activation_height = B (inbound; only after the final manifest/pins)
bridge_withdrawal_activation_height = unset (W; later reviewed release, W >= B)
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

### Mandatory data-directory migration

`CDiskBlockIndex` contains appended, unversioned B3 transition fields. A node
whose data directory was written by the legacy client or an incompatible
pre-transition build must perform one full `-reindex` on first transition-line
startup. Current transition-beta indexes already use this layout and must not
be needlessly rebuilt during the short corridor. Full reindex reuses the raw
block files and rebuilds both the block index and chainstate;
`-reindex-chainstate` is not sufficient for an incompatible index. Release
notes, operator checks, and the startup error must give this exact distinction.

The repaired release has a separate one-time chainstate safety gate. If the
current transition-beta block index and coins database already contain blocks
after H, startup must level-4 disconnect/reconnect every block H+1 through the
coins tip under the current `ConnectBlock` rules and synchronously write a
versioned marker into that coins database only after all pass. The marker value
must equal the exact `DB_BEST_BLOCK`; patched `BatchWrite` advances both in one
atomic database batch. It must fail closed on missing/pruned data, insufficient
cache, interruption, validation failure, or marker-write failure. A
fresh/pre-H database, or a chainstate that this same patched binary has just
wiped for rebuild, may be marked immediately because no old post-H state
remains. This check must never be described as requiring a full reindex of a
compatible transition-beta database. If an older binary advances or replaces
the chainstate, its marker is absent/stale on return and the check runs again.
One migration exception is an old active tip which is now provably off the
pinned X branch. Startup first revokes any pre-pin marker, verifies that the
block and undo data are readable, and uses the existing undo records to unwind
it to the anchor before selecting the canonical branch. Canonical blocks
connected by that recovery pass use the repaired `ConnectBlock` rules; the next
startup (or an explicit verification pass) then performs the full post-H
reconnect and writes a new marker. An on-anchor invalid tip is never given this
exception and must stop startup.
Mainnet publishes no AssumeUTXO snapshot. Any future post-H snapshot support
must carry an explicit B3 validation commitment; until then such a snapshot is
unsupported rather than eligible to inherit this marker.

After the rebuild, a wallet owning a historical FN recipient key must discover
the block-810,001 output. Test both an ordinary rescan and a BASIC-filter-index
rescan; post-H B3A1 carrier blocks may not be skipped using only the embedded
owner suffix. The operator fallback is `rescanblockchain 810001`.

### Mandatory live STAKE/asset-owner scan

The v1.1.0 rules make P2SH, witness, and nested B3-policy owner suffixes
invalid in a B3S1 STAKE carrier. B3A1 asset owners also reject any nested B3
policy carrier.
P2SH and witness are special only when they are the complete authorization
script; an extra carrier layer can prevent their intended key check from
running. The built-in Stake page and `createstake` use safe legacy P2PKH, and
FN Genesis uses ruled P2PKH owners, but the corridor was already live when this
audit fix was made. Before tagging, run the following against a fully
synchronized node and require `unsafe=0`. Any unsafe result means an
already-confirmed block would disagree with v1.1.0 and the release must stop
for an explicit compatibility decision.

```python
import json
import subprocess

def cli(*args):
    raw = subprocess.check_output(["b3coin-cli", *map(str, args)], text=True)
    return json.loads(raw)

def first_push(raw):
    if not raw:
        return None
    opcode = raw[0]
    cursor = 1
    if opcode <= 75:
        size = opcode
    elif opcode == 76 and len(raw) >= 2:
        size, cursor = raw[1], 2
    elif opcode == 77 and len(raw) >= 3:
        size, cursor = int.from_bytes(raw[1:3], "little"), 3
    elif opcode == 78 and len(raw) >= 5:
        size, cursor = int.from_bytes(raw[1:5], "little"), 5
    else:
        return None
    end = cursor + size
    if end > len(raw):
        return None
    return raw[cursor:end], end

def carrier(raw):
    pushed = first_push(raw)
    if pushed is None:
        return None
    payload, end = pushed
    if end >= len(raw) or raw[end] != 0x75:  # OP_DROP
        return None
    return payload, raw[end + 1:]

MAGICS = (b"B3A1", b"B3S1", b"B3MC")

def claims_b3_carrier(raw):
    pushed = first_push(raw)
    return pushed is not None and pushed[0][:4] in MAGICS

def claims_asset_carrier(raw):
    if claims_b3_carrier(raw) and first_push(raw)[0][:4] == b"B3A1":
        return True
    return raw[:1] == b"\x6a" and claims_b3_carrier(raw[1:]) \
        and first_push(raw[1:])[0][:4] == b"B3A1"

tip = cli("getblockcount")
stake_count = 0
asset_count = 0
unsafe = []
for height in range(810001, tip + 1):
    block = cli("getblock", cli("getblockhash", height), 2)
    for tx in block["tx"]:
        for output in tx["vout"]:
            raw = bytes.fromhex(output["scriptPubKey"]["hex"])
            parsed = carrier(raw)
            if parsed is None:
                continue
            payload, owner = parsed
            if payload[:4] == b"B3S1" and len(payload) == 38:
                stake_count += 1
                p2sh = len(owner) == 23 and owner[:2] == b"\xa9\x14" and owner[-1] == 0x87
                witness = (
                    4 <= len(owner) <= 42
                    and (owner[0] == 0 or 0x51 <= owner[0] <= 0x60)
                    and 2 <= owner[1] <= 40
                    and len(owner) == owner[1] + 2
                )
                if p2sh or witness or claims_b3_carrier(owner):
                    unsafe.append((height, tx["txid"], output["n"], "stake", owner.hex()))
            elif payload[:4] == b"B3A1":
                asset_count += 1
                # OP_FALSE is the exact keyless BURN/vault terminator (and is
                # harmless as a bare owner). Direct P2SH/witness asset owners
                # are safe because B3A1 explicitly unwraps one owner layer.
                if owner != b"\x00" and (
                    claims_b3_carrier(owner) or claims_asset_carrier(owner)
                ):
                    unsafe.append((height, tx["txid"], output["n"], "asset", owner.hex()))

print(
    f"tip={tip} stake_outputs={stake_count} asset_outputs={asset_count} "
    f"unsafe={len(unsafe)}"
)
for item in unsafe:
    print(item)
raise SystemExit(bool(unsafe))
```

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

The published baseline remains `v1.1.0`; its tag, version, notes, and artifacts
are immutable and `doc/release-notes-v1.1.0.md` stays a historical record of
that exact source. This bridge/consensus candidate now uses source version
`v1.1.1` and the matching release-notes filename, but that exact version and
commit still need owner approval before tagging. Preserve the beta.1,
beta.2, beta.3, and v1.1.0 notes unchanged. Confirm the guard tests use exact
value-by-value assertions for X, R0, manifest checksum/count/root, FN genesis
height, A1, A2, A3, fee, and every Modern PoS parameter.

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
   arm each initial validator's finality signer before its first checkpoint is
   signable, preserve `<network-datadir>/finality_signer` with the wallet, and
   never operate the same validator/BLS key from two independent datadirs or
   machines; a wallet backup without its live signer journal is not a safe
   active-validator migration;
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
3. The `v1.1.0` tag and release are already published and immutable. These
   post-v1.1.0 consensus/contract changes use the owner-proposed `v1.1.1`; do
   not move, recreate, or upload replacement assets to v1.1.0. After the owner
   approves the exact v1.1.1 commit, tag it, push
   the release branch and tag, and let CI build all five package variants,
   including the static headless Linux operator package and Windows x86-64.
   Win32 is deferred from this automated release; any later upload must be built
   from the exact tag, independently architecture/runtime checked, and
   checksummed. A manual
   **Run workflow** build produces downloadable Actions artifacts for testing
   only; it does not create or update a GitHub Release. The publish job runs
   only for a pushed tag that exactly matches the source version.
   Extended GitHub qualification is the default. If the exact tagged commit has
   already completed the full local unit, Qt, functional, Solidity, relayer,
   and packaging qualification and GitHub time is the only constraint, a
   repository administrator may explicitly set the Actions variable
   `B3_SKIP_EXTENDED_RELEASE_TESTS_SHA` to that tag's exact 40-character commit
   id. The workflow emits a visible warning and still requires every platform
   build/checksum job. Save the local command logs and unset the variable
   immediately afterward; the SHA binding prevents this exception from
   silently carrying over to a later source revision.
4. Attach binaries and SHA-256 checksums to each release. Publish X prominently
   with the old-client verification command `getblockhash 810000`.
5. Publish the full FN manifest, checksum, R, and rights root beside the release
   so anyone can independently reproduce them.
6. Publish and sign an update manifest only if §0's updater prerequisites are
   actually met. Otherwise give manual download links and say installation is
   manual.
7. Announce the honest pause duration, required upgrade, FN Genesis in block
   810,001, 30-block maturity, and the later A1/A2/A3 feature heights.

## 7. After adoption

The first valid block on X is corridor block 810,001 and must contain FN
Genesis. Native-B3 STAKE outputs and validator-authorized public BLS bindings
may be included in that same block and throughout corridor blocks
810,001..811,000. Set0 is the exact snapshot after block 811,000 and must have
at least two qualified members; otherwise Modern PoS cannot start. Modern PoS
begins at M = 811,001. A two- or three-member Set0 can finalize B3 and initialize
the Ethereum verifier, but cannot authorize bridge withdrawals: both the
current and successor canonical sets must have at least four members, meet the
pinned minimum total weight, and satisfy the verifier's freshness rules.
Modern FN PoD creation activates at A1 = 812,000.
Simple-v1 colored assets and FlowMesh seat/vault preparation activate at
A2 = 813,000. Full FlowMesh spot trading and settlement activate at
A3 = 815,000, after a 2,000-block preparation runway. Each market nevertheless
waits for its own unique earliest canonical block at or after
`market.created_height` whose post-block FN set has at least four seats, and
sequence zero starts only once that block is 30-deep. No post-adoption FN
rights scan or list selection exists; those were completed and pinned before
the tag.

Do not announce bUSD as active merely because the Ethereum contracts are
deployed. The vault may be deployed before M, but deposits remain disabled
until initialization and a fresh valid certificate prove qualified current
and successor sets, and until the later B3 build's complete deployment pins
match. There are no corridor deposits. A3 = 815,000 activates FlowMesh trading
and never activates the bridge. The production checkpoint,
fork schedule, caps, approval interval, adapter/rules commitments, deployment
manifest, verifier/vault runtime evidence, bootstrap handoff, audits, and
outbound calldata rehearsal must all pass.

If that later pin enables inbound B while outbound W remains unset, Ethereum
deposits may mint bUSD after contract readiness, but every B3 withdrawal record
is consensus-invalid. This intentional waiting period must be disclosed; do
not describe the bridge as two-way until W is pinned and active.

The historical managed vault is immutable and remains callable, but it is not
the production registry. Because it had no observed liability and B3 bridge
minting never activated, the planned new vault is a pre-activation replacement.
Recheck that statement immediately before pinning. If it is false, apply the
full old-registry cutoff, late-deposit handling, reserve/liability reconciliation,
and burn/swap/reissue migration instead.
