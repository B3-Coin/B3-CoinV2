#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Preserve the endpoint's typed reconciliation observation in an engine-off client.

Only generated regtest operators are used. The bounded relay changes public
runtime status, never certificate/state bytes. No client action is submitted.
"""

import json
from pathlib import Path

from feature_flowmesh_latency import FlowMeshLatencyTest
from test_framework.util import assert_equal, assert_raises_rpc_error


class FlowMeshReconciliationStatusTest(FlowMeshLatencyTest):
    def set_test_params(self):
        super().set_test_params()

    def run_test(self):
        self.options.nocleanup = True
        market, _ = self.bootstrap_latency_market()
        self.start_ordinary_client()
        baseline = self.client.getflowmeshmarketdata(market)
        head = baseline['snapshot']['last_microblock_hash']
        saved = self.client.listflowmeshactions(market)
        txcount = self.client.getwalletinfo()['txcount']
        observations = []
        native_status = []

        for full_snapshot in (False, True):
            for case, flag in (('reconciling', True), ('ready', False), ('legacy-missing', None),
                               ('malformed', 'true')):
                seen = []

                def mutate(method, body):
                    if method not in ('updates', 'snapshot'):
                        return body
                    value = json.loads(body)
                    if not value.get('ok'):
                        return body
                    result = value['result']
                    native_status.append((result['status'].get('chain_reconciling'),
                                          result['reported_data']['snapshot']['chain_reconciling']))
                    status = result['status']
                    if not full_snapshot or method == 'snapshot':
                        status['paused'] = case != 'ready'
                        status['error'] = '' if case == 'ready' else 'FlowMesh service is not active at the current B3 tip'
                        if flag is None:
                            status.pop('chain_reconciling', None)
                        else:
                            status['chain_reconciling'] = flag
                    if full_snapshot and method == 'updates':
                        result['gap'] = True
                    seen.append(method)
                    return json.dumps(value, separators=(',', ':')).encode()

                for relay in self.tls_relays:
                    relay.configure(reply_mutation=mutate)
                try:
                    if case == 'malformed':
                        assert_raises_rpc_error(-1, 'Expected boolean: chain_reconciling',
                                                self.client.getflowmeshmarketdata, market,
                                                {'known_head': head})
                    else:
                        data = self.client.getflowmeshmarketdata(market, {'known_head': head})
                        assert_equal(data['snapshot']['chain_reconciling'], flag is True)
                        assert_equal(data['snapshot']['paused'], case != 'ready')
                        assert_equal(data['snapshot']['error'] != '', case != 'ready')
                        assert_equal(data['unchanged'], True)
                        for field in ('last_microblock_hash', 'state_root', 'next_microblock_sequence',
                                      'epoch', 'active_seats', 'quorum_required'):
                            assert_equal(data['snapshot'][field], baseline['snapshot'][field])
                        assert_equal(data['verification']['certificate_verified'], True)
                        assert_equal(data['verification']['account_state_verified'], True)
                        assert_equal(data['verification']['execution_result_verified'], False)
                    if full_snapshot:
                        assert 'snapshot' in seen
                    assert seen
                    observations.append({'case': case, 'full_snapshot': full_snapshot, 'methods': seen})
                finally:
                    for relay in self.tls_relays:
                        relay.configure()

        # Check the actual server serializer too, before our mutation above.
        assert native_status
        assert all(type(flag) is bool and flag == reported for flag, reported in native_status), native_status
        assert_equal(self.client.listflowmeshactions(market), saved)
        assert_equal(self.client.getwalletinfo()['txcount'], txcount)
        assert not any(row['method'] == 'submit' for relay in self.tls_relays for row in relay.snapshot()['requests'])
        self.assert_engine_off()
        Path(self.options.tmpdir, 'reconciliation-status-report.json').write_text(json.dumps({
            'observations': observations, 'native_typed_status_preserved': True,
            'client_actions_submitted': 0, 'certified_head_unchanged': head,
            'scope': 'public runtime-status reporting only; no reconciliation latency claim'}, indent=2))


if __name__ == '__main__':
    FlowMeshReconciliationStatusTest(__file__).main()
