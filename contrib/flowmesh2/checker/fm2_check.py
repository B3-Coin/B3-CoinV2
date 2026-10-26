#!/usr/bin/env python3
# Copyright (c) 2026 The B3 developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""M3-ENG-05: run seeded batches through the real engine (fm2-trace) and through
the independent model in fm2check, and require byte-for-byte agreement on every
slot: the verdict, the header and its hash, the canonical order, every outcome,
the five state roots and every resting order.

    fm2_check.py --trace build/bin/fm2-trace [--seeds N] [--slots N] [--kinds random,adversarial,dust,caps,slotcap]
                 [--keep DIR] [--skeleton]

--skeleton also recomputes the walking skeleton's goldens that are pinned from
the C++ output (O-87): it takes only the skeleton's committed slot inputs from
`fm2-trace --export-skeleton`, reads the pinned values from the C++ sources and
requires the model to reproduce each of them.

Exit status 0 only at 100% agreement. A refused slot agrees when both sides
refuse it with the same error code and detail; the reasons are counted and
printed.
"""

import argparse
import collections
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from fm2check import batch as batchgen  # noqa: E402
from fm2check import engine  # noqa: E402
from fm2check.codec import (  # noqa: E402
    CODE_NAMES, Action, Market, decode_header, encode_header, encode_outcome, header_hash, order_leaf_bytes,
)

FM2 = os.path.join(HERE, '..', '..', '..', 'src', 'flowmesh2')
PROFILE_JSON = os.path.join(FM2, 'PROFILE_DEX1_REGTEST.json')


def cursor(v):
    return (int(v[0]), int(v[1]))


def anchor(v):
    return (int(v['height']), bytes.fromhex(v['hash']))


def model_run(b, profile_hash):
    """The model's fm2-trace-result/1, built from the batch alone."""
    markets = [Market(bytes.fromhex(m['base']), bytes.fromhex(m['quote']), int(m['lot']), int(m['tick']))
               for m in b['markets']]
    domain = bytes.fromhex(b['domain'])
    head = engine.genesis(domain, profile_hash, int(b['authority_epoch']), bytes.fromhex(b['seat_set_root']),
                          anchor(b['genesis_anchor']), cursor(b['facts_start']), markets)
    out = {'genesis': {'header': encode_header(head['header']).hex(), 'hash': header_hash(head['header']).hex(),
                       'roots': {k: v.hex() for k, v in head['state'].roots().items()}}, 'slots': []}
    for s in b['slots']:
        inputs = {'profile_hash': profile_hash, 'domain': domain, 'anchor': anchor(s['anchor']),
                  'facts_from': cursor(s['facts_from']), 'facts_to': cursor(s['facts_to']),
                  'facts': [bytes.fromhex(x) for x in s['facts']],
                  'originals': [bytes.fromhex(x) for x in s['originals']]}
        try:
            child, canonical, outcomes, roots = engine.apply_slot(head, inputs)
        except engine.Invalid as e:
            out['slots'].append({'ok': False, 'error': e.code, 'detail': e.detail})
            continue
        out['slots'].append({
            'ok': True, 'header': encode_header(child['header']).hex(), 'hash': header_hash(child['header']).hex(),
            'originals': [o.hex() for o in canonical], 'outcomes': [encode_outcome(o).hex() for o in outcomes],
            'roots': {k: v.hex() for k, v in roots.items()},
            'orders': [order_leaf_bytes(o).hex() for _, o in sorted(child['state'].orders.items())],
            'accounts': [leaf.hex() for leaf in child['state'].account_leaves()]})
        child['cert_hash'] = bytes.fromhex(s['cert_hash'])
        head = child
    return out


def compare(real, model):
    """The first disagreement as text, or None."""
    for key in ('header', 'hash', 'roots'):
        if real['genesis'][key] != model['genesis'][key]:
            return 'genesis %s differs' % key
    if len(real['slots']) != len(model['slots']):
        return 'slot counts differ'
    for i, (r, m) in enumerate(zip(real['slots'], model['slots'])):
        if r['ok'] != m['ok']:
            return 'slot %d: engine %s, model %s' % (
                i, 'accepts' if r['ok'] else 'refuses (%s)' % r['detail'],
                'accepts' if m['ok'] else 'refuses (%s)' % m['detail'])
        if not r['ok']:
            # A refusal agrees only for the same reason: the same error code and detail.
            if r['error'] != m['error'] or r['detail'] != m['detail']:
                return 'slot %d: engine refuses (%s %s), model refuses (%s)' % (i, r['error'], r['detail'], m['detail'])
            continue
        for key in ('originals', 'outcomes', 'accounts', 'orders', 'roots', 'header', 'hash'):
            # fm2-trace lists resting orders in book order; the commitment sorts
            # them, so they compare as a set.
            if (sorted(r[key]) if key == 'orders' else r[key]) != m[key]:
                return 'slot %d: %s differ' % (i, key)
    return None


def pinned_goldens():
    """The skeleton goldens as the C++ sources pin them (GetHex order for hashes)."""
    with open(os.path.join(FM2, 'sim', 'scenario.cpp'), encoding='utf-8') as f:
        order = re.search(r'GOLDEN_CANONICAL_ORDER\{([^}]*)\}', f.read()).group(1)
    with open(os.path.join(FM2, 'test', 'sim_walking_skeleton_tests.cpp'), encoding='utf-8') as f:
        text = f.read()
    return {
        'canonical_order': re.findall(r'"([^"]*)"', order),
        'slot5_header': re.search(r'GOLDEN_SLOT5_HEADER\{"([0-9a-f]{64})"\}', text).group(1),
        'final_root': re.search(r'GOLDEN_FINAL_ROOT\{"([0-9a-f]{64})"\}', text).group(1),
    }


