# FlowMesh ordinary-client qualification

This checklist is for fresh, isolated regtest wallets only. It is not deployment
authorization. Do not restart or reconfigure live validators, open production
wallets, import an operator key, or use real funds.

## Automated fixture

`test/functional/feature_flowmesh_remote_client.py` reuses the four independent
headless operators and the real custody/trade/withdrawal fixture from
`feature_flowmesh_independent.py`. All four explicitly enable the engine and
use independent FMN2 operator networking. FMN2 remains authenticated plaintext
TCP; the separate public client API uses HTTPS.

The fifth process has its own wallet and B3 datadir, no FN or finality key, two
configured HTTPS trading endpoints, and `-enableflowmeshvalidator=0`. Test-only TLS
relays use an ephemeral CA and server keys inside the fixture's temporary
directory. They never forward wallet/admin RPC. A deliberately lost response
after upstream admission exercises proof-backed status resolution or retry of
the same saved signed action and ActionId, not another economic instruction.
An action already proven certified is never resubmitted. The local client
outbox lives in `datadir/flowmesh_client`, not an operator production store.

The result artifact is `flowmesh-remote-client-qualification.json` under the
functional test directory. Preserve the directory and node debug logs when
recording evidence. Its `success` field is meaningful only after an actual
completed run; adding or compiling the harness does not qualify delivery.
The initial run uses explicit engine-off. The development default is already
false; run with `--check-default-off` to additionally qualify startup without
an enable flag. `default_off_qualified` remains false otherwise. Neither this
default nor a daemon-only pass establishes actual-Qt or deployment readiness.

The daemon fixture does **not** establish that Qt was exercised. It also does
not claim WAN performance, independently replayed per-action outcomes, or
invalid-checkpoint/withdrawal block parity. Those need separate evidence.

The adversarial portion mutates both isolated HTTPS relays: wrong domain or
market, missing/tampered state or certificate, a regressing reported head,
malformed/oversized responses and unavailable endpoints must fail closed.
A forged adjacent account/nonce row must not change the balance derived from
the certified state. A later endpoint's rejection after an earlier ambiguous
delivery must remain unknown until real proof resolves it. Finally, a client
restart followed by synthetic receipt expiry must not resubmit a previously
certified action; restoring the endpoint proof resolves status without a POST.
These remain planned checks until the result artifact records a successful run.
The ordinary client also prepares and publishes checkpoint and vault payouts;
the same exact valid type-8/type-9 transactions are checked by all four enabled
nodes and the engine-off client before their single broadcast.
The corresponding bounded invalid variants change only the MPA payload and
must receive the same specific FlowMesh rejection at the same B3 tip. Positive
controls are checked again afterwards. This is transaction-validation parity
through `testmempoolaccept`, not invalid-block/`submitblock` qualification.
Separate unchanged-head checks require an explicit gap on an endpoint-instance
switch and on an account-filter switch, without changing balances or sending
an action; normal certification progress cannot satisfy those assertions.

## Actual Qt verification checklist

Record the exact source revision, dirty task diff, executable SHA256, platform,
Qt/TLS backend, run label, test directory, and the start/end time. Preserve the
four headless regtest operators. Only the disposable fifth client may be
stopped to release its datadir before opening the newly built Qt executable.
Never point Qt at an already-running datadir.
Use `--qt-review-hold-seconds=900` only for a deliberately attended isolated
review. This optional 0–900 second window starts after automated success and
workload shutdown, stops only client4, and temporarily retains the four
operators and Python proxies. It also preserves the test directory. The default
is zero: ordinary automated tests do not wait. There is no `--noshutdown` option.
Wait for `FLOWMESH_QT_REVIEW_READY` and inspect `qt-review.json` for the exact
stopped datadir, public startup arguments, endpoints, CA, mock time and ports.
The harness does not launch Qt. The opt-in `test_b3_flowmeshclient-gui` target
provides preference isolation without changing the production GUI entry point:
configure `B3_FLOWMESHCLIENT_GUI_TEST_DATADIR` to this exact disposable client
directory, then explicitly build that target. It is excluded from normal builds,
installation and CTest. Its first argument must be
`--test-settings-dir=<fresh empty absolute directory>`, followed by the reviewed
regtest/client arguments. It redirects both QSettings scopes to INI files before
calling the existing `GuiMain`; an unconfigured or wrong-datadir launch refuses.
This is a test accidental-use guard, not a sandbox: review fixture configuration
for external paths. If isolation is not established, do not launch and mark actual
Qt verification unqualified. Stop the isolated Qt process before
creating the artifact's `qt-review-complete` marker. The hold ends on that marker
or its deadline; operator/proxy shutdown then proceeds normally. Neither the
marker nor a successful daemon run marks the Qt path qualified.

