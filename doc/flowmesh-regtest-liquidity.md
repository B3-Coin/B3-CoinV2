# Generated regtest liquidity fixture

`test/functional/feature_flowmesh_liquidity.py` is a bounded campaign for a
candidate build, not a production liquidity service or a P2FV node release.
All wallets, keys, funds, seats and TLS credentials are generated regtest data.
It reuses the existing fresh-market bootstrap; it does not run the inherited
release, withdrawal/reindex, adversarial-response or latency campaigns.

The intended workload is four native V1 pre-agreement operators and one
engine-off HTTPS client, six genuine matched auctions, then one certified
resting bid and one certified resting ask. B3 advances between matched fills.
The client buys nine raw base units for 475,000,000 B3 atoms in total. The
seller pays 47,500 atoms in aggregate fees. Final resting liquidity is a bid
for three units at 30,000,000 atoms and an ask for five at 80,000,000 atoms.
These are seeded test orders, not independent market makers or external demand.
Funding deposits are checkpointed and swept. The later trading checks certify
off-chain state; this fixture does not qualify B3 settlement of those trades or
a withdrawal. A pending checkpoint is not silently treated as a completed one.

| Canonical curve | Base/B3 meaning | Reverse B3/base view |
|---|---|---|
| bid | Buy base, sell B3 | Sell B3 |
| ask | Sell base, buy B3 | Buy B3 |

Every order uses an explicit certified account sequence. A submission receipt
is not a fill: the fixture waits for its exact certified action, all four
replicas' application, the resulting balances/reservations, and matching
clearing history. Unknown client outcomes use the original saved ActionId;
local unknown outcomes are polled and fail on timeout, never silently replaced.
The original bootstrap deposit is identified before mining and consumed by its
exact certified sweep, not substituted with another pending transaction.

The engine-off client verifies state and standing curves. Its returned trade
history remains explicitly endpoint-reported. The fixture cross-checks that
history against all four locally executing replicas, but does not change the
client's verification claim. Equal certified whole-state roots across all
replicas bind the independently checked buyer and seller balances.

No execution timestamps are invented. The chart input contains the real
sequence-ordered clears and their original verification labels. Native chart
tests may group them by microblock sequence; they must not label these as
wall-clock minute candles.

## Invocation and artifacts

Run only after separately building the candidate and approving this isolated
campaign. This is an opt-in direct invocation, not a default CI campaign:

```sh
python3 test/functional/feature_flowmesh_liquidity.py \
  --configfile=build/test/config.ini \
  --tmpdir=/absolute/path/to/a/new/disposable-liquidity-test \
  --portseed=738 --nocleanup
```

Replace `build` with the actual build directory and choose a fresh temporary
directory and unused port seed. Build `b3coind` and `b3coin-cli` first.
Do not point it at an existing node, production wallet or community database.

Normal runtime is estimated at roughly 2–5 minutes on the development host,
dominated by bootstrap/deposit depth. This estimate is not a latency result.
Individual waits and shutdown waits are bounded. No test run is claimed by
the addition of this fixture.

Artifacts retained in the fresh temporary directory:

- `flowmesh-liquidity.json`: exact fill counts, prices, quantities, fees,
  certified book/balances, transaction confirmations, daemon exit statuses and
  completion status.
- `flowmesh-liquidity-engine-off-market-data.json`: the decoded public
  `getflowmeshmarketdata` response for native chart rendering. This is not a
  wallet export and contains no private key or RPC/TLS authentication secret.
- Normal generated datadirs and logs remain available for failure diagnosis.

Default behavior stops every tracked daemon and the test relays. No process
is intentionally retained. Nonzero/forced child exits fail the campaign.
An explicit `--qt-review-hold-seconds=N` uses the existing bounded manual-review
hook, with a maximum of 900 seconds; it never launches Qt or proves Qt behavior.
The candidate remains the current V1 runtime, not full native P2FV trading.
