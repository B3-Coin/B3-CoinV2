#!/usr/bin/env python3
import copy
from decimal import Decimal
import importlib.util
import pathlib
import sys
import tempfile
import unittest
from unittest.mock import patch

spec=importlib.util.spec_from_file_location('reconcile',pathlib.Path(__file__).with_name('wallet-reconcile.py'))
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)


def observation():
    coin={'bestblock':'bb'*32,'confirmations':12,'value':Decimal('1.000000001'),
          'scriptPubKey':{'hex':'21'+'02'+'11'*32+'ac','type':'pubkey'},'coinbase':False}
    return {'chain_before':{'height':111,'hash':'bb'*32},'chain_after':{'height':111,'hash':'bb'*32},
      'scan_target':{'height':111,'hash':'bb'*32},'scan_success':True,
      'mempool_sequence_before':1,'mempool_sequence_after':1,'wallet_private_keys_enabled':True,
      'records':[{'outpoint':'aa'*32+':0','descriptor_matched':True,
      'wallet_record':{'present':True},'wallet_listed':True,'wallet_locked':False,
      'stake':None,'chain':coin,'mempool_view':copy.deepcopy(coin),
      'scan':{'height':100},'wallet_spendable':True,'wallet_solvable':True}]}


def stake_observation():
    o=observation();row=o['records'][0]
    script='26'+'42335331'+'33'*32+'00007576a914'+'11'*20+'88ac'
    row['stake']={'amount':'333','status':'PENDING','validator_key':'33'*32}
    row['stake_script']=script
    for view in ('chain','mempool_view'):
        row[view].update(value=Decimal('333'),scriptPubKey={'hex':script,'type':'pubkeyhash'})
    return o


