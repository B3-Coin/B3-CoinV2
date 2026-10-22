B3 raw Arbitrum history: untrusted availability components
========================================================

Mainnet, chain writes and Arbitrum source activation remain disabled. This is
not a complete RPC-independent Arbitrum node or operator source integration.
CAS hashes, HTTPS, peer names, feed signatures and feed confirmation notices
do not establish execution correctness or finality. The separate native
verifier must check its own Ethereum authority, reviewed rollup binding, raw
header ancestry, complete receipt AND transaction tries, and state witnesses.

Implemented interfaces
----------------------

history_store.py:
  HistoryStore(absolute_directory, policy, create=False), a context manager.
  create=True accepts only a new path below safe ancestors. Existing paths
  never initialize/reset. Linux ownership, 0700 directories, 0600 regular
  single-link files, O_NOFOLLOW, exclusive flock and per-instance RLock are
  required. Exact policy bytes remain pinned across restart.

  put(kind, raw_bytes, {transport, peer_id}) -> {sha256, size, kind}
  get(sha256) -> exact bytes, rehashed on every read
  stats() -> bounded availability counts/tip, always authority/mainnet false
  raw_block(header_digest, receipt_digests, transaction_digests=[...],
            chain_id=42161) -> untrusted b3-raw-receipt-block/1 envelope:
      header_rlp: hex, receipts: [{index, encoded: hex}],
      transactions: [{index, encoded: hex}], authority: false.
  Both arrays have identical explicit contiguous indices. This merely carries
  bytes; it cannot establish completeness or transaction identity. The native
  consumer recomputes both trie roots and each transaction hash.

  Raw object kinds: l1_header_rlp, l2_header_rlp, l1_receipt, l2_receipt,
  l1_transaction, l2_transaction, mpt_node, beacon_block_ssz, blob_sidecar_ssz,
  data_column_ssz, nitro_batch, delayed_message, sequencer_feed_json.

  Strict default maxima: 16 MiB/object, 512 MiB/store, 4096 objects, 4 KiB
  journal entry, 32 MiB/raw block envelope input. Lower explicit policy bounds
  are allowed. Objects fsync before journal fsync and published index. A torn
  journal/orphan/missing/corrupt/aliased file refuses; no automatic repair or
  deletion. Failed writes fence the instance. Preserve that evidence and the
  separate native authority journal. A hash chain cannot detect rollback of
  an entire filesystem image; it is not a consensus rollback checkpoint.

acquire_history.py:
  RawObjectClient(https_origin).fetch(sha256, expected_size) -> raw bytes
  .acquire(store, kind, sha256, expected_size) -> stored descriptor
  Literal read-only GET /b3/history/v1/objects/sha256/<lowercase-sha256>.
  This route is a B3-specific content exchange, NOT an existing Nitro peer
  protocol. No public archive is asserted to implement it. It needs an actual
  raw-witness producer; a producer's own acquisition provenance is unverified.

  Public endpoints require HTTPS, CA/hostname/SNI checks, all-public DNS
  answers and a pinned connection address. No proxies, redirects, cookies,
  authentication, query strings, arbitrary paths/methods or JSON-RPC calls.
  Exact Content-Length and application/octet-stream; no compressed/chunked
  CAS objects. Compressed, truncated, wrong hash and ambiguous framing refuse.
  A total deadline bounds DNS/header/body progress; a timed-out resolver fences
  that client. One request/client at a time. Only explicit fixture clients may
  use literal http://127.0.0.1:<port>; the CLI has no loopback override.

  Optional real Nitro v3.12.1 availability protocol:
    NitroFeedBacklogClient(https_origin).fetch_info()
      GET /feed/v1/info -> exact chainId=42161/feedVersion=1/chunkSize1..10000
    .fetch_chunk(aligned_first_sequence, exact_chunk_size) -> raw JSON bytes
      GET /feed/v1/chunk/<first_sequence>, required gzip, bounded standard
      Content-Length or chunked transfer. At most 8 MiB compressed AND 8 MiB
      decoded; concatenated/trailing/truncated gzip refuses. Full contiguous
      requested range must be present. 404 never means empty canonical history.
    .acquire_chunk(store, first_sequence, chunk_size) -> soft-only descriptor.
  The optional protocol follows upstream broadcastclient/backfill.go at the
  pinned Nitro commit recorded in provenance.json. It does not impersonate
  Nitro's sequencer-signature verifier. Remote chain/version/signatures and
  blockHash are untrusted availability claims. Retention is not guaranteed.

