# B3 FlowMesh candidate04: local, loopback-only closed test

This is not a public production release. Use only fresh generated wallets and
valueless regtest coins. Never import a holder wallet, WIF, seed, FN/BLS key,
signer journal or an earlier test wallet into this environment.

## Supported roles and platform

The ordinary Qt client has `-enableflowmeshvalidator=0`. It signs locally and
uses the verified remote HTTPS backend; a trader needs no FN Coin/operator key.
The local **test coordinator**, separately, runs four disposable operator
processes. This is Mode A infrastructure, not a requirement for normal traders.
FMN2 is authenticated plaintext TCP, restricted to loopback here. HTTPS does
not encrypt the operator transport.

The currently supported native package is Apple Silicon arm64, macOS26 or
later, with the exact dependencies in its manifest. Do not lower only the
Info.plist minimum. The default package procedure signs locally ad-hoc, not
Developer ID/notarized. Do not disable Gatekeeper or other system protections.
External distribution needs separately approved signing, private hosting,
availability ownership and reporting instructions.

## Build and start one fresh Mode A session

Use a clean reviewed source checkout and an output directory outside it. The
following names are placeholders for absolute paths you choose, not literal
commands with preconfigured developer paths. CMake, C++ build dependencies,
Python3 and OpenSSL must already be installed. `BUILD/test/config.ini` must
refer to this exact source and build. Do not use an old build or copied binary.

1. Configure a separate arm64 macOS build with `BUILD_GUI=ON`,
   `BUILD_GUI_TESTS=ON`, `BUILD_TESTS=ON`, `BUILD_DAEMON=ON`, `BUILD_CLI=ON`,
   `WITH_USDT=OFF`, `WITH_ZMQ=OFF`, `CMAKE_EXPORT_COMPILE_COMMANDS=ON`, and
   `CMAKE_OSX_DEPLOYMENT_TARGET=26.0`. Set the dependency prefix explicitly.
   Build the daemon/CLI and the focused test targets documented by the candidate
   qualification manifest. Retain actual command results. Do not infer a pass
   from a successful build.
2. Compute the built daemon SHA256. Start the bounded coordinator in a terminal:

   ```text
   python3 SOURCE/contrib/closed-test/local-pilot.py --build-dir BUILD --session-dir NEW_SESSION --profile-id client-YYYYMMDD-pilot04 --daemon-sha256 EXACT_HASH --duration 7200
   ```

   `NEW_SESSION` must not exist. The helper chooses available loopback ports,
   creates fresh operators/TLS keys, establishes the normal regtest market and
   certifying quorum, and writes `ready.json` only after verified endpoint
   responses. Bootstrap transactions may be manually relayed and are labelled;
   after readiness client transactions use ordinary B3 relay. This is not a WAN
   or whole-network performance test. Keep the coordinator terminal running.
3. Read `NEW_SESSION/ready.json`; no ready file means no usable session. The
   public enabled build profile is `NEW_SESSION/enabled-profile.json`. The
   session deadline includes setup/build time and a cleanup reserve. No
   automatic extension or background promise of availability exists.
4. Reconfigure the same clean-source build with
   `-DB3_FLOWMESH_CLOSED_TEST_PROFILE=NEW_SESSION/enabled-profile.json` and build
   `test_b3_flowmeshclosed-gui` and `test_b3_flowmeshclosed-policy`. Run the
   latter; retain exit status. Reconfiguring only this explicit external public
   profile does not modify source or reuse another session's private keys.
5. Collect exact dependency notices outside source (requires outbound HTTPS):

   ```text
   python3 SOURCE/contrib/closed-test/collect-notices.py --output-dir NEW_NOTICES --qt-version EXACT_QT_VERSION --homebrew-prefix DECLARED_PREFIX
   ```

   Collection records hashes and missing notices; it is not an external
   distribution/compliance approval or a substitute for a required source offer.
6. Package and verify the enabled application:

   ```text
   python3 SOURCE/contrib/closed-test/package-mac.py --build-dir BUILD --output-dir NEW_PACKAGE --profile NEW_SESSION/enabled-profile.json --candidate-id fm-closed-mac-YYYYMMDD-04 --macdeployqt EXACT_MACDEPLOYQT --qtsvg-framework MATCHING_QTSVG_FRAMEWORK --notices-dir NEW_NOTICES
   ```

   The script checks clean committed source, actual profile, dependencies,
   minimum OS, app-relative library references, app/dSYM UUID, local signature,
   archive extraction and hashes. An explicit `--source-manifest` may preserve
   an uncommitted source export when commit authorization is blocked; that is
   labelled **NOT clean-checkout qualification**, never SOURCE REVIEW READY.
7. On an unlocked screen start the extracted package under exact-child capture:

   ```text
   python3 SOURCE/contrib/closed-test/capture-qt.py --package-dir NEW_PACKAGE --session-dir NEW_SESSION --name first-start --timeout 1200
   ```

   Create the fresh wallet named `closed-test` in the normal UI. The guard fixes
   regtest, one loopback B3 peer, HTTPS/CA, no listeners/admin RPC, validator-off
   and isolated storage under `Library/Application Support/B3FlowMeshClosedTest/`
   plus its unique profile ID. Extra launch arguments/payment URIs are refused.
   Never delete a lock or repoint this app at a normal wallet.

The profile is **embedded at build time**. A different fresh coordinator has a
different generated CA/profile and needs a new package identity/build through
the same mechanism. Never distribute CA private keys or edit a frozen profile
to make an old binary connect. Reusing the same wallet is supported only with
its exact preserved profile and same retained session environment.

## Coordinator controls

All commands include `--session-dir NEW_SESSION`. Responses are private files;
queued is not successful. Read the numbered response before proceeding.

```text
python3 SOURCE/contrib/closed-test/local-pilot-control.py --session-dir NEW_SESSION snapshot
python3 SOURCE/contrib/closed-test/local-pilot-control.py --session-dir NEW_SESSION fund --address FRESH_PUBLIC_REGTEST_ADDRESS
python3 SOURCE/contrib/closed-test/local-pilot-control.py --session-dir NEW_SESSION requests
python3 SOURCE/contrib/closed-test/local-pilot-control.py --session-dir NEW_SESSION stop
```

Funding is one-shot, exactly3 valueless B3, to the tester's newly generated
address, never an operator address. `mine --blocks 31` waits for real-time B3
progress; `checkpoint` publishes only the existing pending market checkpoint.
Optional `hold_read --milliseconds 3500` and `clear_holds` affect passive test
responses only. They never sign, cancel or replace a client's instruction.

Follow TESTER-CHECKLIST.md. For same-wallet reopening use capture-qt with
`--reopen --name same-wallet-reopen`; profile and storage ownership are checked.
Capture actual UI Quit and the direct child exit separately. A timeout or
cleanup signal is not a clean-Quit pass. An HTTPS operation deadline is not a
promise of five-second total shutdown.

## Stop and preserve

Quit Qt normally before stopping the coordinator. Read its exit.json and
confirm every owned operator exited0 with Shutdown done and no remaining
owner. The helper normally stops/reaps only its own children; if bounded
graceful cleanup reports a remaining process, inspect that exact test process,
do not delete data/locks or start another signer. All session data is preserved.
Services are unavailable after stop/expiration. Saved instructions remain;
closing a window or cancelling a local HTTP request is not order cancellation.

Mode B shared private hosting is NOT supplied by loopback URLs. A designated
approved host/network/operator, reachable private endpoints, valid trust,
compatible testers, private report destination and distribution approval are
separate prerequisites. No service should remain implied always-on after the
task ends.
