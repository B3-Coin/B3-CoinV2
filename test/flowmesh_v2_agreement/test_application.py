"""Agreement/application boundary tests using unchanged accounting fixtures."""
from copy import deepcopy
import hashlib
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fm_application import (Application, InvalidValue, NeedData, anchor_chain,
                            canonical, digest, initial_anchor, make_anchor,
                            normalize_batch, value_id)
from model import Model, action, action_id, order_id
from test_model import (BUYER, SELLER, COIN, USD_A, SUBACCOUNT, RISK_CONFIG,
                        Fixture, limit_curve)


class ApplicationTests(unittest.TestCase):
    def setUp(self):
        self.fixture = Fixture()
        self.app = Application(self.fixture.model.snapshot())
        self.evidence = anchor_chain()
        self.parent = initial_anchor()
        self.anchors = {header["height"]: header for header in self.evidence.values()}
        self.instance = {"profile": "flowmesh-v2-single-sequence-pbft-test/1",
                         "domain": "synthetic-flowmesh-agreement-not-a-network",
                         "configuration": "1" * 64, "epoch": 0,
                         "ordered_set": "2" * 64, "sequence": 0,
                         "parent": "3" * 64}

    def build(self, batch=None, height=855500):
        return self.app.build(self.instance, self.anchors[height], batch or {})

    def validate(self, body, cache=None):
        return self.app.validate(body, self.instance, self.parent,
                                 self.evidence if cache is None else cache)

    def commit(self, batch):
        body = self.build(batch)
        old = self.app.snapshot
        snapshot = self.validate(body)
        self.assertEqual(old, self.app.snapshot)
        self.app.apply_snapshot(snapshot)
        self.parent = body["anchor"]
        self.instance = dict(self.instance, sequence=self.instance["sequence"] + 1,
                             parent=value_id(body))
        return Model.restore(self.app.snapshot)

    def test_strict_codec_and_versioned_value_identity(self):
        self.assertEqual(canonical({"z": "é", "a": True}), b'{"a":true,"z":"\\u00e9"}')
        self.assertNotEqual(canonical(True), canonical(1))
        body = self.build()
        self.assertEqual(value_id(body), hashlib.sha256(
            canonical(["TEST/V2/VALUE/1", body])).hexdigest())
        self.assertEqual(digest("tag", {}), digest("tag", {}))
        for invalid in (1.0, {1: "value"}, (1, 2), b"bytes"):
            with self.subTest(invalid=invalid), self.assertRaises(InvalidValue):
                canonical(invalid)

    def test_preview_build_validate_are_nonmutating_until_install(self):
        batch = {"deposits": [self.fixture.fact(BUYER, USD_A, 100)]}
        original = self.app.snapshot
        root = self.app.root
        preview = self.app.preview(batch)
        body = self.build(batch)
        self.assertEqual(set(body), {"instance", "anchor", "batch", "result"})
        self.assertEqual(self.validate(body), preview)
        self.assertEqual(self.app.snapshot, original)
        self.assertEqual(self.app.root, root)
        self.assertEqual(self.fixture.model.snapshot(), original)
        self.app.apply_snapshot(preview)
        self.assertEqual(self.app.root, body["result"])
        self.assertEqual(self.app.root, Model.restore(preview).digest())

    def test_caller_mutation_cannot_change_built_body_or_snapshot(self):
        batch = {"deposits": [self.fixture.fact(BUYER, USD_A, 100)]}
        anchor = deepcopy(self.anchors[855500])
        instance = deepcopy(self.instance)
        body = self.app.build(instance, anchor, batch)
        preserved = deepcopy(body)
        batch["deposits"][0]["amount"] = 900
        anchor["height"] = 999
        instance["sequence"] = 900
        self.assertEqual(body, preserved)
        self.validate(body)

    def test_wrong_result_root_and_wrong_instance_reject_without_effect(self):
        body = self.build({"deposits": [self.fixture.fact(BUYER, USD_A, 100)]})
        before = self.app.snapshot
        with self.assertRaisesRegex(InvalidValue, "RESULT_ROOT_MISMATCH"):
            self.validate(dict(body, result="0" * 64))
        for instance in (dict(self.instance, sequence=1), dict(self.instance, epoch=False)):
            with self.assertRaisesRegex(InvalidValue, "WRONG_INSTANCE"):
                self.validate(dict(body, instance=instance))
        self.assertEqual(self.app.snapshot, before)

    def test_invalid_snapshot_install_preserves_committed_state(self):
        before = self.app.snapshot
        with self.assertRaises(InvalidValue):
            self.app.apply_snapshot(b"{}")
        self.assertEqual(self.app.snapshot, before)

    def test_normalization_deduplicates_and_sorts_facts_and_actions(self):
        facts = [self.fixture.fact(BUYER, USD_A, 100),
                 self.fixture.fact(SELLER, COIN, 1)]
        actions = [action(BUYER, 0, "OPEN", market=self.fixture.market, side="BUY",
                          curve=limit_curve("BUY", 10, 1)),
                   action(SELLER, 0, "OPEN", market=self.fixture.market, side="SELL",
                          curve=limit_curve("SELL", 10, 1))]
        left = self.build({"deposits": facts, "actions": actions})
        right = self.build({"deposits": list(reversed(facts)) + [deepcopy(facts[0])],
                            "actions": list(reversed(actions)) + [deepcopy(actions[0])]})
        self.assertEqual(left, right)
        self.assertEqual(value_id(left), value_id(right))
        self.assertEqual(set(left["batch"]), {"deposits", "settlements", "actions", "capacities", "risk"})
        self.assertEqual(left["batch"]["deposits"], sorted(facts, key=canonical))
        self.assertEqual(self.validate(left), self.validate(right))
        normalized = normalize_batch({"settlements": [{"x": 2}, {"x": 1}, {"x": 2}]})
        self.assertEqual(normalized["settlements"], [{"x": 1}, {"x": 2}])

    def test_receivers_reject_noncanonical_batches_and_unknown_fields(self):
        body = self.build({"deposits": [self.fixture.fact(BUYER, USD_A, 100)]})
        cases = []
        repeated = deepcopy(body)
        repeated["batch"]["deposits"] *= 2
        cases.append(repeated)
        missing = deepcopy(body)
        del missing["batch"]["actions"]
        cases.append(missing)
        cases.append(dict(body, view=0))
        for malformed in cases:
            with self.subTest(body=malformed), self.assertRaises(InvalidValue):
                self.validate(malformed)
        for batch in ({"fault": None}, {"fault": "after_actions"}, {"unknown": []},
                      {"actions": ()}, {"capacities": None}, {"risk": {"bad": True}}):
            with self.subTest(batch=batch), self.assertRaises(InvalidValue):
                self.app.preview(batch)

    def test_missing_candidate_or_intermediate_anchor_needs_exact_data(self):
        body = self.build(height=855501)
        for missing in (self.anchors[855501]["hash"], self.anchors[855500]["hash"]):
            cache = deepcopy(self.evidence)
            del cache[missing]
            with self.subTest(missing=missing), self.assertRaises(NeedData) as caught:
                self.validate(body, cache)
            self.assertEqual(caught.exception.identity, missing)
        self.validate(body)

    def test_anchor_context_hash_height_and_chain_are_verified(self):
        body = self.build(height=855501)
        for change in ({"context": "mainnet"}, {"hash": "f" * 64},
                       {"height": True}, {"parent": "f" * 64}, {"extra": 1}):
            malformed = dict(body, anchor=dict(body["anchor"], **change))
            with self.subTest(change=change), self.assertRaises(InvalidValue):
                self.validate(malformed)
        bad_cache = deepcopy(self.evidence)
        bad_cache[self.anchors[855500]["hash"]] = self.anchors[855501]
        with self.assertRaises(InvalidValue):
            self.validate(body, bad_cache)
        skipped = make_anchor(855501, self.parent["hash"])
        skipped_body = self.app.build(self.instance, skipped, {})
        with self.assertRaisesRegex(InvalidValue, "BROKEN_ANCHOR_CHAIN"):
            self.validate(skipped_body, dict(self.evidence, **{skipped["hash"]: skipped}))

    def test_anchor_regression_and_equal_height_fork_reject(self):
        parent = self.anchors[855500]
        lower = self.app.build(self.instance, self.parent, {})
        fork = make_anchor(parent["height"], "f" * 64)
        fork_body = self.app.build(self.instance, fork, {})
        for body in (lower, fork_body):
            with self.subTest(body=body), self.assertRaises(InvalidValue):
                self.app.validate(body, self.instance, parent, self.evidence)
        same = self.app.build(self.instance, parent, {})
        self.assertEqual(self.app.validate(same, self.instance, parent, {}), self.app.snapshot)

    def test_alternate_ancestry_cannot_extend_agreed_parent(self):
        fork = make_anchor(855500, "f" * 64)
        descendant = make_anchor(855501, fork["hash"])
        body = self.app.build(self.instance, descendant, {})
        evidence = {fork["hash"]: fork, descendant["hash"]: descendant}
        with self.assertRaisesRegex(InvalidValue, "ANCHOR_NOT_DESCENDANT"):
            self.validate(body, evidence)

    def test_anchor_choice_changes_value_but_not_instance_or_execution(self):
        batch = {"deposits": [self.fixture.fact(BUYER, USD_A, 100)]}
        first, second = self.build(batch, 855500), self.build(batch, 855501)
        self.assertEqual(first["instance"], second["instance"])
        self.assertEqual(first["batch"], second["batch"])
        self.assertEqual(first["result"], second["result"])
        self.assertNotEqual(value_id(first), value_id(second))
        self.assertEqual(self.validate(first), self.validate(second))

    def test_identical_reproposal_and_consecutive_instance_identity(self):
        first = self.build()
        self.assertEqual(first, self.build())
        self.assertEqual(value_id(first), value_id(deepcopy(first)))
        next_instance = dict(self.instance, sequence=1, parent=value_id(first))
        second = self.app.build(next_instance, self.anchors[855500], {})
        self.assertNotEqual(first["instance"], second["instance"])
        self.assertNotEqual(value_id(first), value_id(second))

    def test_imported_facts_are_synthetic_and_not_newer_than_anchor(self):
        for changes in ({"height": 855501}, {"verified": False}, {"unknown": True}):
            fact = self.fixture.fact(BUYER, USD_A, 100, **changes)
            with self.subTest(changes=changes), self.assertRaises(InvalidValue):
                self.build({"deposits": [fact]}, 855500)
        body = self.build({"deposits": [self.fixture.fact(BUYER, USD_A, 100, height=855501)]}, 855501)
        body["anchor"] = self.anchors[855500]
        with self.assertRaisesRegex(InvalidValue, "FACT_BEYOND_ANCHOR"):
            self.validate(body)

    def test_reservation_fill_cancel_and_explicit_transfer_sequence(self):
        f = self.fixture
        self.commit({"deposits": [f.fact(BUYER, USD_A, 100000), f.fact(SELLER, COIN, 2)]})
        buy = action(BUYER, 0, "OPEN", market=f.market, side="BUY",
                     curve=limit_curve("BUY", 20000, 4))
        state = self.commit({"actions": [buy]}).state
        self.assertEqual(state["spot"][BUYER][USD_A], 19996)
        self.assertTrue(state["orders"][order_id(buy)]["active"])
        blocked = action(BUYER, 1, "SPOT_TO_FUTURES", subaccount=SUBACCOUNT,
                         asset=USD_A, amount=20000, risk_config=RISK_CONFIG)
        state = self.commit({"actions": [blocked], "risk": f.risk()}).state
        self.assertEqual(state["outcomes"][action_id(blocked)]["status"], "REJECTED")
        self.assertEqual(state["futures"], {})
        sell = action(SELLER, 0, "OPEN", market=f.market, side="SELL",
                      curve=limit_curve("SELL", 20000, 2))
        state = self.commit({"actions": [sell]}).state
        self.assertEqual(state["spot"][BUYER][COIN], 2)
        self.assertEqual(state["spot"][BUYER][USD_A], 19996)
        self.assertEqual(state["spot"][SELLER][USD_A], 39998)
        cancel = action(BUYER, 2, "CANCEL", order_id=order_id(buy), expected_revision=0)
        state = self.commit({"actions": [cancel]}).state
        self.assertFalse(state["orders"][order_id(buy)]["active"])
        self.assertEqual(state["spot"][BUYER][USD_A], 59998)
        transfer = action(BUYER, 3, "SPOT_TO_FUTURES", subaccount=SUBACCOUNT,
                          asset=USD_A, amount=10000, risk_config=RISK_CONFIG)
        state = self.commit({"actions": [transfer], "risk": f.risk()}).state
        self.assertEqual(state["spot"][BUYER][USD_A], 49998)
        self.assertEqual(state["futures"][SUBACCOUNT][USD_A], 10000)
        back = action(BUYER, 4, "FUTURES_TO_SPOT", subaccount=SUBACCOUNT,
                      asset=USD_A, amount=10000, risk_config=RISK_CONFIG)
        restored = self.commit({"actions": [back], "risk": f.risk(withdrawable=10000)})
        self.assertEqual(restored.state["spot"][BUYER][USD_A], 59998)
        self.assertEqual(restored.state["futures"][SUBACCOUNT][USD_A], 0)
        restored.assert_invariants()


if __name__ == "__main__":
    unittest.main()
