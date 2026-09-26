# B3 FlowMesh REGTEST 1.1.5-flowmesh-test.4

Temporary tester prerelease — NOT a production wallet upgrade, finished V2 or
mainnet activation. Separate regtest-only GUI; no mainnet executable/daemon/CLI,
shared wallet, seed, operator key or administrator RPC is included.

- Fixed VPS B3/HTTPS profile and dedicated public CA; normal certificate and IP
  verification remain enabled.
- Separate generated-wallet storage and settings, no runtime path/network
  overrides; fail-closed identity/ownership checks preserve old data.
- Spot orderbook/depth and sequence-indexed candles. Futures tab is informational.
- Includes pending-pin catch-up repair and source regressions; see committed
  source. Not a community-wallet recovery claim.
- Windows x86-64 portable ZIP; macOS arm64 and Intel bundled apps.
  Exact dependency minimums and binary hashes are verified during packaging.

Read [tester instructions](../contrib/flowmesh-regtest/README.md). Existing
mainnet wallets and validators must not be changed for this test.

Unsigned / macOS ad-hoc signed, not publisher-signed or notarized. Do not disable
OS/antivirus protections. Original accessibility SIGSEGV remains OPEN.
Four validators share one VPS; no decentralized/WAN/latency qualification.
No native futures, full V2, new stable-asset fees or live migration.
Fresh clients need coordinator-issued valueless test funds; liquidity and
operator-serviced deposit/withdrawal steps are not an automated faucet.
Session maintenance ends no later than 2026-10-09; HTTPS leaf expires October 10.
