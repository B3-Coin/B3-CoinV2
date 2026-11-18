Owned full Nitro settlement laboratory
====================================

This laboratory runs a real deployed Nitro rollup, calldata batch posting,
MakeNodes assertion posting/confirmation, and a separate WASM validation
service against an owned Geth development parent. It preserves every runtime
directory and container. It never invokes upstream test-node --init, Docker
Compose teardown, volume removal, host PID mode, or a Docker socket bind.

Trust scope
-----------
L1 is chain1337 using Geth's development SimulatedBeacon clock. This is NOT
Ethereum PoS finality and is NOT public Arbitrum Sepolia or Arbitrum One.
L2 is chain412346. All keys/funds are public upstream development fixtures.
Mainnet remains disabled. The exporter is untrusted availability and cannot
mint a source authority, enable mainnet, or change the native deployment policy.

The five roles are geth, sequencer, poster, validator, validation_node. Their
combined hard allocation is12GiB RAM/eight CPUs; their chain data has a64GiB
monitor limit. Ports are published only to127.0.0.1. The fresh private Docker
bridge disables IP masquerading. Config and keystore mounts are read-only;
container roots are read-only, all capabilities dropped, PID limit512, logs
bounded. No Redis/HA promise is made: one sequencer uses the explicit upstream
no-coordinator development mode. All local L1 DA is calldata; blobs and public
Ethereum beacon/blob availability are separate public-network work.

Preparation
-----------
Use Linux with local Docker. Create separate private root-owned asset/runtime
parents; the runtime parent's final component must be B3-peer-work. Images in
provenance.json must already exist by exact digest. Prepare before starting
the full stack; helper builds are capped4GiB/two CPUs and refuse to run beside
other owned settlement roles.

  sudo python3 prepare.py --assets-parent /absolute/private/assets-parent \
      --forge /absolute/pinned/foundry-v1.8.5/forge
  sudo python3 prepare.py --verify /absolute/new/assets
  sudo python3 runtime.py start --runtime-parent /absolute/private/B3-peer-work \
      --assets /absolute/new/assets --port 18550 --serve-seconds 28800
The monitor must remain running while the laptop's WSL instance is needed.
It checks original chain identity, container health and disk use, and stops
only its exact owned containers at the bounded deadline or on failure.

  sudo python3 runtime.py status --runtime /absolute/runtime
  sudo python3 runtime.py resume --runtime /absolute/runtime --serve-seconds 28800
  sudo python3 runtime.py stop --runtime /absolute/runtime

Resume starts the same original containers and chain stores, verifies both
original genesis hashes, and never reinitializes or resets a journal.

Fresh starts use validator feed.input.url=[] from genesis and refuse assumed
staked validation. A real delayed Inbox deposit funds the public funnel's L2
gas balance before the remaining heavy services start. The separate WASM
service has one JIT and one arbitrator worker. These future-start changes have
boundary tests; the preserved runtime below predates the feed change.

Raw producer API
----------------
  from export_witness import export_settled
  export_settled(runtime, fresh_output,
      previous_height=height, previous_hash="0x...",
      to_height=end, to_hash="0x...",
      accounts=[{"address":"0x...", "storage_keys":["0x..."]}])

The CLI accepts --runtime, --output, --previous-height, --previous-hash,
--to-height, --to-hash and optional --accounts JSON array. One selected range
contains at most128 blocks and sixteen account requests, each with at most64
exact storage keys. The current confirmed head can be much newer than the
selected range; ancestry uses at most64 pages of128 raw headers.

Outputs:
* manifest.json: owned chain/genesis, exact requested range, canonical L1
  checkpoint/header, actual latestConfirmed assertion creation bytes, account
  object references, validator observation and all authority flags false.
* objects/<sha256>: bounded raw bytes; SHA256 is storage addressing only.
* l1-header.rlp and l2-settled-header.rlp: actual raw header preimages.
* settlement-proof.bin: B3ARPR01 assertion preimage, first at most16 exact L1
  MPT account/storage witnesses and settled L2 header; no L2 state witnesses.
* settlement-group-1.bin: second B3ARPR01 frame only if a seventeenth distinct
  L1 account exists. Each frame retains the existing sixteen-account limit.
