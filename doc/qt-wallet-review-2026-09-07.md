# Qt wallet review — 7 September 2026

## Scope and publication

At the operator's request, GitHub `master` was first fast-forwarded from
`bcd3408` to the existing tested `release/1.1.4` commit `f48a81b`. No tag or
release was created. GitHub reported that the authenticated account bypassed
the PR-required branch rule; no branch protection settings were changed.

The following UI work and separately approved unlock-security fix are being
prepared on `release/1.1.4`. Verification results and final commit identifiers
are recorded below when complete. The live wallet executable is not overwritten
by the preview build.

## UI findings and changes

| Finding | Change |
|---|---|
| Several inherited macOS buttons intentionally discarded their icons. | Keep button icons enabled under the B3 theme on all supported platforms. |
| Windows retained some black icons against dark surfaces; custom/scalable icons with no enumerated sizes could disappear. | Tint icon engines at render time, preserving modes, on/off states and display scaling. |
| Native checkbox, radio, tool-button and arrow controls could remain light or indistinct. | Explicit themed surfaces and checked/partial/disabled glyphs; use built-in XPM resources to avoid an unavailable static Qt SVG plugin. |
| The console's font-smaller, font-larger and clear controls remained white; the initial tool-button fix matched only `QDialog`, while `RPCConsole` is a `QWidget`. | Extend the same state-specific surfaces to `#RPCConsole QToolButton`, including embedded consoles, without changing the custom sidebar. Test the actual console form and all three icons. |
| Send/Receive could display an Overview heading and had no dedicated sidebar destination. | Explicit destinations and current-wallet page-change routing; retain the existing send/receive dialogs and confirmation logic. |
| Ordinary wallet navigation was hidden with the old toolbar. | Restore a View menu using the existing actions and shortcuts. |
| Sidebar Activity did not mirror privacy restrictions. | Mirror the original action's availability; preserve access to the no-wallet Overview/Welcome screen. |
| Sidebar always claimed modern features were inactive. | Remove the unsupported fixed status claim. |
| Overview's staking card was permanently hidden. | Show the existing controller's staking/finality state, including another-wallet, loading and failure states, without another polling loop or invented rewards. |
| Settings action buttons did not mirror icons/visibility and accumulated on replacement. | Mirror the underlying action and safely replace/remove its presentation. |
| Modal Settings fallback and visible-page replacement could leave inconsistent navigation. | Keep the selected destination, title and visible content consistent. |

## Features that are still genuinely unfinished

- **Trading:** the only wired trading backend is `B3NullTradingBackend`.
  An order book/chart display is not an implemented trading service. Order
  submission stays disabled; this review does not add an exchange backend.
- **Non-native assets and bridge actions:** the Assets page displays owned
  outputs, but non-native Send and FlowMesh deposit/withdraw do not have a
  completed Qt submission-and-confirmation path. They stay disabled. Their
  console operations are financial actions, not harmless diagnostics.
- **Finality:** better status and relay handling do not create another
  validator's signature, restore a deleted signer journal, or fix a missing
  quorum. No consensus or signer-recovery rule changes are part of this pass.

## Security findings

### Unlock failure can leave spending enabled — concrete, conditional issue

`CWallet::Unlock` installs the decrypted master key before descriptor-cache
upgrades complete. A subsequent exception can leave an initially locked wallet
unlocked while ordinary Qt displays an unlock failure. In RPC `walletpassphrase`,
such a failure can also occur before the promised relock is scheduled; further
post-unlock preparation must be included in the same failure-state protection.

This requires a correct passphrase and a cache/storage or related execution
failure. Theft additionally requires local access or an authenticated RPC actor
able to use the unexpectedly unlocked wallet. This is not an unauthenticated
remote spending bypass. The operator approved a separate focused fix to restore
the previous authorization state on failure and test Qt/core/RPC boundaries.

### Release authority and artifact authenticity — operational risk

The release workflow currently describes unsigned builds, publishes checksums
with the artifacts, and uses ad-hoc macOS signing. Checksums detect accidental
corruption but do not authenticate an artifact against an attacker controlling
both the executable and its checksum. The observed PR-rule bypass is an
additional reason to review who can update source, tags and releases.

Recommended follow-up: reviewed release permissions, independent artifact
signatures and platform signing/notarization. No repository permission changes,
signing credentials or release publication were authorized in this review.

