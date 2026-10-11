# Regtest release gates and local qualification

## October 11 renewal update

The owner authorized renewal of the isolated regtest service and certificate,
with no mainnet changes or data reset. Its HTTPS leaf now expires October 21
at 14:02:47 UTC and verifies under the unchanged pinned public CA. The declared
session ends October 18 at 00:00 UTC; the lifetime 128-operation limit remains
unchanged, with two operations already completed. This is a bounded renewal,
not an indefinite service promise.

Only the API-owning regtest node was cleanly restarted, using the same binary,
configuration and data directory. All four finality signer journal files were
byte-identical before and after restart. Data-directory identities, CA, existing
server key, CSR and all four node configurations were preserved. The old leaf
and maintenance script remain archived on the VPS. Mainnet's process identity,
executable hash, configuration hash and restart count were unchanged.

Renewal inspection found a separate operational blocker before starting the
checkpoint manager. Its unchanged helper broadcasts a checkpoint and then
calls `confirm()`, which calls `mine_to()` with the current height plus missing
confirmations. `mine_to()` has a bootstrap-only upper bound of 10,000, while
the preserved test chain is at 100,837. Starting the manager would therefore
risk an avoidable failure after broadcast. It remains stopped, with no new
checkpoint attempt, transaction, manual signer action or data repair. All four
replicas agreed on the existing tip; native finality lineage was unbroken.
Read-only pre-arm checks passed, but they do not repair the confirmation bound.

The renewed leaf's DER SHA256 is
`33466d27abd4ae34bf0574a4991b652070570676382997e8563f10327e935454`.
The existing profile and CA bytes remain unchanged. Forty-six packaging,
session and workflow checks passed after making negative date tests relative
to the declared window instead of assuming October 13 is always expired.
The live TLS preflight returned `WINDOW_VALID_TLS_VERIFIED` on October 11
at 14:03:40 UTC. It explicitly reports `full_release_qualified: false`.

GitHub builds may produce review artifacts, not a public release or proof of
network trading readiness. Maintenance recovery, tester-network trading and
the existing unresolved limits below remain gates. No binary upgrade, mainnet
activation, wallet funding or data migration is part of this renewal.

## Earlier frozen qualification

The local trading round trip and Mac guarded-client checks pass. Tester
distribution remains blocked by the expired declared service window and the
remaining platform and tester-network qualification. The VPS is unchanged;
there was no push, live wallet operation, public release or activation.

The frozen release-tooling revision is
`c3811e11899af8a5a871aa651d8a64882a21ea19` on `flowmeshV2-dev`, following
`316721a16a3037114774b17687685787b9432f86`. Unrelated uncommitted V2 market
work remains outside the release candidate. This report and the README update
are documentation-only successors.

## Release tooling corrections

The tester-build workflow previously watched packaging and a few Qt files,
excluding staking, networking and most native dependencies linked into the app.
It now watches `src/**` and the relevant CMake, dependency and packaging inputs
on the existing V2 branch. Permission remains `contents: read`; artifacts are
not releases and there is no deployment job.

The package validator previously accepted a ready profile whose availability
was only descriptive text. The immutable profile still declares maintenance
ending October 11 at 00:00 UTC and leaf expiry October 12 at 14:59:29 UTC.
The new [session window](../../contrib/flowmesh-regtest/session-window.json)
and [preflight](../../contrib/flowmesh-regtest/session_preflight.py) make those
existing limits machine-checkable without rewriting the profile's storage
identity. The one-hour handoff margin is an explicit tooling policy, not a
consensus or economic rule. No new service window was authorized or fabricated.

The current preflight returns `NOT_READY`, exit 1, because maintenance has ended.
Even `--probe-tls` refuses before connecting in this state. When the declared
window is valid, that optional mode performs one CA- and hostname-verified TLS
handshake; it neither disables verification nor sends an HTTP/action request.
It checks the observed leaf expiry against the recorded value. TLS availability
does not prove chain identity, quorum, funds, trading or full release readiness.

The window is checked before bundling and again before copying checked artifacts
from private staging to the requested handoff directory. A changed source or
expired window leaves no final handoff directory. Copying is exclusive but not
an atomic multi-file publication: a copy failure may leave partial output.
Only exit-zero completion can authorize selecting an artifact; ordinary CI
does not upload failed package output.

## Executed local tests

All commands below ran against committed source, not the unrelated dirty
market prototype. No test connected to the VPS or any external chain.

| Qualification | Actual result |
| --- | --- |
| Session/preflight unit suite | 10 passed, exit 0 |
| Packaging unit suite | 28 passed, exit 0 |
| Workflow contract suite | 8 passed, exit 0 |
| Workflow YAML syntax | Passed with Ruby Psych |
| Mac arm64 guarded app, policy and dialog builds | All three targets built, exit 0 |
| Mac launcher/storage policy QTest rows | 239 passed, zero failed/skipped, exit 0 |
| Mac wallet-dialog QTest rows | 12 passed, zero failed/skipped, exit 0 |
| Compiled app identity and isolated build configuration | Exact `c3811e11899a` identity accepted |
| Actual stale-session package invocation | Refused with exit 1 before output creation, as required |
| Native engine-off HTTPS trading round trip | Passed, exit 0; five disposable local daemons |

