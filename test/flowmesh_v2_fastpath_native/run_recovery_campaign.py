#!/usr/bin/env python3
"""Run the declared finite native recovery cases; no latency claims."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

from run_recovery import execute

CUTS = ('after_intent', 'before_compute', 'after_compute_before_durable',
        'after_durable_before_publish', 'after_publish',
        'after_decision_before_apply', 'during_apply_before_atomic_commit', 'after_apply')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary', type=Path, required=True)
    p.add_argument('--evidence', type=Path, required=True)
    a = p.parse_args()
    binary = a.binary.resolve(strict=True)
    a.evidence.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='native-campaign-', dir=a.evidence))
    generated = subprocess.run([str(binary), 'generate'], check=True, timeout=30,
                               capture_output=True, text=True)
    fixture = json.loads(generated.stdout)
    (root/'fixture.json').write_text(json.dumps(fixture, indent=2)+'\n')
    cases = [(s,None,None) for s in ('healthy','leader-failure','hidden-fast','missing-body')]
    cases += [('cut',c,None) for c in CUTS]
    cases += [('serialize-failure',None,None)]
    cases += [('record-insert-failure',None,None)]
    cases += [('future-pc',None,None), ('two-view-timeout',None,None)]
    cases += [(s,None,seed) for seed in range(4) for s in ('healthy','leader-failure','hidden-fast','missing-body')]
    report = {'profile':'native-p2fv-recovery-test/1', 'status':'RUNNING',
              'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
              'case_count':len(cases), 'cases':[]}
    print(json.dumps({'evidence':str(root)}), flush=True)
    try:
        for i,(scenario,cut,seed) in enumerate(cases):
            path = root/f'{i:02d}-{scenario}'
            path.mkdir()
            result = execute(binary,fixture,path,scenario,cut,seed)
            row = {k:result[k] for k in ('scenario','cut','seed','status','delivery_steps',
                                       'unique_honest_signatures','retained_public_signatures','exits')}
            row['evidence'] = path.name
            report['cases'].append(row)
            print(json.dumps({k:v for k,v in row.items() if k != 'exits'}), flush=True)
        report['status'] = 'PASS'
    except BaseException:
        report['status'] = 'FAILED'
        raise
    finally:
        (root/'summary.json').write_text(json.dumps(report,indent=2)+'\n')


if __name__ == '__main__':
    main()
