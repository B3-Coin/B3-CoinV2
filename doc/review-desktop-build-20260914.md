# Review-only desktop compilation follow-up

This delta follows the published draft review branch. It does not replace any
frozen Candidate04/05 artifacts or their retained evidence. It is not a public
release, deployment, mainnet qualification or repair of the historical
accessibility SIGSEGV, which remains open.

## Changes

- Fix strict C++ initialization warnings with explicit existing empty defaults.
- Request the OpenSSL feature of libevent for native Windows builds.
- Rebuild Homebrew libevent against the installed OpenSSL on ephemeral Mac
  build runners, preventing stale Cellar-header references.
- Use GNU C11 only for the OpenSSL depends package, whose C helpers require GNU
  `asm`, preserving other packages' C11 and existing hardening flags.
- Reject the historical self-reported `/B3-Coin:3.*` software after the sealed
  legacy boundary without IP bans. Retained official tags `v3.0.0.0` and
  `v3.1.2.2` establish the prefix; the latter is commit
  `bd15d5cf55c5429a086a6c33cc4a221f074dc302`. This is not authenticated software
  detection. Modern B3Hive wallets using protocol 80008 during historical
  bootstrap remain compatible. Pre-boundary history rules are unchanged.
- Automatically retrieve optional public asset display metadata during
  FlowMesh trading discovery/refreshes. The operator explicitly configures its
  public catalog; private wallet labels are never exported. Exact chain/AssetId
  precision proofs are locally verified; names remain sourced cosmetic claims.
  Details and limitations are in `asset-metadata.md`.
- Include source/run/architecture/signing identity in each desktop artifact.

No execution, fees, market configuration, membership, quorum, signing history,
signed actions, certificate formats or settlement validity change.

## Verification and build policy

The user explicitly requested skipping tests and compiling Windows and Mac
only. New mocked peer-policy and metadata-parser regression sources are
registered for future normal test builds, but are not compiled or executed in
this pass. No new native UI or end-to-end qualification is claimed.

Production syntax-only compilation succeeded on macOS arm64 for the changed
peer manager, asset catalog parser, FlowMesh client, node interfaces, wallet
metadata resolver, wallet FlowMesh RPC, initialization and trading panel. The
two strict-initializer fixes also passed with the relevant warning as an error.
Workflow YAML/packaging Python syntax, dependency flag expansion and whitespace
were inspected. These checks are not tests or complete platform builds.

Publication commits use `[skip ci]` to avoid automatically launching the broad
PR CI matrix for this explicitly compile-only request. The dedicated manual
`release-build.yml` invocation uses `desktop_build_only=true` and
`windows_build_only=false`, selecting portable Windows x86-64 and macOS arm64
and x86-64 apps, without test jobs or test binaries. No Linux artifact is built.
The tag-push-only release publication job is not eligible for manual dispatch.
Consult the actual Actions run for compilation results; skipped tests are not
passing tests.

## Remaining limits

The desktop artifacts are ordinary B3 Hive development builds, not the guarded
loopback Candidate05 package. Windows is unsigned; Mac is ad-hoc signed and not
notarized. Mac deployment target/dependency checks remain at macOS 15. Do not
ask users to disable platform protections. Platform runtime, external/WAN
operation and production safety are not established by compilation alone.

FMN2 remains authenticated plaintext TCP. Independent peer configuration and
an approved operator environment are still needed. No live wallet, real funds,
validator, configuration or journal was used for these changes.

Remote asset metadata is cached in memory and learned by trading requests,
not an Assets-only background network scanner. Missing later labels preserve
the previous sourced cache until restart. The original deposit response gap
and original accessibility crash remain distinct unresolved observations.
