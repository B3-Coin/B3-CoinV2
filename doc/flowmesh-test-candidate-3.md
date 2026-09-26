# FlowMesh test candidate 3

Source identity: `1.1.5-flowmesh-test.3`, on the existing `flowmeshV2-dev`
branch. This is a local regtest candidate, not a mainnet upgrade, public release,
or declaration that native FlowMesh V2 is complete.

## Changes and intended checks

- Native Qt open/high/low/close candles, volume and pointer inspection.
  Select 1, 5 or 20 **microblocks** per candle. The current history has no
  authenticated trade timestamp: these are not minute candles. Missing clears
  do not manufacture prices; the bounded first/latest bucket may be partial.
- Exact reverse-price handling, including extrema reversal and refusal to
  invent a finite inverse candle containing a zero-price clearing. Floating
  point is used for drawing coordinates, not prices or signing.
- Exchange-style order-book display: red sell levels above green buy levels,
  price, amount, level total, last trade, spread and cumulative depth shading.
  It groups only exact limit-shaped orders. General curves remain available
  in the separate **Curve depth** view, whose sampled rows must not be summed.
  Matching stays a uniform-price auction, not a price-time-priority CLOB.
  The reverse view's B3 amounts are gross equivalents at each limit, not
  fixed-B3 fill promises. Partial books do not claim a complete-market spread.
- [Generated liquidity fixture](flowmesh-regtest-liquidity.md): four generated
  operators, an engine-off HTTPS client, six actual matched clears, then one
  resting bid and ask. No mainnet funds or existing market are used.
- Pending finality-pin catch-up accepts canonical history while enforcing the
  saved checkpoint's height/hash, even before that hash is indexed. Checked
  block/undo and index writes precede a new pin; write failures are explicit.
  The saved pin and all signer history remain intact.

The earlier source rejects canonical catch-up when a saved finality pin is
above the loaded tip. Separately, a pin can be written before its replayable
carrier data. Regression-only baseline results and repaired-tree results must
be recorded separately. Review also caught and corrected an intermediate
unknown-pin exemption: full conflicting blocks must not cross the saved pin.
This does not diagnose every community stall or repair orphaned signer locks.

## Focused local qualification

Build `b3coind`, `b3coin-cli`, `b3coin-qt`, `test_bitcoin`,
`test_b3_flowmeshworkspace-qt`, `test_b3_flowmeshtrading-qt`, and
`test_b3_flowmeshtradingpanel-qt` with GUI and GUI tests enabled.

```sh
build/bin/test_bitcoin --run_test=finality_pin_persist_tests,finality_pin_tests \
  --report_level=detailed --log_level=message
QT_QPA_PLATFORM=offscreen build/bin/test_b3_flowmeshworkspace-qt
QT_QPA_PLATFORM=offscreen build/bin/test_b3_flowmeshtrading-qt
QT_QPA_PLATFORM=offscreen build/bin/test_b3_flowmeshtradingpanel-qt
```

Then invoke the isolated functional fixture as described in its document.
The optional `renderCapturedEngineOffLiquidity` workspace-test slot reads its
exact public `getflowmeshmarketdata` JSON through `B3_FLOWMESH_CHART_CAPTURE`
and exports PNGs to `B3_FLOWMESH_CHART_GALLERY`. It must preserve endpoint-reported
history labels. The gallery is a widget rendering check, not attended Qt
signing, shutdown/reopen or cross-platform qualification.

## Release gates and unchanged limits

- Current native market execution is V1 pre-agreement plus the existing V1
  certificate. The new P2FV implementation gate stays off; V2 agreement models
  are not silently wired into live trading. No new latency result is claimed.
- No changes to fees, market identities, matching, quorum, signed actions,
  checkpoints, external bridge contract or mainnet activation height.
- Historical conflicting signer locks and bridge-finality recovery remain
  separate blockers. A B3 software upgrade cannot revoke previously issued
  external-contract-usable signatures. The bridge supported-fork horizon and
  lineage transition require their own verification and approved treatment.
- The original accessibility SIGSEGV remains OPEN. No recurrence is not a fix.
- Independent operator transport remains authenticated plaintext TCP. HTTPS
  trading endpoints still require valid hostname/CA verification. Loopback
  test endpoints do not serve a remote tester's computer.
- The pin tests exercise controlled process/chain-manager restart and injected
  file failures, not physical power loss. Existing directory-sync error
  handling and LevelDB coins-write semantics do not become a new atomic
  multi-file transaction through this repair.
- No generated wallet, operator key, TLS private key or signer journal belongs
  in a distribution archive. Testers create their own wallets. A shared regtest
  operator/network and real HTTPS access/trust must exist before handoff.
- This local executable links development-machine dependencies. Portable
  Windows/macOS packaging, actual dependency minimums, signing/distribution and
  connection from a separate tester machine remain distinct gates.

No release tag, production deployment, public package or mainnet migration is
implied by this candidate identity. Record the exact tested commit, binary
hashes, actual child exits and remaining failures with every handoff.
