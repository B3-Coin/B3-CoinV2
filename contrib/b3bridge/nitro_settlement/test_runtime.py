import copy
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import runtime


def config(role):
    return {'chain': {'id': 412346}, 'parent-chain': {'id': 1337}, 'node': {
        'staker': {'dangerous': {'without-block-validator': False}, 'start-validation-from-staked': False,
                   'enable': role == 'validator', 'strategy': 'MakeNodes'},
        'bold': {'start-validation-from-staked': False},
        'batch-poster': {'post-4844-blobs': False}, 'dangerous': {'disable-blob-reader': True},
        'feed': {'input': {'url': []}},
        'block-validator': {'enable': role == 'validator'}}}


class RuntimeBoundaryTests(unittest.TestCase):
    def test_full_role_allocations_obey_combined_resource_contract(self):
        self.assertEqual(sum(value[0] for value in runtime.ROLES.values()), 12)
        self.assertEqual(sum(value[1] for value in runtime.ROLES.values()), 8)
        self.assertEqual(set(runtime.ROLES), {'geth', 'sequencer', 'poster', 'validator', 'validation_node'})

    def test_configuration_requires_real_validator_and_no_assumed_progress(self):
        runtime.validate_config('validator', config('validator'))
        mutations = [
            lambda c: c['node']['staker']['dangerous'].__setitem__('without-block-validator', True),
            lambda c: c['node']['bold'].__setitem__('start-validation-from-staked', True),
            lambda c: c['node']['staker'].__setitem__('start-validation-from-staked', True),
            lambda c: c['node']['block-validator'].__setitem__('enable', False),
            lambda c: c['node']['staker'].__setitem__('enable', False),
            lambda c: c['node']['feed']['input'].__setitem__('url', ['ws://sequencer:9642']),
        ]
        for mutate in mutations:
            value = config('validator'); mutate(value)
            with self.assertRaises(ValueError):
                runtime.validate_config('validator', value)

    def test_owned_configuration_cannot_be_relabelled_live(self):
        for chain in (1, 42161, 421614):
            value = config('validator'); value['chain']['id'] = chain
            with self.assertRaises(ValueError):
                runtime.validate_config('validator', value)
            value = config('validator'); value['parent-chain']['id'] = chain
            with self.assertRaises(ValueError):
                runtime.validate_config('validator', value)

    def test_l1_availability_reader_cannot_send_or_control(self):
        rpc = runtime.Rpc(18550)
        for method in ('eth_sendTransaction', 'eth_sendRawTransaction', 'evm_increaseTime', 'admin_addPeer', 'engine_forkchoiceUpdatedV3'):
            with self.assertRaises(ValueError):
                rpc.call(method, [])

    def test_duplicate_json_and_symbolic_paths_are_refused(self):
        with self.assertRaises(ValueError): runtime.unique([('a', 1), ('a', 2)])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); (root / 'file').write_text('{}')
            (root / 'alias').symlink_to(root / 'file')
            with self.assertRaises(ValueError): runtime.read_json(root / 'alias')

    def test_docker_always_uses_local_socket_absolute_binary_and_clean_environment(self):
        with patch('runtime.subprocess.run') as run:
            run.return_value.returncode = 0; run.return_value.stdout = '[]'
            runtime.docker('ps')
            self.assertEqual(run.call_args.args[0][:3], ['/usr/bin/docker', '--host', 'unix:///var/run/docker.sock'])
            self.assertEqual(set(run.call_args.kwargs['env']), {'PATH', 'HOME', 'LANG', 'LC_ALL', 'TZ'})

    def test_single_container_caps_refuse_an_oversized_role(self):
        with self.assertRaises(ValueError): runtime.base_args('x', 'x', runtime.NODE, 5, 2)
        with self.assertRaises(ValueError): runtime.base_args('x', 'x', runtime.NODE, 4, 3)


if __name__ == '__main__':
    unittest.main(verbosity=2)
