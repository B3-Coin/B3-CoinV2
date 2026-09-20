"""Generated, synthetic accounting adversaries; no network, wallets, or keys.

Identifiers are canonical 32-byte values encoded directly as lowercase hex.
No display-order reversal is performed anywhere in these fixtures.
Run with ``python3 -m unittest discover -s test/flowmesh_v2_model -v``.
"""

import copy
import json
from pathlib import Path
import random
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from model import Model, ModelError, action, action_id, canonical, load_profile, order_id


def ident(value):
    """Encode a generated integer in canonical raw-byte order, never reversed."""
    return value.to_bytes(32, "big").hex()


def limit_curve(side, price, lots):
    """One crossing tick, retaining the V1 zero-quantity endpoint rules."""
    if side == "BUY":
        return [[price, lots], [price + 1, 0]]
    return [[price - 1, 0], [price, lots]]


USD_A = "a" * 64
USD_B = "b" * 64
COIN = "c" * 64
ALT = "d" * 64
BUYER, SELLER, THIRD = ident(1), ident(2), ident(3)
DESTINATION, OTHER_DESTINATION = ident(10), ident(11)
SUBACCOUNT, OTHER_SUBACCOUNT = ident(20), ident(21)
RISK_CONFIG, OTHER_RISK_CONFIG = ident(30), ident(31)
SEAT_LOW, SEAT_HIGH = ident(0x100), ident(0x10000)
SEAT_LOW_OWNER, SEAT_HIGH_OWNER = ident(40), ident(41)
TREASURY_OWNER = ident(42)


class Fixture:
    def __init__(self, profile=None, reverse_markets=False):
        self.profile = copy.deepcopy(profile if profile is not None else load_profile())
        self.assets = {
            USD_A: {"decimals": 8, "symbol": "USD"},
            USD_B: {"decimals": 8, "symbol": "USD"},
            COIN: {"decimals": 8, "symbol": "COIN"},
            ALT: {"decimals": 8, "symbol": "ALT"},
        }
        self.markets = [
            {"base": COIN, "quote": USD_A, "base_atoms_per_lot": 1, "quote_atoms_per_tick": 1},
            {"base": ALT, "quote": USD_A, "base_atoms_per_lot": 1, "quote_atoms_per_tick": 1},
            {"base": COIN, "quote": USD_B, "base_atoms_per_lot": 1, "quote_atoms_per_tick": 1},
        ]
        self.seats = {SEAT_HIGH: SEAT_HIGH_OWNER, SEAT_LOW: SEAT_LOW_OWNER}
        self.model = Model(
            self.profile, self.assets,
            list(reversed(self.markets)) if reverse_markets else self.markets,
            self.seats, TREASURY_OWNER,
            {SUBACCOUNT: BUYER, OTHER_SUBACCOUNT: THIRD},
        )
        ids = self.model.market_ids
        self.market, self.alt_market, self.other_usd_market = list(reversed(ids)) if reverse_markets else ids
        self.deposit_counter = 0

    def fact(self, account, asset, amount, **changes):
        self.deposit_counter += 1
        fact = {
            "chain": ident(100), "custody_version": 2, "custody_domain": ident(101),
            "txid": ident(1000 + self.deposit_counter), "vout": 0,
            "account": account, "asset": asset, "amount": amount,
            "height": self.deposit_counter, "tx_index": 0, "output_index": 0,
            "verified": True,
        }
        fact.update(changes)
        return fact

    def fund(self, account, asset, amount):
        fact = self.fact(account, asset, amount)
        self.model.apply_batch(deposits=[fact])
        return fact

    def risk(self, asset=USD_A, status="PASS", withdrawable=1000000, config=RISK_CONFIG):
        return {SUBACCOUNT: {"config": config, "enabled": True, "asset": asset,
                             "status": status, "withdrawable": withdrawable}}


class AccountingHelpers:
    def setUp(self):
        self.f = Fixture()
        self.m = self.f.model

    def balance(self, account=BUYER, asset=USD_A, model=None):
        model = model or self.m
        return model.state["spot"].get(account, {}).get(asset, 0)

    def futures(self, asset=USD_A, model=None):
        model = model or self.m
        return model.state["futures"].get(SUBACCOUNT, {}).get(asset, 0)

    def outcome(self, report, instruction, status=None):
        result = report["outcomes"][action_id(instruction)]
        if status is not None:
            self.assertEqual(result["status"], status, result)
        return result

    def submit(self, instruction, status="ACCEPTED", **kwargs):
        report = self.m.apply_batch(actions=[instruction], **kwargs)
        self.m.assert_invariants()
        return self.outcome(report, instruction, status)

    def buy(self, sequence=0, price=20000, lots=1, market=None, account=BUYER):
        return action(account, sequence, "OPEN", market=market or self.f.market,
                      side="BUY", curve=limit_curve("BUY", price, lots))

    def sell(self, sequence=0, price=20000, lots=1, market=None, account=SELLER):
        return action(account, sequence, "OPEN", market=market or self.f.market,
                      side="SELL", curve=limit_curve("SELL", price, lots))

    def withdrawal(self, sequence, amount, account=BUYER, asset=USD_A, destination=DESTINATION):
        return action(account, sequence, "WITHDRAW", asset=asset, amount=amount,
                      destination=destination)

    def transfer(self, sequence, kind, amount, **changes):
        params = {"subaccount": SUBACCOUNT, "asset": USD_A, "amount": amount,
                  "risk_config": RISK_CONFIG}
        params.update(changes)
        return action(BUYER, sequence, kind, **params)

    def trade(self, price=20000, lots=1):
        self.f.fund(BUYER, USD_A, price * lots + 100)
        self.f.fund(SELLER, COIN, lots)
        buy, sell = self.buy(price=price, lots=lots), self.sell(price=price, lots=lots)
        report = self.m.apply_batch(actions=[buy, sell])
        self.outcome(report, buy, "ACCEPTED")
        self.outcome(report, sell, "ACCEPTED")
        self.m.assert_invariants()
        return buy, sell, report


