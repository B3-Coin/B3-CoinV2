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
  `wallet.rs`, `receive.rs`, `psbt.rs`, `settings.rs`, `folder.rs`, `cli.rs`,
  `update.rs` and `bootstrap.rs` each cover one page or one job. `fm2/` is
  the FlowMesh V2 client and `trade.rs` the Trade page's commands (below).
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

## Trading on FlowMesh V2

The Trade page is a client of FlowMesh V2 (`src/flowmesh2`), the trading
layer that replaces FlowMesh V1. FlowMesh V2 runs on regtest networks only,
and the page says so everywhere: nothing on it is a mainnet market. It needs
no running B3 node and no wallet.

**Two views of one page.** The switch in the page's header chooses how much
the page shows, and B3 Hive remembers it. Both views use the same account,
the same market and the same approval window.

- *Simple* is one card: buy or sell, what you pay, what you receive. The
  page works out an immediate order from the order book and gives it a price
  limit, so a buy can never cost more than the card says and a sale never
  goes below the price shown. Either box can be typed in; the other follows.
  The button names the trade and its money ("Buy 0.004 B3 for 404 rUSD") and
  says under it that approval comes first. Three lines above it are the
  receipt: what you pay or receive, the price, the fee. A number is exact
  when the order's own limit makes it so; when the order reaches past the
  best price it reads "about", with the bound next to it ("never more than",
  "at least"). The price line also names what the other direction gets at
  that moment ("if you were selling: 98 000"), so the gap between buying
  and selling is in sight before a trade. A budget that does not come out
  even says where the rest goes, or why it is left when the order reaches
  several prices. Whatever the card offers in place of the typed amount is
  worded as a change of the amount ("Change to 0.005 B3"), never as a
  second button that buys, and says first what it would cost. A warning
  appears only when the order reaches prices five percent or more from the
  best one: it names the best price, how much there is at it, the furthest
  price and the difference in money, says that the sum above has that in
  already, and offers the amount that gets the best price only, with its
  cost. Below that, one sentence says what the reach costs. The order
  arrives in about a second; if prices move before then it trades what
  still fits, perhaps nothing, and nothing is left waiting. The card says
  so above the button. The result is one sentence built from the
  confirmed fills: what was bought or sold, for how much, and what was left
  out and why. A request that was not approved leaves a note above the
  button: nothing happened and no money moved.