### Boundaries checked and remaining operator risks

- Send confirmation escapes recipient labels and validates addresses before
  constructing transactions; no label-based confirmation injection was found
  in the inspected path. Clipboard malware can still substitute a different
  *valid* address, so verify the full recipient address in the final prompt.
- Staking retains dedicated validator/BLS keys in memory, not the wallet
  master key. Those keys remain sensitive even while spending is locked.
  The staking-only equivalent is a brief normal unlock followed by relocking,
  not a separate restricted permission for concurrent authenticated RPC calls.
- Inspected locked-wallet paths reject validator-secret retrieval and ordinary
  transaction/PSBT signing. This is not an exhaustive audit of every export.
- RPC is disabled by default in Qt; when enabled, normal defaults use loopback
  and authentication. The running preview was started with loopback-only RPC.
  Exposing unrestricted RPC or sharing its credentials can authorize spending.
- Console asset-send and bridge-withdraw operations can sign and broadcast
  after unlocking without a second GUI confirmation. Do not paste unreviewed
  purported support commands into an unlocked wallet console.
- The built-in updater checks signed manifests, host restrictions and artifact
  hashes and does not automatically install. No authentication bypass was
  found in the bounded source review.

## Verification and limitations

### Combined release follow-up — 8 September

The approved unlock-failure fix is a separate focused commit, `955a93b`, and
its six core/RPC tests passed. A confirmed shutdown crash was fixed by
detaching wallet-backed views before controller deletion and guarding the
stake page's model lifetime. Real temporary-model destruction tests pass.

The top-right wallet name was passive text; the real selector was owned by a
hidden toolbar. The actual combo now lives in the top bar and routes wallet,
asset, stake and console context together. A real two-wallet keyboard-switch
test covers selection and closing/removing wallets without accessing user data.

A second cross-platform icon issue was reproduced: native disabled rendering
could halve source opacity before our muted tint was applied. The production
icon engine now uses undimmed, same-state artwork and applies the disabled
appearance once. All 15 theme tests pass under both `minimal` and `offscreen`.
All four focused GUI targets passed together in 3.72 seconds; the full GUI
test target also compiled. The complete application integration suite and a
native Windows run are not claimed by these checks.

The release also includes separately reviewed explicit operator recovery;
that is outside the original GUI-only review and is documented in
[the recovery guide](finality-recovery-rpc.md). No live recovery was activated.

### Console screenshot follow-up — 8 September

The user reproduced three white icon-button surfaces in the running older
`bin-staking-preview` executable. The pending dialog-only selector also missed
the console's actual `QWidget` root. That scope gap is now corrected with
`#RPCConsole QToolButton` rules, without broadening the sidebar's style.

The actual console form now has a six-row rendering regression: standalone and
embedded widgets, with each of the macOS/Windows/other icon policies. All three
22px icons and their normal, pressed and disabled surfaces are checked. The
full focused theme suite passed **14 test rows, 0 failures, 148 ms** using Qt
6.11.1's offscreen backend on macOS. This does not run a native Windows backend.
A pre-existing scalable-icon test initially failed because its synthetic icon
engine painted into an uninitialized pixmap; the test engine now explicitly
initializes transparency and verifies its own source before testing tinting.
No production icon-engine change was needed for that failure.

`cmake --build build --target b3coin-qt -j4` completed successfully. The new
GUI-preview executable is `build/bin-ui-preview/b3coin-qt`; the running older
wallet was not stopped or replaced. An isolated console PNG was visually
inspected and shows dark surfaces with legible A−, A+ and clear icons. This
build is from the ordinary `release/1.1.4` worktree, not the separate
operator-recovery branch, and is not a published release.

### Broader review

Verification is in progress. The focused UI runner uses temporary settings,
regtest parameters, synthetic display data and no live wallet, networking or
mining. Visual frames are rendered from the actual Qt widgets, including
Overview, Send, Receive, Activity, Assets, Trade, Stake, Settings and message
signing, with narrow-window checks.

Computer Use could not attach by name to the running standalone executable.
The installed bundle list points to older, non-running copies, which were not
launched as a substitute. Offscreen renders therefore do not certify the live
macOS window, native Windows behavior, packaging, or every interaction path.
No funds were moved, no signer state was reset, and no theft exploitation was
attempted against the live wallet. This is a bounded review, not a certification
that the wallet or bridge is free of security vulnerabilities.
