#!/usr/bin/env python3
"""Generated-wallet qualification of public descriptor / validated UTXO comparison."""
from decimal import Decimal
import importlib.util
import json
import pathlib
import subprocess
import sys

from test_framework.authproxy import JSONRPCException
from test_framework.descriptors import descsum_create
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

tool_path=pathlib.Path(__file__).resolve().parents[2]/'contrib/wallet-reconcile.py'
spec=importlib.util.spec_from_file_location('reconcile',tool_path)
tool=importlib.util.module_from_spec(spec);spec.loader.exec_module(tool)


class WalletReadOnlyReconcile(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes=1;self.setup_clean_chain=True;self.wallet_names=[]
        # The integrated client has an explicit default-off validator switch.
        # Keep both the flag and the published service-dormancy assertions.
        self.extra_args=[['-connect=0','-dnsseed=0','-fixedseeds=0','-natpmp=0',
                          '-enableflowmeshvalidator=0','-keypool=20','-unsafesqlitesync=0']]

    def skip_test_if_missing_module(self):self.skip_if_no_wallet()

    def run_test(self):
        node=self.nodes[0]
        node.createwallet('generated-owner')
        owner=node.get_wallet_rpc('generated-owner')
        validator=owner.getflowmeshvalidatorinfo()
        assert_equal(validator['service_enabled'],False)
        assert_equal(validator['service_running'],False)
        assert_equal(validator['armed'],False)
        mining=owner.getnewaddress('', 'legacy')
        self.generatetoaddress(node,101,mining)
        target=owner.getnewaddress('', 'legacy')
        pubkey=owner.getaddressinfo(target)['pubkey']
        node.createwallet('generated-watch',disable_private_keys=True,blank=True)
        watch=node.get_wallet_rpc('generated-watch')
        imported=watch.importdescriptors([{'desc':descsum_create('combo('+pubkey+')'),'timestamp':0}])
        assert_equal(imported[0]['success'],True)
        payment=owner.sendtoaddress(target,Decimal('0.001'))
        self.generatetoaddress(node,1,mining)
        # P2PK receives exactly the same generated public key, not an address alias.
        self.generatetodescriptor(node,1,descsum_create('pk('+pubkey+')'))

        def rpc(wallet):
            def call(method,*params):
                assert method in tool.READ_METHODS
                try:return getattr(wallet,method)(*params)
                except JSONRPCException as e:raise tool.RpcError(e.error['code']) from None
            return call

        before=owner.listdescriptors(False)
        first=tool.collect(rpc(owner),max_range=1000,history_limit=300)
        analyzed=tool.analyze(first)
        assert_equal(analyzed['chain_stable'],True)
        assert_equal(analyzed['scan_complete'],True)
        assert any(r['outpoint'].startswith(payment+':') for r in analyzed['records'])
        assert_equal(owner.listdescriptors(False),before)

        watched=tool.analyze(tool.collect(rpc(watch),max_range=1000))
        # Only the watch wallet has the explicit combo descriptor covering P2PK;
        # an owner's pkh descriptor alone is not silently expanded to another script.
        assert any(r['value'] and r['value'].get('policy')=='standard_script' and
                   r['scriptPubKey']=='21'+pubkey+'ac' for r in watched['records'])
        assert any('no_local_private_keys_watch_only_or_external_signer' in r['flags'] for r in watched['records'])
        payment_output=next(u for u in owner.listunspent() if u['txid']==payment and u.get('address')==target)
        assert_equal(payment_output['amount'],Decimal('0.001'))
        chosen={'txid':payment,'vout':payment_output['vout']}
        owner.lockunspent(False,[chosen])
        locked=tool.analyze(tool.collect(rpc(owner),max_range=1000,history_limit=300))
        assert any(r['outpoint']==tool.outpoint(chosen) and 'user_locked' in r['flags'] for r in locked['records'])
        assert_equal(owner.listlockunspent(),[chosen])
        owner.lockunspent(True,[chosen])
        # Explicit test-only spend demonstrates mempool versus validated UTXO state.
        destination=owner.getnewaddress()
        raw=owner.createrawtransaction([chosen],{destination:Decimal('0.0009')})
        signed=owner.signrawtransactionwithwallet(raw)
        assert_equal(signed['complete'],True)
        spent_tx=owner.sendrawtransaction(signed['hex'])
        pending=tool.analyze(tool.collect(rpc(owner),max_range=1000,history_limit=300))
        assert any(r['outpoint']==tool.outpoint(chosen) and 'pending_mempool_spend' in r['flags'] for r in pending['records'])
        self.generatetoaddress(node,1,mining)
        confirmed=tool.analyze(tool.collect(rpc(owner),max_range=1000,history_limit=300))
        assert any(r['outpoint']==tool.outpoint(chosen) and 'wallet_history_output_not_in_active_utxo_set' in r['flags'] for r in confirmed['records'])
        # Reports are test-local public evidence; no descriptor private material/signatures.
        evidence={'first':analyzed,'watch_only':watched,'locked':locked,
                  'pending':pending,'confirmed':confirmed,'test_spend_txid':spent_tx,
                  'tool_requested_no_signing':True,'final_height':node.getblockcount()}
        output=pathlib.Path(self.options.tmpdir)/'reconciliation-public-result.json'
        with output.open('x') as f:json.dump(evidence,f,indent=2)
        output.chmod(0o600)
        before_restart=owner.listdescriptors(False)
        self.restart_node(0)
        if 'generated-owner' not in node.listwallets():node.loadwallet('generated-owner')
        assert_equal(node.get_wallet_rpc('generated-owner').listdescriptors(False),before_restart)
        validator=node.get_wallet_rpc('generated-owner').getflowmeshvalidatorinfo()
        assert_equal(validator['service_enabled'],False)
        assert_equal(validator['service_running'],False)
        assert_equal(validator['armed'],False)
        # Exercise the shipped diagnostic entry point, including real CLI JSON
        # conversion and protected output creation, not only an in-process RPC shim.
        cli=pathlib.Path(self.config['environment']['BUILDDIR'])/'bin'/('b3coin-cli'+self.config['environment']['EXEEXT'])
        cli_report=pathlib.Path(self.options.tmpdir)/'reconciliation-cli-result.json'
        child=subprocess.run([sys.executable,str(tool_path),'--cli',str(cli),
            '--datadir',str(node.datadir_path),'--wallet','generated-owner','--network','regtest',
            '--output',str(cli_report),'--max-range','1000','--deadline-seconds','60'],
            capture_output=True,text=True,timeout=75)
        assert_equal(child.returncode,0)
        assert_equal(cli_report.stat().st_mode & 0o777,0o600)
        cli_result=json.loads(cli_report.read_text())
        assert_equal(cli_result['analysis']['status'],'BOUNDED_COMPARISON')
        assert_equal(cli_result['analysis']['target']['hash'],node.getbestblockhash())
        assert_equal(node.get_wallet_rpc('generated-owner').listdescriptors(False),before_restart)
        self.log.info('Read-only comparisons passed; generated wallet reopened, no tool signing or repair.')


if __name__=='__main__':WalletReadOnlyReconcile(__file__).main()