class AccountingTests(AccountingHelpers, unittest.TestCase):
    def test_fixture_identifiers_are_raw_canonical_bytes(self):
        self.assertEqual(ident(1), "0" * 63 + "1")
        self.assertEqual(bytes.fromhex(ident(256))[-2:], b"\x01\x00")
        self.assertLess(SEAT_LOW, SEAT_HIGH)
        self.assertEqual(len(bytes.fromhex(USD_A)), 32)
        self.assertNotEqual(USD_A, USD_B)
        self.assertEqual(self.f.assets[USD_A]["symbol"], self.f.assets[USD_B]["symbol"])

    def test_canonical_encoding_and_semantic_action_identity(self):
        first = self.buy()
        reordered = dict(reversed(list(first.items())))
        self.assertEqual(canonical(first), canonical(reordered))
        self.assertEqual(action_id(first), action_id(reordered))
        unauthenticated = copy.deepcopy(first)
        unauthenticated["authorized"] = False
        self.assertEqual(action_id(first), action_id(unauthenticated))
        self.assertNotEqual(action_id(first), action_id(self.buy(price=20001)))
        self.assertIsInstance(canonical(first), bytes)

    def test_market_ids_include_decimals_and_ignore_constructor_order(self):
        reverse = Fixture(reverse_markets=True)
        self.assertEqual(self.f.market, reverse.market)
        self.assertEqual(self.f.alt_market, reverse.alt_market)
        altered_assets = copy.deepcopy(self.f.assets)
        altered_assets[COIN]["decimals"] = 7
        altered = Model(self.f.profile, altered_assets, self.f.markets, self.f.seats,
                        TREASURY_OWNER, {SUBACCOUNT: BUYER, OTHER_SUBACCOUNT: THIRD})
        self.assertNotEqual(self.f.market, altered.market_ids[0])

    def test_distinct_asset_ids_with_same_symbol_never_share_money(self):
        self.f.fund(BUYER, USD_A, 30000)
        missing_asset = self.buy(market=self.f.other_usd_market)
        self.submit(missing_asset, "REJECTED")
        self.assertEqual(self.balance(), 30000)
        self.assertEqual(self.balance(asset=USD_B), 0)
        self.f.fund(BUYER, USD_B, 20001)
        self.submit(self.buy(sequence=1, market=self.f.other_usd_market))
        self.assertEqual(self.balance(), 30000)
        self.assertEqual(self.balance(asset=USD_B), 0)

    def test_exact_asset_balance_is_shared_between_markets(self):
        self.f.fund(BUYER, USD_A, 30000)
        first = self.buy()
        self.submit(first)
        self.assertEqual(self.balance(), 9999)
        self.submit(self.buy(sequence=1, market=self.f.alt_market), "REJECTED")
        self.assertEqual(self.balance(), 9999)
        self.submit(action(BUYER, 2, "CANCEL", order_id=order_id(first), expected_revision=0))
        self.assertEqual(self.balance(), 30000)
        self.submit(self.buy(sequence=3, market=self.f.alt_market))
        self.assertEqual(self.balance(), 9999)

    def test_base_reservations_are_shared_between_quote_markets(self):
        self.f.fund(SELLER, COIN, 1)
        first = self.sell()
        self.submit(first)
        self.submit(self.sell(sequence=1, market=self.f.other_usd_market), "REJECTED")
        self.assertEqual(self.balance(SELLER, COIN), 0)
        self.submit(action(SELLER, 2, "CANCEL", order_id=order_id(first), expected_revision=0))
        self.assertEqual(self.balance(SELLER, COIN), 1)

    def test_open_reservation_blocks_withdrawal_and_futures_transfer(self):
        self.f.fund(BUYER, USD_A, 30000)
        self.submit(self.buy())
        self.submit(self.withdrawal(1, 10000), "REJECTED", capacities={USD_A: 30000})
        self.submit(self.transfer(2, "SPOT_TO_FUTURES", 10000), "REJECTED", risk=self.f.risk())
        self.assertEqual(self.balance(), 9999)
        self.assertEqual(self.futures(), 0)
        self.submit(self.transfer(3, "SPOT_TO_FUTURES", 9999), risk=self.f.risk())
        self.assertEqual(self.balance(), 0)
        self.assertEqual(self.futures(), 9999)

    def test_pending_withdrawal_and_futures_balances_do_not_back_orders(self):
        self.f.fund(BUYER, USD_A, 30000)
        self.submit(self.withdrawal(0, 15000), capacities={USD_A: 30000})
        self.submit(self.buy(sequence=1), "REJECTED")
        self.submit(self.transfer(2, "SPOT_TO_FUTURES", 10000), risk=self.f.risk())
        self.submit(self.buy(sequence=3, price=10000), "REJECTED")
        self.assertEqual(self.balance(), 5000)
        self.assertEqual(self.futures(), 10000)

    def test_explicit_transfers_preserve_exact_assets_and_round_trip(self):
        self.f.fund(BUYER, USD_A, 30000)
        self.f.fund(BUYER, USD_B, 500)
        self.submit(self.transfer(0, "SPOT_TO_FUTURES", 20000), risk=self.f.risk())
        self.assertEqual((self.balance(), self.futures()), (10000, 20000))
        self.submit(self.transfer(1, "FUTURES_TO_SPOT", 19999), risk=self.f.risk(withdrawable=19999))
        self.assertEqual((self.balance(), self.futures()), (29999, 1))
        self.assertEqual(self.balance(asset=USD_B), 500)
        self.submit(self.transfer(2, "FUTURES_TO_SPOT", 1), risk=self.f.risk(withdrawable=1))
        self.assertEqual((self.balance(), self.futures()), (30000, 0))

    def test_cash_adapters_never_implicitly_debit_spot_or_other_ledger(self):
        self.f.fund(BUYER, USD_A, 30000)
        self.submit(self.transfer(0, "FUTURES_TO_SPOT", 1), "REJECTED", risk=self.f.risk())
        self.assertEqual((self.balance(), self.futures()), (30000, 0))
        self.submit(self.transfer(1, "SPOT_TO_FUTURES", 20000), risk=self.f.risk())
        self.submit(self.withdrawal(2, 10001), "REJECTED", capacities={USD_A: 30000})
        self.submit(self.transfer(3, "FUTURES_TO_SPOT", 20001), "REJECTED", risk=self.f.risk())
        self.assertEqual((self.balance(), self.futures()), (10000, 20000))

    def test_missing_denied_unknown_or_undefined_risk_rejects_both_directions(self):
        for kind in ("SPOT_TO_FUTURES", "FUTURES_TO_SPOT"):
            for status in (None, "DENY", "UNKNOWN", "NOT_DEFINED"):
                with self.subTest(kind=kind, status=status):
                    fixture = Fixture()
                    model = fixture.model
                    fixture.fund(BUYER, USD_A, 100)
                    if kind == "FUTURES_TO_SPOT":
                        model.apply_batch(actions=[self.transfer(0, "SPOT_TO_FUTURES", 50)],
                                          risk=fixture.risk())
                        sequence = 1
                    else:
                        sequence = 0
                    before = (self.balance(model=model), self.futures(model=model))
                    instruction = self.transfer(sequence, kind, 1)
                    report = model.apply_batch(actions=[instruction],
                                               risk=None if status is None else fixture.risk(status=status))
                    self.outcome(report, instruction, "REJECTED")
                    self.assertEqual((self.balance(model=model), self.futures(model=model)), before)
                    model.assert_invariants()

    def test_risk_configuration_owner_and_withdrawable_are_checked(self):
        self.f.fund(BUYER, USD_A, 100)
        self.submit(self.transfer(0, "SPOT_TO_FUTURES", 50, risk_config=OTHER_RISK_CONFIG),
                    "REJECTED", risk=self.f.risk())
        self.submit(self.transfer(1, "SPOT_TO_FUTURES", 50, subaccount=OTHER_SUBACCOUNT),
                    "REJECTED", risk={OTHER_SUBACCOUNT: self.f.risk()[SUBACCOUNT]})
        self.submit(self.transfer(2, "SPOT_TO_FUTURES", 50), risk=self.f.risk())
        self.submit(self.transfer(3, "FUTURES_TO_SPOT", 11), "REJECTED", risk=self.f.risk(withdrawable=10))
        self.assertEqual((self.balance(), self.futures()), (50, 50))

    def test_batch_transfers_share_precomputed_withdrawable_allowance(self):
        self.f.fund(BUYER, USD_A, 200)
        self.submit(self.transfer(0, "SPOT_TO_FUTURES", 100), risk=self.f.risk())
        first = self.transfer(1, "FUTURES_TO_SPOT", 20)
        second = self.transfer(2, "FUTURES_TO_SPOT", 20)
        report = self.m.apply_batch(actions=[second, first], risk=self.f.risk(withdrawable=30))
        self.outcome(report, first, "ACCEPTED")
        self.outcome(report, second, "REJECTED")
        self.assertEqual((self.balance(), self.futures()), (120, 80))

    def test_spot_deposit_to_futures_does_not_expand_precomputed_allowance(self):
        self.f.fund(BUYER, USD_A, 100)
        into = self.transfer(0, "SPOT_TO_FUTURES", 100)
        out = self.transfer(1, "FUTURES_TO_SPOT", 21)
        report = self.m.apply_batch(actions=[out, into], risk=self.f.risk(withdrawable=20))
        self.outcome(report, into, "ACCEPTED")
        self.outcome(report, out, "REJECTED")
        self.assertEqual((self.balance(), self.futures()), (0, 100))

    def test_partial_fill_replace_stale_cancel_and_final_release(self):
        self.f.fund(BUYER, USD_A, 100000)
        self.f.fund(SELLER, COIN, 2)
        buy, sell = self.buy(lots=4), self.sell(lots=2)
        self.m.apply_batch(actions=[sell, buy])
        self.assertEqual(self.balance(BUYER, COIN), 2)
        self.assertEqual(self.balance(), 19996)
        self.assertEqual(self.balance(SELLER, USD_A), 39998)
        replace = action(BUYER, 1, "REPLACE", order_id=order_id(buy), expected_revision=0,
                         curve=limit_curve("BUY", 30000, 1))
        self.submit(replace)
        self.submit(action(BUYER, 2, "CANCEL", order_id=order_id(buy), expected_revision=0), "REJECTED")
        self.submit(action(BUYER, 3, "CANCEL", order_id=order_id(buy), expected_revision=1))
        self.assertEqual(self.balance(), 59998)
        self.assertEqual(self.balance(BUYER, COIN), 2)

    def test_failed_replace_preserves_order_and_reservation(self):
        self.f.fund(BUYER, USD_A, 30000)
        buy = self.buy()
        self.submit(buy)
        self.submit(action(BUYER, 1, "REPLACE", order_id=order_id(buy), expected_revision=0,
                           curve=limit_curve("BUY", 40000, 1)), "REJECTED")
        self.assertEqual(self.balance(), 9999)
        self.submit(action(BUYER, 2, "CANCEL", order_id=order_id(buy), expected_revision=0))
        self.assertEqual(self.balance(), 30000)

    def test_cumulative_order_fee_floor_survives_partial_fill_and_replace(self):
        self.f.fund(BUYER, USD_A, 100000)
        self.f.fund(SELLER, COIN, 2)
        buy = self.buy(price=10000, lots=2)
        self.m.apply_batch(actions=[buy, self.sell(price=10000)])
        self.submit(action(BUYER, 1, "REPLACE", order_id=order_id(buy), expected_revision=0,
                           curve=limit_curve("BUY", 10000, 1)))
        self.submit(self.sell(sequence=1, price=10000))
        self.assertEqual(self.balance(BUYER, COIN), 2)
        self.assertEqual(self.balance(), 79999)
        self.assertEqual(self.balance(SELLER, USD_A), 20000)
        self.assertEqual(sum(v.get(USD_A, 0) for v in self.m.state["fn"].values()), 1)

    def test_full_fill_charges_each_side_integer_fee(self):
        self.trade(price=60000)
        self.assertEqual(self.balance(), 97)
        self.assertEqual(self.balance(BUYER, COIN), 1)
        self.assertEqual(self.balance(SELLER, USD_A), 59997)
        self.assertEqual(self.m.state["treasury"].get(USD_A, 0), 1)
        self.assertEqual(self.m.state["fn"][SEAT_LOW].get(USD_A, 0), 3)
        self.assertEqual(self.m.state["fn"][SEAT_HIGH].get(USD_A, 0), 2)

    def test_treasury_floor_is_per_batch_without_fractional_carry(self):
        self.f.fund(BUYER, USD_A, 100000)
        self.f.fund(SELLER, COIN, 4)
        for sequence in (0, 1):
            self.m.apply_batch(actions=[self.buy(sequence, lots=2), self.sell(sequence, lots=2)])
            self.m.assert_invariants()
        self.assertEqual(self.m.state["treasury"].get(USD_A, 0), 0)
        self.assertEqual(sum(v.get(USD_A, 0) for v in self.m.state["fn"].values()), 8)
        self.assertEqual(self.balance(), 19996)
        self.assertEqual(self.balance(SELLER, USD_A), 79996)

    def test_fee_claim_uses_historical_seat_authority(self):
        self.trade(price=60000)
        claimant = self.m.fn_account(SEAT_LOW)
        wrong = action(claimant, 0, "CLAIM_FN", seat=SEAT_LOW, asset=USD_A, amount=3,
                       destination=DESTINATION, authority=SEAT_HIGH_OWNER)
        before = self.m.snapshot()
        with self.assertRaises(ModelError):
            self.m.apply_batch(actions=[wrong], capacities={USD_A: 100})
        self.assertEqual(self.m.snapshot(), before)
        self.assertEqual(self.m.state["fn"][SEAT_LOW][USD_A], 3)
        good = action(claimant, 0, "CLAIM_FN", seat=SEAT_LOW, asset=USD_A, amount=3,
                      destination=DESTINATION, authority=SEAT_LOW_OWNER)
        accepted = self.submit(good, capacities={USD_A: 100})
        self.assertEqual(self.m.state["fn"][SEAT_LOW].get(USD_A, 0), 0)
        self.assertEqual(self.submit(good, capacities={USD_A: 100}), accepted)

    def test_treasury_claim_uses_frozen_authority(self):
        self.trade(price=60000)
        account = self.m.treasury_account()
        wrong = action(account, 0, "CLAIM_TREASURY", asset=USD_A, amount=1,
                       destination=DESTINATION, authority=SEAT_LOW_OWNER)
        before = self.m.snapshot()
        with self.assertRaises(ModelError):
            self.m.apply_batch(actions=[wrong], capacities={USD_A: 100})
        self.assertEqual(self.m.snapshot(), before)
        good = action(account, 0, "CLAIM_TREASURY", asset=USD_A, amount=1,
                      destination=DESTINATION, authority=TREASURY_OWNER)
        self.submit(good, capacities={USD_A: 100})
        self.assertEqual(self.m.state["treasury"].get(USD_A, 0), 0)

    def test_identical_deposit_duplicates_credit_once_within_and_across_batches(self):
        fact = self.f.fact(BUYER, USD_A, 100)
        self.m.apply_batch(deposits=[fact, copy.deepcopy(fact)])
        self.assertEqual(self.balance(), 100)
        before = self.m.snapshot()
        self.m.apply_batch(deposits=[copy.deepcopy(fact)])
        self.assertEqual(self.m.snapshot(), before)

    def test_contradictory_deposits_reject_whole_batch(self):
        fact = self.f.fact(BUYER, USD_A, 100)
        contradictory = dict(fact, amount=101)
        before = self.m.snapshot()
        with self.assertRaises(ModelError):
            self.m.apply_batch(deposits=[fact, contradictory])
        self.assertEqual(self.m.snapshot(), before)
        self.m.apply_batch(deposits=[fact])
        before = self.m.snapshot()
        with self.assertRaises(ModelError):
            self.m.apply_batch(deposits=[contradictory])
        self.assertEqual(self.m.snapshot(), before)

    def test_custody_domain_and_output_are_part_of_deposit_identity(self):
        fact = self.f.fact(BUYER, USD_A, 100)
        other_domain = dict(fact, custody_domain=ident(102))
        other_output = dict(fact, vout=1, output_index=1)
        self.m.apply_batch(deposits=[fact, other_domain, other_output])
        self.assertEqual(self.balance(), 300)
        self.m.assert_invariants()

    def test_unverified_fact_and_unauthorized_action_rollback_whole_batch(self):
        before = self.m.snapshot()
        with self.assertRaises(ModelError):
            self.m.apply_batch(deposits=[self.f.fact(BUYER, USD_A, 100, verified=False)])
        self.assertEqual(self.m.snapshot(), before)
        unauthorized = self.withdrawal(0, 1)
        unauthorized["authorized"] = False
        with self.assertRaises(ModelError):
            self.m.apply_batch(deposits=[self.f.fact(BUYER, USD_A, 100)],
                               actions=[unauthorized], capacities={USD_A: 100})
        self.assertEqual(self.m.snapshot(), before)

    def test_completed_a_with_conflicting_b_returns_stored_a_without_reexecution(self):
        self.f.fund(BUYER, USD_A, 20)
        first = self.withdrawal(0, 5)
        stored = self.submit(first, capacities={USD_A: 20})
        conflict = self.withdrawal(0, 6, destination=OTHER_DESTINATION)
        report = self.m.apply_batch(actions=[conflict, first, copy.deepcopy(first)], capacities={USD_A: 20})
        self.assertEqual(self.outcome(report, first), stored)
        self.outcome(report, conflict, "REJECTED")
        self.assertEqual(self.balance(), 15)
        self.m.assert_invariants()

    def test_unconsumed_equivocation_does_not_pick_winner_by_input_order(self):
        first, conflict = self.withdrawal(0, 5), self.withdrawal(0, 6)
        outputs = []
        snapshots = []
        for candidates in ([first, conflict], [conflict, first]):
            fixture = Fixture()
            fixture.fund(BUYER, USD_A, 20)
            report = fixture.model.apply_batch(actions=candidates, capacities={USD_A: 20})
            for instruction in candidates:
                outcome = self.outcome(report, instruction, "REJECTED")
                self.assertIn("EQUIVOCATION", outcome["code"])
            self.assertEqual(self.balance(model=fixture.model), 20)
            outputs.append(report)
            snapshots.append(fixture.model.snapshot())
        self.assertEqual(outputs[0], outputs[1])
        self.assertEqual(snapshots[0], snapshots[1])

    def test_rejected_outcome_is_permanent_even_after_new_funding(self):
        instruction = self.withdrawal(0, 10)
        rejected = self.submit(instruction, "REJECTED", capacities={USD_A: 20})
        self.f.fund(BUYER, USD_A, 20)
        self.assertEqual(self.submit(instruction, "REJECTED", capacities={USD_A: 20}), rejected)
        self.assertEqual(self.balance(), 20)
        self.submit(self.withdrawal(1, 10), capacities={USD_A: 20})
        self.assertEqual(self.balance(), 10)

    def test_state_rejection_consumes_sequence_and_next_action_can_succeed_same_batch(self):
        self.f.fund(BUYER, USD_A, 10)
        rejected, accepted = self.withdrawal(0, 11), self.withdrawal(1, 10)
        report = self.m.apply_batch(actions=[accepted, rejected], capacities={USD_A: 10})
        self.outcome(report, rejected, "REJECTED")
        self.outcome(report, accepted, "ACCEPTED")
        self.assertEqual(self.balance(), 0)

    def test_sequence_gap_is_nonterminal(self):
        self.f.fund(BUYER, USD_A, 10)
        early = self.withdrawal(2, 1)
        self.submit(early, "REJECTED", capacities={USD_A: 10})
        self.assertEqual(self.balance(), 10)
        report = self.m.apply_batch(actions=[early, self.withdrawal(1, 1), self.withdrawal(0, 1)],
                                    capacities={USD_A: 10})
        self.outcome(report, early, "ACCEPTED")
        self.assertEqual(self.balance(), 7)

    def test_input_mutation_cannot_change_instruction_or_committed_state(self):
        fact = self.f.fact(BUYER, USD_A, 30000)
        instruction = self.buy()
        original_semantics = {key: value for key, value in copy.deepcopy(instruction).items()
                              if key != "authorized"}
        self.m.apply_batch(deposits=[fact], actions=[instruction])
        self.assertEqual(self.m.state["instructions"][action_id(instruction)],
                         canonical(original_semantics).decode("ascii"))
        before = self.m.snapshot()
        fact["amount"] = 999999
        instruction["params"]["curve"][0][1] = 999999
        self.assertEqual(self.m.snapshot(), before)
        self.m.assert_invariants()

    def test_pending_capacity_is_cumulative_and_exact_asset_scoped(self):
        self.f.fund(BUYER, USD_A, 100)
        self.f.fund(BUYER, USD_B, 100)
        self.submit(self.withdrawal(0, 60), capacities={USD_A: 100, USD_B: 100})
        self.submit(self.withdrawal(1, 41), "REJECTED", capacities={USD_A: 100, USD_B: 100})
        self.submit(self.withdrawal(2, 100, asset=USD_B), capacities={USD_A: 100, USD_B: 100})
        self.submit(self.withdrawal(3, 40), capacities={USD_A: 100, USD_B: 100})
        self.assertEqual(self.balance(), 0)
        self.assertEqual(self.balance(asset=USD_B), 0)

    def test_settlement_is_single_use_and_releases_capacity(self):
        self.f.fund(BUYER, USD_A, 120)
        first = self.submit(self.withdrawal(0, 60), capacities={USD_A: 60})
        settlement = {"receipt_id": first["receipt_id"], "asset": USD_A,
                      "amount": 60, "destination": DESTINATION,
                      "height": 100, "tx_index": 0, "event_index": 0,
                      "verified": True}
        self.m.apply_batch(settlements=[settlement, copy.deepcopy(settlement)])
        self.assertEqual(self.balance(), 60)
        before = self.m.snapshot()
        self.m.apply_batch(settlements=[copy.deepcopy(settlement)])
        self.assertEqual(self.m.snapshot(), before)
        self.submit(self.withdrawal(1, 60), capacities={USD_A: 60})
        self.assertEqual(self.balance(), 0)
        before = self.m.snapshot()
        with self.assertRaises(ModelError):
            self.m.apply_batch(settlements=[dict(settlement, destination=OTHER_DESTINATION)])
        self.assertEqual(self.m.snapshot(), before)

    def test_invalid_settlement_cannot_consume_pending_claim(self):
        self.f.fund(BUYER, USD_A, 100)
        result = self.submit(self.withdrawal(0, 50), capacities={USD_A: 100})
        valid = {"receipt_id": result["receipt_id"], "asset": USD_A,
                 "amount": 50, "destination": DESTINATION,
                 "height": 100, "tx_index": 0, "event_index": 0,
                 "verified": True}
        for changes in ({"amount": 49}, {"asset": USD_B},
                        {"destination": OTHER_DESTINATION}, {"verified": False}):
            with self.subTest(changes=changes):
                before = self.m.snapshot()
                with self.assertRaises(ModelError):
                    self.m.apply_batch(settlements=[dict(valid, **changes)])
                self.assertEqual(self.m.snapshot(), before)
        self.m.apply_batch(settlements=[valid])
        self.m.assert_invariants()

    def test_missing_capacity_rejects_withdrawal_without_money_changes(self):
        self.f.fund(BUYER, USD_A, 100)
        self.submit(self.withdrawal(0, 1), "REJECTED")
        self.assertEqual(self.balance(), 100)

    def test_amount_overflow_and_aggregate_custody_overflow_are_atomic(self):
        maximum = self.f.profile["limits"]["amount"]
        before = self.m.snapshot()
        with self.assertRaises(ModelError):
            self.m.apply_batch(deposits=[self.f.fact(BUYER, USD_A, maximum + 1)])
        self.assertEqual(self.m.snapshot(), before)
        self.f.fund(BUYER, USD_A, maximum)
        before = self.m.snapshot()
        with self.assertRaises(ModelError):
            self.m.apply_batch(deposits=[self.f.fact(SELLER, USD_A, 1)])
        self.assertEqual(self.m.snapshot(), before)
        self.m.assert_invariants()

    def test_reservation_notional_plus_fee_overflow_rejects_whole_candidate(self):
        maximum = self.f.profile["limits"]["amount"]
        markets = copy.deepcopy(self.f.markets)
        markets[0]["quote_atoms_per_tick"] = maximum
        model = Model(self.f.profile, self.f.assets, markets, self.f.seats,
                      TREASURY_OWNER, {SUBACCOUNT: BUYER, OTHER_SUBACCOUNT: THIRD})
        model.apply_batch(deposits=[self.f.fact(BUYER, USD_A, maximum)])
        instruction = action(BUYER, 0, "OPEN", market=model.market_ids[0], side="BUY",
                             curve=limit_curve("BUY", 1, 1))
        before = model.snapshot()
        with self.assertRaises(ModelError):
            model.apply_batch(actions=[instruction])
        self.assertEqual(model.snapshot(), before)

    def test_intermediate_fee_product_limit_is_enforced_atomically(self):
        profile = copy.deepcopy(self.f.profile)
        profile["limits"]["intermediate"] = 1000
        fixture = Fixture(profile=profile)
        fixture.fund(BUYER, USD_A, 30000)
        instruction = action(BUYER, 0, "OPEN", market=fixture.market, side="BUY",
                             curve=limit_curve("BUY", 20000, 1))
        before = fixture.model.snapshot()
        with self.assertRaises(ModelError):
            fixture.model.apply_batch(actions=[instruction])
        self.assertEqual(fixture.model.snapshot(), before)

    def test_fault_after_actions_restores_entire_candidate(self):
        self.f.fund(BUYER, USD_A, 30000)
        before = self.m.snapshot()
        with self.assertRaises(ModelError):
            self.m.apply_batch(actions=[self.buy()], fault="after_actions")
        self.assertEqual(self.m.snapshot(), before)
        self.submit(self.buy())

    def test_fault_after_first_fill_restores_balances_orders_fees_and_sequences(self):
        self.f.fund(BUYER, USD_A, 30000)
        self.f.fund(SELLER, COIN, 1)
        buy, sell = self.buy(), self.sell()
        before = self.m.snapshot()
        with self.assertRaises(ModelError):
            self.m.apply_batch(actions=[buy, sell], fault="after_first_fill")
        self.assertEqual(self.m.snapshot(), before)
        report = self.m.apply_batch(actions=[buy, sell])
        self.outcome(report, buy, "ACCEPTED")
        self.outcome(report, sell, "ACCEPTED")
        self.assertEqual(self.balance(BUYER, COIN), 1)

    def test_all_markets_clear_in_canonical_id_order(self):
        self.f.fund(BUYER, USD_A, 50000)
        self.f.fund(SELLER, COIN, 1)
        self.f.fund(SELLER, ALT, 1)
        instructions = [self.buy(), self.sell(), self.buy(1, market=self.f.alt_market),
                        self.sell(1, market=self.f.alt_market)]
        report = self.m.apply_batch(actions=list(reversed(instructions)))
        cleared = [entry["market"] for entry in report["fills"] if entry["volume"]]
        self.assertEqual(cleared, sorted([self.f.market, self.f.alt_market]))
        self.assertEqual(self.balance(BUYER, COIN), 1)
        self.assertEqual(self.balance(BUYER, ALT), 1)

    def test_snapshot_restore_is_byte_exact_and_replay_deterministic(self):
        self.f.fund(BUYER, USD_A, 100000)
        self.f.fund(SELLER, COIN, 3)
        opening = self.buy(lots=3)
        self.m.apply_batch(actions=[opening, self.sell()])
        snapshot = self.m.snapshot()
        restored = Model.restore(snapshot)
        self.assertEqual(restored.snapshot(), snapshot)
        self.assertEqual(restored.digest(), self.m.digest())
        instructions = [self.sell(sequence=1), action(BUYER, 1, "REPLACE",
                        order_id=order_id(opening), expected_revision=0,
                        curve=limit_curve("BUY", 20000, 2))]
        left = self.m.apply_batch(actions=instructions)
        right = restored.apply_batch(actions=list(reversed(instructions)))
        self.assertEqual(left, right)
        self.assertEqual(self.m.snapshot(), restored.snapshot())
        self.m.assert_invariants()
        restored.assert_invariants()

    def test_malformed_snapshot_is_rejected(self):
        for value in (b"not-json", b"{}", b"[]", self.m.snapshot()[:-1]):
            with self.subTest(value=value[:20]), self.assertRaises(ModelError):
                Model.restore(value)

    def test_snapshot_tampering_with_balance_or_stored_instruction_is_rejected(self):
        self.f.fund(BUYER, USD_A, 30000)
        instruction = self.buy()
        self.submit(instruction)
        original = json.loads(self.m.snapshot())
        changed_balance = copy.deepcopy(original)
        changed_balance["state"]["spot"][BUYER][USD_A] += 1
        changed_instruction = copy.deepcopy(original)
        stored = json.loads(changed_instruction["state"]["instructions"][action_id(instruction)])
        stored["params"]["curve"][0][1] += 1
        changed_instruction["state"]["instructions"][action_id(instruction)] = canonical(stored).decode("ascii")
        for altered in (changed_balance, changed_instruction):
            with self.assertRaises(ModelError):
                Model.restore(canonical(altered))

    def test_seeded_random_sequences_permutations_and_periodic_restore(self):
        """Two independently executed histories must agree after every batch."""
        rng = random.Random(0xF10A2026)
        first, second = Fixture(), Fixture()
        accounts = [BUYER, SELLER, THIRD]
        for fixture in (first, second):
            for account in accounts:
                fixture.fund(account, USD_A, 100000)
                fixture.fund(account, COIN, 20)
                fixture.fund(account, ALT, 20)
        next_sequence = dict.fromkeys(accounts, 0)
        history = []
        for step in range(80):
            candidates = []
            for account in rng.sample(accounts, rng.randint(1, 3)):
                sequence = next_sequence[account]
                next_sequence[account] += 1
                choice = rng.randrange(7)
                if choice < 3:
                    side = rng.choice(["BUY", "SELL"])
                    price, lots = rng.choice([9999, 10000, 20000]), rng.randint(1, 4)
                    candidates.append(action(account, sequence, "OPEN",
                        market=rng.choice([first.market, first.alt_market]),
                        side=side, curve=limit_curve(side, price, lots)))
                elif choice == 3:
                    candidates.append(self.withdrawal(sequence, rng.randint(1, 1000), account=account))
                elif choice == 6 and account == BUYER:
                    candidates.append(self.transfer(sequence, rng.choice(["SPOT_TO_FUTURES", "FUTURES_TO_SPOT"]),
                                                    rng.randint(1, 500)))
                else:
                    owned = [record for record in first.model.state["orders"].values()
                             if record["account"] == account and record["active"]]
                    if owned:
                        selected = rng.choice(sorted(owned, key=lambda record: record["order_id"]))
                        params = {"order_id": selected["order_id"], "expected_revision": selected["revision"]}
                        if choice == 5:
                            params["curve"] = limit_curve(selected["side"], rng.choice([9999, 10000, 20000]),
                                                          rng.randint(1, 4))
                        candidates.append(action(account, sequence, "REPLACE" if choice == 5 else "CANCEL", **params))
                    else:
                        candidates.append(action(account, sequence, "CANCEL",
                            order_id=ident(100000 + step), expected_revision=0))
            if history and step % 4 == 0:
                candidates.append(copy.deepcopy(rng.choice(history)))
            left_input = copy.deepcopy(candidates)
            right_input = copy.deepcopy(candidates)
            rng.shuffle(left_input)
            rng.shuffle(right_input)
            left = first.model.apply_batch(actions=left_input, capacities={USD_A: 300000}, risk=first.risk())
            right = second.model.apply_batch(actions=right_input, capacities={USD_A: 300000}, risk=second.risk())
            self.assertEqual(left, right, f"report divergence in generated batch {step}")
            self.assertEqual(first.model.snapshot(), second.model.snapshot(), f"state divergence at {step}")
            first.model.assert_invariants()
            second.model.assert_invariants()
            history.extend(candidates)
            if step % 7 == 0:
                second.model = Model.restore(second.model.snapshot())


