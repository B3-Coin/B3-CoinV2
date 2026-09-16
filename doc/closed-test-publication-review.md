# Closed-test source publication review — 2026-09-14

This is a source-review checkpoint, not production-release or external binary
handoff approval. Earlier candidate artifacts and qualification evidence remain
frozen. No live wallet, production signer or real funds were used in this pass.

## Source identities

The review branch is `review/closed-test-20260914-candidate04`, based on
`372adcbd8e7836b37791c8da1adc076292748bb2`. The already-published wallet-safety
commits are reused, not duplicated. The integration contains its source
prerequisites; it does not depend on untracked whole-file snapshots.

`f9f639efff283401507625e604cb375938a4e270` reproduces the preserved frozen
Candidate04/05 source content and modes: 3,627 files, source-manifest SHA-256
`879d8f81c57676911b20e269054649a2ac527c7a54b6be899d67f3f279bc1027`.
This is a source-composition identity, not a claim that those historical binaries
were originally built from that newly created Git commit.

The newly compiled and tested detached checkout is
`98ff213c6f2e9dd3eeb69be366eb63f28e9ace20`, tree
`d84776a9bc901aa88b6d5d04e768b27e30688486`, source-manifest SHA-256
`8c109057c88b1091797b3337b6c692dc45665cd3e2b02b4f7baa911547573be8`.
The only changes after the frozen composition are five helper/test files,
committed separately in three units:

- supported HTTPS forms in `capture-qt.py`, `package-mac.py`, `test-tools.py`;
- bounded test-suite selectors in `run-qualification.py`;
- validator-disabled observation in `wallet_readonly_reconcile.py`.

There is no later C++ change. This note is a subsequent documentation-only
commit; the recorded compiled source remains the exact commit above.

## Newly executed checks

A clean Apple Silicon Debug build completed successfully using Qt 6.11.1 and
an actual minimum macOS target of 26.0. Nine selected targets, including daemon,
CLI, Qt and affected test executables, built from the detached committed tree.
No replacement candidate archive was produced.

| Check | Result |
| --- | --- |
| FlowMesh core | PASS, 98 cases |
| HTTPS/client core | PASS, 29 cases |
| Wallet core | PASS, 14 cases |
| Qt trading | PASS, 25 cases |
| Qt workspace | PASS, 71 cases; one explicitly optional PNG-export check skipped |
| Qt trading panel | PASS, 5 cases |
| Qt stake coin control | PASS, 12 cases |
| Read-only reconciliation unit | PASS, 23 cases |
| Read-only reconciliation functional | PASS, isolated generated nodes |
| Ordinary-client functional | PASS, isolated generated nodes; real block-validation parity included |
| Closed-client policy | PASS, 69 cases with completed cleanup |
| Packaging helpers / local control helpers | PASS, 8 / 3 cases |

The ten-group qualification driver exited 0, reported unchanged source
identity and no unexecuted selected groups. These automated checks do not
qualify actual screen interaction. An initial policy wrapper was unable to
inspect cleanup processes under the sandbox despite passing its assertions;
that capture remains incomplete. The separately approved repeat captured
successful cleanup and is the clean policy result above.

## Targeted manual checks

The owner used ordinary interaction to switch the preserved generated
stake-owner wallet to the generated watch-only wallet. In a second check, one
ordinary owner output was deliberately selected, then the owner switched back
to the watch-only wallet and opened coin selection. The watch-only wallet
showed Quantity 0 with no inherited owner selection. Captured wallet identity,
private-key-disabled UI and unsigned-only controls agreed. No recipient,
transaction confirmation or wallet unlock was required. Native Quit completed
with child exit 0, `Shutdown done`, no timeout or cleanup signal, and no
remaining test child.

Saved order-to-cancellation selection in the preserved Candidate05 wallet is
pending the owner's final attended interaction. Its historical saved cards
can be inspected offline; stopped HTTPS services do not establish live status
or fresh proof. Do not infer this manual result from an RPC or model-index test.

## Initial deposit-gap correction

The retained Candidate05 trace now establishes that the original first deposit
admission request reached the operator HTTPS server and received upstream
HTTP 200. A later retry retained the identical ActionId and canonical admission
payload bytes. This deposit-credit action is keyless and does not allocate an
account sequence; the trading orders and cancellations are signed instructions.
An offline HTTPS operator or failure to reach that server is therefore not
the explanation for that first attempt.

The first missing observation is the original HTTP 200 **application response
body**, including its admission/refusal result and reason. The API can return
HTTP 200 with a `rejected` application receipt. A socket write or HTTP success
is not runtime admission, certification, durable credit or proof that the
client application processed the response. The precise original refusal/cause
is still unknown; no timeout, packet-loss or quorum cause is asserted.

One new generated fixture captured its first response explicitly as `queued`,
then admission, verified certification and identical durable application on
all four operators. A subsequent already-certified retry made no new submit
request. This did not reproduce the original gap, so no production repair was
made. The overall diagnostic fixture nevertheless exited 1: a later convergence
observation hit `RemoteDisconnected` on an idle admin-RPC connection. That
failure is preserved separately from the completed delivery assertions; it is
not an overall green functional run. All five generated processes subsequently
exited 0 with `Shutdown done` and their data retained.

For a genuinely uncertain action, the existing recovery is status inspection
and, where permitted, an explicit retry of the same saved ActionId and bytes.
It is not a replacement deposit, new sequence or history reset. The original
Candidate05 deposit is already certified and must not be resubmitted as a new
instruction.

## Frozen package and handoff limits

Candidate04's failed reopen and Candidate05's successful local session remain
separate evidence. Candidate05 is still a LOCAL PILOT. Its artifact/profile
identify candidate05, but bundle/version metadata and packaged README retain
candidate04 values. Do not modify the frozen artifacts to conceal this. Any
future external successor needs a new consistent identity and recorded hashes.

The profile availability text describes the intended service window; it is
not itself a parsed expiry guard. Offline inspection does not bypass a guard
or restore services. The prior loopback endpoints are stopped and cannot serve
another tester's computer.

External handoff still requires an approved isolated shared host/private
network, named availability operator, real private B3 and hostname-verified
HTTPS access/trust, compatible tester platforms, private reporting destination,
and approved signing/distribution procedure. Current packaging is local
ad-hoc-signed arm64/macOS 26.0 Debug, not a notarized external distribution.
Do not disable system protections or lower only the declared OS requirement.

The original `accessibilitySelectedChildren` SIGSEGV remains **OPEN /
UNRESOLVED**, distinct from repaired lifecycle/startup/dialog issues. No
recurrence is not a repair. FMN2 remains authenticated plaintext TCP in a
controlled operator environment, not QUIC/encrypted transport. WAN,
crash/power-loss recovery, other platform packaging/durability and public-service
availability remain outside this qualification. No market, fee, quorum,
membership, signing-history or settlement-rule change is introduced by this
publication pass.
