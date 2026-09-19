# FlowMesh experimental tester candidate — 1.1.5-flowmesh-test.2

**Engineering test build, not deployment-ready.** No public release or live
upgrade is implied. Keep previous binaries and data. This candidate must not
be used to attempt recovery of existing mainnet signing conflicts.

## What is included

- Default-off, exact-market opt-in preliminary agreement before permanent V1
  signing. A complete COMMIT certificate is required; PREPARE alone is not
  enough. The anchored roster and quorum are unchanged.
- Durable preliminary signing records, exact-message retry, restart checks and
  a downgrade guard. Duplicate COMMIT delivery may supply missing verified
  proof without counting a seat twice.
- The existing Qt client, reverse trading view, receipt preservation, independent
  operator network and live operator `flowmeshconnect` RPC are inherited from
  the published base. They are not new improvements in this candidate.
- Test.2 adds `reconnectflowmeshclient`: a read-only fresh HTTPS probe/failover
  for ordinary traders. It never signs, resubmits, cancels or changes an action.

No B3 consensus, execution, action identity, fee currency, settlement encoding
or membership change is introduced. V2 asset-to-asset markets, shared balances,
stable-asset trading fees and inactive-validator policy are not implemented here.
Creator-authenticated universal asset metadata is not added by this patch.

## Known failed path — do not hide this

Two generated four-operator/fifth-client process runs certified deposits,
orders, matches and cancellations but stalled on a later remote withdrawal
for the 90-second observation window. All four evaluated the same candidate;
a complete preliminary COMMIT certificate was not observed. Rate-limit
deferrals and critical queue refusals were observed. The precise avoidable
delivery/scheduling cause is not yet isolated or repaired.

The later bulk-throttling/reconnecting-validator phase and remote type-9
constructed-block parity were not reached. They are **pending**, not passing.
One successful trade or a growing peer count does not close this failure.

Retained local qualification before publication: 122 focused cases / 115,987
assertions passed; a separate transport selection passed 26 cases / 8,135
assertions. Nine-runtime split/heal and restart tests use an in-memory network
and mocked chain dependency, not nine WAN nodes. Four real local operators
demonstrated type-8 constructed-block engine-on/off parity and base-fixture
custody/payouts. This is not full withdrawal, WAN or power-loss qualification.

## Test boundary

1. Use a separate explicit regtest datadir, a generated test wallet and valueless
   coins. Never copy a production wallet, BLS key, outbox or signing journal.
2. The coordinator must supply the actual isolated B3 network, reachable HTTPS
   trading URL and its verified CA/hostname configuration. This package does
   not invent public endpoints or make a stopped loopback service remote-ready.
3. Use only a genuinely fresh test market, agreed by all participating operators
   **before its first signing**. Do not enable this mode on an old market.
4. Do not reset a journal, lower quorum, force rearming or create replacement
   instructions to obtain a pass. A stopped market is a result to report.
5. A diagnostic retry, when justified, must preserve the original signed bytes
   and ActionId. Never resubmit an already-certified action as a new action.

All participating operators must use the matching experimental protocol. Older
FMN2 operators cannot understand its new preliminary message. Existing stores
remain in legacy mode; an opted-in store requires its mode on every restart
and refuses downgrade. Prepared/decided anchors invalidated by reorg remain
an explicit unresolved recovery boundary. This is not a repair for old split
final signatures and not a new finality/bridge recovery protocol.

## Connecting: operator versus ordinary Qt trader

These are different connections. Commands below are entered in **Qt's RPC
console**, not directly into Windows Command Prompt. Use `help COMMAND` for the
exact schema in the running binary.

### FN test operator

The local operator service must have `enableflowmeshvalidator=1` in the
isolated test configuration. Add a reachable operator without restarting:

```text
flowmeshconnect "OTHER_OPERATOR_PUBLIC_KEY@TEST_NETWORK_IP:PORT"
getflowmeshnetworkinfo
getflowmeshvalidatorinfo
```

Replace the template: the network key is a **66-hex compressed operator public
key**, not the FN's 96-hex BLS public key. The other operator obtains the public
network key from `getflowmeshnetworkinfo`. Never share its private key file.

