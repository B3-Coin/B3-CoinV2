# Bridge V2 regtest integration milestone

The Ethereum bridge V2 regtest implementation is integrated with the ongoing
`flowmeshV2-dev` history. This is a development milestone, not a production
bridge, a complete FlowMesh V2 release or activation approval.

## Preserved revisions

- Previous local V2 head: `30825f9305afaf63d80468d587ace9fe0b447d76`.
- Completed incoming work: `965e23cc0830001ca33c9048063b81d5383d776a`.
- Common baseline: `b4b1de423a4be3efe852deb3ed851d0bfce156fc`.
- History-preserving integration: `0942e706f50e7ccc1d2098ab2653a1025ddb7cba`.
- Integration merge tree: `2f78f1f13a854a0cf133025b90d0698ab191947e`.

All 40 completed incoming commits remain reachable. The integration needed no
source conflict resolution. Its `src`, `contracts`, `contrib`, `test`, `ci`
and workflow trees equal the incoming revision; the earlier devlog history is
also retained. Neither the newer uncommitted key and transport work nor the
separate unfinished native trading prototype is part of this milestone.

## Implemented regtest scope

The slice includes a local synthetic Ethereum beacon harness, deposit receipt
proofs, a separate fresh finality lineage and journal namespace, a V2 verifier
and vault, exact bootstrap domain binding, observation-based mint and burn
gates, conflict closure, and migration into an upfront-pinned successor vault.

Deposits mint only against the connected Ethereum light-client and receipt
proof path, not a bridge signer assertion. Withdrawal leaves bind the exact
asset, destination chain, vault, token, recipient, amount and withdrawal id.
Failed token payments revert reserve and paid-marker changes. Migration first
accounts for retired-vault reserve in transit; successor reserve is credited
only against the proven absorption state. Old claim identities remain separate.

The node activation knobs reject non-regtest chains. The deployment script
rejects chains other than local chain id 31337. Tests use disposable chains,
mock tokens and generated test keys. No live node, wallet, signing key,
production contract, network configuration or activation height was changed.

## Qualification

Qualification uses a clean detached integration checkout, not an actively
edited developer build. Native configuration: macOS arm64, AppleClang 17,
`RelWithDebInfo`, wallet enabled, GUI disabled, bridge tools enabled. The
daemon identifies itself as `v1.1.5-flowmesh-test.6-0942e706f50e`.
Python is 3.14.6; Foundry is 1.7.1 and Solidity is the repository-pinned 0.8.35.

- Contract suite: **310 passed**, zero failures or skips.
- Bridge helper suite: **158 passed**.
- Existing isolated model runner: **446 passed**: 113 accounting,
  142 agreement, 69 storage, 20 coordinator, 64 fastpath research,
  30 native-observer checks and 8 runner checks.
- Targeted native bridge/finality suites: **183 cases passed**, 28,708
  assertions passed. The other 2,122 native cases were not run by this selection.
- Six disposable-chain functional drills passed, each with child exit status 0:

| Drill | Verified path | Random seed / port seed |
|---|---|---|
| RT0 | Toolchain, local Anvil and clock/finality setup | 4103 / 3103 |
| RT2 | Synthetic light client, receipt proofs, relayer, pin refusal and reindex | 4106 / 3106 |
| RT4 | K-of-N bootstrap, complete burn gate, stale/fresh observations and exact-once payout | 4105 / 3105 |
| RT4a | Start-certificate bootstrap, deposit/mint/burn/payout, restart and reindex | 4101 / 3101 |
| RT4c | Conflict closure, retained accepted-root payout and restart/reindex | 4104 / 3104 |
| RT4d | Successor-vault migration, reserve-in-transit accounting and restart/reindex | 4102 / 3102 |

RT3 and the standalone full fresh-lineage functional drill were not rerun.
An earlier broad native `bridge_*,finality_*` campaign was deliberately
interrupted with exit status 130 while still producing legacy scrypt fixtures;
it is not counted as a pass. The narrower native selection and six functional
drills above completed. Initial loopback-permission failures were retained,
then rerun with approved disposable-localhost access in separate directories.

The final publication report/counterexample commit changes documentation only.
The qualified native source remains the integration revision above; source
equivalence, not a rebuilt daemon claiming the later documentation hash,
establishes its relationship to the published head.

These results do not prove WAN latency, physical power-loss safety or full
native V2 integration. The contract suite includes the preserved V1 contracts.

From the repository root, the relevant commands are:

```sh
cmake -S . -B build-bridge -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_GUI=OFF -DBUILD_TESTS=ON -DBUILD_BENCH=OFF -DB3_BRIDGE_TOOLS=ON
cmake --build build-bridge -j8 --target test_bitcoin b3coind b3coin-cli \
  b3-bridge-ethcheck b3-bridge-regtest-beacon b3-bridge-regtest-certify
(cd contracts && forge test --offline --summary)
python3.14 -B -m unittest discover -s contrib/b3bridge -p 'test*.py'
python3.14 -B ci/run_flowmesh_models.py
build-bridge/bin/test_bitcoin \
  --run_test=bridge_burn_gate_v2_tests,bridge_state_observation_tests,bridge_state_proof_tests,bridge_v2_tests,bridge_regtest_beacon_tests,bridge_regtest_certify_tests,bridge_regtest_pins_tests,bridge_state_tests,bridge_exec_chain_tests,bridge_proof_tests,finality_fresh_params_tests,finality_v2_tests,finality_signer_store_tests \
  --log_level=error --report_level=detailed
```

