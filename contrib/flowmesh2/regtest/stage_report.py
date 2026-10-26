#!/usr/bin/env python3
"""Split benchmark.py's measured fills into stages from a stage_trace.py directory.

  stage_report.py TRACE_DIR [--json OUT]

For each measured sample (between the client's sample start and end events)
it takes, on the shared CLOCK_MONOTONIC axis:

  submit_ms        client: the first POST /v1/submit round trip
  staged_ms        earliest operator enqueue of the action, from sample start
  seal_start_ms    earliest seal on any operator, from sample start
  seal_ms          that seal's duration (observation, candidate, PBFT start)
  observe_ms       the source observation inside that seal
  applied_first_ms first operator that applied the slot, from sample start
  applied_last_ms  last of the four operators to apply it
  detect_ms        from the first apply to the end of the first client poll that saw it
  polls            client HTTPS requests in the sample (submit, polls, totals)
  https_ms_mean    mean client HTTPS request duration in the sample
  guard_ms         per operator: time in the source and custody re-checks
                   (validate_pending, verify_retained, validate_proposal;
                   nested calls are counted once)
  guard_calls      per operator: verify_retained calls (each guard makes one)
  dispatches       per operator: worker network_dispatch calls
  dispatch_ms      per operator: time inside those calls

and the per-process call totals (worker commands, RPCs, cast, fsync).
"""
import argparse
import json
import math
from pathlib import Path


GUARDS = ('validate_pending', 'verify_retained', 'validate_proposal')


def covered(intervals):
    """Total length of the union of (start, end) intervals."""
    total, reach = 0, None
    for start, end in sorted(intervals):
        if reach is None or start > reach:
            total += end - start
            reach = end
        elif end > reach:
            total += end - reach
            reach = end
    return total


def load(directory):
    processes = []
    for path in sorted(Path(directory).glob('*.jsonl')):
        events = [json.loads(line) for line in path.read_text(encoding='utf-8').splitlines() if line.strip()]
        role = next((item['role'] for item in events if item['ev'] == 'process'), 'unknown')
        processes.append({'file': path.name, 'role': role, 'events': events})
    return processes


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[max(0, math.ceil(fraction * len(ordered)) - 1)]


def summary(values):
    if not values:
        return None
    return {'n': len(values), 'p50': percentile(values, .5), 'p95': percentile(values, .95),
            'p99': percentile(values, .99), 'mean': sum(values) / len(values), 'max': max(values)}


def windows(client):
    starts, out = {}, []
    for item in client['events']:
        if item['ev'] != 'sample':
            continue
        if item['phase'] == 'start':
            starts[item['index']] = item['t']
        elif item['index'] in starts:
            out.append({'index': item['index'], 'start': starts[item['index']], 'end': item['t'],
                        'latency_ms': item['latency_ms']})
    return out


def first_after(events, name, start, end, key=None):
    found = [item for item in events if item['ev'] == name and start <= item['t'] <= end and (key is None or key(item))]
    return min(found, key=lambda item: item['t']) if found else None


def analyse(processes, warmup):
    clients = [item for item in processes if item['role'] == 'client']
    operators = [item for item in processes if item['role'] == 'operator']
    if len(clients) != 1:
        raise SystemExit('expected exactly one client trace, found ' + str(len(clients)))
    client = clients[0]
    stages = {name: [] for name in ('latency_ms', 'submit_ms', 'staged_ms', 'seal_start_ms', 'seal_ms', 'observe_ms',
                                    'applied_first_ms', 'applied_last_ms', 'detect_ms', 'polls', 'https_ms_mean',
                                    'guard_ms', 'guard_calls', 'dispatches', 'dispatch_ms')}
    for window in windows(client):
        if window['index'] < warmup:
            continue
        start, end = window['start'], window['end']
        ms = lambda t: (t - start) / 1e6
        requests = [item for item in client['events'] if item['ev'] == 'https' and start <= item['t'] <= end]
        submits = [item for item in requests if item['call'] == 'POST submit']
        stages['latency_ms'].append(window['latency_ms'])
        stages['polls'].append(len(requests))
        if requests:
            stages['https_ms_mean'].append(sum(item['dur'] for item in requests) / len(requests) / 1e6)
        if submits:
            stages['submit_ms'].append(submits[0]['dur'] / 1e6)
        staged = [first_after(op['events'], 'enqueue', start, end) for op in operators]
        staged = [item for item in staged if item]
        if staged:
            stages['staged_ms'].append(ms(min(item['t'] for item in staged)))
        seals = [first_after(op['events'], 'seal', start, end) for op in operators]
        seals = [(item, op) for item, op in zip(seals, operators) if item]
        if seals:
            seal, owner = min(seals, key=lambda pair: pair[0]['t'])
            stages['seal_start_ms'].append(ms(seal['t']))
            stages['seal_ms'].append(seal['dur'] / 1e6)
            observe = first_after(owner['events'], 'observe', seal['t'], seal['t'] + seal['dur'])
            if observe:
                stages['observe_ms'].append(observe['dur'] / 1e6)
        for op in operators:
            inside = [item for item in op['events'] if start <= item['t'] <= end]
            guards = [(item['t'], item['t'] + item['dur']) for item in inside if item['ev'] in GUARDS]
            dispatched = [item['dur'] for item in inside if item['ev'] == 'worker' and item.get('cmd') == 'network_dispatch']
            if guards or dispatched:
                stages['guard_ms'].append(covered(guards) / 1e6)
                stages['guard_calls'].append(sum(1 for item in inside if item['ev'] == 'verify_retained'))
                stages['dispatches'].append(len(dispatched))
                stages['dispatch_ms'].append(sum(dispatched) / 1e6)
        applied = [first_after(op['events'], 'applied', start, end) for op in operators]
        applied = [item['t'] for item in applied if item]
        if applied:
            stages['applied_first_ms'].append(ms(min(applied)))
            stages['applied_last_ms'].append(ms(max(applied)))
            seen = [item for item in submits if item.get('included')]
            if seen:
                stages['detect_ms'].append((seen[0]['t'] + seen[0]['dur'] - min(applied)) / 1e6)
    totals = {}
    for process in processes:
        for item in process['events']:
            if item['ev'] == 'totals':
                totals[process['file']] = {'role': process['role'], 'calls': item['calls']}
    return {'stages': {name: summary(values) for name, values in stages.items()}, 'totals': totals}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('trace_dir')
    parser.add_argument('--warmup', type=int, default=20, help='leading samples to skip (benchmark.py --warmup)')
    parser.add_argument('--json')
    args = parser.parse_args()
    report = analyse(load(args.trace_dir), args.warmup)
    for name, value in report['stages'].items():
        if value:
            print(f"{name:18s} n={value['n']:4d} p50={value['p50']:9.2f} p95={value['p95']:9.2f} "
                  f"p99={value['p99']:9.2f} mean={value['mean']:9.2f} max={value['max']:9.2f}")
    for name, value in sorted(report['totals'].items()):
        heavy = sorted(value['calls'].items(), key=lambda pair: -pair[1]['ns'])[:12]
        print(f"-- {name} ({value['role']})")
        for call, numbers in heavy:
            print(f"   {call:48s} count={numbers['count']:8d} total_ms={numbers['ns'] / 1e6:10.1f} "
                  f"mean_ms={numbers['ns'] / numbers['count'] / 1e6:8.3f}")
    if args.json:
        Path(args.json).write_text(json.dumps(report, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
