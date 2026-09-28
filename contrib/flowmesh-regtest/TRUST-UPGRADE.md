# TEST5 connection-trust repair

TEST4 points at retired endpoints and cannot authenticate the replacement test
server. Adding its new URL through `flowmeshclientconnect` does not import a CA:
new URLs use default trust; configured URLs retain their explicit trust.

TEST5 embeds the approved session3 endpoints and public CA, with hostname/IP
verification unchanged. It accepts no runtime trust-import shortcut. It does
not install a system-wide certificate or enable a validator engine.

## Same-wallet upgrade

The new connection identity is `vps-regtest-20260928-test5`; the intentionally
stable storage directory is `vps-regtest-20260926-test4`. The exact approved
public profile alone gets this internal alias. Arbitrary JSON aliases are
rejected. A changed profile ID alone would select an empty wallet directory;
changing the old profile bytes alone would instead fail its identity check.

Under the existing exclusive launcher lock, the update recognizes either its
fresh-install identity or the exact retained TEST4 identity. For legacy data it
also verifies the original public CA on every open. It preserves both files
in place, validates wallet/settings ownership, and adds only:

- `test-endpoint-ca-test5.pem`
- `connection-test5.identity`

Complete identical writes are reusable after interruption. Partial, different,
unsafe or redirected files are refused and preserved, not overwritten. Special
files cannot block the guard by substituting a FIFO. No wallet, outbox, chain,
signed action, receipt, sequence, journal or finality anchor is rewritten.

Old executables still recognize the preserved original files after TEST5
exits. Launch the new app, not an old shortcut. Both share the ownership lock.

## Focused evidence

Before the policy repair, the current profile was refused. With just the new
allowlist, preservation/exclusive-ownership tests failed because a different
storage root was selected. The corrected implementation passed 160 native
macOS policy checks and 22 Python packaging tests. Native Mac GUI compilation
also passed. These are newly executed local checks, not Windows/CI claims.

Coverage includes fresh and existing-storage reopening, preserved synthetic
wallet/action/chain bytes, exact scoped CA arguments, concurrent-launch refusal,
complete and partial interruption layouts, unknown identities, permissions,
symlinks/hardlinks, FIFO rejection and invalid settings. The synthetic byte
sentinels are not an attended native wallet test or power-loss qualification.

A bounded HTTPS probe against the approved replacement server verified with
the new CA and rejected the old CA. The probe used GET (HTTP 405 after successful
TLS); it was not a trade or proof of a complete client session.

Reproduce the focused checks in a configured desktop build:

```sh
cmake --build <build> --target test_b3_flowmeshclosed-policy test_b3_flowmeshclosed-gui --parallel 4
<build>/bin/test_b3_flowmeshclosed-policy
python3 -B contrib/flowmesh-regtest/test_package.py -v
```

The normal tester-build workflow additionally runs the policy executable on
native Windows and both Mac architectures when tests are enabled. Refer to the
actual run for the tested revision/results; a workflow file is not a CI pass.

### Windows qualification follow-up

Native Windows at `5dae4b10016ddea7bd281571e08364545b02757f` reached the storage
tests after correcting the ordinary CI account's temporary directory. It
reported 150 passes and two failures: a native-versus-Qt path separator
comparison in the test, and rejection of the valid persisted JSON
`{"wallet":["closed-test"]}` during TEST4-to-TEST5 reopening.

The successor normalizes separators only in that assertion, and copy-initializes
the parsed `QJsonArray` explicitly to avoid the initializer-list wrapping path.
The accepted settings remain exactly an empty wallet array or one `closed-test`
entry. Eight additional raw-JSON tests reject nested arrays, extra/duplicate or
wrong wallets, and a string instead of an array. They verify unchanged settings
bytes and no new trust files when an input is refused.

The successor's local Mac policy suite passed 168 tests. The compiler-specific
Windows result must be established by the successor's actual native CI run,
not inferred from the Mac result. No wallet format, history or signing record
is changed by this correction.

## Not old-chain recovery

If a tester remains on the abandoned chain with conflicting checkpoint history,
this repair does not make that history compatible. Do not clear locks, discard
journals or silently substitute balances. The README supplies the current
block145 identity for a per-installation comparison. Old-chain recovery or an
approved fresh disposable setup is separate from this TLS repair.

The historical accessibility crash remains open. This is a regtest-only V1-based
client package, not full FlowMesh V2, futures, stable-asset fees, a production
upgrade, or a bridge/consensus recovery release.
