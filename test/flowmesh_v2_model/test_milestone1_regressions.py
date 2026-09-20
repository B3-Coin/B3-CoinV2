"""Milestone 1 regressions from the frozen dc5682785f3d accounting audit.

All funding and identities are synthetic. Snapshot edits below deliberately
contradict retained execution evidence; they do not model forged complete
histories, authenticate snapshots, or qualify production persistence.
"""

from copy import deepcopy
import json
import unittest

from test_model import (
    BUYER, COIN, SEAT_HIGH, SEAT_LOW, SELLER, USD_A, Fixture, ident,
    limit_curve,
)
from model import Model, ModelError, action, action_id, canonical, order_id


class MilestoneOneRestorationTests(unittest.TestCase):
    def setUp(self):
        self.fixture = Fixture()
        self.model = self.fixture.model

    def document(self):
        return json.loads(self.model.snapshot())

    def reject(self, document):
        with self.assertRaises(ModelError):
            Model.restore(canonical(document))

    def open_unfilled_buy(self):
        self.fixture.fund(BUYER, USD_A, 11)
        opening = action(BUYER, 0, "OPEN", market=self.fixture.market,
                         side="BUY", curve=limit_curve("BUY", 10, 1))
        report = self.model.apply_batch(actions=[opening])
        self.assertEqual(report["outcomes"][action_id(opening)]["status"],
                         "ACCEPTED")
        return order_id(opening)

    def test_restore_rejects_spot_owner_substitution(self):
        self.fixture.fund(BUYER, USD_A, 10)
        document = self.document()
        spot = document["state"]["spot"]
        spot[SELLER] = spot.pop(BUYER)
        self.reject(document)

    def test_restore_rejects_fabricated_cancel_and_reservation_release(self):
        oid = self.open_unfilled_buy()
        document = self.document()
        order = document["state"]["orders"][oid]
        document["state"]["spot"][BUYER][USD_A] += order["reservation"]
        order.update(active=False, terminal="CANCELLED", reservation=0)
        self.reject(document)

    def test_restore_rejects_revision_spend_without_lifetime_notional(self):
        oid = self.open_unfilled_buy()
        document = self.document()
        order = document["state"]["orders"][oid]
        document["state"]["spot"][BUYER][USD_A] += order["reservation"]
        order.update(spent=order["bound"], reservation=0)
        self.assertEqual(order["notional"], 0)
        self.assertEqual(order["lifetime_filled"], 0)
        self.reject(document)

    def test_restore_rejects_lifetime_fill_without_execution(self):
        oid = self.open_unfilled_buy()
        document = self.document()
        document["state"]["orders"][oid]["lifetime_filled"] = 1
        self.reject(document)

    def test_restore_rejects_revision_above_action_revision_limit(self):
        oid = self.open_unfilled_buy()
        document = self.document()
        document["state"]["orders"][oid]["revision"] = (
            self.model.limits["sequence"] + 1
        )
        self.reject(document)

    def test_restore_rejects_fee_reassignment_between_historical_seats(self):
        self.fixture.fund(BUYER, USD_A, 60003)
        self.fixture.fund(SELLER, COIN, 1)
        buy = action(BUYER, 0, "OPEN", market=self.fixture.market,
                     side="BUY", curve=limit_curve("BUY", 60000, 1))
        sell = action(SELLER, 0, "OPEN", market=self.fixture.market,
                      side="SELL", curve=limit_curve("SELL", 60000, 1))
        self.model.apply_batch(actions=[buy, sell])
        document = self.document()
        rewards = document["state"]["fn"]
        self.assertEqual(rewards[SEAT_LOW][USD_A], 3)
        self.assertEqual(rewards[SEAT_HIGH][USD_A], 2)
        rewards[SEAT_HIGH][USD_A] += rewards[SEAT_LOW][USD_A]
        rewards[SEAT_LOW][USD_A] = 0
        self.reject(document)

    def test_restore_rejects_v1_reservation_reclassified_as_pending(self):
        self.model.seed_v1(ident(800), USD_A, 0, 10, 0)
        document = self.document()
        vault = next(iter(document["state"]["v1"].values()))
        vault.update(reserved=0, pending=10)
        self.reject(document)


class MilestoneOneZeroReplacementTests(unittest.TestCase):
    def setUp(self):
        self.fixture = Fixture()
        self.model = self.fixture.model
        self.fixture.fund(BUYER, USD_A, 11)
        opening = action(BUYER, 0, "OPEN", market=self.fixture.market,
                         side="BUY", curve=limit_curve("BUY", 10, 1))
        report = self.model.apply_batch(actions=[opening])
        self.assertEqual(report["outcomes"][action_id(opening)]["status"],
                         "ACCEPTED")
        self.oid = order_id(opening)

    def replace(self, curve):
        instruction = action(BUYER, 1, "REPLACE", order_id=self.oid,
                             expected_revision=0, curve=curve)
        report = self.model.apply_batch(actions=[instruction])
        return report["outcomes"][action_id(instruction)]

    def reject_without_moving_funds(self, curve):
        before = deepcopy(self.model.state)
        outcome = self.replace(curve)
        self.assertEqual((outcome["status"], outcome["code"]),
                         ("REJECTED", "INVALID_CURVE"))
        self.assertEqual(self.model.state["next_sequence"][BUYER], 2)
        self.assertEqual(self.model.state["orders"], before["orders"])
        for bucket in ("spot", "futures", "custody", "pending", "settled",
                       "fn", "treasury", "fee_pool", "fees_collected"):
            self.assertEqual(self.model.state[bucket], before[bucket], bucket)
        self.model.assert_invariants()

    def test_zero_replacement_rejects_seventeen_points(self):
        self.assertEqual(self.model.limits["curve_points"], 16)
        self.reject_without_moving_funds([[price, 0] for price in range(17)])

    def test_zero_replacement_rejects_repeated_prices(self):
        self.reject_without_moving_funds([[10, 0], [10, 0]])

    def test_zero_replacement_rejects_descending_prices(self):
        self.reject_without_moving_funds([[11, 0], [10, 0]])

    def test_valid_zero_replacement_still_cancels_and_releases_reserve(self):
        before = deepcopy(self.model.state["orders"][self.oid])
        outcome = self.replace([[10, 0], [11, 0]])
        self.assertEqual((outcome["status"], outcome["code"]),
                         ("ACCEPTED", "OK"))
        self.assertEqual(self.model.state["next_sequence"][BUYER], 2)
        expected = dict(before, active=False, terminal="CANCELLED", reservation=0)
        self.assertEqual(self.model.state["orders"][self.oid], expected)
        self.assertEqual(self.model.state["spot"][BUYER][USD_A], 11)
        self.assertEqual(self.model.state["custody"][USD_A], 11)
        self.model.assert_invariants()


if __name__ == "__main__":
    unittest.main()
