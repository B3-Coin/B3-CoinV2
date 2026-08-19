# B3 Test Baseline

Status of `test_bitcoin` after codec and era isolation. The rule for this
baseline: repair by **implementation isolation**, never by rewriting stock
Bitcoin Core expected vectors and never by changing B3's historical
constants or genesis.

## Required suites (green)

The suites exercised by codec and era isolation all pass:

- `transaction_tests` (tx_valid / tx_invalid / sighash-adjacent)
- `sighash_tests`
- `script_tests`
- `blockmanager_tests`
- `hardfork_tests`
- `legacy_genesis_tests`, `legacy_identity_tests`
- `legacy_pos_tests`, `pos_dispatch_tests`
- `legacy_replay_tests`, `legacy_replay_persist_tests`
- `legacy_boundary_tests`
- `legacy_transition_tests` (synthetic H / H+1)
- `legacy_net_tests`, `net_tests`, `denialofservice_tests`

## Isolation fixes applied here

- **Monetary bound.** Upstream tx vectors describe Bitcoin's `MAX_MONEY`
  (21e6 × 1e8). B3's historical cap is larger, so the two BADTX vectors
  encoding Bitcoin's ceiling are in range on B3 and would read as valid.
  `CheckTransaction` gained a defaulted `max_value_out` argument (default =
  B3 `MAX_MONEY`, so production behavior is unchanged); the stock-vector
  driver passes `GENERIC_MAX_MONEY`. No vector or constant changed.
- **Transport message length.** `MAX_PROTOCOL_MESSAGE_LENGTH` is restored to
  the stock 4 MB; legacy B3 chains, whose historical blocks reach the 5 MB
  legacy limit, use `MAX_LEGACY_PROTOCOL_MESSAGE_LENGTH` on the v1 transport
  (selected by `legacy_b3coin`). This restores `net_tests/v2transport_test`
  while preserving the legacy 5 MB capability.

## Denomination model (owner ruling, 2026-08-17)

B3 carries an **intentional denomination transition** across the era
boundary, ruled by the project owner on 2026-08-17 and reconciled here per
the precedence order in `CLAUDE.md` §Authority:

> **1 modern B3 = 1 kB3 = 1,000 legacy B3.**

The transition is **display-only**. The atomic on-chain unit is identical
in both eras; no state value is converted at the boundary. This is required
by the core invariant (one immutable ledger through H) and is what the code
does: `TrustedReplay` reconstructs legacy UTXOs byte-exactly and the
`U == U'` equivalence pipeline (`doc/design/b3-utxo-equivalence.md`)
compares raw `CAmount`s across sync modes. Any ×1000 state rewrite would
break `U == U'` by construction.