The retained run5 launched this isolated Qt process, verified wallet accessibility
and the remote verified balance via RPC, and stopped it cleanly. Computer Use
permissions were not granted, so no button/confirmation walkthrough was completed.
The checklist below remains required; a working Qt RPC is not a UI-path pass.

1. Launch the built Qt application with the fifth isolated regtest datadir,
   `-enableflowmeshvalidator=0`, both `-flowmeshendpoint=https://127.0.0.1:<port>` values,
   and `-flowmeshendpointca=<fixture>/flowmesh-client-tls/ca.pem`. Retain the
   fixture's B3 consensus test arguments and ordinary B3 connection. Do not use
   a production wallet or change any headless operator's configuration.
2. Open Trade. Confirm the configured endpoint connection and remote-backend
   status are visible. Confirm the local engine, FN signing and independent
   operator listener are absent; no optional execution-history store appears.
   Discovery and market selection must still work without an FN coin or key.
3. Verify the selected chain/domain, canonical asset/B3 market, execution
   configuration and decimal units. Record the distinction between
   endpoint-reported rows, certificate-verified inclusion, whole-state-root
   verified balances, and B3 settlement. A certificate for a root alone must
   not turn an adjacent arbitrary account row into a verified balance.
4. Use the existing deposit dialog. Review the precise asset, amount, account,
   vault and native fee before local signing; cancel once and confirm that no
   transaction is sent. Then approve an isolated small deposit, wait for the
   existing depth, admit its outpoint, and observe credit and sweep status.
5. Place one small limit bid against the existing persistent-curve auction.
   Check the wallet/market/side/price/quantity/sequence confirmation, cancel
   that confirmation once, then explicitly approve. Observe local submission,
   endpoint admission, and certificate-verified inclusion as separate states.
   Initial elapsed time must not reset at admission or endpoint retry.
6. From a different isolated account, place the matching ask. Observe an
   actual fill and exact balance/fee changes, not merely an accepted RPC.
   Verify chart/book/own-order views remain internally consistent. Open and
   cancel a second order and observe the certified cancellation.
7. Have the test relay lose the selected endpoint's submit response **after**
   forwarding. Verify the UI shows unknown rather than definite rejection,
   then fails over to proof-backed status or offers retry of the saved action
   while its outcome remains unknown. Never resubmit an already certified
   action. If retry is needed, compare exact `action_hex` and ActionId at both
   relays. Account sequence must advance exactly once; no changed sequence or
   silently re-signed instruction.
8. Disconnect/reconnect event delivery. Switch to the independent endpoint
   instance and require an explicit cursor gap plus snapshot/resume, including
   when the durable head is unchanged. Verify no silent missing order/fill
   event and no stale or out-of-order rollback of displayed state.
9. Test an unavailable endpoint set, wrong-domain/market replies, missing or
   forged evidence, a false account row beside a valid certificate, oversized
   and malformed replies, wrong CA/hostname, and an expired certificate.
   Controls must fail closed or label cached/reported data honestly; no
   TLS-verification bypass or automatic submission is allowed.
10. Request a small withdrawal to an address generated in this ordinary test
    wallet. Review the exact destination and amount, observe the certified
    receipt separately from the connected B3 payout, and confirm the wallet's
    actual spendable output. Retain exact valid transaction parity checks from
    an engine-enabled node and the engine-off client.
11. Leave the UI idle/slow and disconnect it while operators continue trading
    and producing B3 blocks. Then close Qt. The independent headless services
    must remain running; no operator shutdown follows client teardown.
12. Relaunch the same ordinary client without any engine-enable option. The
    default must still be engine-off, trading must remain available, and its
    locally retained exact actions, no-replay markers and head high-water
    checks must survive. Event cursors are process-memory only: restart must
    recover verified state through a snapshot and report an explicit event
    gap, not claim replay of the missed events. Existing operator
    deployments require explicit `-enableflowmeshvalidator=1`; this does not register
    or revoke a seat or alter quorum.

`-enableflowmeshvalidator` is the only validator-engine enable flag and defaults
to false. No alternate enable flag or alias is part of this interface.
This is the current development default, not a future configuration change;
deployment and actual-Qt qualification still require the checks above.

## Evidence boundaries

Automated Qt parser/widget tests may prove confirmations, stale-state handling,
labels and worker teardown. They do not replace the actual backend/HTTPS trade
steps above. Mark every unexecuted item as not qualified. Keep client admission,
certificate formation, durable apply, client observation and endpoint recovery
timestamps separate; socket writes are not peer application acknowledgments.

Enabled/disabled B3 compatibility needs the same valid and invalid checkpoint
and vault-withdrawal fixtures under both modes. The existing checkpoint-index
tests cover connected proofs, bitmap/activation, destination/change and replay
rules; an empty-block sync test alone is not compatibility evidence.
