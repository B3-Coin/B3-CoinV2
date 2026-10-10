# V2 branch integration review — 2026-10-10

Status: bounded integration qualification, not release or activation approval.
Ongoing branch: `flowmeshV2-dev`. No pull request, history rewrite or deployment.

## Frozen inputs and scope

Previous ongoing-branch head: `a76db51502a2b894e1d7007ae951c7a8dabf0f5c`.
Completed source head: `2fcf8fa6366e340656a4c04880e5841638d65e5b`.
The previous integrated source was `965e23cc0830001ca33c9048063b81d5383d776a`.
The 20 new source commits were integrated by a conflict-free, history-preserving
merge, `890dc4897f20219b9c0e437a897405980c77efc7`.

This slice adds fresh per-epoch finality keys and native vote gossip, bridge V2
Arbitrum route forwarders and local LayerZero/USDT0 mocks, bUSD-tip/native-B3
fee sponsorship, and opt-in regtest H1 producer rotation/H2 checkpoint pins.
It does not change the FlowMesh engine, Qt trading or the isolated agreement,
accounting and storage model sources. No latency improvement is inferred.

The integration preserves unrelated local market-model prototypes without
including them. The obsolete bridge recovery is not part of this milestone;
its history and retained failure evidence remain intact.

## Concrete defect reproduced and repaired

Finding: `wallet::GenerateFreshFinalityKey` reused a replacement decision after
its store became stale. A separate wallet sharing the same validator identity
could record K1's binding between the first eligibility read and the erase.
The store correctly refused that stale erase, but the helper reopened it and
erased K1 without re-evaluating whether the now-bound key was replaceable.

Retained failing regression commit:
`d8eb8fcdf8acbf6322ea57927233b8b7c693d912`.
Repair/source qualification commit:
`9c91abadcba972f3d72d8a61fc5898adcd5cec1e`.

Deterministic interleaving in
`src/wallet/test/finality_fresh_key_wallet_tests.cpp`,
`replacement_refuses_a_key_bound_after_the_policy_read`:

1. Generate an unbound K1 for one epoch in a disposable signer directory.
2. Replacement policy observes the unbound record.
3. A second store instance records K1's binding txid before replacement returns.
4. The stale caller must refuse, leaving exactly K1, its secret and binding.

Before repair: exit 201, five failing assertions. Replacement succeeded,
reported a replacement, erased K1's held secret and left two records.
After repair: exit 0; the regression and all five fresh-key wallet tests pass.

Minimal correction: remove the reopen-and-erase fallback. Any stale erase
refuses; a subsequent invocation opens current state and re-evaluates policy.
No quorum, certificate, timing, journal or key-erasure predicate was relaxed.
A separate read-only reviewer checked the race, regression and proposed fix;
that review did not execute the tests or qualify the entire consensus system.

## Windows compile blocker