- *Pro* is for an order at a price of the user's own. At the default window
  size the order book, the order form and the open orders are on one screen;
  the price chart, the depth chart and the list of the market's trades share
  one card below them, and a wide window puts that card between the book
  and the form. The book lists price, amount and the amounts summed
  from the best price, with a dot at a price where an order of the account
  waits; it shows five to ten prices a side, by the room the window gives.
  The form takes a limit order or an immediate one against the book.
  What a limit order does with the part that cannot trade at once is one of
  three choices in sight: it waits (good till cancelled), it is dropped
  (immediate or cancel), or the whole order may only wait (post only). A
  line under them says what the order would do against the book as it
  stands ("0.024 B3 trades at once, at 101 000 rUSD, for 2 424 rUSD, and
  the other 0.006 B3 waits"), then what its own price guarantees: the most
  it can cost or the least all of it brings, and the fee. An order that
  would be refused as the book stands, a post-only order that could trade,
  is not offered: the button says so and the line names the price it
  needs. An immediate order says what is sent in its place (a limit order
  at the furthest price it needs, the rest dropped), with its average
  price, the estimate, the bound and the fee. While a request from the
  form is under way its steps stand where the summary was, above the
  button; its result stays there until the form is changed. The open
  orders are a table (placed, side, price, amount, filled) with a cancel
  button on each row. A cancellation names what still waits of the order,
  and its result says what it gave back, from the confirmed change of the
  balances.

The wording of both views and of the approval window is a set of pure
functions (`web/src/fm2words.ts`) with their own tests: what a form's
button says and why it is off, what an order would do right now, the
question the approval window asks, and the sentence for every way a
request can end.

**Units.** Amounts are in the asset's own unit and prices are for one whole
unit of the base asset (`101 000 rUSD per B3`), on the page, in the approval
window and in the results; one set of functions formats all three
(`web/src/fm2fmt.ts`). The engine's lots and ticks are called steps on the
page ("B3 trades in steps of 0.001 B3") and appear as exact integers only
under Details in the approval window and in a history row. The genesis
document names decimals for B3 only, so other assets are shown as whole
numbers of their smallest unit.

**Connecting.** The page asks once for the network's folder, which holds the
two files an operator hands out: `genesis.json` (`fm2-genesis/1`: seats,
assets, markets) and `pins.json` (the SHA-256 of each seat's certificate
key, as `fm2-cli --pins` takes it). The two files can also be chosen one by
one. Both are trust inputs; nothing is taken from a seat. Seats must be on
this computer (loopback addresses). Each connection is TLS 1.3 and trusts
exactly its seat's pinned key. B3 Hive remembers the two paths and connects
by itself the next time the page opens. FlowMesh V2 has no public network,
so most people have no such folder: the card says that in the open, and
that nothing is wrong on their side.

**Getting started.** Until the account holds funds, the page shows the steps
that are left in place of the trading form: create the account (one click),
then get test funds. On a test network funds come from whoever runs it, so
that step shows the account number to hand over, says that the number is
safe to share, and names the assets the network has; the page notices the
deposit by itself and opens the form.

**The account.** B3 Hive creates one key per network and keeps it, with the
outbox, in its config folder under `fm2/<network>/`. The key file is plain
hex, as `fm2-cli` writes it, and is not encrypted: this is a test network.
Never share a key between B3 Hive and `fm2-cli`; each keeps its own record
of the nonces it signed.

**What is checked.** The core checks every answer against the genesis
document before the page sees it (`fm2/verify.rs`, the rules of
`src/flowmesh2/cli/trust.cpp`):

- a slot counts only with a certificate signed by a quorum of the genesis
  seats, over a header of this network and these rules;
- balances are shown only from an account leaf that proves into such a
  slot's state;
- the result of an order, a cancellation or a transfer is shown only once
  its outcome proves into such a slot, and each fill is recomputed;
- "expired" is shown only with the proof that no slot can include the
  action any more.

The page calls this *confirmed*. The order book and the trade list come from
one seat's live feed, which carries no proof; the page calls that the *live
view* and treats everything read from it as an estimate. That is why every
order, also the one the Simple view builds, carries a price limit of its
own. FlowMesh V2 serves no price history, so the chart and the trade list
start when the page connects. The page says "validator" for a seat.

**Sending.** An order, a cancellation, cancel-all and a transfer are drafted
by the core and shown in the approval window; nothing is signed before
approval, and one request waits there at a time. The window asks one
question whose title carries the bound ("Buy 0.004 B3 for at most 404
rUSD?"), says in a few sentences what approving does and whether it can be
undone, and states only what the order guarantees, never an estimate. It
knows the order and not the book, so the page may have said "about 2 933"
where the window says "at least 2 879": amounts read "up to", a sale's
floor reads "for all of it", and each line says which way the true amount
can differ, so that the two do not read as two prices. An order that waits
is approved with "Place the order", since it promises no trade. The other
button is named by what it leaves as it is ("Don't buy", "Keep the
order"), and the page uses the same name. Exact lots and ticks, the
account and the validity window are under Details. While a request waits,
the page says so, with a button that brings the window back and one that
takes the request back (`fm2_withdraw`, which can only remove a FlowMesh
request that is still undecided); every other form says that another
request waits for approval. An approved action is signed once, written to the
outbox on disk, and then posted to every seat.
After a crash or a lost connection the same bytes are posted again; a second
signature is never made. The History list is the outbox: each row is a
sentence about what happened, and opens to the confirmed fills behind it
(amount, price, value and fee of each), the slot, the exact size and the
action's id. A row still waiting shows what the seats last said.

**When things go wrong.** The page says what it is waiting for instead of
offering a button that cannot work: no seller or buyer on the other side,
an amount below one step or beyond the book or the balance (with the amount
that would work, one click away), live prices not arriving, or too few
validators answering to confirm anything.

**Open orders.** FlowMesh V2 has no query for an account's orders. The page
rebuilds them from the account's certified results and the fills a seat
reported since, and checks the list against the certified account (its count
of open orders and its reserved balances). When they disagree, for example
after a fill while B3 Hive was closed, the page says so.

**Not offered.** Withdrawals (the regtest rules refuse them) and decimals
for assets other than B3 (the genesis document names none). The engine has
no market order; the page's immediate order is an immediate-or-cancel limit
order priced from the book. It has no stop orders and no way to change an
order in place, and it serves no statistics of a day's trading (change,
high, low, volume), so the page shows none of these.

To try it, start a regtest cluster with the node tools (`fm2-keygen`,
`fm2-devchain`, `fm2d`; see `src/flowmesh2/README.md`), put its
`genesis.json` and a `pins.json` in one folder, choose that folder on the
Trade page, create the account, and credit it with
`fm2-devchain ... deposit <account> <asset> <atoms>`.

The signing and certificate code is the node's own: `build.rs` compiles
`src/secp256k1` and `src/blst` from this repository into the core.

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
  command-line parsing, settings, the stake logic, and the FlowMesh V2
  client: codecs, proof checks against replies captured from a real cluster,
  the outbox). One of them scans the sources and fails when a page calls an
  RPC the whitelist does not allow.
- `npm test`: the pages' pure functions (number formatting, the trading
  formats, the Trade page's and the approval window's wording, the staking
  outlook).