The functional runner's preflight currently refuses execution because several
pre-existing FlowMesh scripts are absent from its registration lists. That
check was not disabled or weakened. The bridge drills can instead be invoked
individually with the generated build configuration, for example:

```sh
python3.14 -B build-bridge/test/functional/feature_b3_bridge_rt4a_round_trip.py \
  --configfile=build-bridge/test/config.ini --randomseed=4101 --portseed=3101
```

The other drill scripts are `feature_b3_bridge_rt0_toolchain.py`,
`feature_b3_bridge_rt2_light_client.py`, `feature_b3_bridge_rt4_kofn_gate.py`,
`feature_b3_bridge_rt4c_conflict.py` and `feature_b3_bridge_rt4d_migration.py`
in that same functional-test directory. Use the corresponding seeds above.

Use disposable test directories and distinct port seeds when running drills
concurrently. Loopback socket permissions, Anvil, the pinned compiler and
Foundry are required; skipped drills are not a qualification pass.

## Review finding requiring recovery work

**P2 liveness defect: a fresh validator missing two certified epochs cannot
resume even when its original locked checkpoint remains an ancestor.**

In the frozen [signer](../../src/node/finality_signature.cpp),
`EnsureFreshPersistentSafety` returns with the old lock unchanged when that
checkpoint is still canonical. `SignFresh` skips already-finalized heights.
The [journal store](../../src/node/finality_signer_store.cpp)
`CommitSignedCheckpoint` permits only the locked epoch or its exact committed
successor. An epoch-2 vote against an epoch-0 journal therefore refuses with
`checkpoint does not follow the durable validator-set lineage`. The signer
then retains that refusal locally; reopening the unchanged journal repeats it.

The counterexample uses four validators with weights 15, 1, 10 and 8. The
weight-1 validator records its epoch-0 vote, then goes offline. The other three
have 33/34 weight and 3/4 headcount, enough to certify both handovers and an
epoch-2 checkpoint without inventing an offline signature. When the original
validator returns, a desired epoch-2 vote is unavailable despite intact chain
ancestry. This is not an offline-quorum failure or an orphaned lock.

The [retained counterexample patch](bridge-v2-counterexamples/two-epoch-signer-restart.patch)
was compiled and executed against the integration source. It failed exactly
the two liveness assertions: the fresh lineage error was nonempty and the
expected one vote was absent. Exit status was **201**; 627 assertions passed
and two failed. In the scaled fixture the desired checkpoint was height 137.
The failure was:

```text
cannot persist fresh-lineage checkpoint 137 before signing relay:
checkpoint does not follow the durable validator-set lineage
critical check votes.size() == 1U has failed [0 != 1]
```

This is retained as a deliberately failing, opt-in counterexample, not a
silently skipped test or a passing recovery claim. On a disposable checkout:

```sh
git apply --check doc/design/bridge-v2-counterexamples/two-epoch-signer-restart.patch
git apply doc/design/bridge-v2-counterexamples/two-epoch-signer-restart.patch
cmake --build build-bridge -j8 --target test_bitcoin
build-bridge/bin/test_bitcoin \
  --run_test=finality_fresh_lineage_tests/staging_signer_restarts_after_two_certified_epochs \
  --log_level=test_suite --report_level=detailed
```

A safe repair requires exact historical, consensus-validated handover evidence
and sequential durable descendant transitions preserving the original
`last_signed_*` record. It must retain the separate same-set rule for recovering
an orphaned lock. Clearing journals, relaxing the epoch predicate to arbitrary
jumps, assuming online-peer authority or merely marking the error transient
would not establish that evidence. This recovery implementation is outside the
frozen incoming slice and blocks production recovery qualification.

## Remaining production gates

- Conflict closes the bridge deposit and burn path permanently. A verifier
  conflict halt and a fresh-lineage extension break are permanent, but a
  native bridge conflict does not itself break the fresh finality lineage.
  Previously accepted withdrawal roots remain claimable. Migration needs a
  live compatible verifier and is not an escape after halt.
- The bounded production committee, per-epoch keys, native fresh vote transport,
  historical catch-up and PoS V2 agreement remain separate work.
- Arbitrum forwarding and its full route qualification are not implemented here.
- Mainnet deployment, verifier identities, token behavior, fee economics,
  bridge compatibility and activation require their own review and approval.
- Stable-asset trading fees, shared native spot balances, futures risk and full
  client integration are not made production-ready by this bridge milestone.
- The functional runner registration gap and cross-platform tester packaging
  still require release qualification. Model CI is not a desktop release test.

The original [architecture](b3-bridge-v2-arbitrum-proposal.md),
[contract specification](b3-bridge-v2-contracts-spec.md),
[node design](b3-bridge-v2-node-changes.md) and
[regtest plan](b3-bridge-v2-regtest-plan.md) retain their proposed choices.
Publishing this development milestone does not approve those choices.

## Publication boundaries

Only source, synthetic fixtures and sanitized review documentation belong in
the published tree. The incoming history and previously unpublished V2 history
were inspected for private artifacts and credential patterns. No runtime
wallets, databases, private journals, generated runtime private keys, TLS private keys
or private operation logs are included. Synthetic key derivation and public
test vectors remain intentionally reproducible. Generated Anvil and B3 test
state and raw test logs remain local.

The existing model workflow runs on `flowmeshV2-dev`. Tester builds publish CI
artifacts only, with read-only repository permissions. The release workflow
requires a version tag or deliberate manual dispatch; neither is requested.
No pull request, force push, production-branch update, release tag or deployment
is part of this handoff. A remote workflow result must be checked for the exact
published revision; a committed workflow is not a successful run.
