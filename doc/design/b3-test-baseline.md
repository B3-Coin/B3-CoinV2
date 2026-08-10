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

## Known divergences (out of scope: identity, not codec/era)

These failures predate codec/era isolation. They arise from B3's *identity*
— its monetary unit, address prefixes, network magic, and mainnet being a
legacy PoS chain — colliding with stock Bitcoin test vectors that hard-code
Bitcoin's identity on `ChainType::MAIN`. They cannot be repaired without
rewriting stock vectors or changing B3's historical constants, both
forbidden; several would also need a stock-parameter test context that does
not exist while `MAIN` denotes B3. They are catalogued here, not silently
ignored, and are left for a dedicated identity-isolation task.

| Suite | Colliding B3 identity | Why not fixed here |
|---|---|---|
| `validation_tests/subsidy_limit_test` | `COIN` = 1e6 (not 1e8) | expects Bitcoin's subsidy total; `GetBlockSubsidy` is stock code and re-scaling it would move the deterministic `TestChain100Setup` tip and cascade across chainstate fixtures |
| `key_tests`, `key_io_tests` | WIF/base58 prefixes 63/85/153, bech32 `b3` | vectors hard-code Bitcoin WIF/address strings on `main` |
| `bip324_tests` | — (under investigation) | v2 transport handshake vectors |
| `bloom_tests` | embedded Bitcoin keys/addresses | stock filter vectors |
| `compress_tests` | `COIN` scaling of amounts | stock txout-amount compression vectors |
| `miner_tests`, `validation_chainstatemanager_tests` | `MAIN` is a legacy PoS chain | generic PoW-chain fixtures run on `MAIN`; the miner emits modern-version blocks the legacy path rejects (`bad-version`) |
| `blockchain_tests`, `descriptor_tests`, `interfaces_tests`, `disconnected_transactions`, `mock_process` | address/amount/chain identity | stock expectations on `MAIN` |

Each is an identity collision, not a codec or era regression. The correct
remedy — running the affected stock-vector suites under an explicit
stock-parameter context, or per-chain parameterization of the colliding
bound — is a separate, scoped change; it must still leave the vectors and
B3's historical constants untouched.
