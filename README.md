# B3 Hive

## This branch: `r0-rebirth`

This is a review branch for the project owner, not a release. It builds
**1.1.5-flowmesh-test.6** and is `rebirth` (the owner's from-scratch FlowMesh
V2 work in `src/flowmesh2/`, see
[the owner plan](doc/design/b3-flowmesh-greenfield-plan-20261016.md)) plus:

- the non-consensus fixes proposed for an interim repair release: faster
  startup (parallel block-index hashing, progress while loading), stake
  release from the wallet, the wallet tool without a linked node, fee-warning
  thresholds in the B3 display unit, crash reports, Qt padlock and chart
  fixes, an MSVC build, one-line RPC help for nested objects, and test and CI
  hygiene;
- FlowMesh V1 runtime hardening and agreement views;
- the fixes found while integrating `rebirth`: a test race in the FlowMesh
  API health suite, a staking test that skipped its proposer round in Debug,
  and duplicate queued copies of retained local actions;
- gap-filling work on FlowMesh V2 in `src/flowmesh2/`, each item with a
  failing-first test and its row in
  [`src/flowmesh2/TODO.md`](src/flowmesh2/TODO.md): the M0-AUD-01 audit,
  portable regtest tool paths, journal segments and recovery, state
  snapshots, decoder fuzzing, seat-mesh drop reasons, the fixed-capacity
  band book, copy-on-write slot state, sparse Merkle commitment v1, short
  IDs with body fetch, seat counts of 4 to 7 in the regtest PBFT, and a
  constant-time signer-log lookup.
  Where a step needs an owner ruling, the row says BLOCKED-ON-RULING and the
  code stops at the groundwork. Every open question is collected in
  [doc/design/b3-rebirth-owner-questions.md](doc/design/b3-rebirth-owner-questions.md);
- the interim repair status in
  [doc/design/b3-interim-repair-status.md](doc/design/b3-interim-repair-status.md),
  with tests showing that a lowered finality quorum cannot unfreeze the
  broken lineage, and the H1 escape-race check (the `escape_race_*` cases
  and the bounded model in [`test/b3_h1_escape/`](test/b3_h1_escape/));
- a proposal for bringing height C forward:
  [doc/design/b3-height-c-fast-track.md](doc/design/b3-height-c-fast-track.md),
  with the gap list, a B0 undo design, a comparison of escape rules and a
  bounded model of Finality V2 voting against reorganizations
  ([`test/b3_p3_reorg/`](test/b3_p3_reorg/)). Nothing in it is implemented
  or ruled;
- on Windows, a refusal at startup of a `-datadir` too long for MAX_PATH
  (with a message naming both remedies) and the `longPathAware` manifest
  setting;
- **B3 Hive, the new desktop app** in [`hive/`](hive/README.md), a Tauri 2
  shell around an unchanged `b3coind`.

Consensus and mainnet chain parameters are byte-identical to `rebirth`. The
FlowMesh V1 tester notes in [FLOWMESH-TESTING.md](FLOWMESH-TESTING.md) still
describe the opt-in preliminary-agreement mode and its known stall; nothing on
this branch repairs existing locked markets. Use generated wallets and
valueless coins on an isolated test network. Build artifacts and passing
tests are not end-to-end certification or deployment approval.

B3 Hive is the desktop wallet and full-node software for the B3 FlowMesh
network. It downloads and validates the B3 chain locally, manages
user-controlled keys, and provides the foundation for B3's modern protocol
components.

Further information is available in the [doc folder](doc/).

## Programs

- `b3coin-qt` — the Qt desktop wallet (also called B3 Hive)
- `b3-hive` — the new desktop app built from [`hive/`](hive/README.md); it
  ships `b3coind` beside itself as `b3-hive-node` and the update helper as
  `b3-hive-update`
- `b3coind` — headless full node
- `b3coin-cli` — RPC command-line client
- `b3coin-wallet` — offline wallet utility
- `b3-hive-update` — the app's update helper, pinned to a release channel
  (`-DB3_HIVE_UPDATER=ON`, see
  [doc/design/b3-hive-update-system.md](doc/design/b3-hive-update-system.md))
- `b3-bridge-ethcheck` — optional bridge proof verifier and canonical payload
  emitter (`-DB3_BRIDGE_TOOLS=ON`)
- `b3-bridge-arbitrumcheck` — optional offline checker of bounded Arbitrum
  Nitro history proofs (`-DB3_BRIDGE_TOOLS=ON`)
- `b3-bridge-bootstrap-proof` — optional offline 3-of-4 bootstrap proof and
  Ethereum initialization-calldata builder (`-DB3_BRIDGE_TOOLS=ON`)

Linux operator archives also include the automatic Ethereum-to-B3 relayer and
its fail-closed operating guide under `contrib/b3bridge/`. Bridge tools being
present in an archive does not activate an unconfigured mainnet bridge.

## B3 Hive desktop app

[`hive/`](hive/README.md) holds a replacement for the Qt wallet: TypeScript
pages in a Tauri 2 window, a Rust core that runs the packaged `b3coind` over
loopback RPC with a per-launch login and an RPC whitelist, and separate
approval and passphrase windows for everything that moves value or unlocks a
wallet. It runs B3 mainnet in the legacy data folder, so a Qt user finds their
chain and wallets. Pages: Home, Send, Receive, Activity, Stake, Trade
(FlowMesh markets, with a demo market while the network has none, and a
read-only card for a FlowMesh V2 regtest operator's HTTPS API), Address
book, Wallets, Console, Network, Settings and Updates. It is a prototype
offered for comparison with the Qt wallet, not a release; the
[hive README](hive/README.md) has the build, run and test instructions and
the list of what is not finished.

## License

B3 Hive is released under the terms of the MIT license. See
[COPYING](COPYING) for more information. The source retains the copyright and
attribution notices of Bitcoin Core and other upstream projects from which it
derives.

## Development process

Development branches are built and tested regularly, but are not guaranteed to
be stable. Release branches and version tags identify reviewed releases. Check
each release's notes for its exact signature and checksum guarantees.

The contribution workflow is described in
[CONTRIBUTING.md](CONTRIBUTING.md), and useful guidance is available in
[doc/developer-notes.md](doc/developer-notes.md).

## Testing

Testing and code review are essential for security-sensitive software.
Developers are strongly encouraged to add [unit tests](src/test/README.md) for
new behavior and regressions. Unit tests can be compiled and run with `ctest`.

Regression and integration tests live under [test](test/) and can be run with
`build/test/functional/test_runner.py` when their dependencies are installed.
Release changes must be tested on all supported platforms.

### Manual quality assurance

Changes should be tested by somebody other than the developer who wrote the
code, especially when they affect wallet storage, transaction encoding,
consensus boundaries, or release packaging. A clear test plan should accompany
changes that are not straightforward to verify.

## Translations

Translation sources and inherited locale files live under `src/qt/locale`.
See the [translation process](doc/translation_process.md) and
[translation strings policy](doc/translation_strings_policy.md) before making
translation changes.
