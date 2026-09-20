"""Test-only batch-ordering and configured-limit boundary vectors."""
import copy
import unittest

from model import Model, ModelError, action, action_id, load_profile, order_id
from test_model import (Fixture, BUYER, SELLER, USD_A, COIN, DESTINATION,
                        TREASURY_OWNER, limit_curve)


class ModelBoundaryTests(unittest.TestCase):
    def test_nonstable_quote_remains_explicitly_unimplemented(self):
        f = Fixture()
        markets = copy.deepcopy(f.markets)
        markets[0]["quote"] = markets[1]["base"]
        with self.assertRaisesRegex(ModelError, "NON_STABLE_QUOTE_NOT_IMPLEMENTED"):
            Model(f.profile, f.assets, markets, f.seats, TREASURY_OWNER, {})

    def test_eligible_same_batch_deposit_can_back_order(self):
        f = Fixture()
        a = action(BUYER, 0, "OPEN", market=f.market, side="BUY",
                   curve=limit_curve("BUY", 59, 1))
        report = f.model.apply_batch(deposits=[f.fact(BUYER, USD_A, 100)], actions=[a])
        self.assertEqual(report["outcomes"][action_id(a)]["status"], "ACCEPTED")
        self.assertEqual(f.model.state["orders"][order_id(a)]["reservation"], 60)
        self.assertEqual(f.model.state["spot"][BUYER][USD_A], 40)

    def test_cancel_release_can_fund_next_sequence_withdrawal_same_batch(self):
        f = Fixture()
        f.fund(BUYER, USD_A, 20001)
        opening = action(BUYER, 0, "OPEN", market=f.market, side="BUY",
                         curve=limit_curve("BUY", 20000, 1))
        f.model.apply_batch(actions=[opening])
        cancel = action(BUYER, 1, "CANCEL", order_id=order_id(opening), expected_revision=0)
        withdraw = action(BUYER, 2, "WITHDRAW", asset=USD_A, amount=20001, destination=DESTINATION)
        report = f.model.apply_batch(actions=[withdraw, cancel], capacities={USD_A: 20001})
        self.assertTrue(all(x["status"] == "ACCEPTED" for x in report["outcomes"].values()))
        self.assertEqual(f.model.state["spot"][BUYER][USD_A], 0)
        self.assertEqual(sum(c["amount"] for c in f.model.state["pending"].values()), 20001)
        self.assertEqual(f.model.state["custody"][USD_A], 20001)

    def test_later_clearing_cannot_retroactively_fund_earlier_withdrawal(self):
        f = Fixture()
        f.fund(BUYER, USD_A, 20001)
        f.fund(SELLER, COIN, 1)
        buy = action(BUYER, 0, "OPEN", market=f.market, side="BUY", curve=limit_curve("BUY", 20000, 1))
        sell = action(SELLER, 0, "OPEN", market=f.market, side="SELL", curve=limit_curve("SELL", 20000, 1))
        withdraw = action(SELLER, 1, "WITHDRAW", asset=USD_A, amount=19999, destination=DESTINATION)
        report = f.model.apply_batch(actions=[withdraw, sell, buy], capacities={USD_A: 20001})
        self.assertEqual(report["outcomes"][action_id(withdraw)]["code"], "INSUFFICIENT_AVAILABLE")
        self.assertEqual(f.model.state["spot"][SELLER][USD_A], 19999)
        self.assertEqual(f.model.state["next_sequence"][SELLER], 2)
        self.assertEqual(f.model.state["pending"], {})

    def test_sequence_bound_refuses_without_wrapping_or_replacing_history(self):
        profile = load_profile()
        profile["limits"]["sequence"] = 2
        f = Fixture(profile=profile)
        for seq in range(2):
            a = action(BUYER, seq, "WITHDRAW", asset=USD_A, amount=1, destination=DESTINATION)
            f.model.apply_batch(actions=[a])
        before = f.model.snapshot()
        final = action(BUYER, 2, "WITHDRAW", asset=USD_A, amount=1, destination=DESTINATION)
        report = f.model.apply_batch(actions=[final])
        self.assertEqual(report["outcomes"][action_id(final)]["code"], "SEQUENCE_EXHAUSTED")
        self.assertEqual(f.model.snapshot(), before)

    def test_interpolation_bound_failure_rolls_back_whole_candidate(self):
        profile = load_profile()
        profile["limits"]["intermediate"] = 100000
        f = Fixture(profile=profile)
        f.fund(BUYER, USD_A, 1000)
        before = f.model.snapshot()
        a = action(BUYER, 0, "OPEN", market=f.market, side="BUY", curve=[[0, 100], [2000, 0]])
        with self.assertRaises(ModelError):
            f.model.apply_batch(actions=[a])
        self.assertEqual(f.model.snapshot(), before)


if __name__ == "__main__":
    unittest.main()
