#!/usr/bin/env python3
"""Controller provenance tests; fixtures do not claim cryptographic acceptance.

Native BLS/SSZ and real maintained-backend adversarial tests cover the crypto
boundary. These controlled child processes exercise warm-cache/live accounting.
"""
import json
from pathlib import Path
import tempfile
import unittest

import run_gloas_peer_light_client as peer
from build_gloas_peer_collector import FORMAT, NIMBUS_COMMIT


class HistoryProvenanceTests(unittest.TestCase):
    def run_control(self, initial_sequence=4, accepted=False, advance=False):
        with tempfile.TemporaryDirectory() as temporary:
            directory=Path(temporary)
            backend=directory/'controlled-availability.py'
            captured=directory/'arguments.json'
            version=dict(format=FORMAT,upstream_commit=NIMBUS_COMMIT,authority='availability-only')
            backend.write_text('#!/usr/bin/env python3\nimport json,sys,time\n'
                'if sys.argv[1:]==["--b3-version"]:\n print('+repr(json.dumps(version))+');sys.exit(0)\n'
                'open('+repr(str(captured))+',"w").write(json.dumps(sys.argv[1:]))\n'
                'print("controlled untrusted availability frame",flush=True)\ntime.sleep(30)\n')
            backend.chmod(0o700)
            row=dict(format='b3-ethereum-sepolia-gloas-verification/1',mainnet_enabled=False,
                arbitrum_authority=False,writes_enabled=False,chain_id=11155111,
                genesis_validators_root=peer.SEPOLIA_GVR,result='READY',accepted=False,
                sequence=initial_sequence,policy_sha256='11'*32,journal_sha256='22'*32,
                beacon_slot=100,fresh_finalized_execution_hash=True,fenced=False)
            native=directory/'controlled-native-diagnostics.py'
            native.write_text('#!/usr/bin/env python3\nimport json,sys\n'
                'row='+repr(row)+'\n'
                'def emit():\n print(json.dumps(row),flush=True)\n'
                'emit()\nfor line in sys.stdin:\n'
                ' row.update(result='+repr('OK' if accepted else 'BAD_RECORD')+',accepted='+repr(accepted)+')\n'
                +(' row.update(sequence=row["sequence"]+1,journal_sha256="33"*32)\n' if accepted else '')
                +(' row["beacon_slot"]+=1\n' if advance else '')
                +' emit()\nrow.update(result="READY",accepted=False)\nemit()\n')
            native.chmod(0o700)
            history=directory/'untrusted-history.jsonl';history.write_text('{}\n')
            bootstrap=directory/'untrusted-bootstrap.json';bootstrap.write_text('{}\n')
            result=peer.run_session(backend,native,directory/'policy.json',
                {'trusted_root':'0x'+'12'*32},directory/'journal',1,19470,
                bootstrap_file=bootstrap,history_file=history)
            arguments=json.loads(captured.read_text())
            self.assertIn('--no-el',arguments)
            self.assertEqual(arguments[-2:],['--history-file',str(history)])
            self.assertFalse(result['mainnet_enabled'])
            self.assertFalse(result['arbitrum_authority'])
            return result

    def test_cache_only_cannot_count_as_live(self):
        result=self.run_control()
        self.assertEqual(result['result'],'SEPOLIA_GLOAS_VERIFIED_REPLAY_ONLY')
        self.assertEqual(result['accepted_live_updates'],0)
        self.assertFalse(result['live_bls_update_verified'])
        self.assertFalse(result['finalized_head_advanced_this_run'])

    def test_committee_only_update_requires_finality_advance(self):
        result=self.run_control(accepted=True)
        self.assertEqual(result['result'],'SEPOLIA_GLOAS_PEER_UPDATE_NO_FINALITY_ADVANCE')
        self.assertEqual(result['accepted_live_updates'],1)
        self.assertFalse(result['live_bls_update_verified'])
        self.assertFalse(result['finalized_head_advanced_this_run'])

    def test_peer_head_advance_qualifies_after_native_acceptance(self):
        result=self.run_control(accepted=True,advance=True)
        self.assertEqual(result['result'],'SEPOLIA_GLOAS_LIVE_UPDATE_VERIFIED')
        self.assertTrue(result['live_bls_update_verified'])
        self.assertTrue(result['finalized_head_advanced_this_run'])

    def test_warm_seed_rejects_bootstrap_only_native_store(self):
        with self.assertRaisesRegex(ValueError,'pre-existing authenticated native history journal'):
            self.run_control(initial_sequence=1)


if __name__=='__main__':unittest.main()