def skeleton(trace, profile_hash):
    """Recompute the O-87 goldens from the skeleton's inputs; returns the number of mismatches."""
    proc = subprocess.run([trace, '--export-skeleton'], capture_output=True, text=True)
    if proc.returncode != 0:
        print('skeleton: export failed: ' + proc.stderr.strip())
        return 1
    b = json.loads(proc.stdout)
    model = model_run(b, profile_hash)
    if not all(s['ok'] for s in model['slots']):
        print('skeleton: the model refuses a committed slot: %s' % [s.get('detail') for s in model['slots']])
        return 1
    labels = b['labels']

    def order_of(height):
        out = []
        for o in model['slots'][height - 1]['originals']:
            out.append(labels.get(Action.decode(bytes.fromhex(o), 4096).id().hex(), '?'))
        return ','.join(out)

    def gethex(h):
        return bytes.fromhex(h)[::-1].hex()

    final = decode_header(bytes.fromhex(model['slots'][-1]['header']))['state_root']
    got = {'canonical_order': [order_of(h) for h in (2, 3, 4)],
           'slot5_header': gethex(model['slots'][4]['hash']),
           'final_root': final[::-1].hex()}
    want = pinned_goldens()
    bad = 0
    for key in ('canonical_order', 'slot5_header', 'final_root'):
        ok = got[key] == want[key]
        bad += not ok
        print('skeleton %-15s %s  model %s  pinned %s' % (key, 'agrees' if ok else 'DIFFERS', got[key], want[key]))
    # The engine on the same inputs, as for any batch.
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, 'skeleton.json')
        with open(path, 'w', encoding='utf-8') as f:
            json.dump(b, f)
        real = json.loads(subprocess.run([trace, path], capture_output=True, text=True, check=True).stdout)
    why = compare(real, model)
    print('skeleton engine replay: %s (%d slots)' % (why or 'agrees', len(b['slots'])))
    return bad + (why is not None)


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument('--trace', required=True)
    p.add_argument('--seeds', type=int, default=20)
    p.add_argument('--slots', type=int, default=12)
    p.add_argument('--kinds', default=','.join(batchgen.KINDS))
    p.add_argument('--keep', help='write the batch and both results of a disagreement here')
    p.add_argument('--skeleton', action='store_true', help="recompute the walking skeleton's pinned goldens")
    args = p.parse_args()
    with open(PROFILE_JSON, encoding='utf-8') as f:
        profile_hash = bytes.fromhex(json.load(f)['profile_hash'])

    totals = collections.Counter()
    refusals = collections.Counter()
    codes = collections.Counter()
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for kind in args.kinds.split(','):
            for seed in range(args.seeds):
                b = batchgen.Generator(kind, seed, slots=args.slots).batch()
                path = os.path.join(tmp, 'batch.json')
                with open(path, 'w', encoding='utf-8') as f:
                    json.dump(b, f)
                proc = subprocess.run([args.trace, path], capture_output=True, text=True)
                if proc.returncode != 0:
                    print('%s/%d: fm2-trace failed: %s' % (kind, seed, proc.stderr.strip()))
                    failed += 1
                    continue
                real = json.loads(proc.stdout)
                model = model_run(b, profile_hash)
                for r, m in zip(real['slots'], model['slots']):
                    totals['slots'] += 1
                    totals['accepted' if r['ok'] else 'refused'] += 1
                    for o in r.get('outcomes', ()):
                        totals['outcomes'] += 1
                        codes[int.from_bytes(bytes.fromhex(o[146:150]), 'little')] += 1
                    if not r['ok'] and not m['ok']:
                        refusals[('%s %s' % (r['error'], r['detail']), '%s %s' % (m['error'], m['detail']))] += 1
                why = compare(real, model)
                totals['batches'] += 1
                if why:
                    failed += 1
                    print('%s/%d: DISAGREE: %s' % (kind, seed, why))
                    if args.keep:
                        os.makedirs(args.keep, exist_ok=True)
                        for name, obj in (('batch', b), ('engine', real), ('model', model)):
                            with open(os.path.join(args.keep, '%s-%d-%s.json' % (kind, seed, name)), 'w',
                                      encoding='utf-8') as f:
                                json.dump(obj, f, indent=1)
    print('batches %d, slots %d (accepted %d, refused %d), outcomes %d' % (
        totals['batches'], totals['slots'], totals['accepted'], totals['refused'], totals['outcomes']))
    print('outcome codes: ' + ', '.join('%s %d' % (CODE_NAMES.get(c, c), n) for c, n in sorted(codes.items())))
    if engine.CAP_STOPS:
        print('model fill-cap stops: ' + ', '.join('%s %d' % kv for kv in sorted(engine.CAP_STOPS.items())))
    for (real_detail, model_detail), n in sorted(refusals.items()):
        print('  refused %4d  engine: %s  |  model: %s' % (n, real_detail, model_detail))
    if args.skeleton:
        failed += skeleton(args.trace, profile_hash)
    if failed:
        print('DISAGREEMENT in %d of %d batches (or the skeleton)' % (failed, totals['batches']))
        return 1
    print('agreement 100%')
    return 0


if __name__ == '__main__':
    sys.exit(main())