* history-proof.bin: B3ARPR01 complete selected block/receipt/transaction range
  newest-first, neutral assertion/empty L1 accounts, and exact endpoint state
  witnesses in confirmed_accounts (the codec's newest SELECTED block field).
  The historical_accounts field would refer to the oldest selected block and
  is empty. The JSON historical_accounts records name the range endpoint data.
* ancestor-pages.json: rawheader references starting at parent of settled head
  and ending at the selected endpoint, matching native WalkBack semantics.
* deployment-policy.json: observed code/configuration bytes and exact compiler
  storageLayout. This is evidence to compare with native independently pinned
  policy; it is never a JSON allowlist accepted as source authority.

The full owned graph has seventeen named roles but sixteen distinct addresses:
its governance-proxy-admin and core-proxy-admin are the same contract. The
public Sepolia deployment has separate admins and requires sixteen+one groups.
The producer records this alias explicitly instead of duplicating addresses.
Governance executes the real pinned upgrade-executor dependency artifact.
The equivalent inherited storage declarations in compiled UpgradeExecutorMock
are used solely as layout metadata, and are labelled accordingly in policy.

Actual bounded run,2026-10-25
----------------------------
Preserved runtime:
  /home/josh/B3-nitro-settlement/B3-peer-work/settlement-20261025-150332-c15146d5
Preserved built assets:
  /home/josh/B3-nitro-settlement-assets/20261025-a3d96f808fc9
L1 genesis:
  0x2dfffa2bf9ea07523fafd6f0a75c7f26b650c02cf813ee9ba26d43614d55cbdf
L2 genesis:
  0x96e374992910099c426be5abdb92a663135a264a8de2c75642fc6f5966bb2536
Ports: L118550, L2 sequencer18551, validator18552, poster18553.
Rollup:0x47BbFA1e1ef9eB48c6b7F74c53afc6A29Afb22e1, L1 deployment block133.
An actual MakeNodes assertion was posted and confirmed by time on L1.
arb_latestValidated returned nonempty WasmRoots containing the exact pinned
module root and canonical L2 block10:
  0x455a20b79db0e0856056a692b2b0767dabdcd1f6fa15d85177a57124fa62067d
  Batch3, PosInBatch0.
The real delayed Inbox deposit of100 fixture ETH arrived on L2, L1 transaction:
  0x8b8d4576c4cc8ea83ef0bfe2dc5392b701f7e63db425bf5c4e4ee43a22006f4e
export-bootstrap-04 exported the complete owned graph plus range0->8 under
settled head10,154899 raw object bytes/220 objects. Its two ancestor headers
bridge block10 to block8. This preserved validator initially consumed the
sequencer feed; this observation proves actual WASM execution and contract
settlement, and does not claim L1-only replay from genesis.

Checks
------
  python3 -B -m unittest discover -s contrib/b3bridge/nitro_settlement -p 'test_*.py'
Twenty-one focused boundary checks pass, covering caps, assumed-validation and
feed bypasses, live chain relabels, read-only acquisition, symlinks/duplicate
JSON, exact proof keys/account/header preimages, source archive traversal,
source hash substitution, and exact proxy slots. Existing built assets pass
prepare.py --verify. Native proof verification and the USDT0 deposit/trade/
withdraw qualification are performed by the parent regtest integration; Python
producer success alone is never that qualification.

The separate full-settlement runner uses the same USDT0/native economics
sequence and real 300-second custody delay as the existing standalone lab,
with an explicit full-rollup adapter and a new private settlement-gated seat.
It does not relax the old standalone --dev provenance checks. Before deposits,
it captures one immutable full code/configuration baseline and domain-binds
its SHA256 plus both original genesis hashes in the deployment manifest. Every
observation and vote rechecks the native owned settlement/history gate against
that baseline and an independently fetched owned L1 canonical checkpoint.
Every private seat retains/fsyncs the whole raw bundle. On restart, the source
cursor must equal the SourceRangeFact decoded from the native committed body;
an actual altered-cursor restart negative is part of the run.

Example full run on this laptop (the evidence directory must be new):

  sudo python3 /home/josh/B3-rebirth/contrib/b3bridge/nitro_settlement/run_roundtrip.py \
      --source-root /home/josh/B3-rebirth \
      --worker /home/josh/B3-builds/rebirth-20261018T065433Z-xaWC1q/bin/fm2-regtest-node \
      --native-checker /home/josh/B3-builds/rebirth-20261018T065433Z-xaWC1q/bin/b3-bridge-owned-arbitrumcheck \
      --runtime /home/josh/B3-nitro-settlement/B3-peer-work/settlement-20261025-150332-c15146d5 \
      --tools /home/josh/B3-tools/foundry-v1.8.5 \
      --solc /home/josh/B3-tools/solc-0.8.35 \
      --b3coind /home/josh/B3-builds/regtest-b3-node/bin/b3coind \
      --b3coin-cli /home/josh/B3-builds/regtest-b3-node/bin/b3coin-cli \
      --evidence-dir /home/josh/B3-evidence/full-owned-settlement-20261025-v1

Actual full run PASSED, 2026-10-25
--------------------------------
Evidence:
  /home/josh/B3-evidence/full-owned-settlement-20261025-v1/roundtrip-report.json
  /home/josh/B3-evidence/full-owned-settlement-20261025-v1.log
  /home/josh/B3-evidence/full-owned-settlement-20261025-v1/source-inventory-during-freeze.json
  /home/josh/B3-evidence/full-owned-settlement-20261025-v1/full-runtime-binding.json

All four actual deposits, both native maker/taker markets and the actual
5 USDT0 custody withdrawal passed. Source users retained zero ETH and no token
allowance. Each of four private seats independently checked five complete
settlement/history ranges: 20 successful native owned-settlement checks.
The actual custody delay was 301.480919 seconds. The recipient received exactly
5,000,000 raw USDT0 units; quote vault actual=locked=liabilities=105,000,000,
pending=0, and protocol stable fees=2,200 raw units (1,200 trading + 1,000
withdrawal service). Both synthetic vaults reconciled at 10,000,000 raw units.
The payout transaction is:
  0xa47daa4e2d4a54d46f28d107defc4bc70690b8286b9827e4677fa846813e7189

Original private-seat restart, altered proof/omitted event refusal, removed B3
anchor refusal, insufficient quorum and an actual changed-cursor journal
reopen refusal passed. The preserved full validator input cutover occurred at
L2 height 11; subsequent E2E blocks were derived from posted L1 batches and
actually WASM validated. This does not claim L1-only replay from genesis.

Five local commit samples measured 417.45-625.89 ms from direct-coordinator
client submission through all four durable native replies. These measurements
exclude L1 settlement waits and WAN latency; they do not qualify public DEX
speed or a production bridge SLA.

Source, worker and checker bytes were identical before/after the run. The
complete 4,647-file source inventory SHA256 was:
  4d3877f65a4cacd9e75b2d5152ba9db534628e7ad778ef95a75ef09cc231903d
The immutable owned baseline policy SHA256 was:
  dce0b6d98cb214ceb7762f926fbea00c635cd1b9c7ebca0ec169a256dc99ab45
The standalone native qualification passed 22 actual cases in a disconnected
network namespace:
  /home/josh/B3-evidence/owned-settlement-native-20261025-v3/report.json

The original v1 report retains a legacy unscoped l1_settlement_verified=false
from the standalone lab and the new owned_l1_settlement_verified=true field.
The current new-format runner removes that ambiguous inherited field and
reports public_ethereum_pos_verified=false and
public_arbitrum_settlement_verified=false explicitly. Owned Geth-dev L1
settlement is distinct from public Ethereum PoS/public Arbitrum qualification.

Public history feasibility
--------------------------
Official Nitro derives canonical history from L1 batches, not ordinary L2
execution peer history. Archive HashDB is needed for local historical state
proofs; PathDB does not support this block validator profile. For new public
test deposits, archive since a verified bootstrap can suffice; genesis archive
is not automatically required. No public snapshot size was verified here and
no multi-hundred-GB snapshot download was started. The machine measured about
843GB free before work. Public Sepolia native PoS authority, public finalized
contract proofs and public DA/history availability remain separate from this
owned local trust scope. Exact primary sources are in provenance.json.
Official Sepolia snapshot pointer requests for pruned/archive/full-path/
archive-path returned HTTP 403, error1010; no snapshot archive was downloaded.
Public history/DA bootstrap therefore remains unqualified on this laptop.
