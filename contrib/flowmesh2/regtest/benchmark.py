#!/usr/bin/env python3
"""Measure sequential actual local PBFT fills, with real fsync and proof checks."""
import argparse
import json
import math
import os
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import stage_trace
import tool_paths
from custody import Q_ASSET, SYNTHETIC_ASSETS, atomic_json, expect, q_units
from local_process import raw_hex
from operator_wrapper import private_json
from run_roundtrip import Roundtrip


def distribution(samples):
    ordered = sorted(samples)
    return {**{name: ordered[max(0, math.ceil(fraction * len(ordered)) - 1)]
               for name, fraction in (('p50_ms', .5), ('p95_ms', .95), ('p99_ms', .99))},
            'min_ms': ordered[0], 'max_ms': ordered[-1], 'samples': len(samples), 'all_ms': samples}


def run(fixture, samples, warmup):
    fixture.build()
    fixture.start_b3()
    fixture.start_evm()
    fixture.bootstrap_operators()
    fixture.deploy_routes()
    fixture.open_operators()
    seeds = {'alice': os.urandom(32).hex(), 'bob': os.urandom(32).hex()}
    private_json(fixture.evidence / 'private-client-identities.json', seeds)
    count = samples + warmup
    market = fixture.markets['TOKENA_Q']
    maker = fixture.client_action('benchmark-maker', seeds['alice'], 1, 'PLACE',
                                  {'market': market['id'], 'side': 'SELL', 'tif': 'GTC',
                                   'price_ticks': 100, 'lots': count})
    first = fixture.client_action('benchmark-ioc-1', seeds['bob'], 1, 'PLACE',
                                  {'market': market['id'], 'side': 'BUY', 'tif': 'IOC',
                                   'price_ticks': 100, 'lots': 1}, ref_slot=2)
    events = []
    for route, account in (('TOKENA', maker['account']), ('Q', first['account'])):
        driver = fixture.routes[route]['driver']
        amount = q_units(str(count + 10))
        driver.mint(0, amount)
        driver.approve(0, amount)
        receipt = driver.deposit(0, '0x' + raw_hex(account), amount)
        record = driver.expected_deposit(receipt, '0x' + raw_hex(account), amount, driver.accounts[0])
        events.append({'kind': 'deposit', 'route': route, 'transaction_hash': record['transaction_hash'],
                       'account': record['account'], 'amount_raw': record['amount_raw'], 'sender': record['sender']})
    fixture.observe(fixture.mine_b3(), events)
    fixture.commit([], label='benchmark actual custody funding')
    public = getattr(fixture, 'profile_name', '') == 'fm2-regtest-stable-q/3'
    if public:
        fixture.enable_automatic()
    commit = fixture.commit_public if public else fixture.commit
    if not public:
        fixture.observe(fixture.b3[0]['rpc'].anchor(), [])
    commit([maker['frame']], label='benchmark real maker order')
    measured, client_loop = [], []
    for index in range(count):
        start = time.perf_counter_ns()
        stage_trace.event('sample', index=index, phase='start')
        # The same actual B3 anchor can support many trading slots. Trading
        # must not require a freshly mined B3 block for every order.
        if not public:
            fixture.observe(fixture.b3[0]['rpc'].anchor(), [])
        if index == 0:
            action = first
        else:
            head = fixture.infos()[0]
            action = fixture.client_action('benchmark-ioc-' + str(index + 1), seeds['bob'], index + 1, 'PLACE',
                                            {'market': market['id'], 'side': 'BUY', 'tif': 'IOC',
                                             'price_ticks': 100, 'lots': 1}, ref_slot=head['height'])
        result = commit([action['frame']], label='benchmark warmup' if index < warmup else 'benchmark measured fill')
        elapsed = (time.perf_counter_ns() - start) / 1_000_000
        stage_trace.event('sample', index=index, phase='end', latency_ms=result['latency_ms'], client_loop_ms=elapsed)
        if index >= warmup:
            measured.append(result['latency_ms'])
            client_loop.append(elapsed)
    alice = fixture.balances(maker['account'])
    bob = fixture.balances(first['account'])
    expect(alice[raw_hex(SYNTHETIC_ASSETS['TOKENA'])] == (10_000_000, 0), 'every measured order must consume actual maker inventory')
    expect(bob[raw_hex(SYNTHETIC_ASSETS['TOKENA'])] == (count * 1_000_000, 0), 'every measured order must really fill')
    expect(alice[raw_hex(Q_ASSET)] == (count * 999_900, 0), 'seller must receive exact net Q proceeds')
    expect(bob[raw_hex(Q_ASSET)] == (10_000_000, 0), 'buyer must pay exact stable quote')
    totals = [item['process'].request('totals') for item in fixture.operators]
    # Native Totals verifies each proof; equivalent q3 signer subsets need
    # not have byte-identical aggregate signatures or bitmaps.
    comparable = [{key: value for key, value in item.items() if key != 'certificate'} for item in totals]
    expect(len({json.dumps(value, sort_keys=True) for value in comparable}) == 1,
           'all four full ledgers, certified headers and state roots must agree')
    q_total = next(value for value in totals[0]['assets'] if raw_hex(value['asset']) == raw_hex(Q_ASSET))
    expect(int(q_total['protocol_available']) == count * 100, 'cumulative seller fees must all be stable')
    for route in ('Q', 'TOKENA'):
        driver = fixture.routes[route]['driver']
        item = next(value for value in totals[0]['assets'] if raw_hex(value['asset']) == raw_hex(SYNTHETIC_ASSETS[route]))
        expect(driver.call_uint(driver.token, 'balanceOf(address)', driver.vault) == int(item['total_liabilities']),
               'benchmark actual reserves must equal full ledger liabilities')
    latency = distribution(measured)
    report = {'passed': True, 'scope': 'single Windows laptop/WSL, actual local B3+Anvil+four operators; ' + fixture.agreement + (' over authenticated BIP324 TCP and pinned TLS1.3' if getattr(fixture, 'profile_name', '') == 'fm2-regtest-stable-q/3' else ' over trusted pipes'),
              'latency_scope': 'HTTPS signed submit through certified inclusion and q3 state proof; peer convergence afterward' if public else 'private submit through all-peer certified application',
              'manual_trading_seal_required': False if public else True,
              'all_fills_verified': count, 'warmup': warmup, 'latency': latency,
              'client_loop_including_signing_and_observer_preparation': distribution(client_loop),
              'target_p95_ms': 200, 'target_met': latency['p95_ms'] <= 200,
              'source_finality_wait_and_withdrawal_dispute_excluded_from_trading_latency': True,
              'production_or_internet_latency_qualified': False,
              'final_heads': fixture.infos()}
    if stage_trace.ENABLED:
        import stage_report
        report['stage_breakdown'] = stage_report.analyse(stage_report.load(fixture.evidence / 'stage-trace'), warmup)['stages']
    atomic_json(fixture.evidence / 'benchmark-report.json', report)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', required=True)
    parser.add_argument('--evidence-dir', required=True)
    parser.add_argument('--agreement', choices=('pbft-fixed-view', 'pbft-view-change'), default='pbft-fixed-view')
    parser.add_argument('--worker', required=True)
    tool_paths.add(parser, 'b3coind', 'b3coin_cli', 'tools', 'solc')
    parser.add_argument('--transport', choices=('private-pipes', 'authenticated-tcp'), default='private-pipes')
    parser.add_argument('--custody', choices=('fixture', 'production'), default='fixture')
    parser.add_argument('--samples', type=int, default=100)
    parser.add_argument('--warmup', type=int, default=20)
    parser.add_argument('--stage-trace', action='store_true',
                        help='write per-stage timestamps to EVIDENCE/stage-trace and add stage_breakdown to the report')
    args = parser.parse_args()
    tool_paths.resolve(parser, args, 'b3coind', 'b3coin_cli', 'tools', 'solc')
    if not 100 <= args.samples <= 1000 or not 5 <= args.warmup <= 100:
        parser.error('samples must be100..1000 and warmup5..100')
    fixture = None
    try:
        if args.transport == 'authenticated-tcp':
            from run_network_roundtrip import NetworkRoundtrip
            fixture = NetworkRoundtrip(args)
        else:
            fixture = Roundtrip(args)
        if args.stage_trace:
            stage_trace.enable(fixture.evidence / 'stage-trace')
        stage_trace.install('client')
        report = run(fixture, args.samples, args.warmup)
        print(json.dumps({'passed': True, 'target_met': report['target_met'],
                          'p95_ms': report['latency']['p95_ms'], 'report': str(fixture.evidence / 'benchmark-report.json')}))
        return 0 if report['target_met'] else 1
    except Exception as error:
        if fixture is not None:
            atomic_json(fixture.evidence / 'benchmark-failure.json', {'passed': False, 'error': str(error)})
        print('local benchmark failed: ' + str(error), file=sys.stderr)
        return 1
    finally:
        if fixture is not None:
            fixture.close()


if __name__ == '__main__':
    raise SystemExit(main())
