"""Bounded validation regressions for generated model snapshots and V1 facts."""

import json
import unittest

from test_model import BUYER, DESTINATION, USD_A, Fixture, ident
from model import Model, ModelError, action, action_id, canonical, load_profile


class SnapshotValidationTests(unittest.TestCase):
    def setUp(self):
        self.fixture = Fixture()
        self.model = self.fixture.model

    def document(self):
        return json.loads(self.model.snapshot())

    def reject(self, document):
        with self.assertRaises(ModelError):
            Model.restore(canonical(document))

    def test_restore_revalidates_deposit_authentication(self):
        self.fixture.fund(BUYER, USD_A, 10)
        document = self.document()
        next(iter(document["state"]["deposits"].values()))["verified"] = False
        self.reject(document)

    def test_restore_revalidates_deposit_identity(self):
        self.fixture.fund(BUYER, USD_A, 10)
        document = self.document()
        deposits = document["state"]["deposits"]
        _, fact = deposits.popitem()
        deposits[ident(9001)] = fact
        self.reject(document)

    def test_restore_revalidates_settlement_against_claim(self):
        self.fixture.fund(BUYER, USD_A, 10)
        request = action(BUYER, 0, "WITHDRAW", asset=USD_A, amount=10,
                         destination=DESTINATION)
        report = self.model.apply_batch(actions=[request], capacities={USD_A: 10})
        receipt = report["outcomes"][action_id(request)]["receipt_id"]
        self.model.apply_batch(settlements=[{
            "receipt_id": receipt, "asset": USD_A, "amount": 10,
            "destination": DESTINATION, "height": 2, "tx_index": 0,
            "event_index": 0, "verified": True,
        }])
        document = self.document()
        document["state"]["settled"][receipt]["fact"]["destination"] = ident(999)
        self.reject(document)

    def test_restore_binds_pending_receipt_to_original_instruction(self):
        self.fixture.fund(BUYER, USD_A, 10)
        request = action(BUYER, 0, "WITHDRAW", asset=USD_A, amount=10,
                         destination=DESTINATION)
        self.model.apply_batch(actions=[request], capacities={USD_A: 10})
        document = self.document()
        pending = document["state"]["pending"]
        _, claim = pending.popitem()
        pending[ident(9002)] = claim
        self.reject(document)

    def test_restore_revalidates_outcome_sequence(self):
        request = action(BUYER, 0, "WITHDRAW", asset=USD_A, amount=10,
                         destination=DESTINATION)
        self.model.apply_batch(actions=[request], capacities={USD_A: 10})
        document = self.document()
        document["state"]["outcomes"][action_id(request)]["sequence"] = 9
        self.reject(document)

    def test_restore_revalidates_record_bounds(self):
        profile = load_profile()
        profile["limits"]["records"] = 2
        fixture = Fixture(profile=profile)
        fixture.fund(BUYER, USD_A, 1)
        fixture.fund(BUYER, USD_A, 1)
        document = json.loads(fixture.model.snapshot())
        third = fixture.fact(BUYER, USD_A, 1)
        document["state"]["deposits"][fixture.model._deposit_identity(third)] = third
        document["state"]["spot"][BUYER][USD_A] += 1
        document["state"]["custody"][USD_A] += 1
        self.reject(document)

    def test_restore_rejects_unknown_custody_asset(self):
        document = self.document()
        document["state"]["custody"][ident(899)] = 100
        self.reject(document)

    def test_restore_revalidates_v1_receipt_against_external_funds(self):
        vault, receipt = ident(800), ident(801)
        self.model.seed_v1(vault, USD_A, 0, 0, 10)
        self.model.settle_v1(vault, USD_A, receipt, 10, BUYER)
        document = self.document()
        document["state"]["v1_receipts"][receipt]["amount"] = 11
        self.reject(document)

    def test_one_deposit_cannot_consume_two_v1_funding_receipts(self):
        for vault, receipt in ((ident(800), ident(801)), (ident(802), ident(803))):
            self.model.seed_v1(vault, USD_A, 0, 0, 10)
            self.model.settle_v1(vault, USD_A, receipt, 10, BUYER)
        deposit = self.fixture.fact(BUYER, USD_A, 10)
        self.model.redeposit_v1(ident(801), deposit)
        document = self.document()
        external = document["state"]["v1_external"]
        external[ident(803)]["remaining"] = 0
        external[ident(803)]["deposit_id"] = external[ident(801)]["deposit_id"]
        self.reject(document)


if __name__ == "__main__":
    unittest.main()
