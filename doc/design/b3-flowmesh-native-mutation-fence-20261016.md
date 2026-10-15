# Native chain mutation fence

Date: 2026-10-16. Eight targeted native cases and 823 assertions passed;
the complete 19-case chainstate-manager suite also passed in both subsequent
ordinary macOS qualification campaigns.

This optional kernel component invalidates process-local publication tickets
before selected native chain mutations. It publishes no chain facts and is not
enabled by node initialization. Existing signing, custody, anchor, finality and
client-verification checks remain unchanged, including their blocking queries.
It is a prerequisite for a future chain-facts adapter, not that adapter itself.

## Contract and ownership

The [implementation](../../src/kernel/chain_mutation.h) is a final, header-only
observer owned by immutable `ChainstateManager` options. The
[option](../../src/kernel/chainstatemanager_opts.h) defaults to null; normal node
and standalone kernel callers install nothing. Tests opt in through their
existing setup options. No node logger, JSON, service callback or FlowMesh
runtime dependency is introduced into the kernel.

One atomic word holds generation, active mutation depth and fenced, exhausted
and stopped state. Beginning a lexical mutation scope changes generation and
active depth in one compare-and-swap before the covered live state can change.
Capture and completion refuse throughout that scope, including synchronous
reentrant validation callbacks. Nested scopes and exception unwinding preserve
the fence; leaving the outer scope does not mark anything current.

A future single publication owner must capture a ticket while copying the
required facts under the real chain lock, install its immutable bundle as
unavailable, then complete only that exact ticket. Completion cannot reopen a
newer generation. Readers must recheck the bundle's ticket: `CURRENT` means only
that this local generation matches and is not fenced. It does **not** establish
canonical ancestry, authenticated membership, balances, certificate validity,
freshness or permission to spend or sign. It is not a lease against future
mutation.

Tickets retain their issuer, preventing address-reuse confusion with a different
observer. They do not retain or lease the chainstate manager. A lexical scope
borrows the immutable options-owned observer and cannot be copied or moved; its
owner must outlive the call. Manager destruction terminally sets `STOPPED`, so a
retained observer cannot issue or complete a fresh usable ticket afterward.
Generation and nesting depth saturate to `EXHAUSTED` rather than wrapping.

The hook itself performs only scalar atomic operations: no arbitrary callback,
mutex acquisition, allocation, logging, disk work or published-snapshot
destruction. It requires always-lock-free 64-bit atomics; unsupported platforms
refuse this optional facility without an atomic fallback lock. Compare-and-swap
is lock-free, not a wait-free or absolute-latency guarantee. The surrounding
native validation functions still perform their ordinary work and locking.

## Covered native mutation sites

Scopes begin before the following covered mutations and remain active across
their subsequent lexical work and early returns. Failed attempts may fence
conservatively even when no durable chain change succeeds.

| Native area | Instrumented functions |
| --- | --- |
| Active tip | `Chainstate::ConnectTip`, `DisconnectTip`, changed-tip `LoadChainTip` |
| Chainstate lifetime and selection | `ChainstateManager::InitializeChainstate`, `AddChainstate`, `RemoveChainstate`, `DeleteChainstate`, `ResetChainstates`, destructor |
| Coins view lifetime | `Chainstate::InitCoinsDB`, `InitCoinsCache`, `ResetCoinsViews` |
| Target selection | `Chainstate::SetTargetBlock`, `SetTargetBlockHash` |
| Snapshot validation mode | `ChainstateManager::MaybeValidateSnapshot` success and invalid-snapshot transitions |

The hooks are in [validation.cpp](../../src/validation.cpp) and
[validation.h](../../src/validation.h). Snapshot preparation before a chainstate
is selected is not published active-chain authority; `AddChainstate` fences
before selection changes. Per-kind call counters are diagnostic observations,
not a coherent snapshot or authority.

## Review findings and reproduced component counterexample

Source review found three gaps in the earlier unpublished draft:

- `ResetCoinsViews` could clear usable views while a completed ticket remained
  current. View initialization, cache replacement and reset now have scopes.
