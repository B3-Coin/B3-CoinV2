# Integrated FlowMesh: ordinary clients and opt-in validators

Work in progress, uncommitted for review. No live deployment is authorized by
this document. FlowMesh stays inside the B3 codebase; Qt consumes the trading
client boundary. There is no replacement execution engine.

## One validator enable option

`-enableflowmeshvalidator` defaults to **false**. With it unset, ordinary B3
validation, custody/checkpoint/withdrawal rules and wallet operations remain
active. The optional FlowMesh service, execution-history synchronization,
proposer loop and independent validator listener are not constructed. Ordinary
trading uses configured HTTPS endpoints and local wallet signing.

Operators use `-enableflowmeshvalidator=1` to run full synchronization,
execution and independent validator networking. An eligible FN seat and
authorized keys still need explicit arming. The default enabled network role
is `validator`; explicit observer/sentry roles remain available. Disabling the
service does not unregister a seat, remove it from quorum, delete its history
or alter its signing protections.

There is no second engine-enable flag or compatibility alias. Existing
operators must explicitly enable validator mode when deploying this change;
this work does not edit their configurations.

## Ordinary-client configuration

Repeat `-flowmeshendpoint=https://host:port` for independent endpoint failover
(at most eight). Set `-flowmeshendpointca=/path/to/ca.pem` for an explicit PEM
trust bundle, or configure one per endpoint in the same order. Optional
`-flowmeshendpointpin` leaf-certificate SHA256 pins supplement, never replace,
CA and hostname validation. Packaged Windows/macOS OpenSSL trust-store
integration is not qualified: use an explicit CA bundle there.

An enabled operator may expose the restricted trading listener using
`-flowmeshapi=1`, `-flowmeshapibind`, `-flowmeshapiport` (default 5650),
`-flowmeshapicert` and `-flowmeshapikey`. The last two refer to TLS files, not
FN/wallet keys. This listener cannot dispatch wallet or administrator RPC.
It provides market information, bounded signed-action submission/status,
cursor events, and existing checkpoint/effect proofs.

The HTTPS API is separate from FMN2. FMN2 remains authenticated plaintext TCP.
This work does not establish QUIC support, encrypted operator transport or WAN
performance.

## Verification and recovery boundaries

Market/domain/configuration pins and certificate authority come from the
client's mandatory B3 indexes. A bounded whole-state snapshot is decoded with
the existing state format and checked against its quorum-certified root;
account balances, nonces and book rows are derived locally from that state.
Adjacent endpoint account rows are not trusted.

Queue admission is not pool admission. An endpoint's inclusion observation is
not a proof. Canonical certified entry evidence verifies semantic action
inclusion, but does not independently establish its execution outcome. Fill
history remains explicitly endpoint-reported. Newer network-head freshness is
not proven by a certificate. B3 checkpoint confirmation and payout validation
are separately checked against connected B3 evidence and local live coins.

The client durably retains exact signed bytes before sending, preserves the
initial submission time and reuses the same ActionId across retries. An
ambiguous delivery cannot become a definite rejection merely because a second
endpoint rejects it. Previously verified inclusion leaves a durable no-replay
marker; missing fresh evidence after restart does not authorize resubmission.
These records live in `flowmesh_client`, never in validator signing journals.

Bounds include: 512 retained actions, an 8 MiB serialized client journal,
8 MiB total in-memory inclusion-proof copies, eight cached markets, 8 MiB per
whole-state snapshot, 24 MiB HTTPS replies, and a 2,048-event per-runtime ring
with pages of at most 256. Unresolved signed actions are not evicted to make
room. Expired/restarted cursors require explicit gap reporting and verified
snapshot recovery. A legal larger state is an explicit client-policy limit,
not a change to B3 or FlowMesh validity.

## Qualification status

The original FMN2 integration was committed locally as `4700e85`. Existing
unrelated Qt/metadata changes and retained evidence were preserved separately.
This client stage remains uncommitted, with no push, merge or deployment.

Checks actually completed on the development stage:

- The daemon, CLI, Qt wallet and core test targets compile locally.
- Seven client-evidence tests pass.
- Four HTTPS tests pass with loopback networking permitted. The initial
  restricted-sandbox attempt could not bind its test sockets.
- Both focused Qt suites pass: 16 trading tests and 34 workspace tests, with
  one optional synthetic-image export skipped. These are not an actual Qt
  session against the HTTPS backend.
- Four pure invalid-transaction mutation tests pass.
- Retained `regtest-client-4` passes the five-process, same-host qualification:
  four independent headless operators, B3-carried FlowMesh disabled, and one
  ordinary client with validator mode off. Deposit, matched trade, cancellation,
  withdrawal and startup without an enable flag pass while B3 advances.
- Fourteen hostile-response/recovery checks pass, including exact signed-action
  failover, forged adjacent account data, same-head endpoint/account scope gaps,
  and restart with missing endpoint receipts. Previously certified actions are
  not resubmitted; restart obtains a verified snapshot and reports a cursor gap.
- All four enabled nodes and the ordinary client accept the exact same valid
  checkpoint/withdrawal transactions and reject the same deliberately invalid
  variants with specific FlowMesh reasons. This is transaction-validation
  parity, not a constructed-invalid-block acceptance test.

For the five measured remote actions in that run, initial submission to observed
verified inclusion was 263–499 ms; observed admission was 31–46 ms. These are five
same-machine regtest samples, including an intentional lost response, not a
normal-load distribution or WAN benchmark. The retained JSON separates these
observations and includes operator delivery events. Missing timestamps are not
inferred from socket writes or queue responses.

An independent repeat, `regtest-client-5`, also passes these automated checks.
Its five observations span 152–1,077 ms; this is retained rather than discarded
as an outlier. No normal-load or WAN claim is made from either small run.

The actual Qt process was launched through a test-only entry point redirecting
both QSettings scopes before unchanged `GuiMain`, pinned to the stopped fifth
regtest client. Its existing wallet was accessible, validator mode remained
off, and its RPC returned locally verified remote account state and B3 checkpoint
confirmation. It shut down cleanly. UI interaction was blocked by the computer-use
tool reporting **Computer Use permissions are not granted**. Therefore actual
button/confirmation/trading-screen qualification remains **not completed**.

Windows/macOS
distribution, WAN performance, operator encryption/QUIC, power-loss recovery,
and invalid-block parity are not established by this run. Per-action execution
outcomes/history remain endpoint-reported; only inclusion, certified whole-state
projection and locally checked B3 settlement have the stated evidence.

The detailed isolated workflow and actual-Qt manual checklist are in
`flowmesh-remote-client-qualification.md`. Compilation, a queue response or a
daemon-only trade must not be reported as complete Qt qualification.
