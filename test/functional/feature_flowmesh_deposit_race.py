#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Real engine-off deposit preparation across a B3 tip change.

Generated five-node fixture only. A transparent test relay advances B3 during
the readiness response; it does not replace a signed action or certificate.
This is a wallet race regression, not a latency or V2 consensus qualification.
"""
import json
from decimal import Decimal
from pathlib import Path
from urllib.parse import quote

from feature_flowmesh_latency import FlowMeshLatencyTest
from test_framework.authproxy import AuthServiceProxy
from test_framework.util import assert_equal, assert_raises_rpc_error


class FlowMeshDepositRaceTest(FlowMeshLatencyTest):
    def set_test_params(self):
        super().set_test_params()

    def run_test(self):
        self.options.nocleanup = True
        market, asset = self.bootstrap_latency_market()
        self.start_ordinary_client()
        funding = self.nodes[0].sendtoaddress(self.client.getnewaddress(), Decimal('3'))
        self.synchronize_mempools()
        self.mine_pos_blocks(1, allow_overshoot=True)
        self.wait_client_b3_sync()
        assert self.client.gettransaction(funding)['confirmations'] > 0
        self.client.getflowmeshbalance(market)  # Prime the certified cache.
        # get_wallet_rpc intentionally shares its parent's HTTP connection.
        # The relay callback needs a separate connection to this same node.
        wallet = AuthServiceProxy(self.client.url + '/wallet/' + quote(self.default_wallet_name, safe=''))
        observations = []
        fired = False
        before_count = self.client.getwalletinfo()['txcount']

        def advance_tip(method, body):
            nonlocal fired
            if method not in ('updates', 'snapshot') or fired:
                return body
            fired = True
            before = self.client.getbestblockhash()
            self.mine_pos_blocks(1, allow_overshoot=True)
            self.wait_client_b3_sync()
            after = self.client.getbestblockhash()
            assert before != after
            observations.append({'case': 'tip_advance_during_readiness', 'before': before, 'after': after})
            return body

        for relay in self.tls_relays:
            relay.configure(reply_mutation=advance_tip)
        try:
            prepared = wallet.flowmeshdeposit(asset, 'B3', Decimal('1'), {'broadcast': False})
        finally:
            for relay in self.tls_relays:
                relay.configure()
        assert fired, 'The regression must cross a real B3 tip during this call'
        assert_equal(prepared['broadcast'], False)
        assert_equal(prepared['amount'], Decimal('1'))
        assert_equal(prepared['market_id'], market)
        assert_equal(self.client.getwalletinfo()['txcount'], before_count)
        assert_equal(self.client.getrawmempool(), [])
        decoded = self.client.decoderawtransaction(prepared['hex'])
        assert_equal(decoded['txid'], prepared['txid'])
        assert self.client.testmempoolaccept([prepared['hex']])[0]['allowed']
        assert_equal(self.client.getrawmempool(), [])
        # Publishing the exact inspected object still uses ordinary validation.
        assert_equal(self.client.sendrawtransaction(prepared['hex']), prepared['txid'])
        self.publish_client_transaction(prepared)
        assert self.client.gettransaction(prepared['txid'])['confirmations'] >= 1

        # Competing-spend negative: invalidate the selected input AFTER signing,
        # during the second readiness response. Current-tip validation must fail
        # before the deposit is committed, even though the tip itself is stable.
        coin = next(c for c in self.client.listunspent() if c['txid'] == prepared['txid'])
        destination = self.client.getnewaddress()
        conflict = self.client.signrawtransactionwithwallet(self.client.createrawtransaction(
            [{'txid': coin['txid'], 'vout': coin['vout']}],
            {destination: coin['amount'] - Decimal('0.001')}))
        assert conflict['complete']
        reads = 0

        def consume_input(method, body):
            nonlocal reads
            if method in ('updates', 'snapshot'):
                reads += 1
                if reads == 2:
                    txid = self.client.sendrawtransaction(conflict['hex'])
                    observations.append({'case': 'input_spent_after_construction', 'txid': txid})
            return body

        for relay in self.tls_relays:
            relay.configure(reply_mutation=consume_input)
        try:
            assert_raises_rpc_error(-26, '', wallet.flowmeshdeposit, asset, 'B3', Decimal('1'), {'broadcast': False})
        finally:
            for relay in self.tls_relays:
                relay.configure()
        assert_equal(reads, 2)
        assert_equal(self.client.getwalletinfo()['txcount'], before_count + 2)  # exact deposit + conflict only
        assert_equal(self.client.listflowmeshactions(market)['actions'], [])

        # Real pause reason remains visible; no synthetic four-seat diagnosis.
        def paused(method, body):
            if method in ('updates', 'snapshot'):
                value = json.loads(body)
                value['result']['status'].update(paused=True, error='FlowMesh service is reconciling the B3 tip')
                return json.dumps(value).encode()
            return body

        for relay in self.tls_relays:
            relay.configure(reply_mutation=paused)
        try:
            assert_raises_rpc_error(-1, 'FlowMesh service is reconciling the B3 tip',
                                    wallet.flowmeshdeposit, asset, 'B3', Decimal('1'), {'broadcast': False})
        finally:
            for relay in self.tls_relays:
                relay.configure()
        self.assert_engine_off()
        Path(self.options.tmpdir, 'deposit-race-report.json').write_text(json.dumps({
            'observations': observations, 'prepared_txid': prepared['txid'],
            'published_exact_bytes': True, 'dry_run_did_not_insert_mempool': True,
            'competing_spend_refused': True, 'paused_reason_preserved': True,
            'scope': 'generated isolated wallet construction; no consensus or latency claim'}, indent=2))


if __name__ == '__main__':
    FlowMeshDepositRaceTest(__file__).main()
