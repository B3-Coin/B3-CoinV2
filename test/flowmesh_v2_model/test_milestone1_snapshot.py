"""Bounded synthetic replay-envelope checks; no production persistence claim."""
from copy import deepcopy
import json
import unittest
from unittest.mock import patch

from model import Model, ModelError, action, action_id, canonical, load_profile
from test_model import Fixture, BUYER, SELLER, USD_A, DESTINATION, limit_curve


class ModelReplayEnvelopeTests(unittest.TestCase):
    def test_permuted_and_duplicate_wrappers_have_same_replay_history(self):
        left, right = Fixture(), Fixture()
        facts = [left.fact(BUYER, USD_A, 100), left.fact(SELLER, USD_A, 100)]
        actions = [action(account, 0, "WITHDRAW", asset=USD_A, amount=10,
                          destination=DESTINATION) for account in (BUYER, SELLER)]
        left.model.apply_batch(deposits=facts, actions=actions, capacities={USD_A: 200})
        right.model.apply_batch(deposits=list(reversed(facts)) + [deepcopy(facts[0])],
                                actions=list(reversed(actions)) + [deepcopy(actions[0])],
                                capacities={USD_A: 200})
        self.assertEqual(left.model.snapshot(), right.model.snapshot())
        before = left.model.snapshot()
        stored = deepcopy(left.model.state["outcomes"])
        left.model.apply_batch(deposits=facts, actions=actions)
        self.assertEqual(before, left.model.snapshot())
        restored = Model.restore(before)
        self.assertEqual(stored, restored.state["outcomes"])
        self.assertEqual(before, restored.snapshot())

    def test_legacy_snapshot_refuses_without_silent_recovery_or_rewrite(self):
        f = Fixture()
        f.fund(BUYER, USD_A, 10)
        document = json.loads(f.model.snapshot())
        document.pop("history")
        document["format"] = "TEST-MODEL-SNAPSHOT/1"
        original = canonical(document)
        with self.assertRaisesRegex(ModelError, "LEGACY_SNAPSHOT_REQUIRES_ORIGINAL_REPLAY_INPUTS"):
            Model.restore(original)
        self.assertEqual(original, canonical(document))

    def test_missing_history_does_not_reconstitute_credit(self):
        f = Fixture()
        f.fund(BUYER, USD_A, 10)
        document = json.loads(f.model.snapshot())
        document["history"] = []
        with self.assertRaisesRegex(ModelError, "HISTORY_STATE_MISMATCH"):
            Model.restore(canonical(document))

    def test_changed_authority_or_amount_in_history_is_not_original_retry(self):
        f = Fixture()
        f.fund(BUYER, USD_A, 10)
        document = json.loads(f.model.snapshot())
        for field, value in (("verified", False), ("amount", 11), ("account", SELLER)):
            with self.subTest(field=field):
                changed = deepcopy(document)
                changed["history"][0]["arguments"]["deposits"][0][field] = value
                with self.assertRaises(ModelError):
                    Model.restore(canonical(changed))

    def test_replay_has_no_dynamic_dispatch_or_fault_injection(self):
        f = Fixture()
        f.fund(BUYER, USD_A, 10)
        document = json.loads(f.model.snapshot())
        for variant in ("unknown_operation", "extra_fault_argument", "no_op_record"):
            with self.subTest(variant=variant):
                changed = deepcopy(document)
                if variant == "unknown_operation":
                    changed["history"][0]["operation"] = "_credit"
                elif variant == "extra_fault_argument":
                    changed["history"][0]["arguments"]["fault"] = "after_actions"
                else:
                    changed["history"].append({"operation": "batch", "arguments": {
                        "deposits": [], "settlements": [], "actions": [], "capacities": {}, "risk": {}}})
                with self.assertRaises(ModelError):
                    Model.restore(canonical(changed))

    def test_record_limit_is_atomic_for_state_outcomes_and_history(self):
        profile = load_profile()
        profile["limits"]["records"] = 2
        f = Fixture(profile=profile)
        f.fund(BUYER, USD_A, 10)
        first = action(BUYER, 0, "WITHDRAW", asset=USD_A, amount=1, destination=DESTINATION)
        f.model.apply_batch(actions=[first], capacities={USD_A: 10})
        before = f.model.snapshot()
        second = action(BUYER, 1, "WITHDRAW", asset=USD_A, amount=1, destination=DESTINATION)
        with self.assertRaisesRegex(ModelError, "MODEL_HISTORY_LIMIT"):
            f.model.apply_batch(actions=[second], capacities={USD_A: 10})
        self.assertEqual(f.model.snapshot(), before)
        self.assertNotIn(action_id(second), f.model.state["outcomes"])
        self.assertEqual(Model.restore(before).snapshot(), before)

    def test_byte_limits_fail_before_commit_and_before_restore(self):
        f = Fixture()
        before = f.model.snapshot()
        fact = f.fact(BUYER, USD_A, 10)
        for name, value in (("HISTORY_BYTES_LIMIT", 2), ("SNAPSHOT_BYTES_LIMIT", len(before))):
            with self.subTest(limit=name), patch("model." + name, value):
                with self.assertRaises(ModelError):
                    f.model.apply_batch(deposits=[fact])
                self.assertEqual(f.model.snapshot(), before)
        f.model.apply_batch(deposits=[fact])
        after = f.model.snapshot()
        with patch("model.SNAPSHOT_BYTES_LIMIT", len(after) - 1), self.assertRaises(ModelError):
            Model.restore(after)

    def test_boolean_cannot_replace_integer_in_restored_derived_state(self):
        f = Fixture()
        f.fund(BUYER, USD_A, 1)
        document = json.loads(f.model.snapshot())
        document["state"]["spot"][BUYER][USD_A] = True
        with self.assertRaises(ModelError):
            Model.restore(canonical(document))

    def test_supplied_action_id_cannot_override_canonical_content(self):
        f = Fixture()
        f.fund(BUYER, USD_A, 20)
        original = action(BUYER, 0, "WITHDRAW", asset=USD_A, amount=10, destination=DESTINATION)
        f.model.apply_batch(actions=[original], capacities={USD_A: 20})
        before = f.model.snapshot()
        for params in ({"amount": 11}, {"destination": SELLER}):
            changed = deepcopy(original)
            changed["params"].update(params)
            changed["action_id"] = action_id(original)
            with self.assertRaisesRegex(ModelError, "BAD_ACTION_SHAPE"):
                f.model.apply_batch(actions=[changed], capacities={USD_A: 20})
            self.assertEqual(before, f.model.snapshot())


if __name__ == "__main__":
    unittest.main()
