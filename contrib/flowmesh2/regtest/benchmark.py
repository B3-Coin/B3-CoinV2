#!/usr/bin/env python3
"""Measure sequential actual local PBFT fills, with real fsync and proof checks.

The default invocation is the gate (100 measured fills after 20 warmup fills,
abort on the first failure, pass when p95 <= 200 ms). Extra reports, which
never change that rule (task 1.5, PERF-3):

  --samples up to 1000   running p50/p95/p99 checkpoints every 100 fills
  --failures count       a refused or timed-out fill counts as one sample at
                         the 45 s commit deadline (with_failures in the report);
                         the run still fails when any fill failed
  --concurrency N        N clients submit at once, each with its own account
                         and HTTPS client, every round (authenticated path only)
"""
import argparse
import copy
import json
import math
import os
from pathlib import Path
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import stage_trace
import tool_paths
from bench_stats import RunningPercentiles, with_failures
from custody import Q_ASSET, SYNTHETIC_ASSETS, CustodyError, atomic_json, expect, q_units
from local_process import raw_hex, wait_until
from operator_wrapper import header_fields, private_json
from run_roundtrip import Roundtrip

# commit_public's autonomous deadline; a failed fill is counted at this value.
FAILURE_VALUE_MS = 45_000
LOT = 1_000_000  # raw units of one lot of TOKENA, and of its 100-tick Q price


def distribution(samples):
    ordered = sorted(samples)
    return {**{name: ordered[max(0, math.ceil(fraction * len(ordered)) - 1)]
               for name, fraction in (('p50_ms', .5), ('p95_ms', .95), ('p99_ms', .99))},
            'min_ms': ordered[0], 'max_ms': ordered[-1], 'samples': len(samples), 'all_ms': samples}


def start(fixture):
    fixture.build()
    fixture.start_b3()
    fixture.start_evm()
    fixture.bootstrap_operators()
    fixture.deploy_routes()
    fixture.open_operators()


def fund(fixture, deposits):
    """Deposit (route, account, lots) on the actual custody routes and commit the facts."""
    events = []
    for route, account, lots in deposits:
        driver = fixture.routes[route]['driver']
        amount = q_units(str(lots))
        driver.mint(0, amount)
        driver.approve(0, amount)
        receipt = driver.deposit(0, '0x' + raw_hex(account), amount)
        record = driver.expected_deposit(receipt, '0x' + raw_hex(account), amount, driver.accounts[0])
        events.append({'kind': 'deposit', 'route': route, 'transaction_hash': record['transaction_hash'],
                       'account': record['account'], 'amount_raw': record['amount_raw'], 'sender': record['sender']})
    fixture.observe(fixture.mine_b3(), events)
    fixture.commit([], label='benchmark actual custody funding')


def buy(fixture, market, name, seed, nonce, ref_slot):
    return fixture.client_action(name, seed, nonce, 'PLACE', {'market': market['id'], 'side': 'BUY', 'tif': 'IOC',
                                                              'price_ticks': 100, 'lots': 1}, ref_slot=ref_slot)


def verify_final(fixture, maker_account, maker_lots, buyers):
    """Check every ledger, proof and reserve after the fills.

    buyers lists (account, deposited lots of Q, fills). With every fill made
    these are exactly the gate's original checks.
    """
    fills = sum(item[2] for item in buyers)
    alice = fixture.balances(maker_account)
    expect(alice[raw_hex(SYNTHETIC_ASSETS['TOKENA'])] == (10 * LOT, (maker_lots - fills) * LOT),
           'every measured order must consume actual maker inventory')
    for account, deposited, filled in buyers:
        bob = fixture.balances(account)
        expect(bob[raw_hex(SYNTHETIC_ASSETS['TOKENA'])] == (filled * LOT, 0), 'every measured order must really fill')
        expect(bob[raw_hex(Q_ASSET)] == ((deposited - filled) * LOT, 0), 'buyer must pay exact stable quote')
    expect(alice[raw_hex(Q_ASSET)] == (fills * 999_900, 0), 'seller must receive exact net Q proceeds')
    totals = [item['process'].request('totals') for item in fixture.operators]
    # Native Totals verifies each proof; equivalent q3 signer subsets need
    # not have byte-identical aggregate signatures or bitmaps.
    comparable = [{key: value for key, value in item.items() if key != 'certificate'} for item in totals]
    expect(len({json.dumps(value, sort_keys=True) for value in comparable}) == 1,
           'all four full ledgers, certified headers and state roots must agree')
    q_total = next(value for value in totals[0]['assets'] if raw_hex(value['asset']) == raw_hex(Q_ASSET))
    expect(int(q_total['protocol_available']) == fills * 100, 'cumulative seller fees must all be stable')
    for route in ('Q', 'TOKENA'):
        driver = fixture.routes[route]['driver']
        item = next(value for value in totals[0]['assets'] if raw_hex(value['asset']) == raw_hex(SYNTHETIC_ASSETS[route]))
        expect(driver.call_uint(driver.token, 'balanceOf(address)', driver.vault) == int(item['total_liabilities']),
               'benchmark actual reserves must equal full ledger liabilities')
    return fills