| Quantity | Atomic units | Era | Governing source |
|---|---|---|---|
| Atomic unit (`CAmount` 1) | 1 | both, unchanged | `src/consensus/amount.h:12` ("Amount in B3Coin's smallest unit") |
| `COIN` — legacy **B3** display unit | 1e6 | legacy; historical sub-unit thereafter | `src/consensus/amount.h:15`; historical `master:src/util.h:35` |
| `KILO_COIN` — **kB3** display unit | 1e9 | legacy (already the client's **default** display unit) | historical `master:src/util.h:34`; unit table `master:src/qt/bitcoinunits.cpp` (kB3 factor 1000000000); default `master:src/qt/optionsmodel.cpp` (`nDisplayUnit` defaults to `KBTC`) |
| Modern **B3** display unit (= kB3) | 1e9 | modern (> H) | owner ruling 2026-08-17 (this section is its reconciliation); **not yet implemented in code** — see flag below |
| FN registration marker | 1 × `COIN` = 1e6 | legacy fact, frozen | historical `master:src/fn-activity.cpp:407` (`nValue == 1 * COIN`, "exactly"); mirrored `src/node/fn_pod.cpp` `MARKER_VALUE` |
| Historical PoD collateral tiers | 25M / 20M / 15M × `COIN` | legacy fact, frozen | `src/legacy/consensus.cpp` `GetFNCollateral` (verbatim from historical `fn-activity.h`) |
| Historical stake-supply cap | 75,656,908 × `KILO_COIN` | legacy fact, frozen | `doc/design/b3-legacy-fork-choice.md` §reward cap |
| `MAX_MONEY` sanity bound | 662,200,000,000 × `COIN` | both (consensus-critical sanity check, not supply) | `src/consensus/amount.h:27` |

Interpretation rules that follow:

- **Historical PoD amounts stay in legacy B3 units.** The 25M/20M/15M
  collateral tiers are 25M/20M/15M *legacy* B3 (= 25,000/20,000/15,000
  kB3). They must never be silently reinterpreted through the modern
  display denomination, in code, tests, reports, or documentation. Raw
  atomic values and consensus serialization are preserved everywhere.
- **FN issuance is denomination-independent.** The disintegration value
  determines whether a historical PoD qualified; every qualifying PoD is
  exactly one FN eligibility event; no denomination conversion changes the
  number of FN Coins issued.
- **Current modern-code display state (recorded, not resolved here):**
  `FormatMoney`/RPC amount formatting divide by `COIN` = 1e6
  (`src/util/moneystr.cpp`), i.e. they format in *legacy* B3 units, not
  the modern unit. **FLAGGED DISCREPANCY:** the modern Qt unit table
  (`src/qt/bitcoinunits.cpp` `factor(Unit::BTC)`) returns `100'000'000`
  (1e8) under the label "B3" — a stock-Bitcoin leftover matching *neither*
  the legacy B3 unit (1e6) nor the ruled modern unit (1e9). Aligning
  display code with the ruling is a separate code change requiring owner
  direction; this document changes no production constant and introduces
  no conversion logic.

The architecture contract and master handoff are silent on display
denomination (checked 2026-08-17); no tracked document conflicts with this
ruling.

## Known divergences (out of scope for codec/era isolation)

These failures predate codec/era isolation and none is a codec or era
regression. They are **not** one uniform "identity collision": they fall
into three classes, and the `COIN`-scale rows are *intentional era and
denomination behavior* (see the denomination model above), not accidental
divergence. All were re-verified on 2026-08-17 in a from-scratch build at
`28d7818`, serially, one suite per process, with identical results —
ruling out stale artifacts and toolchain drift. They cannot be repaired
without rewriting stock vectors or changing B3's historical constants,
both forbidden. They are catalogued here, not silently ignored, and are
left for a dedicated isolation task.

Classes: **DENOM** — intentional B3 monetary scale and era denomination;
needs era-aware expectations. **IDENT** — B3's network/address identity
(prefixes, magic, port, HRP) vs stock Bitcoin vectors. **FIXTURE** —
generic PoW-chain test fixtures that cannot run while `MAIN` is a legacy
PoS chain.

| Suite | Class | Mechanism (evidence) | Why not fixed here |
|---|---|---|---|
| `validation_tests/subsidy_limit_test` | DENOM | `COIN` = 1e6 (not 1e8) | expects Bitcoin's subsidy total; `GetBlockSubsidy` is stock code and re-scaling it would move the deterministic `TestChain100Setup` tip and cascade across chainstate fixtures |
| `compress_tests` | DENOM | stock txout-amount compression vectors sample Bitcoin-scale amounts (`TestPair(COIN, 0x9)` assumes 1e8) | era-aware expectations needed; vectors untouched |
| `util_tests` (`util_FormatMoney` and amount cases) | DENOM | `FormatMoney` formats at B3 atomic scale: `[0.00 != 0.0000001]`, `[9223372036854.775807 != 92233720368.54775807]` | expectations assume Bitcoin's 1e8 scale; must become era-aware |
| `rpc_tests` (monetary cases) | DENOM | `ValueFromAmount` decimals at B3 scale (`0.17622195` expected, B3 emits 6-decimal scale) | same; the suite also has an IDENT failure (below) |
| `key_tests`, `key_io_tests` | IDENT | WIF/base58 prefixes 63/85/153: `DecodeSecret` rejects Bitcoin 0x80 WIF vectors → invalid key → `assert(keydata)` at `key.cpp:184` (lldb-verified) | vectors hard-code Bitcoin WIF/address strings on `main` |
| `bip324_tests` | IDENT | **resolved 2026-08-17:** the v2-transport HKDF salt embeds the network message-start magic (`src/bip324.cpp:36-38`); B3's magic diverges every derived key/session-id from the BIP-324 vectors (280 checks) | vectors are defined over Bitcoin's magic |
| `bloom_tests` | IDENT | embedded Bitcoin keys/addresses | stock filter vectors |
| `script_standard_tests` | IDENT | bech32 HRP: emits `b31…` where vectors expect `bc1…` (same payload, different HRP/checksum) | vectors hard-code Bitcoin HRP |
| `net_peer_connection_tests` | IDENT | expects log `Added connection to 127.0.0.1:8333`; B3's default port is 5647 | stock port expectation |
| `rpc_tests` (`rpc_rawsign`) | IDENT | `Invalid Bitcoin address: 3HqAe9…` — hardcoded Bitcoin P2SH address | stock address string |
| `miner_tests`, `validation_chainstatemanager_tests` | FIXTURE | generic PoW-chain fixtures run on `MAIN`; the miner emits modern-version blocks the legacy path rejects (`bad-version`) | `MAIN` is a legacy PoS chain |
| `txpackage_tests` | FIXTURE | SEGV inside `TestChain100Setup::CreateValidMempoolTransaction` (lldb: `EXC_BAD_ACCESS` in `COutPoint` ctor) — downstream of the broken 100-block fixture | same broken fixture |
| `validation_chainstate_tests` | FIXTURE | `CreateAndActivateUTXOSnapshot` fixture fails on `MAIN` | same fixture class |
| `blockchain_tests`, `descriptor_tests`, `interfaces_tests`, `disconnected_transactions`, `mock_process` | IDENT/FIXTURE | address/amount/chain identity; stock expectations on `MAIN` (not yet individually diagnosed) | stock expectations on `MAIN` |

The correct remedy — running the affected stock-vector suites under an
explicit stock-parameter context, and giving the DENOM rows *era-aware
expectations* (parameterized by B3's atomic scale and, where display
strings appear, by the era's display denomination) — is a separate,
scoped change; it must still leave the vectors, B3's historical constants,
and all raw atomic values and consensus serialization untouched.