class V1SegregationTests(AccountingHelpers, unittest.TestCase):
    """Keep V1 fixture arithmetic separate from all V2 credit paths."""

    def test_v1_balance_and_reservation_do_not_fund_v2_order_or_withdrawal(self):
        self.m.seed_v1(BUYER, USD_A, 100000, 20000, 10000)
        self.assertEqual(self.balance(), 0)
        self.submit(self.buy(), "REJECTED")
        self.submit(self.withdrawal(1, 1), "REJECTED", capacities={USD_A: 1000000})
        self.assertEqual(self.balance(), 0)
        self.m.assert_invariants()

    def test_v1_settlement_then_distinct_v2_deposit_is_single_use(self):
        self.m.seed_v1(BUYER, USD_A, 100, 20, 10)
        receipt = ident(9001)
        self.m.settle_v1(BUYER, USD_A, receipt, 10, DESTINATION)
        self.assertEqual(self.balance(DESTINATION, USD_A), 0)
        settled = self.m.snapshot()
        self.m.settle_v1(BUYER, USD_A, receipt, 10, DESTINATION)
        self.assertEqual(self.m.snapshot(), settled)
        with self.assertRaises(ModelError):
            self.m.settle_v1(BUYER, USD_A, receipt, 9, DESTINATION)
        self.assertEqual(self.m.snapshot(), settled)
        fact = self.f.fact(DESTINATION, USD_A, 10)
        self.m.redeposit_v1(receipt, fact)
        self.assertEqual(self.balance(DESTINATION, USD_A), 10)
        before = self.m.snapshot()
        self.m.redeposit_v1(receipt, copy.deepcopy(fact))
        self.assertEqual(self.m.snapshot(), before)
        with self.assertRaises(ModelError):
            self.m.redeposit_v1(receipt, self.f.fact(DESTINATION, USD_A, 10))
        self.assertEqual(self.m.snapshot(), before)
        self.m.assert_invariants()

    def test_v1_receipt_cannot_redeposit_different_asset_amount_or_destination(self):
        for changes in ({"asset": USD_B}, {"amount": 9}, {"account": OTHER_DESTINATION}):
            with self.subTest(changes=changes):
                fixture = Fixture()
                model = fixture.model
                model.seed_v1(BUYER, USD_A, 100, 20, 10)
                receipt = ident(9002)
                model.settle_v1(BUYER, USD_A, receipt, 10, DESTINATION)
                before = model.snapshot()
                fact = fixture.fact(DESTINATION, USD_A, 10)
                fact.update(changes)
                with self.assertRaises(ModelError):
                    model.redeposit_v1(receipt, fact)
                self.assertEqual(model.snapshot(), before)

    def test_existing_v2_deposit_cannot_be_retroactively_linked_to_v1_funding(self):
        fact = self.f.fund(DESTINATION, USD_A, 10)
        self.m.seed_v1(BUYER, USD_A, 0, 0, 10)
        receipt = ident(9004)
        self.m.settle_v1(BUYER, USD_A, receipt, 10, DESTINATION)
        before = self.m.snapshot()
        with self.assertRaises(ModelError):
            self.m.redeposit_v1(receipt, fact)
        self.assertEqual(self.m.snapshot(), before)
        self.assertEqual(self.balance(DESTINATION, USD_A), 10)

    def test_v1_overwithdraw_and_repeated_seed_are_atomic(self):
        self.m.seed_v1(BUYER, USD_A, 100, 20, 10)
        before = self.m.snapshot()
        with self.assertRaises(ModelError):
            self.m.settle_v1(BUYER, USD_A, ident(9003), 11, DESTINATION)
        self.assertEqual(self.m.snapshot(), before)
        with self.assertRaises(ModelError):
            self.m.seed_v1(BUYER, USD_A, 100, 20, 10)
        self.assertEqual(self.m.snapshot(), before)


if __name__ == "__main__":
    unittest.main()
