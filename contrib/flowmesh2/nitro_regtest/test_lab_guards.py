#!/usr/bin/env python3
"""Bounded lab safety regressions; the real economic run is separate."""
import json
from pathlib import Path
import subprocess
import tempfile
import types
import unittest
from unittest.mock import Mock, patch

import lab_common as common
from lab_operator import LabOperator, check_custody_roster


class LabGuards(unittest.TestCase):
    def test_owned_b3_test_controls_use_exact_local_cli_and_bounded_operations(self):
        node = {'rpc': types.SimpleNamespace(url='http://127.0.0.1:12345', cookie_file=Path('/private/owned/cookie'), assert_regtest=Mock()),
                'process': Mock(), 'directory': Path('/private/owned/node')}
        node['process'].process.poll.return_value = None
        with patch.object(common.subprocess, 'run', return_value=types.SimpleNamespace(returncode=0, stdout=b'', stderr=b'')) as run:
            for operation in ('invalidateblock', 'reconsiderblock'):
                common.owned_b3_control('/pinned/b3coin-cli', node, operation, 'ab' * 32)
                command = run.call_args.args[0]
                self.assertEqual(command[-2:], [operation, 'ab' * 32])
                self.assertIn('-regtest', command)
                self.assertIn('-rpcconnect=127.0.0.1', command)
                self.assertIn('-datadir=/private/owned/node', command)
                self.assertIn('-rpccookiefile=/private/owned/cookie', command)
            calls = run.call_count
            for operation, block_hash in (('stop', 'ab' * 32), ('reconsiderblock', 'not-a-hash')):
                with self.assertRaises(common.CustodyError):
                    common.owned_b3_control('/pinned/b3coin-cli', node, operation, block_hash)
            self.assertEqual(run.call_count, calls)

    def test_roster_requires_initial_zero_epoch_and_exact_operators(self):
        expected = ['0x' + format(i, '040x') for i in range(1, 5)]
        chain = Mock()
        chain.number.return_value = 0
        chain.call.side_effect = lambda vault, signature, i: '0x' + '00' * 12 + expected[i][2:]
        check_custody_roster(chain, '0x' + 'aa' * 20, expected)
        chain.number.return_value = 1
        with self.assertRaises(common.CustodyError):
            check_custody_roster(chain, '0x' + 'aa' * 20, expected)
        chain.number.return_value = 0
        chain.call.side_effect = lambda vault, signature, i: '0x' + '00' * 12 + ('bb' * 20 if i == 2 else expected[i][2:])
        with self.assertRaises(common.CustodyError):
            check_custody_roster(chain, '0x' + 'aa' * 20, expected)

    def chain(self, height=8, block_hash='0x' + 'ab' * 32):
        chain = object.__new__(common.NitroLabChain)
        chain.identity = Mock()
        chain.read = Mock()
        chain.read.call.side_effect = lambda method, params: ({'number': hex(height)} if params[0] == 'latest'
            else {'hash': block_hash, 'number': params[0]})
        return chain

    def test_checkpoint_requires_real_successor(self):
        chain = self.chain()
        with self.assertRaisesRegex(common.CustodyError, 'successor'):
            chain.check_point({'height': 8, 'hash': '0x' + 'ab' * 32, 'minimum_confirmations': 1})
        chain.check_point({'height': 7, 'hash': '0x' + 'ab' * 32, 'minimum_confirmations': 1})

    def test_checkpoint_policy_cannot_be_zero_or_boolean(self):
        for confirmations in (0, True, 2):
            with self.subTest(confirmations=confirmations), self.assertRaises(common.CustodyError):
                self.chain().check_point({'height': 7, 'hash': '0x' + 'ab' * 32, 'minimum_confirmations': confirmations})

    def test_checkpoint_changed_hash_refuses(self):
        with self.assertRaisesRegex(common.CustodyError, 'changed'):
            self.chain().check_point({'height': 7, 'hash': '0x' + 'cd' * 32, 'minimum_confirmations': 1})

    def test_point_selects_confirmed_predecessor(self):
        point = self.chain().point()
        self.assertEqual(point['height'], 7)
        self.assertEqual(point['minimum_confirmations'], 1)

    def test_proxy_storage_high_bytes_cannot_be_ignored(self):
        chain = object.__new__(common.NitroLabChain)
        chain.code_hash = Mock(side_effect=['0x' + '11' * 32, '0x' + '22' * 32])
        chain.extra_read = Mock(return_value='0x01' + '00' * 11 + '33' * 20)
        with self.assertRaisesRegex(common.CustodyError, 'implementation'):
            chain.bind_usdt0('0x' + '44' * 20, '0x' + '33' * 20,
                {'proxy_runtime_hash': '0x' + '11' * 32, 'implementation_runtime_hash': '0x' + '22' * 32})

    def test_vote_rechecks_current_b3_anchor_before_native_signing(self):
        operator = object.__new__(LabOperator)
        operator.opened = True; operator.pending = {'check': {}, 'observation': {'anchor': {'height': 1, 'hash': 'ab' * 32}}}
        operator.recheck = Mock(); operator.worker = Mock(); operator.b3 = Mock()
        operator.b3.verify_anchor.side_effect = common.CustodyError('canonical B3 anchor removed')
        with self.assertRaisesRegex(common.CustodyError, 'anchor removed'):
            operator.handle('vote', {'header': '00', 'body': '00'})
        operator.worker.request.assert_not_called()

    def test_candidate_rechecks_current_b3_anchor(self):
        operator = object.__new__(LabOperator)
        operator.opened = True; operator.pending = {'check': {}, 'observation': {'anchor': {}}}
        operator.recheck = Mock(); operator.worker = Mock(); operator.b3 = Mock()
        operator.b3.verify_anchor.side_effect = common.CustodyError('canonical B3 anchor removed')
        with self.assertRaises(common.CustodyError):
            operator.handle('candidate', {'actions': []})
        operator.worker.request.assert_not_called()

    def test_caller_cannot_supply_facts_or_verified_flag(self):
        operator = object.__new__(LabOperator)
        with self.assertRaises(common.CustodyError):
            operator.observe({'verified': True, 'facts': [], 'anchor': {}})

    def test_arbitrary_signing_command_is_not_exposed(self):
        operator = object.__new__(LabOperator); operator.opened = True
        with self.assertRaises(common.CustodyError):
            operator.handle('evm_sign', {'seed': '11' * 32, 'digest': '22' * 32})

    def test_ambiguous_transaction_is_fenced_and_chain_bound(self):
        with tempfile.TemporaryDirectory() as temporary:
            chain = object.__new__(common.NitroLabChain)
            chain.identity = Mock(); chain.evidence = Path(temporary); chain.tools = Path('/fixed/tools')
            chain.url = 'http://127.0.0.1:18548'; chain.dev_address = '0x' + '11' * 20; chain.genesis = '0x' + '22' * 32
            chain.fenced = False
            with patch.object(common.subprocess, 'run', return_value=types.SimpleNamespace(returncode=1, stdout='', stderr='refused')) as run:
                with self.assertRaises(common.CustodyError):
                    chain.send(chain.dev_address)
            self.assertTrue(chain.fenced)
            command = run.call_args.args[0]
            self.assertEqual(command[command.index('--chain') + 1], '412346')
            self.assertEqual(len(list(Path(temporary).glob('*.intent.json'))), 1)

    def test_immutable_intent_cannot_be_replaced(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'intent.json'
            common.private_new(path, {'amount': 1})
            common.private_new(path, {'amount': 1})
            with self.assertRaises(common.CustodyError):
                common.private_new(path, {'amount': 2})

    def test_proof_input_rejects_symlink(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'source'; path.write_bytes(b'proof')
            link = Path(temporary) / 'link'; link.symlink_to(path)
            with self.assertRaises(common.CustodyError):
                common.raw_file(link, 32)


if __name__ == '__main__':
    unittest.main()