feed_history.py:
  parse_feed_frame(raw) bounds Nitro version1 JSON. Explicit known legacy
  signature and current signatureV2/blockHash variants are supported, including
  current batchDataTokens. Mixed signature keys, unknown fields, overflow,
  duplicate JSON keys, deep nesting, oversized integers and gaps refuse.
  FeedCursor(explicit_next_sequence).ingest(store, raw, peer_id=...) durably
  spools exact bytes before advancing a SOFT diagnostic cursor. Reorder,
  equivocation or delayed-message regression fences it. A feed-confirmed
  sequence cannot advance it. No cursor is passed to operator source facts.
  This module is not a WebSocket client; obtain live frames from a separately
  maintained Nitro feed/relay implementation. Do not implement a homemade
  sequencer authentication protocol or call unsigned data verified.

CLI: explicit public raw-object origin from an environment variable
------------------------------------------------------------------

  python3 -B acquire_history.py --store /absolute/new/private/history \
    --policy /absolute/reviewed/history-policy.json --create \
    --origin-env B3_HISTORY_ARCHIVE --kind l2_receipt \
    --sha256 <explicit-requested-SHA256> --size <explicit-byte-count>

Use default_policy() to create a reviewed disabled policy, or provide the exact
shape. Endpoint URLs never enter public reports; only an origin digest is
journaled. The CLI reports zero JSON-RPC requests by this client and explicitly
leaves producer provenance/native proof verification false. This does NOT
certify that the server obtained its data without RPC. Never relabel an RPC
dump or RPC-seeded Anvil fork as RPC-independent acquisition.

Actual remaining deployment requirements
----------------------------------------

Arbitrum One has no ordinary general L2 node-to-node history synchronization
protocol. Nitro derives canonical ordered inputs from Ethereum transactions,
events, delayed inbox messages and calldata/blobs; the real-time feed is a
sequencer promise. The REST backlog above is a useful soft-data service, not
an account/receipt archive or canonical-parent completeness proof.

One maintained no-external-provider deployment is an OWNED Ethereum EL+CL with
historical availability plus OWNED Nitro, using local APIs between them. This
is external-provider independent; it is not literally free of local APIs.
Arbitrum One requires an authenticated Classic-to-Nitro starting-state path;
Nitro cannot process the Classic era from scratch. Official supplied snapshots
are one bootstrap option. File checksums are not state-root verification.

An independently reviewed confirmed-assertion witness can instead be supplied
by an untrusted sparse-witness archive and cryptographically checked natively.
That can reduce resources, but no maintained public sparse raw-witness archive
has been qualified here. The local-store API does not create one. An owned
multi-terabyte archive is ONE deployment option, not a mathematical minimum
for all possible future proof services. See node_preflight.py for a separate
read-only assessment; nothing here downloads a snapshot or installs a node.

Current Fulu peers use data-column availability. Fetching obsolete blob sidecar
messages alone cannot reconstruct current batches. Full historical columns,
maintained reconstruction/KZG, exact L1 commitments and gaps must be handled.
Raw blob/column kinds in this store provide no KZG verification.

Confirmed rollup assertions have a challenge delay. That proof path cannot be
described as sequencer-speed deposit finality. Fast independently checked
Arbitrum execution needs maintained Nitro STF/DA replay (or a separately
qualified execution-proof system) and sound starting state. Native operator
source activation, live proof assembly and end-to-end source handoff remain
separate gates. Existing journals/finality policies remain untouched.

