# FlowMesh networking — first uncommitted integration stage

Date: 2026-09-11. Inspected branch: `release/v1.1.5`, HEAD
`59057cb6c85d32129e1e38c3dd5b67e7c8b36e02`.

Historical stage report: its proposed `-enable_flowmesh` name and deferred
default-off work below are superseded in current development by the sole
`-enableflowmeshvalidator` option, default false. There is no old-name alias.
Enabled operators now default to independent transport and validator role;
explicit legacy/observer/sentry settings remain available. See
`flowmesh-client-stage3.md` for the current integration and qualification status.
The original stage-specific findings below are retained as historical evidence.

This stage is local and uncommitted for owner review. It is **not** completion
of the consolidated networking/client/hosting brief or a deployment candidate.
Existing uncommitted metadata, Qt, and runtime work was preserved. No live wallet,
fund movement, production key migration, journal reset, push, or merge was used.

## Existing code and reuse

The inspected release tree runs one local `FlowMeshService` using B3-carried
`fmhello`, `fmaction`, `fmprop`, `fmattest`, `fmcert`, `fmget`, and `fmentries`.
Qt calls local wallet RPCs; wallet actions are signed locally. The narrow
`interfaces::Chain` read/submit boundary currently requires the local service.

The clean sibling worktree `B3-FlowMesh-fmnet`, branch
`work/flowmesh-independent-network`, commit `69d8f19`, already supplied an
experimental independent transport. This stage selectively reuses its adapter,
diagnostics and tests, rather than replacing the current runtime or importing
the older branch wholesale. Its unrelated block-storage changes are not ported.

Reused transport: versioned FMN1, authenticated dual-channel TCP, distinct
operator identity, pinned static peers, reconnects, bounded queues, and a
separate listener. It authenticates frames but does not encrypt them. There is
no QUIC implementation in these two inspected source trees. Replacing TCP is
not necessary to integrate and measure the existing boundary; QUIC selection
and qualification remain a separate task.

## Exact scope

- `src/node/flowmesh_net.{h,cpp}`: independent sockets, identity and transport.
  Retain one bounded decoded ingress object per channel on retryable runtime
  refusal; stop TCP reads for that channel, retry with bounded backoff, and
  re-enter ordinary runtime validation on admission. Record retries and any
  eventual discard explicitly. Return outgoing admission failure to the caller
  and count later per-peer queue rejections.
- `src/node/flowmesh_service.{h,cpp}`: one runtime and durable history for
  legacy, dual and independent modes; disjoint connection IDs keep directed
  replies on the originating network. Network failure neither silently falls
  back to legacy nor grants signing authority. Independent-only signing is
  unavailable if its network worker is unavailable.
- `src/init.cpp`, `src/net_processing.cpp`: optional independent transport
  configuration and removal of B3-carried FlowMesh application handling in
  independent mode. Ordinary B3 traffic and validation remain enabled.
- `src/rpc/flowmesh_network.cpp`, `src/rpc/register.h`: walletless public
  diagnostics, including admission/retry counters. Authentication and outbox
  admission are explicitly not quorum, execution or delivery proofs.
- Build/test registration, frozen-vector tests, shared-runtime ingress tests,
  real-socket tests, and a three-process walletless networking smoke test.

`-flowmeshtransport=legacy|dual|independent` retains `legacy` as this stage's
default. Optional settings include `-flowmeshlisten`, `-flowmeshbind`,
`-flowmeshport`, repeated `-flowmeshconnect`, `-flowmeshrole`, and
`-flowmeshdatadir`. These are transport settings, not aliases for an engine flag.

## Deliberately not completed in this stage

The requested `-enable_flowmesh` default-off behavior is not introduced yet.
Changing that default now would remove trading from ordinary Qt wallets because
the remote trading backend does not exist. The eventual flag must gate optional
execution/networking only, never consensus-required B3 trackers or validation.

The reused transport still shares an I/O/verification worker and a live socket
between actions and critical messages. Separate bulk sockets and reserved
credits do not prove complete connection/worker/bandwidth isolation. Discovery
is static; signed advertisements, several bootstrap sources and scalable
topology management are not completed. Reconnect delay remains fixed.

Ingress retry is bounded (100 ms rising to 1 s, with a 30 s retention deadline).
Expiration or disconnect is observable and requires normal reconnect/catch-up;
this is not guaranteed delivery. The existing service reconciliation gate can
still suppress outgoing relays, and later egress queues can refuse admitted
outbox work. The runtime's relay callback remains void. End-to-end critical
retry/backpressure is therefore still incomplete, not hidden by separate lanes.

