"""TEST ONLY: focused checks of V1 matching and persistent reservation."""

import copy
import itertools
import unittest

try:
    from .curves import auction, buy_bound, evaluate, validate_curve
except ImportError:
    from curves import auction, buy_bound, evaluate, validate_curve


def identifier(number):
    return f"{number:064x}"


def order(number, side, curve, account=None, filled=0):
    return {"order_id": identifier(number),
            "account": account if account is not None else identifier(number),
            "side": side, "curve": curve, "revision_filled": filled}


class CurveTests(unittest.TestCase):
    def test_validation(self):
        validate_curve("BUY", [[0, 4], [2, 4], [4, 0]], 4, 4, 3)
        validate_curve("SELL", [[0, 0], [4, 4]], 4, 4, 3)
        validate_curve("SELL", [[4, 4]], 4, 4, 3)
        invalid = [
            ("BID", [[0, 1], [1, 0]], 4, 4, 3),
            ("BUY", [], 4, 4, 3),
            ("BUY", [[0, 1]], 4, 4, 3),
            ("BUY", [[0, 0], [1, 0]], 4, 4, 3),
            ("BUY", [[0, 1], [0, 0]], 4, 4, 3),
            ("BUY", [[1, 1], [0, 0]], 4, 4, 3),
            ("BUY", [[0, 1], [2, 2], [4, 0]], 4, 4, 3),
            ("SELL", [[0, 2], [1, 1]], 4, 4, 3),
            ("SELL", [[0, 0]], 4, 4, 3),
            ("SELL", [[-1, 1]], 4, 4, 3),
            ("SELL", [[0, -1]], 4, 4, 3),
            ("SELL", [[5, 1]], 4, 4, 3),
            ("SELL", [[0, 5]], 4, 4, 3),
            ("SELL", [[True, 1]], 4, 4, 3),
            ("SELL", [[0, 1.0]], 4, 4, 3),
            ("SELL", [[0, 1, 2]], 4, 4, 3),
            ("BUY", [[0, 4], [2, 4], [4, 0]], 4, 4, 2),
            ("SELL", [[0, 1]], 4, 4, 65),
            ("SELL", [[0, 1]], 4, 4, 0),
            ("SELL", [[0, 1]], 4, True, 3),
        ]
        for args in invalid:
            with self.subTest(args=args), self.assertRaises(ValueError):
                validate_curve(*args)

    def test_floor_interpolation_and_flat_clamps(self):
        buy = [[2, 4], [5, 0]]
        sell = [[2, 0], [5, 4]]
        self.assertEqual([evaluate(buy, p) for p in range(8)],
                         [4, 4, 4, 2, 1, 0, 0, 0])
        self.assertEqual([evaluate(sell, p) for p in range(8)],
                         [0, 0, 0, 1, 2, 4, 4, 4])
        self.assertEqual(evaluate([], 2), 0)
        self.assertEqual(evaluate([[2, 7]], 100), 7)

    def test_candidates_are_only_existing_breakpoints(self):
        orders = [order(1, "BUY", [[0, 4], [4, 0]]),
                  order(2, "SELL", [[0, 0], [4, 4]])]
        self.assertEqual(evaluate(orders[0]["curve"], 2), 2)
        self.assertEqual(evaluate(orders[1]["curve"], 2), 2)
        self.assertEqual(auction(orders),
                         {"price": None, "volume": 0, "imbalance": 0,
                          "fills": {identifier(1): 0, identifier(2): 0}})
        self.assertEqual(auction([]),
                         {"price": None, "volume": 0, "imbalance": 0, "fills": {}})

    def test_volume_then_imbalance_then_lowest_price(self):
        result = auction([order(1, "BUY", [[0, 4], [4, 0]]),
                          order(2, "SELL", [[0, 0], [1, 1], [2, 2]])])
        self.assertEqual((result["price"], result["volume"]), (2, 2))
        result = auction([order(1, "BUY", [[0, 4], [2, 4], [4, 0]]),
                          order(2, "SELL", [[0, 2], [3, 2]])])
        self.assertEqual((result["price"], result["volume"], result["imbalance"]),
                         (3, 2, 0))
        result = auction([order(1, "BUY", [[2, 2], [4, 0]]),
                          order(2, "SELL", [[1, 2]])])
        self.assertEqual((result["price"], result["volume"], result["imbalance"]),
                         (1, 2, 0))

    def test_largest_remainder_not_simple_account_priority(self):
        orders = [order(1, "BUY", [[1, 1], [2, 0]]),
                  order(2, "BUY", [[1, 2], [2, 0]]),
                  order(3, "BUY", [[1, 4], [2, 0]]),
                  order(4, "SELL", [[1, 4]])]
        result = auction(orders)
        self.assertEqual(result["volume"], 4)
        self.assertEqual(result["fills"],
                         {identifier(1): 1, identifier(2): 1,
                          identifier(3): 2, identifier(4): 4})

    def test_tied_remainders_use_canonical_account_bytes(self):
        # Raw byte order chooses 00...01 before 01...00. Reversed uint256
        # display-byte ordering and order-ID sorting would pick the other.
        low = "00" * 31 + "01"
        high = "01" + "00" * 31
        orders = [order(1, "BUY", [[1, 1], [2, 0]], high),
                  order(2, "BUY", [[1, 1], [2, 0]], low),
                  order(3, "SELL", [[1, 1]])]
        before = copy.deepcopy(orders)
        expected = auction(orders)
        self.assertEqual(expected["fills"][identifier(1)], 0)
        self.assertEqual(expected["fills"][identifier(2)], 1)
        for permutation in itertools.permutations(orders):
            self.assertEqual(auction(list(permutation)), expected)
        self.assertEqual(orders, before)

    def test_seller_long_side_and_revision_residual(self):
        orders = [order(1, "BUY", [[1, 4], [2, 0]], filled=2),
                  order(2, "SELL", [[1, 3]], filled=1),
                  order(3, "SELL", [[1, 2]]),
                  order(4, "SELL", [[1, 5]], filled=5)]
        result = auction(orders)
        self.assertEqual(result["volume"], 2)
        self.assertEqual(result["fills"],
                         {identifier(1): 2, identifier(2): 1,
                          identifier(3): 1, identifier(4): 0})
        for side in ("BUY", "SELL"):
            self.assertEqual(sum(result["fills"][o["order_id"]]
                                 for o in orders if o["side"] == side), 2)

    def test_exact_large_aggregates(self):
        quantity = 2 ** 70
        orders = [order(1, "BUY", [[1, quantity], [2, 0]]),
                  order(2, "BUY", [[1, quantity], [2, 0]]),
                  order(3, "SELL", [[1, 3]])]
        result = auction(orders)
        self.assertEqual(result["imbalance"], 2 * quantity - 3)
        self.assertEqual(result["fills"],
                         {identifier(1): 2, identifier(2): 1, identifier(3): 3})

    def test_intermediate_limit_checks_absolute_interpolation(self):
        for points in ([[0, 4], [4, 0]], [[0, 0], [4, 4]]):
            with self.subTest(points=points):
                self.assertEqual(evaluate(points, 2, max_intermediate=8),
                                 evaluate(points, 2))
                with self.assertRaisesRegex(ValueError, "interpolation numerator"):
                    evaluate(points, 2, max_intermediate=7)

    def test_intermediate_limit_checks_staircase_products_sums_and_scale(self):
        points = [[0, 4], [2, 2], [4, 0]]
        for scale, ceiling, failure in ((1, 5, "staircase product"),
                                         (1, 7, "staircase sum"),
                                         (3, 23, "scaled staircase bound")):
            with self.subTest(scale=scale, ceiling=ceiling):
                with self.assertRaisesRegex(ValueError, failure):
                    buy_bound(points, scale, max_intermediate=ceiling)
        self.assertEqual(buy_bound(points, 3, max_intermediate=24), 24)
        self.assertEqual(buy_bound(points, 3), 24)

    def test_intermediate_limit_checks_auction_totals_and_rationing(self):
        buyers_long = [order(1, "BUY", [[1, 4], [2, 0]]),
                       order(2, "BUY", [[1, 4], [2, 0]]),
                       order(3, "SELL", [[1, 3]])]
        sellers_long = [order(1, "BUY", [[1, 3], [2, 0]]),
                        order(2, "SELL", [[1, 4]]),
                        order(3, "SELL", [[1, 4]])]
        for orders, total_name in ((buyers_long, "aggregate demand"),
                                    (sellers_long, "aggregate supply")):
            with self.subTest(side=total_name):
                before = copy.deepcopy(orders)
                with self.assertRaisesRegex(ValueError, total_name):
                    auction(orders, max_intermediate=7)
                with self.assertRaisesRegex(ValueError, "rationing product"):
                    auction(orders, max_intermediate=8)
                self.assertEqual(auction(orders, max_intermediate=12), auction(orders))
                self.assertEqual(orders, before)

    def test_auction_propagates_interpolation_limit(self):
        orders = [order(1, "BUY", [[0, 4], [4, 0]]),
                  order(2, "SELL", [[3, 1]])]
        with self.assertRaisesRegex(ValueError, "interpolation numerator"):
            auction(orders, max_intermediate=8)
        self.assertEqual(auction(orders, max_intermediate=12), auction(orders))

    def test_invalid_intermediate_limits_reject_even_empty_execution(self):
        for ceiling in (-1, True, 1.5, "12"):
            with self.subTest(ceiling=ceiling):
                with self.assertRaises(ValueError):
                    evaluate([], 0, max_intermediate=ceiling)
                with self.assertRaises(ValueError):
                    buy_bound([[0, 1], [1, 0]], 1, max_intermediate=ceiling)
                with self.assertRaises(ValueError):
                    auction([], max_intermediate=ceiling)
        self.assertEqual(evaluate([], 0, max_intermediate=0), 0)
        self.assertEqual(buy_bound([[0, 1], [1, 0]], 1, max_intermediate=0), 0)
        self.assertEqual(auction([], max_intermediate=0), auction([]))

    def test_duplicate_and_bad_order_rejection(self):
        first = order(1, "BUY", [[1, 2], [2, 0]])
        duplicate_account = order(2, "BUY", [[1, 2], [2, 0]], first["account"])
        invalid_lists = [[first, copy.deepcopy(first)], [first, duplicate_account],
                         [dict(first, revision_filled=-1)],
                         [dict(first, revision_filled=3)],
                         [dict(first, revision_filled=True)],
                         [dict(first, account="ab")],
                         [dict(first, account="AB" * 32)],
                         [dict(first, order_id="gg" * 32)]]
        for orders in invalid_lists:
            with self.subTest(orders=orders), self.assertRaises(ValueError):
                auction(orders)
        # The V1 key permits the same account once on each side.
        result = auction([first, order(2, "SELL", [[1, 2]], first["account"])])
        self.assertEqual(result["volume"], 2)

    def test_limit_order_bound_is_exact_and_scaled(self):
        for price, quantity, tick_atoms in itertools.product(range(5), range(1, 5),
                                                           (1, 3, 1000)):
            self.assertEqual(buy_bound([[price, quantity], [price + 1, 0]], tick_atoms),
                             price * quantity * tick_atoms)
        with self.assertRaises(ValueError):
            buy_bound([[1, 2], [2, 0]], 0)

    def test_persistent_bound_exhaustive_small_curves_and_fill_sequences(self):
        curve_count = transition_count = 0
        for count in range(2, 5):
            for prices in itertools.combinations(range(5), count):
                for ascending in itertools.combinations_with_replacement(range(4), count):
                    quantities = ascending[::-1]
                    if quantities[0] == 0 or quantities[-1] != 0:
                        continue
                    points = [list(pair) for pair in zip(prices, quantities)]
                    validate_curve("BUY", points, 4, 3, 4)
                    curve_count += 1
                    for scale in (1, 3):
                        bound = buy_bound(points, scale)
                        # Every possible integer price is admitted here: a
                        # counterparty can contribute it as a breakpoint.
                        # At each cumulative fill, retain ALL reachable spend
                        # values, covering every nonzero partial-fill sequence.
                        reachable = {0: {0}}
                        for filled in range(quantities[0]):
                            for spent in tuple(reachable.get(filled, ())):
                                for price in range(prices[-1] + 1):
                                    residual = max(0, evaluate(points, price) - filled)
                                    for take in range(1, residual + 1):
                                        transition_count += 1
                                        total = spent + take * price * scale
                                        self.assertLessEqual(total, bound,
                                                             (points, filled, spent,
                                                              price, take, scale))
                                        reachable.setdefault(filled + take, set()).add(total)
                        self.assertIn(quantities[0], reachable)
        self.assertEqual(curve_count, 215)
        self.assertGreater(transition_count, 1000)

    def test_single_auction_notional_bound_is_unsafe(self):
        points = [[0, 4], [4, 0]]
        single_auction_bound = max(p * evaluate(points, p) for p in range(5))
        buyer = order(1, "BUY", points)
        spent = 0
        for number, price in enumerate((3, 2, 1, 0), start=2):
            result = auction([buyer, order(number, "SELL", [[price, 1]])])
            self.assertEqual(result["price"], price)
            self.assertEqual(result["fills"][buyer["order_id"]], 1)
            buyer["revision_filled"] += 1
            spent += result["price"]
        self.assertEqual(single_auction_bound, 4)
        self.assertEqual(spent, 6)
        self.assertGreater(spent, single_auction_bound)
        self.assertLessEqual(spent, buy_bound(points, 1))


if __name__ == "__main__":
    unittest.main()
