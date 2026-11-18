#!/usr/bin/env python3
"""Only typed issuer restrictions defer custody; identity/source faults fence."""
import copy
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from custody import CustodyError, Q_ASSET
from arbitrum_assets import AssetComplianceBlocked
from custody_runtime import AutomaticCustody


class CustodyComplianceTests(unittest.TestCase):
    def test_typed_compliance_preserves_pending_claim_and_existing_votes(self):
        custody = AutomaticCustody.__new__(AutomaticCustody)
        custody.records = {'claim': {'signatures': {0: 'durable-own-vote', 1: 'verified-peer-vote'}}}
        original = copy.deepcopy(custody.records)
        custody.stats = {}
        custody._step = Mock(side_effect=AssetComplianceBlocked('issuer-blocked'))
        custody.step(10.0, {})
        self.assertEqual(custody.records, original)
        self.assertEqual(custody.issuer_compliance, 'issuer-blocked')
        self.assertEqual(custody.next_step, 10.5)
        self.assertEqual(custody.stats['compliance_deferrals'], 1)

    def test_unrelated_identity_source_or_native_refusal_still_propagates(self):
        for detail in ('proxy implementation changed', 'source checkpoint replaced', 'native proof invalid'):
            custody = AutomaticCustody.__new__(AutomaticCustody)
            custody._step = Mock(side_effect=CustodyError(detail)); custody.stats = {}
            with self.subTest(detail=detail), self.assertRaisesRegex(CustodyError, detail): custody.step(10.0, {})
            self.assertEqual(custody.stats, {})

    def test_recipient_check_runs_after_native_and_source_guards(self):
        calls = []
        custody = AutomaticCustody.__new__(AutomaticCustody)
        custody.operator = SimpleNamespace(_verify_retained=lambda: calls.append('retained'),
            b3=SimpleNamespace(verify_anchor=lambda value: calls.append('anchor')))
        custody.route = SimpleNamespace(config={'regtest_arbitrum_fork': {}},
            verify_contract_identity=lambda: calls.append('identity'))
        authority = {'header': {'anchor': {}}, 'source_checkpoint': {}, 'record': {'recipient': 'ab' * 20}}
        checker = Mock(side_effect=lambda *args, **kw: calls.append('compliance'))
        with patch.dict(sys.modules, {'arbitrum_fork': SimpleNamespace(verify_fork_driver=checker)}), \
                patch('custody_runtime.verify_source_point', side_effect=lambda *args: calls.append('source')):
            custody._guard(authority)
        self.assertEqual(calls, ['retained', 'identity', 'anchor', 'source', 'compliance'])
        checker.assert_called_once_with(custody.route, accounts=['0x' + 'ab' * 20], check_compliance=True)
        self.assertIsNone(custody.issuer_compliance)

    def test_cached_own_authorization_still_requires_current_compliance_before_gossip(self):
        custody = AutomaticCustody.__new__(AutomaticCustody)
        value = {'id': '0x' + '11' * 32, 'account': '0x' + '22' * 32, 'asset': Q_ASSET,
            'amount': '1000000', 'recipient': '0x' + '44' * 20, 'slot': 1, 'stateRoot': '0x' + '55' * 32, 'validUntil': 200}
        custody.seat, custody._matches = 0, Mock(return_value=True)
        custody._guard = Mock(side_effect=AssetComplianceBlocked('issuer-paused'))
        entry = {'withdrawal': value, 'signatures': {0: 'retained-vote'}}
        with self.assertRaises(AssetComplianceBlocked): custody._authorize('claim', entry, value, {}, 100)
        self.assertEqual(entry['signatures'], {0: 'retained-vote'})


if __name__ == '__main__':
    unittest.main()
