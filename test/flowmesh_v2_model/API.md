# Test-profile model interface contract

Standalone Python model; no node integration or real authentication. Profile
and all input facts are TEST ONLY. `model.py` exports `Model`, `ModelError`,
`load_profile()`, `action(account, sequence, kind, **params)`, `action_id(a)`,
`order_id(a)`, `canonical(value)` (UTF-8 bytes).

Constructor: `Model(profile, assets, markets, seats, treasury_owner, subaccounts)`.
Assets map exact 64-lowercase-hex AssetId to `{decimals:int, symbol:str}`.
Markets list contains `{base, quote, base_atoms_per_lot, quote_atoms_per_tick}`;
model derives immutable MarketId including configured decimals/profile identity.
`model.market_ids` preserves constructor order for fixture convenience only;
execution always sorts MarketId. Seats map SeatId to synthetic historical
authority ID (both exact 64-hex), treasury_owner is a synthetic authority ID.
Subaccounts map subaccount ID to owner account ID (both exact 64-hex).

`model.apply_batch(deposits=(), settlements=(), actions=(), capacities=None,
risk=None, fault=None)` returns deterministic outcomes/fills. Whole candidate
rollback on invalid external fact/overflow/invariant failure; individual
authenticated state failures consume only sequence/outcome, no partial money.
`fault` is TEST ONLY, `after_actions` or `after_first_fill`, to verify atomicity.
All caller inputs are copied; late mutation cannot mutate committed state.

Deposit fact: `{chain, custody_version:2, custody_domain, txid, vout,
account, asset, amount, height, tx_index, output_index, verified:True}`.
Identity = chain/version/domain/outpoint. Sorted by height/tx/output; identical
duplicates collapse, contradictory duplicates reject entire batch.
Settlement fact: `{receipt_id, asset, amount, destination, height, tx_index,
event_index, verified:True}` must match an existing pending claim. Identical
duplicates have no effect; conflicting ones reject. Deposits/settlements are
already anchored synthetic evidence; no B3 proof or import completeness claim.

Actions are built by `action`; semantic ID excludes the `authorized` synthetic
authentication flag, which defaults True. False authentication rejects candidate.
Kinds/params:

- OPEN: market, side (BUY/SELL), curve (list of [price_ticks, lots]).
- REPLACE: order_id, expected_revision, curve (additional remaining lots).
- CANCEL: order_id, expected_revision.
- WITHDRAW: asset, amount, destination (64-hex). Requires synthetic capacity
  dict AssetId->atoms in this batch; existing pending plus request <= capacity.
- SPOT_TO_FUTURES / FUTURES_TO_SPOT: subaccount, asset, amount,
  risk_config (64-hex). `risk` maps subaccount to `{config, enabled:True,
  asset, status:PASS|DENY|UNKNOWN|NOT_DEFINED, withdrawable:int}`.
  No missing/undefined risk permits either transfer. Mismatched config rejects.
  The supplied withdrawable allowance is consumed across all transfer-outs in
  the batch; a transfer-in does not increase that allowance. It is a synthetic
  cash gate, not an oracle, margin calculation or loss engine.
- CLAIM_FN: seat, asset, amount, destination; account must equal that seat's
  derived `model.fn_account(seat)` and authorization authority must be the
  historical ID via param `authority`. Separate sequence stream.
- CLAIM_TREASURY: asset, amount, destination, authority; account must equal
  `model.treasury_account()` and authority exactly frozen treasury owner.

Every consumed outcome retained under ActionId with semantic instruction bytes
and sequence. Completed A replayed alongside conflicting B returns A's stored
outcome and rejects B; only unconsumed-sequence groups get EQUIVOCATION. Gaps
are nonterminal. Returned batch report is sorted by action ID. Rejected actions
may be followed by their next sequence in the same batch.

`model.state` is a JSON-serializable test inspection object. Balances are
`state['spot'][account][asset]`, `state['futures'][subaccount][asset]`;
orders/reservations, custody, pending/consumed receipts, outcomes, next_sequence,
fee totals and historical FN/treasury balances are retained explicitly.
`model.snapshot()` returns full canonical state+configuration bytes;
`Model.restore(bytes)` validates/reconstructs the same model;
`model.digest()` hashes snapshot; `model.assert_invariants()` validates it.
Snapshots are not authenticated state commitments: these checks do not detect
a coherently forged history or prove protection against external rollback.

V1 synthetic case API: `seed_v1(vault, asset, available, reserved, pending)`
establishes a separate synthetic fixture (once per vault/asset).
`settle_v1(vault, asset, receipt_id, amount, destination)` consumes only V1
pending, moves its backed atoms into tracked synthetic external funds, and
retains immutable receipt. `redeposit_v1(receipt_id, deposit_fact)` consumes
that external funding once and admits a distinct V2 deposit in a batch.
No direct V1->V2 snapshot credit. These operations do not authorize real
migration or validate a B3 withdrawal/deposit. No active V1 orders are replayed.