`accepted` means a connection target was admitted, not that a connection or
quorum was reached. Inspect target retry errors and authenticated channels in
`getflowmeshnetworkinfo`. The target is memory-only; the existing config form
`flowmeshconnect=PUBLIC_KEY@IP:PORT` preserves it across restart. This command
does not open a firewall, change listening addresses or arm a signing key.

Only use reachable addresses in the approved test network. An inbound operator
needs the correct bind address and allowed TCP port; `127.0.0.1` is reachable
only from the same machine. Keep FMN2 inside the controlled operator network:
it is authenticated **plaintext TCP**, not encrypted or QUIC.

For the genuinely fresh test market, the coordinator arranges the identical
`flowmeshpreagreementmarket=EXACT_FRESH_MARKET_ID` entry on every operator
before bootstrap. There is no permission to retrofit it onto existing history.
Arming is separate, only after confirming a generated seat has no duplicate
signer. `armed=true` still does not prove an accepted vote or certification.

### Ordinary Qt trader

Keep `enableflowmeshvalidator=0`. Trading uses the configured HTTPS service:

```text
flowmeshendpoint=https://COORDINATOR_PROVIDED_TEST_HOST:PORT
```

This is a configuration template, not a working public endpoint. A private test
CA needs the coordinator-provided `flowmeshendpointca` bundle; hostname/IP and
CA verification remain mandatory. Never disable verification. Wallet/admin RPC
must remain private and is not the trading endpoint.

Read-only Qt console checks:

```text
getflowmeshclientinfo
reconnectflowmeshclient
listflowmeshactions
```

`reconnectflowmeshclient` makes a fresh read-only request using the configured
endpoints and unchanged CA/hostname/pin verification. `status: "reachable"`
means one replied; it does not prove quorum or certification. An empty market
list may be a valid availability response. Market and balance proof checks still
run when trading data is requested. `actions_submitted` is always zero.

`busy` means another client request owns the worker: wait for it to finish and
retry the read-only command. No second worker or hidden action retry is queued.
`unavailable` reports the failure; fix the endpoint/tunnel/certificate rather
than clearing history. There can be up to eight sequential endpoint attempts,
each with the existing five-second transport deadline, so this is not a promise
of a five-second total wait. `getflowmeshclientinfo` lists the per-endpoint errors.

`not_configured` needs the existing `flowmeshendpoint` startup configuration and
a clean restart. `not_remote` means the local engine is selected; no HTTPS probe
was attempted. There is still no runtime endpoint-add RPC. `flowmeshconnect`
connects operators, not an engine-off trader. Do not enable a local validator
merely to make a trading button active. HTTPS reconnection is separate from
exact-action recovery, quorum and settlement.

## What testers should record

Check a generated deposit, order, cancellation and withdrawal individually.
Record admission, certified inclusion, durable replica application and eventual
B3 payout separately; socket writes and HTTP acceptance are not certification.
Keep the first submission timestamp and original ActionId across retries.

Send privately: candidate version, BUILD-INFO source commit/platform/run ID,
OS, exact market/ActionId, UTC interval, observed versus expected result and
bounded sanitized status/log excerpts. Never send passphrases, wallet files,
private keys, RPC cookies, TLS keys or complete signing journals. Do not post
the full config or full debug log publicly.

## Artifacts and unresolved limits

GitHub's manual desktop build produces Windows x86-64 portable ZIP (no installer)
and macOS arm64/Intel application ZIPs, with BUILD-INFO and SHA256 checksums.
That job compiles/packages only; its test suites are disabled explicitly.
Local focused checks are separate and do not turn the failed process test green.
Verify the recorded source commit and archive hash, not a filename alone.

Windows is unsigned; macOS is ad-hoc signed, not notarized. macOS packaging
checks every bundled Mach-O dependency against macOS 15.0. Platform build
success is not an attended test on each platform. Do not disable antivirus,
Gatekeeper or other system protections to run a flagged artifact.

The original accessibility SIGSEGV remains OPEN / UNRESOLVED. A clean launch is
not a repair. Mixed operator versions, WAN/load behavior, crash/power-loss
recovery and full resource-limit stress remain unqualified. Explicit proof,
roster (worst-case 92 seats), candidate and 128-view bounds can halt safely;
this prototype does not promise unbounded liveness or progress without quorum.
