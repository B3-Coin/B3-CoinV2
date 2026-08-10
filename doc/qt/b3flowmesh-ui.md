# B3FlowMesh Qt UI implementation

This document describes the Qt/product layer added in the B3FlowMesh UI
sprint. Everything here is presentation-only: no consensus, wallet
database, RPC, networking or policy behavior was changed.

## Theme

- `src/qt/b3theme.{h,cpp}` — the single source of truth for colors,
  text roles, spacing, radii, border widths, typography, icon sizing
  and card/hover/focus/disabled styling. It is applied once at startup
  (`B3Theme::apply`) as a palette plus one stylesheet; widgets opt into
  styling with `B3Theme::markCard(widget)` and
  `B3Theme::markTextRole(widget, role)` (`h1`, `h2`, `h3`,
  `secondary`), never with scattered inline stylesheets.
- Reduced motion: `B3Theme::reducedMotion()` returns true when
  `B3_REDUCED_MOTION=1` is set or the platform is `offscreen`; an
  explicit `B3_REDUCED_MOTION=0` overrides the platform heuristic. All
  animation (currently the startup splash) must consult it.

## Application shell

- `src/qt/b3shell.{h,cpp}` — top status area over a left sidebar and a
  `QStackedWidget` content area. The existing `WalletFrame` is hosted
  verbatim as the Dashboard/Activity content; Trade, Assets, Stake and
  Settings are installed with `setTradePage` / `setAssetsPage` /
  `setStakePage` / `setSettingsPage`. The shell owns no application
  state; it re-emits navigation as `pageSelected(B3Page)`.
- `src/qt/b3navsidebar.{h,cpp}` — keyboard-focusable navigation with
  accessible names/descriptions, emitting the canonical `B3Page` enum.
- `src/qt/b3topstatus.{h,cpp}` — pure view for identity, network badge
  (mainnet quiet, TESTNET/REGTEST unmistakable), sync, connections,
  wallet and staking chips. `BitcoinGUI` drives it from the existing
  `ClientModel`/`WalletModel` slots; it fabricates nothing.
- `src/qt/b3placeholderpage.{h,cpp}` — reusable honest empty state.

## Pages

- Dashboard — `src/qt/b3dashboardpage.{h,cpp}` (wallet builds only).
  Balances from `WalletModel::balanceChanged`/`getCachedBalance`,
  recent activity through a `TransactionFilterProxy`, sync/network from
  `ClientModel` signals, privacy masking mirroring the Overview page.
  Hosted inside each `WalletView`; `gotoOverviewPage()` shows it. The
  original `OverviewPage` remains fully wired.
- Assets — `src/qt/b3assetspage.{h,cpp}` over
  `src/qt/b3assetmodel.{h,cpp}`.
- Trade — `src/qt/b3tradepage.{h,cpp}` over
  `src/qt/b3marketmodel.{h,cpp}` and `src/qt/b3chartwidget.{h,cpp}`.
- Stake — `src/qt/b3stakepage.{h,cpp}`: real lock state, real balance,
  real `Generated` reward history; everything else states "Not
  available" because no staking model is exposed to Qt.
- Activity — the existing `TransactionView` (filtering, details,
  copy/export, watch-only) framed by the shell.
- Settings — `src/qt/b3settingspage.{h,cpp}`: routes to the existing
  `OptionsDialog` tabs and mirrors the window's existing wallet
  security `QAction`s. It stores nothing.
- Startup — `src/qt/splashscreen.{h,cpp}`: deterministic QPainter mesh
  animation converging on the `NetworkStyle` mark, real init messages,
  reduced-motion static frame, timer stopped before teardown. Created
  behind `-splash` exactly as before; `-splash=0` and startup
  cancellation are unchanged.

## Model interfaces (how a real backend connects)

- Assets: implement `B3AssetSource` (`assets()`,
  `coloredAssetsAvailable()`, `flowMeshAvailable()`, signal
  `assetsChanged`) and hand it to `B3AssetsPage::setSource`.
  `B3AssetRecord` carries id, ticker, name, confirmed/pending/
  available/reserved/FlowMesh balances with per-field availability
  flags, decimals, metadata availability and status. The only shipped
  implementation is `B3NativeAssetSource` (native B3 from the real
  wallet). Fields whose backend does not exist render "Not available",
  never zero.
- Trading: implement `B3TradingBackend` (`available()`,
  `unavailableReason()`, `estimatedFee()`, signal
  `availabilityChanged`) and call `B3TradePage::setBackend`. Feed
  market data through the page's `B3CandleSeries`, `B3OrderBookModel`
  and `B3TradesModel`. The shipped `B3NullTradingBackend` reports
  unavailable, which keeps the ticket's submit action disabled and
  disconnected. The order/position/fill tabs are plain
  `QAbstractTableModel`s and can be replaced wholesale.
- Chart: `B3ChartWidget::setSeries(B3CandleSeries*)`. The series
  sanitizes input (sorted, deduplicated, malformed candles dropped) and
  supports incremental `append`. The widget clips painting to the
  visible range, supports candle/line modes, volume, crosshair,
  wheel zoom, drag pan and `resetView()`, and performs no network
  access.
- Fixed-point: `src/qt/b3fixed.{h,cpp}` — integer-only formatting and
  checked multiplication. Financial values never round-trip through
  floating point.

## Build

    cmake -B build-qt -DBUILD_GUI=ON -DBUILD_GUI_TESTS=ON
    cmake --build build-qt --target b3coin-qt test_bitcoin-qt -j 8

## Tests

    QT_QPA_PLATFORM=offscreen ./build-qt/bin/test_bitcoin-qt

Suites added by this sprint: `B3ShellTests`, `B3SplashTests`,
`B3TradeTests`, `B3DashboardTests`, `B3AssetTests`,
`B3StakeSettingsTests`, `B3HardeningTests` (registered in
`src/qt/test/test_main.cpp`). Deterministic sample data appears only
inside tests, never in production views.

## Known limitations

- `RPCNestedTests` aborts with an unhandled `UniValue` exception. This
  failure exists at the base of the sprint (before any UI change), is
  unrelated to the Qt layer, and also prevents the suites registered
  after it (`WalletTests`, `AddressBookTests`) from running in the same
  process. The B3 suites are registered ahead of it so they execute.
- No approved B3 logo artwork exists in the repository; all icon assets
  are still the Bitcoin mark, so application/window/tray/splash icons
  keep the existing artwork until approved art lands in the Qt
  resources (`src/qt/res/icons`, `share/pixmaps`).
- Windows (`src/qt/res/bitcoin-qt-res.rc`, NSIS bitmaps) and macOS
  (`b3coin.icns` copy in `cmake/module/Maintenance.cmake`) resources
  were statically inspected only; no Windows or macOS packaging build
  was run in this sprint.
- Only macOS (arm64, Qt 6.11) was built and tested here.