No four-operator trading milestone, engine-off trader, Qt remote path, WAN/load
benchmark, hosting RPC or commission payout has been completed by this stage.

## Client and hosting boundaries for the following stages

Keep local wallet signing RPCs local. Add a separate client backend behind
`Chain::flowMeshMarkets`, `flowMeshMarketStatus`, `flowMeshMarketData`, and
`submitFlowMeshAction`. Failover must retry the identical signed action rather
than give a server owner keys or silently re-sign different instructions.

Current `MarketData` rows do not carry balance proofs. A valid certificate for
a state root cannot verify arbitrary remote account rows. Existing full-state
commitments can support verified whole-state snapshots; an efficient partial
proof format does not already exist and must not be invented silently. Separate
submission, admission, certified inclusion, applied-result evidence, and B3
settlement in the client UI. Qt already uses `known_head` and bounded pages,
but it is polling rather than a remote subscription service.

FN ownership remains separate from the BLS operating key. Public-key-plus-PoP
owner binding needs wallet tooling, not a new FN policy. The current BLS key
also authorizes historical reward claims; unrestricted host-held BLS secrets
cannot offer owner-protected reward remainders or enforced commissions under
unchanged rules. Leave automatic splitting unimplemented. Separate service
billing has different trust guarantees and does not itself protect rewards.

## Compatibility evidence and tests

No changes in this stage to seat rules, matching/clearing, action or attestation
digests, state roots, certificate format, checkpoint/vault validation, or
durable signing rules. B3's FN-seat/checkpoint/vault trackers remain on the
ordinary validation path independently of this optional transport.

Frozen vectors preserve all existing application message bytes and identities.
An additional test constructs two valid 3-of-4 signer subsets for the same
entry: distinct evidence bytes do not imply distinct semantic identities.
The shared-ingress test mixes positive legacy and negative independent peer
IDs, checks duplicate seats cannot add weight, and applies one certificate to
one retained history. This test intentionally does not claim socket routing.

## Executed qualification of this current tree

Headless macOS/Apple Silicon Release build succeeded for `b3coind`,
`b3coin-cli`, and `test_bitcoin` in `build-fmnet-stage1`. An initial new-test
compile error used `fs::path / std::string`; it was corrected with explicit
`fs::PathFromString`, then rebuilt. Final compilation has no errors; the linker
reports a duplicate-static-library warning. No Qt binary was built or replaced.

| Check | Result | Measured duration |
| --- | --- | --- |
| Transport, service, startup, shared-ingress and frozen vectors | 31 cases, 2,683 assertions passed | 3.660 s reported by Boost |
| Existing checkpoint codec, FN binding/index and fee allocation | 30 cases, 10,865 assertions passed | 0.064 s reported by Boost |
| Final three-process walletless regtest network-policy smoke | Passed, exit 0; test nodes stopped | 5.388 s from framework start to success |

This is 61 focused cases / 13,548 assertions, not the complete 1,534-case test
binary. The functional smoke runs two independent transports and a default
legacy node on the same Mac. It verifies paired authentication, old-transport
message isolation, persistent transport identity, occupied-listener failure
without stopping B3, exact empty-block hash convergence, and invalid-option
errors. It does not test four active FN operators or certified trading.

Logs in the build directory:

- `final-incremental-build.log` and `rebuild.log`
- `focused-tests.log`
- `compatibility-tests.log`
- `network-policy.log`

The final functional run's temporary evidence remains at
the private retained generated-fixture evidence directory.
It contains disposable regtest data, not a production wallet. An earlier smoke
also passed; its measurements are not substituted for this final run.

`git diff --check` passed. Independent read-only review found no introduced
high-priority lifecycle/deadlock or ordinary-B3 message-processing regression.
The outgoing-retry and traffic-isolation limitations above remain open.

No end-to-end trading, replication-tail, loaded-host or WAN performance claim
follows from these test durations. Frozen vectors/source validation-path checks
do not replace the future actual type-8/type-9 old-node settlement gate.

## Next bounded stage

After review of this uncommitted stage, complete explicit outgoing retention,
retry and three-class scheduling across the service/transport boundary. Then
build the distinct remote client backend and evidence verification before
introducing the default-off engine flag. The four-headless-operator plus
engine-off-client milestone, advancing B3 blocks, and Qt path must qualify that
architecture before hosting tooling is added. No owner decision is required
for these ordinary engineering gaps. Automatic protected host commissions
remain incompatible with unrestricted host-held BLS reward authority and are
not part of the no-consensus-change implementation.
