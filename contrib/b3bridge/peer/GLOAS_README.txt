B3 Sepolia Gloas peer verification
================================

This additive testnet path does not change the mainnet v1 policy, journal,
collector, stale recovery limits or closed Arbitrum One registry. All command
diagnostics keep mainnet_enabled, arbitrum_authority and writes_enabled false.

Authority and availability
--------------------------
The native bridge::peer::gloas::LightClient pins Ethereum Sepolia chain 11155111,
its genesis validators root/time and the Gloas fork version/activation. It
verifies the trusted bootstrap's SSZ branches, sync committee BLS signatures,
finality/committee branches and durable journal chain. Participation remains
at least 342 of 512. Sync and recovery lag remain at most 8192 slots. The
compiled fork schedule has a finite reviewed horizon of epoch 365000; unknown
forks and stale checkpoints stop progress.

The initial recent weak-subjectivity beacon checkpoint is an external trust
input. Agreement between two HTTPS checkpoint operators is a cross-check,
not proof from Ethereum genesis. Record checkpoint provenance separately.
No saved JSON field such as "verified" grants authority.

Gloas authenticates an execution block HASH. State/receipt roots and block
number are obtained only by matching the exact raw Amsterdam execution-header
RLP preimage to that hash in the native verifier. A privately constructed
FinalizedExecution capability is bound to the owning native client, Sepolia
identity, policy digest, journal digest/sequence and current finalized header.
The Sepolia source verifier consumes this capability in the same process.
CLI diagnostic JSON cannot substitute for it.

The maintained Nimbus adapter is availability only. It is built against
Nimbus v26.10.1, commit 963756c91c19b7824cea57e7aab5857ce2c776c7 with exact pinned
submodules. The build rejects modified tracked sources and local shell
overrides, uses content-addressed new untracked adapter modules, and retains
old adapter modules/binaries. New executable content pins must be explicit.

Cold restart and bootstrap caches
---------------------------------
Public bootstrap caches can expire much sooner than the 8192-slot native
freshness limit. The optional --bootstrap-file accepts one bounded raw frame:

  {"kind":"bootstrap","data":{...Gloas LightClientBootstrap...}}

The frame is untrusted. Nimbus's maintained initialize_light_client_store
checks its header proof, committee proof and EXACT unchanged trusted root
before the collector seeds its store. B3 independently validates the emitted
same proof. A local cache never changes the policy checkpoint or bypasses a
stale recovery guard. A wrong root, altered branch or oversize frame is refused.
When a native journal already contains the bootstrap, the duplicate frame is
rejected without modifying journal state; valid later updates can continue.

Create a new explicitly labelled Sepolia store for a new policy. Do not reset
or replace existing journals to evade stale policy or bootstrap availability.
In particular, retained mainnet journals and finality checkpoints are separate
and must never be used by this path. Journal/policy files require private POSIX
ownership and durability; run under Linux/WSL.

Build and run
-------------
Build b3-bridge-gloascheck with the normal CMake build. Separately run:

  python3 build_gloas_peer_collector.py --nimbus-source /absolute/pinned/nimbus \
    --output /absolute/new-collector --report /absolute/new-build-report.json

Run the native consumer through its bounded owning launcher:

  python3 run_gloas_peer_light_client.py \
    --backend /absolute/new-collector --backend-sha256 LOWERCASE_SHA256 \
    --verifier /absolute/b3-bridge-gloascheck --verifier-sha256 LOWERCASE_SHA256 \
    --policy /absolute/private/sepolia-policy.json \
    --journal /absolute/private/gloas.journal --seconds 300 \
    --bootstrap-file /absolute/private/untrusted-bootstrap.json \
    --report /absolute/fresh-session-report.json \
    --private-log-dir /absolute/fresh-private-log-directory

The bootstrap-file and private-log-dir options are optional. The latter stores
at most 1 MiB per child in private files; public reports contain hashes/counts.
An execution_header input frame to b3-bridge-gloascheck carries only untrusted
raw header bytes; native matching is still mandatory. Its format is:

  {"kind":"execution_header","data":"0xRAW_RLP_HEX"}

Maintained Geth peer availability
--------------------------------
The separate ../execution_peer Go module uses pinned Geth v1.17.10 RLPx/ETH and
SNAP, with no Ethereum execution JSON-RPC endpoint. --hash requests an exact
raw EL header preimage. The header collector also supports bounded valid empty
SNAP responses for state it does not hold. This respects maintained extension
admission while advertising no invented synchronized execution head. Maintained
fork filters remain intact; sanitized drop counters expose availability failures. --witness-request accepts bounded data only:

  {"state_root":"0x32_BYTES","accounts":[
    {"address":"0x20_BYTES","storage_keys":["0x32_BYTES"]}]}

Alternatively supply execution_hash instead of state_root (never both). The
same physical ETH/SNAP connection fetches the exact expected raw EL header,
checks its hash and derives the state root before issuing proof queries. The
output includes the raw header for independent native Gloas preimage checks.
This avoids a second discovery round and helps keep recent state available.

The request root/hash is not source authority. The adapter verifies/trims range
proofs through maintained Geth trie.VerifyProof and outputs native-shaped
account/storage proof paths. The source verifier independently checks them
against the authenticated native execution capability. The adapter requires
a matching Sepolia ETH handshake on the SAME physical peer connection before
our SNAP proof queries. A reconnect cannot inherit prior readiness. A bounded
duplex reader answers early/interleaved incoming SNAP requests with valid empty
unavailable-data replies, so they cannot block ETH status delivery. Failed
readers release the gate promptly; unsolicited/duplicate responses cannot fill
its queue. Complete received replies remain usable if EOF immediately follows.

Bounds are 16 accounts, 65 requested slots per account, 128 total slots,
64 nodes per proof, 8 KiB per node, 32 MiB of SNAP input per peer and
32 MiB of encoded witness output,
1 MiB per SNAP message, 256 messages per peer and a 1..300-second deadline.
The Sepolia registry's 17 accounts use separate groups of 16 and 1.
SNAP peers may not serve a root outside their recent state retention window.
Missing/censored/pruned data causes failure; it never authorizes a source.

Validation
-----------
Native Gloas tests exercise actual BLS verification, SSZ branches, rotation,
domain separation, RLP preimage binding, owner/current/freshness capability
checks, strict JSON and journal tamper/storage fences. Original v1 tests remain.
Python boundary tests:

  python3 -m unittest test_gloas_adapter.py test_peer_adapter.py

Go tests exercise trie inclusion/absence/tamper, strict request bounds, ordered
proof trimming, actual ETH/SNAP loopback negotiation and wrong-network gating:

  cd ../execution_peer
  go test -count=1 ./...
  go test -race -count=1 ./...

An observed P2P bootstrap failure must remain visible even if a later cache
restart succeeds. A cached Beacon REST bootstrap followed by a verified P2P
update qualifies the update's acquisition transport, not a pure-P2P initial
checkpoint/bootstrap or complete RPC-free Arbitrum history. Mainnet remains
disabled until all independent production release gates pass.

Primary references
------------------
https://github.com/eth-clients/sepolia/blob/main/metadata/config.yaml
https://github.com/ethereum/consensus-specs/blob/master/specs/gloas/light-client/sync-protocol.md
https://github.com/ethereum/go-ethereum/blob/v1.17.10/eth/protocols/snap/protocol.go
https://github.com/ethereum/go-ethereum/blob/v1.17.10/core/types/block.go
https://eth-clients.github.io/checkpoint-sync-endpoints/#sepolia
