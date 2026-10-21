B3 Ethereum peer proof foundation (read-only, bridge activation disabled)
=====================================================================

This is a small Ethereum consensus peer adapter, not an Arbitrum light client.
It gets complete bootstrap, sync-committee update and finality proofs through
Nimbus's existing Discovery v5/libp2p Req/Resp and gossip implementation. The
B3 native verifier independently checks SSZ branches and aggregate BLS, then
fsyncs its journal before exposing an Ethereum finalized execution anchor.
Peer identities, Nimbus status, sequencer signatures and Python booleans never
authorize a deposit. No chain transaction API or execution RPC is configured.

Backend pin and supported interfaces
------------------------------------
Nimbus consensus release v26.10.1, published 2026-10-16:
  https://github.com/status-im/nimbus-eth2/releases/tag/v26.10.1
Source commit: 963756c91c19b7824cea57e7aab5857ce2c776c7
The public createLightClient/bootstrapObserver/updateObserver/
finalityUpdateObserver APIs are in:
  https://github.com/status-im/nimbus-eth2/blob/v26.10.1/beacon_chain/light_client.nim
The collector uses the release's RestJson serializer for actual typed proof
objects. Its NDJSON data field contains the ordinary Beacon API proof object,
not a header-only summary. Strict finalization mode disables force-update;
optimistic callbacks are omitted. Nimbus is an availability dependency, while
the native B3 verifier remains the proof authority.

The release pins eth-clients/mainnet at:
  8cad4805e9cb5aa5e8e74d30e3f5029a7f133372
  https://github.com/eth-clients/mainnet/blob/8cad4805e9cb5aa5e8e74d30e3f5029a7f133372/metadata/config.yaml
This mainnet configuration schedules Fulu at epoch411392 and no Gloas mainnet
activation. The native policy must include all fork versions and a finite
reviewed fork-schedule horizon. Electra's SSZ gindices remain applicable to
the Fulu light-client data layout. An unsupported future fork must stop
authority rather than guess a compatible layout.

Building and running
--------------------
Use a separate Linux/WSL source worktree. The builder refuses a different
commit, modified tracked sources or missing/moved submodules. It adds only
the exact collector source to that isolated tree and builds the collector,
not a full Nitro or beacon database. Dependencies and tools require ordinary
compiler/build disk space and network downloads on first build.

  GIT_LFS_SKIP_SMUDGE=1 git clone --depth 1 --branch v26.10.1 \
    --recurse-submodules --shallow-submodules \
    https://github.com/status-im/nimbus-eth2.git NIMBUS_WORKTREE

  python3 build_peer_collector.py --nimbus-source NIMBUS_WORKTREE \
    --output FRESH_BACKEND_PATH --report FRESH_BUILD_REPORT

The B3 build provides b3-bridge-peercheck. Obtain its exact SHA256 and the
backend SHA256 from local build evidence. Supply a reviewed native policy
with a recent explicitly trusted finalized beacon root/slot and its external
provenance. The collector does not choose a checkpoint from its own peers.
Proof verification cannot remove Ethereum proof-of-stake weak subjectivity.
See https://nimbus.guide/consensus-light-client.html .
The native policy also has a conservative max_recovery_lag_slots bound of
at most 8192 slots. A first bootstrap or resumed store beyond its allowed gap
cannot fast-forward itself into fresh authority; it requires a separately
reviewed checkpoint policy and a new store while preserving the old journal.
This operational bound is not a claim to compute Ethereum's dynamic weak
subjectivity period.

  python3 run_peer_light_client.py --backend FRESH_BACKEND_PATH \
    --backend-sha256 BACKEND_SHA256 --verifier B3_BRIDGE_PEERCHECK \
    --verifier-sha256 VERIFIER_SHA256 --policy REVIEWED_POLICY_JSON \
    --journal PRIVATE_DURABLE_JOURNAL --report FRESH_PUBLIC_REPORT \
    --seconds 300 --peer-base-port 19000

The launcher hashes both executables, limits peers/process time/output/queues,
filters inherited environment, uses only owned children, and keeps raw proofs
in the native journal. It does not persist the collector's ephemeral peer
secret. Its public report contains native diagnostics and content hashes.
The raw policy-file SHA256 is recorded separately from the native tagged,
canonical policy digest. The native journal digest is its authenticated
hash-chain tip, not a SHA256 of the whole journal file. Policy digests must
stay identical and sequence/journal-tip changes must remain consistent.
An OS pipe supplies bounded backpressure. Missing proofs, period gaps,
invalid signatures and stale anchors cannot become successful Arbitrum facts.
After restart the collector begins again at the policy checkpoint so every
required committee transition remains available to the native verifier.

  python3 -m unittest -v test_peer_adapter.py

The Python tests check process/input boundaries. They do not replace native
cryptographic tests or an actual live peer run. A live read-only connection
qualifies this Ethereum foundation only, not Arbitrum source finality.

Remaining Arbitrum work
-----------------------
Nitro does not provide a node-to-node P2P sync protocol to reuse:
  https://docs.arbitrum.io/run-a-node/faq
It derives posted history from Ethereum and uses the sequencer feed for
unposted messages. The feed is fast availability/ordering information; it
does not prove Ethereum finality or correct Arbitrum execution:
  https://docs.arbitrum.io/run-a-node/sequencer/read-sequencer-feed

A compact conservative observer could prove an AssertionConfirmed event
inside a finalized Ethereum receipt, authenticate its L2 block hash, then
verify L2 header ancestry and receipt/account/storage proofs against that
root. The rollup contract exposes the exact event:
  https://github.com/OffchainLabs/nitro-contracts/blob/main/src/rollup/IRollupCore.sol
This requires reviewed deployed contract/proxy pins, complete log ranges,
nonmembership where zero storage matters, code/witness validation for calls,
and an explicit upgrade policy. Confirmation follows the rollup challenge
process; it must not silently replace the fast deposit path with a roughly
week-delayed assertion path.

Fast locally verified Arbitrum data-finalized execution instead needs Nitro
batch derivation/reexecution, or a separately verified execution proof system.
The Ethereum light-client root alone does not verify that execution. Data
finality semantics:
  https://docs.arbitrum.io/how-arbitrum-works/reference/finality-and-reorgs

Light verification removes trust in transport, not the need to obtain bytes.
Pruned peers cannot recreate unavailable state/blob history. Self-hosted Nitro
still needs a snapshot/Classic-state bootstrap for Arbitrum One and parent
blob availability, and is not a small laptop light client:
  https://docs.arbitrum.io/run-a-node/nitro/nitro-database-snapshots
  https://docs.arbitrum.io/run-a-node/beacon-nodes-historical-blobs
No archive subscription is introduced by this peer foundation, and no claim
is made that it already replaces Arbitrum archive witnesses or fork inputs.

Mainnet bridge readiness and activation remain false.
