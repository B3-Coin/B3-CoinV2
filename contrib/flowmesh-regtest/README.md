# B3 FlowMesh REGTEST — 1.1.5-flowmesh-test.4

This is a temporary, valueless test network. It is NOT a mainnet upgrade or
finished FlowMesh V2. Do not import any real wallet, seed, private key or FN key.

## Start

1. Download the archive for your platform from this prerelease and check its
   SHA256 against the release's checksum file.
2. Extract the whole archive. Windows: run `B3FlowMeshRegtest.exe`.
   Mac: open `B3 FlowMesh REGTEST.app`.
3. The app accepts **no extra arguments**. Do not edit your normal
   `b3coin.conf`; the approved peer, HTTPS endpoint, public CA and regtest rules
   are built into this app.
4. If prompted to create a wallet, name it **closed-test**, leaving external
   signer/watch-only options off. Generate it locally; nobody shares a wallet.
5. Allow block synchronization to catch up. Open Trade -> Spot and select the
   rUSD market (or its exact AssetId if the metadata is not yet available).
6. Give only your generated **test receiving address** to the coordinator for
   test funds. Never send seeds, passwords, keys, wallet files or signing history.
   Fresh wallets are unfunded. This release does not run an automatic faucet.
7. Quit through the app menu. Reopening uses the same generated wallet and
   preserves its signed instructions and receipts.

No administrator privileges, port forwarding, VPN or validator key is needed.
Mainnet may run separately; this app has its own data/settings and no inbound
listener or administrator RPC. Opening it does not enable your mainnet FN.

If Windows security/antivirus or macOS security blocks the app, **stop and report
the exact message**. Do not disable protection, whitelist a detection, or bypass
a warning on our instruction. These binaries are unsigned (Mac ad-hoc signing
is integrity sealing, not an Apple-verified publisher identity). Notarization
and publisher signing are not claimed.

## Identity and isolation

- Branch: `flowmeshV2-dev`; see the archive's `BUILD-INFO.json` for exact commit,
  binary hashes, embedded profile digest and platform.
- Profile: `vps-regtest-20260926-test4`.
- B3 peer: `88.216.63.161:18547` (regtest only).
- HTTPS: `https://88.216.63.161:18580/flowmesh/v1`.
- CA file SHA256: `03d252bba9e5723f943415a74038d9367ffe0bfd9e6a683d3a65d43922b23d83`.
  The application uses this CA only for the test endpoint; it does not install
  it into the operating system trust store or disable hostname/IP verification.
- Genesis: `10d0f5bb6fde880011fd56ea35dddbf9b32da2a7825f5579ac587a7904d6929a`.
- Market: `0f36a1dcd1755a98ea2cbc507ad5588bf4dd462045b065641c8add29e9ad936f`.
- Asset: `ffd80f614f91e15244b3549f81d685a72c5f637da87ff979e97cdc9758134a66`.
- Data stays under the OS's local application-data folder:
  `B3FlowMeshClosedTest/vps-regtest-20260926-test4`.
  It is not the earlier Mac operator's `remote_verification` wallet.
- Existing data with a changed profile, unexpected wallet or unsafe path is
  refused, not deleted or silently migrated. Preserve it and report the error.
- Never copy an operator wallet into this directory. Network selection comes
  from the fixed regtest rules, **not** from using 127.0.0.1 versus 127.0.0.2.

## Scope and known limits

The native engine is V1-based, with the reviewed client/networking and UI
improvements. Spot includes the reverse B3 view, exact limit-level book and
sequence-indexed candles (not wall-clock TradingView candles). Market balances
and fees retain V1 rules. Futures is an informational tab only.

Four independent generated signer identities run on ONE VPS: one failure domain,
not independent hosting. FMN2 operator links remain authenticated plaintext TCP
inside the operator environment. HTTPS protects the client connection.

No full V2 BFT/PoS/shared-balance/stable-fee/futures-margin implementation,
mainnet activation, external bridge, arbitrary-load, 200 ms or WAN performance
qualification is claimed. The original accessibility SIGSEGV remains
OPEN/UNRESOLVED; no recurrence is not a repair.

The existing VPS session has no seeded two-sided book yet. Deposit sweeps and
withdrawals require coordinator handling; do not promise automatic liquidity or
immediate payout. A saved/submitted action is not necessarily certified or filled.
Do not press Retry repeatedly or replace an uncertain instruction.

Maintenance ends by 2026-10-09 00:00 UTC or the bounded checkpoint/sweep budget.
The TLS leaf expires 2026-10-10 14:02:31 UTC. Stop if trust or synchronization
fails; do not weaken verification. Services may be unavailable before that time.

## Private bug report

Send privately to the coordinator who invited you; no separate public bug inbox
has been approved. Include version/commit from BUILD-INFO, OS/architecture,
REGTEST label, test market, ActionId if relevant, timestamps and the exact error.
A sanitized screenshot is useful. Do not attach full configuration, wallet,
credentials, outbox/journal contents, private keys or unrelated logs.
