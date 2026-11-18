Read-only Sepolia execution peer availability
============================================

Build with the pinned module in go.mod/go.sum. No Ethereum RPC endpoint,
wallet, signer, chain override, or transaction broadcast is accepted.
The native light client, not this process, supplies consensus authority.

Examples (the binary path is installation-specific):

  b3-execution-peer --hash 0x<64 lowercase hex> --seconds 180 --max-peers 8
  b3-execution-peer --witness-request /absolute/request.json --seconds 180
  b3-execution-peer --receipts-block <positive decimal block> --seconds 180

Header mode returns the exact raw RLP preimage for the supplied hash. A
separate native Gloas verification must authenticate that hash before its
state or receipt roots are used. --untrusted-peer-tip is diagnostic only.

Witness mode accepts either a previously authenticated state_root or an
execution_hash plus a bounded exact account/storage request. It checks
proofs using maintained Geth trie code; native verification must separately
bind the root, exact deployment pins, assertion and selected source range.

Receipt mode fetches the requested block header and complete consensus
receipts over ETH/69 or ETH/70+ (including paginated replies). It rebuilds
the receipt trie and checks cumulative gas against the header. A matching
trie does not authenticate the peer's header or prove finality. Arbitrum's
native verifier must match the decoded AssertionCreated preimage against
the latestConfirmed hash proved under the authenticated L1 state root.
Use of this mode alone cannot create a settled-head capability.

While waiting, all three modes can answer legitimate receipt, block access-list
and blob-cell requests with version-correct unavailable responses. The outbound
guard checks that these replies contain no requested data. This preserves a
read-only connection without pretending to hold bodies, receipts or state.

Every result is availability_only, consensus_authority=false,
native_verification_required=true, mainnet_enabled=false. A transport
passed flag describes only a complete matching availability response.
Partial/incorrect/malformed replies are refused without partial output. The
final JSON envelope, diagnostics and newline all fit the 32 MiB output cap.

The client uses the pinned Sepolia genesis, Geth fork filter, signed DNS
discovery and official bootstrap seeds. Public seeds are not static ETH
servers: Geth prioritizes static dials and repeatedly selecting busy seeds
can starve discovered execution peers. Only an explicit owned loopback
fixture is static. Public dials reject private and loopback addresses;
the fixture mode disables discovery and accepts literal 127.0.0.1 only.

Limits: 1..8 peers, 1..300 seconds globally, fixed per-peer timers, bounded
messages and response bytes. Receipt mode permits at most 128 pages,
8192 receipts, 32 MiB raw receipt data and a 32 MiB JSON result. All modes
refuse outbound transaction messages. No persistent node/database service
is started. TCP listens only on loopback; UDP discovery binds a wildcard
address without NAT port mapping.

Tests:
  go test -race -count=1 -mod=readonly ./...

Tests cover canonical header binding, maintained Geth handshakes, SNAP
companions and physical-peer gates, trie proofs, message/byte budgets,
receipt partial pages, request IDs, number/root/gas consistency, framing,
empty blocks, cancellation, and fail-closed authority/mainnet flags.