- A one-shot pre-mutation hook allowed a synchronous `BlockChecked` callback to
  capture and complete a new ticket while `ConnectTip` still had mutations left
  to perform. Atomic active-depth tracking now refuses capture and completion
  until the lexical mutation scopes end.
- A destruction-generation increment alone permitted fresh capture and
  completion after manager destruction. Terminal `STOPPED` now forbids both.

An independently executed, disposable C++ component control compared the
preserved pre-review header with the final header. Inside the same mutating
lexical region, it attempted capture and completion, then checked normal
publication after that region:

| Component | Capture inside mutation | Completion inside mutation | Actual exit |
| --- | ---: | ---: | ---: |
| Earlier draft using its `BeforeMutation` hook | 1 | 1 | 1 |
| Final header using `ChainMutationScope` | 0 | 0 | 0 |

After the final scope ended, a fresh ticket remained fenced and exact completion
succeeded. This is a real failing-before/passing-after **draft-component**
counterexample. It is not a before/after node, chain-facts authority or performance
qualification. The predecessor and control outputs remain retained privately.
The reviewed final header SHA-256 is
`0d3f700999569e708337267caa08c7dc35a646729471764e7adf3eca7283911c`.

## Targeted native qualification

Eight new cases in
[validation_chainstatemanager_tests.cpp](../../src/test/validation_chainstatemanager_tests.cpp)
exercise these native seams:

- Ticket supersession, issuer ownership, default-null behavior and exhaustion.
- Nested mutation, refused reentrant publication and exception unwind.
- Real `ConnectTip` and recursive `BlockChecked` observation with asynchronous
  validation callbacks deliberately delayed.
- Real disconnect and a leaf ticket check while another thread holds `cs_main`.
- Snapshot selection, tip load, targets, removal, reset and coins-view lifetime.
- Successful and invalid snapshot validation-mode transitions.
- Manager destruction and refusal of post-destruction capture.

The ordinary integrated macOS build executed the eight targeted cases: all
823 assertions passed, actual exit 0. Its test executable SHA-256 is
`be37384508008d2fb04eabe42872ad520e0649dca144634bb3062dced1f5fdc1`.
The invalid-snapshot negative case intentionally emitted a fatal notification
inside its disposable fixture; that case and the test process passed. This was
not a live node failure.

The complete 19-case chainstate-manager suite passed in the 398-case selected
native campaign, using test executable SHA-256
`fbd7949f09fafade9b3544a1b2786ed4194593de59391633ff39bd2e06d0b676`.
A separate 30-case campaign (19 chainstate-manager, seven finality-pin persistence
and four asset-metadata cases) also passed, with 3,596 assertions and actual
exit 0, using the earlier `be373845…` executable identified above. These
overlapping selections are not an additional combined test count. Deliberate
write-refusal and invalid-snapshot notifications came only from generated
negative fixtures, not preserved node data. Separate source
review accepted the closed lifecycle, reentrancy and destruction findings; that
source verdict and the draft-component counterexample are distinct from native
test execution. Linux, TSan and standalone kernel-library qualification remain
pending. The [Linux CI profile](b3-flowmesh-native-linux-guardrails-20261016.md)
requires the exact eight case names; absent prerequisites cannot silently pass
discovery. After a fresh native build, the focused cases can be selected with:

```sh
./build/bin/test_bitcoin \
  '--run_test=validation_chainstatemanager_tests/chain_mutation_*' \
  --report_level=short --log_level=error
```

## Remaining authority work

No production publisher or consumer uses these tickets. A complete typed
chain-facts adapter still needs synchronous coverage and version binding for all
facts it claims, including canonical ancestry, current and historical seats,
custody, checkpoint/index readiness, finality and IBD state. Some can change
without an active-tip change; this generation must not silently stand for them.
That adapter must resolve missing evidence outside market locks and distinguish
temporary unavailability from verified invalidity without discarding signing
obligations. No blocking authority query has been replaced here.

This component does not change protocol, quorum, fork choice, timing policy,
serialization, durable journals or economic rules. It does not qualify signing
across restored old backups, changing membership, bridge verification, machine
power loss or a 200 ms trading target.