def settled_fills(fixture, accounts):
    """After failures: wait until no operator holds a pending action, then read each buyer's verified fills."""
    wait_until(lambda: all(item['https'].request('GET', '/v1/info').get('pending_actions') == 0 for item in fixture.operators),
               60, 'operators still hold pending actions after the measured fills')
    fills = []
    for account in accounts:
        tokens = fixture.balances(account)[raw_hex(SYNTHETIC_ASSETS['TOKENA'])]
        expect(tokens[1] == 0 and tokens[0] % LOT == 0, 'buyer token balance must be whole filled lots')
        fills.append(tokens[0] // LOT)
    return fills


def extra_reports(report, measured, failed, running, failures):
    if running is not None:
        report['running_percentiles'] = running.checkpoints
    if failures == 'count':
        report['with_failures'] = with_failures(measured, len(failed), FAILURE_VALUE_MS)
        report['failed_samples'] = failed
        report['passed'] = not failed
        report['target_met'] = report['target_met'] and not failed


def scope(fixture):
    return ('single Windows laptop/WSL, actual local B3+Anvil+four operators; ' + fixture.agreement +
            (' over authenticated BIP324 TCP and pinned TLS1.3' if getattr(fixture, 'profile_name', '') == 'fm2-regtest-stable-q/3'
             else ' over trusted pipes'))


def run(fixture, samples, warmup, failures='abort'):
    start(fixture)
    seeds = {'alice': os.urandom(32).hex(), 'bob': os.urandom(32).hex()}
    private_json(fixture.evidence / 'private-client-identities.json', seeds)
    count = samples + warmup
    market = fixture.markets['TOKENA_Q']
    maker = fixture.client_action('benchmark-maker', seeds['alice'], 1, 'PLACE',
                                  {'market': market['id'], 'side': 'SELL', 'tif': 'GTC',
                                   'price_ticks': 100, 'lots': count})
    first = buy(fixture, market, 'benchmark-ioc-1', seeds['bob'], 1, 2)
    fund(fixture, (('TOKENA', maker['account'], count + 10), ('Q', first['account'], count + 10)))
    public = getattr(fixture, 'profile_name', '') == 'fm2-regtest-stable-q/3'
    if public:
        fixture.enable_automatic()
    commit = fixture.commit_public if public else fixture.commit
    if not public:
        fixture.observe(fixture.b3[0]['rpc'].anchor(), [])
    commit([maker['frame']], label='benchmark real maker order')
    measured, client_loop, failed = [], [], []
    running = RunningPercentiles(every=100) if samples > 100 else None
    for index in range(count):
        started = time.perf_counter_ns()
        stage_trace.event('sample', index=index, phase='start')
        # The same actual B3 anchor can support many trading slots. Trading
        # must not require a freshly mined B3 block for every order.
        if not public:
            fixture.observe(fixture.b3[0]['rpc'].anchor(), [])
        if index == 0:
            action = first
        else:
            head = fixture.infos()[0]
            action = buy(fixture, market, 'benchmark-ioc-' + str(index + 1), seeds['bob'], index + 1, head['height'])
        try:
            result = commit([action['frame']], label='benchmark warmup' if index < warmup else 'benchmark measured fill')
        except Exception as error:
            if failures != 'count':
                raise
            stage_trace.event('sample', index=index, phase='failed')
            if index >= warmup:
                failed.append({'index': index, 'error': str(error)[:400]})
            continue
        elapsed = (time.perf_counter_ns() - started) / 1_000_000
        stage_trace.event('sample', index=index, phase='end', latency_ms=result['latency_ms'], client_loop_ms=elapsed)
        if index >= warmup:
            measured.append(result['latency_ms'])
            client_loop.append(elapsed)
            if running is not None:
                running.add(result['latency_ms'])
                if len(running.ordered) % running.every == 0:
                    print('benchmark running percentiles: ' + json.dumps(running.snapshot()), file=sys.stderr, flush=True)
    fills = count
    if failures == 'count':
        fills = settled_fills(fixture, [first['account']])[0]
    verify_final(fixture, maker['account'], count, [(first['account'], count + 10, fills)])
    latency = distribution(measured) if measured else None
    report = {'passed': True, 'scope': scope(fixture),
              'latency_scope': 'HTTPS signed submit through certified inclusion and q3 state proof; peer convergence afterward' if public else 'private submit through all-peer certified application',
              'manual_trading_seal_required': False if public else True,
              'all_fills_verified': fills, 'warmup': warmup, 'latency': latency,
              'client_loop_including_signing_and_observer_preparation': distribution(client_loop) if client_loop else None,
              'target_p95_ms': 200, 'target_met': latency is not None and latency['p95_ms'] <= 200,
              'source_finality_wait_and_withdrawal_dispute_excluded_from_trading_latency': True,
              'production_or_internet_latency_qualified': False,
              'final_heads': fixture.infos()}
    extra_reports(report, measured, failed, running, failures)
    if stage_trace.ENABLED:
        import stage_report
        report['stage_breakdown'] = stage_report.analyse(stage_report.load(fixture.evidence / 'stage-trace'), warmup)['stages']
    atomic_json(fixture.evidence / 'benchmark-report.json', report)
    return report


def concurrent_round(fixture, frames):
    """Submit every frame at once, each from its own HTTPS client; time each one like commit_public."""
    live = fixture.operators
    origin = live[0]
    parent_height = min(item['https'].request('GET', '/v1/info')['height'] for item in live)
    barrier = threading.Barrier(len(frames))
    results = [None] * len(frames)

    def client(position, frame):
        https = copy.copy(origin['https'])
        try:
            barrier.wait(timeout=30)
            started = time.monotonic_ns()
            reply = https.request('POST', '/v1/submit', {'frame': frame})
            expect(reply['code'] in ('ADMITTED', 'DUPLICATE'), 'every concurrent public action must enter native admission')
            deadline = time.monotonic() + FAILURE_VALUE_MS / 1000
            while time.monotonic() < deadline:
                reply = https.request('POST', '/v1/submit', {'frame': frame})
                if reply['code'] == 'DUPLICATE' and reply.get('included') and reply.get('outcome'):
                    proof = https.request('GET', '/v1/totals')
                    if proof['certificate'] is not None and header_fields(proof['header'])['height'] > parent_height:
                        results[position] = {'latency_ms': (time.monotonic_ns() - started) / 1_000_000}
                        return
                time.sleep(.002)
            raise CustodyError('concurrent public action did not return a certified included outcome within the deadline')
        except Exception as error:
            results[position] = {'error': str(error)[:400]}

    threads = [threading.Thread(target=client, args=(position, frame), daemon=True) for position, frame in enumerate(frames)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(FAILURE_VALUE_MS / 1000 + 60)
    expect(all(result is not None for result in results), 'every concurrent client must finish within its deadline')
    # Convergence is checked outside every timed window, as in commit_public.
    final = max(item['https'].request('GET', '/v1/info')['height'] for item in live)
    wait_until(lambda: all(item['https'].request('GET', '/v1/info')['height'] >= final for item in live),
               30, 'live operators did not converge after concurrent certified inclusion')
    expect(len({item['https'].request('GET', '/v1/info')['head_hash'] for item in live}) == 1,
           'public consensus must converge every live certified head')
    return results


def run_concurrent(fixture, samples, warmup, concurrency, failures='abort'):
    expect(getattr(fixture, 'profile_name', '') == 'fm2-regtest-stable-q/3', 'concurrency needs the public authenticated path')
    start(fixture)
    names = ['bob' + str(position) for position in range(concurrency)]
    seeds = {'alice': os.urandom(32).hex(), **{name: os.urandom(32).hex() for name in names}}
    private_json(fixture.evidence / 'private-client-identities.json', seeds)
    warm_rounds, rounds = math.ceil(warmup / concurrency), math.ceil(samples / concurrency)
    total_rounds = warm_rounds + rounds
    count = total_rounds * concurrency
    market = fixture.markets['TOKENA_Q']
    maker = fixture.client_action('benchmark-maker', seeds['alice'], 1, 'PLACE',
                                  {'market': market['id'], 'side': 'SELL', 'tif': 'GTC',
                                   'price_ticks': 100, 'lots': count})
    firsts = [buy(fixture, market, 'benchmark-' + name + '-ioc-1', seeds[name], 1, 2) for name in names]
    fund(fixture, [('TOKENA', maker['account'], count + 10)] +
         [('Q', action['account'], total_rounds + 10) for action in firsts])
    fixture.enable_automatic()
    fixture.commit_public([maker['frame']], label='benchmark real maker order')
    measured, failed = [], []
    running = RunningPercentiles(every=100) if samples > 100 else None
    for index in range(total_rounds):
        if index == 0:
            actions = firsts
        else:
            head = fixture.infos()[0]
            actions = [buy(fixture, market, 'benchmark-' + name + '-ioc-' + str(index + 1), seeds[name], index + 1,
                           head['height']) for name in names]
        stage_trace.event('round', index=index, phase='start', clients=concurrency)
        results = concurrent_round(fixture, [action['frame'] for action in actions])
        stage_trace.event('round', index=index, phase='end', results=results)
        for position, result in enumerate(results):
            if 'error' in result:
                if failures != 'count':
                    raise CustodyError('concurrent client ' + names[position] + ' failed: ' + result['error'])
                if index >= warm_rounds:
                    failed.append({'round': index, 'client': names[position], 'error': result['error']})
            elif index >= warm_rounds:
                measured.append(result['latency_ms'])
                if running is not None:
                    running.add(result['latency_ms'])
    accounts = [action['account'] for action in firsts]
    fills = settled_fills(fixture, accounts) if failures == 'count' else [total_rounds] * concurrency
    verified = verify_final(fixture, maker['account'], count,
                            [(account, total_rounds + 10, filled) for account, filled in zip(accounts, fills)])
    latency = distribution(measured) if measured else None
    report = {'passed': True, 'scope': scope(fixture), 'concurrency': concurrency,
              'latency_scope': 'per client: HTTPS signed submit through certified inclusion and q3 state proof, '
                               'all clients submitting at once; peer convergence afterward',
              'manual_trading_seal_required': False, 'rounds': rounds, 'warmup_rounds': warm_rounds,
              'all_fills_verified': verified, 'warmup': warm_rounds * concurrency, 'latency': latency,
              'target_p95_ms': 200, 'target_met': latency is not None and latency['p95_ms'] <= 200,
              'gate_rule_applies_to_default_invocation_only': True,
              'source_finality_wait_and_withdrawal_dispute_excluded_from_trading_latency': True,
              'production_or_internet_latency_qualified': False,
              'final_heads': fixture.infos()}
    extra_reports(report, measured, failed, running, failures)
    atomic_json(fixture.evidence / 'benchmark-report.json', report)
    return report


def parse_arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
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
    parser.add_argument('--concurrency', type=int, default=1, help='clients submitting at once (1..8; extra report)')
    parser.add_argument('--failures', choices=('abort', 'count'), default='abort',
                        help='abort on the first failed fill (the gate), or count it at the 45 s deadline (extra report)')
    args = parser.parse_args(argv)
    tool_paths.resolve(parser, args, 'b3coind', 'b3coin_cli', 'tools', 'solc')
    if not 100 <= args.samples <= 1000 or not 5 <= args.warmup <= 100:
        parser.error('samples must be100..1000 and warmup5..100')
    if not 1 <= args.concurrency <= 8:
        parser.error('concurrency must be 1..8')
    if (args.concurrency > 1 or args.failures == 'count') and args.transport != 'authenticated-tcp':
        parser.error('--concurrency and --failures count need --transport authenticated-tcp')
    return args


def main():
    args = parse_arguments()
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
        if args.concurrency > 1:
            report = run_concurrent(fixture, args.samples, args.warmup, args.concurrency, args.failures)
        else:
            report = run(fixture, args.samples, args.warmup, args.failures)
        print(json.dumps({'passed': report['passed'], 'target_met': report['target_met'],
                          'p95_ms': report['latency']['p95_ms'] if report['latency'] else None,
                          'report': str(fixture.evidence / 'benchmark-report.json')}))
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