Tests
-----

  python3 -B -m unittest -v test_history_store test_history_transport test_feed_history

Linux tests use actual owner-only stores, locks/fsync and owned loopback
transport. Synthetic bytes are deterministic public adversarial fixtures,
not generated operator keys or claims about real chain events. Tests cover
crash evidence, concurrency, aliasing, quotas, total deadlines, gzip bombs,
range substitution, both known feed schemas and refusal without progress.
Component qualification is separate from live acquisition, real-token fork
testing and mainnet readiness. All activation flags remain false.

Primary sources and exact upstream pins are in provenance.json.


Native offline history verification (encoding B3ARPR01)
----------------------------------------------------

proof_bundle.py assembles a bounded native proof from exact hash-addressed
raw objects. It can read this module's HistoryStore or a separately labelled
owned-regtest object directory. Context is explicit: chain412346 development
exports cannot be relabelled as Arbitrum One. The binary contains only
untrusted witness bytes, never a verified flag or an authority capability.

  python3 -B proof_bundle.py export-native-proof \
    --manifest /absolute/proof-manifest.json \
    --objects-dir /absolute/objects --output /absolute/fresh-proof.bin

The native b3-bridge-arbitrumcheck tool reads the binary on standard input.
Its expected range endpoints must be supplied separately. Hash/address
arguments are lowercase 0x-prefixed raw bytes; heights are strict decimals.

  /absolute/build/bin/b3-bridge-arbitrumcheck \
    --previous-height <height> --previous-hash <0x32bytes> \
    --to-height <height> --to-hash <0x32bytes> \
    --vault <0x20bytes> < /absolute/fresh-proof.bin

It reconstructs BOTH complete ordered receipt and transaction tries for every
header, checks exact parent ancestry/range endpoints, and verifies requested
account/storage witnesses at distinct newest/oldest state roots. Exact vault
Deposit/Withdrawn ABI records become candidate events; their indices include
unrelated/system/token logs. Amounts retain all 256 bits. Malformed frames,
over-budget inputs, incomplete ranges and changed proofs refuse.

This tool authenticates neither endpoint. Commitments can match a fabricated
chain when the supplied checkpoint is fabricated. Its successful reply is
candidate data only, not ConfirmedRange or an operator Apply/ACK capability.
It always reports arbitrum_authority=false, execution_verified=false,
rollup_settlement_verified=false and mainnet_enabled=false. Production
Verifier::VerifyConfirmedRange still refuses UNREVIEWED_DEPLOYMENT. An owned
maintained Nitro node is the explicit local trust root in the separate lab.

Build with the existing bridge tools/test build options, then qualify the
actual executable against the retained genuine Nitro two-block fixture:

  python3 -B contrib/b3bridge/arbitrum_history/test_native_proof_cli.py \
    --native /absolute/build/bin/b3-bridge-arbitrumcheck \
    --fixture /absolute/source/src/test/data/nitro_history_v1 \
    --evidence-dir /absolute/fresh-evidence-outside-source

The Linux process qualification creates a distinct disconnected network
namespace and requires IPv4/IPv6 TCP/UDP denial before and after execution.
It verifies exact deposit/payout records, both historical storage values,
absence and deterministic replay; re-encoded header/transaction/receipt
changes, event omission and state-proof swaps must fail. Fixture and tool
hashes are bound before/after. It has no live-provider fallback or skip mode.

Additional Python codec/object-boundary tests:
  python3 -B -m unittest -v test_proof_bundle
Native suites in test_bitcoin:
  bridge_arbitrum_source_tests, bridge_arbitrum_proof_codec_tests,
  bridge_arbitrum_history_tests

The fixture's Solidity emitter deliberately has no ERC20 custody. Actual
USDT0-bytecode custody/trading is a separate owned-Nitro lab under
contrib/flowmesh2/nitro_regtest/ with its own encoding5 profile. Neither local
test enables Arbitrum One, authenticates a live L1 assertion, proves current
data availability, or qualifies permissionless exits.
