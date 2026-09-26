# First full-node connection: verified TEST bootstrap (2026-09-26)

## Result, not a new latency claim

The existing real-daemon regtest campaigns were not forgotten or replaced.
They already exercised independent FMN2, an engine-off HTTPS wallet, actual
fills and advancing B3 using V1 preagreement plus V1 final certificates.
The newer recovery worker instead used one synthetic-base instance and pipes.
This change implements only the first missing connection between those paths:
a real node can verify a selected V1-certified base and derive a TEST instance
from its own B3 authority, without enabling a local FlowMesh engine.

Frozen implementation and functional test:
`22cb4ada6e431b61d4ffca4e68736d1bbde525f5` on `flowmeshV2-dev`.

Newly executed, not inherited passes:

* Four generated regtest operators and one engine-off client derived exactly
  the same instance. Python independently reproduced its 1,242-byte preimage.
* Thirteen malformed/forged/incorrect-context inputs were explicitly rejected,
  including a state larger than 8 MiB before its hex-to-byte allocation.
* The instance remained identical as real B3 advanced from height164 to167.
* The same generated engine-off wallet reopened, remained accessible and
  verified the same base. No client action was signed or submitted.
* Ten actual child launches exited0, including the original pre-bootstrap
  restarts and the client reopen. All five logs recorded two `Shutdown done`
  events. No test process was left running.
* Fifteen focused native tests passed: eight BLS-certificate/authority-cache
  cases and seven client-evidence cases, totaling1,914 assertions. Other1,690
  native cases were intentionally skipped, not reported as passes.
* The existing P2FV proof checker was rebuilt and passed46 assertions.
* A separate build-OFF control launched one generated engine-off regtest node.
  Both experimental RPCs returned `Method not found` (-32601), not a parameter
  or authority rejection. Its actual child and harness exits were0.

Build: AppleClang17, RelWithDebInfo, M4 Max, macOS26.5.2. Gate-enabled daemon
SHA256: `8bec577a826e7d46608991385d78b4b276166550abd8be152dbfdfb459baf745`.
[Public generated-data capture](captures/regtest-bootstrap-20260926.json)
SHA256: `d9d431ebc6e0615aedddaf8f1d58a56b70800fbb227569f02378b1837d55ec15`.
Private disposable wallets/databases/TLS keys are not publication content.
Build-OFF control daemon SHA256:
`976c212b00679e2c4d19f19f6b7fddf1883eb8df12becf22a38ef67aec8e5a35`.
It uses the same committed C++ source with the gate disabled; the additional
negative-control harness and report do not change native source.

## Versioned TEST profile and exact boundary

`TEST-P2FV-fixed4-views0to2-bootstrap-v1` is an observation of a pinned existing
V1 head. Both RPCs are compiled only with the separate default-OFF
`BUILD_FLOWMESH_P2FV_REGTEST_GATE` option, and reject non-regtest use.

* `getflowmeshregtestbootstrap(market_id, expected_head)` exports one exact
  existing certificate and canonical state, and verifies them independently.
* `verifyflowmeshregtestbootstrap(market_id, expected_head, certificate, state)`
  works without a FlowMesh service or wallet keys. It trusts no supplied roster,
  market symbols, endpoint status or latest-tip claim.

Both resolve the market and exact four-seat roster locally, validate PoPs and
the V1 certificate/state, enforce deep canonical anchors, reject connected
checkpoint conflicts, handoffs and counter overflow, then recheck the actual
authority. A harmless tip advance is not a different authority. Ordinary
derived chain indexes may synchronize; no client/signing journal is altered.

Instance encoding is `TaggedHash("B3/TEST-ONLY/P2FV/REGTEST-BOOTSTRAP/INSTANCE/1")`
over the fixed-width fields in `src/test/flowmesh_p2fv_bootstrap.cpp::Instance`:
version, genesis/domain, market/base/vault/configuration, epoch/set hash,
seat and entry anchors, exact base hash/root, next sequence/effect cursor and
four ordered seat/outpoint/key/PoP tuples. The functional test independently
implements the same declared encoding. Seat order is binary canonical order,
not the lexicographic ordering of displayed hex strings.

Every output says `cutover_authorized:false` and `execution_authorized:false`.
A local base pin does NOT create protocol-cutover authority, prove the latest
uncheckpointed head or its ancestry, or replay every earlier trade. No TEST
signature is accepted as a V1 certificate or B3 settlement proof.

## Reproduce without live data

From the repository root, use a separate build directory and leave `--tmpdir`
unset so the framework creates its own disposable directory:

```sh
cmake -S . -B build-p2fv-regtest -DBUILD_GUI=OFF -DBUILD_TESTS=ON -DBUILD_FLOWMESH_P2FV_REGTEST_GATE=ON -DBUILD_FLOWMESH_FASTPATH_PROBE=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-p2fv-regtest --target b3coind b3coin-cli test_bitcoin flowmesh-p2fv-proof-check -j 8
python3.14 test/functional/feature_flowmesh_p2fv_bootstrap.py --configfile=build-p2fv-regtest/test/config.ini --nocleanup --latency-production-logging
build-p2fv-regtest/bin/test_bitcoin --run_test=flowmesh_client_evidence_tests,flowmesh_bls_certificate_tests --report_level=detailed
build-p2fv-regtest/bin/flowmesh-p2fv-proof-check
cmake -S . -B build-p2fv-regtest -DBUILD_FLOWMESH_P2FV_REGTEST_GATE=OFF
cmake --build build-p2fv-regtest --target b3coind -j 8
python3.14 test/functional/feature_flowmesh_p2fv_build_gate.py --configfile=build-p2fv-regtest/test/config.ini --nocleanup
```

The bootstrap fixture overrides the broad inherited campaign. It does not
repeat withdrawals, block parity, a latency campaign or Qt interaction. Its
result file contains actual child exit statuses and explicit qualification
flags. Do not supply a holder's datadir, keys or wallet.

## Review and remaining integration

A separate read-only context checked the source and actual capture; it also
independently reconstructed the instance and all four seat IDs. No concrete
blocker was found within this observation-only boundary. This is bounded code
review, not an external audit or approval to activate consensus.

This run used one sequence0/effect0 base. It did not exercise nonempty trading
state, multiple instances, a chain reorg/authority race, changed roster,
handoff, shallow anchor or newer conflicting connected checkpoint. Those code
refusals are not claimed as dynamically qualified by the thirteen mutations.
The non-regtest runtime rejection was source-reviewed, not exercised on a
live/non-regtest node. The build-OFF method-absence control is separate.

Next work remains the real execution/multi-instance store and client-proof
adapter, then independent-network delivery/recovery. Only after those exist
can one new matched fill, coordinator loss, a recovered fill and catch-up be
measured through the ordinary engine-off client while B3 advances. The earlier
approximately79ms standalone core figure is NOT this daemon or recovery code's
latency. No new speed result, Qt/WAN result or external-tester readiness is
claimed here. V1 operation and its histories remain unchanged.
