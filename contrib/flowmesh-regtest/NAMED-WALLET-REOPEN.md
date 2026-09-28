# TEST6 — preserve the earlier UI's `test` wallet

Baseline: `0a0a83906b850079ba65898b619c0a1391e092eb` (TEST5).
The earlier Create Wallet dialog accepted arbitrary names, whereas the tester
launcher admitted only `closed-test` and always selected that name. A user who
created `test` could use it, then be refused on reopening. This is separate from
the HTTPS trust repair, chain-history conflicts and the open accessibility crash.

## Minimal admission correction

For the already-approved public connection profile only, select the sole
existing private real directory named `test` or `closed-test`. Fresh installations
still select `closed-test`. Multiple/unknown/hidden entries, unsafe links,
ownership or type are refused. Every saved-settings variant must be empty or
name exactly the selected wallet, not another wallet/path. The resulting node
arguments clear lower-priority wallet lists and select exactly that in-place
wallet. No wallet, settings, action, outbox, history or finality bytes are moved,
rewritten or deleted by this admission correction.

TEST6 deliberately keeps the exact TEST5 connection profile and trust files,
and the original TEST4 storage directory. The package/app version changes;
there is no new chain, certificate, wallet or profile migration.

## Prevent a new UI mismatch

Only the tester build fixes new wallet creation to `closed-test`, excludes
external signer name substitution and checks the dedicated wallet directory
before opening Create Wallet and again in the serialized creation worker.
It refuses a second wallet instead of creating an ambiguous sibling of `test`.
Ordinary builds retain editable names and their existing signer behavior.
This is UI prevention, not a redesigned RPC restriction or import policy.

## Reproducible focused checks

```sh
cmake --build <build> --parallel 4 --target test_b3_flowmeshclosed-policy test_b3_createwalletdialog-qt b3coind b3coin-cli
B3_TEST_CORE_DAEMON="$(pwd)/<build>/bin/b3coind" B3_TEST_CORE_CLI="$(pwd)/<build>/bin/b3coin-cli" <build>/bin/test_b3_flowmeshclosed-policy
QT_QPA_PLATFORM=minimal <build>/bin/test_b3_createwalletdialog-qt
python3 -B contrib/flowmesh-regtest/test_package.py -v
```

Use absolute paths for the two generated-wallet test binaries. If omitted,
the actual core-wallet test explicitly skips: policy-only success is not a
native wallet-reopening claim. The fixture creates only a temporary generated
wallet, has no funds or P2P connections, disables validator mode, and uses
loopback cookie-authenticated RPC. It checks clean child exit and Shutdown done,
unchanged wallet/settings across admission, then ownership of the same receiving
address after same-directory reopening. It does not operate a tester's wallet.

On the unmodified baseline policy, the new focused cases recorded 51 passes
and 20 failures, including the exact unexpected-entry refusal for `test`.
The corrected initial policy run passed all 238 cases. With the real generated
core-wallet roundtrip enabled, the full local policy suite passed 239 cases,
with no failures or skips. Both daemon children exited 0 with Shutdown done;
the original receiving address remained owned after reopening. The separate
dialog suite passed 12 cases, and packaging/profile checks passed 22 tests.
Native Windows/Intel results must come from the actual successor CI run, not
be inferred from the local Apple Silicon checks.

Synthetic sentinels cover saved unknown/certified instructions and chain/history
preservation. The core fixture covers real wallet loading, not an attended
Create Wallet→Quit interaction, power-loss recovery, trading or old-chain repair.
Keep those qualification boundaries separate. Never rename or delete a user's
wallet directory to make the launcher pass.