class ReconciliationTests(unittest.TestCase):
    def flags(self,o):return m.analyze(o)['records'][0]['flags']
    def test_exact_native_units_and_p2pk(self):
        r=m.analyze(observation());self.assertEqual(r['records'][0]['value']['amount_atoms'],1000000001)
        self.assertEqual(r['records'][0]['value']['asset'],'B3');self.assertFalse(r['full_wallet_recovery_proven'])
        for bad in [0.1,'0.0000000001','-1','NaN']:
            with self.assertRaises(ValueError):m.atoms(bad)
    def test_missing_history_not_zero_recovery(self):
        o=observation();o['records'][0]['wallet_record']['present']=False
        self.assertIn('chain_script_match_missing_wallet_history',self.flags(o))
    def test_pending_spend_separate_from_chain_spent(self):
        o=observation();o['records'][0]['mempool_view']=None
        self.assertIn('pending_mempool_spend',self.flags(o))
        o['mempool_sequence_after']=2;self.assertIn('possible_mempool_spend',self.flags(o))
        o['records'][0]['chain']=None
        self.assertIn('wallet_history_output_not_in_active_utxo_set',self.flags(o))
    def test_unconfirmed_credit(self):
        o=observation();o['records'][0]['chain']=None
        self.assertIn('unconfirmed_output',self.flags(o))
    def test_locks_watchonly_and_solvable_not_authority(self):
        o=observation();o['wallet_private_keys_enabled']=False
        o['records'][0].update(wallet_locked=True,wallet_spendable=False)
        self.assertIn('user_locked',self.flags(o))
        self.assertIn('solvable_is_not_spending_authority',self.flags(o))
        self.assertIn('no_local_private_keys_watch_only_or_external_signer',self.flags(o))
    def test_stake_activation_not_spend_maturity(self):
        o=stake_observation()
        r=m.analyze(o)['records'][0];self.assertEqual(r['value']['amount_atoms'],333000000000)
        self.assertIn('stake_activation_is_not_spend_maturity',r['flags'])
    def test_stake_history_without_current_coin_has_no_current_value(self):
        o=stake_observation();o['records'][0].update(chain=None,mempool_view=None)
        r=m.analyze(o)['records'][0]
        self.assertIsNone(r['value'])
        self.assertIn('stake_history_without_matching_current_coin',r['flags'])
    def test_stake_amount_script_and_validator_must_match(self):
        for mismatch in ('amount','script','validator'):
            with self.subTest(mismatch=mismatch):
                o=stake_observation();row=o['records'][0]
                if mismatch=='amount':row['stake']['amount']='999'
                if mismatch=='script':row['stake_script']+='00'
                if mismatch=='validator':row['stake']['validator_key']='44'*32
                result=m.analyze(o)
                self.assertNotEqual(result['records'][0]['value']['asset'],'B3')
                self.assertEqual(result['status'],'INCONCLUSIVE')
    def test_stake_classification_is_positive_known_bare_owner_subset(self):
        prefix='26'+'42335331'+'33'*32+'000075'
        supported=('21'+'02'+'11'*32+'ac','41'+'04'+'11'*64+'ac',
                   '76a914'+'11'*20+'88ac')
        unsupported=('a914'+'11'*20+'87','0014'+'11'*20,
                     prefix+supported[-1], '51', supported[-1]+'00')
        for owner in (*supported,*unsupported):
            with self.subTest(owner=owner):
                o=stake_observation();row=o['records'][0]
                script=prefix+owner;row['stake_script']=script
                for view in ('chain','mempool_view'):
                    row[view]['scriptPubKey']['hex']=script
                result=m.analyze(o)
                if owner in supported:
                    self.assertEqual(result['records'][0]['value']['asset'],'B3')
                else:
                    self.assertNotEqual(result['records'][0]['value']['asset'],'B3')
                    self.assertEqual(result['status'],'INCONCLUSIVE')
        o=stake_observation();row=o['records'][0]
        row['stake']['amount']='0'
        for view in ('chain','mempool_view'):row[view]['value']=Decimal('0')
        result=m.analyze(o)
        self.assertNotEqual(result['records'][0]['value']['asset'],'B3')
        self.assertEqual(result['status'],'INCONCLUSIVE')
    def test_scan_in_progress_and_recent_history_limit_are_inconclusive(self):
        for field,value in (('wallet_scanning',{'progress':0.5}),('history_limit_reached',True)):
            with self.subTest(field=field):
                o=observation();o[field]=value
                self.assertEqual(m.analyze(o)['status'],'INCONCLUSIVE')
    def test_noncoinbase_does_not_prove_legacy_reward_maturity(self):
        self.assertIn('legacy_coinstake_maturity_not_exposed_by_gettxout',self.flags(observation()))
    def test_inconclusive_exit_is_nonzero(self):
        with tempfile.TemporaryDirectory(prefix='b3-reconcile-unit-') as d:
            o=observation();o['scan_success']=False
            output=pathlib.Path(d)/'report.json'
            argv=['wallet-reconcile','--cli','/tmp/test-cli','--datadir',d,
                  '--wallet','generated','--network','regtest','--output',str(output)]
            with patch.object(sys,'argv',argv),patch.object(m,'collect',return_value=o):
                with self.assertRaises(SystemExit) as error:m.main()
            self.assertEqual(error.exception.code,2)
            self.assertEqual(m.json.loads(output.read_text())['analysis']['status'],'INCONCLUSIVE')
            self.assertEqual(output.stat().st_mode&0o777,0o600)
    def test_malformed_response_or_generic_error_is_private_incomplete(self):
        for malformed in (True,False):
            with self.subTest(malformed=malformed),tempfile.TemporaryDirectory(prefix='b3-reconcile-unit-') as d:
                output=pathlib.Path(d)/'report.json'
                argv=['wallet-reconcile','--cli','/tmp/test-cli','--datadir',d,
                      '--wallet','generated','--network','regtest','--output',str(output)]
                response=observation();response['records']=None
                behavior={'return_value':response} if malformed else {'side_effect':ValueError('private-provider-error-sentinel')}
                with patch.object(sys,'argv',argv),patch.object(m,'collect',**behavior):
                    with self.assertRaises(SystemExit) as error:m.main()
                self.assertEqual(error.exception.code,1)
                contents=output.read_text();result=m.json.loads(contents)
                self.assertEqual(result['status'],'INCOMPLETE')
                self.assertEqual(result['error_type'],'TypeError' if malformed else 'ValueError')
                self.assertNotIn('private-provider-error-sentinel',contents)
                self.assertIn('scan you own',result['warning'])
                self.assertEqual(output.stat().st_mode&0o777,0o600)
    def test_reward_maturity_not_guessed(self):
        o=observation();o['records'][0]['chain']['coinbase']=True
        self.assertIn('reward_maturity_requires_current_chain_rules',self.flags(o))
    def test_reorg_and_intermediate_coin_context(self):
        o=observation();o['chain_after']['hash']='cc'*32
        self.assertEqual(m.analyze(o)['status'],'INCONCLUSIVE')
        o=observation();o['records'][0]['chain']['bestblock']='cc'*32
        self.assertEqual(m.analyze(o)['status'],'INCONCLUSIVE')
    def test_cancelled_scan_not_complete(self):
        o=observation();o['scan_success']=False
        self.assertEqual(m.analyze(o)['status'],'INCONCLUSIVE')
        self.assertIn('scan_incomplete',self.flags(o))
    def test_asset_policy_not_counted_as_native_balance(self):
        o=observation();o['records'][0]['chain']['scriptPubKey']['type']='nonstandard'
        self.assertEqual(m.analyze(o)['records'][0]['value']['asset'],'UNCLASSIFIED')
    def test_wrapped_owner_rpc_type_is_not_bare_native(self):
        owner='76a914'+'11'*20+'88ac'
        # Canonical OWNER-v1 B3A1 payload (48 bytes), and a STAKE payload
        # (38 bytes). Solver returns the owner's pubkeyhash type for both;
        # gettxout retains the full wrapped hex. These are public script
        # vectors, not chain-valid transactions or a recovery fixture.
        asset='30'+'42334131'+'22'*32+'0000000000000001'+'00010001'+'75'+owner
        stake='26'+'42335331'+'33'*32+'0000'+'75'+owner
        for script,value in ((asset,Decimal('0')),(stake,Decimal('333'))):
            with self.subTest(script=script[:10]):
                o=observation()
                for view in ('chain','mempool_view'):
                    o['records'][0][view].update(value=value,
                        scriptPubKey={'hex':script,'type':'pubkeyhash'})
                r=m.analyze(o)['records'][0]
                self.assertEqual(r['value']['asset'],'UNCLASSIFIED')
                self.assertIn('asset_policy_classification_required_before_totalling',r['flags'])
                self.assertEqual(r['scriptPubKey'],script)
    def test_native_classification_requires_exact_bare_script(self):
        forms=(
            ('pubkey','21'+'02'+'11'*32+'ac'),
            ('pubkey','41'+'04'+'11'*64+'ac'),
            ('pubkeyhash','76a914'+'11'*20+'88ac'),
            ('scripthash','a914'+'11'*20+'87'),
            ('witness_v0_keyhash','0014'+'11'*20),
            ('witness_v0_scripthash','0020'+'11'*32),
            ('witness_v1_taproot','5120'+'11'*32),
        )
        for kind,script in forms:
            with self.subTest(kind=kind,length=len(script)//2):
                o=observation()
                o['records'][0]['chain']['scriptPubKey']={'hex':script,'type':kind}
                self.assertEqual(m.analyze(o)['records'][0]['value']['asset'],'B3')
        for kind,script in (
            ('pubkeyhash','76a914'+'11'*19+'88ac'),
            ('pubkey','21'+'02'+'11'*32+'ac00'),
            ('witness_v1_taproot','0020'+'11'*32),
            ('multisig','51'+'21'+'02'+'11'*32+'51ae'),
        ):
            with self.subTest(unclassified=kind,script=script[:10]):
                o=observation()
                o['records'][0]['chain']['scriptPubKey']={'hex':script,'type':kind}
                self.assertEqual(m.analyze(o)['records'][0]['value']['asset'],'UNCLASSIFIED')
    def test_duplicate_or_malformed_outpoint_rejected(self):
        o=observation();o['records'].append(copy.deepcopy(o['records'][0]))
        with self.assertRaises(ValueError):m.analyze(o)
        for row in [{'txid':'xx','vout':0},{'txid':'aa'*32,'vout':-1},{'txid':'aa'*32,'vout':True}]:
            with self.assertRaises(ValueError):m.outpoint(row)
    def test_cli_allowlist_and_string_encoding(self):
        rpc=m.CliRpc('/tmp/public-test-cli','/tmp/disposable-test-wallet','test','regtest',10**12)
        with self.assertRaises(ValueError):rpc('sendtoaddress','unused',1)
        with patch.object(m.subprocess,'run') as run:
            run.return_value.returncode=0;run.return_value.stdout='{}'
            rpc('scantxoutset','start',[{'desc':'raw(51)'}])
            self.assertEqual(run.call_args.args[0][-2:],['-stdin','scantxoutset'])
            self.assertEqual(run.call_args.kwargs['input'],'start\n[{"desc": "raw(51)"}]\n')
            self.assertNotIn('raw(51)',str(run.call_args.args[0]))
            for flag in ('-regtest=0','-signet=0','-testnet=0','-testnet4=0','-chain=regtest'):
                self.assertIn(flag,run.call_args.args[0])
            run.return_value.stdout='{"chain":"main"}'
            with self.assertRaisesRegex(ValueError,'chain differs'):rpc('getblockchaininfo')
            run.return_value.stdout=''
            self.assertIsNone(rpc('gettxout','aa'*32,0,False))
            with self.assertRaises(ValueError):rpc('scantxoutset','start',[])
            with self.assertRaisesRegex(ValueError,'line breaks'):rpc('gettransaction','aa\nbb')

    def test_receive_history_is_not_listunspent(self):
        txid='aa'*32
        calls=[]
        def rpc(method,*params):
            calls.append((method,params))
            if method=='getblockchaininfo':return {'blocks':111,'bestblockhash':'bb'*32}
            if method=='getwalletinfo':return {'private_keys_enabled':True}
            if method=='listdescriptors':
                self.assertEqual(params,(False,))
                return {'descriptors':[]}
            if method in ('listunspent','listlockunspent'):return []
            if method=='listtransactions':return [{'txid':txid,'vout':0,'category':'receive'}]
            if method=='getstakinginfo':return {'stakes':[]}
            if method=='getrawmempool':return {'mempool_sequence':1}
            if method=='scantxoutset':return {'unspents':[],'height':111,'bestblock':'bb'*32,'success':True,'txouts':0}
            if method=='gettransaction':return {'confirmations':12}
            if method=='gettxout':return None
            self.fail('Unexpected RPC '+method)
        result=m.collect(rpc)
        self.assertTrue(result['records'][0]['wallet_record']['present'])
        self.assertFalse(result['records'][0]['wallet_listed'])
        self.assertIn('wallet_history_output_not_in_active_utxo_set',self.flags(result))
        self.assertTrue(all(method in m.READ_METHODS for method,_ in calls))

    def test_oversized_range_fails_before_scan(self):
        calls=[]
        def rpc(method,*params):
            calls.append(method)
            if method=='getblockchaininfo':return {'blocks':111,'bestblockhash':'bb'*32}
            if method=='getwalletinfo':return {}
            if method=='listdescriptors':return {'descriptors':[{'desc':'public-fixture','range':[0,1000]}]}
            self.fail('Unexpected RPC after rejected descriptor range')
        with self.assertRaisesRegex(ValueError,'range exceeds'):m.collect(rpc,max_range=1000)
        self.assertNotIn('scantxoutset',calls)


if __name__=='__main__':unittest.main(verbosity=2)
