# Stage 0 R1 — inbound bridge boundary and unchanged-contract evidence

Status: read-only source/public-state verification, 2026-09-20. No B3 node,
wallet, VPS, signer, pin, contract or configuration was changed. Proposed
transition requirements below are not an implemented or approved transition.

The inspected V1 source is commit
`0b930e303e4c2c6bc28beb3bf656488b636ad49d`. The original Stage 0 freeze is
`f327bd2cc334282eec17a09c1d15d04f68b7f4c5`; its code is the same V1 baseline.
The original audit remains historical evidence; this appendix corrects and
qualifies its inbound-horizon wording without changing that audit.

## BR-1. Which source/release configurations contain the bound?

The mainnet pin was introduced by
[`6d34cb9030b51e58ba9fa280398105e8c865691a`](https://github.com/B3-Coin/B3-CoinV2/commit/6d34cb9030b51e58ba9fa280398105e8c865691a).
Inspection of each following Git object confirms mainnet
`fork_schedule_valid_through_epoch = 479999`, with bridge activation 811001.
The `1000000` value elsewhere in chainparams belongs to a generated regtest
configuration, not a mainnet extension.

| Inspected label | Peeled source commit | Mainnet bound |
|---|---|---:|
| v1.1.1 | `d34fe33a569e884a14b32b5fe3a6141e4ad8f187` | 479999 |
| v1.1.2 | `7c4bf72a371510b168aca6f870c83934001519b3` | 479999 |
| v1.1.3 | `bcd340858167432db294d79474108a23c06f8445` | 479999 |
| v1.1.4 | `b8457dba57298bd377deb1fbb9e2bf091aeb47dc` | 479999 |
| preview-1.1.5-dev-20260915-sync344404 | `201de005059008034579a8e8c3433f38a809540b` | 479999 |
| preview-1.1.5-dev-20260916-sync344404 | `515220ff1da7d5dc651b19ffead9786f44614b30` | 479999 |
| reviewed V1 integration | `0b930e303e4c2c6bc28beb3bf656488b636ad49d` | 479999 |

Read-only GitHub `ls-remote` confirmed v1.1.1–v1.1.4 and the September 16
preview tag objects/peeled commits against these local identities, and confirmed
the published reviewed-integration branch at `0b930e3`. The September 15 label
was inspected locally only. v1.1.0 source `d30a730a3f00fa52a9b9eda31861ee292b871545`
does not contain this active mainnet bound. These are source/tag findings, not
proof that any particular holder or server runs those bytes. Binary asset
hashes, individual installations and all other refs were not inventoried.

Sources: [mainnet pins](https://github.com/B3-Coin/B3-CoinV2/blob/0b930e303e4c2c6bc28beb3bf656488b636ad49d/src/kernel/chainparams.cpp#L227),
[v1.1.1 pins](https://github.com/B3-Coin/B3-CoinV2/blob/d34fe33a569e884a14b32b5fe3a6141e4ad8f187/src/kernel/chainparams.cpp#L212),
[release manifest](https://github.com/B3-Coin/B3-CoinV2/blob/0b930e303e4c2c6bc28beb3bf656488b636ad49d/contracts/deployments/ethereum-mainnet-v1.1.1.json).

## BR-2. Exact update boundary; the recorded date is not a blanket outage time

Let `Emax=479999`, `S=32*(Emax+1)=15360000`.
The node requires all three predicates:

1. `signature_slot > 0` and `floor((signature_slot-1)/32) <= Emax`;
2. `floor(attested.beacon.slot/32) <= Emax`;
3. `floor(finalized.beacon.slot/32) <= Emax`.

Failure precedes light-client `ProcessUpdate` and returns
`bridge light-client update uses an unpinned Ethereum fork`. Passing this guard
alone is not a valid update: signatures, branches, monotonicity, committee
transitions, non-no-op behavior and backfill bounds still apply.

| Field/event | Last permitted by this guard | First refused by this guard |
|---|---:|---:|
| Attested/finalized header slot | 15359999 | 15360000 |
| Nonzero signature slot | 15360000 | 15360001 |
| Header epoch | 479999 | 480000 |

The signature boundary differs because the signing domain uses
`signature_slot-1`. In particular, a signature at slot 15360000 over otherwise
supported older headers is not rejected merely for being at that boundary.
An attested header at slot 15360000 is rejected even if another field is old.
This limits the pinned known-fork validation envelope; it is not evidence of
an Ethereum hard fork scheduled at epoch 480000.

Ethereum's pinned metadata gives genesis time `1606824023` and 12-second slots.
Therefore:

- Slot 15359999 begins **2026-10-04T20:00:11Z**.
- Slot 15360000 begins **2026-10-04T20:00:23Z**.

The retained manifest's `operational_horizon_end_utc` is the latter boundary,
not the start timestamp of the last supported header slot. A genuine finalized
update may stop advancing earlier than that arithmetic maximum; this calculation
does not assert a finality update exists for every permitted slot.

Sources: [fork predicates](https://github.com/B3-Coin/B3-CoinV2/blob/0b930e303e4c2c6bc28beb3bf656488b636ad49d/src/node/bridge_state.cpp#L120),
[update admission](https://github.com/B3-Coin/B3-CoinV2/blob/0b930e303e4c2c6bc28beb3bf656488b636ad49d/src/node/bridge_state.cpp#L440),
[Ethereum genesis metadata](https://github.com/eth-clients/mainnet/blob/858a24808915672eb23760a34f16203652aeb35a/README.md#L10),
[Ethereum slot configuration](https://github.com/eth-clients/mainnet/blob/858a24808915672eb23760a34f16203652aeb35a/metadata/config.yaml#L65).

## BR-3. Mint freshness and retained deposits

Let `T` be the candidate **B3 block timestamp**, and `F` the stored finalized
Ethereum header's **execution timestamp**. Mainnet `max_sync_lag_slots=8192`.
`FinalizedHeadFresh` accepts nonnegative `T` when `T<=F`, or when
`T-F<=8192*12=98304` seconds. Equality is accepted; `98305` is refused. The mint
also requires the stored finalized beacon slot to be inside the pinned fork
envelope. These conditions are separate from an RPC receipt, UI clock or local
wall time. They are not a test that the latest public Ethereum tip was observed.

Thus unsupported future updates do not instantly erase old anchors or minted
balances. A later mint may still validate while the retained head is within
the exact freshness bound and its deposit proof, caps, recipient and nullifier
checks pass. Once that candidate-time freshness condition fails, even an old
otherwise valid deposit proof is refused until an authorized supported head
is available. No precise live mint-stop time is established without the actual
node's accepted head, pins and candidate block time.

Deposit identity/nullification is `(origin_chain_id, vault_address, deposit_id)`;
it does not contain the registry ID. The block delta records the nullifier only
with a fully admitted mint and exact authorized output. Replay protection and
reorg undo therefore remain tied to the same deposit, not to a replacement
registry name. An unminted Ethereum deposit remains a claim requiring evidence;
neither a refused proof nor a new registry authorizes another deposit/credit.

Sources: [freshness function](https://github.com/B3-Coin/B3-CoinV2/blob/0b930e303e4c2c6bc28beb3bf656488b636ad49d/src/node/bridge_state.cpp#L140),
[mint validation](https://github.com/B3-Coin/B3-CoinV2/blob/0b930e303e4c2c6bc28beb3bf656488b636ad49d/src/node/bridge_state.cpp#L535),
[deposit nullifier construction](https://github.com/B3-Coin/B3-CoinV2/blob/0b930e303e4c2c6bc28beb3bf656488b636ad49d/src/bridge/admission.h#L268).

## BR-4. Supported transition today versus requirements for a future one

**No implemented general node-side registry/fork-pin transition path was found
in the inspected V1 baseline.** It materializes one `params.busd_bridge`
registry, not a height-indexed history of approved intervals. Bootstrap refuses
an already-bootstrapped light client. There is no operator RPC/configuration
override that legitimately extends consensus fork support.

`BridgeAssetIdV1` deliberately excludes these safety pins, but
`BridgeRegistryIdV1` includes checkpoint, fork list, maximum epoch, lag and other
approval values. Changing them therefore changes the registry ID. Simply
replacing chainparams would make historical mint/burn records fail their old
registry-ID checks during replay; changing activation to skip the old history
would not preserve its balances, nullifiers or withdrawal obligations.

Any later approved extension must therefore specify and test, before deployment:

1. Height-indexed old/new registry validity and an authenticated activation
   authority. Historical blocks continue using their original registry and pins.
2. Verified Ethereum fork domains/header schema support, not just a larger epoch
   integer. A checkpoint replacement needs explicit authenticated continuity;
   a fresh bootstrap must not silently replace an existing store.
3. Continuity of the same reserve AssetId, light-client/anchor provenance,
   all deposit nullifiers and epoch mint accounting, cumulative withdrawal IDs,
   roots, pending burns and consumed claims. Registry rotation cannot reset them.
4. Handling of pending old-registry transaction bytes and unknown outcomes.
   Reconcile the original identity first. Any newly authorized proof envelope
   must prove the same deposit, never create a replacement entitlement or silently
   rewrite an existing signed instruction.
5. Old/new boundary replay, reorg and restart tests, including duplicate deposit
   submission across the boundary and already-spent withdrawal receipts.

This is a future consensus-approval gate, not permission to patch mainnet pins
now. The future B3 upgrade can be designed without changing Ethereum's contract,
but unchanged balances/contract addresses alone do not prove compatibility.

Sources: [asset and registry identity](https://github.com/B3-Coin/B3-CoinV2/blob/0b930e303e4c2c6bc28beb3bf656488b636ad49d/src/modern/bridge_asset.h#L20),
[single configured registry](https://github.com/B3-Coin/B3-CoinV2/blob/0b930e303e4c2c6bc28beb3bf656488b636ad49d/src/bridge/admission.h#L276),
[bootstrap refusal](https://github.com/B3-Coin/B3-CoinV2/blob/0b930e303e4c2c6bc28beb3bf656488b636ad49d/src/node/bridge_state.cpp#L406),
[history replay](https://github.com/B3-Coin/B3-CoinV2/blob/0b930e303e4c2c6bc28beb3bf656488b636ad49d/src/node/bridge_state.cpp#L1725).

## BR-5. Public deployed evidence, pinned to one finalized Ethereum block

Read-only public JSON-RPC, no credentials or transaction submission:

- Primary provider: `https://eth.drpc.org`; `eth_chainId` returned `0x1`.
- `eth_getBlockByNumber("finalized",false)` selected block **26018233**
  (`0x18d01b9`), hash
  `0xa19115f987fc9b94f878e8e3dc022678e179a9a74a0feecbfe9eeae47fd03bf7`.
- Timestamp `1789901399` (`0x6aafba57`), **2026-09-20T10:49:59Z**.
- All following `eth_getCode` and `eth_call` requests used that explicit block
  number, not `latest`. The calls below are getter signatures; no signer was used.
- Independent provider `https://ethereum-rpc.publicnode.com` returned the same
  block hash/time and repeated `depositViable()`, `lastCertificateTime()` and
  `currentSetHash()` with identical results. This is two-provider corroboration
  of those observations, not an independently verified execution/state proof.
- A batched primary-provider request returned HTTP500. Sequential bounded reads
  succeeded; the failure is not evidence about contract validity or readiness.

### Runtime hashes

`eth_getCode(address,"0x18d01b9")`, then local Keccak-256:

| Contract | Address | Bytes | Runtime Keccak-256 |
|---|---|---:|---|
| Vault | `0x077839b12cebfbF163acAEAC3A59A015D100c64b` | 3516 | `0xdb267712887568bffd394e46538bddba01da11cefc38e32b2428c00911237f8d` |
| Verifier | `0xE72B3Fe73F0d42A6e964D33E7BB1cc2EA7a3F690` | 9994 | `0xafdba8befb1aacc832bff4e08dcd92e6645a012ea8a8088b0f2811d916022902` |
| Prover | `0x8e612aE4D475d25940E2A2FC907F21b6813eedA7` | 7759 | `0x77d2aea2d2a6842fae8b29e64a146622e2f45e772a6c351640ffe8362211a959` |
| USDT | `0xdAC17F958D2ee523a2206206994597C13D831ec7` | 11075 | `0xb44fb4e949d0f78f87f79ee46428f23a2a5713ce6fc6e0beb3dda78c2ac1ea55` |

All four match the retained deployment manifest. This newly establishes
**runtime-to-manifest equality at this block**. It does not newly reproduce
Solidity compilation, constructor/immutable linking or the full source-to-bytecode
provenance recorded by that manifest.

### Verifier state

| Getter signature | Observed value |
|---|---|
| `initialized()(bool)` | true |
| `currentEpoch()(uint64)` | 1 |
| `currentSetHash()(bytes32)` | `0x3ad66153b83118b9c5dacd21c69dcfe6d24f5e17867c6c03741d9f5e57d4d76d` |
| `nextSetHash()(bytes32)` | `0xded3b28ef817cde52b190a994a2f25388daa5df81c1963e2affc6c80ce40359e` |
| `lastRotationTime()(uint256)` | 1788887519 (2026-09-08T17:11:59Z) |
| `lastCertificateTime()(uint256)` | 1788887519 |
| `GENESIS_TIME()(uint256)` | 1788523511 (2026-09-04T12:05:11Z) |
| `MIN_EPOCH_DURATION()(uint256)` | 86400 |
| `MAX_EPOCH_LAG()(uint256)` | 2592000 |
| `MAX_CERTIFICATE_AGE()(uint256)` | 86400 |
| `MIN_DEPOSIT_EXIT_WINDOW()(uint256)` | 604800 |
| `latestBridgeFinalizedHeight()(uint64)` | 813771 |
| `latestBridgeWithdrawalRoot()(bytes32)` | `0x27ae5ba08d7291c96c8cbddcc148bf48a6d68c7974b94356f53754ef6171d757` |
| `depositViable()(bool)` | false |
| `releaseReady()(bool)` | true |

`latest()(uint64,bytes32,bytes32,bytes32,uint64)` returned height `813771`,
block hash `0xc9e6a033270c3429aad71818e7e4d6cfab3567138353bd8fd353e907d829e9d5`,
the withdrawal root above, successor hash `0xded3b28e...40359e`, and epoch `1`.

`currentSet()` and `nextSet()` were decoded as
`(uint64,uint16,uint32,uint64,uint64,bytes,bytes32)`:

| Field | Current | Successor |
|---|---|---|
| epoch / ruleset | 1 / 1 | 2 / 1 |
| count | 17 | 34 |
| total weight | 58782918 | 126108063 |
| quorum weight | 39188613 | 84072043 |
| aggregate key | `0x818c1c57b4b7a0ac1803047d06f7a5725713307eb7bbf0bfd2ef29cc0e49869583dd27a6549daa81df0d1023e07ffd21` | `0xb4c24a0399b5469c24e4b6ef0705ab1022f98c6bf9db3f4583cd286503bd3be6d2f88de30e99cfcc126ba16f14ab0153` |
| members root | `0xc422faa1a987b3826f0418a25d45c21b15d87457211301198734af5130a8e2d5` | `0x03d392535ce40685fcf8898f8714daba7eb1e72ae5db5f6cff08aaf5411776fe` |

### Vault state and narrow interpretation

At the vault address, getters returned:

- `nextDepositId()(uint64) = 1`;
- `locked()(uint256) = 3000000` (raw six-decimal token units);
- `verifier()(address)` equals the verifier address above;
- `ORIGIN_TOKEN()(address)` equals USDT above;
- `B3_ASSET_ID()(bytes32) = 0xc69c6bd581c3188fe80d97cf9946f34c79ec502ccf204cd597dc9366315d61ad`.

At the pinned block, certificate age is `1013880` seconds, exceeding the
observed `MAX_CERTIFICATE_AGE=86400`. This alone is a sufficient source-level
reason for `depositViable=false`; the getter independently confirms the false
result. This is an actual contract-read observation, not a UI inference.
The same age is **less than** `MAX_EPOCH_LAG=2592000`: it does not establish
permanent lineage expiry. `releaseReady=true` means previously accepted qualified
roots retain release authority; it does not prove any requested withdrawal has
an accepted leaf, unused receipt, exact proof, sufficient reserve or B3 burn.

The October inbound fork-pin boundary is **not the cause** of this September
contract freshness result. The verifier cannot observe B3's Ethereum light-client
head or its fork-pin horizon. Conversely, the vault's retained locked amount
does not establish whether the corresponding deposit has minted on B3.

Sources: [deposit readiness and retained release authority](https://github.com/B3-Coin/B3-CoinV2/blob/0b930e303e4c2c6bc28beb3bf656488b636ad49d/contracts/src/B3FinalityVerifier.sol#L298),
[certificate lineage checks](https://github.com/B3-Coin/B3-CoinV2/blob/0b930e303e4c2c6bc28beb3bf656488b636ad49d/contracts/src/B3FinalityVerifier.sol#L190),
[vault deposit](https://github.com/B3-Coin/B3-CoinV2/blob/0b930e303e4c2c6bc28beb3bf656488b636ad49d/contracts/src/B3StakerBridge.sol#L95).

## BR-EXT. Exact remaining evidence and qualification gate

The public runtime and recognized-lineage observations above narrow the earlier
evidence gap. The following remain unestablished; they are listed once here:

1. Actual approved B3 installation binary/source identity, selected chainparams,
   current accepted Ethereum light-client head, anchor provenance and candidate
   times. No live B3/VPS inspection was performed. The retained manifest is
   labelled `production_approved:false`; it is not current approval evidence.
2. Newly reproduced source/compiler/settings/constructor/immutable-to-runtime
   equivalence. Runtime hashes matched retained pins, but no compiler rebuild
   or full constructor-input verification was performed in this pass.
3. Authenticated B3 ancestry and complete historical certificate/member evidence
   connecting the observed Ethereum current/successor sets to a proposed PoS V2
   bootstrap, plus availability of all required old export quorums. Contract
   getters do not prove present B3 finality health or extinguish issued signatures.
4. Already-issued but unrelayed signatures, all pending withdrawals, consumed
   receipts, and the B3 nullifier/mint status of retained Ethereum deposits.
   No private signer journals or wallet data were inspected.
5. An approved, implemented and tested height-versioned inbound registry/fork
   transition with verified supported Ethereum rules. No such path was found
   in the inspected V1 implementation.

Consequently, source-level adapter compatibility remains a design candidate;
end-to-end V2 compatibility/activation is not qualified. Neither BFT agreement
nor a new binary can erase V1 obligations, reset the Ethereum verifier's lineage,
or make an expired immutable lineage accept a later certificate.

## Proposed isolated bridge boundary tests (not executed here)

- Header slots `S-1/S`; signature slots `S/S+1`; zero signature slot; all other
  proof validity held constant; separate guard rejection from proof rejection.
- Freshness `T=F`, `F+98304`, `F+98305`; negative candidate time; old supported
  deposit proof under fresh versus stale latest store; unsupported stored head.
- Original deposit duplicate before/after a synthetic registry interval boundary;
  preserve nullifier identity and exact output recipient/amount.
- Old block replay under old registry, first new block under new registry;
  restart and reorg around the boundary; no replacement bootstrap of a live store.
- Preserve cumulative burn IDs/withdrawal roots and consumed claims across that
  synthetic boundary. Reject a fresh registry attempting to reset them.
- Independently test Ethereum certificate-age, rotation-age and absolute-epoch
  clocks; show they are unrelated to the inbound Ethereum fork-support integer.

These are proposed tests for a separately approved bridge transition stage, not
prerequisites for the pure shared-spot accounting model and not new test passes.