The prior package run
[37653070062](https://github.com/B3-Coin/B3-CoinV2/actions/runs/37653070062)
failed Windows compilation; both macOS jobs passed. Its first C++ error was
the Windows `SERVICE_STOPPED` macro expanding the identically named member of
`FlowMeshNotReady` in `src/node/flowmesh_health.h`.

The compile regression `c346603` defines that Windows macro before including
the header on non-Windows builders. Before correction, the native build exits
2 at the same enum declaration. `dbb79a2` renames only the internal enumerator
to `SERVICE_IS_STOPPED`. Its numeric value, readiness predicates and public
`service_stopped` JSON string remain unchanged. The regression removes only
the macro it introduced; it does not undefine a real Windows system macro.
This is an advisory-health portability fix, not a consensus or timing change.
An actual fresh Windows package build remains a distinct qualification.

## Personally executed qualification

Build: macOS arm64, AppleClang 17, RelWithDebInfo, wallet enabled, GUI off,
bridge test tools enabled. Built `test_bitcoin`, `b3coind`, `b3coin-cli`,
`b3-bridge-ethcheck`, `b3-bridge-regtest-beacon`, `b3-bridge-regtest-certify`.

Results before this documentation-only commit. Native daemon/tests were rebuilt
on `9c91abad`; contracts/helpers were run earlier in the integration with
byte-identical relevant source. The later health-only portability successor
does not change any bridge, key-store or producer-rotation logic:

| Campaign | Actual result |
| --- | --- |
| `forge test --offline --summary`, under `contracts/` | 386 passed, 25 suites, exit 0 |
| `python3 -B -m unittest discover -s contrib/b3bridge -p 'test*.py'` | 234 passed, exit 0 |
| Native forwarder, sponsored-rule, sponsor-wallet, fresh-parameter and fresh-wallet suites | 40 cases passed, exit 0 |
| Pure fresh-key cases, transport cases and H1 parameter cases | 30 cases passed, exit 0 |
| `finality_fresh_key_tests/fresh_key_signers_on_chain` | 1 case, 1,185 assertions passed, exit 0 |
| RT7s sponsored round trip, seed 10102026 | Passed, exit 0; both routes, tamper rejection and reindex replay |
| H1 producer rotation fixture I, seed 10102027 | Passed, exit 0; new producer, V1 finality, restart, reindex and fresh sync |
| Advisory-health suite with Windows macro environment on `dbb79a2` | 11 cases passed, exit 0; daemon/test build passed |

The five fresh-key wallet cases were rerun on `dbb79a2` and passed, including
the stale-binding race regression. In total 82 distinct targeted native cases
passed across the described selections; reruns are not counted twice.

The initial RT7s attempt stopped before starting a node because the sandbox
refused a localhost socket. Its failure was retained, not counted as a pass.
The normal approved test invocation uses only disposable local regtest nodes
and local mock Ethereum/Arbitrum chains, never a live endpoint or production key.

Native tests are targeted selections, **not all 2,396 native test cases**.
The expensive full native, Qt, release-package and WAN suites were not rerun.
The isolated model trees and runner are byte-identical to the previous branch
head; prior results are not represented as freshly executed here. GitHub's
model run, if triggered by publication, is reported by its actual run identity.

RT7s passed the zero-native-B3 user's sponsored Ethereum and Arbitrum-stand-in
withdrawals, duplicate/tampered request refusals, relayer pool/tip accounting
and reindex equality. Fixture I admitted n4 as a producer at 220 while V1's
checkpoint-eligible set still excluded it; V1 advanced epoch 2 to 4, finalized
296 and remained intact. These are local functional results, not WAN evidence.

Reproduce the central regression after building the normal native test target:

```sh
build/bin/test_bitcoin \
  --run_test=finality_fresh_key_wallet_tests/replacement_refuses_a_key_bound_after_the_policy_read
```

Functional sources and framework describe their exact synthetic identities,
knobs and fixtures. Invoke each generated build-tree script with its build's
`test/config.ini`, a fresh temporary directory, and the recorded seed. RT7s
needs local `anvil`/`forge` plus the built bridge tools; it never forks Ethereum.

## Remaining gates, not silently closed

- Fresh-key mode is explicit regtest opt-in. Its lineage catch-up does not
  repair the old V1 lock counterexample merely by being present in a binary.
- Manual epoch bindings can lose bridge quorum/qualification. Automatic
  rebinding, expiry/stranding handling and production key custody remain open.
  Fresh secret files are not protected by wallet encryption/backups.
- H1 separates block production from checkpoint approval; it is not complete
  PoS V2/BFT recovery. Escape-set economics and competing-fork behavior remain
  design/qualification gates. No mainnet activation height or H2 hash was set.
- H1 broken-lineage fixture B uses silent signers and changed regtest extension
  settings, not the planned every-signer-live/withheld-certificate drill.
- LayerZero/OFT/USDT0 are local mocks. Real DVNs, contracts, options, delivery,
  costs and configuration equivalence are unverified.
- Sponsored burns still consume native B3 from the relayer. bUSD tips do not
  replenish that pool automatically. In-memory coin locks do not revoke signed
  partials. Multi-input ANYONECANPAY partials are not exactly-once requests;
  reconciliation must account for every actually confirmed funded burn.
- Forwarder pins need append-only, height-versioned treatment before migration.
- No live-wallet/production-key operation, live-validator change, live-contract
  transaction, public deployment, consensus activation, trading benchmark or
  release was performed. Disposable local test chains did execute transactions.

History publication checks must inspect every newly reachable commit, exclude
private runtime artifacts, and push only the existing V2 branch without force.
Existing workflows are unchanged: branch publication can run model CI, not
tag/release deployment. External production audit remains necessary.
