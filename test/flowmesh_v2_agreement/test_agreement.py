"""Bounded fixed-set scenarios; ticks are not latency measurements."""
from copy import deepcopy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fm_application import anchor_chain, initial_anchor, value_id, canonical
from fm_protocol import PROFILE, Invalid, Proofs, message
from fm_simulator import Simulator
from model import Model, action, order_id
from test_model import Fixture, BUYER, SELLER, COIN, USD_A, SUBACCOUNT, RISK_CONFIG, limit_curve


class AgreementTests(unittest.TestCase):
    def fresh(self, n=4, byzantine=(), policy=None):
        f = Fixture()
        return f, Simulator(f.model.snapshot(), n, byzantine=byzantine, policy=policy)

    def checked(self, sim):
        from fm_checker import check
        return check(sim)

    def test_normal_multiple_sequences_same_value_parent_not_proof_subset(self):
        for n in (4, 7):
            with self.subTest(n=n):
                f, s = self.fresh(n)
                values = []
                for seq in range(4):
                    body = s.offer({"deposits": [f.fact(BUYER, USD_A, 10 + seq)]})
                    values.append(value_id(body))
                    s.run(15, stop=lambda x: x.settled(seq + 1))
                    self.assertTrue(s.settled(seq + 1))
                    self.checked(s)
                self.assertEqual(len(set(values)), 4)
                self.assertEqual(len({x.d["snapshot"] for x in s.nodes}), 1)
                for node in s.nodes:
                    self.assertEqual(node.instance["parent"], values[-1])
                    self.assertEqual([r["apply_count"] for r in node.d["records"].values() if r["applied"]], [1] * 4)

    def test_ordinary_proposer_unavailable_progress_next_honest_view(self):
        for n in (4, 7):
            f, s = self.fresh(n, byzantine=(0,))
            s.offer({"deposits": [f.fact(BUYER, USD_A, 25)]})
            s.run(100, stop=lambda x: x.settled())
            self.assertTrue(s.settled(), [node.last_reason for node in s.nodes])
            self.assertTrue(all(s.nodes[i].d["records"][0]["decision"]["prepared"]["proposal"]["payload"]["view"] == 1
                                for i in range(1, n)))
            self.checked(s)

    def test_equivocating_proposer_split_prepares_recover_without_erasure(self):
        f, s = self.fresh(byzantine=(0,))
        first = s.offer({"deposits": [f.fact(BUYER, USD_A, 10)]})
        second = s.nodes[1].application.build(s.nodes[1].instance, initial_anchor(),
                                             {"deposits": [f.fact(SELLER, COIN, 5)]})
        for index, body in ((1, first), (2, second), (3, second)):
            s.nodes[index]._store_body(body)
            s.byzantine_message(message("PROPOSE", 0, s.nodes[index].instance, 0,
                                        value_id(body), new_view=None), [index])
        s.run(8)
        issued_before = {v["auth"] for v in s.authentication.issued}
        self.assertFalse(s.settled())
        self.assertEqual(len({v["payload"]["value"] for v in s.published("PREPARE", 0)}), 2)
        s.run(100, stop=lambda x: x.settled())
        self.assertTrue(s.settled())
        self.assertTrue(issued_before <= {v["auth"] for v in s.authentication.issued})
        self.checked(s)

    def test_local_tips_do_not_change_validity_distinct_candidate_anchors(self):
        f, s = self.fresh()
        headers = {a["height"]: a for a in anchor_chain().values()}
        batch = {"deposits": [f.fact(BUYER, USD_A, 10)]}
        body = s.nodes[0].application.build(s.nodes[0].instance, headers[855500], batch)
        other = s.nodes[0].application.build(s.nodes[0].instance, headers[855501], batch)
        self.assertNotEqual(value_id(body), value_id(other))
        for i, node in enumerate(s.nodes):
            node.local_tip = 855499 + i * 100
        s.offer(body=body)
        s.run(15, stop=lambda x: x.settled())
        self.assertTrue(s.settled())
        self.assertEqual({n.d["anchor"]["height"] for n in s.nodes}, {855500})
        self.checked(s)

    def test_missing_anchor_fetches_exact_evidence_before_voting(self):
        f, s = self.fresh()
        headers = {a["height"]: a for a in anchor_chain().values()}
        body = s.nodes[0].application.build(s.nodes[0].instance, headers[855501], {})
        s.nodes[3].anchors = {initial_anchor()["hash"]: initial_anchor()}
        s.offer(body=body, nodes=(0, 1, 2))
        s.run(20, stop=lambda x: x.settled())
        self.assertTrue(s.settled())
        self.assertTrue(any(e["node"] == 3 and e["event"] == "defer" and "anchor:" in e["reason"] for e in s.trace))
        self.checked(s)

    def test_four_two_two_and_seven_four_three_partitions_stop_then_heal(self):
        for n, groups in ((4, ({0, 1}, {2, 3})), (7, ({0, 1, 2, 3}, {4, 5, 6}))):
            f, s = self.fresh(n)
            s.partitions = groups
            s.offer({"deposits": [f.fact(BUYER, USD_A, 10)]})
            s.run(80)
            self.assertTrue(all(node.d["sequence"] == 0 for node in s.nodes))
            self.assertEqual(self.checked(s)["possible_commit_quorums"], 0)
            s.partitions = None
            s.run(180, stop=lambda x: x.settled())
            self.assertTrue(s.settled(), [node.last_reason for node in s.nodes])
            self.checked(s)

    def test_seven_five_two_progress_requires_five_responsive_seats(self):
        f, s = self.fresh(7)
        s.partitions = ({0, 1, 2, 3, 4}, {5, 6})
        s.offer({"deposits": [f.fact(BUYER, USD_A, 10)]})
        s.run(15, stop=lambda x: x.settled(nodes=range(5)))
        self.assertTrue(s.settled(nodes=range(5)))
        self.assertTrue(all(s.nodes[i].d["sequence"] == 0 for i in (5, 6)))
        self.checked(s)
        s.partitions = None
        s.run(80, stop=lambda x: x.settled())
        self.assertTrue(s.settled())
        self.checked(s)
        # Same peer partition, but one of the five withholds all participation.
        f, blocked = self.fresh(7, byzantine=(4,))
        blocked.partitions = ({0, 1, 2, 3, 4}, {5, 6})
        blocked.offer({"deposits": [f.fact(BUYER, USD_A, 10)]})
        blocked.run(100)
        self.assertTrue(all(node.d["sequence"] == 0 for node in blocked.nodes))
        self.assertEqual(self.checked(blocked)["possible_commit_quorums"], 0)

    def test_sparse_final_operation_does_not_require_next_trade(self):
        f, s = self.fresh()
        body = s.offer({"deposits": [f.fact(BUYER, USD_A, 1)]})
        s.run(30, stop=lambda x: x.settled())
        self.assertTrue(s.settled())
        self.assertEqual({n.d["records"][0]["body"]["result"] for n in s.nodes}, {body["result"]})
        self.checked(s)

    def test_accounting_reservation_partial_fill_cancel_and_cash_transfers(self):
        f, s = self.fresh()
        def commit(batch):
            target = s.nodes[0].d["sequence"] + 1
            s.offer(batch)
            s.run(20, stop=lambda x: x.settled(target))
            self.assertTrue(s.settled(target))
            self.checked(s)
        commit({"deposits": [f.fact(BUYER, USD_A, 100000), f.fact(SELLER, COIN, 20)]})
        buy = action(BUYER, 0, "OPEN", market=f.market, side="BUY", curve=limit_curve("BUY", 10000, 3))
        commit({"actions": [buy]})
        snapshots = [n.d["snapshot"] for n in s.nodes]
        sell = action(SELLER, 0, "OPEN", market=f.market, side="SELL", curve=limit_curve("SELL", 10000, 1))
        proposed = s.nodes[2].application.build(s.nodes[2].instance, initial_anchor(), {"actions": [sell]})
        self.assertEqual(snapshots, [n.d["snapshot"] for n in s.nodes])
        commit({"actions": [sell]})
        self.assertEqual(Model.restore(s.nodes[0].d["snapshot"]).state["orders"][order_id(buy)]["lifetime_filled"], 1)
        commit({"actions": [action(BUYER, 1, "CANCEL", order_id=order_id(buy), expected_revision=0),
                             action(BUYER, 2, "SPOT_TO_FUTURES", subaccount=SUBACCOUNT, asset=USD_A,
                                    amount=100, risk_config=RISK_CONFIG)], "risk": f.risk()})
        commit({"actions": [action(BUYER, 3, "FUTURES_TO_SPOT", subaccount=SUBACCOUNT, asset=USD_A,
                                    amount=30, risk_config=RISK_CONFIG)], "risk": f.risk(withdrawable=30)})
        state = Model.restore(s.nodes[0].d["snapshot"]).state
        self.assertEqual(state["futures"][SUBACCOUNT][USD_A], 70)
        self.assertFalse(state["orders"][order_id(buy)]["active"])
        self.assertEqual(len({n.d["snapshot"] for n in s.nodes}), 1)

    def test_wrong_domain_set_phase_view_and_duplicates_do_not_supply_quorum(self):
        f, s = self.fresh(byzantine=(3,))
        # Isolate invalid vote traffic: a valid client OFFER now normally
        # reaches the idle primary, which would independently certify it.
        s.policy = lambda source, destination, kind, data: None if kind == "OFFER" else 1
        body = s.offer({}, nodes=(1, 2))  # primary has no offered work
        value, instance = value_id(body), s.nodes[1].instance
        for field in ("domain", "set", "config", "parent"):
            wrong = dict(instance, **{field: "f" * 64})
            s.byzantine_message(message("PREPARE", 3, wrong, 0, value))
        for phase in ("PREPARE", "COMMIT", "WRONG_PHASE"):
            signed = s.byzantine_message(message(phase, 3, instance, 1, value))
            for _ in range(5):
                s.send(3, "SIGNED", signed)
        s.send(3, "SIGNED", {"payload": {}, "auth": "forged"})
        s.run(8)
        self.assertTrue(all(n.d["sequence"] == 0 for n in s.nodes))
        self.assertFalse(s.published("PREPARE"))
        self.assertTrue(any(e["event"] == "refused" for e in s.trace))
        self.checked(s)

    def test_equivalent_commit_signer_subsets_do_not_change_parent_or_apply_twice(self):
        f, s = self.fresh()
        s.offer({})
        s.run(10)
        rec = s.nodes[0].d["records"][0]
        cert = deepcopy(rec["decision"])
        commits = [x for x in s.authentication.issued if x["payload"]["phase"] == "COMMIT"]
        self.assertEqual(len(commits), 4)
        cert["commits"] = sorted(commits, key=lambda x: x["payload"]["sender"])[1:]
        original = [n.d["snapshot"] for n in s.nodes]
        s.send(0, "CERT", cert)
        s.run(5)
        self.assertEqual(original, [n.d["snapshot"] for n in s.nodes])
        self.assertTrue(all(n.d["records"][0]["apply_count"] == 1 for n in s.nodes))
        self.checked(s)

    def test_view_exhaustion_retains_every_signed_obligation(self):
        f, s = self.fresh(byzantine=(0,))
        s.offer({})
        node = s.nodes[1]
        # Explicit finite-state boundary test, not a fair-network progress run.
        for v in range(1, PROFILE["limits"]["views"] + 1):
            node.change_view(v)
        self.assertEqual(node.d["halt"], "VIEW_EXHAUSTED")
        self.assertEqual(len(node.d["records"][0]["signed"]), PROFILE["limits"]["views"] - 1)
        node.restart()
        self.assertEqual(node.d["halt"], "VIEW_EXHAUSTED")
        self.checked(s)


if __name__ == "__main__":
    unittest.main()
