# B3 Hive desktop app

A replacement for the Qt wallet, built beside it: TypeScript pages in a
Tauri 2 window and a Rust core that runs the unchanged `b3coind` as a
sidecar. Nothing here touches consensus; the node is the one built from
`src/`. The app is a prototype offered for comparison with the Qt wallet,
not a release.

## How it is put together

- **`web/`**: the pages (`index.html`, `src/*.ts`), the approval window
  (`approval.html`, `approval/main.ts`), the passphrase window and the
  console. Plain TypeScript, no framework; the CSS under `web/shared/css/`
  is the b3_docker design (same tokens, three screen sizes: phone, tablet
  rail, desktop rail).
- **`src-tauri/src/`**: the Rust core. `node.rs` starts and stops `b3coind`
  with a per-launch `-rpcauth` login and the RPC whitelist the pages may use
  (`PAGE_METHODS`); the console window gets a second login with full
  access. `drafts.rs` holds every request that moves value until the
  approval window decides it, so a page can never send on its own. `stake.rs`,
  `flowmesh.rs`, `wallet.rs`, `receive.rs`, `psbt.rs`, `settings.rs`,
  `folder.rs`, `cli.rs`, `update.rs` and `bootstrap.rs` each cover one page
  or one job.
- **Windows the core opens**: the approval window shows exactly what a
  request does and sends it only when approved (a locked wallet unlocks for
  that one request and locks again); the passphrase window unlocks a wallet
  to encrypt, change its passphrase or start staking, never for a page.
- **Sidecars**: the bundle installs `b3coind` next to the app as
  `b3-hive-node` (so it never clashes with an installed `b3coind`) and the
  update helper as `b3-hive-update`.

## Running it

The app runs **B3 mainnet** by default, in the legacy client's data folder
(`%APPDATA%\B3-CoinV2`, `~/.B3-CoinV2`, `~/Library/Application Support/B3-CoinV2`),
so a Qt user finds their chain and wallets. A folder without a chain starts
the setup wizard, which can install a chain bootstrap from the explorer.

B3-Qt's node options work on the command line and go to `b3coind` as they
are, for example `b3-hive -datadir=D:\b3 -port=5649 -listen=0 -debug=net`.
`-datadir` and `-regtest` are taken by the app itself. The options the app
must own (`-server`, the RPC login and whitelist, `-daemon`, the network
selectors other than `-regtest`, the fee bounds) are refused with a message
naming them. Qt-only options (`-lang`, `-min`, `-splash`, ...) are dropped.

Environment variables, all for development:

| Variable | Effect |
| --- | --- |
| `B3_HIVE_NETWORK=regtest` | A peerless regtest node in the app's own data folder (`-regtest` on the command line does the same) |
| `B3_HIVE_DATADIR` | The data folder for either network; `-datadir` on the command line wins over it, both win over the folder chosen on the Settings page |
| `B3_HIVE_B3COIND` | Another `b3coind` than the packaged sidecar |
| `B3_HIVE_REGTEST_ARGS` | Replaces the peerless regtest defaults, to join a local test network; mainnet refuses it |
| `B3_HIVE_SETUP=1` | Shows the setup wizard on regtest too |
| `B3_HIVE_UPDATE`, `B3_HIVE_UPDATE_ARGS` | Another update helper, and extra arguments only unpinned developer builds of it accept |

Files the app keeps in its config folder (`%APPDATA%\co.b3coin.hive` on
Windows): `folder.json` (the data folder chosen on the Settings page, used
from the next start), `display.json` (theme and hidden amounts) and
`hive.log` (every node start, stop, kill and whether the stop RPC was
accepted; the first thing to read when the app or the node misbehaves).
Node settings made on the Settings page go to `settings.json` in the data
folder, which B3-Qt reads too.

Closing the window stops the node first and waits for it (up to ten
minutes), so a second launch never finds the data folder locked. While this
wallet stakes with a bound finality key, closing asks whether to revoke the
key first, because an offline validator still counts toward the finality
quorum.

## Building

Needs a Rust toolchain (stable), Node 22 and, on Linux, Tauri's packages
(see `TAURI_LINUX_DEPS` in `.github/workflows/hive.yml`). Dependencies are
pinned in `package.json` and `Cargo.lock`.

```sh
cd hive
npm ci
npm run build                      # type-check and build the pages into dist/
cd src-tauri && cargo test         # the Rust core's unit tests (no node needed)
```

The packaged app needs both sidecars staged first. Build `b3coind` and the
update helper from `src/` (the helper with `-DB3_HIVE_UPDATER=ON`; how its
release channel is pinned is in
[doc/design/b3-hive-update-system.md](../doc/design/b3-hive-update-system.md)),
then:

```sh
scripts/stage-sidecar.sh b3-hive-node   /path/to/b3coind
scripts/stage-sidecar.sh b3-hive-update /path/to/b3-hive-update
npm run package                    # .deb on Linux, an NSIS installer on Windows
```

For a Windows app from WSL, stage the MinGW builds with the app's target
triple, as the script's header shows. `npm run tauri dev` runs the app with
the pages served by Vite, with live reload.

The CI workflow `.github/workflows/hive.yml` does all of this on every push:
type-check, `cargo fmt`, `clippy`, the unit tests, a static Linux `b3coind`
and a MinGW Windows one, the core against a real regtest node, and the
Linux and Windows packages as unsigned artifacts.

## Tests

- `cd src-tauri && cargo test`: unit tests of the core (RPC parsing, drafts,
  command-line parsing, settings, the stake and FlowMesh logic). One of them
  scans the sources and fails when a page calls an RPC the whitelist does not
  allow.
- `npm test`: the pages' pure functions (number formatting, the trading
  formats, the staking outlook).
- `B3_HIVE_B3COIND=/path/to/b3coind cargo test --test node_regtest -- --ignored --test-threads=1`:
  the core against a real regtest node, no peers: start and stop, the page
  login's limits, drafted sends submitted exactly once, wallets, passphrases,
  PSBTs, the address book, a bootstrap install and node settings.

## Not finished

- No signed releases, no automatic install of updates: the Updates page
  verifies a download against the pinned release keys and leaves running it
  to the user. The packaged builds are unsigned.
- Automation (autostake, scheduled consolidation) is left out on purpose.
- Remote access is not supported: the app targets the user's own machine.
- The Trade page follows FlowMesh V1 as it is; the demo market shows how a
  market page works while mainnet has none.
- The Receive page makes labelled addresses and payment links, but has no
  list of past requests: the node has no RPC for Qt's requested-payments
  history.
- English only.