The QTest totals include framework initialization/cleanup rows and data rows;
they are not an assertion count or the full native/Qt test suites. Pure suites
were rerun from the clean exact-commit checkout. The actual package refusal
preceded notice collection or deployment; it is not a successful bundled,
sealed or extracted archive qualification.

Build platform: macOS arm64, AppleClang 17, Qt 6.11.1, RelWithDebInfo, deployment
target 15.0, wallet on, IPC off. Qt tools are installed separately from Qt base;
explicit `Qt6LinguistTools_DIR` resolved the local configure failure without
changing system packages or bypassing version checks. The compiled guarded app
SHA256 is `57071106b79d5de17b7f9e3b290503e2dd2b8304bf7e1863bc9b66ad68279fc5`.
This is the build executable, not a sealed distribution archive.

The native round trip used the existing frozen daemon source
`66701513201a47d36c06aec260f98f01bf63c6ab`. Its relevant native sources and
functional scripts are unchanged by `316721a` and `c3811e1`, apart from Git
version identity when rebuilt. Daemon SHA256 was
`eae955617af7867a2a2118a0c5101f5de9d3df256de25470b5d71861660a5fc0`.
The run used seed `20261011`, port seed 136 and `--check-default-off`, and
completed October 11 from 11:39:27 to 11:41:55 UTC. All five test children
stopped with their expected exit-zero status.

Observed assertions included actual B3 and test-asset deposits, 30-deep
certification, checkpointed custody sweeps, matched bid/ask balances and exact
fees, cancellation release, a connected type-9 payout and consumed-receipt
refusal. Four operators restarted and one reindexed without losing state.
An engine-off HTTPS client retained five exact original ActionIds across
restart and made zero duplicate submissions, including the synthetic unknown
outcome scenario. Default-off startup and type-8/type-9 operator/client
validation parity also passed. This is a local native HTTPS result, not a Qt
click-path test, H1 activation drill, WAN measurement or latency benchmark.

Raw disposable logs and the JSON result remain outside the repository. The
first functional invocation refused an already-existing temporary root before
starting nodes; the successful rerun used its fresh child directory. This
setup refusal is not counted as a protocol failure or a passing test.

## Review findings and corrections

A separate read-only review found two tooling issues in the first draft:

1. The final window check initially ran after writing apparently complete
   handoff artifacts. The fix stages them privately and checks the window and
   source before copying. New regressions verify no handoff output on expiry
   or changed source, exact copied bytes and no overwrite of frozen output.
2. CI initially did not run the new session tests or current-window check.
   Both now run unconditionally before platform jobs, including build-only
   requests. The configured TLS context, rather than a separate default
   context, is also checked by the regression.

The focused re-review reran all 46 pure tests and found no new defect within
this bounded packaging/preflight scope. That is not a security audit of V2 or
production approval. Profile hash binding establishes identity, not permission
to renew an operator's budget; a trusted source edit still requires explicit
service-window/certificate approval.

## Reproduction and remaining release gates

From the exact source revision:

```sh
python3 -B contrib/flowmesh-regtest/test_session_preflight.py -v
python3 -B contrib/flowmesh-regtest/test_package.py -v
python3 -B contrib/flowmesh-regtest/test_workflow.py -v
python3 -B contrib/flowmesh-regtest/session_preflight.py
```

The last command must currently refuse. After approved renewal, use its
explicit `--probe-tls` mode for live certificate evidence, followed by separate
read-only chain, quorum and readiness checks. Renewing a leaf under the existing
approved CA need not rewrite the embedded profile; changing trust or endpoint
requires its own reviewed compatibility treatment, not deleting markers.

For the native end-to-end case, using a wallet-enabled build and a fresh unused
child directory:

```sh
python3 test/functional/feature_flowmesh_remote_client.py \
  --configfile=build/test/config.ini --tmpdir=/absolute/fresh/test-child \
  --nocleanup --timeout-factor=1 --randomseed=20261011 \
  --portseed=136 --check-default-off
```

Remaining gates are a renewed service/certificate window; a verified operator
configuration aligned with the intended experiment; Windows and Intel Mac
builds and platform launcher checks; actual bundle dependency/license/seal and
extraction verification; and a bounded tester-network trading smoke test.
GitHub CI has not been executed for this local revision. No mainnet height was
selected. The current tester product remains V1-based spot trading with
informational futures; this is not completion of shared-balance V2, native BFT,
PoS V2, bridge V2 or futures margin/liquidation. The previously documented
accessibility SIGSEGV remains unresolved.