- `HIVE_FM2_CLUSTER=DIR cargo test --test fm2_live -- --ignored --nocapture`:
  the FlowMesh V2 client against a running `fm2d` cluster whose
  `genesis.json` and `pins.json` are in `DIR`. It creates an account, waits
  for a deposit to it, then buys, rests and cancels an order, transfers,
  proves an unsent action expired and reopens its outbox.
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
- The Trade page trades on FlowMesh V2 regtest networks only, with seats on
  this computer. FlowMesh V1 is retired and the page no longer speaks it.
  The trading key is not encrypted.
- The Receive page makes labelled addresses and payment links, but has no
  list of past requests: the node has no RPC for Qt's requested-payments
  history.
- English only.

## Open product questions

These are not decided here. Each needs a ruling from the project owners.

- **Supported platforms**: which operating systems and versions get
  packages, and which of them are tested before a release.
- **Front-end framework and dependency policy**: whether the pages stay
  plain TypeScript, and which new Rust crates or npm packages may be added.
  The FlowMesh V2 client builds the repository's own secp256k1 and blst
  instead of adding crates for them.
- **The trading key**: FlowMesh V2 accounts are plain keys with no link to
  a B3 wallet. Before any network of value, the key needs encryption and a
  backup story, or a derivation from the wallet.
- **Trading without a window per action**: every order and every
  cancellation is approved in the approval window. Frequent traders will
  ask for a session in which orders up to a limit are signed without it.
  That changes what the approval window guarantees, so it is a decision
  about the signing model, not a page setting.
- **Order types the engine does not have**: stop orders and changing an
  order in place would have to be built in the client (watching prices,
  cancelling and placing again) or in FlowMesh. Neither is done.
- **First translation languages**: the app is English only.
- **Remote access**: whether B3 Hive may reach a node or a FlowMesh API on
  another machine, and how that connection is authenticated.
- **Plugins**: whether third parties may extend the app, and how a plugin
  would be reviewed and isolated.
