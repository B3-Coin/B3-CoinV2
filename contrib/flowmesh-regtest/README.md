# B3 FlowMesh REGTEST TEST6 — 1.1.5-flowmesh-test.6

This is a temporary, valueless test network. It is NOT a mainnet upgrade or
finished FlowMesh V2. Do not import any real wallet, seed, private key or FN key.

October 11 coordinator status: HTTPS has been renewed under the same CA, but
the existing test checkpoint manager remains stopped pending a separately
reviewed maintenance-helper correction. Build artifacts are not yet a trading
readiness announcement. Wait for the coordinator's network-ready confirmation.

## Start

1. Use the coordinator's verified archive for your platform and check its
   SHA256 against the supplied checksum file. CI artifacts are not a public release.
2. Extract the whole archive. Windows: run `B3FlowMeshRegtest.exe`.
   Mac: open `B3 FlowMesh REGTEST.app`.
3. The app accepts **no extra arguments**. Do not edit your normal
   `b3coin.conf`; the approved peer, HTTPS endpoint, public CA and regtest rules
   are built into this app.
4. If prompted to create a wallet, the tester app fixes its name to **closed-test**, leaving external
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
- Connection profile: `vps-regtest-20260928-test5` (unchanged in TEST6).
- B3 peer: `88.216.63.161:19547` (regtest only).
- HTTPS: `https://88.216.63.161:19580/flowmesh/v1`.
- Public CA file: `session3-ca.pem`.
- CA file SHA256: `6e57595b0db6515ce29f2cd9cf29abbc1d691d85b8325f6ced2ae5e368879a51`.
  The application uses this CA only for the test endpoint; it does not install
  it into the operating system trust store or disable hostname/IP verification.
- Genesis: `10d0f5bb6fde880011fd56ea35dddbf9b32da2a7825f5579ac587a7904d6929a`.
- Market: `ce8374c26dcb226e9e116dd5dfdf4fba75026c3516795b5c767f4406127529cc`.
- Asset: `6fb7668277c3bcc62c60c41f6bb2e39649387c1fb6ef0898857452fdb766130f`.
- Data stays under the OS's local application-data folder:
  `B3FlowMeshClosedTest/vps-regtest-20260926-test4`.
  TEST6 reuses the existing TEST4/TEST5 wallet, outbox, journal, receipts and settings
  in place. The reviewed connection update adds a separate versioned CA and
  connection marker while preserving the previous CA and profile marker.
  The regtest consensus rules and genesis are unchanged.
  It is not the earlier Mac operator's `remote_verification` wallet.
- An existing sole wallet named `test`, created with the earlier UI, is opened
  **in place**. It is not renamed to `closed-test`. Multiple wallets, other names,
  unsafe paths or saved settings naming a different wallet are refused for
  review; nothing is removed. Fresh creation is fixed to `closed-test`, and the
  tester Create Wallet UI refuses a second wallet. Manual RPC creation of other
  wallets is unsupported; this is not a general wallet-import tool.
- Existing data with an unapproved profile, changed trust file, unexpected
  wallet or unsafe path is refused. Preserve it and report the error; do not
  delete a marker, reset the data directory, or import another wallet.
- Never copy an operator wallet into this directory. Network selection comes
  from the fixed regtest rules, **not** from using 127.0.0.1 versus 127.0.0.2.

## Scope and known limits

### Connection upgrade is not old-chain recovery

TEST6 retains TEST5's endpoint/trust repair and corrects named-wallet reopening.
It does not choose a fork,
erase an old finality anchor, reset signing history, replay orders on another
chain, or migrate balances from an abandoned regtest session. Before upgrading
an existing tester installation, compare its block 145 with the current session:
`0961807894e19a06e2df413bf45a4f39c7ee1da1d933e534a4b0039d4e5b1935`.
A different hash means a separate chain-history diagnosis is required. Preserve
the old data and report it; do not delete locks or reindex to force agreement.

The original profile marker and CA remain in place for inspection. The old
TEST4 executable can still recognize them after TEST6 exits, so open TEST6,
not the old shortcut. Both versions use the same exclusive ownership lock.
Incomplete or tampered trust files are preserved and refused, not overwritten.

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

Liquidity is not guaranteed. Deposit sweeps and withdrawals require coordinator
handling; do not promise automatic liquidity or
immediate payout. A saved/submitted action is not necessarily certified or filled.
Do not press Retry repeatedly or replace an uncertain instruction.

The renewed session window ends by 2026-10-18 00:00 UTC or the existing bounded
checkpoint/sweep budget. The renewed TLS leaf expires 2026-10-21 14:02:47 UTC.
Renewal does not resolve the stopped manager described above. Stop if trust or synchronization
fails; do not weaken verification. Services may be unavailable before that time.

## Release session checks

The immutable embedded profile's availability text records the historical
October 11 window. The coordinator authorized renewal on October 11; the
separate versioned session window records the new dates without rewriting the
profile's storage identity. Packaging refuses an expired session before producing handoff archives.
The existing embedded profile, public CA and storage identity remain unchanged;
do not delete or rewrite tester data to bypass the refusal.

[session-window.json](session-window.json) records the declared window and leaf
expiry separately, bound to the exact embedded profile bytes. Its hash binding
is an identity check, not proof of operator authorization. Any extension requires
explicit coordinator approval of the service window, operation budget and
certificate/trust configuration. Editing a date cannot renew a server certificate
or establish chain synchronization, validator quorum or trading readiness.
Packaging requires at least one hour remaining; this is a tooling handoff margin,
not a consensus or staking parameter.

For coordinators building from source:

```sh
python3 -B contrib/flowmesh-regtest/test_session_preflight.py -v
python3 -B contrib/flowmesh-regtest/session_preflight.py
python3 -B contrib/flowmesh-regtest/session_preflight.py --probe-tls
```

The last command, only after a valid declared window, performs one verified TLS
handshake to the fixed approved endpoint. It sends no HTTP, wallet or trading
request and retains CA and hostname verification. An offline window pass alone
does not verify live TLS or service readiness. Expired-window refusal occurs
before any connection. The renewed leaf was independently verified through
the unchanged pinned CA and IP-name checks; chain progress remains a separate gate.

CI runs the offline window gate before expensive platform builds. Source fixes
in the native node and Qt now trigger the guarded build workflow, not only
packaging changes. The workflow creates CI artifacts only; no automatic release
or deployment is enabled. Check the command exit status and exact successful
revision before handoff. Private staging is not a tester package; a failed copy
may leave partial output that must not be distributed.

## Private bug report

Send privately to the coordinator who invited you; no separate public bug inbox
has been approved. Include version/commit from BUILD-INFO, OS/architecture,
REGTEST label, test market, ActionId if relevant, timestamps and the exact error.
A sanitized screenshot is useful. Do not attach full configuration, wallet,
credentials, outbox/journal contents, private keys or unrelated logs.
