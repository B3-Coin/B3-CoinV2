"""Fixed-seed accounting histories checked without model accounting helpers.

Seeds were selected before execution. Every seed executes exactly 40 batches
per replica, including initial funding, with input permutations and periodic
restore. Failure messages retain the seed, batch, and complete candidate.
"""

from collections import defaultdict
from copy import deepcopy
import json
import random
import unittest

from model import Model, action, action_id, order_id
from test_model import (
    ALT, BUYER, COIN, DESTINATION, OTHER_RISK_CONFIG, RISK_CONFIG, SELLER,
    SUBACCOUNT, THIRD, USD_A, USD_B, Fixture, ident, limit_curve,
)


SEEDS = (0xA110, 0xA111, 0xA112, 0xA113)
BATCHES_PER_SEED = 40
ACCOUNTS = (BUYER, SELLER, THIRD)
ASSETS = (USD_A, USD_B, COIN, ALT)


def nonzero(mapping):
    return {key: amount for key, amount in mapping.items() if amount}


class Milestone1RandomizedTests(unittest.TestCase):
    def check_independent_accounting(self, model):
        """Reconstruct both total backing and each user's remaining ownership.

        Settled claims reduce custody and original ownership, but are no longer
        liabilities. Collected-fee counters are history, not additional money.
        This checker calls no Model accounting or invariant helper.
        """
        state = model.state
        custody = defaultdict(int)
        liabilities = defaultdict(int)
        ownership = defaultdict(int)
        owned_available_and_reserved = defaultdict(int)
        earned_fees = defaultdict(int)
        protocol_entitlements = defaultdict(int)

        for fact in state["deposits"].values():
            custody[fact["asset"]] += fact["amount"]
            ownership[fact["account"], fact["asset"]] += fact["amount"]

        for record in state["settled"].values():
            claim = record["claim"]
            custody[claim["asset"]] -= claim["amount"]

        for account, balances in state["spot"].items():
            for asset, amount in balances.items():
                self.assertGreaterEqual(amount, 0)
                liabilities[asset] += amount
                owned_available_and_reserved[account, asset] += amount

        for balances in state["futures"].values():
            for asset, amount in balances.items():
                self.assertGreaterEqual(amount, 0)
                liabilities[asset] += amount

        for instruction_id, encoded in state["instructions"].items():
            if state["outcomes"][instruction_id]["status"] != "ACCEPTED":
                continue
            instruction = json.loads(encoded)
            kind, params = instruction["kind"], instruction["params"]
            if kind in ("SPOT_TO_FUTURES", "FUTURES_TO_SPOT"):
                sign = -1 if kind == "SPOT_TO_FUTURES" else 1
                ownership[instruction["account"], params["asset"]] += sign * params["amount"]

        for order in state["orders"].values():
            market = model.markets[order["market"]]
            account, base, quote = order["account"], market["base"], market["quote"]
            filled_base = order["lifetime_filled"] * market["base_atoms_per_lot"]
            notional = order["notional"]
            # The frozen test profile charges 50 ppm per side. Calculate the
            # cumulative floor directly, independently of Model._fee.
            fee = notional * 50 // 1_000_000
            self.assertEqual(order["charged"], fee)
            earned_fees[quote] += fee
            if order["side"] == "BUY":
                ownership[account, base] += filled_base
                ownership[account, quote] -= notional + fee
                reservation_asset = quote
            else:
                ownership[account, base] -= filled_base
                ownership[account, quote] += notional - fee
                reservation_asset = base
            if order["active"]:
                self.assertGreaterEqual(order["reservation"], 0)
                liabilities[reservation_asset] += order["reservation"]
                owned_available_and_reserved[account, reservation_asset] += order["reservation"]
            else:
                self.assertEqual(order["reservation"], 0)

        for claim in state["pending"].values():
            liabilities[claim["asset"]] += claim["amount"]
        all_claims = list(state["pending"].values())
        all_claims.extend(record["claim"] for record in state["settled"].values())
        for claim in all_claims:
            if claim["source"] == "spot":
                ownership[claim["account"], claim["asset"]] -= claim["amount"]
            else:
                protocol_entitlements[claim["asset"]] += claim["amount"]

        for balances in state["fn"].values():
            for asset, amount in balances.items():
                self.assertGreaterEqual(amount, 0)
                liabilities[asset] += amount
                protocol_entitlements[asset] += amount
        for asset, amount in state["treasury"].items():
            self.assertGreaterEqual(amount, 0)
            liabilities[asset] += amount
            protocol_entitlements[asset] += amount

        self.assertFalse(any(state["fee_pool"].values()))
        self.assertEqual(nonzero(custody), nonzero(state["custody"]), "deposit/payout backing")
        self.assertEqual(nonzero(liabilities), nonzero(custody), "outstanding liabilities")
        self.assertEqual(nonzero(earned_fees), nonzero(protocol_entitlements), "fee ownership")
        for amount in ownership.values():
            self.assertGreaterEqual(amount, 0)
        self.assertEqual(nonzero(ownership), nonzero(owned_available_and_reserved),
                         "per-account deposit/trade/transfer/claim ownership")

    def exercise_seed(self, seed):
        rng = random.Random(seed)
        permutation_rng = random.Random(seed ^ 0xD15EA5E)
        fixture = Fixture()
        replicas = [fixture.model, Fixture().model]
        coverage = defaultdict(int)
        cycle_order = None
        cycle_market = None

        for batch in range(BATCHES_PER_SEED):
            phase = batch % 10
            state = replicas[0].state
            candidate = {"deposits": [], "settlements": [], "actions": [],
                         "capacities": {asset: 5_000_000 for asset in ASSETS}, "risk": {}}
            next_sequence = dict(state["next_sequence"])
            expected_codes = {}

            def add(account, kind, **params):
                sequence = next_sequence.get(account, 0)
                next_sequence[account] = sequence + 1
                instruction = action(account, sequence, kind, **params)
                candidate["actions"].append(instruction)
                return instruction

            def transfer(kind, amount, config=RISK_CONFIG):
                return add(BUYER, kind, subaccount=SUBACCOUNT, asset=USD_A,
                           amount=amount, risk_config=config)

            def opening(account, side, market, price, lots):
                return add(account, "OPEN", market=market, side=side,
                           curve=limit_curve(side, price, lots))

            def active_orders(account=None):
                return sorted((order for order in state["orders"].values()
                               if order["active"] and (account is None or order["account"] == account)),
                              key=lambda order: order["order_id"])

            if batch == 0:
                for account in ACCOUNTS:
                    for asset in ASSETS:
                        amount = 2_000_000 if asset in (USD_A, USD_B) else 100
                        candidate["deposits"].append(fixture.fact(account, asset, amount))
            deposit_asset = rng.choice(ASSETS)
            deposit_amount = rng.randint(100, 2_000) if deposit_asset in (USD_A, USD_B) else rng.randint(1, 5)
            candidate["deposits"].append(fixture.fact(rng.choice(ACCOUNTS), deposit_asset, deposit_amount))
            if batch % 4 == 0:
                candidate["deposits"].append(deepcopy(candidate["deposits"][-1]))

            pending = sorted(state["pending"])
            for receipt in rng.sample(pending, min(len(pending), rng.randint(1, 3))):
                claim = state["pending"][receipt]
                candidate["settlements"].append({
                    "receipt_id": receipt, "asset": claim["asset"], "amount": claim["amount"],
                    "destination": claim["destination"], "height": 10_000 + batch,
                    "tx_index": len(candidate["settlements"]), "event_index": 0, "verified": True,
                })
            if state["settled"] and batch % 5 == 0:
                receipt = rng.choice(sorted(state["settled"]))
                candidate["settlements"].append(deepcopy(state["settled"][receipt]["fact"]))

            if phase == 0:
                cycle_market = rng.choice(fixture.model.market_ids)
                price, lots = rng.randint(15_000, 25_000), rng.randint(7, 10)
                cycle_order = order_id(opening(BUYER, "BUY", cycle_market, price, lots))
                opening(SELLER, "SELL", cycle_market, price, rng.randint(1, 3))
            elif phase == 1:
                order = state["orders"][cycle_order]
                self.assertTrue(order["active"])
                self.assertGreater(order["lifetime_filled"], 0)
                coverage["partial_orders"] += 1
                price = rng.randint(15_000, 25_000)
                add(BUYER, "REPLACE", order_id=cycle_order, expected_revision=order["revision"],
                    curve=limit_curve("BUY", price, rng.randint(5, 8)))
                opening(SELLER, "SELL", cycle_market, price, rng.randint(1, 3))
            elif phase == 2:
                order = state["orders"][cycle_order]
                add(BUYER, "CANCEL", order_id=cycle_order, expected_revision=order["revision"])
            elif phase == 3:
                opening(THIRD, rng.choice(("BUY", "SELL")), rng.choice(fixture.model.market_ids),
                        rng.randint(10_000, 30_000), rng.randint(1, 4))
            elif phase == 5:
                for order in active_orders(THIRD):
                    add(THIRD, "REPLACE", order_id=order["order_id"], expected_revision=order["revision"],
                        curve=limit_curve(order["side"], rng.randint(10_000, 30_000), rng.randint(1, 5)))
            elif phase == 6:
                market = rng.choice(fixture.model.market_ids)
                price, lots = rng.randint(15_000, 25_000), rng.randint(1, 3)
                opening(BUYER, "BUY", market, price, lots)
                opening(SELLER, "SELL", market, price, lots)
            elif phase in (7, 8):
                existing = active_orders()
                if existing and phase == 8:
                    selected = rng.choice(existing)
                    params = {"order_id": selected["order_id"], "expected_revision": selected["revision"]}
                    kind = rng.choice(("REPLACE", "CANCEL"))
                    if kind == "REPLACE":
                        params["curve"] = limit_curve(selected["side"], rng.randint(10_000, 30_000), rng.randint(1, 5))
                    add(selected["account"], kind, **params)
                opening(rng.choice(ACCOUNTS), rng.choice(("BUY", "SELL")),
                        rng.choice(fixture.model.market_ids), rng.randint(10_000, 30_000), rng.randint(1, 5))
            elif phase == 9:
                for order in active_orders():
                    add(order["account"], "CANCEL", order_id=order["order_id"], expected_revision=order["revision"])

            risk_status = {1: "UNKNOWN", 2: "DENY", 7: "NOT_DEFINED"}.get(phase, "PASS")
            if phase != 3:
                candidate["risk"] = fixture.risk(status=risk_status, withdrawable=30 if phase in (6, 8) else 1_000)
                if phase == 4:
                    candidate["risk"][SUBACCOUNT]["enabled"] = False
            if phase in (1, 2, 3, 4, 5, 7):
                for kind in ("SPOT_TO_FUTURES", "FUTURES_TO_SPOT"):
                    instruction = transfer(kind, rng.randint(1, 100), OTHER_RISK_CONFIG if phase == 5 else RISK_CONFIG)
                    expected_codes[action_id(instruction)] = "RISK_NOT_APPROVED"
                coverage["risk_" + ("MISSING" if phase == 3 else "DISABLED" if phase == 4 else
                                   "CONFIG_MISMATCH" if phase == 5 else risk_status)] += 1
            elif phase == 6:
                first, second = transfer("FUTURES_TO_SPOT", 20), transfer("FUTURES_TO_SPOT", 20)
                expected_codes[action_id(first)] = "OK"
                expected_codes[action_id(second)] = "INSUFFICIENT_WITHDRAWABLE_CASH"
            elif phase == 8:
                incoming, outgoing = transfer("SPOT_TO_FUTURES", 500), transfer("FUTURES_TO_SPOT", 31)
                expected_codes[action_id(incoming)] = "OK"
                expected_codes[action_id(outgoing)] = "INSUFFICIENT_WITHDRAWABLE_CASH"
            else:
                instruction = transfer("SPOT_TO_FUTURES" if phase == 0 else "FUTURES_TO_SPOT", 1_000 if phase == 0 else 100)
                expected_codes[action_id(instruction)] = "OK"

            if phase == 3:
                candidate["capacities"] = {asset: 0 for asset in ASSETS}
            add(rng.choice(ACCOUNTS), "WITHDRAW", asset=rng.choice((USD_A, USD_B)),
                amount=rng.randint(1, 2_000), destination=DESTINATION)

            if state["instructions"]:
                chosen_id = rng.choice(sorted(state["instructions"]))
                retry = {**json.loads(state["instructions"][chosen_id]), "authorized": True}
                candidate["actions"].extend((retry, deepcopy(retry)))
                expected_codes[chosen_id] = state["outcomes"][chosen_id]["code"]
                conflict = action(retry["account"], retry["sequence"], "WITHDRAW", asset=USD_A,
                                  amount=1, destination=ident(700_000 + batch))
                candidate["actions"].append(conflict)
                expected_codes[action_id(conflict)] = "CONSUMED_SEQUENCE_CONFLICT"
                coverage["completed_retries"] += 1

            if phase == 3:
                sequence = next_sequence.get(THIRD, 0)
                for amount in (1, 2):
                    conflict = action(THIRD, sequence, "WITHDRAW", asset=USD_A,
                                      amount=amount, destination=DESTINATION)
                    candidate["actions"].append(conflict)
                    expected_codes[action_id(conflict)] = "EQUIVOCATION"
                coverage["unconsumed_equivocation"] += 1
            elif phase == 4:
                gap = action(THIRD, next_sequence.get(THIRD, 0) + 2, "WITHDRAW", asset=USD_A,
                             amount=1, destination=DESTINATION)
                candidate["actions"].append(gap)
                expected_codes[action_id(gap)] = "BAD_SEQUENCE"
                coverage["sequence_gaps"] += 1

            try:
                inputs = [deepcopy(candidate), deepcopy(candidate)]
                for key in ("deposits", "settlements", "actions"):
                    permutation_rng.shuffle(inputs[0][key])
                    inputs[1][key] = list(reversed(inputs[0][key]))
                reports = [replica.apply_batch(**supplied) for replica, supplied in zip(replicas, inputs)]
                self.assertEqual(reports[0], reports[1], "permuted reports")
                self.assertEqual(replicas[0].state, replicas[1].state, "permuted state")
                for replica in replicas:
                    self.check_independent_accounting(replica)
                for instruction_id, code in expected_codes.items():
                    self.assertEqual(reports[0]["outcomes"][instruction_id]["code"], code)
                for instruction in candidate["actions"]:
                    outcome = reports[0]["outcomes"][action_id(instruction)]
                    if outcome["status"] == "ACCEPTED":
                        coverage["accepted_" + instruction["kind"]] += 1
                coverage["settlement_inputs"] += len(candidate["settlements"])
                if (batch + 1) % 10 == 0:
                    restored = [Model.restore(replica.snapshot()) for replica in replicas]
                    for before, after in zip(replicas, restored):
                        self.assertEqual(before.state, after.state, "restore state")
                        self.check_independent_accounting(after)
                    replicas = restored
                    coverage["restore_boundaries"] += 1
            except Exception as error:
                raise AssertionError(
                    f"seed={seed:#x}, batch={batch + 1}/{BATCHES_PER_SEED}, "
                    f"candidate={json.dumps(candidate, sort_keys=True)}"
                ) from error

        for kind in ("OPEN", "REPLACE", "CANCEL", "WITHDRAW", "SPOT_TO_FUTURES", "FUTURES_TO_SPOT"):
            self.assertGreater(coverage["accepted_" + kind], 0, (seed, dict(coverage)))
        self.assertEqual(coverage["partial_orders"], 4)
        self.assertEqual(coverage["completed_retries"], 39)
        self.assertEqual(coverage["unconsumed_equivocation"], 4)
        self.assertEqual(coverage["sequence_gaps"], 4)
        self.assertEqual(coverage["restore_boundaries"], 4)
        self.assertGreater(coverage["settlement_inputs"], 0)
        for mode in ("UNKNOWN", "DENY", "NOT_DEFINED", "MISSING", "DISABLED", "CONFIG_MISMATCH"):
            self.assertEqual(coverage["risk_" + mode], 4, (seed, dict(coverage)))

    def test_seed_a110(self):
        self.exercise_seed(SEEDS[0])

    def test_seed_a111(self):
        self.exercise_seed(SEEDS[1])

    def test_seed_a112(self):
        self.exercise_seed(SEEDS[2])

    def test_seed_a113(self):
        self.exercise_seed(SEEDS[3])

    def test_independent_checker_detects_compensated_owner_swap(self):
        fixture = Fixture()
        fixture.model.apply_batch(deposits=[fixture.fact(BUYER, USD_A, 10), fixture.fact(SELLER, USD_A, 10)])
        fixture.model.state["spot"][BUYER][USD_A] -= 1
        fixture.model.state["spot"][SELLER][USD_A] += 1
        with self.assertRaisesRegex(AssertionError, "per-account deposit/trade/transfer/claim ownership"):
            self.check_independent_accounting(fixture.model)


if __name__ == "__main__":
    unittest.main()
